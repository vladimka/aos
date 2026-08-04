#ifndef VNET_H
#define VNET_H

void vnet_init(void);
int vnet_send(const unsigned char *frame, unsigned int len);

#endif
