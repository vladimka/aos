#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aos_test.h"

int main(void) {
    TEST_SUITE("fstest");

    TEST_ASSERT_EQ(mkdir("/t/a/b", 0777), 0);

    int fd = open("/t/a/b/f", O_CREAT | O_WRONLY);
    TEST_ASSERT_GE(fd, 0);
    TEST_ASSERT_EQ(write(fd, "hello", 5), 5);
    close(fd);

    fd = open("/t/a/b/f", O_RDONLY);
    TEST_ASSERT_GE(fd, 0);
    char buf[32];
    int n = read(fd, buf, sizeof(buf));
    TEST_ASSERT_EQ(n, 5);
    TEST_ASSERT_EQ(memcmp(buf, "hello", 5), 0);

    TEST_ASSERT_EQ(lseek(fd, 3, SEEK_SET), 3);
    n = read(fd, buf, sizeof(buf));
    TEST_ASSERT_EQ(n, 2);
    TEST_ASSERT_EQ(memcmp(buf, "lo", 2), 0);
    close(fd);

    DIR *d = opendir("/");
    TEST_ASSERT(d != NULL);
    int found_t = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] == 't' && e->d_name[1] == 0) { found_t = 1; break; }
    closedir(d);
    TEST_ASSERT(found_t);

    struct stat st;
    TEST_ASSERT_EQ(stat("/t/a/b/f", &st), 0);
    TEST_ASSERT(S_ISREG(st.st_mode));
    TEST_ASSERT_EQ(st.st_size, 5);

    TEST_ASSERT_EQ(unlink("/t/a/b/f"), 0);
    TEST_ASSERT_EQ(rmdir("/t/a/b"), 0);
    TEST_ASSERT_EQ(rmdir("/t/a"), 0);
    TEST_ASSERT_EQ(rmdir("/t"), 0);
    TEST_ASSERT_NE(stat("/t", &st), 0);

    TEST_PASS();
}
