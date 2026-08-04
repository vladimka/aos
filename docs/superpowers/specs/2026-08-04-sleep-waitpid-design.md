# AOS — sleep, waitpid, get_children (kernel processes, TODO 1.2 P0)

Date: 2026-08-04

## Goal

Implement the TODO §1.2 P0 items:

- **`sleep(ms)` / wake-time in TCB**: a task can block for a duration; the
  scheduler skips tasks whose wake time has not yet arrived and wakes them
  (by `tick` threshold) instead of letting them burn round-robin slots.
- **`waitpid` / exit status**: a task can block until a child task exits and
  retrieve its exit code; exit codes are retained after a task exits.
- **`get_children`**: list the pids of a task's live (and zombie) children.

This is one cycle of the larger "углубить ядро" direction (§1.1–1.4). Other
subsystems (VFS, ATA, mmap, user heap) stay out of scope and remain separate
spec→plan→impl cycles.

## Current state (baseline)

- Scheduler: `task_switch_kernel()` in `kernel/task.c`. States are only
  `TASK_FREE/READY/RUNNING`. On every syscall/IRQ, the current task's `esp` is
  saved, its state forced to `TASK_READY` (task.c:121), and the scheduler scans
  the ring from `(current+1)%MAX_TASKS` for the first `TASK_READY` task. If none
  found, it falls back to `next = current_task` (task.c:144) — i.e. the current
  task keeps running.
- Task table: `static struct task tasks[MAX_TASKS]` (`MAX_TASKS=24`), resources
  kmalloc'd at spawn, freed at exit. On exit, the exit path (task.c:124–137)
  marks the task `TASK_FREE`, publishes `MSG_EXIT` to its sink (and event pid),
  then frees its address space/mbox/args/lctx (task.c:167–177). Its kstack is
  freed lazily via the zombie list (task.c:34–38) because `switch_and_restore`
  (`boot/isr.S:141`) restores `%esp` only after `task_switch_kernel` returns.
- Task 0 = kernel idle context (the main shell loop, `hlt` at kernel.c:135).
  It hosts single-user programs (`user_program_start`); its `kstack` is the
  shared static `sys_stack` and is never owned/freed by task.c. It is always
  `TASK_RUNNING` when idle, so it is a natural "no-op runnable" fallback.
- Syscalls: `int 0x80`, dispatch in `syscall_handler` (kernel/syscall.c).
  Numbers 0–29 defined in `kernel/syscall.h`; R/O user wrappers in
  `programs/libaos.{c,h}`. `SYS_EXIT` (16) currently takes no code.
- Blocking precedent: `read_key()` blocks in **userland** (spin + `yield()`,
  libaos.c:59) because `SYS_READ_KEY` is non-blocking. No kernel-side blocking
  currently exists for user tasks. (AGENTS.md's "SYS_READ_KEY blocks via
  sti;hlt;cli in the kernel" is stale — the kernel version is non-blocking.)

## Design

### 1. TCB changes (`kernel/task.h`)

Add states:

```c
#define TASK_SLEEPING 3   // blocked until tick >= wake_tick
#define TASK_WAITING  4   // blocked until child (wait_pid) exits
#define TASK_ZOMBIE   5   // exited, exit_code retained, awaiting reaping
```

Add fields to `struct task`:

```c
unsigned int wake_tick;   // TASK_SLEEPING: wake when tick >= wake_tick
unsigned int wait_pid;    // TASK_WAITING: child pid being waited on
unsigned int exit_code;   // TASK_ZOMBIE: exit code to hand to waitpid
unsigned int parent;      // pid that spawned this task (0 = kernel)
```

`wake_tick` comparison must be wrap-safe (tick is `unsigned int` at 1000 Hz):
use `(int)(tick - wake_tick) >= 0` to test "wake time reached".

### 2. `sleep(ms)` — scheduler-side blocking

New syscall `SYS_SLEEP` (30). Handler (kernel/syscall.c):

```c
case SYS_SLEEP:
    current_task->wake_tick = tick + (r->ebx & 0x7FFFFFFF); // clamp ms
    current_task->state = TASK_SLEEPING;
    while ((int)(tick - current_task->wake_tick) < 0) {
        __asm__ volatile("sti; hlt; cli");
    }
    current_task->wake_tick = 0;
    r->eax = 0;
    break;
```

The handler runs on the task's own kernel stack and blocks there with the
documented `sti; hlt; cli` pattern (AGENTS.md, read_key precedent): each timer
IRQ re-enters `task_switch_kernel` via `switch_and_restore`, which may switch to
other tasks; when this task is resumed, the loop re-checks `tick`. The task
cannot return to ring 3 until the handler returns, so `sleep` is atomic from
the program's perspective.

Scheduler changes in `task_switch_kernel`:

- **Save rule** (task.c:120–121): only force `TASK_READY` if the task is not
  blocked, so a blocked task is skipped by the round-robin scan instead of
  being picked for work:
  ```c
  current_task->kernel_esp = cur_esp;
  if (current_task->state != TASK_SLEEPING &&
      current_task->state != TASK_WAITING)
      current_task->state = TASK_READY;
  ```
- **Promote-then-scan** (task.c:140–143): before checking readiness, promote
  tasks whose block condition has resolved:
  ```c
  struct task *t = &tasks[(current_task->pid + i) % MAX_TASKS];
  if (t->state == TASK_SLEEPING && (int)(tick - t->wake_tick) >= 0) {
      t->state = TASK_READY;
      t->wake_tick = 0;
  }
  if (t->state == TASK_WAITING && task_done(t->wait_pid)) {
      t->state = TASK_READY;
      t->wait_pid = 0;
  }
  if (t->state == TASK_READY) { next = t; break; }
  ```
- **All-blocked fallback** (task.c:144): when no task is READY, the existing
  `next = current_task` fallback is now safe. If the current task is the one
  that called sleep/waitpid, resuming it just iret's back into its own handler
  wait loop, which re-checks and hlts again. If the current task is the idle
  loop (task 0) and some other task is blocked, the scheduler returns to the
  idle `hlt` (kernel.c:135) and the next timer IRQ re-promotes the sleeper.
  **No `sti;hlt;cli` wait inside `task_switch_kernel` itself** — the timer IRQ
  that would wake such a wait also runs the scheduler (via `switch_and_restore`),
  which would re-enter the wait and overwrite `kernel_esp` while an outer
  scheduler frame is still on the stack (the zombie-stack hazard). The wait
  always lives in the syscall handler, never in the scheduler.
- **Zombie bookkeeping** stays as-is: a dying task's kstack is freed via the
  zombie list; the slot itself is retained as `TASK_ZOMBIE` (see §3).

### 3. `waitpid(pid)` + exit status retention

**Exit code plumbing:**

- `SYS_EXIT` (16) now reads the exit code from `r->ebx`:
  ```c
  case SYS_EXIT:
      if (task_current_pid() == 0) user_program_exit();
      else task_exit_current(r->ebx);
      break;
  ```
- `task_exit_current(unsigned int code)` stores the code and sets the
  `current_exited` flag (as before). The code rides the same path as
  `current_exited`: a new `static unsigned int current_exit_code`, set by
  `task_exit_current`, read by the exit path in `task_switch_kernel` (mirrors
  `current_exited` at task.c:119) and cleared alongside it.
- In the exit path (task.c:124–137): instead of `dead->state = TASK_FREE`,
  keep the slot:
  ```c
  dead->exit_code = ep_code;   // carried from task_exit_current
  dead->state = TASK_ZOMBIE;
  ```
  `MSG_EXIT` to sink/event is still sent. Address space/mbox/args/lctx are
  still freed (a zombie holds only the slot's scalar fields), and the kstack is
  still freed lazily via the zombie list.
- A zombie counts as **not alive**: update `task_alive()` (task.c:346) to
  `state != TASK_FREE && state != TASK_ZOMBIE`; update the spawn free-slot scan
  (task.c:185–187) so zombies are reclaimable (see Reaping); update the exit
  path's recipient checks (`tasks[sink].state != TASK_FREE`) so messages are not
  sent to zombies.

**Blocking wait:**

- New syscall `SYS_WAITPID` (31). Semantics: wait for a *specific* child pid
  (positive pid that is a child of the caller — `parent == current`). Returns
  the exit code, or a negative error:
  - `pid` not a child of caller → `-1` (ESRCH-like).
  - child already `TASK_ZOMBIE` → reap immediately, return `exit_code`.
- child alive → mark current `TASK_WAITING` with `wait_pid = pid`, then loop
  `sti; hlt; cli` (in the handler, as in §2) until the child's state is
  ZOMBIE/FREE, then reap. The loop re-checks the child state via the syscall
  argument (local `pid`), **not** the TCB `wait_pid` field (the scheduler
  clears it on promote). While blocked, the task is `TASK_WAITING` and
  invisible to the round-robin scan; the promote rule wakes it, and the
  scheduler iret's back into the spin loop, which re-checks and reaps.
- The scheduler's promote rule (§2) wakes a `TASK_WAITING` task once
  `task_done(child)` (child is ZOMBIE or FREE).
- No children tracking structure beyond `parent`; waitpid does a linear scan of
  `tasks[]` (24 entries).

**Reaping (`task_free_slot(t)`):**

- `waitpid` reaps its zombie child: `t->state = TASK_FREE`.
- On task exit, reap the exiting task's own zombie children (they can no longer
  be waited on).
- `task_spawn`, when no `TASK_FREE` slot exists, reaps the oldest `TASK_ZOMBIE`
  slot (loop index order) before failing with `-1`.

### 4. `get_children`

- `task_spawn` records `t->parent = task_current_pid()` (spawned from task 0 →
  parent 0).
- New syscall `SYS_GET_CHILDREN` (32):
  ```c
  // r->ebx = user buf (unsigned int*), r->ecx = max count
  case SYS_GET_CHILDREN:
      if (in_user((void*)r->ebx, r->ecx * 4)) {
          int n = task_get_children((unsigned int*)r->ebx, r->ecx);
          r->eax = n;
      } else r->eax = -5;
      break;
  ```
  `task_get_children(buf, max)` scans `tasks[]`, collects pids where
  `parent == current` and `state != TASK_FREE` (zombies included — they are
  still children until reaped), up to `max`. Returns the count written.

### 5. libaos user API + test

New wrappers in `programs/libaos.{c,h}`:

```c
void sleep_ms(unsigned int ms);            // SYS_SLEEP
int  waitpid(unsigned int pid);            // SYS_WAITPID (exit code or <0)
int  get_children(unsigned int *pids, unsigned int max); // SYS_GET_CHILDREN
void exit_with_code(int code);             // SYS_EXIT with ebx=code
```

`exit()` keeps working (calls `exit_with_code(0)`).

**Test program `programs/sleeptest.c`:**

1. `sleep_ms(50)` → assert at least ~45 ticks elapsed via `get_tick()` delta.
2. Spawn a child `bin/exitto` (new tiny program) that calls
   `exit_with_code(7)`; `waitpid(child)` must return `7`.
3. `get_children()` must include the child pid (and afterwards, since the child
   is reaped, an empty list).
4. `waitpid` on a bogus pid returns `-1`.
5. Prints `SLEEPTEST PASS` / `SLEEPTEST FAIL` to stdout (route_text → terminal).

**Test script `scripts/sleeptest.py`:** boots `aos.iso` headless (pattern of
`ipctest.py`/`linhello.py`), spawns a terminal from the dock, runs `sleeptest`,
asserts serial log has no `KERNEL PANIC`, and asserts the term text band grew /
shows `SLEEPTEST PASS`. Added to the `TESTS` list in the Makefile.

### 6. Non-goals (explicitly out of scope)

- Signals, priorities, time slices, fork/exec, cwd/env (later TODO 1.2 items).
- `nanosleep`, `sleep` in Linux ABI (linux_syscall already has its own
  nanosleep spin).
- Real-time/priority scheduling; keep round-robin order.
- Blocking `waitpid` with `-1` (any child) — only specific pid.

## Error handling & edge cases

| Case | Behavior |
|---|---|
| `sleep_ms(0)` | wake_tick = tick → handler loop exits immediately; effectively a yield |
| `wake_tick` wraparound | compared as signed diff; safe for < ~49 days of ticks |
| All tasks blocked, no task-0 | `next = current_task` fallback resumes the blocked handler loop, which re-hlts (safe, no scheduler recursion) |
| `waitpid` on non-child pid | `-1` (no block) |
| `waitpid` on reaped/free pid | `-1` (no block) |
| Child exits before waitpid called | already ZOMBIE → reap immediately |
| Parent exits with live zombies | parent exit reaps them |
| Spawn with no free slot + zombies exist | reaps oldest zombie slot |
| Spawn with no free slot + no zombies | returns `-1` as today |
| `get_children` with small buffer | truncates to `max`, returns count written |

## Verification

- `make` (builds ISO, embeds `sleeptest`/`exitto`).
- `make test` runs the full regression: `linhello lincat ipctest manytest
  notepadtest sleeptest`. All must stay green.
- `scripts/sleeptest.py` asserts the pass marker on screen and no panic in the
  serial log.

## Files touched

- `kernel/task.h` — states + fields
- `kernel/task.c` — scheduler promote/skip, idle-wait fallback, exit zombie
  retention, reaping, `task_get_children`, `task_alive` zombie-awareness
- `kernel/syscall.h` — SYS_SLEEP=30, SYS_WAITPID=31, SYS_GET_CHILDREN=32
- `kernel/syscall.c` — new handlers, exit-code plumbing
- `programs/libaos.{c,h}` — wrappers
- `programs/exitto.c` — new tiny program (exit_with_code(7))
- `programs/sleeptest.c` — new test program
- `Makefile` — add `sleeptest`, `exitto` to PROGRAMS; `sleeptest` to TESTS
- `scripts/sleeptest.py` — new headless regression
- `AGENTS.md` — syscall count 30→33, document sleep/waitpid/zombie semantics
