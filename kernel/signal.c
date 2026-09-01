#include "interrupts.h"
#include "signal.h"
#include "task.h"
#include "user.h"
#include "string.h"
#include "kmm.h"
#include "linux_syscall.h"

// Linux i386 signal ABI (musl bits/signal.h)
// struct sigaction (140 B):
//   sa_handler   +0
//   sa_mask      +4   (sigset_t = 128 B)
//   sa_flags     +132
//   sa_restorer  +136
// mcontext gregs indices:
//   REG_GS=0 FS=1 ES=2 DS=3 EDI=4 ESI=5 EBP=6 ESP=7 EBX=8
//   EDX=9 ECX=10 EAX=11 TRAPNO=12 ERR=13 EIP=14 CS=15 EFL=16 UESP=17 SS=18
// ucontext layout:
//   uc_flags(0) uc_link(4) uc_stack(8,12) uc_mcontext(20,88)
//   uc_sigmask(108,128)
// mcontext (88 B): gregs[19](0,76) fpregs(76) oldmask(80) cr2(84)

#define SA_RESTORER 0x04000000

// Range-check a user pointer against the current Linux window.
static int in_lin_win(unsigned int a, unsigned int n) {
    struct linux_ctx *lc = task_current_lctx();
    return a >= lc->win_lo && a < lc->win_hi && n <= lc->win_hi - a;
}

// gregs indices (array offsets in words)
#define RG_GS 0
#define RG_FS 1
#define RG_ES 2
#define RG_DS 3
#define RG_EDI 4
#define RG_ESI 5
#define RG_EBP 6
#define RG_ESP 7
#define RG_EBX 8
#define RG_EDX 9
#define RG_ECX 10
#define RG_EAX 11
#define RG_TRAPNO 12
#define RG_ERR 13
#define RG_EIP 14
#define RG_CS 15
#define RG_EFL 16
#define RG_UESP 17
#define RG_SS 18

// default-ignored signals when set to SIG_DFL
static int sig_default_ignore(int sig) {
    switch (sig) {
    case 17:  // SIGCHLD
    case 18:  // SIGCONT
    case 23:  // SIGURG
    case 28:  // SIGWINCH
        return 1;
    }
    return 0;
}

static struct aos_sigstate *sig_state(struct task *t) {
    if (!t->sig) {
        t->sig = kmalloc(sizeof(struct aos_sigstate));
        if (!t->sig) return 0;
        memset(t->sig, 0, sizeof(struct aos_sigstate));
    }
    return t->sig;
}

void sig_reset(struct task *t) {
    // execve: reset all handlers to SIG_DFL, clear pending and block.
    // (SIG_IGN handlers would normally survive exec, but we reset for simplicity.)
    if (!t->sig) return;
    memset(t->sig->sigact, 0, sizeof(t->sig->sigact));
    memset(t->sig->pending, 0, sizeof(t->sig->pending));
    memset(t->sig->block, 0, sizeof(t->sig->block));
}

void sig_pending_set(struct task *t, int sig) {
    if (sig < 1 || sig > 64) return;
    if (sig_state(t)) {
        int w = (sig - 1) >> 5;
        t->sig->pending[w] |= 1u << ((sig - 1) & 31);
    }
}

void sig_pending_clear(struct task *t, int sig) {
    if (sig < 1 || sig > 64) return;
    if (t->sig) {
        int w = (sig - 1) >> 5;
        t->sig->pending[w] &= ~(1u << ((sig - 1) & 31));
    }
}

int sig_pending_test(struct task *t, int sig) {
    if (sig < 1 || sig > 64 || !t->sig) return 0;
    int w = (sig - 1) >> 5;
    return (t->sig->pending[w] >> ((sig - 1) & 31)) & 1;
}

// rt_sigaction (174): sig, act, oact, sigsetsize
void sig_rt_sigaction(struct registers *r) {
    struct task *t = get_current_task();
    int sig = (int)r->ebx;
    unsigned int act = r->ecx;
    unsigned int oact = r->edx;

    if (sig < 1 || sig > 64) { r->eax = -22; return; }
    if (sig == SIGKILL || sig == SIGSTOP) { r->eax = -22; return; }

    struct aos_sigstate *st = sig_state(t);
    if (!st) { r->eax = -12; return; }   // -ENOMEM

    struct aos_ksig *ka = &st->sigact[sig];

    if (oact) {
        // musl reads sa_handler, sa_mask, sa_flags, sa_restorer
        if (!in_lin_win(oact, 140)) { r->eax = -14; return; }
        unsigned int *u = (unsigned int *)oact;
        u[0] = ka->handler;                                  // sa_handler
        int *m = (int *)(oact + 4);                            // sa_mask (128 B)
        for (int i = 0; i < 32; i++) m[i] = 0;
        m[0] = (int)ka->mask[0];
        m[1] = (int)ka->mask[1];
        u[33] = ka->flags;                                    // sa_flags
        u[34] = ka->restorer;                                 // sa_restorer
    }

    if (act) {
        if (!in_lin_win(act, 140)) { r->eax = -14; return; }
        unsigned int *u = (unsigned int *)act;
        ka->handler = u[0];
        int *m = (int *)(act + 4);
        ka->mask[0] = (unsigned int)m[0];
        ka->mask[1] = (unsigned int)m[1];
        ka->flags = u[33];
        ka->restorer = u[34];
    }

    r->eax = 0;
}

// rt_sigprocmask (175): how, set, oset, sigsetsize
void sig_rt_sigprocmask(struct registers *r) {
    struct task *t = get_current_task();
    int how = (int)r->ebx;
    unsigned int set = r->ecx;
    unsigned int oset = r->edx;

    struct aos_sigstate *st = sig_state(t);
    if (!st) { r->eax = -12; return; }

    if (oset) {
        if (!in_lin_win(oset, 128)) { r->eax = -14; return; }
        int *m = (int *)oset;
        for (int i = 0; i < 32; i++) m[i] = 0;
        m[0] = (int)st->block[0];
        m[1] = (int)st->block[1];
    }

    if (set) {
        if (!in_lin_win(set, 128)) { r->eax = -14; return; }
        int *m = (int *)set;
        unsigned int nw[2];
        nw[0] = (unsigned int)m[0];
        nw[1] = (unsigned int)m[1];
        // SIGKILL/SIGSTOP can never be blocked
        unsigned int unblockable = (1u << (SIGKILL - 1)) | (1u << (SIGSTOP - 1));
        if (SIGKILL <= 32 && SIGSTOP <= 32)
            nw[0] &= ~unblockable;

        if (how == SIG_BLOCK) {
            st->block[0] |= nw[0]; st->block[1] |= nw[1];
        } else if (how == SIG_UNBLOCK) {
            st->block[0] &= ~nw[0]; st->block[1] &= ~nw[1];
        } else {   // SIG_SETMASK
            st->block[0] = nw[0]; st->block[1] = nw[1];
        }
    }

    r->eax = 0;
}

// tkill (238) / tgkill (270): set pending on target
void sig_tgkill(struct registers *r) {
    // tgkill: tgid, tid, sig     tkill: tid, sig
    int tgid, tid, sig;
    if (r->eax == 270) {
        tgid = (int)r->ebx; tid = (int)r->ecx; sig = (int)r->edx;
    } else {
        tgid = 0; tid = (int)r->ebx; sig = (int)r->ecx;
    }
    (void)tgid;
    if (sig < 1 || sig > 64) { r->eax = -22; return; }

    struct task *me = get_current_task();
    if (tid == (int)task_current_pid()) {
        // deliver to self
        if (sig == SIGKILL) { task_exit_current(128 + SIGKILL); return; }
        if (sig_state(me) == 0) { r->eax = -12; return; }
        sig_pending_set(me, sig);
        r->eax = 0;
        return;
    }

    // cross-task kill: find the task by pid
    struct task *target = task_find_pid(tid);
    if (!target) { r->eax = -3; return; }   // -ESRCH
    if (sig == SIGKILL) {
        int rc = task_kill(tid);            // schedules exit(9) on next syscall
        r->eax = (rc == 0) ? 0 : -3;
        return;
    }
    sig_pending_set(target, sig);
    r->eax = 0;
}

// gettid (224): return own pid
void sig_gettid(struct registers *r) {
    r->eax = task_current_pid();
}

// Build an rt_sigframe on the user stack and redirect the frame to the handler.
// sig_rt_sigreturn is invoked by __restore_rt after the handler returns.
static void build_rt_frame(struct registers *r, int sig, struct aos_ksig *ka) {
    struct linux_ctx *lc = task_current_lctx();
    unsigned int frame_size = 1024;
    unsigned int sp = (r->user_esp - frame_size) & ~0xFu;
    unsigned int lo = lc->win_lo, hi = lc->win_hi;
    if (sp < lo || sp + frame_size > hi) return;   // can't build frame here

    unsigned int pinfo = sp + 16;
    unsigned int puc = sp + 144;

    unsigned int *f = (unsigned int *)sp;
    f[0] = ka->restorer ? ka->restorer : 0;  // pretcode
    f[1] = (unsigned int)sig;
    f[2] = pinfo;
    f[3] = puc;

    // siginfo at sp+16
    int *si = (int *)(sp + 16);
    for (int i = 0; i < 32; i++) si[i] = 0;
    si[0] = sig;

    // ucontext at sp+144
    unsigned char *uc = (unsigned char *)(sp + 144);
    for (int i = 0; i < 528; i++) uc[i] = 0;
    int *gregs = (int *)(uc + 20 + 0);
    gregs[RG_GS]  = 0;              // gs/fs/es/ds reloaded or unused
    gregs[RG_FS]  = 0;
    gregs[RG_ES]  = 0;
    gregs[RG_DS]  = 0;
    gregs[RG_EDI] = (int)r->edi;
    gregs[RG_ESI] = (int)r->esi;
    gregs[RG_EBP] = (int)r->ebp;
    gregs[RG_ESP] = 0;
    gregs[RG_EBX] = (int)r->ebx;
    gregs[RG_EDX] = (int)r->edx;
    gregs[RG_ECX] = (int)r->ecx;
    gregs[RG_EAX] = (int)r->eax;
    gregs[RG_TRAPNO] = 0;
    gregs[RG_ERR] = 0;
    gregs[RG_EIP]  = (int)r->eip;
    gregs[RG_CS]   = (int)r->cs;
    gregs[RG_EFL]  = (int)r->eflags;
    gregs[RG_UESP] = (int)r->user_esp;
    gregs[RG_SS]   = (int)r->ss;

    // uc_sigmask at uc+108
    struct task *t = get_current_task();
    int *mask = (int *)(uc + 108);
    for (int i = 0; i < 32; i++) mask[i] = 0;
    mask[0] = (int)t->sig->block[0];
    mask[1] = (int)t->sig->block[1];

    // redirect the frame: enter the handler at sp with args on the stack
    r->eip = ka->handler;
    r->user_esp = sp;
}

// Called at the end of every linux syscall. Delivers at most one pending,
// unblocked signal per return to user space.
void sig_check_deliver(struct registers *r) {
    struct task *t = get_current_task();
    if (!t || t->abi != ABI_LINUX) return;
    if (!t->sig) return;

    for (int sig = 1; sig <= 64; sig++) {
        int w = (sig - 1) >> 5;
        unsigned int bit = 1u << ((sig - 1) & 31);
        if ((t->sig->pending[w] & bit) == 0) continue;
        if (!t->sig->block[w] || !(t->sig->block[w] & bit)) {
            // take this signal
            t->sig->pending[w] &= ~bit;

            struct aos_ksig *ka = &t->sig->sigact[sig];

            if (sig == SIGKILL || sig == SIGSTOP) {
                // cannot be caught or blocked; always terminate
                task_exit_current(128 + sig);
                return;
            }

            if (ka->handler == SIG_IGN) {
                // dropped; keep scanning for other signals
                continue;
            }
            if (ka->handler == SIG_DFL) {
                if (sig_default_ignore(sig)) continue;   // ignored
                task_exit_current(128 + sig);            // default terminate
                return;
            }

            // user handler: build the frame and return to user space in it
            build_rt_frame(r, sig, ka);
            return;
        }
    }
}

// rt_sigreturn (173): restore context from the frame the handler was entered in
void sig_rt_sigreturn(struct registers *r) {
    // After the handler `ret`s, esp = frame+4 (it popped pretcode).
    unsigned int frame = r->user_esp - 4;
    struct linux_ctx *lc = task_current_lctx();
    if (frame < lc->win_lo || frame + 1024 > lc->win_hi) { r->eax = -14; return; }

    unsigned int *f = (unsigned int *)frame;
    unsigned int puc = f[3];

    if (puc < lc->win_lo || puc + 528 > lc->win_hi) { r->eax = -14; return; }

    unsigned char *uc = (unsigned char *)puc;
    int *gregs = (int *)(uc + 20);
    r->edi = (unsigned int)gregs[RG_EDI];
    r->esi = (unsigned int)gregs[RG_ESI];
    r->ebp = (unsigned int)gregs[RG_EBP];
    r->ebx = (unsigned int)gregs[RG_EBX];
    r->edx = (unsigned int)gregs[RG_EDX];
    r->ecx = (unsigned int)gregs[RG_ECX];
    r->eax = (unsigned int)gregs[RG_EAX];
    r->eip = (unsigned int)gregs[RG_EIP];
    r->cs  = (unsigned int)gregs[RG_CS];
    r->eflags = (unsigned int)gregs[RG_EFL];
    r->user_esp = (unsigned int)gregs[RG_UESP];
    r->ss  = (unsigned int)gregs[RG_SS];

    // restore the blocked mask
    int *mask = (int *)(uc + 108);
    struct task *t = get_current_task();
    if (t->sig) {
        t->sig->block[0] = (unsigned int)mask[0];
        t->sig->block[1] = (unsigned int)mask[1];
    }

    r->eax = 0;

    // deliver any still-pending signal before returning to the interrupted code
    sig_check_deliver(r);
}
