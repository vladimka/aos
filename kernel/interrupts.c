#include "interrupts.h"
#include "idt.h"
#include "vga.h"
#include "serial.h"
#include "printf.h"
#include "ports.h"
#include "bt.h"
#include "user.h"
#include "task.h"

#define PIC1_CMD  0x20
#define PIC2_CMD  0xA0

static unsigned int spurious_count[16];

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

    // Copy-on-write: a user-mode write to a fork-shared read-only page is
    // resolved transparently and the faulting instruction re-run. Anything
    // else falls through to the panic below.
    if (r->int_no == 14) {
        if (task_handle_cow_fault(get_current_task(), cr2, r->err_code))
            return;
    }

    vga_set_color(VGA_WHITE, VGA_RED);
    printf("\n=== KERNEL PANIC ===\nException: %s (int %d)\nEIP: 0x%x  CS: 0x%x  EFLAGS: 0x%x  ERR: 0x%x  CR2: 0x%x  CR3: 0x%x\n",
           exception_messages[r->int_no], r->int_no, r->eip, r->cs, r->eflags, r->err_code, cr2, cr3v);
    printf("EAX: 0x%x  EBX: 0x%x  ECX: 0x%x  EDX: 0x%x\n", r->eax, r->ebx, r->ecx, r->edx);
    printf("ESI: 0x%x  EDI: 0x%x  EBP: 0x%x  ESP: 0x%x\n", r->esi, r->edi, r->ebp, r->esp);
    printf("USER_ESP: 0x%x  SS: 0x%x  DS: 0x%x  ES: 0x%x\n", r->user_esp, r->ss, r->ds, r->es);
    extern unsigned int saved_esp;
    printf("saved_esp=0x%x  program_active=%d  cur_pid=%d\n",
           saved_esp, user_program_active(), task_current_pid());
    printf("task0 kesp=0x%x  ktop=0x%x  state=%d\n",
           task_kernel_esp(0), task_kstack_top(0), task_state(0));
    printf("--- stack @ esp (0x%x, 24 words) ---\n", r->esp);
    unsigned int *dp = (unsigned int *)(r->esp & ~3u);
    for (int i = 0; i < 24; i++) {
        printf("  esp+%x: %x\n", (unsigned int)(dp + i), dp[i]);
    }
    // The syscall frame (syscall_common's pusha region) sits just above the
    // faulting C frame's EBP: [ebp+12..24] = gs..ds, [ebp+28..56] = edi..esp.
    printf("--- syscall frame @ ebp (0x%x) ---\n", r->ebp);
    unsigned int *fp = (unsigned int *)(r->ebp & ~3u);
    for (int i = 0; i < 20; i++) {
        printf("  ebp+%x: %x\n", (unsigned int)(fp + i), fp[i]);
    }
    backtrace((unsigned int *)r->ebp, 16);
    for (;;)
        __asm__ volatile("cli; hlt");
}

static unsigned char pic_get_isr(unsigned char cmd_port) {
    outb(cmd_port, 0x0B);
    return inb(cmd_port);
}

static void irq_eoi(int irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, 0x20);
        outb(PIC1_CMD, 0x20);
    } else {
        outb(PIC1_CMD, 0x20);
    }
}

void irq_handler(struct registers *r) {
    int irq = r->int_no - 32;

    unsigned char cmd = (irq >= 8) ? PIC2_CMD : PIC1_CMD;
    unsigned char isr = pic_get_isr(cmd);
    if (!(isr & (1 << (irq & 7)))) {
        // Spurious IRQ (e.g. the slave PIC's spurious vector IRQ15 after a
        // PS/2 mouse byte is drained before the PIC latches it on real
        // hardware). Benign — just EOI and drop it; printing here would
        // clobber the framebuffer the window manager is drawing to.
        spurious_count[irq]++;
        irq_eoi(irq);
        return;
    }

    // Acknowledge the PIC before running the handler: the timer handler may
    // switch to ring 3 (serial newline -> command) and iret away before its
    // own EOI, which would leave IRQ0 in-service and block lower-priority
    // IRQs (keyboard IRQ1) for the whole user program lifetime.
    irq_eoi(irq);

    if (irq_routines[irq]) {
        irq_routines[irq]();
    } else {
        printf("unexpected IRQ %d (no handler)\n", irq);
        spurious_count[irq]++;
    }
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
