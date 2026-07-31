#include "mouse.h"
#include "vga.h"
#include "serial.h"
#include "ports.h"
#include "task.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64

static int packet[4];
static int pcount = 0;
static int has_wheel = 0;
static int wheel_acc = 0;

// Absolute cursor position + buttons, consumed by the window manager
static int mouse_x = 0;
static int mouse_y = 0;
static int mouse_buttons = 0;
static int mouse_wheel = 0;
static int mouse_xmax = 0;
static int mouse_ymax = 0;

static int ps2_wait_write(void) {
    int timeout = 100000;
    while (timeout--) {
        if (!(inb(PS2_STATUS) & 2)) return 1;
    }
    return 0;
}

static int ps2_wait_read(void) {
    int timeout = 100000;
    while (timeout--) {
        if (inb(PS2_STATUS) & 1) return 1;
    }
    return 0;
}

static int mouse_cmd(unsigned char cmd) {
    if (!ps2_wait_write()) return 0;
    outb(PS2_STATUS, 0xD4);
    if (!ps2_wait_write()) return 0;
    outb(PS2_DATA, cmd);
    if (!ps2_wait_read()) return 0;
    unsigned char ack = inb(PS2_DATA);
    return (ack == 0xFA);
}

void mouse_init(void) {
    __asm__ volatile("cli");

    // Drain any stale bytes in the PS/2 output buffer
    while (inb(PS2_STATUS) & 1)
        inb(PS2_DATA);

    ps2_wait_write();
    outb(PS2_STATUS, 0xA8);

    // Read + set bit 1 (enable IRQ12)
    ps2_wait_write();
    outb(PS2_STATUS, 0x20);
    ps2_wait_read();
    unsigned char config = inb(PS2_DATA);
    config |= 2;
    ps2_wait_write();
    outb(PS2_STATUS, 0x60);
    ps2_wait_write();
    outb(PS2_DATA, config);

    mouse_cmd(0xF6);

    // IntelliMouse detection: set sample rate 200, 100, 80, then read device ID
    mouse_cmd(0xF3); mouse_cmd(200);
    mouse_cmd(0xF3); mouse_cmd(100);
    mouse_cmd(0xF3); mouse_cmd(80);

    if (ps2_wait_write()) {
        outb(PS2_STATUS, 0xD4);
        if (ps2_wait_write()) {
            outb(PS2_DATA, 0xF2);
            if (ps2_wait_read()) {
                unsigned char ack = inb(PS2_DATA);
                if (ack == 0xFA && ps2_wait_read()) {
                    unsigned char dev_id = inb(PS2_DATA);
                    if (dev_id == 3) has_wheel = 1;
                }
            }
        }
    }

    mouse_cmd(0xF4);
    pcount = 0;
    __asm__ volatile("sti");

    unsigned int fb_addr = 0, fb_size = 0;
    vga_get_fb_info(&fb_addr, &fb_size);
    if (fb_size) {
        mouse_xmax = 1023;
        mouse_ymax = 767;
    }
    mouse_x = mouse_xmax ? mouse_xmax / 2 : 512;
    mouse_y = mouse_ymax ? mouse_ymax / 2 : 384;

    if (has_wheel) {
        serial_print("Mouse with wheel detected.\n");
    } else {
        serial_print("Mouse (standard) initialized.\n");
    }
}

static void irq_save(unsigned int *flags) {
    unsigned int f;
    __asm__ volatile("pushfl; pop %0" : "=r"(f));
    __asm__ volatile("cli");
    *flags = f;
}

static void irq_restore(unsigned int flags) {
    if (flags & 0x200)
        __asm__ volatile("sti");
}

void mouse_process_byte(unsigned char data) {
    if (has_wheel) {
        if (pcount == 0) {
            if (!(data & 0x08)) return;
            packet[0] = data;
            pcount = 1;
        } else if (pcount == 1) {
            packet[1] = data;
            pcount = 2;
        } else if (pcount == 2) {
            packet[2] = data;
            pcount = 3;
        } else {
            packet[3] = data;
            pcount = 0;
            int wheel = (signed char)packet[3];
            if (wheel)
                mouse_wheel += wheel;

            mouse_x += (signed char)packet[1];
            mouse_y += (signed char)packet[2];
            mouse_buttons = packet[0] & 0x07;
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_x > mouse_xmax) mouse_x = mouse_xmax;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_y > mouse_ymax) mouse_y = mouse_ymax;

            if (wheel)
                wheel_acc += (wheel > 0) ? -3 : 3;
        }
    } else {
        if (pcount == 0) {
            if (!(data & 0x08)) return;
            packet[0] = data;
            pcount = 1;
        } else if (pcount == 1) {
            packet[1] = data;
            pcount = 2;
        } else {
            packet[2] = data;
            pcount = 0;
            mouse_x += (signed char)packet[1];
            mouse_y += (signed char)packet[2];
            mouse_buttons = packet[0] & 0x07;
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_x > mouse_xmax) mouse_x = mouse_xmax;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_y > mouse_ymax) mouse_y = mouse_ymax;
        }
    }
}

// Apply accumulated wheel deltas as a single vga_scroll, but only when no GUI
// event consumer is active (the window manager polls the raw wheel itself).
// Called from the main loop with IF=1 (never from an IRQ): the multi-MB redraw
// takes tens of ms in TCG, and running it with interrupts masked would
// overflow the 16-byte emulated PS/2 queue. Draining in the IRQ stays instant.
void mouse_flush_wheel(void) {
    if (wheel_acc && task_event_pid() == 0) {
        int d = wheel_acc;
        wheel_acc = 0;
        vga_scroll(d);
    }
}

// Snapshot the cursor for the window manager; consumes the accumulated wheel.
void mouse_get_state(int *x, int *y, int *buttons, int *wheel) {
    unsigned int flags;
    irq_save(&flags);
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    if (buttons) *buttons = mouse_buttons;
    if (wheel) *wheel = mouse_wheel;
    mouse_wheel = 0;
    irq_restore(flags);
}
