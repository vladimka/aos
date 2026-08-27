#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include "uutils.h"

static int lflag, aflag, hflag, Rflag, rflag, oneflag;

/* ---- uid/gid → name lookup via /etc/passwd and /etc/group ---- */
struct idmap { unsigned int id; char name[16]; };
static struct idmap uid_cache[32];
static int uid_count;
static struct idmap gid_cache[32];
static int gid_count;

static void load_passwd(void) {
    if (uid_count > 0) return;
    int fd = open("/etc/passwd", 0);
    if (fd < 0) return;
    char buf[2048];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    char *p = buf;
    while (*p && uid_count < 32) {
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        *eol = '\0';
        /* format: name:x:uid:gid:... */
        char *tok[6];
        int nt = 0;
        char *t = p;
        while (*t && nt < 6) { tok[nt++] = t; while (*t && *t != ':') t++; if (*t) *t++ = '\0'; }
        if (nt >= 4) {
            uid_cache[uid_count].id = 0;
            const char *d = tok[2];
            while (*d) uid_cache[uid_count].id = uid_cache[uid_count].id * 10 + (*d++ - '0');
            int j = 0;
            while (tok[0][j] && j < 15) { uid_cache[uid_count].name[j] = tok[0][j]; j++; }
            uid_cache[uid_count].name[j] = '\0';
            uid_count++;
        }
        p = eol + 1;
    }
}

static void load_group(void) {
    if (gid_count > 0) return;
    int fd = open("/etc/group", 0);
    if (fd < 0) return;
    char buf[2048];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    char *p = buf;
    while (*p && gid_count < 32) {
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        *eol = '\0';
        char *tok[4];
        int nt = 0;
        char *t = p;
        while (*t && nt < 4) { tok[nt++] = t; while (*t && *t != ':') t++; if (*t) *t++ = '\0'; }
        if (nt >= 3) {
            gid_cache[gid_count].id = 0;
            const char *d = tok[2];
            while (*d) gid_cache[gid_count].id = gid_cache[gid_count].id * 10 + (*d++ - '0');
            int j = 0;
            while (tok[0][j] && j < 15) { gid_cache[gid_count].name[j] = tok[0][j]; j++; }
            gid_cache[gid_count].name[j] = '\0';
            gid_count++;
        }
        p = eol + 1;
    }
}

static const char *lookup_uid(unsigned int uid) {
    load_passwd();
    for (int i = 0; i < uid_count; i++)
        if (uid_cache[i].id == uid) return uid_cache[i].name;
    return "?";
}

static const char *lookup_gid(unsigned int gid) {
    load_group();
    for (int i = 0; i < gid_count; i++)
        if (gid_cache[i].id == gid) return gid_cache[i].name;
    return "?";
}

static void mode_string(unsigned int mode, char *out) {
    /* out must be at least 11 bytes: t rwxrwxrwx\0 */
    unsigned int t = (mode >> 12) & 017;
    if (t == 010) out[0] = '-';
    else if (t == 004) out[0] = 'd';
    else if (t == 002) out[0] = 'l';
    else out[0] = '?';
    for (int i = 0; i < 9; i++) {
        unsigned int bit = 1u << (8 - i);
        out[1 + i] = (mode & bit) ? "rwxrwxrwx"[i] : '-';
    }
    if (mode & 04000) out[3] = 's';   /* SUID */
    if (mode & 02000) out[6] = 's';   /* SGID */
    if (mode & 01000) out[9] = 't';   /* sticky */
    out[10] = '\0';
}

static void show_entries(const char *dirpath, const struct u_entry *ent, int n) {
    fflush(stdout);
    if (lflag) {
        for (int i = 0; i < n; i++) {
            char full[512];
            snprintf(full, sizeof full, "%s/%s", dirpath, ent[i].name);
            struct stat st;
            char perms[11] = "----------";
            const char *owner = "?", *grp = "?";
            if (stat(full, &st) == 0) {
                mode_string(st.st_mode, perms);
                owner = lookup_uid(st.st_uid);
                grp = lookup_gid(st.st_gid);
            }
            char sz[16];
            if (hflag) u_hsize(ent[i].size, sz, sizeof sz);
            else snprintf(sz, sizeof sz, "%u", ent[i].size);
            printf("%s %-8s %-8s %s %s\n", perms, owner, grp, sz, ent[i].name);
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
            char perms[11] = "----------";
            const char *owner = "?", *grp = "?";
            mode_string(st.st_mode, perms);
            owner = lookup_uid(st.st_uid);
            grp = lookup_gid(st.st_gid);
            char sz[16];
            if (hflag) u_hsize((unsigned int)st.st_size, sz, sizeof sz);
            else snprintf(sz, sizeof sz, "%u", (unsigned int)st.st_size);
            printf("%s %-8s %-8s %s %s\n", perms, owner, grp, sz, base);
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
            if (lflag) {
                char full[512];
                snprintf(full, sizeof full, "%s/%s", path, ent[i].name);
                struct stat rst;
                char perms[11] = "----------";
                const char *owner = "?", *grp = "?";
                if (stat(full, &rst) == 0) {
                    mode_string(rst.st_mode, perms);
                    owner = lookup_uid(rst.st_uid);
                    grp = lookup_gid(rst.st_gid);
                }
                printf("%s %-8s %-8s %u %s\n", perms, owner, grp, ent[i].size, ent[i].name);
            } else {
                if (u_have_color(1) && ent[i].type == 2) u_color(1, U_C_DIR);
                printf("%s%s", ent[i].name, ent[i].type == 2 ? "/" : "");
                if (u_have_color(1)) u_color_reset(1);
                printf("\n");
            }
        }
    } else {
        show_entries(path, ent, n);
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