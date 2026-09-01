#ifndef AOS_SIGNAL_H
#define AOS_SIGNAL_H

#include "task.h"

// Linux signal ABI constants
#define SIG_DFL 0
#define SIG_IGN 1

#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

#define SIGKILL 9
#define SIGSTOP 19
#define SIGCHLD 17

// Kernel copy of a registered handler (subset of struct sigaction)
struct aos_ksig {
    unsigned int handler;    // SIG_DFL (0), SIG_IGN (1), or user fn address
    unsigned int flags;      // SA_* flags (SA_RESTORER bit is 0x04000000)
    unsigned int restorer;   // __restore_rt address (SA_RESTORER)
    unsigned int mask[2];    // signals blocked while the handler runs
};

// Per-task signal state (indexed by signal number 0..64; 0 unused)
struct aos_sigstate {
    struct aos_ksig sigact[65];
    unsigned int pending[2]; // pending signal bits (word0=sig 1..31, word1=32..63)
    unsigned int block[2];   // rt_sigprocmask blocked set
};

void sig_reset(struct task *t);               // clear handlers/pending/block (exec)
void sig_check_deliver(struct registers *r);  // called at end of each syscall

void sig_pending_set(struct task *t, int sig);
void sig_pending_clear(struct task *t, int sig);
int  sig_pending_test(struct task *t, int sig);

// linux_syscall dispatch targets
void sig_rt_sigaction(struct registers *r);     // syscall 174
void sig_rt_sigprocmask(struct registers *r);   // syscall 175
void sig_rt_sigreturn(struct registers *r);     // syscall 173
void sig_tgkill(struct registers *r);           // syscall 270 / tkill 238
void sig_gettid(struct registers *r);           // syscall 224

#endif
