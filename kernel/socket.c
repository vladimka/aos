#include "socket.h"
#include <stddef.h>
#include "string.h"
#include "kmm.h"
#include "serial.h"
#include "task.h"

/* ------------------------------------------------------------------ *
 *  Pool of AF_UNIX sockets (kernel-static, follows pipefs pattern).
 * ------------------------------------------------------------------ */

static struct aos_sock socks[SOCK_MAX];

static struct aos_sock *stream_owner(struct aos_sock *s) {
    return s->stream ? s->stream : s;
}

static struct aos_sock *find_bound(const char *name) {
    for (int i = 0; i < SOCK_MAX; i++) {
        if (!socks[i].used || socks[i].state != SOCK_LISTENING)
            continue;
        if (strcmp(socks[i].name, name) == 0)
            return &socks[i];
    }
    return 0;
}

struct aos_sock *sock_from_inode(struct vfs_inode *in) {
    if (!in || in->fs != &sockfs_fs)
        return 0;
    for (int i = 0; i < SOCK_MAX; i++)
        if (socks[i].used && &socks[i].inode == in)
            return &socks[i];
    return 0;
}

/* ------------------------------------------------------------------ *
 *  VFS read/write backends.
 * ------------------------------------------------------------------ */

int sock_read_nonblock(struct vfs_fs *fs, unsigned int ino, void *buf,
                       unsigned int len, unsigned int off) {
    (void)fs; (void)off;
    if (ino < 1 || ino > (unsigned int)SOCK_MAX) return -9;
    struct aos_sock *end = &socks[ino - 1];
    struct aos_sock *s = stream_owner(end);
    if (len == 0) return 0;
    if (s->count == 0)
        return (s->peer_eof || !s->used) ? 0 : -11;   /* 0=EOF, -11=EAGAIN */
    unsigned int n = len < s->count ? len : s->count;
    for (unsigned int i = 0; i < n; i++) {
        ((unsigned char *)buf)[i] = s->buf[s->tail];
        s->tail = (s->tail + 1) % SOCK_BUF_SIZE;
    }
    s->count -= n;
    return (int)n;
}

int sock_write_nonblock(struct vfs_fs *fs, unsigned int ino, const void *buf,
                        unsigned int len, unsigned int off) {
    (void)fs; (void)off;
    if (ino < 1 || ino > (unsigned int)SOCK_MAX) return -9;
    struct aos_sock *end = &socks[ino - 1];
    struct aos_sock *s = stream_owner(end);
    if (len == 0) return 0;
    if (!s->used || s->peer_eof == 2) return -32;      /* EPIPE */
    if (s->count == SOCK_BUF_SIZE) return -11;         /* EAGAIN */
    unsigned int space = SOCK_BUF_SIZE - s->count;
    unsigned int n = len < space ? len : space;
    for (unsigned int i = 0; i < n; i++) {
        s->buf[s->head] = ((const unsigned char *)buf)[i];
        s->head = (s->head + 1) % SOCK_BUF_SIZE;
    }
    s->count += n;
    return (int)n;
}

int sock_poll(struct vfs_inode *in, short events, short *ready) {
    if (!in || in->fs != &sockfs_fs) return -9;      /* -EBADF */
    struct aos_sock *end = sock_from_inode(in);
    if (!end) return -9;
    short r = 0;
    if (end->is_listener && end->state == SOCK_LISTENING) {
        /* A pending connection makes the listener readable. */
        if (end->n_pending > 0) r |= POLLIN;
        r |= POLLOUT;                    /* a listener can always add backlog */
    } else {
        struct aos_sock *s = stream_owner(end);
        /* Readable: buffered data, or EOF when the peer closed. */
        if (s->count > 0) r |= POLLIN;
        else if (s->peer_eof || !s->used) r |= POLLIN | POLLHUP;
        /* Writable: space in the stream buffer. */
        if (s->count < SOCK_BUF_SIZE) r |= POLLOUT;
        if (!s->used || s->peer_eof == 2) r |= POLLERR;
    }
    *ready = (short)(r & events);
    return 0;
}

static int sock_read_at(struct vfs_fs *fs, unsigned int ino, void *buf,
                        unsigned int len, unsigned int off) {
    (void)fs; (void)off;
    if (ino < 1 || ino > (unsigned int)SOCK_MAX) return -9;
    struct aos_sock *end = &socks[ino - 1];
    struct aos_sock *s = stream_owner(end);
    if (len == 0) return 0;
    /* spin until data or EOF, letting the scheduler run */
    while (s->count == 0 && s->used && !s->peer_eof)
        __asm__ __volatile__("sti; hlt; cli");
    if (s->count == 0) return 0;                        /* EOF */
    unsigned int n = len < s->count ? len : s->count;
    for (unsigned int i = 0; i < n; i++) {
        ((unsigned char *)buf)[i] = s->buf[s->tail];
        s->tail = (s->tail + 1) % SOCK_BUF_SIZE;
    }
    s->count -= n;
    return (int)n;
}

static int sock_write_at(struct vfs_fs *fs, unsigned int ino, const void *buf,
                         unsigned int len, unsigned int off) {
    (void)fs; (void)off;
    if (ino < 1 || ino > (unsigned int)SOCK_MAX) return -9;
    struct aos_sock *end = &socks[ino - 1];
    struct aos_sock *s = stream_owner(end);
    if (len == 0) return 0;
    unsigned int done = 0;
    while (done < len) {
        if (!s->used || s->peer_eof == 2) return -32;   /* EPIPE */
        while (s->count == SOCK_BUF_SIZE && s->used && s->peer_eof != 2)
            __asm__ __volatile__("sti; hlt; cli");
        unsigned int space = SOCK_BUF_SIZE - s->count;
        unsigned int n = len - done;
        if (n > space) n = space;
        for (unsigned int i = 0; i < n; i++) {
            s->buf[s->head] = ((const unsigned char *)buf)[done + i];
            s->head = (s->head + 1) % SOCK_BUF_SIZE;
        }
        s->count += n;
        done += n;
    }
    return (int)len;
}

/* ------------------------------------------------------------------ *
 *  close / dup
 * ------------------------------------------------------------------ */

void sock_dup(struct vfs_fs *fs, unsigned int ino, int flags) {
    (void)fs; (void)flags;
    if (ino < 1 || ino > (unsigned int)SOCK_MAX) return;
    struct aos_sock *end = &socks[ino - 1];
    end->n_endpoints++;
    (void)stream_owner(end);
}

void sock_close(struct vfs_fs *fs, unsigned int ino, int flags) {
    (void)fs; (void)flags;
    if (ino < 1 || ino > (unsigned int)SOCK_MAX) return;
    struct aos_sock *end = &socks[ino - 1];

    /* A bound listener: just release it. */
    if (end->is_listener && end->state == SOCK_LISTENING) {
        end->used = 0;
        end->state = SOCK_FREE;
        return;
    }

    /* Pending client socket sitting in some listener's queue: mark EOF. */
    if (end->state == SOCK_CONNECTED && !end->stream) {
        end->used = 0;
        end->state = SOCK_FREE;
        return;
    }

    struct aos_sock *s = stream_owner(end);
    if (!s || s == end) {
        end->used = 0;
        end->state = SOCK_FREE;
        return;
    }

    /* Connected end: drop our reference on the shared stream. */
    end->used = 0;
    end->state = SOCK_FREE;
    /* Remove from the partner's pending list if present. */
    for (int i = 0; i < SOCK_PENDING_MAX; i++) {
        if (s->pending[i] == end) {
            for (int j = i; j < s->n_pending - 1; j++)
                s->pending[j] = s->pending[j + 1];
            s->n_pending--;
            break;
        }
    }
    if (s->n_endpoints > 0) s->n_endpoints--;
    if (s->n_endpoints <= 0) {
        s->used = 0;
        s->state = SOCK_FREE;
    }
}

/* Mark the peer write-end closed for EOF detection. */
static void mark_peer_eof(struct aos_sock *s) {
    s->peer_eof = 2;   /* peer shut down write side */
}

/* ------------------------------------------------------------------ *
 *  Stub directory ops + fs descriptor (mirrors pipefs)
 * ------------------------------------------------------------------ */

static int sock_ro_lookup(struct vfs_fs *fs, unsigned int dir_ino,
                          const char *name, unsigned int *out_ino) {
    (void)fs; (void)dir_ino; (void)name; (void)out_ino;
    return VFS_ENOTDIR;
}
static int sock_ro_readdir(struct vfs_fs *fs, unsigned int dir_ino,
                           unsigned int idx, char *name_out, unsigned int *ino_out) {
    (void)fs; (void)dir_ino; (void)idx; (void)name_out; (void)ino_out;
    return VFS_ENOTDIR;
}
static int sock_ro_add(struct vfs_fs *fs, unsigned int dir_ino,
                       unsigned int child_ino, const char *name) {
    (void)fs; (void)dir_ino; (void)child_ino; (void)name;
    return VFS_ENOTDIR;
}
static int sock_ro_remove(struct vfs_fs *fs, unsigned int dir_ino,
                          const char *name, unsigned int *out_child) {
    (void)fs; (void)dir_ino; (void)name; (void)out_child;
    return VFS_ENOTDIR;
}
static int sock_ro_mkdir(struct vfs_fs *fs, unsigned int parent_ino,
                         const char *name, unsigned int *out_ino) {
    (void)fs; (void)parent_ino; (void)name; (void)out_ino;
    return VFS_ENOTDIR;
}
static int sock_ro_rmdir(struct vfs_fs *fs, unsigned int parent_ino,
                         const char *name) {
    (void)fs; (void)parent_ino; (void)name;
    return VFS_ENOTDIR;
}
static int sock_ro_unlink(struct vfs_fs *fs, unsigned int dir_ino,
                          const char *name) {
    (void)fs; (void)dir_ino; (void)name;
    return VFS_ENOTDIR;
}
static int sock_ro_truncate(struct vfs_fs *fs, unsigned int ino,
                            unsigned int newsize) {
    (void)fs; (void)ino; (void)newsize;
    return VFS_EINVAL;
}
static int sock_ro_alloc(struct vfs_fs *fs, unsigned int type) {
    (void)fs; (void)type;
    return 0;
}
static int sock_stat(struct vfs_fs *fs, unsigned int ino, struct aos_stat *st) {
    (void)fs; (void)ino;
    st->type = 1;
    st->size = 0;
    st->mtime = 0;
    st->nlink = 1;
    return 0;
}

struct vfs_fs sockfs_fs = {
    .name = "sockfs",
    .read_at = sock_read_at,
    .write_at = sock_write_at,
    .truncate = sock_ro_truncate,
    .lookup = sock_ro_lookup,
    .add_dirent = sock_ro_add,
    .remove_dirent = sock_ro_remove,
    .mkdir = sock_ro_mkdir,
    .rmdir = sock_ro_rmdir,
    .unlink = sock_ro_unlink,
    .readdir = sock_ro_readdir,
    .stat = sock_stat,
    .alloc_inode = sock_ro_alloc,
    .close = sock_close,
};

void sock_init(void) {
    memset(socks, 0, sizeof(socks));
    serial_print("socket: AF_UNIX ready (SOCK_MAX=");
    serial_print_dec(SOCK_MAX);
    serial_print(")\n");
}

/* ------------------------------------------------------------------ *
 *  Linux syscall dispatchers
 * ------------------------------------------------------------------ */

static struct aos_sock *alloc_sock(int *out_ino) {
    for (int i = 0; i < SOCK_MAX; i++) {
        if (socks[i].used) continue;
        struct aos_sock *s = &socks[i];
        memset(s, 0, sizeof(*s));
        s->used = 1;
        s->state = SOCK_CONNECTED;
        s->domain = AF_UNIX;
        s->type = 1;          /* SOCK_STREAM */
        s->protocol = 0;
        s->stream = s;        /* own buffer by default */
        s->n_endpoints = 1;
        s->inode.ino = (unsigned int)i + 1;
        s->inode.fs = &sockfs_fs;
        s->inode.type = 1;
        s->inode.nlink = 1;
        s->inode.size = 0;
        s->inode.refcount = 1;
        s->inode.valid = 0;
        *out_ino = i + 1;
        return s;
    }
    return 0;
}

/* Copy a user sockaddr_un into a kernel-local name string. Returns 0 on
   success with kname filled (NUL-terminated), or a negative errno. */
static int copy_name_in(const void *addr, unsigned int addrlen,
                        char *kname, unsigned int kname_sz) {
    memset(kname, 0, kname_sz);
    if (!addr || addrlen < 2) return -22;            /* -EINVAL */
    const char *up = (const char *)addr + 2;         /* sun_path */
    unsigned int off = 0;
    for (unsigned int i = 0; i + 2 < addrlen && off < kname_sz - 1; i++) {
        char c = up[i];
        if (c == 0) break;
        kname[off++] = c;
    }
    return off > 0 ? 0 : -22;                        /* -EINVAL */
}

static int attach_fd(struct aos_sock *s) {
    int fd = vfs_attach_ofile(&s->inode);
    if (fd < 0) return fd;
    return fd;
}

/* Map a task fd to its socket, else NULL. */
static struct aos_sock *fd_sock(int fd) {
    struct task *t = get_current_task();
    if (fd < 3 || fd >= TASK_MAX_FDS || !t->fds[fd]) return 0;
    return sock_from_inode(t->fds[fd]->inode);
}

int sock_sys_socket(int domain, int type, int protocol) {
    if (domain != AF_UNIX || type != 1 || protocol != 0)
        return -97;          /* -EAFNOSUPPORT */
    int ino;
    struct aos_sock *s = alloc_sock(&ino);
    if (!s) return -24;      /* -EMFILE */
    int fd = attach_fd(s);
    if (fd < 0) { s->used = 0; return fd; }
    struct task *t = get_current_task();
    if (fd < TASK_MAX_FDS) t->fds[fd] = vfs_ofile_ptr(fd);
    return fd;
}

int sock_sys_socketpair(int domain, int type, int protocol, int *sv) {
    if (domain != AF_UNIX || type != 1 || protocol != 0)
        return -97;          /* -EAFNOSUPPORT */
    struct task *t = get_current_task();
    (void)t;
    int inoA, inoB;
    struct aos_sock *a = alloc_sock(&inoA);
    struct aos_sock *b = alloc_sock(&inoB);
    if (!a || !b) {          /* -EMFILE; free whichever side was taken */
        if (a) a->used = 0;
        if (b) b->used = 0;
        return -24;
    }
    /* b owns the shared stream buffer; a references it. This mirrors the
       connect/accept layout (client->stream = server-end owner, the owner has
       n_endpoints=2), so both ends read/write one common FIFO. a->state and
       b->state are already SOCK_CONNECTED from alloc_sock. */
    a->stream = b;
    b->n_endpoints = 2;

    int fda = attach_fd(a);
    if (fda < 0) { a->used = 0; b->used = 0; return fda; }
    int fdb = attach_fd(b);
    if (fdb < 0) { a->used = 0; b->used = 0; return fdb; }
    if (fda < TASK_MAX_FDS) t->fds[fda] = vfs_ofile_ptr(fda);
    if (fdb < TASK_MAX_FDS) t->fds[fdb] = vfs_ofile_ptr(fdb);

    /* write back fds[0]=fda, fds[1]=fdb (byte-wise, like getsockopt writing a
       struct to user space) */
    unsigned int *dst = (unsigned int *)sv;
    dst[0] = (unsigned int)fda;
    dst[1] = (unsigned int)fdb;
    return 0;
}

int sock_sys_bind(int fd, const struct sockaddr_un *addr, unsigned int addrlen) {
    struct aos_sock *s = fd_sock(fd);
    if (!s) return -9;                        /* -EBADF */
    char kname[SOCK_NAME_MAX];
    int rc = copy_name_in(addr, addrlen, kname, sizeof(kname));
    if (rc < 0) return rc;
    if (find_bound(kname)) return -98;        /* -EADDRINUSE */
    strncpy(s->name, kname, SOCK_NAME_MAX - 1);
    s->name[SOCK_NAME_MAX - 1] = '\0';
    s->is_listener = 1;
    s->state = SOCK_CONNECTED;
    return 0;
}

int sock_sys_listen(int fd, int backlog) {
    struct aos_sock *s = fd_sock(fd);
    if (!s) return -9;                        /* -EBADF */
    if (!s->is_listener) return -22;          /* -EINVAL */
    (void)backlog;
    s->state = SOCK_LISTENING;
    return 0;
}

int sock_sys_connect(int fd, const struct sockaddr_un *addr, unsigned int addrlen) {
    struct aos_sock *end = fd_sock(fd);
    if (!end) return -9;                     /* -EBADF */
    char kname[SOCK_NAME_MAX];
    int rc = copy_name_in(addr, addrlen, kname, sizeof(kname));
    if (rc < 0) return rc;
    struct aos_sock *srv = find_bound(kname);
    if (!srv || srv->state != SOCK_LISTENING)
        return -111;                         /* -ECONNREFUSED */
    if (srv->n_pending >= SOCK_PENDING_MAX)
        return -105;                         /* -ENOBUFS */
    srv->pending[srv->n_pending++] = end;
    return 0;
}

int sock_sys_accept(int fd, struct sockaddr_un *addr, unsigned int *addrlen) {
    struct aos_sock *srv = fd_sock(fd);
    if (!srv) return -9;                     /* -EBADF */
    if (!srv->is_listener || srv->state != SOCK_LISTENING)
        return -22;                          /* -EINVAL */
    /* Spin until a client arrives. */
    while (srv->n_pending == 0)
        __asm__ __volatile__("sti; hlt; cli");
    struct aos_sock *client = srv->pending[0];
    for (int i = 0; i < srv->n_pending - 1; i++)
        srv->pending[i] = srv->pending[i + 1];
    srv->n_pending--;

    /* Build the server end of the pair; it owns the shared stream. */
    int ino;
    struct aos_sock *srv_end = alloc_sock(&ino);
    if (!srv_end) return -24;                /* -EMFILE */
    client->stream = srv_end;
    srv_end->n_endpoints = 2;                /* srv_end + client */

    int nfd = attach_fd(srv_end);
    if (nfd < 0) { srv_end->used = 0; client->stream = NULL; return nfd; }
    struct task *t = get_current_task();
    if (nfd < TASK_MAX_FDS) t->fds[nfd] = vfs_ofile_ptr(nfd);
    if (addr && addrlen) *addrlen = 0;
    return nfd;
}

int sock_sys_accept4(int fd, struct sockaddr_un *addr, unsigned int *addrlen,
                     int flags) {
    (void)flags;
    return sock_sys_accept(fd, addr, addrlen);
}

int sock_sys_shutdown(int fd, int how) {
    struct aos_sock *end = fd_sock(fd);
    if (!end) return -9;                      /* -EBADF */
    (void)how;
    mark_peer_eof(stream_owner(end));
    return 0;
}

static int copy_name_out(const char *src,
                         struct sockaddr_un *addr, unsigned int *addrlen) {
    if (!addr || !addrlen) return 0;
    unsigned char *dst = (unsigned char *)addr;
    dst[0] = AF_UNIX & 0xFF;
    dst[1] = (AF_UNIX >> 8) & 0xFF;
    unsigned int i = 0;
    while (src[i] && i + 2 < 110) { dst[2 + i] = src[i]; i++; }
    *addrlen = 2 + i;
    return 0;
}

int sock_sys_getsockname(int fd, struct sockaddr_un *addr, unsigned int *addrlen) {
    struct aos_sock *s = fd_sock(fd);
    if (!s) return -9;                        /* -EBADF */
    return copy_name_out(s->name, addr, addrlen);
}

int sock_sys_getpeername(int fd, struct sockaddr_un *addr, unsigned int *addrlen) {
    struct aos_sock *s = fd_sock(fd);
    if (!s) return -9;                        /* -EBADF */
    return copy_name_out(s->name, addr, addrlen);
}

int sock_sys_getsockopt(int fd, int level, int optname, void *optval,
                        unsigned int *optlen) {
    struct aos_sock *s = fd_sock(fd);
    if (!s) return -9;
    /* SO_PEERCRED = 17 at SOL_SOCKET (1) */
    if (level == 1 && optname == 17) {
        struct { unsigned int pid, uid, gid; } cred;
        if (!optval || !optlen || *optlen < sizeof(cred)) return -22;
        struct task *t = get_current_task();
        cred.pid = t->pid;
        cred.uid = t->uid;
        cred.gid = t->gid;
        unsigned char *dst = (unsigned char *)optval;
        for (unsigned int i = 0; i < sizeof(cred); i++)
            dst[i] = ((unsigned char *)&cred)[i];
        *optlen = sizeof(cred);
        return 0;
    }
    return -92;                               /* -ENOPROTOOPT */
}

int sock_sys_setsockopt(int fd, int level, int optname, const void *optval,
                        unsigned int optlen) {
    struct aos_sock *s = fd_sock(fd);
    if (!s) return -9;
    (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;                                 /* no-op */
}

int sock_sys_sendto(int fd, const void *buf, unsigned int len, int flags,
                    const struct sockaddr_un *addr, unsigned int addrlen) {
    (void)flags; (void)addr; (void)addrlen;
    struct task *t = get_current_task();
    struct open_file *of = (fd >= 3 && fd < TASK_MAX_FDS && t->fds[fd])
                               ? t->fds[fd] : 0;
    if (!of) return -9;
    if (!addr)
        return vfs_write_fd(fd, buf, len);
    return -105;                              /* -ENOBUFS (datagram unsupported) */
}

int sock_sys_recvfrom(int fd, void *buf, unsigned int len, int flags,
                      struct sockaddr_un *addr, unsigned int *addrlen) {
    (void)flags;
    if (addr && addrlen) *addrlen = 0;
    return vfs_read_fd(fd, buf, len);
}
