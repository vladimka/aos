# Kernel Log (dmesg) + strace — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a dmesg-style kernel log (`/proc/klog`, every `printf` line timestamped+leveled) and a `strace` shell built-in that records every syscall of a program and its children across all three ABIs, prints it after the program exits, and exposes a live dump via `/proc/<pid>/trace`.

**Architecture:** `kernel/klog.c` owns a 32 KB ring fed char-by-char by `printf()` (INFO) and by a leveled `klog()` writer (COM1+ring, no VGA); `procfs.c` exposes it as `/proc/klog`. Per-task trace state (`trace_on`, lazy kmalloc ring of 512×28-byte records) lives in `struct task`, is inherited at spawn, and is recorded by wrappers at the entry/exit of the three syscall dispatch handlers (`syscall_handler`, `linux_syscall_handler`, `aos_gui_handler`). `kernel/trace.c` provides name tables (AOS 0–48, AOS_EXT 500–519, Linux ~40 sparse) and rendering; a `strace` built-in in `commands.c` sets the flag, runs the program in-place, then dumps the shell task's own ring plus every inherited task's ring. Because all musl GUI programs are ABI_LINUX binaries that also issue AOS_EXT (500+) GUI syscalls, name lookup checks the AOS_EXT range **first** regardless of ABI.

**Tech Stack:** Kernel C11 (`-ffreestanding -nostdlib`, `-Wall -Wextra`), static musl i386 user programs (`tools/musl-i686/bin/i686-linux-musl-gcc -static -no-pie -Os -Wall -Wextra -Iprograms`). No libc in the kernel. Verification is QEMU serial-log assertions via new python scripts; per AGENTS.md the human runs every QEMU-based test.

**Spec:** `docs/superpowers/specs/2026-08-06-debug-utils-design.md` (committed as `9acffb7`).

## Global Constraints

- **The assistant never launches QEMU** (`qemu-system-i386`, `make test`, any `scripts/*.py`). Builds (`make`) and static checks are the assistant's job; every `python3 scripts/*.py` run is handed to the human. Each task ends with a `make` build + commit; the test scripts are written by the assistant and run by the human (typically one batched `make test` at the end).
- `make` must stay warning-free (`-Wall -Wextra`).
- Klog ring: `KLOG_SIZE 32768` bytes in BSS, line max `KLOG_LINE_MAX 256`. Each committed line is `[tttttttt] L text\n` where `tttttttt` is the low 32 bits of `tick` (1000 Hz PIT) as 8 lowercase hex digits and `L` is `I`/`W`/`E`.
- Trace ring: `TRACE_MAX 512` records of `struct trace_rec { num, a0..a4, ret }` (7 u32 = 28 bytes), ~14 KB kmalloc'd lazily on the first traced syscall; allocation failure silently clears `trace_on`.
- Record points are wrappers with **no semantic change** to any syscall: `trace_record(r)` at handler entry, `trace_finish(r)` at handler exit (all three handlers are single-entry/single-exit switch functions). `trace_record` captures `ebx,ecx,edx,esi,edi` from `struct registers` (see `kernel/interrupts.h`) and `ret` = final `eax`.
- **Name lookup rule (ABI trap):** `num` in `[500, 600)` uses the AOS_EXT table **regardless of task ABI**; otherwise ABI_LINUX tasks use the Linux table, ABI_AOS tasks use the AOS table. Unknown numbers print `syscall_0x%x`.
- procfs inode scheme: `PROCFS_ROOT 1`, `UPTIME 2`, `VERSION 3`, `MOUNTS 4`, `KLOG 5`, pid pseudo-dirs `0x1000 + pid`, trace files `0x2000 + pid`.
- The shell runs shell commands **in-place in task 0** (`user_program_start` / `user_program_start_linux`), so a traced program is task 0 itself; its `task_spawn` children inherit `trace_on` (that is `strace -f`).
- Exit/reclaim rules for trace buffers: a traced task's buffer is freed at exit only when already dumped or untraced (a zombie keeps it so the strace session can collect it); slot reuse in `task_spawn` frees any leftover buffer; `trace_session_dump` frees buffers only of dead tasks (live children keep theirs so `/proc/<pid>/trace` keeps working) and marks every printed task `trace_dumped`.
- Session dump and `/proc/<pid>/trace` render identically: header `== pid N ==\n`, one `name(a0, a1, ...) = 0xret\n` per record (exactly `nargs` args, hex lowercase, `ret` as `= 0x..`), then `... N records overwritten\n` when the ring wrapped. Output goes through `terminal_write`/`terminal_print` (NOT `printf`, to avoid feeding the trace dump back into the klog ring).

---

### Task 1: klog ring buffer core

**Files:**
- Create: `kernel/klog.h`, `kernel/klog.c`
- Modify: `kernel/printf.c:6-9` (`putc`), `kernel/kernel.c:132` (add marker after `config_load()`)

**Interfaces:**
- Produces (used by Task 2): `void klog_putc(char c);`, `void klog(int level, const char *fmt, ...);`, `unsigned int klog_read(unsigned int off, void *buf, unsigned int len);`, `unsigned int klog_size(void);`, and `#define KLOG_INFO 0 / KLOG_WARN 1 / KLOG_ERR 2` — all in `kernel/klog.h`.
- Consumes: `serial_putchar` (`drivers/serial.h`), global `volatile unsigned int tick` (declared `extern` in `kernel/kernel.c`, defined in kernel).

- [ ] **Step 1: Create `kernel/klog.h`**

```c
#ifndef KLOG_H
#define KLOG_H

// dmesg-style kernel log: a ring of timestamped, leveled lines fed by
// printf() (as INFO) and by the explicit leveled writer klog(). Readable
// from userland via /proc/klog (procfs.c).

#define KLOG_INFO 0
#define KLOG_WARN 1
#define KLOG_ERR  2

// Accumulate one character of the current line; printf routes every output
// byte through here. A complete line is flushed to the ring on '\n'.
void klog_putc(char c);

// Leveled write: renders fmt to COM1 AND the ring, but not the framebuffer.
void klog(int level, const char *fmt, ...);

// Copy up to `len` bytes of the log starting at virtual offset `off` into
// `buf`; returns the number of bytes copied (0 at/past the end). The log
// holds the most recent KLOG_SIZE bytes written since boot.
unsigned int klog_read(unsigned int off, void *buf, unsigned int len);

// Total bytes available (min(klog_total, KLOG_SIZE)); the virtual size.
unsigned int klog_size(void);

#endif
```

- [ ] **Step 2: Create `kernel/klog.c`**

```c
#include "klog.h"
#include "serial.h"
#include <stdarg.h>

#define KLOG_SIZE 32768
#define KLOG_LINE_MAX 256

static char klog_ring[KLOG_SIZE];
static unsigned int klog_pos;      // next ring write offset (wraps)
static unsigned int klog_total;    // bytes ever written (virtual size)

static char line_buf[KLOG_LINE_MAX];
static unsigned int line_len;
static int line_level = KLOG_INFO;
static unsigned int line_ticks;    // tick at the start of the current line
static int level_override;         // 1 = line level set explicitly (klog())

extern volatile unsigned int tick;

// Commit line_buf as "[tttttttt] L text\n" into the ring.
static void line_commit(void) {
    char out[KLOG_LINE_MAX + 16];
    const char *hex = "0123456789abcdef";
    unsigned int i = 0;
    out[i++] = '[';
    for (int s = 28; s >= 0; s -= 4)
        out[i++] = hex[(line_ticks >> s) & 0xF];
    out[i++] = ']';
    out[i++] = ' ';
    out[i++] = line_level == KLOG_INFO ? 'I'
             : line_level == KLOG_WARN ? 'W' : 'E';
    out[i++] = ' ';
    for (unsigned int k = 0; k < line_len; k++)
        out[i++] = line_buf[k];
    out[i++] = '\n';
    for (unsigned int k = 0; k < i; k++) {
        klog_ring[klog_pos] = out[k];
        klog_pos = (klog_pos + 1) % KLOG_SIZE;
        klog_total++;
    }
    line_len = 0;
}

void klog_putc(char c) {
    if (line_len == 0) {
        line_ticks = tick;
        if (!level_override) line_level = KLOG_INFO;
        level_override = 0;
    }
    if (c == '\n') { line_commit(); return; }
    if (line_len >= KLOG_LINE_MAX - 1)
        line_commit();
    line_buf[line_len++] = c;
}

// Minimal printf engine that writes to COM1 and the ring (leveled, no VGA).
static void kvputc(char c) {
    serial_putchar(c);
    klog_putc(c);
}

static void kprint_str(const char *s) {
    while (*s) kvputc(*s++);
}

static void kprint_uint(unsigned int n, unsigned int base, int upper) {
    static const char lower[] = "0123456789abcdef";
    static const char upperd[] = "0123456789ABCDEF";
    const char *digits = upper ? upperd : lower;
    char buf[12];
    int i = 0;
    if (n == 0) { kvputc('0'); return; }
    while (n) { buf[i++] = digits[n % base]; n /= base; }
    while (i > 0) kvputc(buf[--i]);
}

static void klog_vprintf(int level, const char *fmt, va_list ap) {
    level_override = 1;
    line_level = level;
    line_ticks = tick;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { kvputc(*p); continue; }
        char c = *++p;
        if (!c) break;
        switch (c) {
        case 'c': kvputc((char)va_arg(ap, int)); break;
        case 's': kprint_str(va_arg(ap, const char *)); break;
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) { kvputc('-'); v = -v; }
            kprint_uint((unsigned int)v, 10, 0);
            break;
        }
        case 'u': kprint_uint(va_arg(ap, unsigned int), 10, 0); break;
        case 'x': kprint_uint(va_arg(ap, unsigned int), 16, 0); break;
        case 'X': kprint_uint(va_arg(ap, unsigned int), 16, 1); break;
        case '%': kvputc('%'); break;
        default: kvputc('%'); kvputc(c); break;
        }
    }
}

void klog(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    klog_vprintf(level, fmt, ap);
    va_end(ap);
    kvputc('\n');
}

unsigned int klog_size(void) {
    return klog_total < KLOG_SIZE ? klog_total : KLOG_SIZE;
}

unsigned int klog_read(unsigned int off, void *buf, unsigned int len) {
    unsigned int flags;
    __asm__ volatile("pushfl; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    unsigned int total = klog_size();
    if (off >= total) len = 0;
    else if (len > total - off) len = total - off;
    unsigned int copied = len;
    unsigned int idx = (klog_pos + KLOG_SIZE - total + off) % KLOG_SIZE;
    for (unsigned int i = 0; i < copied; i++)
        ((char *)buf)[i] = klog_ring[(idx + i) % KLOG_SIZE];
    if (flags & 0x200) __asm__ volatile("sti");
    return copied;
}
```

- [ ] **Step 3: Wire `printf` into the ring**

In `kernel/printf.c`, add `#include "klog.h"` and extend `putc`:

```c
static void putc(char c) {
    vga_putchar(c);
    serial_putchar(c);
    klog_putc(c);
}
```

- [ ] **Step 4: Emit a deterministic boot marker**

In `kernel/kernel.c`, add `#include "klog.h"` at the top, and immediately after `config_load();` insert:

```c
    klog(KLOG_INFO, "klog: ready");
```

- [ ] **Step 5: Build**

Run: `make`
Expected: clean build, no new `-Wall -Wextra` warnings, `aos.iso` produced.

- [ ] **Step 6: Commit**

```bash
git add kernel/klog.h kernel/klog.c kernel/printf.c kernel/kernel.c
git commit -m "feat: kernel dmesg ring buffer (klog)"
```

Note for the human: a boot now emits `klog: ready` on COM1 (visible in the serial log), and every `printf` line is buffered into the 32 KB ring (nothing visible changes yet — the ring is only read via `/proc/klog` in Task 2).

---

### Task 2: `/proc/klog` + `klogtest.py`

**Files:**
- Modify: `kernel/procfs.c` (klog inode: `proc_files[]`, `proc_stat`, `proc_read_at`)
- Create: `scripts/klogtest.py`
- Modify: `Makefile:117` (add `klogtest` to `TESTS`)

**Interfaces:**
- Consumes: `klog_read` / `klog_size` from `kernel/klog.h` (Task 1).
- Produces: procfs inode 5 named `klog` visible in `/proc`; `cat /proc/klog` prints the ring. Used by Task 5's script convention; no other task depends on it.

- [ ] **Step 1: Write the failing test `scripts/klogtest.py`**

```python
#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-klog.sock"
SER = "/tmp/aos-klog.log"

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
    keys = {"\n": "ret", " ": "spc", "/": "slash"}
    for ch in text:
        key = keys.get(ch, ch) or ch
        hmp("sendkey " + key)
        time.sleep(0.04)

def read_log():
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
        end = time.time() + 30
        while time.time() < end and "klog: ready" not in read_log():
            time.sleep(0.5)
        if "klog: ready" not in read_log():
            raise AssertionError("boot marker 'klog: ready' not on serial")
        send_text("cat /proc/klog\n")
        end = time.time() + 20
        log = ""
        while time.time() < end:
            time.sleep(1)
            log = read_log()
            if "klog: ready" in log and log.count("[") > 10: break
        lines = [l for l in log.splitlines() if "[0" in l or "[1" in l or "[f" in l]
        import re
        tstamped = [l for l in log.splitlines() if re.match(r"^\[\w{8}\] [IWE] ", l)]
        if len(tstamped) < 5:
            raise AssertionError("cat /proc/klog produced few timestamped lines (%d)"
                                 % len(tstamped))
        if not any("GDT" in l or "PMM" in l for l in tstamped):
            raise AssertionError("klog missing a boot line (GDT/PMM)")
        if not any("klog: ready" in l for l in tstamped):
            raise AssertionError("klog missing the 'klog: ready' INFO line")
        if "KERNEL PANIC" in log:
            raise AssertionError("cat /proc/klog triggered a kernel panic")
        print("PASS: /proc/klog shows timestamped INFO boot lines")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
```

(On the Task-1 ISO this fails at `cat /proc/klog` with "File not found" — that is the expected red.)

- [ ] **Step 2: Add the `klog` procfs inode**

In `kernel/procfs.c`: add `#include "klog.h"`, `#define PROCFS_KLOG 5`, add `{ "klog", PROCFS_KLOG },` to `proc_files[]`, and special-case the two functions:

```c
static int proc_stat(struct vfs_fs *fs, unsigned int ino, struct aos_stat *st) {
    (void)fs;
    if (ino == PROCFS_ROOT) {
        st->type = 2;
        st->size = 0;
        st->mtime = 0;
        st->nlink = 1;
        return 0;
    }
    if (ino == PROCFS_KLOG) {
        st->type = 1;
        st->size = klog_size();
        st->mtime = 0;
        st->nlink = 1;
        return 0;
    }
    for (unsigned int i = 0; i < PROC_FILES; i++) {
        if (proc_files[i].ino == ino) {
            unsigned int len;
            proc_content(ino, &len);
            st->type = 1;
            st->size = len;
            st->mtime = 0;
            st->nlink = 1;
            return 0;
        }
    }
    return -1;
}
```

```c
static int proc_read_at(struct vfs_fs *fs, unsigned int ino, void *buf,
                        unsigned int len, unsigned int off) {
    (void)fs;
    if (ino == PROCFS_KLOG)
        return (int)klog_read(off, buf, len);
    unsigned int clen;
    char *content = proc_content(ino, &clen);
    if (off >= clen) return 0;
    if (len > clen - off) len = clen - off;
    for (unsigned int i = 0; i < len; i++)
        ((char *)buf)[i] = content[off + i];
    return (int)len;
}
```

`proc_lookup` / `proc_readdir` pick up `klog` automatically from `proc_files[]`; the `proc_stat` loop already covers the `proc_files[]` entries, and `proc_read_at`'s static path returns 0 for unknown inos.

- [ ] **Step 3: Add to the test suite**

In `Makefile`, change the `TESTS` line to:

```make
TESTS = ipctest manytest notepadtest sleeptest rngtest blktest virtiotest netlooptest rtctest configtest klogtest $(LINUX_TESTS)
```

- [ ] **Step 4: Build**

Run: `make`
Expected: clean build, no new warnings.

- [ ] **Step 5: Commit**

```bash
git add kernel/procfs.c scripts/klogtest.py Makefile
git commit -m "feat: /proc/klog and klogtest regression"
```

Hand to the human: `python3 scripts/klogtest.py` — expected PASS (boots, `cat /proc/klog`, asserts `[xxxxxxxx] I` boot lines incl. GDT/PMM and `klog: ready`).

---

### Task 3: per-task trace state, record points, tables, session dump

**Files:**
- Modify: `kernel/task.h:20-43` (`struct task` fields + `task_slot` decl)
- Modify: `kernel/task.c` (`task_slot`, `task_spawn` inherit+reclaim, `task_switch_kernel` exit free-rule)
- Create: `kernel/trace.h`, `kernel/trace.c`
- Modify: `kernel/syscall.c:139-152,579` (record points), `kernel/aos_gui.c:55`, `kernel/linux_syscall.c:96`
- Modify: `Makefile:24` (append `kernel/klog.o` … `kernel/trace.o` to `KERNEL_OBJS`)

**Interfaces:**
- Consumes: nothing user-visible yet; internal only.
- Produces (used by Tasks 4–5):
  - `struct task` gains `trace_on`, `trace_buf`, `trace_head`, `trace_count`, `trace_wrapped`, `trace_dumped` (all unsigned int, `trace_buf` is `unsigned char *`).
  - `struct task *task_slot(unsigned int i);` in `kernel/task.h` (returns `&tasks[i]` for `i < MAX_TASKS`, else 0).
  - `kernel/trace.h`: `struct trace_rec { unsigned int num, a0, a1, a2, a3, a4, ret; };`, `#define TRACE_MAX 512`, `void trace_record(struct registers *r);`, `void trace_finish(struct registers *r);`, `void trace_session_dump(void);`, `unsigned int trace_render_at(unsigned int pid, unsigned int off, void *dst, unsigned int cap, unsigned int *total_out);`.

- [ ] **Step 1: Add trace fields to `struct task`**

In `kernel/task.h`, after `char cwd[PATH_MAX];`:

```c
    unsigned int trace_on;        // 1 = record this task's syscalls
    unsigned char *trace_buf;     // kmalloc'd ring of struct trace_rec (lazy)
    unsigned int trace_head;      // next write slot (wraps)
    unsigned int trace_count;     // records written so far
    unsigned int trace_wrapped;   // 1 = the ring overwrote old records
    unsigned int trace_dumped;    // 1 = already printed by a strace session
```

And add to the declarations near `get_current_task`:

```c
// Task-table slot accessor (task_slot(i) == 0 for i >= MAX_TASKS).
struct task *task_slot(unsigned int i);
```

- [ ] **Step 2: `task.c` — accessor, inheritance, reclaim, exit free-rule**

Add at the end of `kernel/task.c` (near `get_current_task`):

```c
struct task *task_slot(unsigned int i) {
    return i < MAX_TASKS ? &tasks[i] : 0;
}
```

In `task_spawn`, before `memset(t, 0, sizeof(*t));` (line ~263), free a reused slot's abandoned buffer:

```c
    struct task *t = &tasks[pid];
    if (t->trace_buf) {
        kfree(t->trace_buf);       // abandoned trace from a previous owner
        t->trace_buf = 0;
    }
    memset(t, 0, sizeof(*t));
```

Immediately after `t->pid = pid;` add inheritance (this is `strace -f`):

```c
    t->trace_on = current_task->trace_on;
```

In `task_switch_kernel`, inside the `if (exited)` cleanup block (after `kfree(dead->args);`), add the keep-vs-free rule:

```c
        // Trace buffer: a traced task's log is collected by the strace session
        // that owns it. Free it here when already dumped or untraced; a
        // traced-but-undumped zombie keeps it so trace_session_dump (or slot
        // reclamation) can still collect it.
        if (dead->trace_dumped || !dead->trace_on) {
            if (dead->trace_buf) kfree(dead->trace_buf);
            dead->trace_buf = 0;
        }
```

- [ ] **Step 3: Create `kernel/trace.h`**

```c
#ifndef TRACE_H
#define TRACE_H

// Syscall tracing (strace). Per-task state lives in struct task (trace_on,
// trace_buf, ...); the three syscall dispatch handlers wrap themselves with
// trace_record()/trace_finish(). Rendering (name tables, hex args) and the
// shell/`/proc/<pid>/trace` dump live here.

struct registers;
struct task;

#define TRACE_MAX 512

struct trace_rec {
    unsigned int num;
    unsigned int a0, a1, a2, a3, a4;
    unsigned int ret;
};

// Entry/exit record points for the three ABI handlers.
void trace_record(struct registers *r);
void trace_finish(struct registers *r);

// Print (to the kernel terminal) the trace of every traced, undumped task in
// pid order and mark them dumped. Freed here only for dead tasks.
void trace_session_dump(void);

// Render task `pid`'s ring starting at virtual offset `off` into `dst` (up to
// `cap` bytes); returns bytes copied (0 at/past the end or untraced). When
// `total_out` is non-NULL it receives the full virtual size.
unsigned int trace_render_at(unsigned int pid, unsigned int off, void *dst,
                             unsigned int cap, unsigned int *total_out);

#endif
```

- [ ] **Step 4: Create `kernel/trace.c`**

```c
#include "trace.h"
#include "task.h"
#include "terminal.h"
#include "kmm.h"
#include "interrupts.h"

struct trace_name {
    const char *name;
    unsigned char nargs;
};

// ---- AOS ABI (int 0x80, syscall_handler switch): dense 0..48 ----
static const struct trace_name aos_names[49] = {
    [0]  = { "print", 1 },
    [1]  = { "print_hex", 1 },
    [2]  = { "print_dec", 1 },
    [3]  = { "putchar", 1 },
    [10] = { "tick", 0 },
    [11] = { "clear", 0 },
    [12] = { "reboot", 0 },
    [13] = { "panic", 0 },
    [14] = { "shutdown", 0 },
    [15] = { "get_args", 2 },
    [16] = { "exit", 1 },
    [17] = { "read_key", 0 },
    [18] = { "yield", 0 },
    [19] = { "getpid", 0 },
    [20] = { "send", 5 },
    [21] = { "recv", 1 },
    [22] = { "event", 0 },
    [23] = { "mouse", 4 },
    [24] = { "fb_info", 5 },
    [25] = { "text", 1 },
    [26] = { "fill", 1 },
    [27] = { "setout", 1 },
    [28] = { "spawn", 3 },
    [29] = { "getevent", 0 },
    [30] = { "sleep", 1 },
    [31] = { "waitpid", 1 },
    [32] = { "get_children", 2 },
    [33] = { "random", 2 },
    [34] = { "rtc", 1 },
    [35] = { "uptime", 0 },
    [36] = { "open", 2 },
    [37] = { "close", 1 },
    [38] = { "read", 3 },
    [39] = { "write", 3 },
    [40] = { "lseek", 3 },
    [41] = { "mkdir", 1 },
    [42] = { "rmdir", 1 },
    [43] = { "readdir", 3 },
    [44] = { "chdir", 1 },
    [45] = { "getcwd", 2 },
    [46] = { "stat", 2 },
    [47] = { "fstat", 2 },
    [48] = { "unlink", 1 },
};

// ---- AOS_EXT (500-519, aos_gui_handler) ----
static const struct trace_name aos_ext_names[20] = {
    [0]  = { "fb_info", 5 },
    [1]  = { "text", 1 },
    [2]  = { "fill", 1 },
    [3]  = { "clear", 0 },
    [4]  = { "mouse", 4 },
    [5]  = { "read_key", 0 },
    [6]  = { "key_poll", 0 },
    [7]  = { "reg_events", 0 },
    [8]  = { "get_event_pid", 0 },
    [9]  = { "send", 2 },
    [10] = { "recv", 1 },
    [11] = { "setout", 1 },
    [12] = { "spawn", 3 },
    [13] = { "waitpid", 1 },
    [14] = { "get_children", 2 },
    [15] = { "get_args", 2 },
    [16] = { "get_rtc", 1 },
    [17] = { "uptime", 0 },
    [18] = { "get_tick", 0 },
    [19] = { "panic", 0 },
};

// ---- Linux ABI (linux_syscall_handler): sparse, ascending ----
struct trace_lin {
    unsigned int num;
    const char *name;
    unsigned char nargs;
};

static const struct trace_lin linux_names[] = {
    { 1, "exit", 1 },          { 3, "read", 3 },
    { 4, "write", 3 },         { 5, "open", 3 },
    { 6, "close", 1 },         { 10, "unlink", 1 },
    { 12, "chdir", 1 },        { 13, "time", 1 },
    { 19, "lseek", 3 },        { 20, "getpid", 0 },
    { 24, "getuid", 0 },       { 33, "access", 2 },
    { 39, "mkdir", 2 },        { 40, "rmdir", 1 },
    { 45, "brk", 1 },          { 47, "getgid", 0 },
    { 49, "geteuid", 0 },      { 50, "getegid", 0 },
    { 54, "ioctl", 3 },        { 78, "gettimeofday", 2 },
    { 88, "reboot", 3 },       { 91, "munmap", 2 },
    { 123, "modify_ldt", 3 },  { 125, "mprotect", 3 },
    { 140, "_llseek", 5 },     { 146, "writev", 3 },
    { 162, "nanosleep", 2 },   { 183, "getcwd", 2 },
    { 192, "mmap2", 6 },       { 195, "stat64", 2 },
    { 197, "fstat64", 2 },     { 220, "getdents64", 3 },
    { 243, "set_thread_area", 1 }, { 252, "exit_group", 1 },
    { 258, "set_tid_address", 1 }, { 265, "clock_gettime", 2 },
    { 295, "openat", 4 },      { 300, "fstatat64", 4 },
    { 355, "getrandom", 3 },
};
#define NLIN (sizeof(linux_names) / sizeof(linux_names[0]))

#define AOS_EXT_BASE 500

// AOS_EXT range wins over the task ABI (musl GUI programs are ABI_LINUX but
// issue 500-519 GUI syscalls).
static const struct trace_name *trace_name_for(const struct task *t,
                                               unsigned int num) {
    if (num >= AOS_EXT_BASE && num < AOS_EXT_BASE + 100)
        return num - AOS_EXT_BASE < 20 ? &aos_ext_names[num - AOS_EXT_BASE] : 0;
    if (t->abi == ABI_LINUX) {
        for (unsigned int i = 0; i < NLIN; i++)
            if (linux_names[i].num == num) return (const struct trace_name *)&linux_names[i];
        return 0;
    }
    if (num < 49 && aos_names[num].name) return &aos_names[num];
    return 0;
}
```

Note: `trace_name_for` returns `const struct trace_name *` for the dense tables and casts the sparse `struct trace_lin *` to it — both start with `(const char *name)`. To keep the compiler happy with `-Wall -Wextra` (the two structs differ), declare a single shared `struct trace_name` for all three tables and give the Linux table `struct trace_name` entries (dropping the `num` field and searching by index — see `linux_name()` below). Replace the Linux table + lookup with:

```c
static const struct trace_name linux_names[] = {
    { "exit", 1 },          { "read", 3 },
    { "write", 3 },         { "open", 3 },
    { "close", 1 },         { "unlink", 1 },
    { "chdir", 1 },         { "time", 1 },
    { "lseek", 3 },         { "getpid", 0 },
    { "getuid", 0 },        { "access", 2 },
    { "mkdir", 2 },         { "rmdir", 1 },
    { "brk", 1 },           { "getgid", 0 },
    { "geteuid", 0 },       { "getegid", 0 },
    { "ioctl", 3 },         { "gettimeofday", 2 },
    { "reboot", 3 },        { "munmap", 2 },
    { "modify_ldt", 3 },    { "mprotect", 3 },
    { "_llseek", 5 },       { "writev", 3 },
    { "nanosleep", 2 },     { "getcwd", 2 },
    { "mmap2", 6 },         { "stat64", 2 },
    { "fstat64", 2 },       { "getdents64", 3 },
    { "set_thread_area", 1 }, { "exit_group", 1 },
    { "set_tid_address", 1 }, { "clock_gettime", 2 },
    { "openat", 4 },        { "fstatat64", 4 },
    { "getrandom", 3 },
};
static const unsigned int linux_nums[] = {
    1, 3, 4, 5, 6, 10, 12, 13, 19, 20, 24, 33, 39, 40, 45, 47, 49, 50,
    54, 78, 88, 91, 123, 125, 140, 146, 162, 183, 192, 195, 197, 220,
    243, 252, 258, 265, 295, 300, 355,
};
#define NLIN (sizeof(linux_nums) / sizeof(linux_nums[0]))

static const struct trace_name *linux_name(unsigned int num) {
    for (unsigned int i = 0; i < NLIN; i++)
        if (linux_nums[i] == num) return &linux_names[i];
    return 0;
}

static const struct trace_name *trace_name_for(const struct task *t,
                                               unsigned int num) {
    if (num >= AOS_EXT_BASE && num < AOS_EXT_BASE + 100)
        return num - AOS_EXT_BASE < 20 ? &aos_ext_names[num - AOS_EXT_BASE] : 0;
    if (t->abi == ABI_LINUX)
        return linux_name(num);
    if (num < 49 && aos_names[num].name) return &aos_names[num];
    return 0;
}
```

Continue `kernel/trace.c`:

```c
static unsigned int hex_str(unsigned int v, char *dst, unsigned int cap) {
    const char *hex = "0123456789abcdef";
    char tmp[16];
    unsigned int n = 0;
    unsigned int started = 0;
    for (int s = 28; s >= 0; s -= 4) {
        unsigned int d = (v >> s) & 0xF;
        if (d || started || s == 0) { tmp[n++] = hex[d]; started = 1; }
    }
    if (n > cap) n = cap;
    for (unsigned int i = 0; i < n; i++) dst[i] = tmp[i];
    return n;
}

static unsigned int dec_str(unsigned int v, char *dst, unsigned int cap) {
    char tmp[16];
    unsigned int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    unsigned int out = 0;
    for (unsigned int i = n; i > 0 && out < cap;) {
        i--;
        dst[out++] = tmp[i];
    }
    return out;
}

// Render one record as "name(a0, a1, ...) = 0xret\n" into dst (dcap bytes).
// Exactly nargs args are printed. Returns characters written (NUL-terminated).
// Lines fit comfortably in 96 bytes (longest ~90), so no truncation occurs.
static unsigned int emit_rec(const struct task *t, const struct trace_rec *rec,
                             char *dst, unsigned int dcap) {
    const struct trace_name *tn = trace_name_for(t, rec->num);
    unsigned int i = 0;
    if (tn) {
        const char *nm = tn->name;
        while (*nm && i + 1 < dcap) dst[i++] = *nm++;
    } else {
        const char *p = "syscall_0x";
        while (*p && i + 1 < dcap) dst[i++] = *p++;
        i += hex_str(rec->num, dst + i, dcap - i);
    }
    if (i + 1 < dcap) dst[i++] = '(';
    unsigned int nargs = tn ? tn->nargs : 5;
    unsigned int args[5] = { rec->a0, rec->a1, rec->a2, rec->a3, rec->a4 };
    for (unsigned int k = 0; k < nargs; k++) {
        if (k) {
            if (i + 1 < dcap) dst[i++] = ',';
            if (i + 1 < dcap) dst[i++] = ' ';
        }
        if (i + 2 < dcap) dst[i++] = '0';
        if (i + 2 < dcap) dst[i++] = 'x';
        i += hex_str(args[k], dst + i, dcap - i);
    }
    if (i + 1 < dcap) dst[i++] = ')';
    const char *eq = " = 0x";
    while (*eq && i + 1 < dcap) dst[i++] = *eq++;
    i += hex_str(rec->ret, dst + i, dcap - i);
    if (i + 1 < dcap) dst[i++] = '\n';
    if (i < dcap) dst[i] = '\0';
    return i;
}

static unsigned int emit_header(unsigned int pid, char *line, unsigned int cap) {
    const char *h = "== pid ";
    unsigned int n = 0;
    while (*h && n + 1 < cap) line[n++] = *h++;
    n += dec_str(pid, line + n, cap - n - 1);
    if (n + 3 < cap) { line[n++] = ' '; line[n++] = '='; line[n++] = '='; line[n++] = '\n'; }
    return n;
}

static unsigned int emit_overwritten(unsigned int lost, char *line,
                                     unsigned int cap) {
    const char *a = "... ";
    const char *b = " records overwritten\n";
    unsigned int n = 0;
    while (*a && n + 1 < cap) line[n++] = *a++;
    n += dec_str(lost, line + n, cap - n - 1);
    while (*b && n + 1 < cap) line[n++] = *b++;
    return n;
}
```

Record / finish (the ring is written only by the owning task's own syscall
context — single CPU, IF=0 — so no locking is needed):

```c
void trace_record(struct registers *r) {
    struct task *t = get_current_task();
    if (!t->trace_on) return;
    if (!t->trace_buf) {
        t->trace_buf = kmalloc(TRACE_MAX * sizeof(struct trace_rec));
        if (!t->trace_buf) { t->trace_on = 0; return; }
        t->trace_head = 0;
        t->trace_count = 0;
        t->trace_wrapped = 0;
    }
    struct trace_rec *ring = (struct trace_rec *)t->trace_buf;
    struct trace_rec *rec = &ring[t->trace_head];
    rec->num = r->eax;
    rec->a0 = r->ebx;
    rec->a1 = r->ecx;
    rec->a2 = r->edx;
    rec->a3 = r->esi;
    rec->a4 = r->edi;
    rec->ret = 0;
    t->trace_head = (t->trace_head + 1) % TRACE_MAX;
    if (t->trace_count < TRACE_MAX)
        t->trace_count++;
    else
        t->trace_wrapped = 1;
}

void trace_finish(struct registers *r) {
    struct task *t = get_current_task();
    if (!t->trace_on || !t->trace_buf) return;
    unsigned int slot = (t->trace_head + TRACE_MAX - 1) % TRACE_MAX;
    ((struct trace_rec *)t->trace_buf)[slot].ret = r->eax;
}
```

Session dump (shell path: uses `terminal_write`, NOT `printf`, so the dump does
not feed back into the klog ring):

```c
static void dump_one(struct task *t) {
    if (!t->trace_on || !t->trace_buf || t->trace_dumped) return;
    char line[96];
    unsigned int n = emit_header(t->pid, line, sizeof(line));
    terminal_write(line, n);
    unsigned int total = t->trace_wrapped ? TRACE_MAX : t->trace_count;
    unsigned int start = t->trace_wrapped ? t->trace_head : 0;
    for (unsigned int i = 0; i < total; i++) {
        const struct trace_rec *rec =
            (const struct trace_rec *)t->trace_buf + (start + i) % TRACE_MAX;
        n = emit_rec(t, rec, line, sizeof(line));
        terminal_write(line, n);
    }
    if (t->trace_wrapped) {
        n = emit_overwritten(t->trace_count - TRACE_MAX, line, sizeof(line));
        terminal_write(line, n);
    }
    t->trace_dumped = 1;
    // Only dead tasks lose their buffer here; a live child keeps tracing so
    // /proc/<pid>/trace keeps working (its buffer is freed on task exit).
    if (t->state == TASK_ZOMBIE || t->state == TASK_FREE) {
        kfree(t->trace_buf);
        t->trace_buf = 0;
    }
}

void trace_session_dump(void) {
    for (unsigned int i = 0; i < MAX_TASKS; i++) {
        struct task *t = task_slot(i);
        if (t) dump_one(t);
    }
}
```

Live render (procfs path; runs in the reader's syscall context, IF=0, so the
copy is atomic on this single CPU):

```c
struct render_ctx {
    char *dst;
    unsigned int cap;
    unsigned int off;
    unsigned int total;
    unsigned int copied;
};

static void rpush(struct render_ctx *c, const char *s, unsigned int n) {
    unsigned int start = c->total;
    c->total += n;
    if (c->total <= c->off) return;
    unsigned int skip = c->off > start ? c->off - start : 0;
    unsigned int take = n - skip;
    if (take > c->cap - c->copied) take = c->cap - c->copied;
    for (unsigned int i = 0; i < take; i++)
        c->dst[c->copied + i] = s[skip + i];
    c->copied += take;
}

unsigned int trace_render_at(unsigned int pid, unsigned int off, void *dst,
                             unsigned int cap, unsigned int *total_out) {
    struct task *t = pid < MAX_TASKS ? task_slot(pid) : 0;
    if (!t || !t->trace_on || !t->trace_buf) {
        if (total_out) *total_out = 0;
        return 0;
    }
    struct render_ctx c = { (char *)dst, cap, off, 0, 0 };
    char line[96];
    unsigned int n = emit_header(t->pid, line, sizeof(line));
    rpush(&c, line, n);
    unsigned int total = t->trace_wrapped ? TRACE_MAX : t->trace_count;
    unsigned int start = t->trace_wrapped ? t->trace_head : 0;
    for (unsigned int i = 0; i < total; i++) {
        const struct trace_rec *rec =
            (const struct trace_rec *)t->trace_buf + (start + i) % TRACE_MAX;
        n = emit_rec(t, rec, line, sizeof(line));
        rpush(&c, line, n);
    }
    if (t->trace_wrapped) {
        n = emit_overwritten(t->trace_count - TRACE_MAX, line, sizeof(line));
        rpush(&c, line, n);
    }
    if (total_out) *total_out = c.total;
    return c.copied;
}
```

- [ ] **Step 5: Add the record points to the three handlers**

`kernel/syscall.c` — add `#include "trace.h"` and wrap the AOS path (the
`AOS_EXT`/`ABI_LINUX` paths return early, so they are wrapped inside their own
handlers):

```c
    if (task_current_abi() == ABI_LINUX) {
        linux_syscall_handler(r);
        return;
    }

    trace_record(r);
    switch (n) {
    ...
    }
    trace_finish(r);
```

`kernel/aos_gui.c` — add `#include "trace.h"` and wrap `aos_gui_handler` (it
is single-entry/single-exit):

```c
void aos_gui_handler(struct registers *r) {
    unsigned int n = r->eax;
    trace_record(r);
    switch (n) {
    ...
    }
    trace_finish(r);
}
```

`kernel/linux_syscall.c` — add `#include "trace.h"` and wrap
`linux_syscall_handler` identically.

- [ ] **Step 6: Add the objects to the build**

In `Makefile`, append to `KERNEL_OBJS` (line ~24):

```make
              kernel/task.o kernel/linux_syscall.o kernel/block.o kernel/sfs2.o \
              kernel/klog.o kernel/trace.o
```

- [ ] **Step 7: Build**

Run: `make`
Expected: clean build, no new warnings. (Nothing is observable yet — the
record points run only when `trace_on` is set, which Task 4 wires up.)

- [ ] **Step 8: Commit**

```bash
git add kernel/task.h kernel/task.c kernel/trace.h kernel/trace.c \
        kernel/syscall.c kernel/aos_gui.c kernel/linux_syscall.c Makefile
git commit -m "feat: per-task syscall trace recording with name tables"
```

---

### Task 4: `strace` shell built-in + `bgspawn` test program

**Files:**
- Modify: `kernel/commands.c` (refactor `try_exec`/PATH search, add `cmd_strace`)
- Create: `programs/musl/bgspawn.c`
- Modify: `Makefile:23` (append `bgspawn` to `PROGRAMS`)

**Interfaces:**
- Consumes: `trace_session_dump()` and `struct task::trace_on` (Task 3).
- Produces: `strace <prog> [args]` shell built-in. Also produces embedded
  `bin/bgspawn` (spawns `bin/clock` with sink=1, prints `bgspawn: clock pid=%d`, exits) for the Task 5 live test.

- [ ] **Step 1: Refactor the exec path in `kernel/commands.c`**

Add `#include "trace.h"`. Change `try_exec` to take a trace flag and clear the
flag + dump on return:

```c
static int try_exec(const char *full_path, const char *arg, int trace) {
    struct task *me = get_current_task();
    if (trace) me->trace_on = 1;
    void (*entry)(void) = program_load(full_path, arg);
    if (entry) {
        if (task_current_abi() == ABI_LINUX)
            user_program_start_linux(entry, task_current_lctx()->stack_sp);
        else
            user_program_start(entry);
        if (trace) {
            trace_session_dump();
            me->trace_on = 0;
        }
        terminal_set_prompt();
        return 1;
    }
    if (trace) me->trace_on = 0;
    terminal_print("\nFailed to load: ");
    terminal_print(full_path);
    return 0;
}
```

Extract the PATH search + raw-path fallback out of `commands_execute` into a
helper (this is exactly the code currently at `commands.c:161-197`, with the
`try_exec` calls passing `trace`):

```c
// Locate `cmd` in the PATH (or as a raw path) and execute it in-place.
// Returns 1 if a program was found and ran to its exit.
static int exec_from_path(const char *cmd, const char *arg, int trace) {
    struct aos_stat st2;
    char path_copy[PATH_MAX];
    strncpy(path_copy, command_path, PATH_MAX - 1);
    path_copy[PATH_MAX - 1] = '\0';

    char *dir = path_copy;
    while (*dir) {
        char *next = dir;
        while (*next && *next != ':') next++;
        int dir_len = next - dir;
        int has_sep = (*next == ':');
        *next = '\0';

        if (dir_len > 0) {
            char full_path[32];
            int i;
            for (i = 0; i < dir_len && i < 30; i++)
                full_path[i] = dir[i];
            if (i < 31) {
                full_path[i++] = '/';
                for (unsigned int j = 0; cmd[j] && i < 31; j++, i++)
                    full_path[i] = cmd[j];
            }
            full_path[i] = '\0';

            if (vfs_kernel_stat(full_path, &st2) == 0)
                if (try_exec(full_path, arg, trace)) return 1;
        }

        if (!has_sep) break;
        dir = next + 1;
    }

    if (vfs_kernel_stat(cmd, &st2) == 0)
        if (try_exec(cmd, arg, trace)) return 1;
    return 0;
}
```

In `commands_execute`, replace the inline PATH search block (currently
`commands.c:161-197`, from `struct aos_stat st2;` through the raw-path
fallback) with:

```c
    if (exec_from_path(cmd, arg, 0)) return;
```

Add the `strace` built-in check before that (next to the other built-ins, e.g.
after the `pwd` block):

```c
    if (strcmp(cmd, "strace") == 0) {
        cmd_strace(arg);
        terminal_set_prompt();
        return;
    }
```

And add the command implementation above `commands_execute`:

```c
// strace <prog> [args]: run <prog> in-place with this task's syscalls traced,
// then dump the trace of this task and every task that inherited the flag.
static void cmd_strace(const char *line) {
    const char *p = line;
    while (*p && *p != ' ') p++;
    unsigned int plen = (unsigned int)(p - line);
    while (*p == ' ') p++;
    if (plen == 0) {
        terminal_print("\nusage: strace <prog> [args]");
        return;
    }
    char prog[16];
    unsigned int cl = plen < 15 ? plen : 15;
    for (unsigned int i = 0; i < cl; i++) prog[i] = line[i];
    prog[cl] = '\0';
    exec_from_path(prog, p, 1);
}
```

- [ ] **Step 2: Create `programs/musl/bgspawn.c`**

```c
#include <stdio.h>
#include "aosabi.h"

// Test helper for stracelive.py: spawn bin/clock (a long-running GUI task)
// with sink=1 (the WM) and exit immediately. The child inherits the parent's
// trace_on, so /proc/<pid>/trace shows its live AOS_EXT syscall stream.
int main(void) {
    int child = aos_spawn("bin/clock", "", 1);
    printf("bgspawn: clock pid=%d\n", child);
    return 0;
}
```

- [ ] **Step 3: Register the program**

In `Makefile`, append `bgspawn` to the `PROGRAMS` list (line 23). `gen_progs.py`
auto-embeds it as `bin/bgspawn` on the next build.

- [ ] **Step 4: Build**

Run: `make`
Expected: clean build, no new warnings; `bin/bgspawn` embedded.

- [ ] **Step 5: Commit**

```bash
git add kernel/commands.c programs/musl/bgspawn.c Makefile
git commit -m "feat: strace shell built-in and bgspawn test program"
```

---

### Task 5: `/proc/<pid>/trace` + regression scripts

**Files:**
- Modify: `kernel/procfs.c` (pid pseudo-dirs + trace files in `proc_lookup`, `proc_readdir`, `proc_stat`, `proc_read_at`)
- Create: `scripts/stracetest.py`, `scripts/stracelive.py`
- Modify: `Makefile:117` (add `stracetest stracelive` to `TESTS`)

**Interfaces:**
- Consumes: `trace_render_at` (Task 3), `task_slot` + `TASK_FREE`/`TASK_ZOMBIE` (task.h), `strace` built-in + `bin/bgspawn` (Task 4), `bin/linrun` (existing).
- Produces: `/proc/<pid>` pseudo-directories (decimal task names) each containing `trace`; `cat /proc/<pid>/trace` renders the live ring of a traced task.

- [ ] **Step 1: Write the failing test `scripts/stracetest.py`**

```python
#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-strace.sock"
SER = "/tmp/aos-strace.log"

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
    keys = {"\n": "ret", " ": "spc", "/": "slash"}
    for ch in text:
        key = keys.get(ch, ch) or ch
        hmp("sendkey " + key)
        time.sleep(0.04)

def read_log():
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
        send_text("strace linrun\n")
        end = time.time() + 25
        log = ""
        while time.time() < end:
            time.sleep(1)
            log = read_log()
            if "== pid 0 ==" in log and "== pid 2 ==" in log: break
        tail = log[-900:]
        if "== pid 0 ==" not in log:
            raise AssertionError("strace session did not dump == pid 0 ==\n" + tail)
        if "== pid 2 ==" not in log:
            raise AssertionError("strace did not collect the spawned child (lin/hello is pid 2)\n" + tail)
        for probe in ("spawn(", "write(", "exit_group("):
            if probe not in log:
                raise AssertionError("strace output missing %r\n%s" % (probe, tail))
        if "KERNEL PANIC" in log:
            raise AssertionError("strace linrun triggered a kernel panic")
        print("PASS: strace shows AOS_EXT spawn + linux write/exit_group, child collected")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
```

(On the Task-4 ISO this fails: `strace` is an unknown command — expected red.)

- [ ] **Step 2: Write the failing live test `scripts/stracelive.py`**

```python
#!/usr/bin/env python3
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-strace-live.sock"
SER = "/tmp/aos-strace-live.log"

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
    keys = {"\n": "ret", " ": "spc", "/": "slash"}
    for ch in text:
        key = keys.get(ch, ch) or ch
        hmp("sendkey " + key)
        time.sleep(0.04)

def read_log():
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
        send_text("strace bgspawn\n")
        end = time.time() + 20
        pid = None
        log = ""
        while time.time() < end:
            time.sleep(1)
            log = read_log()
            m = re.search(r"bgspawn: clock pid=(\d+)", log)
            if m and "== pid 0 ==" in log:
                pid = m.group(1)
                break
        if pid is None:
            raise AssertionError("strace bgspawn did not report a spawned clock pid\n" + log[-900:])
        time.sleep(2)                                # let clock repaint once
        send_text("cat /proc/%s/trace\n" % pid)
        end = time.time() + 20
        log = ""
        while time.time() < end:
            time.sleep(1)
            log = read_log()
            if ("== pid %s ==" % pid) in log and "fill(" in log: break
        tail = log[-900:]
        if ("== pid %s ==" % pid) not in log:
            raise AssertionError("cat /proc/%s/trace did not dump the live trace\n%s" % (pid, tail))
        for probe in ("fill(", "text(", "send("):
            if probe not in log:
                raise AssertionError("live trace missing %r\n%s" % (probe, tail))
        if "KERNEL PANIC" in log:
            raise AssertionError("kernel panic during live /proc trace read")
        print("PASS: /proc/%s/trace shows live AOS_EXT fill/text/send" % pid)
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Implement pid pseudo-dirs in `kernel/procfs.c`**

Add includes (`"klog.h"`, `"task.h"`) and inode macros:

```c
#define PROCFS_KLOG     5
#define PROCFS_PID_BASE   0x1000
#define PROCFS_TRACE_BASE 0x2000
#define PROCFS_PID_DIR(pid)   (PROCFS_PID_BASE + (pid))
#define PROCFS_TRACE(pid)     (PROCFS_TRACE_BASE + (pid))
```

`proc_lookup` — decimal names under root resolve to pid dirs; a pid dir
resolves `trace`:

```c
static int proc_lookup(struct vfs_fs *fs, unsigned int dir_ino,
                       const char *name, unsigned int *out_ino) {
    (void)fs;
    if (dir_ino != PROCFS_ROOT) {
        if (dir_ino >= PROCFS_PID_BASE && dir_ino < PROCFS_PID_BASE + MAX_TASKS) {
            if (strcmp(name, "trace") == 0) {
                *out_ino = PROCFS_TRACE(dir_ino - PROCFS_PID_BASE);
                return 0;
            }
            return VFS_ENOENT;
        }
        return VFS_ENOTDIR;
    }
    for (unsigned int i = 0; i < PROC_FILES; i++) {
        if (strcmp(proc_files[i].name, name) == 0) {
            *out_ino = proc_files[i].ino;
            return 0;
        }
    }
    unsigned int pid = 0;
    const char *p = name;
    while (*p >= '0' && *p <= '9') {
        pid = pid * 10 + (unsigned int)(*p - '0');
        p++;
    }
    if (*p == '\0' && pid > 0 && pid < MAX_TASKS) {
        struct task *t = task_slot(pid);
        if (t && t->state != TASK_FREE) {
            *out_ino = PROCFS_PID_DIR(pid);
            return 0;
        }
    }
    return VFS_ENOENT;
}
```

`proc_readdir` — static files first, then live tasks as decimal dirs; a pid
dir lists `trace`:

```c
static int proc_readdir(struct vfs_fs *fs, unsigned int dir_ino,
                        unsigned int idx, char *name_out,
                        unsigned int *ino_out) {
    (void)fs;
    if (dir_ino == PROCFS_ROOT) {
        if (idx < PROC_FILES) {
            const struct proc_file *pf = &proc_files[idx];
            unsigned int i = 0;
            while (pf->name[i] && i < VFS_NAME_MAX) {
                name_out[i] = pf->name[i];
                i++;
            }
            name_out[i] = '\0';
            if (ino_out) *ino_out = pf->ino;
            return 1;
        }
        unsigned int k = idx - PROC_FILES;
        for (unsigned int pid = 1; pid < MAX_TASKS; pid++) {
            struct task *t = task_slot(pid);
            if (!t || t->state == TASK_FREE || t->state == TASK_ZOMBIE) continue;
            if (k-- != 0) continue;
            char tmp[12];
            unsigned int n = 0;
            unsigned int v = pid;
            while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
            unsigned int i = 0;
            for (unsigned int j = n; j > 0 && i < VFS_NAME_MAX;) {
                j--;
                name_out[i++] = tmp[j];
            }
            name_out[i] = '\0';
            if (ino_out) *ino_out = PROCFS_PID_DIR(pid);
            return 1;
        }
        return 0;
    }
    if (dir_ino >= PROCFS_PID_BASE && dir_ino < PROCFS_PID_BASE + MAX_TASKS) {
        if (idx == 0) {
            unsigned int i = 0;
            const char *nm = "trace";
            while (nm[i] && i < VFS_NAME_MAX) { name_out[i] = nm[i]; i++; }
            name_out[i] = '\0';
            if (ino_out) *ino_out = PROCFS_TRACE(dir_ino - PROCFS_PID_BASE);
            return 1;
        }
        return 0;
    }
    return VFS_ENOTDIR;
}
```

`proc_stat` — add the klog branch (already in Task 2), plus pid-dir and trace
branches before the static-file loop:

```c
    if (ino >= PROCFS_PID_BASE && ino < PROCFS_PID_BASE + MAX_TASKS) {
        st->type = 2;
        st->size = 0;
        st->mtime = 0;
        st->nlink = 1;
        return 0;
    }
    if (ino >= PROCFS_TRACE_BASE && ino < PROCFS_TRACE_BASE + MAX_TASKS) {
        unsigned int sz = 0;
        trace_render_at(ino - PROCFS_TRACE_BASE, 0, 0, 0, &sz);
        st->type = 1;
        st->size = sz;
        st->mtime = 0;
        st->nlink = 1;
        return 0;
    }
```

`proc_read_at` — add the trace branch next to the klog branch:

```c
    if (ino == PROCFS_KLOG)
        return (int)klog_read(off, buf, len);
    if (ino >= PROCFS_TRACE_BASE && ino < PROCFS_TRACE_BASE + MAX_TASKS)
        return (int)trace_render_at(ino - PROCFS_TRACE_BASE, off, buf, len, 0);
```

- [ ] **Step 4: Add to the test suite**

In `Makefile`, extend the `TESTS` line:

```make
TESTS = ipctest manytest notepadtest sleeptest rngtest blktest virtiotest netlooptest rtctest configtest klogtest stracetest stracelive $(LINUX_TESTS)
```

- [ ] **Step 5: Build**

Run: `make`
Expected: clean build, no new warnings. Check `ls /proc` still lists
`uptime version mounts klog` (plus pid dirs) — `lindirtest.py`'s "added >= 3
rows" assertion is a lower bound, so the extra entries cannot regress it.

- [ ] **Step 6: Commit**

```bash
git add kernel/procfs.c scripts/stracetest.py scripts/stracelive.py Makefile
git commit -m "feat: /proc/<pid>/trace live dump and strace regression scripts"
```

- [ ] **Step 7: Hand the full suite to the human**

```bash
make test        # human runs this
```

Expected: all existing tests green (`manytest`, `ipctest`, `notepadtest`,
`sleeptest`, `rngtest`, `blktest`, `virtiotest`, `netlooptest`, `rtctest`,
`configtest`, `linhello`, `lincat`, `lindirtest`) **and** the new
`klogtest`, `stracetest`, `stracelive`.

---

## Self-review notes (checked against the spec)

- Spec §1 (klog ring, `[tick] L text`, printf→INFO, `klog()` WARN/ERR COM1-only,
  `/proc/klog` non-destructive offset read) → Tasks 1–2. Overflow silently
  overwrites; `klog_size` caps the virtual size — matches spec.
- Spec §2 (per-task `trace_on`/ring, inheritance = `strace -f`, record points in
  all three handlers, `== pid N ==` dump, live `/proc/<pid>/trace`) → Tasks 3–5.
  Name-lookup AOS_EXT-first rule is the key correctness point for musl GUI
  programs (ABI_LINUX tasks issuing 500+ GUI syscalls).
- Spec §3 (errors/tests): kmalloc failure silently clears `trace_on`; unknown
  pid → `VFS_ENOENT`; untraced pid → empty; `... N records overwritten`;
  `strace` usage on empty args; `klogtest`/`stracetest`/`stracelive` written by
  the assistant, run by the human → Tasks 2/4/5.
- Placeholder scan: every step has concrete code; no TBD/TODO.
- Type consistency: `trace_record`/`trace_finish`/`trace_session_dump`/
  `trace_render_at` signatures match between Task 3 and Task 5; `task_slot`
  used consistently; `struct trace_rec` layout is the same in `trace.h` and
  `trace.c`.
