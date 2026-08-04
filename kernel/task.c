#include "task.h"
#include "gdt.h"
#include "paging.h"
#include "elf.h"
#include "fs.h"
#include "string.h"
#include "serial.h"
#include "user.h"
#include "kmm.h"
#include "pmm.h"
#include "linux_syscall.h"

#define PTE_PRESENT  0x1
#define PTE_WRITABLE 0x2
#define PTE_USER     0x4

#define USER_PD_LO 4
#define USER_PD_HI 6

#define TASK_USTACK_TOP  0x01804000
#define TASK_KSTACK_SIZE 8192
#define TASK_USER_MB     12u

#define STACK_MARGIN 0x10000   // Linux stack/mmap margin (must match loader)

#define FRAME_WORDS 19

#define MSG_CAP 128
#define MSG_TYPE_DATA 2
#define MSG_TYPE_EXIT 6

static struct task tasks[MAX_TASKS];

// Kernel stacks of exited tasks, freed only from a live task's context (the
// exit path still runs on the dying task's own stack until switch_and_restore
// does "mov %eax, %esp" after task_switch_kernel returns).
static struct task *zombies[MAX_TASKS];
static int nzombies = 0;

static struct task *current_task;
static int event_pid = 0;
static int current_exited = 0;

// ---- IF-preserving cli/sti (mailbox + kmalloc ops run in IRQ and syscall ctx) ----
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

static void drain_zombies(void) {
    while (nzombies > 0) {
        struct task *z = zombies[--nzombies];
        if (z->kstack) {
            kfree(z->kstack);
            z->kstack = 0;
            z->kstack_top = 0;
        }
    }
}

// Free a task's user frames, its 3 user PTs, and its PD page.
static void task_free_addrspace(struct task *t) {
    for (int pdn = USER_PD_LO; pdn <= USER_PD_HI; pdn++) {
        unsigned int *pt = t->pts[pdn - USER_PD_LO];
        for (int p = 0; p < 1024; p++)
            if (pt[p] & PTE_PRESENT)
                page_free((void *)(pt[p] & 0xFFFFF000));
        page_free(pt);
        t->pts[pdn - USER_PD_LO] = 0;
    }
    if (t->abi == ABI_LINUX) {
        for (int i = 0; i < 32; i++) {
            unsigned int *pt = t->lpts[i];
            if (!pt) continue;
            for (int p = 0; p < 1024; p++)
                if (pt[p] & PTE_PRESENT)
                    page_free((void *)(pt[p] & 0xFFFFF000));
            page_free(pt);
            t->lpts[i] = 0;
        }
    }
    page_free(t->pd);
    t->pd = 0;
    t->cr3 = 0;
}

void task_init(void) {
    memset(tasks, 0, sizeof(tasks));
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_FREE;
        tasks[i].pid = i;
    }
    // Task 0 is the kernel idle context (the main shell loop). It also hosts
    // single-user programs (user_program_start): its ring3->ring0 transitions
    // land on the shared static sys_stack, so esp0 points there and kstack is
    // not owned/freed by task.c. It needs a mailbox to receive exit notices.
    tasks[0].state = TASK_RUNNING;
    tasks[0].cr3 = (unsigned int)paging_kernel_pd();
    tasks[0].kstack_top = user_kstack_top();
    tasks[0].mbox = kmalloc(MSG_CAP * 5 * 4);
    tasks[0].mbox_head = 0;
    tasks[0].mbox_tail = 0;
    tasks[0].abi = ABI_AOS;
    tasks[0].lctx = kmalloc(sizeof(struct linux_ctx));
    linux_ctx_init(tasks[0].lctx);
    current_task = &tasks[0];
    tss_set_esp0(tasks[0].kstack_top);
}

unsigned int task_switch_kernel(unsigned int cur_esp) {
    drain_zombies();
    int exited = current_exited;
    current_task->kernel_esp = cur_esp;
    current_task->state = TASK_READY;

    struct task *dead = 0;
    if (exited) {
        current_exited = 0;
        dead = current_task;
        unsigned int sink = dead->sink;
        unsigned int ep = (unsigned int)event_pid;
        dead->state = TASK_FREE;
        // Exit notifications are best-effort: a full recipient mailbox
        // (task_mailbox_send returns -3) silently drops the message.
        if (sink < MAX_TASKS && sink != dead->pid && tasks[sink].state != TASK_FREE)
            task_mailbox_send(sink, MSG_TYPE_EXIT, dead->pid, 0, 0, 0);
        if (ep > 0 && ep < MAX_TASKS && ep != dead->pid && ep != sink &&
            tasks[ep].state != TASK_FREE)
            task_mailbox_send(ep, MSG_TYPE_EXIT, dead->pid, 0, 0, 0);
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
        // A Linux task that installed TLS must see the descriptor again when
        // it resumes: %gs is reloaded from the GDT TLS slot (selector 0x33).
        if (next->abi == ABI_LINUX && next->lctx && next->lctx->tls_seg32) {
            ldt_set_tls(next->lctx->tls_base, next->lctx->tls_limit,
                        next->lctx->tls_seg32, next->lctx->tls_ro,
                        next->lctx->tls_gran_pages);
            tls_reload_gs();
        }
    } else {
        current_task->state = TASK_RUNNING;
    }

    // The dead task can no longer run and its address space is no longer
    // active (CR3 switched above). Free everything except its kstack, which we
    // are still executing on: defer it to the zombie list.
    if (exited) {
        task_free_addrspace(dead);
        kfree(dead->mbox);
        kfree(dead->args);
        kfree(dead->lctx);
        dead->lctx = 0;
        dead->mbox = 0;
        dead->args = 0;
        if (nzombies < MAX_TASKS)
            zombies[nzombies++] = dead;
    }

    return next->kernel_esp;
}

int task_spawn(const char *path, const char *args, unsigned int sink, unsigned int *out_pid) {
    drain_zombies();

    int pid = -1;
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_FREE) { pid = i; break; }
    if (pid < 0) return -1;

    struct task *t = &tasks[pid];
    memset(t, 0, sizeof(*t));
    t->pid = pid;
    t->sink = sink;

    unsigned char *ks = kmalloc(TASK_KSTACK_SIZE);
    unsigned int *pd = page_alloc_zero();
    unsigned int *pts[3];
    for (int i = 0; i < 3; i++) pts[i] = page_alloc_zero();
    unsigned int (*mbox)[5] = (unsigned int (*)[5])kmalloc(MSG_CAP * 5 * 4);
    char *argsb = kmalloc(256);
    if (!ks || !pd || !pts[0] || !pts[1] || !pts[2] || !mbox || !argsb) {
        if (ks) kfree(ks);
        if (pd) page_free(pd);
        for (int i = 0; i < 3; i++) if (pts[i]) page_free(pts[i]);
        if (mbox) kfree(mbox);
        if (argsb) kfree(argsb);
        return -1;
    }
    t->kstack = ks;
    t->kstack_top = (unsigned int)(ks + TASK_KSTACK_SIZE);
    t->pd = pd;
    t->pts[0] = pts[0];
    t->pts[1] = pts[1];
    t->pts[2] = pts[2];
    t->mbox = mbox;
    t->mbox_head = 0;
    t->mbox_tail = 0;
    t->args = argsb;

    t->abi = ABI_AOS;
    t->lctx = kmalloc(sizeof(struct linux_ctx));
    if (!t->lctx) {
        kfree(ks); page_free(pd); kfree(mbox); kfree(argsb);
        for (int i = 0; i < 3; i++) page_free(pts[i]);
        t->state = TASK_FREE;
        return -1;
    }
    linux_ctx_init(t->lctx);

    unsigned int *kpd = paging_kernel_pd();
    for (int i = 0; i < 1024; i++)
        pd[i] = kpd[i];

    for (int pdn = USER_PD_LO; pdn <= USER_PD_HI; pdn++) {
        unsigned int *pt = pts[pdn - USER_PD_LO];
        for (int p = 0; p < 1024; p++) {
            unsigned int frame = (unsigned int)page_alloc_zero();
            if (!frame) {
                task_free_addrspace(t);
                kfree(t->kstack);
                kfree(t->mbox);
                kfree(t->args);
                kfree(t->lctx); t->lctx = 0;
                t->kstack = 0; t->mbox = 0; t->args = 0;
                t->state = TASK_FREE;
                return -1;
            }
            pt[p] = frame | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }
        pd[pdn] = (unsigned int)pt | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }
    t->cr3 = (unsigned int)pd;

    // Load the ELF into the task's address space. The temporary CR3 switch
    // must not be interrupted: an IRQ mid-switch would leave the interrupted
    // kernel code with the wrong page tables after rescheduling.
    void *entry;
    int abi;
    int probed = elf_probe(path, &abi);
    t->abi = (probed == 0 && abi == ABI_LINUX) ? ABI_LINUX : ABI_AOS;

    if (t->abi == ABI_LINUX) {
        struct linux_ctx *lc = t->lctx;
        linux_ctx_init(lc);
        lc->win_lo = 0x08000000;
        lc->win_hi = 0x10000000;
        lc->stack_top = 0x10000000;
        lc->mmap_cur = 0x10000000 - STACK_MARGIN;
        for (int pdn = 32; pdn <= 63; pdn++) {
            unsigned int *pt = page_alloc_zero();
            if (!pt) {
                task_free_addrspace(t);
                kfree(t->kstack); kfree(t->mbox); kfree(t->args); kfree(t->lctx);
                t->kstack = 0; t->mbox = 0; t->args = 0; t->lctx = 0;
                t->state = TASK_FREE;
                return -1;
            }
            t->lpts[pdn - 32] = pt;
            pd[pdn] = (unsigned int)pt | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }
    }

    __asm__ volatile("cli");
    paging_set_cr3(t->cr3);
    entry = (t->abi == ABI_LINUX)
                ? elf_load_linux(path, args, t->lctx)
                : elf_load(path);
    paging_set_cr3((unsigned int)kpd);
    __asm__ volatile("sti");

    if (!entry) {
        task_free_addrspace(t);
        kfree(t->kstack);
        kfree(t->mbox);
        kfree(t->args);
        kfree(t->lctx); t->lctx = 0;
        t->kstack = 0; t->mbox = 0; t->args = 0;
        t->state = TASK_FREE;
        return -2;
    }

    unsigned int ai = 0;
    if (args)
        while (args[ai] && ai < 255) { t->args[ai] = args[ai]; ai++; }
    t->args[ai] = '\0';

    // Synthetic interrupt frame (matches the real CPU+isr_common layout:
    // gs,fs,es,ds,edi..eax,int_no,err_code,eip,cs,eflags,user_esp,ss) so the
    // restore path iret's straight into ring 3.
    unsigned int *w = (unsigned int *)(t->kstack + TASK_KSTACK_SIZE - FRAME_WORDS * 4);
    w[0] = 0x23; w[1] = 0x23; w[2] = 0x23; w[3] = 0x23;   // gs fs es ds
    for (int i = 4; i < 12; i++) w[i] = 0;                // edi..eax
    w[12] = 0;              // int_no
    w[13] = 0;              // err_code
    w[14] = (unsigned int)entry;
    w[15] = 0x1B;           // user cs
    w[16] = 0x202;          // eflags (IF set)
    w[17] = (t->abi == ABI_LINUX) ? t->lctx->stack_sp : TASK_USTACK_TOP;
    w[18] = 0x23;           // user ss
    t->kernel_esp = (unsigned int)w;

    t->state = TASK_READY;
    if (out_pid) *out_pid = (unsigned int)pid;
    return 0;
}

void task_exit_current(void) {
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

// syscall.c only calls this for pid > 0; task 0 uses its own prog_args buffer.
const char *task_current_args(void) {
    return current_task->args;
}

int task_mailbox_send(unsigned int pid, unsigned int t, unsigned int a,
                      unsigned int b, unsigned int c, unsigned int d) {
    unsigned int flags;
    irq_save(&flags);
    if (pid >= MAX_TASKS) {
        irq_restore(flags);
        return -1;
    }
    struct task *target = &tasks[pid];
    if (target->state == TASK_FREE) {
        irq_restore(flags);
        return -2;
    }
    unsigned int next = (target->mbox_tail + 1) % MSG_CAP;
    if (next == target->mbox_head) {
        irq_restore(flags);
        return -3;
    }
    target->mbox[target->mbox_tail][0] = t;
    target->mbox[target->mbox_tail][1] = a;
    target->mbox[target->mbox_tail][2] = b;
    target->mbox[target->mbox_tail][3] = c;
    target->mbox[target->mbox_tail][4] = d;
    target->mbox_tail = next;
    irq_restore(flags);
    return 0;
}

int task_mailbox_recv(unsigned int *t, unsigned int *a, unsigned int *b, unsigned int *c, unsigned int *d) {
    unsigned int flags;
    irq_save(&flags);
    struct task *self = current_task;
    int rc = -1;
    if (self->mbox_head != self->mbox_tail) {
        unsigned int i = self->mbox_head;
        if (t) *t = self->mbox[i][0];
        if (a) *a = self->mbox[i][1];
        if (b) *b = self->mbox[i][2];
        if (c) *c = self->mbox[i][3];
        if (d) *d = self->mbox[i][4];
        self->mbox_head = (i + 1) % MSG_CAP;
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

unsigned int task_current_abi(void) {
    return current_task->abi;
}

int task_set_abi_current(unsigned int abi) {
    current_task->abi = abi;
    return 0;
}

struct linux_ctx *task_current_lctx(void) {
    return current_task->lctx;
}
