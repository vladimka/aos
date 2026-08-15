#ifndef AHCI_H
#define AHCI_H

void ahci_init(void);
int ahci_present(void);
unsigned int ahci_capacity_sectors(void);
int ahci_read(unsigned int lba, void *buf);
int ahci_write(unsigned int lba, const void *buf);
int ahci_read_multi(unsigned int lba, unsigned int count, void *buf);
int ahci_write_multi(unsigned int lba, unsigned int count, const void *buf);

#endif