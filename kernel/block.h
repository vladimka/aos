#ifndef BLOCK_H
#define BLOCK_H

#define BLOCK_SIZE 512
#define BLOCK_CACHE_SECTORS 128

#define RAMDISK_BASE 0x400000
#define RAMDISK_SECTORS 4096

struct sdev {
    int (*read)(unsigned int lba, void *buf);
    int (*write)(unsigned int lba, const void *buf);
    int (*read_multi)(unsigned int lba, unsigned int count, void *buf);
    int (*write_multi)(unsigned int lba, unsigned int count, const void *buf);
    int present;
    unsigned int capacity_sectors;
};

int block_init(void);
unsigned char *block_pin(unsigned int lba);
void block_unpin(unsigned int lba);
void block_mark_dirty(unsigned int lba);
void block_flush(void);
int block_read_multi(unsigned int lba, unsigned int count, void *buf);
int block_write_multi(unsigned int lba, unsigned int count, const void *buf);
struct sdev *block_get_sdev(void);
void block_set_sdev(struct sdev *d);
void block_reset(void);
int block_disk_present(void);

#endif
