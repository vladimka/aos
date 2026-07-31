#include "vga.h"
#include "serial.h"
#include "ports.h"

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

static const unsigned int color_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

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
    text_color = (bg << 4) | fg;
}

static void draw_pixel(unsigned int x, unsigned int y, unsigned int rgb) {
    if (x >= fb_width || y >= fb_height) return;
    unsigned char *p = (unsigned char *)fb_addr + y * fb_pitch + x * (fb_bpp / 8);
    p[0] = rgb & 0xFF;
    p[1] = (rgb >> 8) & 0xFF;
    p[2] = (rgb >> 16) & 0xFF;
}

// UTF-8 decoder state
static int utf8_state = 0;
static unsigned int utf8_codepoint = 0;

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
    unsigned int fg_rgb = color_rgb[fg_color];
    unsigned int bg_rgb = color_rgb[bg_color];
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

static void fb_scroll(void) {
    mirror_shift_up();
    unsigned int row_bytes = fb_pitch * 16;
    unsigned int total_h = fb_height * fb_pitch;
    unsigned char *fb = (unsigned char *)fb_addr;
    for (unsigned int y = 0; y < total_h - row_bytes; y++)
        fb[y] = fb[y + row_bytes];
    unsigned int bg_rgb = color_rgb[bg_color];
    for (unsigned int y = fb_height - 16; y < fb_height; y++)
        for (unsigned int x = 0; x < fb_width; x++)
            draw_pixel(x, y, bg_rgb);
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
                          fb_glyph_from_cp(' '), color_rgb[fg_color], color_rgb[bg_color]);
        }
        screen_mirror[cursor_y][cursor_x] = ' ';
        return;
    }

    // UTF-8 decode
    unsigned short cp;
    if (utf8_state == 0) {
        if ((uc & 0x80) == 0) {
            cp = uc;
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
            cp = utf8_codepoint;
            goto render;
        }
        return;
    }

    // Invalid continuation byte, reset
    utf8_state = 0;
    return;

render:
    if (scroll_offset == 0)
        fb_draw_glyph(cursor_x * 8, cursor_y * 16,
                      fb_glyph_from_cp(cp), color_rgb[fg_color], color_rgb[bg_color]);
    if (cursor_x < mirror_cols)
        screen_mirror[cursor_y][cursor_x] = cp;
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

static void render_cp_row(const unsigned short *cp_row, int len, int screen_row) {
    unsigned int fg_rgb = color_rgb[fg_color];
    unsigned int bg_rgb = color_rgb[bg_color];

    if (fb_initialized) {
        for (int col = 0; col < len && col < mirror_cols; col++) {
            unsigned short cp = cp_row[col];
            if (cp == 0) cp = ' ';
            unsigned short glyph = fb_cp_to_glyph(cp);
            for (int r = 0; r < 16; r++) {
                unsigned char bits = fb_font[glyph * 16 + r];
                for (int c = 0; c < 8; c++) {
                    if (bits & (0x80 >> c))
                        draw_pixel(col * 8 + c, screen_row * 16 + r, fg_rgb);
                    else
                        draw_pixel(col * 8 + c, screen_row * 16 + r, bg_rgb);
                }
            }
        }
        // Clear rest of line
        for (int col = len; col < mirror_cols; col++)
            for (int r = 0; r < 16; r++)
                for (int c = 0; c < 8; c++)
                    draw_pixel(col * 8 + c, screen_row * 16 + r, bg_rgb);
    } else if (text_initialized) {
        for (int col = 0; col < TEXT_COLS; col++) {
            unsigned short cp = col < len ? cp_row[col] : 0;
            if (cp == 0) cp = ' ';
            TEXT_ADDR[screen_row * TEXT_COLS + col] = text_color << 8 | (cp > 0xFF ? '?' : (unsigned char)cp);
        }
    }
}

static void render_scrollback(void) {
    if (scroll_offset <= 0 || scrollback_count == 0) return;

    int visible_rows = max_y + 1;
    int total = scrollback_count;

    // Combined viewport: scrollback lines 0..total-1 followed by the current
    // screen (screen_mirror rows 0..visible_rows-1). Scrolling back by
    // `scroll_offset` moves the top of the viewport up that many lines.
    int sb_start = total - scroll_offset;
    if (sb_start < 0) sb_start = 0;

    unsigned int bg_rgb = color_rgb[bg_color];

    if (fb_initialized) {
        // Clear screen
        for (unsigned int y = 0; y < fb_height; y++)
            for (unsigned int x = 0; x < fb_width; x++)
                draw_pixel(x, y, bg_rgb);
    } else if (text_initialized) {
        unsigned short blank = text_color << 8 | ' ';
        for (int y = 0; y < TEXT_ROWS; y++)
            for (int x = 0; x < TEXT_COLS; x++)
                TEXT_ADDR[y * TEXT_COLS + x] = blank;
    }

    int screen_row = 0;
    for (int si = sb_start; si < total && screen_row <= max_y; si++, screen_row++) {
        int idx = (scrollback_head + si) % SCROLLBACK_LINES;
        render_cp_row(scrollback_lines[idx], scrollback_len[idx], screen_row);
    }
    // Continue with the current live screen below the scrollback portion
    for (int row = 0; screen_row <= max_y && row < visible_rows; row++, screen_row++)
        render_cp_row(screen_mirror[row], mirror_cols, screen_row);

    reset_utf8_state();
}

void vga_scroll(int delta) {
    int old_offset = scroll_offset;
    scroll_offset += delta;
    if (scroll_offset < 0) scroll_offset = 0;
    if (scroll_offset > scrollback_count) scroll_offset = scrollback_count;

    if (scroll_offset == 0 && old_offset > 0) {
        // Return to live: re-render current screen mirror
        unsigned int fg_rgb = color_rgb[fg_color];
        unsigned int bg_rgb = color_rgb[bg_color];

        if (fb_initialized) {
            for (unsigned int y = 0; y < fb_height; y++)
                for (unsigned int x = 0; x < fb_width; x++)
                    draw_pixel(x, y, bg_rgb);
            for (int row = 0; row <= max_y && row < VGA_MAX_ROWS; row++) {
                for (int col = 0; col < mirror_cols; col++) {
                    unsigned short cp = screen_mirror[row][col];
                    if (cp == 0) cp = ' ';
                    unsigned short glyph = fb_cp_to_glyph(cp);
                    for (int r = 0; r < 16; r++) {
                        unsigned char bits = fb_font[glyph * 16 + r];
                        for (int c = 0; c < 8; c++) {
                            if (bits & (0x80 >> c))
                                draw_pixel(col * 8 + c, row * 16 + r, fg_rgb);
                            else
                                draw_pixel(col * 8 + c, row * 16 + r, bg_rgb);
                        }
                    }
                }
            }
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
    } else if (scroll_offset > 0) {
        render_scrollback();
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
        unsigned int rgb = color_rgb[fg_color];
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
        unsigned int rgb = color_rgb[bg_color];
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
        unsigned int fg = color_rgb[fg_color];
        unsigned int bg = color_rgb[bg_color];
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
        text_color = (bg_color << 4) | fg_color;
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

    unsigned int bg_rgb = color_rgb[bg_color];
    for (unsigned int y = 0; y < fb_height; y++)
        for (unsigned int x = 0; x < fb_width; x++)
            draw_pixel(x, y, bg_rgb);

    cursor_x = 0;
    cursor_y = 0;
    max_x = (int)(fb_width / 8) - 1;
    max_y = (int)(fb_height / 16) - 1;
    fb_initialized = 1;
    mirror_cols = max_x + 1;
    for (int r = 0; r <= max_y; r++)
        mirror_clear_row(r);
}
