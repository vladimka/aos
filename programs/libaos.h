#ifndef LIBAOS_H
#define LIBAOS_H

#include "aosipc.h"

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

void exit(void);
void sleep_ms(unsigned int ms);                    // SYS_SLEEP (block ~ms)
int  waitpid(unsigned int pid);                    // SYS_WAITPID (exit code or <0)
int  get_children(unsigned int *pids, unsigned int max); // SYS_GET_CHILDREN
void exit_with_code(int code);                     // SYS_EXIT with ebx=code
int  read_key(void);
int  get_random(unsigned char *buf, unsigned int maxlen);  // SYS_RANDOM (virtio-rng)
void *malloc(unsigned int size);
void free(void *p);

// Multitasking + GUI
void yield(void);
unsigned int getpid(void);
int send_msg(unsigned int pid, const struct aos_msg *m);
int recv_msg(struct aos_msg *m);
int register_events(void);
int get_event_pid(void);
int get_mouse(int *x, int *y, int *buttons, int *wheel);
int get_fb_info(unsigned int *addr, unsigned int *w, unsigned int *h,
                unsigned int *pitch, unsigned int *bpp);
int render_text(unsigned int *buf, unsigned int pitch, int x, int y,
                const char *str, unsigned int fg, unsigned int bg);
int fill_rect(unsigned int *buf, unsigned int pitch, int x, int y,
              int w, int h, unsigned int rgb);
int set_stdout(unsigned int pid);
int spawn(const char *path, const char *args, unsigned int sink);

#endif
