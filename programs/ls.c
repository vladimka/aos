#include "libaos.h"

void _start(void) {
    print("\nFiles:");
    int found = 0;
    int i = 0;
    while (1) {
        char name[28];
        unsigned int size;
        if (fs_list_get(i, name, &size) < 0) break;
        found = 1;
        print("\n  ");
        print(name);
        print(" (");
        print_hex(size);
        print(" bytes)");
        i++;
    }
    if (!found) print(" (empty)");
}
