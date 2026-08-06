#ifndef ICO_H
#define ICO_H

// Decode a Windows .ico file (BMP-encoded images) into a pixel buffer.
//
// On success returns 0 and fills out_px (caller must provide room for
// max_w * max_h unsigned ints) with a full max_w x max_h image where the
// decoded picture is nearest-neighbour scaled to fit and centered, leaving
// transparent margins. Pixels are 0xAARRGGBB: alpha 0xFF = opaque,
// 0x00 = fully transparent. *out_w and *out_h are set to max_w / max_h.
//
// Returns -1 if the data is not a usable ICO, -2 if the best image uses an
// unsupported encoding (e.g. an embedded PNG) or the palette/depth cannot
// be handled.
int ico_decode(const unsigned char *data, unsigned int size,
               unsigned int max_w, unsigned int max_h,
               unsigned int *out_w, unsigned int *out_h,
               unsigned int *out_px);

#endif
