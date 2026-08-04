#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "vga.h"

#define MAX_ORDER 16

struct pframe pmm_frames[PM_NR_MAX];

static void *free_lists[MAX_ORDER + 1];
static unsigned int total_pages = 0;
static unsigned int free_pages = 0;

struct region { unsigned int start; unsigned int end; };
static struct region reserved[16];
static unsigned int nreserved = 0;

static void reserve(unsigned int start, unsigned int end) {
    if (nreserved < 16 && end > start) {
        reserved[nreserved].start = start;
        reserved[nreserved].end = end;
        nreserved++;
    }
}

static int is_reserved(unsigned int addr, unsigned int len) {
    for (unsigned int i = 0; i < nreserved; i++)
        if (addr < reserved[i].end && addr + len > reserved[i].start)
            return 1;
    return 0;
}

// Largest order whose block fits inside [base,end) AND is aligned at base.
static unsigned int max_order_for(unsigned int base, unsigned int end) {
    unsigned int o = 0;
    while (o < MAX_ORDER) {
        unsigned int block = PAGE_SIZE << (o + 1);
        if ((base & (block - 1)) != 0) break;
        if (base + block > end) break;
        o++;
    }
    return o;
}

static void push_block(unsigned int base, unsigned int order) {
    *(unsigned int *)base = (unsigned int)free_lists[order];
    free_lists[order] = (void *)base;
    free_pages += 1u << order;
}

// Returns 0 when the free list is empty (physical address 0 is never handed
// out: the first MB is reserved).
static unsigned int pop_block(unsigned int order) {
    if (!free_lists[order]) return 0;
    unsigned int b = (unsigned int)free_lists[order];
    free_lists[order] = (void *)*(unsigned int *)b;
    free_pages -= 1u << order;
    return b;
}

static void buddy_free(unsigned int base, unsigned int order) {
    while (order < MAX_ORDER) {
        unsigned int buddy = base ^ (PAGE_SIZE << order);
        // Is the buddy free (i.e. present in this order's list)?
        unsigned int *prev = (unsigned int *)&free_lists[order];
        unsigned int *cur = free_lists[order];
        int found = 0;
        while (cur) {
            if ((unsigned int)cur == buddy) { found = 1; break; }
            prev = cur;
            cur = (unsigned int *)*cur;
        }
        if (!found) break;
        *prev = (unsigned int)*cur;   // unlink the buddy
        free_pages -= 1u << order;
        if (buddy < base) base = buddy;
        order++;
    }
    push_block(base, order);
}

// Decompose [base,end) into maximal aligned power-of-two blocks and free each.
static void buddy_free_range(unsigned int base, unsigned int end) {
    while (base < end) {
        unsigned int o = max_order_for(base, end);
        push_block(base, o);
        base += PAGE_SIZE << o;
    }
}

// Add an available physical range, cutting out any reserved regions.
// Round to whole pages so no free block is ever inserted at an unaligned
// address (an unaligned base would mis-index pmm_frames[] when returned).
static void add_available(unsigned int base, unsigned int end) {
    base = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    end = end & ~(PAGE_SIZE - 1);
    if (base >= end) return;
    for (unsigned int i = 0; i < nreserved; i++) {
        if (reserved[i].start >= end || reserved[i].end <= base) continue;
        if (reserved[i].start > base)
            add_available(base, reserved[i].start);
        base = reserved[i].end;
        if (base >= end) return;
    }
    buddy_free_range(base, end);
}

void *page_alloc_order(unsigned int order) {
    if (order > MAX_ORDER) return 0;
    unsigned int b = pop_block(order);
    if (b) {
        pmm_frames[b >> 12].order = (unsigned char)order;
        return (void *)b;
    }
    unsigned int o = order + 1;
    while (o <= MAX_ORDER && !free_lists[o]) o++;
    if (o > MAX_ORDER) return 0;
    unsigned int big = pop_block(o);
    while (o > order) {
        o--;
        push_block(big + (PAGE_SIZE << o), o);
    }
    pmm_frames[big >> 12].order = (unsigned char)order;
    return (void *)big;
}

void *page_alloc(void) {
    return page_alloc_order(0);
}

void *page_alloc_zero(void) {
    void *p = page_alloc_order(0);
    if (p) memset(p, 0, PAGE_SIZE);
    return p;
}

void page_free_order(void *addr, unsigned int order) {
    unsigned int base = (unsigned int)addr;
    if (!base || order > MAX_ORDER) return;
    pmm_frames[base >> 12].order = 0;
    buddy_free(base, order);
}

void page_free(void *addr) {
    page_free_order(addr, 0);
}

unsigned int pmm_total_pages(void) { return total_pages; }
unsigned int pmm_free_pages(void) { return free_pages; }

static void parse_memmap(unsigned char *mbi, unsigned int *starts,
                         unsigned int *ends, int *navail) {
    unsigned int total = *(unsigned int *)mbi;
    unsigned char *tag = mbi + 8;
    while ((unsigned int)(tag - mbi) < total) {
        unsigned int type = *(unsigned int *)tag;
        unsigned int size = *(unsigned int *)(tag + 4);
        if (type == 0) break;
        if (type == 6) {
            unsigned int es = *(unsigned int *)(tag + 8);   // entry_size
            unsigned char *e = tag + 16;                    // after entry_version
            unsigned char *tend = tag + size;
            while (e + es <= tend) {
                unsigned long long base = *(unsigned long long *)e;
                unsigned long long len = *(unsigned long long *)(e + 8);
                unsigned int etype = *(unsigned int *)(e + 16);
                if (etype == 1 && base < 256u * 1024 * 1024) {
                    unsigned long long b = base;
                    unsigned long long eend = base + len;
                    if (eend > 256u * 1024 * 1024) eend = 256u * 1024 * 1024;
                    if (eend > b && *navail < 16) {
                        starts[*navail] = (unsigned int)b;
                        ends[*navail] = (unsigned int)eend;
                        (*navail)++;
                    }
                }
                e += es;
            }
            return;
        }
        tag += (size + 7) & ~7;
    }
}

// Fallback: MB2 basic-meminfo tag (type 4) — mem_upper in KB above 1 MB.
static int parse_mem_upper(unsigned char *mbi, unsigned int *start, unsigned int *end) {
    unsigned int total = *(unsigned int *)mbi;
    unsigned char *tag = mbi + 8;
    while ((unsigned int)(tag - mbi) < total) {
        unsigned int type = *(unsigned int *)tag;
        unsigned int size = *(unsigned int *)(tag + 4);
        if (type == 0) break;
        if (type == 4) {
            unsigned int upper = *(unsigned int *)(tag + 12);
            if (!upper) return 0;
            *start = 0x100000;
            *end = 0x100000 + upper * 1024u;
            if (*end > 256u * 1024 * 1024) *end = 256u * 1024 * 1024;
            return 1;
        }
        tag += (size + 7) & ~7;
    }
    return 0;
}

void pmm_init(unsigned int mb_info_addr) {
    memset(pmm_frames, 0, sizeof(pmm_frames));
    memset(free_lists, 0, sizeof(free_lists));
    nreserved = 0;

    extern unsigned int _start, _end;

    reserve(0x00000000, 0x00100000);                     // low MB (BIOS/multiboot)
    reserve(0x00100000, (unsigned int)&_end);            // kernel image
    reserve(0x00200000, 0x00200000 + 1024 * 1024);        // ramdisk (kernel/sfs.c)
    reserve(0x01000000, 0x01C00000);                     // task-0 user area
    reserve(0x03000000, 0x04000000);                     // shared slab window
    reserve(0x08000000, 0x08800000);                     // task-0 Linux window
    unsigned int fb_addr = 0, fb_size = 0;
    vga_get_fb_info(&fb_addr, &fb_size);
    if (fb_size && fb_addr < 256u * 1024 * 1024) {
        unsigned int fbe = fb_addr + fb_size;
        if (fbe > 256u * 1024 * 1024) fbe = 256u * 1024 * 1024;
        fb_addr = (fb_addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        fbe = fbe & ~(PAGE_SIZE - 1);
        if (fbe > fb_addr) reserve(fb_addr, fbe);
    }

    unsigned char *mbi = (unsigned char *)mb_info_addr;
    unsigned int starts[16], ends[16];
    int navail = 0;
    if (mbi) {
        parse_memmap(mbi, starts, ends, &navail);
        if (!navail) {
            unsigned int s = 0, e = 0;
            if (parse_mem_upper(mbi, &s, &e)) {
                starts[0] = s; ends[0] = e; navail = 1;
            }
        }
    }
    for (int i = 0; i < navail; i++)
        add_available(starts[i], ends[i]);

    total_pages = 0;
    for (unsigned int i = 0; i < navail; i++)
        total_pages += (ends[i] - starts[i]) >> 12;
}

void pmm_selftest(void) {
    unsigned int pages[256];
    int n = 0, bad = 0;
    for (n = 0; n < 256; n++) {
        void *p = page_alloc();
        if (!p) break;
        pages[n] = (unsigned int)p;
        if (is_reserved((unsigned int)p, PAGE_SIZE)) bad = 1;
        if (bad) break;
    }
    for (int i = 0; i < n; i++)
        page_free((void *)pages[i]);
    printf("PMM: %u total, %u free pages, selftest %s\n",
           pmm_total_pages(), pmm_free_pages(), bad ? "FAIL" : "OK");
}
