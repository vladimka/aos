#include "uhci.h"
#include "pci.h"
#include "serial.h"
#include "ports.h"
#include "mouse.h"

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

static volatile unsigned int frame_list[1024] __attribute__((aligned(4096)));
static volatile struct uhci_qh ctrl_qh __attribute__((aligned(16)));
static volatile struct uhci_qh intr_qh __attribute__((aligned(16)));

static unsigned int td_read_status(struct uhci_td *td) {
    return *(volatile unsigned int *)&td->status;
}

static int td_active(struct uhci_td *td) {
    return (td_read_status(td) & TD_CTRL_ACTIVE) != 0;
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
    unsigned int st = td_read_status(td);
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
                           (uhci_devaddr << 8) | (dir_in ? USB_PID_OUT : USB_PID_IN);
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
    // QEMU fires the device reset on the PR 0->1 edge and keeps PR set until
    // the guest clears it (real UHCI clears PR itself after ~50 ms). Hold the
    // reset long enough to be real-hardware-correct, then release it and
    // enable the port: QEMU only serves ports with PE set.
    outw(uhci_base + USBPORTSC1, USBPORTSC_PR);
    start = tick;
    while ((int)(tick - start) < 50) { }
    outw(uhci_base + USBPORTSC1, 0);
    start = tick;
    while (inw(uhci_base + USBPORTSC1) & USBPORTSC_PR) {
        if ((int)(tick - start) >= 500) return -2;
    }
    outw(uhci_base + USBPORTSC1, inw(uhci_base + USBPORTSC1) | USBPORTSC_PE);
    start = tick;
    while (!(inw(uhci_base + USBPORTSC1) & USBPORTSC_PE)) {
        if ((int)(tick - start) >= 500) return -3;
    }
    if (inw(uhci_base + USBPORTSC1) & USBPORTSC_LSDA) return -4;
    return 0;
}

static int uhci_enum_tablet(void) {
    if (uhci_port_reset() != 0) {
        serial_print("USB: enum port_reset failed, portsc=0x");
        serial_print_hex(inw(uhci_base + USBPORTSC1));
        serial_print("\n");
        return -1;
    }
    uhci_devaddr = 0;
    if (uhci_control(0x00, 5, 1, 0, 0, 0) != 0) {
        serial_print("USB: enum set_address failed, td_st=0x");
        serial_print_hex(td_read_status(&td_status));
        serial_print("\n");
        return -2;
    }
    uhci_devaddr = 1;
    if (uhci_control(0x80, 6, 0x0100, 0, 18, data_buf) != 0) {
        serial_print("USB: enum get_descriptor failed, td_st=0x");
        serial_print_hex(td_read_status(&td_status));
        serial_print("\n");
        return -3;
    }
    if (data_buf[7] != 8)                        return -4;   /* bMaxPacketSize0 */
    if (data_buf[8] != 0x27 || data_buf[9] != 0x06) return -5; /* idVendor 0x0627 */
    if (data_buf[10] != 0x01 || data_buf[11] != 0x00) return -6; /* idProduct 0x0001 */
    if (uhci_control(0x00, 9, 1, 0, 0, 0) != 0) {
        serial_print("USB: enum set_config failed, td_st=0x");
        serial_print_hex(td_read_status(&td_status));
        serial_print("\n");
        return -7;
    }
    return 0;
}

static struct uhci_td tablet_td __attribute__((aligned(16)));
static unsigned char tablet_buf[8];
static int tablet_present;
static int tablet_td_busy;

void uhci_tablet_poll(void) {
    if (!tablet_present) return;
    if (tablet_td_busy) {
        if (td_active(&tablet_td)) return;
        unsigned int st = td_read_status(&tablet_td);
        tablet_td_busy = 0;
        if (!(st & (TD_CTRL_STALLED | TD_CTRL_DBUFERR | TD_CTRL_BABBLE |
                    TD_CTRL_CRCTIMEO | TD_CTRL_BITSTUFF))) {
            int n = (st & TD_ACTLEN_MASK) + 1;
            if (n >= 6) {
                int b = tablet_buf[0] & 0x07;
                int x = tablet_buf[1] | ((unsigned int)tablet_buf[2] << 8);
                int y = tablet_buf[3] | ((unsigned int)tablet_buf[4] << 8);
                int w = (signed char)tablet_buf[5];
                mouse_tablet_set(x, y, b, w);
            }
        }
    }
    tablet_td.link   = 0x0001;
    tablet_td.status = TD_CTRL_ACTIVE | uhci_maxerr(2);
    tablet_td.token  = uhci_explen(8) | (1u << 15) | (uhci_devaddr << 8) | USB_PID_IN;
    tablet_td.buffer = (unsigned int)tablet_buf;
    intr_qh.element  = (unsigned int)&tablet_td;
    tablet_td_busy   = 1;
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
    if (uhci_enum_tablet() != 0) {
        serial_print("USB: tablet enumeration failed.\n");
        return;
    }
    serial_print("USB tablet enumerated.\n");
    tablet_present = 1;
}
