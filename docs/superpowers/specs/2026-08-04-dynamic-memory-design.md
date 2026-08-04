# Design: dynamic kernel memory subsystem (buddy + kmalloc)

Date: 2026-08-04
Status: approved for specification review

## Goal

Replace the fixed per-task static buffers (`kstacks`, `task_pds`, `task_pts`,
`mbox`, `task_args`) and the hard `MAX_TASKS = 6` cap with a real dynamic memory
subsystem: a physical page allocator (buddy) and a kernel heap allocator
(`kmalloc`). Task resources are allocated at spawn and freed at exit, so the
number of simultaneous tasks becomes limited by RAM (~18 GUI tasks at 256 MB)
instead of compile-time arrays. This is the foundation for future sub-projects
(demand paging, COW, `mmap`) and the longer-term Linux-ELF compatibility
roadmap.

## Scope

Delivers the memory subsystem only. It does not add demand paging, COW, `mmap`,
slab-page reclamation, or new public syscalls. Framebuffer and slab-window
mapping behavior is unchanged. Task-0 user area remains statically mapped.

## Physical page allocator (`kernel/pmm.c/.h`)

Classic binary buddy over 4 KB frames:

- **Sources of RAM**: multiboot memory-map tag (MB2 type 6, entry type 1 =
  available). Fallback: MB2 basic-meminfo tag (type 4) `mem_upper`. `phys_top`
  is the maximum available range end, capped at 256 MB (the identity-map limit
  of `paging.c`). Available ranges that cannot be represented as one aligned
  buddy block are decomposed into aligned power-of-two blocks.
- **Buddy state**: one free-list per order 0..16 (256 MB = 2^16 pages). A free
  block stores its next free-list link in its own first 4 bytes; buddy index is
  `page_index ^ (1 << order)`.
- **Per-frame metadata**: `struct pframe { u8 order; u8 flags; u8 slab_class; }`
  indexed by physical page number (`PM_NR_MAX = 65536`, ~192 KB BSS). Needed so
  `kfree` can recover the size class or buddy order of an allocated block and to
  enable later sub-projects (demand paging, mmap).
- **Reserved regions** are subtracted from available ranges before buddy
  insertion:
  1. `[0x00000000, 0x00100000)` — first MB (BIOS, multiboot structures)
  2. `[0x00100000, _end)` — kernel image (requires `_end` symbol in `linker.ld`)
  3. `[0x00200000, 0x00200000 + 160 KB)` — ramdisk (SFS, `kernel/sfs.c`)
  4. `[0x01000000, 0x01C00000)` — task-0 static user area (identity-mapped)
  5. `[0x03000000, 0x04000000)` — shared slab window
  6. framebuffer `[fb_addr, fb_addr + fb_size)` when it falls below 256 MB

API:

```c
void pmm_init(unsigned int mb_info_addr);
void *page_alloc(void);                 // order 0, returns phys addr (identity-mapped)
void *page_alloc_zero(void);
void *page_alloc_order(unsigned int order);
void page_free(void *addr);
void page_free_order(void *addr, unsigned int order);
unsigned int pmm_total_pages(void);
```

All functions are IF-preserving IRQ-safe (`cli`/`sti`), because task exit cleanup
(`kfree`, `page_free`) runs inside the timer IRQ.

## Kernel heap allocator (`kernel/kmm.c/.h`)

Slab-lite size-class allocator backed by buddy pages:

- **Size classes**: 16, 32, 64, 128, 256, 512, 1024, 2048, 4096. Each class has
  its own free-list. A miss allocates one buddy page, carves it into objects of
  the class size, and links them into the class free-list. Objects are carved
  directly at offsets `0, class, 2*class, ...` so data is naturally aligned to
  the class size (>= 16 bytes) and no per-object header is needed.
- **Class recovery for `kfree`**: the slab page's frame keeps its size class in
  `pframe.slab_class` and a `PF_SLAB` flag bit; the free-list link of an object
  is stored in the object's own first word.
- **Large allocations** (> 4096): one contiguous buddy block of
  `order = ceil(log2(size / 4096))`; the order is recorded in `pframe.order` of
  the block's first frame; `kfree` recovers it via the frame index. No header
  inside the block, so no adjacent-free-page clobbering.
- All operations under an IF-preserving critical section.

API:

```c
void kmm_init(void);
void *kmalloc(unsigned int size);
void *kcalloc(unsigned int n, unsigned int sz);
void kfree(void *ptr);
```

## Dynamic task resources (`kernel/task.c/.h`)

`struct task` gains resource pointers; the static per-task arrays are removed:

```c
struct task {
    unsigned int pid;
    unsigned int state;
    unsigned int kernel_esp;
    unsigned int cr3;
    unsigned char *kstack;      // kmalloc(8192)
    unsigned int kstack_top;
    unsigned int sink;
    unsigned int *pd;           // page_alloc_zero() page directory
    unsigned int *pts[3];       // 3 page_alloc_zero() user PTs
    unsigned int *mbox;         // kmalloc(MSG_CAP*5*4) mailbox ring
    unsigned int mbox_head, mbox_tail;
    char *args;                 // kmalloc(256) arg buffer
};
```

- `MAX_TASKS` goes 6 → 24 (the `struct task` array is ~1.5 KB; the real cap is
  RAM: ~225 MB free / 12 MB pre-mapped per task ≈ 18 simultaneous tasks).
- `task_spawn` allocates kstack, PD, 3 PTs, 3072 user-area frames
  (one `page_alloc` each), mbox, and args. On any allocation failure it frees
  everything already allocated and returns `-1` (out of memory) instead of
  silently exceeding a cap.
- `task_switch_kernel` exit path order matters:
  1. mark dead `TASK_FREE`, send exit notifications (unchanged);
  2. pick next, switch CR3 (must happen **before** freeing the dead PD);
  3. free dead PD + 3 PTs (`page_free` x4), mbox + args (`kfree`);
  4. **defer** freeing dead `kstack`: `switch_and_restore` (`boot/isr.S`) restores
     the next stack only after `task_switch_kernel` returns, so the function
     still runs on the dead task's stack. Push the kstack onto a small zombie
     list; drain the list at the top of the next `task_switch_kernel` call, which
     runs on a live task's stack.
- `task_mailbox_send/recv` and `task_current_args` dereference
  `tasks[pid].mbox` / `tasks[pid].args` instead of the old static arrays
  (guard with the existing IF-preserving critical section).
- `task_spawn`'s temporary CR3 switch around `elf_load` stays as-is.

## Infrastructure

- `linker.ld`: add `_end = .;` (after `.bss`, `ALIGN(4096)`).
- `kernel_main`: call `pmm_init(__saved_mb_info)` and `kmm_init()` after
  `vga_init` (so framebuffer info is available for reservation) and before
  `paging_init`.
- QEMU: raise RAM to **256 MB** (`-m 256`) in the Makefile `run` target and in
  the test harnesses (`ipctest.py`, `notepadtest.py`, `guitester.py`).
- AGENTS.md: correct the stale "64 KB ramdisk" to 160 KB, document the new
  memory subsystem and task limit.

## Test Strategy

- New `programs/many.c`: spawns 10 children (more than the old `MAX_TASKS`) that
  each exit immediately; parent collects `MSG_TYPE_EXIT` from every child; repeat
  the spawn/wait cycle several times to prove no resource leak (a leak makes a
  later round fail to spawn 10). Runs both from the shell (task 0) and as a GUI
  task where feasible.
- Regression: existing `ipctest` task/IPC stability scenario, `notepadtest.py`
  (WM + notepad E2E), `guitester.py` all still pass.
- Baseline build check is `make`; `make clean` first is part of the plan.

## Out of Scope

- Demand paging, COW, `mmap` (future sub-projects; `pframe.flags` is the hook)
- Slab-page return to buddy (kept for reuse; avoids thrash)
- Blocking mailbox receive, process wait syscalls, priorities, `cpu_time` stats
- ext2 / FAT32 (separately planned), Linux-ELF syscall-compat (separate roadmap)

## Acceptance Criteria

1. `make` (from clean) completes; kernel builds with `-nostdlib -ffreestanding`.
2. Buddy never hands out a reserved region (kernel, ramdisk, task-0 user area,
   slab window, first MB, framebuffer).
3. More than 6 concurrent tasks run simultaneously; `many` spawns 10 and
   reclaims all resources across repeated spawn/exit rounds.
4. Exit cleanup cannot corrupt the running stack: zombie-kstack drain frees dead
   stacks from a live context, and no crash occurs on task exit.
5. `ipctest`, `notepadtest.py`, `guitester.py` pass unmodified (only QEMU
   `-m 256` added).
