#ifndef SFS2_H
#define SFS2_H

#define SFS2_MAGIC0 'S'
#define SFS2_MAGIC1 'F'
#define SFS2_MAGIC2 'S'
#define SFS2_MAGIC3 '2'

#define SFS2_INODES 256
#define SFS2_INODE_SIZE 48
#define SFS2_INDIRECT_BLOCKS 128          // 512 / 4 pointers per indirect block
#define SFS2_MAX_FILE_BLOCKS (8 + SFS2_INDIRECT_BLOCKS)
#define SFS2_DIRENT_SIZE 32
#define SFS2_DIRENTS_PER_BLOCK (512 / SFS2_DIRENT_SIZE)   // 16
#define SFS2_NAME_MAX (SFS2_DIRENT_SIZE - 5)              // 27
#define SFS2_BITMAP_MAX 8                 // sectors; covers 32768 blocks

#define SFS2_TYPE_FREE 0
#define SFS2_TYPE_FILE 1
#define SFS2_TYPE_DIR  2

#define SFS2_ERR_ENOENT  -2
#define SFS2_ERR_EEXIST  -17
#define SFS2_ERR_ENOSPC  -28
#define SFS2_ERR_ENOTDIR -20
#define SFS2_ERR_EISDIR  -21
#define SFS2_ERR_ENOTEMPTY -39

struct sfs2_super {
    char magic[4];
    unsigned int version;
    unsigned int block_count;
    unsigned int inode_count;
    unsigned int inode_start;
    unsigned int bitmap_start;
    unsigned int data_start;
    unsigned int root_inode;
};

struct sfs2_inode {
    unsigned char type;        // 0
    unsigned char pad0;        // 1
    unsigned short nlink;      // 2
    unsigned int size;         // 4
    unsigned int mtime;        // 8
    unsigned int direct[8];    // 12..43
    unsigned int indirect;     // 44
};                             // 48 bytes total

struct sfs2_dirent {
    unsigned int ino;          // 4
    char name[SFS2_NAME_MAX + 1];   // 28
};                             // 32 bytes total

struct sfs2_fs {
    unsigned int block_count;
    unsigned int inode_start, bitmap_start, data_start;
    unsigned int inode_blocks, bitmap_blocks;
    unsigned int root_inode;
};

int sfs2_init(struct sfs2_fs *fs);
int sfs2_format(struct sfs2_fs *fs);
int sfs2_selftest(void);

struct sfs2_inode *sfs2_get_inode(struct sfs2_fs *fs, unsigned int ino);
int sfs2_alloc_inode(struct sfs2_fs *fs, unsigned int type);
int sfs2_lookup(struct sfs2_fs *fs, unsigned int dir_ino, const char *name,
                unsigned int *out_ino);
int sfs2_read_at(struct sfs2_fs *fs, unsigned int ino, void *buf,
                 unsigned int len, unsigned int off);
int sfs2_write_at(struct sfs2_fs *fs, unsigned int ino, const void *buf,
                  unsigned int len, unsigned int off);
int sfs2_truncate(struct sfs2_fs *fs, unsigned int ino, unsigned int newsize);
int sfs2_add_dirent(struct sfs2_fs *fs, unsigned int dir_ino,
                    unsigned int child_ino, const char *name);
int sfs2_remove_dirent(struct sfs2_fs *fs, unsigned int dir_ino,
                       const char *name, unsigned int *out_child_ino);
int sfs2_mkdir(struct sfs2_fs *fs, unsigned int parent_ino, const char *name,
               unsigned int *out_ino);
int sfs2_rmdir(struct sfs2_fs *fs, unsigned int parent_ino, const char *name);
int sfs2_unlink(struct sfs2_fs *fs, unsigned int dir_ino, const char *name);
int sfs2_readdir(struct sfs2_fs *fs, unsigned int dir_ino, unsigned int idx,
                 char *name_out, unsigned int *ino_out);
int sfs2_flush(struct sfs2_fs *fs);

#endif
