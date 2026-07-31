#ifndef MOUSE_H
#define MOUSE_H

void mouse_init(void);
void mouse_process_byte(unsigned char data);
void mouse_flush_wheel(void);
void mouse_get_state(int *x, int *y, int *buttons, int *wheel);

#endif
