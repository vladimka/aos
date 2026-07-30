#ifndef SFS_H
#define SFS_H

#include "fs.h"

#define SFS_MAX_FILES 64

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

#endif
