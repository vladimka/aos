#ifndef TASK_H
#define TASK_H

#define MAX_TASKS 24

#define TASK_FREE   0
#define TASK_READY  1
#define TASK_RUNNING 2

enum task_abi { ABI_AOS = 0, ABI_LINUX = 1 };

struct task {
    unsigned int pid;
    unsigned int state;
    unsigned int kernel_esp;
    unsigned int cr3;
    unsigned char *kstack;
    unsigned int kstack_top;
    unsigned int sink;
    unsigned int *pd;           // task's own page directory page
    unsigned int *pts[3];       // the 3 user-area page table pages
    unsigned int *lpts[32];     // Linux window (PD 32..63) page-table pages
    unsigned int (*mbox)[5];    // mailbox ring buffer (kmalloc'd): MSG_CAP x 5 words
    unsigned int mbox_head;
    unsigned int mbox_tail;
    char *args;                 // argument buffer (kmalloc'd)
    unsigned int abi;           // ABI_AOS or ABI_LINUX
    struct linux_ctx *lctx;     // Linux runtime context (kmalloc'd)
};

void task_init(void);
unsigned int task_switch_kernel(unsigned int cur_esp);
int task_spawn(const char *path, const char *args, unsigned int sink, unsigned int *out_pid);
void task_exit_current(void);

unsigned int task_current_pid(void);
unsigned int task_current_sink(void);
int task_set_sink(unsigned int pid);
int task_alive(unsigned int pid);
const char *task_current_args(void);

int task_mailbox_send(unsigned int pid, unsigned int t, unsigned int a, unsigned int b, unsigned int c, unsigned int d);
int task_mailbox_recv(unsigned int *t, unsigned int *a, unsigned int *b, unsigned int *c, unsigned int *d);

int task_event_pid(void);
int task_set_event_pid(void);

unsigned int task_current_abi(void);
int task_set_abi_current(unsigned int abi);
struct linux_ctx *task_current_lctx(void);

#endif
