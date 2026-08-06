#include "sfs2.h"
#include "block.h"
#include "string.h"
#include "serial.h"
#include "pmm.h"
#include "rtc.h"
#include "printf.h"

#define SFS2_ERR_EINVAL  -22

_Static_assert(sizeof(struct sfs2_inode) == SFS2_INODE_SIZE, "sfs2 inode size");
_Static_assert(sizeof(struct sfs2_dirent) == SFS2_DIRENT_SIZE, "sfs2 dirent size");
_Static_assert(sizeof(struct sfs2_super) <= BLOCK_SIZE, "sfs2 superblock size");

static unsigned char inode_mem[SFS2_INODES * SFS2_INODE_SIZE];
static unsigned char bitmap_mem[SFS2_BITMAP_MAX * BLOCK_SIZE];
static unsigned char super_mem[BLOCK_SIZE];

static struct sfs2_inode *inodes = (struct sfs2_inode *)inode_mem;

static unsigned int fs_time_now(void) {
    struct aos_time t;
    if (rtc_get(&t) != 0) return 0;
    return rtc_epoch(&t);
}

static inline int bit_test(const unsigned char *bm, unsigned int i) {
    return (bm[i >> 3] >> (i & 7)) & 1;
}

static void bit_set(unsigned char *bm, unsigned int i) {
    bm[i >> 3] |= (unsigned char)(1u << (i & 7));
}

static void bit_clear(unsigned char *bm, unsigned int i) {
    bm[i >> 3] &= (unsigned char)~(1u << (i & 7));
}

static int block_read_into(unsigned int lba, void *dst) {
    unsigned char *p = block_pin(lba);
    if (!p) return -1;
    memcpy(dst, p, BLOCK_SIZE);
    block_unpin(lba);
    return 0;
}

static int block_write_from(unsigned int lba, const void *src) {
    unsigned char *p = block_pin(lba);
    if (!p) return -1;
    memcpy(p, src, BLOCK_SIZE);
    block_mark_dirty(lba);
    block_unpin(lba);
    return 0;
}

static unsigned int bitmap_blocks_for(unsigned int block_count) {
    unsigned int inode_blocks = (SFS2_INODES * SFS2_INODE_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE;
    unsigned int bs = 1 + inode_blocks;
    return (block_count - bs) / (BLOCK_SIZE * 8) + 1;
}

static void layout_init(struct sfs2_fs *fs, unsigned int block_count) {
    fs->block_count = block_count;
    fs->inode_blocks = (SFS2_INODES * SFS2_INODE_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE;
    fs->inode_start = 1;
    fs->bitmap_blocks = bitmap_blocks_for(block_count);
    fs->bitmap_start = fs->inode_start + fs->inode_blocks;
    fs->data_start = fs->bitmap_start + fs->bitmap_blocks;
    fs->root_inode = 1;
}

static void write_super(struct sfs2_fs *fs) {
    struct sfs2_super *s = (struct sfs2_super *)super_mem;
    memset(super_mem, 0, BLOCK_SIZE);
    s->magic[0] = SFS2_MAGIC0;
    s->magic[1] = SFS2_MAGIC1;
    s->magic[2] = SFS2_MAGIC2;
    s->magic[3] = SFS2_MAGIC3;
    s->version = 1;
    s->block_count = fs->block_count;
    s->inode_count = SFS2_INODES;
    s->inode_start = fs->inode_start;
    s->bitmap_start = fs->bitmap_start;
    s->data_start = fs->data_start;
    s->root_inode = fs->root_inode;
    block_write_from(0, super_mem);
}

static void load_super(struct sfs2_fs *fs) {
    struct sfs2_super *s = (struct sfs2_super *)super_mem;
    block_read_into(0, super_mem);
    fs->block_count = s->block_count;
    fs->inode_start = s->inode_start;
    fs->bitmap_start = s->bitmap_start;
    fs->data_start = s->data_start;
    fs->root_inode = s->root_inode;
    fs->inode_blocks = (SFS2_INODES * SFS2_INODE_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE;
    fs->bitmap_blocks = bitmap_blocks_for(fs->block_count);
}

static void load_meta(struct sfs2_fs *fs) {
    for (unsigned int i = 0; i < fs->inode_blocks; i++)
        block_read_into(fs->inode_start + i, inode_mem + i * BLOCK_SIZE);
    for (unsigned int i = 0; i < fs->bitmap_blocks; i++)
        block_read_into(fs->bitmap_start + i, bitmap_mem + i * BLOCK_SIZE);
}

static void save_meta(struct sfs2_fs *fs) {
    for (unsigned int i = 0; i < fs->inode_blocks; i++)
        block_write_from(fs->inode_start + i, inode_mem + i * BLOCK_SIZE);
    for (unsigned int i = 0; i < fs->bitmap_blocks; i++)
        block_write_from(fs->bitmap_start + i, bitmap_mem + i * BLOCK_SIZE);
}

struct sfs2_inode *sfs2_get_inode(struct sfs2_fs *fs, unsigned int ino) {
    (void)fs;
    if (ino == 0 || ino >= SFS2_INODES) return 0;
    return &inodes[ino];
}

static unsigned int alloc_block(struct sfs2_fs *fs) {
    for (unsigned int b = fs->data_start; b < fs->block_count; b++) {
        if (bit_test(bitmap_mem, b)) {
            bit_clear(bitmap_mem, b);
            return b;
        }
    }
    return 0;
}

static void free_block(struct sfs2_fs *fs, unsigned int blk) {
    if (blk == 0 || blk >= fs->block_count) return;
    bit_set(bitmap_mem, blk);
}

static unsigned int file_block(struct sfs2_inode *ino, unsigned int blkidx) {
    if (blkidx < 8) return ino->direct[blkidx];
    unsigned int idx = blkidx - 8;
    if (idx >= SFS2_INDIRECT_BLOCKS) return 0;
    if (ino->indirect == 0) return 0;
    unsigned int ptrs[SFS2_INDIRECT_BLOCKS];
    if (block_read_into(ino->indirect, ptrs) != 0) return 0;
    return ptrs[idx];
}

static int set_block(struct sfs2_fs *fs, struct sfs2_inode *ino,
                     unsigned int blkidx, unsigned int blk) {
    if (blkidx < 8) {
        ino->direct[blkidx] = blk;
        return 0;
    }
    unsigned int idx = blkidx - 8;
    if (idx >= SFS2_INDIRECT_BLOCKS) return -1;
    if (ino->indirect == 0) {
        unsigned int ib = alloc_block(fs);
        if (ib == 0) return -1;
        ino->indirect = ib;
        unsigned char z[BLOCK_SIZE];
        memset(z, 0, BLOCK_SIZE);
        if (block_write_from(ib, z) != 0) return -1;
    }
    unsigned int ptrs[SFS2_INDIRECT_BLOCKS];
    if (block_read_into(ino->indirect, ptrs) != 0) return -1;
    ptrs[idx] = blk;
    if (block_write_from(ino->indirect, ptrs) != 0) return -1;
    return 0;
}

int sfs2_format(struct sfs2_fs *fs) {
    layout_init(fs, fs->block_count ? fs->block_count : RAMDISK_SECTORS);
    memset(inode_mem, 0, sizeof(inode_mem));
    memset(bitmap_mem, 0, sizeof(bitmap_mem));
    for (unsigned int b = fs->data_start; b < fs->block_count; b++)
        bit_set(bitmap_mem, b);
    inodes[1].type = SFS2_TYPE_DIR;
    inodes[1].nlink = 1;
    inodes[1].size = 0;
    inodes[1].mtime = fs_time_now();
    write_super(fs);
    save_meta(fs);
    block_flush();
    return 0;
}

int sfs2_init(struct sfs2_fs *fs) {
    unsigned char sector[BLOCK_SIZE];
    if (block_read_into(0, sector) != 0) return -1;
    if (sector[0] == SFS2_MAGIC0 && sector[1] == SFS2_MAGIC1 &&
        sector[2] == SFS2_MAGIC2 && sector[3] == SFS2_MAGIC3) {
        printf("SFS2 mounted (%s).\n", block_disk_present() ? "disk" : "ramdisk");
    } else {
        printf("SFS2 formatting new %s.\n", block_disk_present() ? "disk" : "ramdisk");
        sfs2_format(fs);
    }
    load_super(fs);
    load_meta(fs);
    return 0;
}

int sfs2_alloc_inode(struct sfs2_fs *fs, unsigned int type) {
    (void)fs;
    for (unsigned int ino = 2; ino < SFS2_INODES; ino++) {
        if (inodes[ino].type == SFS2_TYPE_FREE) {
            inodes[ino].type = (unsigned char)type;
            inodes[ino].nlink = 1;
            inodes[ino].size = 0;
            inodes[ino].mtime = fs_time_now();
            for (int i = 0; i < 8; i++) inodes[ino].direct[i] = 0;
            inodes[ino].indirect = 0;
            return (int)ino;
        }
    }
    return 0;
}

int sfs2_free_inode(struct sfs2_fs *fs, unsigned int ino) {
    struct sfs2_inode *in = sfs2_get_inode(fs, ino);
    if (!in || in->type == SFS2_TYPE_FREE) return SFS2_ERR_ENOENT;
    sfs2_truncate(fs, ino, 0);
    in->type = SFS2_TYPE_FREE;
    in->nlink = 0;
    return 0;
}

int sfs2_read_at(struct sfs2_fs *fs, unsigned int ino, void *buf,
                 unsigned int len, unsigned int off) {
    struct sfs2_inode *in = sfs2_get_inode(fs, ino);
    if (!in || in->type == SFS2_TYPE_FREE) return SFS2_ERR_ENOENT;
    if (off >= in->size) return 0;
    if (len > in->size - off) len = in->size - off;
    unsigned int copied = 0;
    while (copied < len) {
        unsigned int blkidx = (off + copied) / BLOCK_SIZE;
        unsigned int blk = file_block(in, blkidx);
        if (blk == 0) break;
        unsigned int within = (off + copied) % BLOCK_SIZE;
        unsigned int take = BLOCK_SIZE - within;
        if (take > len - copied) take = len - copied;
        unsigned char *p = block_pin(blk);
        if (!p) break;
        memcpy((char *)buf + copied, p + within, take);
        block_unpin(blk);
        copied += take;
    }
    return (int)copied;
}

int sfs2_write_at(struct sfs2_fs *fs, unsigned int ino, const void *buf,
                  unsigned int len, unsigned int off) {
    struct sfs2_inode *in = sfs2_get_inode(fs, ino);
    if (!in || in->type == SFS2_TYPE_FREE) return SFS2_ERR_ENOENT;
    if (off + len > in->size) {
        unsigned int end = off + len;
        unsigned int needed = (end + BLOCK_SIZE - 1) / BLOCK_SIZE;
        if (needed > SFS2_MAX_FILE_BLOCKS) return SFS2_ERR_ENOSPC;
        unsigned int have = (in->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        for (unsigned int i = have; i < needed; i++) {
            unsigned int b = alloc_block(fs);
            if (b == 0) return SFS2_ERR_ENOSPC;
            if (set_block(fs, in, i, b) != 0) return SFS2_ERR_ENOSPC;
        }
        in->size = end;
    }
    unsigned int written = 0;
    while (written < len) {
        unsigned int blkidx = (off + written) / BLOCK_SIZE;
        unsigned int blk = file_block(in, blkidx);
        if (blk == 0) break;
        unsigned int within = (off + written) % BLOCK_SIZE;
        unsigned int take = BLOCK_SIZE - within;
        if (take > len - written) take = len - written;
        unsigned char *p = block_pin(blk);
        if (!p) break;
        memcpy(p + within, (const char *)buf + written, take);
        block_mark_dirty(blk);
        block_unpin(blk);
        written += take;
    }
    if (written > 0) in->mtime = fs_time_now();
    return (int)written;
}

int sfs2_truncate(struct sfs2_fs *fs, unsigned int ino, unsigned int newsize) {
    struct sfs2_inode *in = sfs2_get_inode(fs, ino);
    if (!in || in->type == SFS2_TYPE_FREE) return SFS2_ERR_ENOENT;
    unsigned int old_blocks = (in->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    unsigned int new_blocks = (newsize + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (new_blocks >= old_blocks) {
        in->size = newsize;
        return 0;
    }
    for (unsigned int i = new_blocks; i < old_blocks; i++) {
        unsigned int b = file_block(in, i);
        if (b) free_block(fs, b);
    }
    for (unsigned int i = new_blocks; i < 8 && i < old_blocks; i++)
        in->direct[i] = 0;
    if (old_blocks > 8) {
        if (new_blocks <= 8) {
            if (in->indirect) free_block(fs, in->indirect);
            in->indirect = 0;
        } else if (in->indirect) {
            unsigned int idx = new_blocks - 8;
            unsigned int ptrs[SFS2_INDIRECT_BLOCKS];
            if (block_read_into(in->indirect, ptrs) == 0) {
                for (unsigned int j = idx; j < SFS2_INDIRECT_BLOCKS; j++)
                    ptrs[j] = 0;
                block_write_from(in->indirect, ptrs);
            }
        }
    }
    in->size = newsize;
    in->mtime = fs_time_now();
    return 0;
}

int sfs2_lookup(struct sfs2_fs *fs, unsigned int dir_ino, const char *name,
                unsigned int *out_ino) {
    struct sfs2_inode *in = sfs2_get_inode(fs, dir_ino);
    if (!in || in->type != SFS2_TYPE_DIR) return SFS2_ERR_ENOTDIR;
    unsigned int idx = 0;
    char n[SFS2_NAME_MAX + 1];
    unsigned int ent_ino;
    for (;;) {
        int r = sfs2_readdir(fs, dir_ino, idx, n, &ent_ino);
        if (r <= 0) return SFS2_ERR_ENOENT;
        if (ent_ino != 0 && strcmp(n, name) == 0) {
            if (out_ino) *out_ino = ent_ino;
            return 0;
        }
        idx++;
    }
}

int sfs2_readdir(struct sfs2_fs *fs, unsigned int dir_ino, unsigned int idx,
                 char *name_out, unsigned int *ino_out) {
    struct sfs2_inode *in = sfs2_get_inode(fs, dir_ino);
    if (!in || in->type != SFS2_TYPE_DIR) return SFS2_ERR_ENOTDIR;
    if (idx * SFS2_DIRENT_SIZE >= in->size) return 0;
    struct sfs2_dirent rec;
    int n = sfs2_read_at(fs, dir_ino, &rec, SFS2_DIRENT_SIZE, idx * SFS2_DIRENT_SIZE);
    if (n != SFS2_DIRENT_SIZE) return 0;
    if (ino_out) *ino_out = rec.ino;
    if (name_out) {
        strncpy(name_out, rec.name, SFS2_NAME_MAX);
        name_out[SFS2_NAME_MAX] = 0;
    }
    return 1;
}

int sfs2_add_dirent(struct sfs2_fs *fs, unsigned int dir_ino,
                    unsigned int child_ino, const char *name) {
    struct sfs2_inode *in = sfs2_get_inode(fs, dir_ino);
    if (!in || in->type != SFS2_TYPE_DIR) return SFS2_ERR_ENOTDIR;
    unsigned int namelen = strlen(name);
    if (namelen == 0 || namelen > SFS2_NAME_MAX) return SFS2_ERR_EINVAL;
    unsigned int existing;
    if (sfs2_lookup(fs, dir_ino, name, &existing) == 0) return SFS2_ERR_EEXIST;
    unsigned int idx = 0;
    for (;;) {
        struct sfs2_dirent rec;
        int n = sfs2_read_at(fs, dir_ino, &rec, SFS2_DIRENT_SIZE,
                             idx * SFS2_DIRENT_SIZE);
        if (n != SFS2_DIRENT_SIZE) break;
        if (rec.ino == 0) {
            rec.ino = child_ino;
            memset(rec.name, 0, sizeof(rec.name));
            strncpy(rec.name, name, SFS2_NAME_MAX);
            int r = sfs2_write_at(fs, dir_ino, &rec, SFS2_DIRENT_SIZE,
                                  idx * SFS2_DIRENT_SIZE);
            if (r < 0) return r;
            in->mtime = fs_time_now();
            return 0;
        }
        idx++;
    }
    struct sfs2_dirent rec;
    rec.ino = child_ino;
    memset(rec.name, 0, sizeof(rec.name));
    strncpy(rec.name, name, SFS2_NAME_MAX);
    int r = sfs2_write_at(fs, dir_ino, &rec, SFS2_DIRENT_SIZE, idx * SFS2_DIRENT_SIZE);
    if (r < 0) return r;
    in->mtime = fs_time_now();
    return 0;
}

int sfs2_remove_dirent(struct sfs2_fs *fs, unsigned int dir_ino,
                       const char *name, unsigned int *out_child_ino) {
    struct sfs2_inode *in = sfs2_get_inode(fs, dir_ino);
    if (!in || in->type != SFS2_TYPE_DIR) return SFS2_ERR_ENOTDIR;
    unsigned int idx = 0;
    for (;;) {
        struct sfs2_dirent rec;
        int n = sfs2_read_at(fs, dir_ino, &rec, SFS2_DIRENT_SIZE,
                             idx * SFS2_DIRENT_SIZE);
        if (n != SFS2_DIRENT_SIZE) return SFS2_ERR_ENOENT;
        if (rec.ino != 0 && strcmp(rec.name, name) == 0) {
            unsigned int child = rec.ino;
            rec.ino = 0;
            int r = sfs2_write_at(fs, dir_ino, &rec, SFS2_DIRENT_SIZE,
                                  idx * SFS2_DIRENT_SIZE);
            if (r < 0) return r;
            struct sfs2_inode *ci = sfs2_get_inode(fs, child);
            if (ci) {
                if (ci->nlink > 0) ci->nlink--;
                ci->mtime = fs_time_now();
            }
            in->mtime = fs_time_now();
            if (out_child_ino) *out_child_ino = child;
            return 0;
        }
        idx++;
    }
}

int sfs2_mkdir(struct sfs2_fs *fs, unsigned int parent_ino, const char *name,
               unsigned int *out_ino) {
    struct sfs2_inode *p = sfs2_get_inode(fs, parent_ino);
    if (!p || p->type != SFS2_TYPE_DIR) return SFS2_ERR_ENOTDIR;
    unsigned int existing;
    if (sfs2_lookup(fs, parent_ino, name, &existing) == 0) return SFS2_ERR_EEXIST;
    unsigned int child = (unsigned int)sfs2_alloc_inode(fs, SFS2_TYPE_DIR);
    if (child == 0) return SFS2_ERR_ENOSPC;
    int r = sfs2_add_dirent(fs, parent_ino, child, name);
    if (r < 0) {
        inodes[child].type = SFS2_TYPE_FREE;
        inodes[child].nlink = 0;
        return r;
    }
    if (out_ino) *out_ino = child;
    return 0;
}

int sfs2_rmdir(struct sfs2_fs *fs, unsigned int parent_ino, const char *name) {
    unsigned int child;
    int r = sfs2_lookup(fs, parent_ino, name, &child);
    if (r < 0) return r;
    struct sfs2_inode *in = sfs2_get_inode(fs, child);
    if (!in || in->type != SFS2_TYPE_DIR) return SFS2_ERR_ENOTDIR;
    unsigned int idx = 0;
    for (;;) {
        struct sfs2_dirent rec;
        int n = sfs2_read_at(fs, child, &rec, SFS2_DIRENT_SIZE,
                             idx * SFS2_DIRENT_SIZE);
        if (n != SFS2_DIRENT_SIZE) break;
        if (rec.ino != 0) return SFS2_ERR_ENOTEMPTY;
        idx++;
    }
    r = sfs2_remove_dirent(fs, parent_ino, name, &child);
    if (r < 0) return r;
    return sfs2_free_inode(fs, child);
}

int sfs2_unlink(struct sfs2_fs *fs, unsigned int dir_ino, const char *name) {
    unsigned int child;
    int r = sfs2_remove_dirent(fs, dir_ino, name, &child);
    if (r < 0) return r;
    struct sfs2_inode *ci = sfs2_get_inode(fs, child);
    if (ci && ci->type == SFS2_TYPE_DIR) return SFS2_ERR_EISDIR;
    if (ci && ci->nlink == 0) sfs2_free_inode(fs, child);
    return 0;
}

void sfs2_flush(struct sfs2_fs *fs) {
    save_meta(fs);
    block_flush();
}

// ---- selftest (isolated RAM device, never touches the real backend) ----

// The inode table and bitmap are shared global buffers; the selftest must
// save/restore them so the live filesystem metadata survives the run.
static unsigned char saved_inode_mem[SFS2_INODES * SFS2_INODE_SIZE];
static unsigned char saved_bitmap_mem[SFS2_BITMAP_MAX * BLOCK_SIZE];
static unsigned char saved_super_mem[BLOCK_SIZE];

static unsigned char *selftest_ram;
static struct sdev selftest_sdev;

static int selftest_read(unsigned int lba, void *buf) {
    memcpy(buf, selftest_ram + lba * BLOCK_SIZE, BLOCK_SIZE);
    return 0;
}

static int selftest_write(unsigned int lba, const void *buf) {
    memcpy(selftest_ram + lba * BLOCK_SIZE, buf, BLOCK_SIZE);
    return 0;
}

static void selftest_fail(const char *what) {
    serial_print("sfs2: selftest FAIL ");
    serial_print(what);
    serial_print("\n");
}

int sfs2_selftest(void) {
    selftest_ram = (unsigned char *)page_alloc_order(8);   // 1 MiB scratch
    if (!selftest_ram) {
        selftest_fail("(no pages)");
        return -1;
    }
    memset(selftest_ram, 0, 1024 * 1024);
    memcpy(saved_inode_mem, inode_mem, sizeof(saved_inode_mem));
    memcpy(saved_bitmap_mem, bitmap_mem, sizeof(saved_bitmap_mem));
    memcpy(saved_super_mem, super_mem, sizeof(saved_super_mem));
    selftest_sdev.read = selftest_read;
    selftest_sdev.write = selftest_write;
    selftest_sdev.present = 1;
    selftest_sdev.capacity_sectors = RAMDISK_SECTORS;

    struct sdev *saved = block_get_sdev();
    block_flush();                    // push dirty live-fs blocks to the real device
    block_set_sdev(&selftest_sdev);
    block_reset();

    struct sfs2_fs fs;
    memset(&fs, 0, sizeof(fs));
    if (sfs2_init(&fs) != 0) {
        selftest_fail("(init)");
        goto done;
    }
    if (fs.root_inode != 1) {
        selftest_fail("(root)");
        goto done;
    }
    unsigned int dir_ino;
    if (sfs2_mkdir(&fs, 1, "dir", &dir_ino) != 0) {
        selftest_fail("(mkdir)");
        goto done;
    }
    unsigned int f_ino;
    if (sfs2_lookup(&fs, 1, "dir", &f_ino) != 0 || f_ino != dir_ino) {
        selftest_fail("(lookup dir)");
        goto done;
    }
    int f2 = sfs2_alloc_inode(&fs, SFS2_TYPE_FILE);
    if (f2 <= 0) {
        selftest_fail("(alloc inode)");
        goto done;
    }
    unsigned int file_ino = (unsigned int)f2;
    if (sfs2_add_dirent(&fs, dir_ino, file_ino, "a.txt") != 0) {
        selftest_fail("(add dirent)");
        goto done;
    }
    static unsigned char wbuf[68000];
    for (unsigned int i = 0; i < sizeof(wbuf); i++) wbuf[i] = (unsigned char)(i * 31 + 7);
    if (sfs2_write_at(&fs, file_ino, wbuf, sizeof(wbuf), 0) != (int)sizeof(wbuf)) {
        selftest_fail("(write big)");
        goto done;
    }
    struct sfs2_inode *fin = sfs2_get_inode(&fs, file_ino);
    if (fin->size != sizeof(wbuf) || fin->indirect == 0) {
        selftest_fail("(size/indirect)");
        goto done;
    }
    static unsigned char rbuf[68000];
    if (sfs2_read_at(&fs, file_ino, rbuf, sizeof(rbuf), 0) != (int)sizeof(rbuf)) {
        selftest_fail("(read big)");
        goto done;
    }
    for (unsigned int i = 0; i < sizeof(rbuf); i++) {
        if (rbuf[i] != wbuf[i]) {
            selftest_fail("(data mismatch)");
            goto done;
        }
    }
    char n[SFS2_NAME_MAX + 1];
    unsigned int ent;
    if (sfs2_readdir(&fs, 1, 0, n, &ent) != 1 || ent != dir_ino ||
        strcmp(n, "dir") != 0) {
        selftest_fail("(readdir root)");
        goto done;
    }
    if (sfs2_readdir(&fs, dir_ino, 0, n, &ent) != 1 || ent != file_ino ||
        strcmp(n, "a.txt") != 0) {
        selftest_fail("(readdir dir)");
        goto done;
    }
    if (sfs2_unlink(&fs, dir_ino, "a.txt") != 0) {
        selftest_fail("(unlink)");
        goto done;
    }
    if (sfs2_lookup(&fs, dir_ino, "a.txt", &ent) == 0) {
        selftest_fail("(unlink left entry)");
        goto done;
    }
    if (sfs2_rmdir(&fs, 1, "dir") != 0) {
        selftest_fail("(rmdir)");
        goto done;
    }
    sfs2_flush(&fs);
    serial_print("sfs2: selftest OK\n");

done:
    block_reset();
    block_set_sdev(saved);
    memcpy(inode_mem, saved_inode_mem, sizeof(saved_inode_mem));
    memcpy(bitmap_mem, saved_bitmap_mem, sizeof(saved_bitmap_mem));
    memcpy(super_mem, saved_super_mem, sizeof(saved_super_mem));
    page_free_order(selftest_ram, 8);
    return 0;
}
