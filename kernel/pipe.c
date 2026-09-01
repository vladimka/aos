#include "pipe.h"
#include "string.h"

static struct aos_pipe pipes[PIPE_MAX];

struct aos_pipe *pipe_alloc(void) {
    for (int i = 0; i < PIPE_MAX; i++) {
        if (pipes[i].used) continue;
        struct aos_pipe *p = &pipes[i];
        p->used = 1;
        p->head = 0;
        p->tail = 0;
        p->count = 0;
        p->nreaders = 1;
        p->nwriters = 1;
        memset(&p->inode, 0, sizeof(p->inode));
        p->inode.ino = (unsigned int)i + 1;
        p->inode.fs = &pipefs_fs;
        p->inode.type = 1;
        p->inode.nlink = 1;
        p->inode.size = 0;
        p->inode.refcount = 2;
        p->inode.valid = 0;     // never cached; vfs_put releases nothing
        return p;
    }
    return 0;
}

static int pipe_read_at(struct vfs_fs *fs, unsigned int ino, void *buf,
                        unsigned int len, unsigned int off) {
    (void)fs; (void)off;
    struct aos_pipe *p = &pipes[ino - 1];
    if (len == 0) return 0;
    while (p->count == 0 && p->nwriters > 0)
        __asm__ __volatile__("sti; hlt; cli");
    if (p->count == 0) return 0;        // EOF: no writers left
    unsigned int n = len < p->count ? len : p->count;
    for (unsigned int i = 0; i < n; i++) {
        ((unsigned char *)buf)[i] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
    }
    p->count -= n;
    return (int)n;
}

static int pipe_write_at(struct vfs_fs *fs, unsigned int ino, const void *buf,
                         unsigned int len, unsigned int off) {
    (void)fs; (void)off;
    struct aos_pipe *p = &pipes[ino - 1];
    if (len == 0) return 0;
    unsigned int done = 0;
    while (done < len) {
        if (p->nreaders == 0) return -32;   // -EPIPE
        while (p->count == PIPE_BUF_SIZE && p->nreaders > 0)
            __asm__ __volatile__("sti; hlt; cli");
        unsigned int space = PIPE_BUF_SIZE - p->count;
        unsigned int n = len - done;
        if (n > space) n = space;
        for (unsigned int i = 0; i < n; i++) {
            p->buf[p->head] = ((const unsigned char *)buf)[done + i];
            p->head = (p->head + 1) % PIPE_BUF_SIZE;
        }
        p->count += n;
        done += n;
    }
    return (int)len;
}

static int pipe_stat(struct vfs_fs *fs, unsigned int ino, struct aos_stat *st) {
    (void)fs;
    st->type = 1;
    st->size = pipes[ino - 1].count;
    st->mtime = 0;
    st->nlink = 1;
    return 0;
}

int pipe_read_nonblock(struct vfs_fs *fs, unsigned int ino, void *buf,
                       unsigned int len, unsigned int off) {
    (void)fs; (void)off;
    if (len == 0) return 0;
    struct aos_pipe *p = &pipes[ino - 1];
    if (p->count == 0)
        return p->nwriters > 0 ? -11 : 0;          // EAGAIN / EOF
    unsigned int n = len < p->count ? len : p->count;
    for (unsigned int i = 0; i < n; i++) {
        ((unsigned char *)buf)[i] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
    }
    p->count -= n;
    return (int)n;
}

int pipe_write_nonblock(struct vfs_fs *fs, unsigned int ino, const void *buf,
                        unsigned int len, unsigned int off) {
    (void)fs; (void)off;
    if (len == 0) return 0;
    struct aos_pipe *p = &pipes[ino - 1];
    if (p->nreaders == 0) return -32;              // EPIPE
    if (p->count == PIPE_BUF_SIZE) return -11;     // EAGAIN
    unsigned int space = PIPE_BUF_SIZE - p->count;
    unsigned int n = len < space ? len : space;
    for (unsigned int i = 0; i < n; i++) {
        p->buf[p->head] = ((const unsigned char *)buf)[i];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
    }
    p->count += n;
    return (int)n;
}

int pipe_poll(struct vfs_inode *in, short events, short *ready) {
    if (!in || in->fs != &pipefs_fs) return -9;      /* -EBADF */
    struct aos_pipe *p = &pipes[in->ino - 1];
    short r = 0;
    if (p->count > 0) r |= POLLIN;
    if (p->nwriters == 0) r |= POLLHUP;              /* writer gone -> EOF */
    if (p->count < PIPE_BUF_SIZE) r |= POLLOUT;
    *ready = (short)(r & events);
    return 0;
}

void pipe_close(struct vfs_fs *fs, unsigned int ino, int flags) {
    (void)fs;
    struct aos_pipe *p = &pipes[ino - 1];
    if (flags & VFS_O_WRONLY) {
        if (p->nwriters > 0) p->nwriters--;
    } else if (p->nreaders > 0) {
        p->nreaders--;
    }
    if (p->nreaders == 0 && p->nwriters == 0)
        p->used = 0;
}

void pipe_dup(struct vfs_fs *fs, unsigned int ino, int flags) {
    (void)fs;
    struct aos_pipe *p = &pipes[ino - 1];
    if (flags & VFS_O_WRONLY) p->nwriters++;
    else p->nreaders++;
}

// Stub directory ops: a pipe fd can be passed as a dirfd to openat/fstatat64,
// which resolves it via vfs_get -> fs->stat (works) and then walks the path
// with fs->lookup. These must never be NULL.
static int pipe_ro_lookup(struct vfs_fs *fs, unsigned int dir_ino,
                          const char *name, unsigned int *out_ino) {
    (void)fs; (void)dir_ino; (void)name; (void)out_ino;
    return VFS_ENOTDIR;
}

static int pipe_ro_readdir(struct vfs_fs *fs, unsigned int dir_ino,
                           unsigned int idx, char *name_out,
                           unsigned int *ino_out) {
    (void)fs; (void)dir_ino; (void)idx; (void)name_out; (void)ino_out;
    return VFS_ENOTDIR;
}

static int pipe_ro_add(struct vfs_fs *fs, unsigned int dir_ino,
                       unsigned int child_ino, const char *name) {
    (void)fs; (void)dir_ino; (void)child_ino; (void)name;
    return VFS_ENOTDIR;
}

static int pipe_ro_remove(struct vfs_fs *fs, unsigned int dir_ino,
                          const char *name, unsigned int *out_child) {
    (void)fs; (void)dir_ino; (void)name; (void)out_child;
    return VFS_ENOTDIR;
}

static int pipe_ro_mkdir(struct vfs_fs *fs, unsigned int parent_ino,
                         const char *name, unsigned int *out_ino) {
    (void)fs; (void)parent_ino; (void)name; (void)out_ino;
    return VFS_ENOTDIR;
}

static int pipe_ro_rmdir(struct vfs_fs *fs, unsigned int parent_ino,
                         const char *name) {
    (void)fs; (void)parent_ino; (void)name;
    return VFS_ENOTDIR;
}

static int pipe_ro_unlink(struct vfs_fs *fs, unsigned int dir_ino,
                          const char *name) {
    (void)fs; (void)dir_ino; (void)name;
    return VFS_ENOTDIR;
}

static int pipe_ro_truncate(struct vfs_fs *fs, unsigned int ino,
                            unsigned int newsize) {
    (void)fs; (void)ino; (void)newsize;
    return VFS_EINVAL;
}

static int pipe_ro_alloc(struct vfs_fs *fs, unsigned int type) {
    (void)fs; (void)type;
    return 0;
}

struct vfs_fs pipefs_fs = {
    .name = "pipefs",
    .read_at = pipe_read_at,
    .write_at = pipe_write_at,
    .truncate = pipe_ro_truncate,
    .lookup = pipe_ro_lookup,
    .add_dirent = pipe_ro_add,
    .remove_dirent = pipe_ro_remove,
    .mkdir = pipe_ro_mkdir,
    .rmdir = pipe_ro_rmdir,
    .unlink = pipe_ro_unlink,
    .readdir = pipe_ro_readdir,
    .stat = pipe_stat,
    .alloc_inode = pipe_ro_alloc,
    .close = pipe_close,
};
