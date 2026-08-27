#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

static const char *lookup_name(const char *file, int target_id) {
    int fd = open(file, 0);
    if (fd < 0) return NULL;
    char buf[2048];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = '\0';
    static char result[16];
    char idstr[12];
    snprintf(idstr, sizeof idstr, ":%d:", target_id);
    char *p = buf;
    while (*p) {
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        char saved = *eol;
        *eol = '\0';
        if (strstr(p, idstr)) {
            int j = 0;
            while (p[j] != ':' && j < 15) { result[j] = p[j]; j++; }
            result[j] = '\0';
            return result;
        }
        *eol = saved;
        if (saved == '\n') p = eol + 1; else break;
    }
    return NULL;
}

int main(void) {
    int uid = getuid();
    const char *name = lookup_name("/etc/passwd", uid);
    if (name)
        printf("%s\n", name);
    else
        printf("%d\n", uid);
    return 0;
}
