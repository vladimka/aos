#include <sched.h>
#include <stdio.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <unistd.h>
#include "aosabi.h"
#include "ico.h"
#include "theme.h"

#define MAX_WINDOWS 8
#define TITLE_H     18
#define BORDER      1
#define CUR_R       8           // cursor snapshot half-size

#define COL_DESKTOP      0x1A2030
#define COL_TITLE_TEXT   0xFFFFFF
#define COL_CURSOR       0xFFFFFF

// Themed colors; overridden from sys/config.cfg if present.
static unsigned int wp_top = COL_DESKTOP;
static unsigned int wp_bot = 0x0E1620;
static unsigned int col_title = 0x263C5E;
static unsigned int col_title_focus = 0x4E86C7;
static unsigned int col_border = 0x12161F;
static unsigned int col_border_focus = 0x6B9BD2;
static unsigned int col_dock_bg = 0x232C40;
static unsigned int col_accent = 0x5B93D8;

// ---- dock ----------------------------------------------------------------

#define DOCK_H       52
#define DOCK_MARGIN  8
#define DOCK_PAD_X   12
#define DOCK_PAD_Y   10
#define DOCK_ICON    32
#define DOCK_STRIDE  40

// ---- top panel -----------------------------------------------------------

#define PANEL_H      26
#define PANEL_BTN    24
#define PMENU_W      140

static unsigned int col_icon_fg = 0xFFFFFF;

// 32x32 two-color icons: 'X' = foreground pixel, 'O' = accent pixel, anything
// else = transparent.
static const char icon_term[32][33] = {
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X.......O......................X",
    "X........O.....................X",
    "X.........O....................X",
    "X..........O...................X",
    "X..........O...................X",
    "X.........O....................X",
    "X........O.....................X",
    "X.......O......................X",
    "X......OOOOOOO.................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X.........OOOOOOOOOOO..........X",
    "X.........OOOOOOOOOOO..........X",
    "X.........OOOOOOOOOOO..........X",
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
    "..X...........OOO............X..",
    "..X...........OOO............X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X............OO............X..",
    "..X............OO............X..",
    "..X............OO............X..",
    "..X............OO............X..",
    "..X............OO............X..",
    "..X.OO.........OO.........OO.X..",
    "..X.OO.........OO.........OO.X..",
    "..X.OO..OOOOOOOOOOOOOOOO..OO.X..",
    "..X.....OOOOOOOOOOOOOOOO.....X..",
    "..X............OO............X..",
    "..X............OO............X..",
    "..X............OO............X..",
    "..X............OO............X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X...........OOO............X..",
    "..X...........OOO............X..",
    "..X..........................X..",
    "..X..........................X..",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "................................",
    "................................",
    "................................",
    "................................",
};

static const char icon_unknown[32][33] = {
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "X..............................X",
    "X..............................X",
    "X..XXXXXXXXXXXXXXXXXXXXXXXXXX..X",
    "X..X........................X..X",
    "X..X........................X..X",
    "X..X........................X..X",
    "X..X.....OOOOOOOO...........X..X",
    "X..X.............O..........X..X",
    "X..X.............O..........X..X",
    "X..X........OOOOO...........X..X",
    "X..X........O...............X..X",
    "X..X........O...............X..X",
    "X..X........O...............X..X",
    "X..X.........OOO............X..X",
    "X..X...........O............X..X",
    "X..X...........O............X..X",
    "X..X........................X..X",
    "X..X........................X..X",
    "X..X........................X..X",
    "X..X...........O............X..X",
    "X..X...........O............X..X",
    "X..X........................X..X",
    "X..X........................X..X",
    "X..X........................X..X",
    "X..X........................X..X",
    "X..X........................X..X",
    "X..X........................X..X",
    "X..XXXXXXXXXXXXXXXXXXXXXXXXXX..X",
    "X..............................X",
    "X..............................X",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
};

static const char icon_folder[32][33] = {
    "................................",
    "................................",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "..X..........................X..",
    "..XOOOOOOOOOOOOOOOOOOOOOOOOOOX..",
    "..XOOOOOOOOOOOOOOOOOOOOOOOOOOX..",
    "..XOOOOOOOOOOOOOOOOOOOOOOOOOOX..",
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
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "................................",
    "................................",
    "................................",
    "................................",
};

static const char icon_file[32][33] = {
    "................................",
    "................................",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X...OOOOOOOOOOOOOOOOOOO....X..",
    "..X...OOOOOOOOOOOOOOOOOOO....X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X...OOOOOOOOOOOOOOOOOOO....X..",
    "..X...OOOOOOOOOOOOOOOOOOO....X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X...OOOOOOOOOOOOOOOOOOO....X..",
    "..X...OOOOOOOOOOOOOOOOOOO....X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X...OOOOOOOOOOOOO..........X..",
    "..X...OOOOOOOOOOOOO..........X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "................................",
    "................................",
};

static const char icon_image[32][33] = {
    "................................",
    "................................",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "..X..........................X..",
    "..X..........................X..",
    "..X...OOOOO..................X..",
    "..X...OOOOO..................X..",
    "..X...OOOOO..................X..",
    "..X...OOOOO..................X..",
    "..X...OOOOO..O...............X..",
    "..X.........O.O..............X..",
    "..X........O...O.............X..",
    "..X.......O.....OOOO.........X..",
    "..X......O......OO..O........X..",
    "..X.....O......O..O..O.......X..",
    "..X....O......O....O..O......X..",
    "..X...O......O......O..O.....X..",
    "..X..O......O........O..O....X..",
    "..X.O......O..........O..OOOOX..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
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
static unsigned int fb_front;   // buffer currently being scanned out
static int back_dirty;          // partial composites started on this back buffer
static unsigned int *scratch;   // slab 0: scratch for title-bar text rendering
static int next_slab = 1;
static unsigned int focus_pid;
static int redraw = 1;
static int gpu_mode;
static int has_cur, cur_x, cur_y;
static unsigned int snap[2 * CUR_R][2 * CUR_R];
static int clip_x0, clip_y0, clip_x1, clip_y1;

// ---- popup menus + create dialog state (used by composite_rect) -----------

#define MENU_W      176
#define MENU_ITEM_H 22
#define MENU_BORDER 1

static unsigned int col_menu_bg = 0x20283A;
static unsigned int col_menu_fg = 0xFFFFFF;

struct popmenu {
    int open;
    int x, y;           // requested position
    int draw_x, draw_y; // clamped position
    int n;
    int w;
    const char *const *items;
    int last_hover;
};

static int g_mx, g_my;              // last mouse position (for hover)
static int dlg_open, dlg_mode;      // mode 0 = file, 1 = folder
static char dlg_name[40];
static int dlg_len;
static int dlg_draw_x, dlg_draw_y;

static const char *const ctx_items[2] = {
    "\xd0\x9d\xd0\xbe\xd0\xb2\xd1\x8b\xd0\xb9 \xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb",   // Новый файл
    "\xd0\x9d\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x8f \xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0", // Новая папка
};
static const char *const pwr_items[2] = {
    "\xd0\x92\xd1\x8b\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb8\xd1\x82\xd1\x8c",   // Выключить
    "\xd0\x9f\xd0\xb5\xd1\x80\xd0\xb5\xd0\xb7\xd0\xb0\xd0\xb3\xd1\x80\xd1\x83\xd0\xb7\xd0\xb8\xd1\x82\xd1\x8c", // Перезагрузить
};

static struct popmenu ctx_menu = {0, 0, 0, 0, 0, 2, MENU_W, ctx_items, -1};
static struct popmenu pwr_menu = {0, 0, 0, 0, 0, 2, PMENU_W, pwr_items, -1};

// ---- small helpers -------------------------------------------------------

static void mcpy(unsigned int *d, const unsigned int *s, unsigned int n) {
    while (n >= 4) {
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        d += 4; s += 4; n -= 4;
    }
    while (n--) *d++ = *s++;
}

// Full-buffer copy for the GPU front->back restore before partial redraws.
// rep movsl: QEMU TCG runs it several times faster than a scalar loop.
static void fb_copy_full(unsigned int src, unsigned int dst) {
    unsigned int n = fb_h * (fb_pitch >> 2);
    __asm__ __volatile__("cld; rep movsl"
                         : "+S"(src), "+D"(dst), "+c"(n)
                         : : "memory");
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

static void fb_hline(int x, int y, int x1, unsigned int rgb) {
    if (y < clip_y0 || y >= clip_y1) return;
    if (x < clip_x0) x = clip_x0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (x >= x1) return;
    unsigned int *fb = (unsigned int *)fb_addr;
    unsigned int pitch = fb_pitch >> 2;
    for (; x < x1; x++) fb[(unsigned)y * pitch + (unsigned)x] = rgb;
}

// Return c lightened by amt/16 toward white, per channel.
static unsigned int lighten(unsigned int c, unsigned int amt) {
    unsigned int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    r += ((0xFF - r) * amt) >> 4;
    g += ((0xFF - g) * amt) >> 4;
    b += ((0xFF - b) * amt) >> 4;
    return (r << 16) | (g << 8) | b;
}

// Fill rect (x,y,w,h) with rgb, cutting a stair-stepped r-pixel L-shape from
// the two top corners. Corner pixels are left untouched (background shows).
// Radius-r stair: row dy (0..r-1) cuts (r-1-dy) px from each edge.
static void fb_round_fill_top(int x, int y, int w, int h, int r,
                              unsigned int rgb) {
    for (int yy = 0; yy < h; yy++) {
        int cut = (yy < r) ? (r - 1 - yy) : 0;
        fb_hline(x + cut, y + yy, x + w - cut, rgb);
    }
}

// Same as fb_round_fill_top, rounding all four corners.
static void fb_round_fill(int x, int y, int w, int h, int r,
                          unsigned int rgb) {
    for (int yy = 0; yy < h; yy++) {
        int cut;
        if (yy < r) cut = r - 1 - yy;
        else if (yy >= h - r) cut = r - 1 - (h - 1 - yy);
        else cut = 0;
        fb_hline(x + cut, y + yy, x + w - cut, rgb);
    }
}

// Vertical gradient from wp_top to wp_bot over the full screen height.
static void draw_desktop_gradient(int x0, int y0, int x1, int y1) {
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return;
    unsigned int *fb = (unsigned int *)fb_addr;
    unsigned int pitch = fb_pitch >> 2;
    int fbh = (int)fb_h;
    if (fbh <= 0) fbh = h;
    int rt = (int)((wp_top >> 16) & 0xFF), gt = (int)((wp_top >> 8) & 0xFF),
        bt = (int)(wp_top & 0xFF);
    int rb = (int)((wp_bot >> 16) & 0xFF), gb = (int)((wp_bot >> 8) & 0xFF),
        bb = (int)(wp_bot & 0xFF);
    int dr = (((rb - rt) << 16) / fbh);
    int dg = (((gb - gt) << 16) / fbh);
    int db = (((bb - bt) << 16) / fbh);
    int cr = (rt << 16) + dr * y0;
    int cg = (gt << 16) + dg * y0;
    int cb = (bt << 16) + db * y0;
    for (int y = 0; y < h; y++) {
        unsigned int rgb = ((unsigned int)((cr >> 16) & 0xFF) << 16) |
                           ((unsigned int)((cg >> 16) & 0xFF) << 8) |
                           ((unsigned int)((cb >> 16) & 0xFF));
        unsigned int *row = fb + (unsigned)(y0 + y) * pitch + (unsigned)x0;
        for (int x = 0; x < w; x++) row[x] = rgb;
        cr += dr; cg += dg; cb += db;
    }
}

static void fb_put(int x, int y, unsigned int rgb);
static void draw_close_btn(const struct win *wn);
static int cursor_overlaps(int x, int y, int w, int h);
static void draw_dock(void);
static int dock_x0(void);
static int dock_y0(void);
static int dock_width(void);
static int app_type_of(unsigned int pid);
static void raise_pid(unsigned int pid);
static void draw_desktop_icons(void);
static int menu_item_at(struct popmenu *m, int mx, int my);
static void draw_menu(struct popmenu *m, int mx, int my);
static void draw_dialog(void);
static void draw_panel(void);
static int pwr_btn_at(int mx, int my);

static void draw_title(const struct win *wn) {
    unsigned int tcol = (wn->pid == focus_pid) ? col_title_focus : col_title;
    fb_fill(wn->x + BORDER, wn->y + BORDER, wn->cw, TITLE_H, tcol);
    fb_fill(wn->x + BORDER, wn->y + BORDER, wn->cw, 1,
            lighten(tcol, 4));                       // lighter top strip
    fb_fill(wn->x + BORDER, wn->y + BORDER + 1, wn->cw, 1,
            lighten(tcol, 2));                       // mid strip
    char buf[8];
    int2str(buf, wn->winid);
    unsigned int *sc = scratch;
    aos_render_text(sc, 1024 * 4, 0, 0, buf, COL_TITLE_TEXT, tcol);
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
    // GPU mode: the back buffer is a stale frame from two flips ago, so a
    // partial redraw leaves ghosts of a moved window's previous position.
    // Restore the back from the visible front first (once per fresh back,
    // unless this redraw already covers the whole screen).
    if (gpu_mode && fb_front && !back_dirty &&
        !(x0 == 0 && y0 == 0 && x1 == (int)fb_w && y1 == (int)fb_h)) {
        fb_copy_full(fb_front, fb_addr);
        back_dirty = 1;
    }
    clip_x0 = x0; clip_y0 = y0; clip_x1 = x1; clip_y1 = y1;
    draw_desktop_gradient(x0, y0, x1, y1);
    draw_desktop_icons();
    for (int k = 0; k < nz; k++) {
        struct win *wn = &wins[zorder[k]];
        if (!wn->used) continue;
        if (wn->x < x1 && wn->x + wn->cw + 2 * BORDER > x0 &&
            wn->y < y1 && wn->y + wn->ch + TITLE_H + 2 * BORDER > y0) {
            unsigned int bcol = (wn->pid == focus_pid) ? col_border_focus
                                                       : col_border;
            fb_round_fill_top(wn->x, wn->y, wn->cw + 2 * BORDER,
                              wn->ch + TITLE_H + 2 * BORDER, 4, bcol);
            draw_title(wn);
            blit_content(wn);
        }
    }
    clip_x0 = 0; clip_y0 = 0;
    clip_x1 = (int)fb_w; clip_y1 = (int)fb_h;
    draw_dock();
    draw_panel();
    draw_menu(&ctx_menu, g_mx, g_my);
    draw_menu(&pwr_menu, g_mx, g_my);
    draw_dialog();
    if (cursor_overlaps(dock_x0(), dock_y0(), dock_width(), DOCK_H))
        has_cur = 0;
    if (cursor_overlaps(0, 0, (int)fb_w, PANEL_H))
        has_cur = 0;
    if (ctx_menu.open &&
        cursor_overlaps(ctx_menu.draw_x, ctx_menu.draw_y, ctx_menu.w,
                        ctx_menu.n * MENU_ITEM_H + 2 * MENU_BORDER))
        has_cur = 0;
    if (pwr_menu.open &&
        cursor_overlaps(pwr_menu.draw_x, pwr_menu.draw_y, pwr_menu.w,
                        pwr_menu.n * MENU_ITEM_H + 2 * MENU_BORDER))
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

static void draw_icon2(int x, int y, const char art[32][33],
                       unsigned int fg, unsigned int accent) {
    for (int r = 0; r < 32; r++)
        for (int c = 0; c < 32; c++) {
            char ch = art[r][c];
            if (ch == 'X') fb_put(x + c, y + r, fg);
            else if (ch == 'O') fb_put(x + c, y + r, accent);
        }
}

static void draw_dock(void) {
    int dx0 = dock_x0(), dy0 = dock_y0();
    int dw = dock_width();
    fb_round_fill_top(dx0, dy0, dw, DOCK_H, 6, col_dock_bg);
    fb_hline(dx0 + 5, dy0, dx0 + dw - 6, col_accent);            // accent line
    fb_hline(dx0 + 5, dy0 + 1, dx0 + dw - 6, lighten(col_dock_bg, 2));
    fb_hline(dx0 + 5, dy0 + 2, dx0 + dw - 6, lighten(col_dock_bg, 1));
    int iy = dy0 + DOCK_PAD_Y;
    draw_icon2(dx0 + DOCK_PAD_X, iy, icon_term, col_icon_fg, col_accent);
    draw_icon2(dx0 + DOCK_PAD_X + DOCK_STRIDE, iy, icon_clock,
               col_icon_fg, col_accent);
    int di = 2;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wins[i].used) continue;
        const char (*ic)[DOCK_ICON + 1];
        if (wins[i].app == APP_TERM) ic = icon_term;
        else if (wins[i].app == APP_CLOCK) ic = icon_clock;
        else ic = icon_unknown;
        int ix = dx0 + DOCK_PAD_X + di * DOCK_STRIDE;
        draw_icon2(ix, iy, ic, col_icon_fg, col_accent);
        unsigned int *fb = (unsigned int *)fb_addr;
        unsigned int pitch = fb_pitch >> 2;
        int dotx = ix + DOCK_ICON / 2 - 2;
        int doty = iy + DOCK_ICON + 4;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                fb[(unsigned)(doty + r) * pitch + (unsigned)(dotx + c)] = col_accent;
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
    int pid = aos_spawn(launchers[i].path, "", (unsigned int)getpid());
    if (pid > 0) {
        launchers[i].pid = (unsigned int)pid;
    } else {
        printf("wm: dock spawn failed\n");
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
#define GRID_Y0   (PANEL_H + 8)
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

// Render a UTF-8 string into the FB at (x, y) with clipping.
static void fb_text(int x, int y, const char *s, unsigned int fg,
                    unsigned int bg) {
    aos_render_text(scratch, 1024 * 4, 0, 0, s, fg, bg);
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
    DIR *d = opendir("/");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (nfiles >= 64) break;
            char *nm = e->d_name;
            if (nm[0] == 0) continue;
            // Programs live in the dock; keep the desktop for data files.
            if (nm[0] == 'b' && nm[1] == 'i' && nm[2] == 'n' && nm[3] == 0)
                continue;
            if (nm[0] == 'l' && nm[1] == 'i' && nm[2] == 'n' && nm[3] == 0)
                continue;
            if (nm[0] == 's' && nm[1] == 'y' && nm[2] == 's' && nm[3] == 0)
                continue;
            int len = 0;
            while (nm[len]) len++;
            int kind;
            struct stat st;
            char full[64];
            int fi = 0;
            full[fi++] = '/';
            for (int k = 0; nm[k] && k < 62; k++) full[fi++] = nm[k];
            full[fi] = 0;
            if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) kind = K_FOLDER;
            else if (ext_match(nm, ".ico")) kind = K_ICO;
            else if (ext_match(nm, ".txt")) kind = K_TEXT;
            else kind = K_OTHER;
            int j;
            for (j = 0; j < 27 && nm[j]; j++) files[nfiles].name[j] = nm[j];
            files[nfiles].name[j] = 0;
            files[nfiles].kind = kind;
            nfiles++;
        }
        closedir(d);
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
    int fd = open(files[i].name, O_RDONLY);
    int sz = fd < 0 ? -1 : read(fd, ico_buf, sizeof(ico_buf));
    if (fd >= 0) close(fd);
    if (sz > 0 &&
        ico_decode((const unsigned char *)ico_buf, (unsigned int)sz,
                   ICON_W, ICON_H, &dw, &dh, px) == 0) {
        for (int r = 0; r < ICON_W; r++)
            for (int c = 0; c < ICON_W; c++) {
                unsigned int p = px[r * ICON_W + c];
                if (p & 0xFF000000) fb_put(x + c, y + r, p & 0x00FFFFFF);
            }
    } else {
        draw_icon2(x, y, icon_image, col_icon_fg, col_accent);
    }
}

static void draw_desktop_icons(void) {
    for (int i = 0; i < nfiles; i++) {
        int x, y;
        icon_rect(i, &x, &y);
        if (x + ICON_W < clip_x0 || x >= clip_x1) continue;
        if (y + ICON_H < clip_y0 || y >= clip_y1) continue;
        switch (files[i].kind) {
        case K_FOLDER: draw_icon2(x, y, icon_folder, col_icon_fg, col_accent); break;
        case K_ICO:    draw_ico_file(i, x, y); break;
        case K_TEXT:   draw_icon2(x, y, icon_file, col_icon_fg, col_accent); break;
        default:       draw_icon2(x, y, icon_unknown, col_icon_fg, col_accent); break;
        }
        if (y + ICON_H + 2 < clip_y1)
            fb_text(x, y + ICON_H + 2, files[i].name, col_icon_fg,
                    wp_top);
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
    aos_spawn("bin/notepad", files[i].name, (unsigned int)getpid());
}

// ---- top panel (clock + power button) -------------------------------------

// 16x16 power symbol: 'X' = white ring, 'O' = accent stroke/line.
static const char icon_power[16][17] = {
    "................",
    ".......OO.......",
    ".......OO.......",
    ".......OO.......",
    "......OOOO......",
    ".....X....X.....",
    "....XX....XX....",
    "...XX......XX...",
    "...XX......XX...",
    "..XX........XX..",
    "..XX........XX..",
    "..XX........XX..",
    "..XX........XX..",
    "...XX......XX...",
    "....XX....XX....",
    ".....XXXXXX.....",
};

static char p_date[16];
static char p_time[16];
static int pwr_hover;
static unsigned int p_last_sec = 0xFFFFFFFFu;

static int pwr_btn_x0(void) { return (int)fb_w - 8 - PANEL_BTN; }
static int pwr_btn_y0(void) { return (PANEL_H - PANEL_BTN) / 2; }

static int pwr_btn_at(int mx, int my) {
    int bx = pwr_btn_x0(), by = pwr_btn_y0();
    return mx >= bx && mx < bx + PANEL_BTN && my >= by && my < by + PANEL_BTN;
}

static void draw_power_icon(int x, int y) {
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 16; c++) {
            char ch = icon_power[r][c];
            if (ch == 'X') fb_put(x + c, y + r, col_icon_fg);
            else if (ch == 'O') fb_put(x + c, y + r, col_accent);
        }
}

static void draw_panel(void) {
    fb_fill(0, 0, (int)fb_w, PANEL_H, col_dock_bg);
    fb_hline(0, PANEL_H - 1, (int)fb_w, col_accent);
    fb_hline(0, PANEL_H - 2, (int)fb_w, lighten(col_dock_bg, 2));
    int date_w = 10 * 8, time_w = 8 * 8, gap = 8;
    int x0 = ((int)fb_w - (date_w + gap + time_w)) / 2;
    int y = (PANEL_H - 16) / 2;
    fb_text(x0, y, p_date, lighten(col_dock_bg, 8), col_dock_bg);
    fb_text(x0 + date_w + gap, y, p_time, col_icon_fg, col_dock_bg);
    int bx = pwr_btn_x0(), by = pwr_btn_y0();
    unsigned int bcol = pwr_hover ? lighten(col_dock_bg, 6) : col_dock_bg;
    fb_round_fill(bx, by, PANEL_BTN, PANEL_BTN, 3, bcol);
    fb_round_fill(bx + 1, by + 1, PANEL_BTN - 2, PANEL_BTN - 2, 2,
                  lighten(bcol, 1));
    draw_power_icon(bx + 4, by + 4);
}

// ---- desktop context menu + create dialog ---------------------------------

static void draw_menu(struct popmenu *m, int mx, int my) {
    if (!m->open) return;
    int x = m->x, y = m->y;
    int mw = m->w;
    int mh = m->n * MENU_ITEM_H + 2 * MENU_BORDER;
    if (x + mw > (int)fb_w) x = (int)fb_w - mw;
    if (y + mh > (int)fb_h) y = (int)fb_h - mh;
    m->draw_x = x;
    m->draw_y = y;
    fb_round_fill(x, y, mw, mh, 3, col_accent);
    fb_round_fill(x + MENU_BORDER, y + MENU_BORDER, mw - 2 * MENU_BORDER,
                  mh - 2 * MENU_BORDER, 2, col_menu_bg);
    int hi = menu_item_at(m, mx, my);
    for (int i = 0; i < m->n; i++) {
        int iy = y + MENU_BORDER + i * MENU_ITEM_H;
        unsigned int bg = col_menu_bg;
        if (i == hi) {
            fb_fill(x + MENU_BORDER + 2, iy, mw - 2 * MENU_BORDER - 4,
                    MENU_ITEM_H, col_accent);
            bg = col_accent;
        }
        fb_text(x + 10, iy + 3, m->items[i], col_menu_fg, bg);
    }
}

static int menu_item_at(struct popmenu *m, int mx, int my) {
    if (!m->open) return -1;
    int x = m->draw_x, y = m->draw_y;
    if (mx < x || mx >= x + m->w) return -1;
    if (my < y || my >= y + m->n * MENU_ITEM_H + 2 * MENU_BORDER) return -1;
    int i = (my - (y + MENU_BORDER)) / MENU_ITEM_H;
    if (i < 0 || i >= m->n) return -1;
    return i;
}

static void do_create(void) {
    if (dlg_len == 0) return;
    char full[30];
    int i;
    for (i = 0; i < dlg_len; i++) full[i] = dlg_name[i];
    full[i] = 0;
    if (dlg_mode == 1) {
        mkdir(full, 0777);                 // real directory
    } else {
        int fd = open(full, O_CREAT | O_WRONLY);
        if (fd >= 0) close(fd);
    }
    files_dirty = 1;
}

static void draw_dialog(void) {
    if (!dlg_open) return;
    int x = (int)fb_w / 2 - 180;
    int y = (int)fb_h / 3;
    dlg_draw_x = x;
    dlg_draw_y = y;
    fb_round_fill(x, y, 360, 88, 3, col_accent);
    fb_round_fill(x + 1, y + 1, 358, 86, 2, col_menu_bg);
    const char *title = dlg_mode
        ? "\xd0\x98\xd0\xbc\xd1\x8f \xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbe\xd0\xb9 \xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb8:"
        : "\xd0\x98\xd0\xbc\xd1\x8f \xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbe\xd0\xb3\xd0\xbe \xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xb0:";
    fb_text(x + 10, y + 8, title, col_menu_fg, col_menu_bg);
    int bx = x + 10, by = y + 36, bw = 340, bh = 20;
    fb_fill(bx, by, bw, bh, 0x101010);
    if (dlg_len)
        fb_text(bx + 4, by + 2, dlg_name, col_menu_fg, 0x101010);
    int cx = bx + 4 + dlg_len * 8;      // dialog names are ASCII only
    fb_fill(cx, by + 3, 2, 14, col_menu_fg);
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
    if (gpu_mode) {
        int vis = (mx >= 0 && mx < (int)fb_w && my >= 0 && my < (int)fb_h);
        aos_cursor(mx, my, vis);
        has_cur = 0;
        return;
    }
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

// Present the back buffer to the display. After the flip front/back swap, so
// re-fetch the new back address before the next frame is drawn.
static void gpu_present(void) {
    if (!gpu_mode) return;
    fb_front = fb_addr;          // this buffer becomes the scanout
    aos_gpu_flip();
    unsigned int a, w, h, p, act;
    aos_fb_info(&a, &w, &h, &p, &act);
    fb_addr = a;
    fb_pitch = p;
    back_dirty = 0;              // fresh back holds the previous visible frame
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
        wins[i].y = PANEL_H + 8 + i * 28;
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

int main(void) {
    unsigned int bpp;
    aos_fb_info(&fb_addr, &fb_w, &fb_h, &fb_pitch, &bpp);
    clip_x1 = (int)fb_w;
    clip_y1 = (int)fb_h;
    printf("wm: fb=%x w=%u h=%u pitch=%u bpp=%u\n",
           fb_addr, fb_w, fb_h, fb_pitch, bpp);
    unsigned int gpu_w = 0, gpu_h = 0, gpu_pitch = 0, gpu_active = 0;
    unsigned int gpu_back = 0;
    aos_gpu_info(&gpu_back, &gpu_w, &gpu_h, &gpu_pitch, &gpu_active);
    if (gpu_active) {
        fb_addr = gpu_back;
        fb_w = gpu_w; fb_h = gpu_h; fb_pitch = gpu_pitch;
        gpu_mode = 1;
        printf("wm: gpu mode, back=%x\n", fb_addr);
    } else {
        printf("wm: vga mode\n");
    }
    fflush(stdout);
    if (bpp != 32) {
        printf("wm: framebuffer is not 32bpp\n");
        return 1;
    }
    scratch = (unsigned int *)AOS_SLAB_BASE;
    aos_register_events();
    theme_load();
    wp_top = theme_color("wallpaper_top", 0x1A2030);
    wp_bot = theme_color("wallpaper_bot", 0x0E1620);
    col_title = theme_color("theme_title", 0x263C5E);
    col_title_focus = theme_color("theme_title_focus", 0x4E86C7);
    col_border = theme_color("theme_border", 0x12161F);
    col_border_focus = theme_color("theme_border_focus", 0x6B9BD2);
    col_dock_bg = theme_color("theme_dock_bg", 0x232C40);
    col_accent = theme_color("theme_accent", 0x5B93D8);

    int last_mx = 0, last_my = 0, last_mb = 0;
    int drag_i = -1, drag_dx = 0, drag_dy = 0;

    for (;;) {
        int mx, my, mb, wheel;
        int drew = 0;           // any composite happened this iteration
        aos_mouse(&mx, &my, &mb, &wheel);
        g_mx = mx;
        g_my = my;
        if (ctx_menu.open && (mx != last_mx || my != last_my)) {
            int hi = menu_item_at(&ctx_menu, mx, my);
            if (hi != ctx_menu.last_hover) { ctx_menu.last_hover = hi; redraw = 1; }
        }
        if (pwr_menu.open && (mx != last_mx || my != last_my)) {
            int hi = menu_item_at(&pwr_menu, mx, my);
            if (hi != pwr_menu.last_hover) { pwr_menu.last_hover = hi; redraw = 1; }
        }
        int over_btn = pwr_btn_at(mx, my);
        if (over_btn != pwr_hover) { pwr_hover = over_btn; redraw = 1; }

        refresh_cnt++;
        if (files_dirty || (refresh_cnt & 127) == 0) refresh_files();

        // Refresh the panel clock once per second.
        unsigned int tk = aos_get_tick();
        if (tk / 1000 != p_last_sec) {
            p_last_sec = tk / 1000;
            struct aos_time t;
            if (aos_get_rtc(&t) == 0) {
                p_time[0] = '0' + t.hour / 10;  p_time[1] = '0' + t.hour % 10;
                p_time[2] = ':';
                p_time[3] = '0' + t.minute / 10; p_time[4] = '0' + t.minute % 10;
                p_time[5] = ':';
                p_time[6] = '0' + t.second / 10; p_time[7] = '0' + t.second % 10;
                p_time[8] = 0;
                p_date[0] = '0' + t.day / 10;    p_date[1] = '0' + t.day % 10;
                p_date[2] = '.';
                p_date[3] = '0' + t.month / 10;  p_date[4] = '0' + t.month % 10;
                p_date[5] = '.';
                p_date[6] = '0' + (t.year / 1000) % 10;
                p_date[7] = '0' + (t.year / 100) % 10;
                p_date[8] = '0' + (t.year / 10) % 10;
                p_date[9] = '0' + t.year % 10;
                p_date[10] = 0;
            }
            redraw = 1;
        }

        struct aos_msg m;
        while (aos_recv(&m) == 0) {
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
                    aos_send(focus_pid, &k);
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
                    aos_send(m.c, &r);
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
                    drew = 1;
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
            if (pwr_btn_at(mx, my)) {
                if (pwr_menu.open) {
                    pwr_menu.open = 0;
                    pwr_menu.last_hover = -1;
                } else {
                    pwr_menu.x = (int)fb_w - 8 - pwr_menu.w;
                    pwr_menu.y = PANEL_H + 2;
                    pwr_menu.open = 1;
                    pwr_menu.last_hover = -1;
                    ctx_menu.open = 0;
                    ctx_menu.last_hover = -1;
                }
                redraw = 1;
            } else if (pwr_menu.open) {
                int it = menu_item_at(&pwr_menu, mx, my);
                pwr_menu.open = 0;
                pwr_menu.last_hover = -1;
                redraw = 1;
                if (it >= 0) {
                    if (it == 0) {
                        printf("wm: shutdown\n");
                        fflush(stdout);
                        reboot(RB_POWER_OFF);
                    } else {
                        printf("wm: reboot\n");
                        fflush(stdout);
                        reboot(RB_AUTOBOOT);
                    }
                }
            } else if (ctx_menu.open) {
                int it = menu_item_at(&ctx_menu, mx, my);
                ctx_menu.open = 0;
                ctx_menu.last_hover = -1;
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
                    aos_send(wins[cb].pid, &cl);
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
            } else if (pwr_menu.open) {
                pwr_menu.open = 0;
                pwr_menu.last_hover = -1;
                redraw = 1;
            } else if (ctx_menu.open) {
                ctx_menu.open = 0;
                ctx_menu.last_hover = -1;
                redraw = 1;
            } else if (!dock_hit(mx, my) && win_index_at(mx, my) < 0 &&
                       !pwr_btn_at(mx, my)) {
                ctx_menu.x = mx;
                ctx_menu.y = my;
                ctx_menu.open = 1;
                ctx_menu.last_hover = -1;
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
            if (ny < PANEL_H) ny = PANEL_H;
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
                drew = 1;
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
            drew = 1;
            has_cur = 0;
            redraw = 0;
            need_cursor = 1;
        }
        if (need_cursor) {
            update_cursor(mx, my);
        }
        if (drew) gpu_present();
        last_mx = mx;
        last_my = my;
        last_mb = mb;
        sched_yield();
    }
}