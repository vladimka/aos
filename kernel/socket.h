#ifndef SOCKET_H
#define SOCKET_H

#include "vfs.h"

#define SOCK_MAX        16
#define SOCK_BUF_SIZE   4096
#define SOCK_NAME_MAX   32
#define SOCK_PENDING_MAX 8

/* Socket lifecycle states */
#define SOCK_FREE        0
#define SOCK_LISTENING   1
#define SOCK_CONNECTED   2

/* Abstract socket namespace (kernel-internal, no SFS files needed) */
struct aos_sock {
    unsigned int used;
    unsigned int state;           /* SOCK_FREE / SOCK_LISTENING / SOCK_CONNECTED */
    int domain;                   /* AF_UNIX = 1 */
    int type;                     /* SOCK_STREAM = 1 */
    int protocol;                 /* 0 */
    char name[SOCK_NAME_MAX];     /* abstract- or path- binding name (NUL-terminated) */
    /* Listening server */
    int is_listener;
    struct aos_sock *pending[SOCK_PENDING_MAX];
    int n_pending;
    /* Stream buffer (shared between connected pair, owned by the accept socket) */
    struct aos_sock *stream;      /* points to the socket holding the buffer */
    unsigned int head;
    unsigned int tail;
    unsigned int count;
    unsigned char buf[SOCK_BUF_SIZE];
    unsigned int peer_eof;        /* partner closed their write end */
    /* Reference management */
    int n_endpoints;              /* number of live ends referencing this stream */
    struct vfs_inode inode;       /* embedded for VFS integration */
};

/* AF_UNIX domain constant */
#define AF_UNIX 1

/* sockaddr_un (i386 Linux layout) */
struct sockaddr_un {
    unsigned short sun_family;    /* AF_UNIX */
    char sun_path[108];
};

/* Linux recvfrom/sendto addressing (minimal) */
struct sockaddr {
    unsigned short sa_family;
    char sa_data[14];
};

/* sock_init called at boot, sockfs_fs registration */
void sock_init(void);
extern struct vfs_fs sockfs_fs;

/* Linux syscall dispatchers (called from linux_syscall_handler) */
int sock_sys_socket(int domain, int type, int protocol);
int sock_sys_socketpair(int domain, int type, int protocol, int *sv);
int sock_sys_bind(int fd, const struct sockaddr_un *addr, unsigned int addrlen);
int sock_sys_listen(int fd, int backlog);
int sock_sys_accept(int fd, struct sockaddr_un *addr, unsigned int *addrlen);
int sock_sys_accept4(int fd, struct sockaddr_un *addr, unsigned int *addrlen, int flags);
int sock_sys_connect(int fd, const struct sockaddr_un *addr, unsigned int addrlen);
int sock_sys_shutdown(int fd, int how);
int sock_sys_getsockname(int fd, struct sockaddr_un *addr, unsigned int *addrlen);
int sock_sys_getpeername(int fd, struct sockaddr_un *addr, unsigned int *addrlen);
int sock_sys_getsockopt(int fd, int level, int optname, void *optval, unsigned int *optlen);
int sock_sys_setsockopt(int fd, int level, int optname, const void *optval, unsigned int optlen);
int sock_sys_sendto(int fd, const void *buf, unsigned int len, int flags,
                    const struct sockaddr_un *addr, unsigned int addrlen);
int sock_sys_recvfrom(int fd, void *buf, unsigned int len, int flags,
                      struct sockaddr_un *addr, unsigned int *addrlen);

/* nonblock read/write helpers for vfs_read_fd / vfs_write_fd */
int sock_read_nonblock(struct vfs_fs *fs, unsigned int ino, void *buf,
                       unsigned int len, unsigned int off);
int sock_write_nonblock(struct vfs_fs *fs, unsigned int ino, const void *buf,
                        unsigned int len, unsigned int off);

/* poll(): set *ready to the subset of `events` that are already satisfiable
   on this socket inode. Returns 0, or a negative errno if `in` is not a
   socket inode. */
int sock_poll(struct vfs_inode *in, short events, short *ready);

/* dup / close helpers */
void sock_close(struct vfs_fs *fs, unsigned int ino, int flags);
void sock_dup(struct vfs_fs *fs, unsigned int ino, int flags);

#endif
