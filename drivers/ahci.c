#include "ahci.h"
#include "pci.h"
#include "paging.h"
#include "serial.h"

#define AHCI_CMD_READ_DMA_EXT   0x25
#define AHCI_CMD_WRITE_DMA_EXT  0x35
#define AHCI_CMD_IDENTIFY       0xEC

#define AHCI_SECTOR_SIZE  512
#define AHCI_MAX_COUNT    128
#define AHCI_TIMEOUT      2000

extern volatile unsigned int tick;

struct ahci_dev {
    unsigned int abar;
    unsigned int port;
    unsigned int capacity_sectors;
    int present;
};

static struct ahci_dev gahci;

// Command list slot 0 (32 dwords), per-slot command table (PRDT), and the
// FIS receive area. Identity-mapped kernel memory: linear == physical.
static unsigned int cmd_list[32] __attribute__((aligned(1024)));
static unsigned int cmd_table[64] __attribute__((aligned(1024)));
static unsigned char fis_area[256] __attribute__((aligned(1024)));

// ---- Global HBA registers (from ABAR) ----
#define HBA_GHC 0x04
#define HBA_PI  0x0C

// ---- Port registers (base + port*0x80) ----
#define P_CLB   0x00
#define P_FB    0x08
#define P_IS    0x10
#define P_CMD   0x18
#define P_TFD   0x20
#define P_SIG   0x24
#define P_SSTS  0x28
#define P_CI    0x38

#define PXCMD_ST   0x1
#define PXCMD_SUD  0x2
#define PXCMD_POD  0x4
#define PXCMD_FRE  0x10
#define PXCMD_CR   0x8000

#define PXIS_TFES  0x40000000

static unsigned int mmio32(unsigned int off) {
    return *(volatile unsigned int *)(gahci.abar + off);
}

static void mmio_out32(unsigned int off, unsigned int v) {
    *(volatile unsigned int *)(gahci.abar + off) = v;
}

static unsigned int p_reg(unsigned int reg) {
    return 0x100 + gahci.port * 0x80 + reg;
}

static int ahci_wait_idle(void) {
    unsigned int start = tick;
    for (;;) {
        if (!(mmio32(p_reg(P_CMD)) & PXCMD_CR)) return 0;
        if ((int)(tick - start) >= AHCI_TIMEOUT) return -1;
    }
}

// Build the H2D Register FIS for command/lba/count, one PRD for buf, and
// issue it on slot 0. Waits for completion by polling PxCI; errors are
// reported via PxIS.TFES / PxTFD.ERR.
static int ahci_command(unsigned char cmd, int write, unsigned int lba,
                        unsigned int count, void *buf) {
    unsigned int bytes = (count == 0 ? 1 : count) * AHCI_SECTOR_SIZE;

    unsigned char *fis = (unsigned char *)cmd_table;
    for (int i = 0; i < 20; i++) fis[i] = 0;
    fis[0] = 0x27;                          // H2D Register FIS
    // QEMU's AHCI uses bit7 as the "update command register" flag and treats
    // flags bits 0..3 as the port-multiplier port (must be 0).
    fis[1] = 0x80;
    fis[2] = cmd;
    fis[7] = 0x40;                          // LBA48, drive select
    unsigned long long ll = lba;
    fis[4] = (unsigned char)(ll);
    fis[5] = (unsigned char)(ll >> 8);
    fis[6] = (unsigned char)(ll >> 16);
    fis[8] = (unsigned char)(ll >> 24);
    fis[9] = (unsigned char)(ll >> 32);
    fis[10] = (unsigned char)(ll >> 40);
    fis[12] = (unsigned char)count;         // sector count (AHCI spec Table 42)
    fis[13] = (unsigned char)(count >> 8);

    // PRDT entry at cmd_table + 0x80 (16 bytes): 64-bit address, then DW3
    // holds flags_size (data byte count - 1, since QEMU treats it as
    // zero-based); the AHCI spec places byte count in DW2, but QEMU's
    // AHCI_SG layout uses DW3.
    cmd_table[0x80 / 4] = (unsigned int)buf;  // PRD DW0: addr low
    cmd_table[0x84 / 4] = 0;                 // PRD DW1: addr high
    cmd_table[0x88 / 4] = 0;                 // PRD DW2: reserved
    cmd_table[0x8C / 4] = bytes - 1;         // PRD DW3: flags_size (byte count - 1)

    cmd_list[0] = 5 | (write ? 0x40 : 0) | (1 << 16);   // CFL=5 dwords, Write, PRDTL=1
    cmd_list[1] = 0;
    cmd_list[2] = (unsigned int)cmd_table;  // tbl_addr low DW
    cmd_list[3] = 0;                        // tbl_addr high DW (32-bit)
    cmd_list[4] = 0;

    unsigned int is = mmio32(p_reg(P_IS));
    mmio_out32(p_reg(P_IS), is);            // clear port interrupt status
    mmio_out32(p_reg(P_CI), 1);             // issue slot 0

    unsigned int start = tick;
    for (;;) {
        if (!(mmio32(p_reg(P_CI)) & 1)) break;
        if ((int)(tick - start) >= AHCI_TIMEOUT) {
            serial_print("ahci: command timeout\n");
            return -1;
        }
    }
    if (mmio32(p_reg(P_IS)) & PXIS_TFES) return -2;
    if (mmio32(p_reg(P_TFD)) & 1) return -2;
    return 0;
}

static int ahci_identify(unsigned int *sectors_out) {
    static unsigned short ident[256];
    int rc = ahci_command(AHCI_CMD_IDENTIFY, 0, 0, 0, ident);
    if (rc != 0) return -1;
    unsigned int cap28 = (unsigned int)ident[60] | ((unsigned int)ident[61] << 16);
    // LBA48 support (word 83 bit10) and enabled (word 86 bit10).
    if ((ident[83] & 0x0400) && (ident[86] & 0x0400)) {
        unsigned long long cap48 =
            (unsigned long long)ident[100] |
            ((unsigned long long)ident[101] << 16) |
            ((unsigned long long)ident[102] << 32) |
            ((unsigned long long)ident[103] << 48);
        if (cap48 > 0) {
            *sectors_out = (unsigned int)cap48;
            return 0;
        }
    }
    if (cap28 == 0) return -1;
    *sectors_out = cap28;
    return 0;
}

int ahci_present(void) {
    return gahci.present;
}

unsigned int ahci_capacity_sectors(void) {
    return gahci.capacity_sectors;
}

int ahci_read_multi(unsigned int lba, unsigned int count, void *buf) {
    if (!gahci.present) return -1;
    if (count == 0 || count > AHCI_MAX_COUNT) return -1;
    return ahci_command(AHCI_CMD_READ_DMA_EXT, 0, lba, count, buf);
}

int ahci_write_multi(unsigned int lba, unsigned int count, const void *buf) {
    if (!gahci.present) return -1;
    if (count == 0 || count > AHCI_MAX_COUNT) return -1;
    return ahci_command(AHCI_CMD_WRITE_DMA_EXT, 1, lba, count, (void *)buf);
}

int ahci_read(unsigned int lba, void *buf) {
    return ahci_read_multi(lba, 1, buf);
}

int ahci_write(unsigned int lba, const void *buf) {
    return ahci_write_multi(lba, 1, buf);
}

static void ahci_selftest(void) {
    unsigned char w[AHCI_SECTOR_SIZE], r[AHCI_SECTOR_SIZE] = {0};
    for (unsigned int i = 0; i < AHCI_SECTOR_SIZE; i++)
        w[i] = (unsigned char)(i * 7 + 3);
    unsigned int last = gahci.capacity_sectors - 1;
    if (ahci_write(last, w) == 0 && ahci_read(last, r) == 0) {
        int ok = 1;
        for (unsigned int i = 0; i < AHCI_SECTOR_SIZE; i++)
            if (w[i] != r[i]) { ok = 0; break; }
        serial_print(ok ? "ahci: selftest OK\n" : "ahci: selftest FAIL\n");
    } else {
        serial_print("ahci: selftest FAIL\n");
    }

    unsigned char wm[4 * AHCI_SECTOR_SIZE], rm[4 * AHCI_SECTOR_SIZE] = {0};
    for (unsigned int i = 0; i < sizeof(wm); i++)
        wm[i] = (unsigned char)(i * 13 + 5);
    unsigned int base = gahci.capacity_sectors - 8;
    if (ahci_write_multi(base, 4, wm) == 0 && ahci_read_multi(base, 4, rm) == 0) {
        int ok = 1;
        for (unsigned int i = 0; i < (int)sizeof(rm); i++)
            if (wm[i] != rm[i]) { ok = 0; break; }
        serial_print(ok ? "ahci: selftest multi OK\n" : "ahci: selftest multi FAIL\n");
    } else {
        serial_print("ahci: selftest multi FAIL\n");
    }
}

void ahci_init(void) {
    struct pci_dev list[16];
    int n = pci_find_all(list, 16);
    for (int i = 0; i < n; i++) {
        if (list[i].classcode != 0x010601) continue;   // SATA AHCI controller
        unsigned int bar5 = pci_config_read(list[i].bus, list[i].dev,
                                            list[i].func, 0x24);
        gahci.abar = bar5 & ~0xFFFu;
        if (paging_identity_map(gahci.abar, 4096) != 0) {
            serial_print("ahci: abar mapping failed\n");
            return;
        }
        serial_print("ahci: hba abar=");
        serial_print_hex(gahci.abar);
        serial_print("\n");

        if (!(mmio32(HBA_GHC) & 0x1))       // global HBA enable
            mmio_out32(HBA_GHC, 0x1);

        unsigned int pi = mmio32(HBA_PI);
        for (gahci.port = 0; gahci.port < 32; gahci.port++) {
            if (!(pi & (1u << gahci.port))) continue;
            unsigned int ssts = mmio32(p_reg(P_SSTS));
            if ((ssts & 0xF) != 3) continue;        // DET: device present
            if (((ssts >> 4) & 0xF) != 1) continue; // IPM: active
            // PxSIG is only valid once the port is running (device signals
            // via D2H FIS), so it cannot gate port selection here.
            break;
        }
        if (gahci.port >= 32) {
            serial_print("ahci: no disk on any port\n");
            return;
        }

        // Stop the port, program the command list / FIS area, then start.
        mmio_out32(p_reg(P_CMD), mmio32(p_reg(P_CMD)) & ~(PXCMD_ST | PXCMD_FRE));
        if (ahci_wait_idle() != 0) {
            serial_print("ahci: port busy\n");
            return;
        }
        mmio_out32(p_reg(P_CLB), (unsigned int)cmd_list);
        mmio_out32(p_reg(P_FB), (unsigned int)fis_area);
        mmio_out32(p_reg(P_CMD), PXCMD_POD | PXCMD_SUD | PXCMD_FRE);
        mmio_out32(p_reg(P_CMD), mmio32(p_reg(P_CMD)) | PXCMD_ST);

        unsigned int cap;
        if (ahci_identify(&cap) != 0 || cap == 0) {
            serial_print("ahci: identify failed\n");
            return;
        }
        gahci.capacity_sectors = cap;
        gahci.present = 1;
        serial_print("ahci: found port");
        serial_print_dec(gahci.port);
        serial_print(" capacity=");
        serial_print_dec(cap / 2048);
        serial_print(" MiB\n");
        ahci_selftest();
        return;
    }
    serial_print("ahci: no controller\n");
}