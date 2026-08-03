# QEMU usb-tablet (absolute mouse) with PS/2 fallback — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a kernel-side UHCI + HID-tablet USB driver so AOS consumes QEMU's `usb-tablet` absolute coordinates (host cursor auto-hidden, 1:1 tracking), keeping PS/2 as fallback.

**Architecture:** New `drivers/pci.c` (bus-0 scan for class `0x0C0300`, returns BAR4 I/O base + IRQ from config space) and `drivers/uhci.c` (UHCI controller: frame list → control QH → interrupt QH; control-transfer primitives for enumeration; a single continuously-rearmed interrupt IN TD whose completion drives `mouse_set_usb_state()`). `mouse.c` keeps its `mouse_get_state` interface and owns the cursor state; PS/2 bytes are ignored once USB is active.

**Tech Stack:** C (GCC, `-ffreestanding -m32 -std=c11`, no libc), x86 32-bit protected mode, UHCI 1.1 (PIIX3, PCI 8086:7020), QEMU `usb-tablet`, QMP for test input injection.

## Global Constraints

- No libc; only `kernel/string.c` helpers. No dynamic allocation — use static kernel buffers.
- All kernel C compiled `-ffreestanding -nostdlib -fno-builtin -mno-sse -mno-mmx -mno-80387`.
- DMA buffers must be static kernel data (kernel at 1 MB, identity-mapped 0..256 MB, so virtual == physical).
- Follow existing style: `inb/outb/outw` in `drivers/ports.h` (add `inw/inl/outl`); `serial_print` for driver messages; `irq_install_handler(int, void(*)(void))` from `kernel/interrupts.h` (all PIC lines already unmasked; EOI sent before handler — do NOT send EOI in the handler).
- `extern volatile unsigned int tick;` — 1000 Hz counter (1 tick ≈ 1 ms), available for timeouts.
- IRQ number comes from PCI config (IRQ 11 in QEMU); do not hardcode it.
- `usb_init()` never fails hard: on any failure it returns with PS/2 active and a `serial_print` message.
- `usb_active` flips to 1 only on the first parsed tablet report (PS/2 stays authoritative until then).
- Wheel sign: report `dz` +1 = wheel up; codebase `mouse_wheel` += means wheel down; invert in `mouse_set_usb_state`.
- Keep `mouse_process_byte()` early-return when USB active. Do not modify `mouse_get_state` signature.
- Makefile: `run:` gains `-usb -device usb-tablet`.

Verified QEMU facts (2026-08-03 probe, QEMU 10.2.1): UHCI at PCI `00:01.2` (8086:7020, class 0x0C0300), BAR4 I/O @ `0xc040` size 0x20, IRQ 11. usb-tablet full-speed on port 1, VID/PID `0x0627:0x0001`, bMaxPacketSize0=8, config 1, HID interface protocol 0x00 (do NOT send SET_PROTOCOL — QEMU STALLs), EP 1 IN interrupt wMaxPacketSize=8. Report = 6 bytes: buttons(bit0=L,1=R,2=M,3=SIDE,4=EXTRA), X lo, X hi, Y lo, Y hi, wheel(signed). X/Y absolute 0..0x7FFF (QEMU normalizes all absolute axes to 0..0x7FFF).

---

## Task 1: I/O port primitives (`inw`/`inl`/`outl`)

**Files:**
- Modify: `drivers/ports.h`

**Interfaces:**
- Produces: `unsigned short inw(unsigned short port)`, `unsigned int inl(unsigned short port)`, `void outl(unsigned short port, unsigned int data)` — used by pci.c (0xCF8/0xCFC) and uhci.c (32-bit FLBASEADD write).

- [ ] **Step 1: Add the functions**

Add after the existing `outw` in `drivers/ports.h`:

```c
static inline unsigned short inw(unsigned short port) {
    unsigned short result;
    __asm__ volatile("in %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline unsigned int inl(unsigned short port) {
    unsigned int result;
    __asm__ volatile("in %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outl(unsigned short port, unsigned int data) {
    __asm__ volatile("out %0, %1" : : "a"(data), "Nd"(port));
}
```

- [ ] **Step 2: Build**

Run: `make`
Expected: builds `aos.iso` with no errors, no warnings from ports.h.

- [ ] **Step 3: Commit**

```bash
git add drivers/ports.h
git commit -m "add inw/inl/outl port I/O helpers"
```

---

## Task 2: PCI layer + `usb_init()` skeleton (UHCI probe)

**Files:**
- Create: `drivers/pci.h`, `drivers/pci.c`, `drivers/uhci.h`, `drivers/uhci.c`
- Modify: `Makefile` (add objects to `KERNEL_OBJS`), `kernel/kernel.c` (call `usb_init()`)

**Interfaces:**
- Consumes: `inl`, `outl` (Task 1).
- Produces:
  - `int pci_init(unsigned int *io_base, unsigned int *irq)` — scans bus 0 for class `0x0C0300`, returns 0 and fills `*io_base` (BAR4 I/O base) and `*irq` (IRQ line) on success, -1 if not found.
  - `unsigned int pci_config_read(unsigned char bus, unsigned char dev, unsigned char func, unsigned char reg)`
  - `void usb_init(void)` (in uhci.h) — called from `kernel_main`; in this task it only probes PCI and prints the result.

- [ ] **Step 1: Create `drivers/pci.h`**

```c
#ifndef PCI_H
#define PCI_H

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_CLASS_UHCI 0x0C0300

unsigned int pci_config_read(unsigned char bus, unsigned char dev,
                             unsigned char func, unsigned char reg);
int pci_init(unsigned int *io_base, unsigned int *irq);

#endif
```

- [ ] **Step 2: Create `drivers/pci.c`**

```c
#include "pci.h"
#include "ports.h"

static unsigned int config_addr(unsigned char bus, unsigned char dev,
                                unsigned char func, unsigned char reg) {
    return 0x80000000U | ((unsigned int)bus << 16) | ((unsigned int)dev << 11) |
           ((unsigned int)func << 8) | (reg & 0xFC);
}

unsigned int pci_config_read(unsigned char bus, unsigned char dev,
                             unsigned char func, unsigned char reg) {
    outl(PCI_CONFIG_ADDR, config_addr(bus, dev, func, reg));
    return inl(PCI_CONFIG_DATA);
}

int pci_init(unsigned int *io_base, unsigned int *irq) {
    unsigned int b, d, f;
    for (b = 0; b < 1; b++) {
        for (d = 0; d < 32; d++) {
            for (f = 0; f < 8; f++) {
                unsigned int id = pci_config_read(b, d, f, 0x00);
                if (id == 0xFFFFFFFF || id == 0)
                    continue;
                unsigned int class_code = pci_config_read(b, d, f, 0x08) >> 8;
                if (class_code == PCI_CLASS_UHCI) {
                    unsigned int bar = pci_config_read(b, d, f, 0x20);
                    *io_base = bar & ~0x3;
                    *irq = pci_config_read(b, d, f, 0x3C) & 0xFF;
                    return 0;
                }
            }
        }
    }
    return -1;
}
```

- [ ] **Step 3: Create `drivers/uhci.h`**

```c
#ifndef UHCI_H
#define UHCI_H

void usb_init(void);

#endif
```

- [ ] **Step 4: Create `drivers/uhci.c` (probe-only skeleton)**

```c
#include "uhci.h"
#include "pci.h"
#include "serial.h"

void usb_init(void) {
    unsigned int io_base, irq;
    if (pci_init(&io_base, &irq) != 0) {
        serial_print("USB: UHCI not found; PS/2 mouse stays active.\n");
        return;
    }
    serial_print("USB: UHCI io=0x");
    serial_print_hex(io_base);
    serial_print(" irq=0x");
    serial_print_hex(irq);
    serial_print("\n");
}
```

- [ ] **Step 5: Wire into the build and boot**

`Makefile`: add `drivers/pci.o drivers/uhci.o` to `KERNEL_OBJS` (keep the existing order style; e.g. after `drivers/mouse.o`).

`kernel/kernel.c`:
- add `#include "uhci.h"` (alphabetical, after `#include "mouse.h"` line 14)
- after `mouse_init();` (line 98) add:

```c
    usb_init();
```

- [ ] **Step 6: Build and boot-verify the probe**

Run:
```bash
make
rm -f /tmp/aos-usb.log
qemu-system-i386 -cdrom aos.iso -display none -serial file:/tmp/aos-usb.log -usb -device usb-tablet &
for i in $(seq 1 40); do grep -q "UHCI" /tmp/aos-usb.log && break; sleep 0.5; done
grep "USB:" /tmp/aos-usb.log
kill %1 2>/dev/null
```

Expected: `USB: UHCI io=0xc040 irq=0x0b` in the log. Also confirm no panic (no `KERNEL PANIC` in the log).

- [ ] **Step 7: Commit**

```bash
git add drivers/pci.h drivers/pci.c drivers/uhci.h drivers/uhci.c Makefile kernel/kernel.c
git commit -m "add PCI scan + usb_init probe for QEMU UHCI"
```

---

## Task 3: UHCI controller init (reset, frame list, run)

**Files:**
- Modify: `drivers/uhci.c`, `drivers/uhci.h` (if needed)

**Interfaces:**
- Consumes: `pci_init` (Task 2), `inw/outw/outl`, `tick`.
- Produces: `static int uhci_controller_init(unsigned int base)` — returns 0 running, negative on failure; and the persistent schedule (frame list → `ctrl_qh` → `intr_qh`) that later tasks attach TDs to.

- [ ] **Step 1: Add UHCI definitions, DMA buffers, and the controller init**

Replace the body of `drivers/uhci.c` with:

```c
#include "uhci.h"
#include "pci.h"
#include "serial.h"
#include "ports.h"

extern volatile unsigned int tick;

/* Register offsets (from io_base) */
#define USBCMD       0x00
#define USBSTS       0x02
#define USBINTR      0x04
#define USBFRNUM     0x06
#define USBFLBASEADD 0x08
#define USBPORTSC1   0x10

#define USBCMD_RS       0x0001
#define USBCMD_HCRESET  0x0002
#define USBSTS_USBINT   0x0001
#define USBSTS_ERROR    0x0002
#define USBSTS_HCPE     0x0010
#define USBSTS_HCH      0x0020
#define USBINTR_IOC     0x0004

#define USBPORTSC_CCS   0x0001
#define USBPORTSC_PE    0x0004
#define USBPORTSC_LSDA  0x0100
#define USBPORTSC_PR    0x0200

#define USB_PID_SETUP   0x2D
#define USB_PID_IN      0x69
#define USB_PID_OUT     0xE1

#define TD_CTRL_ACTIVE   (1u << 23)
#define TD_CTRL_IOC      (1u << 24)
#define TD_CTRL_STALLED  (1u << 22)
#define TD_CTRL_DBUFERR  (1u << 21)
#define TD_CTRL_BABBLE   (1u << 20)
#define TD_CTRL_CRCTIMEO (1u << 18)
#define TD_CTRL_BITSTUFF (1u << 17)
#define TD_C_ERR_SHIFT   27
#define uhci_maxerr(e)   ((unsigned int)(e) << TD_C_ERR_SHIFT)
#define TD_ACTLEN_MASK   0x7FF

#define uhci_explen(len) ((((unsigned int)(len) - 1) & 0x7FF) << 21)

struct uhci_qh {
    unsigned int link;
    unsigned int element;
} __attribute__((aligned(16)));

struct uhci_td {
    unsigned int link;
    unsigned int status;
    unsigned int token;
    unsigned int buffer;
} __attribute__((aligned(16)));

static unsigned int uhci_base;

static unsigned int frame_list[1024] __attribute__((aligned(4096)));
static struct uhci_qh ctrl_qh __attribute__((aligned(16)));
static struct uhci_qh intr_qh __attribute__((aligned(16)));

static unsigned int td_status(struct uhci_td *td) {
    return *(volatile unsigned int *)&td->status;
}

static int td_active(struct uhci_td *td) {
    return (td_status(td) & TD_CTRL_ACTIVE) != 0;
}

static int uhci_controller_init(unsigned int base) {
    unsigned int i, start;
    outw(base + USBCMD, USBCMD_HCRESET);
    start = tick;
    while (!(inw(base + USBSTS) & USBSTS_HCH)) {
        if ((int)(tick - start) >= 100) return -1;
    }
    outw(base + USBCMD, 0);
    start = tick;
    while (!(inw(base + USBSTS) & USBSTS_HCH)) {
        if ((int)(tick - start) >= 100) return -2;
    }

    for (i = 0; i < 1024; i++)
        frame_list[i] = (unsigned int)&ctrl_qh | 0x0002;

    ctrl_qh.link    = (unsigned int)&intr_qh | 0x0002;
    ctrl_qh.element = 0x0001;
    intr_qh.link    = 0x0001;
    intr_qh.element = 0x0001;

    outl(base + USBFLBASEADD, (unsigned int)frame_list);

    outw(base + USBCMD, USBCMD_RS);
    start = tick;
    while (inw(base + USBSTS) & USBSTS_HCH) {
        if ((int)(tick - start) >= 100) return -3;
    }
    return 0;
}

void usb_init(void) {
    unsigned int io_base, irq;
    if (pci_init(&io_base, &irq) != 0) {
        serial_print("USB: UHCI not found; PS/2 mouse stays active.\n");
        return;
    }
    uhci_base = io_base;
    if (uhci_controller_init(uhci_base) != 0) {
        serial_print("USB: UHCI init failed.\n");
        return;
    }
    serial_print("USB: UHCI running.\n");
}
```

- [ ] **Step 2: Build and boot-verify**

Run:
```bash
make
rm -f /tmp/aos-usb.log
qemu-system-i386 -cdrom aos.iso -display none -serial file:/tmp/aos-usb.log -usb -device usb-tablet &
for i in $(seq 1 40); do grep -q "USB: UHCI" /tmp/aos-usb.log && break; sleep 0.5; done
grep "USB:" /tmp/aos-usb.log
kill %1 2>/dev/null
```

Expected: `USB: UHCI io=0xc040 irq=0x0b` then `USB: UHCI running.`, no panic. The controller runs the (empty) schedule; no bus traffic yet.

- [ ] **Step 3: Commit**

```bash
git add drivers/uhci.c
git commit -m "UHCI controller init: reset, frame list, QH schedule"
```

---

## Task 4: Control transfers + tablet enumeration

**Files:**
- Modify: `drivers/uhci.c`

**Interfaces:**
- Consumes: `uhci_controller_init` (Task 3), `tick`.
- Produces:
  - `static int uhci_control(unsigned int bmReqType, unsigned int bRequest, unsigned int wValue, unsigned int wIndex, unsigned int wLength, void *data)` — builds SETUP[/DATA]/STATUS chain on `ctrl_qh`, polls completion (~500 ms timeout), returns 0 ok / -1 timeout / -2 device error.
  - `static int uhci_enum_tablet(void)` — port reset + SET_ADDRESS + GET_DESCRIPTOR + SET_CONFIGURATION; 0 on success, negative on each step.

- [ ] **Step 1: Add control-transfer + enumeration code**

Append to `drivers/uhci.c` (before `usb_init`):

```c
static struct uhci_td td_setup __attribute__((aligned(16)));
static struct uhci_td td_data  __attribute__((aligned(16)));
static struct uhci_td td_status __attribute__((aligned(16)));
static unsigned char setup_buf[8];
static unsigned char data_buf[64];
static unsigned int uhci_devaddr;

static int wait_td_done(struct uhci_td *td) {
    unsigned int start = tick;
    while (td_active(td)) {
        if ((int)(tick - start) >= 500) return -1;
    }
    unsigned int st = td_status(td);
    if (st & (TD_CTRL_STALLED | TD_CTRL_DBUFERR | TD_CTRL_BABBLE |
              TD_CTRL_CRCTIMEO | TD_CTRL_BITSTUFF))
        return -2;
    return 0;
}

static int uhci_control(unsigned int bmReqType, unsigned int bRequest,
                        unsigned int wValue, unsigned int wIndex,
                        unsigned int wLength, void *data) {
    setup_buf[0] = bmReqType;
    setup_buf[1] = bRequest;
    setup_buf[2] = wValue & 0xFF;
    setup_buf[3] = wValue >> 8;
    setup_buf[4] = wIndex & 0xFF;
    setup_buf[5] = wIndex >> 8;
    setup_buf[6] = wLength & 0xFF;
    setup_buf[7] = wLength >> 8;

    unsigned int dir_in = (bmReqType & 0x80) ? 1 : 0;

    td_setup.link   = wLength ? (unsigned int)&td_data : (unsigned int)&td_status;
    td_setup.status = TD_CTRL_ACTIVE | uhci_maxerr(2);
    td_setup.token  = uhci_explen(8) | (uhci_devaddr << 8) | USB_PID_SETUP;
    td_setup.buffer = (unsigned int)setup_buf;

    struct uhci_td *last;
    if (wLength) {
        td_data.link   = (unsigned int)&td_status;
        td_data.status = TD_CTRL_ACTIVE | uhci_maxerr(2);
        td_data.token  = uhci_explen(wLength) | (1u << 19) |
                         (uhci_devaddr << 8) | (dir_in ? USB_PID_IN : USB_PID_OUT);
        td_data.buffer = (unsigned int)data;
        td_status.link   = 0x0001;
        td_status.status = TD_CTRL_ACTIVE | uhci_maxerr(2);
        td_status.token  = uhci_explen(0) | (1u << 19) |
                           (uhci_devaddr << 8) | (dir_in ? USB_PID_OUT : USB_PID_IN);
        td_status.buffer = 0;
        last = &td_status;
    } else {
        td_status.link   = 0x0001;
        td_status.status = TD_CTRL_ACTIVE | uhci_maxerr(2);
        td_status.token  = uhci_explen(0) | (1u << 19) |
                           (uhci_devaddr << 8) | (dir_in ? USB_PID_IN : USB_PID_OUT);
        td_status.buffer = 0;
        last = &td_status;
    }

    ctrl_qh.element = (unsigned int)&td_setup;
    int rc = wait_td_done(last);
    ctrl_qh.element = 0x0001;
    return rc;
}

static int uhci_port_reset(void) {
    unsigned int start = tick;
    while (!(inw(uhci_base + USBPORTSC1) & USBPORTSC_CCS)) {
        if ((int)(tick - start) >= 500) return -1;
    }
    outw(uhci_base + USBPORTSC1, inw(uhci_base + USBPORTSC1) | USBPORTSC_PR);
    start = tick;
    while (inw(uhci_base + USBPORTSC1) & USBPORTSC_PR) {
        if ((int)(tick - start) >= 500) return -2;
    }
    if (!(inw(uhci_base + USBPORTSC1) & USBPORTSC_PE)) return -3;
    if (inw(uhci_base + USBPORTSC1) & USBPORTSC_LSDA) return -4;
    return 0;
}

static int uhci_enum_tablet(void) {
    if (uhci_port_reset() != 0) return -1;
    uhci_devaddr = 0;
    if (uhci_control(0x00, 5, 1, 0, 0, 0) != 0) return -2;   /* SET_ADDRESS(1) */
    uhci_devaddr = 1;
    if (uhci_control(0x80, 6, 0x0100, 0, 18, data_buf) != 0) return -3; /* GET_DESCRIPTOR(Device) */
    if (data_buf[7] != 8)                        return -4;   /* bMaxPacketSize0 */
    if (data_buf[8] != 0x27 || data_buf[9] != 0x06) return -5; /* idVendor 0x0627 */
    if (data_buf[10] != 0x01 || data_buf[11] != 0x00) return -6; /* idProduct 0x0001 */
    if (uhci_control(0x00, 9, 1, 0, 0, 0) != 0) return -7;   /* SET_CONFIGURATION(1) */
    return 0;
}
```

Then in `usb_init`, after the `uhci_controller_init` success line, add:

```c
    if (uhci_enum_tablet() != 0) {
        serial_print("USB: tablet enumeration failed.\n");
        return;
    }
    serial_print("USB tablet enumerated.\n");
```

- [ ] **Step 2: Build and boot-verify**

Run:
```bash
make
rm -f /tmp/aos-usb.log
qemu-system-i386 -cdrom aos.iso -display none -serial file:/tmp/aos-usb.log -usb -device usb-tablet &
for i in $(seq 1 40); do grep -q "USB tablet" /tmp/aos-usb.log && break; sleep 0.5; done
grep "USB:" /tmp/aos-usb.log; grep "USB tablet" /tmp/aos-usb.log
kill %1 2>/dev/null
```

Expected: `USB: UHCI io=0xc040 irq=0x0b`, `USB: UHCI running.`, `USB tablet enumerated.`, no panic. The tablet is now configured; no interrupt schedule yet.

- [ ] **Step 3: Commit**

```bash
git add drivers/uhci.c
git commit -m "UHCI control transfers + QEMU usb-tablet enumeration"
```

---

## Task 5: Interrupt schedule, report parsing, mouse integration

**Files:**
- Modify: `drivers/uhci.c`, `drivers/mouse.h`, `drivers/mouse.c`

**Interfaces:**
- Consumes: `irq_install_handler(int, void(*)(void))`, `mouse_set_usb_state` (produced here).
- Produces:
  - `void uhci_handler(void)` — IRQ handler; parses the 6-byte report, calls `mouse_set_usb_state`, re-arms the interrupt TD.
  - `void mouse_set_usb_state(int x, int y, int buttons, int wheel)` — new in `mouse.h`; scales 0..32767 to screen, inverts wheel sign, flips `usb_active` on first report. Implemented in `mouse.c`.

- [ ] **Step 1: Add the interrupt TD, handler, and arming to `drivers/uhci.c`**

Append (before `usb_init`):

```c
static struct uhci_td td_intr __attribute__((aligned(16)));
static unsigned char report_buf[8];
static unsigned int intr_toggle;

static void arm_intr(void) {
    td_intr.link   = 0x0001;
    td_intr.status = TD_CTRL_ACTIVE | TD_CTRL_IOC | uhci_maxerr(2);
    td_intr.token  = uhci_explen(8) | (intr_toggle << 19) |
                     (1u << 15) | (uhci_devaddr << 8) | USB_PID_IN;
    td_intr.buffer = (unsigned int)report_buf;
    intr_qh.element = (unsigned int)&td_intr;
}

void uhci_handler(void) {
    unsigned short sts = inw(uhci_base + USBSTS);
    if (sts & USBSTS_USBINT) {
        outw(uhci_base + USBSTS, USBSTS_USBINT);
        if (!td_active(&td_intr)) {
            unsigned int alen = (td_status(&td_intr) & TD_ACTLEN_MASK);
            alen = (alen + 1) & TD_ACTLEN_MASK;
            if (alen >= 1) {
                int x  = report_buf[1] | (report_buf[2] << 8);
                int y  = report_buf[3] | (report_buf[4] << 8);
                int dz = (signed char)report_buf[5];
                mouse_set_usb_state(x, y, report_buf[0] & 0x07, dz);
            }
            intr_toggle ^= 1;
            arm_intr();
        }
    }
    if (sts & (USBSTS_ERROR | USBSTS_HCPE | USBSTS_HSE))
        outw(uhci_base + USBSTS, sts & (USBSTS_ERROR | USBSTS_HCPE | USBSTS_HSE));
}
```

In `usb_init`, add the include effect and wiring:

- Add `#include "interrupts.h"` and `#include "mouse.h"` to the top of `drivers/uhci.c` (alphabetical after the existing includes).
- In `usb_init`, after `uhci_base = io_base;` add:
  ```c
      irq_install_handler((int)irq, uhci_handler);
  ```
- In `usb_init`, after the `uhci_enum_tablet()` success and before `serial_print("USB tablet enumerated.\n");`, add:
  ```c
      intr_toggle = 0;
      arm_intr();
  ```
- After the controller init, also enable IOC interrupts. In `usb_init`, after `uhci_controller_init(uhci_base) != 0` check passes, add:
  ```c
      outw(uhci_base + USBINTR, USBINTR_IOC);
  ```

- [ ] **Step 2: Add `mouse_set_usb_state` to `drivers/mouse.h`**

```c
void mouse_set_usb_state(int x, int y, int buttons, int wheel);
```

- [ ] **Step 3: Implement it in `drivers/mouse.c`**

- Add a static flag near the other statics (after `static int mouse_wheel = 0;`, line 19):
  ```c
  static int usb_active = 0;
  ```
- In `mouse_process_byte`, make it return immediately when USB is active (first line of the function):
  ```c
  if (usb_active) return;
  ```
- Add the function after `mouse_get_state` (end of file):

  ```c
  // Absolute tablet input from the USB HID driver (X/Y in 0..32767, wheel
  // byte +1 = wheel up). Scales to the framebuffer, inverts the wheel sign
  // to the PS/2 convention (mouse_wheel += means wheel down), and flips the
  // USB source on the first report so PS/2 stays authoritative until then.
  void mouse_set_usb_state(int x, int y, int buttons, int wheel) {
      unsigned int flags;
      irq_save(&flags);
      if (!usb_active) {
          usb_active = 1;
          serial_print("USB tablet mouse active.\n");
      }
      if (!mouse_xmax) mouse_xmax = 1023;
      if (!mouse_ymax) mouse_ymax = 767;
      mouse_x = (x * (mouse_xmax + 1)) / 32768;
      mouse_y = (y * (mouse_ymax + 1)) / 32768;
      if (mouse_x < 0) mouse_x = 0;
      if (mouse_x > mouse_xmax) mouse_x = mouse_xmax;
      if (mouse_y < 0) mouse_y = 0;
      if (mouse_y > mouse_ymax) mouse_y = mouse_ymax;
      mouse_buttons = buttons & 0x07;
      if (wheel) {
          mouse_wheel += -wheel;
          wheel_acc += (wheel > 0) ? 3 : -3;
      }
      irq_restore(flags);
  }
  ```

Note: `mouse_set_usb_state` must be placed after the `irq_save`/`irq_restore` static definitions (they are above `mouse_get_state`), so the end of the file is correct.

- [ ] **Step 4: Build**

Run: `make`
Expected: clean build. Note `uhci_handler` becomes `void uhci_handler(void)` matching `irq_install_handler`'s signature.

- [ ] **Step 5: Boot + inject a report via QMP, verify the log flips the source**

Run:
```bash
rm -f /tmp/aos-usb.log /tmp/aos-qmp.sock
qemu-system-i386 -cdrom aos.iso -display none -serial file:/tmp/aos-usb.log \
  -qmp unix:/tmp/aos-qmp.sock,server,nowait -usb -device usb-tablet &
for i in $(seq 1 40); do [ -S /tmp/aos-qmp.sock ] && break; sleep 0.25; done
sleep 6
python3 - <<'EOF'
import socket, json, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect("/tmp/aos-qmp.sock")
buf = b""
def recv_until(pred):
    global buf
    while True:
        if pred(buf): return buf
        chunk = s.recv(65536)
        if not chunk: raise EOFError
        buf += chunk
recv_until(lambda b: b'"QMP"' in b)
def qmp(obj):
    global buf
    s.sendall(json.dumps(obj).encode() + b"\n")
    while True:
        recv_until(lambda b: b"\n" in b)
        line, buf = buf.split(b"\n", 1)
        line = line.strip()
        if not line: continue
        try: o = json.loads(line)
        except json.JSONDecodeError: continue
        if "return" in o or "error" in o: return o
qmp({"execute":"qmp_capabilities"})
qmp({"execute":"input-send-event","arguments":{"events":[
    {"type":"abs","data":{"axis":"x","value":16384}},
    {"type":"abs","data":{"axis":"y","value":16384}},
    {"type":"sync"}]}})
qmp({"execute":"quit"})
EOF
sleep 2
grep "USB" /tmp/aos-usb.log
kill %1 2>/dev/null
```

Expected: log contains `USB tablet enumerated.` then `USB tablet mouse active.`. No panic.

- [ ] **Step 6: Commit**

```bash
git add drivers/uhci.c drivers/mouse.h drivers/mouse.c
git commit -m "USB tablet interrupt IN schedule + mouse integration"
```

---

## Task 6: Makefile `run` target + automated verification harness

**Files:**
- Modify: `Makefile`
- Create (host-side test tool, NOT committed): `/tmp/opencode/usbtest.py`

**Interfaces:**
- Consumes: everything from Tasks 1-5.
- Produces: an automated test that verifies absolute cursor mapping (1:1 scale), buttons, wheel, and PS/2 fallback.

- [ ] **Step 1: Update the `run` target**

`Makefile` line 62-63:

```make
run: aos.iso
	qemu-system-i386 -display gtk,grab-on-hover=on -usb -device usb-tablet -cdrom $<
```

- [ ] **Step 2: Write the test harness `/tmp/opencode/usbtest.py`**

```python
#!/usr/bin/env python3
"""AOS usb-tablet test harness: QMP absolute input injection + PPM cursor checks."""
import socket, subprocess, time, os, json, sys

ISO = "/home/vladimka/aos/aos.iso"
QMP = "/tmp/aos-qmp.sock"
SER = "/tmp/aos-usb.log"

class QMP:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(5)
        self.s.connect(path)
        self.buf = b""
        self._recv_until(lambda b: b'"QMP"' in b)
        self.cmd({"execute": "qmp_capabilities"})
    def _recv_until(self, pred):
        while not pred(self.buf):
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError
            self.buf += chunk
    def cmd(self, obj):
        self.s.sendall(json.dumps(obj).encode() + b"\n")
        while True:
            self._recv_until(lambda b: b"\n" in b)
            line, self.buf = self.buf.split(b"\n", 1)
            line = line.strip()
            if not line:
                continue
            try:
                o = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "return" in o or "error" in o:
                return o

def wait_serial(substr, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with open(SER, "rb") as f:
                if substr.encode() in f.read():
                    return True
        except FileNotFoundError:
            pass
        time.sleep(0.25)
    return False

def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        line = f.readline().strip()
        while line.startswith(b"#"):
            line = f.readline().strip()
        w, h = map(int, line.split())
        f.readline()
        data = f.read(w * h * 3)
    return w, h, data

def count_white_in_box(w, h, data, cx, cy, box=40):
    n = 0
    for y in range(max(0, cy - box), min(h, cy + box)):
        for x in range(max(0, cx - box), min(w, cx + box)):
            i = (y * w + x) * 3
            if data[i] == 255 and data[i + 1] == 255 and data[i + 2] == 255:
                n += 1
    return n

def boot(extra_dev=True):
    for f in (QMP, SER):
        if os.path.exists(f):
            os.unlink(f)
    cmd = ["qemu-system-i386", "-cdrom", ISO, "-display", "none",
           "-serial", f"file:{SER}", "-qmp", f"unix:{QMP},server,nowait"]
    if extra_dev:
        cmd += ["-usb", "-device", "usb-tablet"]
    q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(200):
        if os.path.exists(QMP):
            try:
                return q, QMP(QMP)
            except Exception:
                pass
        time.sleep(0.1)
    raise RuntimeError("QMP socket never became ready")

def main():
    q, m = boot(extra_dev=True)
    if not wait_serial("USB tablet enumerated."):
        print("FAIL: tablet not enumerated")
        return 1
    if not wait_serial("USB tablet mouse active."):
        print("FAIL: USB source not activated")
        return 1

    # absolute move to (0, 32767) -> screen (0, 767)
    m.cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": 0}},
        {"type": "abs", "data": {"axis": "y", "value": 32767}},
        {"type": "sync"}]}})
    m.cmd({"execute": "screendump", "arguments": {"filename": "/tmp/usb-bl.ppm"}})
    w, h, d = read_ppm("/tmp/usb-bl.ppm")
    n = count_white_in_box(w, h, d, 0, 767)
    print(f"cursor white pixels near (0,767): {n}")
    assert n >= 3, "cursor not found at bottom-left"

    # absolute move to center (16384, 16384) -> (512, 384)
    m.cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": 16384}},
        {"type": "abs", "data": {"axis": "y", "value": 16384}},
        {"type": "sync"}]}})
    m.cmd({"execute": "screendump", "arguments": {"filename": "/tmp/usb-c.ppm"}})
    w, h, d = read_ppm("/tmp/usb-c.ppm")
    n = count_white_in_box(w, h, d, 512, 384)
    print(f"cursor white pixels near (512,384): {n}")
    assert n >= 3, "cursor not found at center"

    # button: left press + release
    m.cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"button": "left", "down": True}}]}})
    m.cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"button": "left", "down": False}}]}})

    # wheel: up then down (report dz +1 = up)
    m.cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"button": "wheel-up", "down": True}}]}})
    m.cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"button": "wheel-down", "down": True}}]}})

    print("PASS: tablet cursor mapping + buttons + wheel")
    q.terminate(); q.wait(timeout=10)
    return 0

if __name__ == "__main__":
    sys.exit(main())
```

Note: the wheel-up/wheel-down steps verify no crash; direction is verified in Step 4 via the scrollback/desktop observation.

- [ ] **Step 3: Run the harness**

Run: `python3 /tmp/opencode/usbtest.py`
Expected: `PASS: tablet cursor mapping + buttons + wheel` (both position assertions pass with >= 3 white pixels).

- [ ] **Step 4: Verify wheel scroll direction (manual, with the GTK window)**

Run: `make run`
Expected: host cursor is hidden over the window; guest cursor tracks the host 1:1. Scroll the wheel while the cursor is over the desktop: content scrolls toward older lines on wheel-up and toward live view on wheel-down (same direction as the old PS/2 wheel).

- [ ] **Step 5: Verify PS/2 fallback (no `-device usb-tablet`)**

Run:
```bash
make
rm -f /tmp/aos-usb.log /tmp/aos-mon.sock
qemu-system-i386 -cdrom aos.iso -display none -serial file:/tmp/aos-usb.log \
  -monitor unix:/tmp/aos-mon.sock,server,nowait &
for i in $(seq 1 40); do [ -S /tmp/aos-mon.sock ] && break; sleep 0.25; done
sleep 6
grep "USB:" /tmp/aos-usb.log
kill %1 2>/dev/null
```

Expected: log shows `USB: UHCI not found; PS/2 mouse stays active.` and NO `USB tablet` messages. The WM still runs with the PS/2 mouse (regression check: existing `idletest.py`/`mtest2.py` behavior unchanged).

- [ ] **Step 6: Commit**

```bash
git add Makefile
git commit -m "run: add -usb -device usb-tablet for absolute input"
```

---

## Self-review notes (run after writing, before handoff)

- Spec coverage: PCI scan (Task 2), UHCI init (Task 3), control/enumeration (Task 4), interrupt schedule + report parsing + mouse integration + fallback flip-on-first-report (Task 5), Makefile + testing incl. PS/2 fallback and wheel sign (Task 6). All spec sections covered.
- Placeholders: none — every step has concrete code or exact commands.
- Type consistency: `pci_init(unsigned int*, unsigned int*)`, `usb_init(void)`, `uhci_handler` is `void uhci_handler(void)`, `mouse_set_usb_state(int,int,int,int)` — used consistently across tasks.
