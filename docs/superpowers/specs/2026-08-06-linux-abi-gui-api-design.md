# Linux ABI как нативный + GUI API (удаление libaos)

Дата: 2026-08-06
Статус: approved (дизайн)

## Цель

Сделать **Linux ABI (musl) единственной нативной ABI** ОС:

- все программы компилируются и линкуются на static musl (настоящий libc),
  а не на самодельном `programs/libaos.o`;
- пользовательские и AOS-специфические возможности становятся **чистым
  расширением ABI**: зарезервированный диапазон номеров `int 0x80`
  (500–599), оборачиваемый для musl заголовком `programs/aosabi.h`;
- `programs/libaos.c`/`libaos.h`, флаг `ABI_AOS`, user-область `0x01000000`,
  freestanding-правила сборки и `programs/programs.ld` **полностью удаляются**.

## Итоговая модель

```text
int 0x80  (единая точка входа, ring 3)
  номер >= AOS_EXT (500)  --> aos_gui_handler(r)   (GUI + AOS-специфика)
  иначе                   --> linux_syscall_handler(r) (musl: write/open/...)
```

Все задачи получают `ABI_LINUX`. Программы вызывают расширения через
`syscall(N, ...)` (вариадический `syscall()` из musl); обёртки даёт `aosabi.h`.
Рендер текста/прямоугольников остаётся в ядре (встроенный шрифт).

## Перенос на стандартный Linux ABI (свои номера исчезают)

| libaos-функция | Linux-эквивалент |
|---|---|
| `print`/`print_hex`/`print_dec`/`putchar` | `write(1/2, ...)`, musl `printf` |
| `sd_open/read/write/close/lseek/mkdir/rmdir/readdir/chdir/stat/fstat/unlink` | `open/read/write/close/lseek/mkdir/rmdir/getdents/chdir/fstat/stat64/unlink` (уже в `linux_syscall.c`/`vfs.c`) |
| `malloc`/`free` | musl heap через `brk`/`mmap2` (уже) |
| `get_tick` | `clock_gettime`/`gettimeofday`, `/proc/uptime` |
| `yield` | `sched_yield` (158) |
| `getpid` | `getpid` (20) |
| `sleep_ms` | `nanosleep` (162) |
| `reboot`/`shutdown` | `reboot` (88) + `LINUX_REBOOT_CMD_RESTART/POWER_OFF` |
| `get_random` | `getrandom` (355) |
| `exit`/`exit_with_code` | `exit`/`exit_group` (1/252) |

`get_uptime` оставляем расширением (простое зеркало `/proc/uptime`, дешевле).

## Таблица расширений (база `AOS_EXT = 500`)

| № | имя | аргументы / описание |
|---|---|---|
| 500 | `aos_fb_info` | addr, w, h, pitch, bpp |
| 501 | `aos_render_text` | `aos_render_req*` (buf,pitch,x,y,str,fg,bg) |
| 502 | `aos_fill` | `aos_fill_req*` (buf,pitch,x,y,w,h,rgb) |
| 503 | `aos_clear` | fullscreen clear |
| 504 | `aos_mouse` | x, y, buttons, wheel |
| 505 | `aos_read_key` | блокирующий (ждёт в ядре) |
| 506 | `aos_key_poll` | неблокирующий: -1, если пусто |
| 507 | `aos_register_events` | подписка на события ввода |
| 508 | `aos_get_event_pid` | pid получателя событий |
| 509 | `aos_send` | pid, `aos_msg` |
| 510 | `aos_recv` | `aos_msg*` |
| 511 | `aos_setout` | pid для stdout |
| 512 | `aos_spawn` | path, args, sink -> pid |
| 513 | `aos_waitpid` | pid -> код выхода |
| 514 | `aos_get_children` | массив pid, max |
| 515 | `aos_get_args` | buf, max (аргументы spawn) |
| 516 | `aos_get_rtc` | `aos_time*` |
| 517 | `aos_uptime` | секунды с загрузки |
| 518 | `aos_get_tick` | тик PIT (~1000 Гц) |
| 519 | `aos_panic` | триггер kernel panic |

Диапазон выше 500 свободен (i386 Linux использует максимум ~358) — коллизий нет.

## Общий ABI-заголовок `programs/aosabi.h`

Самодостаточный (без kernel-инклудов; включаемый и musl-программами, и ядром):

- структуры: `aos_msg`, `aos_stat`, `aos_time`, `aos_render_req`, `aos_fill_req`;
- константы: `MSG_KEY/DATA/UPDATE/CREATE/WININFO/EXIT/CLOSE`,
  `AOS_SLAB_BASE/SIZE/SLABS`, номера расширений (500–519);
- обёртка `aos_syscall(n, ...)` над musl `syscall()`.

Ядро включает этот заголовок вместо дубликатов в `kernel/vfs.h`
(`struct aos_stat`) и в старом `libaos.h` — единый источник правды. Layout
структур не меняется относительно текущего ABI.

## Изменения в ядре

- `kernel/syscall.c`: удалить ветку AOS и весь `switch(0–51)`; оставить только
  `linux_syscall_handler` с предиспатчем `500..599 -> aos_gui_handler`.
- `kernel/aos_gui.c` (новый): реализация 500–519 — перенос логики из старого
  ABI (mailbox send/recv, setout, mouse, read_key, fb/text/fill/clear, spawn,
  waitpid/get_children, args, rtc, uptime/tick) с проверками `in_user`/
  `copy_user_str` по Linux-окну.
- `kernel/linux_syscall.c`: добавить `reboot(88)`, `getrandom(355)`; `read(0)`
  неблокирующий остаётся как есть.
- `kernel/elf.c`: удалить ABI_AOS-загрузку (user-область `0x01000000`);
  все ELF грузятся linux-загрузчиком в окно `0x08000000..0x10000000`.
- `kernel/task.c`, `kernel/user_tramp.S`, `kernel/user.c`: убрать AOS entry/
  структуру `ABI_AOS`; запуск только через musl-startup (`_start` musl).
- `kernel/paging.c`, `kernel/pmm.c`: освободить user-область
  `0x01000000..0x01804000` и PDE 4–6 (возврат 12 МБ-бюджета).
- `kernel/commands.c`: shell-запуск через musl-ELF; argv через `aos_get_args`.

## Сборка (Makefile, gen_progs.py)

- Все программы: `tools/musl-i686/bin/i686-linux-musl-gcc -static -no-pie -Os`;
  entry = musl `_start`, `main(argc, argv)`; без freestanding-обёртки.
- Удалить `programs/%.o`/`%.elf`-правила, `programs/libaos.o`, `programs.ld`;
  `wm` включает `ico.c` как обычный `.c` (не отдельный `.o`).
- `gen_progs.py` эмбедентирует все `bin/*` в ramdisk тем же способом, что
  сейчас `lin/*`; `lin/` консолидируется в `bin/`.
- Стабильная сборка без musl-toolchain (как сейчас `lin/*`).

## Программы (29): переписание

- Общее: `print*` -> `write`/`printf`; файлы -> musl `open`/`read`/`write`;
  `get_args` -> argv.
- GUI (`wm`, `term`, `clock`, `notepad`): через `aosabi.h` (fb_info,
  `aos_render_text`, `aos_fill`, `aos_mouse`, `aos_recv/send`, `aos_spawn`,
  `aos_setout`).
- Новые утилиты: `mkdir`, `cp`, `mv`, `head`, `wc`.
- Shell-команды (`reboot`, `panic`, `shutdown`): через расширение/`reboot(88)`.
- `wm` — самый сложный, переписывается последним в том же заходе.

## Тесты

- `make test`: `linhello`, `lincat`, `ipctest`, `manytest`, `notepadtest`
  остаются зелёными.
- `guitester.py`, `notepadtest.py`, `mounttest.py`, `cwdtest.py` не регрессируют.
- Новый smoke `wm_hello`: минимальная musl-GUI-программа рисует текст через
  расширение 501 в окне (fb_info + render_text + recv + spawn), доказывая
  полный GUI-путь поверх Linux ABI.

## Критерии успеха

1. Все `scripts/*test*.py` зелёные.
2. В дереве/рантайме отсутствуют `libaos.o`, `ABI_AOS`, user-область
   `0x01000000`.
3. `AOS>`-shell и WM/term работают; текст через расширение отрисовывается.
4. musl-программа `wm_hello` рисует текст в окне через расширение ABI.