# Настоящие пайпы: pipe() syscall + произвольные цепочки `a | b | c` — дизайн

Дата: 2026-08-09

## Problem

Сейчас `|` в kernel shell работает **последовательно** через временный файл
`/pipe_tmp` (`exec_stage`, kernel/commands.c:504-537): сначала левая команда
полностью выполняется и пишет вывод в файл, потом правая читает файл. Это
не настоящий пайп:

- левая и правая части не выполняются параллельно, поток данных не течёт;
- объём вывода ограничен размером файла, а не буфером;
- цепочки `a | b | c` не поддерживаются (рекурсия через `run_command_raw`
  ломается при операторах в правой части);
- `&` с `|` печатает «bg: pipes not supported»;
- нет syscall `pipe()` для пользовательских программ.

TODO.md: пайпы — P0, строки 53 («минимальный pipe: страница общего буфера +
событие») и 85 («синтаксис `|`, запуск двух задач с pipe()»).

## Goal

1. Настоящий `pipe()` syscall (Linux ABI 42, musl-совместимый) для
   пользовательских программ.
2. Произвольные цепочки `a | b | c` в kernel shell: каждая часть —
   отдельная задача, соединённая с соседями pipe; параллельное выполнение
   с блокировкой чтения/записи в ядре.
3. Корректная семантика EOF (читатель получает `0`, когда все писатели
   закрыли конец) и EPIPE (писатель получает `-32`, когда читатель закрыл
   конец).

Вне скоупа этого шага: pipe в GUI-терминале (`programs/musl/term.c`) и
перенаправления `>`/`<` внутри pipe-частей.

## Scope

- `kernel/pipe.c` (новый): виртуальная ФС pipefs — ring-буфер 4 КБ,
  `read_at`/`write_at`/`stat`/`close`.
- `kernel/vfs.h`/`vfs.c`: хук `void (*close)(fs, ino, flags)` в
  `struct vfs_fs` + вызов в `vfs_close_fd`; `int vfs_pipe(int *rd, int *wr)`.
- `kernel/linux_syscall.c`: `case 42` (pipe) + блокирующее чтение fd0 при
  `stdin_fd >= 0` + проброс EPIPE из write fd1/2.
- `kernel/syscall.c`: блокирующее чтение fd0 при `stdin_fd >= 0` (AOS ABI);
  `route_text()` возвращает `int`.
- `kernel/commands.c`: `exec_pipe()` вместо tmp-файловой ветки `OP_PIPE`.
- `scripts/pipetest.py` (новый) + Makefile `TESTS`.
- Без изменений GUI-приложений, WM, Linux-ABI (кроме добавления pipe/read) и
  существующих тестовых ассертов.

## Architecture

### 1. pipefs (`kernel/pipe.c`)

Новая виртуальная ФС, **не монтируется** (путей в ней нет, inode резолвятся
только через fd). Статический массив:

```c
#define PIPE_BUF_SIZE 4096
#define PIPE_MAX 8

struct aos_pipe {
    unsigned char buf[PIPE_BUF_SIZE];   // ring buffer
    unsigned int head, tail, count;     // читаются/пишутся при IF=0
    int nreaders, nwriters;             // открытые read/write концы
    struct vfs_inode inode;             // valid=0 (НЕ в кеше), fs=&pipefs_fs,
                                        // ino = index + 1, type=1
    int in_use;
};

static struct aos_pipe pipes[PIPE_MAX];
static struct vfs_fs pipefs_fs;         // extern, как procfs_fs
```

Глобал `extern struct vfs_fs pipefs_fs;` в vfs.h.

**`pipe_read_at(fs, ino, buf, len, off)`**:
- пока `count == 0 && nwriters > 0` → `sti; hlt; cli` (паттерн
  `task_sleep`, kernel/task.c:528-536);
- при `count == 0 && nwriters == 0` → вернуть `0` (EOF);
- скопировать `n = min(len, count)` байт из ring, обновить
  `head`/`count`, вернуть `n`. `off` игнорируется.

**`pipe_write_at(fs, ino, buf, len, off)`**:
- цикл пока записаны не все `len` байт:
  - если `nreaders == 0` → вернуть `-32` (EPIPE);
  - если `count == PIPE_BUF_SIZE` → `sti; hlt; cli` (ждём места);
  - иначе записать `min(осталось, PIPE_BUF_SIZE - count)` байт, обновить
    `tail`/`count`.
- вернуть `len`. `off` игнорируется.

Полная запись (все `len` байт) соответствует Linux-семантике. Блокирующий
цикл без смены `state` задачи: задача остаётся `TASK_RUNNING` в syscall,
планировщик round-robin по тикам переключает на другие части пайпа; латентность
пробуждения ≤1 мс (тик 1000 Гц). Deadlock невозможен: «буфер полон» и «буфер
пуст» взаимоисключающие, поэтому в любой момент максимум одна сторона
заблокирована.

**`pipe_stat(fs, ino, st)`**: `type=1`, `size=count`, `nlink=1`, `mtime=0`.
Нужен, чтобы `vfs_fstat_fd` → `fill_stat64` (linux fstat64/fstatat64,
linux_syscall.c:504) не падал на pipe-дескрипторе.

**Заглушки остальных операций.** `openat`/`fstatat64` с dirfd, указывающим
на pipe, вызывают `vfs_get(of->inode->fs, of->inode->ino)`
(linux_syscall.c:290, 492), после чего `vfs_resolve` дойдёт до
`cur->fs->lookup`. Чтобы не словить NULL-вызов (в `struct vfs_fs` lookup
и пр. без инициализации = NULL), pipefs реализует **все** lookup-операции
как заглушки: `lookup`/`readdir`/`mkdir`/`rmdir`/`unlink`/`add_dirent`/
`remove_dirent` → `-20` (ENOTDIR), `truncate` → `-22` (EINVAL),
`alloc_inode` → 0. Реально работают только `read_at`/`write_at`/`stat`/
`close`.

**`pipe_close(fs, ino, flags)`**: по `flags` (`VFS_O_WRONLY` → `nwriters--`,
иначе `nreaders--`); если `nreaders + nwriters == 0` → `in_use = 0` (слот
свободен). Это единственный источник истины о жизненном цикле: inode имеет
`valid = 0`, поэтому `vfs_put` при `refcount == 0` безопасно ничего не
освобождает (cache_entry_release не вызывается, vfs.c:208-213).

### 2. VFS (`kernel/vfs.h`, `kernel/vfs.c`)

**Хук закрытия.** В `struct vfs_fs` добавить:

```c
void (*close)(struct vfs_fs *fs, unsigned int ino, int flags);
```

NULL для sfs2 и procfs. В `vfs_close_fd` (vfs.c:465-472), **после** `vfs_put`:

```c
if (of->inode->fs->close)
    of->inode->fs->close(of->inode->fs, of->inode->ino, of->flags);
```

**`int vfs_pipe(int *rd, int *wr)`** — создаёт pipe-концы напрямую в
глобальной `ofiles[]` (не через `vfs_open_fd`/`vfs_resolve`):

1. Найти свободный слот `pipes[]` (`in_use == 0`); нет → `VFS_ENFILE`-подобный
   код (возьмём `-24`, VFS_EMFILE).
2. Найти два свободных fd в `ofiles[3..VFS_OFILES)` (как `vfs_open_fd`).
3. `p->inode` инициализировать: `fs=&pipefs_fs`, `ino=index+1`, `type=1`,
   `valid=0`, `refcount=2`.
4. `ofiles[rd] = {.inode=&p->inode, .flags=VFS_O_RDONLY, .pos=0, .refcount=1}`,
   `ofiles[wr] = {..., .flags=VFS_O_WRONLY, ...}`.
5. `p->nreaders=1`, `p->nwriters=1`, `p->count=0`, `p->in_use=1`.

На ошибке (нет fd) — слот `pipes[]` вернуть. `vfs_pipe` объявить в vfs.h.

### 3. Блокирующее чтение stdin и проброс EPIPE

`terminal_read_key()` (terminal.c:39-53) читает key_queue **раньше**
`stdin_fd` — для pipe это баг: в очередь мог накопиться «мусор» (serial/PS/2
байты во время работы других задач), и read(0) правой части получил бы его
вместо pipe-данных. Поэтому чтение stdin для pipe делается **отдельным путём**,
`terminal_read_key` не трогаем.

**AOS `SYS_READ` fd0** (syscall.c:216-227): перед `terminal_read_key()`:

```c
struct task *t = get_current_task();
if (t->stdin_fd >= 0) {
    if (len == 0 || !in_user(buf, len)) { r->eax = -5; break; }
    r->eax = vfs_read_fd(t->stdin_fd, buf, len);   // блокируется, EOF -> 0
    break;
}
```

**Linux `read` fd0** (linux_syscall.c:264-269): аналогично:

```c
struct task *t = get_current_task();
if (t->stdin_fd >= 0) {
    r->eax = vfs_read_fd(t->stdin_fd, buf, count);
    break;
}
```

Побочный эффект: redirect `< file` (уже идущий через `stdin_fd`) тоже получает
блокирующее чтение с корректным EOF=0 вместо текущего `-EAGAIN`.

**Проброс EPIPE.** `route_text` (syscall.c:80-107) — `void`, глотает ошибку
`vfs_write_fd`. Без проброса `yes | head -1` зависнет: левая часть продолжит
писать, каждый `write_at` мгновенно вернёт `-32` (читатель закрыл конец), musl
будет считать, что запись удалась, и не завершится. Изменить:

```c
int route_text(const char *s, unsigned int len);  // 0 или отрицательный errno
```

- ветка `stdout_fd >= 0` → `return vfs_write_fd(t->stdout_fd, s, len);`
- остальные ветки (mailbox/console) → `return 0`.

Места вызова:
- Linux `write` fd1/2 (linux_syscall.c:120-122): `int rc = route_text(...);
  r->eax = (rc < 0) ? rc : count;`
- Linux `writev` (146): маршрутизация iov по-прежнему через `route_text`,
  результат — сумма длин (EPIPE игнорируется, как сейчас; writev на pipe —
  редкий путь).
- AOS `SYS_WRITE` fd1/2 (syscall.c:247-250): аналогично `r->eax = (rc<0)?rc:len`.
- AOS `SYS_PRINT`/`SYS_PUTCHAR`/route_*: результат игнорируется.

### 4. `pipe()` syscall (Linux ABI 42)

В `linux_syscall_handler` добавить:

```c
case 42: {  // pipe(int fds[2])
    int *fds = (int *)r->ebx;
    if (!in_luser(fds, 8)) { r->eax = -14; break; }   // -EFAULT
    int rd, wr;
    int rc = vfs_pipe(&rd, &wr);
    if (rc < 0) { r->eax = rc; break; }
    struct task *t = get_current_task();
    t->fds[rd] = vfs_ofile_ptr(rd);
    t->fds[wr] = vfs_ofile_ptr(wr);
    fds[0] = rd; fds[1] = wr;
    r->eax = 0;
    break;
}
```

Musl `pipe()` = syscall 42 на i386. `pipe2` (331) и AOS-ABI pipe не делаем.
Syscall запишется в trace автоматически (`trace_record`).

### 5. Shell: `a | b | c` (`kernel/commands.c`)

Новая `exec_pipe(const char *line)` заменяет tmp-файловую ветку `OP_PIPE`
(commands.c:504-537). `commands_execute` продолжает вызывать `exec_stage`
для строк с операторами; внутри `exec_stage` ветка `op == OP_PIPE` зовёт
`exec_pipe`.

Алгоритм:

1. **Разбить** `line` на части по `|` (оператор, окружённый пробелами —
   текущая семантика `find_operator`). Часть = простая команда `cmd args`.
   Для каждой части: проверить `find_operator` — если найден `>`/`<` →
   ошибка «pipe: redirect in pipe part not supported». Проверить
   `cmd_is_builtin(cmd)` → ошибка «pipe: builtin in pipe not supported».
2. **Разрешить** каждую часть через `path_resolve` (уже есть, commands.c:239);
   при провале — обычное «Unknown command: <cmd>», ничего не спавнить.
3. **Создать** N-1 pipe: массив `int pipes_fd[2*(N-1)]`, `vfs_pipe`.
4. **Спавн** всех частей (`task_spawn`, как `bg_spawn`, commands.c:543) с
   подводкой концов (как `run_bg_redirect`, commands.c:623):

```c
// часть i, pipe p = pipes_fd[2*i] (rd) / [2*i+1] (wr)
struct task *c = task_slot(pid);
if (i > 0)     { c->fds[rd_prev] = vfs_ofile_ptr(rd_prev); c->stdin_fd = rd_prev; }
if (i < N-1)   { c->fds[wr_i]    = vfs_ofile_ptr(wr_i);    c->stdout_fd = wr_i; }
```

   Часть 0: только `stdout_fd = pipe0.wr`. Средние: `stdin_fd = pipe_{i-1}.rd`,
   `stdout_fd = pipe_i.wr`. Последняя: только `stdin_fd = pipe_{N-2}.rd`.
   При провале `task_spawn` — `vfs_close_fd` всех ещё не отданных концов
   (или всех созданных; двойное закрытие безопасно: `ofile_get` вернёт NULL →
   `VFS_EBADF`), напечатать ошибку, вернуться. Левая часть, уже запущенная,
   получит EPIPE при следующей записи, когда мы закроем её read-конец (или
   завершится сама, если ничего не пишет).
5. **Ожидание**: `task_waitpid(pid)` для каждой части по порядку
   (task 0 — их parent, `task_waitpid` требует `tasks[pid].parent ==
   current_task->pid`, task.c:541; работает из idle-контекста
   `terminal_run_pending`, не из IRQ). Пока task 0 в `TASK_WAITING`,
   планировщик крутит детей, данные текут через буферы.
6. **Статус**: `shell_set_status(exit_code последней части)`.

Почему нет утечек: концы живут в глобальной `ofiles[]`; ни shell (task 0), ни
части не «владеют» ими по-особому. Каждая часть при выходе (`task_close_fds`,
task.c:83-89) закрывает свои концы → `nreaders`/`nwriters` уменьшаются → EOF
/EPIPE / free. Пока task 0 не записал `fd` себе в `fds[]`, двойного закрытия
не будет.

## Data flow

```
AOS> cat f | grep x | wc -l
       │
       ▼
 exec_pipe: split → resolve → N-1 × vfs_pipe → N × task_spawn (stdin/stdout_fd)
       │
       ▼
cat ──write(1)──► pipe0.wr ──► [pipe0 buf] ──► pipe0.rd──► read(0)──► grep
grep ──write(1)──► pipe1.wr ──► [pipe1 buf] ──► pipe1.rd──► read(0)──► wc
wc ──write(1)──► route_text (stdout_fd=-1) ──► terminal
       │
       ▼
 task 0: task_waitpid(cat); task_waitpid(grep); task_waitpid(wc)  [$? = wc]
```

```
musl pipe(fds) ──► int 0x80 eax=42 ──► linux_syscall_handler case 42
              ──► vfs_pipe ──► ofiles[rd]/[wr] + pipes[i] ──► fds[0]=rd,fds[1]=wr
```

## Error handling

- Часть не найдена → «Unknown command: <cmd>», ничего не спавнится.
- Builtin в цепочке (`cd`, `pwd`, `setpath`, `format`, `strace`, `export`)
  → «pipe: builtin in pipe not supported».
- Redirect внутри части → «pipe: redirect in pipe part not supported».
- `vfs_pipe` не нашёл слот/fd → «pipe: cannot create pipe», `$?`=1.
- `task_spawn` провалился → «pipe: spawn failed», закрыть концы.
- Писатель при закрытом читателе → EPIPE (-32) прокидывается через write
  (см. §3) в musl errno; SIGPIPE нет (в ОС нет сигналов).
- Читатель при закрытых писателях → read возвращает 0 (EOF).

## Testing

Скрипт пишет ассистент; прогоны QEMU передаются человеку (AGENTS.md).
Следовать serial-burst паттерну существующих тестов (cwdtest.py, shelltest.py):
подключиться к serial-сокету при буте, выдать burst команд при появлении
`AOS>`, до регистрации WM.

`scripts/pipetest.py` — один burst, ассерты на serial-логе:
- `ls | cat` → вывод `ls` появляется (pipe работает);
- `cat lin/test.txt | wc` → число строк/байт файла;
- `cat lin/test.txt | head -3` → первые 3 строки (частичный вывод);
- `cat lin/test.txt | head -1` → первые строки, левая получает EPIPE после
  закрытия читателя (буфер 4 КБ, блокировка);
- `echo hi | cat` → `hi`;
- `cd | ls` → «pipe: builtin in pipe not supported»;
- `cat /nonexistent | wc` → EOF-ветка, `wc` выводит 0-результат;
- цепочка из 3: `echo zzz | cat | head -1` → `zzz`;
- `pipe`-программа (musl: `pipe(2)` + write/read) — покрывает syscall 42
  (добавить `tools/linux/piptest.c` или проверить вручную);
- No `KERNEL PANIC` в логе.
- Регрессия: полный `make test` (ipctest, manytest, notepadtest, sleeptest,
  shelltest, cwdtest, linhello, lincat, lindirtest, configtest, ...) остаётся
  зелёным — изменений GUI/WM/ассертов нет.
- Build: `make` без новых `-Wall -Wextra` предупреждений.

## Out of scope

- Pipe в GUI-терминале (`programs/musl/term.c`) — следующий шаг.
- Redirect внутри pipe-частей (`a | b > f`, `a < f | b`).
- Фоновые пайпы `a | b &`, job control, `wait`.
- `pipe2` (331), `O_CLOEXEC` для pipe, AOS-ABI pipe.
- `dup` на pipe-конец: `vfs_dup_fd` увеличивает inode refcount, но не
  `nreaders`/`nwriters` — счётчики концов разойдутся (документированное
  ограничение; dup pipe в нашем коде не используется).
- SIGPIPE (нет подсистемы сигналов).
- Настоящее event-пробуждение: блокировка — busy-poll по тикам
  (латентность ≤1 мс), а не event-wake.
