# Linux ELF Execution (Step 1: static i686-musl utilities) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run statically-linked i686-musl ELF utilities (`lin/hello`, `lin/ls`, `lin/cat`) in AOS from the kernel shell (task 0) and via `SYS_SPAWN` (pid > 0), using a dedicated Linux syscall dispatcher, a real LDT for musl TLS, and a dynamic address-space window at `0x08000000..0x10000000`.

**Architecture:** ABI detection (ELF entry >= `0x01100000` or `ET_DYN` ⇒ Linux) splits AOS vs Linux ELFs at load time. Linux tasks get a private window `0x08000000..0x10000000` (task 0: a reserved 8 MB identity window `0x08000000..0x08800000` with `PTE_USER`). A new `kernel/linux_syscall.c` dispatches Linux syscalls (numbers from the i686 Linux ABI) to SFS-backed file operations and stubbed time/process calls, returning `-errno`. musl TLS is satisfied with a real per-CPU LDT (`set_thread_area`/`modify_ldt` install an entry; `%gs` loads from it). The ramdisk grows 160 KB → 1 MB to hold the extra binaries.

**Tech Stack:** i686 musl-cross toolchain (`tools/musl-i686/bin/i686-linux-musl-gcc`), QEMU E2E test scripts (manytest.py pattern), SFS ramdisk, i386 kernel.

## Global Constraints

- All kernel C code: `-ffreestanding -nostdlib -fno-builtin -fno-stack-protector -mno-sse -mno-mmx -mno-80387 -m32 -std=c11`. Use `__asm__ __volatile__`, never plain `asm`.
- Language of all user-visible strings/comments in this project: Russian (kernel messages, docs). Code identifiers stay English.
- User binaries built with `tools/musl-i686/bin/i686-linux-musl-gcc -static -no-pie -Os`; **no** `-static-pie`.
- Linux window for spawned tasks: `0x08000000..0x10000000` (PD 32..63). Task-0 window: `0x08000000..0x08800000` (PD 32..33), reserved in pmm, `PTE_USER` in the kernel identity map.
- Ramdisk `FS_SIZE` = `1024 * 1024` at `0x200000` (kernel `_end` ≈ 0x1D7000, slab window at 0x03000000, FB_STAGE at 0x00C00000 — no collisions).
- No ASLR: ET_EXEC loads at its link address (`0x08048000`); `AT_BASE` = 0.
- `kernel/progs.c` is generated; never edit by hand. `tools/musl-i686/` (105 MB) and `build/` must stay out of git.
- PATH stays `"bin"`; Linux utilities are launched by explicit path `lin/hello`.
- Linux syscalls return `-errno` (negative). `-5` is reserved for AOS pointer-validation failures; Linux path uses `-EINVAL`/`-EFAULT` equivalents.
- Sync rule: `task_switch_kernel` re-installs the current task's LDT entry before resuming a `ABI_LINUX` task; EOI is already sent before handlers in `interrupts.c`.

---

### Task 1: Grow the ramdisk to 1 MB and make `format` restore embedded data

**Files:**
- Modify: `kernel/sfs.c:5`
- Modify: `kernel/pmm.c:215`
- Modify: `kernel/commands.c:13-21`

**Interfaces:**
- Consumes: existing `fs_format()`, `load_embedded_programs()`, `load_embedded_data()`.
- Produces: `FS_SIZE == 1 MiB`; `cmd_format()` restores embedded data files (including the future `lin/*`) after `format`.

- [ ] **Step 1: Grow the SFS**

Edit `kernel/sfs.c:5`:

```c
#define FS_SIZE  (1024 * 1024)
```

Edit `kernel/pmm.c:215` to match:

```c
reserve(0x00200000, 0x00200000 + 1024 * 1024);        // ramdisk (kernel/sfs.c)
```

- [ ] **Step 2: Make `format` reload embedded data**

Edit `kernel/commands.c` `cmd_format()` (lines 13-21) to add the `load_embedded_data()` call after `load_embedded_programs()`:

```c
static void cmd_format(void) {
    terminal_print("\nFormatting filesystem...");
    fs_format();

    extern void load_embedded_programs(void);
    load_embedded_programs();
    extern void load_embedded_data(void);
    load_embedded_data();

    terminal_print(" done");
}
```

- [ ] **Step 3: Build and boot-check**

Run: `make`
Expected: builds clean (no errors), `aos.iso` produced.

- [ ] **Step 4: Manual QEMU check**

Run: `make run` (or the regression harness) — boot, type `format\n` then `ls\n`.
Expected: `Formatting filesystem... done` on serial/VGA; `ls` lists `bin/*` and `demo.ico`.

- [ ] **Step 5: Commit**

```bash
git add kernel/sfs.c kernel/pmm.c kernel/commands.c
git commit -m "fs: grow ramdisk to 1 MiB and restore embedded data on format"
```

---

### Task 2: musl toolchain integration + embed `lin/*` binaries

**Files:**
- Create: `tools/linux/ls.c`, `tools/linux/cat.c`, `tools/linux/test.txt`
- Modify: `Makefile`
- Modify: `.gitignore`
- Test: `build/linux/*` binaries + ISO

**Interfaces:**
- Consumes: existing `scripts/gen_progs.py --data` mechanism, `load_embedded_data()`.
- Produces: `LINUX_BINS` (`build/linux/hello|ls|cat`), embedded as SFS files `lin/hello`, `lin/ls`, `lin/cat`, `lin/test.txt`.

- [ ] **Step 1: Write the musl source files**

Create `tools/linux/ls.c`:

```c
#include <dirent.h>
#include <stdio.h>

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    DIR *d = opendir(dir);
    if (!d) {
        printf("ls: %s: No such file or directory\n", dir);
        return 1;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        printf("%s\n", e->d_name);
    }
    closedir(d);
    return 0;
}
```

Create `tools/linux/cat.c`:

```c
#include <stdio.h>

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            printf("cat: %s: No such file or directory\n", argv[i]);
            return 1;
        }
        int c;
        while ((c = fgetc(f)) != EOF)
            putchar(c);
        fclose(f);
    }
    return 0;
}
```

Create `tools/linux/test.txt`:

```
AOS musl cat test
second line
```

- [ ] **Step 2: Wire the Makefile**

Edit `Makefile`. Add after line 24 (`PROG_OBJS`):

```makefile
LINUX_CC  = tools/musl-i686/bin/i686-linux-musl-gcc
LINUX_SRCS = $(wildcard tools/linux/*.c)
LINUX_BINS = $(patsubst tools/linux/%.c,build/linux/%,$(LINUX_SRCS))
```

Add a build rule before the `kernel/progs.c` rule (line 59):

```makefile
build/linux/%: tools/linux/%.c
	@mkdir -p build/linux
	$(LINUX_CC) -static -no-pie -Os -o $@ $<
```

Replace the `kernel/progs.c` rule (lines 59-60):

```makefile
kernel/progs.c: $(PROG_ELFS) scripts/demo.ico scripts/gen_progs.py $(LINUX_BINS)
	$(PYTHON) scripts/gen_progs.py $(PROG_ELFS) --data demo.ico=scripts/demo.ico \
		--data lin/hello=build/linux/hello --data lin/ls=build/linux/ls \
		--data lin/cat=build/linux/cat --data lin/test.txt=tools/linux/test.txt > $@
```

Add `rm -rf build` to the `clean:` target (after line 80).

- [ ] **Step 3: Gitignore the toolchain and build output**

Edit `.gitignore`, append:

```
build/
tools/musl-i686/
tools/*.tgz
```

- [ ] **Step 4: Build and inspect**

Run: `make`
Expected: `build/linux/hello`, `build/linux/ls`, `build/linux/cat` are built.

Run: `tools/musl-i686/bin/i686-linux-musl-readelf -h build/linux/hello`
Expected: `Type: EXEC (Executable file)`, `Entry point address: 0x8048...`.

Run: `tools/musl-i686/bin/i686-linux-musl-objdump -d build/linux/hello | grep -c "int \$0x80"`
Expected: several syscall trampolines (>= 1).

- [ ] **Step 5: Verify embedding**

Run: `make clean && make && make` (idempotence, `.SECONDARY` guard).
Expected: second `make` is a no-op (nothing rebuilt).

- [ ] **Step 6: Commit**

```bash
git add Makefile .gitignore tools/linux/ build/linux
git commit -m "build: embed i686-musl lin/hello, lin/ls, lin/cat into ramdisk"
```

---

### Task 3: ABI flag + `linux_ctx` plumbing in the task layer

**Files:**
- Modify: `kernel/task.h`
- Create: `kernel/linux_syscall.h`
- Modify: `kernel/task.c`
- Test: build + boot (no behavior change yet)

**Interfaces:**
- Consumes: existing `struct task`, `task_init()`, `task_spawn()`.
- Produces:
  - `enum task_abi { ABI_AOS = 0, ABI_LINUX = 1 }`
  - `struct task` gains `unsigned int abi; struct linux_ctx *lctx;`
  - `struct linux_ctx` (in `kernel/linux_syscall.h`) with `fds[32]`, `fd_name[32][32]`, `fd_off[32]`, `brk_base/brk_cur`, `mmap_cur`, `stack_top`, `stack_sp`, `win_lo/win_hi`, TLS fields.
  - `void linux_ctx_init(struct linux_ctx *lc)`
  - `unsigned int task_current_abi(void)`, `int task_set_abi_current(unsigned int abi)`, `struct linux_ctx *task_current_lctx(void)`

- [ ] **Step 1: Add the ABI enum and fields**

Edit `kernel/task.h`: insert before `struct task`:

```c
enum task_abi { ABI_AOS = 0, ABI_LINUX = 1 };
```

Add two fields to `struct task` (after `char *args;`):

```c
    unsigned int abi;           // ABI_AOS or ABI_LINUX
    struct linux_ctx *lctx;     // Linux runtime context (kmalloc'd)
```

Add declarations (before `#endif`):

```c
unsigned int task_current_abi(void);
int task_set_abi_current(unsigned int abi);
struct linux_ctx *task_current_lctx(void);
```

- [ ] **Step 2: Define `linux_ctx`**

Create `kernel/linux_syscall.h`:

```c
#ifndef LINUX_SYSCALL_H
#define LINUX_SYSCALL_H

#define LINUX_FDS 32

struct linux_ctx {
    int fds[LINUX_FDS];              // -1 = free; 0,1,2 = std; >=3 = open file
    char fd_name[LINUX_FDS][32];     // SFS name behind each open fd
    unsigned int fd_off[LINUX_FDS];  // read cursor / getdents64 index
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

void linux_ctx_init(struct linux_ctx *lc);
void linux_syscall_handler(struct registers *r);

#endif
```

(`struct registers` is declared in `kernel/interrupts.h`; `linux_syscall.c` will include it before this header.)

- [ ] **Step 3: Allocate + reset per task**

Edit `kernel/task.c`:

In `task_init()`, after `tasks[0].mbox = kmalloc(...)` (line 93):

```c
    tasks[0].abi = ABI_AOS;
    tasks[0].lctx = kmalloc(sizeof(struct linux_ctx));
    linux_ctx_init(tasks[0].lctx);
```

In `task_spawn()`, after `t->args = argsb;` (line 191):

```c
    t->abi = ABI_AOS;
    t->lctx = kmalloc(sizeof(struct linux_ctx));
    if (!t->lctx) {
        kfree(ks); page_free(pd); kfree(mbox); kfree(argsb);
        for (int i = 0; i < 3; i++) page_free(pts[i]);
        t->state = TASK_FREE;
        return -1;
    }
    linux_ctx_init(t->lctx);
```

In the two later failure paths in `task_spawn()` (lines ~227-233 and the frame-building path uses no other alloc), free `t->lctx` wherever `kfree(t->args)` is already done. Concretely add `kfree(t->lctx); t->lctx = 0;` next to each `kfree(t->args)` cleanup in `task_spawn`.

In `task_switch_kernel()` exit path, after `kfree(dead->args);` (line 145):

```c
        kfree(dead->lctx);
        dead->lctx = 0;
```

Add the accessor functions at the end of `task.c`:

```c
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
```

Add `#include "linux_syscall.h"` to `kernel/task.c`.

- [ ] **Step 4: Build and boot-check**

Run: `make`; then boot (`make run` or harness).
Expected: builds clean; boots; WM spawns; no new warnings from `task.c` (`-Wall -Wextra`).

- [ ] **Step 5: Commit**

```bash
git add kernel/task.h kernel/task.c kernel/linux_syscall.h
git commit -m "task: add ABI flag and per-task linux_ctx plumbing"
```

---

### Task 4: LDT support for musl TLS

**Files:**
- Modify: `arch/i386/gdt.c`
- Modify: `arch/i386/gdt.h`
- Test: build + boot (no crash; kernel `%gs` unaffected)

**Interfaces:**
- Consumes: existing `gdt_set_gate`, `gdt_init`.
- Produces: `void ldt_set_tls(unsigned int base, unsigned int limit, unsigned int seg_32bit, unsigned int read_exec_only, unsigned int limit_in_pages)`.
- Later tasks call `ldt_set_tls()` from `kernel/linux_syscall.c` (on `set_thread_area`) and `kernel/task.c` (on switch).

- [ ] **Step 1: Extend the GDT to host an LDT descriptor**

Edit `arch/i386/gdt.c`:

```c
static struct gdt_entry gdt[7];      // +6: LDT descriptor
static struct gdt_entry ldt[8];      // LDT: entry 0 = current task's TLS
```

Add a gate-writer that takes an entry pointer (reuse for both tables):

```c
static void seg_set(struct gdt_entry *e, unsigned int base, unsigned int limit,
                    unsigned char access, unsigned char gran) {
    e->base_low     = base & 0xFFFF;
    e->base_middle  = (base >> 16) & 0xFF;
    e->base_high    = (base >> 24) & 0xFF;
    e->limit_low    = limit & 0xFFFF;
    e->granularity  = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    e->access       = access;
}
```

Rewrite `gdt_set_gate` to delegate:

```c
static void gdt_set_gate(int idx, unsigned int base, unsigned int limit,
                         unsigned char access, unsigned char gran) {
    seg_set(&gdt[idx], base, limit, access, gran);
}
```

Add `ldt_set_tls` after `tss_set_esp0`:

```c
#define LDT_SEL 0x30                // GDT index 6, RPL 0

void ldt_set_tls(unsigned int base, unsigned int limit,
                 unsigned int seg_32bit, unsigned int read_exec_only,
                 unsigned int limit_in_pages) {
    if (limit_in_pages)
        limit >>= 12;
    unsigned char access = read_exec_only ? 0xF0 : 0xF2;
    unsigned char gran = (seg_32bit ? 0x40 : 0x00) |
                         (limit_in_pages ? 0x80 : 0x00);
    seg_set(&ldt[0], base, limit, access, gran);
}
```

In `gdt_init()`:
- After the existing gate setup (line 71), add the LDT descriptor gate and clear the LDT:

```c
    gdt_set_gate(6, (unsigned int)&ldt, sizeof(ldt) - 1, 0x82, 0x00);
    for (unsigned int i = 0; i < sizeof(ldt) / 4; i++)
        ((unsigned int *)&ldt)[i] = 0;
```

- After `ltr` (line 92), load the LDTR:

```c
    __asm__ volatile("lldt %0" : : "r"((unsigned short)LDT_SEL));
```

- [ ] **Step 2: Declare the function**

Edit `arch/i386/gdt.h`:

```c
void ldt_set_tls(unsigned int base, unsigned int limit,
                 unsigned int seg_32bit, unsigned int read_exec_only,
                 unsigned int limit_in_pages);
```

- [ ] **Step 3: Build and boot-check**

Run: `make`; boot.
Expected: builds clean; boots; shell works; no #GP; `%gs` still kernel-data (GDT 0x10) — no regression in VGA/serial or any AOS program.

- [ ] **Step 4: Commit**

```bash
git add arch/i386/gdt.c arch/i386/gdt.h
git commit -m "gdt: add LDT with settable TLS entry for musl %gs support"
```

---

### Task 5: Task-0 Linux window + `paging_map_user_page`

**Files:**
- Modify: `kernel/paging.c`
- Modify: `kernel/paging.h`
- Modify: `kernel/pmm.c`
- Test: build + boot (no regression)

**Interfaces:**
- Consumes: `page_alloc_zero()` (pmm), kernel identity map `page_dir`/`page_tables`.
- Produces:
  - Task-0 Linux window `0x08000000..0x08800000` is identity-mapped with `PTE_USER`.
  - `int paging_map_user_page(unsigned int vaddr)` — maps one 4 KB page user-R/W into the currently-active PD's window PT (no-op if already present); returns 0 or -1.

- [ ] **Step 1: User bit on the task-0 window**

Edit `kernel/paging.c`. After the SLAB block (line 46), add:

```c
    // Task-0 Linux window: 0x08000000 .. 0x08800000 identity-mapped, user-accessible.
    for (int t = 32; t <= 33; t++) {
        page_dir[t] |= PTE_USER;
        for (int p = 0; p < 1024; p++)
            page_tables[t][p] |= PTE_USER;
    }
```

- [ ] **Step 2: Reserve the window in pmm**

Edit `kernel/pmm.c`, after the task-0 user area reserve (line 216):

```c
    reserve(0x08000000, 0x08800000);                     // task-0 Linux window
```

- [ ] **Step 3: Add `paging_map_user_page`**

Edit `kernel/paging.c`, append:

```c
int paging_map_user_page(unsigned int vaddr) {
    unsigned int pdi = vaddr >> 22;
    unsigned int pti = (vaddr >> 12) & 0x3FF;
    unsigned int *pd = (unsigned int *)paging_get_cr3();
    if (!(pd[pdi] & PTE_PRESENT)) return -1;
    unsigned int *pt = (unsigned int *)(pd[pdi] & 0xFFFFF000);
    if (pt[pti] & PTE_PRESENT) return 0;
    unsigned int frame = (unsigned int)page_alloc_zero();
    if (!frame) return -1;
    pt[pti] = frame | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    return 0;
}
```

Add `#include "pmm.h"` to `kernel/paging.c`.

Edit `kernel/paging.h`: add `int paging_map_user_page(unsigned int vaddr);`

- [ ] **Step 4: Build and boot-check**

Run: `make`; boot.
Expected: builds clean; boots; WM + desktop work; PMM selftest shows ~6 MB fewer free pages (8 MB window reserved) than before (57825 → ~55681, modulo TCG variance).

- [ ] **Step 5: Commit**

```bash
git add kernel/paging.c kernel/paging.h kernel/pmm.c
git commit -m "paging: reserve task-0 Linux window and add map_user_page"
```

---

### Task 6: Linux ELF loader + stack + task-0 launch + minimal syscalls (hello runs)

**Files:**
- Modify: `kernel/elf.h`, `kernel/elf.c`
- Modify: `kernel/progload.c`
- Modify: `kernel/user.c`, `kernel/user_tramp.S`
- Modify: `kernel/commands.c`
- Modify: `kernel/syscall.c`, `kernel/syscall.h`
- Create: `kernel/linux_syscall.c`
- Create: `scripts/linhello.py`
- Test: `scripts/linhello.py` PASS (Cyrillic hello text appears in the terminal band)

**Interfaces:**
- Consumes: `paging_map_user_page` (Task 5), `ldt_set_tls` (Task 4), `task_*` abi/lctx helpers (Task 3).
- Produces:
  - `int elf_probe(const char *path, int *abi)` — reads the ELF header; returns 0 + sets `*abi` (ABI_AOS/ABI_LINUX), or -1 on failure.
  - `void *elf_load_linux(const char *path, const char *args, struct linux_ctx *lc)` — loads PT_LOAD segments into the window, maps pages, builds argv/envp/auxv on the stack at `lc->stack_top`, sets `lc->brk_base/brk_cur`, `lc->stack_sp`, returns entry.
  - `void user_program_start_linux(void (*entry)(void), unsigned int esp)`
  - `void launch_user_linux(void (*entry)(void), unsigned int esp)` (in `user_tramp.S`)
  - `void route_text(const char *s, unsigned int len)` (now non-static, declared in `syscall.h`)
  - `linux_syscall_handler(struct registers *r)` — dispatches Linux syscalls 1,4,45,91,125,123,192,243,252,258 (step 1 set).
  - Dispatch hook at top of `syscall_handler`.

- [ ] **Step 1: ELF probe + Linux loader**

Edit `kernel/elf.h`: add to the header (after `struct elf_prog_header`):

```c
#define ET_DYN 3
#define LINUX_ENTRY_MIN 0x01100000
#define LINUX_BASE 0x08048000

int elf_probe(const char *path, int *abi);
void *elf_load_linux(const char *path, const char *args, struct linux_ctx *lc);
```

Edit `kernel/elf.c`. Add `#include "paging.h"`, `#include "task.h"`, `#include "linux_syscall.h"`, `#include "string.h"` (string.h is already included).

Append:

```c
int elf_probe(const char *path, int *abi) {
    int sz = fs_get_size(path);
    if (sz <= 0) return -1;
    char buf[128];
    if (fs_read_at(path, buf, sizeof(buf), 0) <= 0) return -1;
    struct elf_header *ehdr = (struct elf_header *)buf;
    if (ehdr->magic != ELF_MAGIC || ehdr->arch != 1 || ehdr->machine != 3)
        return -1;
    if (ehdr->type == ET_DYN || ehdr->entry >= LINUX_ENTRY_MIN)
        *abi = ABI_LINUX;
    else
        *abi = ABI_AOS;
    return 0;
}

// Build the initial user stack: argc/argv/envp/auxv + strings, returned via
// lc->stack_sp. Writes are performed top-down from lc->stack_top.
#define MAX_ARGC 16
#define STACK_MARGIN 0x10000   // pages mapped ahead of stack building
static void stack_build(struct linux_ctx *lc, const char *prog,
                        const char *args, struct elf_header *ehdr,
                        unsigned int phdr_vaddr) {
    const char *argv[MAX_ARGC];
    int argc = 0;
    argv[argc++] = prog;
    const char *p = args;
    while (p && *p && argc < MAX_ARGC - 1) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }

    unsigned char *s = (unsigned char *)lc->stack_top;

    // AT_RANDOM: 16 pseudo-random bytes
    unsigned char rnd[16];
    for (int i = 0; i < 16; i++)
        rnd[i] = (unsigned char)(0x9e * (i + 1) + 0x37 * i + i * i);
    s -= 16;
    memcpy(s, rnd, 16);
    unsigned int rand_addr = (unsigned int)s;

    // AT_EXECFN string
    unsigned int el = strlen(prog);
    s -= el + 1;
    memcpy(s, prog, el + 1);
    unsigned int execfn_addr = (unsigned int)s;

    // argv strings (copied into the stack)
    unsigned int arg_addrs[MAX_ARGC];
    for (int i = argc - 1; i >= 0; i--) {
        unsigned int n = strlen(argv[i]);
        s -= n + 1;
        memcpy(s, argv[i], n + 1);
        arg_addrs[i] = (unsigned int)s;
    }

    // auxv
    struct auxv_pair { unsigned int a, v; } auxv[] = {
        { 3, phdr_vaddr },              // AT_PHDR
        { 4, ehdr->phentsize },         // AT_PHENT
        { 5, ehdr->phnum },             // AT_PHNUM
        { 6, 4096 },                    // AT_PAGESZ
        { 7, 0 },                       // AT_BASE
        { 9, ehdr->entry },             // AT_ENTRY
        { 11, 0 },                      // AT_UID
        { 12, 0 },                      // AT_EUID
        { 13, 0 },                      // AT_GID
        { 14, 0 },                      // AT_EGID
        { 25, rand_addr },              // AT_RANDOM
        { 31, execfn_addr },            // AT_EXECFN
        { 0, 0 },
    };
    int naux = (int)(sizeof(auxv) / sizeof(auxv[0]));
    s -= naux * 8;
    memcpy(s, auxv, naux * 8);

    // envp array = { NULL }
    unsigned int z = 0;
    s -= 4;
    memcpy(s, &z, 4);

    // argv pointer array + argc
    s -= (argc + 1) * 4;
    unsigned int *av = (unsigned int *)s;
    for (int i = 0; i < argc; i++)
        av[i] = arg_addrs[i];
    av[argc] = 0;
    s -= 4;
    *(unsigned int *)s = (unsigned int)argc;

    lc->stack_sp = ((unsigned int)s) & ~0xFu;
}

void *elf_load_linux(const char *path, const char *args, struct linux_ctx *lc) {
    int sz = fs_get_size(path);
    if (sz <= 0) { elf_error("linux: not found"); return 0; }

    char buf[4096];
    int got = fs_read_at(path, buf, sizeof(buf), 0);
    if (got <= 0) { elf_error("linux: read failed"); return 0; }

    struct elf_header *ehdr = (struct elf_header *)buf;
    if (ehdr->magic != ELF_MAGIC || ehdr->arch != 1 || ehdr->machine != 3 ||
        ehdr->phentsize != sizeof(struct elf_prog_header)) {
        elf_error("linux: bad header");
        return 0;
    }
    if (ehdr->phoff + ehdr->phnum * ehdr->phentsize > (unsigned int)got) {
        elf_error("linux: phdrs out of range");
        return 0;
    }
    if (ehdr->entry < LINUX_BASE || ehdr->entry >= lc->win_hi) {
        elf_error("linux: entry out of range");
        return 0;
    }

    // Map the stack region first (identity pages for task 0 are already there).
    for (unsigned int a = lc->stack_top - STACK_MARGIN; a < lc->stack_top; a += 0x1000)
        if (paging_map_user_page(a) < 0) { elf_error("linux: stack map failed"); return 0; }

    struct elf_prog_header *phdr = (struct elf_prog_header *)(buf + ehdr->phoff);
    unsigned int phdr_vaddr = 0;
    unsigned int brk_hi = 0;

    for (unsigned int i = 0; i < ehdr->phnum; i++) {
        if (phdr[i].type != PT_LOAD) continue;

        unsigned int vaddr  = phdr[i].vaddr;
        unsigned int memsz  = phdr[i].memsz;
        unsigned int filesz = phdr[i].filesz;
        unsigned int offset = phdr[i].offset;

        if (vaddr < lc->win_lo || vaddr + memsz > lc->win_hi ||
            vaddr + memsz < vaddr) {
            elf_error("linux: segment out of range");
            return 0;
        }
        if (filesz > memsz) filesz = memsz;

        if (offset <= ehdr->phoff &&
            ehdr->phoff - offset < filesz)
            phdr_vaddr = vaddr + (ehdr->phoff - offset);

        for (unsigned int a = vaddr & ~0xFFFu; a < vaddr + memsz; a += 0x1000)
            if (paging_map_user_page(a) < 0) {
                elf_error("linux: segment map failed");
                return 0;
            }

        char *dst = (char *)vaddr;
        if (filesz > 0) {
            if (fs_read_at(path, dst, filesz, offset) <= 0) {
                elf_error("linux: segment read failed");
                return 0;
            }
        }
        for (unsigned int j = filesz; j < memsz; j++)
            dst[j] = 0;

        unsigned int end = (vaddr + memsz + 0xFFF) & ~0xFFFu;
        if (end > brk_hi) brk_hi = end;
    }

    if (!phdr_vaddr) {
        // phdr table lies before the first LOAD's file offset in unusual
        // builds; fall back to the base + phoff (covers the musl layout).
        phdr_vaddr = LINUX_BASE + ehdr->phoff;
    }

    lc->brk_base = brk_hi;
    lc->brk_cur  = brk_hi;

    stack_build(lc, path, args, ehdr, phdr_vaddr);

    // Map any stack pages below the pre-mapped margin that stack_build used.
    for (unsigned int a = lc->stack_sp & ~0xFFFu; a < lc->stack_top; a += 0x1000)
        if (paging_map_user_page(a) < 0) { elf_error("linux: stack tail map failed"); return 0; }

    return (void *)ehdr->entry;
}
```

Note on `elf_error`: it's a `static` in `elf.c`; `elf_load_linux` uses it (same file, fine).

- [ ] **Step 2: Task-0 launch path with a caller-specified ESP**

Edit `kernel/user_tramp.S`, add after `launch_user_asm`:

```asm
# void launch_user_linux(void (*entry)(void), unsigned int esp)
# Like launch_user_asm, but takes the initial user ESP from the caller
# (used for Linux binaries loaded at 0x08048000+).
.global launch_user_linux
launch_user_linux:
    movl 4(%esp), %ecx          # entry point
    movl 8(%esp), %edx          # user ESP
    movl %esp, saved_esp        # kernel stack to return to on program exit
    movl $0x23, %eax            # user data segment
    movl %eax, %ds
    movl %eax, %es
    movl %eax, %fs
    movl %eax, %gs
    pushl $0x23                 # user SS
    pushl %edx                  # user ESP
    pushl $0x202                # EFLAGS: IF set, IOPL 0
    pushl $0x1B                 # user CS
    pushl %ecx                  # EIP
    iret
```

Edit `kernel/user.c`: add declarations + wrapper:

```c
void launch_user_linux(void (*entry)(void), unsigned int esp);
void user_program_start_linux(void (*entry)(void), unsigned int esp) {
    program_active = 1;
    launch_user_linux(entry, esp);
}
```

- [ ] **Step 3: `program_load` dispatches on ABI**

Edit `kernel/progload.c`:

```c
#include "task.h"
#include "linux_syscall.h"

#define LINUX_WIN_LO 0x08000000
#define LINUX_WIN_HI 0x08800000

void *program_load(const char *path, const char *args) {
    syscall_set_args(args);
    int abi;
    if (elf_probe(path, &abi) < 0) return 0;
    if (abi == ABI_LINUX) {
        struct linux_ctx *lc = task_current_lctx();
        linux_ctx_init(lc);
        lc->win_lo = LINUX_WIN_LO;
        lc->win_hi = LINUX_WIN_HI;
        lc->stack_top = LINUX_WIN_HI;
        lc->mmap_cur = LINUX_WIN_HI - STACK_MARGIN;   // below the stack margin
        task_set_abi_current(ABI_LINUX);
        return elf_load_linux(path, args, lc);
    }
    return elf_load(path);
}
```

Add `#define STACK_MARGIN 0x10000` locally (or reference the loader's constant — repeat the definition here to keep files independent; the value must match Task 6 Step 1's `STACK_MARGIN`).

- [ ] **Step 4: Shell launch chooses the trampoline**

Edit `kernel/commands.c`, `try_exec()` (lines 28-38):

```c
static int try_exec(const char *full_path, const char *arg) {
    void (*entry)(void) = program_load(full_path, arg);
    if (entry) {
        if (task_current_abi() == ABI_LINUX)
            user_program_start_linux(entry, task_current_lctx()->stack_sp);
        else
            user_program_start(entry);
        terminal_set_prompt();  // runs after the program exits
        return 1;
    }
    terminal_print("\nFailed to load: ");
    terminal_print(full_path);
    return 0;
}
```

Add `#include "task.h"` and `#include "linux_syscall.h"` to `kernel/commands.c`.

- [ ] **Step 5: Syscall dispatch hook + expose `route_text`**

Edit `kernel/syscall.c`:
- Change `static void route_text(` to `void route_text(` (line 71).
- At the very top of `syscall_handler` (before `switch`), add:

```c
    if (task_current_abi() == ABI_LINUX) {
        linux_syscall_handler(r);
        return;
    }
```

- Ensure `#include "linux_syscall.h"` is present.

Edit `kernel/syscall.h`, append:

```c
void route_text(const char *s, unsigned int len);
```

- [ ] **Step 6: Minimal Linux syscall dispatcher**

Create `kernel/linux_syscall.c`:

```c
#include "interrupts.h"
#include "linux_syscall.h"
#include "task.h"
#include "user.h"
#include "fs.h"
#include "sfs.h"
#include "paging.h"
#include "gdt.h"
#include "string.h"
#include "syscall.h"

static char lin_str[1024];

static struct linux_ctx *cur_lctx(void) {
    return task_current_lctx();
}

static int in_luser(const void *p, unsigned int n) {
    unsigned int a = (unsigned int)p;
    struct linux_ctx *lc = cur_lctx();
    return a >= lc->win_lo && n <= lc->win_hi - a;
}

static int copy_lin_str(const void *usr, char *dst, unsigned int max) {
    unsigned int a = (unsigned int)usr;
    struct linux_ctx *lc = cur_lctx();
    if (a < lc->win_lo) return -1;
    unsigned int i = 0;
    while (i < max - 1) {
        if (a + i >= lc->win_hi) break;
        char c = *(const char *)(a + i);
        dst[i++] = c;
        if (c == '\0') return i;
    }
    dst[i] = '\0';
    return i;
}

static int lc_alloc_fd(struct linux_ctx *lc, const char *name) {
    if (!fs_exists(name)) return -1;
    for (int i = 3; i < LINUX_FDS; i++) {
        if (lc->fds[i] < 0) {
            strncpy(lc->fd_name[i], name, 31);
            lc->fd_name[i][31] = '\0';
            lc->fds[i] = 1;
            lc->fd_off[i] = 0;
            return i;
        }
    }
    return -24;   // EMFILE
}

static void linux_exit(void) {
    if (task_current_pid() == 0) {
        task_set_abi_current(ABI_AOS);
        linux_ctx_init(task_current_lctx());
        user_program_exit();
    }
    task_exit_current();
}

void linux_syscall_handler(struct registers *r) {
    unsigned int n = r->eax;
    struct linux_ctx *lc = cur_lctx();

    switch (n) {
    case 1:    // exit
    case 252:  // exit_group
        linux_exit();
        break;

    case 4: {  // write(fd, buf, count)
        int fd = r->ebx;
        const char *buf = (const char *)r->ecx;
        unsigned int count = r->edx;
        if (!in_luser(buf, count)) { r->eax = -14; break; }   // -EFAULT
        if (fd <= 2) {
            route_text(buf, count);
            r->eax = count;
        } else {
            r->eax = -9;                                      // -EBADF
        }
        break;
    }

    case 45: {  // brk(addr)
        unsigned int addr = r->ebx;
        if (addr == 0) { r->eax = lc->brk_cur; break; }
        if (addr < lc->brk_base || addr > lc->win_hi) { r->eax = lc->brk_cur; break; }
        for (unsigned int a = (lc->brk_cur + 0xFFF) & ~0xFFFu; a < addr; a += 0x1000)
            if (paging_map_user_page(a) < 0) { r->eax = lc->brk_cur; break; }
        lc->brk_cur = addr;
        r->eax = lc->brk_cur;
        break;
    }

    case 192: {  // mmap2(addr, len, prot, flags, fd, off_pages)
        unsigned int addr = r->ebx;
        unsigned int len = r->ecx;
        unsigned int flags = r->edx;
        if (len == 0) { r->eax = -22; break; }                // -EINVAL
        len = (len + 0xFFF) & ~0xFFFu;
        unsigned int base;
        if (addr && (flags & 0x10)) {                          // MAP_FIXED
            base = addr & ~0xFFFu;
        } else {
            if (lc->mmap_cur < lc->win_lo + len) { r->eax = -12; break; } // -ENOMEM
            lc->mmap_cur -= len;
            base = lc->mmap_cur;
        }
        for (unsigned int a = base; a < base + len; a += 0x1000)
            if (paging_map_user_page(a) < 0) { r->eax = -12; break; }
        r->eax = base;
        break;
    }

    case 91:     // munmap — pages stay mapped (leak) for step 1
    case 125:    // mprotect
        r->eax = 0;
        break;

    case 243: {  // set_thread_area(user_desc*)
        struct {
            unsigned int entry_number;
            unsigned int base_addr;
            unsigned int limit;
            unsigned int flags;
        } ud;
        if (!in_luser((const void *)r->ebx, sizeof(ud))) { r->eax = -14; break; }
        memcpy(&ud, (const void *)r->ebx, sizeof(ud));
        unsigned int bitfield = ud.flags;
        lc->tls_base = ud.base_addr;
        lc->tls_limit = ud.limit;
        lc->tls_seg32 = (bitfield >> 0) & 1;
        lc->tls_ro = (bitfield >> 3) & 1;
        lc->tls_gran_pages = (bitfield >> 4) & 1;
        ldt_set_tls(lc->tls_base, lc->tls_limit,
                    lc->tls_seg32, lc->tls_ro, lc->tls_gran_pages);
        *(unsigned int *)r->ebx = 0;   // entry_number = 0 -> selector 0x03
        r->eax = 0;
        break;
    }

    case 123: {  // modify_ldt(func, ptr, bytecount) — func 1 == write
        unsigned int func = r->ebx;
        if (func == 1) {
            struct {
                unsigned int entry_number;
                unsigned int base_addr;
                unsigned int limit;
                unsigned int flags;
            } ld;
            if (!in_luser((const void *)r->ecx, sizeof(ld))) { r->eax = -14; break; }
            memcpy(&ld, (const void *)r->ecx, sizeof(ld));
            lc->tls_base = ld.base_addr;
            lc->tls_limit = ld.limit;
            lc->tls_seg32 = (ld.flags >> 0) & 1;
            lc->tls_ro = (ld.flags >> 3) & 1;
            lc->tls_gran_pages = (ld.flags >> 4) & 1;
            ldt_set_tls(lc->tls_base, lc->tls_limit,
                        lc->tls_seg32, lc->tls_ro, lc->tls_gran_pages);
            *(unsigned int *)r->ecx = 0;
        }
        r->eax = 0;
        break;
    }

    case 258:    // set_tid_address(ptr) — no kernel pid stored; tid is 0
        r->eax = 0;
        break;

    default:
        r->eax = -38;   // -ENOSYS
        break;
    }
}
```

Add `kernel/linux_syscall.c` to `KERNEL_OBJS` in the Makefile (line 12-20 block, next to `kernel/syscall.o`).

- [ ] **Step 7: Write the E2E test**

Create `scripts/linhello.py` (mirror of `scripts/manytest.py`; own socket/serial/ppm names so it does not collide with `guitester.py`):

```python
#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-linhello.sock"
SER = "/tmp/aos-linhello.log"
PPM = "/tmp/aos-linhello.ppm"
BEFORE = "/tmp/aos-linhello-before.ppm"

TXT_X0, TXT_X1 = 21, 660          # term text band, x range
TXT_Y0, TXT_Y1 = 39, 39 + 26 * 16 # term text band, y range (all 26 rows)
TXT_THRESHOLD = 500               # band must grow by more than this (pixels)

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

def ppm_data(path):
    with open(path, "rb") as f:
        if f.readline().strip() != b"P6":
            raise AssertionError("not a P6 PPM: " + path)
        dim = f.readline().split()
        f.readline()                       # maxval
        return int(dim[0]), int(dim[1]), f.read()

def count_text_pixels(path, x0, y0, x1, y1):
    w, h, data = ppm_data(path)
    n = 0
    for y in range(y0, y1 + 1):
        row = data[(y * w + x0) * 3:(y * w + x1 + 1) * 3]
        for i in range(0, len(row), 3):
            if row[i] >= 0xC0 and row[i + 1] >= 0xC0 and row[i + 2] >= 0xC0:
                n += 1
    return n

def main():
    for path in (MON, SER, PPM, BEFORE):
        try: os.unlink(path)
        except FileNotFoundError: pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-serial", "file:" + SER, "-monitor", "unix:" + MON + ",server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(MON)
        time.sleep(5)
        hmp("mouse_move -39 341")    # dock launcher spawns a terminal
        hmp("mouse_button 1")
        hmp("mouse_button 0")
        time.sleep(1)
        hmp("screendump " + BEFORE)
        wait_for(BEFORE)
        send_text("lin/hello\n")
        end = time.time() + 25
        log = ""
        while time.time() < end:
            time.sleep(1)
            log = ""
            try:
                with open(SER, "r", errors="replace") as f: log = f.read()
            except FileNotFoundError:
                pass
            if "KERNEL PANIC" in log: break
        if "KERNEL PANIC" in log:
            raise AssertionError("lin/hello triggered a kernel panic")
        hmp("screendump " + PPM)
        wait_for(PPM)
        if os.path.getsize(PPM) <= 1024:
            raise AssertionError("window manager did not produce a framebuffer dump")
        before_txt = count_text_pixels(BEFORE, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        after_txt = count_text_pixels(PPM, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        if after_txt - before_txt <= TXT_THRESHOLD:
            raise AssertionError(
                "terminal did not render lin/hello output (band text grew %d, want > %d)"
                % (after_txt - before_txt, TXT_THRESHOLD))
        print("PASS: musl hello runs in the kernel shell")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 8: Run the test**

Run: `make && python3 scripts/linhello.py`
Expected: `PASS: musl hello runs in the kernel shell` — Cyrillic `Привет от программы...` appears in the term window (bright-pixel band grows by > 500 px). If the band does not grow, check serial log for `KERNEL PANIC`, and check the WM pixel test is using the correct term coordinates (term window at ~(20,20) 640x416 px; text starts at x≈21, y≈39).

- [ ] **Step 9: Commit**

```bash
git add kernel/elf.h kernel/elf.c kernel/progload.c kernel/user.c \
        kernel/user_tramp.S kernel/commands.c kernel/syscall.c kernel/syscall.h \
        kernel/linux_syscall.c Makefile scripts/linhello.py
git commit -m "linux: run static musl hello via loader, task-0 trampoline and Linux syscalls"
```

---

### Task 7: Full Linux syscall set + file descriptors (ls/cat run)

**Files:**
- Modify: `kernel/linux_syscall.c`
- Create: `scripts/lincat.py`
- Test: `scripts/lincat.py` PASS (`lin/cat lin/test.txt` prints both lines in the term band)

**Interfaces:**
- Consumes: `linux_syscall_handler` (Task 6), `fs_*`/`sfs_get_entry` API, `route_text`.
- Produces: Linux syscalls 3,5,6,10,13,19,20,24,33,47,49,50,54,78,122,140,162,195,197,220,265,295,300 implemented with `-errno` semantics.

- [ ] **Step 1: Add file descriptor + read + file-open syscalls**

Append to the `switch` in `linux_syscall_handler` (before `default:`), keeping existing cases:

```c
    case 3: {  // read(fd, buf, count)
        int fd = r->ebx;
        char *buf = (char *)r->ecx;
        unsigned int count = r->edx;
        if (fd == 0) { r->eax = -11; break; }                 // -EAGAIN
        if (fd < 0 || fd >= LINUX_FDS || lc->fds[fd] < 0 || fd <= 2) { r->eax = -9; break; }
        if (!in_luser(buf, count)) { r->eax = -14; break; }
        int n = fs_read_at(lc->fd_name[fd], buf, count, lc->fd_off[fd]);
        if (n < 0) { r->eax = -2; break; }                    // -ENOENT
        lc->fd_off[fd] += (unsigned int)n;
        r->eax = n;
        break;
    }

    case 5:   // open(path, flags, mode)
    case 295: { // openat(dirfd, path, flags, mode)
        const void *pp = (n == 295) ? (const void *)r->ecx : (const void *)r->ebx;
        if (copy_lin_str(pp, lin_str, sizeof(lin_str)) < 0) { r->eax = -14; break; }
        int fd = lc_alloc_fd(lc, lin_str);
        r->eax = (fd < 0) ? (fd == -24 ? -24 : -2) : fd;
        break;
    }

    case 6: {  // close(fd)
        int fd = r->ebx;
        if (fd < 0 || fd >= LINUX_FDS || lc->fds[fd] < 0) { r->eax = -9; break; }
        lc->fds[fd] = -1;
        r->eax = 0;
        break;
    }

    case 10:   // unlink(path)
        if (copy_lin_str((const void *)r->ebx, lin_str, sizeof(lin_str)) < 0) {
            r->eax = -14;
        } else {
            r->eax = fs_delete(lin_str) == 0 ? 0 : -2;
        }
        break;

    case 19:   // lseek(fd, offset, whence)
    case 140: { // _llseek(fd, off_hi, off_lo, res, whence)
        int fd = r->ebx;
        if (fd < 0 || fd >= LINUX_FDS || lc->fds[fd] < 0 || fd <= 2) { r->eax = -9; break; }
        unsigned int off, whence;
        unsigned int *res = 0;
        if (n == 140) {
            off = r->edx;              // off_lo
            res = (unsigned int *)r->esi;
            whence = r->edi;
            if (!in_luser(res, 4)) { r->eax = -14; break; }
        } else {
            off = r->ecx;
            whence = r->edx;
        }
        int sz = fs_get_size(lc->fd_name[fd]);
        unsigned int cur = lc->fd_off[fd];
        unsigned int newoff = cur;
        if (whence == 0) newoff = off;
        else if (whence == 1) newoff = cur + off;
        else if (whence == 2) newoff = (sz < 0 ? 0 : (unsigned int)sz) + off;
        if (newoff > 0x7FFFFFFF) newoff = 0x7FFFFFFF;
        lc->fd_off[fd] = newoff;
        if (n == 140)
            *res = newoff;
        r->eax = (n == 140) ? 0 : newoff;
        break;
    }

    case 33:   // access(path, mode)
        if (copy_lin_str((const void *)r->ebx, lin_str, sizeof(lin_str)) < 0) {
            r->eax = -14;
        } else {
            r->eax = fs_exists(lin_str) ? 0 : -2;
        }
        break;

    case 13:   // time(t) — return 0
    case 20:   // getpid
        if (n == 20) { r->eax = task_current_pid(); break; }
        r->eax = 0;
        break;

    case 24:   // getuid
    case 47:   // getgid
    case 49:   // geteuid
    case 50:   // getegid
        r->eax = 0;
        break;

    case 54:   // ioctl — no ttys
        r->eax = -25;                                       // -ENOTTY
        break;

    case 78: {  // gettimeofday(tv, tz)
        void *tv = (void *)r->ebx;
        if (tv) {
            if (!in_luser(tv, 8)) { r->eax = -14; break; }
            memset(tv, 0, 8);
        }
        r->eax = 0;
        break;
    }

    case 122: {  // uname(struct utsname*)
        unsigned char *u = (unsigned char *)r->ebx;
        if (!in_luser(u, 390)) { r->eax = -14; break; }
        memset(u, 0, 390);
        strncpy((char *)(u + 0),   "Linux", 65);
        strncpy((char *)(u + 65),  "aos", 65);
        strncpy((char *)(u + 130), "5.0.0", 65);
        strncpy((char *)(u + 195), "#1", 65);
        strncpy((char *)(u + 260), "i686", 65);
        strncpy((char *)(u + 325), "aos", 65);
        r->eax = 0;
        break;
    }

    case 162: {  // nanosleep(req, rem) — spin on the PIT tick
        extern volatile unsigned int tick;
        const unsigned char *req = (const unsigned char *)r->ebx;
        if (!in_luser(req, 8)) { r->eax = -14; break; }
        unsigned int sec, nsec;
        memcpy(&sec, req, 4);
        memcpy(&nsec, req + 4, 4);
        unsigned int ms = sec * 1000 + nsec / 1000000u;
        unsigned int start = tick;
        while (tick - start < ms);
        r->eax = 0;
        break;
    }

    case 265: {  // clock_gettime(clockid, timespec*)
        void *ts = (void *)r->ecx;
        if (!in_luser(ts, 8)) { r->eax = -14; break; }
        memset(ts, 0, 8);
        r->eax = 0;
        break;
    }
```

- [ ] **Step 2: stat syscalls**

Add a helper before `linux_syscall_handler`:

```c
// i386 musl struct stat (see toolchain bits/stat.h): 108 bytes.
static void fill_stat64(struct linux_ctx *lc, unsigned int size, unsigned char *st) {
    memset(st, 0, 108);
    *(unsigned int *)(st + 8)  = 1;                        // __st_ino_truncated
    *(unsigned int *)(st + 12) = 0x81A4;                   // st_mode S_IFREG|0644
    *(unsigned int *)(st + 16) = 1;                        // st_nlink
    *(unsigned int *)(st + 20) = 0;                        // st_uid
    *(unsigned int *)(st + 24) = 0;                        // st_gid
    *(unsigned long long *)(st + 36) = size;               // st_size
    *(unsigned int *)(st + 44) = 4096;                     // st_blksize
    *(unsigned long long *)(st + 48) = (unsigned long long)(size + 511) / 512; // st_blocks
    *(unsigned int *)(st + 80) = 1;                        // st_ino
}
```

Add cases:

```c
    case 195:   // stat64(path, st)
    case 300: { // fstatat64(dirfd, path, st, flags)
        const void *pp = (n == 300) ? (const void *)r->ecx : (const void *)r->ebx;
        unsigned char *st = (unsigned char *)r->edx;
        if (copy_lin_str(pp, lin_str, sizeof(lin_str)) < 0) { r->eax = -14; break; }
        if (!in_luser(st, 108)) { r->eax = -14; break; }
        int size = fs_get_size(lin_str);
        if (size < 0) { r->eax = -2; break; }
        fill_stat64(lc, (unsigned int)size, st);
        r->eax = 0;
        break;
    }

    case 197: {  // fstat64(fd, st)
        int fd = r->ebx;
        unsigned char *st = (unsigned char *)r->ecx;
        if (fd < 0 || fd >= LINUX_FDS || lc->fds[fd] < 0 || fd <= 2) { r->eax = -9; break; }
        if (!in_luser(st, 108)) { r->eax = -14; break; }
        int size = fs_get_size(lc->fd_name[fd]);
        if (size < 0) size = 0;
        fill_stat64(lc, (unsigned int)size, st);
        r->eax = 0;
        break;
    }
```

- [ ] **Step 3: getdents64**

Add a helper before `linux_syscall_handler`:

```c
// linux_dirent64: u64 d_ino, i64 d_off, u16 d_reclen, u8 d_type, char d_name[]
static void put_dirent64(unsigned char *dst, unsigned long long ino,
                         unsigned long long off, unsigned char type,
                         const char *name) {
    unsigned int len = (unsigned int)strlen(name);
    unsigned short reclen = (unsigned short)(19 + len);
    *(unsigned long long *)(dst + 0) = ino;
    *(unsigned long long *)(dst + 8) = off;
    *(unsigned short *)(dst + 16) = reclen;
    dst[18] = type;
    for (unsigned int i = 0; i < len; i++)
        dst[19 + i] = (unsigned char)name[i];
}
```

Add a case:

```c
    case 220: {  // getdents64(fd, buf, count)
        int fd = r->ebx;
        unsigned char *buf = (unsigned char *)r->ecx;
        unsigned int count = r->edx;
        if (fd < 0 || fd >= LINUX_FDS || lc->fds[fd] < 0 || fd <= 2) { r->eax = -9; break; }
        if (!in_luser(buf, count)) { r->eax = -14; break; }
        unsigned int idx = lc->fd_off[fd];   // reused as the SFS entry cursor
        unsigned int written = 0;
        for (; idx < SFS_MAX_FILES; idx++) {
            char name[32];
            unsigned int size;
            if (sfs_get_entry(idx, name, &size) != 0) continue;
            unsigned int len = (unsigned int)strlen(name);
            unsigned int reclen = 19 + len;
            if (written + reclen > count) break;
            unsigned char type = (name[len - 1] == '/') ? 4 : 8;
            put_dirent64(buf + written, (unsigned long long)idx + 1,
                         (unsigned long long)(written + reclen), type, name);
            written += reclen;
            lc->fd_off[fd] = idx + 1;
        }
        r->eax = written;
        break;
    }
```

Add `#include "sfs.h"` if not already present.

- [ ] **Step 4: Write the E2E test**

Create `scripts/lincat.py` — copy `scripts/linhello.py` and change only: `MON/SER/PPM/BEFORE` to `/tmp/aos-lincat.*`, the sent text to `lin/cat lin/test.txt\n`, and the PASS message to `PASS: musl cat prints an embedded text file`.

- [ ] **Step 5: Run the tests**

Run: `make && python3 scripts/linhello.py && python3 scripts/lincat.py`
Expected: both PASS. `lin/cat lin/test.txt` prints `AOS musl cat test` / `second line` (band grows > 500 px).

- [ ] **Step 6: Commit**

```bash
git add kernel/linux_syscall.c scripts/lincat.py
git commit -m "linux: implement file fds, stat, getdents64 and time syscalls for ls/cat"
```

---

### Task 8: Spawned Linux tasks (pid > 0, private window)

**Files:**
- Modify: `kernel/task.h`, `kernel/task.c`
- Create: `programs/linrun.c`
- Modify: `Makefile` (add `linrun` to `PROGRAMS`; add to `KERNEL_OBJS` nothing — it is a user program)
- Test: boot, type `linrun\n`, `PASS` in serial + hello text in band (extends `linhello.py` or manual)

**Interfaces:**
- Consumes: `elf_probe`, `elf_load_linux`, `paging_map_user_page`, `ldt_set_tls`, `task_current_lctx`.
- Produces:
  - `task_spawn()` Linux branch: window PTs PD 32..63 (32 page tables), synthetic frame user ESP = `lc->stack_sp`.
  - `struct task` gains `unsigned int *lpts[32];` (window PT pages).
  - `task_free_addrspace()` frees window PT frames for ABI_LINUX.
  - `task_switch_kernel()` reinstalls LDT for a resumed ABI_LINUX task.

- [ ] **Step 1: Extend `struct task`**

Edit `kernel/task.h`, add after `unsigned int *pts[3];`:

```c
    unsigned int *lpts[32];     // Linux window (PD 32..63) page-table pages
```

- [ ] **Step 2: Linux branch in `task_spawn`**

Edit `kernel/task.c`:

In `task_spawn`, replace the load section (lines 216-224) with:

```c
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
```

Define `STACK_MARGIN` as `0x10000` near the top of `kernel/task.c` (must match the loader's value).

Update the frame builder (lines 244-254): for ABI_LINUX use `t->lctx->stack_sp`:

```c
    w[17] = (t->abi == ABI_LINUX) ? t->lctx->stack_sp : TASK_USTACK_TOP;
```

- [ ] **Step 3: Free the window address space**

Edit `task_free_addrspace` (lines 66-78) to add after the existing 3-PT loop, before `page_free(t->pd)`:

```c
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
```

- [ ] **Step 4: Reinstall LDT on task switch**

Edit `task_switch_kernel`, inside `if (next != current_task)` (after `paging_set_cr3`, line 134):

```c
        // A Linux task that installed TLS (tls_seg32 is set) must see the
        // descriptor again when it resumes: %gs reloads pull from the LDT.
        if (next->abi == ABI_LINUX && next->lctx && next->lctx->tls_seg32)
            ldt_set_tls(next->lctx->tls_base, next->lctx->tls_limit,
                        next->lctx->tls_seg32, next->lctx->tls_ro,
                        next->lctx->tls_gran_pages);
```

- [ ] **Step 5: A small launcher program for the E2E check**

Create `programs/linrun.c`:

```c
#include "libaos.h"

// Spawn lin/hello as a real (pid>0) Linux task with sink 0 so its stdout
// routes straight to the kernel terminal. Exercises the private-window path.
int main(void) {
    unsigned int pid;
    int rc = sys_spawn("lin/hello", "", 0, &pid);
    if (rc < 0) {
        print("spawn failed rc=");
        print_dec(rc);
        print("\n");
        return 1;
    }
    print("linux task ");
    print_dec(pid);
    print(" spawned\n");
    return 0;
}
```

Check `programs/libaos.h` for the exact `sys_spawn` signature; adjust to match (args may be `(const char *path, const char *args, unsigned int sink, unsigned int *pid)`).

Add `linrun` to `PROGRAMS` in the Makefile (line 22).

- [ ] **Step 6: Boot-test**

Run: `make`; boot; type `linrun\n`.
Expected: serial/VGA prints `linux task N spawned` and `Привет от программы...` appears on the terminal. No panic; WM keeps rendering.

- [ ] **Step 7: Commit**

```bash
git add kernel/task.h kernel/task.c programs/linrun.c Makefile
git commit -m "task: spawn Linux ELFs with a private 0x08000000 window and LDT on switch"
```

---

### Task 9: Regression + documentation

**Files:**
- Modify: `AGENTS.md`
- Modify: `docs/superpowers/specs/2026-08-04-linux-elf-design.md` (mark implemented status, if it has one)
- Test: full regression suite

- [ ] **Step 1: Run the regression suite**

Run each (each boots its own QEMU; allow ~2-4 min total):

```bash
python3 scripts/manytest.py
python3 scripts/ipctest.py
python3 scripts/notepadtest.py
python3 scripts/linhello.py
python3 scripts/lincat.py
```

Expected: all PASS. `guitester.py` optional (needs a display).

- [ ] **Step 2: Update AGENTS.md**

Append a "Linux ELF execution (step 1)" section covering: ABI detection rule, the window layout, LDT/TLS handling, the syscall list + `-errno` convention, the toolchain build command (`-static -no-pie -Os`), the `lin/*` embed mechanism, and the new test scripts (`linhello.py`, `lincat.py`).

- [ ] **Step 3: Update the spec doc**

Mark the accepted design as implemented (strike-through or a status note), listing which parts shipped in step 1 (task-0 + spawned paths, syscalls) and any deferred items (real time, TTY ioctls, file writes on fds > 2, munmap reclaim).

- [ ] **Step 4: Commit**

```bash
git add AGENTS.md docs/superpowers/specs/2026-08-04-linux-elf-design.md
git commit -m "docs: record Linux ELF step-1 implementation and test scripts"
```

---

## Self-Review Notes

- **Spec coverage:** ABI detection (Task 6/8), memory model — task-0 window (Task 5), spawned window (Task 8), ramdisk 1 MB (Task 1), toolchain + embed (Task 2), syscall dispatch + `-errno` (Tasks 6-7), LDT/TLS (Tasks 4,6,8), `format` reloads embedded data (Task 1), E2E tests (Tasks 6,7,9).
- **Type consistency:** `elf_probe(path, int *abi)` / `elf_load_linux(path, args, lc)` / `linux_ctx` fields (`win_lo`, `win_hi`, `stack_top`, `stack_sp`, `brk_base`, `brk_cur`, `mmap_cur`, TLS fields) are identical everywhere they are used (progload, task_spawn, linux_syscall, elf loader). `STACK_MARGIN == 0x10000` is defined once per file that needs it (loader, progload, task.c) — keep the three definitions equal.
- **Placeholder scan:** no TODOs; every step has concrete code and a runnable check.
