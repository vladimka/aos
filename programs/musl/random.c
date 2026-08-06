#include <stdio.h>
#include <sys/random.h>

int main(void) {
    unsigned char buf[32];
    ssize_t n = getrandom(buf, sizeof(buf), 0);
    if (n < 0) {
        printf("random: no entropy device\n");
        return 1;
    }
    for (ssize_t i = 0; i < n; i++)
        printf("%x", buf[i]);
    printf("\n");
    return 0;
}