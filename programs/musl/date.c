#include <stdio.h>
#include <string.h>
#include "aosabi.h"

static void print2(unsigned int n) { putchar((char)('0' + n / 10)); putchar((char)('0' + n % 10)); }

int main(int argc, char **argv) {
    struct aos_time t;
    if (aos_get_rtc(&t) != 0) { fprintf(stderr, "date: rtc unavailable\n"); return 1; }
    const char *fmt = "%Y-%m-%d %H:%M:%S";
    if (argc > 1 && argv[1][0] == '+') fmt = argv[1] + 1;
    for (const char *p = fmt; *p; p++) {
        if (*p == '%') {
            p++;
            switch (*p) {
            case 'Y': printf("%u", (unsigned int)t.year); break;
            case 'm': print2((unsigned int)t.month); break;
            case 'd': print2((unsigned int)t.day); break;
            case 'H': print2((unsigned int)t.hour); break;
            case 'M': print2((unsigned int)t.minute); break;
            case 'S': print2((unsigned int)t.second); break;
            default: putchar('%'); putchar(*p); break;
            }
        } else putchar(*p);
    }
    printf("\n");
    return 0;
}
