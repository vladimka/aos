# Userland-шелл `bin/sh` + term как VT-эмулятор — дизайн

Дата: 2026-08-14

## Problem

Сейчас GUI-терминал `programs/musl/term.c` — это самостоятельная программа с
**собственным** мини-шелом: он парсит только `cd`/`pwd`/`bin/<cmd>` (term.c:108-180),
у него свой prompt `aos>`, своя логика курсора, нет PATH/`$?`/env/пайпов/
redirects/Tab/истории. Полноценный шелл живёт только в ядре
(`kernel/commands.c` + `kernel/terminal.c`) и принимает ввод только через serial
COM1: PS/2-клавиатура уходит в WM (`route_gui_key` → `MSG_KEY`), поэтому из GUI
в настоящий шелл попасть нельзя — `term` не работает как шелл.

Пользователь выбрал архитектуру: **перенести шелл в userland как `bin/sh`**;
каждый `term` спавнит свою копию с stdin/stdout на пайпах → получаем настоящий
шелл в GUI, N независимых терминалов, фундамент для fork/exec (TODO 1.2).
Ядреный шелл остаётся для serial/VGA-консоли (отладка) — его не трогаем.

## Goal

1. Настоящий пользовательский шелл `bin/sh` (static musl, ABI_LINUX) с полным
   набором фич ядреного шелла: PATH-поиск, `cd`/`pwd`/`export`/`setpath`/`exit`,
   `$?` и `$NAME`, история, Tab-дополнение, пайпы `a | b | c`, redirects
   `> >> <`, фон `&`, запуск и `bin/*`, и `lin/*`.
2. `term.c` превращается в **VT-эмулятор**: рисует вывод sh, передаёт клавиши;
   свой мини-шелл удаляется.
3. Минимальные правки ядра: redirect fds при spawn (без `dup2`) + O_NONBLOCK
   для pipe.

Вне скоупа этого шага: `format` в bin/sh (остаётся ядреным builtin'ом для
serial-консоли), job control / `wait`, сигналы, `pipe2`/`O_CLOEXEC`, resize
окна.

## Scope

- `programs/aosabi.h`: `AOS_SPAWN_FDS` = 520, `AOS_INHERIT_FD` = 0xFFFFFFFE,
  `VFS_O_NONBLOCK`-флаг на уровне syscall ioctl, константа `FIONBIO` для
  userland.
- `kernel/aos_gui.c`: `case AOS_SPAWN_FDS` (redirect fds при spawn).
- `kernel/vfs.h`/`vfs.c`: флаг `VFS_O_NONBLOCK` в `struct open_file`; неблокирующие
  ветки pipe-read/write через vfs_read_fd/vfs_write_fd.
- `kernel/linux_syscall.c`: `case 54` ioctl — поддержка `FIONBIO` (set/clear).
- `kernel/pipe.c`/`pipe.h`: `pipe_read_nonblock`/`pipe_write_nonblock`
  (возвращают `-11`/`-EAGAIN` вместо блокировки).
- `programs/musl/sh.c` (новый): userland-шелл.
- `programs/musl/term.c`: переделка в VT-эмулятор.
- `Makefile`: `build/prog/sh.elf`, `sh` в `PROGRAMS`.
- `scripts/shtest.py` (новый): serial-тест bin/sh + GUI smoke через QTest.
- Без изменений: ядреный `commands.c`/`terminal.c`, WM, остальные GUI-приложения,
  существующие тесты.

## Architecture

### A. Redirect fds при spawn: `AOS_SPAWN_FDS` (=520)

Без fork/exec классический `dup2` в ребёнке невозможен (spawn сразу запускает
ELF, у родителя нет доступа к `fds[]` ребёнка). Ядреный `exec_pipe`
(commands.c:447-452) уже делает перенаправление **после** `task_spawn`,
подменяя `c->fds[fd]` до первого планирования ребёнка. Выносим этот механизм
в syscall.

Сигнатура (aosabi.h + aos_gui.c):

```c
// r->ebx = path, r->ecx = args, r->edx = sink, r->esi = redirs_ptr
// redirs_ptr → массив пар {u32 child_fd, u32 global_fd};
// терминатор — пара с child_fd == 0xFFFFFFFF. Максимум 16 пар.
#define AOS_SPAWN_FDS 520
```

Обработка в `aos_gui_handler` (по образцу `AOS_SPAWN`, aos_gui.c:182-200):

1. Скопировать `path`/`args` через `copy_lstr` (как AOS_SPAWN).
2. Если `redirs_ptr` ненулевой: скопировать пары из user-памяти
   (`in_luser(redirs_ptr, 8)` на каждый элемент), максимум 16. Каждая пара
   проверяется:
   - `child_fd < TASK_MAX_FDS` (64);
   - `global_fd == AOS_INHERIT_FD` (0xFFFFFFFE) → ребёнок наследует
     родительский fd: `c->fds[child_fd] = current_task->fds[child_fd]`
     (должен быть открыт, иначе `-5`);
   - иначе `global_fd >= 3 && < VFS_OFILES` (64) и
     `vfs_ofile_ptr(global_fd) != 0`.
   Ошибка → `-5`.
3. `task_spawn(path, args, sink, &pid)`; провал → вернуть код, ничего не менять.
4. Для каждой валидной пары: `struct task *c = task_slot(pid)`;
   `c->fds[child_fd] = vfs_ofile_ptr(global_fd)` (для INHERIT —
   `current_task->fds[child_fd]`); если `child_fd == 0` → `c->stdin_fd = 0`;
   если `child_fd == 1` → `c->stdout_fd = 1`. Аналогично `exec_pipe`.

Почему безопасно: ребёнок ещё не планировался (state TASK_SPAWNING при
возврате из `task_spawn`), состояние `fds[]` подменяется до первого
переключения — тот же аргумент, что в commands.c:434.

**Почему нужен `AOS_INHERIT_FD`:** `task_spawn` даёт ребёнку fds 0–2 = console
(task.c:331-333), и `vfs_ofile_ptr(1)` в ядре — всегда консольный ofile, а не
pipe. Без наследования вывод детей `sh` (например, `ls` в term-окне) ушёл бы
на невидимую VGA-консоль, а не в pipe → в term. Поэтому `bin/sh` по умолчанию
спавнит каждую команду с `{0: AOS_INHERIT_FD, 1: AOS_INHERIT_FD}`: ребёнок
получает те же stdin/stdout, что у sh (pipe от term или console). Redirects
`> >> <` и pipe-концы (`{1: wr}`, `{0: rd}`) перекрывают этот дефолт — их
глобальные fd известны sh напрямую.

### B. O_NONBLOCK для pipe

`pipe_read_at` блокирует через `sti;hlt;cli` (pipe.c:34), `pipe_write_at` —
при полном буфере (pipe.c:54). `term` не может блокироваться в read — его цикл
обязан крутить `AOS_RECV` (мышь, MSG_KEY). Добавляем флаг на уровне `ofile`.

1. `kernel/vfs.h`: `#define VFS_O_NONBLOCK 0x400000` (свободный бит в
   `open_file.flags`).
2. `kernel/linux_syscall.c` `case 54` (сейчас `-ENOTTY`, стр. 449-451):
   `ioctl(fd, FIONBIO, &val)` — запрос `r->edx == 0x5421`; `fd = r->ebx` →
   `vfs_ofile_ptr(fd)` (иначе `-9`, EBADF); `val` из `r->ecx` через `in_luser`:
   ненулевой → `of->flags |= VFS_O_NONBLOCK`, нулевой → сброс. Остальные
   запросы → `-25` (ENOTTY). `FIONBIO` вынести как константу в aosabi.h.
3. `kernel/pipe.c`/`pipe.h`:
   - `pipe_read_nonblock(fs, ino, buf, len, off)`: `count == 0 && nwriters > 0`
     → `-11` (EAGAIN); `count == 0 && nwriters == 0` → `0` (EOF); иначе читает.
   - `pipe_write_nonblock(...)`: `nreaders == 0` → `-32` (EPIPE);
     `count == PIPE_BUF_SIZE` → `-11`; иначе пишет сколько влезает.
4. `kernel/vfs.c`: в `vfs_read_fd`/`vfs_write_fd` перед вызовом `fs->read_at`:
   если `of->inode->fs == &pipefs_fs && (of->flags & VFS_O_NONBLOCK)` →
   вызвать неблокирующую версию. (Единственная точечная зависимость
   `vfs.c → pipe.h`; обычные pipe-чтения и ядреные пайпы не затронуты.)

Обычные (блокирующие) пайпы и ядреный `exec_pipe` работают как раньше — флаг
по умолчанию не выставлен.

### C. `bin/sh` (programs/musl/sh.c, static musl, ABI_LINUX)

Портируем логику ядреного шелла (`terminal.c` + `commands.c`) в userland.
Шелл интерактивен: читает fd0 (пайп от term или console), пишет fd1
(пайп в term или console). Внешние команды — через `AOS_SPAWN_FDS`; вывод
детей уходит в их fds (наследуется по redir), а не через sink.

**Line editor** (по образцу terminal.c:589-723):
- вход — байты из fd0: печатаемые ASCII, UTF-8 (кириллица), `\r`, `\b`/`\x7f`,
  `\t`; ANSI-последовательности от term: `ESC[A` Up, `ESC[B` Down, `ESC[D` Left,
  `ESC[C` Right, `ESC[H` Home, `ESC[F` End, `ESC[3~` Del.
- курсор по байтовому индексу; backspace удаляет целый UTF-8 символ
  (скан start-байта, как `utf8_char_len_rev`).
- история: 16 записей, Up/Down.
- Tab: собрать имя текущего слова, перечислить каталоги PATH через
  `opendir`/`readdir` (VFS), совпадения по префиксу; один → дописать, несколько
  → вывести список; повторный Tab — цикл по совпадениям.
- `vga_reset_scroll`-аналог не нужен (это не скроллбэк VGA).

**Перерисовка строки**: после каждого изменения печатается
`"\r" + "\x1b[K" + prompt + line`, затем курсор возвращается на позицию
относительными движениями `\x1b[{n}D` (CUB). term понимает эти коды (секция D).
`cmd_col` (старт командной строки) хранится в sh — это `5` (длина prompt
`AOS> `).

**Встроенные**: `cd <path>` (chdir; ошибка → `cd: no such directory`),
`pwd` (getcwd), `export VAR=value` (внутренний env, до 16 переменных),
`setpath <dirs>` (внутренний PATH), `exit` (выход из sh), `$?`/`$NAME`
(подстановка при разборе строки, как commands.c:87-93: неизвестная → пусто).
`echo` — внешняя команда `bin/echo` (уже есть в PROGRAMS), builtin не нужен:
`echo $?` раскрывается в `echo 0` и исполняется bin/echo. Встроенные **не
поддерживаются внутри пайпа** (как ядро): стадия-builtin → `sh: builtin not
supported in pipeline`, `$?=1`, ничего не спавнить.

**Разбор строки** (по commands.c `find_operator`/`exec_stage`): токены через
пробелы; операторы окружены пробелами: `|`, `>`, `>>`, `<`, `&`.
- `cmd > file` → `open(file, O_WRONLY|O_CREAT|O_TRUNC)` → `spawn_fds {1: fd}`
  → `close(fd)`. `>>` → `O_APPEND`. `cmd < file` → `open(file, O_RDONLY)` →
  `spawn_fds {0: fd}` → `close(fd)`.
- `a | b | c` (до 8 стадий): `pipe()` (linux 42) ×(N-1); каждая стадия —
  `spawn_fds(path, args, 0, {stdin: prev_rd, stdout: next_wr})`; sh закрывает
  свои концы (`close`), затем `waitpid` всех по порядку; `$?` = код последней.
- `cmd &` → `spawn_fds` без ожидания, печать `bg: pid N`.
- foreground: `spawn_fds` + `waitpid` → `$?`.
- `cmd` без PATH-префикса → поиск в PATH-каталогах (`bin/`, `lin/` и т.д.),
  как `path_resolve` (commands.c:239). Builtin-префикс `bin/`/`lin/` тоже
  работает напрямую.
- Неизвестная команда → `Unknown command: <cmd>` (как ядро), `$?=127`.
- `echo $?` → печатает последний код.

**Ввод-вывод sh**: fd0/fd1 — пайпы от term. Блокирующий `read(0)` ждёт клавиши
(термин пишет в pipe), это ок для интерактивного шелла. EOF (`read==0`, term
закрылся) → `exit`. При запуске из serial-консоли (ядреный `bin/sh`): fd0/1 —
console (`terminal_read_key` возвращает `-EAGAIN` при пустой очереди) — sh
крутит `yield()` при `-EAGAIN`/`-1`. Печать — `write(1)`.

### D. `term.c` → VT-эмулятор

**Удаляется**: `cmd_col`, `read_cmd`, `do_enter`, `prompt()` (свой prompt),
spawn с sink=term, `child_active`-логика (заменяется флагом «sh жив»).

**Остаётся**: `screen[TH][TW]`, `crow`/`ccol`, `render()`, `utf8_encode`,
жизненный цикл окна (MSG_CREATE/MSG_WININFO/MSG_CLOSE), theme.

**Запуск sh** (в main, после получения win):

```c
int in[2], out[2];
pipe(in); pipe(out);                        // [rd, wr]
struct aos_redir redirs[] = {{0, in[0]}, {1, out[1]}, {0xFFFFFFFF, 0}};
aos_spawn_fds("bin/sh", "", 0, redirs);
close(in[0]); close(out[1]);                // копии уходят ребёнку
int fd_in_write = in[1];                    // клавиши → sh
int fd_out_read = out[0];                   // вывод sh → term (FIONBIO)
```

**Вывод** (fd_out_read, неблокирующий через `ioctl(FD, FIONBIO)`):
`read(fd, buf, 256)` в главном цикле; `-11`/`-EAGAIN` → пропустить; `0` → EOF,
sh умер (см. ниже); иначе — мини-ANSI-парсер над байтами:

- обычный байт: декодировать UTF-8 (1–3 байта, state machine как в vga.c),
  codepoint → `screen[crow][ccol]`, `ccol++`; на `ccol == TW` → newline;
- `\r` → `ccol = 0` (без перевода строки);
- `\n` → newline (scroll up при `crow == TH-1`, как сейчас);
- `\b` → `ccol--`, `screen[crow][ccol] = 0`;
- `\x7f` → как `\b`;
- `ESC` → state machine:
  - `ESC[ K` → очистить `screen[crow][ccol..TW-1]` (EL);
  - `ESC[ H` (без параметров) → `crow=ccol=0`; `ESC[ {r};{c} H` → CUP;
  - `ESC[ {n} D` → `ccol -= n` (CUB, clamp 0); `ESC[ {n} C` → `ccol += n`;
  - `ESC[ ? 25 h` / `l` → флаг курсора вкл/выкл;
  - `ESC[ 2 J` → очистить весь экран, `crow=ccol=0`;
  - прочее → игнорировать.
- после каждой пачки байт → `render()` (курсор рисуется по `crow`/`ccol`
  если флаг видимости установлен).

**Ввод** (MSG_KEY → fd_in_write):

```c
if (key < 0x100)  // печатаемый/пробел: utf8_encode(key) → write(fd, ..., n)
else switch (key) {
  case 0x0101: write("\x1b[A");   // GUI_KEY_UP
  case 0x0102: write("\x1b[B");   // DOWN
  case 0x0103: write("\x1b[D");   // LEFT
  case 0x0104: write("\x1b[C");   // RIGHT
  case 0x0105: write("\x1b[H");   // HOME
  case 0x0106: write("\x1b[F");   // END
  case 0x0107: write("\x1b[3~");  // DEL
  case '\r':   write("\r");
  case '\t':   write("\t");
  case '\b':   write("\x7f");
}
```

Печатаемые codepoint'ы > 0xFF (кириллица от MSG_KEY) → utf8_encode в 2 байта.

**Смерть sh**: `read == 0` (EOF: `nwriters` обоих концов обнулились) →
вывести `\r\n[sh exited]\r\n`, флаг `sh_alive = 0`; следующий `MSG_KEY` == `\r`
→ перезапуск sh (новые пайпы + spawn, секция выше). `MSG_CLOSE` → выход из term.

### E. Makefile / интеграция

- `Makefile`: правило `build/prog/sh.elf: programs/musl/sh.c programs/aosabi.h`
  (как term.elf без theme.c); `sh` добавить в `PROGRAMS` (стр. 24).
  `gen_progs.py` подхватит ELF автоматически; ramdisk получит `bin/sh`.
- Dock/desktop не трогаем: `term` и так спавнится WM по `bin/term`; `bin/sh`
  остаётся доступным и из serial-консоли (`bin/sh` как обычная команда).
- `format` остаётся ядреным builtin (serial-консоль); из bin/sh недоступен
  (см. Out of scope).

## Data flow

```
WM: PS/2 key ─► MSG_KEY ─► term (сфокусирован)
term: MSG_KEY ─► UTF-8/ANSI ─► write(in[1]) ─► [pipe A] ─► read(0) ─► sh
sh: write(1) ─► [pipe B] ─► read(out[0]) ─► ANSI-парсер term ─► screen ─► render
sh: spawn_fds(cmd, redirs) ─► ребёнок пишет в redir-fds / pipe-концы
```

```
term: spawn_fds("bin/sh", "", 0, {{0, in[0]}, {1, out[1]}, {0xFFFFFFFF,0}})
       └► task_spawn ─► t->fds[0]=ofile(in[0]); t->fds[1]=ofile(out[1])
```

```
serial: "bin/sh" ─► ядреный шелл ─► spawn bin/sh (fds 0/1 = console)
       ─► sh интерактивно (key_queue + terminal_write)
```

## Error handling

- `spawn_fds`: плохой `path` → как AOS_SPAWN (код spawn'а); невалидный
  `redirs_ptr`/fd/`AOS_INHERIT_FD` на закрытом родительском fd → `-5`; задача
  не найдена в `task_slot` → вернуть код spawn'а.
- `ioctl(FIONBIO)` на закрытом fd → `-9` (EBADF); чужой запрос → `-25`.
- pipe nonblock: пусто → `-11`; EOF → `0`; полный → `-11`; нет читателя → `-32`.
- sh: неизвестная команда → `Unknown command: <cmd>`, `$?=127`; spawn fail →
  `cannot run command`, `$?=1`; `cd` в несуществующий каталог → ошибка, `$?≠0`.
- sh: `pipe()`/`open()` fail (нет слотов) → сообщение, `$?=1`, ничего не спавнить.
- term: read EOF → «[sh exited]», перезапуск по Enter; MSG_CLOSE → выход.
- Эпилог фоновых задач: нет сигналов/jobs (вне скоупа); фоновые живут до exit.

## Testing

Скрипты пишет ассистент; QEMU-прогоны — через существующие каркасы
(AGENTS.md). Два уровня:

**1. `scripts/shtest.py` — serial-тест bin/sh** (по паттерну pipetest.py):
- boot ISO headless, при `AOS>` отправить burst:
  - `bin/sh` → появляется prompt sh (`AOS> `);
  - `pwd` → вывод cwd;
  - `ls /bin` → список `bin/` (проверка PATH-поиска);
  - `echo $?` после успешной/провальной команды → `0`/`127`;
  - `echo hi > f` + `cat f` → `hi` (redirect);
  - `ls /bin | bin/cat` → пайп в userland (AOS→AOS);
  - `bin/ls / | bin/cat | lin/cat` → 3 стадии (AOS→AOS→Linux);
  - `bin/ls | lin/cat` → AOS→Linux пайп через userland-шелл;
  - `exit` → возврат в ядреный шелл;
  - No `KERNEL PANIC` в логе.

**2. GUI smoke (QTest-каркас, как notepadtest.py)**:
- boot, дождаться WM, клик по dock-иконке term (или спавн через shell),
- в окне term набрать `pwd`, Enter, затем `echo hi`, Enter;
- PPM-скриншот: assert появления текста в области окна (не-фоновые пиксели в
  строках вывода, как делает notepadtest) — подтверждает, что вывод sh
  реально рисуется в окне term;
- закрыть term, убедиться что WM жив.

**Регрессия**: полный `make test` — ipctest, manytest, notepadtest, sleeptest,
shelltest, cwdtest, linhello, lincat, lindirtest, pipetest, configtest и т.д.
остаются зелёными (ядреный шелл, WM, остальные приложения не менялись).
Build: `make` без новых `-Wall -Wextra` предупреждений.

## Out of scope

- `format` из bin/sh (остаётся ядреным builtin для serial-консоли).
- Job control, `wait`, сигналы, SIGPIPE, status уведомления о фоновых задачах.
- `pipe2` (331), `O_CLOEXEC`, `dup`/`dup2` syscall (не нужны: всё через redir).
- Несколько pipe-концов term: один term = один sh.
- Resize окна term (пока 80×26 фикс), выбор шрифта/цвета, скроллбэк в term.
- `$()` command substitution, `;`, `&&`/`||`, globbing (`*`), quoting.
- Перенос остальных builtin'ов ядра (strace, format) в userland.
