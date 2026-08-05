#include "syscall.h"
#include "interrupts.h"
#include "terminal.h"
#include "vga.h"
#include "fs.h"
#include "string.h"
#include "ports.h"
#include "user.h"
#include "task.h"
#include "mouse.h"
#include "aosipc.h"
#include "linux_syscall.h"
#include "vrng.h"
#include "kmm.h"
#include "rtc.h"

extern volatile unsigned int tick;

static char *prog_args;

// User memory regions (see paging.c): program area + shared window slabs
#define USER_LO 0x01000000
#define USER_HI 0x01804000
#define SLAB_LO 0x03000000
#define SLAB_HI 0x04000000

static int in_user(const void *p, unsigned int n) {
    unsigned int a = (unsigned int)p;
    return a >= USER_LO && n <= USER_HI - a;
}

static int in_user_area(const void *p, unsigned int n) {
    unsigned int a = (unsigned int)p;
    if (a >= USER_LO && n <= USER_HI - a) return 1;
    if (a >= SLAB_LO && n <= SLAB_HI - a) return 1;
    return 0;
}

static char *copy_user_str_alloc(const void *usr) {
    unsigned int a = (unsigned int)usr;
    if (a < USER_LO) return 0;
    unsigned int len = 0;
    while (a + len < USER_HI) {
        if (((const char *)a)[len] == '\0') break;
        len++;
    }
    char *dst = kmalloc(len + 1);
    if (!dst) return 0;
    for (unsigned int i = 0; i <= len; i++)
        dst[i] = ((const char *)a)[i];   // copies the NUL too
    return dst;
}

static char *copy_user_str(const void *usr) {
    return copy_user_str_alloc(usr);
}

static char *copy_user_str2(const void *usr) {
    return copy_user_str_alloc(usr);
}

void syscall_set_args(const char *args) {
    if (!prog_args) prog_args = kmalloc(256);
    if (!prog_args) return;             // OOM: keep args empty, don't crash
    unsigned int i;
    for (i = 0; i < 255 && args[i]; i++)
        prog_args[i] = args[i];
    prog_args[i] = '\0';
}

// Stdout routing: a task with a mailbox sink delivers output as MSG_DATA
// messages (up to 12 bytes each); otherwise output goes to the kernel terminal.
void route_text(const char *s, unsigned int len) {
    unsigned int pid = task_current_pid();
    unsigned int sink = task_current_sink();
    if (pid > 0 && sink > 0 && task_alive(sink)) {
        for (unsigned int i = 0; i < len; i += 12) {
            unsigned int n = len - i;
            if (n > 12) n = 12;
            unsigned int b = 0, c = 0, d = 0;
            for (unsigned int j = 0; j < n; j++) {
                unsigned int byte = (unsigned char)s[i + j];
                if (j < 4)
                    b |= byte << (8 * j);
                else if (j < 8)
                    c |= byte << (8 * (j - 4));
                else
                    d |= byte << (8 * (j - 8));
            }
            task_mailbox_send(sink, MSG_DATA, n, b, c, d);
        }
        return;
    }
    terminal_write(s, len);
}

static void route_hex(unsigned int n) {
    char buf[12];
    const char *hex = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    int len = 2;
    int started = 0;
    for (int i = 28; i >= 0; i -= 4) {
        int d = (n >> i) & 0xF;
        if (d || started || i == 0) {
            buf[len++] = hex[d];
            started = 1;
        }
    }
    route_text(buf, len);
}

static void route_dec(unsigned int n) {
    char buf[12];
    int i = 0;
    if (n == 0) {
        route_text("0", 1);
        return;
    }
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    for (int j = 0; j < i / 2; j++) {
        char t = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = t;
    }
    route_text(buf, i);
}

void syscall_handler(struct registers *r) {
    unsigned int n = r->eax;

    if (task_current_abi() == ABI_LINUX) {
        linux_syscall_handler(r);
        return;
    }

    switch (n) {
    case SYS_PRINT: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s) {
            route_text(s, strlen(s));
            kfree(s);
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_PRINT_HEX:
        route_hex(r->ebx);
        break;
    case SYS_PRINT_DEC:
        route_dec(r->ebx);
        break;
    case SYS_PUTCHAR: {
        char c = (char)r->ebx;
        route_text(&c, 1);
        break;
    }
    case SYS_FS_WRITE: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s && in_user((const void *)r->ecx, r->edx)) {
            r->eax = fs_write(s, (const char *)r->ecx, r->edx);
            kfree(s);
        } else {
            if (s) kfree(s);
            r->eax = -5;
        }
        break;
    }
    case SYS_FS_READ: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s && in_user((const void *)r->ecx, r->edx)) {
            r->eax = fs_read(s, (char *)r->ecx, r->edx);
            kfree(s);
        } else {
            if (s) kfree(s);
            r->eax = -5;
        }
        break;
    }
    case SYS_FS_DELETE: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s) {
            r->eax = fs_delete(s);
            kfree(s);
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_FS_SIZE: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s) {
            r->eax = fs_get_size(s);
            kfree(s);
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_FS_EXISTS: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s) {
            r->eax = fs_exists(s);
            kfree(s);
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_FS_LIST_GET:
        if (in_user((void *)r->ecx, 28) && in_user((void *)r->edx, 4))
            r->eax = sfs_get_entry(r->ebx, (char *)r->ecx, (unsigned int *)r->edx);
        else
            r->eax = -5;
        break;
    case SYS_TICK:
        r->eax = tick;
        break;
    case SYS_CLEAR:
        vga_clear();
        break;
    case SYS_REBOOT:
        unsigned char good = 0x02;
        while (good & 0x02)
            good = inb(0x64);
        outb(0x64, 0xFE);
        for (;;);
    case SYS_PANIC:
        __asm__ volatile("int $0x0");
        break;
    case SYS_SHUTDOWN:
        outw(0x604, 0x2000);
        for (;;);
    case SYS_GET_ARGS: {
        char *dst = (char *)r->ebx;
        unsigned int maxlen = r->ecx;
        if (in_user(dst, maxlen) && maxlen > 0) {
            const char *args = task_current_pid() > 0 ? task_current_args() : prog_args;
            unsigned int i;
            for (i = 0; i < maxlen - 1 && args && args[i]; i++)
                dst[i] = args[i];
            dst[i] = '\0';
            r->eax = i;
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_READ_KEY:
        r->eax = terminal_read_key();
        break;
    case SYS_EXIT:
        if (task_current_pid() == 0)
            user_program_exit();
        else
            task_exit_current(r->ebx);
        break;
    case SYS_YIELD:
        r->eax = 0;
        break;
    case SYS_GETPID:
        r->eax = task_current_pid();
        break;
    case SYS_SEND: {
        struct aos_msg *m = (struct aos_msg *)r->ecx;
        if (in_user(m, sizeof(struct aos_msg))) {
            struct aos_msg mv;
            mv.type = m->type;
            mv.a = m->a;
            mv.b = m->b;
            mv.c = m->c;
            mv.d = m->d;
            r->eax = task_mailbox_send(r->ebx, mv.type, mv.a, mv.b, mv.c, mv.d);
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_RECV: {
        struct aos_msg *m = (struct aos_msg *)r->ebx;
        if (in_user(m, sizeof(struct aos_msg))) {
            unsigned int t, av, bv, cv, dv;
            if (task_mailbox_recv(&t, &av, &bv, &cv, &dv) == 0) {
                m->type = t;
                m->a = av;
                m->b = bv;
                m->c = cv;
                m->d = dv;
                r->eax = 0;
            } else {
                r->eax = -1;
            }
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_EVENT:
        r->eax = task_set_event_pid();
        break;
    case SYS_MOUSE: {
        int *x = (int *)r->ebx;
        int *y = (int *)r->ecx;
        int *b = (int *)r->edx;
        int *w = (int *)r->esi;
        if ((x == 0 || in_user(x, 4)) && (y == 0 || in_user(y, 4)) &&
            (b == 0 || in_user(b, 4)) && (w == 0 || in_user(w, 4))) {
            mouse_get_state(x, y, b, w);
            r->eax = 0;
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_FB_INFO: {
        unsigned int *addr = (unsigned int *)r->ebx;
        unsigned int *w = (unsigned int *)r->ecx;
        unsigned int *h = (unsigned int *)r->edx;
        unsigned int *pitch = (unsigned int *)r->esi;
        unsigned int *bpp = (unsigned int *)r->edi;
        if ((addr == 0 || in_user(addr, 4)) && (w == 0 || in_user(w, 4)) &&
            (h == 0 || in_user(h, 4)) && (pitch == 0 || in_user(pitch, 4)) &&
            (bpp == 0 || in_user(bpp, 4))) {
            unsigned int a, wv, hv, pv, bv;
            vga_get_fb_dimensions(&a, &wv, &hv, &pv, &bv);
            if (addr) *addr = a;
            if (w) *w = wv;
            if (h) *h = hv;
            if (pitch) *pitch = pv;
            if (bpp) *bpp = bv;
            r->eax = 0;
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_TEXT: {
        struct aos_render_req *req = (struct aos_render_req *)r->ebx;
        char *s = (in_user(req, sizeof(struct aos_render_req)) &&
                   in_user_area(req->buf, 1))
                      ? copy_user_str_alloc(req->str)
                      : 0;
        if (s) {
            vga_render_text_buffer(req->buf, req->pitch, req->x, req->y,
                                   s, req->fg, req->bg);
            kfree(s);
            r->eax = 0;
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_FILL: {
        struct aos_fill_req *req = (struct aos_fill_req *)r->ebx;
        if (in_user(req, sizeof(struct aos_fill_req)) &&
            in_user_area(req->buf, 1)) {
            vga_fill_buffer(req->buf, req->pitch, req->x, req->y,
                            req->w, req->h, req->rgb);
            r->eax = 0;
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_SETOUT:
        r->eax = task_set_sink(r->ebx);
        break;
    case SYS_SPAWN: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s) {
            char *a = r->ecx ? copy_user_str2((const void *)r->ecx) : 0;
            if (r->ecx && !a) {
                kfree(s);
                r->eax = -5;
                break;
            }
            unsigned int pid;
            int rc = task_spawn(s, a, r->edx, &pid);
            kfree(s);
            if (a) kfree(a);
            r->eax = rc == 0 ? (int)pid : rc;
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_GETEVENT:
        r->eax = task_event_pid();
        break;
    case SYS_SLEEP:
        task_sleep(r->ebx);
        r->eax = 0;
        break;
    case SYS_WAITPID:
        r->eax = task_waitpid(r->ebx);
        break;
    case SYS_GET_CHILDREN: {
        unsigned int max = r->ecx;
        if (max > MAX_TASKS) max = MAX_TASKS;
        if (in_user((void *)r->ebx, max * 4)) {
            int n = task_get_children((unsigned int *)r->ebx, max);
            r->eax = n;
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_RANDOM: {
        unsigned char *buf = (unsigned char *)r->ebx;
        unsigned int maxlen = r->ecx;
        if (maxlen > 512) maxlen = 512;
        if (maxlen > 0 && in_user(buf, maxlen))
            r->eax = vrng_bytes(buf, maxlen);
        else
            r->eax = -5;
        break;
    }
    case SYS_RTC: {
        struct aos_time *t = (struct aos_time *)r->ebx;
        if (in_user(t, sizeof(struct aos_time))) {
            if (rtc_get(t) == 0) r->eax = 0;
            else r->eax = -1;
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_UPTIME:
        r->eax = tick / 1000;             // seconds since boot
        break;
    default:
        r->eax = -1;
        break;
    }
}
