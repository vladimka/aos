#ifndef USER_H
#define USER_H

void user_init(void);
void user_program_start(void (*entry)(void));
void user_program_start_linux(void (*entry)(void), unsigned int esp);
void user_program_exit(void) __attribute__((noreturn));
int  user_program_active(void);
unsigned int user_kstack_top(void);

#endif
