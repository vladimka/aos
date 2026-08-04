#include "vblk.h"
#include "virtio.h"
#include "serial.h"
#include "string.h"
#include "ports.h"

#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1
#define SECTOR_SIZE 512

struct virtio_blk_req {
    unsigned int type;
    unsigned int reserved;
    unsigned long long sector;
} __attribute__((packed));

extern volatile unsigned int tick;

static struct virtio_dev *gdev;
static struct virtio_blk_req req;
static unsigned char databuf[SECTOR_SIZE];
static unsigned char status_byte;
static unsigned long long capacity_bytes;

static int vblk_request(unsigned int type, unsigned int sector) {
    req.type = type;
    req.reserved = 0;
    req.sector = sector;
    status_byte = 0xFF;

    unsigned int h = virtio_alloc_desc(gdev, 0);
    unsigned int m = virtio_alloc_desc(gdev, 0);
    unsigned int t = virtio_alloc_desc(gdev, 0);
    if (h == 0xFFFF || m == 0xFFFF || t == 0xFFFF) return -1;

    virtio_desc_set(gdev, 0, h, (unsigned int)&req, sizeof(req), 0);
    virtio_desc_set(gdev, 0, m, (unsigned int)databuf, SECTOR_SIZE,
                    type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);
    virtio_desc_set(gdev, 0, t, (unsigned int)&status_byte, 1, VRING_DESC_F_WRITE);
    virtio_desc_link(gdev, 0, h, m);
    virtio_desc_link(gdev, 0, m, t);
    virtio_submit(gdev, 0, h);

    unsigned int start = tick;
    for (;;) {
        unsigned int id, len;
        while (virtio_used_pop(gdev, 0, &id, &len) == 0) {
            struct virtio_vq *vq = &gdev->vq[0];
            vq->desc[id].next = vq->free_head;
            vq->free_head = id;
        }
        if (status_byte == 0) return 0;
        if (status_byte == 0xFF) {
            if ((int)(tick - start) >= 500) return -2;
        } else {
            return -3;   // device-reported error
        }
    }
}

int vblk_read(unsigned int lba, void *buf) {
    int rc = vblk_request(VIRTIO_BLK_T_IN, lba);
    if (rc < 0) return rc;
    memcpy(buf, databuf, SECTOR_SIZE);
    return 0;
}

int vblk_write(unsigned int lba, const void *buf) {
    memcpy(databuf, buf, SECTOR_SIZE);
    return vblk_request(VIRTIO_BLK_T_OUT, lba);
}

int vblk_present(void) {
    return gdev != 0;
}

unsigned int vblk_capacity_bytes(void) {
    return (unsigned int)(capacity_bytes & 0xFFFFFFFFu);
}

void vblk_init(void) {
    static struct virtio_dev d;
    if (virtio_probe_pci(&d, VIRTIO_DEV_BLK) < 0) return;
    if (!virtio_legacy_probe(d.bar)) {
        serial_print("vblk: not legacy\n");
        return;
    }
    unsigned int features;
    if (virtio_dev_init(&d, 0, &features) < 0) {
        serial_print("vblk: init failed\n");
        return;
    }
    if (virtio_setup_queue(&d, 0, VIRTQ_NUM) < 0) {
        serial_print("vblk: queue failed\n");
        return;
    }
    virtio_ready(&d);
    virtio_register(&d);
    gdev = &d;
    unsigned int lo = inl(d.bar + VIRTIO_PCI_DEV_CFG);
    unsigned int hi = inl(d.bar + VIRTIO_PCI_DEV_CFG + 4);
    capacity_bytes = ((unsigned long long)hi << 32) | lo;
    serial_print("vblk: ready, capacity=");
    serial_print_hex((unsigned int)(capacity_bytes >> 20));
    serial_print(" MiB\n");

    // Selftest: write a pattern to the last sector and read it back.
    unsigned char w[SECTOR_SIZE], r[SECTOR_SIZE];
    for (unsigned int i = 0; i < SECTOR_SIZE; i++)
        w[i] = (unsigned char)(i * 7 + 3);
    unsigned int last = (unsigned int)(capacity_bytes / SECTOR_SIZE) - 1;
    if (vblk_write(last, w) == 0 && vblk_read(last, r) == 0) {
        int ok = 1;
        for (unsigned int i = 0; i < SECTOR_SIZE; i++)
            if (w[i] != r[i]) { ok = 0; break; }
        serial_print(ok ? "blk: selftest OK\n" : "blk: selftest FAIL\n");
    } else {
        serial_print("blk: selftest FAIL\n");
    }
}
