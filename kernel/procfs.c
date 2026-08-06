#include "vfs.h"
#include "string.h"
#include "serial.h"
#include "klog.h"

// procfs inode numbers
#define PROCFS_ROOT    1
#define PROCFS_UPTIME  2
#define PROCFS_VERSION 3
#define PROCFS_MOUNTS  4
#define PROCFS_KLOG    5

extern volatile unsigned int tick;

struct proc_file {
    const char *name;
    unsigned int ino;
};

static const struct proc_file proc_files[] = {
    { "uptime", PROCFS_UPTIME },
    { "version", PROCFS_VERSION },
    { "mounts", PROCFS_MOUNTS },
    { "klog", PROCFS_KLOG },
};

#define PROC_FILES (sizeof(proc_files) / sizeof(proc_files[0]))

static void u32_str(unsigned int v, char *buf) {
    char tmp[12];
    int i = 0;
    if (v == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while (v > 0) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (int j = 0; j < i; j++)
        buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

// Build content for a procfs inode into dst (static buffer, NUL-terminated).
static char *proc_content(unsigned int ino, unsigned int *len_out) {
    static char buf[128];
    if (ino == PROCFS_UPTIME) {
        char n[12];
        u32_str(tick, n);
        buf[0] = 0;
        unsigned int i = 0;
        const char *h = "uptime ";
        while (h[i]) { buf[i] = h[i]; i++; }
        unsigned int j = 0;
        while (n[j]) { buf[i++] = n[j]; j++; }
        const char *t = " ticks\n";
        j = 0;
        while (t[j]) { buf[i++] = t[j]; j++; }
        buf[i] = '\0';
        *len_out = i;
    } else if (ino == PROCFS_VERSION) {
        const char *v = "AOS 0.1 i386\n";
        unsigned int i = 0;
        while (v[i]) { buf[i] = v[i]; i++; }
        buf[i] = '\0';
        *len_out = i;
    } else if (ino == PROCFS_MOUNTS) {
        const char *v = "rootfs / sfs2\nproc /proc procfs\n";
        unsigned int i = 0;
        while (v[i]) { buf[i] = v[i]; i++; }
        buf[i] = '\0';
        *len_out = i;
    } else {
        buf[0] = '\0';
        *len_out = 0;
    }
    return buf;
}

static int proc_stat(struct vfs_fs *fs, unsigned int ino, struct aos_stat *st) {
    (void)fs;
    if (ino == PROCFS_ROOT) {
        st->type = 2;
        st->size = 0;
        st->mtime = 0;
        st->nlink = 1;
        return 0;
    }
    if (ino == PROCFS_KLOG) {
        st->type = 1;
        st->size = klog_size();
        st->mtime = 0;
        st->nlink = 1;
        return 0;
    }
    for (unsigned int i = 0; i < PROC_FILES; i++) {
        if (proc_files[i].ino == ino) {
            unsigned int len;
            proc_content(ino, &len);
            st->type = 1;
            st->size = len;
            st->mtime = 0;
            st->nlink = 1;
            return 0;
        }
    }
    return -1;
}

static int proc_lookup(struct vfs_fs *fs, unsigned int dir_ino,
                       const char *name, unsigned int *out_ino) {
    (void)fs;
    if (dir_ino != PROCFS_ROOT) return VFS_ENOTDIR;
    for (unsigned int i = 0; i < PROC_FILES; i++) {
        if (strcmp(proc_files[i].name, name) == 0) {
            *out_ino = proc_files[i].ino;
            return 0;
        }
    }
    return VFS_ENOENT;
}

static int proc_readdir(struct vfs_fs *fs, unsigned int dir_ino,
                        unsigned int idx, char *name_out,
                        unsigned int *ino_out) {
    (void)fs;
    if (dir_ino != PROCFS_ROOT) return VFS_ENOTDIR;
    if (idx >= PROC_FILES) return 0;
    const struct proc_file *pf = &proc_files[idx];
    unsigned int i = 0;
    while (pf->name[i] && i < VFS_NAME_MAX) {
        name_out[i] = pf->name[i];
        i++;
    }
    name_out[i] = '\0';
    if (ino_out) *ino_out = pf->ino;
    return 1;
}

static int proc_read_at(struct vfs_fs *fs, unsigned int ino, void *buf,
                        unsigned int len, unsigned int off) {
    (void)fs;
    if (ino == PROCFS_KLOG)
        return (int)klog_read(off, buf, len);
    unsigned int clen;
    char *content = proc_content(ino, &clen);
    if (off >= clen) return 0;
    if (len > clen - off) len = clen - off;
    for (unsigned int i = 0; i < len; i++)
        ((char *)buf)[i] = content[off + i];
    return (int)len;
}

static int proc_ro_write(struct vfs_fs *fs, unsigned int ino, const void *buf,
                         unsigned int len, unsigned int off) {
    (void)fs; (void)ino; (void)buf; (void)len; (void)off;
    return VFS_EPERM;
}

static int proc_ro_truncate(struct vfs_fs *fs, unsigned int ino,
                            unsigned int newsize) {
    (void)fs; (void)ino; (void)newsize;
    return VFS_EPERM;
}

static int proc_ro_add(struct vfs_fs *fs, unsigned int dir_ino,
                       unsigned int child_ino, const char *name) {
    (void)fs; (void)dir_ino; (void)child_ino; (void)name;
    return VFS_EPERM;
}

static int proc_ro_remove(struct vfs_fs *fs, unsigned int dir_ino,
                          const char *name, unsigned int *out_child) {
    (void)fs; (void)dir_ino; (void)name; (void)out_child;
    return VFS_EPERM;
}

static int proc_ro_rm(struct vfs_fs *fs, unsigned int dir_ino,
                      const char *name) {
    (void)fs; (void)dir_ino; (void)name;
    return VFS_EPERM;
}

static int proc_ro_mkdir(struct vfs_fs *fs, unsigned int parent,
                         const char *name, unsigned int *out) {
    (void)fs; (void)parent; (void)name; (void)out;
    return VFS_EPERM;
}

static int proc_readonly_stat(struct vfs_fs *fs, unsigned int ino,
                              struct aos_stat *st) {
    return proc_stat(fs, ino, st);
}

static int proc_ro_alloc(struct vfs_fs *fs, unsigned int type) {
    (void)fs; (void)type;
    return 0;
}

struct vfs_fs procfs_fs = {
    .name = "procfs",
    .read_at = proc_read_at,
    .write_at = proc_ro_write,
    .truncate = proc_ro_truncate,
    .lookup = proc_lookup,
    .add_dirent = proc_ro_add,
    .remove_dirent = proc_ro_remove,
    .mkdir = proc_ro_mkdir,
    .rmdir = proc_ro_rm,
    .unlink = proc_ro_rm,
    .readdir = proc_readdir,
    .stat = proc_readonly_stat,
    .alloc_inode = proc_ro_alloc,
};
