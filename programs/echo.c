#include "libaos.h"

void main(void) {
    char args[256];
    get_args(args, 256);

    char *p = args;
    while (*p == ' ') p++;
    if (!*p) { print("\n"); return; }

    char *gt = 0;
    for (char *q = p; *q; q++) {
        if (*q == '>') { gt = q; break; }
    }

    if (gt) {
        while (p < gt && *p == ' ') p++;
        unsigned int text_len = 0;
        char *t = p;
        while (t < gt && *t == ' ') t++;
        while (t + text_len < gt && *(t + text_len) != ' ') text_len++;

        char *fn = gt + 1;
        while (*fn == ' ') fn++;
        if (!*fn) {
            print("\nUsage: echo text > filename");
            return;
        }

        char text_buf[256];
        unsigned int i;
        for (i = 0; i < text_len && i < 255; i++)
            text_buf[i] = t[i];
        text_buf[i] = '\0';

        fs_write(fn, text_buf, text_len);
    } else {
        print("\n");
        print(p);
    }
}
