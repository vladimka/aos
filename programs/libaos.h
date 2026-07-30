#ifndef LIBAOS_H
#define LIBAOS_H

void print(const char *s);
void print_hex(unsigned int n);
void print_dec(unsigned int n);
void putchar(char c);

int fs_write(const char *name, const char *data, unsigned int size);
int fs_read(const char *name, char *buf, unsigned int size);
int fs_delete(const char *name);
int fs_get_size(const char *name);
int fs_exists(const char *name);
int fs_list_get(unsigned int idx, char *name_buf, unsigned int *size_out);

unsigned int get_tick(void);
void clear_screen(void);
void reboot(void);
void shutdown(void);
void panic(void);
void get_args(char *buf, unsigned int maxlen);

#endif
