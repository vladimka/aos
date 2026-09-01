/* polltest.c -- smoke test for the kernel poll(2) (syscall 168).
 *
 * Deterministic checks (no sockets needed):
 *   1. poll on fd 1 with POLLOUT and timeout 0 -> immediately ready (1).
 *   2. poll on a bogus fd with POLLIN -> POLLNVAL, still counts as ready (1).
 *   3. poll on an empty pipe read end with timeout 0 -> not ready (0).
 *   4. poll on the same empty pipe with a positive timeout -> 0 after it
 *      expires (exercises the blocking-with-deadline path).
 */
#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

static void wr(const char *s) {
    while (*s) {
        ssize_t n = write(1, s, 1);
        if (n <= 0) return;
        s++;
    }
}

static int check(int cond, const char *what) {
    if (!cond) {
        wr("FAIL: ");
        wr(what);
        wr("\n");
        return 0;
    }
    return 1;
}

int main(void) {
    struct pollfd fds[2];
    int ok = 1;

    wr("PT:start\n");

    /* 1. stdout writable */
    wr("PT:poll1-before\n");
    fds[0].fd = 1;
    fds[0].events = POLLOUT;
    fds[0].revents = 0;
    int n = poll(fds, 1, 0);
    ok &= check(n == 1, "fd1 POLLOUT ready");
    ok &= check((fds[0].revents & POLLOUT) != 0, "fd1 revents POLLOUT");

    /* 2. bogus fd -> POLLNVAL */
    wr("PT:poll2-before\n");
    fds[0].fd = 99999;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    n = poll(fds, 1, 0);
    ok &= check(n == 1, "bogus fd count 1");
    ok &= check((fds[0].revents & POLLNVAL) != 0, "bogus fd POLLNVAL");
    wr("PT:poll2-after\n");

    /* empty pipe: read end empty, timeout 0 -> not ready */
    wr("PT:poll3-before\n");
    int p[2];
    if (pipe(p) != 0) { wr("FAIL: pipe\n"); return 2; }
    fds[0].fd = p[0];
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    n = poll(fds, 1, 0);
    ok &= check(n == 0, "empty pipe timeout0 not ready");
    wr("PT:poll3-after\n");

    /* empty pipe: positive timeout -> 0 after it expires */
    wr("PT:poll4-before\n");
    fds[0].fd = p[0];
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    n = poll(fds, 1, 300);
    ok &= check(n == 0, "empty pipe timeout300 returns 0");
    wr("PT:poll4-after\n");

    /* pipe writer side is writable */
    wr("PT:poll5-before\n");
    fds[0].fd = p[1];
    fds[0].events = POLLOUT;
    fds[0].revents = 0;
    n = poll(fds, 1, 0);
    ok &= check(n == 1, "pipe WR POLLOUT ready");
    ok &= check((fds[0].revents & POLLOUT) != 0, "pipe WR revents POLLOUT");
    wr("PT:poll5-after\n");

    wr(ok ? "POLLTEST OK\n" : "POLLTEST FAILED\n");
    return ok ? 0 : 1;
}
