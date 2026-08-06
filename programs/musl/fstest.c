#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail(const char *what) {
    printf("\nFSTEST FAIL %s", what);
    _exit(1);
}

static int check_str(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

int main(void) {
    // mkdir with auto-parents (/t, /t/a created implicitly)
    if (mkdir("/t/a/b", 0777) != 0) fail("mkdir");

    int fd = open("/t/a/b/f", O_CREAT | O_WRONLY);
    if (fd < 0) fail("open-create");
    if (write(fd, "hello", 5) != 5) fail("write");
    close(fd);

    fd = open("/t/a/b/f", O_RDONLY);
    if (fd < 0) fail("open-read");
    char buf[32];
    int n = read(fd, buf, sizeof(buf));
    if (n != 5) fail("read-size");
    if (!check_str(buf, "hello", 5)) fail("read-data");

    if (lseek(fd, 3, SEEK_SET) != 3) fail("lseek");
    n = read(fd, buf, sizeof(buf));
    if (n != 2 || !check_str(buf, "lo", 2)) fail("lseek-read");
    close(fd);

    DIR *d = opendir("/");
    if (!d) fail("open-root");
    int found_t = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] == 't' && e->d_name[1] == 0) { found_t = 1; break; }
    closedir(d);
    if (!found_t) fail("readdir-t");

    struct stat st;
    if (stat("/t/a/b/f", &st) != 0) fail("stat");
    if (!S_ISREG(st.st_mode) || st.st_size != 5) fail("stat-fields");

    if (unlink("/t/a/b/f") != 0) fail("unlink");
    if (rmdir("/t/a/b") != 0) fail("rmdir-b");
    if (rmdir("/t/a") != 0) fail("rmdir-a");
    if (rmdir("/t") != 0) fail("rmdir-t");
    if (stat("/t", &st) == 0) fail("stat-after-rmdir");

    printf("\nFSTEST PASS\n");
    return 0;
}