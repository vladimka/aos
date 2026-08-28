#ifndef INITRAMFS_H
#define INITRAMFS_H

// Staging window for the boot initramfs: right above the 2 MB SFS ramdisk
// (0x400000..0x600000), below the fb scrollback staging (0x00C00000).
// Reserved in pmm_init so the buddy never hands these pages out.
#define INITRAMFS_BASE 0x00600000
#define INITRAMFS_MAX (2 * 1024 * 1024)

void initramfs_stage(unsigned int mb_info);
void initramfs_unpack(void);

#endif