# Спецификация: AHCI/SATA бэкенд для блок-слоя

Дата: 2026-08-15
Статус: утверждён (объём согласован в чате: реализовать AHCI-бэкенд рядом с ata/vblk)

## Цель

Добавить третий диск-бэкенд `ahci` для QEMU `-device ich9-ahci` (SATA, AHCI HBA).
Драйвер работает через HBA DMA (PRDT) — в AHCI нет классического PIO-пути данных,
поэтому «PIO сначала» интерпретируется как минимальный синхронный драйвер: одна
команда за раз через командный лист (slot 0), завершение — опросом `PxCI`/`PxIS`,
без IRQ и без NCQ. Это «AHCI HBA DMA» из предложенного варианта; NCQ/IRQ — вне
объёма (потенциальная следующая задача).

Бэкенд предоставляет `struct sdev` для `kernel/block.c` (как `ata`/`vblk`/`ram`).

## Контекст

- `kernel/block.c`: `block_init()` выбирает бэкенд; сейчас `ata > vblk > ram`.
  **Дефект пункта 2**: `read_multi`/`write_multi` не назначаются для `sdev_ata`/
  `sdev_vblk`, поэтому блок-слой всегда ходит посекторно (`dev->read`). Исправить
  в этой задаче: назначить multi-слоты всем дисковым бэкендам.
- `block_init()` вызывается из `virtio_init()` (drivers/virtio.c:187) ПОСЛЕ `vblk_init()`.
  `ahci_init()` должен отработать ДО `block_init()`: вставить в `kernel.c` между
  `ata_init()` и `virtio_init()`.
- PCI: `pci_find_all()` → classcode в формате `[23:16]=base class, [15:8]=subclass,
  [7:0]=prog-if`. AHCI: base 0x01 (mass storage), subclass 0x06 (SATA), prog-if 0x01 →
  `classcode == 0x010601`. BAR5 = регистр 0x24.
- QEMU 10.2.1 `ich9-ahci`: устройство 8086:2922, класс 0x010601, **BAR5 = 0xFEB81000**
  (проверено в мониторе) — выше 256 MB, требует identity-маппинга как framebuffer.
- Память ядра identity-map: физический адрес = линейный.
- IRQ не используем (PxIE = 0) — как ATA с nIEN; лишних прерываний не будет.

## AHCI: регистры и структуры

### Глобальные регистры HBA (от ABAR)

| Смещение | Назначение |
|----------|------------|
| 0x00 | HBA_CAP |
| 0x04 | HBA_GHC (bit0 AE, bit31 HR) |
| 0x08 | HBA_IS |
| 0x0C | HBA_PI (бит порта = реализован) |
| 0x10 | HBA_VS |
| 0x14 | HBA_CCCCTL |

Регистры порта n: база `ABAR + 0x100 + n*0x80`.

| Смещение | Назначение |
|----------|------------|
| 0x00/0x04 | PxCLB / PxCLBU — адрес командного листа |
| 0x08/0x0C | PxFB / PxFBU — адрес области приёма FIS |
| 0x10 | PxIS — статус прерываний (write-1-to-clear); bit0 DHR, bit30 TFES |
| 0x14 | PxIE — маска прерываний (оставляем 0) |
| 0x18 | PxCMD — bit0 ST, bit1 SUD, bit2 POD, bit4 FRE, bit15 CR, bit28 ICC |
| 0x20 | PxTFD — task file (bit0 ERR) |
| 0x24 | PxSIG — сигнатура устройства |
| 0x28 | PxSSTS — статус линка (DET [3:0], IPM [7:4]) |
| 0x2C | PxSCTL |
| 0x30 | PxSERR |
| 0x38 | PxCI — биты активных слотов |

### Командный лист (1 команда, slot 0)

- `cmd_list[32]` (32-байтные слоты, массив 1KB, выравнивание 1024). Используем слот 0:
  - DW0: `CFL(5)` для H2D Register FIS, `Write(6)` для записи;
  - DW1: `PRDTL` = число PRD-записей;
  - DW2: `PRDBC` = 0;
  - DW3/DW4: физический адрес командной таблицы.
- `cmd_table[256]` (выравнивание 1024):
  - `[0x00..0x13]`: H2D Register FIS (20 байт):
    - byte0 type = 0x27; byte1 flags = 0x80; byte2 command; byte3 = feature;
    - bytes 4..6 = LBA[23:0]; byte7 = device (0x40 — LBA48);
    - bytes 8..10 = LBA[31:24]; bytes 12..14 = LBA[47:40]; byte11 = feature ext;
    - byte15 = count low; byte16 = count high (1..255); остальное 0.
  - `[0x80..]`: PRD-записи (16 байт): [31:0] = физ. адрес; [95:64] byte count [21:0]
    (<= 0x400000); [127:96] bit31 = I (Interrupt on completion, ставим 0).
- `fis_area[256]` (выравнивание 1024) — приём D2H FIS.

## Компоненты

### 1. `kernel/paging.c` / `paging.h` — общий identity-маппинг

Выделить из framebuffer-маппинга общий `int paging_identity_map(unsigned int phys,
unsigned int bytes)` (identity-map физического диапазона через `extra_pt`/существующие
page tables). Заменить дублирующий код в `paging_init()` для framebuffer на вызов
функции. Для ABAR (1 страница) достаточно одной записи.

### 2. `drivers/ahci.h` / `drivers/ahci.c` — драйвер

```c
void ahci_init(void);
int ahci_present(void);
unsigned int ahci_capacity_sectors(void);
int ahci_read(unsigned int lba, void *buf);
int ahci_write(unsigned int lba, const void *buf);
int ahci_read_multi(unsigned int lba, unsigned int count, void *buf);
int ahci_write_multi(unsigned int lba, unsigned int count, const void *buf);
```

- MMIO-хелперы: `mmio32(addr)`, `mmio_out32(addr, v)` через `*(volatile unsigned int*)`.
- `ahci_init()`:
  1. `pci_find_all` → ищем `classcode == 0x010601`; читаем BAR5 (`0x24`) → `abar = bar5 & ~0xFFF`;
  2. `paging_identity_map(abar, 4096)`;
  3. читаем `HBA_PI`; выбираем первый установленный порт; проверяем `PxSSTS` (DET==3,
     IPM==1) и `PxSIG` (ATA-диск, не ATAPI 0xEB140101);
  4. инициализация порта: `PxCMD &= ~(ST|FRE)` → дождаться `!CR`; записать `PxCLB`/
     `PxFB`; `PxCMD |= POD|SUD|FRE`; затем `PxCMD |= ST`;
  5. IDENTIFY (0xEC, PRDT 1 сектор) → `ahci_command_read(0xEC, ...)`: слова 60/61
     (28-bit LBA), word 83 bit10 + word 86 bit10 (LBA48), слова 100..103 (48-bit LBA);
  6. лог `ahci: found port0 ... capacity=... MiB`, `ahci: hba abar=...`;
  7. selftest'ы: одиночный и multi (4 сектора) roundtrip через `ahci_read_multi`/
     `ahci_write_multi`.
- `ahci_command(type, lba, count, buf)` — общий путь:
  1. собрать H2D FIS (READ DMA EXT 0x25 / WRITE DMA EXT 0x35 / IDENTIFY 0xEC), PRDT;
  2. очистить `PxIS` (записать прочитанное значение), `PxCI = 1` (slot 0);
  3. ждать `!(PxCI & 1)` по `tick`-таймауту (2000 мс);
  4. если `PxIS & 0x40000000` (TFES) или `PxTFD & 1` (ERR) → -1.
- `ahci_read_multi`/`ahci_write_multi`: `count <= 128` (буфер в PRDT ограничен),
  `lba + count` без переполнения 48-bit.

### 3. `kernel/block.c` — интеграция

- `sdev_ahci` + обёртки на `ahci_*`; в `block_init()` приоритет `ahci > ata > vblk > ram`
  с логом `block: ahci backend, N sectors`.
- **Фикс пункта 2**: назначить `read_multi`/`write_multi` и для `sdev_ata` (обёртки на
  `ata_read_multi`/`ata_write_multi`) и для `sdev_vblk` (обёртки на
  `vblk_read_multi`/`vblk_write_multi`). Фолбэк в `block_read_multi` остаётся.

### 4. `kernel/kernel.c` — порядок init

`ata_init(); ahci_init(); virtio_init();` (virtio_init внутри вызывает `block_init()`).

### 5. `Makefile`

Добавить `drivers/ahci.o` в `KERNEL_OBJS`.

## Тестирование

- Новый `scripts/ahcitest.py`: QEMU с `-drive ...,if=none,id=hd` + `-device ich9-ahci` +
  `-device ide-hd,drive=hd,bus=ahci.0` (диск 4 MiB). Ассерты: `ahci: found`,
  `ahci: selftest OK`, `ahci: selftest multi OK`, `block: ahci backend, 8192 sectors`,
  `SFS2 mounted (disk)`, `SFS2 formatting new disk` (новый диск форматируется).
  RED до кода.
- Регрессия: `atatest.py` (ata/IDE), `blktest.py` (virtio), `persisttest.py`,
  `make test-fast`.

## Риски

- ABAR выше 256 MB — если `paging_identity_map` не сработает, MMIO-чтения вернут мусор
  (страница не маппится) → паника. Проверять логом `ahci: hba abar=...` и первыми
  регистрами (HBA_PI != 0).
- TCG-тайминги QEMU AHCI: PxCI может очищаться с задержкой — таймаут 2000 мс по `tick`.
- IF/порядок операций: строго «ST=0 → программировать CLB/FB → POD/SUD/FRE → ST»,
  иначе HBA уйдёт в неопределённое состояние.
- `PxSIG` для QEMU SATA-диска = `0x00000101`; если устройство выдаст другое — фолбэк
  на DET==3 без проверки сигнатуры (или лог `ahci: not a disk`).