#include "mouse.h"
#include "vga.h"
#include "serial.h"
#include "ports.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64

static int packet[4];
static int pcount = 0;
static int has_wheel = 0;

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
    if (has_wheel) {
        serial_print("Mouse with wheel detected.\n");
    } else {
        serial_print("Mouse (standard) initialized.\n");
    }
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
                vga_scroll((wheel > 0) ? -3 : 3);
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
        }
    }
}
