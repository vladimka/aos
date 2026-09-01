// socktest: minimal AF_UNIX echo smoke test for AOS.
// Uses raw write syscalls for output (musl stdio buffers until exit, which
// hides where a blocking program stalls).
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#define NAME "aos-socktest"

static void p(const char *s) {
    int n = 0;
    while (s[n]) n++;
    write(1, s, n);
    write(1, "\n", 1);
}

static void pf(const char *s, long v) {
    char b[96];
    int i = 0;
    while (s[i]) { b[i] = s[i]; i++; }
    b[i++] = '=';
    char tmp[16]; int t = 0;
    if (v == 0) { tmp[t++] = '0'; }
    else { unsigned long u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
           while (u) { tmp[t++] = '0' + u % 10; u /= 10; } }
    if (v < 0) b[i++] = '-';
    while (t) b[i++] = tmp[--t];
    b[i++] = '\n';
    write(1, b, i);
}

int main(void) {
    p("START");
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    pf("socket", ls);
    if (ls < 0) { pf("errno", errno); return 1; }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, NAME, sizeof(sa.sun_path) - 1);

    int r = bind(ls, (struct sockaddr *)&sa, sizeof(sa));
    pf("bind", r);
    if (r < 0) { pf("errno", errno); return 1; }
    r = listen(ls, 4);
    pf("listen", r);
    if (r < 0) { pf("errno", errno); return 1; }

    int cl = socket(AF_UNIX, SOCK_STREAM, 0);
    pf("socket2", cl);
    if (cl < 0) { pf("errno", errno); return 1; }
    struct sockaddr_un ca;
    memset(&ca, 0, sizeof(ca));
    ca.sun_family = AF_UNIX;
    strncpy(ca.sun_path, NAME, sizeof(ca.sun_path) - 1);
    r = connect(cl, (struct sockaddr *)&ca, sizeof(ca));
    pf("connect", r);
    if (r < 0) { pf("errno", errno); return 1; }

    int as = accept(ls, 0, 0);
    pf("accept", as);
    if (as < 0) { pf("errno", errno); return 1; }

    const char *msg = "HELLO-FROM-CLIENT";
    r = (int)send(cl, msg, strlen(msg), 0);
    pf("send", r);
    if (r != (int)strlen(msg)) { pf("errno", errno); return 1; }

    char rbuf[64];
    memset(rbuf, 0, sizeof(rbuf));
    r = (int)recv(as, rbuf, 63, 0);
    pf("recv", r);
    if (r < 0) { pf("errno", errno); return 1; }
    rbuf[r] = 0;

    const char *ack = "ACK-FROM-SERVER";
    r = (int)send(as, ack, strlen(ack), 0);
    pf("send2", r);
    if (r != (int)strlen(ack)) { pf("errno", errno); return 1; }

    char cbuf[64];
    memset(cbuf, 0, sizeof(cbuf));
    r = (int)recv(cl, cbuf, 63, 0);
    pf("recv2", r);
    if (r < 0) { pf("errno", errno); return 1; }
    cbuf[r] = 0;

    p("rbuf="); p(rbuf);
    p("cbuf="); p(cbuf);
    if (strcmp(rbuf, msg) != 0) return 1;
    if (strcmp(cbuf, ack) != 0) return 1;

    close(as); close(cl); close(ls);
    p("SOCKTEST OK");
    return 0;
}
