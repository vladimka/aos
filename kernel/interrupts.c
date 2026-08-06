#include "interrupts.h"
#include "idt.h"
#include "vga.h"
#include "serial.h"
#include "printf.h"
#include "ports.h"

extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);

extern void isr128(void);

static void (*irq_routines[16])(void) = { 0 };

void irq_install_handler(int irq, void (*handler)(void)) {
    irq_routines[irq] = handler;
}

void irq_uninstall_handler(int irq) {
    irq_routines[irq] = 0;
}

void irq_remap(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0x00);
    outb(0xA1, 0x00);
}

char *exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",
    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};

void isr_handler(struct registers *r) {
    unsigned int cr2, cr3v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3v));
    vga_set_color(VGA_WHITE, VGA_RED);
    printf("\n=== KERNEL PANIC ===\nException: %s\nEIP: 0x%x  CS: 0x%x  EFLAGS: 0x%x  ERR: 0x%x  CR2: 0x%x  CR3: 0x%x\nSystem halted.\n",
           exception_messages[r->int_no], r->eip, r->cs, r->eflags, r->err_code, cr2, cr3v);
    serial_print("kstack scan:\n");
    unsigned int *sp = (unsigned int *)&r;
    for (int i = 0; i < 64 && (unsigned int)sp < 0x3000000; i++, sp++) {
        unsigned int v = *sp;
        if (v >= 0x100000 && v <= 0x110000) {
            serial_print("  [kstack] word=");
            serial_print_hex(v);
            serial_print("\n");
        }
    }
    for (;;)
        __asm__ volatile("cli; hlt");
}

void irq_handler(struct registers *r) {
    int irq = r->int_no - 32;

    // Acknowledge the PIC before running the handler: the timer handler may
    // switch to ring 3 (serial newline -> command) and iret away before its
    // own EOI, which would leave IRQ0 in-service and block lower-priority
    // IRQs (keyboard IRQ1) for the whole user program lifetime.
    if (irq >= 8)
        outb(0xA0, 0x20);
    outb(0x20, 0x20);

    if (irq_routines[irq])
        irq_routines[irq]();
}

void interrupts_init(void) {
    irq_remap();

    idt_install_irq(0,  isr0);
    idt_install_irq(1,  isr1);
    idt_install_irq(2,  isr2);
    idt_install_irq(3,  isr3);
    idt_install_irq(4,  isr4);
    idt_install_irq(5,  isr5);
    idt_install_irq(6,  isr6);
    idt_install_irq(7,  isr7);
    idt_install_irq(8,  isr8);
    idt_install_irq(9,  isr9);
    idt_install_irq(10, isr10);
    idt_install_irq(11, isr11);
    idt_install_irq(12, isr12);
    idt_install_irq(13, isr13);
    idt_install_irq(14, isr14);
    idt_install_irq(15, isr15);
    idt_install_irq(16, isr16);
    idt_install_irq(17, isr17);
    idt_install_irq(18, isr18);
    idt_install_irq(19, isr19);
    idt_install_irq(20, isr20);
    idt_install_irq(21, isr21);
    idt_install_irq(22, isr22);
    idt_install_irq(23, isr23);
    idt_install_irq(24, isr24);
    idt_install_irq(25, isr25);
    idt_install_irq(26, isr26);
    idt_install_irq(27, isr27);
    idt_install_irq(28, isr28);
    idt_install_irq(29, isr29);
    idt_install_irq(30, isr30);
    idt_install_irq(31, isr31);

    idt_install_irq(32, irq0);
    idt_install_irq(33, irq1);
    idt_install_irq(34, irq2);
    idt_install_irq(35, irq3);
    idt_install_irq(36, irq4);
    idt_install_irq(37, irq5);
    idt_install_irq(38, irq6);
    idt_install_irq(39, irq7);
    idt_install_irq(40, irq8);
    idt_install_irq(41, irq9);
    idt_install_irq(42, irq10);
    idt_install_irq(43, irq11);
    idt_install_irq(44, irq12);
    idt_install_irq(45, irq13);
    idt_install_irq(46, irq14);
    idt_install_irq(47, irq15);

    idt_install_irq_flags(0x80, isr128, 0xEE);

    __asm__ volatile("sti");

    printf("Interrupts initialized.\n");
}
