#include "libaos.h"

static void fail(const char *what) {
    print("\nFSTEST FAIL ");
    print(what);
    exit_with_code(1);
    for (;;);
}

static int check_str(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

void main(void) {
    // mkdir with auto-parents (/t, /t/a created implicitly)
    if (sd_mkdir("/t/a/b") != 0) fail("mkdir");

    // write a file and read it back
    int fd = sd_open("/t/a/b/f", O_CREAT | O_WRONLY);
    if (fd < 0) fail("open-create");
    if (sd_write(fd, "hello", 5) != 5) fail("write");
    sd_close(fd);

    fd = sd_open("/t/a/b/f", O_RDONLY);
    if (fd < 0) fail("open-read");
    char buf[32];
    int n = sd_read(fd, buf, sizeof(buf));
    if (n != 5) fail("read-size");
    if (!check_str(buf, "hello", 5)) fail("read-data");

    // lseek to offset 3 and read the tail
    if (sd_lseek(fd, 3, SEEK_SET) != 3) fail("lseek");
    n = sd_read(fd, buf, sizeof(buf));
    if (n != 2 || !check_str(buf, "lo", 2)) fail("lseek-read");
    sd_close(fd);

    // readdir of / contains t
    fd = sd_open("/", O_RDONLY);
    if (fd < 0) fail("open-root");
    int found_t = 0;
    char name[28];
    while (sd_readdir(fd, name, sizeof(name)) > 0)
        if (name[0] == 't' && name[1] == 0) { found_t = 1; break; }
    sd_close(fd);
    if (!found_t) fail("readdir-t");

    // stat the file
    struct aos_stat st;
    if (sd_stat("/t/a/b/f", &st) != 0) fail("stat");
    if (st.type != 1 || st.size != 5) fail("stat-fields");

    // unlink + rmdir chain, then confirm it is gone
    if (sd_unlink("/t/a/b/f") != 0) fail("unlink");
    if (sd_rmdir("/t/a/b") != 0) fail("rmdir-b");
    if (sd_rmdir("/t/a") != 0) fail("rmdir-a");
    if (sd_rmdir("/t") != 0) fail("rmdir-t");
    if (sd_stat("/t", &st) == 0) fail("stat-after-rmdir");

    print("\nFSTEST PASS");
}
