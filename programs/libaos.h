#ifndef LIBAOS_H
#define LIBAOS_H

#include "aosipc.h"

struct aos_time {
    int year, month, day, hour, minute, second;
};

void print(const char *s);
void print_hex(unsigned int n);
void print_dec(unsigned int n);
void putchar(char c);

// fd-based filesystem API (SYS_OPEN..SYS_UNLINK)
#define O_RDONLY     0x00000
#define O_WRONLY     0x00001
#define O_RDWR       0x00002
#define O_CREAT      0x00040
#define O_TRUNC      0x00200
#define O_APPEND     0x00400
#define O_DIRECTORY  0x10000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

struct aos_stat {
    unsigned int type;    // 1 file, 2 dir
    unsigned int size;
    unsigned int mtime;
    unsigned int nlink;
};

int sd_open(const char *path, int flags);
int sd_close(int fd);
int sd_read(int fd, void *buf, int len);
int sd_write(int fd, const void *buf, int len);
int sd_lseek(int fd, int off, int whence);
int sd_mkdir(const char *path);
int sd_rmdir(const char *path);
int sd_readdir(int fd, char *name, int name_len);
int sd_chdir(const char *path);
int sd_getcwd(char *buf, int len);
int sd_stat(const char *path, struct aos_stat *st);
int sd_fstat(int fd, struct aos_stat *st);
int sd_unlink(const char *path);

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
int read_key(void);
int get_random(unsigned char *buf, unsigned int maxlen);  // SYS_RANDOM (virtio-rng)
int get_rtc(struct aos_time *t);                          // SYS_RTC (CMOS wall clock)
unsigned int get_uptime(void);                            // SYS_UPTIME (seconds since boot)
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
