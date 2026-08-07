# Shell reliability: background `&`, `$?`, env vars, backtrace symbols — design

Date: 2026-08-07

## Problem

Three independent P0 gaps (TODO.md §2.1 and §1.6):

1. **No background jobs**: every command runs in-place in task 0 and the shell
   blocks until it exits. Long-running programs (`sleep`, `lin/hello` loops)
   freeze the prompt. There is no `$?` — the shell does not know the exit code
   of the last command, and both in-place exit paths (`SYS_EXIT` pid 0 and
   musl `exit_group` pid 0) discard the code today.
2. **No environment variables**: `export VAR=val` does not exist, and `$VAR`
   is not substituted anywhere in command lines.
3. **Anonymous backtraces**: a kernel panic prints `eip=0x...` addresses only
   (TODO §1.6: «сопоставить с System.map/адресами символов»). Nothing maps
   them to function names, so reading a crash dump requires an offline `nm`.

## Goal

1. Shell supports trailing `&` — run the command as a real spawned task
   without blocking, including `>`/`>>`/`<` redirects; `$?` expands to the
   exit status of the last command.
2. Shell supports `export NAME=value` (and `export` alone to list) plus
   `$NAME` / `$?` expansion in command lines.
3. Kernel panic backtraces resolve EIPs to function names via an embedded,
   build-generated symbol table.

All three are kernel/shell-side only. No GUI app, WM, or Linux-ABI changes.

## Scope

- `kernel/commands.c`: `&` parsing, `run_bg()`, `export` builtin,
  `shell_expand()`, `shell_status` global + accessors, and wiring a spawned
  child's `stdout_fd`/`stdin_fd`/`fds[]` for bg redirects (the fd model in
  `kernel/task.c`/`vfs.c` already supports it; only the caller is new).
- `kernel/syscall.c`: capture exit code in the `SYS_EXIT` pid-0 branch.
- `kernel/linux_syscall.c`: capture exit code in `linux_exit()` pid-0 branch.
- `kernel/bt.c`: symbol lookup; `kernel/symtab.c` (generated) +
  `scripts/gen_symtab.py` + Makefile two-pass link.
- `scripts/shelltest.py`, `scripts/panictest.py` (new) + Makefile `TESTS`.
- No changes to the GUI apps, the WM, existing test scripts' assertions, or
  the Linux ABI.

## Architecture

### 1. Background `&` and `$?` (`kernel/commands.c`)

**Parsing.** `commands_execute(line)` gains a bg path:

1. Strip leading spaces. Copy `line` into a local `expanded[LINE_BUF_SIZE]`
   via `shell_expand()` (see §2).
2. Detect a trailing `&`: scan from the end of `expanded`, skipping spaces;
   if the last non-space char is `&`, truncate it there and set `bg = 1`.
   `foo &` and `foo&` both work. An `&` mid-line is left alone (no `&&` —
   that is the P1 parser).
3. If `bg`:
   - no operator (`|`/`>`/`<`) → `run_bg_simple(expanded)`;
   - `>` / `>>` / `<` → `run_bg_redirect(expanded, op)`;
   - `|` → `terminal_print("bg: pipes not supported")` (two-task pipes are
     P1; foreground `|` keeps working unchanged);
   - builtins (`export`, `cd`, `pwd`, `setpath`, `format`, `strace`) run
     inline as today — `&` is ignored for builtins.

**`run_bg_simple(cmdline)`.** Parse the first token as the program name and
the rest as args (same slicing as `run_command_raw`). Resolve the program in
PATH with a new helper `path_resolve(cmd, out, outsz)` factored out of the
existing `exec_from_path` search loop (both callers share it). Spawn with
`task_spawn(full_path, arg, 0, &pid)`; on success set
`shell_set_status(0)` and print `bg: pid N`; on not-found/failure set
`shell_set_status(127)` and print the normal "Unknown command"/"Failed to
load" error. The shell does **not** wait.

**`run_bg_redirect(cmdline, op)`.** Mirror the foreground `exec_stage`
redirect code, but spawn instead of exec-in-place:

1. Split left/right at the operator (reuse the existing `find_operator` /
   token-slice logic). `right` is the file name.
2. Open the file with `vfs_open_fd(current_task_cwd(), fn, flags)`
   (`VFS_O_WRONLY|VFS_O_CREAT` + `VFS_O_APPEND` for `>>`, `VFS_O_TRUNC`
   for `>`, `VFS_O_RDONLY` for `<`). On failure print the existing
   "redirect: cannot open <fn>" and abort.
3. Resolve + spawn the left side as in `run_bg_simple`.
4. Wire the child: `struct task *c = task_slot(pid);
   c->fds[fd] = vfs_ofile_ptr(fd); c->stdout_fd = fd;` (`>`/`>>`) or
   `c->stdin_fd = fd;` (`<`).
5. Set `shell_set_status(0)` and print `bg: pid N`.

Why this is safe and leak-free: `vfs_open_fd` allocates the descriptor in the
global `ofiles[]` table but does **not** set task 0's per-task `fds[fd]`
(the syscall layer does that; here the kernel calls it directly). The shell
spawn + wiring runs inside the timer IRQ (serial command), so the child
cannot be scheduled before `stdout_fd`/`stdin_fd` are set. On child exit
`task_close_fds` closes the descriptor and clears the child's `fds[fd]`;
task 0 never claimed it, so there is no double-close and no leak. Output
routing already consults `t->stdout_fd` (`route_text`, syscall.c:79) and
`terminal_read_key` consults `t->stdin_fd` (terminal.c:36), so `>`/`<` in a
spawned task work with zero new syscall code.

**`$?` status plumbing.** New global + accessors in `commands.c`
(declared in `commands.h`):

```c
int shell_status(void);
void shell_set_status(int code);
```

`$?` is captured at the two places where an in-place (pid 0) program exits —
both currently discard the code:

- `kernel/syscall.c` `case SYS_EXIT` pid-0 branch (syscall.c:402):
  `shell_set_status(r->ebx);` before `user_program_exit()`.
- `kernel/linux_syscall.c` `linux_exit()` pid-0 branch (linux_syscall.c:50):
  `shell_set_status(code);` before `user_program_exit()`. This is the path
  real musl programs take (`exit_group` 252).

Semantics: after a foreground program, `$?` = its exit code. After a
successful `&` launch, `$?` = 0; after a failed one, 127. Builtins leave
`$?` unchanged. Initial value 0.

### 2. Environment variables (`kernel/commands.c`)

**Storage.** Static table, global in `commands.c` (the shell is task 0;
per-task env is TODO §1.2 P1 and lives elsewhere later):

```c
#define ENV_MAX 16
static struct { char name[24]; char val[64]; } shell_env[ENV_MAX];
```

**`export` builtin** (new branch in `run_command_raw` before `exec_from_path`):

- `export` (no arg) → print every set entry as `NAME=value`, one per line.
- `export NAME=value` → set (overwrite existing, or use the first free slot;
  table full → error `export: env full`).
- `export NAME` (no `=`) → set `NAME` to the empty string.
- Empty `NAME` → ignore.

**Expansion — `shell_expand(const char *in, char *out)`** applied at the top
of `commands_execute` (before operator parsing), so args, redirect targets,
and the command name all get it:

- Walk `in`; on `$`:
  - `$?` → decimal rendering of `shell_status()` (via the existing
    `terminal_print_dec`-style conversion into the out buffer);
  - `$NAME` (NAME = `[A-Za-z0-9_]+`) → the stored value, or the empty string
    if unset;
  - `$` not followed by a valid name char → literal `$`.
- Everything else copied verbatim. Output truncated to
  `LINE_BUF_SIZE - 1`. No quote handling (P1 parser).

### 3. Backtrace symbols (`scripts/gen_symtab.py`, `kernel/bt.c`, Makefile)

**Generated table.** `scripts/gen_symtab.py` runs `nm -n` on the freshly
linked `aos.elf`, keeps only `T`/`t` (text) symbols with address in
`[0x100000, _end)`, and emits `kernel/symtab.c`:

```c
/* generated by scripts/gen_symtab.py — do not edit */
struct symtab_entry { unsigned int addr; const char *name; };
const struct symtab_entry kernel_symtab[] = {
    {0x00100000, "start"},
    ...
};
const unsigned int kernel_symtab_count = <N>;
```

A stub `kernel/symtab.c` (empty table, `kernel_symtab_count = 0`) is
committed so a clean checkout links without a generated file present.

**Build — two-pass link** (Makefile, `aos.elf` recipe):

```
aos.elf: $(KERNEL_OBJS) linker.ld scripts/gen_symtab.py
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)
	$(PYTHON) scripts/gen_symtab.py $@ kernel/symtab.c
	$(CC) $(CFLAGS) -c -o kernel/symtab.o kernel/symtab.c
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)
```

`kernel/symtab.o` is appended **last** in `KERNEL_OBJS`, so its
`.text`/`.rodata` land after every other object and the addresses of all
other symbols are identical between pass 1 (used for `nm`) and pass 2 (the
image that runs) — the generated table stays exact. `make clean` keeps the
stub (it is the build bootstrap; like `kernel/progs.c` it is a generated file
never edited by hand). The recompile of `symtab.o` inside the recipe keeps
the embedded table in sync with the current objects on every `make`.

**Lookup** (`kernel/bt.c`):

```c
const char *addr_to_sym(unsigned int eip, unsigned int *off);
```

Binary search for the largest `addr <= eip` in `kernel_symtab`; returns the
name and `*off = eip - addr`, or NULL. `backtrace()` prints, for a hit,
`  [%d] eip=0x%08x  <name>+0x%x`; otherwise the current `eip=0x...` line.
EIPs outside `[0x100000, _end)` (user-mode panics) never match and print
raw, which is correct — the table only knows kernel symbols. `backtrace`'s
existing `[0x100000, _end)` bound keeps it from chasing user stacks.

## Data flow

```
int 0x80 exit/exit_group (pid 0) ──► shell_set_status(code) ──► $? ──► shell_expand
                                                                       │
AOS> export FOO=bar ──► shell_env ──────────────────────────────────────┘
AOS> echo $FOO / $?   ──► expand ──► echo program with substituted args
AOS> prog args &      ──► run_bg ──► task_spawn ──► child (bg: pid N)
AOS> prog > f &       ──► run_bg_redirect ──► child stdout_fd = f

aos.elf ──► nm ──► kernel/symtab.c ──► kernel_symtab[] ──► backtrace() ──► "eip=0x..  func+0x.."
```

## Error handling

- `&` with pipes → printed error, nothing spawned.
- `&` where the program is not found → normal "Unknown command" /
  "Failed to load", `$?` = 127.
- Redirect open failure in bg → "redirect: cannot open <fn>", nothing
  spawned.
- `export` table full → `export: env full`.
- `$NAME` unset → empty string (no error).
- `kernel_symtab` empty (no toolchain, stub) → `addr_to_sym` returns NULL,
  backtrace prints raw addresses as before — graceful degradation.
- Spawn failure (no task slot) → `bg: spawn failed (rc=N)`, `$?` = 1.

## Testing

Scripts written by the assistant; QEMU runs handed to the human
(AGENTS.md workflow convention). Both tests follow the existing
serial-burst pattern (scripts/cwdtest.py, scripts/stracetest.py): connect to
the serial socket at boot, queue the whole command burst the moment `AOS>`
appears, before the WM registers as the event consumer and captures serial
input.

- `scripts/shelltest.py` — one burst, asserts on the serial log:
  - `export FOO=bar` then `echo $FOO` → `bar`;
  - `lin/hello` then `echo $?` → `0`;
  - `exitto` then `echo $?` → `7` (`exitto` is `_exit(7)`, so the status is
    deterministic; `cat /nonexistent` returns 0 and cannot be used here);
  - `uptime &` then `echo $?` → `0` (spawn status), plus a `bg: pid`
    line and the spawned task's `uptime` output appearing;
  - `echo hi > /bg.txt &` then `cat /bg.txt` then `cat /bg.txt` again
    → `hi` appears (redirect to a background task lands in the file). The
    second `cat` runs after another foreground command, giving the bg echo
    time to finish; an empty early `cat` prints nothing, so it cannot false-
    positive. Assertions check the accumulated log, not the last chunk.
  - No `KERNEL PANIC` in the log.
- `scripts/panictest.py` — burst runs `panic`; asserts the serial log
  contains `KERNEL PANIC` and a resolved frame line, e.g.
  `eip=0x` … `+0x` with a known kernel function name (`isr_handler`,
  `backtrace`, or `kernel_main`). The kernel is expected to halt after
  (cli;hlt) — the test reads the log then terminates QEMU.
- Regression: full `make test` (ipctest, manytest, notepadtest, sleeptest,
  rngtest, blktest, virtiotest, netlooptest, rtctest, configtest, klogtest,
  stracetest, stracelive, linhello, lincat, lindirtest) stays green — no
  GUI/WM/assertion changes.
- Build: `make` clean; two-pass link produces a bootable `aos.iso` with no
  new `-Wall -Wextra` warnings; `make` from a clean checkout works with only
  the committed stub present.

## Out of scope

- No `wait`, `$!`, job control, or background pipeline (`a | b &`) — the
  last prints an error.
- No `unset`, no quoting, no `&&`/`||`/`;` (TODO §2.1 P1 parser).
- No env inheritance into spawned programs (per-task env is TODO §1.2 P1).
- No DWARF/System.map at runtime; the symbol table is baked into the kernel.
- No user-space symbol resolution in backtraces (user EIPs print raw).
