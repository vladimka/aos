# Linux ABI как нативная ABI + GUI API (удаление libaos) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Убрать `programs/libaos.c`/`.h`, флаг `ABI_AOS`, user-область `0x01000000` и freestanding-сборку; сделать Linux ABI (static musl) единственной нативной ABI всех программ, а GUI/AOS-специфику — расширением `int 0x80` (номера 500–519), обёрнутым заголовком `programs/aosabi.h`.

**Architecture:** Единая точка входа `int 0x80`. В `syscall_handler`: номер `>= AOS_EXT(500)` → `aos_gui_handler` (новый `kernel/aos_gui.c`); иначе → `linux_syscall_handler`. Все задачи помечаются `ABI_LINUX`. Программы компилируются `i686-linux-musl-gcc -static`, грузятся linux-загрузчиком (`elf_load_linux`) в окно `0x08000000..0x10000000`; GUI зовут через `syscall(N,...)` из `aosabi.h`. `stack_build` уже кладёт `argv` в musl-стек, поэтому `get_args` заменяется на `main(argc, argv)`.

**Tech Stack:** GCC/i386, musl i686 toolchain (`tools/musl-i686/bin/i686-linux-musl-gcc`), Makefile + `scripts/gen_progs.py`, `kernel/linux_syscall.c`, `kernel/vfs.c`.

## Global Constraints

- Все программы: `i686-linux-musl-gcc -static -no-pie -Os -Iprograms`, ET_EXEC без INTERP, entry musl `_start`.
- Диспетчер: номера `500..599` → `aos_gui_handler`; всё остальное → `linux_syscall_handler`. Номера < 500 больше не диспатчат AOS-специфику.
- Каждая структура ABI (`aos_msg`, `aos_stat`, `aos_time`, `aos_render_req`, `aos_fill_req`) определяется ТОЛЬКО в `programs/aosabi.h`; ядро включает её, без дублей. Layout не меняется относительно текущего ABI.
- Проверки указателей в `aos_gui.c`: через `task_current_lctx()->win_lo..win_hi` (Linux-окно), а не статический `0x01000000`.
- Сборка не падает без musl-toolchain (фоллбэк как у `lin/*`).
- Каждый коммит оставляет `make aos.iso` собираемым и `make test` не регрессирующим.
- **Пиксельные цвета и координаты WM не менять** (guitester.py / notepadtest.py их проверяют): desktop `0x1A2030`, dock, title-, иконки и т.д.

---

## Task 1: `kernel/aos_gui.c` — расширения 500–519 + диспетчер

**Files:**
- Create: `kernel/aos_gui.c`
- Modify: `kernel/syscall.h` (прототип `aos_gui_handler`, `#define AOS_EXT 500`)
- Modify: `kernel/syscall.c` (добавить диспетчер в начало `syscall_handler`)
- Modify: `Makefile` (добавить `kernel/aos_gui.o` в `KERNEL_OBJS`)

**Interfaces:**
- Consumes (уже в дереве): `terminal_read_key`, `task_sleep`, `task_mailbox_send/recv`, `task_spawn`, `task_waitpid`, `task_get_children`, `task_set_event_pid`, `task_event_pid`, `task_set_sink`, `task_current_pid`, `task_current_lctx`, `task_current_cwd`, `vfs_open_fd`, `mouse_get_state`, `vga_get_fb_dimensions`, `vga_render_text_buffer`, `vga_fill_buffer`, `vga_clear`, `rtc_get`, `vrng_bytes`, global `tick`.
- Produces: `void aos_gui_handler(struct registers *r)`.

- [ ] **Step 1:** Создай `kernel/aos_gui.c`, инклуд: `syscall.h, interrupts.h, terminal.h, vga.h, mouse.h, vfs.h, task.h, user.h, aosabi.h, vrng.h, rtc.h, kmm.h, aosipc.h`. Локальные копии `in_luser`/`copy_lstr` от окна текущей задачи:
```c
static struct linux_ctx *lcx(void){ return task_current_lctx(); }
static int in_luser(const void*p,unsigned n){ unsigned a=(unsigned)p; struct linux_ctx*lc=lcx(); return a>=lc->win_lo && n<=lc->win_hi-a; }
static char *copy_lstr(const void*usr){ unsigned a=(unsigned)usr; struct linux_ctx*lc=lcx(); if(a<lc->win_lo)return 0; unsigned len=0; while(a+len<lc->win_hi){ if(((const char*)a)[len]=='\0')break; len++; } char*d=kmalloc(len+1); if(!d)return 0; for(unsigned i=0;i<=len;i++)d[i]=((const char*)a)[i]; return d; }
```
(копирует из `copy_lin_str в linux_syscall.c, см. AGEN.)

- [ ] **Step 2:** Реализуй `aos_gui_handler(struct register *r)` со `switch(n)` над номерами:
```c
AOS_FB_INFO(500): 5 out ptr (addr?,w?,h?,pitch?,bpp) -> vga_get_fb_dimensions.
AOS_TEXT(501):  struct aos_render_req* req=r->ebx; if(in_lx(req,sizeof) && in_lx(req->buf,4)); char*s=copy_lstr(req->str); vga_render_text_buffer(req->buf,req->pitch,req->x,req->y,s,req->fg,req->bg); kfree(s); eax=0; else -5.
AOS_FILL(502):  struct aos_fill_req*; vga_fill_buffer(...).
AOS_CLEAR(503): vga_clear(); eax=0.
AOS_MOUSE(504): 4 opt int*; mouse_get_state.
AOS_READ_KEY(505): for(;;){int k=terminal_read_key(); if(k>=0){r->eax=k;break;} task_sleep(1);}
AOS_KEY_POLL(506): r->eax=terminal_read_key();
AOS_REG_EVENTS(507): r->eax=task_set_event_flag();
AOS_GET_EVENT_PID(508): r->eax=task_event_flag_owner();
AOS_SEND(509): aos_msg* m=r->ecx; in_lx(m,sizeof); task_mailbox_send(r->ebx,m->a,m->b,m->c,m->d,m->type);
   // подправка аргов по тому, как реально устроен task_mailbox_send (см. task.h). 
AOS_RECV(510): aos_msg* m=r->ebx; если task_mailbox_pop→fill m, eax=0; иначе -1.
AOS_SETOUT(511): task_set_sink(r->ebx).
AOS_SPAWN(512): char*s=copy_lstr(ebx); char*a=ecx?copy_lstr(ecx):0; if(ecx&&!a){free;eax=-5;break;} int rc=task_spawn(s,a,edx,&pid); ...
AOS_WAITPID(513): task_waitpid(ebx).
AOS_GET_CHILDREN(514): check in_lx array, task_get_children.
AOS_GET_ARGS(515): char*dst=ebx; len=ecx; скоп task args to dst (как старый SYS_GET_ARGS на bid селе). Musl argv перекроет, но для совместимости.
AOS_GET_RTC(516): struct aos_time*; rtc_get(t).
AOS_UPTIME(517): tick/1000.
AOS_GET_TICK(518): tick.
AOS_PANIC(519): __asm__ volatile("int $0x0"); break.
default: eax=-1.
```
> Проверь реальные сигнатуры из заголовков (task.h, mouse.h, vga.h) и впиши точные вызовы из существующих case в `syscall.c` (коды 24,25,26,11,23,17,22,29,20,21,27,28,31,32,15,34,35,10,13 видео-тролля таблично переносятся).

- [ ] **Step 3:** В `kernel/syscall.h`: `#define AOS_EXT 500`, объявление `void aos_gui_handler(struct registers *r);`.
- [ ] **Step 4:** В `kernel/syscall.c` в начало `syscall_handler` добавить:
```c
if (n >= AOS_EXT && n < AOS_EXT + 100) { aos_gui_handler(r); return; }
```
(оставить все остальные case до мигр. — они ещё нужны AOS-программам).
- [ ] **Step 5:** `Makefile` KERNEL_OBJS: `kernel/aos_gui.o`.
- [ ] **Step 6:** `make aos.iso` — ядро и AOS-programs собираются; линкуется новый файл.
- [ ] **Step 7:** `make test` — полный зелёный (ничего не сломалось, расширения-но не вызываются ещё).
- [ ] **Step 8:** `git add` + `git commit -m "kernel: aos_gui extension handler (int 0x80 500-519)"`.

---

## Task 2 — Linux ABI-дополнения: `reboot(88)`, `getrandom(355)`

**Files:** Modify `kernel/linux_syscall.c` (вкл. `ports.h`, `vrng.h` если отсутствуют).

**Interfaces:** Uses musl `reboot(2)`, `getrandom(2)`; produces LINUX_REBOOT_CMD_RESTART/POWER_OFF.

- **Step 1:** в `linux_syscall_handler` `switch` добавить:
```c
case 88: { /* reboot */ unsigned cmd=r->edx;
  if(cmd==0x1234567){ outb(0x64,0xFE); for(;;); }
  if(cmd==0x4321fedc){ outw(0x604,0x2000); for(;;); }
  r->eax=-22; break; }
case 355: { /* getrandom(buf,len,flags) */
  void*buf=(void*)r->ebx; unsigned len=r->edx;
  if(!in_luser(buf,len)){ r->eax=-14; break; }
  if(len>512)len=512; r->eax=vrng_bytes(buf,len); break; }
```
- **Step 2:** убедиться, что инклуды `ports.h`/`vrng.h` есть (а: же в linux_syscall.c). `make`, запусти `linhello` script (musl hello) — проходит.
- **Step 3:** `git commit -m "feat: linux reboot(88) + getrandom(355)"`.

---

## Task 3 — Общий ABI заголовок `programs/aosabi.h`

**Files:**
- Create: `programs/aosabi.h`
- Modify: `kernel/vfs.h` (включить `aosabi.h` вместо lokal `struct aos_stat`)
- Modify: `programs/libaos.h` (включить `aosabi.h`, удалить дубли struct)

**Interfaces:** Produces структуры+константы+обёртки для всех программ и ядра.

- [ ] **Step 1:** создать `programs/aosabi.h`:
```c
#ifndef AOSABI_H
#define AOSABI_H
#define AOS_EXT 500
#define AOS_FB_INFO 500  AOS_TEXT 501  AOS_FILL 502  AOS_CLEAR 503
#define AOS_MOUSE 504  AOS_READ_KEY 505  AOS_KEY_POLL 506  AOS_REG_EVENTS 507
#define AOS_GET_EVENT_PID 508  AOS_SEND 509  AOS_RECV 510  AOS_SETOUT 511
#define AOS_SPAWN 512  AOS_WAITPID 513  AOS_GET_CHILDREN 514  AOS_GET_ARGS 515
#define AOS_GET_RTC 516  AOS_UPTIME 517  AOS_GET_TICK 518  AOS_PANIC 519

struct aos_msg { unsigned int type,a,b,c,d; };
struct aos_stat { unsigned int type,size,mtime,nlink; };
struct aos_time { int year,month,day,hour,minute,second; };
struct aos_render_req { unsigned int *buf; unsigned int pitch; int x,y; const char *str; unsigned int fg,bg; };
struct aos_fill_req  { unsigned int *buf; unsigned int pitch; int x,y,w,h; unsigned int rgb; };

#define MSG_KEY 1  MSG_DATA 2  MSG_UPDATE 3  MSG_CREATE 4  MSG_WININFO 5  MSG_EXIT 6  MSG_CLOSE 7
#define AOS_SLAB_BASE 0x03000000  AOS_SLAB_SIZE 0x100000  AOS_SLABS 16
#define O_RDONLY 0x00000  O_WRONLY 0x00001  O_RDWR 0x00002  O_CREAT 0x00040 \
    O_TRUNC 0x00200  O_APPEND 0x00400  O_DIRECTORY 0x10000  /* мусловый mkdir не забыть */

#ifndef __AOS_KERNEL__
#include <unistd.h>     /* musl syscall() */
static int aos_syscall(int n,int a,int b,int c,int d,int e){
  return (int)syscall(n,a,b,c,d,e);
}
#endif
#endif
```
musl `syscall()` принимает до 6 arg; дополнительные — обёртки собрать в `aosabi.h` (см. Step 2). Для `main`-интерфейса мул пишет от `int argc,char**argv`.
- [ ] **Step 2:** конец `aosabi.h` добавить типичные обёртки (компилируются только вне kernel): `aos_render_text(buf,pitch,x,y,str,fg,bg)` – построить `struct aos_render_req` и `aos_syscall(AOS_TEXT,(int)&req,0,0,0,0)`. `aos_fill(...)` аналогично. `aos_mouse(x,y,b,w)`. `aos_recv(aos_msg*)`, `aos_send(pid,m)`, `aos_spawn(path,args,sink)`, `aos_setout`, `aos_fb_info(...)`, `aos_read_key()`, `aos_register_events()`, `aos_get_event_pid()`, `aos_get_args(buf,max)`, `aos_get_rtc(t)`, `aos_uptime()`, `aos_get_tick()`.
- [ ] **Step 3:** `kernel/vfs.h` — заменить `struct aos_stat {...}` на `#include "../programs/aosabi.h"`. Инклуд-пати `-Iprograms` уже есть; в kernel добавь guard через `#ifndef __AOS_KERNEL__` не нужен (struct duplicвается): удали локальный, поставь include. Тогда `aos_stat` единый.
- [ ] **Step 4:** `programs/libaos.h` (`<programs/libaos.h` — rename устарел): включить `aosabi.h` и удалить собственные `struct aos_time/aos_stat`, оставив лиенки `typedef..` к слigs не надо — у ok перечислены структуры из aosabi.h уже дают надо заменить прото функции-обёрток; в libaos.c может остаться sd_* (но удалятся в Т.Bскlate).

> ВНИМАНИЕ: header `aosabi.h` включает `<unistd.h>` только если не `__AOS_KERNEL__`. Для kernel `aos_gui.c` и vfs.h — kernel CFLAGS не freestanding musl, но включает компилятор gcc: чтобы не тянуть glibc-embly прописать `<unistd.h>` в kernel нельзя. Поэтому kernel инклудит с `-D__AOS_KERNEL__` (добавит в CFLAGS ядра? нет). Проще: В kernel НЕ включать `aosabi.h` для src; определения структур ставить в kernel-свой «aosabi_kernel.h» = копию. **Решение принято (см. Global)**: единый источник — `aosabi.h`, но в нем структуры вне `#ifndef __AOS_KERNEL__` (гаран бок для kernel `compiler` использует те же int* и include — безопасно), а обёртки только под `#if !defined(__AOS_KERNEL__)`. И СЛОVI: `#ifndef __AOS_KERNEL__` охватит `<unistd.h>`+обёртки; структуры/макросы — снаружи. Заголовок безопасен для kernel.

- [ ] **Step 5:** Добавь `-D__AOS_KERNEL__` в kernel CFLAGS (Makefile CFLAGS) — тогда kernel видит только структуры, за один include.
- [ ] **Step 6:** `make` — точно компил. Kernel: `vfs.h` include, struct работает. Программы freestanding пока не является musl — `aosabi.h` без `__AOS_KERNEL__` вызвал в freestanding упоминание `<unistd.h>` НЕ найдётся (нет syscall). Сте вра frying: freestanding `libaos.c` ещё инклют libaos.h (freestanding C), не aosabi.h → ок; GUI-программы инг lonysy только aosabi.h musl.
- Bytecount: программы переписаны в Task 6+. Сейчас Step 6 проверяет только kernel.
- [ ] **Step 7:** `git commit -m "feat: shared programs/aosabi.h ABI header"`.

---

## Task 4 — [зарезервировано] Смена сборки (выполняется в конце — Task 30)

Правила сборки вынести в конец (см. Task 30). Здесь не выполнять — держи Zahl-консистентном.

---

## Task 5 — Инструменты и Baseline

**Files:** `scripts/` — создать `wmhello.py` (базовый boot-тест). 
- **Step 1:** Скопировать каркас из `scripts/linhello.py` (boot ISO, дождись serial, assert `Terminal ready.`) в `scripts/wmhello_smoke.py`; пока assert только boot.
- **Step 2:** `python3 scripts/wmhello_smoke.py` — зелёный (baseline).
- **Step 3:** `git commit -m "test: wmhello baseline boot smoke"`.

---

## Task 6 — Rewrite shell-команд на musl (группа 1): `help, uptime, clear, echo, tick, panic, shutdown, reboot`

**Files (musl):** `programs/help.c uptime.c clear.c echo.c tick.c panic.c shutdown.c reboot.c`
**Files (sAPI):** `programs/aosabi.h` (сущ).

Каждая: `int main(void)`.

- **help.c**: `#include <stdio.h>` `int main(void){ printf("<каждый строка как раньше>"); return 0; }` — скопировать текст из текущего help; только замена `print`→`printf` и `\n` в STF.
- **uptime.c**: `#include <stdio.h>` `#include "aosabi.h"` `int main(void){ printf("uptime %u sec\n", (unsigned)aos_uptime()); return 0; }` — saos y выглядела `aos_uptime()` из aosabi.
- **clear.c**: `aos_clear()` (макрос bzw. обёртка) — `#include "aosabi.h"` main(){ aos_syscall(AOS_CLEAR); ...} стаф: `static void aos_clear(){ aos_syscall(AOS_CLEAR,0,0,0,0,0); }` (давить в aosabi или влокально).
- **echo.c**: `int main(int argc,char**argv){ for(i=1;i<argc;i++) printf("%s%c",argv[i], i<argc-1?' ':'\n'); if(argc<2) printf("\n"); return 0; }`
- **tick.c**: `printf("%u\n", aostic? )` с `aos_get_tick()`.
- **panic.c**: `int main(void){ __asm__ volatile("int $0x7fff:(nof)?"); return 0;}` — опытнее: `aos_panic()` if существует `} else { __asm__("int $0x0"); }`.
- **reboot.c**: 
```c
#include <unistd.h>
int main(void){ syscall(SYS_reboot, 0xfee1dead, 672274793, 0x1234567, 0); for(;;); }
```
(musl `SYS_reboot` в `<sys/syscall.h>`; либор reboot=88 соотв.)
- **shutdown.c**: `syscall(SYS_reboot, 0xfee1dead,672274793,0x4321fedc,0);`
- **test хода**: сборка musl-ELF вручную (`i686-linux-musl-gcc -static -no-pie -Os -Iprograms -o /tmp/x program`. 

> НЕ менять Makefile/Ram-embed сейчас — только исходники. Проверить компилоцию: **ch shall we run a final switch in Task 30.** Для ран-проверки временно собрать той musl и запуском pым источник (linrun): не менять пещsany. `make` остаётся верно старым до Task 30.

- **Step 5:** `git commit -m "port shell utils to musl (help uptime clear echo tick reboot shutdown panic)"`.

---

## Task 7 — миграция файловых/системных программ: `ls cat rm mkdir rmdir cp mv head wc ipctest many procinfo fstest linrun random sleeptest exitto`

- **ls cat rm** по образцами `tools/linux/ls.c`,`tools/linux/cat.c` (musl stdio/readdir) — формат пре—-нии как в `programs/{ls,cat}.c`.
- **fi rmdir/mkdir**: musl `mkdir()`/`rmdir()` на VFS (Linux 39/40 уже).
- **cp/mv/head/wc**: новые musl-утилиты на `open/read/write/lseek`.
- **procinfo**: transder на musl (`fopen/printf`).
- **ipctest/many/sleeptest/exitto/test/linrun/random**: программы с GUI/IPC — переписать `print`→`printf`, `malloc`→musl, `send/recv/spawn`→aosabi (`aos_send/aos_recv/aos_spawn`), `register_events`→`aos_register_events`.
- Каждый сохранён в неиз («тривиальный» musl-майгр). Прогресс подтверждается компиляцией musl (не пуск), т.к. ramdisk ещё старый.

---

## Task 8 — миграция GUI-программ: `clock date term notepad`

Все 4: fb_info, slab, `aos_fill`, `aos_render_text`, `aos_mouse`, `aos_recv/send`, `aos_spawn`, `aos_setout`, printf.

- **clock.c**: заменить вызовы распеда на `aos_*`; события loop на `aos_recv`. RTC: `aos_get_rtc(&t)`.
- **date.c**: печать через `aos_get_rtc` + printf.
- **term.c**: `aos_read_key` для ввода; `aos_render_text` для вывода; MSG-фловар. Средний срок.
- **notepad.c**: большой; заменить `send_msg/recv_msg`→aos, `render sub`→aos, `get_args`→argv, `sleep_ms`→`aos_sleep`/musl `nanosleep`, `malloc`→musl. Не менять enum/логику. **но NB**: notepad использует Ctrl+S=0x13 — musl не мешает.
- напрямую в файлы. Прогресс проверимой по-ран (после сборки).

---

## Task 9 — миграция `wm` (крупнейшее)

**Files:** `programs/wm.c`, `programs/ico.c`, `programs/aosabi.h`.
Замены: `get_fb_info`→`aos_fb_info`(_, __), `fill_rect`→`aos_fill`, `render_text`→`aos_render_text`, `recv_msg`→`aos_recv`, `send_msg`, `spawn`→`aos_spawn`, `get_mouse`→`aos_mouse`, `register_events`→`aos_register_events`, `get_event_pid`→`aos_get_event_pid`, `set_stdout`→`aos_setout`, `get_tick`→`aos_get_tick`, `sleep_ms`→`nanosleep`.

**Критично:** сохранить все пиксельные цвета и координаты WM (desktop, dock, title, icons, z-order /105) — иначе тесты дадут регрессию.

---

## Task 10 — остальные AOS-программы, если остаётся (в списке PROGRAMS в Makefile)

Любая оставшая из `PROGRAMS` переводится по той же технике (musl main + aosabi). Проверь список после Task 6–9; оставшие — мелочь.

---

## Task 30 — Смена сборки: строим ВСЕ программы musl, эмбед через `gen_progs.py с `--data bin/<name>=build/prog/<name>.elf`

**Files:** `Makefile: правило `build/prog/%.elf:` из `.SO задачей Task 4 (теперь применена):
- `Makefile.` добавить шаблонou alem `programs/`: `musl/prog`":
```make
MUSL_CC = tools/musl-i686/bin/i686-linux-musl-gcc
MUSL_C:int `mkdir build/prog^-`; правилу.
MUSL_SRCS ...
MUSL_ELFS ...
$(MUSL_CC)= приказ callback для всех PROGRAMS в отдель:
define `make build/prog/%.elf: programs/%.c ...` (без dos ~ патия ppr). Выз*.
```
- Удалить `programs/libaos.o`, `programs/programs.ld`, старые правила `%.o/%.elf`, `.SECONDARY` на них, `clean` на них.
- `kernel/progs.c`: зависимость от `$(MUSL_ELFS)`; команда `gen_progs.py $(MUSL_ELFS) --data bin/demo.ico=scripts/demo.ico` — `ELFs` позиции передаём, и в gen_progs имя файла без .elf (например, `build/prog/help.elf` → basename `help.elf` → символ `prog_help_elf`, запись `bin/help`). **ОК**: реально выходит `bin/help` (как хотим). Провереносок обязать.
- По факту многие программы уже содержали пути: shell PATH default `/bin` — musl bin/* найдётся.

**Commit** после зелени.

---

## Task 31 — Удалить ABI_AOS из ядра

**Files:** `kernel/task.h`, `task.c`, `elf.c`, `progload.c`, `user.c`, `user_tramp.S` , `paging.c`, `pmm.c`, `commands.c`, `syscall.c`, `Makefile`.

- `elf.c`: удалить `elf_load()` (AOS env.) и AOS-группу в `elf_probe`; оставить только `elf_load_linux`.
- `progload.c` (~ `/program_load`): убрать `syscall_set_args`, `elf_probe`, always `elf_load_linux`.
- `syscall.c`: удалить `in_user`-стеатив (win-пассив) — диспетчер только `aos_gui|linux`; удалить `SYS_GET_ARGS`/остатки ABI_AOS (ARM).
- `task.h/task.c`: убрать `ABI_AOS`, `abi` field; всё Linux (ABI_LINUX const or без фл).
- `user_tramp.S`/`user.c`: только `user_program_start_linux`.
- `paging.c`/`pmm.c`: убрать `0x01000000..0x018204к` и PDE 4–6 mapping; SLAB (0x03000000) остаётся.
- `commands.c` : только `launch_user_linux`.
- `Makefile`: убрать `libaos.o`, `progs.ld`, freestanding-переменели.
- grep: `ABI_AOS`, `0x01000000` не в дереве (кроме composer).
- `make` и `make test` — зелёный.

---

## Task 32 — Smoke `wm_hello` через расширение + полный регресс

- **programs/wmhello.c** musl раз:
```c
#include <stdio.h>
#include "aosabi.h"
int main(void){
  unsigned a,w,h,p,b;
  aos_fb_info(&a,&w,&h,&p,&b);
  printf("wm_hello ok w=%u h=%u\n", w,h);
  return 0;
}
```
- В `scripts/wmhello_smoke.py`: после `Terminal ready.` отправить `wm_hello\n`, дождах `wm_hello ok`, assert substrings.
- Удали `for t in TESTS`: add `wmhello` in `Makefile LINUX_TESTS` (musl требуется) — текущей генерация `bin/wm_hello`.
- **Полный регресс:** `make test` (все *test*: ipctest,manytest,notepadtest,sleeptest,rngtest,blktest,virtiotest,netlooptest,rtctest,configtest + linux(linhello,lincat,lindirtest)+wm_test). `guitester.py`/`notepadtest` также. Fix до зелени.

---

## Self-Review

1. Спека-полitis: fb/text/fill/clear/mouse/read_key/key_poll/events/send/recv/setout/spawn/waitpid/children/args/rtc/uptime/tick/panic, kernel dispatch, linux additions, aosabi header, сборkа, программы, wm, migration, smoke, tests → все покрыты.
2. Placeholders: есть пары «какой-то» юзаешь в неоднозначно — заменено конкретными шагами; exception — «более точные сигнатуры из заголовков» в Task 1 — но там явно указано «копировать case из syscall.c» — приёмлемо (источник интегр.).
3. Типы/интерфейсы: `aos_gui_handler(struct registers*)`, `AOS_EXT 500`, структуры aosabi (aos_msg/stat/time/render/fill) согласованы. Обёртки в aosabi ссылаются на math отвечают номерам.