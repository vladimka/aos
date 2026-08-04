#include "kmm.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"

#define KMM_CLASSES 9

static const unsigned int class_size[KMM_CLASSES] =
    { 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 };

static void *slab_free[KMM_CLASSES];

static void irq_save(unsigned int *flags) {
    unsigned int f;
    __asm__ volatile("pushfl; pop %0" : "=r"(f));
    __asm__ volatile("cli");
    *flags = f;
}

static void irq_restore(unsigned int flags) {
    if (flags & 0x200)
        __asm__ volatile("sti");
}

static int class_index(unsigned int size) {
    for (int i = 0; i < KMM_CLASSES; i++)
        if (size <= class_size[i]) return i;
    return -1;
}

// Split a fresh page into same-size objects linked through their first word.
static void *slab_carve(unsigned int base, unsigned int stride) {
    void *head = 0;
    for (unsigned int off = 0; off + stride <= PAGE_SIZE; off += stride) {
        unsigned int obj = base + off;
        *(unsigned int *)obj = (unsigned int)head;
        head = (void *)obj;
    }
    return head;
}

void kmm_init(void) {
    memset(slab_free, 0, sizeof(slab_free));
}

void *kmalloc(unsigned int size) {
    unsigned int flags;
    irq_save(&flags);
    void *p = 0;
    int idx = class_index(size ? size : 16);
    if (idx >= 0) {
        if (!slab_free[idx]) {
            void *page = page_alloc_zero();
            if (!page) goto out;
            unsigned int fr = pmm_frame_of((unsigned int)page);
            pmm_frames[fr].flags |= PF_SLAB;
            pmm_frames[fr].slab_class = (unsigned char)idx;
            slab_free[idx] = slab_carve((unsigned int)page, class_size[idx]);
        }
        p = slab_free[idx];
        slab_free[idx] = (void *)*(unsigned int *)p;
    } else {
        unsigned int need = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        unsigned int order = 0;
        while ((1u << order) < need) order++;
        p = page_alloc_order(order);
    }
out:
    irq_restore(flags);
    return p;
}

void *kcalloc(unsigned int n, unsigned int sz) {
    unsigned int total = n * sz;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void kfree(void *ptr) {
    if (!ptr) return;
    unsigned int flags;
    irq_save(&flags);
    unsigned int fr = pmm_frame_of((unsigned int)ptr);
    if (pmm_frames[fr].flags & PF_SLAB) {
        int idx = pmm_frames[fr].slab_class;
        *(unsigned int *)ptr = (unsigned int)slab_free[idx];
        slab_free[idx] = ptr;
    } else {
        page_free_order(ptr, pmm_frames[fr].order);
    }
    irq_restore(flags);
}

void kmm_selftest(void) {
    static const unsigned int sizes[] = { 10, 64, 100, 1000, 4096, 8192, 20000, 65536 };
    void *p[8];
    int ok = 1;
    int i;
    for (i = 0; i < 8; i++) {
        p[i] = kmalloc(sizes[i]);
        if (!p[i]) { ok = 0; break; }
        memset(p[i], 0xA5, sizes[i]);
        unsigned char *b = p[i];
        for (unsigned int j = 0; j < sizes[i]; j++)
            if (b[j] != 0xA5) { ok = 0; break; }
        if (!ok) break;
    }
    if (ok)
        for (int j = 0; j < i; j++) kfree(p[j]);
    void *q = ok ? kmalloc(64) : 0;
    if (!q) ok = 0;
    if (ok) kfree(q);
    printf("KMM: selftest %s\n", ok ? "OK" : "FAIL");
}
