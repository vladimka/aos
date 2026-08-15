# Аппаратное видеоускорение через VirtIO-GPU

Дата: 2026-08-15
Статус: черновик
Цель: устранить мерцание экрана при обновлении, переведя рендер WM на virtio-gpu с атомарным сменой кадра (`SET_SCANOUT`) и аппаратным курсором.

## Проблема

Сейчас WM (`programs/musl/wm.c`) композитует прямо в VRAM (физический фреймбуфер из MB2-тега, `fb_addr`): все `composite_rect`-обновления пишут прямоугольники сразу в отображаемую поверхность. В QEMU под TCG кадр «собирается» частями — видны промежуточные состояния (мерцание). Курсор рисуется как оверлей в VRAM со снапшотом и перерисовывается после каждого обновления.

QEMU предоставляет `virtio-gpu` (`virtio-vga`), где guest создаёт ресурсы (2D-поверхности), прикрепляет к ним страницы RAM (`RESOURCE_ATTACH_BACKING`) и переключает видимую поверхность командой `SET_SCANOUT` — кадр меняется целиком и атомарно. Это устраняет мерцание принципиально, плюс даёт аппаратный курсор (cursor queue: `UPDATE_CURSOR`/`MOVE_CURSOR`).

## Архитектура

Новый драйвер `drivers/virtio_gpu.c` в ядре поверх существующего legacy-транспорта (`drivers/virtio.c`, `disable-modern=on`, I/O BAR). WM по-прежнему пишет пиксели в обычную RAM (backing-страницы), но кадр уходит на экран атомарно через flip. VGA остаётся только для boot: до инициализации GPU экран показывает VGA-фреймбуфер из MB2-тега; после старта WM виден GPU scanout.

### Компоненты

1. **Драйвер `drivers/virtio_gpu.c`**
   - Probe: `virtio_probe_pci(d, 0x1040)` (legacy GPU id; QEMU `virtio-vga` отдаёт именно его при `disable-modern=on`). Подключение к списку virtio-устройств (`virtio_register`).
   - Инициализация: `virtio_dev_init`, 2 virtqueue — control (qidx 0) и cursor (qidx 1). Команды:
     - `RESOURCE_CREATE_2D` ×2 — два буфера `width=1024, height=768`, формат `VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM` (=1; little-endian u32 `0xRRGGBB` = B8G8R8A8 — совпадает с нашим рендером).
     - `RESOURCE_ATTACH_BACKING` — прикрепить страницы буферов (физические адреса; виртуальный = физический за счёт identity-map).
     - `SET_SCANOUT(0, buf0)` — показать первый буфер.
   - API: `vgu_init()`, `vgu_flip()`, `vgu_cursor(x, y)`, `vgu_active()`.
   - `vgu_flip()`: `SET_SCANOUT(0, other_buf)` + `RESOURCE_FLUSH` на новый буфер; меняет роли буферов. Синхронный (ожидание ответа через used ring).
   - `vgu_cursor()`: при первом вызове `UPDATE_CURSOR` с курсор-битмапом 64×64 (встроенный art, два цвета + прозрачность), далее `MOVE_CURSOR`. `visible=0` → `UPDATE_CURSOR` с `resource_id=0` (скрыть).
   - Память: 2 буфера по 3 МБ в user-accessible окне (см. ниже). Формат записи тот же, что сейчас (32bpp).

2. **Память буферов (`kernel/paging.c`, `kernel/pmm.c`)**
   - Новое зарезервированное окно `GPU_BASE 0x04000000..0x04600000` (6 МБ, PDE 16–17), identity-map + user bit (по аналогии с slab-окном `0x03000000..0x04000000`). Каждый буфер = 3 МБ (768 страниц), выделяется из этого окна.
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
   - `make run`, `scripts/qemu-debug.sh`, GUI-тесты `scripts/qtest.py` → добавить `-vga virtio`.
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

- Нет virtio-gpu (нет `-vga virtio`, старый QEMU, probe не нашёл): `vgu_active()==0`, `AOS_GPU_INFO.active=0` → WM на старом VGA-пути. Полный fallback.
- Неприемлемые указатели в syscall'ах → `-5` (как везде в `aos_gui_handler`).
- Ошибка драйвера при init (не создался ресурс/backing): деактивировать GPU (`vgu_active()=0`), WM уходит на fallback.

## Тестирование

- `scripts/guitester.py` — базовые пиксели (desktop gradient, окна) на GPU scanout.
- `scripts/notepadtest.py` — E2E (right-click → create → notepad → Ctrl+S) на GPU-пути.
- `scripts/configtest.py` — пиксели темы на GPU scanout.
- Fallback-путь: без `-vga virtio` (или если драйвер не находит GPU) WM продолжает работать — проверяется не-GUI регрессией (stracelive и т.п.) и тем, что при отсутствии `-vga virtio` пиксельные тесты не гоняются.
- Проверка «мерцания»: аппаратное — flips атомарные; визуально оценивается человеком (TCG). Отдельного автотеста на мерцание нет.

## Вне скоупа

- Virtio-gpu 3D/virgl, blob-resources, EDID, scanout > 1, динамический ресайз, переключение разрешения. Всё — фиксированные 1024×768×32.
- VGA-путь (kernel-консоль, scrollback) не переделывается.