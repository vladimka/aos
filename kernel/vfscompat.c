// Flat SFS1-compatible API, implemented over the VFS layer. Kept for the
// AOS flat FS syscalls (SYS_FS_*) and the Linux fd emulation until they are
// migrated to the fd-based API (Task 4/5). All names are resolved relative
// to the VFS root, so "bin/help" and "/bin/help" are equivalent.
#include "fs.h"
#include "vfs.h"
#include "string.h"

void fs_init(void) {}                  // VFS init happens in vfs_init()

void fs_format(void) {
    vfs_format();
}

int fs_exists(const char *name) {
    struct aos_stat st;
    return vfs_kernel_stat(name, &st) == 0;
}

int fs_get_size(const char *name) {
    struct aos_stat st;
    if (vfs_kernel_stat(name, &st) < 0) return -1;
    return (int)st.size;
}

int fs_create(const char *name) {
    struct vfs_inode *root = vfs_get_root();
    if (!root) return -1;
    struct aos_stat st;
    if (vfs_stat(root, name, &st) == 0) {
        vfs_put(root);
        return -1;                    // already exists
    }
    struct vfs_inode *in = vfs_resolve(root, name, VFS_O_CREAT);
    vfs_put(root);
    return in ? 0 : -2;
}

int fs_delete(const char *name) {
    struct vfs_inode *root = vfs_get_root();
    if (!root) return -1;
    int r = vfs_unlink(root, name);
    vfs_put(root);
    return r < 0 ? r : 0;
}

int fs_read_at(const char *name, char *buf, unsigned int size,
               unsigned int offset) {
    struct vfs_inode *root = vfs_get_root();
    if (!root) return -1;
    struct vfs_inode *in = vfs_resolve(root, name, 0);
    vfs_put(root);
    if (!in) return -1;
    if (in->type != 1) {
        vfs_put(in);
        return -1;
    }
    int n = in->fs->read_at(in->fs, in->ino, buf, size, offset);
    vfs_put(in);
    return n;
}

int fs_read(const char *name, char *buf, unsigned int size) {
    return fs_read_at(name, buf, size, 0);
}

int fs_write(const char *name, const char *data, unsigned int size) {
    struct vfs_inode *root = vfs_get_root();
    if (!root) return -1;
    struct vfs_inode *in = vfs_resolve(root, name, VFS_O_CREAT);
    vfs_put(root);
    if (!in) return -2;
    if (in->type != 1) {
        vfs_put(in);
        return -1;
    }
    int r = in->fs->truncate(in->fs, in->ino, 0);
    if (r < 0) {
        vfs_put(in);
        return r;
    }
    in->size = 0;
    int n = in->fs->write_at(in->fs, in->ino, data, size, 0);
    if (n > 0) in->size = (unsigned int)n;
    vfs_put(in);
    return n;
}

// Enumerate root entries. idx is a count of entries already returned.
int sfs_get_entry(unsigned int idx, char *name_buf, unsigned int *size_out) {
    struct vfs_inode *root = vfs_get_root();
    if (!root) return -1;
    char nbuf[VFS_NAME_MAX + 1];
    unsigned int pos = 0;
    unsigned int seen = 0;
    for (;;) {
        unsigned int ino;
        int r = root->fs->readdir(root->fs, root->ino, pos, nbuf, &ino);
        if (r <= 0) {
            vfs_put(root);
            return -1;
        }
        pos++;
        if (ino == 0) continue;
        if (seen != idx) {
            seen++;
            continue;
        }
        strncpy(name_buf, nbuf, 27);
        name_buf[27] = '\0';
        struct aos_stat st;
        if (root->fs->stat(root->fs, ino, &st) == 0) {
            *size_out = st.size;
            // SFS1 convention: directories carry a trailing '/' so flat-API
            // consumers (wm.c folder/hide checks, getdents64 DT_DIR) work.
            if (st.type == 2) {
                unsigned int l = 0;
                while (l < 27 && name_buf[l]) l++;
                if (l < 27) {
                    name_buf[l] = '/';
                    name_buf[l + 1] = '\0';
                }
            }
        } else {
            *size_out = 0;
        }
        vfs_put(root);
        return 0;
    }
}
