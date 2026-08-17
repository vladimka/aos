# Дизайн: рефакторинг утилит и поддержка цвета в терминале

Дата: 2026-08-17. Статус: согласовано с пользователем (brainstorming).

## Контекст и проблема

Утилиты AOS (`programs/musl/*.c`) — минимальные реализации без стандартных
флагов и единого стиля:

- `ls` всегда показывает корень `/` (игнорирует cwd), без флагов, размеры в hex.
- `head` принимает `head <file> [lines]` вместо стандартного `-n`.
- `wc`, `cat`, `cp`, `mv`, `mkdir`, `rmdir`, `rm` — однострочники без флагов,
  сообщения об ошибках нестандартные (`File not found:`, `Copied:`,
  `Moved:`, `Deleted:`), выводятся в stdout.
- `cd`/`pwd` — builtin-и в kernel shell (`kernel/commands.c`) и в GUI-оболочке
  `bin/sh` (`programs/musl/sh.c`); `cd` без аргумента и `cd -` не поддерживаются.
- Терминал (`drivers/vga.c`, `programs/musl/term.c`) не понимает ANSI SGR-цвета —
  только позиционирование курсора, `K`/`J`.
- Программы не получают окружение: `stack_build()` (`kernel/elf.c`) строит
  `envp = {NULL}`.

Цель — стандартизировать утилиты (флаги, stderr, exit-коды), добавить цветной
`ls`, и сделать это консистентно через общий слой `uutils`. Тесты, завязанные на
точные строки вывода, обновляются.

## Не-цели

- Не переносим утилиты в ядро (остаются Linux-ABI musl-программами).
- Не трогаем `wm`, `term`, `clock`, `notepad`, `sh` как GUI-приложения (кроме
  `sh.c`/`term.c` в части env `TERM` и SGR-рендера).
- Не добавляем владельцев/даты в `ls -l` (SFS их не хранит).
- Не добавляем права доступа в SFS (всегда `rwxrwxrwx`).

## Архитектура

### 1. Общий слой `programs/musl/uutils.{c,h}`

Подключается к musl-программам через Makefile (правило `build/prog/%.elf`
получает зависимость и компиляцию `programs/musl/uutils.c`; как это сделано для
`theme.c` в правилах `wm/term/clock/notepad.elf`). Содержит:

- `u_optarg`/`u_opterr`: единый парсер опций поверх musl `getopt` с общим
  сообщением об ошибке и флагами `-h`/`--help` (печать `usage` в stdout, exit 0).
- `u_color(fd, idx)`, `u_color_reset()`: эмиттят `\x1b[38;5;Nm`,
  `\x1b[48;5;Nm`, `\x1b[0m`.
- `u_have_color(int fd)`: возвращает 1, если `getenv("TERM")` непуст. НЕ проверяет
  fd — признак терминальности передаётся шеллом через env и снимается при
  редиректе (см. §3).
- `u_hsize(unsigned int n, char *buf, unsigned int bufsz)`: `1234` → `1.2K`,
  `1048576` → `1.0M`.
- `u_put_err(const char *prog, const char *fmt, ...)`: вывод в stderr в виде
  `prog: message`.
- `u_list_dir(dir, flags)` → массив `struct u_entry {char name[VFS_NAME_MAX+1];
  unsigned int type; unsigned int size;}`, отсортированный по имени. Возвращает
  количество, -1 при ошибке. Использует `opendir`/`readdir`/`stat`.
- `u_print_columns(entries, n, flags)`: раскладка имён в колонки по ширине
  терминала (80 колонок), маркеры `/` для директорий и `*` для исполняемых
  (в не-`-l` режиме).
- Таблица цветов xterm (перенесена в userspace; ядро имеет свою, см. §2):
  `U_C_DIR` (33 синий), `U_C_EXEC` (70 зелёный).

### 2. SGR в терминале (ядро и GUI-терминал)

**`drivers/vga.c`** — парсер SGR в существующей `ansi_state`-машине
(`vga_putchar`, строка ~224 и `fb_putchar`, строка ~285):

- Обрабатывается `ESC[...m` (final `m`): разбор параметров через `;`,
  поддерживаются `0` (reset), `1` (bold — толще цвет), `38;5;N` (fg index),
  `48;5;N` (bg index).
- Таблица `static const unsigned int xterm_rgb[256]` — стандартная xterm-палитра
  (16 базовых, 216 cube, 24 grayscale). Используется в fb-режиме для
  непосредственной отрисовки `color_rgb`.
- Хранение текущих fg/bg расширяется до 8-битного индекса палитры
  (`fg_index`/`bg_index`); `color_rgb[16]` остаётся для text-режима, а для
  fb-пути используется `xterm_rgb[fg_index]`.
- Text-режим (VGA-атрибуты): 256-цветный индекс аппроксимируется ближайшим из
  16 базовых (простая таблица маппинга 256→16).
- Сброс цвета происходит на `ESC[0m`; состояние не наследуется между строками
  (после `\n` сохраняется — как в реальном терминале).

**`programs/musl/term.c`** — GUI-терминал:

- `screen[TH][TW]` расширяется: `struct tcell {unsigned int cp; unsigned char fg;
  unsigned char bg;} screen[TH][TW]`.
- В `term_out_byte` добавляется парсер SGR (final `m`), цвета как индексы
  0..255 xterm-палитры (`xterm_rgb` таблица в term.c).
- `put_cp` рисует глиф цветом `xterm_rgb[fg]` на `xterm_rgb[bg]` (через
  существующие примитивы `fb_*`/`draw_*` term.c).
- `clear`/`EL`/`ED` обнуляют не только codepoint, но и fg/bg.

**`kernel/serial.c` / `terminal.c`** — фильтр SGR из serial-вывода:

- `serial_putchar`/`serial_write` пропускают ESC-последовательности SGR
  (состояние «внутри ESC/CSI») так, что в serial-лог не попадают `\x1b[`-байты.
  Реализуется маленьким парсером (reset на не-цифровом/не-`;`/не-`[` символе).
- Это гарантирует чистоту serial-логов для тестов.

### 3. env `TERM` и терминальность

Программы красят тогда и только тогда, когда `getenv("TERM")` непуст. Признак
выставляется шеллом в окружение дочерней программы и **снимается** при
перенаправлении stdout:

**Kernel shell (`kernel/commands.c`):**
- `shell_env` уже есть (таблица `name`/`val`, `export` builtin). `TERM` задаётся
  по умолчанию при старте (после `terminal_init`) в `shell_env`.
- При запуске программы (`task_spawn`) передать env-строки задачи-ребёнку.
- В `exec_stage` (OP_GT/OP_GTG) и в `exec_pipe` (stages, кроме последнего)
  `TERM` не передаётся — ребёнок получит env без `TERM`.

**GUI-оболочка `programs/musl/sh.c`:**
- В `main` (или при старте) `env_set("TERM", "aos")` в локальную таблицу vars.
- `aos_spawn_fds` должен передавать env. Новый syscall `SYS_SPAWN_ENV`
  (или расширение существующего) получает путь, args, sink, env-строки
  (NUL-terminated, `NAME=VAL`, последняя — пустая) и redirs.
- В `run_stage`, когда есть `out_f` (редирект stdout) или `i+1 < nstages`
  (в pipeline), env без `TERM`.

**Kernel: передача env (`kernel/elf.c`, `kernel/task.c`, `kernel/syscall.c`):**
- `stack_build()` принимает список env-строк вместо `envp = {NULL}`:
  массив указателей на строки `NAME=VAL`, завершающийся NULL; в стек кладутся
  сами строки и массив указателей.
- `struct task` получает поле env (буфер `char env[ENV_BUF][ENV_MAX]` или
  массив строк); `task_spawn` копирует env родителя в ребёнка (или env из
  syscall-аргумента).
- Новый syscall `SYS_SPAWN_ENV` (номер рядом с `SYS_SPAWN_FDS`): аргументы
  `path, args, sink, env_ptr, redirs_ptr` (или два отдельных — для `sh.c`
  нужны и env, и redirs). Реализация в `kernel/syscall.c` копирует env через
  `copy_user_str`-подобный разбор NUL-разделённого блока.
- `stack_build` не меняет существующий `AOS_*`/Linux ABI: Linux-задачи получают
  env от родителя, AOS-задачи — как раньше (env пока не критичен).

### 4. Утилиты

**`ls`** (переработка `programs/musl/ls.c`):
- Аргументы: `[options] [file...]`; по умолчанию — cwd (`.`).
- Флаги: `-a` (включая `.`-файлы), `-l`, `-h`, `-R` (рекурсивно), `-r`
  (обратная сортировка), `-1` (одна колонка), `-h`/`--help`.
- `-l`: `d rwxrwxrwx 1234 name` (тип `d`/`-`, права фиксированные, размер,
  имя). Внутри `-l` маркеры `/`/`*` опускаются.
- Цвет: если `u_have_color(1)` — директории синим (`38;5;33`), исполняемые
  зелёным (`38;5;70`). SGR только вокруг имени, reset после.
- `-R`: рекурсивный обход; перед каждой поддиректорией заголовок `dir:`.
- Несколько аргументов: перед списком каждой директории заголовок `dir:`.
- Ошибка: `ls: /x: No such file or directory` в stderr, exit 1.
- Сортировка: по имени (strcmp). `-r` — обратная.

**`head`**: `-n N` (по умолчанию 10), `head file`; старый `head file N`
удаляется. `head -n 0` → пусто. Ошибка отсутствующего файла в stderr, exit 1.

**`wc`**: `-l`, `-w`, `-c` (без флагов — все три, как раньше), несколько файлов
с итогом `total`. Формат строки: `6 3 17 /t.txt` (количество строк, слов,
байт) — сохраняется, чтобы тесты минимально менялись. Ошибка в stderr, exit 1.

**`cat`**: несколько файлов последовательно, `-n` (нумерация строк, GNU-стиль),
`-u` (без буферизации — игнорируется, musl и так unbuffered). Отсутствующий
файл — `cat: /x: No such file or directory` в stderr, не прерывает остальные.

**`cp`**: `cp src... dst` (несколько src в директорию dst), `-r` (директории),
`-v` (verbose: `'a' -> 'b'`), `-f` (перезапись — и так делается). Ошибка в
stderr, exit 1.

**`mv`**: `mv src... dst` (перемещение директории целиком — рекурсивное
копирование + unlink), `-v`, `-f`. Ошибка в stderr, exit 1.

**`mkdir`**: `mkdir [-p] dir...`, `-p` создаёт промежуточные директории
(реализуется разбиением пути и пошаговым `mkdir` на префиксы). Ошибка:
`mkdir: cannot create directory 'x': No such file or directory` в stderr.

**`rmdir`**: `rmdir dir...`, только пустые директории, ошибка в stderr.
**`rm`**: `rm [-r] [-f] file...`, `-r` рекурсивно (обход директории, unlink
файлов, rmdir директорий), `-f` подавляет ошибки «не найдено».

**`date`**: аргумент `+%Y-%m-%d %H:%M:%S` (поддерживаются `%Y %m %d %H %M %S`),
по умолчанию тот же формат. Без argv — как сейчас.

**`uptime`**: печатает время работы в секундах (syscall `AOS_UPTIME`) —
семантика сохраняется.

**`procinfo`**: `-a` (все разделы), по умолчанию краткая сводка (`[uptime]`,
`[version]`, `[mounts]`).

**`sync`**: `sync` — `vfs_sync()` (как сейчас); `-f` — без изменений (fsync не
применяется к «всем файлам» без fd; флаг добавляется для совместимости вызова,
действие то же).

**`echo`**: добавить `-n` (без перевода строки), `-e` (escape) — минимально.
**`help`**: обновить список под новые флаги.
**`clear`**: без изменений (уже работает).

### 5. cd/pwd (обе оболочки: `kernel/commands.c` и `programs/musl/sh.c`)

- `cd` без аргумента → `/` (HOME=/).
- `cd -` → предыдущая директория (поле `last_cwd` в task / локальная переменная
  в `sh.c`); печатает имя новой директории (как в bash).
- `~` и `~/x` → раскрытие в `/` (`HOME=/`).
- `pwd -P` — то же, что `pwd` (флаг принимается, физический путь = cwd).

### 6. Тесты

Обновляются существующие (`scripts/`):
- `fstoolstest.py`: новые строки (`head -n 2`, stderr-сообщения, `wc` формат
  сохраняется). Проверяемые подстроки: `6 3 17 /t.txt`, `3 1 9 /t.txt` и т.п.
  вместо `Copied:`/`Moved:`/`File not found:`/`Created:`/`Removed:`.
- `atatest.py`: `bytes_of_wc` (формат `wc` сохраняется), `ls` — список содержит
  имена (формат меняется, но имена остаются).
- `cwdtest.py`: `pwd` выводит `\n/proc\n`, `\n/\n`, `\n/bin\n` — без изменений.
  Добавляются проверки `cd` (без аргумента → `/`), `cd -`.
- `lindirtest.py`: `ls` выводит cwd по умолчанию; root listing теперь без
  префикса `Files in /:` — поправить ожидания.

Новые тесты:
- `scripts/sgrcolor.py` (или расширение `fstoolstest`): `ls` в консоли (fb) с
  TERM даёт `\x1b[38;5;33m`-байты; `ls > f` в файле их не содержит; serial-лог
  чист (нет `\x1b`).
- `scripts/lsflagstest.py`: `ls -a -l -h -R -r` комбинации на подготовленном
  дереве (`/d`, `/d/sub`, `.hidden`), проверка маркеров `/`, колонок, размеров
  с `-h`, рекурсии.
- `scripts/toolflags.py`: `head -n 2`, `wc -l/-w/-c`, `cat` нескольких файлов,
  `rm -r`, `cp -r`, `mkdir -p`, `cd -`.

Все новые тесты — в `Makefile` `TESTS` (или `LINUX_TESTS`, т.к. требуют
musl-программ).

## Файлы

- `programs/musl/uutils.c`, `programs/musl/uutils.h` — новый общий слой.
- `programs/musl/{ls,head,wc,cat,cp,mv,mkdir,rmdir,rm,date,uptime,procinfo,sync,echo,help}.c` — переработка.
- `programs/musl/sh.c` — env `TERM`, `cd -`/`cd`/`~`, передача env в spawn.
- `programs/musl/term.c` — SGR-рендер, `tcell`.
- `drivers/vga.c` — SGR-парсер, `xterm_rgb[256]`, маппинг в text-режим.
- `kernel/serial.c` — фильтр SGR.
- `kernel/elf.c` (`stack_build`), `kernel/task.c`/`task.h` (env в task),
  `kernel/syscall.c` (SYS_SPAWN_ENV), `kernel/commands.c` (env TERM, cd -/~/`~`),
  `kernel/terminal.c` (TERM по умолчанию).
- `programs/aosabi.h` — `aos_spawn_env()` wrapper.
- `Makefile` — правило `uutils` в компиляцию musl-программ; новые тесты.
- `scripts/*.py` — обновление и новые тесты.
- `AGENTS.md` — документация по SGR, `uutils`, env `TERM`.

## Порядок работ (эскиз для плана)

1. `uutils.{c,h}` + Makefile-интеграция.
2. SGR: `vga.c` → `term.c` → `serial.c`.
3. env-механизм: `task`/`stack_build`/`syscall.c`/`commands.c`/`sh.c`/`aosabi.h`.
4. Утилиты по одной (ls последним — на нём цвет и `uutils`-функции).
5. `cd/pwd` в обеих оболочках.
6. Тесты: обновление существующих, новые.
7. AGENTS.md.

## Риски

- **env в `stack_build`**: меняет ABI-стек (envp), нужно аккуратно выровнять
  (текущий расчёт `total`/`pad` в `stack_build`). Проверить, что musl-`_start`
  корректно читает envp после env-массива.
- **SGR в `vga.c`**: парсер `ansi_state` уже обрабатывает `K`/`D`/`J`;
  `m`-параметры вписываются без регрессий. Text-режим — аппроксимация 256→16.
- **serial-фильтр**: должен не трогать нормальный вывод (только в состоянии
  «после ESC»), сброс на любой неожиданный байт.
- **Тесты на цвета**: GUI-терминал (term.c) и fb-консоль красят по-разному;
  автотесты проверяют fb-консоль и serial-чистоту, GUI-цвет — вручную/скриншотом.
- **Обратная совместимость**: `wc` формат и `pwd`-строки сохранены; остальные
  сообщения меняются вместе с тестами.