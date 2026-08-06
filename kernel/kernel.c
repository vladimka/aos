#include "vga.h"
#include "serial.h"
#include "terminal.h"
#include "gdt.h"
#include "idt.h"
#include "interrupts.h"
#include "paging.h"
#include "pmm.h"
#include "kmm.h"
#include "user.h"
#include "task.h"
#include "printf.h"
#include "ports.h"
#include "progload.h"
#include "config.h"
#include "mouse.h"
#include "uhci.h"
#include "virtio.h"
#include "vfs.h"
#include "sfs2.h"
#include "aosipc.h"
#include "klog.h"

volatile unsigned int tick = 0;
unsigned int __saved_mb_info = 0;
unsigned int __saved_magic = 0;

// PIT channel 0 at 1000 Hz (divisor = 1193182 / 1000)
static void pit_init_1000(void) {
    outb(0x43, 0x36);
    outb(0x40, 1193 & 0xFF);
    outb(0x40, 1193 >> 8);
}

// Forward a serial byte or scancode event to the GUI event consumer's mailbox.
static void route_gui_key(int key) {
    if (key < 0) return;
    int ep = task_event_pid();
    if (ep > 0)
        task_mailbox_send((unsigned int)ep, MSG_KEY, (unsigned int)key, 0, 0, 0);
}

static void timer_handler(void) {
    tick++;
    while (serial_available()) {
        if (task_event_pid() > 0)
            route_gui_key(serial_read());
        else
            terminal_serial_byte(serial_read());
    }
    if (tick % 500 == 0 && task_event_pid() == 0)
        vga_cursor_toggle();
}

static void keyboard_handler(void) {
    // Drain every pending PS/2 byte; wheel deltas accumulate in mouse.c and
    // are applied from the main loop (never inside an IRQ).
    while (inb(0x64) & 1) {
        unsigned char status = inb(0x64);
        unsigned char data = inb(0x60);

        if (status & 0x20)
            mouse_process_byte(data);
        else if (task_event_pid() > 0)
            route_gui_key(terminal_scan_event(data));
        else
            terminal_keyboard_handler(data);
    }
}

static void mouse_handler(void) {
    while (inb(0x64) & 1) {
        if (inb(0x64) & 0x20)
            mouse_process_byte(inb(0x60));
    }
}

void kernel_main(unsigned int magic, unsigned int mb_info) {
    __saved_mb_info = mb_info;
    __saved_magic = magic;

    serial_init();
    vga_init();

    // Boot logo (text phase, before the WM takes over the framebuffer).
    // One printf call with a string literal; also lands in the COM1 log.
    printf("\n"
           "  AAA    OOO    SSS \n"
           " A   A  O   O  S    \n"
           " AAAAA  O   O   SSS \n"
           " A   A  O   O      S\n"
           " A   A   OOO    SSS \n");
    printf("=== AOS Kernel v0.3 ===\n");

    gdt_init();
    printf("GDT initialized.\n");

    idt_init();
    printf("IDT initialized.\n");

    pmm_init(__saved_mb_info);
    kmm_init();
    pmm_selftest();
    kmm_selftest();

    paging_init();
    user_init();
    task_init();

    interrupts_init();

    pit_init_1000();

    irq_install_handler(0, timer_handler);
    irq_install_handler(1, keyboard_handler);
    irq_install_handler(12, mouse_handler);

    mouse_init();

    usb_init();

    virtio_init();

    vfs_init();
    printf("Filesystem ready.\n");
    sfs2_selftest();

    config_load();

    klog(KLOG_INFO, "klog: ready");

    terminal_init();

    // Multitasking + GUI: spawn the window manager as the first user task.
    // It takes over the screen and registers as the event consumer; the idle
    // task keeps running the main loop below (mouse flush + hlt).
    unsigned int wm_pid;
    int wm_rc = task_spawn("bin/wm", "", 0, &wm_pid);
    if (wm_rc == 0) {
        printf("Window manager spawned (pid %u).\n", wm_pid);
    } else {
        struct aos_stat st;
        if (vfs_kernel_stat("bin/wm", &st) != 0)
            printf("Failed to spawn window manager (rc=%d): /bin/wm missing on the filesystem. Run `format`.\n",
                   wm_rc);
        else
            printf("Failed to spawn window manager (rc=%d): /bin/wm exists but load/spawn failed.\n",
                   wm_rc);
    }

    while (1) {
        // Wheel scrolls run here (IF=1), never inside an IRQ: the multi-MB
        // redraw would stall the interrupt for tens of ms in TCG and overflow
        // the 16-byte emulated PS/2 queue. Here keyboard/mouse IRQs keep
        // draining during the scroll, so bursts never lose packets; the
        // accumulated delta is applied in one call and any new wheel bytes
        // are picked up on the next iteration.
        mouse_flush_wheel();
        __asm__ volatile("hlt");
    }
}
