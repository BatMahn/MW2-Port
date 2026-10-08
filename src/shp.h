/*
 * shp.h - MW2 "1.10" shape files (SHP resources: HUD and cockpit sprites).
 *
 * Layout (engine draw routine 0x10062241, 3dfx MW2.DLL):
 *   "1.10", u32 shape count, count x { u32 offset, u32 (unused here) } from +8;
 *   shape at file+offset: +8 i32 left, +0x0c i32 top, +0x10 i32 right, +0x14 i32 bottom
 *   (inclusive, relative to the hotspot), then rows from +0x18:
 *     byte b: n = b >> 1; b == 0 end of row; b == 1 skip <next byte> pixels;
 *     even b: n copies of the next byte; odd b > 1: n literal bytes.
 *   Pixels are palette indices (the engine maps them through a 16-bit table).
 */
#ifndef MW2_SHP_H
#define MW2_SHP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int      left, top, right, bottom;   /* hotspot-relative, inclusive */
    int      w, h;
    uint8_t *pix;                        /* w*h palette indices */
    uint8_t *mask;                       /* w*h: 1 = opaque */
} shp_frame;

int  shp_count(const uint8_t *d, size_t len);
/* Decode shape `i`; returns 0 on success. Free with shp_frame_free. */
int  shp_decode(const uint8_t *d, size_t len, int i, shp_frame *out);
void shp_frame_free(shp_frame *f);

#endif
