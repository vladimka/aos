#ifndef IDT_H
#define IDT_H

void idt_init(void);
void idt_install_irq(unsigned char irq, void (*handler)(void));

#endif
