# Init-система `/bin/init` — дизайн

Дата: 2026-08-22

## Problem

Сейчас в AOS нет процесса-инициализатора:

- **WM спавнится захардкожено** из `kernel_main` (kernel/kernel.c:156-172) — состав
  загрузочных сервисов зашит в ядро.
- **Нет надзора**: краш `wm` оставляет систему без GUI до ребута; зомби WM висит
  слотом до давления на таблицу (`MAX_TASKS=24`).
- **Сироты не репарентятся**: если родитель умер (например, GUI `sh` закрыли
  вместе с терминалом), его живые дети сохраняют `parent = pid_умершего`; когда
  сирота выходит, его зомби никем не собирается — только аварийным реклаймом
  `task_spawn` при исчерпании слотов (task.c:297-305).
- **Нет способа убить процесс**: syscall'а kill нет; зависшее GUI-приложение
  нельзя завершить даже из шелла.
- **Выключение/рестарт** идут напрямую через syscall 88/14 без координации с
  процессами.

## Goal

1. `/bin/init` (static musl, ABI_LINUX) — первый заспавненный пользовательский
   процесс: читает `/etc/init.conf`, запускает сервисы (wm, clock), собирает
   зомби детей, **респавнит** упавшие сервисы с backoff и защитой от crash-loop.
2. Ядро: репарентинг сирот на init + уведомление init'а о смерти ребёнка
   (mailbox, аналог SIGCHLD); ядро перезапускает init, если он сам умер.
3. `AOS_KILL` (525) — кооперативное убийство процесса (целевой таск завершается
   на следующем syscall) + builtin `kill` в ядерном шелле.
4. Ядреный шелл (task 0, serial-консоль) остаётся как есть — line editing,
   история и Tab не переезжают в userland.

Вне скоупа: runlevels, getty, сигналы общего вида, shutdown-протокол через init
(wm по-прежнему зовёт syscall 88 напрямую), перенос builtins в userland.

## Scope

- `kernel/task.h`/`task.c`: глобальный `init_pid`, репарентинг сирот в
  `task_switch_kernel`, `MSG_EXIT` родителю-init'у, `task_kill()` +
  `kill_pending`, `task_reassign_children()`.
- `kernel/syscall.c`: проверка `kill_pending` в начале `syscall_handler`.
- `kernel/aos_gui.c`: `case AOS_KILL`.
- `kernel/commands.c`: builtin `kill`.
- `kernel/kernel.c`: spawn `bin/init` вместо spawn'а wm; `ensure_init()` в
  главном цикле.
- `programs/aosabi.h`: `AOS_KILL 525` + wrapper `aos_kill`.
- `programs/musl/init.c` (новый): сама init-система.
- `scripts/init.conf` (новый): `etc/init.conf`, embedded через
  `gen_progs.py --data`.
- `Makefile`: `init` в `PROGRAMS`, `--data etc/init.conf=...`, `inittest` в
  `TESTS`.
- `scripts/inittest.py` (новый): serial-регрессия.
- Без изменений: terminal.c, vfs, pipe, procfs (PPid покажет нового родителя
  автоматически), WM и остальные приложения, существующие тесты.

## Architecture

### A. init_pid и репарентинг сирот (kernel/task.c)

Глобальный `static unsigned int init_pid = 0;` + акцессоры
`task_set_init_pid(pid)` / `task_init_pid()`. Значение ставит `kernel_main`
после первого spawn'а `bin/init`.

В `task_switch_kernel`, в exited-блоке, **после** существующего цикла сбора
зомби-детей (task.c:190-194): выжившие дети умершего (state не FREE и не
ZOMBIE) получают `parent = init_pid`, если `init_pid != 0` и
`init_pid != dead->pid`. Если умер сам init — дети остаются со старым parent;
ими занимается `task_reassign_children()` при пересоздании init'а (секция C).

Дети init'а имеют `parent == init_pid` с рождения (`task_spawn` ставит
`parent = task_current_pid()`, task.c:336), поэтому репарентинг и «обычные»
сервисы идут через один и тот же механизм.

### B. Уведомление init о смерти ребёнка

Тип сообщения — уже существующий `MSG_EXIT` (6), поля: `a = pid`,
`b = exit_code`. В exited-блоке `task_switch_kernel`, сразу после установки
`TASK_ZOMBIE`:

```c
if (dead->parent == init_pid && dead->parent != dead->pid &&
    task_alive(dead->parent))
    task_mailbox_send(dead->parent, MSG_TYPE_EXIT, dead->pid,
                      current_exit_code, 0, 0);
```

Init получает одно сообщение на каждую смерть своего ребёнка (сервиса или
сироты) и делает `aos_waitpid(pid)` → код выхода. Существующие нотификации
`sink`/`event_pid` не затрагиваются (у сервисов sink=0, init не event
consumer — задвоений нет).

Почему mailbox, а не polling `GET_CHILDREN`: mbox уже есть, init не жжёт CPU,
сообщения копятся пока init занят.

### C. Перезапуск init ядром

`kernel/kernel.c`, функция `ensure_init()` в начале главной итерации `while(1)`
(до `terminal_run_pending()`):

- `init_pid` жив → ничего.
- Мёртв/ещё не запущен, throttle не чаще раза в 100 тиков →
  `task_spawn("bin/init", "", 0, &npid, env)`, где env =
  `"AOS_MODE=gui\0"` или `"AOS_MODE=text\0"` по `vga_fb_active()`.
- При успехе: `task_set_init_pid(npid)`; если был старый pid —
  `task_reassign_children(old, npid)` (новая функция в task.c): живым детям
  старого init меняет parent на нового, зомби старого init FREE-ит (ждать их
  больше некому).

Если spawn не удался (нет `bin/init` в ramdisk / нет слотов) — повтор на
следующем throttle-тике; система продолжает работать на ядерном шелле.
Лог: `printf("init spawned (pid %u).\n")` при старте,
`serial_print("init: respawn attempt failed\n")` при неудаче.

### D. Кооперативный kill: `AOS_KILL` (525)

Прерывание ring-3 таска извне невозможно, поэтому kill — кооперативный:
целевой таск завершается на следующем входе в ядро.

- `struct task` + поле `unsigned int kill_pending;` (обнуляется `memset` в
  `task_spawn`).
- `int task_kill(unsigned int pid)` (task.c): отказ для `pid == 0` (ядро) и
  не-живых тасков (`-1`); иначе под `irq_save` ставит `kill_pending = 1`,
  возвращает 0. Kill самого себя разрешён (работает как `exit(9)`), kill
  init разрешён (ядро его перезапустит — это же механизм проверки секции C).
- `kernel/syscall.c`, в самом начале `syscall_handler`: если
  `get_current_task()->kill_pending` — снять флаг, `task_exit_current(9)`,
  `r->eax = 0`, `return` (тот же путь, что SYS_EXIT: iret в ring 3, прибирает
  планировщик на следующем тике). Покрывает все ABI (route на aos_gui/linux
  происходит ниже).
- `task_sleep()`: та же проверка в начале — спящий таск умирает, не дожидаясь
  `wake_tick`.
- `kernel/aos_gui.c`: `case AOS_KILL: r->eax = task_kill(r->ebx); break;`
  (500–599 маршрутизируются в `aos_gui_handler` независимо от ABI, поэтому
  musl-процессам syscall доступен).
- `programs/aosabi.h`: `#define AOS_KILL 525` + wrapper
  `aos_kill(unsigned int pid)`.

Код выхода убитого таска — 9 (по аналогии с номером SIGKILL); в serial-логе
виден как `TEC:pid=N code=9`.

Ограничение: таск в бесконечном ring-3 цикле без syscall'ов не умрёт никогда
(документируем; все наши GUI-приложения крутят `aos_recv`/`sleep` — им подходит).

### E. Builtin `kill` в ядерном шелле

`kernel/commands.c`: `cmd_is_builtin()` += `"kill"`; диспетчер
(`run_command_raw`) → `cmd_kill(arg)`: парс десятичного pid,
`task_kill(pid)`, печать `\nkill: pid N signaled` либо
`\nusage: kill <pid>` / `\nkill: no such process`. GUI `sh` может запускать
`bin/kill`? — нет, отдельная программа не нужна: из `sh` kill доступен через
будущий userland-тул (out of scope); тест драйвит ядерный builtin через serial.

### F. `/etc/init.conf`

Плоский key=value, парсит сам init (ядро конфиг не читает). Embedded в
ramdisk через `gen_progs.py --data etc/init.conf=scripts/init.conf`;
`load_embedded_data()` кладёт его в SFS при первом буте (механизм demo.ico).

Формат (плотные индексы svc1..svc8, ключи в любом порядке внутри группы):

```
svc1.path=bin/wm
svc1.respawn=1
svc1.mode=gui
svc2.path=bin/clock
svc2.respawn=1
svc2.mode=gui
```

- `svcN.path` — программа (обязательное; группа без path игнорируется);
- `svcN.args` — аргументы одной строкой (опционально);
- `svcN.respawn=1` — респавнить при любом выходе (краш и чистый exit);
- `svcN.mode=gui` — спавнить только при `AOS_MODE=gui`; без `mode` — всегда.

### G. `/bin/init` (programs/musl/init.c)

Цикл (весь файл ~180 строк):

```
main:
  load_conf()          # open/read /etc/init.conf, strncmp-парсер
  gui = getenv("AOS_MODE") == "gui"
  for svc: if (подходит по mode) spawn_svc()
  loop:
    if (aos_recv(&m) == 0 && m.type == MSG_EXIT):
        найти svc по pid; code = aos_waitpid(pid); лог
        life = tick_now - start_tick
        if life > 60s: fast_deaths = 0
        elif life < 1s: fast_deaths++
        if respawn && !failed:
            if fast_deaths >= 5: failed = 1; лог "crashed 5x fast, giving up"
            else: pending = 1; due = now + 100
        continue
    for svc: if pending && !failed && due наступил: spawn_svc()
    usleep(10ms)
```

Политика респавна (зафиксировано с пользователем — респавн обязателен):

- **Backoff** 100 тиков (1 c) между смертью и рестартом; во время паузы
  mailbox копит следующие MSG_EXIT — сбор зомби не теряется.
- **Crash-loop guard**: сервис, умерший 5 раз «быстро» (< 100 тиков жизни)
  подряд, помечается `failed` и больше не респавнится; счётчик сбрасывается,
  если сервис прожил дольше 60 с. Лог: `init: <path> crashed 5x fast, giving up`.
- **Слоты**: если `task_spawn` вернул ошибку (нет слотов), сервис просто не
  запущен; retry произойдёт при следующей смерти другого ребёнка либо вручную.
  Отдельного планировщика нет.
- Вывод init'а — `write(1, ...)` → fd 1 консоль → COM1 (sink=0, stdout_fd=-1 →
  `terminal_write`), т.е. весь журнал виден в serial-логе — на нём и строятся
  проверки теста.

Сообщения init: `init: started (pid N, mode gui)`, `init: started <path>
(pid P)`, `init: exited <path> (code C, life L ticks)`,
`init: respawn <path> scheduled`, `init: <path> crashed 5x fast, giving up`.

### H. kernel_main

Блок spawn'а wm (kernel/kernel.c:152-172) удаляется; вместо него — spawn
`bin/init` с env `AOS_MODE=gui|text` и `task_set_init_pid()`. В главном цикле —
`ensure_init()`. Text-mode boot: init запускается, не спавнит ни одного
`mode=gui`-сервиса и тихо висит в цикле (консоль остаётся у ядерного шелла).

## Data flow

```
boot ─► kernel_main ─► task_spawn(bin/init, env=AOS_MODE=…) ─► task_set_init_pid(P)
init: /etc/init.conf ─► aos_spawn(bin/wm), aos_spawn(bin/clock)   [parent=P]
wm crash ─► task_switch_kernel: TASK_ZOMBIE ─► mbox(init): MSG_EXIT{pid,code}
init: aos_waitpid ─► лог ─► backoff 1s ─► aos_spawn(bin/wm) ─► новый pid
shell: kill <pid> ─► AOS_KILL ─► target.kill_pending=1
target: следующий syscall ─► syscall_handler ─► task_exit_current(9) ─► MSG_EXIT
init death ─► главный цикл task 0: ensure_init ─► respawn + task_reassign_children
```

## Error handling

- `task_kill(0)` / несуществующий pid → `-1` (builtin печатает
  `kill: no such process`).
- `kill_pending` у task 0 невозможен (kill pid 0 запрещён).
- MSG_EXIT в полный mailbox молча теряется (как у существующих нотификаций):
  зомби останется до реклайма слотов — приемлемая деградация.
- init умер и `bin/init` недоступен → система живёт на ядерном шелле, попытки
  раз в 100 мс, каждая неудача — одна строка в serial.
- `/etc/init.conf` отсутствует/битый → init логирует и продолжает жить пустым
  (GUI нет, но система управляема через serial).
- Конфиг с кривыми индексами (`svc9` при максимуме 8, разрывы индексов) —
  такие группы игнорируются.

## Testing

**1. `scripts/inittest.py`** (serial-only, по паттерну sleeptest.py):

- boot, `ps` → в выводе есть строка `init` (PID/PPID/STATE/ABI/CMD) и `wm`;
- распарсить PID wm из `ps`, послать `kill <pid>` → в логе
  `TEC:pid=<pid> code=9`, затем `init: started bin/wm (pid <новый>)` в течение
  ~30 c;
- повторный `ps` → wm с новым pid, PPid = pid init'а;
- негатив: `kill 0` → `kill: no such process`; `kill 999` → то же;
- сироты: из GUI-шела не проверить серийно — косвенно покрывается тем, что
  после kill wm старые зомби не копятся (повторный `ps` не показывает zomb-wm);
- ни одного `KERNEL PANIC` за прогон.

Crash-loop guard в автотест не включается (под TCG wm редко умирает «быстрее
100 тиков») — проверяется вручную: пять быстрых `kill` подряд → в логе
`giving up`, шестой kill не приводит к респавну.

**2. Регрессия**: `make test-fast` + GUI-тесты (`vguitest`, `powertest`,
`notepadtest`) — WM теперь ребёнок init'а, поведение WM не менялось; `pstest`
(PPid у процессов изменился на ожидаемый), `pipetest` (exec_pipe не тронут).
Build: `make` без новых `-Wall -Wextra` предупреждений.

## Out of scope

- Shutdown-протокол (kill-all → sync → reboot через init); wm по-прежнему
  зовёт syscall 88/14 напрямую.
- Сигналы общего вида, приоритеты, per-service зависимости/таймауты старта.
- `bin/kill` как внешняя утилита для GUI `sh`.
- Getty/перенос консольного шелла в userland.
- Runlevels/targets, перечитывание конфига на лету (reload).
