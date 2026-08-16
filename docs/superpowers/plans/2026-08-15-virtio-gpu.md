# VirtIO-GPU Аппаратное Видеоускорение — План Реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Устранить мерцание экрана при обновлении, переведя рендер WM на virtio-gpu с атомарным `SET_SCANOUT` (двойной буфер) и аппаратным курсором, сохранив полный VGA-fallback.

**Architecture:** Ядро создаёт 2 framebuffer-ресурса virtio-gpu (1024×768×32, `B8G8R8X8_UNORM`, backing = обычные страницы RAM из user-accessible окна `0x04000000..0x04800000`). WM пишет в «back»-буфер (identity-адрес, который возвращает `AOS_FB_INFO` в GPU-режиме), затем `AOS_GPU_FLIP` атомарно переключает кадр; курсор уходит через `AOS_CURSOR` (cursor queue, QEMU рисует поверх). VGA остаётся для boot и как fallback (`vgu_active()==0`).

**Tech Stack:** C (ядро, `-ffreestanding -m32`), **modern virtio-pci транспорт** (`drivers/virtio_modern.c`, capability-based, memory BAR), virtio-gpu протокол, QEMU `-vga none -device virtio-vga,disable-modern=on`, Python QEMU-тесты (`scripts/qtest.py`).

**Spec:** `docs/superpowers/specs/2026-08-15-virtio-gpu-design.md`

## Global Constraints

- Язык: код/коммиты на английском, общение/доки на русском (AGENTS.md).
- Фиксированное разрешение 1024×768×32 (`B8G8R8X8_UNORM`), один scanout, один дисплей.
- virtio-gpu — non-transitional устройство, PCI id `0x1050`; QEMU не даёт legacy I/O BAR → нужен modern транспорт (`drivers/virtio_modern.c`, `vm_*` API).
- Двойной буфер: 2 буфера по 3 МБ в окне `0x04000000..0x04800000` (PDE 16–17), identity-map + user bit; окно резервируется в `pmm.c`.
- Формат пикселя: наш рендер пишет u32 `0x00RRGGBB` (little-endian: байты BB GG RR 00) — совпадает с `VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM` (alpha игнорируется, в отличие от `B8G8R8A8` где alpha=0 дал бы прозрачные пиксели).
- Валидация пользовательских указателей через `in_luser` (`kernel/aos_gui.c`), ошибка `-5`.
- Без GPU (`vgu_active()==0`): полный fallback — WM работает как сейчас, пиксельные GUI-тесты без `-vga none -device virtio-vga,...` не гоняются.
- `AOS_EXT` syscall-слоты 521–523 свободны.

---

### Task 1: Резерв GPU-окна в памяти (pmm + paging)

**Files:**
- Modify: `kernel/pmm.c:258` (добавить резерв рядом с slab window)
- Modify: `kernel/paging.c:40-47` (маппинг окна с user bit)
- Test: `make` + `make test-fast` (регрессия не ломается)

**Interfaces:**
- Consumes: существующий `pmm_init(mb_info_addr)`, `paging_init()`.
- Produces: физический=виртуальный диапазон `0x04000000..0x04600000`, user-accessible, никогда не отдаётся buddy; константы `GPU_BASE 0x04000000`, `GPU_STRIDE 0x300000` (3 МБ), `GPU_WINDOW 0x600000`.

- [ ] **Step 1: Проверить текущее состояние**

Читать `kernel/pmm.c:247-270` (список `reserve(...)` в `pmm_init`) и `kernel/paging.c:23-63` (`paging_init`, slab-маппинг). Убедиться, что `0x04000000..0x04600000` нигде не используется (не входит в slab `0x03000000..0x04000000` и в Linux window `0x08000000..0x10000000`).

- [ ] **Step 2: Зарезервировать окно в pmm**

В `kernel/pmm.c`, в `pmm_init()` рядом с `reserve(0x03000000, 0x04000000)` добавить:

```c
reserve(0x04000000, 0x04600000);  // virtio-gpu double-buffer window
```

- [ ] **Step 3: Сделать окно user-accessible в paging**

В `kernel/paging.c`, в `paging_init()` после блока slab (строки 40-47) добавить:

```c
// VirtIO-GPU double-buffer window: 0x04000000 .. 0x04600000, user-accessible
// (the window manager, a ring-3 task, composites into the back buffer).
for (int t = 16; t <= 17; t++) {
    page_dir[t] |= PTE_USER;
    for (int p = 0; p < 1024; p++)
        page_tables[t][p] |= PTE_USER;
}
```

- [ ] **Step 4: Собрать и прогнать регрессию**

Run: `make && make test-fast`
Expected: сборка чистая, тесты зелёные (окно просто резервируется, никто его ещё не трогает).

- [ ] **Step 5: Commit**

```bash
git add kernel/pmm.c kernel/paging.c
git commit -m "paging: reserve a user-accessible window for virtio-gpu buffers"
```

---

### Task 2a: Modern virtio-pci транспорт (capability-based)

**Files:**
- Create: `drivers/virtio_modern.h`
- Create: `drivers/virtio_modern.c`
- Modify: `Makefile:12-23` (`KERNEL_OBJS` — добавить `drivers/virtio_modern.o`)
- Test: `make` (интеграция с GPU-драйвером проверяется в Task 2)

**Почему:** QEMU 10.2.1 не предоставляет virtio-gpu legacy I/O BAR (устройство non-transitional, PCI id `0x1050`); legacy-транспорт `drivers/virtio.c` (только I/O-порты) не может с ним общаться. Нужен modern virtio-pci транспорт через VNDR-capabilities и memory BAR'ы.

**Interfaces:**
- Consumes: `pci_find_all`/`pci_config_read` (`drivers/pci.h`), `paging_identity_map` (`kernel/paging.h`), `page_alloc_order` (`kernel/pmm.h`), `serial_print` (`drivers/serial.h`).
- Produces: `struct virtio_modern` + API `vm_probe`/`vm_dev_init`/`vm_setup_queue`/`vm_ready`/`vm_alloc_desc`/`vm_desc_set`/`vm_submit`/`vm_free_chain`/`vm_used_pop` (сигнатуры ниже). Split-vring layout совпадает с legacy, поэтому логика `alloc_desc`/`desc_set`/`free_chain`/`used_pop` копируется из `drivers/virtio.c`.

**Нормативный layout (источник: `/usr/include/linux/virtio_pci.h`, которому следует QEMU):**

```c
struct virtio_pci_cap {          // 16 байт
    u8 cap_vndr;                 // 0x00 = PCI_CAP_ID_VNDR (0x09)
    u8 cap_next;                 // 0x01 next ptr в PCI config space
    u8 cap_len;                  // 0x02
    u8 cfg_type;                 // 0x03
    u8 bar;                      // 0x04
    u8 id;                       // 0x05
    u8 padding[2];               // 0x06
    u32 offset;                  // 0x08 (LE)
    u32 length;                  // 0x0C (LE)
} __attribute__((packed));
// cfg_type: 1=COMMON_CFG, 2=NOTIFY_CFG, 3=ISR_CFG, 4=DEVICE_CFG
// для NOTIFY_CFG: u32 notify_off_multiplier на offset 0x10 (вслед за cap)

struct virtio_pci_common_cfg {   // mmio-регистры (LE)
    u32 device_feature_select;   // 0x00
    u32 device_feature;          // 0x04 (ro)
    u32 guest_feature_select;    // 0x08
    u32 guest_feature;           // 0x0C
    u16 msix_config;             // 0x10
    u16 num_queues;              // 0x12 (ro)
    u8  device_status;           // 0x14
    u8  config_generation;       // 0x15 (ro)
    u16 queue_select;            // 0x16
    u16 queue_size;              // 0x18
    u16 queue_msix_vector;       // 0x1A
    u16 queue_enable;            // 0x1C
    u16 queue_notify_off;        // 0x1E (ro)
    u32 queue_desc_lo;           // 0x20
    u32 queue_desc_hi;           // 0x24
    u32 queue_avail_lo;          // 0x28
    u32 queue_avail_hi;          // 0x2C
    u32 queue_used_lo;           // 0x30
    u32 queue_used_hi;           // 0x34
};
```

- Статусы: `ACKNOWLEDGE 1`, `DRIVER 2`, `FEATURES_OK 8`, `DRIVER_OK 4`; запись `device_status=0` = reset.
- `VIRTIO_F_VERSION_1 = (1ULL<<32)` — обязательный modern-флаг; negotiate через select/feature пары (select 0 → биты 0-31, select 1 → биты 32-63).
- Notify: `addr = notify_base + queue_notify_off * notify_off_multiplier`; записать 16-битный qidx. `queue_notify_off` — индекс, не байтовый сдвиг.
- ISR: 1 байт, bit 0 = queue, bit 1 = config; чтение сбрасывает (для polling не критично).
- Queue setup (modern): `queue_select=qidx` → `queue_size=n` → записать три физ. адреса (`queue_desc`, `queue_avail`, `queue_used` — 32-bit lo/hi пары) → `queue_enable=1`. **Queue PFN (legacy) отсутствует.**
- BAR'ы: читать из PCI config (BAR0=0x10, BAR1=0x14, BAR2=0x18, BAR3=0x1C, BAR4=0x20, BAR5=0x24). Для memory BAR: `addr = raw & ~0xF`; для 64-bit (raw & 0x6 == 0x4): `addr |= hi<<32`. Бар-адреса virtio-vga выше 256 МБ → `paging_identity_map(addr, length)` перед mmio-доступом.

**Шаги:**

- [ ] **Step 1: Написать `drivers/virtio_modern.h`**

```c
#ifndef VIRTIO_MODERN_H
#define VIRTIO_MODERN_H

#include "virtio.h"   // struct vring_desc/vring_avail/vring_used

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

#define VM_STATUS_ACKNOWLEDGE 1
#define VM_STATUS_DRIVER      2
#define VM_STATUS_FEATURES_OK 8
#define VM_STATUS_DRIVER_OK   4

#define VM_F_VERSION_1 (1ULL << 32)

struct vm_cap {
    unsigned char cap_vndr, cap_next, cap_len, cfg_type;
    unsigned char bar, id, padding[2];
    unsigned int offset, length;
} __attribute__((packed));

struct virtio_modern {
    unsigned char bus, dev, func;
    volatile unsigned char *common;    // common cfg mmio base
    volatile unsigned char *notify;    // notify base
    unsigned int notify_multiplier;
    volatile unsigned char *isr;
    // vq 0..1 (control, cursor)
    volatile struct vring_desc *desc;
    volatile struct vring_avail *avail;
    volatile struct vring_used *used;
    unsigned short size, free_head, last_used;
    unsigned short notify_off[2];
};

int vm_probe(struct virtio_modern *m, unsigned int device_id);
int vm_dev_init(struct virtio_modern *m, unsigned long long supported);
int vm_setup_queue(struct virtio_modern *m, unsigned int qidx, unsigned int n);
void vm_ready(struct virtio_modern *m);
unsigned int vm_alloc_desc(struct virtio_modern *m, unsigned int qidx);
void vm_desc_set(struct virtio_modern *m, unsigned int qidx, unsigned int idx,
                 unsigned int addr, unsigned int len, unsigned short flags);
void vm_submit(struct virtio_modern *m, unsigned int qidx, unsigned int head);
void vm_free_chain(struct virtio_modern *m, unsigned int qidx, unsigned int head);
int vm_used_pop(struct virtio_modern *m, unsigned int qidx,
                unsigned int *id, unsigned int *len);

#endif
```

- [ ] **Step 2: Написать `drivers/virtio_modern.c`**

Реализовать по спецификации выше. Ключевые фрагменты (полный код пишет имплементер по этим спецификациям):

```c
#include "virtio_modern.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"

// mmio-доступ к common cfg
static inline u32 vm_cfg32(struct virtio_modern *m, unsigned int off) {
    return *(volatile unsigned int *)(m->common + off);
}
static inline void vm_cfg_w32(struct virtio_modern *m, unsigned int off, unsigned int v) {
    *(volatile unsigned int *)(m->common + off) = v;
}
static inline void vm_cfg_w16(struct virtio_modern *m, unsigned int off, unsigned short v) {
    *(volatile unsigned short *)(m->common + off) = v;
}
static inline unsigned short vm_cfg_r16(struct virtio_modern *m, unsigned int off) {
    return *(volatile unsigned short *)(m->common + off);
}
static inline void vm_cfg_w8(struct virtio_modern *m, unsigned int off, unsigned char v) {
    *(volatile unsigned char *)(m->common + off) = v;
}
static inline unsigned char vm_cfg_r8(struct virtio_modern *m, unsigned int off) {
    return *(volatile unsigned char *)(m->common + off);
}

// vm_probe: найти устройство, просканировать VNDR-capabilities, identity-map BAR'ы,
// заполнить common/notify/isr базы. Возвращает 0 при успехе, -1 нет устройства.
int vm_probe(struct virtio_modern *m, unsigned int device_id) {
    struct pci_dev list[8];
    int n = pci_find_all(list, 8);
    int idx = -1;
    for (int i = 0; i < n; i++)
        if (list[i].vendor == 0x1AF4 && list[i].device == device_id) { idx = i; break; }
    if (idx < 0) return -1;
    m->bus = list[idx].bus; m->dev = list[idx].dev; m->func = list[idx].func;
    // capability list
    unsigned int caps = pci_config_read(m->bus, m->dev, m->func, 0x34) & 0xFF;
    unsigned int notify_off = 0, notify_len = 0, notify_bar = 0;
    // пройти capability list: каждый cap - dword с reg=cap_ptr
    unsigned int cp = caps;
    while (cp) {
        unsigned int w = pci_config_read(m->bus, m->dev, m->func, cp & 0xFC);
        // байт0=cap_vndr, байт1=cap_next, байт3=cfg_type
        unsigned char vndr = w & 0xFF;
        unsigned char nxt = (w >> 8) & 0xFF;
        if (vndr != 0x09) { cp = nxt; continue; }   // не VNDR
        // читаем 16-байт struct (4 dword)
        // ... заполнить struct vm_cap
        // по cfg_type сохранить bar/offset/length
        // notify: читать notify_off_multiplier на offset 0x10
        cp = nxt;
    }
    // ... прочитать BAR регистры, identity-map каждый используемый BAR,
    //     вычислить common/notify/isr базы (bar_base + offset)
    return 0;
}
```

Правила для `vm_probe`:
- Capability struct читается по `cp` (уже dword-выровнен, т.к. `pci_config_read` требует reg & 0xFC); cap-следующий указатель — байт 1. Читать `struct vm_cap` из 4 чтений: `pci_config_read(bus,dev,func, cp)`, `+4`, `+8`, `+0xC`.
- Игнорировать не-VNDR cap'ы, идти по `cap_next`.
- Для cfg_type 1/2/3/4 сохранить `bar`, `offset`, `length`. Для notify (2) дополнительно прочитать `notify_off_multiplier` (u32) на `cap+0x10`.
- После скан-а: для каждого используемого BAR прочитать config BAR регистр (`0x10 + 4*bar`), маска `& ~0xF`, если 64-bit (`raw & 0x6 == 0x4`) → прочитать `bar+1` регистр как hi. `paging_identity_map(addr, length)` (округлять length до страницы вверх). `bar_base = addr`.
- `m->common = (volatile u8*)(bar_base + common.offset)`, аналогично notify/isr.
- `m->notify_multiplier = notify_multiplier`.

`vm_dev_init`: reset (device_status=0) → status=ACK|DRIVER → прочитать device features (select 0/1) → negotiated = features & supported → записать guest features (select 0/1) → status |= FEATURES_OK → проверить, что не FAILED (устройство сбросит FEATURES_OK и выставит FAILED=0x80 при неверной negotiate) → вернуть 0.

`vm_setup_queue(m, qidx, n)`: как legacy `virtio_setup_queue` (page_alloc_order, размещение desc/avail/used, init free_head/last_used), но вместо QUEUE_PFN — `queue_select=qidx`, `queue_size=n`, записать физ. адреса в `queue_desc_lo/hi`, `queue_avail_lo/hi`, `queue_used_lo/hi`, затем `queue_enable=1`. Прочитать `queue_notify_off` и сохранить в `m->notify_off[qidx]`.

`vm_ready`: `device_status |= DRIVER_OK`.

`vm_submit(m, qidx, head)`: записать desc в avail ring, `avail->idx++`, затем `*(volatile u16*)(m->notify + m->notify_off[qidx]*m->notify_multiplier) = qidx`.

`vm_alloc_desc`/`vm_desc_set`/`vm_free_chain`/`vm_used_pop`: скопировать логику из `drivers/virtio.c` (те же алгоритмы на `m->desc`/`m->avail`/`m->used`/`m->size`/`m->free_head`/`m->last_used`).

- [ ] **Step 3: Подключить в build**
- `Makefile` `KERNEL_OBJS`: добавить `drivers/virtio_modern.o`.

- [ ] **Step 4: Собрать**
- Run: `make`
- Expected: сборка чистая (функции пока никто не вызывает — только компиляция).

- [ ] **Step 5: Commit**

```bash
git add drivers/virtio_modern.c drivers/virtio_modern.h Makefile
git commit -m "virtio: add modern virtio-pci transport for non-transitional devices"
```

> Примечание: корректность `vm_probe`/`vm_setup_queue` проверяется в Task 2, когда GPU-драйвер реально шлёт команды. Если что-то не работает — отладить по QEMU (serial-лог + `info pci`).

---

### Task 2: Драйвер virtio-gpu (probe, init, ресурсы, flip) + selftest-лог

**Files:**
- Create: `drivers/virtio_gpu.h`
- Create: `drivers/virtio_gpu.c`
- Modify: `Makefile:12-23` (добавить `drivers/virtio_gpu.o` в `KERNEL_OBJS`)
- Modify: `kernel/kernel.c:126` (вызвать `vgu_init()` после `virtio_init()`)
- Create: `scripts/vguitest.py`
- Modify: `Makefile` `TESTS` (добавить `vguitest`)
- Test: `scripts/vguitest.py`

**Interfaces:**
- Consumes: `vm_probe`, `vm_dev_init`, `vm_setup_queue`, `vm_ready`, `vm_alloc_desc`, `vm_desc_set`, `vm_submit`, `vm_free_chain`, `vm_used_pop` (из `drivers/virtio_modern.h`, Task 2a); `GPU_BASE`/`GPU_STRIDE` (Task 1); `serial_print`, `memcpy`/`memset` (string.h).
- Produces:
  - `int vgu_init(void);` — probe+init, возвращает 0 при успехе, -1 нет GPU.
  - `int vgu_active(void);` — 1 после успешного init, 0 иначе.
  - `void vgu_flip(void);` — `SET_SCANOUT` на другой буфер + `RESOURCE_FLUSH`, меняет роли (front/back), ничего не делает при `!vgu_active()`.
  - `unsigned int vgu_back(void);` — адрес текущего back-буфера (тот, в который рисует WM), 0 при `!vgu_active()`.
  - `void vgu_info(unsigned int *w, unsigned int *h, unsigned int *pitch);` — 1024/768/4096 при активном GPU, 0/0/0 иначе.
  - selftest-строки в serial: `vgu: active`, `vgu: flip ok` (на каждый `SET_SCANOUT` при selftest).

**Детали протокола (modern virtio-gpu):**
- Командный заголовок 24 байта: `{u32 type; u32 flags; u64 fence_id; u32 ctx_id; u32 padding;}` packed.
- Команды (controlq, qidx 0). **Номера команд из `/usr/include/linux/virtio_gpu.h` (QEMU следует ему) — сверяться с ним, а не с вики/spec!**:
  - `RESOURCE_CREATE_2D` = `0x0101` (payload: resource_id, format, width, height)
  - `RESOURCE_ATTACH_BACKING` = `0x0106` (payload: resource_id, nr_entries, entries[] каждый `{u64 addr; u32 length; u32 padding;}`) — **НЕ 0x0105** (0x0105 = TRANSFER_TO_HOST_2D)
  - `RESOURCE_FLUSH` = `0x0104` (payload: **rect{x,y,w,h}, resource_id, padding** — rect первым!)
  - `SET_SCANOUT` = `0x0103` (payload: rect{x,y,w,h}, scanout_id, resource_id) — **НЕ 0x0002**
- Курсорные команды (cursorq, qidx 1, только в Task 3).
- Ответ: заголовок с `type == VIRTIO_GPU_RESP_OK_NODATA (0x1100)`.
- Формат `VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2`.
- Ресурсы: buf0=1, buf1=2.
- **Команда — цепочка из двух дескрипторов: `[cmd | resp]`** (первый — readable, второй `VRING_DESC_F_WRITE` с буфером под ответ). QEMU пишет ответ в writable descriptor; без него отвечает `INVALID_RESOURCE_ID` (0x1203).
- Синхронные команды: отправить через `vm_submit`, затем `vm_used_pop` с таймаутом-поллингом по `tick` (500 ticks), считать ответ из `resp_buf`.
- Командный буфер `cmd_buf` и ответный `resp_buf`: `static` в ядре (ниже 256 МБ, identity-mapped) — физический адрес = адрес переменной.
- **IRQ**: virtio-vga INTx level-triggered; установить `irq_install_handler(11, vgu_irq)`, где `vgu_irq` читает ISR (сброс). Без этого линия держит IRQ11 активным и может заморить polling.

- [ ] **Step 1: Написать драйвер-заголовок `drivers/virtio_gpu.h`**

```c
#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#define VGPU_FORMAT_B8G8R8X8 2
#define VGPU_CMD_RESOURCE_CREATE_2D  0x0101
#define VGPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VGPU_CMD_RESOURCE_FLUSH 0x0104
#define VGPU_CMD_SET_SCANOUT 0x0103
#define VGPU_RESP_OK_NODATA 0x1100

struct vgpu_hdr {
    unsigned int type;
    unsigned int flags;
    unsigned long long fence_id;
    unsigned int ctx_id;
    unsigned int padding;
} __attribute__((packed));

int vgu_init(void);
int vgu_active(void);
void vgu_flip(void);
unsigned int vgu_back(void);
void vgu_info(unsigned int *w, unsigned int *h, unsigned int *pitch);

#endif
```

- [ ] **Step 2: Написать тест `scripts/vguitest.py` (RED)**

Копия структуры `scripts/linhello.py` (serial-only, `serial_mode="file"`), бут с `-vga none -device virtio-vga,disable-modern=on`:

```python
#!/usr/bin/env python3
"""Boot the ISO with virtio-vga and assert the virtio-gpu driver initializes."""
import os, sys, time, re
sys.path.insert(0, os.path.dirname(__file__))
from qtest import QTest, ROOT

ISO = os.path.join(ROOT, "aos.iso")

def main():
    with QTest("vgu", serial_mode="file") as q:
        q.start(extra_args=["-vga", "none", "-device", "virtio-vga,disable-modern=on"])
        log = q.serial_read()
    assert "vgu: active" in log, "virtio-gpu driver did not activate"
    assert "vgu: flip ok" in log, "vgu selftest flip did not run"
    print("VGU TEST OK")

if __name__ == "__main__":
    main()
```

В `qtest.py` метод называется `serial_read()` (не `serial()`). Прогнать: `python3 scripts/vguitest.py` — Expected: FAIL, "virtio-gpu driver did not activate".

- [ ] **Step 3: Написать драйвер `drivers/virtio_gpu.c`**

Ключевые части (полный код ниже — вставка по секциям). **Modern API: `virtio_*` → `vm_*`; команды синхронные через polling used ring, но ставится IRQ11 handler для сброса level-triggered INTx (см. «Детали протокола»).**

```c
#include "virtio_gpu.h"
#include "virtio_modern.h"
#include "serial.h"
#include "string.h"
#include "interrupts.h"

extern volatile unsigned int tick;

#define GPU_BASE   0x04000000
#define GPU_STRIDE 0x300000        // 3 MiB per buffer
#define FB_W       1024
#define FB_H       768
#define FB_PITCH   (FB_W * 4)

static struct virtio_modern vgpu;
static int gpu_active;
static int front;                  // 0 or 1: currently displayed buffer
static unsigned char cmd_buf[16384] __attribute__((aligned(16)));
static unsigned char resp_buf[64] __attribute__((aligned(16)));

// Ack the device interrupt (reading the modern ISR status register clears it
// and deasserts INTx). Installed as the IRQ handler so a level-triggered line
// that raises mid-submit does not starve the driver's used-ring polling.
static void vgu_irq(void) {
    if (vgpu.isr) *(volatile unsigned char *)vgpu.isr;
}

// ---- low-level command submission (controlq, qidx 0) ----
// A command is a two-descriptor chain: [cmd | resp]. The device reads the
// request from the first (readable) descriptor and writes the response into
// the second (writable) one; the used entry carries the head id.
static int vgu_send(unsigned int qidx, unsigned int len) {
    unsigned int head = vm_alloc_desc(&vgpu, qidx);
    if (head == 0xFFFF) return -1;
    unsigned int rhead = vm_alloc_desc(&vgpu, qidx);
    if (rhead == 0xFFFF) { vm_free_chain(&vgpu, qidx, head); return -1; }
    vgpu.desc[head].addr = (unsigned int)cmd_buf;
    vgpu.desc[head].len = len;
    vgpu.desc[head].flags = VRING_DESC_F_NEXT;
    vgpu.desc[head].next = rhead;
    vgpu.desc[rhead].addr = (unsigned int)resp_buf;
    vgpu.desc[rhead].len = sizeof(struct vgpu_hdr);
    vgpu.desc[rhead].flags = VRING_DESC_F_WRITE;
    vm_submit(&vgpu, qidx, head);
    // poll used ring (device replies on the same queue)
    unsigned int start = tick;
    while ((int)(tick - start) < 500) {
        unsigned int id, rlen;
        if (vm_used_pop(&vgpu, qidx, &id, &rlen) == 0) {
            vm_free_chain(&vgpu, qidx, id);
            return 0;
        }
    }
    vm_free_chain(&vgpu, qidx, head);
    return -1;
}

static int vgu_cmd(unsigned int type, const void *payload, unsigned int plen) {
    struct vgpu_hdr *h = (struct vgpu_hdr *)cmd_buf;
    h->type = type; h->flags = 0; h->fence_id = 0;
    h->ctx_id = 0; h->padding = 0;
    unsigned int n = sizeof(struct vgpu_hdr) + plen;
    if (n > sizeof(cmd_buf)) return -1;
    if (plen) memcpy(cmd_buf + sizeof(struct vgpu_hdr), payload, plen);
    if (vgu_send(0, n) != 0) return -1;
    struct vgpu_hdr *rh = (struct vgpu_hdr *)resp_buf;
    return rh->type == VGPU_RESP_OK_NODATA ? 0 : -1;
}

// ---- resource create + attach backing ----
static int vgu_create(unsigned int rid, unsigned int w, unsigned int h) {
    struct { unsigned int rid, fmt, w, h; } p;
    p.rid = rid; p.fmt = VGPU_FORMAT_B8G8R8X8; p.w = w; p.h = h;
    return vgu_cmd(VGPU_CMD_RESOURCE_CREATE_2D, &p, sizeof(p));
}

static int vgu_attach(unsigned int rid, unsigned int base, unsigned int bytes) {
    // entries: one per 4 KiB page
    struct vgpu_mem_entry { unsigned long long addr; unsigned int len, pad; } *e;
    unsigned int npages = bytes / 4096;
    // build entries into the tail of cmd_buf via a second buffer
    // (cmd_buf is used by vgu_cmd, so build entries first, then copy)
    static unsigned char ents[768 * 16];
    e = (struct vgpu_mem_entry *)ents;
    for (unsigned int i = 0; i < npages; i++) {
        e[i].addr = (unsigned long long)(base + i * 4096);
        e[i].len = 4096;
        e[i].pad = 0;
    }
    struct { unsigned int rid, nr; } p;
    p.rid = rid; p.nr = npages;
    // assemble: header + p + entries
    struct vgpu_hdr *h = (struct vgpu_hdr *)cmd_buf;
    h->type = VGPU_CMD_RESOURCE_ATTACH_BACKING; h->flags = 0;
    h->fence_id = 0; h->ctx_id = 0; h->padding = 0;
    unsigned int off = sizeof(struct vgpu_hdr);
    memcpy(cmd_buf + off, &p, sizeof(p)); off += sizeof(p);
    memcpy(cmd_buf + off, ents, npages * 16); off += npages * 16;
    if (vgu_send(0, off) != 0) return -1;
    struct vgpu_hdr *rh = (struct vgpu_hdr *)resp_buf;
    return rh->type == VGPU_RESP_OK_NODATA ? 0 : -1;
}

static int vgu_scanout(unsigned int rid) {
    struct { unsigned int x, y, w, h; unsigned int scanout, resource; } p;
    p.x = 0; p.y = 0; p.w = FB_W; p.h = FB_H;
    p.scanout = 0; p.resource = rid;
    return vgu_cmd(VGPU_CMD_SET_SCANOUT, &p, sizeof(p));
}

static void vgu_flush(unsigned int rid) {
    struct { unsigned int x, y, w, h; unsigned int resource, pad; } p;
    p.x = 0; p.y = 0; p.w = FB_W; p.h = FB_H;
    p.resource = rid; p.pad = 0;
    vgu_cmd(VGPU_CMD_RESOURCE_FLUSH, &p, sizeof(p));
}

int vgu_init(void) {
    if (vm_probe(&vgpu, 0x1050) != 0) return -1;
    irq_install_handler(11, vgu_irq);
    if (vm_dev_init(&vgpu, VM_F_VERSION_1) != 0) return -1;
    if (vm_setup_queue(&vgpu, 0, 256) != 0) return -1;
    vm_ready(&vgpu);
    if (vgu_create(1, FB_W, FB_H) != 0) return -1;
    if (vgu_create(2, FB_W, FB_H) != 0) return -1;
    if (vgu_attach(1, GPU_BASE, GPU_STRIDE) != 0) return -1;
    if (vgu_attach(2, GPU_BASE + GPU_STRIDE, GPU_STRIDE) != 0) return -1;
    if (vgu_scanout(1) != 0) return -1;
    vgu_flush(1);
    front = 0;                          // displayed = buffer 0 (rid 1)
    gpu_active = 1;
    serial_print("vgu: active\n");
    // selftest: one flip and back, logging each flip
    vgu_flip();                         // -> "vgu: flip ok"
    vgu_flip();
    return 0;
}

int vgu_active(void) { return gpu_active; }

unsigned int vgu_back(void) {
    if (!gpu_active) return 0;
    return GPU_BASE + (front ^ 1) * GPU_STRIDE;
}

void vgu_info(unsigned int *w, unsigned int *h, unsigned int *pitch) {
    if (gpu_active) { *w = FB_W; *h = FB_H; *pitch = FB_PITCH; }
    else { *w = 0; *h = 0; *pitch = 0; }
}

void vgu_flip(void) {
    if (!gpu_active) return;
    front ^= 1;
    unsigned int rid = (front == 0) ? 1 : 2;
    if (vgu_scanout(rid) == 0) {
        vgu_flush(rid);
        serial_print("vgu: flip ok\n");
    }
}
```

> Примечание: в `vgu_attach` записи entries строятся в отдельный `static ents[]`, затем копируются в `cmd_buf` — т.к. `vgu_cmd`/`vgu_send` используют `cmd_buf`. Проверить `sizeof(ents)` ≥ 768×16 = 12288.

- [ ] **Step 4: Подключить в build и init**

- `Makefile` `KERNEL_OBJS`: добавить `drivers/virtio_gpu.o`.
- `kernel/kernel.c` `virtio_init()`-блок (после `virtio_init()` в `kernel_main` на строке 126): вызвать `vgu_init();` и залогировать результат:

```c
if (vgu_init() == 0)
    serial_print("virtio-gpu: framebuffer flip enabled\n");
else
    serial_print("virtio-gpu: not present, using VGA\n");
```

- [ ] **Step 5: Запустить тест (GREEN)**

Run: `make && python3 scripts/vguitest.py`
Expected: PASS ("VGU TEST OK"). Проверить в serial-логе строки `vgu: active`, `vgu: flip ok`.

- [ ] **Step 6: Регрессия**

Run: `make test-fast`
Expected: зелёные (не-GUI тесты не зависят от `-vga`; `vguitest` уже в `TESTS`).

- [ ] **Step 7: Commit**

```bash
git add drivers/virtio_gpu.c drivers/virtio_gpu.h Makefile kernel/kernel.c scripts/vguitest.py
git commit -m "virtio-gpu: add modern virtio-gpu driver with double-buffer flip"
```

---

### Task 3: Аппаратный курсор (cursor queue)

**Files:**
- Modify: `drivers/virtio_gpu.c`
- Modify: `drivers/virtio_gpu.h`
- Modify: `scripts/vguitest.py` (добавить assert на selftest)
- Test: `scripts/vguitest.py`

**Interfaces:**
- Consumes: `vgu_send`/`vgu_cmd` (Task 2); cursorq qidx 1.
- Produces: `void vgu_cursor(int x, int y, int visible);` — ставит/прячет аппаратный курсор (QEMU рисует поверх scanout); no-op при `!vgu_active()`.

- [ ] **Step 1: Добавить прототип в `drivers/virtio_gpu.h`**

```c
void vgu_cursor(int x, int y, int visible);
```

- [ ] **Step 2: Реализовать курсор в `drivers/virtio_gpu.c`**

```c
#define VGPU_CMD_UPDATE_CURSOR 0x0300
#define VGPU_CMD_MOVE_CURSOR   0x0301
#define VGPU_CURSOR_SIZE 64

static int cursor_initialized;
static unsigned char cursor_pix[VGPU_CURSOR_SIZE * VGPU_CURSOR_SIZE * 4]
    __attribute__((aligned(16)));

static void vgu_cursor_init(void) {
    // resource 3: 64x64 B8G8R8X8, filled with a two-color arrow + transparent border
    if (vgu_create(3, VGPU_CURSOR_SIZE, VGPU_CURSOR_SIZE) != 0) return;
    // backing: pages of cursor_pix (static, identity-mapped)
    static struct { unsigned long long addr; unsigned int len, pad; } ents[4];
    for (unsigned int i = 0; i < 4; i++) {
        ents[i].addr = (unsigned long long)((unsigned int)cursor_pix + i * 4096);
        ents[i].len = 4096; ents[i].pad = 0;
    }
    // build attach cmd manually (like vgu_attach but for cursor_pix)
    struct vgpu_hdr *h = (struct vgpu_hdr *)cmd_buf;
    h->type = VGPU_CMD_RESOURCE_ATTACH_BACKING; h->flags = 0;
    h->fence_id = 0; h->ctx_id = 0; h->padding = 0;
    unsigned int rid_nr[2] = {3, 4};
    memcpy(cmd_buf + sizeof(struct vgpu_hdr), rid_nr, 8);
    memcpy(cmd_buf + sizeof(struct vgpu_hdr) + 8, ents, 4 * 16);
    if (vgu_send(0, sizeof(struct vgpu_hdr) + 8 + 4 * 16) == 0 &&
        ((struct vgpu_hdr *)resp_buf)->type == VGPU_RESP_OK_NODATA) {
        // fill cursor pixels (white body + accent border, transparent elsewhere)
        memset(cursor_pix, 0, sizeof(cursor_pix));
        for (int yy = 0; yy < VGPU_CURSOR_SIZE; yy++)
            for (int xx = 0; xx < VGPU_CURSOR_SIZE; xx++) {
                // simple arrow outline: draw later — placeholder white box core
                if (xx >= 8 && xx < 56 && yy >= 8 && yy < 56) {
                    unsigned int off = (yy * VGPU_CURSOR_SIZE + xx) * 4;
                    cursor_pix[off + 0] = 0xFF;   // B
                    cursor_pix[off + 1] = 0xFF;   // G
                    cursor_pix[off + 2] = 0xFF;   // R (white)
                    cursor_pix[off + 3] = 0xFF;   // X
                }
            }
        cursor_initialized = 1;
    }
}

void vgu_cursor(int x, int y, int visible) {
    if (!gpu_active) return;
    if (!cursor_initialized) vgu_cursor_init();
    if (!cursor_initialized) return;
    // cursor commands go on cursorq (qidx 1)
    // payload: {hdr, pos{scanout_id, x, y, padding}, resource_id, hot_x, hot_y, padding}
    struct { unsigned int scanout, x, y, pad; unsigned int resource, hot_x, hot_y, pad2; } c;
    c.scanout = 0; c.x = (unsigned int)x; c.y = (unsigned int)y; c.pad = 0;
    c.resource = visible ? 3 : 0;
    c.hot_x = 0; c.hot_y = 0; c.pad2 = 0;
    struct vgpu_hdr *h = (struct vgpu_hdr *)cmd_buf;
    h->type = VGPU_CMD_UPDATE_CURSOR; h->flags = 0;
    h->fence_id = 0; h->ctx_id = 0; h->padding = 0;
    memcpy(cmd_buf + sizeof(struct vgpu_hdr), &c, sizeof(c));
    vgu_send(1, sizeof(struct vgpu_hdr) + sizeof(c));
}
```

> Курсорную команду отправляем в cursorq (qidx 1); `vgu_send` уже принимает `qidx` — но он вызывается для команд, которые читают ответ с used-ring. Cursor queue отвечает `VIRTIO_GPU_RESP_OK_NODATA` тоже через used ring, поэтому `vgu_send` подходит. Проверить, что cursorq настроена: добавить `vm_setup_queue(&vgpu, 1, 64)` в `vgu_init()` (Step 3 ниже).

- [ ] **Step 3: Настроить cursorq в `vgu_init()`**

В `drivers/virtio_gpu.c`, в `vgu_init()` после `vm_setup_queue(&vgpu, 0, 256)` добавить:

```c
if (vm_setup_queue(&vgpu, 1, 64) != 0) return -1;
```

- [ ] **Step 4: Selftest курсора при init**

В конце `vgu_init()` (после flips) добавить вызов, который оставляет курсор спрятанным, но выполняет команды:

```c
vgu_cursor(FB_W / 2, FB_H / 2, 1);
vgu_cursor(0, 0, 0);
serial_print("vgu: cursor ok\n");
```

- [ ] **Step 5: Добавить assert в `scripts/vguitest.py`**

В `main()` после проверки flip добавить:

```python
assert "vgu: cursor ok" in log, "vgu cursor selftest did not run"
```

- [ ] **Step 6: Прогнать тест и регрессию**

Run: `make && python3 scripts/vguitest.py && make test-fast`
Expected: `vguitest` PASS (в логе `vgu: cursor ok`), `make test-fast` зелёный.

- [ ] **Step 7: Commit**

```bash
git add drivers/virtio_gpu.c drivers/virtio_gpu.h scripts/vguitest.py
git commit -m "virtio-gpu: hardware cursor via cursor queue"
```

---

### Task 4: Syscall-интерфейс AOS_GPU_INFO / AOS_GPU_FLIP / AOS_CURSOR

**Files:**
- Modify: `programs/aosabi.h:32` (новые номера после `AOS_SPAWN_FDS 520`)
- Modify: `kernel/aos_gui.c` (case'ы + `AOS_FB_INFO` ветка GPU)
- Test: `scripts/vguitest.py` (не меняется; syscall'ы проверяются в Task 5 через WM) + `make`

**Interfaces:**
- Consumes: `vgu_active`, `vgu_back`, `vgu_info`, `vgu_cursor` (Task 2/3).
- Produces (для WM, Task 5):
  - `AOS_GPU_INFO (521)` — out-параметры `(addr, w, h, pitch, active)`.
  - `AOS_GPU_FLIP (522)` — `vgu_flip()`; возвращает 0.
  - `AOS_CURSOR (523)` — `vgu_cursor(x, y, visible)`; возвращает 0.
  - `AOS_FB_INFO` в GPU-режиме возвращает **back-буфер** (адрес, куда WM должен рисовать) — `vgu_back()`; в VGA-режиме — как сейчас.

- [ ] **Step 1: Добавить номера в `programs/aosabi.h`**

После `#define AOS_SPAWN_FDS     520` добавить:

```c
#define AOS_GPU_INFO       521
#define AOS_GPU_FLIP       522
#define AOS_CURSOR         523
```

- [ ] **Step 2: Добавить case'ы в `kernel/aos_gui.c`**

В `aos_gui_handler` перед `case AOS_GPU...` вставить (после `case AOS_FB_INFO`, строки 60-81, или в любом месте до `default`):

```c
case AOS_GPU_INFO: {
    unsigned int *addr = (unsigned int *)r->ebx;
    unsigned int *wdst = (unsigned int *)r->ecx;
    unsigned int *hdst = (unsigned int *)r->edx;
    unsigned int *pdst = (unsigned int *)r->esi;
    unsigned int *adst = (unsigned int *)r->edi;
    if ((addr == 0 || in_luser(addr, 4)) && (wdst == 0 || in_luser(wdst, 4)) &&
        (hdst == 0 || in_luser(hdst, 4)) && (pdst == 0 || in_luser(pdst, 4)) &&
        (adst == 0 || in_luser(adst, 4))) {
        unsigned int a, wv, hv, pv;
        vgu_info(&wv, &hv, &pv);
        a = vgu_back();
        if (addr) *addr = a;
        if (wdst) *wdst = wv;
        if (hdst) *hdst = hv;
        if (pdst) *pdst = pv;
        if (adst) *adst = vgu_active();
        r->eax = 0;
    } else {
        r->eax = -5;
    }
    break;
}
case AOS_GPU_FLIP:
    vgu_flip();
    r->eax = 0;
    break;
case AOS_CURSOR:
    vgu_cursor((int)r->ebx, (int)r->ecx, (int)r->edx);
    r->eax = 0;
    break;
```

- [ ] **Step 3: Переключить `AOS_FB_INFO` на back-буфер в GPU-режиме**

В `case AOS_FB_INFO` (строки 60-81) заменить `vga_get_fb_dimensions(...)` на:

```c
unsigned int a, wv, hv, pv, bv;
if (vgu_active()) {
    a = vgu_back(); wv = 1024; hv = 768; pv = 4096; bv = 32;
} else {
    vga_get_fb_dimensions(&a, &wv, &hv, &pv, &bv);
}
```

> Примечание: `AOS_FB_INFO` в GPU-режиме возвращает **текущий** back-буфер. После `AOS_GPU_FLIP` роли меняются — WM обязан перезапросить адрес через `AOS_FB_INFO` (или `AOS_GPU_INFO`) перед следующим кадром (см. Task 5).

- [ ] **Step 4: Добавить user-обёртки в `programs/aosabi.h`**

В блок `#ifndef __AOS_KERNEL__` добавить (рядом с другими wrappers):

```c
static __attribute__((unused)) int aos_gpu_info(unsigned int *addr, unsigned int *w,
                       unsigned int *h, unsigned int *pitch, unsigned int *active) {
    return aos_syscall(AOS_GPU_INFO, (int)addr, (int)w, (int)h, (int)pitch, (int)active);
}
static __attribute__((unused)) void aos_gpu_flip(void) {
    aos_syscall(AOS_GPU_FLIP, 0, 0, 0, 0, 0);
}
static __attribute__((unused)) void aos_cursor(int x, int y, int visible) {
    aos_syscall(AOS_CURSOR, x, y, visible, 0, 0);
}
```

- [ ] **Step 5: Собрать и проверить**

Run: `make`
Expected: сборка чистая. Поведение syscall'ов проверим в Task 5 через WM.

- [ ] **Step 6: Commit**

```bash
git add programs/aosabi.h kernel/aos_gui.c
git commit -m "syscall: add AOS_GPU_INFO/FLIP/CURSOR extension syscalls"
```

---

### Task 5: WM — GPU-путь рендера

**Files:**
- Modify: `programs/musl/wm.c`
- Test: `scripts/vguitest.py` (расширить) + ручной прогон `make run`

**Interfaces:**
- Consumes: `aos_gpu_info`, `aos_gpu_flip`, `aos_cursor`, `aos_fb_info` (Task 4).
- Produces: WM рендерит в back-буфер и флипает кадр; курсор через `aos_cursor`; VGA-fallback без изменений.

**Ключевая идея:** `fb_addr` уже глобальный в `wm.c` и используется всеми draw-функциями напрямую (`(unsigned int *)fb_addr`). В GPU-режиме:
- При старте: `aos_gpu_info` → если `active`, то `fb_addr = back-адрес`, `fb_pitch = 4096` (глобальные уже есть: `fb_w`, `fb_h`, `fb_pitch`).
- Рисование идёт в back-буфер как сейчас (невидимый, т.к. scanout показывает front).
- После каждого завершённого кадра (все места, где сейчас `composite()` / `composite_rect(...)`) → `aos_gpu_flip()` и затем перезапрос back-адреса (`aos_fb_info`).
- Курсор: вместо рисования в VRAM — `aos_cursor(mx, my, visible)`; `update_cursor` в GPU-режиме не трогает `fb` и не сохраняет/восстанавливает снапшот.

- [ ] **Step 1: Определить GPU-режим в начале `main()`**

В `wm.c` `main()` после получения fb-инфо (строка ~989 `aos_fb_info(&fb_addr, ...)`) добавить:

```c
unsigned int gpu_w = 0, gpu_h = 0, gpu_pitch = 0, gpu_active = 0;
aos_gpu_info(&fb_addr, &gpu_w, &gpu_h, &gpu_pitch, &gpu_active);
if (gpu_active) {
    fb_w = gpu_w; fb_h = gpu_h; fb_pitch = gpu_pitch;
    serial("wm: gpu mode, back="); serial_hex(fb_addr); serial("\n");
} else {
    serial("wm: vga mode\n");
}
```

> Если в wm.c нет `serial()`-хелперов — писать в serial через существующий механизм (проверить, как wm.c логирует: `dprintf`/`serial_*`); иначе опустить логирование.

- [ ] **Step 2: Глобальная переменная GPU-режима**

Добавить рядом с `static int has_cur, cur_x, cur_y;` (строка 283):

```c
static int gpu_mode;
```

И в `main()` при `gpu_active` выставить `gpu_mode = 1`.

- [ ] **Step 3: Флип + перезапрос back после кадра**

Добавить хелпер:

```c
static void gpu_present(void) {
    if (!gpu_mode) return;
    aos_gpu_flip();
    // roles swapped: re-fetch the new back buffer
    unsigned int a, w, h, p, act;
    aos_fb_info(&a, &w, &h, &p, &act);
    fb_addr = a;
    fb_pitch = p;
}
```

Вызвать `gpu_present()` сразу после каждого полного перерисовывания кадра:
- после `composite();` в блоке `if (redraw)` (строка 1176-1181);
- после `composite_rect(...)` в `MSG_UPDATE` (строка 1074) и при drag (строка 1166).

> Частичные обновления `composite_rect` тоже приводят к flip — это ок и даже лучше (атомарность сохраняется, каждый прямоугольник-кадр цельный). Все вызовы в одном месте, например обернуть финальную часть главного цикла: если `gpu_mode` и был любой redraw/composite за итерацию — `gpu_present()`.

- [ ] **Step 4: Курсор через syscall в GPU-режиме**

В `update_cursor(int mx, int my)` (строки 887-899) в начале:

```c
static void update_cursor(int mx, int my) {
    if (gpu_mode) {
        int vis = (mx >= 0 && mx < (int)fb_w && my >= 0 && my < (int)fb_h);
        aos_cursor(mx, my, vis);
        has_cur = 0;
        return;
    }
    // ... существующий VGA-путь
}
```

> В GPU-режиме курсор не рисуется в кадр и не требует снапшотов: `save_snap`/`restore_snap`/`draw_cursor`/`cursor_overlaps` в GPU-пути не вызываются. Код VGA-пути (`has_cur`, `cursor_overlaps` в `composite_rect` и др.) остаётся для fallback — он не активен при `gpu_mode`.

- [ ] **Step 5: Проверить, что `composite_rect`/`composite` пишут в `fb_addr` (уже так)**

Проверить, что все draw-функции (fb_fill/fb_text/mcpy blits окон) используют глобальный `fb_addr`/`fb_pitch`, а не закэшированный указатель. Если где-то `unsigned int *fb = (unsigned int *)fb_addr;` вычисляется внутри функции — всё ок (Task 5 Step 3 обновляет `fb_addr` до вызова). Если какой-то кэш указателя держится между кадрами — поправить на перезапрос через `fb_addr`.

- [ ] **Step 6: Ручной визуальный тест**

Run: `make run` (GTK, `-device virtio-vga,disable-modern=on` добавить в Makefile `run` — см. Task 6, либо временно).
Expected: рабочий стол, окна term/clock, курсор следует за мышью без мерцания и «призраков», окна таскаются плавно, ничего не мигает.

- [ ] **Step 7: Расширить `scripts/vguitest.py`**

Добавить скриншот-проверку desktop (по образцу `guitester.py`: `q.screenshot()`/HMP `screendump` через `q.mon`):

```python
def desktop_ok(q):
    q.screenshot()          # -> PPM в self.ppm
    px = ppm_pixel(q.ppm, 700, 400)   # область gradient (не поверх окон)
    return px[0] > 20 and px[1] > 20 and px[2] > 20   # не чёрный экран

# в main():
with QTest("vgu", serial_mode="file") as q:
    q.start(extra_args=["-device", "virtio-vga,disable-modern=on"])
    time.sleep(4)
    ok = desktop_ok(q)
    log = q.serial()
assert ok, "desktop did not render through virtio-gpu scanout"
```

Проверить API `screenshot()` в `qtest.py` (guitester.py: как делается скриншот — HMP `screendump`). Скорректировать координаты пикселя под реальную раскладку (gradient где-то на экране). Expected: PASS после Step 6.

- [ ] **Step 8: Commit**

```bash
git add programs/musl/wm.c scripts/vguitest.py
git commit -m "wm: render through virtio-gpu double buffer with atomic flip"
```

---

### Task 6: QEMU-инфраструктура и пиксельные GUI-тесты на GPU

**Files:**
- Modify: `Makefile` `run` (строка ~156) и `scripts/qemu-debug.sh`
- Modify: `scripts/guitester.py`, `scripts/notepadtest.py`, `scripts/configtest.py` (добавить `-device virtio-vga,disable-modern=on` в extra_args)
- Test: `scripts/guitester.py`, `scripts/notepadtest.py`, `scripts/configtest.py`, `make test-fast`, `make test`

**Interfaces:**
- Consumes: WM GPU-путь (Task 5), `vgu_active` fallback.
- Produces: все три пиксельных GUI-теста гоняются на GPU scanout; `make run`/`debug` показывают GPU.

- [ ] **Step 1: Добавить virtio-vga в `make run`**

В `Makefile` `run:` добавить в qemu-строку (после `-cdrom $<`):

```
  -device virtio-vga,disable-modern=on \
```

- [ ] **Step 2: Добавить virtio-vga в `scripts/qemu-debug.sh`**

В qemu-строку (перед `"$@"`) добавить `-device virtio-vga,disable-modern=on`.

- [ ] **Step 3: GUI-тесты на GPU**

В `scripts/guitester.py`, `scripts/notepadtest.py`, `scripts/configtest.py` в `QTest(...)`/`q.start(...)` передать `extra_args=["-device", "virtio-vga,disable-modern=on"]`. Для `configtest.py` есть два запуска (обычный и с диском `disk_extra()`) — объединить оба списка.

- [ ] **Step 4: Прогнать GUI-тесты**

Run: `python3 scripts/guitester.py && python3 scripts/notepadtest.py && python3 scripts/configtest.py`
Expected: все PASS (пиксели desktop/окон/темы через GPU scanout). Если пиксель не совпадает — проверить, что WM в GPU-режиме рисует ровно те же цвета (fallback-координаты тестов те же; формат `B8G8R8X8` не меняет отображение).

- [ ] **Step 5: Полная регрессия**

Run: `make test-fast` и, если есть время, `make test`
Expected: зелёные. `vguitest` в `TESTS`; не-GUI тесты без `-vga` не зависят от GPU (fallback-путь не активируется, т.к. драйвера нет → `vgu_active()==0`, WM идёт по VGA-пути).

- [ ] **Step 6: Обновить AGENTS.md**

Добавить раздел про virtio-gpu (по образцу «Block devices & AHCI»): принцип двойного буфера + `SET_SCANOUT` flip, аппаратный курсор, окно `0x04000000..0x04600000`, syscall'ы 521-523, `-device virtio-vga,disable-modern=on`, `scripts/vguitest.py`, fallback. Русский язык в тексте не требуется (AGENTS.md на английском), но соблюдать стиль.

- [ ] **Step 7: Commit**

```bash
git add Makefile scripts/qemu-debug.sh scripts/guitester.py scripts/notepadtest.py scripts/configtest.py AGENTS.md
git commit -m "qemu: boot with virtio-vga and run pixel GUI tests on the GPU path"
```

---

### Task 7: Финализация — полный прогон

**Files:**
- None (только верификация)

- [ ] **Step 1: Чистая пересборка**

Run: `make clean && make`
Expected: чистая сборка, без варнингов/ошибок.

- [ ] **Step 2: Все GPU-тесты**

Run: `python3 scripts/vguitest.py && python3 scripts/guitester.py && python3 scripts/notepadtest.py && python3 scripts/configtest.py`
Expected: все PASS.

- [ ] **Step 3: Полная регрессия**

Run: `make test-fast`
Expected: зелёный.

- [ ] **Step 4: Fallback-проверка (без virtio-vga)**

Run: `make clean && make` — затем запустить один не-GUI тест без `-device virtio-vga` (например `python3 scripts/linhello.py`) и один GUI-тест, у которого временно убран `extra_args`, убедиться что VGA-fallback работает (WM рисует в VRAM, софтверный курсор). Затем вернуть `extra_args`.

Expected: fallback живёт — драйвер не найден, `vgu_active()==0`, WM на VGA-пути, нет паники.

- [ ] **Step 5: Итоговый commit (если были правки)**

```bash
git add -A
git commit -m "virtio-gpu: final fixes from full regression run"
```

(Пропустить, если правок не было.)