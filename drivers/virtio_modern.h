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
#define VM_STATUS_FAILED      0x80

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
