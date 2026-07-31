#ifndef PAGING_H
#define PAGING_H

void paging_init(void);
unsigned int *paging_kernel_pd(void);
unsigned int paging_get_cr3(void);
void paging_set_cr3(unsigned int cr3);

#endif
