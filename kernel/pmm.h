#ifndef PMM_H
#define PMM_H

#define PAGE_SIZE 4096

// Bit in struct pframe.flags: this frame backs a kmalloc slab page.
#define PF_SLAB 0x1

// Per-frame metadata (mem_map). order is the buddy order for an allocated
// large block (0 for slab pages); slab pages also record their size class.
struct pframe {
    unsigned char order;
    unsigned char flags;
    unsigned char slab_class;
} __attribute__((packed));

#define PM_NR_MAX 65536

extern struct pframe pmm_frames[PM_NR_MAX];

static inline unsigned int pmm_frame_of(unsigned int phys) {
    return phys >> 12;
}

void pmm_init(unsigned int mb_info_addr);
void pmm_selftest(void);

void *page_alloc(void);
void *page_alloc_zero(void);
void *page_alloc_order(unsigned int order);
void page_free(void *addr);
void page_free_order(void *addr, unsigned int order);

unsigned int pmm_total_pages(void);
unsigned int pmm_free_pages(void);

#endif
