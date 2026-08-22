#ifndef TASK_H
#define TASK_H

#include "commands.h"   // PATH_MAX

struct open_file;   // forward decl (fully defined in vfs.h)

#define MAX_TASKS 24
#define TASK_MAX_FDS 64   // per-task open-file table size

#define TASK_FREE    0
#define TASK_READY   1
#define TASK_RUNNING 2
#define TASK_SLEEPING 3   // blocked until tick >= wake_tick
#define TASK_WAITING  4   // blocked until child (wait_pid) exits
#define TASK_ZOMBIE   5   // exited, exit_code retained, awaiting waitpid
#define TASK_SPAWNING 6   // task_spawn in progress; slot held, not schedulable

enum task_abi { ABI_AOS = 0, ABI_LINUX = 1 };

struct task {
    unsigned int pid;
    unsigned int state;
    unsigned int kernel_esp;
    unsigned int cr3;
    unsigned char *kstack;
    unsigned int kstack_top;
    unsigned int sink;
    unsigned int parent;      // pid that spawned this task (0 = kernel)
    unsigned int wake_tick;   // TASK_SLEEPING: wake when tick >= wake_tick
    unsigned int wait_pid;    // TASK_WAITING: child pid being waited on
    unsigned int exit_code;   // TASK_ZOMBIE: exit code to hand to waitpid
    unsigned int kill_pending; // set by task_kill: exit(9) on next syscall
    unsigned int *pd;           // task's own page directory page
    unsigned int *pts[3];       // the 3 user-area page table pages
    unsigned int *lpts[32];     // Linux window (PD 32..63) page-table pages
    unsigned int (*mbox)[5];    // mailbox ring buffer (kmalloc'd): MSG_CAP x 5 words
    unsigned int mbox_head;
    unsigned int mbox_tail;
    char *args;                 // argument buffer (kmalloc'd)
    char name[28];              // program basename from the spawn path
    unsigned int abi;           // ABI_AOS or ABI_LINUX
    struct linux_ctx *lctx;     // Linux runtime context (kmalloc'd)
    struct open_file *fds[TASK_MAX_FDS];  // per-task open-file table (0/1/2 console)
    char cwd[PATH_MAX];         // normalized absolute cwd ("/" = root)
    int stdout_fd;
    int stdin_fd;
    unsigned int trace_on;        // 1 = record this task's syscalls
    unsigned char *trace_buf;     // kmalloc'd ring of struct trace_rec (lazy)
    unsigned int trace_head;      // next write slot (wraps)
    unsigned int trace_count;     // records written so far
    unsigned int trace_wrapped;   // 1 = the ring overwrote old records
    unsigned int trace_dumped;    // 1 = already printed by a strace session
};

void task_init(void);
unsigned int task_switch_kernel(unsigned int cur_esp);
int task_spawn(const char *path, const char *args, unsigned int sink, unsigned int *out_pid, const char *env);
void task_exit_current(unsigned int code);
void task_sleep(unsigned int ms);
int  task_waitpid(unsigned int pid);
int  task_get_children(unsigned int *buf, unsigned int max);
void task_set_init_pid(unsigned int pid);
unsigned int task_init_pid(void);
int task_kill(unsigned int pid);

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

// Console pseudo-file shared by all tasks' fds 0/1/2 (inode NULL, O_RDWR).
extern struct open_file console_open_file;

// The currently-running task.
struct task *get_current_task(void);

// Task-table slot accessor (task_slot(i) == 0 for i >= MAX_TASKS).
struct task *task_slot(unsigned int i);

// Panic-diagnostic accessors.
unsigned int task_kernel_esp(unsigned int pid);
unsigned int task_kstack_top(unsigned int pid);
unsigned int task_state(unsigned int pid);
void task_set_kernel_esp0(unsigned int esp);

#endif
