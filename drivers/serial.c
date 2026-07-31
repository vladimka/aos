#include "serial.h"
#include "ports.h"

#define PORT 0x3F8

void serial_init(void) {
    outb(PORT + 1, 0x00);
    outb(PORT + 3, 0x80);
    outb(PORT + 0, 0x01);
    outb(PORT + 1, 0x00);
    outb(PORT + 3, 0x03);
    outb(PORT + 2, 0xC7);
    outb(PORT + 4, 0x0B);
}

void serial_putchar(char c) {
    while (!(inb(PORT + 5) & 0x20));
    outb(PORT, c);
}

void serial_print(const char *str) {
    for (const char *p = str; *p; p++)
        serial_putchar(*p);
}

void serial_print_hex(unsigned int n) {
    const char *hex = "0123456789ABCDEF";
    serial_print("0x");
    int started = 0;
    for (int i = 28; i >= 0; i -= 4) {
        int d = (n >> i) & 0xF;
        if (d || started || i == 0) {
            serial_putchar(hex[d]);
            started = 1;
        }
    }
}

void serial_print_dec(unsigned int n) {
    char buf[12];
    int i = 0;
    if (n == 0) { serial_putchar('0'); return; }
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i > 0) serial_putchar(buf[--i]);
}

int serial_available(void) {
    return (inb(PORT + 5) & 1) ? 1 : 0;
}

char serial_read(void) {
    return inb(PORT);
}
