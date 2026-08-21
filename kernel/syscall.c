#include "syscall.h"
#include "interrupts.h"
#include "terminal.h"
#include "vga.h"
#include "vfs.h"
#include "string.h"
#include "ports.h"
#include "user.h"
#include "task.h"
#include "mouse.h"
#include "aosipc.h"
#include "linux_syscall.h"
#include "commands.h"
#include "vrng.h"
#include "kmm.h"
#include "rtc.h"
#include "trace.h"
#include "serial.h"

extern volatile unsigned int tick;

static char *prog_args;

// Current task's cwd, resolved to a referenced inode. Callers must vfs_put()
// the result. The task stores cwd as a normalized absolute path string.
struct vfs_inode *current_task_cwd(void) {
    return vfs_resolve(0, get_current_task()->cwd, 0);
}

// User memory regions (see paging.c): program area + shared window slabs
#define USER_LO 0x01000000
#define USER_HI 0x01804000
#define SLAB_LO 0x03000000
#define SLAB_HI 0x04000000

static int in_user(const void *p, unsigned int n) {
    unsigned int a = (unsigned int)p;
    return a >= USER_LO && a < USER_HI && n <= USER_HI - a;
}

static int in_user_area(const void *p, unsigned int n) {
    unsigned int a = (unsigned int)p;
    if (a >= USER_LO && a < USER_HI && n <= USER_HI - a) return 1;
    if (a >= SLAB_LO && a < SLAB_HI && n <= SLAB_HI - a) return 1;
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

int route_text(const char *s, unsigned int len) {
    struct task *t = get_current_task();
    if (t->stdout_fd >= 0)
        return vfs_write_fd(t->stdout_fd, s, len);
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
        return 0;
    }
    terminal_write(s, len);
    return 0;
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

    if (n >= AOS_EXT && n < AOS_EXT + 100) {
        aos_gui_handler(r);
        return;
    }

    if (task_current_abi() == ABI_LINUX) {
        linux_syscall_handler(r);
        return;
    }

    trace_record(r);
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
    case SYS_OPEN: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s) {
            struct vfs_inode *cwd = current_task_cwd();
            int fd = vfs_open_fd(cwd, s, (int)r->ecx);
            vfs_put(cwd);
            if (fd >= 0 && fd < TASK_MAX_FDS)
                get_current_task()->fds[fd] = vfs_ofile_ptr(fd);
            r->eax = fd;
            kfree(s);
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_CLOSE: {
        int fd = (int)r->ebx;
        struct task *t = get_current_task();
        // Console pseudo-fds (0/1/2) live only in the task table; clearing the
        // slot frees it. Real fds are closed through the VFS table.
        if (fd >= 0 && fd < 3) {
            if (fd < TASK_MAX_FDS && t->fds[fd] == &console_open_file)
                t->fds[fd] = 0;
            r->eax = 0;
        } else {
            int rc = vfs_close_fd(fd);
            if (rc == 0 && fd >= 0 && fd < TASK_MAX_FDS)
                t->fds[fd] = 0;
            r->eax = rc;
        }
        break;
    }
    case SYS_READ: {
        int fd = (int)r->ebx;
        void *buf = (void *)r->ecx;
        unsigned int len = r->edx;
        if (fd == 0) {
            if (len == 0 || !in_user(buf, len)) {
                r->eax = -5;
                break;
            }
            if (get_current_task()->stdin_fd >= 0) {
                r->eax = vfs_read_fd(get_current_task()->stdin_fd, buf, len);
                break;
            }
            int k = terminal_read_key();
            if (k < 0) {
                r->eax = -1;             // non-blocking, like SYS_READ_KEY
            } else {
                ((char *)buf)[0] = (char)k;
                r->eax = 1;
            }
        } else if (fd == 1 || fd == 2) {
            r->eax = VFS_EBADF;          // can't read stdout
        } else {
            if (!in_user(buf, len)) {
                r->eax = -5;
                break;
            }
            r->eax = vfs_read_fd(fd, buf, len);
        }
        break;
    }
    case SYS_WRITE: {
        int fd = (int)r->ebx;
        const char *buf = (const char *)r->ecx;
        unsigned int len = r->edx;
        if (!in_user(buf, len)) {
            r->eax = -5;
            break;
        }
        if (fd == 1 || fd == 2) {
            int rc = route_text(buf, len);
            r->eax = (rc < 0) ? rc : (int)len;
        } else if (fd == 0) {
            r->eax = VFS_EBADF;          // can't write stdin
        } else {
            r->eax = vfs_write_fd(fd, buf, len);
        }
        break;
    }
    case SYS_LSEEK:
        r->eax = vfs_lseek_fd((int)r->ebx, (int)r->ecx, (int)r->edx);
        break;
    case SYS_MKDIR: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s) {
            struct vfs_inode *cwd = current_task_cwd();
            r->eax = vfs_mkdir(cwd, s);
            vfs_put(cwd);
            kfree(s);
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_RMDIR: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s) {
            struct vfs_inode *cwd = current_task_cwd();
            r->eax = vfs_rmdir(cwd, s);
            vfs_put(cwd);
            kfree(s);
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_READDIR: {
        char *buf = (char *)r->ecx;
        unsigned int len = r->edx;
        if (in_user(buf, len))
            r->eax = vfs_readdir_fd((int)r->ebx, buf, len);
        else
            r->eax = -5;
        break;
    }
    case SYS_CHDIR: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s) {
            struct task *t = get_current_task();
            char nb[PATH_MAX];
            if (path_norm(t->cwd, s, nb, sizeof(nb)) == 0) {
                struct aos_stat st;
                if (vfs_kernel_stat(nb, &st) == 0 && st.type == 2) {
                    strncpy(t->cwd, nb, PATH_MAX);
                    t->cwd[PATH_MAX - 1] = '\0';
                    r->eax = 0;
                } else {
                    r->eax = VFS_ENOTDIR;
                }
            } else {
                r->eax = VFS_EINVAL;
            }
            kfree(s);
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_GETCWD: {
        char *buf = (char *)r->ebx;
        unsigned int len = r->ecx;
        if (in_user(buf, len)) {
            const char *c = get_current_task()->cwd;
            unsigned int i = 0;
            while (c[i] && i + 1 < len) {
                buf[i] = c[i];
                i++;
            }
            buf[i] = '\0';
            r->eax = 0;
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_STAT: {
        char *s = copy_user_str((const void *)r->ebx);
        struct aos_stat *st = (struct aos_stat *)r->ecx;
        if (s && in_user(st, sizeof(struct aos_stat))) {
            struct vfs_inode *cwd = current_task_cwd();
            r->eax = vfs_stat(cwd, s, st);
            vfs_put(cwd);
            kfree(s);
        } else {
            if (s) kfree(s);
            r->eax = -5;
        }
        break;
    }
    case SYS_FSTAT: {
        struct aos_stat *st = (struct aos_stat *)r->ecx;
        if (in_user(st, sizeof(struct aos_stat)))
            r->eax = vfs_fstat_fd((int)r->ebx, st);
        else
            r->eax = -5;
        break;
    }
    case SYS_UNLINK: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s) {
            struct vfs_inode *cwd = current_task_cwd();
            r->eax = vfs_unlink(cwd, s);
            vfs_put(cwd);
            kfree(s);
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_TICK:
        r->eax = tick;
        break;
    case SYS_CLEAR:
        vga_clear();
        break;
    case SYS_REBOOT:
        vfs_sync();
        unsigned char good = 0x02;
        while (good & 0x02)
            good = inb(0x64);
        outb(0x64, 0xFE);
        for (;;);
    case SYS_PANIC:
        __asm__ volatile("int $0x0");
        break;
    case SYS_SHUTDOWN:
        vfs_sync();
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
        if (task_current_pid() == 0) {
            shell_set_status((int)r->ebx);
            user_program_exit();
        } else {
            task_exit_current(r->ebx);
        }
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
            int rc = task_spawn(s, a, r->edx, &pid, 0);
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
    trace_finish(r);
}
