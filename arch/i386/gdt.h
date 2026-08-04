#ifndef GDT_H
#define GDT_H

#define TLS_ENTRY 6   // GDT index of the per-task TLS descriptor (musl entry_number)

void gdt_init(void);
void tss_set_esp0(unsigned int esp0);
void ldt_set_tls(unsigned int base, unsigned int limit,
                 unsigned int seg_32bit, unsigned int read_exec_only,
                 unsigned int limit_in_pages);
void tls_reload_gs(void);

#endif
