#include <dirent.h>
#include <stdio.h>

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    DIR *d = opendir(dir);
    if (!d) {
        printf("ls: %s: No such file or directory\n", dir);
        return 1;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        printf("%s\n", e->d_name);
    }
    closedir(d);
    return 0;
}
