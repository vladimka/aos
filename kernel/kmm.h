#ifndef KMM_H
#define KMM_H

void kmm_init(void);
void kmm_selftest(void);

void *kmalloc(unsigned int size);
void *kcalloc(unsigned int n, unsigned int sz);
void kfree(void *ptr);

#endif