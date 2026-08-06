#include "printf.h"
#include "vga.h"
#include "serial.h"
#include "klog.h"
#include <stdarg.h>

static void putc(char c) {
    vga_putchar(c);
    serial_putchar(c);
    klog_putc(c);
}

static void print_str(const char *s) {
    while (*s)
        putc(*s++);
}

static void print_uint(unsigned int n, unsigned int base, int upper) {
    static const char lower[] = "0123456789abcdef";
    static const char upperd[] = "0123456789ABCDEF";
    const char *digits = upper ? upperd : lower;
    char buf[12];
    int i = 0;
    if (n == 0) { putc('0'); return; }
    while (n) {
        buf[i++] = digits[n % base];
        n /= base;
    }
    while (i > 0)
        putc(buf[--i]);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { putc(*p); continue; }
        char c = *++p;
        if (!c) break;
        switch (c) {
        case 'c':
            putc((char)va_arg(ap, int));
            break;
        case 's':
            print_str(va_arg(ap, const char *));
            break;
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) { putc('-'); v = -v; }
            print_uint((unsigned int)v, 10, 0);
            break;
        }
        case 'u':
            print_uint(va_arg(ap, unsigned int), 10, 0);
            break;
        case 'x':
            print_uint(va_arg(ap, unsigned int), 16, 0);
            break;
        case 'X':
            print_uint(va_arg(ap, unsigned int), 16, 1);
            break;
        case '%':
            putc('%');
            break;
        default:
            putc('%');
            putc(c);
            break;
        }
    }
    va_end(ap);
    return 0;
}
