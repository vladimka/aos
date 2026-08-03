#include "libaos.h"

#define MAX_WINDOWS 8
#define TITLE_H     18
#define BORDER      1
#define CUR_R       8           // cursor snapshot half-size

#define COL_DESKTOP      0x1A2030
#define COL_BORDER       0x000000
#define COL_TITLE        0x2E4E7B
#define COL_TITLE_FOCUS  0x4A7AB5
#define COL_TITLE_TEXT   0xFFFFFF
#define COL_CURSOR       0xFFFFFF

struct win {
    int used;
    unsigned int pid;
    int winid;
    int slab;
    int x, y;       // screen position (whole window incl. border/title)
    int cw, ch;     // content size
};

static struct win wins[MAX_WINDOWS];
static unsigned int fb_addr, fb_w, fb_h, fb_pitch;
static unsigned int *scratch;   // slab 0: scratch for title-bar text rendering
static int next_slab = 1;
static unsigned int focus_pid;
static int has_cur, cur_x, cur_y;
static unsigned int snap[2 * CUR_R][2 * CUR_R];
static int clip_x0, clip_y0, clip_x1, clip_y1;

// ---- small helpers -------------------------------------------------------

static void mcpy(unsigned int *d, const unsigned int *s, unsigned int n) {
    while (n >= 4) {
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        d += 4; s += 4; n -= 4;
    }
    while (n--) *d++ = *s++;
}

static void int2str(char *buf, int v) {
    char tmp[12];
    int i = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (v > 0) { tmp[i++] = '0' + v % 10; v /= 10; }
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = 0;
}

// ---- framebuffer drawing --------------------------------------------------

static void fb_fill(int x, int y, int w, int h, unsigned int rgb) {
    unsigned int *fb = (unsigned int *)fb_addr;
    unsigned int pitch = fb_pitch >> 2;
    int x0 = x > clip_x0 ? x : clip_x0;
    int y0 = y > clip_y0 ? y : clip_y0;
    int x1 = (x + w < clip_x1) ? x + w : clip_x1;
    int y1 = (y + h < clip_y1) ? y + h : clip_y1;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            fb[(unsigned)yy * pitch + (unsigned)xx] = rgb;
}

static void draw_close_btn(const struct win *wn);

static void draw_title(const struct win *wn) {
    unsigned int tcol = (wn->pid == focus_pid) ? COL_TITLE_FOCUS : COL_TITLE;
    fb_fill(wn->x + BORDER, wn->y + BORDER, wn->cw, TITLE_H, tcol);
    char buf[8];
    int2str(buf, wn->winid);
    unsigned int *sc = scratch;
    render_text(sc, 1024 * 4, 0, 0, buf, COL_TITLE_TEXT, tcol);
    int tw = 8;
    for (int i = 0; buf[i]; i++) tw += 8;
    int tx = wn->x + BORDER + 6;
    int ty = wn->y + BORDER + (TITLE_H - 16) / 2;
    int x0 = tx > clip_x0 ? tx : clip_x0;
    int y0 = ty > clip_y0 ? ty : clip_y0;
    int x1 = (tx + tw < clip_x1) ? tx + tw : clip_x1;
    int y1 = (ty + 16 < clip_y1) ? ty + 16 : clip_y1;
    if (x0 >= x1 || y0 >= y1) return;
    unsigned int *fb = (unsigned int *)fb_addr;
    unsigned int pitch = fb_pitch >> 2;
    for (int r = y0; r < y1; r++)
        mcpy(fb + (unsigned)r * pitch + (unsigned)x0,
             sc + (unsigned)(r - ty) * 1024 + (unsigned)(x0 - tx),
             (unsigned)(x1 - x0));
    draw_close_btn(wn);
}

static void blit_content(const struct win *wn) {
    const unsigned int *src =
        (const unsigned int *)(AOS_SLAB_BASE + wn->slab * AOS_SLAB_SIZE);
    unsigned int *dst = (unsigned int *)fb_addr;
    unsigned int pitch = fb_pitch >> 2;
    int cx = wn->x + BORDER;
    int cy = wn->y + BORDER + TITLE_H;
    int x0 = cx > clip_x0 ? cx : clip_x0;
    int y0 = cy > clip_y0 ? cy : clip_y0;
    int x1 = (cx + wn->cw < clip_x1) ? cx + wn->cw : clip_x1;
    int y1 = (cy + wn->ch < clip_y1) ? cy + wn->ch : clip_y1;
    if (x0 >= x1 || y0 >= y1) return;
    for (int r = y0; r < y1; r++)
        mcpy(dst + (unsigned)r * pitch + (unsigned)x0,
             src + (unsigned)(r - cy) * wn->cw + (unsigned)(x0 - cx),
             (unsigned)(x1 - x0));
}

static void draw_close_btn(const struct win *wn) {
    int bx = wn->x + BORDER + wn->cw - 18;
    int by = wn->y + BORDER + (TITLE_H - 16) / 2;
    unsigned int *fb = (unsigned int *)fb_addr;
    unsigned int pitch = fb_pitch >> 2;
    for (int i = 0; i < 16; i++) {
        fb[(unsigned)(by + i) * pitch + (unsigned)(bx + i)] = COL_TITLE_TEXT;
        fb[(unsigned)(by + i) * pitch + (unsigned)(bx + 15 - i)] = COL_TITLE_TEXT;
    }
}

static void composite_rect(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)fb_w) x1 = (int)fb_w;
    if (y1 > (int)fb_h) y1 = (int)fb_h;
    if (x0 >= x1 || y0 >= y1) return;
    clip_x0 = x0; clip_y0 = y0; clip_x1 = x1; clip_y1 = y1;
    fb_fill(x0, y0, x1 - x0, y1 - y0, COL_DESKTOP);
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct win *wn = &wins[i];
        if (!wn->used) continue;
        if (wn->x < x1 && wn->x + wn->cw + 2 * BORDER > x0 &&
            wn->y < y1 && wn->y + wn->ch + TITLE_H + 2 * BORDER > y0) {
            fb_fill(wn->x, wn->y, wn->cw + 2 * BORDER, BORDER, COL_BORDER);
            fb_fill(wn->x, wn->y + wn->ch + TITLE_H + BORDER,
                    wn->cw + 2 * BORDER, BORDER, COL_BORDER);
            fb_fill(wn->x, wn->y, BORDER, wn->ch + TITLE_H + 2 * BORDER,
                    COL_BORDER);
            fb_fill(wn->x + wn->cw + BORDER, wn->y, BORDER,
                    wn->ch + TITLE_H + 2 * BORDER, COL_BORDER);
            draw_title(wn);
            blit_content(wn);
        }
    }
    clip_x0 = 0; clip_y0 = 0;
    clip_x1 = (int)fb_w; clip_y1 = (int)fb_h;
}

static void composite(void) {
    composite_rect(0, 0, (int)fb_w, (int)fb_h);
}

// ---- cursor (drawn last, erased via its own snapshot) ----------------------

static void save_snap(int x, int y) {
    unsigned int *fb = (unsigned int *)fb_addr;
    unsigned int pitch = fb_pitch >> 2;
    int x0 = x - CUR_R, y0 = y - CUR_R;
    for (int r = 0; r < 2 * CUR_R; r++)
        mcpy(snap[r], fb + (unsigned)(y0 + r) * pitch + (unsigned)x0, 2 * CUR_R);
}

static void restore_snap(int x, int y) {
    unsigned int *fb = (unsigned int *)fb_addr;
    unsigned int pitch = fb_pitch >> 2;
    int x0 = x - CUR_R, y0 = y - CUR_R;
    for (int r = 0; r < 2 * CUR_R; r++)
        mcpy(fb + (unsigned)(y0 + r) * pitch + (unsigned)x0, snap[r], 2 * CUR_R);
}

static void draw_cursor(int x, int y) {
    unsigned int *fb = (unsigned int *)fb_addr;
    unsigned int pitch = fb_pitch >> 2;
    for (int i = -3; i <= 3; i++) {
        fb[(unsigned)y * pitch + (unsigned)(x + i)] = COL_CURSOR;
        fb[(unsigned)(y + i) * pitch + (unsigned)x] = COL_CURSOR;
    }
}

static void update_cursor(int mx, int my) {
    if (has_cur)
        restore_snap(cur_x, cur_y);
    has_cur = 0;
    if (mx >= CUR_R && mx < (int)fb_w - CUR_R &&
        my >= CUR_R && my < (int)fb_h - CUR_R) {
        save_snap(mx, my);
        draw_cursor(mx, my);
        cur_x = mx;
        cur_y = my;
        has_cur = 1;
    }
}

static int cursor_overlaps(int x, int y, int w, int h) {
    if (!has_cur) return 0;
    int cx0 = cur_x - CUR_R, cy0 = cur_y - CUR_R;
    return cx0 < x + w && cx0 + 2 * CUR_R > x &&
           cy0 < y + h && cy0 + 2 * CUR_R > y;
}

// ---- window management ---------------------------------------------------

static int alloc_window(unsigned int pid, int w, int h, int *out_wid, int *out_slab) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wins[i].used) continue;
        wins[i].used = 1;
        wins[i].pid = pid;
        wins[i].winid = i;
        wins[i].slab = next_slab++;
        wins[i].cw = w;
        wins[i].ch = h;
        wins[i].x = 20 + i * 24;
        wins[i].y = 20 + i * 28;
        *out_wid = i;
        *out_slab = wins[i].slab;
        return 0;
    }
    return -1;
}

static int win_index_at(int mx, int my) {
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        struct win *wn = &wins[i];
        if (!wn->used) continue;
        if (mx >= wn->x && mx < wn->x + wn->cw + 2 * BORDER &&
            my >= wn->y && my < wn->y + wn->ch + TITLE_H + 2 * BORDER)
            return i;
    }
    return -1;
}

static int close_btn_at(int mx, int my) {
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        struct win *wn = &wins[i];
        if (!wn->used) continue;
        int bx = wn->x + BORDER + wn->cw - 18;
        int by = wn->y + BORDER + (TITLE_H - 16) / 2;
        if (mx >= bx && mx < bx + 16 && my >= by && my < by + 16)
            return i;
    }
    return -1;
}

static void free_windows(unsigned int pid) {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i].used && wins[i].pid == pid) {
            wins[i].used = 0;
            if (focus_pid == pid) focus_pid = 0;
        }
}

void main(void) {
    unsigned int bpp;
    get_fb_info(&fb_addr, &fb_w, &fb_h, &fb_pitch, &bpp);
    clip_x1 = (int)fb_w;
    clip_y1 = (int)fb_h;
    print("wm: fb="); print_hex(fb_addr); print(" w="); print_dec(fb_w);
    print(" h="); print_dec(fb_h); print(" pitch="); print_dec(fb_pitch);
    print(" bpp="); print_dec(bpp); print("\n");
    if (bpp != 32) {
        print("wm: framebuffer is not 32bpp\n");
        exit();
    }
    scratch = (unsigned int *)AOS_SLAB_BASE;
    register_events();

    // Launch the desktop applications (they create their windows via MSG_CREATE).
    spawn("bin/term", "", getpid());
    spawn("bin/clock", "", getpid());

    int last_mx = 0, last_my = 0, last_mb = 0;
    int redraw = 1;
    int drag_i = -1, drag_dx = 0, drag_dy = 0;

    for (;;) {
        int mx, my, mb, wheel;
        get_mouse(&mx, &my, &mb, &wheel);

        struct aos_msg m;
        while (recv_msg(&m) == 0) {
            switch (m.type) {
            case MSG_KEY:
                if (focus_pid) {
                    struct aos_msg k = {MSG_KEY, m.a, 0, 0, 0};
                    send_msg(focus_pid, &k);
                }
                break;
            case MSG_CREATE: {
                int wid, slab;
                if (alloc_window(m.c, (int)m.a, (int)m.b, &wid, &slab) == 0) {
                    struct aos_msg r = {MSG_WININFO, (unsigned int)wid,
                                        (unsigned int)slab, 0, 0};
                    send_msg(m.c, &r);
                    if (!focus_pid) focus_pid = m.c;
                    redraw = 1;
                }
                break;
            }
            case MSG_UPDATE:
                if (m.a < MAX_WINDOWS && wins[m.a].used) {
                    struct win *wn = &wins[m.a];
                    composite_rect(wn->x, wn->y,
                                   wn->x + wn->cw + 2 * BORDER,
                                   wn->y + wn->ch + TITLE_H + 2 * BORDER);
                    if (cursor_overlaps(wn->x, wn->y,
                                        wn->cw + 2 * BORDER,
                                        wn->ch + TITLE_H + 2 * BORDER)) {
                        has_cur = 0;
                        update_cursor(mx, my);
                    }
                }
                break;
            case MSG_EXIT:
                free_windows(m.a);
                redraw = 1;
                break;
            }
        }

        int moved = mx != last_mx || my != last_my || mb != last_mb;
        if (moved && (mb & 1) && !(last_mb & 1)) {
            int cb = close_btn_at(mx, my);
            if (cb >= 0 && win_index_at(mx, my) == cb) {
                struct aos_msg cl = {MSG_CLOSE, 0, 0, 0, 0};
                send_msg(wins[cb].pid, &cl);
            } else {
                int wi = win_index_at(mx, my);
                if (wi >= 0) {
                    if (focus_pid != wins[wi].pid) redraw = 1;
                    focus_pid = wins[wi].pid;
                    if (my >= wins[wi].y + BORDER &&
                        my < wins[wi].y + BORDER + TITLE_H) {
                        drag_i = wi;
                        drag_dx = mx - wins[wi].x;
                        drag_dy = my - wins[wi].y;
                    }
                }
            }
        }
        if (!(mb & 1) && (last_mb & 1))
            drag_i = -1;
        if (drag_i >= 0 && wins[drag_i].used &&
            (mx != last_mx || my != last_my)) {
            struct win *wn = &wins[drag_i];
            int nx = mx - drag_dx;
            int ny = my - drag_dy;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx > (int)fb_w - (wn->cw + 2 * BORDER))
                nx = (int)fb_w - (wn->cw + 2 * BORDER);
            if (ny > (int)fb_h - (wn->ch + TITLE_H + 2 * BORDER))
                ny = (int)fb_h - (wn->ch + TITLE_H + 2 * BORDER);
            if (nx != wn->x || ny != wn->y) {
                int ox = wn->x, oy = wn->y;
                int ow = wn->cw + 2 * BORDER, oh = wn->ch + TITLE_H + 2 * BORDER;
                wn->x = nx;
                wn->y = ny;
                int x0 = ox < nx ? ox : nx;
                int y0 = oy < ny ? oy : ny;
                int x1 = (ox > nx ? ox : nx) + ow;
                int y1 = (oy > ny ? oy : ny) + oh;
                composite_rect(x0, y0, x1, y1);
                if (cursor_overlaps(x0, y0, x1 - x0, y1 - y0)) {
                    has_cur = 0;
                    update_cursor(mx, my);
                }
            }
        }

        int need_cursor = moved;
        if (redraw) {
            composite();
            has_cur = 0;
            update_cursor(mx, my);
            redraw = 0;
            need_cursor = 1;
        }
        if (need_cursor) {
            update_cursor(mx, my);
        }
        last_mx = mx;
        last_my = my;
        last_mb = mb;
        yield();
    }
}
