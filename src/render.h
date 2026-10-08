/*
 * render.h - software rasterizer drawing models the way the original does:
 * into an 8-bit indexed framebuffer, colour from the game palette, shading
 * through a 16-level lighting table (LUMA resources, e.g. STANDARD.TBL:
 * table[level * 256 + colour]; level 15 = base colour, lower = lit).
 *
 * Palettes are PAL resources: 256 x RGB in VGA 6-bit range (0-63).
 */
#ifndef MW2_RENDER_H
#define MW2_RENDER_H

#include <stdint.h>

#include "mech3d.h"
#include "wtb.h"

typedef struct {
    int      w, h;
    uint8_t *pixels;        /* palette indices */
    float   *depth;
    uint8_t  palette[256][3];   /* 8-bit RGB */
    uint8_t  shade[16 * 256];
} r_target;

int  r_init(r_target *t, int w, int h);
void r_free(r_target *t);
void r_clear(r_target *t, uint8_t colour);

/* Load a 768-byte 6-bit VGA palette and a 4096-byte lighting table. */
int  r_set_palette(r_target *t, const uint8_t *vga6, size_t len);
int  r_set_shade_table(r_target *t, const uint8_t *table, size_t len);

/* Draw a mech in its rest pose, fitted to the target. yaw/pitch in degrees;
 * light is a direction (need not be normalised). */
void r_draw_mech(r_target *t, const mech3d *m, float yaw, float pitch, const float light[3]);

/* ---- true-colour textured path (3D-edition look) ---- */

#include "tex.h"

typedef struct {
    int       w, h;
    uint8_t  *rgb;      /* w*h*3 */
    float    *depth;
} rt_target;

int  rt_init(rt_target *t, int w, int h, uint8_t r, uint8_t g, uint8_t b);
void rt_free(rt_target *t);
/* Polygons with material 0x0Bxx get `camo`; parts whose model name contains
 * "DECL" get `decal` (alpha-tested); the rest use palette[color >> 8]. */
void rt_draw_mech(rt_target *t, const mech3d *m, float yaw, float pitch, const float light[3],
                  const texture *camo, const texture *decal, const uint8_t palette[256][3]);
int  rt_write_ppm(const rt_target *t, const char *path);

/* Write the framebuffer as a binary PPM (RGB). Returns 0 on success. */
int  r_write_ppm(const r_target *t, const char *path);

#endif
