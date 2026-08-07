# Shell Reliability (`&`, `$?`, env, backtrace symbols) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add background jobs `&` + `$?`, environment variables (`export`, `$VAR`/`$?` expansion), and symbol names in kernel panic backtraces.

**Architecture:** Three independent kernel/shell-side features in `kernel/commands.c` (status plumbing, env table + expansion, bg spawn) plus a build-time-generated symbol table (`scripts/gen_symtab.py` → `kernel/symtab.c`) resolved in `kernel/bt.c`. Tests are QEMU scripts driven by the human per AGENTS.md.

**Tech Stack:** gcc i386 freestanding kernel (`-m32 -nostdlib`), GNU `nm` (host binutils), GNU make, Python 3, QEMU serial-socket test harness.

## Global Constraints

- All kernel code: `-ffreestanding -nostdlib -fno-builtin -m32 -std=c11`; use `__asm__ __volatile__`, not `asm` (from `kernel/string.c` convention).
- `LINE_BUF_SIZE` = 256 (`kernel/terminal.h`), `PATH_MAX` = 128 (`kernel/commands.h`).
- QEMU/UI test runs are performed by the human, never by the assistant. The assistant runs `make` (build) and static checks only.
- `kernel/progs.c` and `kernel/symtab.c` are generated files, never edited by hand.
- Exit-code capture must be added in BOTH pid-0 exit paths: `kernel/syscall.c` `case SYS_EXIT` and `kernel/linux_syscall.c` `linux_exit()`.
- The generated `kernel/symtab.o` MUST be the last object on the `aos.elf` link line, so pass-1 (nm) and pass-2 (runtime) symbol addresses are identical.
- Every `make` must stay warning-free (`-Wall -Wextra`).

---

### Task 1: `$?` — capture the exit status of in-place programs

**Files:**
- Modify: `kernel/commands.h`
- Modify: `kernel/commands.c`
- Modify: `kernel/syscall.c:402-407`
- Modify: `kernel/linux_syscall.c:50-57`
- (Build check: `make`)

**Interfaces:**
- Produces:
  - `int shell_status(void)` — current `$?` value (initial 0).
  - `void shell_set_status(int code)` — set `$?`. Called from the kernel's pid-0 exit paths; read by `shell_expand()` (Task 2) and by `run_bg` (Task 3).

- [ ] **Step 1: Declare the accessors in `kernel/commands.h`**

Append after the `path_norm` declaration:

```c
// Exit status of the last command ("$?"). Set by the kernel on program exit
// (pid 0 in-place path), read by the shell's $? expansion and background
// spawn code. 0 = success, 127 = command not found.
int shell_status(void);
void shell_set_status(int code);
```

- [ ] **Step 2: Implement the status global in `kernel/commands.c`**

Add near the top (after `char command_path[PATH_MAX] = "/bin";`):

```c
static int shell_status_code = 0;

int shell_status(void) {
    return shell_status_code;
}

void shell_set_status(int code) {
    shell_status_code = code;
}
```

- [ ] **Step 3: Capture the exit code in the AOS `SYS_EXIT` path**

In `kernel/syscall.c`, `case SYS_EXIT` (currently lines 402-407):

```c
    case SYS_EXIT:
        if (task_current_pid() == 0) {
            shell_set_status((int)r->ebx);
            user_program_exit();
        } else {
            task_exit_current(r->ebx);
        }
        break;
```

Add `#include "commands.h"` to `kernel/syscall.c` (it is not currently included).

- [ ] **Step 4: Capture the exit code in the musl `exit_group` path**

In `kernel/linux_syscall.c`, `linux_exit()` (currently lines 50-57):

```c
static void linux_exit(unsigned int code) {
    if (task_current_pid() == 0) {
        shell_set_status((int)code);
        task_set_abi_current(ABI_AOS);
        linux_ctx_init(task_current_lctx());
        user_program_exit();
    }
    task_exit_current(code);
}
```

Add `#include "commands.h"` to `kernel/linux_syscall.c`.

- [ ] **Step 5: Build**

Run: `make`
Expected: links clean, no new `-Wall -Wextra` warnings. (`user_program_exit()` never returns, so the trailing `task_exit_current(code)` in `linux_exit` is unchanged — it is the pid>0 path only.)

- [ ] **Step 6: Commit**

```bash
git add kernel/commands.h kernel/commands.c kernel/syscall.c kernel/linux_syscall.c
git commit -m "shell: capture last command exit status for \$?"
```

---

### Task 2: Environment variables — `export` builtin + `$VAR`/`$?` expansion

**Files:**
- Modify: `kernel/commands.c`
- Modify: `kernel/commands.h` (nothing needed — helpers stay static; `commands_execute` signature unchanged)
- (Build check: `make`)

**Interfaces:**
- Consumes: `int shell_status(void)` from Task 1.
- Produces:
  - `static const char *env_get(const char *name)` — value or `0` (internal).
  - `static void shell_expand(const char *in, char *out, unsigned int outsz)` — expands `$NAME` and `$?` (internal; used by Task 3's bg path via `commands_execute`).

- [ ] **Step 1: Add the env table and helpers in `kernel/commands.c`**

Add after the `shell_set_status` function:

```c
// ---- Environment variables (shell-local; per-task env is P1) ----
#define ENV_MAX 16
static struct {
    char name[24];
    char val[64];
} shell_env[ENV_MAX];
static unsigned int shell_env_count = 0;

static int env_name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static const char *env_get(const char *name) {
    for (unsigned int i = 0; i < shell_env_count; i++)
        if (strcmp(shell_env[i].name, name) == 0)
            return shell_env[i].val;
    return 0;
}

static void env_set(const char *name, const char *val) {
    for (unsigned int i = 0; i < shell_env_count; i++)
        if (strcmp(shell_env[i].name, name) == 0) {
            strncpy(shell_env[i].val, val, 63);
            shell_env[i].val[63] = '\0';
            return;
        }
    if (shell_env_count < ENV_MAX) {
        strncpy(shell_env[shell_env_count].name, name, 23);
        shell_env[shell_env_count].name[23] = '\0';
        strncpy(shell_env[shell_env_count].val, val, 63);
        shell_env[shell_env_count].val[63] = '\0';
        shell_env_count++;
    }
}

static void cmd_export(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) {
        for (unsigned int i = 0; i < shell_env_count; i++) {
            terminal_print("\n");
            terminal_print(shell_env[i].name);
            terminal_print("=");
            terminal_print(shell_env[i].val);
        }
        return;
    }
    char name[24];
    unsigned int n = 0;
    while (*arg && *arg != '=' && *arg != ' ' && n < 23) name[n++] = *arg++;
    name[n] = '\0';
    if (n == 0) return;
    if (*arg == '=') {
        arg++;
        while (*arg == ' ') arg++;
        env_set(name, arg);
    } else {
        env_set(name, "");
    }
}
```

- [ ] **Step 2: Add `shell_expand` in `kernel/commands.c`**

Add after `cmd_export`:

```c
// Expand $NAME and $? into `out`. Unknown vars become "". A '$' not followed
// by a name char or '?' is copied literally. Truncates at outsz-1.
static void shell_expand(const char *in, char *out, unsigned int outsz) {
    unsigned int o = 0;
    while (*in && o + 1 < outsz) {
        if (*in == '$' && in[1] == '?') {
            int st = shell_status();
            char tmp[12];
            unsigned int t = 0;
            if (st == 0) { tmp[t++] = '0'; }
            else {
                unsigned int v = (st < 0) ? (unsigned int)(-st) : (unsigned int)st;
                while (v > 0) { tmp[t++] = '0' + (v % 10); v /= 10; }
            }
            while (t > 0) out[o++] = tmp[--t];
            in += 2;
            continue;
        }
        if (*in == '$' && env_name_char(in[1])) {
            char name[24];
            unsigned int n = 0;
            const char *p = in + 1;
            while (env_name_char(*p) && n < 23) name[n++] = *p++;
            name[n] = '\0';
            const char *val = env_get(name);
            if (val)
                for (unsigned int i = 0; val[i] && o + 1 < outsz; i++)
                    out[o++] = val[i];
            in = p;
            continue;
        }
        out[o++] = *in++;
    }
    out[o] = '\0';
}
```

- [ ] **Step 3: Wire `export` and expansion into the command executor**

In `kernel/commands.c`, in `run_command_raw` (before the `exec_from_path` call at the bottom), add:

```c
    if (strcmp(cmd, "export") == 0) {
        cmd_export(arg);
        return;
    }
```

Then modify `commands_execute` so the whole line is expanded first (replace the top of the function, currently lines 377-382):

```c
void commands_execute(const char *line) {
    while (*line == ' ') line++;
    if (!*line) {
        terminal_set_prompt();
        return;
    }

    char expanded[LINE_BUF_SIZE];
    shell_expand(line, expanded, sizeof(expanded));

    int op;
    if (find_operator(expanded, &op)) {
        exec_stage(expanded);
        terminal_set_prompt();
        return;
    }

    run_command_raw(expanded);
    terminal_set_prompt();
}
```

(Note: `find_operator`/`exec_stage`/`run_command_raw` keep working on the expanded copy. In `commands_execute`, `expanded` is used in place of `line`.)

- [ ] **Step 4: Build**

Run: `make`
Expected: clean link, no new warnings.

- [ ] **Step 5: Commit**

```bash
git add kernel/commands.c
git commit -m "shell: export builtin and \$NAME/\$? expansion"
```

---

### Task 3: Background jobs `&` (with `>`/`>>`/`<` redirects)

**Files:**
- Modify: `kernel/commands.c`
- (Build check: `make`)

**Interfaces:**
- Consumes: `int shell_status(void)`, `void shell_set_status(int)` (Task 1); `shell_expand()` (Task 2); `find_operator()`, `skip_token()` (already in file); `task_spawn()` / `task_slot()` (`kernel/task.h`); `vfs_open_fd()` / `vfs_ofile_ptr()` / `current_task_cwd()` (`kernel/vfs.h`); `task_current_cwd()` in `commands.c` via `current_task_cwd()`.
- Produces:
  - `static int path_resolve(const char *cmd, char *out, unsigned int outsz)` — returns 1 if `cmd` resolves in PATH (or as a raw path), fills `out`; 0 otherwise. Used by both `exec_from_path` (refactor) and `bg_spawn`.
  - `static int bg_spawn(const char *line, unsigned int *out_pid)` — spawns a background task; prints `bg: pid N`; returns 1 on success, 0 on failure. Never runs builtins (caller handles them).
  - `static void run_bg(const char *line)` — dispatcher for the stripped bg line.

- [ ] **Step 1: Factor out `path_resolve` from `exec_from_path`**

Replace the body of `exec_from_path` (currently lines 122-159) with:

```c
// Locate `cmd` in PATH (or as a raw path). On success fills `out` (up to
// outsz bytes) with the resolved full path and returns 1; else 0.
static int path_resolve(const char *cmd, char *out, unsigned int outsz) {
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

            if (vfs_kernel_stat(full_path, &st2) == 0) {
                if (out && outsz > 0) {
                    for (int k = 0; k <= i && k < (int)outsz - 1; k++)
                        out[k] = full_path[k];
                    out[outsz - 1] = '\0';
                }
                return 1;
            }
        }

        if (!has_sep) break;
        dir = next + 1;
    }

    if (vfs_kernel_stat(cmd, &st2) == 0) {
        if (out && outsz > 0) {
            int k = 0;
            while (cmd[k] && k < (int)outsz - 1) { out[k] = cmd[k]; k++; }
            out[k] = '\0';
        }
        return 1;
    }
    return 0;
}

// Run `cmd` in-place in the current task (task 0). Returns 1 if a program
// was found and ran to its exit.
static int exec_from_path(const char *cmd, const char *arg, int trace) {
    char full_path[PATH_MAX];
    if (!path_resolve(cmd, full_path, sizeof(full_path)))
        return 0;
    return try_exec(full_path, arg, trace);
}
```

- [ ] **Step 2: Add a builtin predicate**

Add after `exec_from_path`:

```c
static int cmd_is_builtin(const char *cmd) {
    return strcmp(cmd, "format") == 0 || strcmp(cmd, "setpath") == 0 ||
           strcmp(cmd, "cd") == 0 || strcmp(cmd, "pwd") == 0 ||
           strcmp(cmd, "strace") == 0 || strcmp(cmd, "export") == 0;
}
```

- [ ] **Step 3: Add `bg_spawn`, `run_bg_redirect`, `run_bg`**

Add after `exec_stage` (before `commands_execute`):

```c
// Spawn `line` (a simple "cmd args" line, no operators) as a background task.
// Prints "bg: pid N" on success and sets $?; returns 1 on success, 0 on
// failure. Builtins are NOT handled here (caller runs them inline).
static int bg_spawn(const char *line, unsigned int *out_pid) {
    while (*line == ' ') line++;
    if (!*line) return 0;

    const char *arg = line;
    while (*arg && *arg != ' ') arg++;
    unsigned int cmd_len = (unsigned int)(arg - line);
    while (*arg == ' ') arg++;

    char cmd[16];
    unsigned int cl = cmd_len < 15 ? cmd_len : 15;
    strncpy(cmd, line, cl);
    cmd[cl] = '\0';

    char full_path[PATH_MAX];
    if (!path_resolve(cmd, full_path, sizeof(full_path))) {
        terminal_print("\nUnknown command: ");
        terminal_write(line, cmd_len);
        terminal_print(". Type 'help'");
        shell_set_status(127);
        return 0;
    }

    unsigned int pid;
    if (task_spawn(full_path, arg, 0, &pid) != 0) {
        terminal_print("\nbg: spawn failed");
        shell_set_status(1);
        return 0;
    }
    shell_set_status(0);
    terminal_print("\nbg: pid ");
    terminal_print_dec(pid);
    if (out_pid) *out_pid = pid;
    return 1;
}

// Background command with a >/>>/< redirect: spawn the left side and wire the
// child's stdout/stdin fd to the opened file.
static void run_bg_redirect(const char *line, int op, const char *op_pos) {
    unsigned int left_len = (unsigned int)(op_pos - line);
    while (left_len > 0 && line[left_len - 1] == ' ') left_len--;

    const char *right = op_pos + (op == OP_GTG ? 2 : 1);
    while (*right == ' ') right++;

    char left_buf[LINE_BUF_SIZE];
    if (left_len >= LINE_BUF_SIZE) left_len = LINE_BUF_SIZE - 1;
    for (unsigned int i = 0; i < left_len; i++) left_buf[i] = line[i];
    left_buf[left_len] = '\0';

    const char *fn_end = skip_token(right);
    unsigned int fn_len = (unsigned int)(fn_end - right);
    if (fn_len == 0) {
        terminal_print("\nredirect: missing file name");
        return;
    }
    char fn[PATH_MAX];
    if (fn_len >= PATH_MAX) fn_len = PATH_MAX - 1;
    for (unsigned int i = 0; i < fn_len; i++) fn[i] = right[i];
    fn[fn_len] = '\0';

    struct vfs_inode *cwd = current_task_cwd();
    int flags = (op == OP_LT)
        ? VFS_O_RDONLY
        : (VFS_O_WRONLY | VFS_O_CREAT | (op == OP_GTG ? VFS_O_APPEND : VFS_O_TRUNC));
    int fd = vfs_open_fd(cwd, fn, flags);
    vfs_put(cwd);
    if (fd < 0) {
        terminal_print("\nredirect: cannot open ");
        terminal_print(fn);
        return;
    }

    unsigned int pid;
    if (!bg_spawn(left_buf, &pid)) {
        vfs_close_fd(fd);   // spawn failed / not found: close, nothing wired
        return;
    }

    struct task *c = task_slot(pid);
    c->fds[fd] = vfs_ofile_ptr(fd);
    if (op == OP_LT) c->stdin_fd = fd;
    else             c->stdout_fd = fd;
}

static void run_bg(const char *line) {
    while (*line == ' ') line++;
    if (!*line) return;

    const char *arg = line;
    while (*arg && *arg != ' ') arg++;
    char cmd[16];
    unsigned int cl = ((unsigned int)(arg - line)) < 15 ? (unsigned int)(arg - line) : 15;
    strncpy(cmd, line, cl);
    cmd[cl] = '\0';

    if (cmd_is_builtin(cmd)) {
        run_command_raw(line);   // builtins run inline; & is ignored
        return;
    }

    int op;
    const char *op_pos = find_operator(line, &op);
    if (op == OP_PIPE) {
        terminal_print("\nbg: pipes not supported");
        return;
    }
    if (op != OP_NONE) {
        run_bg_redirect(line, op, op_pos);
        return;
    }
    bg_spawn(line, 0);
}
```

- [ ] **Step 4: Wire the trailing-`&` detection into `commands_execute`**

Modify `commands_execute` (the version from Task 2) to strip a trailing `&` and dispatch to `run_bg`:

```c
void commands_execute(const char *line) {
    while (*line == ' ') line++;
    if (!*line) {
        terminal_set_prompt();
        return;
    }

    char expanded[LINE_BUF_SIZE];
    shell_expand(line, expanded, sizeof(expanded));

    // Trailing '&' = background. Strip it (and surrounding spaces).
    unsigned int len = 0;
    while (expanded[len]) len++;
    while (len > 0 && expanded[len - 1] == ' ') expanded[--len] = '\0';
    int bg = 0;
    if (len > 0 && expanded[len - 1] == '&') {
        bg = 1;
        expanded[--len] = '\0';
        while (len > 0 && expanded[len - 1] == ' ') expanded[--len] = '\0';
    }

    if (bg) {
        run_bg(expanded);
        terminal_set_prompt();
        return;
    }

    int op;
    if (find_operator(expanded, &op)) {
        exec_stage(expanded);
        terminal_set_prompt();
        return;
    }

    run_command_raw(expanded);
    terminal_set_prompt();
}
```

- [ ] **Step 5: Build**

Run: `make`
Expected: clean link, no new warnings. `exec_from_path` is still referenced by `cmd_strace` and `run_command_raw`, so no dead-code warnings.

- [ ] **Step 6: Commit**

```bash
git add kernel/commands.c
git commit -m "shell: background jobs with & and redirects, \$? set on spawn"
```

---

### Task 4: Symbol names in panic backtraces (generated symtab)

**Files:**
- Create: `kernel/symtab.h`
- Create: `kernel/symtab.c` (committed stub; regenerated by the build)
- Create: `scripts/gen_symtab.py`
- Modify: `kernel/bt.c`
- Modify: `Makefile`
- (Build check: `make`, then `make clean && make` from scratch)

**Interfaces:**
- Produces:
  - `struct symtab_entry { unsigned int addr; const char *name; };` (`kernel/symtab.h`)
  - `extern const struct symtab_entry kernel_symtab[];`
  - `extern const unsigned int kernel_symtab_count;`
  - `const char *addr_to_sym(unsigned int eip, unsigned int *off);` (`kernel/bt.c` — also declared in `kernel/bt.h`)

- [ ] **Step 1: Create `kernel/symtab.h`**

```c
#ifndef SYMTAB_H
#define SYMTAB_H

struct symtab_entry {
    unsigned int addr;
    const char *name;
};

extern const struct symtab_entry kernel_symtab[];
extern const unsigned int kernel_symtab_count;

#endif
```

- [ ] **Step 2: Create the stub `kernel/symtab.c`**

```c
/* Bootstrap stub: overwritten by scripts/gen_symtab.py during the aos.elf
   two-pass link. Never edit by hand. */
#include "symtab.h"

const struct symtab_entry kernel_symtab[] = {
    {0x00100000, "start"},
};
const unsigned int kernel_symtab_count = 1;
```

(Using a non-empty stub keeps the lookup code exercised even if the generator is unavailable, and keeps the table type-checked at every clean build.)

- [ ] **Step 3: Create `scripts/gen_symtab.py`**

```python
#!/usr/bin/env python3
"""Generate kernel/symtab.c from the text symbols of a linked aos.elf.

Usage: gen_symtab.py <elf> <out.c>

Runs `nm -n` on the ELF and emits a sorted {addr,name} table for every
T/t (text) symbol in [0x00100000, 0x00400000) -- the kernel text range
(_end <= RAMDISK_BASE per linker.ld ASSERT). The generated file is compiled
and linked LAST so its presence never shifts the addresses being recorded.
"""
import subprocess
import sys

TEXT_LO = 0x00100000
TEXT_HI = 0x00400000


def main():
    if len(sys.argv) != 3:
        print("usage: gen_symtab.py <elf> <out.c>", file=sys.stderr)
        return 2
    elf, out = sys.argv[1], sys.argv[2]
    try:
        nm_out = subprocess.check_output(["nm", "-n", elf]).decode("utf-8", "replace")
    except subprocess.CalledProcessError:
        nm_out = ""
    entries = []
    for line in nm_out.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        addr_s, typ, name = parts
        if typ not in ("T", "t"):
            continue
        try:
            addr = int(addr_s, 16)
        except ValueError:
            continue
        if not (TEXT_LO <= addr < TEXT_HI):
            continue
        entries.append((addr, name))
    entries.sort()
    with open(out, "w") as f:
        f.write("/* generated by scripts/gen_symtab.py - do not edit */\n")
        f.write('#include "symtab.h"\n\n')
        f.write("const struct symtab_entry kernel_symtab[] = {\n")
        for addr, name in entries:
            f.write('    {0x%08x, "%s"},\n' % (addr, name))
        f.write("};\n")
        f.write("const unsigned int kernel_symtab_count = %d;\n" % len(entries))
    print("gen_symtab: %d text symbols -> %s" % (len(entries), out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Add symbol lookup to `kernel/bt.c` and `kernel/bt.h`**

`kernel/bt.h` — add after the `backtrace` declaration:

```c
// Resolve a kernel text address to the nearest preceding function symbol.
// Returns the symbol name and sets *off = eip - sym_addr, or 0 if the
// address is not in the kernel text range.
const char *addr_to_sym(unsigned int eip, unsigned int *off);
```

`kernel/bt.c` — replace the whole file with:

```c
#include "bt.h"
#include "printf.h"
#include "symtab.h"

const char *addr_to_sym(unsigned int eip, unsigned int *off) {
    if (eip < (unsigned int)_start || eip >= (unsigned int)_end)
        return 0;
    unsigned int lo = 0, hi = kernel_symtab_count;
    while (lo < hi) {
        unsigned int mid = (lo + hi) / 2;
        if (kernel_symtab[mid].addr <= eip) lo = mid + 1;
        else                                hi = mid;
    }
    if (lo == 0) return 0;
    const struct symtab_entry *e = &kernel_symtab[lo - 1];
    *off = eip - e->addr;
    return e->name;
}

void backtrace(uint32_t *ebp, int max_frames) {
    printf("--- backtrace ---\n");
    int i = 0;
    while (ebp && i < max_frames) {
        if ((uint32_t)ebp < 0x100000)
            break;
        if ((uint32_t)ebp & 3)
            break;
        uint32_t eip = ebp[1];
        if (eip < (uint32_t)_start || eip >= (uint32_t)_end)
            break;
        unsigned int off;
        const char *nm = addr_to_sym(eip, &off);
        if (nm)
            printf("  [%d] eip=0x%08x  %s+0x%x\n", i, eip, nm, off);
        else
            printf("  [%d] eip=0x%08x\n", i, eip);
        uint32_t *next = (uint32_t *)ebp[0];
        if (next <= ebp)
            break;
        ebp = next;
        i++;
    }
    printf("--- end ---\n");
}
```

- [ ] **Step 5: Wire the two-pass link into the Makefile**

Append `kernel/symtab.o` **last** to `KERNEL_OBJS` (after `kernel/trace.o`):

```make
               kernel/task.o kernel/linux_syscall.o kernel/block.o kernel/sfs2.o \
               kernel/klog.o kernel/trace.o kernel/symtab.o
```

Replace the `aos.elf` rule (currently lines 93-94) with:

```make
# Two-pass link: link once (with the previous kernel/symtab.c), nm it to
# regenerate kernel/symtab.c, recompile kernel/symtab.o, relink. symtab.o is
# the LAST object, so its own .rodata never shifts other symbols' addresses.
aos.elf: $(KERNEL_OBJS) linker.ld scripts/gen_symtab.py
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)
	$(PYTHON) scripts/gen_symtab.py $@ kernel/symtab.c
	$(CC) $(CFLAGS) -c -o kernel/symtab.o kernel/symtab.c
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)
```

Do NOT add `kernel/symtab.c` to the `clean` target's `rm` list — it is the committed bootstrap stub.

- [ ] **Step 6: Build and verify the generated table**

Run: `make`
Expected: the recipe prints `gen_symtab: N text symbols -> kernel/symtab.c` with a non-zero N (several hundred). `aos.iso` is produced.

Then run a full rebuild from scratch to prove the bootstrap works with only the committed stub:

Run: `make clean && make`
Expected: clean build; `gen_symtab` line again; `make` again is a no-op (idempotent in steady state).

Sanity check the table content:

```bash
grep -c '^    {0x' kernel/symtab.c
nm -n aos.elf | grep -w isr_handler
```

Expected: the first count > 100; the second prints an address matching a `isr_handler` entry in `kernel/symtab.c`.

- [ ] **Step 7: Commit**

```bash
git add kernel/symtab.h kernel/symtab.c scripts/gen_symtab.py kernel/bt.c kernel/bt.h Makefile
git commit -m "backtrace: resolve eip to function names via generated symtab"
```

---

### Task 5: QEMU regression scripts (`shelltest.py`, `panictest.py`) + Makefile wiring

**Files:**
- Create: `scripts/shelltest.py`
- Create: `scripts/panictest.py`
- Modify: `Makefile` (TESTS)
- (Run by the human per AGENTS.md — do NOT launch QEMU yourself.)

**Interfaces:**
- Consumes: Task 1 `$?`, Task 2 env, Task 3 `&`, Task 4 symbols. Programs already on the ramdisk: `lin/hello` (prints «Привет от программы, собранной через musl-gcc!»), `exitto` (`_exit(7)`), `uptime` («Uptime: N.NN seconds»), `echo`, `cat`.
- Note on the harness: the tests follow the `cwdtest.py` pattern — connect to the serial UNIX socket before boot output, drain to the first `AOS>`, then `sendall()` the whole command burst so it is processed before the WM registers as the event consumer and captures serial input. Serial input is NOT echoed into the serial log (echo goes to VGA only), so assertions match only program output.

- [ ] **Step 1: Write `scripts/shelltest.py`**

```python
#!/usr/bin/env python3
"""E2E shell test: env vars ($VAR), exit status ($?), background jobs (&).

Queue a serial burst right at the AOS> prompt (before the WM captures
serial input) and assert on the accumulated serial log.
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
SER = "/tmp/aos-shelltest.ser"

def wait_for(path, seconds=10):
    end = time.time() + seconds
    while time.time() < end:
        if os.path.exists(path): return
        time.sleep(0.05)
    raise RuntimeError("timeout waiting for " + path)

def drain(s, log, end, needle=b""):
    while time.time() < end:
        try:
            d = s.recv(4096)
            if not d: break
            log += d
            if needle and needle in log: break
        except socket.timeout:
            pass
    return log

def main():
    try: os.unlink(SER)
    except FileNotFoundError: pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-no-reboot",
        "-serial", "unix:" + SER + ",server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(SER)
        time.sleep(0.5)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(1)
        s.connect(SER)

        log = drain(s, b"", time.time() + 40, b"AOS>")
        if b"KERNEL PANIC" in log:
            raise AssertionError("kernel panic during boot:\n" + log[-400:].decode(errors="replace"))

        burst = (b"export AOSHOME=/home/test\n"
                 b"echo $AOSHOME\n"
                 b"lin/hello\n"
                 b"echo $?\n"
                 b"exitto\n"
                 b"echo $?\n"
                 b"uptime &\n"
                 b"echo $?\n"
                 b"echo bgtag > /bg.txt &\n"
                 b"cat /bg.txt\n")
        s.sendall(burst)
        out = drain(s, b"", time.time() + 30, b"bgtag")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic during shell commands:\n" + out[-400:].decode(errors="replace"))
        otext = out.decode(errors="replace")

        failures = []
        # $VAR expansion: `echo $AOSHOME` prints "/home/test" on its own line.
        if "\n/home/test\n" not in otext:
            failures.append("echo $AOSHOME did not print /home/test")
        # lin/hello exits 0 -> `echo $?` prints 0.
        if "\n0\n" not in otext:
            failures.append("echo $? after lin/hello did not print 0")
        # exitto exits 7 -> `echo $?` prints 7 (deterministic nonzero).
        if "\n7\n" not in otext:
            failures.append("echo $? after exitto did not print 7")
        # uptime & spawns a background task: bg: pid N line + $? = 0.
        if "bg: pid" not in otext:
            failures.append("uptime & did not print 'bg: pid'")
        # The bg uptime output appears asynchronously.
        if "Uptime:" not in otext:
            failures.append("background uptime output missing")
        # bg redirect: `echo bgtag > /bg.txt &` then foreground `cat /bg.txt`.
        if "bgtag" not in otext:
            failures.append("cat /bg.txt did not show the bg-redirected file")

        if failures:
            raise AssertionError("; ".join(failures) + ";\nout:\n" + otext[-800:])
        print("PASS: env expansion, $?, background jobs, bg redirect")
        return 0
    finally:
        try:
            with open("/tmp/aos-shelltest.log", "wb") as f:
                f.write(out)
        except (NameError, OSError):
            pass
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Write `scripts/panictest.py`**

```python
#!/usr/bin/env python3
"""E2E backtrace-symbol test: run `panic`, assert the serial log shows a
kernel panic and at least one resolved frame ("name+0x.."). The kernel halts
after the panic; the test reads the log and terminates QEMU.
"""
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
SER = "/tmp/aos-panictest.ser"

def wait_for(path, seconds=10):
    end = time.time() + seconds
    while time.time() < end:
        if os.path.exists(path): return
        time.sleep(0.05)
    raise RuntimeError("timeout waiting for " + path)

def drain(s, log, end, needle=b""):
    while time.time() < end:
        try:
            d = s.recv(4096)
            if not d: break
            log += d
            if needle and needle in log: break
        except socket.timeout:
            pass
    return log

def main():
    try: os.unlink(SER)
    except FileNotFoundError: pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-no-reboot",
        "-serial", "unix:" + SER + ",server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(SER)
        time.sleep(0.5)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(1)
        s.connect(SER)

        log = drain(s, b"", time.time() + 40, b"AOS>")
        s.sendall(b"panic\n")
        # The panic backtrace prints a resolved frame line, e.g.:
        #   [0] eip=0x0010xxxx  isr_handler+0x..   (and friends)
        out = drain(s, b"", time.time() + 20, b"--- end ---")

        failures = []
        if b"KERNEL PANIC" not in out:
            failures.append("KERNEL PANIC banner missing")
        if b"--- backtrace ---" not in out or b"--- end ---" not in out:
            failures.append("backtrace block missing")
        else:
            frame_re = re.compile(r"eip=0x[0-9a-f]+\s+\w+\+0x[0-9a-f]+")
            frames = [l for l in out.decode(errors="replace").splitlines()
                      if frame_re.search(l)]
            if not frames:
                failures.append("no resolved (name+offset) backtrace frame")
            if not any("isr_handler" in f for f in frames):
                failures.append("isr_handler not among resolved frames: %r" % frames[:4])

        if failures:
            raise AssertionError("; ".join(failures) + ";\nout:\n" + out[-800:].decode(errors="replace"))
        print("PASS: panic backtrace resolves eip to symbol names")
        return 0
    finally:
        try:
            with open("/tmp/aos-panictest.log", "wb") as f:
                f.write(out)
        except (NameError, OSError):
            pass
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Register the tests in the Makefile**

Replace the `TESTS` line (currently line 118) with:

```make
TESTS = ipctest manytest notepadtest sleeptest rngtest blktest virtiotest netlooptest rtctest configtest klogtest stracetest stracelive shelltest panictest $(LINUX_TESTS)
```

- [ ] **Step 4: Sanity-check the scripts syntactically**

Run: `python3 -m py_compile scripts/shelltest.py scripts/panictest.py`
Expected: no output, exit 0.

- [ ] **Step 5: Commit**

```bash
git add scripts/shelltest.py scripts/panictest.py Makefile
git commit -m "tests: shell &/$?/env and panic backtrace symbol E2E scripts"
```

---

## Self-Review Checklist

**Spec coverage:**
- `&` (simple + redirects, `|`→error) — Task 3 ✓
- `$?` (both pid-0 exit paths, 0/127 after `&`) — Task 1 + Task 3 ✓
- `export` (list/set/empty) + `$NAME`/`$?` expansion — Task 2 ✓
- symtab (two-pass link, stub, binary search, raw fallback) — Task 4 ✓
- shelltest.py + panictest.py + TESTS — Task 5 ✓
- No GUI/WM/Linux-ABI changes — none of the tasks touch them ✓

**Placeholders:** none — every step has concrete code and exact file:line targets.

**Type consistency:** `shell_status()`/`shell_set_status(int)` (Task 1) consumed by Tasks 2-3; `shell_expand(const char*,char*,unsigned int)` (Task 2) consumed by Task 3's `commands_execute` rewrite; `path_resolve(const char*,char*,unsigned int)` (Task 3) used only inside Task 3; `addr_to_sym(unsigned int,unsigned int*)` (Task 4) used only inside `backtrace`. `find_operator`, `skip_token`, `OP_GT/OP_GTG/OP_LT/OP_PIPE` are existing names verified in `kernel/commands.c`.

**Hand-off note (AGENTS.md):** after Task 5, the human runs the new tests (`make test`, or `python3 scripts/shelltest.py` / `python3 scripts/panictest.py` individually).
