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
