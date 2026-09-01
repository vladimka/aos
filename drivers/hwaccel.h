#ifndef HWACCEL_H
#define HWACCEL_H

/* Bochs/VBE (BGA) display controller accel helpers + write-combining fb.
 * Written as pure I/O so it is safe to call before paging is enabled.
 */

int hwaccel_pan_probe(unsigned int linelength, unsigned int vis_px,
                      unsigned int bpp);
void hwaccel_pan_set(int rows);
void hwaccel_pan_reset(void);
unsigned int hwaccel_pan_get_pixels(void);
void hwaccel_wc_framebuffer(unsigned int phys, unsigned int size);

#endif