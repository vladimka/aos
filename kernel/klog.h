#ifndef KLOG_H
#define KLOG_H

// dmesg-style kernel log: a ring of timestamped, leveled lines fed by
// printf() (as INFO) and by the explicit leveled writer klog(). Readable
// from userland via /proc/klog (procfs.c).

#define KLOG_INFO 0
#define KLOG_WARN 1
#define KLOG_ERR  2

// Accumulate one character of the current line; printf routes every output
// byte through here. A complete line is flushed to the ring on '\n'.
void klog_putc(char c);

// Leveled write: renders fmt to COM1 AND the ring, but not the framebuffer.
void klog(int level, const char *fmt, ...);

// Copy up to `len` bytes of the log starting at virtual offset `off` into
// `buf`; returns the number of bytes copied (0 at/past the end). The log
// holds the most recent KLOG_SIZE bytes written since boot.
unsigned int klog_read(unsigned int off, void *buf, unsigned int len);

// Total bytes available (min(klog_total, KLOG_SIZE)); the virtual size.
unsigned int klog_size(void);

#endif
