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

// Diagnostic ranges for the strace kernel-panic investigation.
// linux_syscall_handler's exit case is at +0x274, the task0 exit path at
// +0x1115 (stable offsets; the function base itself shifts between builds).
#define LINUX_EXIT_RET_OFF   0x270u     // exit case: mov 0x20(%edi),%esi; call task_current_pid
#define LINUX_EXIT_PATH_OFF  0x10f0u    // task0 exit path: sub $0xc,%esp .. call user_program_exit
#define SLAB_LO             0x03000000u
#define SLAB_HI             0x04000000u
#define TEXT_LO             0x00100000u
#define TEXT_HI             0x00115500u  // end of .text (readelf -S)

extern char linux_syscall_handler[];

static void serial_byte(unsigned char b) {
    static const char *hex = "0123456789ABCDEF";
    serial_putchar(hex[b >> 4]);
    serial_putchar(hex[b & 0xF]);
}

static void serial_dump_range(unsigned int addr, unsigned int len) {
    for (unsigned int i = 0; i < len; i += 16) {
        serial_print("  @");
        serial_print_hex(addr + i);
        serial_print(": ");
        for (unsigned int j = 0; j < 16; j++)
            serial_byte(*(unsigned char *)(addr + i + j));
        serial_print("\n");
    }
}

// Locate dispatch jump tables in .text by the instruction pattern
// `ff 24 b5 disp32` = jmp *disp32(,%esi,4), then dump the first entries.
static void dump_dispatch_tables(void) {
    serial_print("dispatch tables (jmp *disp32(,%esi,4)):\n");
    for (unsigned int p = TEXT_LO; p + 7 < TEXT_HI; p++) {
        if (*(unsigned char *)(p + 0) == 0xFF &&
            *(unsigned char *)(p + 1) == 0x24 &&
            *(unsigned char *)(p + 2) == 0xB5) {
            unsigned int tbl = *(unsigned int *)(p + 3);
            serial_print("  jmp instr @0x");
            serial_print_hex(p);
            serial_print(" table=0x");
            serial_print_hex(tbl);
            serial_print("\n");
            serial_dump_range(tbl, 32);
            serial_print("  table[252]: ");
            serial_dump_range(tbl + 252 * 4, 8);
        }
    }
}

static void panic_diagnostics(struct registers *r) {
    serial_print("=== diagnostics ===\n");
    if (r->eip >= SLAB_LO && r->eip < SLAB_HI) {
        serial_print("EIP in slab window; page around EIP:\n");
        unsigned int pg = r->eip & ~0xFFFu;
        serial_dump_range(pg, 256);
        if (r->eip >= 0x100000)
            serial_dump_range(r->eip - 64, 128);
    }
    serial_print("linux exit case .text @linux_syscall_handler+0x270:\n");
    serial_dump_range((unsigned int)linux_syscall_handler + LINUX_EXIT_RET_OFF, 64);
    serial_print("linux exit path .text @linux_syscall_handler+0x10f0:\n");
    serial_dump_range((unsigned int)linux_syscall_handler + LINUX_EXIT_PATH_OFF, 80);
    dump_dispatch_tables();
    {
        extern struct task *get_current_task(void);
        struct task *t = get_current_task();
        serial_print("current_task ptr=0x");
        serial_print_hex((unsigned int)t);
        if (t) {
            serial_print(" pid=");
            serial_print_hex(t->pid);
            serial_print(" state=");
            serial_print_hex(t->state);
            serial_print(" abi=");
            serial_print_hex(t->abi);
        }
        serial_print("\n");
    }
    {
        extern struct task *task_slot(unsigned int i);
        serial_print("task table:\n");
        for (unsigned int i = 0; i < MAX_TASKS; i++) {
            struct task *s = task_slot(i);
            if (!s) break;
            serial_print("  [");
            serial_print_hex(i);
            serial_print("] pid=");
            serial_print_hex(s->pid);
            serial_print(" state=");
            serial_print_hex(s->state);
            serial_print(" abi=");
            serial_print_hex(s->abi);
            serial_print(" kesp=");
            serial_print_hex(s->kernel_esp);
            serial_print(" kstack=");
            serial_print_hex((unsigned int)s->kstack);
            serial_print(" top=");
            serial_print_hex(s->kstack_top);
            serial_print(" cr3=");
            serial_print_hex(s->cr3);
            serial_print(" parent=");
            serial_print_hex(s->parent);
            serial_print(" sink=");
            serial_print_hex(s->sink);
            serial_print("\n");
        }
    }
    serial_print("=== end diagnostics ===\n");
}

static unsigned int spurious_count[16];
static unsigned int spurious_log_suppressed[16];

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
    printf("\n=== KERNEL PANIC ===\nException: %s (int %d)\nEIP: 0x%x  CS: 0x%x  EFLAGS: 0x%x  ERR: 0x%x  CR2: 0x%x  CR3: 0x%x\n",
           exception_messages[r->int_no], r->int_no, r->eip, r->cs, r->eflags, r->err_code, cr2, cr3v);
    printf("EAX: 0x%x  EBX: 0x%x  ECX: 0x%x  EDX: 0x%x\n", r->eax, r->ebx, r->ecx, r->edx);
    printf("ESI: 0x%x  EDI: 0x%x  EBP: 0x%x  ESP: 0x%x\n", r->esi, r->edi, r->ebp, r->esp);
    printf("USER_ESP: 0x%x  SS: 0x%x  DS: 0x%x  ES: 0x%x\n", r->user_esp, r->ss, r->ds, r->es);
    extern unsigned int saved_esp;
    printf("saved_esp=0x%x  program_active=%d\n", saved_esp, user_program_active());
    backtrace((unsigned int *)r->ebp, 16);
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
    if ((unsigned int)r->esp >= 0x200000 && (unsigned int)r->esp < 0x10000000) {
        serial_print("fault esp dump:\n");
        unsigned int *fsp = (unsigned int *)((unsigned int)r->esp & ~0xFu);
        for (int i = -16; i < 256 && (unsigned int)(fsp + i) < 0x10000000u; i++) {
            serial_print("  [esp");
            serial_print_hex((unsigned int)fsp + (unsigned int)i * 4);
            serial_print("]=0x");
            serial_print_hex(((unsigned int *)fsp)[i]);
            serial_print("\n");
        }
    }
    panic_diagnostics(r);
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
        spurious_count[irq]++;
        irq_eoi(irq);
        if (!spurious_log_suppressed[irq]) {
            printf("spurious IRQ %d (total %u)\n", irq, spurious_count[irq]);
            if (spurious_count[irq] >= 100) {
                spurious_log_suppressed[irq] = 1;
                printf("spurious IRQ %d: logging suppressed\n", irq);
            }
        }
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
