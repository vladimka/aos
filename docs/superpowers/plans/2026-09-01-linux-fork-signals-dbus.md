# AOS — Linux mkprocesses (fork, signals, multi-process, dbus-daemon) — ПЛАН

Date: 2026-09-01
Ref: [спека](specs/2026-09-01-linux-fork-signals-dbus-design.md)

## Контекст

`dbus-daemon` уже **собран** статически для i686-musl (ET_EXEC, без динамики),
но не работает: ядру не хватает процессов/сигналов (разрыв задокументирован в
спеке). Пользователь выбрал полную многопроцессовую поддержку + минимальный
fork+сигналы + сначала ревизованный план. Этот план — постадийная дорожная
карта; каждая стадия заканчивается работающим тестом под QEMU (text-mode).

## Стадия 1 — fork + dup2 (копия процесса)

- [ ] `kernel/fork.c/.h`: `linux_fork()` — копия задачи (kstack, mbx, args, name,
      lctx, cwd, env, uid/gid); копия адресного пространства (32×PT из `lpts[]`);
      копия `fds[]` с инкрементом refcount; `parent = cur_pid`; `t->state=READY`.
- [ ] Синтетический кадр дочернего (по образцу `task_spawn`, w[0..18]), `eax=0`
      в дочернем фрейме, `eax=child_pid` — в родительском.
- [ ] `dup2(oldfd,newfd)` case 63 (с mask/закрытием newfd, `vfs_attach`-обёрткой).
- [ ] Проверка: `tools/linux/forktest.c` — fork→(child exit code 7)→waitpid
      (стадия 2 для waitpid, тут — через живой кадр: ребёнок печатает), dup2.
- [ ] `scripts/forktest.py` (text-mode harness), регрессия `make test`.

### Стадия 1.5 — не жрать 32 МБ: COW копирование  ✅ выполнено

- [x] COW: `task_fork` демотирует общие writable-страницы в read-only + `PTE_COW`
      (бит 9); `isr_handler`/`task_handle_cow_fault` ловит user-write-`#PF` и
      отдаёт приватную writable-копию.
- [x] Проверка: `cowtest.c` — изоляция записи родителя/ребёнка после fork;
      `forktest` тоже проходит на COW-пути (без OOM).

## Стадия 2 — waitpid/getppid/socketpair/setsid/kill(stub)  ✅ выполнено

- [x] `waitpid`/`wait4(114)` Linux-семантика: `wantpid>0`=конкретный ребёнок,
      `<-1/0`=любой; `WNOHANG`→0; статус `(exit_code&0xff)<<8` в user `int*`;
      `-ECHILD` при отсутствии детей/ребёнка; блокировка через `TASK_WAITING`.
- [x] `getppid(64)`, `getpgrp(65)`, `getpgid(132)`, `getsid(147)`, `setsid(66)`
      → собственный pid; `setpgid(57)` → 0; `prctl(155)` → 0.
- [x] `socketpair(360)` в `kernel/socket.c` (`sock_sys_socketpair`, двунаправленный
      stream поверх одного буфера).
- [x] `kill(37)` stub → `kill_pending`/`task_kill` (настоящая доставка — стадия 3).
- [x] STAGE-2 фикс COW: per-frame `cow_ref` в `pmm_frames[]` (`cow_link`/
      `cow_unlink`) — иначе при выходе одного из процессов fork-доли freeing
      ломал оставшегося родителя (паника `CR2=0x4` после первого reaped child).
- [x] Тест `tools/linux/stage2test.c` (wait4 specific+status / WNOHANG / socketpair /
      setsid+getpgrp+getpgid+getsid) + `scripts/stage2test.py`, регрессия
      `make test`; `forktest`/`cowtest` тоже зелёные на COW-пути.

## Стадия 3 — сигналы (sigaction/sigprocmask/sigreturn/kill)

- [ ] Модель: 32 слота на задачу, pending-маска, kernel-копия
      `struct aos_sigaction`.
- [ ] `rt_sigaction(174)/sigaction(67)`, `rt_sigprocmask(175)/sigprocmask(126)`.
- [ ] Доставка на границе ring3-возврата: сохранение regs на пользовательском
      стеке + trampoline `sigreturn`; case 173/т.п.
- [ ] `kill/raise/tkill` ставят pending; доставка на границу.
- [ ] Тест `tools/linux/sigtest.c`: поставить SIGUSR1-handler, `raise(SIGUSR1)`,
      обработчик печатает + счётчик; `scripts/sigtest.py`.

## Стадия 4 — dbus-daemon: сборка в ramdisk + запуск + клиент

- [ ] `scripts/build-dbus.sh` — воспроизводимая статическая сборка expat+dbus
      (сейчас в /tmp/opencode; руками линкуем `-static` из-за libtool).
- [ ] Встроить `bin/dbus-daemon`, `bin/dbus-send`, `bin/dbus-uuidgen`,
      `bin/dbus-monitor` (через `--data lin/...`).
- [ ] machine-id (`/.dbus-machine-id` из getrandom) на этапе загрузки
      (`load_embedded_data` / первый запуск).
- [ ] Минимальный `system.conf` / session запуск; `dbus-daemon --nofork --session
      --print-address`.
- [ ] `scripts/dbustest.py`: запуск daemon под `bin/linrun`, `dbus-send
      --session --print-reply` → `org.freedesktop.DBus.Peer.Ping`, `dbus-monitor`
      ловит эхо. Регрессия.

## Критерии завершения

- Все стадии: `make` + text-mode тесты зелёные; полная `make test` не ломается.
- `dbus-daemon` стартует, отвечает на `dbus-send Ping`, `dbus-monitor` видит
  трафик — шина реально работает на AOS.

## Файлы (ориентир)

`kernel/fork.{c,h}` `kernel/signal.{c,h}` `kernel/task.{c,h}`
`kernel/paging.{c,h}` `kernel/linux_syscall.c` `kernel/socket.c`
`scripts/build-dbus.sh` `tools/linux/{forktest,forktest2,sigtest}.c`
`scripts/{forktest,forktest2,sigtest,dbustest}.py` + Makefile + AGENTS.md
