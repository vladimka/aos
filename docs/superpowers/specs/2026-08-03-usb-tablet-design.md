# Design: QEMU usb-tablet support (absolute mouse) with PS/2 fallback

Date: 2026-08-03
Status: approved (design review complete)

## Problem

The host mouse cursor is always drawn by the GTK display over the AOS window. It
cannot be hidden in windowed mode (verified against QEMU `ui/gtk.c`: the cursor is
only hidden when `full_screen || qemu_input_is_absolute(vc) || ptr_owner == vc`).
The fix is to make the guest input **absolute** so QEMU auto-hides the host cursor
and the guest WM cursor tracks the host pointer 1:1.

Goal: kernel-side USB (UHCI + HID tablet) driver that consumes QEMU's `usb-tablet`
absolute coordinates, feeds the existing `mouse_get_state` interface, and keeps the
PS/2 path as a fallback when no `usb-tablet` device is present.

## Decisions (from design review)

- **Approach A**: real PCI enumeration (find UHCI by class `0x0C0300`, read BAR4 I/O
  base and IRQ from config space), honest USB enumeration (port reset, SET_ADDRESS,
  GET_DESCRIPTOR device, SET_CONFIGURATION), then a continuously-polled interrupt IN
  schedule. No generic HID descriptor parsing (YAGNI — QEMU tablet is a fixed device).
- **USB priority + PS/2 fallback**: PS/2 remains the source until the first tablet
  report arrives; any USB init/enumeration failure leaves PS/2 active.
- **Wheel**: QEMU `usb-tablet` (current QEMU) *does* report a wheel byte; it is
  sign-inverted relative to this codebase's PS/2 convention and is inverted back in
  the driver. No hybrid/hack needed.
- Makefile `run` gains `-usb -device usb-tablet`.

## Verified QEMU facts (probe + QEMU source)

Probe (`-usb -device usb-tablet`, QEMU 10.2.1, `info pci` / `info usb` / `info qtree`):

- UHCI: PCI `00:01.2`, ID `8086:7020` (piix3-usb-uhci), class `0x0C0300`,
  **IRQ 11** (pin D), **BAR4 I/O @ 0xc040 [0xc05f]** (32 bytes).
- usb-tablet: full-speed (12 Mb/s), root-hub port 1, "QEMU USB Tablet".
- PS/2 kbd+mouse are still emulated alongside — no conflict.

From QEMU `hw/usb/dev-hid.c` (usb-tablet at full speed on UHCI uses the USB 1.1
descriptors):

- VID:PID `0x0627:0x0001`, bcdUSB 0x0100, bMaxPacketSize0 = **8**.
- Config 1: interface 0, class HID, subclass 0x00, **protocol 0x00** (NOT boot
  protocol; do not send SET_PROTOCOL — QEMU STALLs it for tablets).
- Endpoint 1 IN, interrupt, wMaxPacketSize = **8**, bInterval 0x0a.
- HID report descriptor: 5 buttons + X 16-bit absolute (0..0x7FFF) + Y 16-bit
  absolute + wheel 8-bit signed relative.

From QEMU `hw/input/hid.c` (`hid_pointer_poll`, HID_TABLET): interrupt report is
**6 bytes**:

| offset | field |
|---|---|
| 0 | buttons (bit0=L, 1=R, 2=M, 3=SIDE, 4=EXTRA) |
| 1 | X low byte |
| 2 | X high byte |
| 3 | Y low byte |
| 4 | Y high byte |
| 5 | wheel (signed); QEMU applies `dz = 0 - dz`, so **report +1 = wheel up** |

X/Y absolute range 0..0x7FFF. QEMU normalizes all absolute axes to this range
(`include/ui/input.h`: `INPUT_EVENT_ABS_MIN=0x0000`, `INPUT_EVENT_ABS_MAX=0x7FFF`);
GTK and QMP `input-send-event` both inject in this space. Therefore:

- `mouse_set_usb_state` scales `x * (xmax+1) / 32768`, `y * (ymax+1) / 32768`.
- Wheel inversion: codebase convention is `mouse_wheel` += means wheel down
  (PS/2: +1 = down). Report dz: +1 = up. Driver applies `mouse_wheel += -dz`.

## Architecture

New files:

- `drivers/pci.h`, `drivers/pci.c` — PCI config-space access + bus-0 scan.
- `drivers/uhci.h`, `drivers/uhci.c` — UHCI controller, scheduling, control
  transfers, tablet enumeration, interrupt polling, report parsing.
- `drivers/ports.h` — add `inw`, `inl`, `outl`.

Kernel DMA buffers (static, identity-mapped at their physical address; kernel is
loaded at 1 MB and the page tables identity-map 0..256 MB):

```c
static u32 frame_list[1024] __attribute__((aligned(4096)));
static struct uhci_qh ctrl_qh  __attribute__((aligned(16)));
static struct uhci_qh intr_qh  __attribute__((aligned(16)));
static struct uhci_td td_setup, td_data, td_status  __attribute__((aligned(16)));
static struct uhci_td td_intr  __attribute__((aligned(16)));
static u8 setup_buf[8];
static u8 data_buf[64];
static u8 report_buf[8];
```

Schedule: all 1024 frame-list entries → `ctrl_qh` (frame entry = `&ctrl_qh | 0x02`);
`ctrl_qh.link` → `intr_qh` (horizontal, `|0x02`); `intr_qh.link` = TERM (`0x0001`).
`ctrl_qh.element` = TERM when idle, chain of stage TDs during a control transfer.
`intr_qh.element` = `&td_intr` (single continuously-rearmed interrupt IN TD).

Data flow: USB IRQ → `uhci_handler()` → parse 6-byte report → `mouse_set_usb_state()`
→ WM reads `mouse_get_state()` via `SYS_MOUSE` (unchanged).

## PCI layer

- `ports.h`: add `inw`, `inl`, `outl` (matching existing `inb/outb/outw`).
- Config access: `outl(0xCF8, 0x80000000 | (bus<<16) | (dev<<11) | (func<<8) | (reg&0xFC))`,
  then `inl(0xCFC)` / `outl(0xCFC, val)`.
- Scan bus 0, dev 0..31, func 0..7. Skip when VID/DID == 0xFFFFFFFF.
- Match class `0x0C0300` (base 0x0C, sub 0x03) from reg 0x08.
- Read BAR4 (reg 0x20): `io_base = bar & ~0x3`; IRQ line (reg 0x3C) → `irq`.
- `pci_init()` returns found UHCI `io_base`/`irq`, else failure → PS/2 stays.

## UHCI core

Registers (offsets from `io_base`, from Linux `uhci-hcd.h`):

| offset | name | notes |
|---|---|---|
| 0x00 | USBCMD | RS=0x0001, HCRESET=0x0002 |
| 0x02 | USBSTS | USBINT=0x0001, ERROR=0x0002, HCPE=0x0010, HCH=0x0020 (halted), W1C |
| 0x04 | USBINTR | IOC=0x0004 |
| 0x06 | USBFRNUM | frame number |
| 0x08 | USBFLBASEADD | 32-bit frame list base (4K-aligned) |
| 0x10 | USBPORTSC1 | CCS=0x0001, PE=0x0004, LSDA=0x0100, PR(reset)=0x0200 |

Init sequence:

1. `outw(USBCMD, HCRESET)`; poll `USBSTS & HCH` set (controller reset/halted).
2. `outw(USBCMD, 0)`; poll HCH (halted, stopped).
3. `outl(FLBASEADD, &frame_list)`.
4. Fill frame list: every entry = `(u32)&ctrl_qh | 0x02`. Set QH links/elements
   (both elements TERM).
5. `irq_install_handler(irq, uhci_handler)`.
6. `outw(USBCMD, RS)`; poll `USBSTS & HCH` clear (running).

TD layout (16-byte aligned; little-endian): `{ u32 link; u32 status; u32 token; u32 buffer; }`.

- status: ACTIVE=1<<23, IOC=1<<24, C_ERR=2<<27, error bits STALLED=1<<22,
  DBUFERR=1<<21, BABBLE=1<<20, NAK=1<<19, CRCTIMEO=1<<18, BITSTUFF=1<<17,
  ACTLEN=bits 0..10 (n-1; actual length = `(field+1) & 0x7FF`).
- token: PID bits 0..7 (SETUP=0x2D, IN=0x69, OUT=0xE1), devaddr bits 8..14,
  endpoint bits 15..18, toggle bit 19, explen bits 21..31 (`((len-1)&0x7FF)<<21`;
  len 0 → `0x7FF<<21`).
- link: TD link = plain `&next_td` (bits 4..31), TERM = 0x0001.

Control transfer = chain of 1-3 TDs (SETUP, optional DATA, STATUS) linked
horizontally, head in `ctrl_qh.element`. Stages:

- SETUP: PID SETUP, toggle 0, explen 8, buffer = setup_buf (8 bytes).
- DATA IN/OUT: toggle 1, explen = payload len, buffer = data_buf; PID IN/OUT.
- STATUS: zero-length (explen 0x7FF), toggle 1, direction opposite the data stage
  (IN if no data stage); PID IN or OUT.

Completion: poll `td_status.status & ACTIVE` clear with timeout (~500 ms by
`tick`). On completion check error bits; on any failure abort that step. Reset
`ctrl_qh.element` = TERM afterward. Only the interrupt TD carries IOC, so control
transfers are detected purely by polling (no IRQ involvement).

Enumeration steps (all control transfers via EP0; devaddr 0 until SET_ADDRESS):

1. Poll PORTSC1 `CCS` (tablet connected).
2. Port reset: `PORTSC1 |= PR` (0x0200); poll until PR clears; check `PE` set;
   check `LSDA` clear (full-speed tablet).
3. `SET_ADDRESS(1)`: `{00 05 01 00 00 00 00 00}`, no data stage, STATUS IN.
4. `GET_DESCRIPTOR(Device)` 18 bytes: `{80 06 00 01 00 00 12 00}`, DATA IN 18 →
   `data_buf`. Validate `idVendor==0x0627 && idProduct==0x0001 && bMaxPacketSize0==8`.
5. `SET_CONFIGURATION(1)`: `{00 09 01 00 00 00 00 00}`, STATUS IN.
6. `serial_print("USB tablet enumerated.")`.
7. Arm interrupt schedule: `intr_qh.element = &td_intr`;
   `td_intr.token = explen(8)<<21 | toggle0<<19 | 1<<15 (ep1) | 1<<8 (addr1) | PID_IN`;
   `td_intr.status = IOC | ACTIVE | C_ERR`; buffer = report_buf.

NAK behavior (UHCI): a NAK'd interrupt TD stays ACTIVE and is re-polled every frame
without clearing ACTIVE and without generating IOC — so the idle tablet produces no
interrupts and polling is continuous.

Interrupt handler (IRQ 11):

```c
void uhci_handler(void) {
    u16 sts = inw(base + USBSTS);
    if (sts & USBSTS_USBINT) {
        outw(base + USBSTS, USBSTS_USBINT);            /* W1C */
        if (!(td_intr.status & ACTIVE) && actual_len >= 1) {
            buttons = report[0] & 0x07;
            X = report[1] | (report[2] << 8);
            Y = report[3] | (report[4] << 8);
            dz = (signed char)report[5];
            mouse_set_usb_state(X, Y, buttons, dz);
            /* re-arm */
            td_intr.status = IOC | ACTIVE | C_ERR;
            toggle ^= 1;                                /* update TD token bit 19 */
        }
    }
    if (sts & (USBSTS_ERROR | USBSTS_HCPE | USBSTS_HSE))
        outw(base + USBSTS, sts & (USBSTS_ERROR | USBSTS_HCPE | USBSTS_HSE));
}
```

Note: IRQ 11 is shared with the QEMU e1000 NIC; the NIC is unused by AOS, so no
interference today.

## mouse.c integration

New public entry:

```c
/* mouse.h */
void mouse_set_usb_state(int x, int y, int buttons, int wheel);
```

```c
void mouse_set_usb_state(int x, int y, int buttons, int wheel) {
    /* irq_save/cli like mouse_get_state */
    if (!usb_active) { usb_active = 1; serial_print("USB tablet mouse active.\n"); }
    if (!mouse_xmax) mouse_xmax = 1023;   /* text-mode guard */
    if (!mouse_ymax) mouse_ymax = 767;
    mouse_x = x * (mouse_xmax + 1) / 32768;   /* clamp to [0, mouse_xmax] */
    mouse_y = y * (mouse_ymax + 1) / 32768;   /* clamp to [0, mouse_ymax] */
    mouse_buttons = buttons & 0x07;
    if (wheel) mouse_wheel += -wheel;          /* report +1 = up; mouse_wheel += = down */
}
```

`mouse_process_byte()` (PS/2 path) returns immediately when `usb_active` is set.

Fallback semantics: `usb_active` is only set on the first parsed report. Any USB
init/enumeration failure leaves it 0 and PS/2 keeps working. If reports never
arrive (device stuck), PS/2 remains authoritative — no watchdog needed.

## kernel.c wiring

After `mouse_init()` in `kernel_main`:

```c
usb_init();   /* PCI scan → if UHCI found: install IRQ handler + enumerate */
```

`usb_init()` (in uhci.c) owns: PCI scan, controller init, IRQ installation
(it found the IRQ number), enumeration, interrupt schedule. It never fails hard —
on any error it returns with PS/2 still active and a `serial_print` message.

The main `hlt` loop and `mouse_flush_wheel()` are unchanged.

## Makefile

`run:` target becomes:

```
qemu-system-i386 -display gtk,grab-on-hover=on -usb -device usb-tablet -cdrom aos.iso
```

## Testing

New script `/tmp/opencode/usbtest.py` (pattern of `mtest2.py`/`idletest.py` with
the monitor wait-loop; uses QMP `-qmp unix:...` for absolute input injection):

1. Boot with `-usb -device usb-tablet -qmp unix:...` + serial log; wait for
   `"USB tablet enumerated."` and `"USB tablet mouse active."` after first input.
2. QMP `input-send-event` abs `x=16384 y=16384` (center) → `screendump` → PPM →
   verify the WM cursor pixel at ~(512,384).
3. Series of abs points (corners, edges) → verify 1:1 scaling by PPM pixels
   (reuse guitester.py PPM analysis approach).
4. Buttons: QMP btn left down/up → click behavior in WM works.
5. Wheel: QMP btn wheel-up/wheel-down → `vga_scroll` direction correct
   (verifies the sign inversion).
6. Fallback/regression: run WITHOUT `-device usb-tablet` → PS/2 still drives the
   cursor (HMP `mouse_move` works as before).
7. Manual (user): `make run` — host cursor hidden over the window, guest cursor
   tracks host 1:1.

## Known limitations / risks

- Wheel in tablet mode works via the report wheel byte; relative PS/2 wheel
  behavior is unchanged when fallback is active.
- If the driver fails at boot with `-device usb-tablet`, the host cursor is hidden
  (absolute device present) but the guest cursor would not move — the fallback
  only helps if the device is absent. QEMU's configuration is deterministic
  (probed), so this is unlikely; the serial log will say which step failed.
- IRQ 11 is shared with the unused e1000 NIC.
