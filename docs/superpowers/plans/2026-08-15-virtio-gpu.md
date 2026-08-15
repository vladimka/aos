# VirtIO-GPU Аппаратное Видеоускорение — План Реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Устранить мерцание экрана при обновлении, переведя рендер WM на virtio-gpu с атомарным `SET_SCANOUT` (двойной буфер) и аппаратным курсором, сохранив полный VGA-fallback.

**Architecture:** Ядро создаёт 2 framebuffer-ресурса virtio-gpu (1024×768×32, `B8G8R8X8_UNORM`, backing = обычные страницы RAM из user-accessible окна `0x04000000..0x04600000`). WM пишет в «back»-буфер (identity-адрес, который возвращает `AOS_FB_INFO` в GPU-режиме), затем `AOS_GPU_FLIP` атомарно переключает кадр; курсор уходит через `AOS_CURSOR` (cursor queue, QEMU рисует поверх). VGA остаётся для boot и как fallback (`vgu_active()==0`).

**Tech Stack:** C (ядро, `-ffreestanding -m32`), legacy virtio-pci транспорт (`drivers/virtio.c`, `disable-modern=on`), virtio-gpu протокол, QEMU `-device virtio-vga,disable-modern=on`, Python QEMU-тесты (`scripts/qtest.py`).

**Spec:** `docs/superpowers/specs/2026-08-15-virtio-gpu-design.md`

## Global Constraints

- Язык: код/коммиты на английском, общение/доки на русском (AGENTS.md).
- Фиксированное разрешение 1024×768×32 (`B8G8R8X8_UNORM`), один scanout, один дисплей.
- virtio-gpu legacy PCI id `0x1040`; наш legacy транспорт (`I/O BAR`, без MSI-X).
- Двойной буфер: 2 буфера по 3 МБ в окне `0x04000000..0x04600000` (PDE 16–17), identity-map + user bit; окно резервируется в `pmm.c`.
- Формат пикселя: наш рендер пишет u32 `0x00RRGGBB` (little-endian: байты BB GG RR 00) — совпадает с `VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM` (alpha игнорируется, в отличие от `B8G8R8A8` где alpha=0 дал бы прозрачные пиксели).
- Валидация пользовательских указателей через `in_luser` (`kernel/aos_gui.c`), ошибка `-5`.
- Без GPU (`vgu_active()==0`): полный fallback — WM работает как сейчас, пиксельные GUI-тесты без `-vga virtio` не гоняются.
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

### Task 2: Драйвер virtio-gpu (probe, init, ресурсы, flip) + selftest-лог

**Files:**
- Create: `drivers/virtio_gpu.h`
- Create: `drivers/virtio_gpu.c`
- Modify: `drivers/virtio.h:7` (добавить `VIRTIO_DEV_GPU 0x1040`)
- Modify: `Makefile:13` (добавить `drivers/virtio_gpu.o` в `KERNEL_OBJS`)
- Modify: `kernel/kernel.c:126` (вызвать `vgu_init()` после `virtio_init()`)
- Create: `scripts/vguitest.py`
- Modify: `Makefile` `TESTS` (добавить `vguitest`)
- Test: `scripts/vguitest.py`

**Interfaces:**
- Consumes: `virtio_probe_pci`, `virtio_dev_init`, `virtio_setup_queue`, `virtio_alloc_desc`, `virtio_desc_set`, `virtio_submit`, `virtio_free_chain`, `virtio_used_pop` (из `drivers/virtio.h`); `GPU_BASE`/`GPU_STRIDE` (Task 1).
- Produces:
  - `int vgu_init(void);` — probe+init, возвращает 0 при успехе, -1 нет GPU.
  - `int vgu_active(void);` — 1 после успешного init, 0 иначе.
  - `void vgu_flip(void);` — `SET_SCANOUT` на другой буфер + `RESOURCE_FLUSH`, меняет роли (front/back), ничего не делает при `!vgu_active()`.
  - `unsigned int vgu_back(void);` — адрес текущего back-буфера (тот, в который рисует WM), 0 при `!vgu_active()`.
  - `void vgu_info(unsigned int *w, unsigned int *h, unsigned int *pitch);` — 1024/768/4096 при активном GPU, 0/0/0 иначе.
  - selftest-строки в serial: `vgu: active`, `vgu: flip ok` (на каждый `SET_SCANOUT` при selftest).

**Детали протокола (legacy virtio-gpu):**
- Командный заголовок 24 байта: `{u32 type; u32 flags; u64 fence_id; u32 ctx_id; u32 padding;}` packed.
- Команды (controlq, qidx 0): `RESOURCE_CREATE_2D` (0x0101, payload: resource_id, format, width, height), `RESOURCE_ATTACH_BACKING` (0x0105, payload: resource_id, nr_entries, entries[] каждый `{u64 addr; u32 length; u32 padding;}`), `RESOURCE_FLUSH` (0x0104, payload: resource_id, padding, rect{x,y,w,h}), `SET_SCANOUT` (0x0002, payload: rect{x,y,w,h}, scanout_id, resource_id).
- Курсорные команды (cursorq, qidx 1, только в Task 3).
- Ответ: заголовок с `type == VIRTIO_GPU_RESP_OK_NODATA (0x1100)`.
- Формат `VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2`.
- Ресурсы: buf0=1, buf1=2.
- Синхронные команды: отправить через `virtio_submit`, затем `virtio_used_pop` с таймаутом-поллингом (10000 итераций `sti;hlt;cli` или простой цикл), считать ответ по адресу командного буфера.
- Командный буфер и entries-массив: `static` в ядре (ниже 256 МБ, identity-mapped) — физический адрес = адрес переменной.

- [ ] **Step 1: Написать драйвер-заголовок `drivers/virtio_gpu.h`**

```c
#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#define VGPU_FORMAT_B8G8R8X8 2
#define VGPU_CMD_RESOURCE_CREATE_2D  0x0101
#define VGPU_CMD_RESOURCE_ATTACH_BACKING 0x0105
#define VGPU_CMD_RESOURCE_FLUSH 0x0104
#define VGPU_CMD_SET_SCANOUT 0x0002
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

Копия структуры `scripts/linhello.py` (serial-only, `serial_mode="file"`), бут с `-device virtio-vga,disable-modern=on`:

```python
#!/usr/bin/env python3
"""Boot the ISO with virtio-vga and assert the virtio-gpu driver initializes."""
import os, sys, time, re
sys.path.insert(0, os.path.dirname(__file__))
from qtest import QTest, ROOT

ISO = os.path.join(ROOT, "aos.iso")

def main():
    with QTest("vgu", serial_mode="file") as q:
        q.start(extra_args=["-device", "virtio-vga,disable-modern=on"])
        log = q.serial()
    assert "vgu: active" in log, "virtio-gpu driver did not activate"
    assert "vgu: flip ok" in log, "vgu selftest flip did not run"
    print("VGU TEST OK")

if __name__ == "__main__":
    main()
```

Проверить, что в `qtest.py` есть метод `serial()` (если нет — читать `self.ser` файл). Прогнать: `python3 scripts/vguitest.py` — Expected: FAIL, "virtio-gpu driver did not activate".

- [ ] **Step 3: Написать драйвер `drivers/virtio_gpu.c`**

Ключевые части (полный код ниже — вставка по секциям):

```c
#include "virtio_gpu.h"
#include "virtio.h"
#include "serial.h"
#include "string.h"

#define GPU_BASE   0x04000000
#define GPU_STRIDE 0x300000        // 3 MiB per buffer
#define FB_W       1024
#define FB_H       768
#define FB_PITCH   (FB_W * 4)

static struct virtio_dev vgpu;
static int gpu_active;
static int front;                  // 0 or 1: currently displayed buffer
static unsigned char cmd_buf[16384] __attribute__((aligned(16)));
static unsigned int ncmd;

// ---- low-level command submission (controlq, qidx 0) ----
static int vgu_send(unsigned int qidx, unsigned int len) {
    unsigned int head = virtio_alloc_desc(&vgpu, qidx);
    if (head == 0xFFFF) return -1;
    virtio_desc_set(&vgpu, qidx, head, (unsigned int)cmd_buf, len, 0);
    virtio_submit(&vgpu, qidx, head);
    // poll used ring (device replies on the same queue)
    for (unsigned int i = 0; i < 1000000; i++) {
        unsigned int id, rlen;
        if (virtio_used_pop(&vgpu, qidx, &id, &rlen) == 0) {
            virtio_free_chain(&vgpu, qidx, id);
            return 0;
        }
    }
    virtio_free_chain(&vgpu, qidx, head);
    return -1;
}

static int vgu_cmd(unsigned int type, const void *payload, unsigned int plen) {
    struct vgpu_hdr *h = (struct vgpu_hdr *)cmd_buf;
    h->type = type; h->flags = 0; h->fence_id = 0;
    h->ctx_id = 0; h->padding = 0;
    ncmd = sizeof(struct vgpu_hdr) + plen;
    if (ncmd > sizeof(cmd_buf)) return -1;
    if (plen) memcpy(cmd_buf + sizeof(struct vgpu_hdr), payload, plen);
    if (vgu_send(0, ncmd) != 0) return -1;
    return h->type == VGPU_RESP_OK_NODATA ? 0 : -1;
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
    return h->type == VGPU_RESP_OK_NODATA ? 0 : -1;
}

static int vgu_scanout(unsigned int rid) {
    struct { unsigned int x, y, w, h; unsigned int scanout, resource; } p;
    p.x = 0; p.y = 0; p.w = FB_W; p.h = FB_H;
    p.scanout = 0; p.resource = rid;
    return vgu_cmd(VGPU_CMD_SET_SCANOUT, &p, sizeof(p));
}

static void vgu_flush(unsigned int rid) {
    struct { unsigned int rid, pad; unsigned int x, y, w, h; } p;
    p.rid = rid; p.pad = 0; p.x = 0; p.y = 0; p.w = FB_W; p.h = FB_H;
    vgu_cmd(VGPU_CMD_RESOURCE_FLUSH, &p, sizeof(p));
}

int vgu_init(void) {
    if (virtio_probe_pci(&vgpu, 0x1040) != 0) return -1;
    if (virtio_dev_init(&vgpu, 0, 0) != 0) return -1;
    if (virtio_setup_queue(&vgpu, 0, 256) != 0) return -1;
    virtio_register(&vgpu);
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

- `drivers/virtio.h`: после `#define VIRTIO_DEV_RNG 0x1005` добавить `#define VIRTIO_DEV_GPU 0x1040`.
- `Makefile` `KERNEL_OBJS`: добавить `drivers/virtio_gpu.o`.
- `kernel/kernel.c` `virtio_init()`-блок (после `block_init()` в `virtio_init` или в `kernel_main` после `virtio_init()`): вызвать `vgu_init();` и залогировать результат:

```c
if (vgu_init() == 0)
    serial_print("virtio-gpu: framebuffer flip enabled\n");
else
    serial_print("virtio-gpu: not present, using VGA\n");
```

В `kernel_main` `virtio_init()` вызывается на строке 126 — `vgu_init()` поставить сразу после неё (или внутри `virtio_init` в `drivers/virtio.c:187` после `block_init()`).

- [ ] **Step 5: Запустить тест (GREEN)**

Run: `make && python3 scripts/vguitest.py`
Expected: PASS ("VGU TEST OK"). Проверить в serial-логе строки `vgu: active`, `vgu: flip ok`.

- [ ] **Step 6: Регрессия**

Run: `make test-fast`
Expected: зелёные (не-GUI тесты не зависят от `-vga`; `vguitest` уже в `TESTS`).

- [ ] **Step 7: Commit**

```bash
git add drivers/virtio_gpu.c drivers/virtio_gpu.h drivers/virtio.h Makefile kernel/kernel.c scripts/vguitest.py
git commit -m "virtio-gpu: add legacy virtio-gpu driver with double-buffer flip"
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
        h->type == VGPU_RESP_OK_NODATA) {
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
    struct { unsigned int x, y; unsigned int resource; unsigned int hot_x, hot_y; } c;
    c.x = (unsigned int)x; c.y = (unsigned int)y;
    c.resource = visible ? 3 : 0;
    c.hot_x = 0; c.hot_y = 0;
    struct vgpu_hdr *h = (struct vgpu_hdr *)cmd_buf;
    h->type = VGPU_CMD_UPDATE_CURSOR; h->flags = 0;
    h->fence_id = 0; h->ctx_id = 0; h->padding = 0;
    memcpy(cmd_buf + sizeof(struct vgpu_hdr), &c, sizeof(c));
    vgu_send(1, sizeof(struct vgpu_hdr) + sizeof(c));
}
```

> Курсорную команду отправляем в cursorq (qidx 1); `vgu_send` уже принимает `qidx` — но он вызывается для команд, которые читают ответ с used-ring. Cursor queue отвечает `VIRTIO_GPU_RESP_OK_NODATA` тоже через used ring, поэтому `vgu_send` подходит. Проверить, что cursorq настроена: добавить `virtio_setup_queue(&vgpu, 1, 64)` в `vgu_init()` (Step 3 ниже).

- [ ] **Step 3: Настроить cursorq в `vgu_init()`**

В `drivers/virtio_gpu.c`, в `vgu_init()` после `virtio_setup_queue(&vgpu, 0, 256)` добавить:

```c
if (virtio_setup_queue(&vgpu, 1, 64) != 0) return -1;
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