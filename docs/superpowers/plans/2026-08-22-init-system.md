# Init-система `/bin/init` — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ввести процесс `/bin/init`: конфиг-зависимый запуск сервисов (wm, clock), сбор зомби детей через mailbox-нотификации, обязательный respawn с backoff и crash-loop guard. Ядро — репарентинг сирот на init, перезапуск init при его смерти, кооперативный `AOS_KILL` + builtin `kill`.

**Architecture:** Ядро остаётся task 0 (pid 0); `bin/init` — первый заспавненный таск с env `AOS_MODE=gui|text`. Смерть любого ребёнка init'а (сервиса или репарентированной сироты) кладёт `MSG_EXIT{pid, code}` в его mailbox; init делает `waitpid` и респавнит сервисы по `/etc/init.conf` (`svcN.path/args/respawn/mode`, embedded в ramdisk). Kill кооперативный: `task_kill()` ставит `kill_pending`, целевой таск завершается с кодом 9 на следующем syscall (или сразу из `task_sleep`). Главный цикл ядра оживляет умерший init (`ensure_init`) не чаще раза в 100 тиков.

**Tech Stack:** x86 ring-0 ядро (C11, `-ffreestanding`), static musl i386 userland (`tools/musl-i686/bin/i686-linux-musl-gcc`), QEMU test harnesses (Python).

**Spec:** [docs/superpowers/specs/2026-08-22-init-system-design.md](../specs/2026-08-22-init-system-design.md)

## Global Constraints

- Язык общения и документации — русский; код и коммиты — английский (AGENTS.md).
- Все программы ABI_LINUX; syscalls < 500 → `linux_syscall_handler`, 500–599 → `aos_gui_handler` (`kernel/syscall.c:146-149`). Новый kill — `AOS_KILL 525`.
- Не менять: `kernel/terminal.c`, vfs/pipe, WM и остальные GUI-приложения, существующие тесты.
- Респавн сервисов обязателен (решение пользователя): backoff 100 тиков, crash-loop guard = 5 смертей короче 100 тиков подряд → `failed`, сброс счётчика после 6000 тиков жизни.
- Код выхода убитого таска — 9; `kill 0` запрещён всегда.
- Каждый task заканчивается проверкой: сборка + QEMU-прогон + коммит.
- Commit identity не настроен → `git -c user.name="vladimka" -c user.email="32310898+vladimka@users.noreply.github.com" commit ...`.

---

### Task 1: init_pid, репарентинг сирот, MSG_EXIT родителю-init (ядро)

**Files:**
- Modify: `kernel/task.h` (прототипы)
- Modify: `kernel/task.c` (`init_pid`, exited-блок `task_switch_kernel`)

**Interfaces:**
- Produces: `void task_set_init_pid(unsigned int pid);`, `unsigned int task_init_pid(void);`
- Behavior: смерть таска X → живые дети X получают `parent = init_pid`; смерть ребёнка init'а → `MSG_EXIT{a=pid, b=exit_code}` в mailbox init'а.

- [ ] **Step 1: Прототипы в task.h**

В `kernel/task.h` после `int task_get_children(...)` (строка 61):
```c
void task_set_init_pid(unsigned int pid);
unsigned int task_init_pid(void);
```

- [ ] **Step 2: Переменная и акцессоры в task.c**

В `kernel/task.c` рядом с `static struct task *current_task;` (строка 47):
```c
// Pid of /bin/init (set by kernel_main). Orphaned children are re-parented
// here and report their exit through the mailbox (SIGCHLD analog). 0 = none.
static unsigned int init_pid = 0;

void task_set_init_pid(unsigned int pid) { init_pid = pid; }
unsigned int task_init_pid(void) { return init_pid; }
```

- [ ] **Step 3: Exited-блок task_switch_kernel**

В `task_switch_kernel` после существующего цикла сбора зомби-детей (строки 190-194, `tasks[i].state = TASK_FREE;`), перед закрывающей скобкой `if (exited) { dead = ... }`:
```c
        // Re-parent surviving children of a dying non-init task to init so
        // their later exits are reported and reaped there. Children of a
        // dying init keep the stale parent until ensure_init() hands them to
        // the new instance via task_reassign_children().
        if (init_pid > 0 && init_pid != dead->pid) {
            for (int i = 1; i < MAX_TASKS; i++)
                if (tasks[i].parent == dead->pid &&
                    tasks[i].state != TASK_FREE &&
                    tasks[i].state != TASK_ZOMBIE)
                    tasks[i].parent = init_pid;
        }
        // A child of init died (own service or an adopted orphan): queue the
        // SIGCHLD-style notice so the init loop can waitpid() the zombie.
        if (dead->parent == init_pid && dead->parent > 0 &&
            dead->parent != dead->pid && task_alive(dead->parent))
            task_mailbox_send(dead->parent, MSG_TYPE_EXIT, dead->pid,
                              current_exit_code, 0, 0);
```
(`MSG_TYPE_EXIT` = 6 определён в начале task.c:37; поля совпадают с `MSG_EXIT` из aosabi.h.)

- [ ] **Step 4: Сборка**

Run: `make`
Expected: собирается чисто, без новых предупреждений (поведение пока не меняется: `init_pid == 0`).

- [ ] **Step 5: Commit**

```bash
git add kernel/task.h kernel/task.c
git -c user.name="vladimka" -c user.email="32310898+vladimka@users.noreply.github.com" commit -m "task: track init pid, re-parent orphans, notify init on child exit"
```

---

### Task 2: Кооперативный kill: task_kill + kill_pending + AOS_KILL

**Files:**
- Modify: `kernel/task.h` (поле + прототип)
- Modify: `kernel/task.c` (`task_kill`, проверка в `task_sleep`)
- Modify: `kernel/syscall.c` (проверка в начале `syscall_handler`)
- Modify: `kernel/aos_gui.c` (`case AOS_KILL`)
- Modify: `programs/aosabi.h` (константа + wrapper)

**Interfaces:**
- Produces: `int task_kill(unsigned int pid);` (0 = ок, -1 = отказ); поле `unsigned int kill_pending;` в `struct task`; `#define AOS_KILL 525`; wrapper `int aos_kill(unsigned int pid);`
- Semantics: цель завершается с кодом 9 на следующем syscall или внутри `task_sleep`; `kill(0)`/мёртвый pid → `-1`.

- [ ] **Step 1: Поле и прототип**

В `kernel/task.h` в `struct task` после `unsigned int wait_pid;` (строка 31):
```c
    unsigned int kill_pending; // set by task_kill: exit(9) on next syscall
```
Рядом с `int task_waitpid(...)`:
```c
int task_kill(unsigned int pid);
```

- [ ] **Step 2: task_kill в task.c**

После `task_waitpid` (после строки 590):
```c
#define KILL_EXIT_CODE 9

// Cooperative kill: mark the target so it exits with code 9 at its next
// syscall (or immediately from task_sleep). pid 0 is the kernel and can
// never be killed; killing init is allowed (the main loop revives it).
int task_kill(unsigned int pid) {
    if (pid == 0 || pid >= MAX_TASKS || !task_alive(pid)) return -1;
    unsigned int f;
    irq_save(&f);
    tasks[pid].kill_pending = 1;
    irq_restore(f);
    serial_print("KILL:pid=");
    serial_print_dec(pid);
    serial_print("\n");
    return 0;
}
```

- [ ] **Step 3: Проверка в task_sleep**

В начало `task_sleep` (перед `irq_save`, строка ~547):
```c
    if (current_task->kill_pending) {
        current_task->kill_pending = 0;
        task_exit_current(KILL_EXIT_CODE);
        return;
    }
```

- [ ] **Step 4: Проверка в syscall_handler**

В `kernel/syscall.c` в самом начале `syscall_handler`, сразу после `unsigned int n = r->eax;` (строка 144):
```c
    // Cooperative kill: a task marked by AOS_KILL exits at its next syscall,
    // before any dispatch (covers both ABI routes below).
    if (get_current_task()->kill_pending) {
        get_current_task()->kill_pending = 0;
        task_exit_current(9);
        r->eax = 0;
        return;
    }
```

- [ ] **Step 5: case AOS_KILL в aos_gui.c**

В `kernel/aos_gui.c` после `case AOS_SPAWN_FDS_ENV {...}` (строка 347):
```c
    case AOS_KILL:
        r->eax = task_kill(r->ebx);
        break;
```

- [ ] **Step 6: Константа и wrapper в aosabi.h**

В `programs/aosabi.h` после `#define AOS_SPAWN_FDS_ENV 524` (строка 36):
```c
#define AOS_KILL          525
```
Рядом с `aos_waitpid` (после строки 180):
```c
static __attribute__((unused)) int aos_kill(unsigned int pid) { return aos_syscall(AOS_KILL, (int)pid, 0, 0, 0, 0); }
```

- [ ] **Step 7: Сборка + smoke**

Run: `make`
Expected: чистая сборка. Поведенческих изменений без вызывающих сторон ещё нет; регресс `python3 scripts/pipetest.py` должен остаться зелёным.

- [ ] **Step 8: Commit**

```bash
git add kernel/task.h kernel/task.c kernel/syscall.c kernel/aos_gui.c programs/aosabi.h
git -c user.name="vladimka" -c user.email="32310898+vladimka@users.noreply.github.com" commit -m "syscall: cooperative AOS_KILL with kill_pending exit path"
```

---

### Task 3: Builtin `kill` в ядерном шелле

**Files:**
- Modify: `kernel/commands.c`

**Interfaces:**
- Consumes: `task_kill` (Task 2), `terminal_print*`.
- Produces: команда `kill <pid>` serial-консоли.

- [ ] **Step 1: cmd_is_builtin**

В `cmd_is_builtin` (commands.c:356-360) добавить `"kill"`:
```c
    return strcmp(cmd, "format") == 0 || strcmp(cmd, "setpath") == 0 ||
           strcmp(cmd, "cd") == 0 || strcmp(cmd, "pwd") == 0 ||
           strcmp(cmd, "strace") == 0 || strcmp(cmd, "export") == 0 ||
           strcmp(cmd, "kill") == 0;
```

- [ ] **Step 2: cmd_kill + диспетчер**

Перед `run_command_raw` (рядом с `cmd_strace`):
```c
// kill <pid>: cooperative kill via task_kill (the target exits with code 9
// at its next syscall). Refuses pid 0 and unknown pids.
static void cmd_kill(const char *arg) {
    while (*arg == ' ') arg++;
    if (*arg < '0' || *arg > '9') {
        terminal_print("\nusage: kill <pid>");
        return;
    }
    unsigned int pid = 0;
    while (*arg >= '0' && *arg <= '9') { pid = pid * 10 + (unsigned)(*arg - '0'); arg++; }
    if (task_kill(pid) == 0) {
        terminal_print("\nkill: pid ");
        terminal_print_dec(pid);
        terminal_print(" signaled");
    } else {
        terminal_print("\nkill: no such process");
    }
}
```
В диспетчере `run_command_raw` после блока `export` (commands.c:569-572):
```c
    if (strcmp(cmd, "kill") == 0) {
        cmd_kill(arg);
        return;
    }
```
(`task.h` уже включён commands.c — там используется `get_current_task()`/`task_spawn`.)

- [ ] **Step 3: Сборка + ручной smoke**

Run: `make`
Run: `make run` (или `make debug`): в serial-шелле `ps`, затем `kill <pid любого процесса>`, затем `kill 0`, `kill 999`.
Expected: `KILL:pid=N` в логе, `kill: pid N signaled`; для 0/999 — `kill: no such process`; цели исчезают из `ps` (код 9 виден в `TEC:`-строке); нет паники.

- [ ] **Step 4: Commit**

```bash
git add kernel/commands.c
git -c user.name="vladimka" -c user.email="32310898+vladimka@users.noreply.github.com" commit -m "shell: add kill builtin (cooperative process termination)"
```

---

### Task 4: `/bin/init` + `/etc/init.conf`

**Files:**
- Create: `programs/musl/init.c`
- Create: `scripts/init.conf`
- Modify: `Makefile` (PROGRAMS, `--data`)

**Interfaces:**
- Consumes: `aos_spawn`, `aos_recv`, `aos_waitpid`, `aos_get_tick`, `MSG_EXIT` (aosabi.h), musl (`open/read/close/getenv/snprintf/usleep`).
- Produces: сервис-менеджер; сообщения журнала: `init: started (pid N, mode gui|text)`, `init: started <path> (pid P)`, `init: exited <path> (code C, life L ticks)`, `init: respawn <path> scheduled`, `init: <path> crashed 5x fast, giving up`.

- [ ] **Step 1: scripts/init.conf**

```text
svc1.path=bin/wm
svc1.respawn=1
svc1.mode=gui
svc2.path=bin/clock
svc2.respawn=1
svc2.mode=gui
```

- [ ] **Step 2: programs/musl/init.c**

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "aosabi.h"

#define MAX_SVC           8
#define CONF_MAX          1024
#define BACKOFF_TICKS     100    // 1 s pause before a respawn
#define FAST_DEATH_TICKS  100    // life shorter than this counts as a fast death
#define RESET_LIFE_TICKS  6000   // living longer than this resets the counter
#define MAX_FAST_DEATHS   5      // fast deaths in a row -> give up

struct svc {
    char path[64];
    char args[64];
    int respawn;
    int gui_only;
    int active;
    int failed;
    int pending;              // waiting for the backoff to expire
    unsigned int due_tick;
    int pid;
    unsigned int start_tick;
    int fast_deaths;
};

static struct svc svcs[MAX_SVC];
static int nsvc;              // highest svc index seen in the config

static void say(const char *s) { write(1, s, strlen(s)); }

static void spawn_svc(struct svc *s) {
    s->pending = 0;
    int pid = aos_spawn(s->path, s->args, 0);
    char b[128];
    if (pid <= 0) {
        int n = snprintf(b, sizeof b, "init: spawn failed: %s\r\n", s->path);
        write(1, b, (size_t)n);
        return;
    }
    s->active = 1;
    s->pid = pid;
    s->start_tick = (unsigned int)aos_get_tick();
    int n = snprintf(b, sizeof b, "init: started %s (pid %d)\r\n", s->path, pid);
    write(1, b, (size_t)n);
}

static void load_conf(void) {
    int fd = open("/etc/init.conf", O_RDONLY, 0);
    if (fd < 0) { say("init: cannot open /etc/init.conf\r\n"); return; }
    char buf[CONF_MAX];
    int n = (int)read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    struct svc *cur = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(0, "\n")) {
        char *cr = strchr(line, '\r');
        if (cr) *cr = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *key = line, *val = eq + 1;
        if (strncmp(key, "svc", 3) != 0) continue;
        char *dot = strchr(key + 3, '.');
        if (!dot) continue;
        int idx = atoi(key + 3);
        if (idx < 1 || idx > MAX_SVC) continue;
        cur = &svcs[idx - 1];
        dot++;
        if (strcmp(dot, "path") == 0) {
            strncpy(cur->path, val, sizeof cur->path - 1);
            cur->path[sizeof cur->path - 1] = 0;
            if (idx > nsvc) nsvc = idx;
        } else if (strcmp(dot, "args") == 0) {
            strncpy(cur->args, val, sizeof cur->args - 1);
            cur->args[sizeof cur->args - 1] = 0;
        } else if (strcmp(dot, "respawn") == 0) {
            cur->respawn = atoi(val);
        } else if (strcmp(dot, "mode") == 0) {
            cur->gui_only = (strcmp(val, "gui") == 0);
        }
    }
}

int main(void) {
    load_conf();
    const char *mode = getenv("AOS_MODE");
    int gui = mode && strcmp(mode, "gui") == 0;
    char b[80];
    snprintf(b, sizeof b, "init: started (pid %d, mode %s)\r\n",
             (int)getpid(), gui ? "gui" : "text");
    say(b);
    for (int i = 0; i < nsvc; i++) {
        struct svc *s = &svcs[i];
        if (!s->path[0]) continue;
        if (s->gui_only && !gui) continue;
        spawn_svc(s);
    }
    struct aos_msg m;
    for (;;) {
        if (aos_recv(&m) == 0 && m.type == MSG_EXIT) {
            for (int i = 0; i < nsvc; i++) {
                struct svc *s = &svcs[i];
                if (!s->active || s->pid != (int)m.a) continue;
                int code = aos_waitpid((unsigned int)m.a);
                s->active = 0;
                s->pid = 0;
                unsigned int now = (unsigned int)aos_get_tick();
                unsigned int life = now - s->start_tick;
                char lb[128];
                int ln = snprintf(lb, sizeof lb,
                                  "init: exited %s (code %d, life %u ticks)\r\n",
                                  s->path, code, life);
                write(1, lb, (size_t)ln);
                if (life > RESET_LIFE_TICKS) s->fast_deaths = 0;
                else if (life < FAST_DEATH_TICKS) s->fast_deaths++;
                if (s->respawn && !s->failed) {
                    if (s->fast_deaths >= MAX_FAST_DEATHS) {
                        s->failed = 1;
                        ln = snprintf(lb, sizeof lb,
                                      "init: %s crashed %dx fast, giving up\r\n",
                                      s->path, s->fast_deaths);
                        write(1, lb, (size_t)ln);
                    } else {
                        s->pending = 1;
                        s->due_tick = now + BACKOFF_TICKS;
                        ln = snprintf(lb, sizeof lb,
                                      "init: respawn %s scheduled\r\n", s->path);
                        write(1, lb, (size_t)ln);
                    }
                }
            }
            continue;
        }
        unsigned int now = (unsigned int)aos_get_tick();
        for (int i = 0; i < nsvc; i++) {
            struct svc *s = &svcs[i];
            if (s->pending && !s->failed && (int)(now - s->due_tick) >= 0)
                spawn_svc(s);
        }
        usleep(10000);
    }
    return 0;
}
```
Замечания: `MSG_EXIT` (=6) определён в aosabi.h; sink=0 у детей → их stdout идёт на консоль (не мешает); `usleep` опирается на поддержанный nanosleep (162).

- [ ] **Step 3: Makefile**

В `PROGRAMS` (строка 26) добавить `init` (например, `... bgspawn cp mv mkdir rmdir head wc sync envp ps init`).
В recipe `kernel/progs.c` (строки 131-133) добавить данные:
```make
	kernel/progs.c: $(PROG_ELFS) scripts/demo.ico scripts/init.conf scripts/gen_progs.py $(LINUX_BINS)
	$(PYTHON) scripts/gen_progs.py $(PROG_ELFS) --data demo.ico=scripts/demo.ico \
		--data etc/init.conf=scripts/init.conf \
		$(LINUX_EMBED) > $@
```
(`build/prog/init.elf` соберётся общим правилом `build/prog/%.elf`.)

- [ ] **Step 4: Сборка**

Run: `make`
Expected: `build/prog/init.elf` собран, в `kernel/progs.c` появились `prog_init_elf[]` и `data_etc_init_conf[]`.

- [ ] **Step 5: Smoke (init ещё не спавнится ядром)**

Пока `kernel_main` не знает про init (Task 5) — запустить вручную: boot, в serial `ps` (wm/clock от ядра), затем `bin/init`: в логе `init: cannot open /etc/init.conf`? Нет — файл уже embedded: ожидаем `init: started (pid N, mode text)` (env пустой) и отсутствие сервисов (`mode=gui`, а AOS_MODE не задан). Ctrl-выход: `kill <pid init>`.
Expected: сообщения согласно логике, нет паники.

- [ ] **Step 6: Commit**

```bash
git add programs/musl/init.c scripts/init.conf Makefile
git -c user.name="vladimka" -c user.email="32310898+vladimka@users.noreply.github.com" commit -m "userland: add bin/init service manager and embedded /etc/init.conf"
```

---

### Task 5: kernel_main — spawn init вместо wm, ensure_init

**Files:**
- Modify: `kernel/kernel.c`
- Modify: `kernel/task.c` (+ `task_reassign_children`)
- Modify: `kernel/task.h` (прототип)

**Interfaces:**
- Produces: `void task_reassign_children(unsigned int old_parent, unsigned int new_parent);` — живым детям старого родителя меняет parent, зомби FREE-ит.
- Behavior: ядро спавнит только `bin/init`; GUI появляется, если init.conf попросил wm в режиме gui; умерший init перезапускается главным циклом.

- [ ] **Step 1: task_reassign_children в task.c**

После `task_init_pid` (Task 1):
```c
// After init died and was re-spawned under a new pid: hand over its
// surviving children and discard zombies nobody can waitpid anymore.
void task_reassign_children(unsigned int old_parent, unsigned int new_parent) {
    if (old_parent == 0 || old_parent == new_parent) return;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].parent != old_parent) continue;
        if (tasks[i].state == TASK_ZOMBIE)
            tasks[i].state = TASK_FREE;
        else
            tasks[i].parent = new_parent;
    }
}
```
Прототип — в `kernel/task.h` рядом с `task_set_init_pid`:
```c
void task_reassign_children(unsigned int old_parent, unsigned int new_parent);
```

- [ ] **Step 2: ensure_init в kernel.c**

Перед `kernel_main`:
```c
// Revive /bin/init when it dies (crash or kill): spawn a fresh instance,
// hand it the survivors of the dead one, discard its unreaped zombies.
static void ensure_init(void) {
    static unsigned int next_try = 0;
    unsigned int ip = task_init_pid();
    if (ip != 0 && task_alive(ip)) return;
    if ((int)(tick - next_try) < 0) return;      // throttle: one try per 100 ms
    next_try = tick + 100;
    static const char init_env_gui[] = "AOS_MODE=gui";
    static const char init_env_text[] = "AOS_MODE=text";
    unsigned int npid = 0;
    int rc = task_spawn("bin/init", "", 0, &npid,
                        vga_fb_active() ? init_env_gui : init_env_text);
    if (rc != 0) {
        serial_print("init: respawn attempt failed\n");
        return;
    }
    unsigned int old = ip;
    task_set_init_pid(npid);
    task_reassign_children(old, npid);
    printf("init spawned (pid %u).\n", npid);
}
```
Примечание: строковый литерал `"AOS_MODE=gui"` содержит завершающий `\0`; двойной NUL для copy_env_block даёт сам литерал + терминатор строки C → фактически байты `...gui\0\0`. (Если `copy_env_block` потребует явный второй NUL — использовать `"AOS_MODE=gui\0"`.)
В начале тела `while (1)` главного цикла (перед `terminal_pending_cmd`):
```c
        ensure_init();
```

- [ ] **Step 3: Заменить spawn wm на spawn init**

Удалить блок `if (vga_fb_active()) { ... } else { printf("Text mode: window manager not started.\n"); }` (kernel.c:152-172) и заменить на:
```c
    // Multitasking + GUI: /bin/init reads /etc/init.conf and starts services
    // (the window manager et al). The idle loop above revives init if it
    // dies. Text-mode boot passes AOS_MODE=text, so gui-only services stay
    // down and the console remains with the kernel shell.
    ensure_init();
```
(Первый вызов пройдёт throttle-free: `next_try == 0`, `tick` к этому моменту мал, но `(int)(tick - 0) >= 0` — попытка состоится.)

- [ ] **Step 4: Сборка + GUI smoke**

Run: `make`
Run: `python3 scripts/vguitest.py`
Expected: `vgu: active`, рабочий стол рисуется (wm теперь ребёнок init'а); в логе `init spawned (pid N).`, `init: started bin/wm (pid M)`, `init: started bin/clock (pid K)`; `KERNEL PANIC` отсутствует.

- [ ] **Step 5: Kill-init selfheal**

Run: `python3 scripts/vguitest.py`-подобный прогон вручную через `make debug`: в serial `kill <pid init>` → `TEC:pid=N code=9`, затем `init spawned (pid M).`, WM продолжает работать (новый init не перезапускает wm — тот остался жив и перепарентился).
Expected: система жива, десктоп не пропал.

- [ ] **Step 6: Commit**

```bash
git add kernel/kernel.c kernel/task.c kernel/task.h
git -c user.name="vladimka" -c user.email="32310898+vladimka@users.noreply.github.com" commit -m "kernel: boot via /bin/init, auto-revive it from the main loop"
```

---

### Task 6: Регрессия `scripts/inittest.py` + TESTS

**Files:**
- Create: `scripts/inittest.py`
- Modify: `Makefile` (TESTS)

**Interfaces:**
- Consumes: QTest-каркас (`scripts/qtest.py`: `boot_and_ready`, `type_text`, `serial_read`, `screenshot` — по образцу `scripts/sleeptest.py`).
- Produces: автотест init-системы.

- [ ] **Step 1: scripts/inittest.py**

Скелет (уточнить API по qtest.py; `serial_read` может возвращать накопленный лог — сверить со sleeptest):
```python
#!/usr/bin/env python3
import re
import sys
import time

from qtest import QTest


def wait_log(q, pattern, timeout=30):
    end = time.time() + timeout
    rx = re.compile(pattern)
    log = ""
    while time.time() < end:
        time.sleep(0.5)
        log += q.serial_read()
        m = rx.search(log)
        if m:
            return m, log
    raise AssertionError("timeout waiting for %r; tail:\n%s" % (pattern, log[-2000:]))


def main():
    with QTest("inittest") as q:
        q.boot_and_ready()
        _, log = wait_log(q, r"init: started \(pid \d+, mode gui\)")

        # 1) init and wm are listed by ps
        q.type_text("ps\n")
        _, log = wait_log(q, r"init\s*$")
        m = re.search(r"^(\d+)\s+\d+\s+\S+\s+linux\s+wm$", log, re.M)
        assert m, "wm not found in ps:\n%s" % log
        wm_pid = int(m.group(1))

        # 2) kill wm -> exit code 9 -> init respawns it under a new pid
        q.type_text("kill %d\n" % wm_pid)
        _, log = wait_log(q, r"TEC:pid=%d code=9" % wm_pid)
        m, _ = wait_log(q, r"init: started bin/wm \(pid (\d+)\)")
        assert int(m.group(1)) != wm_pid, "wm respawned with the same pid?"

        # 3) negative cases
        q.type_text("kill 0\n")
        _, log = wait_log(q, r"kill: no such process")
        q.type_text("kill 999\n")
        _, log = wait_log(q, r"kill: no such process")

        assert "KERNEL PANIC" not in log, "kernel panic during inittest"
    print("PASS: init system (service start, respawn, kill)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4 (fix-up): подгонка под реальный формат `ps`/лога**

Запустить `python3 scripts/inittest.py`; если regex по `ps`/serial не сходятся — поправить шаблоны (STATE колонка: `ready/run/sleep/wait/zomb/spawn`; ABI: `aos/linux`; строки TEC пишутся как `TEC:pid=N code=M`). Ничего кроме regexes в тесте не менять.

- [ ] **Step 3: Регистрация в TESTS**

В Makefile (строка 183) вставить `inittest` после `pstest`:
```make
TESTS = ... pstest inittest textmodetest ...
```

- [ ] **Step 4: Полная регрессия**

Run: `make test-fast` (ipctest, linhello, lincat)
Run: `python3 scripts/inittest.py && python3 scripts/pstest.py && python3 scripts/sleeptest.py && python3 scripts/ipctest.py && python3 scripts/pipetest.py`
Run: `python3 scripts/vguitest.py && python3 scripts/powertest.py && python3 scripts/notepadtest.py && python3 scripts/configtest.py && python3 scripts/tablettest.py`
Expected: все зелёные; PPid процессов в `pstest` согласован (у сирот — pid init'а); GUI-тесты не заметили переезда wm под init.

- [ ] **Step 5: Commit**

```bash
git add scripts/inittest.py Makefile
git -c user.name="vladimka" -c user.email="32310898+vladimka@users.noreply.github.com" commit -m "tests: add inittest regression (respawn, kill, ps integration)"
```

---

## Чеклист приёмки

- [ ] Boot GUI: wm/clock стартуют из `/etc/init.conf`, журнал init в COM1.
- [ ] `kill <wm>` → код 9 → респавн с новым pid ≤ ~1.5 c, десктоп оживает.
- [ ] `kill <init>` → ядро пересоздаёт init, дети перепарентятся, зомби убраны.
- [ ] `kill 0`/`kill 999` → `kill: no such process`, ничего не ломается.
- [ ] Text-mode entry: init стартует в mode text, gui-сервисов нет, консоль работает.
- [ ] `make test-fast`, GUI-тесты, `pstest`, `pipetest` — зелёные.
