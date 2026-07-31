#ifndef AOSIPC_H
#define AOSIPC_H

// Message types carried by task mailboxes
#define MSG_KEY     1   // key event: a = codepoint (or GUI_KEY_* special code)
#define MSG_DATA    2   // stdout data: a = byte count, b/c/d = 12 payload bytes
#define MSG_UPDATE  3   // app -> wm: "repaint window"   a = winid
#define MSG_CREATE  4   // app -> wm: "create window"    a = width, b = height
#define MSG_WININFO 5   // wm -> app: "window ready"     a = winid, b = slab
#define MSG_EXIT    6   // kernel -> sink: "task exited" a = pid

// Shared-memory window slabs (identity-mapped, user-accessible, see paging.c).
// Every window owns one 1 MB slab; apps draw 32bpp pixels at:
//   AOS_SLAB_BASE + slab * AOS_SLAB_SIZE
#define AOS_SLAB_BASE 0x03000000
#define AOS_SLAB_SIZE 0x100000
#define AOS_SLABS     16

// Kernel-side helpers operating on user pixel buffers (32bpp), via syscalls.
struct aos_render_req {
    unsigned int *buf;
    unsigned int pitch;
    int x, y;
    const char *str;
    unsigned int fg;
    unsigned int bg;
};

struct aos_fill_req {
    unsigned int *buf;
    unsigned int pitch;
    int x, y, w, h;
    unsigned int rgb;
};

// A mailbox message: type + 4 payload words.
struct aos_msg {
    unsigned int type;
    unsigned int a, b, c, d;
};

#endif
