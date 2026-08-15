# ATA PIO Driver Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Добавить в ядро AOS драйвер PATA/IDE дисков (PIO, 28-bit LBA) как приоритетный бэкенд блочного слоя (ATA → virtio → ramdisk).

**Architecture:** Новый драйвер `drivers/ata.c` детектирует до 4 устройств (2 legacy-канала × master/slave), выбирает первый найденный диск и выполняет односекторные PIO-операции с опросом статус-регистра и таймаутом по `tick` (PIT 1000 Гц). `kernel/block.c` подключает его как ещё один `struct sdev`; при наличии ATA-диска корень SFS2 монтируется с него. Проверка — QEMU-тест `scripts/atatest.py` с `-drive file=...,format=raw,if=ide`.

**Tech Stack:** C (GCC, `-ffreestanding -m32`), порты x86 (`inb`/`inw`/`outb`/`outw` из `drivers/ports.h`), `tick` (PIT 1000 Гц), QEMU `piix3-ide` (legacy порты 0x1F0/0x170, IRQ14/15), Python QTest-харнесс (`scripts/qtest.py`).

**Spec:** `docs/superpowers/specs/2026-08-15-ata-driver-design.md`

## Global Constraints

- Коммуникация и документы — на русском; код, комментарии в коде и коммиты — на английском (как в остальном репо).
- 28-bit LBA, 1 сектор (512 Б) за операцию, PIO-опрос без IRQ. Таймауты по `tick` (единица = 1 мс): IDENTIFY — 5000 мс, I/O — 2000 мс.
- Приоритет бэкендов в `block_init()`: ATA → virtio → ramdisk.
- Выбирается только первый найденный диск (среди 4 устройств); ATAPI (word0 == 0xEB14) и устройства с нулевой ёмкостью пропускаются.
- Фиксированные legacy-порты: primary 0x1F0/0x3F6, secondary 0x170/0x376. PCI-обнаружение не требуется.
- `block_disk_present()` возвращает 1, если текущий бэкенд — ATA или virtio (для суффикса `(disk)` в SFS2).
- Логи драйвера — в serial стилем `drivers/vblk.c` (`serial_print`/`serial_print_dec`).
- `ata_read`/`ata_write` возвращают 0 при успехе, −1 при таймауте, −2 при ERR (соглашение `struct sdev`).

---

### Task 1: Обнаружение диска (IDENTIFY) и каркас драйвера

**Files:**
- Create: `drivers/ata.h`
- Create: `drivers/ata.c`
- Create: `scripts/atatest.py`
- Modify: `kernel/kernel.c:118-122` (вызов `ata_init()` перед `virtio_init()`)
- Modify: `Makefile` (`KERNEL_OBJS` — добавить `drivers/ata.o`; `TESTS` — добавить `atatest`)

**Interfaces:**
- Consumes: `serial_print`, `serial_print_dec` (`drivers/serial.h`); `inb`/`inw`/`outb`/`outw` (`drivers/ports.h`); `extern volatile unsigned int tick` (`kernel/kernel.c:24`); `QTest` (`scripts/qtest.py`).
- Produces (используются в Task 2/3):
  - `void ata_init(void)` — детекция, заполняет `gata` (static).
  - `int ata_present(void)` — 1, если диск найден.
  - `unsigned int ata_capacity_bytes(void)` — ёмкость в байтах.
  - Константы `ATA_PRIMARY_BASE`/`ATA_PRIMARY_CTRL`/`ATA_SECONDARY_BASE`/`ATA_SECONDARY_CTRL` из `drivers/ata.h`.

- [ ] **Step 1: Написать падающий тест `scripts/atatest.py`**

```python
#!/usr/bin/env python3
import os
import subprocess
import sys

from qtest import QTest

IMG = "/tmp/aos-atatest.img"


def main():
    try:
        os.unlink(IMG)
    except FileNotFoundError:
        pass
    subprocess.run(["truncate", "-s", "4M", IMG], check=True)
    extra = [
        "-drive", "file=" + IMG + ",format=raw,if=ide",
    ]
    with QTest("atatest", extra_args=extra) as q:
        q.boot_and_ready()
        log = q.serial_read()
        if "ata: found" not in log:
            raise AssertionError("ATA drive was not detected")
    print("PASS: ATA drive detected via IDENTIFY")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

Добавить `atatest` в `TESTS` в `Makefile` (в строку `TESTS = ...`, рядом с `blktest`).

- [ ] **Step 2: Запустить тест — должен упасть**

Run: `make && python3 scripts/atatest.py`
Expected: `AssertionError: ATA drive was not detected` (в логе нет `ata: found`, т.к. драйвера нет). Паники быть не должно.

- [ ] **Step 3: Создать `drivers/ata.h`**

```c
#ifndef ATA_H
#define ATA_H

#define ATA_PRIMARY_BASE   0x1F0
#define ATA_PRIMARY_CTRL   0x3F6
#define ATA_SECONDARY_BASE 0x170
#define ATA_SECONDARY_CTRL 0x376

void ata_init(void);
int ata_read(unsigned int lba, void *buf);
int ata_write(unsigned int lba, const void *buf);
int ata_present(void);
unsigned int ata_capacity_bytes(void);

#endif
```

- [ ] **Step 4: Создать `drivers/ata.c` — детекция (без read/write)**

```c
#include "ata.h"
#include "serial.h"
#include "ports.h"

#define ATA_CMD_IDENTIFY 0xEC
#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

#define ATA_SECTOR_SIZE   512
#define ATA_TIMEOUT_IDENTIFY 5000
#define ATA_TIMEOUT_RESET 2000

extern volatile unsigned int tick;

struct ata_dev {
    unsigned int base;
    unsigned int ctrl;
    int slave;
    unsigned int capacity_sectors;
    int present;
};

static struct ata_dev gata;

// Space register I/O ~400ns per read (ATA register settle time); also used as
// a short delay after resets. Reads the controller port so the bus is warmed.
static void ata_pause(unsigned int ctrl) {
    for (int i = 0; i < 4; i++) (void)inb(ctrl);
}

// Probe one device slot. Returns 0 and *sectors_out on success, -1 otherwise
// (no device, ATAPI, zero capacity, or IDENTIFY timeout).
static int ata_probe_drive(unsigned int base, unsigned int ctrl, int slave,
                           const char *chname, unsigned int *sectors_out) {
    outb(base + 6, 0xE0 | (slave << 4));     // select drive (LBA mode)
    ata_pause(ctrl);
    if (inb(base + 7) == 0x00) return -1;    // no device on this slot

    outb(base + 7, ATA_CMD_IDENTIFY);
    ata_pause(ctrl);

    unsigned int start = tick;
    unsigned char st;
    for (;;) {
        st = inb(base + 7);
        if (!(st & ATA_SR_BSY)) break;
        if ((int)(tick - start) >= ATA_TIMEOUT_IDENTIFY) return -1;
    }
    if (st & ATA_SR_ERR) return -1;          // device rejected IDENTIFY

    start = tick;
    while (!(inb(base + 7) & ATA_SR_DRQ)) {
        if ((int)(tick - start) >= ATA_TIMEOUT_IDENTIFY) return -1;
    }

    unsigned short ident[256];
    for (int i = 0; i < 256; i++) ident[i] = inw(base);

    if (ident[0] == 0xEB14) return -1;       // ATAPI, not a disk
    unsigned int cap = (unsigned int)ident[60] | ((unsigned int)ident[61] << 16);
    if (cap == 0) return -1;

    *sectors_out = cap;
    serial_print("ata: found ");
    serial_print(chname);
    serial_print(slave ? "/slave" : "/master");
    serial_print(", capacity=");
    serial_print_dec(cap / 2048);            // sectors / 2048 = MiB
    serial_print(" MiB\n");
    return 0;
}

int ata_present(void) {
    return gata.present;
}

unsigned int ata_capacity_bytes(void) {
    return gata.capacity_sectors * ATA_SECTOR_SIZE;
}

void ata_init(void) {
    static const struct {
        unsigned int base, ctrl;
        const char *name;
    } channels[2] = {
        { ATA_PRIMARY_BASE, ATA_PRIMARY_CTRL, "primary" },
        { ATA_SECONDARY_BASE, ATA_SECONDARY_CTRL, "secondary" },
    };

    for (int c = 0; c < 2 && !gata.present; c++) {
        unsigned int base = channels[c].base;
        unsigned int ctrl = channels[c].ctrl;

        // Software reset the channel, then wait for BSY to clear (or the
        // channel to report empty: status 0x00). QEMU is instant.
        outb(ctrl, 0x04);                    // SRST
        ata_pause(ctrl);
        outb(ctrl, 0x00);
        unsigned int start = tick;
        for (;;) {
            unsigned char st = inb(base + 7);
            if (!(st & ATA_SR_BSY) || st == 0x00) break;
            if ((int)(tick - start) >= ATA_TIMEOUT_RESET) break;
        }

        for (int slave = 0; slave < 2 && !gata.present; slave++) {
            unsigned int cap;
            if (ata_probe_drive(base, ctrl, slave, channels[c].name, &cap) == 0) {
                gata.base = base;
                gata.ctrl = ctrl;
                gata.slave = slave;
                gata.capacity_sectors = cap;
                gata.present = 1;
            }
        }
    }
    if (!gata.present)
        serial_print("ata: no disk found\n");
}
```

- [ ] **Step 5: Подключить в ядро и сборку**

`kernel/kernel.c`:
- Добавить `#include "ata.h"` к заголовкам (после `#include "virtio.h"`).
- Вставить `ata_init();` сразу перед `virtio_init();` (строка ~120).

`Makefile`:
- В `KERNEL_OBJS` добавить `drivers/ata.o` (в строке с другими `drivers/*.o`).

- [ ] **Step 6: Запустить тест — должен пройти**

Run: `make && python3 scripts/atatest.py`
Expected: `PASS: ATA drive detected via IDENTIFY`. В serial-логе (`/tmp/aos-atatest.log`) есть строка `ata: found primary/master, capacity=4 MiB`. Без ATA-диска (например `python3 scripts/blktest.py`) загрузка не должна падать и не должна сообщать `ata: found` — проверяется, что пустой secondary-канал не вызывает 5-секундную паузу (статус 0x00 → мгновенный выход).

- [ ] **Step 7: Commit**

```bash
git add drivers/ata.h drivers/ata.c scripts/atatest.py kernel/kernel.c Makefile
git commit -m "ata: add PIO driver with IDENTIFY detection"
```

---

### Task 2: PIO чтение/запись и selftest

**Files:**
- Modify: `drivers/ata.c` (добавить `ata_read`, `ata_write`, `ata_wait_ready`, `ata_wait_drq`, `ata_send_lba`, `ata_selftest`; вызвать `ata_selftest()` в конце `ata_init()`)
- Modify: `scripts/atatest.py` (добавить проверки selftest)

**Interfaces:**
- Consumes: `gata` (base/ctrl/slave/present из Task 1), `ATA_SECTOR_SIZE`.
- Produces (используются в Task 3):
  - `int ata_read(unsigned int lba, void *buf)` — 0 / −1 (таймаут) / −2 (ERR).
  - `int ata_write(unsigned int lba, const void *buf)` — 0 / −1 / −2.
  - Лог `ata: selftest OK` / `ata: selftest FAIL`.

- [ ] **Step 1: Расширить тест проверками selftest**

В `scripts/atatest.py` внутри `main()` после проверки `ata: found` добавить:

```python
        if "ata: selftest FAIL" in log:
            raise AssertionError("ATA selftest reported FAIL")
        if "ata: selftest OK" not in log:
            raise AssertionError("ATA selftest did not report OK")
```

- [ ] **Step 2: Запустить тест — должен упасть**

Run: `make && python3 scripts/atatest.py`
Expected: `AssertionError: ATA selftest did not report OK` (selftest ещё не реализован).

- [ ] **Step 3: Добавить PIO I/O в `drivers/ata.c`**

Вставить после `ata_present`/`ata_capacity_bytes` (перед `ata_init`):

```c
static int ata_wait_ready(unsigned int timeout_ms) {
    unsigned int start = tick;
    for (;;) {
        unsigned char st = inb(gata.base + 7);
        if (!(st & ATA_SR_BSY)) {
            if (st & ATA_SR_ERR) return -2;
            return 0;
        }
        if ((int)(tick - start) >= (int)timeout_ms) return -1;
    }
}

static int ata_wait_drq(unsigned int timeout_ms) {
    unsigned int start = tick;
    while (!(inb(gata.base + 7) & ATA_SR_DRQ)) {
        if ((int)(tick - start) >= (int)timeout_ms) return -1;
    }
    return 0;
}

// Load the 28-bit LBA parameters and issue a command (READ 0x20 / WRITE 0x30).
static void ata_send_lba(unsigned int lba, unsigned char cmd) {
    outb(gata.base + 2, 1);                  // sector count = 1
    outb(gata.base + 3, (unsigned char)lba);
    outb(gata.base + 4, (unsigned char)(lba >> 8));
    outb(gata.base + 5, (unsigned char)(lba >> 16));
    outb(gata.base + 6, 0xE0 | (gata.slave << 4) | ((lba >> 24) & 0x0F));
    outb(gata.base + 7, cmd);
}

int ata_read(unsigned int lba, void *buf) {
    if (!gata.present) return -1;
    ata_send_lba(lba, 0x20);
    int rc = ata_wait_drq(2000);
    if (rc < 0) return rc;
    unsigned short *w = (unsigned short *)buf;
    for (int i = 0; i < 256; i++) w[i] = inw(gata.base);
    return ata_wait_ready(2000);
}

int ata_write(unsigned int lba, const void *buf) {
    if (!gata.present) return -1;
    ata_send_lba(lba, 0x30);
    int rc = ata_wait_drq(2000);
    if (rc < 0) return rc;
    const unsigned short *w = (const unsigned short *)buf;
    for (int i = 0; i < 256; i++) outw(gata.base, w[i]);
    return ata_wait_ready(2000);
}
```

И добавить selftest перед `ata_init`:

```c
static void ata_selftest(void) {
    unsigned char w[ATA_SECTOR_SIZE], r[ATA_SECTOR_SIZE];
    for (unsigned int i = 0; i < ATA_SECTOR_SIZE; i++)
        w[i] = (unsigned char)(i * 7 + 3);
    unsigned int last = gata.capacity_sectors - 1;
    if (ata_write(last, w) == 0 && ata_read(last, r) == 0) {
        int ok = 1;
        for (unsigned int i = 0; i < ATA_SECTOR_SIZE; i++)
            if (w[i] != r[i]) { ok = 0; break; }
        serial_print(ok ? "ata: selftest OK\n" : "ata: selftest FAIL\n");
    } else {
        serial_print("ata: selftest FAIL\n");
    }
}
```

В конце `ata_init()` (после цикла по каналам, но до `if (!gata.present)`):

```c
    if (gata.present)
        ata_selftest();
```

Примечание: сравнение `(int)(tick - start)` устойчиво к переполнению `tick` (разница трактуется как знаковая); `tick` увеличивается в таймерном IRQ, который срабатывает между обращениями к портам, не ломая PIO-последовательность.

- [ ] **Step 4: Запустить тест — должен пройти**

Run: `make && python3 scripts/atatest.py`
Expected: `PASS`. В логе `ata: found primary/master, capacity=4 MiB` и `ata: selftest OK`.

- [ ] **Step 5: Commit**

```bash
git add drivers/ata.c scripts/atatest.py
git commit -m "ata: add PIO read/write and selftest"
```

---

### Task 3: Интеграция в блочный слой (sdev_ata)

**Files:**
- Modify: `kernel/block.c` (добавить `sdev_ata`, обёртки, приоритет в `block_init`, обновить `block_disk_present`)
- Modify: `scripts/atatest.py` (проверки `block: ata backend` и монтирования SFS2)

**Interfaces:**
- Consumes: `ata_present()`, `ata_read()`, `ata_write()`, `ata_capacity_bytes()` (Task 1/2), `printf` (`kernel/printf.h`).
- Produces: `struct sdev` с `read`/`write`/`present`/`capacity_sectors`, выбранный `block_init()` при `ata_present()`; корневая ФС SFS2 на ATA-диске.

- [ ] **Step 1: Расширить тест проверками блок-слоя**

В `scripts/atatest.py` добавить после проверок selftest:

```python
        if "block: ata backend" not in log:
            raise AssertionError("block layer did not select ATA backend")
        if "SFS2 mounted (disk)" not in log and "SFS2 formatting new disk" not in log:
            raise AssertionError("SFS2 did not mount from ATA disk")
```

- [ ] **Step 2: Запустить тест — должен упасть**

Run: `make && python3 scripts/atatest.py`
Expected: `AssertionError: block layer did not select ATA backend` (блок-слой пока использует ramdisk).

- [ ] **Step 3: Изменить `kernel/block.c`**

Добавить `#include "ata.h"` в шапку (после `#include "vblk.h"`).

Рядом с `sdev_vblk` (строка ~13) добавить:

```c
static struct sdev sdev_ata;

static int sdev_ata_read(unsigned int lba, void *buf) {
    return ata_read(lba, buf);
}

static int sdev_ata_write(unsigned int lba, const void *buf) {
    return ata_write(lba, buf);
}
```

`block_disk_present()` (строка ~127) заменить на:

```c
int block_disk_present(void) {
    return (dev == &sdev_ata && sdev_ata.present) ||
           (dev == &sdev_vblk && sdev_vblk.present);
}
```

`block_init()` (строка ~131) — в начало функции добавить ветку ATA:

```c
int block_init(void) {
    if (ata_present()) {
        sdev_ata.read = sdev_ata_read;
        sdev_ata.write = sdev_ata_write;
        sdev_ata.present = 1;
        sdev_ata.capacity_sectors = ata_capacity_bytes() / BLOCK_SIZE;
        dev = &sdev_ata;
        printf("block: ata backend, %u sectors\n", sdev_ata.capacity_sectors);
    } else if (vblk_present()) {
        sdev_vblk.read = sdev_vblk_read;
        sdev_vblk.write = sdev_vblk_write;
        sdev_vblk.present = 1;
        sdev_vblk.capacity_sectors = vblk_capacity_bytes() / BLOCK_SIZE;
        dev = &sdev_vblk;
        printf("block: disk backend, %u sectors\n", sdev_vblk.capacity_sectors);
    } else {
        sdev_ram.read = sdev_ram_read;
        sdev_ram.write = sdev_ram_write;
        dev = &sdev_ram;
        printf("block: ram backend, %u sectors\n", RAMDISK_SECTORS);
    }
    return 0;
}
```

- [ ] **Step 4: Запустить тест — должен пройти**

Run: `make && python3 scripts/atatest.py`
Expected: `PASS`. В логе: `ata: found primary/master, capacity=4 MiB`, `ata: selftest OK`, `block: ata backend, 8192 sectors`, `SFS2 formatting new disk` (свежий диск 4 МБ; sfs2 форматирует и монтирует с него корень).

- [ ] **Step 5: Регресс virtio/ramdisk путей**

Run: `python3 scripts/blktest.py` и `python3 scripts/virtiotest.py`
Expected: оба `PASS` (в их запусках ATA-диска нет → бэкенд virtio/ramdisk без изменений). Также `python3 scripts/ipctest.py` — быстрая проверка общей загрузки.

- [ ] **Step 6: Commit**

```bash
git add kernel/block.c scripts/atatest.py
git commit -m "block: select ATA backend when present"
```

---

## Self-Review

- **Spec coverage:**
  - Драйвер `drivers/ata.c` (детекция, IDENTIFY, отсев ATAPI, 28-bit LBA, таймауты) → Task 1/2.
  - PIO read/write 1 сектор, опрос, коды 0/−1/−2 → Task 2.
  - `sdev_ata`, приоритет ATA→virtio→ramdisk, `block_disk_present` → Task 3.
  - `ata_init()` перед `virtio_init()` → Task 1 Step 5.
  - Makefile `KERNEL_OBJS` → Task 1 Step 5; `TESTS` + `scripts/atatest.py` → Task 1 Step 1 + Task 2/3.
  - Ограничения шага 1 (IRQ/DMA/AHCI/ATAPI вне скоупа) — зафиксированы в Global Constraints, реализация их не добавляет.
- **Placeholder scan:** все шаги содержат готовый код; нет TBD/TODO/«добавь обработку ошибок» без кода.
- **Type consistency:** сигнатуры `ata_init`/`ata_present`/`ata_capacity_bytes`/`ata_read`/`ata_write` совпадают между Task 1, 2, 3 и заголовком `drivers/ata.h`. Коды возврата I/O (0/−1/−2) едины. Сообщения логов (`ata: found`, `ata: selftest OK`, `block: ata backend`, `SFS2 ... disk`) совпадают с проверками в `atatest.py`.