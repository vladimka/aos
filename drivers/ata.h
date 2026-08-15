#ifndef ATA_H
#define ATA_H

#define ATA_PRIMARY_BASE   0x1F0
#define ATA_PRIMARY_CTRL   0x3F6
#define ATA_SECONDARY_BASE 0x170
#define ATA_SECONDARY_CTRL 0x376

void ata_init(void);
int ata_read(unsigned int lba, void *buf);
int ata_write(unsigned int lba, const void *buf);
int ata_present(void);
unsigned int ata_capacity_sectors(void);

#endif