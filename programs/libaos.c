#include "libaos.h"

static int syscall(int num, int arg1, int arg2, int arg3) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3)
    );
    return ret;
}

void print(const char *s)    { syscall(0, (int)s, 0, 0); }
void print_hex(unsigned int n) { syscall(1, n, 0, 0); }
void print_dec(unsigned int n) { syscall(2, n, 0, 0); }
void putchar(char c)          { syscall(3, c, 0, 0); }
int  fs_write(const char *name, const char *data, unsigned int size) { return syscall(4, (int)name, (int)data, size); }
int  fs_read(const char *name, char *buf, unsigned int size)         { return syscall(5, (int)name, (int)buf, size); }
int  fs_delete(const char *name)         { return syscall(6, (int)name, 0, 0); }
int  fs_get_size(const char *name)       { return syscall(7, (int)name, 0, 0); }
int  fs_exists(const char *name)         { return syscall(8, (int)name, 0, 0); }
int  fs_list_get(unsigned int idx, char *name_buf, unsigned int *size_out) { return syscall(9, idx, (int)name_buf, (int)size_out); }
unsigned int get_tick(void)              { return syscall(10, 0, 0, 0); }
void clear_screen(void)                  { syscall(11, 0, 0, 0); }
void reboot(void)                        { syscall(12, 0, 0, 0); }
void panic(void)                         { syscall(13, 0, 0, 0); }
void shutdown(void)                      { syscall(14, 0, 0, 0); }
void get_args(char *buf, unsigned int maxlen) { syscall(15, (int)buf, maxlen, 0); }
