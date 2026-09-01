/* stage2test: Stage-2 regression for AOS Linux syscalls:
 *   wait4 (114) - specific pid, exit-status layout, WNOHANG
 *   socketpair (360) - bidirectional stream over one shared buffer
 *   setsid (66) / getpgrp / getpgid / getsid - return own pid
 * Prints STAGE2 OK only if every check passes.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/socket.h>

static int fail;
#define CHK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fail = 1; } } while (0)

int main(void) {
    /* 1. waitpid(specific child) + exit-status layout */
    pid_t c1 = fork();
    if (c1 < 0) CHK(0, "fork c1");
    if (c1 == 0) { _exit(42); }              /* child: exit code 42 */
    int st = -1;
    pid_t r1 = waitpid(c1, &st, 0);
    printf("waitpid spec pid=%d exp=%d status=%d\n", (int)r1, (int)c1, st);
    CHK(r1 == c1, "specific waitpid reaps the child");
    CHK(WIFEXITED(st) && WEXITSTATUS(st) == 42,
        "exit status 42 via (code<<8) layout");

    /* 2. waitpid WNOHANG must return immediately (0 or already-exited pid) */
    pid_t c2 = fork();
    if (c2 < 0) CHK(0, "fork c2");
    if (c2 == 0) { for (volatile unsigned int i = 0; i < 20000000; i++) ; _exit(0); }
    st = -1;
    pid_t r2 = waitpid(c2, &st, WNOHANG);
    printf("waitpid wnohang pid=%d\n", (int)r2);
    CHK(r2 == 0 || r2 == c2, "WNOHANG returns immediately");
    if (r2 == 0) { waitpid(c2, &st, 0); }    /* reap it so it does not stay */

    /* 3. socketpair: one shared bidirectional stream */
    int sv[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    printf("socketpair rc=%d fds=%d,%d\n", rc, sv[0], sv[1]);
    CHK(rc == 0 && sv[0] > 2 && sv[1] > 2 && sv[0] != sv[1],
        "socketpair ok, distinct fds");
    CHK(write(sv[0], "ABCD", 4) == 4, "socketpair write0");
    char rbuf[8] = {0};
    int n = read(sv[1], rbuf, 4);
    printf("socketpair fwd n=%d buf=%s\n", n, rbuf);
    CHK(n == 4 && memcmp(rbuf, "ABCD", 4) == 0, "socketpair forward write/read");
    CHK(write(sv[1], "XY", 2) == 2, "socketpair write1");
    n = read(sv[0], rbuf, 2);
    CHK(n == 2 && rbuf[0] == 'X' && rbuf[1] == 'Y',
        "socketpair reverse write/read");

    /* 4. setsid/getpgrp/getpgid/getsid return own pid */
    pid_t me = getpid();
    CHK(setsid() == me, "setsid returns pid");
    CHK(getpgrp() == me, "getpgrp returns pid");
    CHK(getpgid(0) == me, "getpgid(0) returns pid");
    CHK(getsid(0) == me, "getsid(0) returns pid");

    printf("%s\n", fail ? "STAGE2 FAIL" : "STAGE2 OK");
    return fail ? 1 : 0;
}
