# Temporary Kernel Buffers on `kmalloc` — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the remaining static scratch buffers in the kernel (`user_str`, `user_str2`, `prog_args`, `lin_str`, SFS sector buffers) with `kmalloc` allocations so string sizes are no longer hard-capped and the syscall layer holds no shared mutable state.

**Architecture:** `copy_user_str*`/`copy_lin_str` become allocating helpers that measure the user string (bounded by the user window), `kmalloc(len+1)`, copy, and return a buffer the caller must `kfree`. `prog_args` becomes a lazily kmalloc'd persistent buffer for task 0. SFS sector scratches are per-call `kmalloc(512)`.

**Tech Stack:** Freestanding C11, x86 i386, QEMU headless regression (`make test`).

**Spec:** `docs/superpowers/specs/2026-08-04-temp-buffers-kmalloc-design.md`

## Global Constraints

- Build with `make`; there is no standalone unit-test framework. Verification is `make test` (boot-time regression suite) plus boot-time serial output.
- Do not change the syscall ABI or return codes. kmalloc failure ⇒ helper returns 0 ⇒ syscall returns `-5` (same as an invalid pointer); Linux-ABI helpers return 0 ⇒ syscall returns `-14`.
- Keep `USER_LO`/`USER_HI`/`SLAB_LO`/`SLAB_HI` defines as-is.
- `prog_args` stays persistent (never freed); it holds task-0 args for the kernel lifetime.
- `sfs_flush`/`fs_init` are only called from syscall/boot context, never IRQ; `kmalloc` is IRQ-safe.
- `kernel/linux_syscall.c` compiles even without the musl toolchain (it is always built into the kernel); no Linux runtime tests run here.
- No commits until each task's build + regression passes.

---

### Task 1: Allocating string copy helpers in `kernel/syscall.c`

**Files:**
- Modify: `kernel/syscall.c` (includes, static buffers at lines 18/38-39, `copy_user_str_buf`/`copy_user_str`/`copy_user_str2` at lines 41-62, `SYS_GET_ARGS` at ~214-228)

**Interfaces:**
- Consumes: `kmalloc`/`kfree` from `kernel/kmm.h`.
- Produces: `static char *copy_user_str(const void *usr)` → kmalloc'd NUL-terminated copy or 0; `static char *copy_user_str2(const void *usr)` → same; `static char *copy_user_str_alloc(const void *usr)` → same (shared body). All three are file-local; later tasks (Task 2) rely on them returning heap buffers.

- [ ] **Step 1: Add the `kmm.h` include**

Add after `#include "vrng.h"` (line 14):

```c
#include "kmm.h"
```

- [ ] **Step 2: Replace the static buffers and helpers**

Replace lines 18 and 38-62 (the `static char prog_args[256];`, `static char user_str[1024];`, `static char user_str2[256];`, `copy_user_str_buf`, `copy_user_str`, `copy_user_str2`) with:

```c
static char *prog_args;

// Copy a NUL-terminated user string into a fresh kmalloc'd buffer of exact
// length (no 1024-byte cap). Returns 0 on bad pointer or kmalloc failure.
static char *copy_user_str_alloc(const void *usr) {
    unsigned int a = (unsigned int)usr;
    if (a < USER_LO) return 0;
    unsigned int len = 0;
    while (a + len < USER_HI) {
        if (((const char *)a)[len] == '\0') break;
        len++;
    }
    char *dst = kmalloc(len + 1);
    if (!dst) return 0;
    for (unsigned int i = 0; i <= len; i++)
        dst[i] = ((const char *)a)[i];   // copies the NUL too
    return dst;
}

static char *copy_user_str(const void *usr) {
    return copy_user_str_alloc(usr);
}

static char *copy_user_str2(const void *usr) {
    return copy_user_str_alloc(usr);
}
```

- [ ] **Step 3: Build**

Run: `make`
Expected: compiles. (Sites still call `copy_user_str(...) >= 0`; those now compare a pointer to 0, which still type-checks and behaves identically for the success/failure test, but they LEAK — Task 2 fixes every site.)

- [ ] **Step 4: Commit**

```bash
git add kernel/syscall.c
git commit -m "syscall: allocating user-string copy helpers (TODO 1.1)"
```

---

### Task 2: Free the per-call buffers at every `syscall_handler` site

**Files:**
- Modify: `kernel/syscall.c` (syscall_handler: SYS_PRINT, SYS_FS_WRITE, SYS_FS_READ, SYS_FS_DELETE, SYS_FS_SIZE, SYS_FS_EXISTS, SYS_TEXT, SYS_SPAWN, SYS_GET_ARGS)

**Interfaces:**
- Consumes: `copy_user_str`/`copy_user_str2`/`copy_user_str_alloc` returning heap buffers (Task 1).
- Produces: no leaks — every buffer freed on both success and error paths before `break`.

- [ ] **Step 1: Convert `SYS_PRINT` (lines ~141-146)**

Replace:

```c
    case SYS_PRINT:
        if (copy_user_str((const void *)r->ebx) >= 0)
            route_text(user_str, strlen(user_str));
        else
            r->eax = -5;
        break;
```

with:

```c
    case SYS_PRINT: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s) {
            route_text(s, strlen(s));
            kfree(s);
        } else {
            r->eax = -5;
        }
        break;
    }
```

- [ ] **Step 2: Convert `SYS_FS_WRITE` (lines ~158-164)**

Replace:

```c
    case SYS_FS_WRITE:
        if (copy_user_str((const void *)r->ebx) >= 0 &&
            in_user((const void *)r->ecx, r->edx))
            r->eax = fs_write(user_str, (const char *)r->ecx, r->edx);
        else
            r->eax = -5;
        break;
```

with:

```c
    case SYS_FS_WRITE: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s && in_user((const void *)r->ecx, r->edx)) {
            r->eax = fs_write(s, (const char *)r->ecx, r->edx);
            kfree(s);
        } else {
            if (s) kfree(s);
            r->eax = -5;
        }
        break;
    }
```

- [ ] **Step 3: Convert `SYS_FS_READ` (lines ~165-171)**

Replace:

```c
    case SYS_FS_READ:
        if (copy_user_str((const void *)r->ebx) >= 0 &&
            in_user((const void *)r->ecx, r->edx))
            r->eax = fs_read(user_str, (char *)r->ecx, r->edx);
        else
            r->eax = -5;
        break;
```

with:

```c
    case SYS_FS_READ: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s && in_user((const void *)r->ecx, r->edx)) {
            r->eax = fs_read(s, (char *)r->ecx, r->edx);
            kfree(s);
        } else {
            if (s) kfree(s);
            r->eax = -5;
        }
        break;
    }
```

- [ ] **Step 4: Convert `SYS_FS_DELETE`, `SYS_FS_SIZE`, `SYS_FS_EXISTS` (lines ~172-189)**

Replace all three with:

```c
    case SYS_FS_DELETE: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s) {
            r->eax = fs_delete(s);
            kfree(s);
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_FS_SIZE: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s) {
            r->eax = fs_get_size(s);
            kfree(s);
        } else {
            r->eax = -5;
        }
        break;
    }
    case SYS_FS_EXISTS: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s) {
            r->eax = fs_exists(s);
            kfree(s);
        } else {
            r->eax = -5;
        }
        break;
    }
```

- [ ] **Step 5: Convert `SYS_TEXT` (lines ~317-329)**

`SYS_TEXT` uses `copy_user_str_buf(req->str, user_str, sizeof(user_str))` directly. Replace the whole case with:

```c
    case SYS_TEXT: {
        struct aos_render_req *req = (struct aos_render_req *)r->ebx;
        char *s = (in_user(req, sizeof(struct aos_render_req)) &&
                   in_user_area(req->buf, 1))
                      ? copy_user_str_alloc(req->str)
                      : 0;
        if (s) {
            vga_render_text_buffer(req->buf, req->pitch, req->x, req->y,
                                   s, req->fg, req->bg);
            kfree(s);
            r->eax = 0;
        } else {
            r->eax = -5;
        }
        break;
    }
```

- [ ] **Step 6: Convert `SYS_SPAWN` (lines ~345-363)**

Replace:

```c
    case SYS_SPAWN: {
        if (copy_user_str((const void *)r->ebx) >= 0) {
            const char *args = 0;
            if (r->ecx) {
                if (copy_user_str2((const void *)r->ecx) >= 0)
                    args = user_str2;
                else {
                    r->eax = -5;
                    break;
                }
            }
            unsigned int pid;
            int rc = task_spawn(user_str, args, r->edx, &pid);
            r->eax = rc == 0 ? (int)pid : rc;
        } else {
            r->eax = -5;
        }
        break;
    }
```

with:

```c
    case SYS_SPAWN: {
        char *s = copy_user_str((const void *)r->ebx);
        if (s) {
            char *a = r->ecx ? copy_user_str2((const void *)r->ecx) : 0;
            if (r->ecx && !a) {
                kfree(s);
                r->eax = -5;
                break;
            }
            unsigned int pid;
            int rc = task_spawn(s, a, r->edx, &pid);
            kfree(s);
            if (a) kfree(a);
            r->eax = rc == 0 ? (int)pid : rc;
        } else {
            r->eax = -5;
        }
        break;
    }
```

Note: `task_spawn` copies `args` into its own kmalloc'd `t->args` (`kernel/task.c:356`), so freeing `a` right after `task_spawn` is safe.

- [ ] **Step 7: Harden `SYS_GET_ARGS` for a possibly-null `prog_args`**

Replace the body of the `SYS_GET_ARGS` case (lines ~214-228) with:

```c
    case SYS_GET_ARGS: {
        char *dst = (char *)r->ebx;
        unsigned int maxlen = r->ecx;
        if (in_user(dst, maxlen) && maxlen > 0) {
            const char *args = task_current_pid() > 0 ? task_current_args() : prog_args;
            unsigned int i;
            for (i = 0; i < maxlen - 1 && args && args[i]; i++)
                dst[i] = args[i];
            dst[i] = '\0';
            r->eax = i;
        } else {
            r->eax = -5;
        }
        break;
    }
```

- [ ] **Step 8: Build**

Run: `make`
Expected: compiles with no warnings about unused `user_str`/`user_str2` (they are gone).

- [ ] **Step 9: Commit**

```bash
git add kernel/syscall.c
git commit -m "syscall: free per-call string buffers at every call site (TODO 1.1)"
```

---

### Task 3: `prog_args` on the heap

**Files:**
- Modify: `kernel/syscall.c` (`syscall_set_args` at ~64-69)

**Interfaces:**
- Consumes: `static char *prog_args` (Task 1), `kmalloc`/`kfree` from `kernel/kmm.h`.
- Produces: `syscall_set_args(const char *args)` lazily allocates `prog_args` once; `SYS_GET_ARGS` (Task 2) already null-guards it.

- [ ] **Step 1: Make `syscall_set_args` lazily allocate**

Replace `syscall_set_args` (lines ~64-69):

```c
void syscall_set_args(const char *args) {
    unsigned int i;
    for (i = 0; i < 255 && args[i]; i++)
        prog_args[i] = args[i];
    prog_args[i] = '\0';
}
```

with:

```c
void syscall_set_args(const char *args) {
    if (!prog_args) prog_args = kmalloc(256);
    if (!prog_args) return;             // OOM: keep args empty, don't crash
    unsigned int i;
    for (i = 0; i < 255 && args[i]; i++)
        prog_args[i] = args[i];
    prog_args[i] = '\0';
}
```

- [ ] **Step 2: Build**

Run: `make`
Expected: compiles.

- [ ] **Step 3: Commit**

```bash
git add kernel/syscall.c
git commit -m "syscall: heap-allocate task-0 prog_args (TODO 1.1)"
```

---

### Task 4: Allocating `copy_lin_str` in `kernel/linux_syscall.c`

**Files:**
- Modify: `kernel/linux_syscall.c` (includes; `static char lin_str[1024]` at line 12; `copy_lin_str` at ~24-37; call sites: open ~244, unlink ~259, access ~296, stat64/fstatat64 ~370)

**Interfaces:**
- Consumes: `kmalloc`/`kfree` from `kernel/kmm.h`; `cur_lctx()`/`in_luser`.
- Produces: `static char *copy_lin_str(const void *usr)` → kmalloc'd NUL-terminated copy of a Linux-window user string or 0. All four call sites free the result.

- [ ] **Step 1: Add the `kmm.h` include**

Add after `#include "syscall.h"` (line 10):

```c
#include "kmm.h"
```

- [ ] **Step 2: Replace the static buffer and helper**

Replace lines 12 and 24-37 with:

```c
// Copy a Linux-window user string into a fresh kmalloc'd buffer (no 1024 cap).
// Returns 0 on bad pointer or kmalloc failure.
static char *copy_lin_str(const void *usr) {
    unsigned int a = (unsigned int)usr;
    struct linux_ctx *lc = cur_lctx();
    if (a < lc->win_lo) return 0;
    unsigned int len = 0;
    while (a + len < lc->win_hi) {
        if (((const char *)a)[len] == '\0') break;
        len++;
    }
    char *dst = kmalloc(len + 1);
    if (!dst) return 0;
    for (unsigned int i = 0; i <= len; i++)
        dst[i] = ((const char *)a)[i];   // copies the NUL too
    return dst;
}
```

- [ ] **Step 3: Convert the `open`/`openat` case (line ~241-248)**

Replace:

```c
    case 5:   // open(path, flags, mode)
    case 295: { // openat(dirfd, path, flags, mode)
        const void *pp = (n == 295) ? (const void *)r->ecx : (const void *)r->ebx;
        if (copy_lin_str(pp, lin_str, sizeof(lin_str)) < 0) { r->eax = -14; break; }
        int fd = lc_alloc_fd(lc, lin_str);
        r->eax = (fd < 0) ? (fd == -24 ? -24 : -2) : fd;
        break;
    }
```

with:

```c
    case 5:   // open(path, flags, mode)
    case 295: { // openat(dirfd, path, flags, mode)
        const void *pp = (n == 295) ? (const void *)r->ecx : (const void *)r->ebx;
        char *p = copy_lin_str(pp);
        if (!p) { r->eax = -14; break; }
        int fd = lc_alloc_fd(lc, p);
        kfree(p);
        r->eax = (fd < 0) ? (fd == -24 ? -24 : -2) : fd;
        break;
    }
```

- [ ] **Step 4: Convert the `unlink` case (line ~258-264)**

Replace:

```c
    case 10:   // unlink(path)
        if (copy_lin_str((const void *)r->ebx, lin_str, sizeof(lin_str)) < 0) {
            r->eax = -14;
        } else {
            r->eax = fs_delete(lin_str) == 0 ? 0 : -2;
        }
        break;
```

with:

```c
    case 10: {  // unlink(path)
        char *p = copy_lin_str((const void *)r->ebx);
        if (!p) {
            r->eax = -14;
        } else {
            r->eax = fs_delete(p) == 0 ? 0 : -2;
            kfree(p);
        }
        break;
    }
```

- [ ] **Step 5: Convert the `access` case (line ~295-301)**

Replace:

```c
    case 33:   // access(path, mode)
        if (copy_lin_str((const void *)r->ebx, lin_str, sizeof(lin_str)) < 0) {
            r->eax = -14;
        } else {
            r->eax = fs_exists(lin_str) ? 0 : -2;
        }
        break;
```

with:

```c
    case 33: {  // access(path, mode)
        char *p = copy_lin_str((const void *)r->ebx);
        if (!p) {
            r->eax = -14;
        } else {
            r->eax = fs_exists(p) ? 0 : -2;
            kfree(p);
        }
        break;
    }
```

- [ ] **Step 6: Convert the `stat64`/`fstatat64` case (line ~366-377)**

Replace:

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
```

with:

```c
    case 195:   // stat64(path, st)
    case 300: { // fstatat64(dirfd, path, st, flags)
        const void *pp = (n == 300) ? (const void *)r->ecx : (const void *)r->ebx;
        unsigned char *st = (unsigned char *)r->edx;
        char *p = copy_lin_str(pp);
        if (!p) { r->eax = -14; break; }
        if (!in_luser(st, 108)) { kfree(p); r->eax = -14; break; }
        int size = fs_get_size(p);
        kfree(p);
        if (size < 0) { r->eax = -2; break; }
        fill_stat64(lc, (unsigned int)size, st);
        r->eax = 0;
        break;
    }
```

- [ ] **Step 7: Build**

Run: `make`
Expected: compiles; no remaining references to `lin_str` (grep `lin_str` should return only the helper name `copy_lin_str`).

- [ ] **Step 8: Commit**

```bash
git add kernel/linux_syscall.c
git commit -m "linux_syscall: allocating copy_lin_str (TODO 1.1)"
```

---

### Task 5: SFS sector scratches on `kmalloc`

**Files:**
- Modify: `kernel/sfs.c` (includes; `sfs_flush` lines 29-40; `fs_init` lines 71-94)

**Interfaces:**
- Consumes: `kmalloc`/`kfree` from `kernel/kmm.h`; `vblk_write`/`vblk_read` from `kernel/vblk.h`; `FS_MEM`/`FS_SIZE`/`FS_SECTORS`.
- Produces: no static scratch buffers in `sfs.c`. `sfs_flush`/`fs_init` skip gracefully on kmalloc failure.

- [ ] **Step 1: Add the `kmm.h` include**

Add after `#include "serial.h"` (line 4):

```c
#include "kmm.h"
```

- [ ] **Step 2: Convert `sfs_flush`**

Replace `sfs_flush` (lines 29-40):

```c
void sfs_flush(void) {
    if (!disk_present) return;
    static unsigned char sector[512];
    for (unsigned int s = 0; s < FS_SECTORS; s++) {
        if (!(dirty_bits[s / 8] & (1 << (s % 8)))) continue;
        for (unsigned int j = 0; j < 512; j++)
            sector[j] = FS_MEM[s * 512 + j];
        if (vblk_write(s, sector) != 0)
            serial_print("sfs: flush sector fail\n");
        dirty_bits[s / 8] &= (unsigned char)~(1 << (s % 8));
    }
}
```

with:

```c
void sfs_flush(void) {
    if (!disk_present) return;
    unsigned char *sector = kmalloc(512);
    if (!sector) return;                 // OOM: skip the flush, keep RAM copy
    for (unsigned int s = 0; s < FS_SECTORS; s++) {
        if (!(dirty_bits[s / 8] & (1 << (s % 8)))) continue;
        for (unsigned int j = 0; j < 512; j++)
            sector[j] = FS_MEM[s * 512 + j];
        if (vblk_write(s, sector) != 0)
            serial_print("sfs: flush sector fail\n");
        dirty_bits[s / 8] &= (unsigned char)~(1 << (s % 8));
    }
    kfree(sector);
}
```

- [ ] **Step 3: Convert `fs_init`**

Replace `fs_init` (lines 71-94):

```c
void fs_init(void) {
    if (disk_present) {
        static unsigned char sector[512];
        unsigned int s;
        for (s = 0; s < FS_SECTORS; s++) {
            if (vblk_read(s, sector) != 0) break;
            for (unsigned int j = 0; j < 512; j++)
                FS_MEM[s * 512 + j] = sector[j];
        }
        if (s == FS_SECTORS && hdr->magic[0] == 'S' && hdr->magic[1] == 'F' &&
            hdr->magic[2] == 'S' && hdr->magic[3] == '1') {
            serial_print("SFS mounted from disk.\n");
            return;
        }
        serial_print("SFS formatting new disk.\n");
        fs_format();
        sfs_flush();
        return;
    }
    if (hdr->magic[0] != 'S' || hdr->magic[1] != 'F' ||
        hdr->magic[2] != 'S' || hdr->magic[3] != '1') {
        fs_format();
    }
}
```

with:

```c
void fs_init(void) {
    if (disk_present) {
        unsigned char *sector = kmalloc(512);
        unsigned int s;
        if (sector) {
            for (s = 0; s < FS_SECTORS; s++) {
                if (vblk_read(s, sector) != 0) break;
                for (unsigned int j = 0; j < 512; j++)
                    FS_MEM[s * 512 + j] = sector[j];
            }
            kfree(sector);
            if (s == FS_SECTORS && hdr->magic[0] == 'S' && hdr->magic[1] == 'F' &&
                hdr->magic[2] == 'S' && hdr->magic[3] == '1') {
                serial_print("SFS mounted from disk.\n");
                return;
            }
        }
        serial_print("SFS formatting new disk.\n");
        fs_format();
        sfs_flush();
        return;
    }
    if (hdr->magic[0] != 'S' || hdr->magic[1] != 'F' ||
        hdr->magic[2] != 'S' || hdr->magic[3] != '1') {
        fs_format();
    }
}
```

Note: if `kmalloc` failed, `s` is uninitialized — the code only reaches the `s == FS_SECTORS` test when `sector` was non-null, because that test sits inside the `if (sector)` block. This matches the spec's "degrades to RAM-only mode" on OOM.

- [ ] **Step 4: Build**

Run: `make`
Expected: compiles.

- [ ] **Step 5: Commit**

```bash
git add kernel/sfs.c
git commit -m "sfs: sector scratch buffers via kmalloc (TODO 1.1)"
```

---

### Task 6: Full regression

**Files:**
- None (verification only).

- [ ] **Step 1: Grep for leftover static scratch buffers**

Run:
```bash
grep -n "static char user_str\|static char user_str2\|static char prog_args\[" kernel/*.c
grep -n "lin_str\[" kernel/*.c
grep -n "static unsigned char sector\[512\]" kernel/*.c
```
Expected: no matches.

- [ ] **Step 2: Run the full regression suite**

Run: `make test`
Expected: `ALL 8 TESTS PASSED` (ipctest, manytest, notepadtest, sleeptest, rngtest, blktest, virtiotest, netlooptest). If a test fails, fix the leak/regression before committing.

- [ ] **Step 3: Update TODO.md**

Mark item 1.1 done:

```markdown
- [x] **P0 — Перевести временные буферы ядра на `kmalloc`.** `user_str[1024]`/`user_str2[256]`/`prog_args[256]` в `kernel/syscall.c` и SFS-буферы — сейчас статические; выделять через `kmalloc` (мелкие, живучие, нет лимита в 1024 байта). (готово: per-call точного размера в `syscall.c`/`linux_syscall.c`, `prog_args` на куче, SFS-скретчи через `kmalloc`)
```

- [ ] **Step 4: Commit**

```bash
git add TODO.md
git commit -m "todo: mark temporary kernel buffers on kmalloc done"
```

---

## Self-review notes

- **Spec coverage:** syscall.c strings (Tasks 1-2), prog_args (Task 3), linux_syscall.c lin_str (Task 4), SFS sector buffers (Task 5), regression + TODO (Task 6). Matches the spec section-by-section.
- **Type consistency:** `copy_user_str`/`copy_user_str2`/`copy_lin_str` all return `char *` (0 on failure) consistently; all call sites test the pointer and `kfree`. `copy_user_str_alloc` is the shared body. `prog_args` is `char *` everywhere.
- **OOM handling:** every allocating helper returns 0 on kmalloc failure and every syscall maps that to `-5` (AOS) / `-14` (Linux), preserving the existing error contract.
- **Safety:** `task_spawn` copies args into its own buffer before returning, so freeing the SYS_SPAWN args string after the call is correct.
