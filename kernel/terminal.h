#ifndef TERMINAL_H
#define TERMINAL_H

#define LINE_BUF_SIZE 256

void terminal_init(void);
void terminal_print(const char *str);
void terminal_print_hex(unsigned int n);
void terminal_print_dec(unsigned int n);
void terminal_putchar(char c);
void terminal_keyboard_handler(unsigned char scancode);
void terminal_set_prompt(void);

void terminal_write(const char *buf, unsigned int len);

#endif
