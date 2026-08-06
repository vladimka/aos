#include "interrupts.h"
#include "syscall.h"
#include "terminal.h"
#include "vga.h"
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

// AOS GUI / extension syscalls (int 0x80, numbers 500-519). These are routed
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
    if (a >= lc->win_lo && n <= lc->win_hi - a) return 1;
    if (a >= SLAB_LO && n <= SLAB_HI - a) return 1;
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
            vga_get_fb_dimensions(&a, &wv, &hv, &pv, &bv);
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
            r->eax = task_mailbox_send(r->ebx, mv.type, mv.a, mv.b, mv.c, mv.d);
        } else {
            r->eax = -5;
        }
        break;
    }
    case AOS_RECV: {
        struct aos_msg *m = (struct aos_msg *)r->ebx;
        if (in_luser(m, sizeof(struct aos_msg))) {
            unsigned int t, a, b, c, d;
            if (task_mailbox_recv(&t, &a, &b, &c, &d) == 0) {
                m->type = t; m->a = a; m->b = b; m->c = c; m->d = d;
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
            int rc = task_spawn(s, a, r->edx, &pid);
            kfree(s);
            if (a) kfree(a);
            r->eax = rc == 0 ? (int)pid : rc;
        } else {
            r->eax = -5;
        }
        break;
    }
    case AOS_WAITPID:
        r->eax = task_waitpid(r->ebx);
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