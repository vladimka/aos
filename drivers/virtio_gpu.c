#include "virtio_gpu.h"
#include "virtio_modern.h"
#include "serial.h"
#include "string.h"
#include "interrupts.h"

extern volatile unsigned int tick;

#define GPU_BASE   0x04000000
#define GPU_STRIDE 0x300000        // 3 MiB per buffer
#define FB_W       1024
#define FB_H       768
#define FB_PITCH   (FB_W * 4)

static struct virtio_modern vgpu;
static int gpu_active;
static int front;                  // 0 or 1: currently displayed buffer
static unsigned char cmd_buf[16384] __attribute__((aligned(16)));
static unsigned char resp_buf[64] __attribute__((aligned(16)));

// Ack the device interrupt (reading the modern ISR status register clears it
// and deasserts INTx). Installed as the IRQ handler so a level-triggered line
// that raises mid-submit does not starve the driver's used-ring polling.
static void vgu_irq(void) {
    if (vgpu.isr) *(volatile unsigned char *)vgpu.isr;
}

// ---- low-level command submission (controlq, qidx 0) ----
// A command is a two-descriptor chain: [cmd | resp]. The device reads the
// request from the first (readable) descriptor and writes the response into
// the second (writable) one; the used entry carries the head id.
static int vgu_send(unsigned int qidx, unsigned int len) {
    unsigned int head = vm_alloc_desc(&vgpu, qidx);
    if (head == 0xFFFF) return -1;
    unsigned int rhead = vm_alloc_desc(&vgpu, qidx);
    if (rhead == 0xFFFF) { vm_free_chain(&vgpu, qidx, head); return -1; }
    vgpu.desc[qidx][head].addr = (unsigned int)cmd_buf;
    vgpu.desc[qidx][head].len = len;
    vgpu.desc[qidx][head].flags = VRING_DESC_F_NEXT;
    vgpu.desc[qidx][head].next = rhead;
    vgpu.desc[qidx][rhead].addr = (unsigned int)resp_buf;
    vgpu.desc[qidx][rhead].len = sizeof(struct vgpu_hdr);
    vgpu.desc[qidx][rhead].flags = VRING_DESC_F_WRITE;
    vm_submit(&vgpu, qidx, head);
    // poll used ring (device replies on the same queue)
    unsigned int start = tick;
    while ((int)(tick - start) < 500) {
        unsigned int id, rlen;
        if (vm_used_pop(&vgpu, qidx, &id, &rlen) == 0) {
            vm_free_chain(&vgpu, qidx, id);
            return 0;
        }
    }
    vm_free_chain(&vgpu, qidx, head);
    // Note: on timeout the used entry is intentionally NOT consumed. If the
    // device ever completed this command very late, its stale used entry would
    // be popped by the next command's poll and free a chain that is in flight.
    // Only reachable when a wedged device recovers late; matches vrng behavior.
    return -1;
}

static int vgu_cmd(unsigned int type, const void *payload, unsigned int plen) {
    struct vgpu_hdr *h = (struct vgpu_hdr *)cmd_buf;
    h->type = type; h->flags = 0; h->fence_id = 0;
    h->ctx_id = 0; h->padding = 0;
    unsigned int n = sizeof(struct vgpu_hdr) + plen;
    if (n > sizeof(cmd_buf)) return -1;
    if (plen) memcpy(cmd_buf + sizeof(struct vgpu_hdr), payload, plen);
    if (vgu_send(0, n) != 0) return -1;
    struct vgpu_hdr *rh = (struct vgpu_hdr *)resp_buf;
    return rh->type == VGPU_RESP_OK_NODATA ? 0 : -1;
}

// ---- resource create + attach backing ----
static int vgu_create(unsigned int rid, unsigned int w, unsigned int h) {
    struct { unsigned int rid, fmt, w, h; } p;
    p.rid = rid; p.fmt = VGPU_FORMAT_B8G8R8X8; p.w = w; p.h = h;
    return vgu_cmd(VGPU_CMD_RESOURCE_CREATE_2D, &p, sizeof(p));
}

static int vgu_attach(unsigned int rid, unsigned int base, unsigned int bytes) {
    // entries: one per 4 KiB page
    struct vgpu_mem_entry { unsigned long long addr; unsigned int len, pad; } *e;
    unsigned int npages = bytes / 4096;
    // build entries into a separate buffer, then copy into cmd_buf
    static unsigned char ents[768 * 16];
    e = (struct vgpu_mem_entry *)ents;
    for (unsigned int i = 0; i < npages; i++) {
        e[i].addr = (unsigned long long)(base + i * 4096);
        e[i].len = 4096;
        e[i].pad = 0;
    }
    struct { unsigned int rid, nr; } p;
    p.rid = rid; p.nr = npages;
    // assemble: header + p + entries
    struct vgpu_hdr *h = (struct vgpu_hdr *)cmd_buf;
    h->type = VGPU_CMD_RESOURCE_ATTACH_BACKING; h->flags = 0;
    h->fence_id = 0; h->ctx_id = 0; h->padding = 0;
    unsigned int off = sizeof(struct vgpu_hdr);
    memcpy(cmd_buf + off, &p, sizeof(p)); off += sizeof(p);
    memcpy(cmd_buf + off, ents, npages * 16); off += npages * 16;
    if (vgu_send(0, off) != 0) return -1;
    struct vgpu_hdr *rh = (struct vgpu_hdr *)resp_buf;
    return rh->type == VGPU_RESP_OK_NODATA ? 0 : -1;
}

static int vgu_scanout(unsigned int rid) {
    struct { unsigned int x, y, w, h; unsigned int scanout, resource; } p;
    p.x = 0; p.y = 0; p.w = FB_W; p.h = FB_H;
    p.scanout = 0; p.resource = rid;
    return vgu_cmd(VGPU_CMD_SET_SCANOUT, &p, sizeof(p));
}

static void vgu_flush(unsigned int rid) {
    struct { unsigned int x, y, w, h; unsigned int resource, pad; } p;
    p.x = 0; p.y = 0; p.w = FB_W; p.h = FB_H;
    p.resource = rid; p.pad = 0;
    vgu_cmd(VGPU_CMD_RESOURCE_FLUSH, &p, sizeof(p));
}

// QEMU's 2D path keeps the framebuffer in a host-side pixman image; the guest
// backing memory is only copied into it by a TRANSFER_TO_HOST_2D command, and
// set_scanout/flush then render from that host image. Without the transfer the
// scanout surface stays all-zero (black screen) even though guest RAM holds
// the rendered frame.
static int vgu_transfer(unsigned int rid) {
    struct {
        unsigned int x, y, w, h;
        unsigned long long offset;
        unsigned int resource, pad;
    } __attribute__((packed)) p;
    p.x = 0; p.y = 0; p.w = FB_W; p.h = FB_H;
    p.offset = 0; p.resource = rid; p.pad = 0;
    return vgu_cmd(VGPU_CMD_TRANSFER_TO_HOST_2D, &p, sizeof(p));
}

// ---- hardware cursor (cursorq, qidx 1) ----
#define VGPU_CMD_UPDATE_CURSOR 0x0300
#define VGPU_CMD_MOVE_CURSOR   0x0301
#define VGPU_CURSOR_SIZE 64

static int cursor_initialized;
static unsigned char cursor_pix[VGPU_CURSOR_SIZE * VGPU_CURSOR_SIZE * 4]
    __attribute__((aligned(16)));

static void vgu_cursor_init(void) {
    // resource 3: 64x64 B8G8R8X8, filled with a two-color arrow + transparent border
    if (vgu_create(3, VGPU_CURSOR_SIZE, VGPU_CURSOR_SIZE) != 0) return;
    // backing: pages of cursor_pix (static, identity-mapped)
    static struct { unsigned long long addr; unsigned int len, pad; } ents[4];
    for (unsigned int i = 0; i < 4; i++) {
        ents[i].addr = (unsigned long long)((unsigned int)cursor_pix + i * 4096);
        ents[i].len = 4096; ents[i].pad = 0;
    }
    // build attach cmd manually (like vgu_attach but for cursor_pix)
    struct vgpu_hdr *h = (struct vgpu_hdr *)cmd_buf;
    h->type = VGPU_CMD_RESOURCE_ATTACH_BACKING; h->flags = 0;
    h->fence_id = 0; h->ctx_id = 0; h->padding = 0;
    unsigned int rid_nr[2] = {3, 4};
    memcpy(cmd_buf + sizeof(struct vgpu_hdr), rid_nr, 8);
    memcpy(cmd_buf + sizeof(struct vgpu_hdr) + 8, ents, 4 * 16);
    if (vgu_send(0, sizeof(struct vgpu_hdr) + 8 + 4 * 16) == 0 &&
        ((struct vgpu_hdr *)resp_buf)->type == VGPU_RESP_OK_NODATA) {
        // fill cursor pixels (white body + accent border, transparent elsewhere)
        memset(cursor_pix, 0, sizeof(cursor_pix));
        for (int yy = 0; yy < VGPU_CURSOR_SIZE; yy++)
            for (int xx = 0; xx < VGPU_CURSOR_SIZE; xx++) {
                // simple arrow outline: draw later — placeholder white box core
                if (xx >= 8 && xx < 56 && yy >= 8 && yy < 56) {
                    unsigned int off = (yy * VGPU_CURSOR_SIZE + xx) * 4;
                    cursor_pix[off + 0] = 0xFF;   // B
                    cursor_pix[off + 1] = 0xFF;   // G
                    cursor_pix[off + 2] = 0xFF;   // R (white)
                    cursor_pix[off + 3] = 0xFF;   // X
                }
            }
        // host keeps a separate pixman image; copy our cursor pixels into it
        struct {
            unsigned int x, y, w, h;
            unsigned long long offset;
            unsigned int resource, pad;
        } __attribute__((packed)) p;
        p.x = 0; p.y = 0; p.w = VGPU_CURSOR_SIZE; p.h = VGPU_CURSOR_SIZE;
        p.offset = 0; p.resource = 3; p.pad = 0;
        if (vgu_cmd(VGPU_CMD_TRANSFER_TO_HOST_2D, &p, sizeof(p)) == 0)
            cursor_initialized = 1;
    }
}

void vgu_cursor(int x, int y, int visible) {
    if (!gpu_active) return;
    if (!cursor_initialized) vgu_cursor_init();
    if (!cursor_initialized) return;
    // cursor commands go on cursorq (qidx 1)
    // payload: {hdr, pos{scanout_id, x, y, padding}, resource_id, hot_x, hot_y, padding}
    struct { unsigned int scanout, x, y, pad; unsigned int resource, hot_x, hot_y, pad2; } c;
    c.scanout = 0; c.x = (unsigned int)x; c.y = (unsigned int)y; c.pad = 0;
    c.resource = visible ? 3 : 0;
    c.hot_x = 0; c.hot_y = 0; c.pad2 = 0;
    struct vgpu_hdr *h = (struct vgpu_hdr *)cmd_buf;
    h->type = VGPU_CMD_UPDATE_CURSOR; h->flags = 0;
    h->fence_id = 0; h->ctx_id = 0; h->padding = 0;
    memcpy(cmd_buf + sizeof(struct vgpu_hdr), &c, sizeof(c));
    vgu_send(1, sizeof(struct vgpu_hdr) + sizeof(c));
}

int vgu_init(void) {
    if (vm_probe(&vgpu, 0x1050) != 0) return -1;
    irq_install_handler(11, vgu_irq);
    if (vm_dev_init(&vgpu, VM_F_VERSION_1) != 0) return -1;
    if (vm_setup_queue(&vgpu, 0, 256) != 0) return -1;
    if (vm_setup_queue(&vgpu, 1, 64) != 0) return -1;
    vm_ready(&vgpu);
    if (vgu_create(1, FB_W, FB_H) != 0) return -1;
    if (vgu_create(2, FB_W, FB_H) != 0) return -1;
    if (vgu_attach(1, GPU_BASE, GPU_STRIDE) != 0) return -1;
    if (vgu_attach(2, GPU_BASE + GPU_STRIDE, GPU_STRIDE) != 0) return -1;
    if (vgu_transfer(1) != 0) return -1;
    if (vgu_scanout(1) != 0) return -1;
    vgu_flush(1);
    front = 0;                          // displayed = buffer 0 (rid 1)
    gpu_active = 1;
    serial_print("vgu: active\n");
    // selftest: one flip and back, logging each flip
    vgu_flip();                         // -> "vgu: flip ok"
    vgu_flip();
    // cursor selftest: show then hide, leaves cursor hidden
    vgu_cursor(FB_W / 2, FB_H / 2, 1);
    vgu_cursor(0, 0, 0);
    if (cursor_initialized)
        serial_print("vgu: cursor ok\n");
    return 0;
}

int vgu_active(void) { return gpu_active; }

unsigned int vgu_back(void) {
    if (!gpu_active) return 0;
    return GPU_BASE + (front ^ 1) * GPU_STRIDE;
}

void vgu_info(unsigned int *w, unsigned int *h, unsigned int *pitch) {
    if (gpu_active) { *w = FB_W; *h = FB_H; *pitch = FB_PITCH; }
    else { *w = 0; *h = 0; *pitch = 0; }
}

void vgu_flip(void) {
    if (!gpu_active) return;
    unsigned int rid = (front == 0) ? 2 : 1;
    if (vgu_transfer(rid) == 0 && vgu_scanout(rid) == 0) {
        front ^= 1;
        vgu_flush(rid);
        serial_print("vgu: flip ok\n");
    }
}