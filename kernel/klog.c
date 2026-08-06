#include "klog.h"
#include "serial.h"
#include <stdarg.h>

#define KLOG_SIZE 32768
#define KLOG_LINE_MAX 256

static char klog_ring[KLOG_SIZE];
static unsigned int klog_pos;      // next ring write offset (wraps)
static unsigned int klog_total;    // bytes ever written (virtual size)

static char line_buf[KLOG_LINE_MAX];
static unsigned int line_len;
static int line_level = KLOG_INFO;
static unsigned int line_ticks;    // tick at the start of the current line
static int level_override;         // 1 = line level set explicitly (klog())

extern volatile unsigned int tick;

// Commit line_buf as "[tttttttt] L text\n" into the ring.
static void line_commit(void) {
    char out[KLOG_LINE_MAX + 16];
    const char *hex = "0123456789abcdef";
    unsigned int i = 0;
    out[i++] = '[';
    for (int s = 28; s >= 0; s -= 4)
        out[i++] = hex[(line_ticks >> s) & 0xF];
    out[i++] = ']';
    out[i++] = ' ';
    out[i++] = line_level == KLOG_INFO ? 'I'
             : line_level == KLOG_WARN ? 'W' : 'E';
    out[i++] = ' ';
    for (unsigned int k = 0; k < line_len; k++)
        out[i++] = line_buf[k];
    out[i++] = '\n';
    for (unsigned int k = 0; k < i; k++) {
        klog_ring[klog_pos] = out[k];
        klog_pos = (klog_pos + 1) % KLOG_SIZE;
        klog_total++;
    }
    line_len = 0;
}

void klog_putc(char c) {
    if (line_len == 0) {
        line_ticks = tick;
        if (!level_override) line_level = KLOG_INFO;
        level_override = 0;
    }
    if (c == '\n') { line_commit(); return; }
    if (line_len >= KLOG_LINE_MAX - 1)
        line_commit();
    line_buf[line_len++] = c;
}

// Minimal printf engine that writes to COM1 and the ring (leveled, no VGA).
static void kvputc(char c) {
    serial_putchar(c);
    klog_putc(c);
}

static void kprint_str(const char *s) {
    while (*s) kvputc(*s++);
}

static void kprint_uint(unsigned int n, unsigned int base, int upper) {
    static const char lower[] = "0123456789abcdef";
    static const char upperd[] = "0123456789ABCDEF";
    const char *digits = upper ? upperd : lower;
    char buf[12];
    int i = 0;
    if (n == 0) { kvputc('0'); return; }
    while (n) { buf[i++] = digits[n % base]; n /= base; }
    while (i > 0) kvputc(buf[--i]);
}

static void klog_vprintf(int level, const char *fmt, va_list ap) {
    level_override = 1;
    line_level = level;
    line_ticks = tick;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { kvputc(*p); continue; }
        char c = *++p;
        if (!c) break;
        switch (c) {
        case 'c': kvputc((char)va_arg(ap, int)); break;
        case 's': kprint_str(va_arg(ap, const char *)); break;
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) { kvputc('-'); v = -v; }
            kprint_uint((unsigned int)v, 10, 0);
            break;
        }
        case 'u': kprint_uint(va_arg(ap, unsigned int), 10, 0); break;
        case 'x': kprint_uint(va_arg(ap, unsigned int), 16, 0); break;
        case 'X': kprint_uint(va_arg(ap, unsigned int), 16, 1); break;
        case '%': kvputc('%'); break;
        default: kvputc('%'); kvputc(c); break;
        }
    }
}

void klog(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    klog_vprintf(level, fmt, ap);
    va_end(ap);
    kvputc('\n');
}

unsigned int klog_size(void) {
    return klog_total < KLOG_SIZE ? klog_total : KLOG_SIZE;
}

unsigned int klog_read(unsigned int off, void *buf, unsigned int len) {
    unsigned int flags;
    __asm__ volatile("pushfl; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    unsigned int total = klog_size();
    if (off >= total) len = 0;
    else if (len > total - off) len = total - off;
    unsigned int copied = len;
    unsigned int idx = (klog_pos + KLOG_SIZE - total + off) % KLOG_SIZE;
    for (unsigned int i = 0; i < copied; i++)
        ((char *)buf)[i] = klog_ring[(idx + i) % KLOG_SIZE];
    if (flags & 0x200) __asm__ volatile("sti");
    return copied;
}
