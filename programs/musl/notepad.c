#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "gui.h"
#include "theme.h"

#define TW       80
#define NMAX     200
#define EDIT_H   25
#define TH       26
#define WINDOW_W (TW * FONT_W)
#define WINDOW_H (TH * FONT_H)

static unsigned int col_fg = 0xD8D8D8;
static unsigned int col_bg = 0x101010;
static unsigned int col_status_bg = 0x232C40;
static unsigned int col_status_fg = 0xD8D8D8;

static unsigned int lines[NMAX][TW];
static int llen[NMAX];
static int nlines = 1;
static int crow, ccol;
static int scroll;

static char fname[64];
static int w, h;

static char status_ovr[32];
static unsigned int status_until;

static int utf8_decode(const char *s, unsigned int *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0) {
        *cp = ((c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        *cp = ((c & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) |
              ((unsigned char)s[2] & 0x3F);
        return 3;
    }
    *cp = '?';
    return 1;
}

static void load_file(void) {
    int fd = open(fname, O_RDONLY);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return; }
    static char buf[NMAX * TW * 3 + 1];
    int size = (int)st.st_size;
    if (size > (int)sizeof(buf) - 1) size = (int)sizeof(buf) - 1;
    int got = read(fd, buf, size);
    close(fd);
    if (got <= 0) return;
    buf[got] = 0;
    nlines = 1; llen[0] = 0;
    int i = 0;
    while (buf[i] && nlines < NMAX) {
        unsigned int cp;
        i += utf8_decode(buf + i, &cp);
        if (cp == '\n') { nlines++; llen[nlines - 1] = 0; }
        else if (cp == '\r') continue;
        else if (llen[nlines - 1] < TW) {
            lines[nlines - 1][llen[nlines - 1]] = cp;
            llen[nlines - 1]++;
        }
    }
    crow = 0; ccol = 0; scroll = 0;
}

static int save_file(void) {
    static char buf[NMAX * (TW * 3 + 1)];
    int n = 0;
    for (int r = 0; r < nlines; r++) {
        for (int c = 0; c < llen[r]; c++)
            n += gui_utf8_encode(buf + n, lines[r][c]);
        if (r < nlines - 1) buf[n++] = '\n';
    }
    int fd = open(fname, O_CREAT | O_WRONLY | O_TRUNC);
    if (fd < 0) return -1;
    int rc = write(fd, buf, (unsigned int)n);
    close(fd);
    return rc < 0 ? rc : 0;
}

static void insert_cp(unsigned int cp) {
    if (llen[crow] >= TW) return;
    for (int c = llen[crow]; c > ccol; c--)
        lines[crow][c] = lines[crow][c - 1];
    lines[crow][ccol] = cp;
    llen[crow]++; ccol++;
}

static void do_enter(void) {
    if (nlines >= NMAX) return;
    for (int c = nlines; c > crow + 1; c--) {
        for (int i = 0; i < TW; i++) lines[c][i] = lines[c - 1][i];
        llen[c] = llen[c - 1];
    }
    int rem = llen[crow] - ccol;
    for (int c = 0; c < rem; c++)
        lines[crow + 1][c] = lines[crow][ccol + c];
    llen[crow + 1] = rem;
    llen[crow] = ccol;
    nlines++; crow++; ccol = 0;
}

static void do_backspace(void) {
    if (ccol > 0) {
        for (int c = ccol - 1; c < llen[crow] - 1; c++)
            lines[crow][c] = lines[crow][c + 1];
        llen[crow]--; ccol--;
    } else if (crow > 0) {
        int prev = llen[crow - 1];
        int rem = llen[crow];
        for (int c = 0; c < rem && prev + c < TW; c++)
            lines[crow - 1][prev + c] = lines[crow][c];
        llen[crow - 1] = prev + rem < TW ? prev + rem : TW;
        for (int c = crow; c < nlines - 1; c++) {
            for (int i = 0; i < TW; i++) lines[c][i] = lines[c + 1][i];
            llen[c] = llen[c + 1];
        }
        nlines--; crow--; ccol = prev;
        if (ccol > llen[crow]) ccol = llen[crow];
    }
}

static void do_del(void) {
    if (ccol < llen[crow]) {
        for (int c = ccol; c < llen[crow] - 1; c++)
            lines[crow][c] = lines[crow][c + 1];
        llen[crow]--;
    } else if (crow < nlines - 1) {
        int rem = llen[crow + 1];
        for (int c = 0; c < rem && llen[crow] + c < TW; c++)
            lines[crow][llen[crow] + c] = lines[crow + 1][c];
        llen[crow] = llen[crow] + rem < TW ? llen[crow] + rem : TW;
        for (int c = crow + 1; c < nlines - 1; c++) {
            for (int i = 0; i < TW; i++) lines[c][i] = lines[c + 1][i];
            llen[c] = llen[c + 1];
        }
        nlines--;
    }
}

static void move_left(void) {
    if (ccol > 0) ccol--;
    else if (crow > 0) { crow--; ccol = llen[crow]; }
}
static void move_right(void) {
    if (ccol < llen[crow]) ccol++;
    else if (crow < nlines - 1) { crow++; ccol = 0; }
}
static void move_up(void) {
    if (crow > 0) { crow--; if (ccol > llen[crow]) ccol = llen[crow]; }
}
static void move_down(void) {
    if (crow < nlines - 1) { crow++; if (ccol > llen[crow]) ccol = llen[crow]; }
}

static void ensure_visible(void) {
    if (crow < scroll) scroll = crow;
    if (crow >= scroll + EDIT_H) scroll = crow - EDIT_H + 1;
    int max_scroll = nlines - EDIT_H;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll > max_scroll) scroll = max_scroll;
    if (scroll < 0) scroll = 0;
}

static void build_status(char *dst) {
    if (status_ovr[0] && aos_get_tick() < status_until) {
        const char *s = status_ovr;
        while (*s) *dst++ = *s++;
        *dst = 0;
        return;
    }
    status_ovr[0] = 0;
    const char *p = "\xd0\xa4\xd0\xb0\xd0\xb9\xd0\xbb: ";
    while (*p) *dst++ = *p++;
    p = fname;
    while (*p) *dst++ = *p++;
    p = " [Ctrl+S \xd1\x81\xd0\xbe\xd1\x85\xd1\x80\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x82\xd1\x8c]";
    while (*p) *dst++ = *p++;
    *dst = 0;
}

static void do_render(void) {
    ensure_visible();
    gui_fill(0, 0, w, h, col_bg);

    static char tb[EDIT_H * (TW * 3 + 1) + 1];
    int pos = 0;
    for (int r = 0; r < EDIT_H; r++) {
        int li = scroll + r;
        if (li < nlines)
            for (int c = 0; c < llen[li]; c++)
                pos += gui_utf8_encode(tb + pos, lines[li][c]);
        tb[pos++] = '\n';
    }
    tb[pos] = 0;
    gui_text(0, 0, tb, col_fg, col_bg);

    if (crow >= scroll && crow < scroll + EDIT_H)
        gui_fill(ccol * FONT_W, (crow - scroll) * FONT_H + 14, FONT_W, 2, col_fg);

    gui_fill(0, EDIT_H * FONT_H, w, (TH - EDIT_H) * FONT_H, col_status_bg);
    char status[80];
    build_status(status);
    gui_text(4, EDIT_H * FONT_H, status, col_status_fg, col_status_bg);
    gui_update();
}

static void do_save(void) {
    int rc = save_file();
    const char *msg = rc < 0
        ? "\xd0\xbe\xd1\x88\xd0\xb8\xd0\xb1\xd0\xba\xd0\xb0"
        : "\xd1\x81\xd0\xbe\xd1\x85\xd1\x80\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbd\xd0\xbe";
    int i = 0;
    while (msg[i] && i < 31) { status_ovr[i] = msg[i]; i++; }
    status_ovr[i] = 0;
    status_until = aos_get_tick() + 200;
}

static void on_key(int k) {
    switch (k) {
    case 0x13: do_save(); break;
    case '\r': do_enter(); break;
    case '\b': do_backspace(); break;
    case 0x0107: do_del(); break;
    case 0x0101: move_up(); break;
    case 0x0102: move_down(); break;
    case 0x0103: move_left(); break;
    case 0x0104: move_right(); break;
    case 0x0105: ccol = 0; break;
    case 0x0106: ccol = llen[crow]; break;
    default:
        if (k >= 0x20 && k != 0x7F) insert_cp((unsigned int)k);
        break;
    }
}

static void copy_str(char *dst, int cap, const char *src) {
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void on_init(void) {
    col_fg = theme_color("theme_app_fg", 0x1E1E1E);
    col_bg = theme_color("theme_app_bg", 0xE8E8E8);
    col_status_bg = theme_color("theme_dock_bg", 0x232C40);
    col_status_fg = theme_color("theme_text_fg", 0xD8D8D8);
}

void main(int argc, char **argv) {
    if (argc > 1) copy_str(fname, sizeof(fname), argv[1]);
    else copy_str(fname, sizeof(fname), "untitled.txt");
    load_file();
    w = WINDOW_W;
    h = WINDOW_H;

    struct gui_app app = {
        .title = "Notepad",
        .width = WINDOW_W, .height = WINDOW_H,
        .init = on_init,
        .render = do_render,
        .on_key = on_key
    };
    gui_init(&app);
    gui_run();
}
