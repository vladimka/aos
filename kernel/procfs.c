#include "vfs.h"
#include "string.h"
#include "serial.h"
#include "klog.h"
#include "task.h"
#include "trace.h"

// procfs inode numbers
#define PROCFS_ROOT    1
#define PROCFS_UPTIME  2
#define PROCFS_VERSION 3
#define PROCFS_MOUNTS  4
#define PROCFS_KLOG    5

// Pseudo-dirs per live task and their trace files. The pid range for the
// 0x1000 dirs is limited to MAX_TASKS, so decimal task pids never collide.
#define PROCFS_PID_BASE   0x1000
#define PROCFS_TRACE_BASE 0x2000
#define PROCFS_CMDLINE_BASE 0x3000
#define PROCFS_STATUS_BASE 0x4000
#define PROCFS_PID_DIR(pid)   (PROCFS_PID_BASE + (pid))
#define PROCFS_TRACE(pid)     (PROCFS_TRACE_BASE + (pid))
#define PROCFS_CMDLINE(pid)   (PROCFS_CMDLINE_BASE + (pid))
#define PROCFS_STATUS(pid)    (PROCFS_STATUS_BASE + (pid))

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

static const char *proc_state_str(unsigned int state) {
    switch (state) {
    case TASK_READY:    return "ready";
    case TASK_RUNNING:  return "run";
    case TASK_SLEEPING: return "sleep";
    case TASK_WAITING:  return "wait";
    case TASK_ZOMBIE:   return "zomb";
    case TASK_SPAWNING: return "spawn";
    default:            return "free";
    }
}

// Render /proc/<pid>/cmdline ("<name>[ <args>]") into buf. Returns length.
static unsigned int proc_render_cmdline(unsigned int pid, char *buf,
                                        unsigned int cap) {
    struct task *t = pid < MAX_TASKS ? task_slot(pid) : 0;
    if (!t || t->state == TASK_FREE) return 0;
    unsigned int i = 0;
    for (; t->name[i] && i < cap - 1; i++) buf[i] = t->name[i];
    if (t->args && t->args[0] && i < cap - 1) {
        buf[i++] = ' ';
        for (unsigned int j = 0; t->args[j] && i < cap - 1; j++)
            buf[i++] = t->args[j];
    }
    buf[i] = '\0';
    return i;
}

// Render /proc/<pid>/status as "State:/PPid:/Abi:" lines. Returns length.
static unsigned int proc_render_status(unsigned int pid, char *buf,
                                       unsigned int cap) {
    struct task *t = pid < MAX_TASKS ? task_slot(pid) : 0;
    if (!t || t->state == TASK_FREE) return 0;
    static const char *abis[] = { "aos", "linux" };
    unsigned int abi = (t->abi == ABI_LINUX) ? 1u : 0u;
    const char *state = proc_state_str(t->state);
    char num[12];
    u32_str(t->parent, num);
    unsigned int i = 0;
    const char *parts[] = { "State:\t", state, "\nPPid:\t", num,
                            "\nAbi:\t", abis[abi], "\n" };
    for (unsigned int k = 0; k < sizeof(parts) / sizeof(parts[0]); k++)
        for (unsigned int j = 0; parts[k][j] && i < cap - 1; j++)
            buf[i++] = parts[k][j];
    buf[i] = '\0';
    return i;
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
    if (ino >= PROCFS_PID_BASE && ino < PROCFS_PID_BASE + MAX_TASKS) {
        st->type = 2;
        st->size = 0;
        st->mtime = 0;
        st->nlink = 1;
        return 0;
    }
    if (ino >= PROCFS_TRACE_BASE && ino < PROCFS_TRACE_BASE + MAX_TASKS) {
        unsigned int sz = 0;
        trace_render_at(ino - PROCFS_TRACE_BASE, 0, 0, 0, &sz);
        st->type = 1;
        st->size = sz;
        st->mtime = 0;
        st->nlink = 1;
        return 0;
    }
    if (ino >= PROCFS_CMDLINE_BASE && ino < PROCFS_CMDLINE_BASE + MAX_TASKS) {
        static char cbuf[384];
        st->type = 1;
        st->size = proc_render_cmdline(ino - PROCFS_CMDLINE_BASE,
                                       cbuf, sizeof(cbuf));
        st->mtime = 0;
        st->nlink = 1;
        return 0;
    }
    if (ino >= PROCFS_STATUS_BASE && ino < PROCFS_STATUS_BASE + MAX_TASKS) {
        static char sbuf[128];
        st->type = 1;
        st->size = proc_render_status(ino - PROCFS_STATUS_BASE,
                                      sbuf, sizeof(sbuf));
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
    if (dir_ino != PROCFS_ROOT) {
        if (dir_ino >= PROCFS_PID_BASE && dir_ino < PROCFS_PID_BASE + MAX_TASKS) {
            if (strcmp(name, "trace") == 0) {
                *out_ino = PROCFS_TRACE(dir_ino - PROCFS_PID_BASE);
                return 0;
            }
            if (strcmp(name, "cmdline") == 0) {
                *out_ino = PROCFS_CMDLINE(dir_ino - PROCFS_PID_BASE);
                return 0;
            }
            if (strcmp(name, "status") == 0) {
                *out_ino = PROCFS_STATUS(dir_ino - PROCFS_PID_BASE);
                return 0;
            }
            return VFS_ENOENT;
        }
        return VFS_ENOTDIR;
    }
    for (unsigned int i = 0; i < PROC_FILES; i++) {
        if (strcmp(proc_files[i].name, name) == 0) {
            *out_ino = proc_files[i].ino;
            return 0;
        }
    }
    unsigned int pid = 0;
    const char *p = name;
    while (*p >= '0' && *p <= '9') {
        pid = pid * 10 + (unsigned int)(*p - '0');
        p++;
    }
    if (*p == '\0' && pid > 0 && pid < MAX_TASKS) {
        struct task *t = task_slot(pid);
        if (t && t->state != TASK_FREE) {
            *out_ino = PROCFS_PID_DIR(pid);
            return 0;
        }
    }
    return VFS_ENOENT;
}

static int proc_readdir(struct vfs_fs *fs, unsigned int dir_ino,
                        unsigned int idx, char *name_out,
                        unsigned int *ino_out) {
    (void)fs;
    if (dir_ino == PROCFS_ROOT) {
        if (idx < PROC_FILES) {
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
        unsigned int k = idx - PROC_FILES;
        for (unsigned int pid = 1; pid < MAX_TASKS; pid++) {
            struct task *t = task_slot(pid);
            if (!t || t->state == TASK_FREE || t->state == TASK_ZOMBIE) continue;
            if (k-- != 0) continue;
            char tmp[12];
            unsigned int n = 0;
            unsigned int v = pid;
            while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
            unsigned int i = 0;
            for (unsigned int j = n; j > 0 && i < VFS_NAME_MAX;) {
                j--;
                name_out[i++] = tmp[j];
            }
            name_out[i] = '\0';
            if (ino_out) *ino_out = PROCFS_PID_DIR(pid);
            return 1;
        }
        return 0;
    }
    if (dir_ino >= PROCFS_PID_BASE && dir_ino < PROCFS_PID_BASE + MAX_TASKS) {
        static const char *const pid_files[] = { "trace", "cmdline", "status" };
        if (idx < sizeof(pid_files) / sizeof(pid_files[0])) {
            unsigned int i = 0;
            const char *nm = pid_files[idx];
            while (nm[i] && i < VFS_NAME_MAX) { name_out[i] = nm[i]; i++; }
            name_out[i] = '\0';
            unsigned int base = (idx == 0) ? PROCFS_TRACE_BASE
                              : (idx == 1) ? PROCFS_CMDLINE_BASE
                                           : PROCFS_STATUS_BASE;
            if (ino_out) *ino_out = base + (dir_ino - PROCFS_PID_BASE);
            return 1;
        }
        return 0;
    }
    return VFS_ENOTDIR;
}

static int proc_read_at(struct vfs_fs *fs, unsigned int ino, void *buf,
                        unsigned int len, unsigned int off) {
    (void)fs;
    static char rbuf[384];
    if (ino == PROCFS_KLOG)
        return (int)klog_read(off, buf, len);
    if (ino >= PROCFS_TRACE_BASE && ino < PROCFS_TRACE_BASE + MAX_TASKS)
        return (int)trace_render_at(ino - PROCFS_TRACE_BASE, off, buf, len, 0);
    unsigned int clen;
    if (ino >= PROCFS_CMDLINE_BASE && ino < PROCFS_CMDLINE_BASE + MAX_TASKS)
        clen = proc_render_cmdline(ino - PROCFS_CMDLINE_BASE,
                                   rbuf, sizeof(rbuf));
    else if (ino >= PROCFS_STATUS_BASE && ino < PROCFS_STATUS_BASE + MAX_TASKS)
        clen = proc_render_status(ino - PROCFS_STATUS_BASE,
                                  rbuf, sizeof(rbuf));
    else {
        char *content = proc_content(ino, &clen);
        if (off >= clen) return 0;
        if (len > clen - off) len = clen - off;
        for (unsigned int i = 0; i < len; i++)
            ((char *)buf)[i] = content[off + i];
        return (int)len;
    }
    if (off >= clen) return 0;
    if (len > clen - off) len = clen - off;
    for (unsigned int i = 0; i < len; i++)
        ((char *)buf)[i] = rbuf[off + i];
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
