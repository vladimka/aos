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

extern struct vfs_fs pipefs_fs;

#endif
