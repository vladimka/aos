# Design: Linux ELF execution (step 1 — static musl utilities)

Date: 2026-08-04
Status: approved for specification review

## Goal

Run statically-linked musl Linux ELF32 binaries (`hello`, `ls`, `cat`) in AOS
ring 3 with a working subset of the Linux i386 syscall ABI: `exit`,
`exit_group`, `write`, `read`, `open`, `openat`, `close`, `_llseek`, `brk`,
`mmap2`, `munmap`, `mprotect`, `fstat64`, `fstatat64`, `getdents64`,
`set_thread_area`, `set_tid_address`, `uname`, `access`, `unlink`, `ioctl`,
`getpid`, `getuid`, `getgid`, `geteuid`, `getegid`, `time`, `gettimeofday`,
`nanosleep`, `clock_gettime`. This is step 1 of a longer Linux-ABI roadmap
(fork/waitpid/execve, signals, TTY ioctls are separate future plans).

## Scope

Delivers Linux ELF detection and loading, a per-task ABI flag, the Linux i386
syscall dispatch + fd layer, ramdisk growth to 1 MB, and an E2E test. The AOS
path (`elf_load`, AOS syscall table, fixed 12 MB user window) is unchanged.
No fork, signals, execve, tty termios, or dynamic (shared-lib) linking.

## ABI detection (`kernel/elf.c`, `kernel/task.h`)

`struct task` gains `enum task_abi abi;` with `ABI_AOS` / `ABI_LINUX`.

Detection by ELF properties alone (no name convention):

- **ET_EXEC** with entry `>= 0x01100000` → Linux (AOS programs link at
  `0x01000000`, their entry is always inside the AOS window).
- **ET_DYN (PIE)** → Linux (AOS never emits PIE).

A small probe reads the ELF header (already available: `elf_load` reads
`buf[4096]` first) and decides which loader to use. `elf_load()` keeps its
exact current behavior for AOS binaries.

## Memory model

### Spawned Linux tasks (via `SYS_SPAWN`)

New loader `elf_load_linux()` in `kernel/elf.c`:

- Same magic/arch/machine checks (`elf.c:28-30`); no AOS-window entry check.
- Loads PT_LOAD at their own vaddrs (ET_EXEC base `0x08048000`, PIE at fixed
  base `0x08048000` without ASLR).
- Sets up the Linux ABI stack: `argc`, `argv[]`, `NULL`, `envp[]`, `NULL`,
  `auxv[]` with `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_BASE`,
  `AT_ENTRY`, `AT_UID/GID/EUID/EGID`, `AT_RANDOM`, `AT_EXECFN`.

`task_spawn()` branches on the probed ABI:

- AOS: unchanged (3 PTs, 12 MB window, stack `0x01804000`).
- Linux: clone kernel PD (existing `task.c:193-195`), then **override PDEs
  32..63** (window `0x08000000..0x10000000`) with 32 per-task PTs allocated at
  spawn via `page_alloc_zero()` (32 frames/task, e.g. 768 frames for 24 tasks).
  PTEs start non-present; pages are mapped on demand by the loader and the
  `brk`/`mmap2`/stack syscalls through a new `map_user_page(pd, vaddr)`
  helper in `kernel/paging.c`.
- Window layout: ELF segments from `0x08048000`, `brk` grows upward from after
  the last segment, mmap region grows down from below the stack, stack sits at
  the top of the window (e.g. `0x0FE00000..0x10000000`).
- `task_free_addrspace()` (`task.c:66`) is generalized to walk the ABI's PT
  range and return all mapped frames + PT pages.

Note: `0x08000000..0x10000000` falls inside the kernel's 0..256 MB identity
map. The task's cloned PD simply overrides those PDEs with its own PTs; the
kernel keeps running on its own PD, so nothing kernel-side changes.

### Task 0 (shell) execution

The shell runs programs synchronously on the kernel PD via
`user_program_start` (`kernel/commands.c:28-38`, `kernel/progload.c:40`).
Linux binaries need `0x08048000`, which in the kernel PD is identity-mapped to
physical RAM the buddy could otherwise hand out. So for task 0:

- Reserve a fixed physical window `[0x08000000, 0x08800000)` (8 MB) in
  `pmm_init` (`kernel/pmm.c`) — identity-mapped already. Its PDEs/PTEs in the
  kernel page directory must be marked `PTE_USER` (paging_init only marks
  PD 4..6 and 12..15 today, `kernel/paging.c:33-46`), and the 8 MB range is
  excluded from the buddy allocator. pmm never hands those frames out, so
  task-0 Linux programs cannot collide with other allocations.
- `program_load()` probes the ABI and dispatches to `elf_load_linux()`, which
  writes straight into the identity-mapped window (no CR3 switch needed).
- Before launch: set `tasks[0].abi = ABI_LINUX`, allocate `linux_ctx` for task
  0, and launch with the Linux stack top instead of the hardcoded
  `0x01804000` — `user_tramp.S` gets a variant that pushes a given user ESP.
- Linux `exit_group` in task 0 goes through the existing pid-0 exit path
  (`user_program_exit`, `syscall.c:226-229`); reset `abi` to `ABI_AOS`.
- The AOS 12 MB window and Linux 8 MB window are disjoint, so AOS and Linux
  programs can alternate in task 0 freely.

## Syscall dispatch and fd layer (`kernel/linux_syscall.c`)

`syscall_handler()` (`kernel/syscall.c:130`) branches on `task_current_abi()`:
Linux → `linux_syscall_handler(r)`; the AOS switch is untouched. No changes to
`struct registers` or `boot/isr.S` — Linux i386 uses `eax`=number,
`ebx,ecx,edx,esi,edi` args (6th arg in `ebp`), `-errno` return in `eax`. This
matches the AOS ABI (libaos.c:19-28); `pusha` (boot/isr.S:56) already captures
`ebp`, so `r->ebp` is available for `mmap2`'s offset arg.

`struct task` gains `void *linux_ctx` (kmalloc'd at Linux spawn / first task-0
Linux launch):

```c
struct linux_fd { int used; char name[64]; unsigned int off; unsigned int size; };
struct linux_ctx {
    struct linux_fd fds[32];      // fixed table, no dynamic allocation
    unsigned int brk_cur;         // current break
    unsigned int mmap_cur;        // top of mapped region (for munmap)
    unsigned int stack_top;       // Linux user stack top
    struct user_desc tls;         // last set_thread_area request (see TLS below)
    unsigned int tls_selector;    // computed LDT selector (entry*8+3)
};
```

- fd 0/1/2: `write(1,..)`/`write(2,..)` → `route_text()` (same path as AOS
  `print`, `syscall.c:71`); `read(0,..)` returns `-EAGAIN` (step 1).
- `open`/`openat`: strip leading `/`, match flat SFS name, allocate fd slot.
  `read`/`write`/`lseek`/`_llseek`/`close`/`fstat64` work on fd via
  `fs_read_at`/`fs_write`.
- `getdents64` wraps `fs_list_get()`.
- `brk`/`mmap2`/`munmap`/`mprotect` call `map_user_page`; `munmap` on step 1
  only tracks `mmap_cur` (no VMA list).
- Pointer validation: Linux `in_user()`/`copy_user_str()` check the Linux
  window constants of the current task instead of `USER_LO..USER_HI`.
- `exit`/`exit_group`: `task_current_pid()==0 ? user_program_exit() :
  task_exit_current()` (mirrors `syscall.c:225-230`).
- Returns use `-errno` (`open` miss → `-2`, etc.).

### TLS via `set_thread_area` — a real LDT (not a stub)

musl i386 needs working TLS, and it uses **LDT selectors** (TI bit set):
`__set_thread_area` (disassembled from the actual toolchain, GCC 11.2 musl)
does `lea 0x3(,%edx,8),%edx; mov %edx,%gs` — i.e. `selector = entry*8+3`, an
LDT selector with RPL 3. After that the whole libc dereferences `%gs:0x10`
(pthread-pointer) and `%gs:0x0` (errno area) on every libc call. So the
"store the entry and return 0" stub from the earlier draft would #GP on the
first `mov %gs`.

Plan:

- `set_thread_area` (243) gets a `struct user_desc *` argument
  (kernel/asm/ldt.h layout: `entry_number`, `base_addr`, `limit`,
  `seg_32bit`, `contents`, `read_exec_only`, `limit_in_pages`,
  `seg_not_present`, `useable`). The kernel copies it into `linux_ctx.tls`,
  installs a real descriptor in a small **LDT** so the selector actually
  works, and returns `entry_number` (musl then loads `%gs` itself).
- `modify_ldt` (123) is musl's fallback path (`__set_thread_area` tries 243,
  then 123). Implement it the same way (build a `struct user_desc` from the
  `modify_ldt` args and reuse the set_thread_area path).
- The LDT lives in `arch/i386/gdt.c` beside the GDT: a small static array
  (e.g. `ldt[16]`, one entry is enough for one task's TLS in step 1) loaded
  with `lldt`. Since only the **current** task runs at any moment, a single
  shared LDT is updated on task switch: `task_switch_kernel` copies
  `tasks[next].linux_ctx->tls` into the LDT entry and re-runs `lldt` when
  switching to a task that has TLS. (Kernel PD vs task PD is unrelated —
  LDT is per-CPU, not per-address-space.)
- `set_tid_address` (258): called by `__init_tp` unconditionally (musl stores
  the tid pointer). Return `0`; store the tid pointer in `linux_ctx`.
- `arch/i386/gdt.c` gains an `ldt_init()`/`ldt_set_tls()` helper; called from
  `gdt_init` and from `task_switch_kernel`.

Note on numbering: the spec earlier listed `lseek` (19), `newfstatat` (291).
The actual i686 musl toolchain (`include/bits/syscall.h`) uses
`_llseek` (140), `fstat64` (197), `fstatat64` (300), `stat64` (195),
`set_tid_address` (258). The plan supports **both** `lseek` and `_llseek`
(19 and 140) so host-built binaries of either libc convention work; `_llseek`
is the one musl actually issues.

## Ramdisk growth and binary delivery

- `kernel/sfs.c:5`: `FS_SIZE` 160 KB → **1 MB**.
- `kernel/pmm.c:215`: `reserve(0x00200000, 0x00200000 + 1MB)` (ramdisk stays at
  `0x200000`; next occupied region is `FB_STAGE_ADDR 0x00C00000`, far below —
  no relocation needed). Update the stale comment in `drivers/vga.c:57`.
- `Makefile`: extend the `gen_progs.py --data` invocation with the Linux
  binaries. User-provided static musl binaries live in `tools/linux/`:
  `--data lin/hello=tools/linux/hello --data lin/ls=tools/linux/ls
  --data lin/cat=tools/linux/cat`.
- `gen_progs.py` already supports arbitrary `--data name=path` (used for
  `demo.ico`); `load_embedded_data()` (`kernel/progload.c:34`) writes them to
  SFS as `lin/<name>`. Verify `cmd_format` also reloads `embedded_data` (it
  currently reloads `embedded_progs`).
- PATH stays `"bin"`: Linux binaries are run by direct path (`lin/hello`,
  `lin/ls`), which the existing fallback search already supports
  (`commands.c:76-99`).
- `SFS_MAX_FILES` (64) is not a constraint: step 1 adds only 3 files.

## Testing

- New `scripts/linhello.py` (pattern: `scripts/manytest.py`):
  1. Boot the ISO under QEMU (`-m 256`), wait for the desktop.
  2. Run `lin/hello` (serial input → terminal, or spawn).
  3. Assert `Hello, Linux!` text appears (screendump text-pixel check) and no
     `KERNEL PANIC` in serial.
  4. Assert the task exits cleanly (pid reclaimed; process-count sanity) to
     prove `exit_group` works.
- `scripts/lincat.py`: create a file, `lin/cat <file>`, assert contents —
  validates `open`/`read`/`close`/`fstat64`.
- Manual in QEMU: `lin/hello`, `lin/ls` (getdents64 over SFS), `lin/cat`.
- Regression: `manytest.py`, `ipctest.py`, `notepadtest.py` stay green (AOS
  path untouched).

## Known risks / open items

- `set_thread_area` is implemented via a real LDT (one shared LDT, updated on
  task switch) — this is required, not optional: musl loads `%gs` with an LDT
  selector and dereferences it on every libc call.
- The exact musl binary may call syscalls beyond the table above (e.g.
  `fcntl`, `statfs`, `getdents` variant). The table grows by fact; that is
  expected and in scope.
- Binaries that probe TTY ioctls will get ENOTTY/`-EAGAIN` and must degrade
  gracefully (musl utilities do).

## Out of scope (future plans)

- `fork`, `waitpid`, `execve`, `dup`/`dup2`/`pipe`, `fcntl` (fd layer can host
  them later), signals, full `termios`/TTY ioctls, shared-library dynamic
  linking, ASLR.
- COW / demand paging for the Linux window.

## Acceptance criteria

1. `make` builds from clean; kernel stays `-nostdlib -ffreestanding`.
2. `lin/hello` prints `Hello, Linux!` and the task exits (pid reclaimed,
   no leak across repeated runs).
3. `lin/cat file` prints the file's contents; `lin/ls` lists SFS entries.
4. No `KERNEL PANIC` during any run; buddy never hands out the task-0 Linux
   window or the grown ramdisk.
5. `manytest.py`, `ipctest.py`, `notepadtest.py` all pass unmodified.

## Implemented (2026-08-04)

Status: **complete** — step 1 landed (commits `3e4f7d6`..`1e7201e`).

- ABI probe (`elf_probe`), loader (`elf_load_linux`), musl stack/auxv
  builder (`stack_build`), `elf_load_linux` with PT_LOAD mapping via
  `paging_map_user_page`.
- Task-0 8 MB window `0x08000000..0x08800000` identity-mapped (PDE 32–33),
  reserved in the buddy; spawned Linux tasks get a private
  `0x08000000..0x10000000` (32 MB) via `lpts[32]` page tables.
- `linux_ctx` fd/brk/mmap/stack/TLS runtime context; `linux_ctx_init` at
  spawn; `int 0x80` routed by task ABI to `linux_syscall_handler`.
- Full syscall table from the design (exit/exit_group, write, read,
  open/openat, close, unlink, lseek/_llseek, access, time, getpid,
  getuid/gid/euid/egid, ioctl(-ENOTTY), gettimeofday, uname, brk,
  mmap2 top-down, munmap/mprotect no-op, nanosleep (PIT-tick spin),
  clock_gettime, set_thread_area/modify_ldt via a real LDT
  (`ldt_set_tls`, GDT index 6), set_tid_address, stat64/fstatat64/fstat64,
  getdents64).
- Embedded `lin/hello`, `lin/ls`, `lin/cat`, `lin/test.txt`; `bin/linrun`
  spawns `lin/hello` as a real pid>0 task. `lin/*` hidden from the desktop
  icon grid (`wm.c refresh_files`, alongside `bin/`) to keep
  `notepadtest.py` icon positions stable.
- Regression: `linhello.py` PASS, `lincat.py` PASS, `manytest.py` PASS,
  `ipctest.py` PASS, `notepadtest.py` PASS.
