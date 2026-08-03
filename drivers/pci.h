#ifndef PCI_H
#define PCI_H

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_CLASS_UHCI 0x0C0300

unsigned int pci_config_read(unsigned char bus, unsigned char dev,
                             unsigned char func, unsigned char reg);
int pci_init(unsigned int *io_base, unsigned int *irq);

#endif
