#include "libaos.h"

void main(void) {
    char args[256];
    get_args(args, 256);

    char *p = args;
    while (*p == ' ') p++;
    if (!*p) {
        print("\nUsage: cat <filename>");
        return;
    }

    int fd = sd_open(p, O_RDONLY);
    if (fd < 0) {
        print("\nFile not found: ");
        print(p);
        return;
    }

    static char buf[1024];
    int n = sd_read(fd, buf, 1023);
    if (n > 0) {
        buf[n] = '\0';
        print("\n");
        print(buf);
    }
    sd_close(fd);
}
