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

static unsigned int screen[TH][TW];   // codepoints per cell (0 = empty)
static int crow, ccol;
static unsigned int *win;
static unsigned int winid;
static int w, h;
static char utfbuf[TH * (TW * 3 + 1) + 1];

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
            for (int c = 0; c < TW; c++)
                screen[r][c] = screen[r + 1][c];
        for (int c = 0; c < TW; c++)
            screen[TH - 1][c] = 0;
    }
    ccol = 0;
}

static void put_cp(unsigned int cp) {
    if (cp == '\r') { ccol = 0; return; }
    if (cp == '\n') { newline(); return; }
    if (cp == '\b') {
        if (ccol > 0) { ccol--; screen[crow][ccol] = 0; }
        return;
    }
    if (cp < 0x20 || cp == 0x7F) return;
    if (ccol >= TW) newline();
    screen[crow][ccol] = cp;
    ccol++;
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
    if (cursor_visible && sh_alive)
        aos_fill(win, (unsigned int)w * 4, ccol * FONT_W, crow * FONT_H + 14,
                 FONT_W, 2, col_fg);
    struct aos_msg m = {MSG_UPDATE, winid, 0, 0, 0};
    aos_send((unsigned int)aos_get_event_pid(), &m);
}

// ---- ANSI/VT output parser ----
static int esc_state;              // 0 idle, 1 ESC, 2 ESC[
static int esc_n, esc_r;

static void term_out_byte(unsigned char b) {
    if (esc_state == 1) {
        if (b == '[') { esc_state = 2; esc_n = 0; esc_r = 0; }
        else esc_state = 0;
        return;
    }
    if (esc_state == 2) {
        if (b == '?') { esc_n = -1; return; }
        if (b >= '0' && b <= '9') { if (esc_n >= 0) esc_n = esc_n * 10 + (b - '0'); return; }
        if (b == ';') { esc_r = esc_n; esc_n = 0; return; }
        int pn = esc_n, pr = esc_r;
        esc_state = 0;
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
            for (int c = ccol; c < TW; c++) screen[crow][c] = 0;
            return;
        }
        if (b == 'J') {                   // ED (2J -> full clear)
            for (int r = 0; r < TH; r++)
                for (int c = 0; c < TW; c++)
                    screen[r][c] = 0;
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
