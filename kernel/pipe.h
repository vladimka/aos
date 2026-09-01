#ifndef PIPE_H
#define PIPE_H

#include "vfs.h"

#define PIPE_BUF_SIZE 4096
#define PIPE_MAX      8

struct aos_pipe {
    unsigned int used;
    unsigned int head;
    unsigned int tail;
    unsigned int count;
    unsigned int nreaders;
    unsigned int nwriters;
    unsigned char buf[PIPE_BUF_SIZE];
    struct vfs_inode inode;
};

struct aos_pipe *pipe_alloc(void);
void pipe_close(struct vfs_fs *fs, unsigned int ino, int flags);
void pipe_dup(struct vfs_fs *fs, unsigned int ino, int flags);
int pipe_read_nonblock(struct vfs_fs *fs, unsigned int ino, void *buf,
                       unsigned int len, unsigned int off);
int pipe_write_nonblock(struct vfs_fs *fs, unsigned int ino, const void *buf,
                        unsigned int len, unsigned int off);

/* poll(): set *ready to the subset of `events` satisfiable on this pipe
   inode. Returns 0, or a negative errno if `in` is not a pipe inode. */
int pipe_poll(struct vfs_inode *in, short events, short *ready);

extern struct vfs_fs pipefs_fs;

#endif
