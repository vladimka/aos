#ifndef BLOCK_H
#define BLOCK_H

#define BLOCK_SIZE 512
#define BLOCK_CACHE_SECTORS 128

#define RAMDISK_BASE 0x300000
#define RAMDISK_SECTORS 2048

struct sdev {
    int (*read)(unsigned int lba, void *buf);
    int (*write)(unsigned int lba, const void *buf);
    int present;
    unsigned int capacity_sectors;
};

int block_init(void);
unsigned char *block_pin(unsigned int lba);
void block_unpin(unsigned int lba);
void block_flush(void);
struct sdev *block_get_sdev(void);
void block_set_sdev(struct sdev *d);
void block_reset(void);
int block_disk_present(void);

#endif
