#include "vrng.h"
#include "virtio.h"
#include "serial.h"

extern volatile unsigned int tick;

static struct virtio_dev *gdev;
static unsigned char rng_buf[4096] __attribute__((aligned(4096)));
static unsigned int pool_len;      // bytes currently filled in rng_buf
static unsigned int pool_off;

// Submit one WRITE descriptor and spin for completion. Returns 0 on success.
static int vrng_fetch(void) {
    unsigned int start = tick;
    unsigned int head = virtio_alloc_desc(gdev, 0);
    if (head == 0xFFFF) return -2;
    virtio_desc_set(gdev, 0, head, (unsigned int)rng_buf, 4096,
                    VRING_DESC_F_WRITE);
    virtio_submit(gdev, 0, head);
    for (;;) {
        unsigned int id, len;
        if (virtio_used_pop(gdev, 0, &id, &len) == 0) {
            struct virtio_vq *vq = &gdev->vq[0];
            vq->desc[id].next = vq->free_head;
            vq->free_head = id;
            pool_len = len;
            pool_off = 0;
            return 0;
        }
        if ((int)(tick - start) >= 500) return -3;
    }
}

int vrng_bytes(void *buf, unsigned int n) {
    if (!gdev) return -1;
    if (n > 4096) n = 4096;
    unsigned char *out = (unsigned char *)buf;
    unsigned int filled = 0;
    while (filled < n) {
        if (pool_off >= pool_len) {
            int rc = vrng_fetch();
            if (rc < 0) return rc;
        }
        unsigned int take = n - filled;
        if (take > pool_len - pool_off) take = pool_len - pool_off;
        for (unsigned int i = 0; i < take; i++)
            out[filled + i] = rng_buf[pool_off + i];
        pool_off += take;
        filled += take;
    }
    return (int)filled;
}

void vrng_init(void) {
    static struct virtio_dev d;
    if (virtio_probe_pci(&d, VIRTIO_DEV_RNG) < 0) return;
    if (!virtio_legacy_probe(d.bar)) {
        serial_print("vrng: not legacy\n");
        return;
    }
    unsigned int features;
    if (virtio_dev_init(&d, 0, &features) < 0) {
        serial_print("vrng: init failed\n");
        return;
    }
    if (virtio_setup_queue(&d, 0, VIRTQ_NUM) < 0) {
        serial_print("vrng: queue failed\n");
        return;
    }
    virtio_ready(&d);
    virtio_register(&d);
    gdev = &d;
    unsigned char t[16];
    if (vrng_bytes(t, 16) == 16) {
        serial_print("rng: selftest OK ");
        for (int i = 0; i < 16; i++)
            serial_print_hex(t[i]);
        serial_print("\n");
    } else {
        serial_print("rng: selftest FAIL\n");
    }
}
