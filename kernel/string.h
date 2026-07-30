#ifndef STRING_H
#define STRING_H

int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, unsigned int n);
int strlen(const char *s);
void strncpy(char *dst, const char *src, unsigned int n);

#endif
