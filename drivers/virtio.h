#ifndef VIRTIO_H
#define VIRTIO_H

#define VIRTIO_PCI_VENDOR 0x1AF4
#define VIRTIO_DEV_NET 0x1000
#define VIRTIO_DEV_BLK 0x1001
#define VIRTIO_DEV_RNG 0x1005

#define VIRTIO_PCI_HOST_FEATURES  0x00
#define VIRTIO_PCI_GUEST_FEATURES 0x04
#define VIRTIO_PCI_QUEUE_PFN      0x08
#define VIRTIO_PCI_QUEUE_NUM      0x0C
#define VIRTIO_PCI_QUEUE_SEL      0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY   0x10
#define VIRTIO_PCI_STATUS         0x12
#define VIRTIO_PCI_ISR            0x13
#define VIRTIO_PCI_DEV_CFG        0x14

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FAILED      0x80

#define VIRTQ_NUM 16

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

struct vring_desc {
    unsigned long long addr;
    unsigned int len;
    unsigned short flags;
    unsigned short next;
} __attribute__((packed));

struct vring_avail {
    unsigned short flags;
    unsigned short idx;
    unsigned short ring[VIRTQ_NUM];
} __attribute__((packed));

struct vring_used_elem {
    unsigned int id;
    unsigned int len;
} __attribute__((packed));

struct vring_used {
    unsigned short flags;
    unsigned short idx;
    struct vring_used_elem ring[VIRTQ_NUM];
} __attribute__((packed));

struct virtio_vq {
    volatile struct vring_desc *desc;
    volatile struct vring_avail *avail;
    volatile struct vring_used *used;
    unsigned short size;
    unsigned short free_head;
    unsigned short last_used;
};

struct virtio_dev {
    struct virtio_dev *next;
    unsigned int bar;
    unsigned int irq;
    unsigned char pci_bus, pci_dev, pci_func;
    unsigned int features;
    struct virtio_vq vq[2];
    void (*on_irq)(struct virtio_dev *d);
};

void virtio_init(void);
int virtio_legacy_probe(unsigned int bar);
int virtio_dev_init(struct virtio_dev *d, unsigned int supported,
                    unsigned int *features_out);
void virtio_ready(struct virtio_dev *d);
int virtio_setup_queue(struct virtio_dev *d, unsigned int qidx, unsigned int n);
void virtio_register(struct virtio_dev *d);
void virtio_irq_dispatch(void);
int virtio_probe_pci(struct virtio_dev *d, unsigned int device_id);
unsigned int virtio_alloc_desc(struct virtio_dev *d, unsigned int qidx);
void virtio_desc_set(struct virtio_dev *d, unsigned int qidx, unsigned int idx,
                     unsigned int addr, unsigned int len, unsigned short flags);
void virtio_desc_link(struct virtio_dev *d, unsigned int qidx,
                      unsigned int from, unsigned int to);
void virtio_submit(struct virtio_dev *d, unsigned int qidx, unsigned int head);
void virtio_free_chain(struct virtio_dev *d, unsigned int qidx, unsigned int head);
int virtio_used_pop(struct virtio_dev *d, unsigned int qidx,
                    unsigned int *id, unsigned int *len);

#endif
