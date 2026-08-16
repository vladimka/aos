#include "virtio_modern.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"

static inline unsigned int vm_cfg32(struct virtio_modern *m, unsigned int off) {
    return *(volatile unsigned int *)(m->common + off);
}
static inline void vm_cfg_w32(struct virtio_modern *m, unsigned int off, unsigned int v) {
    *(volatile unsigned int *)(m->common + off) = v;
}
static inline void vm_cfg_w16(struct virtio_modern *m, unsigned int off, unsigned short v) {
    *(volatile unsigned short *)(m->common + off) = v;
}
static inline unsigned short vm_cfg_r16(struct virtio_modern *m, unsigned int off) {
    return *(volatile unsigned short *)(m->common + off);
}
static inline void vm_cfg_w8(struct virtio_modern *m, unsigned int off, unsigned char v) {
    *(volatile unsigned char *)(m->common + off) = v;
}
static inline unsigned char vm_cfg_r8(struct virtio_modern *m, unsigned int off) {
    return *(volatile unsigned char *)(m->common + off);
}

int vm_probe(struct virtio_modern *m, unsigned int device_id) {
    struct pci_dev list[8];
    int n = pci_find_all(list, 8);
    int idx = -1;
    for (int i = 0; i < n; i++)
        if (list[i].vendor == 0x1AF4 && list[i].device == device_id) { idx = i; break; }
    if (idx < 0) return -1;
    m->bus = list[idx].bus; m->dev = list[idx].dev; m->func = list[idx].func;

    struct vm_cap cap;
    unsigned int common_bar = 0xFF, notify_bar = 0xFF, isr_bar = 0xFF;
    unsigned int common_off = 0, notify_off = 0, isr_off = 0;
    unsigned int common_len = 0, notify_len = 0, isr_len = 0;
    unsigned int notify_multiplier = 0;

    unsigned int caps = pci_config_read(m->bus, m->dev, m->func, 0x34) & 0xFF;
    unsigned int cp = caps;
    while (cp) {
        unsigned char vndr = pci_config_read(m->bus, m->dev, m->func, cp) & 0xFF;
        unsigned char nxt = (pci_config_read(m->bus, m->dev, m->func, cp) >> 8) & 0xFF;
        if (vndr == 0x09) {
            unsigned int *p = (unsigned int *)&cap;
            p[0] = pci_config_read(m->bus, m->dev, m->func, cp);
            p[1] = pci_config_read(m->bus, m->dev, m->func, cp + 4);
            p[2] = pci_config_read(m->bus, m->dev, m->func, cp + 8);
            p[3] = pci_config_read(m->bus, m->dev, m->func, cp + 0xC);
            switch (cap.cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                common_bar = cap.bar; common_off = cap.offset; common_len = cap.length;
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                notify_bar = cap.bar; notify_off = cap.offset; notify_len = cap.length;
                notify_multiplier = pci_config_read(m->bus, m->dev, m->func, cp + 0x10);
                break;
            case VIRTIO_PCI_CAP_ISR_CFG:
                isr_bar = cap.bar; isr_off = cap.offset; isr_len = cap.length;
                break;
            default:
                break;
            }
        }
        cp = nxt;
    }

    if (common_bar > 5) return -1;
    if (notify_bar > 5) return -1;

    unsigned long long bar_addr[6] = {0};
    unsigned int bar_need[6] = {0};
    unsigned int used_mask = 0;
    unsigned int bars[3] = { common_bar, notify_bar, isr_bar };
    unsigned int offs[3] = { common_off, notify_off, isr_off };
    unsigned int lens[3] = { common_len, notify_len, isr_len };
    for (int i = 0; i < 3; i++) {
        unsigned int b = bars[i];
        if (b > 5) continue;
        if (!(used_mask & (1u << b))) {
            used_mask |= 1u << b;
            unsigned int raw = pci_config_read(m->bus, m->dev, m->func, 0x10 + 4 * b);
            unsigned long long addr = raw & ~0xFu;
            if ((raw & 0x6u) == 0x4) {
                unsigned int hi = pci_config_read(m->bus, m->dev, m->func, 0x10 + 4 * (b + 1));
                addr |= (unsigned long long)hi << 32;
            }
            bar_addr[b] = addr;
        }
        unsigned int len = lens[i] ? lens[i] : 4096;
        unsigned int end = offs[i] + len;
        if (end > bar_need[b]) bar_need[b] = end;
    }

    for (unsigned int b = 0; b < 6; b++) {
        if (!(used_mask & (1u << b))) continue;
        if (bar_addr[b] >> 32) return -1;
        unsigned int pages = (bar_need[b] + 4095) & ~4095u;
        paging_identity_map((unsigned int)bar_addr[b], pages);
    }

    unsigned int cmd = pci_config_read(m->bus, m->dev, m->func, 0x04) & 0xFFFF;
    pci_config_write(m->bus, m->dev, m->func, 0x04, cmd | 0x7);

    m->common = (volatile unsigned char *)((unsigned int)bar_addr[common_bar] + common_off);
    m->notify = (volatile unsigned char *)((unsigned int)bar_addr[notify_bar] + notify_off);
    m->isr = (isr_bar <= 5) ? (volatile unsigned char *)((unsigned int)bar_addr[isr_bar] + isr_off) : 0;
    m->notify_multiplier = notify_multiplier;
    return 0;
}

int vm_dev_init(struct virtio_modern *m, unsigned long long supported) {
    vm_cfg_w8(m, 0x14, 0);                              // reset
    vm_cfg_w8(m, 0x14, VM_STATUS_ACKNOWLEDGE);
    vm_cfg_w8(m, 0x14, VM_STATUS_ACKNOWLEDGE | VM_STATUS_DRIVER);

    unsigned long long host = 0;
    vm_cfg_w32(m, 0x00, 0);                             // select device features 0
    host |= (unsigned long long)vm_cfg32(m, 0x04);
    vm_cfg_w32(m, 0x00, 1);                             // select device features 1
    host |= (unsigned long long)vm_cfg32(m, 0x04) << 32;

    unsigned long long guest = host & supported;
    vm_cfg_w32(m, 0x08, 0);                             // select guest features 0
    vm_cfg_w32(m, 0x0C, (unsigned int)guest);
    vm_cfg_w32(m, 0x08, 1);                             // select guest features 1
    vm_cfg_w32(m, 0x0C, (unsigned int)(guest >> 32));

    vm_cfg_w8(m, 0x14, VM_STATUS_ACKNOWLEDGE | VM_STATUS_DRIVER | VM_STATUS_FEATURES_OK);
    if (vm_cfg_r8(m, 0x14) & VM_STATUS_FAILED) {
        vm_cfg_w8(m, 0x14, 0);
        return -1;
    }
    return 0;
}

int vm_setup_queue(struct virtio_modern *m, unsigned int qidx, unsigned int n) {
    if (qidx >= 2) return -1;
    vm_cfg_w16(m, 0x16, qidx);                          // queue_select
    unsigned int qnum = vm_cfg_r16(m, 0x18);            // queue_size
    if (qnum == 0) return -1;
    if (qnum < n) n = qnum;
    unsigned int avail_off = qnum * sizeof(struct vring_desc);
    unsigned int used_off = (avail_off + 4 + 2 * qnum + 4095) & ~4095u;
    unsigned int need = used_off + 8 * qnum + 8;
    int order = 0;
    while ((4096u << order) < need) order++;
    unsigned int base = (unsigned int)page_alloc_order(order);
    if (!base) return -2;
    m->desc[qidx] = (volatile struct vring_desc *)base;
    m->avail[qidx] = (volatile struct vring_avail *)(base + avail_off);
    m->used[qidx] = (volatile struct vring_used *)(base + used_off);
    m->size[qidx] = qnum;
    for (unsigned int i = 0; i < qnum; i++) {
        m->desc[qidx][i].addr = 0;
        m->desc[qidx][i].len = 0;
        m->desc[qidx][i].flags = 0;
        m->desc[qidx][i].next = (i + 1 < qnum) ? i + 1 : 0xFFFF;
    }
    m->free_head[qidx] = 0;
    m->last_used[qidx] = 0;
    m->avail[qidx]->flags = 0;
    m->avail[qidx]->idx = 0;
    m->used[qidx]->idx = 0;

    vm_cfg_w32(m, 0x20, base);                          // queue_desc lo
    vm_cfg_w32(m, 0x24, 0);                             // queue_desc hi
    vm_cfg_w32(m, 0x28, base + avail_off);              // queue_avail lo
    vm_cfg_w32(m, 0x2C, 0);                             // queue_avail hi
    vm_cfg_w32(m, 0x30, base + used_off);               // queue_used lo
    vm_cfg_w32(m, 0x34, 0);                             // queue_used hi
    vm_cfg_w16(m, 0x1C, 1);                             // queue_enable
    m->notify_off[qidx] = vm_cfg_r16(m, 0x1E);          // queue_notify_off
    return 0;
}

void vm_ready(struct virtio_modern *m) {
    unsigned char s = vm_cfg_r8(m, 0x14);
    vm_cfg_w8(m, 0x14, s | VM_STATUS_DRIVER_OK);
}

unsigned int vm_alloc_desc(struct virtio_modern *m, unsigned int qidx) {
    if (qidx >= 2) return 0xFFFF;
    unsigned int i = m->free_head[qidx];
    if (i == 0xFFFF) return 0xFFFF;
    m->free_head[qidx] = m->desc[qidx][i].next;
    return i;
}

void vm_desc_set(struct virtio_modern *m, unsigned int qidx, unsigned int idx,
                 unsigned int addr, unsigned int len, unsigned short flags) {
    if (qidx >= 2) return;
    m->desc[qidx][idx].addr = addr;
    m->desc[qidx][idx].len = len;
    m->desc[qidx][idx].flags = flags & ~VRING_DESC_F_NEXT;
}

void vm_submit(struct virtio_modern *m, unsigned int qidx, unsigned int head) {
    if (qidx >= 2) return;
    m->avail[qidx]->ring[m->avail[qidx]->idx % m->size[qidx]] = head;
    m->avail[qidx]->idx++;
    *(volatile unsigned short *)(m->notify + m->notify_off[qidx] * m->notify_multiplier) = qidx;
}

void vm_free_chain(struct virtio_modern *m, unsigned int qidx, unsigned int head) {
    if (qidx >= 2) return;
    unsigned int i = head;
    while (i != 0xFFFF) {
        unsigned int next = m->desc[qidx][i].next;
        unsigned short flags = m->desc[qidx][i].flags;
        m->desc[qidx][i].next = m->free_head[qidx];
        m->free_head[qidx] = i;
        if (!(flags & VRING_DESC_F_NEXT)) break;
        i = next;
    }
}

int vm_used_pop(struct virtio_modern *m, unsigned int qidx,
                unsigned int *id, unsigned int *len) {
    if (qidx >= 2) return -1;
    if (m->used[qidx]->idx == m->last_used[qidx]) return -1;
    unsigned int i = m->last_used[qidx] % m->size[qidx];
    if (id) *id = m->used[qidx]->ring[i].id;
    if (len) *len = m->used[qidx]->ring[i].len;
    m->last_used[qidx]++;
    return 0;
}
