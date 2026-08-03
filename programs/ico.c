#include "ico.h"

#define ICO_RD16(p) (((unsigned int)(p)[0]) | ((unsigned int)(p)[1] << 8))
#define ICO_RD32(p) (((unsigned int)(p)[0]) | ((unsigned int)(p)[1] << 8) | \
                     ((unsigned int)(p)[2] << 16) | ((unsigned int)(p)[3] << 24))

// Context describing one chosen icon image, used for per-pixel sampling.
struct ico_ctx {
    const unsigned char *d;   // whole file
    unsigned int size;        // whole file size
    unsigned int xor_off;     // start of XOR bitmap
    unsigned int and_off;     // start of AND mask
    unsigned int pal_off;     // start of palette (bpp <= 8)
    unsigned int stride;      // XOR row stride (bytes)
    unsigned int and_stride;  // AND row stride (bytes)
    unsigned int w, h;        // image width / height
    unsigned int bpp;
};

// Sample one pixel at top-down coordinates (x, y). Returns 0xAARRGGBB, or
// 0x00000000 for a transparent pixel.
static unsigned int ico_pixel(const struct ico_ctx *c, unsigned int x,
                              unsigned int y) {
    unsigned int yb = c->h - 1 - y;           // bottom-up row index
    unsigned int o = c->xor_off + yb * c->stride;
    unsigned int idx, r = 0, g = 0, b = 0, a = 0xFF;

    switch (c->bpp) {
    case 32: {
        unsigned int p = o + x * 4;
        if (p + 3 >= c->size) return 0;
        b = c->d[p];
        g = c->d[p + 1];
        r = c->d[p + 2];
        a = c->d[p + 3];
        break;
    }
    case 24: {
        unsigned int p = o + x * 3;
        if (p + 2 >= c->size) return 0;
        b = c->d[p];
        g = c->d[p + 1];
        r = c->d[p + 2];
        break;
    }
    case 8:
        if (o + x >= c->size) return 0;
        idx = c->d[o + x];
        goto palette;
    case 4:
        if (o + (x >> 1) >= c->size) return 0;
        idx = (x & 1) ? (c->d[o + (x >> 1)] & 0x0F)
                      : (c->d[o + (x >> 1)] >> 4);
        goto palette;
    case 1:
        if (o + (x >> 3) >= c->size) return 0;
        idx = (c->d[o + (x >> 3)] >> (7 - (x & 7))) & 1;
        goto palette;
    default:
        return 0;
    }

    goto alpha;

palette: {
        unsigned int p = c->pal_off + idx * 4;
        if (p + 3 >= c->size) return 0;
        b = c->d[p];
        g = c->d[p + 1];
        r = c->d[p + 2];
    }

alpha: {
        unsigned int m = c->and_off + yb * c->and_stride + (x >> 3);
        if (m >= c->size) return 0;
        if (c->d[m] & (0x80 >> (x & 7))) return 0;   // AND mask bit set
        if (a < 0x80) return 0;                       // 32bpp alpha
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

int ico_decode(const unsigned char *data, unsigned int size,
               unsigned int max_w, unsigned int max_h,
               unsigned int *out_w, unsigned int *out_h,
               unsigned int *out_px) {
    if (!data || !out_px || max_w == 0 || max_h == 0) return -1;
    if (size < 6) return -1;
    if (ICO_RD16(data) != 0) return -1;          // reserved
    if (ICO_RD16(data + 2) != 1) return -1;      // type must be 1 (icon)

    unsigned int count = ICO_RD16(data + 4);
    if (count == 0) return -1;
    if (6 + count * 16 > size) return -1;

    // Pick the entry whose area is closest to the target area.
    unsigned int target = max_w * max_h;
    int best = -1;
    unsigned int best_delta = 0xFFFFFFFFu;
    unsigned int best_area = 0;
    for (unsigned int i = 0; i < count; i++) {
        const unsigned char *e = data + 6 + i * 16;
        unsigned int w = e[0] ? (unsigned int)e[0] : 256u;
        unsigned int h = e[1] ? (unsigned int)e[1] : 256u;
        unsigned int area = w * h;
        unsigned int delta = area > target ? area - target : target - area;
        if (delta < best_delta || (delta == best_delta && area > best_area)) {
            best_delta = delta;
            best_area = area;
            best = (int)i;
        }
    }
    if (best < 0) return -1;

    const unsigned char *e = data + 6 + (unsigned int)best * 16;
    unsigned int entry_off = ICO_RD32(e + 12);
    unsigned int bytes_in = ICO_RD32(e + 8);
    if (entry_off + 40 > size) return -1;

    // Embedded PNG images are not supported by this decoder.
    if (data[entry_off] == 0x89 && data[entry_off + 1] == 'P' &&
        data[entry_off + 2] == 'N' && data[entry_off + 3] == 'G')
        return -2;

    unsigned int bisize = ICO_RD32(data + entry_off);
    if (bisize < 40) return -2;                  // only BITMAPINFOHEADER

    struct ico_ctx c;
    c.d = data;
    c.size = size;
    c.w = ICO_RD32(data + entry_off + 4);
    if (c.w == 0) return -2;
    c.h = ICO_RD32(data + entry_off + 8) / 2;    // height includes AND mask
    if (c.h == 0) return -2;
    c.bpp = (unsigned int)ICO_RD16(data + entry_off + 14);

    unsigned int clr_used = ICO_RD32(data + entry_off + 32);
    unsigned int pal_bytes = 0;
    if (c.bpp <= 8) {
        unsigned int n = clr_used ? clr_used : (1u << c.bpp);
        pal_bytes = n * 4;
    }

    c.pal_off = entry_off + bisize;
    c.stride = ((c.w * c.bpp + 31) / 32) * 4;
    c.and_stride = ((c.w + 31) / 32) * 4;
    c.xor_off = c.pal_off + pal_bytes;
    c.and_off = c.xor_off + c.stride * c.h;

    if (c.and_off + c.and_stride * c.h > size) return -1;
    if (c.and_off + c.and_stride * c.h - entry_off > bytes_in) return -1;

    // Target size (fit inside max_w x max_h, keep aspect).
    unsigned int ow, oh;
    if (c.w * max_h >= c.h * max_w) {
        ow = max_w;
        oh = (c.h * max_w + c.w / 2) / c.w;
    } else {
        oh = max_h;
        ow = (c.w * max_h + c.h / 2) / c.h;
    }
    if (ow == 0) ow = 1;
    if (oh == 0) oh = 1;
    int off_x = (int)((max_w - ow) / 2);
    int off_y = (int)((max_h - oh) / 2);

    for (unsigned int y = 0; y < max_h; y++)
        for (unsigned int x = 0; x < max_w; x++)
            out_px[y * max_w + x] = 0;

    for (unsigned int oy = 0; oy < oh; oy++) {
        unsigned int sy = (oy * c.h) / oh;
        for (unsigned int ox = 0; ox < ow; ox++) {
            unsigned int sx = (ox * c.w) / ow;
            out_px[(unsigned int)(off_y + (int)oy) * max_w +
                   (unsigned int)(off_x + (int)ox)] =
                ico_pixel(&c, sx, sy);
        }
    }

    if (out_w) *out_w = max_w;
    if (out_h) *out_h = max_h;
    return 0;
}
