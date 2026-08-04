# Temporary Kernel Buffers on `kmalloc`

**Date:** 2026-08-04
**TODO item:** 1.1 «Перевести временные буферы ядра на `kmalloc`» (P0)

## Goal

Replace the remaining static per-call scratch buffers in the kernel with
`kmalloc`-allocated buffers so string sizes are no longer hard-capped (1024/256
bytes) and the syscall layer holds no shared mutable state that could race
under a future preemptible interrupt path.

## Scope

Convert these buffers:

| File | Buffer | Current use |
|------|--------|-------------|
| `kernel/syscall.c` | `user_str[1024]` | temp copy of a user string (`copy_user_str`) |
| `kernel/syscall.c` | `user_str2[256]` | temp copy of args string (`copy_user_str2`) |
| `kernel/syscall.c` | `prog_args[256]` | persistent args for task 0 (`syscall_set_args` / `SYS_GET_ARGS`) |
| `kernel/linux_syscall.c` | `lin_str[1024]` | temp copy of a Linux-ABI user string (`copy_lin_str`) |
| `kernel/sfs.c` | `static unsigned char sector[512]` (x2) | scratch sector buffer in `sfs_flush()` and `fs_init()` |

Out of scope (already dynamic or intentionally static): `tasks[]`, `zombies[]`
(task table is a fixed `MAX_TASKS` array, not a scratch buffer), `sys_stack`,
`page_dir`/`page_tables`/`extra_pt` (paging structures), VGA text-mode buffers,
terminal buffers. `route_hex`/`route_dec` local `char buf[12]` are stack
locals, not statics — left as-is.

## Architecture

### 1. `kernel/syscall.c` — per-call, exact-size allocation

Replace `copy_user_str_buf` with an allocating variant:

```c
// Returns a kmalloc'd NUL-terminated copy of the user string, or 0 on error
// (bad pointer, or kmalloc failure). Caller must kfree().
static char *copy_user_str_alloc(const void *usr) {
    unsigned int a = (unsigned int)usr;
    if (a < USER_LO) return 0;
    // Measure the length, bounded by the user window.
    unsigned int len = 0;
    while (a + len < USER_HI) {
        if (((char *)a)[len] == '\0') break;
        len++;
    }
    char *dst = kmalloc(len + 1);
    if (!dst) return 0;
    for (unsigned int i = 0; i <= len; i++)
        dst[i] = ((char *)a)[i];          // copies the NUL too
    return dst;
}
```

- `copy_user_str()` and `copy_user_str2()` return the allocating result.
- Every call site in `syscall_handler` frees the returned buffer before
  `break`, on both success and error paths.
- No size cap: the copied length is bounded only by the user window
  (`0x01000000..0x01804000`, ~12 MB).

`prog_args`: keep it persistent but heap-allocated.

```c
static char *prog_args;                 // kmalloc'd, task-0 args

void syscall_set_args(const char *args) {
    if (!prog_args) prog_args = kmalloc(256);
    if (!prog_args) return;             // OOM: args stay empty
    unsigned int i;
    for (i = 0; i < 255 && args[i]; i++)
        prog_args[i] = args[i];
    prog_args[i] = '\0';
}
```

`SYS_GET_ARGS` already guards `prog_args` implicitly (uses it only when
`task_current_pid() == 0`); add a null check so a failed lazy allocation
returns an empty string instead of dereferencing 0.

### 2. `kernel/linux_syscall.c` — per-call, exact-size allocation

Same pattern: `copy_lin_str()` measures against the Linux window
(`lc->win_lo..win_hi`), `kmalloc`s `len + 1`, copies, returns the buffer (or
0). Its 4 call sites (open, unlink, access, stat64/fstatat64) kfree after use.

### 3. `kernel/sfs.c` — per-call scratch sector

- `sfs_flush()`: `unsigned char *sector = kmalloc(512); ... kfree(sector);`
  with an early return if allocation fails (flush is best-effort; on OOM just
  skip — RAM-only mode has no disk anyway).
- `fs_init()`: same scratch allocation for the sector-by-sector disk load.

`sfs_flush` is only called from syscall/boot context (never from an IRQ
handler), and `kmalloc` is IRQ-safe, so a heap scratch is safe.

## Error handling

- kmalloc returning 0 → the copy helpers return 0 → the syscall returns `-5`
  (same error as an invalid user pointer). No ABI change.
- `sfs_flush`/`fs_init` on kmalloc failure: skip the flush/load gracefully
  (no panic). For `fs_init` this degrades to RAM-only mode.

## Testing

- `make` builds clean.
- `make test` — full regression (8 tests): `ipctest`, `manytest`,
  `notepadtest`, `sleeptest`, `rngtest`, `blktest`, `virtiotest`,
  `netlooptest`. Exercise paths: `SYS_FS_*` and `SYS_SPAWN` (notepadtest),
  `SYS_TEXT`/`SYS_PRINT` (every GUI boot), `SYS_GET_ARGS` (all spawned
  programs), `sfs_flush` on disk (blktest/virtiotest).
- No Linux tests run without the musl toolchain; `linux_syscall.c` changes are
  compile-checked and reviewed manually.

## Constraints / non-goals

- Keep syscall ABI and return codes unchanged.
- Do not touch the task table, paging structures, terminal buffers, or VGA
  buffers.
- Do not convert persistent per-task storage (`t->args`, mailboxes) — those
  are already kmalloc'd (see dynamic-memory plan).
