# Спецификация: AF_UNIX сокеты в ядре AOS (фундамент для dbus-daemon)

Дата: 2026-09-01
Автор: opencode (по задаче «нам нужен dbus-daemon»)

## Цель

Портировать `dbus-daemon` в AOS. Это разбивается на несколько независимых
блоков; настоящий документ описывает **фундаментальный блок №1 — AF_UNIX
socket subsystem в ядре**. Без него D-Bus (построенного на Unix domain
sockets) просто не существует.

## Мотивация

Из анализа (см. план) ядро `dbus-daemon` не зависит от GLib/systemd; ему
нужны: libc (musl — уже есть), libexpat (портируется отдельно), Unix domain
sockets, `poll()`, источник случайности, `machine-id`. Главный блокер — в AOS
нет socket API вообще:
- `linux_syscall_handler()` возвращает `-ENOSYS` для socket/bind/listen/
  accept/connect/sendto/recvfrom/shutdown/getsockname/getpeername/…
- нет `poll()`/`select()`

Этот документ закрывает сокетную часть.

## Архитектура

AF_UNIX реализуется по образу существующего `pipefs_fs` (`kernel/pipe.c`),
который уже показал паттерн «псевдо-fs с встроенным `vfs_inode` в
структуру»:

| Аспект | pipe | сокет (новое) |
|--------|------|---------------|
| Файл | `kernel/pipe.c` + `pipe.h` | `kernel/socket.c` + `socket.h` |
| fs | `pipefs_fs` | `sockfs_fs` |
| структура | `struct aos_pipe { …, vfs_inode; }` | `struct aos_sock { …, vfs_inode; }` |
| поток данных | `pipe_read_at/write_at` | `sock_read_at/sock_write_at` |
| close-хук | `pipe_close` | `sock_close` |
| блокировка | `sti; hlt; cli` | `sti; hlt; cli` |
| неблокирующий | `pipe_read_nonblock` | `sock_read_nonblock` |

### Пул сокетов

Статический массив (ядерная память), как `pipes[PIPE_MAX]`:

```c
#define SOCK_MAX 16
#define SOCK_BUF_SIZE 4096

enum sock_state { SOCK_FREE=0, SOCK_LISTENING, SOCK_CONNECTED, SOCK_PENDING };

struct aos_sock {
    unsigned int used;          // 1 = занят
    unsigned int state;         // enum sock_state
    char name[32];              // abstract-имя (server) или партнёр-ссылка
    int domain, type, protocol; // AF_UNIX / SOCK_STREAM / 0
    // партнёр для connected-сокета (при accept создаётся pair)
    struct aos_sock *peer;
    // серверная сторона: bound-имя + очередь pending
    int is_listener;            // имеет bound abstract-имя, слушает
    struct aos_sock *pending[8]; // подключившиеся, ждущие accept
    int n_pending;
    // буфер потока (аналог pipe)
    unsigned int head, tail, count;
    unsigned char buf[SOCK_BUF_SIZE];
    unsigned int peer_eof;      // партнёр закрыл свою сторону записи
    struct vfs_inode inode;
};
```

### Абстрактные сокеты

D-Bus по умолчанию использует `unix:path=/run/dbus/system_bus_socket` для
system bus и `unix:path=$XDG_RUNTIME_DIR/bus` / abstract для session.

SFS (файловая система AOS) не имеет понятия socket-файла (типа `S_IFSOCK`),
а создавать сокетный файл как обычный файл нельзя. Поэтому реализуем
**abstract namespace** — имя задаётся как строка длиной `1..108`, с первым
байтом `'\0'` (Linux abstract), либо без NUL (path-имя, которое в AOS
просто сопоставляется внутри ядра, не трогая VFS). В обоих случаях имя
хранится в `struct aos_sock.name` внутри ядра и не создаёт файла в SFS.

Linux `bind()` с `sun_path[0]=='\0'` = abstract; `dbus-daemon` (и musl
`dbus`) умеют `unix:abstract=`. Для совместимости поддерживаем оба: если
первый байт `'\0'` — abstract, иначе — path-like строка. Ни тот, ни другой
не создают файл в VFS.

### Поток данных connected-сокета

Пара `server`/`client` сокетов имеет двунаправленный поток. Используем
**один общий буфер на пару** (как pipe): запись в любой конец идёт в один
буфер, чтение — из него. Так проще и соответствует pipe-модели, которой
уже учит AOS:

```
struct aos_sock *pair = peer_socks_alloc();  // .peer указывает друг на друга
read(sockA)  -> читает из буфера pair
write(sockB) -> пишет в буфер pair
```

Реализация: `struct aos_sock` у server-конца несёт буфер, client-конец
указывает на него через `->peer` или оба указывают на общий
`struct aos_sock *stream`. Для простоты и надёжности: **буфер живёт в
`struct aos_sock` серверной стороны пары** (той, что получена на listen-
сокете при accept). Оба конца партнёрскими указывают на пару.

Дизайн-решение: `accept()` создаёт **новый** сокет `A` (серверный конец
пары) и возвращает новый fd; `connect()` создаёт клиентский сокет `B`.
Оба хранят `->stream`, указывающий на общий буфер, размещённый в `A`
(который живёт, пока хоть один конец пары открыт).

```
accept_fd (A) <--> buf[A] <--> (B) connect_fd
```

### Операции

- `socket(domain, type, proto)`:
  - только `AF_UNIX==1`, `SOCK_STREAM==1`, `proto==0` (иначе `-EAFNOSUPPORT`).
  - аллоцирует `struct aos_sock`, `state=SOCK_CONNECTED`-одиночный (без пары).
  - резервирует fd в `ofiles[]`, `ofiles[fd].inode = &sock->inode`,
    флаги `VFS_O_RDWR`.
- `bind(fd, addr)`: копирует `sun_path`, помечает `is_listener=1`,
  регистрирует abstract-имя. Повторный bind → `-EADDRINUSE`.
- `listen(fd, backlog)`: `state=SOCK_LISTENING`.
- `connect(fd, addr)`: ищет bound-сокет с таким именем; если найден и
  listening — аллоцирует клиентский сокет, добавляет в `pending[]`
  сервера, возвращает 0. Иначе `-ECONNREFUSED`.
- `accept(fd, addr, len)`: на listening-сокете ждёт (spin) появления
  `pending[0]`, вынимает его, создаёт пару с одним потоком, возвращает
  новый fd.
- `read(fd)`/`write(fd)`: через `sock_read_at`/`sock_write_at`,
  блокирующие и неблокирующие варианты (по аналогии с pipe).
- `shutdown(fd, how)`: помечает EOF.
- `getsockname`/`getpeername`: копируют abstract-имя.
- `close(fd)`: через `sock_close` — обновляет счётчики концов пары,
  освобождает слот, когда оба конца закрыты.
- `getsockopt`/`setsockopt`: минимальные заглушки (SO_PEERCRED → uid=0
  текущего task, остальные -ENOPROTOOPT / no-op).

### Встраивание в VFS read/write

`vfs_read_fd`/`vfs_write_fd` (`kernel/vfs.c`) проверяют `of->inode->fs ==
&pipefs_fs` для неблокирующих pipe. Добавляем аналогичную ветку для
`&sockfs_fs`: блокирующий и неблокирующий read/write делегируются
`sock_read_at`/`sock_write_at`.

`vfs_dup_fd` для сокетов — увеличивает количество концов пары (по аналогии
с `pipe_dup`).

### Linux syscall'ы (в `linux_syscall_handler`)

i386 номера:
```
1  socket     2  bind      3  connect    4  listen   5  accept
7  getpeername 8 getsockname 11 sendto    12 recvfrom
14 setsockopt 15 getsockopt 20 sendmsg    21 recvmsg  48 shutdown
364 accept4
```

musl использует `socket()`, `bind()`, `connect()`, `listen()`,
`accept4()`, `sendto()`/`recvfrom()` (как специализации send/recv),
`sendmsg`/`recvmsg`. Для `sendto`/`recvfrom` с `addr==NULL` и
`sendmsg`-с-одним-iov делегируем на read/write. `recvmsg` для D-Bus Unix-сокета
возвращает заполненный `msghdr` с `iov` и флагами.

Добавляем в `linux_syscall_handler` case'ы, вызывающие функции из `socket.c`.

### Структуры адреса (солэ против musl)

musl определяет `struct sockaddr { sa_family_t sa_family; char sa_data[...]; }`.
i386 `sa_family_t` = uint16. Для AF_UNIX `struct sockaddr_un { sa_family_t
sun_family; char sun_path[108]; }`. Те же layout'ы определяем в `socket.h`
для ядра.

## poll() (добавлено позже, syscall 168)

Помимо AF_UNIX в этом блоке реализован `poll(2)` (нужен dbus-daemon для
ожидания событий на дескрипторах):

- `struct aos_pollfd { int fd; short events; short revents; }` (i386, 8 байт)
  в `kernel/vfs.h`; константы `POLLIN/POLLOUT/POLLERR/POLLHUP/POLLNVAL`.
- `vfs_fd_poll(fd, events, *ready)` (`kernel/vfs.c`) диспечерит по типу fs:
  `sockfs_fs` → `sock_poll()` (готов к чтению = данные/EOF/квота listener'а,
  к записи = место в буфере), `pipefs_fs` → `pipe_poll()` (чтение = данные/
  writer ушёл → HUP, запись = место), обычные файлы → по режиму открытия.
  Незакрытый fd → `POLLNVAL` (всегда, независимо от маски events).
- `terminal_key_avail()` (`kernel/terminal.c`) — неразрушающая проверка
  готовности stdin (fd 0).
- `case 168` в `kernel/linux_syscall.c`: перебирает пользовательский массив
  (≤256, валидация через `in_luser`), считает готовые fd; при отсутствии
  готовности блокируется `sti; hlt; cli` до дедлайна `tick + timeout`
  (тик = 1 мс при PIT 1000 Гц), `timeout==0` → опрос без блокировки,
  `timeout<0` → бесконечно.
- Семантика: `POLLNVAL/POLLERR/POLLHUP` отдаются в `revents` всегда, не
  маскируются `events`; возвращается число записей с ненулевым `revents`.
- Тест: `tools/linux/polltest.c` + `scripts/polltest.py` (text-mode, надёжный
  канал serial) — 5 детерминированных кейсов, все проходят (`POLLTEST OK`).

## Что НЕ делаем в этом блоке

- `select()` — dbus использует `poll()`, select не требуется.
- `SO_PEERCRED` полноценный — заглушка (uid текущего task), отдельно для
  EXTERNAL auth в dbus.
- Файловые socket-узлы в SFS (`S_IFSOCK`) — не нужны для abstract.
- Привилегии/creds — один пользователь.
- `UNIX_FD` passing (`SCM_RIGHTS`) — выключается в dbus
  (`max_unix_fds=0`).

## Критерии приёмки

1. `make` собирается с новым `kernel/socket.o`.
2. Внутренний тест (юзерспейс musl `tools/linux/socktest.c`):
   сервер слушает abstract-сокет, клиент подключается, клиент пишет
   сообщение, сервер читает его побайтно, отвечает, клиент читает ответ.
   Всё через реальные Linux syscall'ы (socket/bind/listen/accept/connect/
   read/write/close).
3. В IPC-тесте (сериальный) вывод «SOCKTEST OK».
4. `poll()` (syscall 168): `tools/linux/polltest.c` печатает «POLLTEST OK» —
   stdout/pipe-готовность, POLLNVAL для битого fd, блокировка с таймаутом.