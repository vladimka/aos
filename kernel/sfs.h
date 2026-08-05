#ifndef SFS_H
#define SFS_H

#include "fs.h"

#define SFS_MAX_FILES 64

// Ramdisk physical address and size. Must sit entirely above the kernel
// image/BSS (linker.ld asserts _end <= SFS_BASE) and below the staging
// framebuffer at 0x00C00000. Shared by kernel/sfs.c, kernel/pmm.c.
#define SFS_BASE 0x00300000
#define SFS_SIZE (1024 * 1024)

struct sfs_header {
    char         magic[4];
    unsigned int total_size;
    unsigned int entry_count;
};

struct sfs_entry {
    char         name[28];
    unsigned int size;
    unsigned int offset;
    unsigned char flags;
    unsigned char pad[3];
};

void fs_format(void);
void fs_init(void);
int  sfs_get_entry(unsigned int idx, char *name_buf, unsigned int *size_out);
void sfs_set_disk(int present);
void sfs_flush(void);

#endif
