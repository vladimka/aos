#include "libaos.h"

static void print_name(const char *name) {
    const char *p = name;
    while (*p && *p != '/') p++;
    if (*p == '/') p++;
    print(p);
}

void main(void) {
    print("\nAvailable commands:");
    int i = 0;
    while (1) {
        char name[28];
        unsigned int size;
        if (fs_list_get(i, name, &size) < 0) break;
        print(" ");
        print_name(name);
        i++;
    }
}
