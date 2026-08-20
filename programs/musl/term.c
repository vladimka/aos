#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include "aosabi.h"
#include "theme.h"

#ifndef FIONBIO
#define FIONBIO 0x5421
#endif

#define TW  80
#define TH  26
#define FONT_W 8
#define FONT_H 16

static unsigned int col_fg = 0xD8D8D8;
static unsigned int col_bg = 0x101010;

struct tcell { unsigned int cp; unsigned char fg; unsigned char bg; };
static struct tcell screen[TH][TW];
static unsigned char fg_idx = 15, bg_idx = 0;
static int crow, ccol;
static unsigned int *win;
static unsigned int winid;
static int w, h;
static char utfbuf[TH * (TW * 3 + 1) + 1];

static const unsigned int xterm_rgb[256] = {
    0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0,
    0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
    0x000000, 0x00005F, 0x000087, 0x0000AF, 0x0000D7, 0x0000FF, 0x005F00, 0x005F5F,
    0x005F87, 0x005FAF, 0x005FD7, 0x005FFF, 0x008700, 0x00875F, 0x008787, 0x0087AF,
    0x0087D7, 0x0087FF, 0x00AF00, 0x00AF5F, 0x00AF87, 0x00AFAF, 0x00AFD7, 0x00AFFF,
    0x00D700, 0x00D75F, 0x00D787, 0x00D7AF, 0x00D7D7, 0x00D7FF, 0x00FF00, 0x00FF5F,
    0x00FF87, 0x00FFAF, 0x00FFD7, 0x00FFFF, 0x5F0000, 0x5F005F, 0x5F0087, 0x5F00AF,
    0x5F00D7, 0x5F00FF, 0x5F5F00, 0x5F5F5F, 0x5F5F87, 0x5F5FAF, 0x5F5FD7, 0x5F5FFF,
    0x5F8700, 0x5F875F, 0x5F8787, 0x5F87AF, 0x5F87D7, 0x5F87FF, 0x5FAF00, 0x5FAF5F,
    0x5FAF87, 0x5FAFAF, 0x5FAFD7, 0x5FAFFF, 0x5FD700, 0x5FD75F, 0x5FD787, 0x5FD7AF,
    0x5FD7D7, 0x5FD7FF, 0x5FFF00, 0x5FFF5F, 0x5FFF87, 0x5FFFAF, 0x5FFFD7, 0x5FFFFF,
    0x870000, 0x87005F, 0x870087, 0x8700AF, 0x8700D7, 0x8700FF, 0x875F00, 0x875F5F,
    0x875F87, 0x875FAF, 0x875FD7, 0x875FFF, 0x878700, 0x87875F, 0x878787, 0x8787AF,
    0x8787D7, 0x8787FF, 0x87AF00, 0x87AF5F, 0x87AF87, 0x87AFAF, 0x87AFD7, 0x87AFFF,
    0x87D700, 0x87D75F, 0x87D787, 0x87D7AF, 0x87D7D7, 0x87D7FF, 0x87FF00, 0x87FF5F,
    0x87FF87, 0x87FFAF, 0x87FFD7, 0x87FFFF, 0xAF0000, 0xAF005F, 0xAF0087, 0xAF00AF,
    0xAF00D7, 0xAF00FF, 0xAF5F00, 0xAF5F5F, 0xAF5F87, 0xAF5FAF, 0xAF5FD7, 0xAF5FFF,
    0xAF8700, 0xAF875F, 0xAF8787, 0xAF87AF, 0xAF87D7, 0xAF87FF, 0xAFAF00, 0xAFAF5F,
    0xAFAF87, 0xAFAFAF, 0xAFAFD7, 0xAFAFFF, 0xAFD700, 0xAFD75F, 0xAFD787, 0xAFD7AF,
    0xAFD7D7, 0xAFD7FF, 0xAFFF00, 0xAFFF5F, 0xAFFF87, 0xAFFFAF, 0xAFFFD7, 0xAFFFFF,
    0xD70000, 0xD7005F, 0xD70087, 0xD700AF, 0xD700D7, 0xD700FF, 0xD75F00, 0xD75F5F,
    0xD75F87, 0xD75FAF, 0xD75FD7, 0xD75FFF, 0xD78700, 0xD7875F, 0xD78787, 0xD787AF,
    0xD787D7, 0xD787FF, 0xD7AF00, 0xD7AF5F, 0xD7AF87, 0xD7AFAF, 0xD7AFD7, 0xD7AFFF,
    0xD7D700, 0xD7D75F, 0xD7D787, 0xD7D7AF, 0xD7D7D7, 0xD7D7FF, 0xD7FF00, 0xD7FF5F,
    0xD7FF87, 0xD7FFAF, 0xD7FFD7, 0xD7FFFF, 0xFF0000, 0xFF005F, 0xFF0087, 0xFF00AF,
    0xFF00D7, 0xFF00FF, 0xFF5F00, 0xFF5F5F, 0xFF5F87, 0xFF5FAF, 0xFF5FD7, 0xFF5FFF,
    0xFF8700, 0xFF875F, 0xFF8787, 0xFF87AF, 0xFF87D7, 0xFF87FF, 0xFFAF00, 0xFFAF5F,
    0xFFAF87, 0xFFAFAF, 0xFFAFD7, 0xFFAFFF, 0xFFD700, 0xFFD75F, 0xFFD787, 0xFFD7AF,
    0xFFD7D7, 0xFFD7FF, 0xFFFF00, 0xFFFF5F, 0xFFFF87, 0xFFFFAF, 0xFFFFD7, 0xFFFFFF,
    0x080808, 0x121212, 0x1C1C1C, 0x262626, 0x303030, 0x3A3A3A, 0x444444, 0x4E4E4E,
    0x585858, 0x626262, 0x6C6C6C, 0x767676, 0x808080, 0x8A8A8A, 0x949494, 0x9E9E9E,
    0xA8A8A8, 0xB2B2B2, 0xBCBCBC, 0xC6C6C6, 0xD0D0D0, 0xDADADA, 0xE4E4E4, 0xEEEEEE
};

static int sh_alive;
static int fd_in = -1;                  // term -> sh (keys)
static int fd_out = -1;                 // sh -> term (output, FIONBIO)
static int cursor_visible = 1;

static int u_len;                       // pending UTF-8 continuation bytes
static int u_cp;

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
            for (int c = 0; c < TW; c++) screen[r][c] = screen[r + 1][c];
        for (int c = 0; c < TW; c++) { screen[TH - 1][c].cp = 0; }
    }
    ccol = 0;
}

static void put_cp(unsigned int cp) {
    if (cp == '\r') { ccol = 0; return; }
    if (cp == '\n') { newline(); return; }
    if (cp == '\b') {
        if (ccol > 0) { ccol--; screen[crow][ccol].cp = 0; }
        return;
    }
    if (cp < 0x20 || cp == 0x7F) return;
    if (ccol >= TW) newline();
    screen[crow][ccol].cp = cp;
    screen[crow][ccol].fg = fg_idx;
    screen[crow][ccol].bg = bg_idx;
    ccol++;
}

static void render(void) {
    aos_fill(win, (unsigned int)w * 4, 0, 0, w, h, col_bg);
    int pos = 0;
    for (int r = 0; r < TH; r++) {
        int c = 0;
        while (c < TW) {
            unsigned int cp = screen[r][c].cp;
            unsigned char fg = screen[r][c].fg;
            unsigned char bg = screen[r][c].bg;
            if (cp == 0) { c++; continue; }
            int start = c;
            pos = 0;
            while (c < TW && screen[r][c].cp && screen[r][c].fg == fg &&
                   screen[r][c].bg == bg) {
                pos += utf8_encode(utfbuf + pos, screen[r][c].cp);
                c++;
            }
            utfbuf[pos] = 0;
            aos_render_text(win, (unsigned int)w * 4, start * FONT_W, r * FONT_H,
                            utfbuf, xterm_rgb[fg], xterm_rgb[bg]);
        }
    }
    if (cursor_visible && sh_alive)
        aos_fill(win, (unsigned int)w * 4, ccol * FONT_W, crow * FONT_H + 14,
                 FONT_W, 2, col_fg);
    struct aos_msg m = {MSG_UPDATE, winid, 0, 0, 0};
    aos_send((unsigned int)aos_get_event_pid(), &m);
}

// ---- ANSI/VT output parser ----
static int esc_state;              // 0 idle, 1 ESC, 2 ESC[
static int esc_n, esc_r;
static int esc_params[8], esc_np;

static void sgr_apply(int *p, int np) {
    for (int i = 0; i < np; i++) {
        int v = p[i];
        if (v == 0) { fg_idx = 15; bg_idx = 0; }
        else if (v == 39) fg_idx = 15;
        else if (v == 49) bg_idx = 0;
        else if (v == 38 || v == 48) {
            if (i + 2 < np && p[i + 1] == 5) {
                int idx = p[i + 2];
                if (idx < 0) idx = 0; if (idx > 255) idx = 255;
                if (v == 38) fg_idx = (unsigned char)idx;
                else bg_idx = (unsigned char)idx;
                i += 2;
            } else if (i + 4 < np && p[i + 1] == 2) {
                i += 4;                  /* 38;2;r;g;b: unsupported, skip */
            }
        }
    }
}

static void term_out_byte(unsigned char b) {
    if (esc_state == 1) {
        if (b == '[') {
            esc_state = 2; esc_n = 0; esc_r = 0;
            esc_np = 0; for (int i = 0; i < 8; i++) esc_params[i] = 0;
        }
        else esc_state = 0;
        return;
    }
    if (esc_state == 2) {
        if (b == '?') { esc_n = -1; return; }
        if (b >= '0' && b <= '9') {
            if (esc_n >= 0) esc_n = esc_n * 10 + (b - '0');
            if (esc_np < 8) esc_params[esc_np] = esc_params[esc_np] * 10 + (b - '0');
            return;
        }
        if (b == ';') { esc_r = esc_n; esc_n = 0; if (esc_np < 8) esc_np++; return; }
        int pn = esc_n, pr = esc_r;
        esc_state = 0;
        if (b == 'm') { sgr_apply(esc_params, esc_np + 1); esc_np = 0; return; }
        if (pn < 0) {                    // ESC[?25h / ESC[?25l
            cursor_visible = (b == 'h');
            return;
        }
        if (b == 'H') {                                           // CUP r;c
            crow = pr > 0 ? pr - 1 : 0;
            ccol = pn > 0 ? pn - 1 : 0;
            if (crow < 0) crow = 0;
            if (crow >= TH) crow = TH - 1;
            if (ccol < 0) ccol = 0;
            if (ccol >= TW) ccol = TW - 1;
            return;
        }
        if (b == 'K') {                   // EL
            for (int c = ccol; c < TW; c++) screen[crow][c] = (struct tcell){0};
            return;
        }
        if (b == 'J') {                   // ED (2J -> full clear)
            for (int r = 0; r < TH; r++)
                for (int c = 0; c < TW; c++)
                    screen[r][c] = (struct tcell){0};
            crow = 0; ccol = 0;
            return;
        }
        if (b == 'D') {                   // CUB
            ccol -= pn > 0 ? pn : 1;
            if (ccol < 0) ccol = 0;
            return;
        }
        if (b == 'C') {                   // CUF
            ccol += pn > 0 ? pn : 1;
            if (ccol >= TW) ccol = TW - 1;
            return;
        }
        return;
    }
    if (b == 0x1b) { u_len = 0; esc_state = 1; return; }
    if (u_len > 0) {
        u_cp = (u_cp << 6) | (b & 0x3F);
        if (--u_len == 0) put_cp((unsigned int)u_cp);
        return;
    }
    if (b < 0x80) { put_cp(b); return; }
    if (b >= 0xC0 && b < 0xE0) { u_len = 1; u_cp = b & 0x1F; return; }
    if (b >= 0xE0 && b < 0xF0) { u_len = 2; u_cp = b & 0x0F; return; }
}

static void print_out(const char *s) {
    while (*s) term_out_byte((unsigned char)*s++);
}

// ---- input: keys -> sh ----
static void send_key(unsigned int key) {
    char buf[4];
    switch (key) {
    case 0x0101: write(fd_in, "\x1b[A", 3); break;    // UP
    case 0x0102: write(fd_in, "\x1b[B", 3); break;    // DOWN
    case 0x0103: write(fd_in, "\x1b[D", 3); break;    // LEFT
    case 0x0104: write(fd_in, "\x1b[C", 3); break;    // RIGHT
    case 0x0105: write(fd_in, "\x1b[H", 3); break;    // HOME
    case 0x0106: write(fd_in, "\x1b[F", 3); break;    // END
    case 0x0107: write(fd_in, "\x1b[3~", 4); break;   // DEL
    case '\r': write(fd_in, "\r", 1); break;
    case '\t': write(fd_in, "\t", 1); break;
    case '\b': write(fd_in, "\x7f", 1); break;
    default:
        if (key < 0x100) {
            char c = (char)key;
            write(fd_in, &c, 1);
        } else {
            int n = utf8_encode(buf, key);
            write(fd_in, buf, (size_t)n);
        }
        break;
    }
}

static void spawn_sh(void) {
    if (fd_in > 0) close(fd_in);
    if (fd_out > 0) close(fd_out);
    int in[2], out[2];
    if (pipe(in) != 0) {
        fd_in = -1;
        fd_out = -1;
        return;
    }
    if (pipe(out) != 0) {
        close(in[0]); close(in[1]);
        fd_in = -1;
        fd_out = -1;
        return;
    }
    struct aos_redir redirs[3];
    redirs[0].child_fd = 0; redirs[0].global_fd = in[0];
    redirs[1].child_fd = 1; redirs[1].global_fd = out[1];
    redirs[2].child_fd = 0xFFFFFFFF; redirs[2].global_fd = 0;
    int pid = aos_spawn_fds("bin/sh", "", 0, redirs);
    if (pid < 0) {
        close(in[0]); close(in[1]);
        close(out[0]); close(out[1]);
        fd_in = -1;
        fd_out = -1;
        return;
    }
    close(in[0]);
    close(out[1]);
    fd_in = in[1];
    fd_out = out[0];
    int one = 1;
    ioctl(fd_out, FIONBIO, &one);
    sh_alive = 1;
    cursor_visible = 1;
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
    fg_idx = (col_fg == 0xD8D8D8) ? 253 : 15;

    render();
    spawn_sh();

    for (;;) {
        if (sh_alive) {
            unsigned char buf[256];
            int n = read(fd_out, buf, sizeof buf);
            if (n > 0) {
                for (int i = 0; i < n; i++) term_out_byte(buf[i]);
                render();
            } else if (n == 0) {
                sh_alive = 0;
                print_out("\r\n[sh exited]\r\n");
                render();
            }
        }
        if (aos_recv(&m) == 0) {
            switch (m.type) {
            case MSG_KEY:
                if (sh_alive) {
                    send_key(m.a);
                } else if (m.a == '\r') {
                    spawn_sh();
                    render();
                }
                break;
            case MSG_CLOSE:
                return 0;
            }
        }
        sched_yield();
    }
}
