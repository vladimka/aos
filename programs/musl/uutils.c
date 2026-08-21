#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "uutils.h"

int u_have_color(int fd) {
    const char *t = getenv("TERM");
    return t && t[0] ? 1 : 0;
}

void u_color(int fd, int idx) {
    char b[16];
    int n = snprintf(b, sizeof b, "\x1b[38;5;%dm", idx);
    write(fd, b, (size_t)n);
}

void u_color_bg(int fd, int idx) {
    char b[16];
    int n = snprintf(b, sizeof b, "\x1b[48;5;%dm", idx);
    write(fd, b, (size_t)n);
}

void u_color_reset(int fd) {
    write(fd, "\x1b[0m", 4);
}

const char *u_hsize(unsigned int n, char *buf, unsigned int bufsz) {
    static const char *units[] = { "B", "K", "M", "G" };
    int u = 0;
    unsigned int v = n;
    while (v >= 1024 && u < 3) { v /= 1024; u++; }
    if (u == 0)
        snprintf(buf, bufsz, "%u%s", n, units[0]);
    else
        snprintf(buf, bufsz, "%u.%u%s", v, (n % 1024) / 100, units[u]);
    return buf;
}

static int u_cmp(const void *a, const void *b) {
    return strcmp(((const struct u_entry *)a)->name,
                  ((const struct u_entry *)b)->name);
}

int u_list_dir(const char *dir, struct u_entry *ent, int max, int show_dot) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (!show_dot && e->d_name[0] == '.') continue;
        strncpy(ent[n].name, e->d_name, U_NAME_MAX);
        ent[n].name[U_NAME_MAX] = 0;
        ent[n].type = 1;
        ent[n].size = 0;
        char p[512];
        if (strcmp(dir, "/") == 0)
            snprintf(p, sizeof p, "/%s", e->d_name);
        else
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0) {
            ent[n].type = S_ISDIR(st.st_mode) ? 2 : 1;
            ent[n].size = (unsigned int)st.st_size;
        }
        n++;
    }
    closedir(d);
    if (n > 1) qsort(ent, (size_t)n, sizeof(struct u_entry), u_cmp);
    return n;
}

void u_print_columns(int fd, const struct u_entry *ent, int n, int one_per_line) {
    if (one_per_line) {
        for (int i = 0; i < n; i++) {
            if (u_have_color(fd) && ent[i].type == 2) u_color(fd, U_C_DIR);
            write(fd, ent[i].name, strlen(ent[i].name));
            if (ent[i].type == 2) write(fd, "/", 1);
            if (u_have_color(fd)) u_color_reset(fd);
            write(fd, "\n", 1);
        }
        return;
    }
    int rows = n;
    int per = 1;
    while (per < rows) { per++; rows = (n + per - 1) / per; }   // square-ish grid
    /* simpler fixed layout: 4 columns when n > 4, else 1 */
    int cols = n > 4 ? 4 : 1;
    int each = (n + cols - 1) / cols;
    for (int r = 0; r < each; r++) {
        for (int c = 0; c < cols; c++) {
            int i = c * each + r;
            if (i >= n) continue;
            if (u_have_color(fd) && ent[i].type == 2) u_color(fd, U_C_DIR);
            write(fd, ent[i].name, strlen(ent[i].name));
            if (ent[i].type == 2) write(fd, "/", 1);
            if (u_have_color(fd)) u_color_reset(fd);
            write(fd, "  ", 2);
        }
        write(fd, "\n", 1);
    }
}