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

- **Ядро с нуля** — загрузчик, GDT/IDT, обработчики прерываний, планировщик задач (кооперативный, до 6 процессов), системные вызовы через `int 0x80`
- **Защита памяти** — paging, пользовательские процессы в ring 3 через TSS, валидация всех указателей в системных вызовах
- **30 системных вызовов** — работа с ФС, клавиатурой, мышью, фреймбуфером, задачами и почтовыми ящиками
- **Собственная ФС** — SFS (Simple File System) в 64-КБ ramdisk, до 64 файлов
- **Графика** — линейный фреймбуфер 1024×768×32, шрифт с кириллицей, UTF-8, программный скроллбек на 512 строк
- **Ввод** — PS/2 клавиатура (US + ЙЦУКЕН), мышь с колесом, русская раскладка
- **Оконный менеджер** — окна, dock с иконками запуска, перетаскивание, кнопка закрытия, z-order, общие буферы окон (1 МБ slab на окно)
- **IPC** — почтовые ящики задач, события клавиатуры/мыши, общая память для пиксельных буферов
- **Эмуляция USB** — сканирование PCI и инициализация контроллера UHCI

## Команды оболочки

```
help  uptime  clear  echo  tick  info  reboot  panic  ls  cat  rm
shutdown  format  test  wm  term  clock  setpath
```

## Сборка и запуск

Требования: `gcc`, `ld`, `make`, `python3`, `grub-mkrescue`, `qemu-system-i386`.

```sh
make        # собирает aos.iso (загрузочный GRUB2 ISO)
make run    # запускает в QEMU
make clean  # полная очистка
```

## Структура репозитория

```
kernel/    — ядро: прерывания, syscall'ы, планировщик, SFS, терминал, ELF-загрузчик
drivers/   — VGA/фреймбуфер, последовательный порт, PS/2 мышь, PCI, UHCI
programs/  — пользовательские приложения (shell-команды, term, clock, wm)
arch/i386/ — GDT, IDT
boot/      — загрузчик Multiboot2, обработчики прерываний
scripts/   — генерация встроенных программ, guitester.py (проверка пикселей в QEMU)
```

## Тестирование

`scripts/guitester.py` запускает QEMU с монитором, делает скриншоты и проверяет пиксели — используется для регрессионной проверки оконного менеджера.
