#ifndef VBLK_H
#define VBLK_H

void vblk_init(void);
int vblk_present(void);
int vblk_read(unsigned int lba, void *buf);
int vblk_write(unsigned int lba, const void *buf);
unsigned int vblk_capacity_sectors(void);

#endif
