#include "ata.h"
#include "serial.h"
#include "ports.h"

#define ATA_CMD_IDENTIFY 0xEC
#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

#define ATA_SECTOR_SIZE   512
#define ATA_TIMEOUT_IDENTIFY 5000
#define ATA_TIMEOUT_RESET 2000

extern volatile unsigned int tick;

struct ata_dev {
    unsigned int base;
    unsigned int ctrl;
    int slave;
    unsigned int capacity_sectors;
    int present;
};

static struct ata_dev gata;

// Space register I/O ~400ns per read (ATA register settle time); also used as
// a short delay after resets. Reads the controller port so the bus is warmed.
static void ata_pause(unsigned int ctrl) {
    for (int i = 0; i < 4; i++) (void)inb(ctrl);
}

// Probe one device slot. Returns 0 and *sectors_out on success, -1 otherwise
// (no device, ATAPI, zero capacity, or IDENTIFY timeout).
static int ata_probe_drive(unsigned int base, unsigned int ctrl, int slave,
                           const char *chname, unsigned int *sectors_out) {
    outb(base + 6, 0xE0 | (slave << 4));     // select drive (LBA mode)
    ata_pause(ctrl);
    if (inb(base + 7) == 0x00) return -1;    // no device on this slot

    outb(base + 7, ATA_CMD_IDENTIFY);
    ata_pause(ctrl);

    unsigned int start = tick;
    unsigned char st;
    for (;;) {
        st = inb(base + 7);
        if (!(st & ATA_SR_BSY)) break;
        if ((int)(tick - start) >= ATA_TIMEOUT_IDENTIFY) return -1;
    }
    if (st & ATA_SR_ERR) return -1;          // device rejected IDENTIFY

    start = tick;
    while (!(inb(base + 7) & ATA_SR_DRQ)) {
        if ((int)(tick - start) >= ATA_TIMEOUT_IDENTIFY) return -1;
    }

    unsigned short ident[256];
    for (int i = 0; i < 256; i++) ident[i] = inw(base);

    if (ident[0] == 0xEB14) return -1;       // ATAPI, not a disk
    unsigned int cap = (unsigned int)ident[60] | ((unsigned int)ident[61] << 16);
    if (cap == 0) return -1;

    *sectors_out = cap;
    serial_print("ata: found ");
    serial_print(chname);
    serial_print(slave ? "/slave" : "/master");
    serial_print(", capacity=");
    serial_print_dec(cap / 2048);            // sectors / 2048 = MiB
    serial_print(" MiB\n");
    return 0;
}

int ata_present(void) {
    return gata.present;
}

unsigned int ata_capacity_sectors(void) {
    return gata.capacity_sectors;
}

static int ata_wait_ready(unsigned int timeout_ms) {
    unsigned int start = tick;
    for (;;) {
        unsigned char st = inb(gata.base + 7);
        if (!(st & ATA_SR_BSY)) {
            if (st & ATA_SR_ERR) return -2;
            return 0;
        }
        if ((int)(tick - start) >= (int)timeout_ms) return -1;
    }
}

static int ata_wait_drq(unsigned int timeout_ms) {
    unsigned int start = tick;
    for (;;) {
        unsigned char st = inb(gata.base + 7);
        if (st & ATA_SR_DRQ) return 0;
        if (st & ATA_SR_ERR) return -2;
        if ((int)(tick - start) >= (int)timeout_ms) return -1;
    }
}

// Load the 28-bit LBA parameters and issue a command (READ 0x20 / WRITE 0x30).
static void ata_send_lba(unsigned int lba, unsigned char cmd) {
    outb(gata.base + 2, 1);                  // sector count = 1
    outb(gata.base + 3, (unsigned char)lba);
    outb(gata.base + 4, (unsigned char)(lba >> 8));
    outb(gata.base + 5, (unsigned char)(lba >> 16));
    outb(gata.base + 6, 0xE0 | (gata.slave << 4) | ((lba >> 24) & 0x0F));
    outb(gata.base + 7, cmd);
}

int ata_read(unsigned int lba, void *buf) {
    if (!gata.present) return -1;
    ata_send_lba(lba, 0x20);
    int rc = ata_wait_drq(2000);
    if (rc < 0) return rc;
    unsigned short *w = (unsigned short *)buf;
    for (int i = 0; i < 256; i++) w[i] = inw(gata.base);
    return ata_wait_ready(2000);
}

int ata_write(unsigned int lba, const void *buf) {
    if (!gata.present) return -1;
    ata_send_lba(lba, 0x30);
    int rc = ata_wait_drq(2000);
    if (rc < 0) return rc;
    const unsigned short *w = (const unsigned short *)buf;
    for (int i = 0; i < 256; i++) outw(gata.base, w[i]);
    return ata_wait_ready(2000);
}

static void ata_selftest(void) {
    unsigned char w[ATA_SECTOR_SIZE], r[ATA_SECTOR_SIZE];
    for (unsigned int i = 0; i < ATA_SECTOR_SIZE; i++)
        w[i] = (unsigned char)(i * 7 + 3);
    unsigned int last = gata.capacity_sectors - 1;
    if (ata_write(last, w) == 0 && ata_read(last, r) == 0) {
        int ok = 1;
        for (unsigned int i = 0; i < ATA_SECTOR_SIZE; i++)
            if (w[i] != r[i]) { ok = 0; break; }
        serial_print(ok ? "ata: selftest OK\n" : "ata: selftest FAIL\n");
    } else {
        serial_print("ata: selftest FAIL\n");
    }
}

void ata_init(void) {
    static const struct {
        unsigned int base, ctrl;
        const char *name;
    } channels[2] = {
        { ATA_PRIMARY_BASE, ATA_PRIMARY_CTRL, "primary" },
        { ATA_SECONDARY_BASE, ATA_SECONDARY_CTRL, "secondary" },
    };

    for (int c = 0; c < 2 && !gata.present; c++) {
        unsigned int base = channels[c].base;
        unsigned int ctrl = channels[c].ctrl;

        // Software reset the channel, then wait for BSY to clear (or the
        // channel to report empty: status 0x00). QEMU is instant. nIEN is
        // kept set throughout so the polling driver never sees IRQ14/15.
        outb(ctrl, 0x06);                    // SRST + nIEN
        ata_pause(ctrl);
        outb(ctrl, 0x02);                    // release SRST, nIEN stays set
        unsigned int start = tick;
        for (;;) {
            unsigned char st = inb(base + 7);
            if (!(st & ATA_SR_BSY) || st == 0x00) break;
            if ((int)(tick - start) >= ATA_TIMEOUT_RESET) break;
        }

        for (int slave = 0; slave < 2 && !gata.present; slave++) {
            unsigned int cap;
            if (ata_probe_drive(base, ctrl, slave, channels[c].name, &cap) == 0) {
                gata.base = base;
                gata.ctrl = ctrl;
                gata.slave = slave;
                gata.capacity_sectors = cap;
                gata.present = 1;
            }
        }
    }
    if (gata.present)
        ata_selftest();
    if (!gata.present)
        serial_print("ata: no disk found\n");
}