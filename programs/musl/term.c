#include <sched.h>
#include <unistd.h>
#include <errno.h>
#include "aosabi.h"
#include "theme.h"

#define TW  80
#define TH  26
#define FONT_W 8
#define FONT_H 16

static unsigned int col_fg = 0xD8D8D8;
static unsigned int col_bg = 0x101010;

static unsigned int screen[TH][TW];   // codepoints per cell (0 = empty)
static int crow, ccol;
static int cmd_col;                   // column where the command line starts
static unsigned int *win;             // window slab (content buffer)
static unsigned int winid;
static int w, h;                      // content size in pixels
static int child_active;
static char utfbuf[TH * (TW * 3 + 1) + 1];

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

static void newline(void) {
    if (crow < TH - 1) {
        crow++;
    } else {
        for (int r = 0; r < TH - 1; r++)
            for (int c = 0; c < TW; c++)
                screen[r][c] = screen[r + 1][c];
        for (int c = 0; c < TW; c++)
            screen[TH - 1][c] = 0;
    }
    ccol = 0;
}

static void put_char(unsigned int cp) {
    if (cp == '\r') return;
    if (cp == '\n') { newline(); return; }
    if (cp == '\b') {
        if (ccol > cmd_col) {
            ccol--;
            screen[crow][ccol] = 0;
        }
        return;
    }
    if (cp < 0x20 || cp == 0x7F) return;
    if (ccol >= TW) newline();
    screen[crow][ccol] = cp;
    ccol++;
}

static void prompt(void) {
    cmd_col = 0;
    for (int i = 0; i < 5; i++)
        put_char("aos> "[i]);
    cmd_col = ccol;
}

static void render(void) {
    aos_fill(win, (unsigned int)w * 4, 0, 0, w, h, col_bg);
    int pos = 0;
    for (int r = 0; r < TH; r++) {
        for (int c = 0; c < TW; c++) {
            unsigned int cp = screen[r][c];
            if (cp) pos += utf8_encode(utfbuf + pos, cp);
        }
        utfbuf[pos++] = '\n';
    }
    utfbuf[pos] = 0;
    aos_render_text(win, (unsigned int)w * 4, 0, 0, utfbuf, col_fg, col_bg);
    aos_fill(win, (unsigned int)w * 4, ccol * FONT_W, crow * FONT_H + 14,
             FONT_W, 2, col_fg);
    struct aos_msg m = {MSG_UPDATE, winid, 0, 0, 0};
    aos_send((unsigned int)aos_get_event_pid(), &m);
}

static void print_str(const char *s) {
    while (*s)
        put_char((unsigned int)(unsigned char)*s++);
}

static int read_cmd(char *out, int cap) {
    int n = 0;
    for (int c = cmd_col; c < TW; c++) {
        unsigned int cp = screen[crow][c];
        if (!cp) break;
        if (cp >= 0x100) return -1;
        if (n < cap - 1) out[n++] = (char)cp;
    }
    out[n] = 0;
    return n;
}

static void do_enter(void) {
    char cmd[256];
    int n = read_cmd(cmd, sizeof(cmd));
    put_char('\n');
    if (n <= 0) {
        prompt();
        render();
        return;
    }
    char *p = cmd;
    while (*p == ' ') p++;
    char *args = p;
    while (*args && *args != ' ') args++;
    int clen = args - p;
    if (clen <= 0) {
        prompt();
        render();
        return;
    }
    if (clen > 60) clen = 60;

    if (clen == 2 && p[0] == 'c' && p[1] == 'd') {
        char *a = args;
        while (*a == ' ') a++;
        if (!*a) {
            print_str("usage: cd <path>\n");
        } else if (chdir(a) != 0) {
            if (errno == EINVAL) {
                print_str("cd: bad path\n");
            } else {
                print_str("cd: no such directory: ");
                print_str(a);
                put_char('\n');
            }
        }
        prompt();
        render();
        return;
    }
    if (clen == 3 && p[0] == 'p' && p[1] == 'w' && p[2] == 'd') {
        char buf[256];
        if (getcwd(buf, sizeof(buf)) != NULL) {
            print_str(buf);
            put_char('\n');
        }
        prompt();
        render();
        return;
    }

    char path[70];
    int i = 0;
    for (const char *s = "bin/"; *s; s++) path[i++] = *s;
    for (int j = 0; j < clen; j++) path[i++] = p[j];
    path[i] = 0;

    child_active = 1;
    render();
    int pid = aos_spawn(path, args, (unsigned int)getpid());
    if (pid < 0) {
        i = 0;
        for (int j = 0; j < clen; j++) path[i++] = p[j];
        path[i] = 0;
        pid = aos_spawn(path, args, (unsigned int)getpid());
    }
    if (pid < 0) {
        static const char err[] = "cannot run command\n";
        for (int j = 0; err[j]; j++) put_char(err[j]);
        child_active = 0;
        prompt();
        render();
    }
}

static void handle_key(unsigned int key) {
    switch (key) {
    case '\r':
        if (!child_active) do_enter();
        render();
        break;
    case 0x0103:            // GUI_KEY_LEFT
        if (ccol > cmd_col) { ccol--; render(); }
        break;
    case 0x0104:            // GUI_KEY_RIGHT
        if (ccol < TW - 1 && screen[crow][ccol]) { ccol++; render(); }
        break;
    case 0x0105:            // GUI_KEY_HOME
        ccol = cmd_col;
        render();
        break;
    case 0x0106:            // GUI_KEY_END
        ccol = 0;
        while (ccol < TW - 1 && screen[crow][ccol]) ccol++;
        render();
        break;
    default:
        if (child_active) break;
        if (key < 0x100) {
            put_char(key);
            render();
        }
        break;
    }
}

static void handle_data(const struct aos_msg *m) {
    unsigned int n = m->a;
    if (n > 12) n = 12;
    unsigned int words[3] = {m->b, m->c, m->d};
    for (unsigned int i = 0; i < n; i++)
        put_char((unsigned char)(words[i / 4] >> (8 * (i % 4))));
    render();
}

int main(void) {
    unsigned int my = (unsigned int)getpid();
    struct aos_msg m = {MSG_CREATE, TW * FONT_W, TH * FONT_H, my, 0};
    aos_send((unsigned int)aos_get_event_pid(), &m);

    for (;;) {
        if (aos_recv(&m) == 0 && m.type == MSG_WININFO) {
            winid = m.a;
            unsigned int slab = m.b;
            win = (unsigned int *)(AOS_SLAB_BASE + slab * AOS_SLAB_SIZE);
            break;
        }
        sched_yield();
    }
    w = TW * FONT_W;
    h = TH * FONT_H;

    theme_load();
    col_fg = theme_color("theme_text_fg", 0xD8D8D8);
    col_bg = theme_color("theme_text_bg", 0x101010);

    prompt();
    render();

    for (;;) {
        if (aos_recv(&m) == 0) {
            switch (m.type) {
            case MSG_KEY:
                handle_key(m.a);
                break;
            case MSG_DATA:
                handle_data(&m);
                break;
            case MSG_EXIT:
                child_active = 0;
                put_char('\n');
                prompt();
                render();
                break;
            case MSG_CLOSE:
                return 0;
            }
        }
        sched_yield();
    }
}