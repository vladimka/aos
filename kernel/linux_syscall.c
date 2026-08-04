#include "interrupts.h"
#include "linux_syscall.h"
#include "string.h"

void linux_ctx_init(struct linux_ctx *lc) {
    memset(lc, 0, sizeof(struct linux_ctx));
    for (int i = 0; i < LINUX_FDS; i++)
        lc->fds[i] = i <= 2 ? i : -1;   // 0,1,2 = std; >=3 free
}

void linux_syscall_handler(struct registers *r) {
    (void)r;
}