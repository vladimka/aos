#include "vfs.h"
#include "pipe.h"
#include "sfs2.h"
#include "commands.h"
#include "string.h"
#include "serial.h"
#include "kmm.h"
#include "progload.h"

// ---- embedded payload (generated kernel/progs.c) ----
extern const struct embedded_prog {
    const char *name;
    const unsigned char *data;
    unsigned int size;
} embedded_progs[];

extern const struct embedded_file {
    const char *name;
    const unsigned char *data;
    unsigned int size;
} embedded_data[];

// ---- SFS2 backend ----
static struct sfs2_fs vfs_sfs2;

static int sfs2v_read_at(struct vfs_fs *fs, unsigned int ino, void *buf,
                         unsigned int len, unsigned int off) {
    (void)fs;
    return sfs2_read_at(&vfs_sfs2, ino, buf, len, off);
}

static int sfs2v_write_at(struct vfs_fs *fs, unsigned int ino, const void *buf,
                          unsigned int len, unsigned int off) {
    (void)fs;
    return sfs2_write_at(&vfs_sfs2, ino, buf, len, off);
}

static int sfs2v_truncate(struct vfs_fs *fs, unsigned int ino,
                          unsigned int newsize) {
    (void)fs;
    return sfs2_truncate(&vfs_sfs2, ino, newsize);
}

static int sfs2v_lookup(struct vfs_fs *fs, unsigned int dir_ino,
                        const char *name, unsigned int *out_ino) {
    (void)fs;
    return sfs2_lookup(&vfs_sfs2, dir_ino, name, out_ino);
}

static int sfs2v_add_dirent(struct vfs_fs *fs, unsigned int dir_ino,
                            unsigned int child_ino, const char *name) {
    (void)fs;
    return sfs2_add_dirent(&vfs_sfs2, dir_ino, child_ino, name);
}

static int sfs2v_remove_dirent(struct vfs_fs *fs, unsigned int dir_ino,
                               const char *name, unsigned int *out_child) {
    (void)fs;
    return sfs2_remove_dirent(&vfs_sfs2, dir_ino, name, out_child);
}

static int sfs2v_mkdir(struct vfs_fs *fs, unsigned int parent_ino,
                       const char *name, unsigned int *out_ino) {
    (void)fs;
    return sfs2_mkdir(&vfs_sfs2, parent_ino, name, out_ino);
}

static int sfs2v_rmdir(struct vfs_fs *fs, unsigned int parent_ino,
                       const char *name) {
    (void)fs;
    return sfs2_rmdir(&vfs_sfs2, parent_ino, name);
}

static int sfs2v_unlink(struct vfs_fs *fs, unsigned int dir_ino,
                        const char *name) {
    (void)fs;
    return sfs2_unlink(&vfs_sfs2, dir_ino, name);
}

static int sfs2v_readdir(struct vfs_fs *fs, unsigned int dir_ino,
                         unsigned int idx, char *name_out,
                         unsigned int *ino_out) {
    (void)fs;
    return sfs2_readdir(&vfs_sfs2, dir_ino, idx, name_out, ino_out);
}

static int sfs2v_stat(struct vfs_fs *fs, unsigned int ino, struct aos_stat *st) {
    (void)fs;
    struct sfs2_inode *in = sfs2_get_inode(&vfs_sfs2, ino);
    if (!in || in->type == SFS2_TYPE_FREE) return -1;
    st->type = in->type;
    st->size = in->size;
    st->mtime = in->mtime;
    st->nlink = in->nlink;
    return 0;
}

static int sfs2v_alloc_inode(struct vfs_fs *fs, unsigned int type) {
    (void)fs;
    return sfs2_alloc_inode(&vfs_sfs2, type);
}

static struct vfs_fs sfs2_vfs_fs = {
    .name = "sfs2",
    .read_at = sfs2v_read_at,
    .write_at = sfs2v_write_at,
    .truncate = sfs2v_truncate,
    .lookup = sfs2v_lookup,
    .add_dirent = sfs2v_add_dirent,
    .remove_dirent = sfs2v_remove_dirent,
    .mkdir = sfs2v_mkdir,
    .rmdir = sfs2v_rmdir,
    .unlink = sfs2v_unlink,
    .readdir = sfs2v_readdir,
    .stat = sfs2v_stat,
    .alloc_inode = sfs2v_alloc_inode,
};

// ---- mount table ----
struct vfs_mount {
    const char *prefix;
    struct vfs_fs *fs;
    struct vfs_inode *root;     // held cache entry
    unsigned int root_ino;
};

static struct vfs_mount mounts[VFS_MOUNTS];
static unsigned int n_mounts;

// ---- inode cache ----
static struct vfs_inode cache[VFS_CACHE];
static unsigned int cache_lru;

static struct vfs_inode *cache_find(struct vfs_fs *fs, unsigned int ino) {
    for (unsigned int i = 0; i < VFS_CACHE; i++)
        if (cache[i].valid && cache[i].fs == fs && cache[i].ino == ino)
            return &cache[i];
    return 0;
}

static void cache_entry_release(struct vfs_inode *in) {
    in->valid = 0;
    in->refcount = 0;
    if (in->parent && in->parent != in) {
        struct vfs_inode *p = in->parent;
        in->parent = 0;
        vfs_put(p);
    }
}

static struct vfs_inode *cache_alloc(struct vfs_fs *fs, unsigned int ino) {
    struct vfs_inode *evict = 0;
    unsigned int oldest = 0;
    for (unsigned int i = 0; i < VFS_CACHE; i++) {
        if (!cache[i].valid) {
            evict = &cache[i];
            break;
        }
        if (cache[i].refcount == 0 &&
            (!evict || cache[i].last_used < oldest)) {
            evict = &cache[i];
            oldest = cache[i].last_used;
        }
    }
    if (!evict) return 0;
    if (evict->valid) cache_entry_release(evict);
    memset(evict, 0, sizeof(struct vfs_inode));
    evict->ino = ino;
    evict->fs = fs;
    evict->valid = 1;
    evict->refcount = 1;
    evict->last_used = ++cache_lru;
    return evict;
}

// Make `child` hold a reference on `par` via its parent link, releasing any
// previous parent. `par` must be referenced by the caller; the child's claim
// keeps it alive until the child is evicted.
static void inode_set_parent(struct vfs_inode *child, struct vfs_inode *par) {
    if (child->parent) {
        struct vfs_inode *old = child->parent;
        child->parent = 0;
        vfs_put(old);
    }
    if (par) {
        vfs_get(par->fs, par->ino);   // child's reference
        child->parent = par;
    }
}

struct vfs_inode *vfs_get(struct vfs_fs *fs, unsigned int ino) {
    struct vfs_inode *in = cache_find(fs, ino);
    if (in) {
        in->refcount++;
        in->last_used = ++cache_lru;
        return in;
    }
    struct aos_stat st;
    if (fs->stat(fs, ino, &st) < 0) return 0;
    in = cache_alloc(fs, ino);
    if (!in) return 0;
    in->type = st.type;
    in->nlink = st.nlink;
    in->size = st.size;
    in->mtime = st.mtime;
    return in;
}

void vfs_put(struct vfs_inode *in) {
    if (!in) return;
    if (in->refcount > 0) in->refcount--;
    if (in->refcount == 0 && in->valid)
        cache_entry_release(in);
}

// ---- mount helpers ----
static void mount_setup(struct vfs_mount *m) {
    struct vfs_inode *root = vfs_get(m->fs, m->root_ino);
    if (!root) {
        serial_print("VFS: mount failed: ");
        serial_print(m->prefix);
        serial_print("\n");
        return;
    }
    root->parent = 0;      // mount roots terminate ".."
    m->root = root;
}

static struct vfs_mount *mount_for_path(const char *path) {
    struct vfs_mount *best = 0;
    unsigned int best_len = 0;
    for (unsigned int i = 0; i < n_mounts; i++) {
        const char *pre = mounts[i].prefix;
        unsigned int plen = 0;
        while (pre[plen]) plen++;
        if (plen == 1 && pre[0] == '/') {
            if (!best) {
                best = &mounts[i];
                best_len = 1;
            }
            continue;
        }
        if (strncmp(path, pre, plen) != 0) continue;
        if (path[plen] != '\0' && path[plen] != '/') continue;
        if (plen > best_len) {
            best = &mounts[i];
            best_len = plen;
        }
    }
    return best;
}

// Split "/prefix" into the parent directory and final name. Handles
// relative and absolute paths; strips trailing slashes. dirbuf may be
// empty (target directly in cwd). Returns 0 on success, -1 on bad name.
static int path_split(const char *path, char *dirbuf, unsigned int dirsz,
                      char *name, unsigned int namesz) {
    unsigned int len = 0;
    while (path[len]) len++;
    while (len > 1 && path[len - 1] == '/') len--;   // strip trailing slashes
    if (len == 0) return -1;
    unsigned int last_slash = 0;                       // index of last '/'
    unsigned int name_begin = 0;                       // char after last '/'
    for (unsigned int i = 0; i < len; i++)
        if (path[i] == '/') { last_slash = i; name_begin = i + 1; }
    unsigned int namelen = len - name_begin;
    if (namelen == 0 || namelen > namesz - 1) return -1;
    unsigned int dirlen = last_slash;
    if (dirlen >= dirsz) return -1;
    for (unsigned int i = 0; i < dirlen; i++)
        dirbuf[i] = path[i];
    dirbuf[dirlen] = '\0';
    for (unsigned int i = 0; i < namelen; i++)
        name[i] = path[name_begin + i];
    name[namelen] = '\0';
    return 0;
}

// ---- path resolution ----
struct vfs_inode *vfs_resolve(struct vfs_inode *cwd, const char *path,
                              int flags) {
    struct vfs_inode *cur;
    const char *p = path;

    if (*p == '/') {
        struct vfs_mount *m = mount_for_path(path);
        if (!m || !m->root) return 0;
        cur = vfs_get(m->fs, m->root_ino);
        if (!cur) return 0;
        const char *pre = m->prefix;
        unsigned int plen = 0;
        while (pre[plen]) plen++;
        if (plen > 1) p += plen;             // skip "/proc"
        while (*p == '/') p++;
    } else {
        if (!cwd) return 0;
        cur = vfs_get(cwd->fs, cwd->ino);
        if (!cur) return 0;
    }

    for (;;) {
        while (*p == '/') p++;
        const char *comp = p;
        unsigned int clen = 0;
        while (*p && *p != '/') {
            clen++;
            p++;
        }
        while (*p == '/') p++;
        int is_last = (*p == '\0');

        if (clen == 0) return cur;           // empty path / trailing slash
        if (clen == 1 && comp[0] == '.') continue;
        if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
            if (cur->parent && cur->parent != cur) {
                struct vfs_inode *par = vfs_get(cur->parent->fs, cur->parent->ino);
                if (par) {
                    vfs_put(cur);
                    cur = par;
                }
            }
            continue;
        }

        if (clen > VFS_NAME_MAX) {
            vfs_put(cur);
            return 0;
        }
        char compbuf[VFS_NAME_MAX + 1];
        for (unsigned int i = 0; i < clen; i++)
            compbuf[i] = comp[i];
        compbuf[clen] = '\0';

        unsigned int child_ino;
        int r = cur->fs->lookup(cur->fs, cur->ino, compbuf, &child_ino);
        if (r == 0) {
            struct vfs_inode *child = vfs_get(cur->fs, child_ino);
            if (!child) {
                vfs_put(cur);
                return 0;
            }
            inode_set_parent(child, cur);
            vfs_put(cur);
            cur = child;
            continue;
        }

        if (!(flags & VFS_O_CREAT)) {
            vfs_put(cur);
            return 0;
        }

        if (is_last && (flags & VFS_O_CREAT_DIR)) {
            unsigned int new_ino;
            if (cur->fs->mkdir(cur->fs, cur->ino, compbuf, &new_ino) < 0) {
                vfs_put(cur);
                return 0;
            }
            struct vfs_inode *child = vfs_get(cur->fs, new_ino);
            if (!child) {
                vfs_put(cur);
                return 0;
            }
            inode_set_parent(child, cur);
            vfs_put(cur);
            return child;
        }

        if (is_last) {
            int new_ino = cur->fs->alloc_inode(cur->fs, 1);   // file
            if (new_ino <= 0) {
                vfs_put(cur);
                return 0;
            }
            if (cur->fs->add_dirent(cur->fs, cur->ino,
                                    (unsigned int)new_ino, compbuf) < 0) {
                vfs_put(cur);
                return 0;
            }
            struct vfs_inode *child = vfs_get(cur->fs, (unsigned int)new_ino);
            if (!child) {
                vfs_put(cur);
                return 0;
            }
            inode_set_parent(child, cur);
            vfs_put(cur);
            return child;
        }

        // auto-create missing directory (mkdir -p)
        unsigned int new_ino;
        if (cur->fs->mkdir(cur->fs, cur->ino, compbuf, &new_ino) < 0) {
            vfs_put(cur);
            return 0;
        }
        struct vfs_inode *child = vfs_get(cur->fs, new_ino);
        if (!child) {
            vfs_put(cur);
            return 0;
        }
        inode_set_parent(child, cur);
        vfs_put(cur);
        cur = child;
    }
}

struct vfs_inode *vfs_get_root(void) {
    if (n_mounts == 0 || !mounts[0].root) return 0;
    return vfs_get(mounts[0].fs, mounts[0].root_ino);
}

struct vfs_inode *vfs_get_proc(void) {
    for (unsigned int i = 0; i < n_mounts; i++)
        if (strcmp(mounts[i].prefix, "/proc") == 0 && mounts[i].root)
            return vfs_get(mounts[i].fs, mounts[i].root_ino);
    return 0;
}

// ---- open-file table ----
static struct open_file ofiles[VFS_OFILES];

static struct open_file *ofile_get(int fd) {
    if (fd < 0 || fd >= VFS_OFILES) return 0;
    if (!ofiles[fd].inode) return 0;
    return &ofiles[fd];
}

int vfs_open_fd(struct vfs_inode *cwd, const char *path, int flags) {
    int fd = -1;
    for (int i = 3; i < VFS_OFILES; i++) {   // fds 0-2 are reserved for stdio
        if (!ofiles[i].inode) {
            fd = i;
            break;
        }
    }
    if (fd < 0) return VFS_EMFILE;

    struct vfs_inode *in = vfs_resolve(cwd, path, flags);
    if (!in) return VFS_ENOENT;

    if ((flags & VFS_O_DIRECTORY) && in->type != 2) {
        vfs_put(in);
        return VFS_ENOTDIR;
    }
    if (in->type == 2) {
        if (flags & (VFS_O_WRONLY | VFS_O_RDWR | VFS_O_TRUNC)) {
            vfs_put(in);
            return VFS_EISDIR;
        }
    } else if (flags & VFS_O_TRUNC) {
        int r = in->fs->truncate(in->fs, in->ino, 0);
        if (r < 0) {
            vfs_put(in);
            return r;
        }
        in->size = 0;
    }

    ofiles[fd].inode = in;
    ofiles[fd].flags = flags;
    ofiles[fd].pos = 0;
    ofiles[fd].refcount = 1;
    return fd;
}

int vfs_pipe(int *rd, int *wr) {
    int r = -1, w = -1;
    for (int i = 3; i < VFS_OFILES; i++)
        if (!ofiles[i].inode) { r = i; break; }
    if (r < 0) return VFS_EMFILE;
    for (int i = r + 1; i < VFS_OFILES; i++)
        if (!ofiles[i].inode) { w = i; break; }
    if (w < 0) return VFS_EMFILE;
    struct aos_pipe *p = pipe_alloc();
    if (!p) return VFS_EMFILE;
    ofiles[r].inode = &p->inode;
    ofiles[r].flags = VFS_O_RDONLY;
    ofiles[r].pos = 0;
    ofiles[r].refcount = 1;
    ofiles[w].inode = &p->inode;
    ofiles[w].flags = VFS_O_WRONLY;
    ofiles[w].pos = 0;
    ofiles[w].refcount = 1;
    *rd = r;
    *wr = w;
    return 0;
}

int vfs_close_fd(int fd) {
    struct open_file *of = ofile_get(fd);
    if (!of) return VFS_EBADF;
    struct vfs_fs *fs = of->inode->fs;
    unsigned int ino = of->inode->ino;
    int flags = of->flags;
    vfs_put(of->inode);
    if (fs && fs->close)
        fs->close(fs, ino, flags);
    of->inode = 0;
    of->refcount = 0;
    return 0;
}

int vfs_dup_fd(int fd) {
    struct open_file *of = ofile_get(fd);
    if (!of) return VFS_EBADF;
    int flags = of->flags;
    int fd2 = -1;
    for (int i = 3; i < VFS_OFILES; i++) {
        if (!ofiles[i].inode) {
            fd2 = i;
            break;
        }
    }
    if (fd2 < 0) return VFS_EMFILE;
    ofiles[fd2] = *of;
    ofiles[fd2].refcount = 1;
    vfs_get(of->inode->fs, of->inode->ino);   // extra inode ref for the copy
    if (of->inode->fs == &pipefs_fs)
        pipe_dup(of->inode->fs, of->inode->ino, flags);
    return fd2;
}

// Expose the underlying open_file for a given fd (used by the task fd table).
struct open_file *vfs_ofile_ptr(int fd) {
    return ofile_get(fd);
}

int vfs_read_fd(int fd, void *buf, unsigned int len) {
    struct open_file *of = ofile_get(fd);
    if (!of) return VFS_EBADF;
    if (of->flags & VFS_O_WRONLY) return VFS_EBADF;
    if (of->inode->type == 2) return VFS_EISDIR;
    if (of->inode->fs == &pipefs_fs && (of->flags & VFS_O_NONBLOCK))
        return pipe_read_nonblock(of->inode->fs, of->inode->ino, buf, len,
                                  of->pos);
    int n = of->inode->fs->read_at(of->inode->fs, of->inode->ino, buf, len,
                                   of->pos);
    if (n > 0) of->pos += (unsigned int)n;
    return n;
}

int vfs_write_fd(int fd, const void *buf, unsigned int len) {
    struct open_file *of = ofile_get(fd);
    if (!of) return VFS_EBADF;
    if (!(of->flags & (VFS_O_WRONLY | VFS_O_RDWR))) return VFS_EBADF;
    if (of->inode->type == 2) return VFS_EISDIR;
    if (of->flags & VFS_O_APPEND) of->pos = of->inode->size;
    if (of->inode->fs == &pipefs_fs && (of->flags & VFS_O_NONBLOCK))
        return pipe_write_nonblock(of->inode->fs, of->inode->ino, buf, len,
                                   of->pos);
    int n = of->inode->fs->write_at(of->inode->fs, of->inode->ino, buf, len,
                                    of->pos);
    if (n > 0) {
        of->pos += (unsigned int)n;
        if (of->pos > of->inode->size) of->inode->size = of->pos;
    }
    return n;
}

int vfs_lseek_fd(int fd, int off, int whence) {
    struct open_file *of = ofile_get(fd);
    if (!of) return VFS_EBADF;
    unsigned int base;
    if (whence == VFS_SEEK_SET) base = 0;
    else if (whence == VFS_SEEK_CUR) base = of->pos;
    else if (whence == VFS_SEEK_END) base = of->inode->size;
    else return VFS_EINVAL;
    if (off < 0 && (unsigned int)(-off) > base) return VFS_EINVAL;
    of->pos = base + (unsigned int)off;
    return (int)of->pos;
}

int vfs_readdir_fd(int fd, char *name, unsigned int name_len) {
    struct open_file *of = ofile_get(fd);
    if (!of) return VFS_EBADF;
    if (of->inode->type != 2) return VFS_ENOTDIR;
    char nbuf[VFS_NAME_MAX + 1];
    for (;;) {
        unsigned int ino;
        int r = of->inode->fs->readdir(of->inode->fs, of->inode->ino,
                                       of->pos, nbuf, &ino);
        if (r <= 0) return 0;
        of->pos++;
        if (ino == 0) continue;
        unsigned int i = 0;
        while (nbuf[i] && i + 1 < name_len) {
            name[i] = nbuf[i];
            i++;
        }
        name[i] = '\0';
        return 1;
    }
}

int vfs_fstat_fd(int fd, struct aos_stat *st) {
    struct open_file *of = ofile_get(fd);
    if (!of) return VFS_EBADF;
    return of->inode->fs->stat(of->inode->fs, of->inode->ino, st);
}

int vfs_stat(struct vfs_inode *cwd, const char *path, struct aos_stat *st) {
    struct vfs_inode *in = vfs_resolve(cwd, path, 0);
    if (!in) return VFS_ENOENT;
    int r = in->fs->stat(in->fs, in->ino, st);
    vfs_put(in);
    return r;
}

int vfs_unlink(struct vfs_inode *cwd, const char *path) {
    char dirbuf[PATH_MAX + 1];
    char name[VFS_NAME_MAX + 1];
    if (path_split(path, dirbuf, sizeof(dirbuf), name, sizeof(name)) < 0)
        return VFS_EINVAL;
    struct vfs_inode *dir = vfs_resolve(cwd, dirbuf, 0);
    if (!dir) return VFS_ENOENT;
    if (dir->type != 2) {
        vfs_put(dir);
        return VFS_ENOTDIR;
    }
    struct vfs_inode *target = vfs_resolve(cwd, path, 0);
    if (!target) {
        vfs_put(dir);
        return VFS_ENOENT;
    }
    if (target->refcount > 1) {
        vfs_put(target);
        vfs_put(dir);
        return VFS_EBUSY;
    }
    if (target->type == 2) {
        vfs_put(target);
        vfs_put(dir);
        return VFS_EISDIR;
    }
    vfs_put(target);
    int r = dir->fs->unlink(dir->fs, dir->ino, name);
    vfs_put(dir);
    return r < 0 ? r : 0;
}

int vfs_mkdir(struct vfs_inode *cwd, const char *path) {
    char dirbuf[PATH_MAX + 1];
    char name[VFS_NAME_MAX + 1];
    if (path_split(path, dirbuf, sizeof(dirbuf), name, sizeof(name)) < 0)
        return VFS_EINVAL;
    // Resolve the parent with O_CREAT|O_CREAT_DIR so missing intermediate
    // directories are auto-created (mkdir -p).
    struct vfs_inode *dir = vfs_resolve(cwd, dirbuf,
                                        VFS_O_CREAT | VFS_O_CREAT_DIR);
    if (!dir) return VFS_ENOENT;
    if (dir->type != 2) {
        vfs_put(dir);
        return VFS_ENOTDIR;
    }
    unsigned int new_ino;
    int r = dir->fs->mkdir(dir->fs, dir->ino, name, &new_ino);
    vfs_put(dir);
    return r < 0 ? r : 0;
}

int vfs_rmdir(struct vfs_inode *cwd, const char *path) {
    char dirbuf[PATH_MAX + 1];
    char name[VFS_NAME_MAX + 1];
    if (path_split(path, dirbuf, sizeof(dirbuf), name, sizeof(name)) < 0)
        return VFS_EINVAL;
    struct vfs_inode *dir = vfs_resolve(cwd, dirbuf, 0);
    if (!dir) return VFS_ENOENT;
    if (dir->type != 2) {
        vfs_put(dir);
        return VFS_ENOTDIR;
    }
    struct vfs_inode *target = vfs_resolve(cwd, path, 0);
    if (!target) {
        vfs_put(dir);
        return VFS_ENOENT;
    }
    if (target->refcount > 1) {
        vfs_put(target);
        vfs_put(dir);
        return VFS_EBUSY;
    }
    if (target->type != 2) {
        vfs_put(target);
        vfs_put(dir);
        return VFS_ENOTDIR;
    }
    vfs_put(target);
    int r = dir->fs->rmdir(dir->fs, dir->ino, name);
    vfs_put(dir);
    return r < 0 ? r : 0;
}

// ---- current working directory (global until Task 5) ----
struct vfs_inode *kernel_cwd;

int vfs_chdir(struct vfs_inode *cwd, const char *path) {
    struct vfs_inode *in = vfs_resolve(cwd, path, 0);
    if (!in) return VFS_ENOENT;
    if (in->type != 2) {
        vfs_put(in);
        return VFS_ENOTDIR;
    }
    if (kernel_cwd) vfs_put(kernel_cwd);
    kernel_cwd = in;                  // keeps the reference
    return 0;
}

// Find the name of `child` inside directory `dir` (for getcwd path building).
static int inode_name(struct vfs_inode *dir, struct vfs_inode *child,
                      char *out, unsigned int outsz) {
    unsigned int idx = 0;
    char nbuf[VFS_NAME_MAX + 1];
    for (;;) {
        unsigned int ino;
        int r = dir->fs->readdir(dir->fs, dir->ino, idx, nbuf, &ino);
        if (r <= 0) return VFS_ENOENT;
        idx++;
        if (ino == 0 || ino != child->ino) continue;
        unsigned int i = 0;
        while (nbuf[i] && i + 1 < outsz) {
            out[i] = nbuf[i];
            i++;
        }
        out[i] = '\0';
        return 0;
    }
}

static const char *mount_prefix(struct vfs_inode *in) {
    for (unsigned int i = 0; i < n_mounts; i++)
        if (mounts[i].fs == in->fs && mounts[i].root_ino == in->ino)
            return mounts[i].prefix;
    return 0;
}

// Absolute path of the current directory (walks parent links up to the mount
// root, then prepends the mount prefix).
int vfs_getcwd(char *buf, unsigned int len) {
    if (!kernel_cwd) return VFS_ENOENT;
    struct vfs_inode *cur = kernel_cwd;
    char segs[16][VFS_NAME_MAX + 1];
    int nseg = 0;
    while (cur->parent && cur->parent != cur && nseg < 16) {
        if (inode_name(cur->parent, cur, segs[nseg], sizeof(segs[nseg])) < 0)
            break;
        nseg++;
        cur = cur->parent;
    }
    const char *pre = mount_prefix(cur);
    if (!pre) pre = "/";

    unsigned int o = 0;
    for (const char *p = pre; *p && o + 1 < len; p++) {
        buf[o++] = *p;
    }
    if (o > 0 && buf[o - 1] != '/' && o + 1 < len) buf[o++] = '/';
    for (int s = nseg - 1; s >= 0 && o + 1 < len; s--) {
        for (const char *p = segs[s]; *p && o + 1 < len; p++)
            buf[o++] = *p;
        if (s > 0 && o + 1 < len) buf[o++] = '/';
    }
    buf[o] = '\0';
    return 0;
}

// ---- kernel read/write helpers (paths relative to /) ----
int vfs_kernel_read(const char *path, void *buf, unsigned int len,
                    unsigned int off) {
    struct vfs_inode *root = vfs_get_root();
    if (!root) return VFS_ENOENT;
    struct vfs_inode *in = vfs_resolve(root, path, 0);
    vfs_put(root);
    if (!in) return VFS_ENOENT;
    if (in->type == 2) {
        vfs_put(in);
        return VFS_EISDIR;
    }
    int n = in->fs->read_at(in->fs, in->ino, buf, len, off);
    vfs_put(in);
    return n;
}

int vfs_kernel_write(const char *path, const void *buf, unsigned int len,
                     unsigned int off) {
    struct vfs_inode *root = vfs_get_root();
    if (!root) return VFS_ENOENT;
    struct vfs_inode *in = vfs_resolve(root, path, VFS_O_CREAT);
    vfs_put(root);
    if (!in) return VFS_ENOENT;
    if (in->type == 2) {
        vfs_put(in);
        return VFS_EISDIR;
    }
    int n = in->fs->write_at(in->fs, in->ino, buf, len, off);
    if (n > 0 && in->size < off + (unsigned int)n) in->size = off + (unsigned int)n;
    vfs_put(in);
    return n;
}

int vfs_kernel_stat(const char *path, struct aos_stat *st) {
    struct vfs_inode *root = vfs_get_root();
    if (!root) return VFS_ENOENT;
    int r = vfs_stat(root, path, st);
    vfs_put(root);
    return r;
}

// ---- init / format ----
static void cache_clear(void) {
    for (unsigned int i = 0; i < VFS_CACHE; i++) {
        if (cache[i].valid) cache_entry_release(&cache[i]);
    }
    for (unsigned int i = 0; i < n_mounts; i++)
        mounts[i].root = 0;
}

void vfs_format(void) {
    sfs2_format(&vfs_sfs2);
    if (kernel_cwd) {
        vfs_put(kernel_cwd);
        kernel_cwd = 0;
    }
    cache_clear();
    for (unsigned int i = 0; i < n_mounts; i++)
        mount_setup(&mounts[i]);
    kernel_cwd = vfs_get_root();
    load_embedded_programs();
    load_embedded_data();
}

int vfs_sync(void) {
    return sfs2_flush(&vfs_sfs2);
}

void vfs_init(void) {
    memset(cache, 0, sizeof(cache));

    memset(&vfs_sfs2, 0, sizeof(vfs_sfs2));
    if (sfs2_init(&vfs_sfs2) != 0) {
        serial_print("VFS: sfs2 init failed\n");
    }
    n_mounts = 0;
    mounts[n_mounts].prefix = "/";
    mounts[n_mounts].fs = &sfs2_vfs_fs;
    mounts[n_mounts].root_ino = 1;
    n_mounts++;
    mount_setup(&mounts[0]);
    serial_print("VFS: / [sfs2] root=1\n");

    mounts[n_mounts].prefix = "/proc";
    mounts[n_mounts].fs = &procfs_fs;
    mounts[n_mounts].root_ino = 1;
    n_mounts++;
    mount_setup(&mounts[1]);
    serial_print("VFS: /proc [procfs]\n");

    kernel_cwd = vfs_get_root();
    load_embedded_programs();
    load_embedded_data();
}
