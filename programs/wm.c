#include "libaos.h"
#include "ico.h"

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

// ---- dock ----------------------------------------------------------------

#define DOCK_H       52
#define DOCK_MARGIN  8
#define DOCK_PAD_X   12
#define DOCK_PAD_Y   10
#define DOCK_ICON    32
#define DOCK_STRIDE  40

#define COL_DOCK_BG      0x20283A
#define COL_DOCK_BORDER  0x2E4E7B
#define COL_ICON_FG      0xE8EEF8
#define COL_DOCK_ACTIVE  0x4A7AB5

// 32x32 1bpp icons, 'X' = foreground pixel, anything else = transparent.
static const char icon_term[32][33] = {
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X........XX....................X",
    "X.........XX...................X",
    "X..........XX..................X",
    "X..........XX..................X",
    "X.........XX...................X",
    "X........XX....................X",
    "X..............................X",
    "X..............................X",
    "X..........XXXXXXXXXXX.........X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
};

static const char icon_clock[32][33] = {
    "................................",
    "................................",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "..X..........................X..",
    "..X..........................X..",
    "..X...........XXX............X..",
    "..X...........XXX............X..",
    "..X...........XXX............X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X...........XXX............X..",
    "..X...........XXX............X..",
    "..X...........XXX............X..",
    "..X...........XXX............X..",
    "..X...........XXX............X..",
    "..X.XXX........XXXXXXXXX.XXX.X..",
    "..X.XXX........XXXXXXXXX.XXX.X..",
    "..X...........XXX............X..",
    "..X...........XXX............X..",
    "..X...........XXX............X..",
    "..X...........XXX............X..",
    "..X...........XXX............X..",
    "..X...........XXX............X..",
    "..X..........................X..",
    "..X...........XXX............X..",
    "..X...........XXX............X..",
    "..X...........XXX............X..",
    "..X..........................X..",
    "..X..........................X..",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "................................",
    "................................",
};

static const char icon_unknown[32][33] = {
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..........XXXXXXX.............X",
    "X.........XXXXXXXX.............X",
    "X.........XXXXXXXX.............X",
    "X........XX....XXX.............X",
    "X........XX....XXX.............X",
    "X........XX....XXX.............X",
    "X.............XXX..............X",
    "X.............XX...............X",
    "X............XX................X",
    "X............XX................X",
    "X..............................X",
    "X..............................X",
    "X............XX................X",
    "X............XX................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
};

static const char icon_folder[32][33] = {
    "................................",
    "................................",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXX...",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXX...",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "................................",
    "................................",
    "................................",
};

static const char icon_file[32][33] = {
    "................................",
    "................................",
    "....XXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "....X..........................X",
    "....X..........................X",
    "....X..XXXXXXXXX...............X",
    "....X..X.......X...............X",
    "....X..X.......X...............X",
    "....X..XXXXXXXXX...............X",
    "....X..........................X",
    "....X..........................X",
    "....X..XXXXXXXXX...............X",
    "....X..X.......X...............X",
    "....X..X.......X...............X",
    "....X..XXXXXXXXX...............X",
    "....X..........................X",
    "....X..........................X",
    "....X..XXXXXXXXX...............X",
    "....X..X.......X...............X",
    "....X..X.......X...............X",
    "....X..XXXXXXXXX...............X",
    "....X..........................X",
    "....X..........................X",
    "....X..........................X",
    "....X..........................X",
    "....X..........................X",
    "....X..........................X",
    "....XXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "................................",
    "................................",
    "................................",
    "................................",
};

static const char icon_image[32][33] = {
    "................................",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "..X..........................X..",
    "..X.........XXXX.............X..",
    "..X........X....X............X..",
    "..X........X....X............X..",
    "..X.......X......X...........X..",
    "..X.......X......X...........X..",
    "..X......X........X..........X..",
    "..X......X........X..........X..",
    "..X.....X..........X.........X..",
    "..X.....X..........X.........X..",
    "..X....X............X........X..",
    "..X....X............X........X..",
    "..X...X..............X.......X..",
    "..X...X..............X.......X..",
    "..X..X................X......X..",
    "..X..X................X......X..",
    "..X.X..................X.....X..",
    "..X.X..................X.....X..",
    "..XX....................X....X..",
    "..X......................X...X..",
    "..X......................X...X..",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
};

struct win {
    int used;
    unsigned int pid;
    int winid;
    int slab;
    int x, y;       // screen position (whole window incl. border/title)
    int cw, ch;     // content size
    int app;        // APP_TERM / APP_CLOCK / APP_UNKNOWN
};

enum { APP_TERM = 0, APP_CLOCK = 1, APP_UNKNOWN = 2 };

static struct launcher {
    const char *path;
    unsigned int pid;
    int running;
} launchers[2] = {
    { "bin/term", 0, 0 },
    { "bin/clock", 0, 0 },
};

static int zorder[MAX_WINDOWS];
static int nz;

static struct win wins[MAX_WINDOWS];
static unsigned int fb_addr, fb_w, fb_h, fb_pitch;
static unsigned int *scratch;   // slab 0: scratch for title-bar text rendering
static int next_slab = 1;
static unsigned int focus_pid;
static int redraw = 1;
static int has_cur, cur_x, cur_y;
static unsigned int snap[2 * CUR_R][2 * CUR_R];
static int clip_x0, clip_y0, clip_x1, clip_y1;

// ---- desktop context menu + create dialog state (used by composite_rect) ----
#define MENU_W      176
#define MENU_ITEM_H 22
#define MENU_N       2
#define MENU_BORDER  1

#define COL_MENU_BG     0x20283A
#define COL_MENU_BORDER 0x4A7AB5
#define COL_MENU_FG     0xFFFFFF

static int menu_open, menu_x, menu_y;
static int menu_draw_x, menu_draw_y;
static int dlg_open, dlg_mode;         // mode 0 = file, 1 = folder
static char dlg_name[40];
static int dlg_len;
static int dlg_draw_x, dlg_draw_y;

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
static int cursor_overlaps(int x, int y, int w, int h);
static void draw_dock(void);
static int dock_x0(void);
static int dock_y0(void);
static int dock_width(void);
static int app_type_of(unsigned int pid);
static void raise_pid(unsigned int pid);
static void draw_desktop_icons(void);
static void draw_menu(void);
static void draw_dialog(void);

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
    draw_desktop_icons();
    for (int k = 0; k < nz; k++) {
        struct win *wn = &wins[zorder[k]];
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
    draw_dock();
    draw_menu();
    draw_dialog();
    if (cursor_overlaps(dock_x0(), dock_y0(), dock_width(), DOCK_H))
        has_cur = 0;
    if (menu_open &&
        cursor_overlaps(menu_draw_x, menu_draw_y, MENU_W,
                        MENU_N * MENU_ITEM_H + 2 * MENU_BORDER))
        has_cur = 0;
    if (dlg_open && cursor_overlaps(dlg_draw_x, dlg_draw_y, 360, 88))
        has_cur = 0;
}

static void composite(void) {
    composite_rect(0, 0, (int)fb_w, (int)fb_h);
}

// ---- dock ----------------------------------------------------------------

static int dock_nitems(void) { return 2 + nz; }

static int dock_x0(void) {
    return (int)fb_w / 2 - (16 + dock_nitems() * DOCK_STRIDE) / 2;
}

static int dock_y0(void) { return (int)fb_h - DOCK_H - DOCK_MARGIN; }

static int dock_width(void) { return 16 + dock_nitems() * DOCK_STRIDE; }

static void draw_icon(int x, int y, const char icon[DOCK_ICON][DOCK_ICON + 1],
                      unsigned int fg) {
    unsigned int *fb = (unsigned int *)fb_addr;
    unsigned int pitch = fb_pitch >> 2;
    for (int r = 0; r < DOCK_ICON; r++)
        for (int c = 0; c < DOCK_ICON; c++)
            if (icon[r][c] == 'X')
                fb[(unsigned)(y + r) * pitch + (unsigned)(x + c)] = fg;
}

static void draw_dock(void) {
    int dx0 = dock_x0(), dy0 = dock_y0();
    int dw = dock_width();
    fb_fill(dx0, dy0, dw, DOCK_H, COL_DOCK_BG);
    fb_fill(dx0, dy0, dw, 1, COL_DOCK_BORDER);
    fb_fill(dx0, dy0 + DOCK_H - 1, dw, 1, COL_DOCK_BORDER);
    fb_fill(dx0, dy0, 1, DOCK_H, COL_DOCK_BORDER);
    fb_fill(dx0 + dw - 1, dy0, 1, DOCK_H, COL_DOCK_BORDER);
    // fake rounded corners
    fb_fill(dx0, dy0, 1, 1, COL_DESKTOP);
    fb_fill(dx0 + dw - 1, dy0, 1, 1, COL_DESKTOP);
    fb_fill(dx0, dy0 + DOCK_H - 1, 1, 1, COL_DESKTOP);
    fb_fill(dx0 + dw - 1, dy0 + DOCK_H - 1, 1, 1, COL_DESKTOP);
    int iy = dy0 + DOCK_PAD_Y;
    draw_icon(dx0 + DOCK_PAD_X, iy, icon_term, COL_ICON_FG);
    draw_icon(dx0 + DOCK_PAD_X + DOCK_STRIDE, iy, icon_clock, COL_ICON_FG);
    int di = 2;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wins[i].used) continue;
        const char (*ic)[DOCK_ICON + 1];
        if (wins[i].app == APP_TERM) ic = icon_term;
        else if (wins[i].app == APP_CLOCK) ic = icon_clock;
        else ic = icon_unknown;
        int ix = dx0 + DOCK_PAD_X + di * DOCK_STRIDE;
        draw_icon(ix, iy, ic, COL_ICON_FG);
        unsigned int *fb = (unsigned int *)fb_addr;
        unsigned int pitch = fb_pitch >> 2;
        int dotx = ix + DOCK_ICON / 2 - 2;
        int doty = iy + DOCK_ICON + 4;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                fb[(unsigned)(doty + r) * pitch + (unsigned)(dotx + c)] = COL_DOCK_ACTIVE;
        di++;
    }
}

static int win_at_dock_index(int di) {
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i].used) {
            if (n == di) return i;
            n++;
        }
    return -1;
}

static void launcher_click(int i) {
    if (launchers[i].running) {
        raise_pid(launchers[i].pid);
        return;
    }
    int pid = spawn(launchers[i].path, "", getpid());
    if (pid > 0) {
        launchers[i].pid = (unsigned int)pid;
    } else {
        print("wm: dock spawn failed\n");
    }
}

static int dock_hit(int mx, int my) {
    int dx0 = dock_x0(), dy0 = dock_y0();
    if (my < dy0 || my >= dy0 + DOCK_H) return 0;
    int rel = mx - dx0;
    if (rel < 0 || rel >= dock_width()) return 0;
    int i = (rel - DOCK_PAD_X) / DOCK_STRIDE;
    if (i < 0) return 1;
    if (i < 2) launcher_click(i);
    else if (i < 2 + nz) {
        int wi = win_at_dock_index(i - 2);
        if (wi >= 0) raise_pid(wins[wi].pid);
    }
    return 1;
}

// ---- desktop file icons ---------------------------------------------------

#define ICON_W    32
#define ICON_H    32
#define GRID_X0   16
#define GRID_Y0   24
#define GRID_CELL 52
#define LABEL_H   16

enum { K_FOLDER = 0, K_TEXT = 1, K_ICO = 2, K_OTHER = 3 };

static struct dent {
    char name[28];
    int kind;
} files[64];
static int nfiles;
static int files_dirty = 1;
static int refresh_cnt;

static void fb_put(int x, int y, unsigned int rgb) {
    if (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1) return;
    unsigned int *fb = (unsigned int *)fb_addr;
    ((unsigned int *)fb)[(unsigned)y * (fb_pitch >> 2) + (unsigned)x] = rgb;
}

// Clip-aware 1bpp icon art: 'X' = foreground pixel.
static void draw_icon_art(int x, int y, const char art[ICON_W][ICON_W + 1],
                          unsigned int fg) {
    for (int r = 0; r < ICON_W; r++)
        for (int c = 0; c < ICON_W; c++)
            if (art[r][c] == 'X') fb_put(x + c, y + r, fg);
}

static int utf8_vis_len(const char *s) {
    int n = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c < 0x80) n++;
        else if ((c & 0xE0) == 0xC0) { s += 1; n++; }
        else if ((c & 0xF0) == 0xE0) { s += 2; n++; }
        else { s += 3; n++; }
    }
    return n;
}

// Render a UTF-8 string into the FB at (x, y) with clipping. Text is drawn
// through the scratch slab (same technique as draw_title).
static void fb_text(int x, int y, const char *s, unsigned int fg,
                    unsigned int bg) {
    render_text(scratch, 1024 * 4, 0, 0, s, fg, bg);
    int w = utf8_vis_len(s) * 8;
    int x0 = x > clip_x0 ? x : clip_x0;
    int y0 = y > clip_y0 ? y : clip_y0;
    int x1 = (x + w < clip_x1) ? x + w : clip_x1;
    int y1 = (y + 16 < clip_y1) ? y + 16 : clip_y1;
    if (x0 >= x1 || y0 >= y1) return;
    unsigned int *fb = (unsigned int *)fb_addr;
    unsigned int pitch = fb_pitch >> 2;
    for (int r = y0; r < y1; r++)
        mcpy(fb + (unsigned)r * pitch + (unsigned)x0,
             scratch + (unsigned)(r - y) * 1024 + (unsigned)(x0 - x),
             (unsigned)(x1 - x0));
}

static int ext_match(const char *n, const char *ext) {
    int nl = 0, el = 0;
    while (n[nl]) nl++;
    while (ext[el]) el++;
    if (nl < el) return 0;
    for (int i = 0; i < el; i++) {
        char a = n[nl - el + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static void refresh_files(void) {
    int old_n = nfiles;
    files_dirty = 0;
    nfiles = 0;
    for (int i = 0; i < 64; i++) {
        char nm[28];
        unsigned int sz;
        if (fs_list_get((unsigned int)i, nm, &sz) != 0) continue;
        if (nm[0] == 0) continue;
        // Programs live in the dock; keep the desktop for data files.
        if (nm[0] == 'b' && nm[1] == 'i' && nm[2] == 'n' && nm[3] == '/')
            continue;
        int len = 0;
        while (nm[len]) len++;
        int kind;
        if (len > 0 && nm[len - 1] == '/') kind = K_FOLDER;
        else if (ext_match(nm, ".ico")) kind = K_ICO;
        else if (ext_match(nm, ".txt")) kind = K_TEXT;
        else kind = K_OTHER;
        int j;
        for (j = 0; j < 27 && nm[j]; j++) files[nfiles].name[j] = nm[j];
        files[nfiles].name[j] = 0;
        files[nfiles].kind = kind;
        nfiles++;
        if (nfiles >= 64) break;
    }
    if (nfiles != old_n) redraw = 1;
}

static void icon_rect(int i, int *x, int *y) {
    int cols = ((int)fb_w - GRID_X0) / GRID_CELL;
    if (cols < 1) cols = 1;
    *x = GRID_X0 + (i % cols) * GRID_CELL;
    *y = GRID_Y0 + (i / cols) * (ICON_H + LABEL_H + 8);
}

static void draw_ico_file(int i, int x, int y) {
    static char ico_buf[8192];
    static unsigned int px[ICON_W * ICON_H];
    unsigned int dw, dh;
    int sz = fs_read(files[i].name, ico_buf, sizeof(ico_buf));
    if (sz > 0 &&
        ico_decode((const unsigned char *)ico_buf, (unsigned int)sz,
                   ICON_W, ICON_H, &dw, &dh, px) == 0) {
        for (int r = 0; r < ICON_W; r++)
            for (int c = 0; c < ICON_W; c++) {
                unsigned int p = px[r * ICON_W + c];
                if (p & 0xFF000000) fb_put(x + c, y + r, p & 0x00FFFFFF);
            }
    } else {
        draw_icon_art(x, y, icon_image, COL_ICON_FG);
    }
}

static void draw_desktop_icons(void) {
    for (int i = 0; i < nfiles; i++) {
        int x, y;
        icon_rect(i, &x, &y);
        if (x + ICON_W < clip_x0 || x >= clip_x1) continue;
        if (y + ICON_H < clip_y0 || y >= clip_y1) continue;
        switch (files[i].kind) {
        case K_FOLDER: draw_icon_art(x, y, icon_folder, COL_ICON_FG); break;
        case K_ICO:    draw_ico_file(i, x, y); break;
        case K_TEXT:   draw_icon_art(x, y, icon_file, COL_ICON_FG); break;
        default:       draw_icon_art(x, y, icon_unknown, COL_ICON_FG); break;
        }
        if (y + ICON_H + 2 < clip_y1)
            fb_text(x, y + ICON_H + 2, files[i].name, COL_ICON_FG,
                    COL_DESKTOP);
    }
}

static int icon_at(int mx, int my) {
    for (int i = 0; i < nfiles; i++) {
        int x, y;
        icon_rect(i, &x, &y);
        if (mx >= x && mx < x + ICON_W && my >= y && my < y + ICON_H)
            return i;
    }
    return -1;
}

static void open_file(int i) {
    if (files[i].kind == K_ICO || files[i].kind == K_FOLDER) return;
    spawn("bin/notepad", files[i].name, getpid());
}

// ---- desktop context menu + create dialog ---------------------------------

static const char menu_items[MENU_N][24] = {
    "\xd0\x9d\xd0\xbe\xd0\xb2\xd1\x8b\xd0\xb9 \xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb",   // Новый файл
    "\xd0\x9d\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x8f \xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0", // Новая папка
};

static void draw_menu(void) {
    if (!menu_open) return;
    int x = menu_x, y = menu_y;
    int mw = MENU_W;
    int mh = MENU_N * MENU_ITEM_H + 2 * MENU_BORDER;
    if (x + mw > (int)fb_w) x = (int)fb_w - mw;
    if (y + mh > (int)fb_h) y = (int)fb_h - mh;
    menu_draw_x = x;
    menu_draw_y = y;
    fb_fill(x, y, mw, mh, COL_MENU_BORDER);
    fb_fill(x + MENU_BORDER, y + MENU_BORDER, mw - 2 * MENU_BORDER,
            mh - 2 * MENU_BORDER, COL_MENU_BG);
    fb_text(x + 10, y + MENU_BORDER + 3, menu_items[0], COL_MENU_FG,
            COL_MENU_BG);
    fb_text(x + 10, y + MENU_BORDER + 3 + MENU_ITEM_H, menu_items[1],
            COL_MENU_FG, COL_MENU_BG);
}

static int menu_item_at(int mx, int my) {
    if (!menu_open) return -1;
    int x = menu_draw_x, y = menu_draw_y;
    if (mx < x || mx >= x + MENU_W) return -1;
    if (my < y || my >= y + MENU_N * MENU_ITEM_H + 2 * MENU_BORDER) return -1;
    int i = (my - (y + MENU_BORDER)) / MENU_ITEM_H;
    if (i < 0 || i >= MENU_N) return -1;
    return i;
}

static void do_create(void) {
    if (dlg_len == 0) return;
    char full[30];
    int i;
    for (i = 0; i < dlg_len; i++) full[i] = dlg_name[i];
    if (dlg_mode == 1) full[i++] = '/';   // folders are names ending with '/'
    full[i] = 0;
    fs_write(full, "", 0);
    files_dirty = 1;
}

static void draw_dialog(void) {
    if (!dlg_open) return;
    int x = (int)fb_w / 2 - 180;
    int y = (int)fb_h / 3;
    dlg_draw_x = x;
    dlg_draw_y = y;
    fb_fill(x, y, 360, 88, COL_MENU_BG);
    fb_fill(x, y, 360, 1, COL_MENU_BORDER);
    fb_fill(x, y + 87, 360, 1, COL_MENU_BORDER);
    fb_fill(x, y, 1, 88, COL_MENU_BORDER);
    fb_fill(x + 359, y, 1, 88, COL_MENU_BORDER);
    const char *title = dlg_mode
        ? "\xd0\x98\xd0\xbc\xd1\x8f \xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbe\xd0\xb9 \xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb8:"
        : "\xd0\x98\xd0\xbc\xd1\x8f \xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbe\xd0\xb3\xd0\xbe \xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xb0:";
    fb_text(x + 10, y + 8, title, COL_MENU_FG, COL_MENU_BG);
    int bx = x + 10, by = y + 36, bw = 340, bh = 20;
    fb_fill(bx, by, bw, bh, 0x101010);
    if (dlg_len)
        fb_text(bx + 4, by + 2, dlg_name, COL_MENU_FG, 0x101010);
    int cx = bx + 4 + dlg_len * 8;      // dialog names are ASCII only
    fb_fill(cx, by + 3, 2, 14, COL_MENU_FG);
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
        wins[i].app = app_type_of(pid);
        wins[i].x = 20 + i * 24;
        wins[i].y = 20 + i * 28;
        *out_wid = i;
        *out_slab = wins[i].slab;
        zorder[nz++] = i;
        return 0;
    }
    return -1;
}

static int win_index_at(int mx, int my) {
    for (int k = nz - 1; k >= 0; k--) {
        struct win *wn = &wins[zorder[k]];
        if (!wn->used) continue;
        if (mx >= wn->x && mx < wn->x + wn->cw + 2 * BORDER &&
            my >= wn->y && my < wn->y + wn->ch + TITLE_H + 2 * BORDER)
            return zorder[k];
    }
    return -1;
}

static int close_btn_at(int mx, int my) {
    for (int k = nz - 1; k >= 0; k--) {
        struct win *wn = &wins[zorder[k]];
        if (!wn->used) continue;
        int bx = wn->x + BORDER + wn->cw - 18;
        int by = wn->y + BORDER + (TITLE_H - 16) / 2;
        if (mx >= bx && mx < bx + 16 && my >= by && my < by + 16)
            return zorder[k];
    }
    return -1;
}

static void free_windows(unsigned int pid) {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i].used && wins[i].pid == pid) {
            wins[i].used = 0;
            for (int j = 0; j < nz; j++)
                if (zorder[j] == i) {
                    for (int k = j; k < nz - 1; k++) zorder[k] = zorder[k + 1];
                    nz--;
                    break;
                }
            if (focus_pid == pid) focus_pid = 0;
        }
    for (int i = 0; i < 2; i++)
        if (launchers[i].pid == pid) launchers[i].running = 0;
}

static void raise_pid(unsigned int pid) {
    int at = -1;
    for (int i = 0; i < nz; i++)
        if (wins[zorder[i]].used && wins[zorder[i]].pid == pid) { at = i; break; }
    if (at < 0) return;
    int wi = zorder[at];
    for (int j = at; j < nz - 1; j++) zorder[j] = zorder[j + 1];
    zorder[nz - 1] = wi;
    if (at != nz - 1 || focus_pid != pid) redraw = 1;
    focus_pid = pid;
}

static int app_type_of(unsigned int pid) {
    for (int i = 0; i < 2; i++)
        if (launchers[i].pid == pid) return i;
    return APP_UNKNOWN;
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

    int last_mx = 0, last_my = 0, last_mb = 0;
    int drag_i = -1, drag_dx = 0, drag_dy = 0;

    for (;;) {
        int mx, my, mb, wheel;
        get_mouse(&mx, &my, &mb, &wheel);

        refresh_cnt++;
        if (files_dirty || (refresh_cnt & 127) == 0) refresh_files();

        struct aos_msg m;
        while (recv_msg(&m) == 0) {
            switch (m.type) {
            case MSG_KEY:
                if (dlg_open) {
                    unsigned int k = m.a;
                    if (k == '\r' || k == 27) {          // Enter / Esc
                        if (k == '\r') do_create();
                        dlg_open = 0;
                        redraw = 1;
                    } else if (k == '\b') {
                        if (dlg_len > 0) { dlg_len--; dlg_name[dlg_len] = 0; }
                        redraw = 1;
                    } else if (k >= 0x20 && k < 0x7F) {  // printable ASCII name
                        int cap = (dlg_mode == 1) ? 26 : 27;
                        if (dlg_len < cap) {
                            dlg_name[dlg_len++] = (char)k;
                            dlg_name[dlg_len] = 0;
                        }
                        redraw = 1;
                    }
                    break;
                }
                if (focus_pid) {
                    struct aos_msg k = {MSG_KEY, m.a, 0, 0, 0};
                    send_msg(focus_pid, &k);
                }
                break;
            case MSG_CREATE: {
                int launched = 0;
                for (int i = 0; i < 2; i++)
                    if (launchers[i].pid && launchers[i].pid == m.c) {
                        launchers[i].running = 1;
                        launched = 1;
                    }
                int wid, slab;
                if (alloc_window(m.c, (int)m.a, (int)m.b, &wid, &slab) == 0) {
                    struct aos_msg r = {MSG_WININFO, (unsigned int)wid,
                                        (unsigned int)slab, 0, 0};
                    send_msg(m.c, &r);
                    if (launched || !focus_pid) focus_pid = m.c;
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
            if (menu_open) {
                int it = menu_item_at(mx, my);
                menu_open = 0;
                redraw = 1;
                if (it >= 0) {
                    dlg_open = 1;
                    dlg_mode = it;                  // 0 = file, 1 = folder
                    dlg_len = 0;
                    dlg_name[0] = 0;
                }
            } else if (dlg_open) {
                // clicks are ignored while the dialog is open
            } else if (!dock_hit(mx, my)) {
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
                    } else {
                        int fi = icon_at(mx, my);
                        if (fi >= 0) open_file(fi);
                    }
                }
            }
        }
        if (moved && (mb & 2) && !(last_mb & 2)) {
            if (dlg_open) {
                // keep the dialog open, ignore right clicks
            } else if (menu_open) {
                menu_open = 0;
                redraw = 1;
            } else if (!dock_hit(mx, my) && win_index_at(mx, my) < 0) {
                menu_x = mx;
                menu_y = my;
                menu_open = 1;
                redraw = 1;
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
        if (!has_cur) need_cursor = 1;
        if (redraw) {
            composite();
            has_cur = 0;
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
