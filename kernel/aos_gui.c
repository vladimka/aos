#include "interrupts.h"
#include "syscall.h"
#include "terminal.h"
#include "vga.h"
#include "virtio_gpu.h"
#include "mouse.h"
#include "vfs.h"
#include "task.h"
#include "user.h"
#include "kmm.h"
#include "rtc.h"
#include "vrng.h"
#include "string.h"
#include "aosipc.h"
#include "linux_syscall.h"   // struct linux_ctx (task.h only forward-declares)
#include "trace.h"

// AOS GUI / extension syscalls (int 0x80, numbers 500-523). These are routed
// from syscall_handler when n >= AOS_EXT. Every AOS task is ABI_LINUX, so the
// target user-window for pointer checks is the file's linux_ctx (win_lo..hi),
// not the old fixed 0x01000000 user area.

extern volatile unsigned int tick;

static struct linux_ctx *lcx(void) {
    return task_current_lctx();
}

// Shared slab window the GUI apps render into (mirrors syscall.c USER/SLAB).
#define SLAB_LO 0x03000000
#define SLAB_HI 0x04000000

static int in_luser(const void *p, unsigned int n) {
    unsigned int a = (unsigned int)p;
    struct linux_ctx *lc = lcx();
    if (a >= lc->win_lo && a < lc->win_hi && n <= lc->win_hi - a) return 1;
    if (a >= SLAB_LO && a < SLAB_HI && n <= SLAB_HI - a) return 1;
    return 0;
}

static char *copy_lstr(const void *usr) {
    unsigned int a = (unsigned int)usr;
    struct linux_ctx *lc = lcx();
    if (a < lc->win_lo) return 0;
    unsigned int len = 0;
    while (a + len < lc->win_hi) {
        if (((const char *)a)[len] == '\0') break;
        len++;
    }
    char *dst = kmalloc(len + 1);
    if (!dst) return 0;
    for (unsigned int i = 0; i <= len; i++)
        dst[i] = ((const char *)a)[i];
    return dst;
}

/* Copy a double-NUL-terminated KEY=value block from user memory.
 * copy_lstr() stops at the first NUL and would truncate the block to its
 * first variable (stack_build expects the full "A=1\0B=2\0\0" array). */
#define ENV_BLOCK_MAX 1024
static char *copy_env_block(const void *usr) {
    unsigned int a = (unsigned int)usr;
    struct linux_ctx *lc = lcx();
    if (a < lc->win_lo) return 0;
    unsigned int len = 0;
    int prev_nul = 0;
    while (len < ENV_BLOCK_MAX && a + len < lc->win_hi) {
        char c = ((const char *)a)[len];
        len++;
        if (prev_nul && c == '\0') break;
        prev_nul = (c == '\0');
    }
    char *dst = kmalloc(len + 1);
    if (!dst) return 0;
    for (unsigned int i = 0; i < len; i++)
        dst[i] = ((const char *)a)[i];
    dst[len] = '\0';
    return dst;
}

// Shared body of AOS_SPAWN_FDS (env = 0) and AOS_SPAWN_FDS_ENV (env copied
// from %edi). Parses the redir pair list in %esi, dups real global fds into
// private slots owned by the child, spawns, then wires stdin/stdout/fds.
static int spawn_fds_common(struct registers *r, const char *env) {
    char *s = copy_lstr((const void *)r->ebx);
    if (!s) return -5;
    char *a = r->ecx ? copy_lstr((const void *)r->ecx) : 0;
    if (r->ecx && !a) { kfree(s); return -5; }
    struct aos_redir redirs[16];
    int nredirs = 0;
    const struct aos_redir *rp = (const struct aos_redir *)r->esi;
    while (rp && nredirs < 16) {
        if (!in_luser(rp, 8)) { kfree(s); if (a) kfree(a); return -5; }
        struct aos_redir rv;
        rv.child_fd = ((const unsigned int *)rp)[0];
        rv.global_fd = ((const unsigned int *)rp)[1];
        if (rv.child_fd == 0xFFFFFFFF) break;
        if (rv.child_fd >= TASK_MAX_FDS) {
            kfree(s); if (a) kfree(a); return -5;
        }
        if (rv.global_fd == AOS_INHERIT_FD) {
            if (!get_current_task()->fds[rv.child_fd]) {
                kfree(s); if (a) kfree(a); return -5;
            }
        } else if (rv.global_fd < 3 || rv.global_fd >= VFS_OFILES ||
                   !vfs_ofile_ptr(rv.global_fd)) {
            kfree(s); if (a) kfree(a); return -5;
        }
        redirs[nredirs++] = rv;
        rp = (const struct aos_redir *)((const char *)rp + 8);
    }
    if (rp && nredirs >= 16) {              // too many pairs
        kfree(s); if (a) kfree(a); return -5;
    }
    unsigned int pid;
    // Phase 1: dup every real redirect into a private global slot owned by the
    // child (vfs_dup_fd bumps pipe counters via pipe_dup). AOS_INHERIT_FD and
    // console fds (0/1/2) are passed through untouched.
    for (int i = 0; i < nredirs; i++) {
        if (redirs[i].global_fd == AOS_INHERIT_FD) continue;
        int g2 = vfs_dup_fd((int)redirs[i].global_fd);
        if (g2 < 0) {
            for (int j = 0; j < i; j++)
                if (redirs[j].global_fd != AOS_INHERIT_FD)
                    vfs_close_fd((int)redirs[j].global_fd);
            kfree(s); if (a) kfree(a); return g2;
        }
        redirs[i].global_fd = (unsigned int)g2;
    }
    int rc = task_spawn(s, a, r->edx, &pid, env);
    if (rc == 0) {
        struct task *c = task_slot(pid);
        struct task *parent = get_current_task();
        for (int i = 0; i < nredirs && c; i++) {
            unsigned int cfd = redirs[i].child_fd;
            unsigned int g = redirs[i].global_fd;
            if (g == AOS_INHERIT_FD) {
                // inherit: child references the parent's handle (parent owns it)
                if (cfd == 0) c->stdin_fd = parent->stdin_fd;
                else if (cfd == 1) c->stdout_fd = parent->stdout_fd;
                else c->fds[cfd] = parent->fds[cfd];
            } else {
                // redirect: child gets its OWN dup'd global slot (it will close it)
                c->fds[g] = vfs_ofile_ptr(g);
                if (cfd == 0) c->stdin_fd = (int)g;
                else if (cfd == 1) c->stdout_fd = (int)g;
            }
        }
        kfree(s);
        if (a) kfree(a);
        return (int)pid;
    }
    kfree(s);
    if (a) kfree(a);
    return rc;
}

void aos_gui_handler(struct registers *r) {
    unsigned int n = r->eax;
    trace_record(r);
    switch (n) {
    case AOS_FB_INFO: {
        unsigned int *addr = (unsigned int *)r->ebx;
        unsigned int *wdst = (unsigned int *)r->ecx;
        unsigned int *hdst = (unsigned int *)r->edx;
        unsigned int *pdst = (unsigned int *)r->esi;
        unsigned int *bdst = (unsigned int *)r->edi;
        if ((addr == 0 || in_luser(addr, 4)) && (wdst == 0 || in_luser(wdst, 4)) &&
            (hdst == 0 || in_luser(hdst, 4)) && (pdst == 0 || in_luser(pdst, 4)) &&
            (bdst == 0 || in_luser(bdst, 4))) {
            unsigned int a, wv, hv, pv, bv;
            if (vgu_active()) {
                a = vgu_back(); wv = 1024; hv = 768; pv = 4096; bv = 32;
            } else {
                vga_get_fb_dimensions(&a, &wv, &hv, &pv, &bv);
            }
            if (addr) *addr = a;
            if (wdst) *wdst = wv;
            if (hdst) *hdst = hv;
            if (pdst) *pdst = pv;
            if (bdst) *bdst = bv;
            r->eax = 0;
        } else {
            r->eax = -5;
        }
        break;
    }
    case AOS_GPU_INFO: {
        unsigned int *addr = (unsigned int *)r->ebx;
        unsigned int *wdst = (unsigned int *)r->ecx;
        unsigned int *hdst = (unsigned int *)r->edx;
        unsigned int *pdst = (unsigned int *)r->esi;
        unsigned int *adst = (unsigned int *)r->edi;
        if ((addr == 0 || in_luser(addr, 4)) && (wdst == 0 || in_luser(wdst, 4)) &&
            (hdst == 0 || in_luser(hdst, 4)) && (pdst == 0 || in_luser(pdst, 4)) &&
            (adst == 0 || in_luser(adst, 4))) {
            unsigned int a, wv, hv, pv;
            vgu_info(&wv, &hv, &pv);
            a = vgu_back();
            if (addr) *addr = a;
            if (wdst) *wdst = wv;
            if (hdst) *hdst = hv;
            if (pdst) *pdst = pv;
            if (adst) *adst = vgu_active();
            r->eax = 0;
        } else {
            r->eax = -5;
        }
        break;
    }
    case AOS_GPU_FLIP:
        vgu_flip();
        r->eax = 0;
        break;
    case AOS_CURSOR:
        vgu_cursor((int)r->ebx, (int)r->ecx, (int)r->edx);
        r->eax = 0;
        break;
    case AOS_TEXT: {
        struct aos_render_req *req = (struct aos_render_req *)r->ebx;
        if (in_luser(req, sizeof(struct aos_render_req)) &&
            in_luser(req->buf, 4)) {
            char *s = copy_lstr(req->str);
            if (s) {
                vga_render_text_buffer(req->buf, req->pitch, req->x, req->y,
                                       s, req->fg, req->bg);
                kfree(s);
                r->eax = 0;
            } else {
                r->eax = -5;
            }
        } else {
            r->eax = -5;
        }
        break;
    }
    case AOS_FILL: {
        struct aos_fill_req *req = (struct aos_fill_req *)r->ebx;
        if (in_luser(req, sizeof(struct aos_fill_req)) &&
            in_luser(req->buf, 4)) {
            vga_fill_buffer(req->buf, req->pitch, req->x, req->y,
                            req->w, req->h, req->rgb);
            r->eax = 0;
        } else {
            r->eax = -5;
        }
        break;
    }
    case AOS_CLEAR:
        vga_clear();
        r->eax = 0;
        break;
    case AOS_MOUSE: {
        int *x = (int *)r->ebx;
        int *y = (int *)r->ecx;
        int *b = (int *)r->edx;
        int *w = (int *)r->esi;
        if ((x == 0 || in_luser(x, 4)) && (y == 0 || in_luser(y, 4)) &&
            (b == 0 || in_luser(b, 4)) && (w == 0 || in_luser(w, 4))) {
            mouse_get_state(x, y, b, w);
            r->eax = 0;
        } else {
            r->eax = -5;
        }
        break;
    }
    case AOS_READ_KEY: {
        r->eax = -1;
        for (;;) {
            int k = terminal_read_key();
            if (k >= 0) { r->eax = k; break; }
            task_sleep(1);          // block ~1ms; scheduler can run others
        }
        break;
    }
    case AOS_KEY_POLL:
        r->eax = terminal_read_key();
        break;
    case AOS_REG_EVENTS:
        task_set_event_pid();
        r->eax = 0;
        break;
    case AOS_GET_EVENT_PID:
        r->eax = task_event_pid();
        break;
    case AOS_SEND: {
        struct aos_msg *m = (struct aos_msg *)r->ecx;
        if (in_luser(m, sizeof(struct aos_msg))) {
            struct aos_msg mv;
            mv.type = m->type;
            mv.a = m->a;
            mv.b = m->b;
            mv.c = m->c;
            mv.d = m->d;
            const char *title = (const char *)0;
            char title_buf[16];
            if (mv.d && in_luser((void *)mv.d, 1)) {
                const char *src = (const char *)mv.d;
                int j;
                for (j = 0; j < 15 && src[j]; j++)
                    title_buf[j] = src[j];
                title_buf[j] = '\0';
                title = title_buf;
            }
            r->eax = task_mailbox_send(r->ebx, mv.type, mv.a, mv.b, mv.c, mv.d, title);
        } else {
            r->eax = -5;
        }
        break;
    }
    case AOS_RECV: {
        struct aos_msg *m = (struct aos_msg *)r->ebx;
        if (in_luser(m, sizeof(struct aos_msg))) {
            unsigned int t, a, b, c, d;
            char title_buf[16] = {0};
            if (task_mailbox_recv(&t, &a, &b, &c, &d, title_buf) == 0) {
                m->type = t; m->a = a; m->b = b; m->c = c; m->d = d;
                char *dst = (char *)(m + 1);
                if (in_luser(dst, 16)) {
                    int j;
                    for (j = 0; j < 16; j++)
                        dst[j] = title_buf[j];
                }
                r->eax = 0;
            } else {
                r->eax = -1;
            }
        } else {
            r->eax = -5;
        }
        break;
    }
    case AOS_SETOUT:
        r->eax = task_set_sink(r->ebx);
        break;
    case AOS_SPAWN: {
        char *s = copy_lstr((const void *)r->ebx);
        if (s) {
            char *a = r->ecx ? copy_lstr((const void *)r->ecx) : 0;
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
    case AOS_SPAWN_FDS:
        r->eax = spawn_fds_common(r, 0);
        break;
    case AOS_SPAWN_FDS_ENV: {
        char *envb = r->edi ? copy_env_block((const void *)r->edi) : 0;
        if (r->edi && !envb) { r->eax = -5; break; }
        r->eax = spawn_fds_common(r, envb);
        kfree(envb);
        break;
    }
    case AOS_WAITPID:
        r->eax = task_waitpid(r->ebx);
        break;
    case AOS_KILL:
        r->eax = task_kill(r->ebx);
        break;
    case AOS_TRACE_SET: {
        // strace(1): toggle THIS task's syscall trace flag. Children spawned
        // while it is set inherit it (strace -f semantics, race-free).
        struct task *me = get_current_task();
        r->eax = me->trace_on ? 1 : 0;
        me->trace_on = r->ebx ? 1 : 0;
        break;
    }
    case AOS_TRACE_DUMP:
        // Dump (and reap the buffers of) every traced task of this strace
        // session; ebx = the caller's pid = session owner.
        r->eax = (int)trace_session_dump_root(r->ebx);
        break;
    case AOS_GET_CHILDREN: {
        unsigned int max = r->ecx;
        if (max > MAX_TASKS) max = MAX_TASKS;
        if (in_luser((void *)r->ebx, max * 4)) {
            r->eax = task_get_children((unsigned int *)r->ebx, max);
        } else {
            r->eax = -5;
        }
        break;
    }
    case AOS_GET_ARGS: {
        char *dst = (char *)r->ebx;
        unsigned int maxlen = r->ecx;
        if (in_luser(dst, maxlen) && maxlen > 0) {
            const char *args = task_current_args();
            unsigned int i;
            for (i = 0; i < maxlen - 1 && args && args[i]; i++)
                dst[i] = args[i];
            dst[i] = '\0';
            r->eax = (int)i;
        } else {
            r->eax = -5;
        }
        break;
    }
    case AOS_GET_RTC: {
        struct aos_time *t = (struct aos_time *)r->ebx;
        if (in_luser(t, sizeof(struct aos_time))) {
            r->eax = (rtc_get(t) == 0) ? 0 : -1;
        } else {
            r->eax = -5;
        }
        break;
    }
    case AOS_UPTIME:
        r->eax = tick / 1000;
        break;
    case AOS_GET_TICK:
        r->eax = tick;
        break;
    case AOS_PANIC:
        __asm__ volatile("int $0x0");
        break;
    default:
        r->eax = -1;
        break;
    }
    trace_finish(r);
}