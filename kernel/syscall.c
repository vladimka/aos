#include "syscall.h"
#include "interrupts.h"
#include "terminal.h"
#include "fs.h"
#include "string.h"
#include "ports.h"

extern volatile unsigned int tick;

static char prog_args[256];

void syscall_set_args(const char *args) {
    unsigned int i;
    for (i = 0; i < 255 && args[i]; i++)
        prog_args[i] = args[i];
    prog_args[i] = '\0';
}

void syscall_handler(struct registers *r) {
    unsigned int n = r->eax;

    if (n == SYS_PRINT) {
        terminal_print((const char *)r->ebx);
    } else if (n == SYS_PRINT_HEX) {
        terminal_print_hex(r->ebx);
    } else if (n == SYS_PRINT_DEC) {
        terminal_print_dec(r->ebx);
    } else if (n == SYS_PUTCHAR) {
        terminal_putchar((char)r->ebx);
    } else if (n == SYS_FS_WRITE) {
        r->eax = fs_write((const char *)r->ebx, (const char *)r->ecx, r->edx);
    } else if (n == SYS_FS_READ) {
        r->eax = fs_read((const char *)r->ebx, (char *)r->ecx, r->edx);
    } else if (n == SYS_FS_DELETE) {
        r->eax = fs_delete((const char *)r->ebx);
    } else if (n == SYS_FS_SIZE) {
        r->eax = fs_get_size((const char *)r->ebx);
    } else if (n == SYS_FS_EXISTS) {
        r->eax = fs_exists((const char *)r->ebx);
    } else if (n == SYS_FS_LIST_GET) {
        unsigned int idx = r->ebx;
        char *name_buf = (char *)r->ecx;
        unsigned int *size_out = (unsigned int *)r->edx;

        extern int sfs_get_entry(unsigned int, char *, unsigned int *);
        r->eax = sfs_get_entry(idx, name_buf, size_out);
    } else if (n == SYS_TICK) {
        r->eax = tick;
    } else if (n == SYS_CLEAR) {
        extern void vga_init(void);
        vga_init();
    } else if (n == SYS_REBOOT) {
        terminal_print("\nRebooting...\n");
        unsigned char good = 0x02;
        while (good & 0x02)
            good = inb(0x64);
        outb(0x64, 0xFE);
        for (;;);
    } else if (n == SYS_PANIC) {
        __asm__ volatile("int $0x0");
    } else if (n == SYS_SHUTDOWN) {
        terminal_print("\nShutting down...\n");
        outw(0x604, 0x2000);
        for (;;);
    } else if (n == SYS_GET_ARGS) {
        char *dst = (char *)r->ebx;
        unsigned int maxlen = r->ecx;
        unsigned int i;
        for (i = 0; i < maxlen - 1 && prog_args[i]; i++)
            dst[i] = prog_args[i];
        if (maxlen > 0) dst[i] = '\0';
        r->eax = i;
    }
}
