#include "libaos.h"

void _start(void) {
    char args[256];
    get_args(args, 256);

    char *p = args;
    while (*p == ' ') p++;
    if (!*p) {
        print("\nUsage: cat <filename>");
        return;
    }

    int size = fs_get_size(p);
    if (size < 0) {
        print("\nFile not found: ");
        print(p);
        return;
    }

    static char buf[1024];
    int rsize = size;
    if (rsize > 1023) rsize = 1023;
    if (fs_read(p, buf, rsize) > 0) {
        buf[rsize] = '\0';
        print("\n");
        print(buf);
    }
}
