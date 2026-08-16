# Аппаратное видеоускорение через VirtIO-GPU

Дата: 2026-08-15
Статус: черновик
Цель: устранить мерцание экрана при обновлении, переведя рендер WM на virtio-gpu с атомарным сменой кадра (`SET_SCANOUT`) и аппаратным курсором.

## Проблема

Сейчас WM (`programs/musl/wm.c`) композитует прямо в VRAM (физический фреймбуфер из MB2-тега, `fb_addr`): все `composite_rect`-обновления пишут прямоугольники сразу в отображаемую поверхность. В QEMU под TCG кадр «собирается» частями — видны промежуточные состояния (мерцание). Курсор рисуется как оверлей в VRAM со снапшотом и перерисовывается после каждого обновления.

QEMU предоставляет `virtio-gpu` (`virtio-vga`), где guest создаёт ресурсы (2D-поверхности), прикрепляет к ним страницы RAM (`RESOURCE_ATTACH_BACKING`) и переключает видимую поверхность командой `SET_SCANOUT` — кадр меняется целиком и атомарно. Это устраняет мерцание принципиально, плюс даёт аппаратный курсор (cursor queue: `UPDATE_CURSOR`/`MOVE_CURSOR`).

## Архитектура

Новый драйвер `drivers/virtio_gpu.c` в ядре поверх **modern virtio-pci транспорта** (capability-based, memory BAR). QEMU 10.2.1 не предоставляет virtio-gpu legacy I/O BAR (virtio-gpu — non-transitional устройство, PCI id `0x1050`), поэтому legacy-транспорт `drivers/virtio.c` неприменим; нужен новый `drivers/virtio_modern.c`. WM по-прежнему пишет пиксели в обычную RAM (backing-страницы), но кадр уходит на экран атомарно через flip. VGA остаётся только для boot: до инициализации GPU экран показывает VGA-фреймбуфер из MB2-тега; после старта WM виден GPU scanout.

### Компоненты

1. **Драйвер `drivers/virtio_gpu.c`**
   - Probe: `vm_probe(&gpu, 0x1050)` (modern GPU id = 0x1040 + device_id 16; QEMU `virtio-vga` отдаёт `1af4:1050`).
   - Инициализация: `vm_dev_init`, `vm_ready`, 2 virtqueue — control (qidx 0) и cursor (qidx 1). Команды (значения из `/usr/include/linux/virtio_gpu.h`, которому следует QEMU):
     - `RESOURCE_CREATE_2D` (0x0101) ×2 — два буфера `width=1024, height=768`, формат `VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM` (=2; little-endian u32 `0x00RRGGBB` = байты BB GG RR 00 — совпадает с нашим рендером; alpha игнорируется, в отличие от `B8G8R8A8` где alpha=0 дал бы прозрачные пиксели).
     - `RESOURCE_ATTACH_BACKING` (**0x0106**, НЕ 0x0105 — 0x0105 это `TRANSFER_TO_HOST_2D`) — прикрепить страницы буферов (физические адреса; виртуальный = физический за счёт identity-map). Payload: `{hdr, resource_id, nr_entries, entries[]}`.
     - `SET_SCANOUT` (**0x0103**, НЕ 0x0002) — payload `{hdr, rect{x,y,w,h}, scanout_id, resource_id}`.
     - `RESOURCE_FLUSH` (0x0104) — payload `{hdr, rect{x,y,w,h}, resource_id, padding}` (rect ПЕРВЫМ, потом resource_id — НЕ `{rid,pad,x,y,w,h}`).
   - **Двухдескрипторная цепочка** для каждой команды: `[cmd | resp]` — первый дескриптор читаемый (запрос), второй writable (`VRING_DESC_F_WRITE`) под ответ; QEMU пишет ответ в writable in_sg. Ответ читается из `resp_buf`; проверяется `type == VIRTIO_GPU_RESP_OK_NODATA (0x1100)`.
   - **IRQ11**: virtio-vga INTx level-triggered, срабатывает посреди submit и голодает polling-цикл, поэтому ставится `irq_install_handler(11, vgu_irq)`, читающий ISR (чтение сбрасывает INTx). Все команды остаются синхронными (polling used ring).
   - API: `vgu_init()`, `vgu_flip()`, `vgu_cursor(x, y)`, `vgu_active()`, `vgu_back()`, `vgu_info()`.
   - `vgu_flip()`: `SET_SCANOUT(0, other_buf)` + `RESOURCE_FLUSH` на новый буфер; меняет роли буферов. Синхронный (ожидание ответа через used ring).
   - `vgu_cursor()`: при первом вызове `UPDATE_CURSOR` с курсор-битмапом 64×64 (встроенный art, два цвета + прозрачность), далее `MOVE_CURSOR`. `visible=0` → `UPDATE_CURSOR` с `resource_id=0` (скрыть). Payload курсора: `{hdr, pos{scanout_id, x, y, padding}, resource_id, hot_x, hot_y, padding}` (см. `struct virtio_gpu_update_cursor`).
   - Память: 2 буфера по 3 МБ в user-accessible окне (см. ниже). Формат записи тот же, что сейчас (32bpp).

1a. **Modern virtio-pci транспорт (`drivers/virtio_modern.c/.h`)**
   - Новый транспорт для virtio-gpu: capability-based modern интерфейс (common cfg / notify / isr / device cfg через BAR'ы + VNDR-capabilities), API `vm_*`.
   - BAR'ы virtio-vga (QEMU 10.2.1) выше 256 МБ (BAR0=VRAM, BAR2=64-bit prefetchable, BAR4=32-bit) → identity-map через `paging_identity_map` (как framebuffer/AHCI ABAR).
   - `struct virtio_modern`: указатели на common/notify/isr/devcfg (mmio, volatile), `notify_off_multiplier`, vq (`desc`/`avail`/`used`, `size`/`free_head`/`last_used`).
   - Регистры common cfg (Linux/QEMU layout, `/usr/include/linux/virtio_pci.h`): `device_feature_select` 0x00, `device_feature` 0x04, `guest_feature_select` 0x08, `guest_feature` 0x0c, `msix_config` 0x10, `num_queues` 0x12, `device_status` 0x14 (u8), `config_generation` 0x15, `queue_select` 0x16, `queue_size` 0x18, `queue_msix_vector` 0x1a, `queue_enable` 0x1c, `queue_notify_off` 0x1e, `queue_desc_lo/hi` 0x20/0x24, `queue_avail_lo/hi` 0x28/0x2c, `queue_used_lo/hi` 0x30/0x34.
   - Feature-negotiation: выбрать `VIRTIO_F_VERSION_1` (bit 32) через select/feature пары. Современные устройства определяются наличием VNDR-capability.
   - Queue setup: `queue_select=qidx`, `queue_size=n`, записать три физических адреса (desc/avail/used), `queue_enable=1`. Split vring layout (desc/avail/used) совпадает с legacy, поэтому логика `vm_alloc_desc`/`vm_desc_set`/`vm_free_chain`/`vm_used_pop` переиспользует алгоритмы из `drivers/virtio.c`.
   - Notify: `addr = notify_base + queue_notify_off * notify_off_multiplier`, записать 16-битный qidx.
   - ISR: 1 байт, bit 0 = queue interrupt, bit 1 = config change; чтение сбрасывает.

2. **Память буферов (`kernel/paging.c`, `kernel/pmm.c`)**
   - Новое зарезервированное окно `GPU_BASE 0x04000000..0x04800000` (8 МБ, PDE 16–17), identity-map + user bit (по аналогии с slab-окном `0x03000000..0x04000000`). Буферы — 2 по 3 МБ (768 страниц) из первой половины; хвост `0x04600000..0x04800000` зарезервирован, чтобы весь user-accessible диапазон был недоступен buddy (иначе чужие фреймы читаемы через WM).
   - Резерв окна в `pmm.c` (как slab window), чтобы buddy не отдавал эти страницы.
   - Адреса identity-map: физический = виртуальный = адрес, который WM получает через syscall и пишет в ring 3.

3. **Syscall-интерфейс (`kernel/aos_gui.c`, `programs/aosabi.h`)**
   - Новые номера: `AOS_GPU_INFO 521`, `AOS_GPU_FLIP 522`, `AOS_CURSOR 523` (слоты свободны).
   - `AOS_GPU_INFO` → out-параметры как в `AOS_FB_INFO`: адрес текущего back-буфера, width, height, pitch, `active` (1 = GPU есть, 0 = нет GPU → WM остаётся на старом VGA-пути). Валидация указателей через `in_luser`.
   - `AOS_GPU_FLIP` → `vgu_flip()` (только если `vgu_active()`).
   - `AOS_CURSOR(x, y, visible)` → `vgu_cursor()`.

4. **WM (`programs/musl/wm.c`)**
   - При старте вызывает `AOS_GPU_INFO`.
   - Если `active`: весь композит и обновления (`composite_rect`, окна, dock, иконки) пишутся в back-буфер, адрес которого вернул `AOS_GPU_INFO`; после каждого завершённого кадра вызывается `AOS_GPU_FLIP`. Курсор в кадр **не** рисуется — позиция отправляется через `AOS_CURSOR`. Весь оверлей/снапшот-код курсора (`cursor_overlaps`, `has_cur`, snapshot restore/redraw) в GPU-пути не используется.
   - Если `active==0`: работает как сейчас (прямо в VRAM, софтверный курсор).

5. **QEMU-инфраструктура**
   - `make run`, `scripts/qemu-debug.sh`, GUI-тесты `scripts/qtest.py` → добавить `-vga none -device virtio-vga,disable-modern=on` (virtio-vga как единственный scanout).
   - Boot не ломается: virtio-vga даёт VBE, GRUB передаёт MB2-тег фреймбуфера; до init GPU виден VGA, после — GPU scanout. Не-GUI тесты (serial-only) не зависят от дисплея.

## Поток данных

```
WM (ring 3)                        kernel                         QEMU
─────────────────────────────────────────────────────────────────────
AOS_GPU_INFO ─────────────────────► addr = back_buf (identity) ─────►
рисование в back_buf (RAM) ────────
AOS_GPU_FLIP ─────────────────────► SET_SCANOUT(0, back) + FLUSH ──► атомарная смена кадра
AOS_CURSOR(x,y) ──────────────────► MOVE_CURSOR ───────────────────► аппаратный курсор
```

## Обработка ошибок

- Нет virtio-gpu (нет `-vga none -device virtio-vga,...`, старый QEMU, probe не нашёл): `vgu_active()==0`, `AOS_GPU_INFO.active=0` → WM на старом VGA-пути. Полный fallback.
- Неприемлемые указатели в syscall'ах → `-5` (как везде в `aos_gui_handler`).
- Ошибка драйвера при init (не создался ресурс/backing): деактивировать GPU (`vgu_active()=0`), WM уходит на fallback.

## Тестирование

- `scripts/guitester.py` — базовые пиксели (desktop gradient, окна) на GPU scanout.
- `scripts/notepadtest.py` — E2E (right-click → create → notepad → Ctrl+S) на GPU-пути.
- `scripts/configtest.py` — пиксели темы на GPU scanout.
- Fallback-путь: без `-vga none -device virtio-vga,...` (или если драйвер не находит GPU) WM продолжает работать — проверяется не-GUI регрессией (stracelive и т.п.) и тем, что при отсутствии virtio-vga пиксельные тесты не гоняются.
- Проверка «мерцания»: аппаратное — flips атомарные; визуально оценивается человеком (TCG). Отдельного автотеста на мерцание нет.

## Вне скоупа

- Virtio-gpu 3D/virgl, blob-resources, EDID, scanout > 1, динамический ресайз, переключение разрешения. Всё — фиксированные 1024×768×32.
- VGA-путь (kernel-консоль, scrollback) не переделывается.