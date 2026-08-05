#include "libaos.h"

#define FONT_W   8
#define FONT_H   16
#define TW       80
#define NMAX     200
#define EDIT_H   25                 // visible text rows
#define TH       26                 // EDIT_H + status bar row
#define WINDOW_W (TW * FONT_W)
#define WINDOW_H (TH * FONT_H)

#define COL_FG        0xD8D8D8
#define COL_BG        0x101010
#define COL_STATUS_BG 0x20283A
#define COL_STATUS_FG 0xE8EEF8

static unsigned int lines[NMAX][TW];   // codepoints per cell
static int llen[NMAX];                 // chars per line
static int nlines = 1;
static int crow, ccol;                 // cursor position
static int scroll;                     // first visible line index

static char fname[64];
static unsigned int *win;
static unsigned int winid;
static int w, h;

static char status_ovr[32];            // transient "сохранено"/"ошибка"
static unsigned int status_until;      // tick deadline for status_ovr

static int utf8_encode(char *out, unsigned int cp) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

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

// ---- file load / save ------------------------------------------------------

static void load_file(void) {
    int fd = sd_open(fname, O_RDONLY);
    if (fd < 0) return;
    struct aos_stat st;
    if (sd_fstat(fd, &st) != 0) {
        sd_close(fd);
        return;
    }
    static char buf[NMAX * TW * 3 + 1];
    int size = (int)st.size;
    if (size > (int)sizeof(buf) - 1) size = (int)sizeof(buf) - 1;
    int got = sd_read(fd, buf, size);
    sd_close(fd);
    if (got <= 0) return;
    buf[got] = 0;

    nlines = 1;
    llen[0] = 0;
    int i = 0;
    while (buf[i] && nlines < NMAX) {
        unsigned int cp;
        i += utf8_decode(buf + i, &cp);
        if (cp == '\n') {
            nlines++;
            llen[nlines - 1] = 0;
        } else if (cp == '\r') {
            continue;
        } else if (llen[nlines - 1] < TW) {
            lines[nlines - 1][llen[nlines - 1]] = cp;
            llen[nlines - 1]++;
        }
    }
    crow = 0;
    ccol = 0;
    scroll = 0;
}

static int save_file(void) {
    static char buf[NMAX * (TW * 3 + 1)];
    int n = 0;
    for (int r = 0; r < nlines; r++) {
        for (int c = 0; c < llen[r]; c++)
            n += utf8_encode(buf + n, lines[r][c]);
        if (r < nlines - 1) buf[n++] = '\n';
    }
    int fd = sd_open(fname, O_CREAT | O_WRONLY | O_TRUNC);
    if (fd < 0) return -1;
    int rc = sd_write(fd, buf, (unsigned int)n);
    sd_close(fd);
    return rc < 0 ? rc : 0;
}

// ---- text editing ops ------------------------------------------------------

static void insert_cp(unsigned int cp) {
    if (llen[crow] >= TW) return;
    for (int c = llen[crow]; c > ccol; c--)
        lines[crow][c] = lines[crow][c - 1];
    lines[crow][ccol] = cp;
    llen[crow]++;
    ccol++;
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
    nlines++;
    crow++;
    ccol = 0;
}

static void do_backspace(void) {
    if (ccol > 0) {
        for (int c = ccol - 1; c < llen[crow] - 1; c++)
            lines[crow][c] = lines[crow][c + 1];
        llen[crow]--;
        ccol--;
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
        nlines--;
        crow--;
        ccol = prev;
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

// ---- rendering --------------------------------------------------------------

static void ensure_visible(void) {
    if (crow < scroll) scroll = crow;
    if (crow >= scroll + EDIT_H) scroll = crow - EDIT_H + 1;
    int max_scroll = nlines - EDIT_H;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll > max_scroll) scroll = max_scroll;
    if (scroll < 0) scroll = 0;
}

static void build_status(char *dst) {
    if (status_ovr[0] && get_tick() < status_until) {
        const char *s = status_ovr;
        while (*s) *dst++ = *s++;
        *dst = 0;
        return;
    }
    status_ovr[0] = 0;
    // "Файл: <name> [Ctrl+S сохранить]"
    const char *p = "\xd0\xa4\xd0\xb0\xd0\xb9\xd0\xbb: ";
    while (*p) *dst++ = *p++;
    p = fname;
    while (*p) *dst++ = *p++;
    p = " [Ctrl+S \xd1\x81\xd0\xbe\xd1\x85\xd1\x80\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x82\xd1\x8c]";
    while (*p) *dst++ = *p++;
    *dst = 0;
}

static void render(void) {
    ensure_visible();
    fill_rect(win, (unsigned int)w * 4, 0, 0, w, h, COL_BG);

    static char tb[EDIT_H * (TW * 3 + 1) + 1];
    int pos = 0;
    for (int r = 0; r < EDIT_H; r++) {
        int li = scroll + r;
        if (li < nlines)
            for (int c = 0; c < llen[li]; c++)
                pos += utf8_encode(tb + pos, lines[li][c]);
        tb[pos++] = '\n';
    }
    tb[pos] = 0;
    render_text(win, (unsigned int)w * 4, 0, 0, tb, COL_FG, COL_BG);

    if (crow >= scroll && crow < scroll + EDIT_H)
        fill_rect(win, (unsigned int)w * 4, ccol * FONT_W,
                  (crow - scroll) * FONT_H + 14, FONT_W, 2, COL_FG);

    fill_rect(win, (unsigned int)w * 4, 0, EDIT_H * FONT_H, w,
              (TH - EDIT_H) * FONT_H, COL_STATUS_BG);
    char status[80];
    build_status(status);
    render_text(win, (unsigned int)w * 4, 4, EDIT_H * FONT_H, status,
                COL_STATUS_FG, COL_STATUS_BG);

    struct aos_msg m = {MSG_UPDATE, winid, 0, 0, 0};
    send_msg(get_event_pid(), &m);
}

// ---- keys -------------------------------------------------------------------

static void do_save(void) {
    int rc = save_file();
    const char *msg = rc < 0
        ? "\xd0\xbe\xd1\x88\xd0\xb8\xd0\xb1\xd0\xba\xd0\xb0"       // ошибка
        : "\xd1\x81\xd0\xbe\xd1\x85\xd1\x80\xd0\xb0\xd0\xbd\xd0\xb5\xd0\xbd\xd0\xbe"; // сохранено
    int i = 0;
    while (msg[i] && i < 31) { status_ovr[i] = msg[i]; i++; }
    status_ovr[i] = 0;
    status_until = get_tick() + 200;
}

static void handle_key(unsigned int k) {
    switch (k) {
    case 0x13:                  // Ctrl+S
        do_save();
        break;
    case '\r':
        do_enter();
        break;
    case '\b':
        do_backspace();
        break;
    case 0x0107:                // GUI_KEY_DEL
        do_del();
        break;
    case 0x0101:                // GUI_KEY_UP
        move_up();
        break;
    case 0x0102:                // GUI_KEY_DOWN
        move_down();
        break;
    case 0x0103:                // GUI_KEY_LEFT
        move_left();
        break;
    case 0x0104:                // GUI_KEY_RIGHT
        move_right();
        break;
    case 0x0105:                // GUI_KEY_HOME
        ccol = 0;
        break;
    case 0x0106:                // GUI_KEY_END
        ccol = llen[crow];
        break;
    default:
        if (k >= 0x20 && k != 0x7F) insert_cp(k);
        break;
    }
    render();
}

void main(void) {
    get_args(fname, sizeof(fname));
    if (fname[0] == 0) {
        const char *d = "untitled.txt";
        int i = 0;
        while (d[i]) { fname[i] = d[i]; i++; }
        fname[i] = 0;
    }
    load_file();

    unsigned int my = getpid();
    struct aos_msg m = {MSG_CREATE, WINDOW_W, WINDOW_H, my, 0};
    send_msg(get_event_pid(), &m);

    for (;;) {
        if (recv_msg(&m) == 0 && m.type == MSG_WININFO) {
            winid = m.a;
            unsigned int slab = m.b;
            win = (unsigned int *)(AOS_SLAB_BASE + slab * AOS_SLAB_SIZE);
            break;
        }
        yield();
    }
    w = WINDOW_W;
    h = WINDOW_H;
    render();

    for (;;) {
        if (recv_msg(&m) == 0) {
            switch (m.type) {
            case MSG_KEY:
                handle_key(m.a);
                break;
            case MSG_CLOSE:
                exit();
                break;
            }
        }
        yield();
    }
}
