# Userland-шелл `bin/sh` + term как VT-эмулятор — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Перенести шелл в userland как `bin/sh` (настоящий шелл с PATH, `$?`, историей, Tab, пайпами, redirects, фоном) и превратить `term.c` в VT-эмулятор, который спавнит `bin/sh` с stdin/stdout на пайпах.

**Architecture:** Дети `sh` запускаются через новый syscall `AOS_SPAWN_FDS` (=520), который перенаправляет fds ребёнка после `task_spawn` (до первого планирования), включая наследование родительских fds через `AOS_INHERIT_FD`. Для неблокирующего чтения вывода в `term` добавлен флаг `VFS_O_NONBLOCK` (+ `ioctl FIONBIO`, `pipe_read_nonblock`/`pipe_write_nonblock`). `term` становится ANSI/VT-парсером; `bin/sh` — интерактивным line-редактором с историей и Tab.

**Tech Stack:** x86 ring-0 ядро (C11, `-ffreestanding`, musl i386 userland), static musl i386 toolchain (`tools/musl-i686/bin/i686-linux-musl-gcc`), QEMU test harnesses (Python).

**Spec:** [docs/superpowers/specs/2026-08-14-userland-shell-design.md](../specs/2026-08-14-userland-shell-design.md)

## Global Constraints

- Язык общения и документации — русский; код и коммиты — английский (AGENTS.md).
- Все программы ABI_LINUX; syscalls < 500 → `linux_syscall_handler`, 500–519 → `aos_gui_handler`.
- Не менять ядреный `commands.c`/`terminal.c`, WM, остальные GUI-приложения, существующие тесты.
- Не вводить `dup`/`dup2` syscall: всё перенаправление — через redirs в `AOS_SPAWN_FDS`.
- `VFS_O_NONBLOCK 0x400000` (свободный бит в `open_file.flags`).
- `AOS_INHERIT_FD = 0xFFFFFFFE`, терминатор redirs — пара с `child_fd == 0xFFFFFFFF`, максимум 16 пар.
- `FIONBIO = 0x5421`. Linux errno: `EAGAIN=11`, `EPIPE=32`, `EBADF=9`, `ENOTTY=25`, `EFAULT=14`.
- Builtin'ы `bin/sh`: только `cd`, `pwd`, `export`, `setpath`, `exit`; `echo` — внешний `bin/echo`.
- Builtin внутри пайпа не поддерживается (как ядро): `sh: builtin not supported in pipeline`, `$?=1`.
- Каждый task заканчивается проверкой: сборка + QEMU-прогон + коммит.
- Тесты QEMU — через существующие каркасы (`scripts/*.py`); прогоны не блокируют TCG.

---

### Task 1: O_NONBLOCK для pipe + `ioctl FIONBIO` (ядро)

**Files:**
- Modify: `kernel/vfs.h:11-19` (добавить флаг)
- Modify: `kernel/pipe.h` (прототипы)
- Modify: `kernel/pipe.c` (неблокирующие функции)
- Modify: `kernel/vfs.c:525-549` (диспетчеризация)
- Modify: `kernel/linux_syscall.c:449-451` (ioctl)
- Modify: `programs/aosabi.h` (FIONBIO константа)

**Interfaces:**
- Produces: `int pipe_read_nonblock(struct vfs_fs *fs, unsigned int ino, void *buf, unsigned int len, unsigned int off);` и `int pipe_write_nonblock(struct vfs_fs *fs, unsigned int ino, const void *buf, unsigned int len, unsigned int off);` — возвращают `-11` (EAGAIN) при недоступности, `0` при EOF (read, нет писателей), `-32` (EPIPE) при write без читателей. В `linux_syscall.c` `case 54` обрабатывает `FIONBIO`; в `vfs_read_fd`/`vfs_write_fd` — ветка для pipefs c флагом.

- [ ] **Step 1: Добавить флаг и прототипы**

В `kernel/vfs.h` после строки 19 (`VFS_O_CREAT_DIR`):
```c
#define VFS_O_NONBLOCK 0x400000
```

В `kernel/pipe.h` (после объявления `pipe_close`):
```c
int pipe_read_nonblock(struct vfs_fs *fs, unsigned int ino, void *buf,
                       unsigned int len, unsigned int off);
int pipe_write_nonblock(struct vfs_fs *fs, unsigned int ino, const void *buf,
                        unsigned int len, unsigned int off);
```

- [ ] **Step 2: Реализовать неблокирующие функции в pipe.c**

Добавить в конец `kernel/pipe.c` (структуры `struct aos_pipe` и `pipes[]` уже в этом файле; индексация `&pipes[ino - 1]` как в существующих `pipe_read_at`):
```c
int pipe_read_nonblock(struct vfs_fs *fs, unsigned int ino, void *buf,
                       unsigned int len, unsigned int off) {
    (void)fs; (void)off;
    if (len == 0) return 0;
    struct aos_pipe *p = &pipes[ino - 1];
    if (p->count == 0)
        return p->nwriters > 0 ? -11 : 0;          // EAGAIN / EOF
    unsigned int n = len < p->count ? len : p->count;
    for (unsigned int i = 0; i < n; i++) {
        ((unsigned char *)buf)[i] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
    }
    p->count -= n;
    return (int)n;
}

int pipe_write_nonblock(struct vfs_fs *fs, unsigned int ino, const void *buf,
                        unsigned int len, unsigned int off) {
    (void)fs; (void)off;
    if (len == 0) return 0;
    struct aos_pipe *p = &pipes[ino - 1];
    if (p->nreaders == 0) return -32;              // EPIPE
    if (p->count == PIPE_BUF_SIZE) return -11;     // EAGAIN
    unsigned int space = PIPE_BUF_SIZE - p->count;
    unsigned int n = len < space ? len : space;
    for (unsigned int i = 0; i < n; i++) {
        p->buf[p->head] = ((const unsigned char *)buf)[i];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
    }
    p->count += n;
    return (int)n;
}
```

- [ ] **Step 3: Диспетчеризация в vfs_read_fd/vfs_write_fd**

В `kernel/vfs.c` добавить `#include "pipe.h"` в шапку. В `vfs_read_fd` перед `int n = of->inode->fs->read_at(...)`:
```c
    if (of->inode->fs == &pipefs_fs && (of->flags & VFS_O_NONBLOCK))
        return pipe_read_nonblock(of->inode->fs, of->inode->ino, buf, len,
                                  of->pos);
```
В `vfs_write_fd` перед `int n = of->inode->fs->write_at(...)`:
```c
    if (of->inode->fs == &pipefs_fs && (of->flags & VFS_O_NONBLOCK))
        return pipe_write_nonblock(of->inode->fs, of->inode->ino, buf, len,
                                   of->pos);
```

- [ ] **Step 4: ioctl FIONBIO**

В `kernel/linux_syscall.c` заменить блок (стр. 449-451):
```c
    case 54: {  // ioctl — FIONBIO only (0x5421)
        int fd = (int)r->ebx;
        unsigned int req = r->edx;
        struct open_file *of = vfs_ofile_ptr(fd);
        if (!of) { r->eax = -9; break; }                       // EBADF
        if (req == 0x5421) {                                    // FIONBIO
            const int *arg = (const int *)r->ecx;
            if (!in_luser(arg, 4)) { r->eax = -14; break; }    // EFAULT
            if (*arg) of->flags |= VFS_O_NONBLOCK;
            else of->flags &= ~VFS_O_NONBLOCK;
            r->eax = 0;
        } else {
            r->eax = -25;                                      // ENOTTY
        }
        break;
    }
```
(`vfs.h`, `in_luser`, `vfs_ofile_ptr`, `struct open_file` уже доступны в файле.)

В `programs/aosabi.h` (в секции констант, рядом с `#define AOS_EXT`):
```c
#ifndef FIONBIO
#define FIONBIO 0x5421
#endif
```

- [ ] **Step 5: Сборка и smoke-проверка**

Run: `make` — должна собраться без новых `-Wall -Wextra` предупреждений.
Run: `python3 scripts/pipetest.py` — пайпы работают как раньше (флаг по умолчанию не выставлен).
Expected: `PASS` для всех шагов pipetest; ISO собирается.

- [ ] **Step 6: Commit**

```bash
git add kernel/vfs.h kernel/pipe.h kernel/pipe.c kernel/vfs.c kernel/linux_syscall.c programs/aosabi.h
git commit -m "vfs: add O_NONBLOCK pipe read/write and ioctl FIONBIO"
```

---

### Task 2: `AOS_SPAWN_FDS` + `AOS_INHERIT_FD` (ядро + ABI)

**Files:**
- Modify: `programs/aosabi.h` (константы + `struct aos_redir` + wrapper)
- Modify: `kernel/aos_gui.c` (case AOS_SPAWN_FDS)

**Interfaces:**
- Consumes: `copy_lstr`, `in_luser` (aos_gui.c), `task_spawn`, `task_slot`, `get_current_task`, `vfs_ofile_ptr` — всё уже в ядре.
- Produces:
  - `#define AOS_SPAWN_FDS 520`, `#define AOS_INHERIT_FD 0xFFFFFFFE`
  - `struct aos_redir { unsigned int child_fd; unsigned int global_fd; };`
  - `int aos_spawn_fds(const char *path, const char *args, unsigned int sink, const struct aos_redir *redirs);` — pid при успехе, `<0` при ошибке.

- [ ] **Step 1: ABI-константы и wrapper**

В `programs/aosabi.h`, в секции AOS-констант (после `AOS_GET_CHILDREN`-блока):
```c
#define AOS_SPAWN_FDS 520
#define AOS_INHERIT_FD 0xFFFFFFFE
```
И рядом с существующим `aos_spawn` wrapper'ом:
```c
struct aos_redir {
    unsigned int child_fd;
    unsigned int global_fd;
};
static __attribute__((unused)) int aos_spawn_fds(const char *path,
        const char *args, unsigned int sink,
        const struct aos_redir *redirs) {
    return aos_syscall(AOS_SPAWN_FDS, (int)path, (int)args, (int)sink,
                       (int)redirs, 0);
}
```

- [ ] **Step 1a: pipe-счётчики в `vfs_dup_fd` (kernel/vfs.c)**

`vfs_dup_fd` (vfs.c:503) сейчас **мёртвый код** (только символ в symtab.c) и не
трогает pipe-счётчики. `pipe_alloc` (pipe.c) заводит `nreaders=1; nwriters=1` на
базовую пару rd/wr; каждый доп. слот на тот же inode должен `nreaders++` (RDONLY)
или `nwriters++` (WRONLY), иначе `pipe_close` преждевременно освободит слот.

В `kernel/pipe.h` добавить прототип, в `kernel/pipe.c` реализацию:
```c
void pipe_dup(struct vfs_fs *fs, unsigned int ino, int flags) {
    (void)fs;
    struct aos_pipe *p = &pipes[ino - 1];
    if (flags & VFS_O_WRONLY) p->nwriters++;
    else p->nreaders++;
}
```
В `vfs_dup_fd` после `vfs_get(...)`:
```c
if (of->inode->fs == &pipefs_fs)
    pipe_dup(of->inode->fs, of->inode->ino, flags);
```
(`flags` = `of->flags`; сохранить до `ofiles[fd2] = *of;`). vfs.c уже включает
`pipe.h`. `pipefs_fs` extern объявлен в pipe.h.

- [ ] **Step 2: case AOS_SPAWN_FDS в aos_gui.c**

В `kernel/aos_gui.c`, сразу после `case AOS_SPAWN` (после строки 200):
```c
    case AOS_SPAWN_FDS: {
        char *s = copy_lstr((const void *)r->ebx);
        if (!s) { r->eax = -5; break; }
        char *a = r->ecx ? copy_lstr((const void *)r->ecx) : 0;
        if (r->ecx && !a) { kfree(s); r->eax = -5; break; }
        struct aos_redir redirs[16];
        int nredirs = 0;
        const struct aos_redir *rp = (const struct aos_redir *)r->esi;
        while (rp && nredirs < 16) {
            if (!in_luser(rp, 8)) { kfree(s); if (a) kfree(a); r->eax = -5; break; }
            struct aos_redir rv;
            rv.child_fd = ((const unsigned int *)rp)[0];
            rv.global_fd = ((const unsigned int *)rp)[1];
            if (rv.child_fd == 0xFFFFFFFF) break;
            if (rv.child_fd >= TASK_MAX_FDS) {
                kfree(s); if (a) kfree(a); r->eax = -5; break;
            }
            if (rv.global_fd == AOS_INHERIT_FD) {
                if (!get_current_task()->fds[rv.child_fd]) {
                    kfree(s); if (a) kfree(a); r->eax = -5; break;
                }
            } else if (rv.global_fd < 3 || rv.global_fd >= VFS_OFILES ||
                       !vfs_ofile_ptr(rv.global_fd)) {
                kfree(s); if (a) kfree(a); r->eax = -5; break;
            }
            redirs[nredirs++] = rv;
            rp = (const struct aos_redir *)((const char *)rp + 8);
        }
        if (rp && nredirs >= 16) {              // too many pairs
            kfree(s); if (a) kfree(a); r->eax = -5; break;
        }
        unsigned int pid;
        // Phase 1: dup every real redirect into a private global slot owned by the
        // child (vfs_dup_fd bumps pipe counters via pipe_dup). AOS_INHERIT_FD and
        // console fds (0/1/2) are passed through untouched.
        for (int i = 0; i < nredirs; i++) {
            if (redirs[i].global_fd == AOS_INHERIT_FD) continue;
            int g2 = vfs_dup_fd((int)redirs[i].global_fd);
            if (g2 < 0) {
                for (int j = 0; j < i; j++)
                    if (redirs[j].global_fd != AOS_INHERIT_FD)
                        vfs_close_fd((int)redirs[j].global_fd);
                kfree(s); if (a) kfree(a); r->eax = g2; break;
            }
            redirs[i].global_fd = (unsigned int)g2;
        }
        int rc = task_spawn(s, a, r->edx, &pid);
        if (rc == 0) {
            struct task *c = task_slot(pid);
            struct task *parent = get_current_task();
            for (int i = 0; i < nredirs && c; i++) {
                unsigned int cfd = redirs[i].child_fd;
                unsigned int g = redirs[i].global_fd;
                if (g == AOS_INHERIT_FD) {
                    // inherit: child references the parent's handle (parent owns it)
                    if (cfd == 0) c->stdin_fd = parent->stdin_fd;
                    else if (cfd == 1) c->stdout_fd = parent->stdout_fd;
                    else c->fds[cfd] = parent->fds[cfd];
                } else {
                    // redirect: child gets its OWN dup'd global slot (it will close it)
                    c->fds[g] = vfs_ofile_ptr(g);
                    if (cfd == 0) c->stdin_fd = (int)g;
                    else if (cfd == 1) c->stdout_fd = (int)g;
                }
            }
            r->eax = (int)pid;
        } else {
            r->eax = rc;
        }
        kfree(s);
        if (a) kfree(a);
        break;
    }
```
Проверки: `AOS_INHERIT_FD`, `AOS_SPAWN_FDS` из aosabi.h (aos_gui.c уже включает его), `get_current_task()`/`TASK_MAX_FDS` из task.h, `VFS_OFILES` из vfs.h — всё уже подключено.

- [ ] **Step 3: Сборка + smoke**

Run: `make` — без новых предупреждений.
Run: `python3 scripts/shelltest.py` (или эквивалент для ядреного шелла) — регрессия: ядреные builtin/внешние команды работают.
Expected: сборка чистая, тест PASS.

- [ ] **Step 4: Commit**

```bash
git add programs/aosabi.h kernel/aos_gui.c
git commit -m "syscall: add AOS_SPAWN_FDS with fd redirects and AOS_INHERIT_FD"
```

---

### Task 3: `bin/sh` — каркас, line editor, builtins, внешние команды, `$?`

**Files:**
- Create: `programs/musl/sh.c`

**Interfaces:**
- Consumes: `aos_spawn_fds`, `AOS_INHERIT_FD` (Task 2); musl libc (`read/write/open/close/chdir/getcwd/access/pipe/sched_yield`).
- Produces: функции для Task 4/5 (в этом же файле):
  - `static int tokenize(char *s, char **argv, int max);`
  - `static int path_resolve(const char *cmd, char *out, int outsz);`
  - `static int run_builtin(int argc, char **argv);`
  - `static void execute(void);`
  - `static void handle_byte(unsigned char b);` (полный код — в Task 3; Task 5 дополнит Tab/историю)
  - `static void redraw(void);`

- [ ] **Step 1: Написать полный `programs/musl/sh.c`**

```c
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "aosabi.h"

#define LBUF       256
#define MAX_ARGS   16
#define MAX_STAGES 8

static char line[LBUF];
static int len, cur;                     // byte length / byte cursor offset
static int last_status = 0;
static char shell_path[128] = "bin";

#define MAX_VARS 16
static char var_name[MAX_VARS][32];
static char var_val[MAX_VARS][64];
static int var_count;

static const char *prompt_str = "AOS> ";

static int utf8_lead(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    return 1;
}

static int vis_len(const char *s, int n) {
    int cols = 0;
    for (int i = 0; i < n; ) {
        int cl = utf8_lead((unsigned char)s[i]);
        if (i + cl > n) break;
        cols++;
        i += cl;
    }
    return cols;
}

static void redraw(void) {
    write(1, "\r", 1);
    write(1, "\x1b[K", 3);
    write(1, prompt_str, 5);
    write(1, line, (size_t)len);
    int back = vis_len(line, len) - vis_len(line, cur);
    if (back > 0) {
        char b[16];
        int bn = snprintf(b, sizeof b, "\x1b[%dD", back);
        write(1, b, (size_t)bn);
    }
}

static void insert_byte(unsigned char c) {
    if (len >= LBUF - 1) return;
    memmove(line + cur + 1, line + cur, (size_t)(len - cur));
    line[cur++] = (char)c;
    len++;
    redraw();
}

static void backspace(void) {
    if (cur <= 0) return;
    do { cur--; } while (cur > 0 && ((unsigned char)line[cur] & 0xC0) == 0x80);
    memmove(line + cur, line + cur + 1, (size_t)(len - cur - 1));
    len--;
    redraw();
}

static void delete_char(void) {
    if (cur >= len) return;
    int cl = utf8_lead((unsigned char)line[cur]);
    memmove(line + cur, line + cur + cl, (size_t)(len - cur - cl));
    len -= cl;
    redraw();
}

static void cursor_left(void) {
    if (cur <= 0) return;
    do { cur--; } while (cur > 0 && ((unsigned char)line[cur] & 0xC0) == 0x80);
    redraw();
}

static void cursor_right(void) {
    if (cur >= len) return;
    int cl = utf8_lead((unsigned char)line[cur]);
    cur += cl;
    if (cur > len) cur = len;
    redraw();
}

static const char *env_get(const char *name) {
    for (int i = 0; i < var_count; i++)
        if (strcmp(var_name[i], name) == 0) return var_val[i];
    return 0;
}

static void env_set(const char *name, const char *val) {
    for (int i = 0; i < var_count; i++)
        if (strcmp(var_name[i], name) == 0) {
            strncpy(var_val[i], val, sizeof var_val[i] - 1);
            var_val[i][sizeof var_val[i] - 1] = 0;
            return;
        }
    if (var_count < MAX_VARS) {
        strncpy(var_name[var_count], name, sizeof var_name[0] - 1);
        strncpy(var_val[var_count], val, sizeof var_val[0] - 1);
        var_name[var_count][sizeof var_name[0] - 1] = 0;
        var_val[var_count][sizeof var_val[0] - 1] = 0;
        var_count++;
    }
}

static int tokenize(char *s, char **argv, int max) {
    int argc = 0;
    for (;;) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (argc >= max) break;
        argv[argc++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = 0;
    }
    return argc;
}

static int path_resolve(const char *cmd, char *out, int outsz) {
    if (strchr(cmd, '/')) {
        if (strlen(cmd) < (size_t)outsz) { strcpy(out, cmd); return 1; }
        return 0;
    }
    char *p = shell_path;
    while (*p) {
        char *sep = strchr(p, ':');
        int plen = sep ? (int)(sep - p) : (int)strlen(p);
        if (plen > 0 && plen + 1 + (int)strlen(cmd) + 1 <= outsz) {
            int o = 0;
            for (int i = 0; i < plen; i++) out[o++] = p[i];
            out[o++] = '/';
            for (const char *s = cmd; *s; s++) out[o++] = *s;
            out[o] = 0;
            if (access(out, F_OK) == 0) return 1;
        }
        if (!sep) break;
        p = sep + 1;
    }
    if (strlen(cmd) < (size_t)outsz) { strcpy(out, cmd); return 1; }
    return 0;
}

static int run_builtin(int argc, char **argv) {
    const char *c = argv[0];
    if (strcmp(c, "exit") == 0) {
        write(1, "\r\n", 2);
        exit(0);
    }
    if (strcmp(c, "cd") == 0) {
        if (argc < 2) { write(1, "usage: cd <path>\r\n", 18); last_status = 1; return 1; }
        if (chdir(argv[1]) != 0) {
            write(1, "cd: no such directory: ", 23);
            write(1, argv[1], strlen(argv[1]));
            write(1, "\r\n", 2);
            last_status = 1;
        } else {
            last_status = 0;
        }
        return 1;
    }
    if (strcmp(c, "pwd") == 0) {
        char buf[256];
        if (getcwd(buf, sizeof buf)) {
            write(1, buf, strlen(buf));
            write(1, "\r\n", 2);
            last_status = 0;
        }
        return 1;
    }
    if (strcmp(c, "export") == 0) {
        if (argc >= 2) {
            char *eq = strchr(argv[1], '=');
            if (eq) { *eq = 0; env_set(argv[1], eq + 1); }
        }
        last_status = 0;
        return 1;
    }
    if (strcmp(c, "setpath") == 0) {
        if (argc >= 2) {
            strncpy(shell_path, argv[1], sizeof shell_path - 1);
            shell_path[sizeof shell_path - 1] = 0;
            last_status = 0;
        } else {
            write(1, shell_path, strlen(shell_path));
            write(1, "\r\n", 2);
            last_status = 0;
        }
        return 1;
    }
    return 0;
}

static void run_stage(int argc, char **argv, int bg) {
    if (run_builtin(argc, argv)) return;
    char path[160];
    if (!path_resolve(argv[0], path, sizeof path)) {
        write(1, "Unknown command: ", 17);
        write(1, argv[0], strlen(argv[0]));
        write(1, "\r\n", 2);
        last_status = 127;
        return;
    }
    char args[LBUF];
    int o = 0;
    for (int i = 1; i < argc; i++) {
        for (char *p = argv[i]; *p && o < (int)sizeof args - 2; p++) args[o++] = *p;
        args[o++] = ' ';
    }
    if (o) o--;
    args[o] = 0;
    struct aos_redir redirs[3];
    redirs[0].child_fd = 0; redirs[0].global_fd = AOS_INHERIT_FD;
    redirs[1].child_fd = 1; redirs[1].global_fd = AOS_INHERIT_FD;
    redirs[2].child_fd = 0xFFFFFFFF; redirs[2].global_fd = 0;
    int pid = aos_spawn_fds(path, args, 0, redirs);
    if (pid < 0) {
        write(1, "cannot run command\r\n", 20);
        last_status = 1;
        return;
    }
    if (bg) {
        char b[32];
        int bn = snprintf(b, sizeof b, "bg: pid %d\r\n", pid);
        write(1, b, (size_t)bn);
        return;
    }
    last_status = aos_waitpid((unsigned int)pid);
}

static int has_operator(const char *s) {
    for (; *s; s++)
        if (*s == '|' || *s == '>' || *s == '<' || *s == '&') return 1;
    return 0;
}

static void expand(char *out, int cap, const char *in) {
    int o = 0;
    for (int i = 0; in[i] && o < cap - 1; ) {
        if (in[i] == '$') {
            int j = i + 1;
            if (in[j] == '?') {
                char t[16];
                int tn = snprintf(t, sizeof t, "%d", last_status);
                for (int k = 0; k < tn && o < cap - 1; k++) out[o++] = t[k];
                i = j + 1;
                continue;
            }
            if (in[j] >= 'A' && in[j] <= 'Z') {
                char nm[32];
                int nn = 0;
                while (in[j] && in[j] >= 'A' && in[j] <= 'Z' && nn < 31) nm[nn++] = in[j++];
                nm[nn] = 0;
                const char *v = env_get(nm);
                if (v) while (*v && o < cap - 1) out[o++] = *v++;
                i = j;
                continue;
            }
        }
        out[o++] = in[i++];
    }
    out[o] = 0;
}

static void execute(void) {
    char buf[LBUF];
    memcpy(buf, line, (size_t)len);
    buf[len] = 0;
    write(1, "\r\n", 2);
    if (len == 0) { redraw(); return; }
    char exp[LBUF];
    expand(exp, sizeof exp, buf);

    int bg = 0;
    int n = (int)strlen(exp);
    if (n > 0 && exp[n - 1] == '&') { bg = 1; exp[--n] = 0; }

    if (has_operator(exp)) {
        write(1, "sh: pipelines and redirects: not implemented yet\r\n", 50);
        last_status = 1;
        redraw();
        return;
    }
    char *argv[MAX_ARGS];
    int argc = tokenize(exp, argv, MAX_ARGS);
    if (argc > 0) run_stage(argc, argv, bg);
    redraw();
}

static int in_esc = 0;
static int esc_n = 0;

static void handle_byte(unsigned char b) {
    if (in_esc) {
        if (in_esc == 1) {
            if (b == '[') { in_esc = 2; esc_n = 0; }
            else in_esc = 0;
            return;
        }
        if (b >= '0' && b <= '9') { esc_n = esc_n * 10 + (b - '0'); return; }
        in_esc = 0;
        if (b == ';') return;
        switch (b) {
        case 'C': cursor_right(); break;
        case 'D': cursor_left(); break;
        case 'H': cur = 0; redraw(); break;
        case 'F': cur = len; redraw(); break;
        case '~': if (esc_n == 3) delete_char(); break;
        }
        return;
    }
    if (b == 0x1b) { in_esc = 1; return; }
    switch (b) {
    case '\r': execute(); break;
    case '\b':
    case '\x7f': backspace(); break;
    case '\t': break;                        // Task 5
    default:
        if (b >= 0x20) insert_byte(b);
        break;
    }
}

int main(void) {
    redraw();
    for (;;) {
        unsigned char b;
        int r = read(0, &b, 1);
        if (r == 0) break;                   // EOF: term закрылся
        if (r < 0) { sched_yield(); continue; }  // serial: очередь пуста
        handle_byte(b);
    }
    return 0;
}
```

- [ ] **Step 2: Сборка**

Run: `make` — должен появиться `build/prog/sh.elf` (общее правило `build/prog/%.elf` уже есть) без предупреждений; `bin/sh` вшивается в ramdisk.

- [ ] **Step 3: Serial smoke — базовые команды**

Создать `scripts/shtest.py` (полный вариант — в Task 7; здесь базовая версия). Прогнать:
```bash
python3 scripts/shtest.py
```
Пока скрипт не написан, проверить вручную через `make debug` (qemu-vnc MCP): на `AOS>` ввести `bin/sh`, затем `pwd`, `ls /bin`, `echo $?`, `notacmd`, `echo $?`, `exit`.
Expected: prompt `AOS> ` от sh; `pwd`/`ls /bin` выводят; `echo $?` → `0` после `ls` и `127` после `notacmd`; `exit` возвращает в ядреный шелл; нет `KERNEL PANIC`.

- [ ] **Step 4: Commit**

```bash
git add programs/musl/sh.c
git commit -m "userland: add bin/sh interactive shell (line editor, builtins, PATH, $?)"
```

---

### Task 4: `bin/sh` — пайпы, redirects, фон

**Files:**
- Modify: `programs/musl/sh.c`

**Interfaces:**
- Consumes: `tokenize`, `path_resolve`, `run_builtin`, `expand` (Task 3).
- Produces: `static int split_stages(char *s, char **out, int max);`, `static int parse_redirs(char **argv, int *argc, const char **in_f, const char **out_f, int *append);`, новая `execute()` и новая `run_stage(...)` с redirs.

- [ ] **Step 1: Заменить `has_operator` на `split_stages`**

Удалить `has_operator` и добавить (оператор только если перед ним пробел, как ядро):
```c
static int split_stages(char *s, char **out, int max) {
    int n = 0;
    for (;;) {
        while (*s == ' ' || *s == '\t') s++;
        if (n >= max) return -1;
        out[n++] = s;
        int i = 0;
        while (s[i] && !(s[i] == '|' && i > 0 && s[i - 1] == ' ')) i++;
        if (!s[i]) break;
        s[i] = 0;
        s = s + i + 1;
    }
    return n;
}
```

- [ ] **Step 2: Добавить `parse_redirs`**

```c
static int parse_redirs(char **argv, int *argc, const char **in_f,
                        const char **out_f, int *append) {
    int w = 0;
    for (int i = 0; i < *argc; i++) {
        if (strcmp(argv[i], ">") == 0 || strcmp(argv[i], ">>") == 0) {
            if (i + 1 >= *argc) return -1;
            *out_f = argv[i + 1];
            *append = (argv[i][1] == '>');
            i++;
        } else if (strcmp(argv[i], "<") == 0) {
            if (i + 1 >= *argc) return -1;
            *in_f = argv[i + 1];
            i++;
        } else {
            argv[w++] = argv[i];
        }
    }
    *argc = w;
    return 0;
}
```

- [ ] **Step 3: Переписать `run_stage` с redirs**

Заменить `run_stage` (из Task 3) на:
```c
static void run_stage(int i, int nstages, int argc, char **argv,
                      int *pipes, int *pids, int bg) {
    const char *in_f = 0, *out_f = 0;
    int append = 0;
    if (nstages == 1) {
        if (parse_redirs(argv, &argc, &in_f, &out_f, &append) < 0) {
            write(1, "sh: bad redirect\r\n", 18);
            last_status = 1;
            return;
        }
    }
    if (argc == 0) { pids[i] = -1; return; }
    if (nstages > 1 && run_builtin(argc, argv)) {
        write(1, "sh: builtin not supported in pipeline\r\n", 39);
        last_status = 1;
        return;
    }
    struct aos_redir redirs[2 + MAX_ARGS + 1];
    int nr = 0;
    int has_in = 0, has_out = 0;
    if (i > 0) {
        redirs[nr].child_fd = 0; redirs[nr].global_fd = pipes[2 * (i - 1)];
        nr++; has_in = 1;
    }
    if (i + 1 < nstages) {
        redirs[nr].child_fd = 1; redirs[nr].global_fd = pipes[2 * i + 1];
        nr++; has_out = 1;
    }
    int kept[8];
    int nkeep = 0;
    if (in_f) {
        int fd = open(in_f, O_RDONLY, 0);
        if (fd < 0) {
            write(1, "sh: cannot open ", 16);
            write(1, in_f, strlen(in_f));
            write(1, "\r\n", 2);
            last_status = 1;
            return;
        }
        redirs[nr].child_fd = 0; redirs[nr].global_fd = fd; nr++;
        kept[nkeep++] = fd;
        has_in = 1;
    }
    if (out_f) {
        int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
        int fd = open(out_f, flags, 0644);
        if (fd < 0) {
            write(1, "sh: cannot open ", 16);
            write(1, out_f, strlen(out_f));
            write(1, "\r\n", 2);
            last_status = 1;
            return;
        }
        redirs[nr].child_fd = 1; redirs[nr].global_fd = fd; nr++;
        kept[nkeep++] = fd;
        has_out = 1;
    }
    if (!has_in) {
        redirs[nr].child_fd = 0; redirs[nr].global_fd = AOS_INHERIT_FD; nr++;
    }
    if (!has_out) {
        redirs[nr].child_fd = 1; redirs[nr].global_fd = AOS_INHERIT_FD; nr++;
    }
    redirs[nr].child_fd = 0xFFFFFFFF; redirs[nr].global_fd = 0; nr++;

    char path[160];
    if (!path_resolve(argv[0], path, sizeof path)) {
        write(1, "Unknown command: ", 17);
        write(1, argv[0], strlen(argv[0]));
        write(1, "\r\n", 2);
        last_status = 127;
        return;
    }
    char args[LBUF];
    int o = 0;
    for (int a = 1; a < argc; a++) {
        for (char *p = argv[a]; *p && o < (int)sizeof args - 2; p++) args[o++] = *p;
        args[o++] = ' ';
    }
    if (o) o--;
    args[o] = 0;

    int pid = aos_spawn_fds(path, args, 0, redirs);
    for (int k = 0; k < nkeep; k++) close(kept[k]);
    if (pid < 0) {
        write(1, "cannot run command\r\n", 20);
        last_status = 1;
        pids[i] = -1;
        return;
    }
    pids[i] = pid;
    if (bg) {
        char b[32];
        int bn = snprintf(b, sizeof b, "bg: pid %d\r\n", pid);
        write(1, b, (size_t)bn);
    }
}
```

- [ ] **Step 4: Переписать `execute` с пайпами и redirects**

Заменить `execute` (из Task 3) на:
```c
static void execute(void) {
    char buf[LBUF];
    memcpy(buf, line, (size_t)len);
    buf[len] = 0;
    write(1, "\r\n", 2);
    if (len == 0) { redraw(); return; }
    char exp[LBUF];
    expand(exp, sizeof exp, buf);

    int bg = 0;
    int n = (int)strlen(exp);
    if (n > 0 && exp[n - 1] == '&') { bg = 1; exp[--n] = 0; }

    char *stage[MAX_STAGES];
    int nstages = split_stages(exp, stage, MAX_STAGES);
    if (nstages < 0) {
        write(1, "sh: too many stages\r\n", 21);
        last_status = 1;
        redraw();
        return;
    }
    if (nstages == 0) { redraw(); return; }

    int pipes[2 * (MAX_STAGES - 1)];
    int npipes = nstages - 1;
    for (int i = 0; i < npipes; i++) {
        if (pipe(pipes + 2 * i) != 0) {
            write(1, "sh: pipe failed\r\n", 17);
            last_status = 1;
            redraw();
            return;
        }
    }
    int pids[MAX_STAGES];
    for (int i = 0; i < MAX_STAGES; i++) pids[i] = -1;

    for (int i = 0; i < nstages; i++) {
        char *argv[MAX_ARGS];
        int argc = tokenize(stage[i], argv, MAX_ARGS);
        if (argc == 0) { pids[i] = -1; continue; }
        run_stage(i, nstages, argc, argv, pipes, pids, bg);
    }
    for (int i = 0; i < npipes; i++) {
        close(pipes[2 * i]);
        close(pipes[2 * i + 1]);
    }
    if (!bg) {
        for (int i = 0; i < nstages; i++)
            if (pids[i] > 0) last_status = aos_waitpid((unsigned int)pids[i]);
    }
    redraw();
}
```

- [ ] **Step 5: Сборка + serial smoke — пайпы и redirects**

Run: `make`.
Serial smoke через `make debug`: на `AOS>` → `bin/sh`, затем:
- `echo hi > f` → `cat f` → `hi`
- `ls /bin | bin/cat` → список bin (пайп AOS→AOS)
- `bin/ls / | bin/cat | lin/cat` → список (3 стадии)
- `ls /bin &` → `bg: pid N`
- `exit`
Expected: корректный вывод, никакого `KERNEL PANIC`. Внимание: `echo`/`ls`/`cat` — внешние программы; вывод каждой идёт на console через INHERIT.

- [ ] **Step 6: Commit**

```bash
git add programs/musl/sh.c
git commit -m "userland: bin/sh pipelines, redirects and background tasks"
```

---

### Task 5: `bin/sh` — история и Tab-дополнение

**Files:**
- Modify: `programs/musl/sh.c`

**Interfaces:**
- Consumes: `line`/`len`/`cur`, `utf8_lead`, `redraw`, `shell_path`, `handle_byte` (Task 3).
- Produces: `static void hist_push(void);`, `static void hist_prev(void);`, `static void hist_next(void);`, `static void tab_complete(void);`.

- [ ] **Step 1: Добавить историю**

Вставить после `var_count`-блока (после `env_set`):
```c
#define HIST_MAX 16
static char hist[HIST_MAX][LBUF];
static int hist_count;
static int hist_cur = -1;

static void hist_push(void) {
    if (len == 0) return;
    if (hist_count > 0 && strcmp(hist[(hist_count - 1) % HIST_MAX], line) == 0)
        return;
    memcpy(hist[hist_count % HIST_MAX], line, (size_t)len + 1);
    hist_count++;
    hist_cur = -1;
}

static void hist_load(int idx) {
    memcpy(line, hist[idx % HIST_MAX], LBUF);
    len = (int)strlen(line);
    cur = len;
    redraw();
}

static void hist_prev(void) {
    if (hist_count == 0) return;
    if (hist_cur < 0) hist_cur = hist_count - 1;
    else if (hist_cur > hist_count - HIST_MAX && hist_cur > 0) hist_cur--;
    else return;
    hist_load(hist_cur);
}

static void hist_next(void) {
    if (hist_cur < 0) return;
    if (hist_cur == hist_count - 1) {
        hist_cur = -1;
        line[0] = 0; len = 0; cur = 0;
        redraw();
        return;
    }
    hist_cur++;
    hist_load(hist_cur);
}
```

- [ ] **Step 2: Добавить Tab-дополнение**

Вставить после `hist_next`:
```c
static char tab_word[64];
static int tab_word_off;
static int tab_idx;
static int tab_nmatches;
static char tab_matches[40][64];

static void tab_collect(void) {
    int ws = cur;
    while (ws > 0 && line[ws - 1] != ' ') ws--;
    tab_word_off = ws;
    int wl = cur - ws;
    if (wl >= (int)sizeof tab_word) wl = (int)sizeof tab_word - 1;
    memcpy(tab_word, line + ws, (size_t)wl);
    tab_word[wl] = 0;
    tab_nmatches = 0;
    char *p = shell_path;
    while (*p && tab_nmatches < 40) {
        char *sep = strchr(p, ':');
        int plen = sep ? (int)(sep - p) : (int)strlen(p);
        if (plen > 0) {
            char dir[96];
            int o = 0;
            for (int i = 0; i < plen && o < 95; i++) dir[o++] = p[i];
            dir[o] = 0;
            DIR *d = opendir(dir);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)) && tab_nmatches < 40)
                    if (strncmp(e->d_name, tab_word, (size_t)wl) == 0)
                        strncpy(tab_matches[tab_nmatches++], e->d_name, 63);
                closedir(d);
            }
        }
        if (!sep) break;
        p = sep + 1;
    }
}

static void tab_replace_word(const char *m) {
    int off = tab_word_off;
    while (off < len && line[off] != ' ')
        off += utf8_lead((unsigned char)line[off]);
    memmove(line + tab_word_off, line + off, (size_t)(len - off));
    len -= (off - tab_word_off);
    cur = tab_word_off;
    for (const char *s = m; *s && len < LBUF - 1; s++) line[cur++] = *s;
    len = cur;
    redraw();
}

static void tab_complete(void) {
    if (cur != len) return;
    if (tab_nmatches == 0 || tab_idx == 0) {
        tab_idx = 0;
        tab_collect();
    }
    if (tab_nmatches == 0) return;
    if (tab_nmatches == 1) {
        tab_idx = 1;
        tab_replace_word(tab_matches[0]);
        return;
    }
    if (tab_idx > 0) {                       // повторный Tab — цикл
        tab_replace_word(tab_matches[tab_idx % tab_nmatches]);
        tab_idx++;
        return;
    }
    const char *m0 = tab_matches[0];
    int pl = 0;
    for (;;) {
        int all = 1;
        for (int i = 1; i < tab_nmatches; i++)
            if (tab_matches[i][pl] != m0[pl]) { all = 0; break; }
        if (!all || !m0[pl]) break;
        pl++;
    }
    if (pl > 0) {
        tab_idx = 0;
        char pre[64];
        int o = 0;
        for (int i = 0; i < pl && o < 63; i++) pre[o++] = m0[i];
        pre[o] = 0;
        tab_replace_word(pre);
    } else {
        tab_idx = 1;
        write(1, "\r\n", 2);
        for (int i = 0; i < tab_nmatches; i++) {
            write(1, tab_matches[i], strlen(tab_matches[i]));
            write(1, "  ", 2);
        }
        write(1, "\r\n", 2);
        redraw();
    }
}
```
В `sh.c` добавить `#include <dirent.h>` в шапку.

- [ ] **Step 3: Подключить в `handle_byte` и `execute`**

Заменить `case '\t': break;` на:
```c
    case '\t': tab_complete(); break;
```
В `handle_byte`, в ветке `if (b == 0x1b) { in_esc = 1; return; }`, перед ней добавить сброс состояния дополнения при обычном вводе. Также добавить в `insert_byte` и `backspace` в начало: `tab_idx = 0;`.
В `execute`, в самом начале (после `write(1, "\r\n", 2);`):
```c
    hist_push();
    tab_idx = 0;
```

В `handle_byte` в состоянии ESC (после `switch (b) {`, до `case 'C':`), добавить историю:
```c
        case 'A': hist_prev(); break;
        case 'B': hist_next(); break;
```

- [ ] **Step 4: Сборка + serial smoke — история и Tab**

Run: `make`.
Serial smoke через `make debug`: `bin/sh`, набрать `pwd`, Enter; стрелку Up (serial: послать `ESC [ A`) → строка `pwd` снова; `pw`+Tab → `pwd`; `ec`+Tab → `echo`; `exit`. Tab выводит список при неоднозначности (`l`+Tab → список `ls linrun ...`).
Expected: история и дополнение работают, без паники.

- [ ] **Step 5: Commit**

```bash
git add programs/musl/sh.c
git commit -m "userland: bin/sh history and tab completion"
```

---

### Task 6: `term.c` → VT-эмулятор

**Files:**
- Modify: `programs/musl/term.c` (переписать)

**Interfaces:**
- Consumes: `aos_spawn_fds`, `AOS_INHERIT_FD` не нужен здесь (term задаёт конкретные концы пайпов), `pipe()` (linux 42), `ioctl FIONBIO` (Task 1), `aos_recv`/`aos_send`/MSG_* (как сейчас), `utf8_encode` (уже есть в term.c).
- Produces: полноценный VT-эмулятор: `term_out_byte(unsigned char b)` (ANSI-парсер), `send_key(unsigned int key)`, `spawn_sh(void)`.

- [ ] **Step 1: Переписать `programs/musl/term.c`**

Полный файл:
```c
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "aosabi.h"
#include "theme.h"

#ifndef FIONBIO
#define FIONBIO 0x5421
#endif

#define TW  80
#define TH  26
#define FONT_W 8
#define FONT_H 16

static unsigned int col_fg = 0xD8D8D8;
static unsigned int col_bg = 0x101010;

static unsigned int screen[TH][TW];   // codepoints per cell (0 = empty)
static int crow, ccol;
static unsigned int *win;
static unsigned int winid;
static int w, h;
static char utfbuf[TH * (TW * 3 + 1) + 1];

static int sh_alive;
static int fd_in;                       // term -> sh (keys)
static int fd_out;                      // sh -> term (output, FIONBIO)
static int cursor_visible = 1;

static int u_len;                       // pending UTF-8 continuation bytes
static int u_cp;

static int utf8_encode(char *out, unsigned int cp) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

static void newline(void) {
    if (crow < TH - 1) {
        crow++;
    } else {
        for (int r = 0; r < TH - 1; r++)
            for (int c = 0; c < TW; c++)
                screen[r][c] = screen[r + 1][c];
        for (int c = 0; c < TW; c++)
            screen[TH - 1][c] = 0;
    }
    ccol = 0;
}

static void put_cp(unsigned int cp) {
    if (cp == '\r') { ccol = 0; return; }
    if (cp == '\n') { newline(); return; }
    if (cp == '\b') {
        if (ccol > 0) { ccol--; screen[crow][ccol] = 0; }
        return;
    }
    if (cp < 0x20 || cp == 0x7F) return;
    if (ccol >= TW) newline();
    screen[crow][ccol] = cp;
    ccol++;
}

static void render(void) {
    aos_fill(win, (unsigned int)w * 4, 0, 0, w, h, col_bg);
    int pos = 0;
    for (int r = 0; r < TH; r++) {
        for (int c = 0; c < TW; c++) {
            unsigned int cp = screen[r][c];
            if (cp) pos += utf8_encode(utfbuf + pos, cp);
        }
        utfbuf[pos++] = '\n';
    }
    utfbuf[pos] = 0;
    aos_render_text(win, (unsigned int)w * 4, 0, 0, utfbuf, col_fg, col_bg);
    if (cursor_visible && sh_alive)
        aos_fill(win, (unsigned int)w * 4, ccol * FONT_W, crow * FONT_H + 14,
                 FONT_W, 2, col_fg);
    struct aos_msg m = {MSG_UPDATE, winid, 0, 0, 0};
    aos_send((unsigned int)aos_get_event_pid(), &m);
}

// ---- ANSI/VT output parser ----
static int esc_state;              // 0 idle, 1 ESC, 2 ESC[
static int esc_n, esc_r;

static void term_out_byte(unsigned char b) {
    if (esc_state == 1) {
        if (b == '[') { esc_state = 2; esc_n = 0; esc_r = 0; }
        else esc_state = 0;
        return;
    }
    if (esc_state == 2) {
        if (b == '?') { esc_n = -1; return; }
        if (b >= '0' && b <= '9') { if (esc_n >= 0) esc_n = esc_n * 10 + (b - '0'); return; }
        if (b == ';') { esc_r = esc_n; esc_n = 0; return; }
        int pn = esc_n, pr = esc_r;
        esc_state = 0;
        if (pn < 0) {                    // ESC[?25h / ESC[?25l
            cursor_visible = (b == 'h');
            return;
        }
        if (pn == 0 && b == 'H') { crow = 0; ccol = 0; return; }   // ESC[H
        if (b == 'H') {                                           // CUP r;c
            crow = pr > 0 ? pr - 1 : 0;
            ccol = pn > 0 ? pn - 1 : 0;
            if (crow < 0) crow = 0;
            if (crow >= TH) crow = TH - 1;
            if (ccol < 0) ccol = 0;
            if (ccol >= TW) ccol = TW - 1;
            return;
        }
        if (b == 'K') {                   // EL
            for (int c = ccol; c < TW; c++) screen[crow][c] = 0;
            return;
        }
        if (b == 'J') {                   // ED (2J -> full clear)
            for (int r = 0; r < TH; r++)
                for (int c = 0; c < TW; c++)
                    screen[r][c] = 0;
            crow = 0; ccol = 0;
            return;
        }
        if (b == 'D') {                   // CUB
            ccol -= pn > 0 ? pn : 1;
            if (ccol < 0) ccol = 0;
            return;
        }
        if (b == 'C') {                   // CUF
            ccol += pn > 0 ? pn : 1;
            if (ccol >= TW) ccol = TW - 1;
            return;
        }
        return;
    }
    if (b == 0x1b) { u_len = 0; esc_state = 1; return; }
    if (u_len > 0) {
        u_cp = (u_cp << 6) | (b & 0x3F);
        if (--u_len == 0) put_cp((unsigned int)u_cp);
        return;
    }
    if (b < 0x80) { put_cp(b); return; }
    if (b >= 0xC0 && b < 0xE0) { u_len = 1; u_cp = b & 0x1F; return; }
    if (b >= 0xE0 && b < 0xF0) { u_len = 2; u_cp = b & 0x0F; return; }
}

static void print_out(const char *s) {
    while (*s) term_out_byte((unsigned char)*s++);
}

// ---- input: keys -> sh ----
static void send_key(unsigned int key) {
    char buf[4];
    switch (key) {
    case 0x0101: write(fd_in, "\x1b[A", 3); break;    // UP
    case 0x0102: write(fd_in, "\x1b[B", 3); break;    // DOWN
    case 0x0103: write(fd_in, "\x1b[D", 3); break;    // LEFT
    case 0x0104: write(fd_in, "\x1b[C", 3); break;    // RIGHT
    case 0x0105: write(fd_in, "\x1b[H", 3); break;    // HOME
    case 0x0106: write(fd_in, "\x1b[F", 3); break;    // END
    case 0x0107: write(fd_in, "\x1b[3~", 4); break;   // DEL
    case '\r': write(fd_in, "\r", 1); break;
    case '\t': write(fd_in, "\t", 1); break;
    case '\b': write(fd_in, "\x7f", 1); break;
    default:
        if (key < 0x100) {
            char c = (char)key;
            write(fd_in, &c, 1);
        } else {
            int n = utf8_encode(buf, key);
            write(fd_in, buf, (size_t)n);
        }
        break;
    }
}

static void spawn_sh(void) {
    int in[2], out[2];
    if (pipe(in) != 0 || pipe(out) != 0) return;
    struct aos_redir redirs[3];
    redirs[0].child_fd = 0; redirs[0].global_fd = in[0];
    redirs[1].child_fd = 1; redirs[1].global_fd = out[1];
    redirs[2].child_fd = 0xFFFFFFFF; redirs[2].global_fd = 0;
    int pid = aos_spawn_fds("bin/sh", "", 0, redirs);
    if (pid < 0) {
        close(in[0]); close(in[1]);
        close(out[0]); close(out[1]);
        return;
    }
    close(in[0]);
    close(out[1]);
    fd_in = in[1];
    fd_out = out[0];
    int one = 1;
    ioctl(fd_out, FIONBIO, &one);
    sh_alive = 1;
    cursor_visible = 1;
}

int main(void) {
    unsigned int my = (unsigned int)getpid();
    struct aos_msg m = {MSG_CREATE, TW * FONT_W, TH * FONT_H, my, 0};
    aos_send((unsigned int)aos_get_event_pid(), &m);

    for (;;) {
        if (aos_recv(&m) == 0 && m.type == MSG_WININFO) {
            winid = m.a;
            unsigned int slab = m.b;
            win = (unsigned int *)(AOS_SLAB_BASE + slab * AOS_SLAB_SIZE);
            break;
        }
        sched_yield();
    }
    w = TW * FONT_W;
    h = TH * FONT_H;

    theme_load();
    col_fg = theme_color("theme_text_fg", 0xD8D8D8);
    col_bg = theme_color("theme_text_bg", 0x101010);

    render();
    spawn_sh();

    for (;;) {
        if (sh_alive) {
            unsigned char buf[256];
            int n = read(fd_out, buf, sizeof buf);
            if (n > 0) {
                for (int i = 0; i < n; i++) term_out_byte(buf[i]);
                render();
            } else if (n == 0) {
                sh_alive = 0;
                print_out("\r\n[sh exited]\r\n");
                render();
            }
        }
        if (aos_recv(&m) == 0) {
            switch (m.type) {
            case MSG_KEY:
                if (sh_alive) {
                    send_key(m.a);
                } else if (m.a == '\r') {
                    spawn_sh();
                    render();
                }
                break;
            case MSG_CLOSE:
                return 0;
            }
        }
        sched_yield();
    }
}
```

- [ ] **Step 2: Сборка**

Run: `make` — должен пересобраться `build/prog/term.elf` без предупреждений.

- [ ] **Step 3: GUI smoke через qemu-vnc**

Run: `make debug`, подключиться через qemu-vnc MCP (`vm_connect`, порт 5907, QMP `/tmp/aos-debug.qmp`, serial `/tmp/aos-debug.serial`), кликнуть иконку term в dock, в окне term набрать `pwd`, Enter, `echo hi`, Enter. Сделать скриншот.
Expected: в окне term появляется `AOS> ` prompt, после `pwd` — путь `/`, после `echo hi` — `hi`; курсор мигает; WM жив; нет `KERNEL PANIC`.

- [ ] **Step 4: Commit**

```bash
git add programs/musl/term.c
git commit -m "term: rewrite as VT emulator spawning bin/sh over pipes"
```

---

### Task 7: `bin/sh` в PROGRAMS + автотест `shtest.py` + регрессия

**Files:**
- Modify: `Makefile:24` (PROGRAMS)
- Create: `scripts/shtest.py`

**Interfaces:**
- Consumes: всё из Task 1–6.
- Produces: `python3 scripts/shtest.py` — serial-тест bin/sh.

- [ ] **Step 1: Добавить `sh` в PROGRAMS**

В `Makefile`, в строке `PROGRAMS = ...` (стр. 24), вставить `sh` (например, после `rm`):
```makefile
PROGRAMS = help uptime clear echo tick info reboot panic ls cat rm sh format shutdown test wm term clock date ipctest notepad many linrun sleeptest exitto random fstest procinfo bgspawn cp mv mkdir rmdir head wc
```
`build/prog/sh.elf` уже строится общим правилом `build/prog/%.elf`.

- [ ] **Step 2: Написать `scripts/shtest.py`**

По образцу `scripts/pipetest.py` (headless QEMU, serial-сокет):
```python
#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-shtest.sock"
SER = "/tmp/aos-shtest.sock"

BOOT_MARKS = [
    "VFS: / [sfs2] root=1",
    "Filesystem ready.",
    "AOS>",
]

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
            if needle and needle in log:
                break
        except socket.timeout:
            pass
    return log

def cmd(s, line, needle, seconds=30):
    """Send a line to the (already running) userland shell and drain to needle."""
    s.sendall(line.encode() + b"\n")
    out = drain(s, b"", time.time() + seconds, needle)
    if b"KERNEL PANIC" in out:
        raise AssertionError("kernel panic: %r\n%s"
                             % (line, out[-400:].decode(errors="replace")))
    return out

def main():
    for path in (MON, SER):
        try: os.unlink(path)
        except FileNotFoundError: pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-serial", "unix:" + SER + ",server,nowait",
        "-monitor", "unix:" + MON + ",server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(SER)
        time.sleep(0.5)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(1)
        s.connect(SER)

        # Boot: drain to the kernel shell prompt, check marks.
        log = drain(s, b"", time.time() + 40, b"AOS>")
        text = log.decode(errors="replace")
        if b"KERNEL PANIC" in log:
            raise AssertionError("kernel panic during boot:\n" + text[-400:])
        for mark in BOOT_MARKS:
            if mark not in text:
                raise AssertionError("serial log missing %r; tail:\n%s"
                                     % (mark, text[-400:]))
        print("PASS: boot OK")

        # Start bin/sh, wait for its prompt (echoed by the kernel shell).
        out = cmd(s, "bin/sh", b"AOS> ")
        if b"AOS> " not in out:
            raise AssertionError("bin/sh did not print its prompt; out:\n%s"
                                 % out[-600:].decode(errors="replace"))
        print("PASS: bin/sh prompt")

        # pwd — cwd of the shell (printed via getcwd).
        out = cmd(s, "pwd", b"AOS> ")
        if b"/" not in out:
            raise AssertionError("pwd did not print a path; out:\n%s"
                                 % out[-600:].decode(errors="replace"))
        print("PASS: pwd")

        # PATH lookup: `ls /bin` lists bin/ without an explicit path.
        out = cmd(s, "ls /bin", b"AOS> ")
        if b"uptime" not in out:
            raise AssertionError("ls /bin did not list bin/; out:\n%s"
                                 % out[-600:].decode(errors="replace"))
        print("PASS: PATH lookup (ls /bin)")

        # $? after a successful command.
        out = cmd(s, "echo $?", b"AOS> ")
        if b"0" not in out:
            raise AssertionError("echo $? != 0 after success; out:\n%s"
                                 % out[-600:].decode(errors="replace"))
        print("PASS: $? == 0 after success")

        # Unknown command -> $? == 127.
        cmd(s, "definitely_not_a_cmd", b"AOS> ")
        out = cmd(s, "echo $?", b"AOS> ")
        if b"127" not in out:
            raise AssertionError("$? != 127 after unknown cmd; out:\n%s"
                                 % out[-600:].decode(errors="replace"))
        print("PASS: $? == 127 after unknown command")

        # Redirect: echo hi > f ; cat f.
        cmd(s, "echo hi > f", b"AOS> ")
        out = cmd(s, "cat f", b"AOS> ")
        if b"hi" not in out:
            raise AssertionError("redirect: cat f did not print 'hi'; out:\n%s"
                                 % out[-600:].decode(errors="replace"))
        print("PASS: redirect (echo hi > f; cat f)")

        # Two-stage AOS pipe.
        out = cmd(s, "ls /bin | bin/cat", b"AOS> ")
        if b"uptime" not in out:
            raise AssertionError("ls /bin | bin/cat empty; out:\n%s"
                                 % out[-600:].decode(errors="replace"))
        print("PASS: ls /bin | bin/cat")

        # Three-stage pipe.
        out = cmd(s, "bin/ls / | bin/cat | lin/cat", b"AOS> ")
        if b"bin" not in out:
            raise AssertionError("3-stage pipe empty; out:\n%s"
                                 % out[-600:].decode(errors="replace"))
        print("PASS: bin/ls / | bin/cat | lin/cat")

        # AOS->Linux pipe.
        out = cmd(s, "bin/ls | lin/cat", b"AOS> ")
        if b"bin" not in out:
            raise AssertionError("bin/ls | lin/cat empty; out:\n%s"
                                 % out[-600:].decode(errors="replace"))
        print("PASS: bin/ls | lin/cat")

        # exit returns to the kernel shell.
        out = cmd(s, "exit", b"AOS>")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic on exit; out:\n%s"
                                 % out[-400:].decode(errors="replace"))
        print("PASS: exit returns to kernel shell")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Прогнать shtest**

Run: `make && python3 scripts/shtest.py`
Expected: все `PASS`; без `KERNEL PANIC`.
Замечание: за `bin/sh` ввод serial идёт в key_queue (`user_program_active`), поэтому `cmd()` шлёт строки, пока sh активен; между командами sh печатает `AOS> `, что и есть needle.

- [ ] **Step 4: Полная регрессия**

Run: `make test`
Expected: зелёные ipctest, manytest, notepadtest, sleeptest, shelltest, cwdtest, linhello, lincat, lindirtest, pipetest, configtest и т.д. — ядреный шелл, WM и остальные приложения не менялись.

- [ ] **Step 5: Commit**

```bash
git add Makefile scripts/shtest.py
git commit -m "build: add bin/sh to PROGRAMS; add shtest.py regression"
```

---

## Self-Review

**Спека → план:**
- A (AOS_SPAWN_FDS) → Task 2. ✓ (включая AOS_INHERIT_FD из обновлённой спеки)
- B (O_NONBLOCK + FIONBIO) → Task 1. ✓
- C (bin/sh) → Task 3 (каркас), Task 4 (пайпы/redirects/фон), Task 5 (история/Tab). ✓
- D (term → VT) → Task 6. ✓
- E (Makefile) → Task 7. ✓
- Testing (shtest serial + GUI smoke) → Task 7 (serial), Task 6 Step 3 (GUI smoke). ✓
- Out of scope (format, job control, dup2, resize) — нигде не реализуется. ✓

**Placeholder scan:** во всех code-шагах приведён полный код; проверки-команды конкретные; нет «TBD»/«add error handling».

**Type consistency:** `aos_spawn_fds(path, args, 0, redirs)` — сигнатура совпадает с Task 2; `pipe_read_nonblock`/`pipe_write_nonblock` — 5 параметров как объявлено в Task 1; `run_stage(i, nstages, argc, argv, pipes, pids, bg)` — определение в Task 4 и вызов в нём же согласованы; `struct aos_redir {child_fd; global_fd;}` везде одинаков; терминатор `0xFFFFFFFF` в sh.c и ядре совпадает.
