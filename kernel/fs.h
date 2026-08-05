#ifndef FS_H
#define FS_H

// Flat filesystem API (SFS1-compatible), implemented over the VFS layer by
// kernel/vfscompat.c. Names are resolved relative to the VFS root.
void fs_init(void);
void fs_format(void);
int  fs_create(const char *name);
int  fs_delete(const char *name);
int  fs_write(const char *name, const char *data, unsigned int size);
int  fs_read(const char *name, char *buf, unsigned int size);
int  fs_read_at(const char *name, char *buf, unsigned int size, unsigned int offset);
int  fs_get_size(const char *name);
int  fs_exists(const char *name);

// Enumerate root entries; returns 0 on success, -1 when exhausted.
int  sfs_get_entry(unsigned int idx, char *name_buf, unsigned int *size_out);

#endif
