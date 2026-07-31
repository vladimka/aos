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
    while (n > 0 && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
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

void *memcpy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n >= 4) {
        *(unsigned int *)d = *(const unsigned int *)s;
        d += 4;
        s += 4;
        n -= 4;
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, unsigned int n) {
    unsigned char *d = dst;
    while (n >= 4) {
        *(unsigned int *)d = (unsigned int)(unsigned char)c * 0x01010101u;
        d += 4;
        n -= 4;
    }
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

// rep movsl/movsb copy — QEMU TCG runs these as a fast 32-byte/iter helper
// instead of per-element MMIO/store ops, so multi-MB framebuffer copies are
// several times faster than the scalar memcpy above.
void *memcpy_fast(void *dst, const void *src, unsigned int n) {
    unsigned long di = (unsigned long)dst, si = (unsigned long)src;
    unsigned int c;
    if (!(di & 3) && !(si & 3) && !(n & 3)) {
        c = n >> 2;
        __asm__ __volatile__("rep movsl" : "+D"(di), "+S"(si), "+c"(c) : : "memory");
    } else {
        c = n;
        __asm__ __volatile__("rep movsb" : "+D"(di), "+S"(si), "+c"(c) : : "memory");
    }
    return dst;
}

void memset_fast32(void *dst, unsigned int val, unsigned int nwords) {
    unsigned long di = (unsigned long)dst;
    __asm__ __volatile__("rep stosl" : "+D"(di), "+c"(nwords) : "a"(val) : "memory");
}
