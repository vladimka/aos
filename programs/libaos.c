#include "libaos.h"
#include "aosipc.h"

#define SYS_EXIT     16
#define SYS_READ_KEY 17
#define SYS_YIELD    18
#define SYS_GETPID   19
#define SYS_SEND     20
#define SYS_RECV     21
#define SYS_EVENT    22
#define SYS_MOUSE    23
#define SYS_FB_INFO  24
#define SYS_TEXT     25
#define SYS_FILL     26
#define SYS_SETOUT   27
#define SYS_SPAWN    28
#define SYS_GETEVENT 29
#define SYS_SLEEP     30
#define SYS_WAITPID   31
#define SYS_GET_CHILDREN 32
#define SYS_RANDOM    33
#define SYS_RTC       34
#define SYS_UPTIME    35

static int syscall(int num, int a, int b, int c, int d, int e) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
        : "memory"
    );
    return ret;
}

// Entry trampoline: run main(), then terminate cleanly in ring 3.
extern void main(void);

void _start(void) {
    main();
    syscall(SYS_EXIT, 0, 0, 0, 0, 0);
    for (;;);
}

void print(const char *s)    { syscall(0, (int)s, 0, 0, 0, 0); }
void print_hex(unsigned int n) { syscall(1, n, 0, 0, 0, 0); }
void print_dec(unsigned int n) { syscall(2, n, 0, 0, 0, 0); }
void putchar(char c)          { syscall(3, c, 0, 0, 0, 0); }
int  fs_write(const char *name, const char *data, unsigned int size) { return syscall(4, (int)name, (int)data, size, 0, 0); }
int  fs_read(const char *name, char *buf, unsigned int size)         { return syscall(5, (int)name, (int)buf, size, 0, 0); }
int  fs_delete(const char *name)         { return syscall(6, (int)name, 0, 0, 0, 0); }
int  fs_get_size(const char *name)       { return syscall(7, (int)name, 0, 0, 0, 0); }
int  fs_exists(const char *name)         { return syscall(8, (int)name, 0, 0, 0, 0); }
int  fs_list_get(unsigned int idx, char *name_buf, unsigned int *size_out) { return syscall(9, idx, (int)name_buf, (int)size_out, 0, 0); }
unsigned int get_tick(void)              { return syscall(10, 0, 0, 0, 0, 0); }
void clear_screen(void)                  { syscall(11, 0, 0, 0, 0, 0); }
void reboot(void)                        { syscall(12, 0, 0, 0, 0, 0); }
void panic(void)                         { syscall(13, 0, 0, 0, 0, 0); }
void shutdown(void)                      { syscall(14, 0, 0, 0, 0, 0); }
void get_args(char *buf, unsigned int maxlen) { syscall(15, (int)buf, maxlen, 0, 0, 0); }
void exit(void)                          { syscall(SYS_EXIT, 0, 0, 0, 0, 0); for (;;); }
void exit_with_code(int code)         { syscall(SYS_EXIT, code, 0, 0, 0, 0); for (;;); }
void sleep_ms(unsigned int ms)        { syscall(SYS_SLEEP, (int)ms, 0, 0, 0, 0); }
int  waitpid(unsigned int pid)        { return syscall(SYS_WAITPID, (int)pid, 0, 0, 0, 0); }
int  get_children(unsigned int *pids, unsigned int max) {
    return syscall(SYS_GET_CHILDREN, (int)pids, (int)max, 0, 0, 0);
}
int get_random(unsigned char *buf, unsigned int maxlen) {
    return syscall(SYS_RANDOM, (int)buf, (int)maxlen, 0, 0, 0);
}

int get_rtc(struct aos_time *t) {
    return syscall(SYS_RTC, (int)t, 0, 0, 0, 0);
}
unsigned int get_uptime(void) {
    return (unsigned int)syscall(SYS_UPTIME, 0, 0, 0, 0, 0);
}

// Blocking key read: the syscall is non-blocking; spin + yield so the
// scheduler can run other tasks while we wait.
int read_key(void) {
    for (;;) {
        int k = syscall(SYS_READ_KEY, 0, 0, 0, 0, 0);
        if (k >= 0) return k;
        yield();
    }
}

void yield(void)                         { syscall(SYS_YIELD, 0, 0, 0, 0, 0); }
unsigned int getpid(void)                { return syscall(SYS_GETPID, 0, 0, 0, 0, 0); }
int send_msg(unsigned int pid, const struct aos_msg *m) {
    return syscall(SYS_SEND, (int)pid, (int)m, 0, 0, 0);
}
int recv_msg(struct aos_msg *m) {
    return syscall(SYS_RECV, (int)m, 0, 0, 0, 0);
}
int register_events(void)                { return syscall(SYS_EVENT, 0, 0, 0, 0, 0); }
int get_event_pid(void)                  { return syscall(SYS_GETEVENT, 0, 0, 0, 0, 0); }
int get_mouse(int *x, int *y, int *buttons, int *wheel) {
    return syscall(SYS_MOUSE, (int)x, (int)y, (int)buttons, (int)wheel, 0);
}
int get_fb_info(unsigned int *addr, unsigned int *w, unsigned int *h,
                unsigned int *pitch, unsigned int *bpp) {
    return syscall(SYS_FB_INFO, (int)addr, (int)w, (int)h, (int)pitch, (int)bpp);
}
int render_text(unsigned int *buf, unsigned int pitch, int x, int y,
                const char *str, unsigned int fg, unsigned int bg) {
    struct aos_render_req req;
    req.buf = buf;
    req.pitch = pitch;
    req.x = x;
    req.y = y;
    req.str = str;
    req.fg = fg;
    req.bg = bg;
    return syscall(SYS_TEXT, (int)&req, 0, 0, 0, 0);
}
int fill_rect(unsigned int *buf, unsigned int pitch, int x, int y,
              int w, int h, unsigned int rgb) {
    struct aos_fill_req req;
    req.buf = buf;
    req.pitch = pitch;
    req.x = x;
    req.y = y;
    req.w = w;
    req.h = h;
    req.rgb = rgb;
    return syscall(SYS_FILL, (int)&req, 0, 0, 0, 0);
}
int set_stdout(unsigned int pid)         { return syscall(SYS_SETOUT, (int)pid, 0, 0, 0, 0); }
int spawn(const char *path, const char *args, unsigned int sink) {
    return syscall(SYS_SPAWN, (int)path, (int)args, (int)sink, 0, 0);
}

// Simple bump allocator over the user heap region (0x01100000..0x01800000)
static char *heap_next = (char *)0x01100000;

void *malloc(unsigned int size) {
    size = (size + 15) & ~15u;
    char *p = heap_next;
    if ((unsigned int)p + size > 0x01800000) return 0;
    heap_next = p + size;
    return p;
}

void free(void *p) {
    (void)p;
}
