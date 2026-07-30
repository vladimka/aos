#ifndef INTERRUPTS_H
#define INTERRUPTS_H

struct registers {
    unsigned int gs, fs, es, ds;
    unsigned int edi, esi, ebp, esp;
    unsigned int ebx, edx, ecx, eax;
    unsigned int int_no, err_code;
    unsigned int eip, cs, eflags, user_esp, ss;
};

void interrupts_init(void);
void irq_install_handler(int irq, void (*handler)(void));
void syscall_handler(struct registers *r);

#endif
