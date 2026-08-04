#include "virtio.h"
#include "pci.h"
#include "ports.h"
#include "serial.h"
#include "pmm.h"
#include "interrupts.h"

static struct virtio_dev *dev_list;

int virtio_legacy_probe(unsigned int bar) {
    return inl(bar + VIRTIO_PCI_HOST_FEATURES) != 0xFFFFFFFFu;
}

int virtio_dev_init(struct virtio_dev *d, unsigned int supported,
                    unsigned int *features_out) {
    outb(d->bar + VIRTIO_PCI_STATUS, 0);                       // reset
    outb(d->bar + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    outb(d->bar + VIRTIO_PCI_STATUS,
         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    unsigned int host = inl(d->bar + VIRTIO_PCI_HOST_FEATURES);
    if (host == 0xFFFFFFFFu) return -1;
    unsigned int guest = host & supported;
    outl(d->bar + VIRTIO_PCI_GUEST_FEATURES, guest);
    d->features = guest;
    outb(d->bar + VIRTIO_PCI_STATUS,
         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    if (features_out) *features_out = guest;
    return 0;
}

int virtio_setup_queue(struct virtio_dev *d, unsigned int qidx, unsigned int n) {
    outw(d->bar + VIRTIO_PCI_QUEUE_SEL, qidx);
    unsigned short qnum = inw(d->bar + VIRTIO_PCI_QUEUE_NUM);
    if (qnum < n) return -1;
    unsigned int base = (unsigned int)page_alloc_order(2);   // 16 KiB, contiguous
    if (!base) return -2;
    struct virtio_vq *vq = &d->vq[qidx];
    vq->desc = (volatile struct vring_desc *)base;
    vq->avail = (volatile struct vring_avail *)(base + 4096);
    vq->used = (volatile struct vring_used *)(base + 8192);
    vq->size = n;
    for (unsigned int i = 0; i < n; i++) {
        vq->desc[i].addr = 0;
        vq->desc[i].len = 0;
        vq->desc[i].flags = 0;
        vq->desc[i].next = (i + 1 < n) ? i + 1 : 0xFFFF;
    }
    vq->free_head = 0;
    vq->last_used = 0;
    vq->avail->flags = 0;
    vq->avail->idx = 0;
    vq->used->idx = 0;
    outl(d->bar + VIRTIO_PCI_QUEUE_PFN, base >> 12);
    return 0;
}

unsigned int virtio_alloc_desc(struct virtio_dev *d, unsigned int qidx) {
    struct virtio_vq *vq = &d->vq[qidx];
    unsigned int i = vq->free_head;
    if (i == 0xFFFF) return 0xFFFF;
    vq->free_head = vq->desc[i].next;
    return i;
}

void virtio_desc_set(struct virtio_dev *d, unsigned int qidx, unsigned int idx,
                     unsigned int addr, unsigned int len, unsigned short flags) {
    struct virtio_vq *vq = &d->vq[qidx];
    vq->desc[idx].addr = addr;
    vq->desc[idx].len = len;
    vq->desc[idx].flags = flags & ~VRING_DESC_F_NEXT;
}

void virtio_desc_link(struct virtio_dev *d, unsigned int qidx,
                      unsigned int from, unsigned int to) {
    struct virtio_vq *vq = &d->vq[qidx];
    vq->desc[from].next = to;
    vq->desc[from].flags |= VRING_DESC_F_NEXT;
}

void virtio_submit(struct virtio_dev *d, unsigned int qidx, unsigned int head) {
    struct virtio_vq *vq = &d->vq[qidx];
    vq->avail->ring[vq->avail->idx % vq->size] = head;
    vq->avail->idx++;
    outw(d->bar + VIRTIO_PCI_QUEUE_NOTIFY, qidx);
}

int virtio_used_pop(struct virtio_dev *d, unsigned int qidx,
                    unsigned int *id, unsigned int *len) {
    struct virtio_vq *vq = &d->vq[qidx];
    if (vq->used->idx == vq->last_used) return -1;
    unsigned int i = vq->last_used % vq->size;
    if (id) *id = vq->used->ring[i].id;
    if (len) *len = vq->used->ring[i].len;
    vq->last_used++;
    return 0;
}

// One dispatch handles every virtio IRQ: it reads each device's ISR register.
// An idle device reads ISR == 0, so checking the whole list per IRQ is safe.
static void virtio_irq_dispatch(void) {
    for (struct virtio_dev *d = dev_list; d; d = d->next) {
        unsigned char isr = inb(d->bar + VIRTIO_PCI_ISR);
        if (isr & 1) {
            if (d->on_irq) d->on_irq(d);
        }
        // bit 2 = config change: ignored in this iteration
    }
}

void virtio_register(struct virtio_dev *d) {
    d->next = dev_list;
    dev_list = d;
    irq_install_handler(d->irq, virtio_irq_dispatch);
}

int virtio_probe_pci(struct virtio_dev *d, unsigned int device_id) {
    struct pci_dev list[8];
    int n = pci_find_all(list, 8);
    for (int i = 0; i < n; i++) {
        if (list[i].vendor != VIRTIO_PCI_VENDOR || list[i].device != device_id)
            continue;
        d->bar = list[i].bar0 & ~0x3u;
        d->irq = list[i].irq;
        d->pci_bus = list[i].bus;
        d->pci_dev = list[i].dev;
        d->pci_func = list[i].func;
        d->features = 0;
        d->on_irq = 0;
        d->next = 0;
        unsigned int cmd = pci_config_read(list[i].bus, list[i].dev,
                                           list[i].func, 0x04);
        pci_config_write(list[i].bus, list[i].dev, list[i].func, 0x04, cmd | 0x7);
        return 0;
    }
    return -1;
}

// Task 1: transport probe only — logs every virtio device found on the bus.
// Later tasks replace this body with calls to vrng_init/vblk_init/vnet_init.
void virtio_init(void) {
    struct pci_dev list[8];
    int n = pci_find_all(list, 8);
    for (int i = 0; i < n; i++) {
        if (list[i].vendor != VIRTIO_PCI_VENDOR) continue;
        serial_print("virtio: dev=");
        serial_print_hex(list[i].device);
        serial_print(" bar=");
        serial_print_hex(list[i].bar0 & ~0x3u);
        serial_print(" irq=");
        serial_print_dec(list[i].irq);
        serial_print("\n");
    }
}
