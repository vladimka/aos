#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void show_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        printf("\n  (cannot open %s)", dir);
        return;
    }
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        found = 1;
        struct stat st;
        unsigned int size = 0;
        char path[300];
        if (dir[0] == '/' && dir[1] == 0)
            snprintf(path, sizeof(path), "/%s", e->d_name);
        else
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (stat(path, &st) == 0) size = (unsigned int)st.st_size;
        printf("\n  %s (%x bytes)", e->d_name, size);
    }
    if (!found) printf(" (empty)");
    closedir(d);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        printf("\nFiles in %s:", argv[1]);
        show_dir(argv[1]);
    } else {
        printf("\nFiles:");
        show_dir("/");
    }
    return 0;
}