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

static int ser_esc = 0;   // 0 idle, 1 = saw ESC, 2 = in CSI
static int ser_csi_n = 0; // bytes seen in CSI (cap to avoid runaway skip)

void serial_putchar(char c) {
    if (ser_esc == 0) {
        if (c == 0x1b) { ser_esc = 1; return; }
        while (!(inb(PORT + 5) & 0x20));
        outb(PORT, c);
        return;
    }
    if (ser_esc == 1) {
        if (c == '[') { ser_esc = 2; ser_csi_n = 0; }
        else ser_esc = 0;                 // lone ESC: drop it (wasn't followed by [)
        return;
    }
    /* in CSI: digits, ;, ? are skipped; a final alpha byte ends the sequence.
       A >64-byte "sequence" is treated as garbage and dropped too. */
    ser_csi_n++;
    if (ser_csi_n > 64 || !(c >= 0x20 && c < 0x7F)) { ser_esc = 0; return; }
    if (c >= '0' && c <= '9') return;
    if (c == ';' || c == '?') return;
    ser_esc = 0;                           // final byte consumed
    return;
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
