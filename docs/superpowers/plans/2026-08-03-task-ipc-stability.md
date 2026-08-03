# Task and IPC Stability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make task exit notifications deterministic and mailbox operations atomic, with a QEMU regression test that detects duplicate exit messages and scheduler failures.

**Architecture:** `kernel/task.c` retains ownership of scheduling, task lifetime, and mailbox storage. It will publish task exit only from the scheduler cleanup path and make each mailbox producer/consumer transaction fully IRQ-atomic. A small ring-3 `ipctest` program acts as a sink for a short-lived child and deliberately panics on an unexpected exit-message count; a host Python harness drives it through the existing WM terminal and checks the serial log and framebuffer.

**Tech Stack:** Freestanding C11 and x86 ring 3/ring 0 system calls, QEMU i386 HMP monitor and serial log, Python 3 standard library.

## Global Constraints

- No libc or dynamic allocation in kernel or user programs.
- Keep the existing task and syscall ABI unchanged: `task_mailbox_send`, `task_mailbox_recv`, `SYS_SEND`, `SYS_RECV`, `SYS_SPAWN`, and message definitions retain their signatures and values.
- `MSG_CAP` remains 128 and mailbox errors remain `-1` (invalid PID), `-2` (free target), and `-3` (full mailbox).
- `task_exit_current()` must only mark the task exited; `task_switch_kernel()` is the only cleanup and exit-notification publisher.
- A full mailbox must not block, retry, or delay task cleanup.
- Preserve unrelated user changes in `kernel/task.c`; do not revert them.
- Build with `make`; there is no standalone unit-test framework.

---

### Task 1: Ring-3 Exit-Notification Regression Program

**Files:**
- Create: `programs/ipctest.c`
- Modify: `Makefile:21`

**Interfaces:**
- Consumes: `int spawn(const char *path, const char *args, unsigned int sink)`, `int recv_msg(struct aos_msg *m)`, `unsigned int get_tick(void)`, `void yield(void)`, and `void panic(void)` from `programs/libaos.h`.
- Produces: ELF program `bin/ipctest`, which verifies that its `bin/echo` child causes exactly one `MSG_EXIT` where `a` is that child's PID, then prints `IPC TEST PASS\n` and exits. It triggers `panic()` if spawning fails, no matching exit arrives within 1000 ticks, or a second matching exit arrives during the following 100 ticks.

- [ ] **Step 1: Add `ipctest` to the embedded program list**

In `Makefile`, append `ipctest` to `PROGRAMS` so the existing `programs/%.elf` rule builds it and `scripts/gen_progs.py` embeds it in the ramdisk:

```make
PROGRAMS = help uptime clear echo tick info reboot panic ls cat rm format shutdown test wm term clock ipctest
```

- [ ] **Step 2: Write the failing regression program**

Create `programs/ipctest.c` with this complete program. It has no special kernel hooks: it tests the public spawn and mailbox interfaces that GUI programs already use.

```c
#include "libaos.h"

static void fail(void) {
    print("IPC TEST FAIL\n");
    panic();
}

void main(void) {
    unsigned int start = get_tick();
    int child = spawn("bin/echo", "ipc", getpid());
    if (child < 0) fail();

    int exits = 0;
    struct aos_msg m;
    while ((int)(get_tick() - start) < 1000) {
        if (recv_msg(&m) == 0 && m.type == MSG_EXIT && m.a == (unsigned int)child)
            exits++;
        if (exits > 1) fail();
        if (exits == 1) break;
        yield();
    }
    if (exits != 1) fail();

    start = get_tick();
    while ((int)(get_tick() - start) < 100) {
        if (recv_msg(&m) == 0 && m.type == MSG_EXIT && m.a == (unsigned int)child)
            fail();
        yield();
    }
    print("IPC TEST PASS\n");
}
```

- [ ] **Step 3: Build the regression program and demonstrate the pre-fix failure when applicable**

Run: `make`

Expected: `programs/ipctest.elf` is linked and `kernel/progs.c` contains the `bin/ipctest` payload. On an unmodified baseline where `task_exit_current()` and `task_switch_kernel()` both publish `MSG_EXIT`, launching `ipctest` panics with `IPC TEST FAIL`; retain the test even if the current worktree already contains the partial exit-handling fix.

- [ ] **Step 4: Commit the independent test program**

```bash
git add Makefile programs/ipctest.c kernel/progs.c
git commit -m "test: add IPC exit notification regression program"
```

### Task 2: Deterministic Task Cleanup and Atomic Mailboxes

**Files:**
- Modify: `kernel/task.c:75-108, 180-182, 207-240`

**Interfaces:**
- Consumes: `task_mailbox_send(unsigned int pid, unsigned int t, unsigned int a, unsigned int b, unsigned int c, unsigned int d)` and `task_event_pid()`'s `event_pid` state.
- Produces: exactly one `MSG_TYPE_EXIT` per distinct live recipient when a non-idle task exits; fully IRQ-atomic `task_mailbox_send()` and `task_mailbox_recv()` transactions with unchanged return values.

- [ ] **Step 1: Establish the expected failing behavior with `ipctest`**

Build and boot a no-display QEMU instance with a monitor and serial log, then use the host harness from Task 3's `send_text()` sequence interactively to launch `ipctest` in a terminal. Before this task's fix, the old double-publication behavior must reach `IPC TEST FAIL` and a `KERNEL PANIC`; if the current worktree already includes the user-authored single-publisher change, record that it is the expected partial implementation and proceed without reverting it.

- [ ] **Step 2: Make scheduler cleanup the only exit publisher**

In `task_switch_kernel()`, retain the `current_exited` branch immediately after saving the current context. Capture `dead->sink` and `event_pid` before freeing the task, then send after `dead->state = TASK_FREE`:

```c
if (current_exited) {
    current_exited = 0;
    struct task *dead = current_task;
    unsigned int sink = dead->sink;
    unsigned int ep = (unsigned int)event_pid;
    dead->state = TASK_FREE;
    if (sink < MAX_TASKS && sink != dead->pid && tasks[sink].state != TASK_FREE)
        task_mailbox_send(sink, MSG_TYPE_EXIT, dead->pid, 0, 0, 0);
    if (ep > 0 && ep < MAX_TASKS && ep != dead->pid && ep != sink &&
        tasks[ep].state != TASK_FREE)
        task_mailbox_send(ep, MSG_TYPE_EXIT, dead->pid, 0, 0, 0);
}
```

Replace `task_exit_current()` with only:

```c
void task_exit_current(void) {
    current_exited = 1;
}
```

- [ ] **Step 3: Extend mailbox critical sections to cover validation and fullness**

Replace the beginning of `task_mailbox_send()` so the target state and ring-full check cannot race an IRQ or a task switch:

```c
int task_mailbox_send(unsigned int pid, unsigned int t, unsigned int a,
                      unsigned int b, unsigned int c, unsigned int d) {
    unsigned int flags;
    irq_save(&flags);
    if (pid >= MAX_TASKS) {
        irq_restore(flags);
        return -1;
    }
    if (tasks[pid].state == TASK_FREE) {
        irq_restore(flags);
        return -2;
    }
    unsigned int next = (mbox_tail[pid] + 1) % MSG_CAP;
    if (next == mbox_head[pid]) {
        irq_restore(flags);
        return -3;
    }
    mbox[pid][mbox_tail[pid]][0] = t;
    mbox[pid][mbox_tail[pid]][1] = a;
    mbox[pid][mbox_tail[pid]][2] = b;
    mbox[pid][mbox_tail[pid]][3] = c;
    mbox[pid][mbox_tail[pid]][4] = d;
    mbox_tail[pid] = next;
    irq_restore(flags);
    return 0;
}
```

Keep `task_mailbox_recv()`'s existing critical section spanning empty inspection, the five-word copy, and the head update. Do not add blocking or retry behavior.

- [ ] **Step 4: Build the kernel and test program**

Run: `make`

Expected: clean successful build of `aos.iso`; no compiler warnings introduced by `kernel/task.c` or `programs/ipctest.c`.

- [ ] **Step 5: Commit the scheduler and mailbox fix**

```bash
git add kernel/task.c kernel/progs.c
git commit -m "fix: make task exits and mailboxes deterministic"
```

### Task 3: Automated QEMU IPC Regression Harness

**Files:**
- Create: `scripts/ipctest.py`

**Interfaces:**
- Consumes: `aos.iso`, the `bin/ipctest` program, QEMU HMP monitor commands, the serial log, and `scripts/guitester.py`'s monitor socket convention.
- Produces: executable host command `python3 scripts/ipctest.py` that launches a terminal, types `ipctest`, verifies no serial `KERNEL PANIC`, verifies the desktop remains drawable, and exits nonzero on timeout or failed assertion.

- [ ] **Step 1: Write the failing host test harness**

Create `scripts/ipctest.py`. Use the following structure; it starts QEMU in the background, waits for boot, drives the dock terminal launcher through HMP mouse commands, and types each character through HMP `sendkey`.

```python
#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-ipc.sock"
SER = "/tmp/aos-ipc.log"
PPM = "/tmp/aos-ipc.ppm"
BEFORE = "/tmp/aos-ipc-before.ppm"

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
    for path in (MON, SER, PPM, BEFORE):
        try: os.unlink(path)
        except FileNotFoundError: pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-cdrom", ISO, "-display", "none",
        "-serial", "file:" + SER, "-monitor", "unix:" + MON + ",server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(MON)
        time.sleep(5)
        hmp("mouse_move -39 341")
        hmp("mouse_button 1")
        hmp("mouse_button 0")
        time.sleep(1)
        hmp("screendump " + BEFORE)
        wait_for(BEFORE)
        send_text("ipctest\n")
        time.sleep(3)
        if "KERNEL PANIC" in serial_text():
            raise AssertionError("ipctest triggered a kernel panic")
        hmp("screendump " + PPM)
        wait_for(PPM)
        if os.path.getsize(PPM) <= 1024:
            raise AssertionError("window manager did not produce a framebuffer dump")
        with open(BEFORE, "rb") as f: before = f.read()
        with open(PPM, "rb") as f: after = f.read()
        if before == after:
            raise AssertionError("terminal did not process the ipctest command")
        print("PASS: IPC exit notification and WM regression")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
```

The first execution should fail on the old double-exit implementation because `ipctest` calls `panic()`; it must pass after Task 2.

- [ ] **Step 2: Run the full automated regression**

Run: `make && python3 scripts/ipctest.py`

Expected: `PASS: IPC exit notification and WM regression`; the script exits 0 and `/tmp/aos-ipc.log` does not contain `KERNEL PANIC`.

- [ ] **Step 3: Repeat the run to cover task-slot reuse**

Run: `python3 scripts/ipctest.py && python3 scripts/ipctest.py`

Expected: both runs print the pass line. This exercises fresh boot, WM/terminal spawning, child exit, and terminal return twice without a scheduler hang.

- [ ] **Step 4: Commit the host regression harness**

```bash
git add scripts/ipctest.py
git commit -m "test: automate task IPC QEMU regression"
```

## Self-Review

- Spec coverage: Task 2 implements the sole exit publisher, distinct live-recipient filtering, and full mailbox producer atomicity. Task 1 checks exactly one sink notification; Task 3 checks boot, GUI-driven launch, no panic, and a live framebuffer.
- Placeholders: none. Every new file, interface, command, expected result, and commit is explicit.
- Type consistency: `spawn()` returns `int`; mailbox payload uses `struct aos_msg`; `MSG_EXIT` uses `a` as the child PID; task-layer uses unsigned PID and payload words throughout.
