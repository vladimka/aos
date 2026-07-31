#include "task.h"
#include "gdt.h"
#include "paging.h"
#include "elf.h"
#include "fs.h"
#include "string.h"
#include "serial.h"
#include "user.h"
#include "string.h"

#define PTE_PRESENT  0x1
#define PTE_WRITABLE 0x2
#define PTE_USER     0x4

#define USER_PD_LO 4
#define USER_PD_HI 6

#define TASK_USTACK_TOP  0x01804000
#define TASK_KSTACK_SIZE 8192
#define TASK_USER_MB     12u
#define TASK_FRAME_START 0x04000000

#define FRAME_WORDS 19

#define MSG_CAP 128
#define MSG_TYPE_DATA 2
#define MSG_TYPE_EXIT 6

static struct task tasks[MAX_TASKS];
static unsigned char kstacks[MAX_TASKS][TASK_KSTACK_SIZE] __attribute__((aligned(16)));
static unsigned int task_pds[MAX_TASKS][1024] __attribute__((aligned(4096)));
static unsigned int task_pts[MAX_TASKS][3][1024] __attribute__((aligned(4096)));

static unsigned int mbox[MAX_TASKS][MSG_CAP][5];
static unsigned int mbox_head[MAX_TASKS];
static unsigned int mbox_tail[MAX_TASKS];

static char task_args[MAX_TASKS][256];

static struct task *current_task;
static int event_pid = 0;
static int current_exited = 0;

// ---- IF-preserving cli/sti (mailbox ops run both in IRQ and syscall ctx) ----
static void irq_save(unsigned int *flags) {
    unsigned int f;
    __asm__ volatile("pushfl; pop %0" : "=r"(f));
    __asm__ volatile("cli");
    *flags = f;
}

static void irq_restore(unsigned int flags) {
    if (flags & 0x200)
        __asm__ volatile("sti");
}

void task_init(void) {
    memset(tasks, 0, sizeof(tasks));
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_FREE;
        tasks[i].pid = i;
        mbox_head[i] = 0;
        mbox_tail[i] = 0;
    }
    // Task 0 is the kernel idle context (the main shell loop). It also hosts
    // single-user programs (user_program_start): its ring3->ring0 transitions
    // land on the shared sys_stack, so esp0 must point there.
    tasks[0].state = TASK_RUNNING;
    tasks[0].cr3 = (unsigned int)paging_kernel_pd();
    tasks[0].kstack_top = user_kstack_top();
    current_task = &tasks[0];
    tss_set_esp0(tasks[0].kstack_top);
}

unsigned int task_switch_kernel(unsigned int cur_esp) {
    current_task->kernel_esp = cur_esp;
    current_task->state = TASK_READY;

    if (current_exited) {
        current_exited = 0;
        struct task *dead = current_task;
        unsigned int sink = dead->sink;
        dead->state = TASK_FREE;
        if (sink < MAX_TASKS && sink != dead->pid && tasks[sink].state != TASK_FREE)
            task_mailbox_send(sink, MSG_TYPE_EXIT, dead->pid, 0, 0, 0);
    }

    struct task *next = 0;
    for (int i = 1; i <= MAX_TASKS; i++) {
        struct task *t = &tasks[(current_task->pid + i) % MAX_TASKS];
        if (t->state == TASK_READY) { next = t; break; }
    }
    if (!next) next = current_task;

    if (next != current_task) {
        next->state = TASK_RUNNING;
        current_task = next;
        tss_set_esp0(next->kstack_top);
        if (next->cr3 != paging_get_cr3())
            paging_set_cr3(next->cr3);
    } else {
        current_task->state = TASK_RUNNING;
    }
    return next->kernel_esp;
}

int task_spawn(const char *path, const char *args, unsigned int sink, unsigned int *out_pid) {
    int pid = -1;
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_FREE) { pid = i; break; }
    if (pid < 0) return -1;

    struct task *t = &tasks[pid];
    memset(t, 0, sizeof(*t));
    t->pid = pid;
    t->kstack = kstacks[pid];
    t->kstack_top = (unsigned int)(t->kstack + TASK_KSTACK_SIZE);
    t->sink = sink;

    // Per-task address space: clone the kernel PD, point PD4..6 at this
    // task's own physical frames (program at 0x01000000, heap, stack top).
    unsigned int *kpd = paging_kernel_pd();
    for (int i = 0; i < 1024; i++)
        task_pds[pid][i] = kpd[i];

    unsigned int fb = TASK_FRAME_START + (pid - 1) * TASK_USER_MB * 1024 * 1024;
    for (int pd = USER_PD_LO; pd <= USER_PD_HI; pd++) {
        unsigned int *pt = task_pts[pid][pd - USER_PD_LO];
        unsigned int base = fb + (pd - USER_PD_LO) * 4 * 1024 * 1024;
        for (int p = 0; p < 1024; p++)
            pt[p] = (base + p * 4096) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        task_pds[pid][pd] = (unsigned int)pt | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }
    t->cr3 = (unsigned int)task_pds[pid];

    // Load the ELF into the task's address space. The temporary CR3 switch
    // must not be interrupted: an IRQ mid-switch would leave the interrupted
    // kernel code with the wrong page tables after rescheduling.
    void *entry;
    __asm__ volatile("cli");
    paging_set_cr3(t->cr3);
    entry = elf_load(path);
    paging_set_cr3((unsigned int)kpd);
    __asm__ volatile("sti");

    if (!entry) {
        t->state = TASK_FREE;
        return -2;
    }

    unsigned int ai = 0;
    if (args)
        while (args[ai] && ai < 255) { task_args[pid][ai] = args[ai]; ai++; }
    task_args[pid][ai] = '\0';

    // Synthetic interrupt frame (matches the real CPU+isr_common layout:
    // gs,fs,es,ds,edi..eax,int_no,err_code,eip,cs,eflags,user_esp,ss) so the
    // restore path iret's straight into ring 3.
    unsigned int *w = (unsigned int *)(t->kstack + TASK_KSTACK_SIZE - FRAME_WORDS * 4);
    w[0] = 0x23; w[1] = 0x23; w[2] = 0x23; w[3] = 0x23;   // gs fs es ds
    for (int i = 4; i < 12; i++) w[i] = 0;                  // edi..eax
    w[12] = 0;              // int_no
    w[13] = 0;              // err_code
    w[14] = (unsigned int)entry;
    w[15] = 0x1B;           // user cs
    w[16] = 0x202;          // eflags (IF set)
    w[17] = TASK_USTACK_TOP;
    w[18] = 0x23;           // user ss
    t->kernel_esp = (unsigned int)w;

    t->state = TASK_READY;
    if (out_pid) *out_pid = (unsigned int)pid;
    return 0;
}

void task_exit_current(void) {
    struct task *t = current_task;
    if (t->sink < MAX_TASKS && t->sink != t->pid && tasks[t->sink].state != TASK_FREE)
        task_mailbox_send(t->sink, MSG_TYPE_EXIT, t->pid, 0, 0, 0);
    current_exited = 1;
}

unsigned int task_current_pid(void) {
    return current_task->pid;
}

unsigned int task_current_sink(void) {
    return current_task->sink;
}

int task_set_sink(unsigned int pid) {
    if (pid >= MAX_TASKS) return -1;
    if (pid != 0 && tasks[pid].state == TASK_FREE) return -1;
    current_task->sink = pid;
    return 0;
}

int task_alive(unsigned int pid) {
    return pid < MAX_TASKS && tasks[pid].state != TASK_FREE;
}

const char *task_current_args(void) {
    return task_args[current_task->pid];
}

int task_mailbox_send(unsigned int pid, unsigned int t, unsigned int a, unsigned int b, unsigned int c, unsigned int d) {
    if (pid >= MAX_TASKS) return -1;
    if (tasks[pid].state == TASK_FREE) return -2;
    unsigned int next = (mbox_tail[pid] + 1) % MSG_CAP;
    if (next == mbox_head[pid]) return -3;
    unsigned int flags;
    irq_save(&flags);
    mbox[pid][mbox_tail[pid]][0] = t;
    mbox[pid][mbox_tail[pid]][1] = a;
    mbox[pid][mbox_tail[pid]][2] = b;
    mbox[pid][mbox_tail[pid]][3] = c;
    mbox[pid][mbox_tail[pid]][4] = d;
    mbox_tail[pid] = next;
    irq_restore(flags);
    return 0;
}

int task_mailbox_recv(unsigned int *t, unsigned int *a, unsigned int *b, unsigned int *c, unsigned int *d) {
    unsigned int flags;
    irq_save(&flags);
    unsigned int pid = current_task->pid;
    int rc = -1;
    if (mbox_head[pid] != mbox_tail[pid]) {
        unsigned int i = mbox_head[pid];
        if (t) *t = mbox[pid][i][0];
        if (a) *a = mbox[pid][i][1];
        if (b) *b = mbox[pid][i][2];
        if (c) *c = mbox[pid][i][3];
        if (d) *d = mbox[pid][i][4];
        mbox_head[pid] = (i + 1) % MSG_CAP;
        rc = 0;
    }
    irq_restore(flags);
    return rc;
}

int task_event_pid(void) {
    return event_pid;
}

int task_set_event_pid(void) {
    int old = event_pid;
    event_pid = (int)current_task->pid;
    return old;
}
