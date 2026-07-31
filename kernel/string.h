#ifndef STRING_H
#define STRING_H

int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, unsigned int n);
int strlen(const char *s);
void strncpy(char *dst, const char *src, unsigned int n);
void *memcpy(void *dst, const void *src, unsigned int n);
void *memset(void *dst, int c, unsigned int n);
void *memcpy_fast(void *dst, const void *src, unsigned int n);
void memset_fast32(void *dst, unsigned int val, unsigned int nwords);

#endif
