#ifndef AOSABI_H
#define AOSABI_H

// Single source of truth for the AOS userland ABI: data structures, message
// constants, extension syscall numbers (int 0x80 500-599) and wrappers.
// Included by BOTH the kernel (with -D__AOS_KERNEL__) and musl user programs.
// The kernel sees only the structs/constants; user programs additionally get
// <unistd.h> `syscall()` wrappers guarded by #ifndef __AOS_KERNEL__.

// ---- Extension syscall numbers (int 0x80, 500-519) ----
#define AOS_EXT           500
#define AOS_FB_INFO       500
#define AOS_TEXT          501
#define AOS_FILL          502
#define AOS_CLEAR         503
#define AOS_MOUSE         504
#define AOS_READ_KEY      505
#define AOS_KEY_POLL      506
#define AOS_REG_EVENTS    507
#define AOS_GET_EVENT_PID 508
#define AOS_SEND          509
#define AOS_RECV          510
#define AOS_SETOUT        511
#define AOS_SPAWN         512
#define AOS_WAITPID       513
#define AOS_GET_CHILDREN  514
#define AOS_GET_ARGS      515
#define AOS_GET_RTC       516
#define AOS_UPTIME        517
#define AOS_GET_TICK      518
#define AOS_PANIC         519

// ---- Mailbox message types ----
#define MSG_KEY     1   // key event: a = codepoint (or GUI_KEY_* special code)
#define MSG_DATA    2   // stdout data: a = byte count, b/c/d = 12 payload bytes
#define MSG_UPDATE  3   // app -> wm: "repaint window"   a = winid
#define MSG_CREATE  4   // app -> wm: "create window"    a = width, b = height
#define MSG_WININFO 5   // wm -> app: "window ready"     a = winid, b = slab
#define MSG_EXIT    6   // kernel -> sink: "task exited" a = pid
#define MSG_CLOSE   7   // wm -> app: "please exit"    a = unused

// ---- Shared-memory window slabs (identity-mapped, user-accessible) ----
#define AOS_SLAB_BASE 0x03000000
#define AOS_SLAB_SIZE 0x100000
#define AOS_SLABS     16

struct aos_msg {
    unsigned int type;
    unsigned int a, b, c, d;
};

struct aos_stat {
    unsigned int type;    // 1 file, 2 dir
    unsigned int size;
    unsigned int mtime;
    unsigned int nlink;
};

struct aos_time {
    int year, month, day, hour, minute, second;
};

struct aos_render_req {
    unsigned int *buf;
    unsigned int pitch;
    int x, y;
    const char *str;
    unsigned int fg;
    unsigned int bg;
};

struct aos_fill_req {
    unsigned int *buf;
    unsigned int pitch;
    int x, y, w, h;
    unsigned int rgb;
};

// Open flags (Linux-compatible values), used by file syscalls. Kept with the
// historical non-prefixed names so the earlier AOS programs keep compiling.
#define O_RDONLY   0x00000
#define O_WRONLY   0x00001
#define O_RDWR     0x00002
#define O_CREAT    0x00040
#define O_TRUNC    0x00200
#define O_APPEND   0x00400
#define O_DIRECTORY 0x10000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// ---- User-side only: musl syscall() and thin convenience wrappers ----
#ifndef __AOS_KERNEL__
#include <unistd.h>

static int aos_syscall(int n, int a, int b, int c, int d, int e) {
    return (int)syscall(n, a, b, c, d, e);
}

static int aos_fb_info(unsigned int *addr, unsigned int *w, unsigned int *h,
                       unsigned int *pitch, unsigned int *bpp) {
    return aos_syscall(AOS_FB_INFO, (int)addr, (int)w, (int)h,
                       (int)pitch, (int)bpp);
}
static int aos_render_text(unsigned int *buf, unsigned int pitch,
                           int x, int y, const char *str,
                           unsigned int fg, unsigned int bg) {
    struct aos_render_req req;
    req.buf = buf; req.pitch = pitch; req.x = x; req.y = y;
    req.str = str; req.fg = fg; req.bg = bg;
    return aos_syscall(AOS_TEXT, (int)&req, 0, 0, 0, 0);
}
static int aos_fill(unsigned int *buf, unsigned int pitch,
                    int x, int y, int w, int h, unsigned int rgb) {
    struct aos_fill_req req;
    req.buf = buf; req.pitch = pitch; req.x = x; req.y = y;
    req.w = w; req.h = h; req.rgb = rgb;
    return aos_syscall(AOS_FILL, (int)&req, 0, 0, 0, 0);
}
static void aos_clear(void) { aos_syscall(AOS_CLEAR, 0, 0, 0, 0, 0); }
static void aos_panic(void) { aos_syscall(AOS_PANIC, 0, 0, 0, 0, 0); }
static int aos_mouse(int *x, int *y, int *buttons, int *wheel) {
    return aos_syscall(AOS_MOUSE, (int)x, (int)y, (int)buttons, (int)wheel, 0);
}
static int aos_read_key(void) { return aos_syscall(AOS_READ_KEY, 0, 0, 0, 0, 0); }
static int aos_key_poll(void) { return aos_syscall(AOS_KEY_POLL, 0, 0, 0, 0, 0); }
static int aos_register_events(void) { return aos_syscall(AOS_REG_EVENTS, 0, 0, 0, 0, 0); }
static int aos_get_event_pid(void) { return aos_syscall(AOS_GET_EVENT_PID, 0, 0, 0, 0, 0); }
static int aos_send(unsigned int pid, const struct aos_msg *m) {
    return aos_syscall(AOS_SEND, (int)pid, (int)m, 0, 0, 0);
}
static int aos_recv(struct aos_msg *m) { return aos_syscall(AOS_RECV, (int)m, 0, 0, 0, 0); }
static int aos_setout(unsigned int pid) { return aos_syscall(AOS_SETOUT, (int)pid, 0, 0, 0, 0); }
static int aos_spawn(const char *path, const char *args, unsigned int sink) {
    return aos_syscall(AOS_SPAWN, (int)path, (int)args, (int)sink, 0, 0);
}
static int aos_waitpid(unsigned int pid) { return aos_syscall(AOS_WAITPID, (int)pid, 0, 0, 0, 0); }
static int aos_get_children(unsigned int *pids, unsigned int max) {
    return aos_syscall(AOS_GET_CHILDREN, (int)pids, (int)max, 0, 0, 0);
}
static int aos_get_args(char *buf, unsigned int max) {
    return aos_syscall(AOS_GET_ARGS, (int)buf, (int)max, 0, 0, 0);
}
static int aos_get_rtc(struct aos_time *t) { return aos_syscall(AOS_GET_RTC, (int)t, 0, 0, 0, 0); }
static unsigned int aos_uptime(void) { return (unsigned int)aos_syscall(AOS_UPTIME, 0, 0, 0, 0, 0); }
static unsigned int aos_get_tick(void) { return (unsigned int)aos_syscall(AOS_GET_TICK, 0, 0, 0, 0, 0); }
#endif

#endif