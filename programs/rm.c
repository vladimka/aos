#include "libaos.h"

void main(void) {
    char args[256];
    get_args(args, 256);

    char *p = args;
    while (*p == ' ') p++;
    if (!*p) {
        print("\nUsage: rm <filename>");
        return;
    }

    if (fs_delete(p) == 0)
        print("\nDeleted: ");
    else
        print("\nNot found: ");
    print(p);
}
