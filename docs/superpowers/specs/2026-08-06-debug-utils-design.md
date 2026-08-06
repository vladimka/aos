# Kernel log (dmesg) + strace — design

Date: 2026-08-06

## Problem

- **Kernel logging**: `kernel/printf.c` writes straight to VGA + COM1. There is
  no in-kernel log buffer, no levels, no timestamps, and no way to read kernel
  messages from userland after the fact. Debugging "what did the kernel do
  before it misbehaved" is limited to eyeballing the serial log live.
- **Syscall tracing**: there is no way to see what syscalls a program makes.
  GUI programs (`wm`, `term`, `notepad`) are hard to debug because their
  `send`/`fill`/`text` traffic is invisible; musl `lin/*` binaries make Linux
  ABI calls that are equally opaque.

## Goal

Two debug facilities, equally weighted:

1. **dmesg-style kernel log**: a ring buffer holding every kernel `printf`
   line tagged with a tick timestamp and a level, readable from userland via
   `/proc/klog` (`cat /proc/klog`).
2. **strace**: a shell built-in `strace <prog> [args]` that records every
   syscall (all three ABIs: AOS 0–48, AOS_EXT 500–519, Linux) made by the
   program and its children, prints the trace after the program exits, and
   offers a live dump of a long-running task via `/proc/<pid>/trace`.

## Scope

- `kernel/klog.c` (new): ring buffer + leveled writer. `kernel/printf.c`:
  `printf` also feeds the ring.
- `kernel/trace.c` (new): syscall-name tables + trace formatting.
- `kernel/task.c/h`: per-task `trace_on`, trace ring buffer, inheritance,
  free-on-exit/collect.
- `kernel/syscall.c`, `kernel/linux_syscall.c`, `kernel/aos_gui.c`: one record
  point per ABI in the respective handlers.
- `kernel/commands.c`: `strace` built-in (parse, set flag, run, collect,
  print).
- `kernel/procfs.c`: `/proc/klog` + dynamic `/proc/<pid>/trace`.
- `scripts/klogtest.py`, `scripts/stracetest.py`, `scripts/stracelive.py` (new).
- No changes to the GUI apps, the WM, or existing test scripts' assertions.

## Architecture

### 1. Kernel log ring buffer (`kernel/klog.c`)

- `#define KLOG_SIZE 32768` — 32 KB ring in BSS.
- Ring of **lines**: a message is fully accumulated (up to `KLOG_LINE_MAX`
  256 chars) before being committed, so a reader never sees a torn line.
- Each committed line is stored as:
  `[tttttttt] L text\n` where `tttttttt` is the low 32 bits of the global
  `tick` (PIT, 1000 Hz) as 8 hex digits, `L` is one of `I`/`W`/`E`
  (INFO/WARN/ERR), then the text and a trailing newline.
- Write position is a wrapping `u32` `klog_pos`; the ring holds the last
  `KLOG_SIZE` bytes written. A `klog_wrap` counter tracks how many times the
  ring has wrapped (used by the reader to compute the virtual size).
- Writer: `klog_putc(char)` accumulates into a static line buffer and flushes
  the complete line to the ring on `\n` (or when full). `printf.c` keeps its
  current char-by-char `putc()` to VGA + serial **unchanged** and additionally
  calls `klog_putc(c)` — so every existing `printf` becomes an INFO line with
  a timestamp automatically, with zero risk to VGA behaviour, and no
  call-site changes.
- Leveled variant: `void klog(int level, const char *fmt, ...)` — renders via
  a `printf`-engine in `klog.c` into the same line accumulator and flushes to
  COM1 **and** the ring, **not** VGA (WARN/ERR must not clutter the
  framebuffer; panics keep using `printf`). The public `printf` signature
  stays `int printf(const char *, ...)`.
- Levels: `KLOG_INFO 0`, `KLOG_WARN 1`, `KLOG_ERR 2`.

**Reader — `/proc/klog`** (`kernel/procfs.c`):
- New procfs inode `PROCFS_KLOG`, name `klog`.
- `proc_read_at` for this ino renders on demand from the ring for the
  requested `off` (the current 128-byte static `proc_content` path is
  **not** reused; klog gets its own branch). Virtual size = valid bytes
  written so far, capped at `KLOG_SIZE`. Non-destructive: reading does not
  clear the ring. `cat /proc/klog` therefore shows the last ≤32 KB of kernel
  messages.
- Wrapping handled inside `klog_read(off, buf, len)` (new function in
  `klog.c`): it computes which ring byte `off` maps to, handling wrap, and
  never reads more than one contiguous segment per ring boundary.
- `proc_stat` reports `st->size` = virtual size so `cat`/`wc -c` behave.

### 2. strace — per-task trace state (`kernel/task.h`)

Add to `struct task`:

```c
unsigned int trace_on;        /* 1 = record this task's syscalls */
unsigned char *trace_buf;     /* kmalloc'd ring of trace records, lazy */
unsigned int trace_head;      /* next write slot (index, wrapping) */
unsigned int trace_count;     /* records written so far */
unsigned int trace_wrapped;   /* >0 if the ring overwrote old records */
unsigned int trace_dumped;    /* 1 = already printed by a strace session */
```

- Record layout (`kernel/trace.h`):

```c
struct trace_rec {
    unsigned int num;   /* syscall number (AOS / AOS_EXT / Linux) */
    unsigned int a0, a1, a2, a3, a4;  /* ebx, ecx, edx, esi, edi */
    unsigned int ret;   /* eax on exit */
};                       /* 28 bytes */
```

- Ring capacity `TRACE_MAX 512` records (~14 KB). On overflow the oldest
  record is overwritten and `trace_wrapped` is set.
- Buffer allocated lazily on the first traced syscall (`kmalloc(512 * 28)`);
  on allocation failure the task's `trace_on` is cleared (silent disable).
- **Inheritance**: `task_spawn` (`kernel/task.c`) copies
  `trace_on` from the parent task (`t->trace_on = current_task->trace_on`)
  — this is the `strace -f` behaviour. Grandchildren inherit transitively.
- **Exit**: in the task-exit path, free `trace_buf` **only if**
  `trace_dumped`; if the task died with `trace_on && !trace_dumped`, keep the
  buffer (the zombie slot holds it) so the strace session can collect it.
- **Slot reclamation**: when `task_spawn` reuses a zombie/free slot, free any
  leftover `trace_buf` that was never collected.

**Record points** — one per ABI, all funnel to `trace_record(r)`:

```c
/* kernel/trace.c */
void trace_record(struct registers *r);      /* entry: captures args */
void trace_finish(struct registers *r);      /* exit: stores ret */
```

- `kernel/syscall.c::syscall_handler`: after the ABI dispatch decides which
  path, the AOS path calls `trace_record(r)` at entry (if
  `current_task->trace_on`) and `trace_finish(r)` at every exit/return path.
  Simplest correct placement: a wrapper around the whole `switch`, recording
  at entry and storing the return at the end of the handler.
- `kernel/linux_syscall.c::linux_syscall_handler`: same wrapping for Linux
  ABI tasks.
- `kernel/aos_gui.c::aos_gui_handler`: same wrapping for AOS_EXT (500–599).

Arguments captured from `r->ebx, r->ecx, r->edx, r->esi, r->edi` (the
registers struct already saves these; `struct registers` layout confirmed in
`kernel/interrupts.h`). The return is the final `r->eax`.

### 3. strace — shell built-in (`kernel/commands.c`)

The AOS shell executes commands **in-place in task 0** (`user_program_start`
`kernel/user.c`), so the traced program is the shell task itself; children
spawned by the program are real tasks and inherit `trace_on`.

`strace <prog> [args]`:
1. Parse `prog` + args like the normal command path.
2. Set `current_task->trace_on = 1` (the shell task; its trace buffer is the
   program's).
3. Load and run the program in-place exactly as `commands_execute` does.
4. On return (program exited back into the shell):
   - print the shell task's own trace (header `== pid 0 ==`);
   - walk the task table, print every task with
     `trace_on && !trace_dumped`, in pid order, header `== pid N ==`;
   - for each printed task mark `trace_dumped` and `kfree` its buffer;
   - clear the shell task's own `trace_on`, free its buffer;
   - print a `... N records overwritten` line when any printed ring wrapped.
5. If the program was not found / failed to load, print the normal error and
   clear `trace_on`.

**Output format** (per record):

```
open(0x1001f40, 0x0, 0x0, 0x0, 0x0) = 0x3
```

- Syscall name from the ABI table; `nargs` columns printed, hex lowercase,
  return as `= 0x..`.
- Unknown numbers print `syscall_0x%x`.

**Name tables** (`kernel/trace.c`), each `{ name, nargs }`:
- AOS 0–48 (`kernel/syscall.h` names: `print`, `open`, `send`, `spawn`, ...).
- AOS_EXT 500–519 (`aosabi.h`: `fb_info`, `text`, `fill`, `mouse`,
  `send`, `recv`, `spawn`, ...).
- Linux subset implemented in `kernel/linux_syscall.c` (~40: `write`,
  `open`, `read`, `mmap2`, `ioctl`, `brk`, ...). The table is indexed by the
  Linux syscall number used as the record's `num` (the record stores the raw
  number; the ABI that produced it is known from `task_current_abi()`).

**Live dump — `/proc/<pid>/trace`** (`kernel/procfs.c`):
- procfs gains dynamic entries. Root `readdir` returns the static files
  (`uptime`, `version`, `mounts`, `klog`) followed by one pseudo-directory
  per live task (`3`, `4`, ...). `lookup("3")` from the root returns a pid-dir
  inode; `lookup("trace")` inside it returns a trace inode.
- `proc_read_at` on a trace inode renders a snapshot of that task's ring
  (formatted the same as the shell output, including the `== pid N ==` header
  and the wrapped-overwrite line), under a `cli()`/lock so the reader sees a
  consistent ring even while the task writes.
- Unknown pid → `VFS_ENOENT`; pid without `trace_on`/buffer → empty file.
- Reading is non-destructive and does not set `trace_dumped`.

## Data flow

```
printf() ──► vga + com1 + klog ring ──► /proc/klog ──► cat /proc/klog

int 0x80 ──► syscall_handler / linux_syscall_handler / aos_gui_handler
              ├─ trace_record(r)   (entry, if current->trace_on)
              ├─ ... syscall body ...
              └─ trace_finish(r)   (exit, stores ret)
                      │
                      ▼
               task->trace_buf (ring of trace_rec)
                      │
              ┌───────┴───────────────┐
              ▼                        ▼
   `strace` shell built-in        /proc/<pid>/trace
   (prints after exit,            (live snapshot of a
    collects children)             long-running task)
```

## Error handling

- `kmalloc` failure for a trace buffer → `trace_on` cleared for that task,
  tracing silently disabled.
- Reading `/proc/klog` at `off >= virtual size` → 0 bytes (normal EOF).
- `/proc/<pid>/trace`: unknown pid → `VFS_ENOENT`; untraced pid → empty file.
- Ring overflows: klog overwrites oldest lines (no error surfaced, virtual
  size caps at `KLOG_SIZE`); trace records overwrite oldest and the print
  appends `... N records overwritten`.
- `strace` with no args → usage line; unloadable program → the existing
  "Failed to load" message, `trace_on` cleared.
- One strace session at a time: implied by the sequential shell; no global
  session flag needed.

## Testing

Scripts are written by the assistant; QEMU runs are handed to the human
(see AGENTS.md workflow convention).

- `scripts/klogtest.py`: boot, `cat /proc/klog` from the shell, assert the
  AOS banner / `GDT initialized` / `PMM` lines appear with `[` timestamp and
  `I` level markers.
- `scripts/stracetest.py`: boot, run `strace cat lin/test.txt` (or
  `strace ls`) from the shell, assert `== pid N ==` headers and syscall lines
  for `open`/`read`/`write`, plus the final `= 0x..` return values.
- `scripts/stracelive.py`: boot (WM up), `strace term` from the shell,
  then read `/proc/<pid>/trace` for the live term task and assert AOS_EXT
  names (`fill`, `text`, `send`) appear.
- Regression: full `make test` (manytest, ipctest, notepadtest, sleeptest,
  configtest, lin*, ...) must stay green — no changes touch the GUI apps, the
  WM, or existing assertions.
- Build: `make` clean, no new `-Wall -Wextra` warnings.

## Out of scope

- No filtering by syscall name/type in `strace` (all syscalls are recorded).
- No dereferencing of pointer arguments (numeric args only, per design
  decision).
- No VGA output for WARN/ERR klog lines.
- No `dmesg -c` (read-clear) semantics; `/proc/klog` is non-destructive.
- No ptrace, no attaching to an already-running untraced task.
