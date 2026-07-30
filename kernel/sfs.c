#include "sfs.h"
#include "string.h"

#define FS_MEM   ((unsigned char *)0x200000)
#define FS_SIZE  (64 * 1024)

static struct sfs_header *hdr = (struct sfs_header *)FS_MEM;

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
}

void fs_init(void) {
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
    return 0;
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

    if (off + size > FS_SIZE)
        size = FS_SIZE - off;

    char *dst = data_at(off);
    for (unsigned int j = 0; j < size; j++)
        dst[j] = data[j];

    e->size = size;
    return size;
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

void fs_list(fs_list_callback cb) {
    for (unsigned int i = 0; i < SFS_MAX_FILES; i++) {
        struct sfs_entry *e = entry_at(i);
        if (e->flags & 1)
            cb(e->name, e->size);
    }
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
