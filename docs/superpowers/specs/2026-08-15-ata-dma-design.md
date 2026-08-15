# Спецификация: Bus Master DMA для ATA-драйвера

Дата: 2026-08-15
Статус: утверждён (объём согласован в чате: полный DMA read+write, интегрированный в блок-слой)

## Цель

Добавить в `drivers/ata.c` поддержку Bus Master IDE DMA (PIIX3/PIIX4, READ DMA 0xC8 /
WRITE DMA 0xCA) как замену PIO-цикла в многосекторных операциях. DMA используется
для `count >= 2` секторов; посекторный путь (count == 1) остаётся PIO. При отсутствии
BM-контроллера или ошибке DMA — прозрачный фолбэк на существующий PIO.

Завершение — **опросом BM-статуса** (не IRQ): это согласуется с текущим дизайном
драйвера (nIEN установлен, IRQ14/15 не используются). Никаких изменений в
`interrupts.c`/PIC.

## Контекст

- `drivers/ata.c` (после предыдущих шагов): PIO read/write + `ata_read_multi`/
  `ata_write_multi` (сектор-каунт до 255, граница 28-bit LBA). `ata_send_lba(lba,count,cmd)`.
- Блок-слой (`kernel/block.c`): `block_read_multi`/`block_write_multi` пробуют
  `dev->read_multi`/`dev->write_multi`; при отказе фолбэк на посекторный цикл.
- PCI: `pci_find_all` (classcode/bar0/irq), `pci_config_read/write`.
- QEMU 10.2.1 `-drive ...,if=ide`: PIIX3 IDE на PCI 01.1, класс 0x010180,
  BAR4 (offset 0x20) = BM-регион, обычно 0xC000. Подтвердить при реализации
  логом `ata: dma bmba=...`.
- Буферы данных — ядро (BSS/кэш), identity-map: физический адрес = линейный.

## Регистры Bus Master IDE (I/O, от `bmba`)

| Смещение | Ширина | Назначение                                      |
|----------|--------|-------------------------------------------------|
| +0       | 8 бит  | Command: bit0 start/stop, bit3 dir (1 = чтение с диска) |
| +2       | 8 бит  | Status: bit0 active, bit1 interrupt, bit2 error, bit6/7 dma-capable |
| +4       | 32 бит | Адрес таблицы PRD (физический, 4-байт aligned) |

PRD-запись (16 байт): dword0 = физический адрес сегмента; dword1: байты 0..1 =
byte count (чётный, <= 0xFFFE), бит 7 байта 2 = EOT; остальное = 0.

## Компоненты

### 1. `drivers/ata.h` / `drivers/ata.c` — DMA-ядро

```c
struct ata_prd {
    unsigned int phys_addr;
    unsigned short byte_count;
    unsigned char flags;      // 0x80 = EOT
    unsigned char reserved;
    unsigned int reserved2;
} __attribute__((packed));
```

Статический `prd_table[8]` в BSS (identity-map, физический адрес 4-байт aligned).
Поля `struct ata_dev` (`drivers/ata.c`): `unsigned int bmba; int dma;`.

- `ata_dma_init()`: сканирует PCI (`pci_find_all`, `pci.h`), ищет `(classcode >> 8) == 0x0101`,
  читает BAR4 (`pci_config_read(bus,dev,func,0x20)`) → `bmba = bar4 & ~0x3`, включает
  bus mastering (`pci_config_write(...,0x04, cmd | 0x4)`), ставит `dma=1`, печатает
  `ata: dma bmba=...`. Вызывается из `ata_init()` до selftest'ов.
- `ata_prd_setup(void *buf, unsigned int bytes)`: режет буфер на сегменты <= 0xFFFE
  байт (вход кратен 512), последней записи ставит EOT.
- `ata_dma_read_multi(lba, count, buf)` / `ata_dma_write_multi(...)`:
  1. clear BM status: `outb(bmba+2, 0x06)` (сброс interrupt+error);
  2. `ata_prd_setup`; `outl(bmba+4, (unsigned int)prd_table)`;
  3. `outb(bmba+0, dir ? 0x09 : 0x01)` (bit3=1 для read, start);
  4. `ata_send_lba(lba, count, 0xC8 или 0xCA)`;
  5. ждать `!(inb(bmba+2) & 0x01)` (BM active сброшен) по `tick`-таймауту (2000);
  6. `ata_wait_ready(2000)` (BSY/ERR диска);
  7. если `inb(bmba+2) & 0x04` (BM error) → -1; иначе `outb(bmba+0, 0x00)` (stop), вернуть 0.
- `ata_read_multi`/`ata_write_multi`: при `gata.dma && count >= 2` вызывают DMA-вариант,
  иначе PIO. Отказ DMA (-1) всплывает в блок-слой, тот фолбэкается на посекторный PIO.

### 2. Selftest DMA

`ata_dma_selftest()` (вызов из `ata_init()`, только при `gata.dma`): write_multi 4
секторов через DMA в `base = capacity - 16`, read_multi обратно, сравнение побайтно.
Лог: `ata: dma selftest OK` / `ata: dma selftest FAIL`.

## Тестирование

- `scripts/atatest.py`: добавить ассерт `ata: dma selftest OK` (RED до кода).
  Существующий multi-selftest (count=4) теперь идёт через DMA-путь — дополнительно
  покрывает интеграцию `read_multi`→DMA.
- Регрессия: `scripts/blktest.py` (virtio, DMA не задействован), `make test-fast`.

## Риски

- BM-статус bit1 (interrupt) может не взводиться при nIEN — завершение детектится
  только по bit0 (active), затем `ata_wait_ready`.
- Если DMA не работает под QEMU TCG (таймаут) — `ata_dma_selftest FAIL`, тест упадёт,
  потребуется отладка по serial-логу (bmba, PRD, status).
- Буферы пользователя из ring-3 не поддерживаются (DMA ожидает физический адрес
  ядра); в текущей системе все операции идут через кэш ядра — документировано.