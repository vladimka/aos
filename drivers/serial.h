#ifndef SERIAL_H
#define SERIAL_H

void serial_init(void);
void serial_putchar(char c);
void serial_print(const char *str);
void serial_print_hex(unsigned int n);
void serial_print_dec(unsigned int n);

int  serial_available(void);
char serial_read(void);

#endif
