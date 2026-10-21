/*
 * hud.h - the cockpit HUD, drawn from the game's own SHP sprites (OpenGL 3.3 overlay).
 *
 * Sprites: SHP resources decoded with shp.c and the COCKPIT palette; the 640x480 set (names
 * ending in "6"). Layout: the CPIT record (320x200 rectangles, x2 for 640x480).
 */
#ifndef MW2_HUD_H
#define MW2_HUD_H

#include "prj.h"

typedef struct {
    unsigned tex;
    int      w, h, left, top;   /* size and hotspot offset (left/top relative to the hotspot) */
    int      smooth;            /* bilinear + alpha curve (hud_sprite_smooth) */
} hud_sprite;

typedef struct hud_s hud;

hud *hud_create(prj_archive *a);
void hud_destroy(hud *h);
/* The palette the sprites loaded from now on are drawn in (768 bytes of 6-bit VGA values, expanded as the VGA DAC:
 * v << 2 | v >> 4). The engine draws the HUD through its 16-bit table 0x10260324, which only 0x1002ac40 fills (all 256
 * entries, RGB555 from the PAL record of the mission's current PALG slot - 0x101b0d20, set from the PALG chunk by
 * 0x1003e104); hud_create starts with the COCKPIT record. */
void hud_set_palette(hud *h, const uint8_t *p6);
/* A palette change while the sprites are loaded (the engine reloading 0x10260324, 0x1002ac40; the DOS edition's VGA
 * palette fades): the new palette (768 bytes of 6-bit values) for everything drawn from now on, and every loaded sprite
 * that uses a changed colour redrawn. Returns 1 if any colour changed. */
int  hud_repalette(hud *h, const uint8_t *p6);
/* the current palette's colour idx as RGBA 0..1 */
void hud_pal_color(const hud *h, int idx, float rgba[4]);
/* Load SHP `name` frame `frame`; returns 0 on success. */
int  hud_sprite_load(hud *h, prj_archive *a, const char *name, int frame, hud_sprite *out);
void hud_sprite_free(hud_sprite *s);
/* As hud_sprite_load, with palette index `from` drawn as index `to` (the engine's remap, 0x10062208). */
int  hud_sprite_load_remap(hud *h, prj_archive *a, const char *name, int frame, int from, int to, hud_sprite *out);
/* Smooth (bilinear) sampling for a sprite - for line art drawn scaled, e.g. the damage outline. */
void hud_sprite_smooth(hud_sprite *s, int on);
/* Draw only the sub-rectangle (rx, ry, rw, rh) of the sprite (sprite pixels), sprite hotspot at (x, y). */
void hud_draw_region(hud *h, const hud_sprite *s, float x, float y, float rx, float ry, float rw, float rh);
/* Begin / end the overlay. Positions are in a 640x480 virtual screen mapped onto the 4:3 area
 * of the window; sprites and text are drawn at native pixel size (x1, or a whole multiple on
 * windows 1536+ px tall), snapped to screen pixels - as the DOS game draws its art 1:1 at
 * 640x480 and 1024x768. */
void hud_begin(hud *h, int w, int h_px);
/* The whole w x h_px target is the 640 x 480 layout, scaled separately across and down (a 320 x 200 screen shown at
 * 4:3: the original's VGA HUD, pixels 1:1) */
void hud_begin_stretched(hud *h, int w, int h_px);
/* a size in screen pixels in layout units, across / down */
float hud_px_x(const hud *h, float px);
float hud_px_y(const hud *h, float px);
void hud_end(hud *h);
/* Draw sprite with its hotspot at (x, y); tint multiplies the colour (1,1,1,1 = as is). */
void hud_draw(hud *h, const hud_sprite *s, float x, float y, const float tint[4]);
/* As hud_draw, k times the size (whole multiple: stays pixel-exact). */
void hud_draw_scaled(hud *h, const hud_sprite *s, float x, float y, int k);
/* as hud_draw_scaled with a fractional scale; k < 0: the DOS 1024x768 size (one sprite pixel per 1/768 of the height) */
void hud_draw_f(hud *h, const hud_sprite *s, float x, float y, float k);
/* the sprite stretched across the whole viewport width at the top, height_px tall (screen pixels; <= 0: its height on
 * a 4:3 screen it spans) */
void hud_draw_stretch_top(hud *h, const hud_sprite *s, float height_px);
/* the same at the top (bottom = 0) or the bottom edge of the window (engine 0x10002e40: message line 1) */
void hud_draw_stretch_row(hud *h, const hud_sprite *s, float height_px, int bottom);
/* the 640-space x of the viewport's left edge (negative when the viewport is wider than 4:3) */
float hud_left_x(const hud *h);
/* The port's wide HUD layout (mw2port.cfg hud_layout=wide): the drawing that follows placed as if the 4:3 area stood at
 * the window's left edge (side -1), right edge (+1) or in the centre (0, as hud_begin leaves it) - corner instruments
 * keep their size and their distance from the 4:3 area's edge. No effect unless the window is wider than 4:3. */
void hud_side(hud *h, int side);
/* a 640-space point in window pixels (top-left origin), with the current side */
float hud_screen_x(const hud *h, float x);
float hud_screen_y(const hud *h, float y);
/* Draw sprite columns [u0, u0 + width) (sprite pixels, wrapping) centred on virtual x, top at y. */
void hud_draw_window(hud *h, const hud_sprite *s, float u0, float width, float x, float y, const float tint[4]);
/* Vertical window: sprite rows [v0, v0 + height) with the top-left at virtual (x, y). */
void hud_draw_window_v(hud *h, const hud_sprite *s, float v0, float height, float x, float y);
/* A line between virtual points, width in screen pixels. */
void hud_line(hud *h, float x0, float y0, float x1, float y1, float width_px, const float rgba[4]);
/* n segments {x0, y0, x1, y1} (640 units) in one draw */
void hud_lines(hud *h, const float *seg, int n, float width_px, const float rgba[4]);
/* A circle outline (segments), radius in virtual units. */
void hud_circle(hud *h, float cx, float cy, float r, float width_px, const float rgba[4]);
void hud_tint(hud *h, int w, int hp, const float mul[3], const float add[3]);
/* the palette flash for the HUD drawn from the next hud_begin on: every colour mixed toward (r, g, b) by k (0 off) */
void hud_set_flash(hud *h, float r, float g, float b, float k);   /* takes effect at once (the HUD's program) */
float hud_flash_level(const hud *h);
void hud_rect(hud *h, float x, float y, float w, float hgt, const float rgba[4]);
/* a filled convex polygon of n virtual points {x, y} */
void hud_poly(hud *h, const float *xy, int n, const float rgba[4]);
/* Clip the HUD drawing that follows to the virtual rectangle (x, y, w, hgt) (scissor; empty: nothing drawn), until
 * hud_clip_off - the engine's instrument windows drawn into their animating rectangle (0x10021730 / 0x10011aa0) */
void hud_clip(hud *h, float x, float y, float w, float hgt);
void hud_clip_off(hud *h);
/* The game font (FONT records "1." : 128 glyphs, height, transparent index, offsets; each glyph
 * u32 width + width x height bytes). Returns 0 on success. */
int  hud_font_load(hud *h, prj_archive *a, const char *name);
/* Draw text with its top-left at virtual (x, y), tinted. Returns the width in virtual units. */
float hud_text(hud *h, float x, float y, const char *str, const float rgba[4]);
/* Width of text in virtual units (as hud_text would draw it). */
float hud_text_width(hud *h, const char *str);
/* The font's line height in virtual units (as hud_text draws it; the engine's font height, 0x100600d8). */
float hud_text_height(hud *h);
/* CPIT layout rectangles (320x200 units); returns the count. */
int  hud_layout(prj_archive *a, int rects[][4], int max);

#endif
