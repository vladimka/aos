#include "sfs.h"
#include "string.h"
#include "vblk.h"
#include "serial.h"
#include "kmm.h"

#define FS_MEM   ((unsigned char *)SFS_BASE)
#define FS_SIZE  SFS_SIZE

#define FS_SECTORS (FS_SIZE / 512)

static struct sfs_header *hdr = (struct sfs_header *)FS_MEM;

static int disk_present;
static unsigned char dirty_bits[FS_SECTORS / 8];

void sfs_set_disk(int present) {
    disk_present = present;
}

static void sfs_touch(unsigned int off, unsigned int len) {
    if (!disk_present || len == 0) return;
    unsigned int s0 = off / 512;
    unsigned int s1 = (off + len - 1) / 512;
    if (s1 >= FS_SECTORS) s1 = FS_SECTORS - 1;
    for (unsigned int s = s0; s <= s1; s++)
        dirty_bits[s / 8] |= (unsigned char)(1 << (s % 8));
}

void sfs_flush(void) {
    if (!disk_present) return;
    unsigned char *sector = kmalloc(512);
    if (!sector) return;                 // OOM: skip the flush, keep RAM copy
    for (unsigned int s = 0; s < FS_SECTORS; s++) {
        if (!(dirty_bits[s / 8] & (1 << (s % 8)))) continue;
        for (unsigned int j = 0; j < 512; j++)
            sector[j] = FS_MEM[s * 512 + j];
        if (vblk_write(s, sector) != 0)
            serial_print("sfs: flush sector fail\n");
        dirty_bits[s / 8] &= (unsigned char)~(1 << (s % 8));
    }
    kfree(sector);
}

static struct sfs_entry *entry_at(unsigned int i) {
    return (struct sfs_entry *)(FS_MEM + sizeof(struct sfs_header) + i * sizeof(struct sfs_entry));
}

static char *data_at(unsigned int offset) {
    return (char *)(FS_MEM + offset);
}

static unsigned int data_start(void) {
    return sizeof(struct sfs_header) + SFS_MAX_FILES * sizeof(struct sfs_entry);
}

void fs_format(void) {
    hdr->magic[0] = 'S'; hdr->magic[1] = 'F';
    hdr->magic[2] = 'S'; hdr->magic[3] = '1';
    hdr->total_size = FS_SIZE - data_start();
    hdr->entry_count = 0;

    for (unsigned int i = 0; i < SFS_MAX_FILES; i++) {
        struct sfs_entry *e = entry_at(i);
        e->name[0] = '\0';
        e->size = 0;
        e->offset = 0;
        e->flags = 0;
    }
    sfs_touch(0, FS_SIZE);
    sfs_flush();
}

void fs_init(void) {
    if (disk_present) {
        unsigned char *sector = kmalloc(512);
        unsigned int s;
        if (sector) {
            for (s = 0; s < FS_SECTORS; s++) {
                if (vblk_read(s, sector) != 0) break;
                for (unsigned int j = 0; j < 512; j++)
                    FS_MEM[s * 512 + j] = sector[j];
            }
            kfree(sector);
            if (s == FS_SECTORS && hdr->magic[0] == 'S' && hdr->magic[1] == 'F' &&
                hdr->magic[2] == 'S' && hdr->magic[3] == '1') {
                serial_print("SFS mounted from disk.\n");
                return;
            }
        }
        serial_print("SFS formatting new disk.\n");
        fs_format();
        sfs_flush();
        return;
    }
    if (hdr->magic[0] != 'S' || hdr->magic[1] != 'F' ||
        hdr->magic[2] != 'S' || hdr->magic[3] != '1') {
        fs_format();
    }
}

static int find_entry(const char *name) {
    for (unsigned int i = 0; i < SFS_MAX_FILES; i++) {
        struct sfs_entry *e = entry_at(i);
        if (e->flags & 1 && strcmp(e->name, name) == 0)
            return i;
    }
    return -1;
}

static int find_free(void) {
    for (unsigned int i = 0; i < SFS_MAX_FILES; i++) {
        struct sfs_entry *e = entry_at(i);
        if (!(e->flags & 1))
            return i;
    }
    return -1;
}

static unsigned int next_data(void) {
    unsigned int off = data_start();
    for (unsigned int i = 0; i < SFS_MAX_FILES; i++) {
        struct sfs_entry *e = entry_at(i);
        if (e->flags & 1) {
            unsigned int end = e->offset + e->size;
            if (end > off) off = end;
        }
    }
    return off;
}

int fs_exists(const char *name) {
    return find_entry(name) >= 0;
}

int fs_get_size(const char *name) {
    int i = find_entry(name);
    if (i < 0) return -1;
    return entry_at(i)->size;
}

int fs_create(const char *name) {
    if (find_entry(name) >= 0) return -1;
    int i = find_free();
    if (i < 0) return -2;

    struct sfs_entry *e = entry_at(i);
    strncpy(e->name, name, 27);
    e->name[27] = '\0';
    e->size = 0;
    e->offset = next_data();
    e->flags = 1;
    hdr->entry_count++;
    sfs_touch(0, data_start());
    sfs_flush();
    return 0;
}

int fs_delete(const char *name) {
    int i = find_entry(name);
    if (i < 0) return -1;

    struct sfs_entry *e = entry_at(i);
    e->name[0] = '\0';
    e->size = 0;
    e->offset = 0;
    e->flags = 0;
    hdr->entry_count--;
    sfs_touch(0, data_start());
    sfs_flush();
    return 0;
}

int fs_read(const char *name, char *buf, unsigned int size) {
    int i = find_entry(name);
    if (i < 0) return -1;

    struct sfs_entry *e = entry_at(i);
    char *src = data_at(e->offset);

    if (size > e->size) size = e->size;
    for (unsigned int j = 0; j < size; j++)
        buf[j] = src[j];

    return size;
}

int fs_read_at(const char *name, char *buf, unsigned int size, unsigned int offset) {
    int i = find_entry(name);
    if (i < 0) return -1;

    struct sfs_entry *e = entry_at(i);
    if (offset >= e->size) return 0;
    if (size > e->size - offset) size = e->size - offset;

    char *src = data_at(e->offset + offset);
    for (unsigned int j = 0; j < size; j++)
        buf[j] = src[j];

    return size;
}

int fs_write(const char *name, const char *data, unsigned int size) {
    int i = find_entry(name);
    if (i < 0) {
        int r = fs_create(name);
        if (r < 0) return r;
        i = find_entry(name);
    }

    struct sfs_entry *e = entry_at(i);
    unsigned int off = e->offset;

    if (size > FS_SIZE - off) return -3;  // not enough space
    unsigned int end = off + size;

    // Must not overwrite another file's data
    for (unsigned int k = 0; k < SFS_MAX_FILES; k++) {
        if (k == (unsigned int)i) continue;
        struct sfs_entry *f = entry_at(k);
        if (!(f->flags & 1)) continue;
        unsigned int f_end = f->offset + f->size;
        if (f_end > off && f->offset < end) return -4;  // overlaps another file
    }

    char *dst = data_at(off);
    for (unsigned int j = 0; j < size; j++)
        dst[j] = data[j];

    e->size = size;
    sfs_touch(off, size);
    sfs_flush();
    return size;
}

int sfs_get_entry(unsigned int idx, char *name_buf, unsigned int *size_out) {
    if (idx >= SFS_MAX_FILES) return -1;
    struct sfs_entry *e = entry_at(idx);
    if (!(e->flags & 1)) return -1;
    strncpy(name_buf, e->name, 27);
    name_buf[27] = '\0';
    *size_out = e->size;
    return 0;
}
