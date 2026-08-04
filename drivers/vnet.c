#include "vnet.h"
#include "virtio.h"
#include "serial.h"
#include "pmm.h"
#include "ports.h"

#define RX_BUFS 8
#define FRAME_MAX 2048
#define NET_HDR_LEN 10
#define VIRTIO_NET_F_MAC (1u << 5)

static struct virtio_dev *gdev;
static unsigned char *rx_bufs[RX_BUFS];
static unsigned char tx_hdr[NET_HDR_LEN];

// One static zeroed header is safe because TX is fire-and-forget and the echo
// path sends at most one frame at a time from the IRQ handler.
int vnet_send(const unsigned char *frame, unsigned int len) {
    if (!gdev || len == 0 || len > FRAME_MAX - NET_HDR_LEN) return -1;
    // Recycle completed TX descriptors so the queue never exhausts.
    unsigned int id, rlen;
    while (virtio_used_pop(gdev, 1, &id, &rlen) == 0)
        virtio_free_chain(gdev, 1, id);
    unsigned int h = virtio_alloc_desc(gdev, 1);
    unsigned int m = virtio_alloc_desc(gdev, 1);
    if (h == 0xFFFF || m == 0xFFFF) return -2;
    virtio_desc_set(gdev, 1, h, (unsigned int)tx_hdr, NET_HDR_LEN, 0);
    virtio_desc_set(gdev, 1, m, (unsigned int)frame, len, 0);
    virtio_desc_link(gdev, 1, h, m);
    virtio_submit(gdev, 1, h);
    return 0;
}

static void vnet_on_irq(struct virtio_dev *d) {
    unsigned int id, len;
    while (virtio_used_pop(d, 0, &id, &len) == 0) {
        unsigned int frame_len = len >= NET_HDR_LEN ? len - NET_HDR_LEN : 0;
        serial_print("net: RX frame len=");
        serial_print_dec(frame_len);
        serial_print("\n");
        if (frame_len)
            vnet_send(rx_bufs[id] + NET_HDR_LEN, frame_len);   // echo (loopback)
        virtio_desc_set(d, 0, id, (unsigned int)rx_bufs[id], FRAME_MAX,
                        VRING_DESC_F_WRITE);
        virtio_submit(d, 0, id);
    }
}

void vnet_init(void) {
    static struct virtio_dev d;
    if (virtio_probe_pci(&d, VIRTIO_DEV_NET) < 0) return;
    if (!virtio_legacy_probe(d.bar)) {
        serial_print("vnet: not legacy\n");
        return;
    }
    unsigned int features;
    if (virtio_dev_init(&d, VIRTIO_NET_F_MAC, &features) < 0) {
        serial_print("vnet: init failed\n");
        return;
    }
    if (virtio_setup_queue(&d, 0, VIRTQ_NUM) < 0 ||   // rx
        virtio_setup_queue(&d, 1, VIRTQ_NUM) < 0) {   // tx
        serial_print("vnet: queue failed\n");
        return;
    }
    for (int i = 0; i < RX_BUFS; i++) {
        rx_bufs[i] = page_alloc_zero();
        if (!rx_bufs[i]) {
            serial_print("vnet: no buffers\n");
            return;
        }
    }
    if (features & VIRTIO_NET_F_MAC) {
        serial_print("vnet: mac=");
        for (int i = 0; i < 6; i++) {
            serial_print_hex(inb(d.bar + VIRTIO_PCI_DEV_CFG + i));
            if (i < 5) serial_print(":");
        }
        serial_print("\n");
    }
    for (int i = 0; i < NET_HDR_LEN; i++)
        tx_hdr[i] = 0;
    d.on_irq = vnet_on_irq;
    virtio_register(&d);
    gdev = &d;
    for (int i = 0; i < RX_BUFS; i++) {
        unsigned int head = virtio_alloc_desc(&d, 0);
        virtio_desc_set(&d, 0, head, (unsigned int)rx_bufs[i], FRAME_MAX,
                        VRING_DESC_F_WRITE);
        virtio_submit(&d, 0, head);
    }
    // DRIVER_OK: without it QEMU's is_guest_ready() is false and incoming
    // frames are dropped before they ever reach the RX queue.
    virtio_ready(&d);
    serial_print("vnet: ready\n");
}
