# Спецификация: ATA (PIO) драйвер

Дата: 2026-08-15
Статус: утверждён (дизайн согласован в чате)

## Цель

Добавить в ядро AOS драйвер классических PATA/IDE дисков (PIO mode, 28-bit LBA)
как ещё один бэкенд блочного слоя (`struct sdev`). Когда ATA-диск присутствует,
он становится корневым устройством SFS2 (приоритет ATA → virtio → ramdisk).

Это шаг 1: корректный PIO-драйвер с опросом. IRQ-ориентированный PIO и DMA
(UDMA/AHCI) осознанно отложены; API драйвера (`ata_read`/`ata_write`) остаётся
чистым, чтобы позже заменить опрос на ожидание по IRQ без переделки блочного
слоя.

## Контекст

- Блочный слой (`kernel/block.c`): единый `struct sdev` (read/write/present/
  capacity_sectors) + кэш 128 секторов. Бэкенд выбирается в `block_init()`,
  вызываемом из `virtio_init()` (`drivers/virtio.c:187`).
- SFS2 (`kernel/sfs2.c`) монтируется поверх текущего `sdev` в `vfs_init()` →
  `sfs2_init()`; корневая ФС живёт на выбранном диске.
- В `kernel_main` (`kernel/kernel.c`) `interrupts_init()` идёт до
  `virtio_init()`, так что `tick` (PIT 1000 Гц) доступен для таймаутов опроса.
- Уже есть инфраструктура: `ports.h` (inb/outb/inw/inl), PCI-скан
  (`pci_find_all`), таймауты по `tick` (паттерн `vblk.c`), тест-харнесс
  `scripts/qtest.py`.
- QEMU 10.2.1: `-drive file=...,format=raw,if=ide` создаёт `piix3-ide`
  (PCI 01.1) с `ide-hd` на primary master. Подтверждено пробой: legacy порты
  0x1F0/0x170, IRQ14/15.

## Регистры ATA (legacy)

| Назначение            | Primary | Secondary | Ширина |
|-----------------------|---------|-----------|--------|
| Data                  | 0x1F0   | 0x170     | 16 бит |
| Error / Features      | 0x1F1   | 0x171     | 8 бит  |
| Sector Count          | 0x1F2   | 0x172     | 8 бит  |
| LBA Low               | 0x1F3   | 0x173     | 8 бит  |
| LBA Mid               | 0x1F4   | 0x174     | 8 бит  |
| LBA High              | 0x1F5   | 0x175     | 8 бит  |
| Drive/Head            | 0x1F6   | 0x176     | 8 бит  |
| Status / Command      | 0x1F7   | 0x177     | 8 бит  |
| Alt Status / Control  | 0x3F6   | 0x376     | 8 бит  |

Команды: `IDENTIFY` = 0xEC, `READ SECTORS` = 0x20, `WRITE SECTORS` = 0x30.

Бит статуса: BSY = 7, DRQ = 3, ERR = 0.
Drive/Head: `0xE0 | (slave << 4) | ((lba >> 24) & 0x0F)` (LBA mode, 28-bit).

## Компоненты

### 1. Драйвер — `drivers/ata.c` + `drivers/ata.h`

Структуры:

```c
#define ATA_PRIMARY_BASE   0x1F0
#define ATA_PRIMARY_CTRL   0x3F6
#define ATA_SECONDARY_BASE 0x170
#define ATA_SECONDARY_CTRL 0x376

// Один выбранный диск (первый найденный среди 4 устройств) — единое состояние
// для ata_read/ata_write. base/ctrl фиксируются при детекции.
struct ata_dev {
    unsigned int base;              // data/regs base (0x1F0 или 0x170)
    unsigned int ctrl;              // alt status / device control (0x3F6/0x376)
    int slave;                      // master=0 / slave=1 на выбранном канале
    unsigned int capacity_sectors;  // 28-bit LBA
    int present;
};
```

API:

- `void ata_init(void)` — детекция всех 4 устройств (2 канала × master/slave),
  выбор первого найденного диска, selftest чтение/запись последнего сектора.
- `int ata_read(unsigned int lba, void *buf)` — чтение 1 сектора (512 Б).
- `int ata_write(unsigned int lba, const void *buf)` — запись 1 сектора.
- `int ata_present(void)` — есть ли найденный диск.
- `unsigned int ata_capacity_bytes(void)`.

Детекция (в `ata_init`):
1. Сброс канала: `outb(ctrl, 0x04)` (SRST), пауза ≥ 2 мкс (busy-loop через
   чтение `inb(ctrl)`), `outb(ctrl, 0x00)`, ожидание готовности по `tick`.
2. Для каждого из master/slave: выбрать устройство (`outb(base+6, 0xE0|(slave<<4))`),
   пауза, послать `IDENTIFY` (0xEC), опросить статус.
   - Ошибка при выборе (ERR в статусе сразу после select) → устройства нет.
   - BSY не снимается в течение таймаута → устройства нет.
3. Прочитать 256 слов идентификационных данных в буфер `ata_ident[256]`.
4. Отсев ATAPI: если word0 == 0xEB14 (ATAPI device) — пропустить (не ATA-диск).
5. Ёмкость: word60 | (word61 << 16) — 28-bit LBA в секторах; 0 → устройство
   некорректно, пропустить.
6. Лог: `ata: found primary/master, capacity=<MiB> MiB`.

I/O (опрос, таймаут по `tick`):
- `ata_ready()`: ждать снятия BSY; если ERR или таймаут (> 2 с) → ошибка.
- `ata_wait_drq()`: после `ata_ready()` ждать DRQ.
- `ata_read`: `outb(base+2, 1)` (count), LBA младшие байты, drive/head,
  `outb(base+7, 0x20)`, `ata_wait_drq()`, 256 × `inw(base)` в `buf`.
- `ata_write`: как read, но `outb(base+7, 0x30)`, `ata_wait_drq()`, 256 ×
  `outw(base, ...)`, затем `ata_ready()` до конца записи.
- Возврат: 0 при успехе, -1 при таймауте, -2 при ERR (соответствует
  соглашению sdev).

Selftest (в `ata_init`, стиль `vblk.c`):
- Заполнить буфер паттерном `(i*7+3)`, записать в последний сектор, прочитать
  обратно, сравнить. Лог `ata: selftest OK` / `ata: selftest FAIL`.

Таймауты: IDENTIFY — 5 с (по `tick`), операции I/O — 2 с.

### 2. Интеграция в блочный слой — `kernel/block.c`

- Добавить `static struct sdev sdev_ata;` с обёртками `sdev_ata_read/write`
  на `ata_read`/`ata_write`.
- В `block_init()` порядок: если `ata_present()` → `sdev_ata` (лог
  `block: ata backend, %u sectors`); иначе если `vblk_present()` → `sdev_vblk`;
  иначе ramdisk.
- `block_disk_present()`: вернуть 1, если `dev == &sdev_ata || dev == &sdev_vblk`.

### 3. Инициализация — `kernel/kernel.c`

- Вызвать `ata_init()` перед `virtio_init()` (после `interrupts_init()`,
  таймауты работают по `tick`). `block_init()` (в конце `virtio_init()`) увидит
  ATA-диск первым.

### 4. Сборка — `Makefile`

- Добавить `drivers/ata.o` в `KERNEL_OBJS`.

## Поток данных

```
SFS2 (kernel/sfs2.c) → block cache (kernel/block.c) → sdev_ata
      → ata_read/ata_write (drivers/ata.c)
      → порты 0x1F0/0x170 (PIO, 28-bit LBA)
```

При загрузке: `kernel_main` → `interrupts_init` → `ata_init` (детекция+selftest)
→ `virtio_init` (в конце `block_init` выбирает ATA-бэкенд) → `vfs_init`
→ `sfs2_init` монтирует/форматирует SFS2 на ATA-диске.

## Обработка ошибок

- Нет диска на устройстве (ERR сразу после select / BSY не снимается /
  ATAPI / нулевая ёмкость) → устройство помечается отсутствующим, переход к
  следующему. Если ни один диск не найден → `ata_present() == 0`,
  бэкенд остаётся virtio/ramdisk.
- Ошибка I/O: таймаут → -1, ERR → -2; sdev-обёртки пробрасывают код; кэш
  `block.c` при неуспешном чтении не делает слот валидным (текущее поведение).
- Паника при работе с диском недопустима: все обращения к портам — в пределах
  фиксированных legacy-адресов, деления/приведения к ёмкости защищены от
  нуля.

## Тестирование

- `scripts/atatest.py` (стиль `blktest.py`): `truncate -s 4M /tmp/aos-atatest.img`,
  QEMU `-drive file=...,format=raw,if=ide`, boot. Проверки serial-лога:
  - нет `KERNEL PANIC`;
  - есть `ata: selftest OK`;
  - есть `block: ata backend`;
  - есть `SFS2 mounted (disk)` или `SFS2 formatting new disk`.
- Добавить `atatest` в `TESTS` (Makefile), рядом с `blktest`/`virtiotest`.
- Регресс: `blktest`, `virtiotest`, `ipctest` должны оставаться зелёными
  (виртуальные устройства в их харнессах не конфликтуют с ATA, так как в тех
  запусках ATA-диска нет).

## Ограничения (шаг 1)

- Только 28-bit LBA (ёмкость ≤ ~128 ГБ).
- PIO, один сектор за операцию, опрос вместо IRQ. IRQ14/15 не используются.
- Только ATA-диски; ATAPI (CD-ROM) игнорируются.
- PCI-обнаружение контроллера не требуется: используются фиксированные
  legacy-порты (достаточно для QEMU `pc` machine и классического железа).

## Вне скоупа

- IRQ-ориентированный PIO, DMA/UDMA, AHCI/SATA, NCQ, мультисекторные команды,
  ATAPI, 48-bit LBA, несколько одновременных дисков в VFS.