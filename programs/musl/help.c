#include <stdio.h>
#include <dirent.h>

int main(void) {
    printf("\nAvailable commands: ");
    DIR *d = opendir("/bin");
    if (!d) return 0;
    struct dirent *e;
    int first = 1;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (e->d_type != DT_REG) continue;
        if (!first) printf(" ");
        printf("%s", e->d_name);
        first = 0;
    }
    printf("\n");
    closedir(d);
    return 0;
}