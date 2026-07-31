#include "gdt.h"

struct gdt_entry {
    unsigned short limit_low;
    unsigned short base_low;
    unsigned char  base_middle;
    unsigned char  access;
    unsigned char  granularity;
    unsigned char  base_high;
} __attribute__((packed));

struct gdt_ptr {
    unsigned short limit;
    unsigned int   base;
} __attribute__((packed));

struct tss {
    unsigned int link;
    unsigned int esp0;
    unsigned int ss0;
    unsigned int esp1;
    unsigned int ss1;
    unsigned int esp2;
    unsigned int ss2;
    unsigned int cr3;
    unsigned int eip;
    unsigned int eflags;
    unsigned int eax, ecx, edx, ebx;
    unsigned int esp, ebp, esi, edi;
    unsigned int es, cs, ss, ds, fs, gs;
    unsigned int ldt;
    unsigned short trap;
    unsigned short iomap;
} __attribute__((packed));

// 0: null, 1: kernel code, 2: kernel data, 3: user code, 4: user data, 5: TSS
static struct gdt_entry gdt[6];
static struct gdt_ptr   gp;
static struct tss       tss_entry;

#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE 0x18
#define SEL_UDATA 0x20
#define SEL_TSS   0x28

static void gdt_set_gate(int idx, unsigned int base, unsigned int limit,
                         unsigned char access, unsigned char gran) {
    gdt[idx].base_low    = base & 0xFFFF;
    gdt[idx].base_middle = (base >> 16) & 0xFF;
    gdt[idx].base_high   = (base >> 24) & 0xFF;
    gdt[idx].limit_low   = limit & 0xFFFF;
    gdt[idx].granularity = (limit >> 16) & 0x0F;
    gdt[idx].granularity |= gran & 0xF0;
    gdt[idx].access      = access;
}

void tss_set_esp0(unsigned int esp0) {
    tss_entry.esp0 = esp0;
    tss_entry.ss0  = SEL_KDATA;
}

void gdt_init(void) {
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (unsigned int)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    for (unsigned int i = 0; i < sizeof(struct tss) / 4; i++)
        ((unsigned int *)&tss_entry)[i] = 0;
    gdt_set_gate(5, (unsigned int)&tss_entry, sizeof(struct tss) - 1, 0x89, 0x00);

    __asm__ volatile("lgdt %0" : : "m"(gp));
    __asm__ volatile(
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        "ljmp $0x08, $1f\n"
        "1:\n"
        :
        :
        : "ax", "memory"
    );

    __asm__ volatile("ltr %0" : : "r"((unsigned short)SEL_TSS));
}
