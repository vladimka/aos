#ifndef SYSCALL_H
#define SYSCALL_H

#define SYS_PRINT        0
#define SYS_PRINT_HEX    1
#define SYS_PRINT_DEC    2
#define SYS_PUTCHAR      3
#define SYS_FS_WRITE     4
#define SYS_FS_READ      5
#define SYS_FS_DELETE    6
#define SYS_FS_SIZE      7
#define SYS_FS_EXISTS    8
#define SYS_FS_LIST_GET  9
#define SYS_TICK         10
#define SYS_CLEAR        11
#define SYS_REBOOT       12
#define SYS_PANIC        13
#define SYS_SHUTDOWN     14
#define SYS_GET_ARGS     15
#define SYS_EXIT         16
#define SYS_READ_KEY     17

// Multitasking + GUI
#define SYS_YIELD        18
#define SYS_GETPID       19
#define SYS_SEND         20
#define SYS_RECV         21
#define SYS_EVENT        22
#define SYS_MOUSE        23
#define SYS_FB_INFO      24
#define SYS_TEXT         25
#define SYS_FILL         26
#define SYS_SETOUT       27
#define SYS_SPAWN        28
#define SYS_GETEVENT     29
#define SYS_SLEEP        30
#define SYS_WAITPID      31
#define SYS_GET_CHILDREN 32

void syscall_set_args(const char *args);
void route_text(const char *s, unsigned int len);

#endif
