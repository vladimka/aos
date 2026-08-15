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
static volatile unsigned char status_byte;
static unsigned long long capacity_bytes;

static int vblk_request(unsigned int type, unsigned int sector,
                        void *buf, unsigned int len) {
    req.type = type;
    req.reserved = 0;
    req.sector = sector;
    status_byte = 0xFF;

    unsigned int h = virtio_alloc_desc(gdev, 0);
    unsigned int m = virtio_alloc_desc(gdev, 0);
    unsigned int t = virtio_alloc_desc(gdev, 0);
    if (h == 0xFFFF || m == 0xFFFF || t == 0xFFFF) return -1;

    virtio_desc_set(gdev, 0, h, (unsigned int)&req, sizeof(req), 0);
    virtio_desc_set(gdev, 0, m, (unsigned int)buf, len,
                    type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);
    virtio_desc_set(gdev, 0, t, (unsigned int)&status_byte, 1, VRING_DESC_F_WRITE);
    virtio_desc_link(gdev, 0, h, m);
    virtio_desc_link(gdev, 0, m, t);
    virtio_submit(gdev, 0, h);

    unsigned int start = tick;
    for (;;) {
        unsigned int id, len;
        while (virtio_used_pop(gdev, 0, &id, &len) == 0)
            virtio_free_chain(gdev, 0, id);
        // QEMU completes virtio-blk I/O asynchronously (AIO); a timer IRQ may
        // land between two reads of the status byte. Read it exactly once so
        // both tests observe a consistent value.
        unsigned char st = status_byte;
        if (st == 0) return 0;
        if (st != 0xFF) {
            serial_print("vblk: dev err status=");
            serial_print_hex(st);
            serial_print("\n");
            return -3;   // device-reported error
        }
        if ((int)(tick - start) >= 500) {
            serial_print("vblk: timeout used_idx=");
            serial_print_hex(gdev->vq[0].used->idx);
            serial_print(" avail_idx=");
            serial_print_hex(gdev->vq[0].avail->idx);
            serial_print(" isr=");
            serial_print_hex(inb(gdev->bar + VIRTIO_PCI_ISR));
            serial_print("\n");
            return -2;
        }
    }
}

int vblk_read_multi(unsigned int lba, unsigned int count, void *buf) {
    return vblk_request(VIRTIO_BLK_T_IN, lba, buf, count * SECTOR_SIZE);
}

int vblk_write_multi(unsigned int lba, unsigned int count, const void *buf) {
    return vblk_request(VIRTIO_BLK_T_OUT, lba, (void *)buf,
                        count * SECTOR_SIZE);
}

int vblk_read(unsigned int lba, void *buf) {
    return vblk_read_multi(lba, 1, buf);
}

int vblk_write(unsigned int lba, const void *buf) {
    return vblk_write_multi(lba, 1, buf);
}

int vblk_present(void) {
    return gdev != 0;
}

unsigned int vblk_capacity_sectors(void) {
    return (unsigned int)(capacity_bytes / SECTOR_SIZE);
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
    // virtio-blk reports capacity in 512-byte sectors.
    capacity_bytes = (((unsigned long long)hi << 32) | lo) * SECTOR_SIZE;
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

    // Multi-sector round-trip through a single request.
    unsigned char wm[4 * SECTOR_SIZE], rm[4 * SECTOR_SIZE];
    for (unsigned int i = 0; i < sizeof(wm); i++)
        wm[i] = (unsigned char)(i * 13 + 5);
    unsigned int base = last - 8;
    if (vblk_write_multi(base, 4, wm) == 0 && vblk_read_multi(base, 4, rm) == 0) {
        int ok = 1;
        for (unsigned int i = 0; i < (int)sizeof(rm); i++)
            if (wm[i] != rm[i]) { ok = 0; break; }
        serial_print(ok ? "blk: selftest multi OK\n" : "blk: selftest multi FAIL\n");
    } else {
        serial_print("blk: selftest multi FAIL\n");
    }
}
