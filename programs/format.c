#include "libaos.h"

void main(void) {
    char args[256];
    get_args(args, 256);
    if (args[0] == '-') {
        char *p = args;
        while (*p == ' ') p++;
        p++;
        if (*p == 'y') {
            print("Formatting... ");
            int i = 0;
            while (1) {
                char name[28];
                unsigned int size;
                if (fs_list_get(i, name, &size) < 0) break;
                fs_delete(name);
                i++;
            }
            print("done");
            return;
        }
    }
    print("\nUse 'format -y' to format (removes all files)");
}
