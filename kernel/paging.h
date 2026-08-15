#ifndef PAGING_H
#define PAGING_H

void paging_init(void);
unsigned int *paging_kernel_pd(void);
unsigned int paging_get_cr3(void);
void paging_set_cr3(unsigned int cr3);
int paging_map_user_page(unsigned int vaddr);
int paging_identity_map(unsigned int phys, unsigned int bytes);

#endif
