#ifndef FS_H
#define FS_H

void fs_init(void);
void fs_format(void);
int  fs_create(const char *name);
int  fs_delete(const char *name);
int  fs_write(const char *name, const char *data, unsigned int size);
int  fs_read(const char *name, char *buf, unsigned int size);
int  fs_get_size(const char *name);
int  fs_exists(const char *name);

typedef void (*fs_list_callback)(const char *name, unsigned int size);
void fs_list(fs_list_callback cb);

#endif
