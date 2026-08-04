#ifndef PCI_H
#define PCI_H

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_CLASS_UHCI 0x0C0300

struct pci_dev {
    unsigned char bus, dev, func;
    unsigned int vendor, device;
    unsigned int classcode;
    unsigned int bar0;
    unsigned char irq;
};

unsigned int pci_config_read(unsigned char bus, unsigned char dev,
                             unsigned char func, unsigned char reg);
void pci_config_write(unsigned char bus, unsigned char dev,
                      unsigned char func, unsigned char reg, unsigned int value);
int pci_init(unsigned int *io_base, unsigned int *irq);
int pci_find_all(struct pci_dev *out, int max);

#endif
