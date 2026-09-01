#include "paging.h"
#include "vga.h"
#include "serial.h"
#include "pmm.h"

#define PTE_PRESENT  0x1
#define PTE_WRITABLE 0x2
#define PTE_USER     0x4

// 64 page tables = identity map of 0..256 MB (kernel, ramdisk, user area)
static unsigned int page_dir[1024] __attribute__((aligned(4096)));
static unsigned int page_tables[64][1024] __attribute__((aligned(4096)));
static unsigned int extra_pt[8][1024] __attribute__((aligned(4096)));
static int extra_count = 0;

#define USER_PD_LO 4
#define USER_PD_HI 6

// Shared-memory window slabs live here (identity-mapped), see aosipc.h
#define SLAB_PD_LO 12
#define SLAB_PD_HI 15

void paging_init(void) {
    for (int i = 0; i < 1024; i++)
        page_dir[i] = 0;

    for (int t = 0; t < 64; t++) {
        page_dir[t] = (unsigned int)page_tables[t] | PTE_PRESENT | PTE_WRITABLE;
        for (int p = 0; p < 1024; p++)
            page_tables[t][p] = (t << 22) | (p << 12) | PTE_PRESENT | PTE_WRITABLE;
    }

    // User-accessible pages: 0x01000000 .. 0x01C00000 (programs, heap, stack)
    for (int t = USER_PD_LO; t <= USER_PD_HI; t++) {
        page_dir[t] |= PTE_USER;
        for (int p = 0; p < 1024; p++)
            page_tables[t][p] |= PTE_USER;
    }

    // Shared-memory window slabs: 0x03000000 .. 0x04000000, user-accessible.
    // Every task's address space clones these PDEs, so all apps see the same
    // physical window buffers at the same virtual addresses.
    for (int t = SLAB_PD_LO; t <= SLAB_PD_HI; t++) {
        page_dir[t] |= PTE_USER;
        for (int p = 0; p < 1024; p++)
            page_tables[t][p] |= PTE_USER;
    }

    // VirtIO-GPU double-buffer window: 0x04000000 .. 0x04600000, user-accessible
    // (the window manager, a ring-3 task, composites into the back buffer).
    for (int t = 16; t <= 17; t++) {
        page_dir[t] |= PTE_USER;
        for (int p = 0; p < 1024; p++)
            page_tables[t][p] |= PTE_USER;
    }

    // Task-0 Linux window: 0x08000000 .. 0x08800000 identity-mapped, user-accessible.
    for (int t = 32; t <= 33; t++) {
        page_dir[t] |= PTE_USER;
        for (int p = 0; p < 1024; p++)
            page_tables[t][p] |= PTE_USER;
    }

    // Map the framebuffer physical range (may live above 256 MB).
    // The user bit lets the window manager (a ring-3 task) composite directly.
    unsigned int fb_addr = 0, fb_size = 0;
    vga_get_fb_info(&fb_addr, &fb_size);
    // The VBE-pan console keeps off-screen headroom lines above the scanout
    // in VRAM, so the identity map must cover more than the visible screen.
    fb_size = vga_get_fb_map_size();
    if (fb_size)
        paging_identity_map(fb_addr, fb_size);

    serial_print("Paging enabled (identity map, ring3 user area).\n");

    __asm__ volatile("movl %0, %%cr3" :: "r"((unsigned int)page_dir));
    __asm__ volatile(
        "movl %%cr0, %%eax\n"
        "orl $0x80000000, %%eax\n"
        "movl %%eax, %%cr0\n"
        ::: "eax", "memory");
    __asm__ volatile("jmp 1f\n1:");
}

unsigned int *paging_kernel_pd(void) {
    return page_dir;
}

unsigned int paging_get_cr3(void) {
    unsigned int cr3;
    __asm__ volatile("movl %%cr3, %0" : "=r"(cr3));
    return cr3;
}

void paging_set_cr3(unsigned int cr3) {
    __asm__ volatile("movl %0, %%cr3" :: "r"(cr3) : "memory");
}

// Identity-map a physical range that lives outside the pre-built 0..256 MB
// map (framebuffer, AHCI ABAR, ...). Pages in the existing map are left
// untouched. Safe to call before or after CR3 is loaded (callers reload CR3).
int paging_identity_map(unsigned int phys, unsigned int bytes) {
    unsigned int end = phys + bytes;
    for (unsigned int a = phys; a < end; a += 4096) {
        unsigned int pd = a >> 22;
        if (pd >= 1024) return -1;
        if (page_dir[pd] == 0) {
            if (extra_count >= 8) return -1;
            unsigned int *pt = extra_pt[extra_count++];
            for (int p = 0; p < 1024; p++)
                pt[p] = (pd << 22) | (p << 12) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
            page_dir[pd] = (unsigned int)pt | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }
    }
    return 0;
}

int paging_map_user_page(unsigned int vaddr) {
    unsigned int pdi = vaddr >> 22;
    unsigned int pti = (vaddr >> 12) & 0x3FF;
    unsigned int *pd = (unsigned int *)paging_get_cr3();
    if (pdi >= 1024) return -1;
    if (!(pd[pdi] & PTE_PRESENT)) return -1;
    unsigned int *pt = (unsigned int *)(pd[pdi] & 0xFFFFF000);
    if (pt[pti] & PTE_PRESENT) return 0;
    unsigned int frame = (unsigned int)page_alloc_zero();
    if (!frame) return -1;
    pt[pti] = frame | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    return 0;
}

// Mark the PWT bit (bit 3) on every present PTE whose identity-mapped frame
// falls inside [phys, phys+size). Used with PAT index 1 = WC to make the
// framebuffer write-combining without MTRRs. Callers reload CR3 afterwards.
void paging_mark_pwt(unsigned int phys, unsigned int size) {
    if (size == 0) return;
    unsigned int end = phys + size;
    unsigned int pdl = phys >> 22;
    unsigned int pdr = (end - 1) >> 22;
    for (unsigned int pd = pdl; pd <= pdr && pd < 1024; pd++) {
        unsigned int pde = page_dir[pd];
        if (!(pde & PTE_PRESENT)) continue;
        unsigned int *pt = (unsigned int *)(pde & 0xFFFFF000);
        for (unsigned int p = 0; p < 1024; p++) {
            unsigned int frame = (pd << 22) | (p << 12);
            if (frame >= phys && frame < end && (pt[p] & PTE_PRESENT))
                pt[p] |= 0x8;   /* PWT */
        }
    }
}
