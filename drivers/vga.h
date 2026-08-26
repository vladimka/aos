#ifndef VGA_H
#define VGA_H

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

enum vga_color {
    VGA_BLACK         = 0,
    VGA_BLUE          = 1,
    VGA_GREEN         = 2,
    VGA_CYAN          = 3,
    VGA_RED           = 4,
    VGA_MAGENTA       = 5,
    VGA_BROWN         = 6,
    VGA_LIGHT_GREY    = 7,
    VGA_DARK_GREY     = 8,
    VGA_LIGHT_BLUE    = 9,
    VGA_LIGHT_GREEN   = 10,
    VGA_LIGHT_CYAN    = 11,
    VGA_LIGHT_RED     = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_LIGHT_BROWN   = 14,
    VGA_WHITE         = 15,
};

void vga_init(void);
void vga_clear(void);
void vga_set_color(unsigned char fg, unsigned char bg);
void vga_putchar(char c);
void vga_print(const char *str);
void vga_print_hex(unsigned int n);
void vga_print_dec(unsigned int n);
void vga_get_fb_info(unsigned int *addr, unsigned int *size);
void vga_get_fb_dimensions(unsigned int *addr, unsigned int *width, unsigned int *height,
                           unsigned int *pitch, unsigned int *bpp);
/* Non-zero when GRUB handed us a linear framebuffer (graphics boot);
 * zero in classic VGA text mode (the "AOS (text)" menu entry). */
int vga_fb_active(void);

int  vga_get_cursor_x(void);
int  vga_get_cursor_y(void);
void vga_set_cursor(int x, int y);
void vga_clear_eol(void);

void vga_cursor_on(void);
void vga_cursor_off(void);
void vga_cursor_toggle(void);

int vga_get_max_x(void);

void vga_scroll(int delta);
int  vga_get_scroll_offset(void);
void vga_reset_scroll(void);

// Render into an arbitrary 32bpp user pixel buffer (window slabs), using the
// built-in CyrSlav font. pitch is the row size in bytes.
void vga_render_text_buffer(unsigned int *buf, unsigned int pitch, int x, int y,
                            const char *str, unsigned int fg, unsigned int bg);
void vga_fill_buffer(unsigned int *buf, unsigned int pitch, int x, int y,
                     int w, int h, unsigned int rgb);

#endif
