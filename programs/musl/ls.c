#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "uutils.h"

static int lflag, aflag, hflag, Rflag, rflag, oneflag;

static void show_entries(const struct u_entry *ent, int n) {
    fflush(stdout);  // flush headers/separators before the raw-write listing
    if (lflag) {
        for (int i = 0; i < n; i++) {
            char sz[16];
            if (hflag) u_hsize(ent[i].size, sz, sizeof sz);
            else snprintf(sz, sizeof sz, "%u", ent[i].size);
            printf("%c rwxrwxrwx %s %s\n",
                   ent[i].type == 2 ? 'd' : '-', sz, ent[i].name);
        }
        return;
    }
    u_print_columns(1, ent, n, oneflag);
}

static int list_one(const char *path, int header, int depth) {
    struct stat st;
    if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (lflag) {
            char sz[16];
            if (hflag) u_hsize((unsigned int)st.st_size, sz, sizeof sz);
            else snprintf(sz, sizeof sz, "%u", (unsigned int)st.st_size);
            printf("- rwxrwxrwx %s %s\n", sz, base);
        } else {
            printf("%s\n", base);
        }
        return 0;
    }
    struct u_entry ent[256];
    int n = u_list_dir(path, ent, 256, aflag);
    if (n < 0) { fprintf(stderr, "ls: %s: No such file or directory\n", path); return 1; }
    if (header) printf("%s:\n", path);
    if (rflag) {
        for (int i = n - 1; i >= 0; i--) {
            if (lflag)
                printf("%c rwxrwxrwx %u %s\n",
                       ent[i].type == 2 ? 'd' : '-', ent[i].size, ent[i].name);
            else {
                if (u_have_color(1) && ent[i].type == 2) u_color(1, U_C_DIR);
                printf("%s%s", ent[i].name, ent[i].type == 2 ? "/" : "");
                if (u_have_color(1)) u_color_reset(1);
                printf("\n");
            }
        }
    } else {
        show_entries(ent, n);
    }
    if (Rflag) {
        for (int i = 0; i < n; i++) {
            if (ent[i].type == 2) {
                char sub[512];
                if (path[0] == '/' && path[1] == 0)
                    snprintf(sub, sizeof sub, "/%s", ent[i].name);
                else
                    snprintf(sub, sizeof sub, "%s/%s", path, ent[i].name);
                printf("\n");
                list_one(sub, 1, depth + 1);
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        printf("usage: ls [-alhRr1] [file...]\n");
        return 0;
    }
    int c;
    while ((c = getopt(argc, argv, "alhRr1")) != -1) {
        switch (c) {
        case 'a': aflag = 1; break;
        case 'l': lflag = 1; break;
        case 'h': hflag = 1; break;
        case 'R': Rflag = 1; break;
        case 'r': rflag = 1; break;
        case '1': oneflag = 1; break;
        default: return 1;
        }
    }
    int rc = 0;
    if (optind >= argc) {
        rc = list_one(".", 0, 0);
    } else {
        int many = (argc - optind > 1);
        for (int i = optind; i < argc; i++)
            if (list_one(argv[i], many, 0)) rc = 1;
    }
    return rc;
}