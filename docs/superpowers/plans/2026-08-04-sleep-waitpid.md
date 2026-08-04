# Sleep, waitpid, get_children Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add kernel-side blocking `sleep(ms)` (wake-time in the TCB), `waitpid` with exit-status retention via zombies, and `get_children` to the AOS kernel, proven by a QEMU regression.

**Architecture:** The TCB (`struct task` in `kernel/task.c`) gains block states and the scheduler promotes/skips blocked tasks on each switch. Blocking waits live in task.c functions (`task_sleep`/`task_waitpid`) invoked from the syscall handler, using the documented `sti; hlt; cli` loop; they never run inside `task_switch_kernel` itself (a nested wait there would corrupt `kernel_esp` of an outer scheduler frame). Exited tasks keep their slot as `TASK_ZOMBIE` with the exit code until `waitpid` reaps it. A ring-3 `sleeptest` program and host `sleeptest.py` harness (panic-detection pattern from `ipctest.py`) verify it end to end.

**Tech Stack:** Freestanding C11 + x86 ring 3/ring 0 syscalls (`int 0x80`), QEMU i386 HMP monitor + serial log, Python 3 standard library.

## Global Constraints

- No libc or dynamic allocation in kernel or user programs.
- `tick` is `volatile unsigned int` at 1000 Hz, defined in `kernel/kernel.c:20`; declare `extern volatile unsigned int tick;` where used (pattern: `kernel/syscall.c:15`).
- `MAX_TASKS` is 24; task `pid` always equals its slot index in `tasks[]`.
- Task table `tasks[]` is `static` in `kernel/task.c` — all access must stay inside task.c; `kernel/syscall.c` only calls exported task.c functions.
- Keep the existing task and syscall ABI unchanged for numbers 0–29; only add 30–32 and extend `SYS_EXIT` (16) to read the code from `%ebx`.
- Blocking waits (`sti; hlt; cli`) live in `task_sleep`/`task_waitpid` (called from `syscall_handler`), **never** inside `task_switch_kernel` — the timer IRQ re-enters the scheduler and a wait inside it would overwrite an outer scheduler frame on the stack.
- A `TASK_ZOMBIE` slot's kstack is still freed lazily via the existing `zombies[]` list; the slot itself is retained until reaped.
- Build with `make`; regression with `python3 scripts/<name>.py`; no standalone unit-test framework.

---

### Task 1: User-facing API + regression programs + host harness (red)

**Files:**
- Modify: `programs/libaos.h`
- Modify: `programs/libaos.c:4-17, 55`
- Create: `programs/exitto.c`
- Create: `programs/sleeptest.c`
- Modify: `Makefile:22`
- Create: `scripts/sleeptest.py`

**Interfaces:**
- Produces (used by Task 2's kernel): syscall numbers 30/31/32 in `programs/libaos.c` and the four libaos wrappers `sleep_ms`, `waitpid`, `get_children`, `exit_with_code` — plus `bin/exitto` (exits with code 7) and `bin/sleeptest` (the red/green detector, panics on any failed check so the harness can detect it via the serial log). These build and run on the current kernel even though syscalls 30–32 are unhandled (they return `-1` from the `default` branch, `kernel/syscall.c:366-368`).

- [ ] **Step 1: Declare the new libaos API**

In `programs/libaos.h`, after the existing `void exit(void);` line, add:

```c
void sleep_ms(unsigned int ms);                    // SYS_SLEEP (block ~ms)
int  waitpid(unsigned int pid);                    // SYS_WAITPID (exit code or <0)
int  get_children(unsigned int *pids, unsigned int max); // SYS_GET_CHILDREN
void exit_with_code(int code);                     // SYS_EXIT with ebx=code
```

- [ ] **Step 2: Add the syscall numbers and wrappers**

In `programs/libaos.c`, after `#define SYS_GETEVENT 29`, add:

```c
#define SYS_SLEEP     30
#define SYS_WAITPID   31
#define SYS_GET_CHILDREN 32
```

In `programs/libaos.c`, after the existing `void exit(void)` wrapper (line 55), add:

```c
void exit_with_code(int code)         { syscall(SYS_EXIT, code, 0, 0, 0, 0); for (;;); }
void sleep_ms(unsigned int ms)        { syscall(SYS_SLEEP, (int)ms, 0, 0, 0, 0); }
int  waitpid(unsigned int pid)        { return syscall(SYS_WAITPID, (int)pid, 0, 0, 0, 0); }
int  get_children(unsigned int *pids, unsigned int max) {
    return syscall(SYS_GET_CHILDREN, (int)pids, (int)max, 0, 0, 0);
}
```

`exit()` (line 55) is unchanged: it already calls `syscall(SYS_EXIT, 0, ...)` → exit code 0.

- [ ] **Step 3: Create `programs/exitto.c`**

```c
#include "libaos.h"

void main(void) {
    exit_with_code(7);
}
```

- [ ] **Step 4: Create `programs/sleeptest.c`**

Every failed check prints a diagnostic then `fail()`, which prints `SLEEPTEST FAIL` and panics — the host harness detects the panic in the serial log. On the current kernel (syscalls 30–32 unhandled), check 1 already fails (sleep returns instantly, `dt` ~ 0), so the red build panics immediately.

```c
#include "libaos.h"

static void fail(void) {
    print("SLEEPTEST FAIL\n");
    panic();
}

void main(void) {
    print("SLEEPTEST\n");

    // 1. sleep_ms(50) must block for ~50 ticks (tick = 1 ms).
    unsigned int t0 = get_tick();
    sleep_ms(50);
    unsigned int dt = get_tick() - t0;
    if (dt < 45) {
        print("sleep: only ");
        print_dec(dt);
        print(" ticks\n");
        fail();
    }

    // 2. Spawn a child that exits with code 7; get_children must list it and
    //    waitpid must return 7.
    int child = spawn("bin/exitto", "", getpid());
    if (child < 0) { print("spawn failed\n"); fail(); }

    unsigned int kids[4];
    int n = get_children(kids, 4);
    int found = 0;
    for (int i = 0; i < n; i++)
        if (kids[i] == (unsigned int)child) found = 1;
    if (!found) { print("child missing from get_children\n"); fail(); }

    int code = waitpid((unsigned int)child);
    if (code != 7) {
        print("waitpid got ");
        print_dec((unsigned int)code);
        print(" want 7\n");
        fail();
    }

    // 3. After reaping, the child must be gone from get_children.
    n = get_children(kids, 4);
    for (int i = 0; i < n; i++)
        if (kids[i] == (unsigned int)child) {
            print("child still listed after reap\n");
            fail();
        }

    // 4. waitpid on a bogus pid returns -1.
    if (waitpid(9999) != -1) { print("waitpid bogus accepted\n"); fail(); }

    print("SLEEPTEST PASS\n");
}
```

- [ ] **Step 5: Add the two programs to the build**

In `Makefile:22`, append `sleeptest exitto` to `PROGRAMS`:

```make
PROGRAMS = help uptime clear echo tick info reboot panic ls cat rm format shutdown test wm term clock ipctest notepad many linrun sleeptest exitto
```

- [ ] **Step 6: Build and verify the payload is embedded**

Run: `make`

Expected: clean build of `aos.iso`; `kernel/progs.c` now contains `bin/sleeptest` and `bin/exitto` payloads (they link via the existing `programs/%.elf` rule with `programs/libaos.o`, which now exports the four new wrappers).

- [ ] **Step 7: Write the host harness `scripts/sleeptest.py`**

Copy `scripts/linhello.py` and replace only the command sent and the final assertion, plus tighten the text-band check so the test is specific. The harness boots `aos.iso` headless, clicks the dock terminal launcher (`mouse_move -39 341` from the boot-centered cursor at (511,383)), types `sleeptest`, then asserts the serial log stays panic-free and the terminal text band grew.

```python
#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-sleeptest.sock"
SER = "/tmp/aos-sleeptest.log"
PPM = "/tmp/aos-sleeptest.ppm"
BEFORE = "/tmp/aos-sleeptest-before.ppm"

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
        hmp("sendkey " + keys.get(ch, ch))
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

def serial_text():
    try:
        with open(SER, "r", errors="replace") as f: return f.read()
    except FileNotFoundError:
        return ""

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
        send_text("sleeptest\n")
        end = time.time() + 25
        log = ""
        while time.time() < end:
            time.sleep(1)
            log = serial_text()
            if "KERNEL PANIC" in log: break
        if "KERNEL PANIC" in log:
            raise AssertionError("sleeptest triggered a kernel panic")
        if "Unknown command" in log or "cannot run command" in log:
            raise AssertionError("sleeptest did not launch: %r"
                                 % log.strip().splitlines()[-1])
        hmp("screendump " + PPM)
        wait_for(PPM)
        if os.path.getsize(PPM) <= 1024:
            raise AssertionError("window manager did not produce a framebuffer dump")
        before_txt = count_text_pixels(BEFORE, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        after_txt = count_text_pixels(PPM, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        if after_txt - before_txt <= TXT_THRESHOLD:
            raise AssertionError(
                "terminal did not render sleeptest output (band text grew %d, want > %d)"
                % (after_txt - before_txt, TXT_THRESHOLD))
        print("PASS: sleep, waitpid, get_children regression")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 8: Run the harness to verify it fails (red)**

Run: `python3 scripts/sleeptest.py`

Expected: FAIL — `AssertionError: sleeptest triggered a kernel panic`. On this baseline, syscall 30 is unhandled so `sleep_ms(50)` returns instantly, `dt` ~ 0, `sleeptest` prints `SLEEPTEST FAIL` and panics, and the harness detects `KERNEL PANIC` in `/tmp/aos-sleeptest.log`. This is the red test for Task 2.

- [ ] **Step 9: Commit the failing test scaffold**

```bash
git add programs/libaos.h programs/libaos.c programs/exitto.c programs/sleeptest.c Makefile kernel/progs.c scripts/sleeptest.py
git commit -m "test: add sleeptest/exitto programs and QEMU harness (red)"
```

---

### Task 2: Kernel — TCB states, scheduler promote/skip, zombies, syscalls (green)

**Files:**
- Modify: `kernel/task.h:6-8, 12-29, 34`
- Modify: `kernel/task.c:32, 40-42, 117-144, 182-188, 194, 327-329, 339-348, 355-368`
- Modify: `kernel/linux_syscall.c:59`
- Modify: `kernel/syscall.c:231-236, 366`

**Interfaces:**
- Consumes: `tick` (`kernel/kernel.c:20`); libaos wrappers and `bin/sleeptest`/`bin/exitto` from Task 1.
- Produces: `TASK_SLEEPING`/`TASK_WAITING`/`TASK_ZOMBIE` states and `wake_tick`/`wait_pid`/`exit_code`/`parent` fields; `task_exit_current(unsigned int code)`; `task_sleep(unsigned int ms)`; `task_waitpid(unsigned int pid)` returning exit code or negative error; `task_get_children(unsigned int *buf, unsigned int max)` returning count; syscall handlers for `SYS_SLEEP`(30)/`SYS_WAITPID`(31)/`SYS_GET_CHILDREN`(32) and the `SYS_EXIT` exit-code path.

- [ ] **Step 1: Add block states and fields to the TCB**

In `kernel/task.h`, replace the state defines (lines 6–8) with:

```c
#define TASK_FREE    0
#define TASK_READY   1
#define TASK_RUNNING 2
#define TASK_SLEEPING 3   // blocked until tick >= wake_tick
#define TASK_WAITING  4   // blocked until child (wait_pid) exits
#define TASK_ZOMBIE   5   // exited, exit_code retained, awaiting waitpid
```

In `kernel/task.h`, after the `unsigned int sink;` member, add:

```c
    unsigned int parent;      // pid that spawned this task (0 = kernel)
    unsigned int wake_tick;   // TASK_SLEEPING: wake when tick >= wake_tick
    unsigned int wait_pid;    // TASK_WAITING: child pid being waited on
    unsigned int exit_code;   // TASK_ZOMBIE: exit code to hand to waitpid
```

Change the prototype (line 34) from `void task_exit_current(void);` to:

```c
void task_exit_current(unsigned int code);
```

Add the new task.c API to `kernel/task.h` (near `task_alive`, line 39):

```c
void task_sleep(unsigned int ms);
int  task_waitpid(unsigned int pid);
int  task_get_children(unsigned int *buf, unsigned int max);
```

- [ ] **Step 2: Add `tick` extern and the exit-code state**

In `kernel/task.c`, after the `#include` block, add:

```c
extern volatile unsigned int tick;
```

Replace the two statics at lines 41–42:

```c
static int current_exited = 0;
static unsigned int current_exit_code = 0;
```

- [ ] **Step 3: Save-rule, exit-retention, and promote-then-scan in the scheduler**

In `kernel/task.c`, replace lines 117–144 (from `unsigned int task_switch_kernel(unsigned int cur_esp) {` through `if (!next) next = current_task;`) with:

```c
unsigned int task_switch_kernel(unsigned int cur_esp) {
    drain_zombies();
    int exited = current_exited;
    current_task->kernel_esp = cur_esp;
    // A task that blocked itself in sleep()/waitpid() stays in its block state
    // so the round-robin scan skips it; the promote checks below move it back
    // to TASK_READY when its wake time arrives or its child dies.
    if (current_task->state != TASK_SLEEPING &&
        current_task->state != TASK_WAITING)
        current_task->state = TASK_READY;

    struct task *dead = 0;
    if (exited) {
        current_exited = 0;
        dead = current_task;
        unsigned int sink = dead->sink;
        unsigned int ep = (unsigned int)event_pid;
        dead->exit_code = current_exit_code;
        dead->state = TASK_ZOMBIE;   // retain the exit code until waitpid reaps it
        // Exit notifications are best-effort: a full recipient mailbox
        // (task_mailbox_send returns -3) silently drops the message. Zombies
        // have a freed mailbox, so never send to one (task_alive excludes them).
        if (sink < MAX_TASKS && sink != dead->pid && task_alive(sink))
            task_mailbox_send(sink, MSG_TYPE_EXIT, dead->pid, 0, 0, 0);
        if (ep > 0 && ep < MAX_TASKS && ep != dead->pid && ep != sink &&
            task_alive(ep))
            task_mailbox_send(ep, MSG_TYPE_EXIT, dead->pid, 0, 0, 0);
        // Reap this task's own zombie children: nobody can waitpid them now
        // (only a parent may wait on its children, and we are exiting).
        for (int i = 1; i < MAX_TASKS; i++)
            if (tasks[i].parent == dead->pid && tasks[i].state == TASK_ZOMBIE)
                tasks[i].state = TASK_FREE;
    }

    struct task *next = 0;
    for (int i = 1; i <= MAX_TASKS; i++) {
        struct task *t = &tasks[(current_task->pid + i) % MAX_TASKS];
        // Promote blocked tasks whose block condition has resolved. Compare
        // wake_tick as a signed diff so wraparound is safe.
        if (t->state == TASK_SLEEPING && (int)(tick - t->wake_tick) >= 0) {
            t->state = TASK_READY;
            t->wake_tick = 0;
        }
        // Note: wait_pid is NOT cleared on promote — task_waitpid clears it
        // when it actually reaps, so the spawn slot-reap guard still sees a
        // promoted-but-not-yet-reaped waiter and leaves its zombie alone.
        if (t->state == TASK_WAITING && task_done(t->wait_pid))
            t->state = TASK_READY;
        if (t->state == TASK_READY) { next = t; break; }
    }
    // No READY task: resume whatever was current. If it is blocked this just
    // iret's back into its own sti;hlt;cli wait loop, which re-checks and hlts
    // again. Never wait inside the scheduler itself: the timer IRQ that would
    // wake a sleeper re-enters task_switch_kernel, and a nested wait there
    // would overwrite kernel_esp of an outer scheduler frame on the stack.
    if (!next) next = current_task;
```

- [ ] **Step 4: Add the `task_done` helper**

In `kernel/task.c`, add just above `task_switch_kernel` (before line 117):

```c
// A wait resolves when the child is a retained zombie or is gone entirely.
static int task_done(unsigned int pid) {
    if (pid >= MAX_TASKS) return 1;
    unsigned int s = tasks[pid].state;
    return s == TASK_ZOMBIE || s == TASK_FREE;
}
```

- [ ] **Step 5: Spawn slot reaping and `parent` recording**

In `kernel/task.c`, replace the free-slot scan (lines 185–188):

```c
    int pid = -1;
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_FREE) { pid = i; break; }
    if (pid < 0) {
        // No free slot: reuse the oldest zombie that no live task is waiting
        // on. A zombie someone waits on must survive until waitpid reaps it.
        for (int i = 1; i < MAX_TASKS && pid < 0; i++) {
            if (tasks[i].state != TASK_ZOMBIE) continue;
            int waited = 0;
            for (int j = 0; j < MAX_TASKS; j++)
                if (tasks[j].wait_pid == tasks[i].pid &&
                    tasks[j].state != TASK_FREE && tasks[j].state != TASK_ZOMBIE)
                    { waited = 1; break; }
            if (!waited) pid = i;
        }
    }
    if (pid < 0) return -1;
```

After `t->pid = pid; t->sink = sink;` (line 193), add:

```c
    t->parent = task_current_pid();
```

- [ ] **Step 6: Exit-code plumbing and the new task.c functions**

Replace `task_exit_current` (lines 327–329) with:

```c
void task_exit_current(unsigned int code) {
    current_exit_code = code;
    current_exited = 1;
}
```

Replace `task_alive` (lines 346–348) so zombies count as not alive:

```c
int task_alive(unsigned int pid) {
    return pid < MAX_TASKS && tasks[pid].state != TASK_FREE &&
           tasks[pid].state != TASK_ZOMBIE;
}
```

Replace `task_set_sink` (lines 339–344) so it cannot target a zombie:

```c
int task_set_sink(unsigned int pid) {
    if (pid >= MAX_TASKS) return -1;
    if (pid != 0 && !task_alive(pid)) return -1;
    current_task->sink = pid;
    return 0;
}
```

In `task_mailbox_send` (line 364), reject zombie targets — a zombie's mailbox was freed at exit:

```c
    if (target->state == TASK_FREE || target->state == TASK_ZOMBIE) {
        irq_restore(flags);
        return -2;
    }
```

Add the three new functions after `task_alive` (after line 348):

```c
// Block the current task for ~ms ticks. The wait loop runs on this task's own
// kernel stack: each timer IRQ may switch away and back; on resume the loop
// re-checks tick. State is re-marked TASK_SLEEPING before every hlt because the
// scheduler's no-ready fallback forces the resumed task to TASK_RUNNING.
void task_sleep(unsigned int ms) {
    current_task->wake_tick = tick + (ms & 0x7FFFFFFF);
    while ((int)(tick - current_task->wake_tick) < 0) {
        current_task->state = TASK_SLEEPING;
        __asm__ volatile("sti; hlt; cli");
    }
    current_task->wake_tick = 0;
    current_task->state = TASK_RUNNING;
}

// Wait for a specific child to exit and return its exit code, reaping it.
// Returns -1 if pid is not a child of the current task or is already gone.
int task_waitpid(unsigned int pid) {
    if (pid == 0 || pid >= MAX_TASKS || tasks[pid].parent != current_task->pid)
        return -1;
    for (;;) {
        struct task *c = &tasks[pid];
        if (c->state == TASK_ZOMBIE) {
            current_task->wait_pid = 0;
            int code = (int)c->exit_code;
            c->state = TASK_FREE;   // reap
            return code;
        }
        if (c->state == TASK_FREE) {
            current_task->wait_pid = 0;
            return -1;              // reaped out from under us (spawn reuse)
        }
        // Child still alive: block until the scheduler promotes us. wait_pid
        // stays set until we reap (the spawn slot-reap guard depends on it),
        // and the state is re-marked TASK_WAITING before every hlt because the
        // scheduler's no-ready fallback forces the resumed task to TASK_RUNNING.
        current_task->wait_pid = pid;
        current_task->state = TASK_WAITING;
        __asm__ volatile("sti; hlt; cli");
    }
}

// Collect the pids of the current task's live and zombie children (a zombie is
// still a child until waitpid reaps it). Self is never a child. Returns the
// count written, truncated to max.
int task_get_children(unsigned int *buf, unsigned int max) {
    unsigned int me = current_task->pid;
    int n = 0;
    for (unsigned int i = 0; i < MAX_TASKS && (unsigned int)n < max; i++) {
        if (i != me && tasks[i].parent == me && tasks[i].state != TASK_FREE)
            buf[n++] = tasks[i].pid;
    }
    return n;
}
```

- [ ] **Step 7: Update the Linux exit call**

In `kernel/linux_syscall.c:59`, change `task_exit_current();` to:

```c
    task_exit_current(0);   // Linux exit codes are ignored for now
```

- [ ] **Step 8: Wire the new syscalls into the handler**

In `kernel/syscall.c`, replace the `SYS_EXIT` case (lines 231–236) with:

```c
    case SYS_EXIT:
        if (task_current_pid() == 0)
            user_program_exit();
        else
            task_exit_current(r->ebx);
        break;
```

In `kernel/syscall.c`, just before the `default:` case (line 366), add:

```c
    case SYS_SLEEP:
        task_sleep(r->ebx);
        r->eax = 0;
        break;
    case SYS_WAITPID:
        r->eax = task_waitpid(r->ebx);
        break;
    case SYS_GET_CHILDREN: {
        unsigned int max = r->ecx;
        if (max > MAX_TASKS) max = MAX_TASKS;
        if (in_user((void *)r->ebx, max * 4)) {
            int n = task_get_children((unsigned int *)r->ebx, max);
            r->eax = n;
        } else {
            r->eax = -5;
        }
        break;
    }
```

- [ ] **Step 9: Build the kernel**

Run: `make`

Expected: clean build of `aos.iso`. No compiler warnings from `kernel/task.c`, `kernel/syscall.c`, or `kernel/linux_syscall.c`. (`task.h` now declares `task_sleep`/`task_waitpid`/`task_get_children`; `task_exit_current` takes a code.)

- [ ] **Step 10: Run the regression — must now pass (green)**

Run: `python3 scripts/sleeptest.py`

Expected: `PASS: sleep, waitpid, get_children regression`, exit 0. `sleep_ms(50)` blocks ~50 ticks; the `bin/exitto` child's exit code 7 flows through `task_exit_current(7)` → `TASK_ZOMBIE` → `task_waitpid` → 7; `get_children` lists the live child, excludes it after reaping, and `waitpid(9999)` returns `-1`. No `KERNEL PANIC` in `/tmp/aos-sleeptest.log`.

- [ ] **Step 11: Run the existing regression suite — must stay green**

Run: `make test`

Expected: `ALL 5 TESTS PASSED` (`linhello lincat ipctest manytest notepadtest`). Zombie retention must not change exit-notification or mailbox behavior that those tests rely on.

- [ ] **Step 12: Commit the kernel feature**

```bash
git add kernel/task.h kernel/task.c kernel/linux_syscall.c kernel/syscall.c kernel/progs.c
git commit -m "feat: kernel-side sleep, waitpid with zombie exit status, get_children"
```

---

### Task 3: Wire into the regression target + document

**Files:**
- Modify: `Makefile:89`
- Modify: `AGENTS.md:11, 35`

**Interfaces:**
- Consumes: `scripts/sleeptest.py` (Task 1) and the kernel feature (Task 2).
- Produces: `make test` includes `sleeptest`; AGENTS.md documents the new syscall count and blocking semantics.

- [ ] **Step 1: Add `sleeptest` to the automated suite**

In `Makefile:89`, change:

```make
TESTS = linhello lincat ipctest manytest notepadtest sleeptest
```

- [ ] **Step 2: Run the full regression suite**

Run: `make test`

Expected: `ALL 6 TESTS PASSED` — each script boots `aos.iso` under QEMU; `sleeptest` additionally proves kernel-side blocking, exit-status retention, and reaping.

- [ ] **Step 3: Update AGENTS.md**

- In `AGENTS.md:35`, change "**30 syscalls via `int 0x80`**" to "**33 syscalls via `int 0x80`**".
- In `AGENTS.md:11`, correct the stale note. Replace "`SYS_READ_KEY` blocks in the kernel via `sti; hlt; cli` loop" with a pointer to the new blocking syscalls, and add a new bullet after the "Syscall hardening" block describing the process syscalls:

```
- **Process syscalls** (`SYS_SLEEP` 30, `SYS_WAITPID` 31, `SYS_GET_CHILDREN` 32):
  `SYS_SLEEP`/`SYS_WAITPID` block **in the syscall handler** via `sti; hlt; cli`
  (`task_sleep`/`task_waitpid`, `kernel/task.c`) — the scheduler skips
  `TASK_SLEEPING`/`TASK_WAITING` tasks and promotes them when `tick >= wake_tick`
  or the waited child dies. `SYS_EXIT` (16) reads the exit code from `%ebx`;
  the exiting task stays as `TASK_ZOMBIE` holding `exit_code` until `waitpid`
  reaps it (its address space/mbox are freed as before, only the slot scalars
  are kept). `TASK_ZOMBIE` slots are reclaimed by `task_spawn` when no free slot
  exists (never one a live `TASK_WAITING` task waits on). `SYS_READ_KEY` is
  **non-blocking** (returns -1 when empty); `read_key()` blocks in userland by
  spinning + `yield()`.
```

- [ ] **Step 4: Commit the wiring and docs**

```bash
git add Makefile AGENTS.md
git commit -m "test: add sleeptest to make test; docs: 33 syscalls, process block semantics"
```

---

## Self-Review

**1. Spec coverage:**
- sleep(ms)/wake-time in TCB → Task 2 Step 3 (save-rule + promote) and Step 6 (`task_sleep`, `wake_tick`).
- waitpid/exit status retention via zombies → Task 2 Steps 3, 6 (`TASK_ZOMBIE`, `exit_code`, `task_waitpid`), Step 7 (Linux exit calls with 0), Step 8 (`SYS_EXIT` reads `%ebx`).
- get_children → Task 2 Steps 5 (`parent`) and 6 (`task_get_children`), Step 8 (`SYS_GET_CHILDREN`).
- Blocking wait lives in handler, never scheduler → save-rule/promote code comments + `task_sleep`/`task_waitpid` `sti;hlt;cli` loops; scheduler keeps the `next = current_task` fallback.
- Zombie awareness: `task_alive` (Step 6), `task_mailbox_send` zombie reject (Step 6), `task_set_sink` (Step 6), exit-path recipient checks via `task_alive` (Step 3), spawn free-slot reaping with wait guard (Step 5), parent-exit reaping of zombie children (Step 3).
- libaos wrappers + `sleeptest`/`exitto` + `sleeptest.py` → Task 1.
- `make test` regression incl. `sleeptest` → Task 3 Steps 1–2.
- AGENTS.md updates → Task 3 Step 3.

**2. Placeholder scan:** No "TBD"/"similar to Task N" — every step has concrete code or exact file/line targets. `sleeptest.py` is a full copy with only the command/assertion changed from `linhello.py`.

**3. Type consistency:** `task_exit_current` takes `unsigned int` everywhere (syscall.c passes `r->ebx`, linux_syscall.c passes `0`). `task_waitpid(unsigned int pid)` returns `int` exit code or `-1`. `task_get_children(unsigned int *buf, unsigned int max)` returns `int` count; syscall caps `max` at `MAX_TASKS` and validates `in_user(buf, max*4)`. Wrapper signatures in libaos.h match libaos.c and the three task.c functions. Syscall numbers 30/31/32 match in `libaos.c` (`SYS_SLEEP`/`SYS_WAITPID`/`SYS_GET_CHILDREN`) and the handler cases.

One deviation from the spec's illustrative handler snippet: `task_sleep`/`task_waitpid` live in task.c (not inline in syscall.c) because `tasks[]` is `static` to task.c, and each loop iteration re-marks the block state before `hlt` because the scheduler's no-ready fallback forces the resumed task to `TASK_RUNNING`. `task_waitpid` also keeps `wait_pid` set until it actually reaps (the spec cleared it on promote): clearing it on promote lets the spawn slot-reap guard reap a zombie between a waiter's promotion and its resume, causing a spurious `-1`. Both changes match the spec's intent and its "never wait inside the scheduler" constraint.
