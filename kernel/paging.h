#ifndef PAGING_H
#define PAGING_H

/* x86 PTE flag bits. Bit 9 is "available to software" — reused here as the
   copy-on-write marker: a present page with WRITABLE clear and PTE_COW set is
   shared read-only between fork siblings and gets a private writable copy on
   the first write (handled in isr_handler -> task_handle_cow_fault). */
#define PTE_PRESENT  0x1
#define PTE_WRITABLE 0x2
#define PTE_USER     0x4
#define PTE_COW      0x200

void paging_init(void);
unsigned int *paging_kernel_pd(void);
unsigned int paging_get_cr3(void);
void paging_set_cr3(unsigned int cr3);
int paging_map_user_page(unsigned int vaddr);
int paging_identity_map(unsigned int phys, unsigned int bytes);
void paging_mark_pwt(unsigned int phys, unsigned int size);

#endif
