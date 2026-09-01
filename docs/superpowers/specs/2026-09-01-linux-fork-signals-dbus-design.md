# AOS — Linux mkprocesses: fork, signals, multi-process, dbus-daemon (СПЕКА)

Date: 2026-09-01
Status: spec (roadmap + design); реализация постадийная.

## Goal

Дать ядру полноценную Linux-модель процессов, чтобы на AOS** реально запускался
`dbus-daemon` (системная и сессионная шина), а в перспективе — другие
многопроцессные musl-программы (`dbus-send`, активируемые сервисы).

Текущий порт `dbus-daemon` **собран** (статический `ET_EXEC` для
i686-linux-musl, ~1 МБ, без динамических зависимостей), но **не работает**:
его POSIX-прослойка (`dbus-sysdeps-unix.c`, `dbus-spawn-unix.c`) использует
syscall'ы, которых в AOS нет — `fork`, `execve`, `sigaction` (доставка сигналов),
`socketpair`, `dup2`, `setsid`, `waitpid`, `kill`, `getppid`. Без них
`dbus-daemon` не может даже пройти инициализацию главного цикла.

## Текущее состояние (базис)

- AOS исполняет статические musl-ELF (ET_EXEC, без INTERP) как отдельные задачи
  (`ABI_LINUX`), каждая со своим адресным пространством `0x08000000..0x10000000`
  (PD 32..63, приватные `lpts[32]`), 1:1 identity-картами страниц.
- Scheduler: round-robin по `struct task tasks[MAX_TASKS=24]`
  (`kernel/task.c`); состояния `FREE/READY/RUNNING/SLEEPING/WAITING/ZOMBIE/SPAWNING`;
  блокирование через `sti; hlt; cli` в хендлере syscall.
- Linux-ABI syscalls: `int 0x80`, диспетчер `linux_syscall_handler(struct registers *r)`
  (82 case'а). `struct registers` (`kernel/interrupts.h`) — полный фрейм gs..ss.
- Пайпы и AF_UNIX-сокеты есть; `poll(168)` и `getrandom(355)` реализованы.
- `execve(11)` заменяет адресное пространство текущей задачи (перезагрузка ELF
  в тот же `t->cr3`), закрывает fds≥3, обновляет `r->eip/user_esp` — но
  **fork из него нет, процессов как дерева нет** (есть только `task_spawn`).
- Сигналов в ядре нет вообще: `sigaction(67)`, `sigprocmask(126)`,
  `sigreturn`, `kill` не реализованы; нет механизма доставки сигнала в
  пользовательский обработчик в ring 3.

## Чего не хватает (точный разрыв, из `dbus-sysdeps-unix.c`/`dbus-spawn-unix.c`)

| Syscall | № i386 | Использование в dbus | Нужно для |
|---|---|---|---|
| `fork` | 2 | dbus-spawn, daemonize | создание процесса |
| `execve` | 11 | реактивация сервисов | (частично есть — для текущей задачи) |
| `dup2` | 63 | перестановка fds (well-known fd, spawn) | fd-слой |
| `setsid` | 66 | daemonize | сессии |
| `sigaction` | 67 | главный цикл (SIGTERM/INT/HUP) | сигналы |
| `sigprocmask` | 126 | маска для daemonize-warning | сигналы |
| `socketpair` | 360 | spawn-errpipe, error-pipe соединений | сокеты |
| `kill` | 37 | отправка сигналов | сигналы |
| `waitpid` | 114 | реap спавн-детей | процессы |
| `getppid` | 64 | обнаружение смерти родителя | процессы |
| `getpgid/setpgid` | 121/122 | группы | опционально |
| `prctl` | 157 | ограничения | заглушка |
| `signal`/`sigaction` trampoline | — | `_dbus_handle_unix_signals` | сигналы |

Все перечисленное — syscall'ы i386; номера подтверждены
`tools/musl-i686/.../bits/syscall.h`.

## Подход: стадии (roadmap)

Реализация разбита на 4 стадии, каждая — самостоятельный проверяемый блок
(спека→план→impl→тест по конвенции проекта). Каждая стадия добавляет тест
под QEMU (text-mode harness, устойчивый к serial-гонке — см. `polltest.py`).

### Стадия 1 — `fork` + `dup2` (копия процесса, COW-прозрачно)

Ядро:
- **`kernel/fork.c`** (новый): `linux_fork(r)` — создаёт дочернюю задачу как
  **копию** родителя:
  - выделяет новый `struct task` slot, kstack, mailbox, args (копия), name (тот же),
    новый `lctx` (побайтовая копия), копию `cwd`, `env`/`umask`/uid/gid.
  - **копирует адресное пространство**: для 32 записей `lpts[32]` родителя
    создаёт новые PT-страницы и копирует их содержимое → дочерние `lpts[]`.
  - копирует таблицу `fds[]` с инкрементом refcount (`vfs_get`/pipe `pipe_dup`/
    sock `sock_dup`) — как в `vfs_dup_fd`/`task_spawn`, но для **всех** fds≥3.
  - `parent` у ребёнка = текущий pid; оба процесса видят один вход в `fork`.
- **Возврат**: в родителе `r->eax = child_pid`; в ребёнке синтезируется
  идентичный кадр, но `r->eax = 0`. Механизм «второго iret»: дочерний
  `kernel_esp` строится так, чтобы после возврата из ребёнка `switch_and_restore`
  восстановил фрейм с `eax=0`. Для этого после копирования дочернему кадру
  пере-пишется `eax`. Родитель не блокируется.
- **`dup2(oldfd, newfd)` (63)**: рядом с `dup(58)`? (dup у musl — 63? нет — на
  i386 `dup=41`, `dup2=63`). Реализуем: если `newfd == oldfd` → вернуть newfd;
  если `newfd` открыт — закрыть; скопировать `fds[oldfd]` в `fds[newfd]` с
  инкрементом inode-refcount (VFS/ofile-refcount, pipe/sock dup-хелперы).

Примечание по COW: на AOS адресное пространство Linux-задачи уже 1:1 и компактно;
на стадии 1 делаем **жадное копирование** (32 PT × 1024 PTE = до 32 МБ физики —
это дорого: 8k страниц). Для dbus это неприемлемо по памяти. **Решение на
стадии 1.5**: COW — либо разделяем страницы и ловим page-fault в
`page_fault_handler` (нужно добавить обработку do_page_fault для пользовательских
COW-страниц), либо проще — копируем только «живую» часть по карте `lpts[]`,
страницы которых реально присутствуют. Стадия 1 делает проверяемый прототип
(копия с ленивой нехваткой), стадия 1.5 — COW/тонкий трекинг.

### Стадия 2 — `waitpid`, `getppid`, `touch` новых детей, `socketpair`, `setsid`

- **`waitpid(pid, &status, options)` (114)** для Linux-ABI: текущая AOS-модель —
  `waitpid` по конкретному `pid` из `task_waitpid`. Добавляем Linux-семантику:
  `-1` = любой ребёнок, `WNOHANG` = не блокировать. Возврат: `pid` (или `-1`
  если нет детей; `0` если WNOHANG и нет готовых); запись `status` в
  пользовательский `int*` (код выхода, `WEXITSTATUS`).
- **`getppid` (64)**: вернуть `parent`.
- **`getpgid/setpgid/getsid`**: минимальные (заглушки, возвращают pid).
- **`setsid` (66)**: создать новую сессию — на AOS возврат `pid` (нет реальных
  управляющих терминалов).
- **`socketpair(domain,type,proto,fds[2])` (360)** в `kernel/socket.c`:
  создать два connected-сокета с общим потоковым буфером (как connect-пара),
  записать оба fd в пользовательский `int[2]` через `vfs_attach_ofile`.
- **`kill(pid, sig)` (37)** и **`tkill`**: `_dbus_*` и main-loop используют для
  оповещения; на стадии 2 — «смягчённый kill»: для `SIGTERM/INT/HUP` ставим
  `kill_pending` (по образцу `task_kill` → exit(9)), возврат 0; реальная
  асинхронная доставка обработчику — стадия 3.

Тест стадии 2: `tools/linux/forktest2.c` — fork→(child waitpid)→exit code;
socketpair+запись/чтение; setsid=getpid.

### Стадия 3 — сигналы: `sigaction`, `sigprocmask`, `kill`, доставка в ring 3

Ядро:
- **модель**: у каждой задачи — 32 слота сигналов; обработчик
  `struct aos_sigaction {handler, mask, flags}` (kernel-копия), pending-битовые
  маски. `kill(pid,sig)`/`raise` ставят бит pending.
- **Доставка**: на переходе ring3→обработчик синтезируется кадр:
  1. сохранить текущий `r` (полный regs) в пользовательский стек (как
     `sigreturn`-фрейм musl) или в ядре (kal trampoline).
  2. подменить `r->eip = (handler)`, `r->user_esp` предварительно выделяется на
     стеке (с сохранением старого ESP и возврата), `r->eax/ecx/edx = sig/siginfo/ctx`.
  3. musl вызывает `sigreturn(173)` после обработчика → ядро восстанавливает
     сохранённый `r` (из фрейма на стеке) — классическая RT signal trampoline с
     `movl $__NR_rt_sigreturn, %eax; int 0x80`.
- **`sigaction(67)` / `rt_sigaction(174)`**: записать обработчик, маску, флаги
  (`SA_RESTORER`/`SA_SIGINFO`). `sigprocmask(126)`/`rt_sigprocmask(175)`:
  маскирование.
- **`sigreturn(173)`/`rt_sigreturn(173? no: rt=175?)`**: восстановить контекст.
- Экстремально важно: не инжектить сигналы внутрь syscall-хендлера, а только на
  границе возврата в ring 3 (после того как хендлер вернулся) — как в
  существующем `switch_and_restore`. Расположение инжектирования уточнить по
  `boot/isr.S` / `syscall_entry`.

### Стадия 4 — dbus-daemon запуск и интеграция

- Собрать `dbus-daemon` + `dbus-send`/`dbus-monitor` **ФИНАЛЬНО**:
  записать в `tools/lib/` скрипт воспроизводимой статической сборки
  (`scripts/build-dbus.sh`): fetch expat → cross → fetch dbus → configure
  (`--disable-shared --enable-static -no-pie -static`, ручной линк, чтобы обойти
  libtool, который снимает `-static`) → установить.
- Встроить в ramdisk `bin/dbus-daemon` (и `dbus-send`) через `gen_progs.py --data`.
- **machine-id**: `dbus-daemon` читает `/var/lib/dbus/machine-id`
  (`--with-dbus-machine-uuid-dir`). На AOS нет `/var`; создать `/.dbus-machine-id`
  при первом запуске через `dbus-uuidgen --ensure` или на этапе инициализации
  (kernel/load_embedded_data) — 16 hex из `getrandom(355)`. Настроить
  `--machine-id=` (dbus 1.14 поддерживает) или задать конфиг.
- **Конфиг**: `--config-file=/etc/dbus/system.conf` — подготовить минимальный
  system.conf (слушать `unix:path=/var/run/dbus/system_bus_socket` абстрактно,
  `<policy context="default"><allow send_destination="*" eavesdrop="true"/>...`).
  Или проще session bus: `dbus-daemon --session --nofork --nopidfile`.
- **Запуск как задача**: `bin/dbus-run-session` нет — запускаем
  `dbus-daemon --nofork --print-address` под `bin/linrun`-механизмом
  (реальный pid>0), ждём готовности сокета; `dbus-send`-клиент соединяется.
- **EOF/ожидание**: главный цикл dbus использует `poll` + сигналы; при
  отсутствии активности dbus-daemon должен выходить по SIGTERM.

## Проектирование: reuse-точки в ядре

- Копия адресного пространства: `task_free_addrspace(t)` (заменяемая частями),
  `lpts[32]`, `paging_map_user_page`, `paging_kernel_pd`/`paging_set_cr3`.
- Копия fds: `vfs_dup_fd`, `pipe_dup`, `sock_dup`, `vfs_attach_ofile`,
  `vfs_close_fd`, `vfs_close_all`.
- Блокирование: `task_waitpid`/`task_sleep` (паттерн `sti;hlt;cli` + promote в
  `task_switch_kernel`).
- Синтетический кадр запуска: образец в `task_spawn` (w[0..18]).
- Диспетчер: `linux_syscall_handler(struct registers *r)` — добавить case'ы.

## Реальная сложность (важно для плана)

- **fork-копия 32 МБ** физически несёт 8k page-alloc на ребёнка — для dbus
  нужен COW/ленивое копирование, иначе OOM при 2+ задачах. Это самый большой
  риск и главный пункт стадии 1.5.
- **Сигналы на ring 3** требуют аккуратной работы с кадром/стеком и
  trampoline `sigreturn`; ошибки дают вис или панику — тестировать поэлементно.
- `dbus-daemon` очень требователен к точности (fork+exec, signal self-pipe,
  fds). Полный «жёсткий» тест лучше делать на завершающем этапе.

## Error handling & edge cases (стадия 1 — fork)

| Случай | Поведение |
|---|---|
| Нет свободного slot`а для ребёнка | `fork` → `-1` (EAGAIN) |
| Не хватает страниц памяти для копии | `fork` → `-1` (ENOMEM), родитель цел |
| fork внутри syscall-хендлера, ребёнок должен вернуть 0 | переписать `eax=0` в дочернем фрейме |
| dup2(newfd>63) | `-EBADF` |
| dup2 с oldfd невалидным | `-EBADF`, newfd не трогаем |
| fork+ exec сразу (двойной форк) | дочерний процесс закрывает лишние fds сам |
| Родитель выходит раньше ребёнка | ребёнок → reparent в init (существующий механизм `task_reassign_children`) |

## Verification

- Каждая стадия: `make` + text-mode harness (`scripts/forktest.py` и т.д.),
  устойчивый к serial-гонке (см. `polltest.py`: `--r` + `AOS (text)`).
- Полная регрессия `make test` — не ломать зеро: `linhello lincat pipetest
  socktest polltest` и GUI-тесты должны остаться зелёными.
- Стадия 4: `scripts/dbustest.py` — запуск `dbus-daemon --nofork --session`,
  `dbus-send --session --print-reply` → `/org/freedesktop/DBus` `Peer.Ping`,
  `dbus-monitor` ловит эхо.

## Files touched (ориентир, уточняется по стадиям)

- `kernel/fork.{c,h}` (новый) — копия задачи, dup2, spawn-ребёнка.
- `kernel/task.h/.c` — расширение `struct task` (сигнальные поля, session),
  `task_fork()`, `task_fork_copy_fds()`, waitpid-любой-ребёнок, getppid.
- `kernel/paging.*` — helpers копирования/COW страниц, do_page_fault COW.
- `kernel/linux_syscall.c` — case 2/37/63/64/66/67/114/126/173/174/175/360.
- `kernel/socket.c` — `sock_sys_socketpair`.
- `kernel/signal.{c,h}` (новый) — sigaction/маски/доставка.
- `scripts/build-dbus.sh` — воспроизводимая статическая сборка expat+dbus.
- `scripts/*test.py`, `tools/linux/*test.c` — тесты стадий.
- `AGENTS.md` — счётчик syscall, документация fork/signals.

## Принятые решения (по ответу пользователя)

- Реализуем **полную многопроцессовую поддержку** (fork + exec для детей +
  сигналы + multi-process), не только минимальный --nofork.
- Сначала — **ревизованный план/спека** (этот документ), затем постадийная
  реализация с проверяемыми вехами.
