#include "idt.h"

struct idt_entry {
    unsigned short base_low;
    unsigned short sel;
    unsigned char  always0;
    unsigned char  flags;
    unsigned short base_high;
} __attribute__((packed));

struct idt_ptr {
    unsigned short limit;
    unsigned int   base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

void idt_install_irq(unsigned char irq, void (*handler)(void)) {
    unsigned int base = (unsigned int)handler;
    idt[irq].base_low  = base & 0xFFFF;
    idt[irq].base_high = (base >> 16) & 0xFFFF;
    idt[irq].sel       = 0x08;
    idt[irq].always0   = 0;
    idt[irq].flags     = 0x8E;
}

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (unsigned int)&idt;

    __asm__ volatile("lidt %0" : : "m"(idtp));
}
