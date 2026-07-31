#include "paging.h"
#include "vga.h"
#include "serial.h"

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

    // Map the framebuffer physical range (may live above 256 MB).
    // The user bit lets the window manager (a ring-3 task) composite directly.
    unsigned int fb_addr = 0, fb_size = 0;
    vga_get_fb_info(&fb_addr, &fb_size);
    if (fb_size) {
        unsigned int end = fb_addr + fb_size;
        for (unsigned int a = fb_addr; a < end; a += 4096) {
            unsigned int pd = a >> 22;
            if (pd >= 1024) break;
            if (page_dir[pd] == 0) {
                if (extra_count >= 8) break;
                unsigned int *pt = extra_pt[extra_count++];
                for (int p = 0; p < 1024; p++)
                    pt[p] = (pd << 22) | (p << 12) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
                page_dir[pd] = (unsigned int)pt | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
            }
        }
    }

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
