# Dynamic Kernel Memory Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the static per-task arrays and the `MAX_TASKS = 6` cap with a buddy page allocator, a `kmalloc` kernel heap, and dynamically allocated task resources so the number of concurrent tasks is limited by RAM (~18 at 256 MB) instead of compile-time arrays.

**Architecture:** `kernel/pmm.c` implements a classic binary buddy over multiboot memory-map ranges, excluding reserved regions (low MB, kernel, ramdisk, task-0 user area, slab window, framebuffer). `kernel/kmm.c` provides a slab-lite `kmalloc` (power-of-two size classes, larger blocks from buddy) with per-frame metadata for `kfree`. `kernel/task.c` allocates each task's kstack, page tables, mailbox, and args at spawn and frees them at exit (the dead task's kstack is freed lazily via a zombie list because the exit path still runs on it).

**Tech Stack:** Freestanding C11 and x86 assembly, QEMU i386 (HMP monitor, serial log), Python 3 standard library.

## Global Constraints

- No libc or dynamic allocation in user programs; kernel-only `kmalloc`/`kfree`/`page_alloc`/`page_free`.
- Keep the task and syscall ABI unchanged: `task_mailbox_send`, `task_mailbox_recv`, `SYS_SEND`, `SYS_RECV`, `SYS_SPAWN`, message definitions, and mailbox return codes (`-1` invalid PID, `-2` free target, `-3` full mailbox) keep their signatures and values.
- `MSG_CAP` remains 128; `TASK_KSTACK_SIZE` remains 8192; the user area per task remains 12 MB (`0x01000000..0x01C00000`), fully pre-mapped.
- `task_exit_current()` only marks the task exited; `task_switch_kernel()` is the only cleanup and exit-notification publisher (existing behavior retained).
- All `pmm`/`kmm` functions are IRQ-safe (IF-preserving `cli`/`sti`), because task-exit cleanup runs inside the timer IRQ.
- `_end` symbol must exist in `linker.ld` before `pmm.c` compiles (it is declared `extern` in `pmm.c`).
- Build with `make`; there is no standalone unit-test framework. Boot-time self-tests print to serial via `printf`.
- Multiboot2 only (AGENTS.md): `__saved_magic == 0x36D76289`, booted via GRUB2.
- Do not revert unrelated user changes in `kernel/task.c`; preserve the existing exit-notification and mailbox logic.

---

### Task 1: Buddy Physical Page Allocator

**Files:**
- Modify: `linker.ld`
- Create: `kernel/pmm.h`
- Create: `kernel/pmm.c`
- Modify: `Makefile:12-19`
- Modify: `kernel/kernel.c:72-118`

**Interfaces:**
- Consumes: `__saved_mb_info` (globals in `kernel/kernel.c`), `vga_get_fb_info(unsigned int *addr, unsigned int *size)` (`drivers/vga.h`), `printf` (`kernel/printf.h`), `memset` (`kernel/string.h`).
- Produces: `pmm_init(unsigned int mb_info_addr)`, `page_alloc(void)`, `page_alloc_zero(void)`, `page_alloc_order(unsigned int order)`, `page_free(void *addr)`, `page_free_order(void *addr, unsigned int order)`, `pmm_total_pages(void)`, `pmm_free_pages(void)`, `struct pframe { unsigned char order, flags, slab_class; }`, `pmm_frames[]`, `pmm_frame_of(unsigned int phys)`, `PF_SLAB`. The `pframe.flags/slab_class` fields are consumed by Task 2; `page_alloc_zero`/`page_free` are consumed by Task 3.

- [ ] **Step 1: Add `_start`/`_end` symbols to the linker script**

Modify `linker.ld` so the kernel image bounds are visible to C code:

```ld
ENTRY(start)

SECTIONS {
    . = 1M;
    _start = .;

    .text : {
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    .bss : {
        *(COMMON)
        *(.bss)
    }

    . = ALIGN(4096);
    _end = .;
}
```

- [ ] **Step 2: Write `kernel/pmm.h`**

Create the public interface. The frame metadata is exported so `kernel/kmm.c` can mark slab pages:

```c
#ifndef PMM_H
#define PMM_H

#define PAGE_SIZE 4096

// Bit in struct pframe.flags: this frame backs a kmalloc slab page.
#define PF_SLAB 0x1

// Per-frame metadata (mem_map). order is the buddy order for an allocated
// large block (0 for slab pages); slab pages also record their size class.
struct pframe {
    unsigned char order;
    unsigned char flags;
    unsigned char slab_class;
} __attribute__((packed));

#define PM_NR_MAX 65536

extern struct pframe pmm_frames[PM_NR_MAX];

static inline unsigned int pmm_frame_of(unsigned int phys) {
    return phys >> 12;
}

void pmm_init(unsigned int mb_info_addr);
void pmm_selftest(void);

void *page_alloc(void);
void *page_alloc_zero(void);
void *page_alloc_order(unsigned int order);
void page_free(void *addr);
void page_free_order(void *addr, unsigned int order);

unsigned int pmm_total_pages(void);
unsigned int pmm_free_pages(void);

#endif
```

- [ ] **Step 3: Write `kernel/pmm.c`**

Create the complete buddy allocator. It parses the MB2 memory-map tag (type 6, entry type 1 = available), subtracts reserved regions, and inserts the remainder as aligned power-of-two blocks. It falls back to the MB2 basic-meminfo tag (type 4, `mem_upper` in KB above 1 MB) when no memory-map tag is present:

```c
#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "vga.h"

#define MAX_ORDER 16

struct pframe pmm_frames[PM_NR_MAX];

static void *free_lists[MAX_ORDER + 1];
static unsigned int total_pages = 0;
static unsigned int free_pages = 0;

struct region { unsigned int start; unsigned int end; };
static struct region reserved[16];
static unsigned int nreserved = 0;

static void reserve(unsigned int start, unsigned int end) {
    if (nreserved < 16 && end > start) {
        reserved[nreserved].start = start;
        reserved[nreserved].end = end;
        nreserved++;
    }
}

static int is_reserved(unsigned int addr, unsigned int len) {
    for (unsigned int i = 0; i < nreserved; i++)
        if (addr < reserved[i].end && addr + len > reserved[i].start)
            return 1;
    return 0;
}

// Largest order whose block fits inside [base,end) AND is aligned at base.
static unsigned int max_order_for(unsigned int base, unsigned int end) {
    unsigned int o = 0;
    while (o < MAX_ORDER) {
        unsigned int block = PAGE_SIZE << (o + 1);
        if ((base & (block - 1)) != 0) break;
        if (base + block > end) break;
        o++;
    }
    return o;
}

static void push_block(unsigned int base, unsigned int order) {
    *(unsigned int *)base = (unsigned int)free_lists[order];
    free_lists[order] = (void *)base;
    free_pages += 1u << order;
}

// Returns 0 when the free list is empty (physical address 0 is never handed
// out: the first MB is reserved).
static unsigned int pop_block(unsigned int order) {
    if (!free_lists[order]) return 0;
    unsigned int b = (unsigned int)free_lists[order];
    free_lists[order] = (void *)*(unsigned int *)b;
    free_pages -= 1u << order;
    return b;
}

static void buddy_free(unsigned int base, unsigned int order) {
    while (order < MAX_ORDER) {
        unsigned int buddy = base ^ (PAGE_SIZE << order);
        // Is the buddy free (i.e. present in this order's list)?
        unsigned int *prev = (unsigned int *)&free_lists[order];
        unsigned int *cur = free_lists[order];
        int found = 0;
        while (cur) {
            if ((unsigned int)cur == buddy) { found = 1; break; }
            prev = cur;
            cur = (unsigned int *)*cur;
        }
        if (!found) break;
        *prev = (unsigned int)*cur;   // unlink the buddy
        free_pages -= 1u << order;
        if (buddy < base) base = buddy;
        order++;
    }
    push_block(base, order);
}

// Decompose [base,end) into maximal aligned power-of-two blocks and free each.
static void buddy_free_range(unsigned int base, unsigned int end) {
    while (base < end) {
        unsigned int o = max_order_for(base, end);
        push_block(base, o);
        base += PAGE_SIZE << o;
    }
}

// Add an available physical range, cutting out any reserved regions.
static void add_available(unsigned int base, unsigned int end) {
    for (unsigned int i = 0; i < nreserved; i++) {
        if (reserved[i].start >= end || reserved[i].end <= base) continue;
        if (reserved[i].start > base)
            add_available(base, reserved[i].start);
        base = reserved[i].end;
        if (base >= end) return;
    }
    buddy_free_range(base, end);
}

void *page_alloc_order(unsigned int order) {
    if (order > MAX_ORDER) return 0;
    unsigned int b = pop_block(order);
    if (b) {
        pmm_frames[b >> 12].order = (unsigned char)order;
        return (void *)b;
    }
    unsigned int o = order + 1;
    while (o <= MAX_ORDER && !free_lists[o]) o++;
    if (o > MAX_ORDER) return 0;
    unsigned int big = pop_block(o);
    while (o > order) {
        o--;
        push_block(big + (PAGE_SIZE << o), o);
    }
    pmm_frames[big >> 12].order = (unsigned char)order;
    return (void *)big;
}

void *page_alloc(void) {
    return page_alloc_order(0);
}

void *page_alloc_zero(void) {
    void *p = page_alloc_order(0);
    if (p) memset(p, 0, PAGE_SIZE);
    return p;
}

void page_free_order(void *addr, unsigned int order) {
    unsigned int base = (unsigned int)addr;
    if (!base || order > MAX_ORDER) return;
    pmm_frames[base >> 12].order = 0;
    buddy_free(base, order);
}

void page_free(void *addr) {
    page_free_order(addr, 0);
}

unsigned int pmm_total_pages(void) { return total_pages; }
unsigned int pmm_free_pages(void) { return free_pages; }

static void parse_memmap(unsigned char *mbi, unsigned int *starts,
                         unsigned int *ends, int *navail) {
    unsigned int total = *(unsigned int *)mbi;
    unsigned char *tag = mbi + 8;
    while ((unsigned int)(tag - mbi) < total) {
        unsigned int type = *(unsigned int *)tag;
        unsigned int size = *(unsigned int *)(tag + 4);
        if (type == 0) break;
        if (type == 6) {
            unsigned int es = *(unsigned int *)(tag + 8);
            unsigned char *e = tag + 12;
            unsigned char *tend = tag + size;
            while (e + 24 <= tend) {
                unsigned long long base = *(unsigned long long *)e;
                unsigned long long len = *(unsigned long long *)(e + 8);
                unsigned int etype = *(unsigned int *)(e + 16);
                if (etype == 1 && base < 256u * 1024 * 1024) {
                    unsigned long long b = base;
                    unsigned long long eend = base + len;
                    if (eend > 256u * 1024 * 1024) eend = 256u * 1024 * 1024;
                    if (eend > b && *navail < 16) {
                        starts[*navail] = (unsigned int)b;
                        ends[*navail] = (unsigned int)eend;
                        (*navail)++;
                    }
                }
                e += es ? es : 24;
            }
            return;
        }
        tag += (size + 7) & ~7;
    }
}

// Fallback: MB2 basic-meminfo tag (type 4) — mem_upper in KB above 1 MB.
static int parse_mem_upper(unsigned char *mbi, unsigned int *start, unsigned int *end) {
    unsigned int total = *(unsigned int *)mbi;
    unsigned char *tag = mbi + 8;
    while ((unsigned int)(tag - mbi) < total) {
        unsigned int type = *(unsigned int *)tag;
        unsigned int size = *(unsigned int *)(tag + 4);
        if (type == 0) break;
        if (type == 4) {
            unsigned int upper = *(unsigned int *)(tag + 12);
            if (!upper) return 0;
            *start = 0x100000;
            *end = 0x100000 + upper * 1024u;
            if (*end > 256u * 1024 * 1024) *end = 256u * 1024 * 1024;
            return 1;
        }
        tag += (size + 7) & ~7;
    }
    return 0;
}

void pmm_init(unsigned int mb_info_addr) {
    memset(pmm_frames, 0, sizeof(pmm_frames));
    memset(free_lists, 0, sizeof(free_lists));
    nreserved = 0;

    extern unsigned int _start, _end;

    reserve(0x00000000, 0x00100000);                     // low MB (BIOS/multiboot)
    reserve(0x00100000, (unsigned int)&_end);            // kernel image
    reserve(0x00200000, 0x00200000 + 160 * 1024);        // ramdisk (kernel/sfs.c)
    reserve(0x01000000, 0x01C00000);                     // task-0 user area
    reserve(0x03000000, 0x04000000);                     // shared slab window
    unsigned int fb_addr = 0, fb_size = 0;
    vga_get_fb_info(&fb_addr, &fb_size);
    if (fb_size && fb_addr < 256u * 1024 * 1024) {
        unsigned int fbe = fb_addr + fb_size;
        if (fbe > 256u * 1024 * 1024) fbe = 256u * 1024 * 1024;
        reserve(fb_addr, fbe);
    }

    unsigned char *mbi = (unsigned char *)mb_info_addr;
    unsigned int starts[16], ends[16];
    int navail = 0;
    if (mbi) {
        parse_memmap(mbi, starts, ends, &navail);
        if (!navail) {
            unsigned int s = 0, e = 0;
            if (parse_mem_upper(mbi, &s, &e)) {
                starts[0] = s; ends[0] = e; navail = 1;
            }
        }
    }
    for (int i = 0; i < navail; i++)
        add_available(starts[i], ends[i]);

    total_pages = 0;
    for (unsigned int i = 0; i < navail; i++)
        total_pages += (ends[i] - starts[i]) >> 12;
}

void pmm_selftest(void) {
    unsigned int pages[256];
    int n = 0, bad = 0;
    for (n = 0; n < 256; n++) {
        void *p = page_alloc();
        if (!p) break;
        pages[n] = (unsigned int)p;
        if (is_reserved((unsigned int)p, PAGE_SIZE)) bad = 1;
        if (bad) break;
    }
    for (int i = 0; i < n; i++)
        page_free((void *)pages[i]);
    printf("PMM: %u total, %u free pages, selftest %s\n",
           pmm_total_pages(), pmm_free_pages(), bad ? "FAIL" : "OK");
}
```

- [ ] **Step 4: Wire `pmm.o` into the build**

In `Makefile`, add `kernel/pmm.o` to `KERNEL_OBJS` after `kernel/paging.o`:

```make
KERNEL_OBJS = boot/boot.o boot/isr.o kernel/kernel.o drivers/vga.o \
              drivers/serial.o drivers/mouse.o drivers/pci.o drivers/uhci.o \
              kernel/terminal.o kernel/commands.o \
              kernel/sfs.o kernel/string.o arch/i386/gdt.o arch/i386/idt.o \
              kernel/interrupts.o kernel/elf.o kernel/syscall.o \
              kernel/progload.o kernel/paging.o kernel/pmm.o kernel/user.o \
              kernel/user_tramp.o kernel/printf.o kernel/progs.o \
              kernel/task.o
```

- [ ] **Step 5: Initialize pmm from `kernel_main`**

In `kernel/kernel.c`, add `#include "pmm.h"` and call `pmm_init` and the self-test right after `idt_init()` (before `paging_init()`):

```c
    idt_init();
    printf("IDT initialized.\n");

    pmm_init(__saved_mb_info);
    pmm_selftest();

    paging_init();
```

(`kmm_init`/`kmm_selftest` are added in Task 2 Step 4, right after `pmm_selftest()`.)

- [ ] **Step 6: Build and boot-check the buddy allocator**

Run:

```bash
make
timeout 25 qemu-system-i386 -cdrom aos.iso -display none -serial file:/tmp/aos-boot.log &
sleep 9
pkill -f qemu-system-i386 || true
grep "PMM:" /tmp/aos-boot.log
```

Expected: a line like `PMM: 32256 total, 30000 free pages, selftest OK` (numbers depend on RAM; default QEMU is 128 MB so expect ~32256 total pages) and **no** `PMM: ... FAIL`. The kernel continues to the normal boot banner and the WM spawn message.

- [ ] **Step 7: Commit**

```bash
git add linker.ld kernel/pmm.h kernel/pmm.c Makefile kernel/kernel.c
git commit -m "feat: add buddy physical page allocator with reserved-region handling"
```

### Task 2: Kernel Heap Allocator (`kmalloc`)

**Files:**
- Create: `kernel/kmm.h`
- Create: `kernel/kmm.c`
- Modify: `Makefile:12-19`
- Modify: `kernel/kernel.c:85-91` (add the missing `kmm_init`/`kmm_selftest` calls from Task 1 Step 5)

**Interfaces:**
- Consumes: `page_alloc_zero`, `page_alloc_order`, `page_free_order`, `pmm_frames`, `pmm_frame_of`, `PF_SLAB` from `kernel/pmm.h`.
- Produces: `kmm_init(void)`, `kmalloc(unsigned int size)`, `kcalloc(unsigned int n, unsigned int sz)`, `kfree(void *ptr)`, `kmm_selftest(void)`. Task 3 consumes `kmalloc`/`kfree`.

- [ ] **Step 1: Write `kernel/kmm.h`**

```c
#ifndef KMM_H
#define KMM_H

void kmm_init(void);
void kmm_selftest(void);

void *kmalloc(unsigned int size);
void *kcalloc(unsigned int n, unsigned int sz);
void kfree(void *ptr);

#endif
```

- [ ] **Step 2: Write `kernel/kmm.c`**

Slab-lite allocator: size classes 16..4096 backed by buddy pages (class stored in the slab page's `pframe`), larger allocations as contiguous buddy blocks (order recorded in the block's first `pframe`):

```c
#include "kmm.h"
#include "pmm.h"
#include "string.h"

#define KMM_CLASSES 9

static const unsigned int class_size[KMM_CLASSES] =
    { 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 };

static void *slab_free[KMM_CLASSES];

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

static int class_index(unsigned int size) {
    for (int i = 0; i < KMM_CLASSES; i++)
        if (size <= class_size[i]) return i;
    return -1;
}

// Split a fresh page into same-size objects linked through their first word.
static void *slab_carve(unsigned int base, unsigned int stride) {
    void *head = 0;
    for (unsigned int off = 0; off + stride <= PAGE_SIZE; off += stride) {
        unsigned int obj = base + off;
        *(unsigned int *)obj = (unsigned int)head;
        head = (void *)obj;
    }
    return head;
}

void kmm_init(void) {
    memset(slab_free, 0, sizeof(slab_free));
}

void *kmalloc(unsigned int size) {
    unsigned int flags;
    irq_save(&flags);
    void *p = 0;
    int idx = class_index(size ? size : 16);
    if (idx >= 0) {
        if (!slab_free[idx]) {
            void *page = page_alloc_zero();
            if (!page) goto out;
            unsigned int fr = pmm_frame_of((unsigned int)page);
            pmm_frames[fr].flags |= PF_SLAB;
            pmm_frames[fr].slab_class = (unsigned char)idx;
            slab_free[idx] = slab_carve((unsigned int)page, class_size[idx]);
        }
        p = slab_free[idx];
        slab_free[idx] = (void *)*(unsigned int *)p;
    } else {
        unsigned int need = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        unsigned int order = 0;
        while ((1u << order) < need) order++;
        p = page_alloc_order(order);
    }
out:
    irq_restore(flags);
    return p;
}

void *kcalloc(unsigned int n, unsigned int sz) {
    unsigned int total = n * sz;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void kfree(void *ptr) {
    if (!ptr) return;
    unsigned int flags;
    irq_save(&flags);
    unsigned int fr = pmm_frame_of((unsigned int)ptr);
    if (pmm_frames[fr].flags & PF_SLAB) {
        int idx = pmm_frames[fr].slab_class;
        *(unsigned int *)ptr = (unsigned int)slab_free[idx];
        slab_free[idx] = ptr;
    } else {
        page_free_order(ptr, pmm_frames[fr].order);
    }
    irq_restore(flags);
}

void kmm_selftest(void) {
    static const unsigned int sizes[] = { 10, 64, 100, 1000, 4096, 8192, 20000, 65536 };
    void *p[8];
    int ok = 1;
    int i;
    for (i = 0; i < 8; i++) {
        p[i] = kmalloc(sizes[i]);
        if (!p[i]) { ok = 0; break; }
        memset(p[i], 0xA5, sizes[i]);
        unsigned char *b = p[i];
        for (unsigned int j = 0; j < sizes[i]; j++)
            if (b[j] != 0xA5) { ok = 0; break; }
        if (!ok) break;
    }
    if (ok)
        for (int j = 0; j < i; j++) kfree(p[j]);
    void *q = ok ? kmalloc(64) : 0;
    if (!q) ok = 0;
    if (ok) kfree(q);
    printf("KMM: selftest %s\n", ok ? "OK" : "FAIL");
}
```

- [ ] **Step 3: Add `kernel/kmm.o` to the build**

In `Makefile`, add `kernel/kmm.o` to `KERNEL_OBJS` next to `kernel/pmm.o`:

```make
              kernel/progload.o kernel/paging.o kernel/pmm.o kernel/kmm.o \
              kernel/user.o \
```

- [ ] **Step 4: Ensure `kernel_main` calls both initializers**

In `kernel/kernel.c`, extend the Task 1 call site so it now reads (add `#include "kmm.h"` alongside `#include "pmm.h"`):

```c
    pmm_init(__saved_mb_info);
    kmm_init();
    pmm_selftest();
    kmm_selftest();
```

- [ ] **Step 5: Build and boot-check the heap allocator**

Run:

```bash
make
timeout 25 qemu-system-i386 -cdrom aos.iso -display none -serial file:/tmp/aos-boot.log &
sleep 9
pkill -f qemu-system-i386 || true
grep "KMM:" /tmp/aos-boot.log
```

Expected: `KMM: selftest OK` and still `PMM: ... selftest OK`, no `FAIL`, no `KERNEL PANIC`.

- [ ] **Step 6: Commit**

```bash
git add kernel/kmm.h kernel/kmm.c Makefile kernel/kernel.c
git commit -m "feat: add kmalloc kernel heap allocator"
```

### Task 3: Dynamic Task Resources

**Files:**
- Modify: `kernel/task.h`
- Modify: `kernel/task.c` (rewrite: remove static per-task arrays, dynamic allocation, zombie kstack drain)

**Interfaces:**
- Consumes: `kmalloc`/`kfree` (`kernel/kmm.h`), `page_alloc_zero`/`page_free` (`kernel/pmm.h`), `user_kstack_top()` (`kernel/user.h`).
- Produces: `MAX_TASKS = 24`; `struct task` with `pd`, `pts[3]`, `mbox`, `mbox_head/tail`, `args`; unchanged `task_spawn`/`task_switch_kernel`/mailbox signatures.

- [ ] **Step 1: Update `kernel/task.h`**

```c
#ifndef TASK_H
#define TASK_H

#define MAX_TASKS 24

#define TASK_FREE   0
#define TASK_READY  1
#define TASK_RUNNING 2

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
    unsigned int *mbox;         // mailbox ring buffer (kmalloc'd)
    unsigned int mbox_head;
    unsigned int mbox_tail;
    char *args;                 // argument buffer (kmalloc'd)
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

#endif
```

- [ ] **Step 2: Rewrite `kernel/task.c`**

Replace the whole file (the task struct layout, spawn, switch, and mailbox paths all change together):

```c
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

#define PTE_PRESENT  0x1
#define PTE_WRITABLE 0x2
#define PTE_USER     0x4

#define USER_PD_LO 4
#define USER_PD_HI 6

#define TASK_USTACK_TOP  0x01804000
#define TASK_KSTACK_SIZE 8192
#define TASK_USER_MB     12u

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
    unsigned int *mbox = kmalloc(MSG_CAP * 5 * 4);
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
    __asm__ volatile("cli");
    paging_set_cr3(t->cr3);
    entry = elf_load(path);
    paging_set_cr3((unsigned int)kpd);
    __asm__ volatile("sti");

    if (!entry) {
        task_free_addrspace(t);
        kfree(t->kstack);
        kfree(t->mbox);
        kfree(t->args);
        t->kstack = 0; t->mbox = 0; t->args = 0;
        t->state = TASK_FREE;
        return -2;
    }

    unsigned int ai = 0;
    if (args)
        while (args[ai] && ai < 255) { t->args[ai] = args[ai]; ai++; }
    t->args[ai] = '\0';

    // Synthetic interrupt frame (matches isr_common layout) so the restore
    // path iret's straight into ring 3.
    unsigned int *w = (unsigned int *)(t->kstack + TASK_KSTACK_SIZE - FRAME_WORDS * 4);
    w[0] = 0x23; w[1] = 0x23; w[2] = 0x23; w[3] = 0x23;   // gs fs es ds
    for (int i = 4; i < 12; i++) w[i] = 0;                // edi..eax
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
```

- [ ] **Step 3: Build**

Run: `make`

Expected: clean build; `aos.iso` produced. No warnings from `kernel/task.c`.

- [ ] **Step 4: Boot-check: WM still spawns and runs**

Run:

```bash
timeout 25 qemu-system-i386 -cdrom aos.iso -display none -serial file:/tmp/aos-boot.log &
sleep 9
pkill -f qemu-system-i386 || true
grep -E "Window manager spawned|PMM:|KMM:" /tmp/aos-boot.log
```

Expected: `Window manager spawned (pid 1).`, `PMM: ... selftest OK`, `KMM: ... OK`, no `KERNEL PANIC`. (Task 3 itself does not stress >6 tasks; Task 4's `many` does.)

- [ ] **Step 5: Commit**

```bash
git add kernel/task.h kernel/task.c
git commit -m "feat: dynamic task resources, raise MAX_TASKS to 24"
```

### Task 4: `many` Stress Program and 256 MB RAM

**Files:**
- Create: `programs/many.c`
- Modify: `Makefile:21` (PROGRAMS)
- Modify: `Makefile:73-74` (`run` target)
- Modify: `scripts/ipctest.py:85-88`
- Modify: `scripts/notepadtest.py:166-169`

**Interfaces:**
- Consumes: `spawn`, `getpid`, `recv_msg`, `get_tick`, `yield`, `panic`, `print`, `MSG_EXIT`, `struct aos_msg` from `programs/libaos.h`/`aosipc.h`.
- Produces: `bin/many`, a program that spawns 10 children per round for 4 rounds and prints `MANY PASS\n` or panics with `MANY FAIL`; verifies the 24-task slot space and that resources are reclaimed between rounds.

- [ ] **Step 1: Write `programs/many.c`**

```c
#include "libaos.h"

#define CHILDREN 10
#define ROUNDS   4

static void fail(void) {
    print("MANY FAIL\n");
    panic();
}

void main(void) {
    print("MANY: start\n");
    for (int r = 0; r < ROUNDS; r++) {
        int pids[CHILDREN];
        for (int i = 0; i < CHILDREN; i++) {
            int pid = spawn("bin/echo", "m", getpid());
            if (pid < 0) fail();
            pids[i] = pid;
        }
        unsigned int start = get_tick();
        int got = 0;
        while (got < CHILDREN && (int)(get_tick() - start) < 2000) {
            struct aos_msg m;
            if (recv_msg(&m) == 0 && m.type == MSG_EXIT) {
                int known = 0;
                for (int i = 0; i < CHILDREN; i++)
                    if (pids[i] == (int)m.a) known = 1;
                if (!known) fail();
                got++;
            }
            yield();
        }
        if (got != CHILDREN) fail();
    }
    print("MANY PASS\n");
}
```

- [ ] **Step 2: Add `many` to the embedded programs**

In `Makefile`, append `many` to `PROGRAMS`:

```make
PROGRAMS = help uptime clear echo tick info reboot panic ls cat rm format shutdown test wm term clock ipctest notepad many
```

- [ ] **Step 3: Raise QEMU RAM to 256 MB in the run target and harnesses**

In `Makefile` `run` target:

```make
run: aos.iso
	qemu-system-i386 -m 256 -display gtk,grab-on-hover=on -cdrom $<
```

In `scripts/ipctest.py`, insert `"-m", "256",` before `"-display", "none"`:

```python
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-serial", "file:" + SER, "-monitor", "unix:" + MON + ",server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
```

Apply the identical change to `scripts/notepadtest.py`.

- [ ] **Step 4: Build and run `many` interactively**

Run:

```bash
make
```

Then boot with a GUI and run `many` in a terminal, or use the existing dock-click automation once (see Task 5). For a quick manual check, boot `make run` and run `many` in the WM terminal: the terminal should print `MANY PASS`. With the old `MAX_TASKS = 6` this panics with `MANY FAIL` on round 1 spawn #7.

- [ ] **Step 5: Commit**

```bash
git add programs/many.c Makefile scripts/ipctest.py scripts/notepadtest.py kernel/progs.c
git commit -m "feat: many-task stress test; raise QEMU RAM to 256 MB"
```

### Task 5: Automated `many` Regression Harness

**Files:**
- Create: `scripts/manytest.py`

**Interfaces:**
- Consumes: `aos.iso` with `bin/many`, QEMU HMP monitor, serial log.
- Produces: `python3 scripts/manytest.py`, exits 0 only when serial contains `MANY PASS`, no `MANY FAIL`, no `KERNEL PANIC`.

- [ ] **Step 1: Write `scripts/manytest.py`**

```python
#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-many.sock"
SER = "/tmp/aos-many.log"

def wait_for(path, seconds=10):
    end = time.time() + seconds
    while time.time() < end:
        if os.path.exists(path): return
        time.sleep(0.05)
    raise RuntimeError("timeout waiting for " + path)

def hmp(command):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(3)
        s.connect(MON)
        data = b""
        while b"(qemu)" not in data:
            data += s.recv(4096)
        s.sendall(command.encode() + b"\n")
        data = b""
        while b"(qemu)" not in data:
            data += s.recv(4096)
        return data.decode(errors="replace")

def send_text(text):
    keys = {"\n": "ret", " ": "spc"}
    for ch in text:
        key = keys.get(ch, ch)
        hmp("sendkey " + key)
        time.sleep(0.04)

def serial_text():
    try:
        with open(SER, "r", errors="replace") as f: return f.read()
    except FileNotFoundError:
        return ""

def main():
    for path in (MON, SER):
        try: os.unlink(path)
        except FileNotFoundError: pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-serial", "file:" + SER, "-monitor", "unix:" + MON + ",server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(MON)
        time.sleep(5)
        # Dock launcher spawns a terminal (same coordinates as ipctest.py).
        hmp("mouse_move -39 341")
        hmp("mouse_button 1")
        hmp("mouse_button 0")
        time.sleep(1)
        send_text("many\n")
        end = time.time() + 25
        log = ""
        while time.time() < end:
            time.sleep(1)
            log = serial_text()
            if "MANY PASS" in log: break
            if "MANY FAIL" in log or "KERNEL PANIC" in log: break
        if "MANY FAIL" in log:
            raise AssertionError("many failed to spawn/collect tasks")
        if "KERNEL PANIC" in log:
            raise AssertionError("many triggered a kernel panic")
        if "MANY PASS" not in log:
            raise AssertionError("MANY PASS not reached within timeout")
        print("PASS: many-task stress (10 concurrent x 4 rounds)")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Run the automated regression**

Run: `make && python3 scripts/manytest.py`

Expected: `PASS: many-task stress (10 concurrent x 4 rounds)`, exit 0.

- [ ] **Step 3: Run the full regression suite**

Run:

```bash
python3 scripts/ipctest.py
python3 scripts/manytest.py
python3 scripts/notepadtest.py
```

Expected: all three print their PASS lines (existing `ipctest`/`notepadtest` behaviors unchanged, now with `-m 256`). No `KERNEL PANIC` in any serial log.

- [ ] **Step 4: Commit**

```bash
git add scripts/manytest.py
git commit -m "test: automate many-task stress regression"
```

### Task 6: Documentation Updates

**Files:**
- Modify: `AGENTS.md`
- Modify: `TODO.md`

- [ ] **Step 1: Update AGENTS.md facts and memory model**

1. In **Build & run**, change `make run` line to note `-m 256`:

```markdown
make run       # qemu-system-i386 -m 256 -cdrom aos.iso
```

2. In **Architecture**, fix the ramdisk size and syscall count:

```markdown
- **160 KB ramdisk at `0x200000`** — flat SFS (Simple File System), `SFS_MAX_FILES=64`
- **30 syscalls via `int 0x80`** (DPL 3 gate, `idt_install_irq_flags(0x80, isr128, 0xEE)`), R/O user-level interface in `programs/libaos.c`
```

3. In the **Memory model (after `paging_init`)** section, replace the bullets about fixed task resources with the dynamic allocator description:

```markdown
- **Physical memory**: `kernel/pmm.c` — binary buddy page allocator over the multiboot
  memory map (reserved: low MB, kernel `[_start,_end)`, ramdisk, task-0 user area,
  slab window, framebuffer). `page_alloc`/`page_free` are IRQ-safe; per-frame
  metadata in `pmm_frames[]` (`order`/`flags`/`slab_class`, see `kernel/pmm.h`)
- **Kernel heap**: `kernel/kmm.c` — slab-lite `kmalloc`/`kfree` (size classes
  16..4096 backed by buddy pages; larger blocks as contiguous buddy blocks).
  Class/order recovered from `pmm_frames[]` on free
- **Tasks**: up to `MAX_TASKS=24` (RAM-bound: ~18 GUI tasks of 12 MB at 256 MB).
  Each task's kstack/PD/3 PTs/mailbox/args are `kmalloc`/`page_alloc`'d at spawn
  and freed at exit; the dying task's kstack is freed lazily via a zombie list
  because `switch_and_restore` restores `%esp` only after `task_switch_kernel`
  returns (running on the dead stack)
```

4. Keep the rest of the memory model bullets unchanged.

- [ ] **Step 2: Update TODO.md**

1. In the reference-facts paragraph (line 9), change:

```markdown
планировщик round-robin на `MAX_TASKS=6` с фиксированными 3 каталогами страниц на задачу (`kernel/task.c`)
```

to:

```markdown
планировщик round-robin на `MAX_TASKS=24`; buddy-аллокатор страниц (`kernel/pmm.c`) и kmalloc (`kernel/kmm.c`), ресурсы задач выделяются динамически (`kernel/task.c`)
```

2. Mark item 1.1 "Память" done for the allocator and task-limit items:

```markdown
- [x] **P0 — Ядренный кучной аллокатор (`kmalloc`/`kfree`).** ... (allocator готов: buddy + size-class kmalloc; временные буферы `user_str[1024]` и SFS пока статические — вынести на динамику отдельно)
- [x] **P0 — Убрать ограничение `MAX_TASKS=6`.** ... (динамические ресурсы задач, `MAX_TASKS=24`)
```

3. Add a new open item right after them to track the remaining scratch-buffer migration:

```markdown
- [ ] **P0 — Перевести временные буферы ядра на `kmalloc`.** `user_str[1024]`/`user_str2[256]`/`prog_args[256]` в `kernel/syscall.c` и SFS-буферы — сейчас статические; выделять через `kmalloc` (мелкие, живучие, нет лимита в 1024 байта).
```

- [ ] **Step 3: Build once more and run the suite**

Run: `make && python3 scripts/manytest.py && python3 scripts/ipctest.py && python3 scripts/notepadtest.py`

Expected: clean build; all three harnesses pass.

- [ ] **Step 4: Commit**

```bash
git add AGENTS.md TODO.md
git commit -m "docs: document dynamic memory subsystem and 256 MB RAM"
```

## Self-Review

- **Spec coverage:**
  - pmm buddy + reserved regions + memmap parsing + `_end` symbol → Task 1.
  - kmalloc size classes + large blocks + per-frame class/order recovery + IRQ safety → Task 2.
  - dynamic task resources + `MAX_TASKS=24` + exit cleanup with deferred kstack (zombie list) → Task 3.
  - `-m 256` in Makefile and both harnesses → Task 4.
  - `many` test (>6 tasks, reclamation across rounds) → Tasks 4–5; regression (ipctest, notepadtest) → Task 5.
  - AGENTS.md corrections (160 KB ramdisk, syscall count, memory model) and TODO.md checkboxes → Task 6.
  - Out-of-scope items (demand paging, COW, mmap, slab-page return, blocking IPC, ext2, Linux-compat) are intentionally not implemented.
- **Placeholder scan:** every task has full code or exact edits, concrete commands, and expected results. No "TBD"/"similar to Task N".
- **Type consistency:** `page_alloc_zero`/`page_free` return/take `void*`; `kmalloc`/`kfree` take/return `void*`; `pmm_frame_of`/`pmm_frames`/`PF_SLAB` are shared exactly as declared in `pmm.h`; `struct task` fields used by `task.c` match the new `task.h`; `spawn()` returns `int` pid or negative error; `MSG_EXIT.a` is the child pid. `kmm_init`/`kmm_selftest` are declared in `kmm.h` and added to `kernel_main` in Task 2 Step 4, after Task 1's `pmm_init`/`pmm_selftest`.
- **Edge cases handled in code:** buddy split/coalesce via alignment + first-word free-list links; `page_alloc` returns 0 on exhaustion; spawn OOM frees partial resources and returns `-1`; zombie drain runs on live context in both `task_switch_kernel` and `task_spawn`; task 0 kstack is static and never freed; mailbox derefs target `t->mbox` only after the `TASK_FREE` check.
