#ifndef TERMINAL_H
#define TERMINAL_H

#define LINE_BUF_SIZE 256

// Special key codes reported to the GUI event stream (beyond ASCII).
#define GUI_KEY_UP    0x0101
#define GUI_KEY_DOWN  0x0102
#define GUI_KEY_LEFT  0x0103
#define GUI_KEY_RIGHT 0x0104
#define GUI_KEY_HOME  0x0105
#define GUI_KEY_END   0x0106
#define GUI_KEY_DEL   0x0107

void terminal_init(void);
void terminal_print(const char *str);
void terminal_print_hex(unsigned int n);
void terminal_print_dec(unsigned int n);
void terminal_putchar(char c);
void terminal_keyboard_handler(unsigned char scancode);
void terminal_set_prompt(void);

void terminal_write(const char *buf, unsigned int len);

void terminal_serial_byte(unsigned char c);
int  terminal_read_key(void);
void terminal_reset_keys(void);
int  terminal_scan_event(unsigned char scancode);

#endif
