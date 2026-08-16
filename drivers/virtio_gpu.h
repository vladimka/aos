#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#define VGPU_FORMAT_B8G8R8X8 2
#define VGPU_CMD_RESOURCE_CREATE_2D  0x0101
#define VGPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VGPU_CMD_RESOURCE_FLUSH 0x0104
#define VGPU_CMD_SET_SCANOUT 0x0103
#define VGPU_RESP_OK_NODATA 0x1100

struct vgpu_hdr {
    unsigned int type;
    unsigned int flags;
    unsigned long long fence_id;
    unsigned int ctx_id;
    unsigned int padding;
} __attribute__((packed));

int vgu_init(void);
int vgu_active(void);
void vgu_flip(void);
unsigned int vgu_back(void);
void vgu_info(unsigned int *w, unsigned int *h, unsigned int *pitch);

#endif