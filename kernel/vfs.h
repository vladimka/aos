#ifndef VFS_H
#define VFS_H

#include "aosabi.h"    // struct aos_stat (single source of truth)

#define VFS_MOUNTS    4
#define VFS_CACHE     128
#define VFS_OFILES    64
#define VFS_NAME_MAX  27

// Open flags (Linux-compatible values)
#define VFS_O_RDONLY   0x00000
#define VFS_O_WRONLY   0x00001
#define VFS_O_RDWR     0x00002
#define VFS_O_CREAT    0x00040
#define VFS_O_TRUNC    0x00200
#define VFS_O_APPEND   0x00400
#define VFS_O_DIRECTORY 0x10000
#define VFS_O_CREAT_DIR 0x20000   // create missing last component as a dir
#define VFS_O_NONBLOCK  0x400000

// Negative errnos
#define VFS_EPERM      -1
#define VFS_ENOENT     -2
#define VFS_EBADF      -9
#define VFS_EBUSY      -16
#define VFS_EEXIST     -17
#define VFS_ENOTDIR    -20
#define VFS_EISDIR     -21
#define VFS_EINVAL     -22
#define VFS_EMFILE     -24
#define VFS_ENOSPC     -28
#define VFS_ENOTEMPTY  -39

#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

struct vfs_fs;

struct vfs_inode {
    unsigned int ino;
    struct vfs_fs *fs;
    unsigned int type;
    unsigned int nlink;
    unsigned int size;
    unsigned int mtime;
    unsigned int refcount;
    struct vfs_inode *parent;   // weak parent link; child holds a reference
    unsigned int last_used;
    int valid;
};

// Per-filesystem operations. read_at/write_at return byte counts or negative
// errnos. lookup/mkdir/rmdir/unlink/add_dirent return 0 or negative errno.
// alloc_inode returns ino (> 0) or 0 on failure.
struct vfs_fs {
    const char *name;
    int (*read_at)(struct vfs_fs *fs, unsigned int ino, void *buf,
                   unsigned int len, unsigned int off);
    int (*write_at)(struct vfs_fs *fs, unsigned int ino, const void *buf,
                    unsigned int len, unsigned int off);
    int (*truncate)(struct vfs_fs *fs, unsigned int ino, unsigned int newsize);
    int (*lookup)(struct vfs_fs *fs, unsigned int dir_ino, const char *name,
                  unsigned int *out_ino);
    int (*add_dirent)(struct vfs_fs *fs, unsigned int dir_ino,
                      unsigned int child_ino, const char *name);
    int (*remove_dirent)(struct vfs_fs *fs, unsigned int dir_ino,
                         const char *name, unsigned int *out_child);
    int (*mkdir)(struct vfs_fs *fs, unsigned int parent_ino, const char *name,
                 unsigned int *out_ino);
    int (*rmdir)(struct vfs_fs *fs, unsigned int parent_ino, const char *name);
    int (*unlink)(struct vfs_fs *fs, unsigned int dir_ino, const char *name);
    int (*readdir)(struct vfs_fs *fs, unsigned int dir_ino, unsigned int idx,
                   char *name_out, unsigned int *ino_out);
    int (*stat)(struct vfs_fs *fs, unsigned int ino, struct aos_stat *st);
    int (*alloc_inode)(struct vfs_fs *fs, unsigned int type);
    void (*close)(struct vfs_fs *fs, unsigned int ino, int flags);
};

struct open_file {
    struct vfs_inode *inode;
    unsigned int flags;
    unsigned int pos;
    unsigned int refcount;
};

void vfs_init(void);
void vfs_format(void);

struct vfs_inode *vfs_get_root(void);   // returns referenced
struct vfs_inode *vfs_get_proc(void);   // returns referenced
struct vfs_inode *vfs_get(struct vfs_fs *fs, unsigned int ino);  // referenced
void vfs_put(struct vfs_inode *in);

struct vfs_inode *vfs_resolve(struct vfs_inode *cwd, const char *path,
                              int flags);   // returns referenced

int vfs_open_fd(struct vfs_inode *cwd, const char *path, int flags);
int vfs_pipe(int *rd, int *wr);
int vfs_close_fd(int fd);
int vfs_read_fd(int fd, void *buf, unsigned int len);
int vfs_write_fd(int fd, const void *buf, unsigned int len);
int vfs_lseek_fd(int fd, int off, int whence);
int vfs_dup_fd(int fd);
struct open_file *vfs_ofile_ptr(int fd);   // open_file for fd, or 0 if not open
int vfs_readdir_fd(int fd, char *name, unsigned int name_len);
int vfs_fstat_fd(int fd, struct aos_stat *st);
int vfs_stat(struct vfs_inode *cwd, const char *path, struct aos_stat *st);
int vfs_unlink(struct vfs_inode *cwd, const char *path);
int vfs_mkdir(struct vfs_inode *cwd, const char *path);
int vfs_rmdir(struct vfs_inode *cwd, const char *path);
int vfs_chdir(struct vfs_inode *cwd, const char *path);
int vfs_getcwd(char *buf, unsigned int len);

// Current working directory. Single kernel-global for Task 4; Task 5 moves it
// into the task struct (current_task_cwd() in syscall.c is the accessor).
extern struct vfs_inode *kernel_cwd;

int vfs_kernel_read(const char *path, void *buf, unsigned int len,
                    unsigned int off);
int vfs_kernel_write(const char *path, const void *buf, unsigned int len,
                     unsigned int off);
int vfs_kernel_stat(const char *path, struct aos_stat *st);

// procfs (kernel/procfs.c)
extern struct vfs_fs procfs_fs;

#endif
