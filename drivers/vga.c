#include "vga.h"
#include "serial.h"
#include "ports.h"
#include "string.h"

static int fb_initialized = 0;
static int text_initialized = 0;

static unsigned int fb_addr;
static unsigned int fb_pitch;
static unsigned int fb_width;
static unsigned int fb_height;

static unsigned char fb_bpp;
static unsigned char fb_type;

static int cursor_x = 0;
static int cursor_y = 0;
static int max_x, max_y;
static unsigned char fg_color = VGA_LIGHT_GREY;
static unsigned char bg_color = VGA_BLACK;

#define TEXT_ADDR    ((volatile unsigned short *)0xB8000)
#define TEXT_COLS    80
#define TEXT_ROWS    25

static unsigned char text_color;

static const unsigned int color_rgb[16] __attribute__((unused)) = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

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

static unsigned char fg_index = 7;   /* xterm light-grey */
static unsigned char bg_index = 0;
static int ansi_bold = 0;
static int ansi_params[8];
static int ansi_np = 0;

static unsigned int cur_fg_rgb(void) {
    return xterm_rgb[ansi_bold ? ((fg_index & 0xF8) | 8) : fg_index];
}
static unsigned int cur_bg_rgb(void) {
    return xterm_rgb[bg_index];
}

static unsigned char xterm_to_vga(int idx) {
    /* nearest of the 16 base colors */
    if (idx < 8) return (unsigned char)idx;
    if (idx < 16) return (unsigned char)(idx & 7);        /* bright -> base */
    if (idx >= 232) return (unsigned char)(idx < 244 ? 8 : 7);
    /* cube colors: pick by luminance buckets */
    int r = ((idx - 16) / 36), g = ((idx - 16) / 6) % 6, b = (idx - 16) % 6;
    int gr = (r + 1) / 2, gg = (g + 1) / 2, gb = (b + 1) / 2;
    unsigned char col = (unsigned char)(gr * 4 + gg * 2 + gb);
    if (col > 8) col = 7;
    return col;
}

/* Kernel callers (vga_set_color) pass VGA color numbers (drivers/vga.h),
   whose 0-15 numbering differs from the xterm base-16 (1=red vs 4=blue). */
static unsigned char vga_to_xterm(unsigned char idx) {
    static const unsigned char map[16] = {
        0, 4, 2, 6, 1, 5, 3, 7,          /* black blue green cyan red magenta brown lgrey */
        8, 12, 10, 14, 9, 13, 11, 15,    /* dgrey lblue lgreen lcyan lred lmag lbrown white */
    };
    return map[idx & 15];
}

static void vga_refresh_text_color(void) {
    text_color = (xterm_to_vga(bg_index) << 4) | xterm_to_vga(fg_index);
}

#include "fb_font.h"

#define VGA_MAX_COLS 128
#define VGA_MAX_ROWS 48

// Screen mirror: codepoints currently displayed on screen
static unsigned short screen_mirror[VGA_MAX_ROWS][VGA_MAX_COLS];
static int mirror_cols = 0;

// Scrollback: circular buffer of lines that scrolled off
#define SCROLLBACK_LINES 512
static unsigned short scrollback_lines[SCROLLBACK_LINES][VGA_MAX_COLS];
static unsigned char scrollback_len[SCROLLBACK_LINES];
static int scrollback_head = 0;
static int scrollback_count = 0;

// View offset: 0 = normal live view, >0 = scrolled back
static int scroll_offset = 0;

// CPU-side staging framebuffer (double buffering): scrollback redraws render
// into shadow RAM first, then one memcpy flips it to VRAM. Placed in the
// free identity-mapped gap between the ramdisk (RAMDISK_BASE, 0x00400000) and
// the user area (0x01000000).
#define FB_STAGE_ADDR 0x00C00000
#define FB_STAGE_SIZE (3u * 1024 * 1024)
static int fb_stage_active = 0;

static void fb_stage_begin(void) {
    fb_stage_active = fb_initialized && fb_height * fb_pitch <= FB_STAGE_SIZE;
}

static void fb_stage_flush(void) {
    if (!fb_stage_active) return;
    memcpy_fast((void *)fb_addr, (void *)FB_STAGE_ADDR, fb_height * fb_pitch);
    fb_stage_active = 0;
}

static void mirror_clear_row(int r) {
    for (int i = 0; i < mirror_cols; i++)
        screen_mirror[r][i] = 0;
}

static void scrollback_save_row(int r) {
    if (scrollback_count < SCROLLBACK_LINES) {
        scrollback_count++;
    } else {
        scrollback_head = (scrollback_head + 1) % SCROLLBACK_LINES;
    }
    int idx = (scrollback_head + scrollback_count - 1) % SCROLLBACK_LINES;
    unsigned char len = 0;
    for (int i = 0; i < mirror_cols && screen_mirror[r][i]; i++) {
        scrollback_lines[idx][i] = screen_mirror[r][i];
        len = i + 1;
    }
    if (len == 0) {
        scrollback_lines[idx][0] = ' ';
        len = 1;
    }
    scrollback_len[idx] = len;
}

static void mirror_shift_up(void) {
    scrollback_save_row(0);
    for (int r = 0; r < max_y; r++)
        for (int i = 0; i < mirror_cols; i++)
            screen_mirror[r][i] = screen_mirror[r + 1][i];
    mirror_clear_row(max_y);
}

void vga_set_color(unsigned char fg, unsigned char bg) {
    fg_color = fg;
    bg_color = bg;
    fg_index = vga_to_xterm(fg);
    bg_index = vga_to_xterm(bg);
    text_color = (bg << 4) | fg;    /* VGA hardware attribute, kernel numbering */
}

static void draw_pixel(unsigned int x, unsigned int y, unsigned int rgb) {
    if (x >= fb_width || y >= fb_height) return;
    unsigned char *p;
    if (fb_stage_active)
        p = (unsigned char *)FB_STAGE_ADDR + y * fb_pitch + x * (fb_bpp / 8);
    else
        p = (unsigned char *)fb_addr + y * fb_pitch + x * (fb_bpp / 8);
    p[0] = rgb & 0xFF;
    p[1] = (rgb >> 8) & 0xFF;
    p[2] = (rgb >> 16) & 0xFF;
}

// UTF-8 decoder state
static int utf8_state = 0;
static unsigned int utf8_codepoint = 0;

// ANSI CSI escape state (ESC [ <params> <final>)
static int ansi_state = 0;

static void ansi_sgr(int *p, int np) {
    for (int i = 0; i < np; i++) {
        int v = p[i];
        if (v == 0) { fg_index = 7; bg_index = 0; ansi_bold = 0; }
        else if (v == 1) ansi_bold = 1;
        else if (v == 39) { fg_index = 7; ansi_bold = 0; }
        else if (v == 49) bg_index = 0;
        else if (v == 38 || v == 48) {
            if (i + 2 < np && p[i + 1] == 5) {
                int idx = p[i + 2];
                if (idx < 0) idx = 0;
                if (idx > 255) idx = 255;
                if (v == 38) fg_index = (unsigned char)idx;
                else bg_index = (unsigned char)idx;
                i += 2;
            } else if (i + 4 < np && p[i + 1] == 2) {
                /* 38;2;r;g;b -> skip, unsupported */
                i += 4;
            }
        }
    }
    vga_refresh_text_color();
}

static void ansi_collect(unsigned char c) {
    /* called when ansi_state==2 and c is a non-digit, non-';' final byte */
    if (c == 'm') ansi_sgr(ansi_params, ansi_np);
    else if (c == 'K') {
        if (fb_initialized && scroll_offset > 0) {
            /* in scrollback: update the mirror only, never draw to the screen */
            for (int x = cursor_x; x <= max_x; x++)
                screen_mirror[cursor_y][x] = ' ';
        } else {
            vga_clear_eol();          /* both paths share clear */
        }
    }
    else if (c == 'D') {                         /* CUB: cursor left N */
        int k = ansi_np > 0 ? ansi_params[0] : 0;
        if (k <= 0) k = 1;                       /* bare ESC[D moves 1 */
        cursor_x -= k;
        if (cursor_x < 0) cursor_x = 0;
    }
    ansi_np = 0;
}

static unsigned char fb_glyph_from_cp(unsigned short cp) {
    return fb_cp_to_glyph(cp);
}

static void fb_draw_glyph(unsigned int x, unsigned int y, unsigned char glyph, unsigned int fg, unsigned int bg) {
    for (int row = 0; row < 16; row++) {
        unsigned char bits = fb_font[glyph * 16 + row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col))
                draw_pixel(x + col, y + row, fg);
            else
                draw_pixel(x + col, y + row, bg);
        }
    }
}

static void fb_erase_underline(void) {
    if (!fb_initialized) return;
    unsigned int fg_rgb = cur_fg_rgb();
    unsigned int bg_rgb = cur_bg_rgb();
    unsigned short cp = screen_mirror[cursor_y][cursor_x];
    if (cp == 0) cp = ' ';
    unsigned short glyph = fb_cp_to_glyph(cp);
    for (int row = 0; row < 16; row++) {
        unsigned char bits = fb_font[glyph * 16 + row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col))
                draw_pixel(cursor_x * 8 + col, cursor_y * 16 + row, fg_rgb);
            else
                draw_pixel(cursor_x * 8 + col, cursor_y * 16 + row, bg_rgb);
        }
    }
}

static void fb_fill_rows(unsigned char *dst, unsigned int y0, unsigned int y1, unsigned int rgb);

static void fb_scroll(void) {
    mirror_shift_up();
    unsigned int row_bytes = fb_pitch * 16;
    unsigned char *fb = (unsigned char *)fb_addr;
    for (unsigned int y = 0; y + row_bytes < fb_height * fb_pitch; y += row_bytes)
        memcpy_fast(fb + y, fb + y + row_bytes, row_bytes);
    unsigned int bg_rgb = cur_bg_rgb();
    fb_fill_rows(fb, fb_height - 16, fb_height, bg_rgb);
    cursor_y--;
}

static void text_scroll(void) {
    mirror_shift_up();
    for (int y = 0; y < TEXT_ROWS - 1; y++)
        for (int x = 0; x < TEXT_COLS; x++)
            TEXT_ADDR[y * TEXT_COLS + x] = TEXT_ADDR[(y + 1) * TEXT_COLS + x];
    unsigned short blank = text_color << 8 | ' ';
    for (int x = 0; x < TEXT_COLS; x++)
        TEXT_ADDR[(TEXT_ROWS - 1) * TEXT_COLS + x] = blank;
    cursor_y--;
}

static void text_putchar(char c) {
    unsigned char uc = (unsigned char)c;
    if (uc == '\r') {
        cursor_x = 0;
        return;
    }
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= TEXT_ROWS) text_scroll();
        return;
    }
    if (c == '\b') {
        if (cursor_x > 0) cursor_x--;
        TEXT_ADDR[cursor_y * TEXT_COLS + cursor_x] = text_color << 8 | ' ';
        screen_mirror[cursor_y][cursor_x] = 0;
        return;
    }
    if (ansi_state == 1) {
        if (uc == '[') {
            ansi_state = 2;
            ansi_np = 0;
            for (int i = 0; i < 8; i++) ansi_params[i] = 0;
            return;
        }
        ansi_state = 0;
    } else if (ansi_state == 2) {
        if (uc >= '0' && uc <= '9') {
            if (ansi_np < 8) ansi_params[ansi_np] = ansi_params[ansi_np] * 10 + (uc - '0');
            return;
        }
        if (uc == ';') { if (ansi_np < 8) ansi_np++; return; }
        ansi_state = 0;
        ansi_collect(uc);
        return;
    } else if (uc == 0x1b) {
        ansi_state = 1;
        return;
    }
    if (c < 0x20) return;
    TEXT_ADDR[cursor_y * TEXT_COLS + cursor_x] = text_color << 8 | (unsigned char)c;
    screen_mirror[cursor_y][cursor_x] = (unsigned char)c;
    cursor_x++;
    if (cursor_x >= TEXT_COLS) {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= TEXT_ROWS) text_scroll();
    }
}

static void fb_putchar(char c) {
    unsigned char uc = (unsigned char)c;

    if (uc == '\n') {
        if (scroll_offset == 0)
            fb_erase_underline();
        cursor_x = 0;
        cursor_y++;
        if (cursor_y > max_y) fb_scroll();
        return;
    }
    if (uc == '\b') {
        if (cursor_x > 0) cursor_x--;
        if (scroll_offset == 0) {
            fb_draw_glyph(cursor_x * 8, cursor_y * 16,
                          fb_glyph_from_cp(' '), cur_fg_rgb(), cur_bg_rgb());
        }
        screen_mirror[cursor_y][cursor_x] = ' ';
        return;
    }
    if (uc == '\r') {
        cursor_x = 0;
        return;
    }
    if (ansi_state == 1) {
        if (uc == '[') {
            ansi_state = 2;
            ansi_np = 0;
            for (int i = 0; i < 8; i++) ansi_params[i] = 0;
            return;
        }
        ansi_state = 0;
    } else if (ansi_state == 2) {
        if (uc >= '0' && uc <= '9') {
            if (ansi_np < 8) ansi_params[ansi_np] = ansi_params[ansi_np] * 10 + (uc - '0');
            return;
        }
        if (uc == ';') { if (ansi_np < 8) ansi_np++; return; }
        ansi_state = 0;
        ansi_collect(uc);
        return;
    } else if (uc == 0x1b) {
        ansi_state = 1;
        return;
    }

    // UTF-8 decode
    unsigned int cpi;
    if (utf8_state == 0) {
        if ((uc & 0x80) == 0) {
            cpi = uc;
            goto render;
        } else if ((uc & 0xE0) == 0xC0) {
            utf8_state = 1;
            utf8_codepoint = uc & 0x1F;
            return;
        } else if ((uc & 0xF0) == 0xE0) {
            utf8_state = 2;
            utf8_codepoint = uc & 0x0F;
            return;
        } else if ((uc & 0xF8) == 0xF0) {
            utf8_state = 3;
            utf8_codepoint = uc & 0x07;
            return;
        }
        return;
    }

    if ((uc & 0xC0) == 0x80) {
        utf8_codepoint = (utf8_codepoint << 6) | (uc & 0x3F);
        utf8_state--;
        if (utf8_state == 0) {
            cpi = utf8_codepoint;
            goto render;
        }
        return;
    }

    // Invalid continuation byte, reset
    utf8_state = 0;
    return;

render:
    // screen_mirror stores 16-bit codepoints only; squash anything larger
    if (cpi > 0xFFFF) cpi = '?';
    if (scroll_offset == 0)
        fb_draw_glyph(cursor_x * 8, cursor_y * 16,
                      fb_glyph_from_cp(cpi), cur_fg_rgb(), cur_bg_rgb());
    if (cursor_x < mirror_cols)
        screen_mirror[cursor_y][cursor_x] = cpi;
    cursor_x++;
    if (cursor_x > max_x) {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y > max_y) fb_scroll();
    }
}

// ---- Scrollback rendering ----
static void reset_utf8_state(void) {
    utf8_state = 0;
    utf8_codepoint = 0;
}

static void fb_render_cell(unsigned char *dst, unsigned short glyph, unsigned int fg_rgb) {
    const unsigned char *fontp = fb_font + glyph * 16;
    if (fb_bpp == 32) {
        for (int r = 0; r < 16; r++) {
            unsigned char bits = fontp[r];
            if (bits == 0) continue;
            unsigned int *p = (unsigned int *)(dst + r * fb_pitch);
            if (bits == 0xFF) {
                p[0] = p[1] = p[2] = p[3] = fg_rgb;
                p[4] = p[5] = p[6] = p[7] = fg_rgb;
            } else {
                if (bits & 0x80) p[0] = fg_rgb;
                if (bits & 0x40) p[1] = fg_rgb;
                if (bits & 0x20) p[2] = fg_rgb;
                if (bits & 0x10) p[3] = fg_rgb;
                if (bits & 0x08) p[4] = fg_rgb;
                if (bits & 0x04) p[5] = fg_rgb;
                if (bits & 0x02) p[6] = fg_rgb;
                if (bits & 0x01) p[7] = fg_rgb;
            }
        }
    } else {
        unsigned int step = fb_bpp / 8;
        for (int r = 0; r < 16; r++) {
            unsigned char bits = fontp[r];
            if (bits == 0) continue;
            unsigned char *p = dst + r * fb_pitch;
            for (int c = 0; c < 8; c++) {
                if (bits & (0x80 >> c)) {
                    unsigned char *q = p + c * step;
                    q[0] = fg_rgb & 0xFF;
                    q[1] = (fg_rgb >> 8) & 0xFF;
                    q[2] = (fg_rgb >> 16) & 0xFF;
                }
            }
        }
    }
}

// Fill pixel rows y0..y1-1 of the visible text region with a solid color.
static void fb_fill_rows(unsigned char *dst, unsigned int y0, unsigned int y1, unsigned int rgb) {
    unsigned int step = fb_bpp / 8;
    unsigned int W = mirror_cols * 8;
    if (fb_bpp == 32) {
        for (unsigned int y = y0; y < y1; y++)
            memset_fast32(dst + y * fb_pitch, rgb, W >> 2);
    } else {
        for (unsigned int y = y0; y < y1; y++) {
            unsigned char *p = dst + y * fb_pitch;
            for (unsigned int x = 0; x < W; x++, p += step) {
                p[0] = rgb & 0xFF;
                p[1] = (rgb >> 8) & 0xFF;
                p[2] = (rgb >> 16) & 0xFF;
            }
        }
    }
}

// Fast row renderer: background is already filled, so only fg pixels are written.
static void fb_render_row_fast(const unsigned short *cp_row, int len, int screen_row) {
    unsigned int fg_rgb = cur_fg_rgb();
    unsigned char *base = fb_stage_active ? (unsigned char *)FB_STAGE_ADDR : (unsigned char *)fb_addr;
    unsigned int row_base = screen_row * 16 * fb_pitch;
    for (int col = 0; col < len && col < mirror_cols; col++) {
        unsigned short cp = cp_row[col];
        if (cp == 0) cp = ' ';
        unsigned short glyph = fb_cp_to_glyph(cp);
        fb_render_cell(base + row_base + col * 8 * (fb_bpp / 8), glyph, fg_rgb);
    }
}

// Virtual viewport row: scrollback lines first, then the live screen mirror.
static const unsigned short *vrow_ptr(int r, int *len) {
    if (r < scrollback_count) {
        int idx = (scrollback_head + r) % SCROLLBACK_LINES;
        *len = scrollback_len[idx];
        return scrollback_lines[idx];
    }
    *len = mirror_cols;
    return screen_mirror[r - scrollback_count];
}

// Full re-render of the scrollback view (entering scrollback, or text mode).
static void render_scrollback_full(void) {
    if (scroll_offset <= 0 || scrollback_count == 0) return;

    int sb_start = scrollback_count - scroll_offset;
    if (sb_start < 0) sb_start = 0;

    if (fb_initialized) {
        fb_stage_begin();
        fb_fill_rows((unsigned char *)FB_STAGE_ADDR, 0, (max_y + 1) * 16, cur_bg_rgb());
        int screen_row = 0;
        for (int si = sb_start; si < scrollback_count && screen_row <= max_y; si++, screen_row++) {
            int idx = (scrollback_head + si) % SCROLLBACK_LINES;
            fb_render_row_fast(scrollback_lines[idx], scrollback_len[idx], screen_row);
        }
        for (int row = 0; screen_row <= max_y && row <= max_y; row++, screen_row++)
            fb_render_row_fast(screen_mirror[row], mirror_cols, screen_row);
        fb_stage_flush();
    } else if (text_initialized) {
        unsigned short blank = text_color << 8 | ' ';
        for (int y = 0; y < TEXT_ROWS; y++)
            for (int x = 0; x < TEXT_COLS; x++)
                TEXT_ADDR[y * TEXT_COLS + x] = blank;
        int screen_row = 0;
        for (int si = sb_start; si < scrollback_count && screen_row < TEXT_ROWS; si++, screen_row++) {
            int idx = (scrollback_head + si) % SCROLLBACK_LINES;
            for (int col = 0; col < TEXT_COLS; col++) {
                unsigned short cp = col < scrollback_len[idx] ? scrollback_lines[idx][col] : 0;
                if (cp == 0) cp = ' ';
                TEXT_ADDR[screen_row * TEXT_COLS + col] = text_color << 8 | (cp > 0xFF ? '?' : (unsigned char)cp);
            }
        }
        for (int row = 0; screen_row < TEXT_ROWS && row < TEXT_ROWS; row++, screen_row++) {
            for (int col = 0; col < TEXT_COLS; col++) {
                unsigned short cp = screen_mirror[row][col];
                if (cp == 0) cp = ' ';
                TEXT_ADDR[screen_row * TEXT_COLS + col] = text_color << 8 | (cp > 0xFF ? '?' : (unsigned char)cp);
            }
        }
    }

    reset_utf8_state();
}

// Full re-render of the live screen (returning from scrollback).
static void render_live_full(void) {
    if (fb_initialized) {
        fb_stage_begin();
        fb_fill_rows((unsigned char *)FB_STAGE_ADDR, 0, (max_y + 1) * 16, cur_bg_rgb());
        for (int row = 0; row <= max_y; row++)
            fb_render_row_fast(screen_mirror[row], mirror_cols, row);
        fb_stage_flush();
    } else if (text_initialized) {
        unsigned short blank = text_color << 8 | ' ';
        for (int y = 0; y < TEXT_ROWS; y++)
            for (int x = 0; x < TEXT_COLS; x++)
                TEXT_ADDR[y * TEXT_COLS + x] = blank;
        for (int row = 0; row < TEXT_ROWS; row++) {
            for (int col = 0; col < TEXT_COLS; col++) {
                unsigned short cp = screen_mirror[row][col];
                if (cp == 0) cp = ' ';
                TEXT_ADDR[row * TEXT_COLS + col] = text_color << 8 | (cp > 0xFF ? '?' : (unsigned char)cp);
            }
        }
    }
    reset_utf8_state();
    vga_cursor_on();
}

// Incremental scroll: shift the staged framebuffer by d text rows (d != 0) and
// render only the newly exposed rows. Used for navigation within scrollback,
// avoiding a full screen redraw on every wheel notch.
static void shift_view(int d) {
    unsigned int pix = (d > 0 ? d : -d) * 16;
    unsigned int H = (max_y + 1) * 16;
    unsigned int rowbytes = mirror_cols * 8 * (fb_bpp / 8);
    unsigned char *base = (unsigned char *)FB_STAGE_ADDR;
    int vstart = scrollback_count - scroll_offset;
    unsigned int bg_rgb = cur_bg_rgb();

    fb_stage_begin();

    if (d > 0) {
        // View moved up: copy rows pix..H-1 to 0..H-pix-1
        for (unsigned int y = 0; y + pix < H; y++)
            memcpy_fast(base + y * fb_pitch, base + (y + pix) * fb_pitch, rowbytes);
        fb_fill_rows(base, H - pix, H, bg_rgb);
        for (int k = 0; k < d; k++) {
            int len;
            const unsigned short *line = vrow_ptr(vstart + max_y - d + 1 + k, &len);
            fb_render_row_fast(line, len, max_y - d + 1 + k);
        }
    } else {
        // View moved down: copy rows 0..H-pix-1 to pix..H-1
        for (unsigned int y = H; y > pix; y--)
            memcpy_fast(base + (y - 1) * fb_pitch, base + (y - 1 - pix) * fb_pitch, rowbytes);
        fb_fill_rows(base, 0, pix, bg_rgb);
        for (int k = 0; k < -d; k++) {
            int len;
            const unsigned short *line = vrow_ptr(vstart + k, &len);
            fb_render_row_fast(line, len, k);
        }
    }

    fb_stage_flush();
    reset_utf8_state();
}

void vga_scroll(int delta) {
    int old_offset = scroll_offset;
    scroll_offset += delta;
    if (scroll_offset < 0) scroll_offset = 0;
    if (scroll_offset > scrollback_count) scroll_offset = scrollback_count;
    if (scroll_offset == old_offset) return;

    if (scroll_offset == 0) {
        render_live_full();
    } else if (!fb_initialized || old_offset == 0) {
        render_scrollback_full();
    } else {
        shift_view(scroll_offset - old_offset);
    }
}

int vga_get_scroll_offset(void) {
    return scroll_offset;
}

void vga_reset_scroll(void) {
    if (scroll_offset > 0)
        vga_scroll(-scroll_offset);
}

// ---- cursor ----
static int cursor_visible = 0;

static void text_cursor_pos(int x, int y) {
    unsigned short pos = y * TEXT_COLS + x;
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, (pos >> 8) & 0xFF);
}

static void text_cursor_hide(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);
}

void vga_cursor_on(void) {
    cursor_visible = 1;
    if (scroll_offset > 0) return;
    if (fb_initialized) {
        unsigned int x = cursor_x * 8;
        unsigned int y = cursor_y * 16 + 14;
        unsigned int rgb = cur_fg_rgb();
        for (unsigned int row = 0; row < 2 && y + row < fb_height; row++)
            for (unsigned int col = 0; col < 8 && x + col < fb_width; col++)
                draw_pixel(x + col, y + row, rgb);
    } else if (text_initialized) {
        text_cursor_pos(cursor_x, cursor_y);
    }
}

void vga_cursor_off(void) {
    cursor_visible = 0;
    if (scroll_offset > 0) return;
    if (fb_initialized) {
        unsigned int x = cursor_x * 8;
        unsigned int y = cursor_y * 16 + 14;
        unsigned int rgb = cur_bg_rgb();
        for (unsigned int row = 0; row < 2 && y + row < fb_height; row++)
            for (unsigned int col = 0; col < 8 && x + col < fb_width; col++)
                draw_pixel(x + col, y + row, rgb);
    } else if (text_initialized) {
        text_cursor_hide();
    }
}

void vga_cursor_toggle(void) {
    if (cursor_visible)
        vga_cursor_off();
    else
        vga_cursor_on();
}

int vga_get_cursor_x(void) {
    return cursor_x;
}

int vga_get_cursor_y(void) {
    return cursor_y;
}

void vga_set_cursor(int x, int y) {
    cursor_x = x;
    cursor_y = y;
}

int vga_get_max_x(void) {
    if (fb_initialized) return max_x;
    return TEXT_COLS - 1;
}

void vga_clear_eol(void) {
    if (fb_initialized) {
        unsigned int fg = cur_fg_rgb();
        unsigned int bg = cur_bg_rgb();
        unsigned char glyph = fb_glyph_from_cp(' ');
        for (int x = cursor_x; x <= max_x; x++) {
            fb_draw_glyph(x * 8, cursor_y * 16, glyph, fg, bg);
            screen_mirror[cursor_y][x] = ' ';
        }
    } else if (text_initialized) {
        unsigned short blank = text_color << 8 | ' ';
        for (int x = cursor_x; x < TEXT_COLS; x++) {
            TEXT_ADDR[cursor_y * TEXT_COLS + x] = blank;
            screen_mirror[cursor_y][x] = ' ';
        }
    }
}

void vga_putchar(char c) {
    if (scroll_offset > 0) {
        // In scrollback mode - don't draw, but update mirror for live catch-up
        if (fb_initialized) fb_putchar(c);
        else if (text_initialized) text_putchar(c);
        return;
    }
    if (text_initialized) text_putchar(c);
    else if (fb_initialized) fb_putchar(c);
}

void vga_print(const char *str) {
    for (const char *p = str; *p; p++)
        vga_putchar(*p);
}

void vga_print_hex(unsigned int n) {
    const char *hex = "0123456789ABCDEF";
    vga_print("0x");
    int started = 0;
    for (int i = 28; i >= 0; i -= 4) {
        int d = (n >> i) & 0xF;
        if (d || started || i == 0) {
            vga_putchar(hex[d]);
            started = 1;
        }
    }
}

void vga_print_dec(unsigned int n) {
    char buf[12];
    int i = 0;
    if (n == 0) { vga_putchar('0'); return; }
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i > 0) vga_putchar(buf[--i]);
}

extern unsigned int __saved_mb_info;
extern unsigned int __saved_magic;

static int mb2_fb(unsigned char *tag) {
    fb_addr   = *(unsigned long long *)(tag + 8);
    fb_pitch  = *(unsigned int *)(tag + 16);
    fb_width  = *(unsigned int *)(tag + 20);
    fb_height = *(unsigned int *)(tag + 24);
    fb_bpp    = *(unsigned char *)(tag + 28);
    fb_type   = *(unsigned char *)(tag + 29);
    return fb_type == 1;
}

static int mb1_fb(unsigned char *mbi) {
    unsigned int flags = *(unsigned int *)mbi;
    if (!(flags & (1 << 12)))
        return 0;
    fb_addr   = *(unsigned long long *)(mbi + 88);
    fb_pitch  = *(unsigned int *)(mbi + 96);
    fb_width  = *(unsigned int *)(mbi + 100);
    fb_height = *(unsigned int *)(mbi + 104);
    fb_bpp    = *(unsigned char *)(mbi + 108);
    fb_type   = *(unsigned char *)(mbi + 109);
    return fb_type == 1;
}

void vga_init(void) {
    unsigned char *mbi = (unsigned char *)__saved_mb_info;
    if (!mbi) return;

    int ok = 0;
    if (__saved_magic == 0x36D76289) {
        unsigned int total = *(unsigned int *)mbi;
        unsigned char *tag = mbi + 8;
        while ((unsigned int)(tag - mbi) < total) {
            unsigned int type = *(unsigned int *)tag;
            unsigned int size = *(unsigned int *)(tag + 4);
            if (type == 0) break;
            if (type == 8) {
                ok = mb2_fb(tag);
                break;
            }
            tag += (size + 7) & ~7;
        }
    } else {
        ok = mb1_fb(mbi);
    }

    if (!ok) {
        serial_print("Framebuffer: not available, using text mode\n");
        vga_refresh_text_color();
        unsigned short blank = text_color << 8 | ' ';
        for (int i = 0; i < TEXT_ROWS * TEXT_COLS; i++)
            TEXT_ADDR[i] = blank;
        cursor_x = 0;
        cursor_y = 0;
        text_initialized = 1;
        mirror_cols = TEXT_COLS;
        for (int r = 0; r < TEXT_ROWS; r++)
            mirror_clear_row(r);
        return;
    }

    unsigned int bg_rgb = cur_bg_rgb();
    for (unsigned int y = 0; y < fb_height; y++)
        for (unsigned int x = 0; x < fb_width; x++)
            draw_pixel(x, y, bg_rgb);

    cursor_x = 0;
    cursor_y = 0;
    max_x = (int)(fb_width / 8) - 1;
    max_y = (int)(fb_height / 16) - 1;
    // Clamp to fixed mirror/scrollback array bounds (screen_mirror etc.)
    if (max_x >= VGA_MAX_COLS) max_x = VGA_MAX_COLS - 1;
    if (max_y >= VGA_MAX_ROWS) max_y = VGA_MAX_ROWS - 1;
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;
    fb_initialized = 1;
    mirror_cols = max_x + 1;
    for (int r = 0; r <= max_y; r++)
        mirror_clear_row(r);
}

void vga_clear(void) {
    scroll_offset = 0;
    scrollback_head = 0;
    scrollback_count = 0;
    cursor_x = 0;
    cursor_y = 0;
    reset_utf8_state();
    ansi_state = 0;
    ansi_np = 0;

    if (fb_initialized) {
        unsigned int bg_rgb = cur_bg_rgb();
        for (unsigned int y = 0; y < fb_height; y++)
            for (unsigned int x = 0; x < fb_width; x++)
                draw_pixel(x, y, bg_rgb);
    } else if (text_initialized) {
        unsigned short blank = text_color << 8 | ' ';
        for (int i = 0; i < TEXT_ROWS * TEXT_COLS; i++)
            TEXT_ADDR[i] = blank;
    }

    if (text_initialized) {
        for (int r = 0; r < TEXT_ROWS; r++)
            mirror_clear_row(r);
    } else if (fb_initialized) {
        for (int r = 0; r <= max_y; r++)
            mirror_clear_row(r);
    }
}

void vga_get_fb_info(unsigned int *addr, unsigned int *size) {
    if (fb_initialized) {
        *addr = fb_addr;
        *size = fb_height * fb_pitch;
    } else {
        *addr = 0;
        *size = 0;
    }
}

int vga_fb_active(void) {
    return fb_initialized;
}

void vga_get_fb_dimensions(unsigned int *addr, unsigned int *width, unsigned int *height,
                           unsigned int *pitch, unsigned int *bpp) {
    if (fb_initialized) {
        if (addr) *addr = fb_addr;
        if (width) *width = fb_width;
        if (height) *height = fb_height;
        if (pitch) *pitch = fb_pitch;
        if (bpp) *bpp = fb_bpp;
    } else {
        if (addr) *addr = 0;
        if (width) *width = 0;
        if (height) *height = 0;
        if (pitch) *pitch = 0;
        if (bpp) *bpp = 0;
    }
}

// ---- Rendering into arbitrary 32bpp user buffers (window slabs) ----

void vga_fill_buffer(unsigned int *buf, unsigned int pitch, int x, int y,
                     int w, int h, unsigned int rgb) {
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; yy++) {
        unsigned int *row = (unsigned int *)((char *)buf + (y + yy) * pitch) + x;
        for (int xx = 0; xx < w; xx++)
            row[xx] = rgb;
    }
}

static void buffer_glyph(unsigned int *buf, unsigned int pitch, int x, int y,
                         unsigned short glyph, unsigned int fg, unsigned int bg) {
    const unsigned char *fontp = fb_font + glyph * 16;
    for (int r = 0; r < 16; r++) {
        unsigned char bits = fontp[r];
        unsigned int *row = (unsigned int *)((char *)buf + (y + r) * pitch) + x;
        for (int c = 0; c < 8; c++)
            row[c] = (bits & (0x80 >> c)) ? fg : bg;
    }
}

void vga_render_text_buffer(unsigned int *buf, unsigned int pitch, int x, int y,
                            const char *str, unsigned int fg, unsigned int bg) {
    int cx = 0;
    int cy = 0;
    while (*str) {
        unsigned int cp;
        unsigned char c = *str;
        if (c < 0x80) {
            cp = c;
            str++;
        } else if ((c & 0xE0) == 0xC0 && str[1]) {
            cp = ((c & 0x1F) << 6) | (str[1] & 0x3F);
            str += 2;
        } else if ((c & 0xF0) == 0xE0 && str[1] && str[2]) {
            cp = ((c & 0x0F) << 12) | ((str[1] & 0x3F) << 6) | (str[2] & 0x3F);
            str += 3;
        } else {
            cp = '?';
            str++;
        }

        if (cp == '\n') {
            cx = 0;
            cy++;
            continue;
        }
        if (cp > 0xFFFF) cp = '?';

        buffer_glyph(buf, pitch, x + cx * 8, y + cy * 16, fb_cp_to_glyph(cp), fg, bg);
        cx++;
    }
}
