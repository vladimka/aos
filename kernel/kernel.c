#include "vga.h"
#include "serial.h"
#include "terminal.h"
#include "gdt.h"
#include "idt.h"
#include "interrupts.h"
#include "ports.h"
#include "fs.h"
#include "progload.h"
#include "mouse.h"

volatile unsigned int tick = 0;
unsigned int __saved_mb_info = 0;
unsigned int __saved_magic = 0;

static void timer_handler(void) {
    tick++;
    if (tick % 9 == 0)
        vga_cursor_toggle();
}

static void keyboard_handler(void) {
    unsigned char status = inb(0x64);
    if (!(status & 1)) return;
    unsigned char data = inb(0x60);

    if (status & 0x20) {
        mouse_process_byte(data);
        status = inb(0x64);
        if (status & 1 && !(status & 0x20)) {
            data = inb(0x60);
            terminal_keyboard_handler(data);
        }
    } else {
        terminal_keyboard_handler(data);
    }
}

static void mouse_handler(void) {
    if (inb(0x64) & 0x20)
        mouse_process_byte(inb(0x60));
}

static void print_both(const char *s) {
    vga_print(s);
    serial_print(s);
}

void kernel_main(unsigned int magic, unsigned int mb_info) {
    __saved_mb_info = mb_info;
    __saved_magic = magic;

    serial_init();
    vga_init();

    print_both("=== AOS Kernel v0.1 ===\n");

    gdt_init();
    print_both("GDT initialized.\n");

    idt_init();
    print_both("IDT initialized.\n");

    interrupts_init();

    irq_install_handler(0, timer_handler);
    irq_install_handler(1, keyboard_handler);
    irq_install_handler(12, mouse_handler);

    mouse_init();

    fs_init();
    print_both("Filesystem ready.\n");

    load_embedded_programs();

    terminal_init();

    while (1) {
        __asm__ volatile("hlt");
    }
}
