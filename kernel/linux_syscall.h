#ifndef LINUX_SYSCALL_H
#define LINUX_SYSCALL_H

struct linux_ctx {
    unsigned int brk_base;           // initial program break (end of ELF BSS)
    unsigned int brk_cur;            // current brk
    unsigned int mmap_cur;           // top-down anonymous mmap cursor
    unsigned int stack_top;          // top of user stack (exclusive)
    unsigned int stack_sp;           // initial ESP built by the loader
    unsigned int win_lo;             // user window lower bound (validation)
    unsigned int win_hi;             // user window upper bound (exclusive)
    unsigned int tls_base;           // installed TLS segment base (0 = none)
    unsigned int tls_limit;
    unsigned int tls_seg32;
    unsigned int tls_ro;
    unsigned int tls_gran_pages;
};

struct registers;

void linux_ctx_init(struct linux_ctx *lc);
void linux_syscall_handler(struct registers *r);

#endif
