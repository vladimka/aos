#include "string.h"

int strlen(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, unsigned int n) {
    while (n-- && *a && *a == *b) { a++; b++; }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

void strncpy(char *dst, const char *src, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
}
