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

// 0: null, 1: kernel code, 2: kernel data, 3: user code, 4: user data, 5: TSS,
// 6: TLS descriptor (current Linux task's thread-local storage, GDT_ENTRY_TLS_MIN)
static struct gdt_entry gdt[7];
static struct gdt_ptr   gp;
static struct tss       tss_entry;

#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE 0x18
#define SEL_UDATA 0x20
#define SEL_TSS   0x28
#define TLS_ENTRY 6                  // GDT index of the TLS descriptor
#define TLS_SEL   0x33               // TLS_ENTRY<<3 | 3, RPL 3 (matches musl)

static void seg_set(struct gdt_entry *e, unsigned int base, unsigned int limit,
                    unsigned char access, unsigned char gran) {
    e->base_low     = base & 0xFFFF;
    e->base_middle  = (base >> 16) & 0xFF;
    e->base_high    = (base >> 24) & 0xFF;
    e->limit_low    = limit & 0xFFFF;
    e->granularity  = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    e->access       = access;
}

static void gdt_set_gate(int idx, unsigned int base, unsigned int limit,
                         unsigned char access, unsigned char gran) {
    seg_set(&gdt[idx], base, limit, access, gran);
}

void tss_set_esp0(unsigned int esp0) {
    tss_entry.esp0 = esp0;
    tss_entry.ss0  = SEL_KDATA;
}

void ldt_set_tls(unsigned int base, unsigned int limit,
                 unsigned int seg_32bit, unsigned int read_exec_only,
                 unsigned int limit_in_pages) {
    if (limit_in_pages)
        limit >>= 12;
    unsigned char access = read_exec_only ? 0xF0 : 0xF2;
    unsigned char gran = (seg_32bit ? 0x40 : 0x00) |
                         (limit_in_pages ? 0x80 : 0x00);
    seg_set(&gdt[TLS_ENTRY], base, limit, access, gran);
}

void tls_reload_gs(void) {
    __asm__ volatile("mov %0, %%gs" : : "r"((unsigned short)TLS_SEL) : "memory");
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
    // TLS slot starts empty (base 0, 4 GB, user data); set per-Linux-task.
    gdt_set_gate(TLS_ENTRY, 0, 0xFFFFF, 0xF2, 0xCF);

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
