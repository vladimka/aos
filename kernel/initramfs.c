#include "initramfs.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"

// newc (SVR4) cpio archive cooked by scripts/gen_progs.py --cpio, byte-for-byte
// the layout Linux usr/gen_init_cpio.c produces:
//   110-byte ASCII header: "070701" + 13 %08X fields
//     [0:6]   magic     [6:14]  ino      [14:22] mode
//     [22:30] uid       [30:38] gid      [38:46] nlink
//     [46:54] mtime     [54:62] filesize [62:70] devmajor
//     [70:78] devminor  [78:86] rdevmajor [86:94] rdevminor
//     [94:102] namesize [102:110] chksum
//   then the name (namesize bytes incl. NUL), padded so the file data starts
//   at a 4-byte boundary relative to the archive start; file data padded to 4;
//   a "TRAILER!!!" entry ends the archive, which is padded to 512 bytes.
// S_IFDIR = 0040000, S_IFREG = 0100000.

#define NEWC_MAGIC "070701"
#define NEWC_HDR 110
#define NEWC_TRAILER "TRAILER!!!"

static unsigned int ir_size = 0;

static unsigned int hexval(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static unsigned int hexn(const unsigned char *s, unsigned int n) {
    unsigned int v = 0;
    for (unsigned int i = 0; i < n; i++) v = (v << 4) | hexval(s[i]);
    return v;
}

// memcpy that survives src/dst overlap (the GRUB-loaded module can straddle
// the staging window): forward when src <= dst, backward otherwise.
static void copy_overlap(void *dst, const void *src, unsigned int n) {
    const unsigned char *s = src;
    unsigned char *d = dst;
    if ((unsigned long)s <= (unsigned long)d) {
        while (n--) *d++ = *s++;
    } else {
        s += n;
        d += n;
        while (n--) *--d = *--s;
    }
}

// Early boot, while everything is still identity-mapped: find the multiboot2
// module tag (type 3) and copy the initramfs into the reserved staging
// window. GRUB drops modules wherever it likes, often overlapping the ramdisk
// that vfs_format/SFS are about to clobber, so copying out of the way is
// mandatory before any filesystem activity.
void initramfs_stage(unsigned int mb_info) {
    if (!mb_info) return;
    unsigned char *mbi = (unsigned char *)mb_info;
    unsigned int total = *(unsigned int *)mbi;
    unsigned char *tag = mbi + 8;
    while ((unsigned int)(tag - mbi) < total) {
        unsigned int type = *(unsigned int *)tag;
        unsigned int size = *(unsigned int *)(tag + 4);
        if (type == 0) break;
        if (type == 3) {                              // module
            unsigned int mod_start = *(unsigned int *)(tag + 8);
            unsigned int mod_end = *(unsigned int *)(tag + 12);
            unsigned int sz = mod_end - mod_start;
            if (sz > INITRAMFS_MAX) {
                printf("initramfs: module too large (%u bytes), ignored\n", sz);
                return;
            }
            copy_overlap((void *)INITRAMFS_BASE, (void *)mod_start, sz);
            ir_size = sz;
            printf("initramfs: staged %u bytes from 0x%x\n", sz, mod_start);
            return;
        }
        tag += (size + 7) & ~7;
    }
    printf("initramfs: no module tag, filesystem stays as-is\n");
}

// Re-extract the SFS seed from the staged initramfs, write-if-absent: a file
// already present in the persistent root wins (user edits are preserved), a
// freshly formatted SFS gets the full population.
static void extract_entry(const char *name, const unsigned char *data,
                          unsigned int size) {
    struct aos_stat st;
    if (vfs_kernel_stat(name, &st) == 0) return;
    int ret = vfs_kernel_write(name, (const char *)data, size, 0);
    if (ret < 0)
        printf("initramfs: write failed: %s (rc=%d)\n", name, ret);
}

void initramfs_unpack(void) {
    unsigned int files = 0, dirs = 0;
    unsigned int off = 0;
    const unsigned char *p = (const unsigned char *)INITRAMFS_BASE;
    while (ir_size && off + NEWC_HDR <= ir_size) {
        const unsigned char *h = p + off;
        if (strncmp((const char *)h, NEWC_MAGIC, 6) != 0) {
            printf("initramfs: bad magic at offset %u\n", off);
            break;
        }
        unsigned int mode = hexn(h + 14, 8);
        unsigned int size = hexn(h + 54, 8);
        unsigned int nsz = hexn(h + 94, 8);
        if (nsz < 2 || off + NEWC_HDR + nsz > ir_size) break;
        const char *name = (const char *)(h + NEWC_HDR);   // NUL-terminated
        unsigned int data_off = off + NEWC_HDR + nsz;
        data_off = (data_off + 3u) & ~3u;
        if (data_off + size > ir_size) break;
        if (strcmp(name, NEWC_TRAILER) == 0)
            break;                                        // end of archive
        if ((mode & 0170000) == 0040000) {                // directory
            vfs_kernel_mkdir(name);
            dirs++;
        } else {
            extract_entry(name, p + data_off, size);
            files++;
        }
        off = (data_off + size + 3u) & ~3u;
    }
    printf("initramfs: unpacked %u files, %u dirs\n", files, dirs);
}