#ifndef UUTILS_H
#define UUTILS_H

#define U_NAME_MAX 128
#define U_C_DIR    33
#define U_C_EXEC   70

struct u_entry {
    char name[U_NAME_MAX + 1];
    unsigned int type;      // 1 file, 2 dir (stat st_mode DT_*: use S_IFDIR)
    unsigned int size;
};

int u_have_color(int fd);
void u_color(int fd, int idx);
void u_color_bg(int fd, int idx);
void u_color_reset(int fd);
const char *u_hsize(unsigned int n, char *buf, unsigned int bufsz);
int u_list_dir(const char *dir, struct u_entry *ent, int max, int show_dot);
void u_print_columns(int fd, const struct u_entry *ent, int n, int one_per_line);

#endif