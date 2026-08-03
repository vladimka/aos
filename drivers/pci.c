#include "pci.h"
#include "ports.h"

static unsigned int config_addr(unsigned char bus, unsigned char dev,
                                unsigned char func, unsigned char reg) {
    return 0x80000000U | ((unsigned int)bus << 16) | ((unsigned int)dev << 11) |
           ((unsigned int)func << 8) | (reg & 0xFC);
}

unsigned int pci_config_read(unsigned char bus, unsigned char dev,
                             unsigned char func, unsigned char reg) {
    outl(PCI_CONFIG_ADDR, config_addr(bus, dev, func, reg));
    return inl(PCI_CONFIG_DATA);
}

int pci_init(unsigned int *io_base, unsigned int *irq) {
    unsigned int b, d, f;
    for (b = 0; b < 1; b++) {
        for (d = 0; d < 32; d++) {
            for (f = 0; f < 8; f++) {
                unsigned int id = pci_config_read(b, d, f, 0x00);
                if (id == 0xFFFFFFFF || id == 0)
                    continue;
                unsigned int class_code = pci_config_read(b, d, f, 0x08) >> 8;
                if (class_code == PCI_CLASS_UHCI) {
                    unsigned int bar = pci_config_read(b, d, f, 0x20);
                    *io_base = bar & ~0x3;
                    *irq = pci_config_read(b, d, f, 0x3C) & 0xFF;
                    return 0;
                }
            }
        }
    }
    return -1;
}
