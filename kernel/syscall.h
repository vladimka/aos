#ifndef SYSCALL_H
#define SYSCALL_H

#define SYS_PRINT        0
#define SYS_PRINT_HEX    1
#define SYS_PRINT_DEC    2
#define SYS_PUTCHAR      3
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
#define SYS_RANDOM       33
#define SYS_RTC          34
#define SYS_UPTIME       35

// fd-based filesystem API
#define SYS_OPEN         36
#define SYS_CLOSE        37
#define SYS_READ         38
#define SYS_WRITE        39
#define SYS_LSEEK        40
#define SYS_MKDIR        41
#define SYS_RMDIR        42
#define SYS_READDIR      43
#define SYS_CHDIR        44
#define SYS_GETCWD       45
#define SYS_STAT         46
#define SYS_FSTAT        47
#define SYS_UNLINK       48

void syscall_set_args(const char *args);
void route_text(const char *s, unsigned int len);

#endif
