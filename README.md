# AOS

<p align="center">
  <img src="https://img.shields.io/badge/%D0%AF%D0%B7%D1%8B%D0%BA-C-%2300599C?style=flat-square" alt="Язык: C">
  <img src="https://img.shields.io/badge/%D0%90%D1%80%D1%85%D0%B8%D1%82%D0%B5%D0%BA%D1%82%D1%83%D1%80%D0%B0-i386-blue?style=flat-square" alt="Архитектура: i386">
  <img src="https://img.shields.io/badge/%D0%A1%D0%B1%D0%BE%D1%80%D0%BA%D0%B0-make-success?style=flat-square" alt="Сборка: make">
  <img src="https://img.shields.io/badge/%D0%9F%D0%BB%D0%B0%D1%82%D1%84%D0%BE%D1%80%D0%BC%D0%B0-QEMU-lightgrey?style=flat-square" alt="Платформа: QEMU">
  <img src="https://img.shields.io/github/repo-size/vladimka/aos?style=flat-square&label=%D0%A0%D0%B0%D0%B7%D0%BC%D0%B5%D1%80" alt="Размер репозитория">
  <img src="https://img.shields.io/badge/%D0%9B%D0%B8%D1%86%D0%B5%D0%BD%D0%B7%D0%B8%D1%8F-MIT-green?style=flat-square" alt="Лицензия: MIT">
</p>

**AOS** — минималистичное ядро для x86 (i386), написанное с нуля на C и ассемблере: собственная файловая система, командная оболочка, многозадачность, IPC и оконный менеджер с графическим интерфейсом. Загружается через GRUB2 по протоколу Multiboot2 и работает в QEMU.

## Возможности

- **Ядро с нуля** — загрузчик, GDT/IDT, обработчики прерываний, планировщик задач (до 24, с блокирующими `sleep`/`waitpid` и зомби-процессами), системные вызовы через `int 0x80`
- **Защита памяти** — paging, пользовательские процессы в ring 3 через TSS, buddy-аллокатор страниц + slab `kmalloc`, валидация всех указателей в системных вызовах
- **33 syscall AOS-ABI + Linux ABI** — работа с ФС, клавиатурой, мышью, фреймбуфером, задачами и почтовыми ящиками; плюс исполнение статических musl i386-бинарников (`lin/*`) с собственным набором Linux-сисколлов
- **Собственная ФС** — SFS2 в 2-МБ ramdisk и на блочных устройствах (ATA/PCI IDE, SATA AHCI, virtio-blk, RAM), до 256 файлов, VFS-слой с `read_at`/`write_at`/`truncate`/`lseek`, write-back кэш блоков с `sync`/`fsync`, пайпы и перенаправления
- **Графика** — линейный фреймбуфер 1024×768×32, шрифт с кириллицей, UTF-8, программный скроллбек на 512 строк
- **Ввод** — PS/2 клавиатура (US + ЙЦУКЕН), мышь с колесом, русская раскладка
- **Оконный менеджер** — окна, dock с иконками запуска, перетаскивание, контекстное меню и создание файлов, z-order, общие буферы окон (slab)
- **IPC** — почтовые ящики задач, события клавиатуры/мыши, общая память для пиксельных буферов
- **Userland-шелл `bin/sh`** — полноценный шелл в GUI-терминале (`term.c` как VT-эмулятор): PATH, builtins, `$?`, redirects, пайпы, фон, история, Tab-дополнение
- **Эмуляция USB** — сканирование PCI и инициализация контроллера UHCI

## Команды оболочки

```
help  uptime  clear  echo  tick  info  reboot  panic  ls  cat  rm  sh
format  shutdown  test  wm  term  clock  date  ipctest  notepad  many
linrun  sleeptest  exitto  random  fstest  procinfo  bgspawn
cp  mv  mkdir  rmdir  head  wc  setpath  sync
```

Linux-бинарики (musl): `lin/hello`, `lin/ls`, `lin/cat`, `lin/piptest`.

## Сборка и запуск

### Требования

`make install` устанавливает все зависимости сборки и тестов (apt-пакеты
`gcc-multilib`, `binutils`, `grub-pc-bin`, `xorriso`, `mtools`,
`qemu-system-x86`, `python3` и др.) и скачивает обязательный статический
musl i386-toolchain (`tools/musl-i686/`) в каталог `tools/`.

### Установка зависимостей

```sh
make install   # sudo apt-get + загрузка musl i386-toolchain с musl.cc
```

musl-тулчейн — **обязательное** требование: из него собираются все программы
`bin/*` и Linux-бинарики `lin/*`. Без него `make` завершится с ошибкой и
подскажет запустить `make install`. Тулчейн не хранится в git (gitignored),
поэтому устанавливается автоматически.

### Цели make

```sh
make install    # установить зависимости сборки и тестов (sudo apt + тулчейн)
make            # собрать aos.iso (загрузочный GRUB2 ISO)
make run        # запустить в QEMU (GTK-дисплей, virtio-blk/rng/net)
make debug      # headless-запуск: VNC :5907 + QMP и serial Unix-сокеты
make test       # полная регрессия (QEMU-тесты ядра, WM и Linux)
make test-fast  # быстрый набор для CI (ipctest + Linux-тесты)
make clean      # полная очистка
```

`make` идемпотентен — повторная сборка ничего не пересобирает без изменений.

### Выбор режима при загрузке

GRUB показывает меню (таймаут 60 с):

- **AOS** — графический режим 1024×768×32 (по умолчанию);
- **AOS (text)** — классический VGA-текст 80×25 без framebuffer: остаётся текстовая консоль на VGA + serial. Оконный менеджер не запускается, драйвер virtio-gpu не инициализируется (даже если устройство есть — например, в `make run`/`make debug`), так что GUI-приложения недоступны, а VGA-текст не перекрывается GPU-scanout'ом.

### Запуск в QEMU

```sh
make run
```

Открывается GTK-окно с AOS. После загрузки виден рабочий стол с dock (иконки `term`, `clock`); курсор мыши захватывается при наведении (`grab-on-hover`). В serial-консоль (`-serial stdio` конфликтует с монитором — используйте `-serial file:serial.log` при отладке) выводится журнал ядра и доступен ядерный шелл.

### Отладка

```sh
make debug
```

Boot в headless-режиме с VNC-сервером (`:5907`), QMP-монитором (`/tmp/aos-debug.qmp`) и serial (`/tmp/aos-debug.serial`). Подключение через qemu-vnc MCP-инструменты.

### Тесты

```sh
make test        # весь регресс (ядерный шелл, WM, notepad, пайпы, Linux)
make test-fast   # быстрый CI-набор
```

Каждый тест — `scripts/*.py`, бутующий `aos.iso` в QEMU и проверяющий serial-лог и/или скриншоты. GUI-тесты чувствительны к таймингам TCG — гоняйте их через `make test`/`make test-fast`, а не вручную.

## Структура репозитория

```
kernel/    — ядро: прерывания, syscall'ы, планировщик, SFS/VFS, терминал, ELF-загрузчик
drivers/   — VGA/фреймбуфер, последовательный порт, PS/2 мышь, PCI, UHCI
programs/  — пользовательские приложения (shell-команды, bin/sh, term, clock, wm)
arch/i386/ — GDT, IDT
boot/      — загрузчик Multiboot2, обработчики прерываний
tools/     — статический musl i386-toolchain и исходники Linux-бинарников (tools/linux/)
scripts/   — генерация встроенных программ, тесты (qtest.py — общий QEMU-каркас)
```

## Тестирование

Каждый тест — `scripts/*.py`, бутующий `aos.iso` в QEMU. Общий каркас `scripts/qtest.py` (класс `QTest`): запуск QEMU, HMP-монитор, абсолютная мышь, PPM-скриншоты и проверка пикселей. Полный регресс — `make test` (ядерный шелл, WM, notepad, пайпы, Linux-бинарики); быстрый CI-набор — `make test-fast`. GUI-тесты (notepadtest, termtest и др.) чувствительны к таймингам TCG — гоняйте их через harness, а не вручную.
