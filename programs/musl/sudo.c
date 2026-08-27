#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "aosabi.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: sudo <command> [args...]\n");
        return 1;
    }
    int uid = getuid();
    /* Only root or users in sudo group can sudo */
    if (uid != 0) {
        /* Check /etc/group for "sudo" group containing this uid */
        int fd = open("/etc/group", 0);
        int allowed = 0;
        if (fd >= 0) {
            char buf[2048];
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                char target[12];
                snprintf(target, sizeof target, ":%d", uid);
                char *p = buf;
                while (*p && !allowed) {
                    char *eol = p;
                    while (*eol && *eol != '\n') eol++;
                    char saved = *eol;
                    *eol = '\0';
                    if (strncmp(p, "sudo:", 5) == 0 && strstr(p + 5, target))
                        allowed = 1;
                    *eol = saved;
                    if (saved == '\n') p = eol + 1; else break;
                }
            }
        }
        if (!allowed) {
            fprintf(stderr, "sudo: user is not in the sudoers file\n");
            return 1;
        }
    }
    /* Become root via kernel credential override */
    setenv("__AOS_UID", "0", 1);
    setenv("__AOS_GID", "0", 1);
    /* Set PATH so execvp can find commands in bin/ */
    setenv("PATH", "bin", 1);
    setenv("TERM", "aos", 1);

    /* execvp replaces the current process with the target command */
    execvp(argv[1], argv + 1);
    fprintf(stderr, "sudo: %s: command not found\n", argv[1]);
    return 127;
}
