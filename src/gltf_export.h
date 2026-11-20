/*
 * gltf_export.h - the game's models and textures out to modern files: PNG images, and glTF 2.0 binary (.glb) scenes that
 * Blender, game engines and model viewers open.
 *
 * Conversions (the game -> glTF): centimetres -> metres; the game's left-handed axes (+x right, +y up, +z forward) ->
 * glTF's right-handed (+z toward the viewer): z negated, so a model keeps its shape (not mirrored) and the scene looks
 * the same with north (game +z) along glTF -z. Every material is double-sided (the data's winding is not consistent;
 * the engines cull nothing). Polygons by their colour word (3D editions' models):
 *   textured (a slot in the mission's texture map; camouflage 0x00 / insignia 0x14 + the owner's offset) -> a material
 *     with that image; colour types 8-11 alpha-masked (cut-off 0.5), the rest opaque;
 *   flat (no texture, slot 0xf0, type 1, laser type 0) -> a material in the palette colour;
 *   type 2 (outlines: lattices, the shield dome) -> LINES in the palette colour, unlit (KHR_materials_unlit);
 *   type 3 billboards (scrub, effect cards) -> left out (drawn as camera-facing sprites in the game).
 * Each node carries the game's names in extras.mw2 (record, OBJ node, model, type word, texture slots on materials),
 * so an edited scene can be matched back to the game's data.
 */
#ifndef MW2_GLTF_EXPORT_H
#define MW2_GLTF_EXPORT_H

#include <stdint.h>
#include "mech3d.h"
#include "tex.h"

/* PNG (RGBA8, rows top first; rgba bytes R, G, B, A). 0 on success. */
int gx_png_write(const char *path, const uint8_t *rgba, int w, int h);
/* a texture as PNG (its 0xAABBGGRR texels) */
int gx_png_texture(const char *path, const texture *t);

typedef struct gx gx;
/* palette: the mission's (flat colours); slots / names: the mission's texture map (names may be NULL) */
gx  *gx_new(const uint8_t palette[256][3], texture *const slots[256], const char (*names)[16]);
void gx_free(gx *g);
/* a group node (no mesh): its translation (game cm) under parent (-1: the scene root); extras: a JSON object or NULL.
 * Returns the node index. */
int  gx_group(gx *g, const char *name, const float pos[3], int parent, const char *extras);
/* a part (its model as a mesh - shared with every part of the same model and texture offsets - placed by its yaw /
 * rotation and position less origin) under parent. Returns the node index, -1 if it has nothing to draw. */
int  gx_part(gx *g, const mech3d_part *p, const float origin[3], int parent, const char *extras);
/* the ground: a square of half-size `half` cm around (cx, cz), the texture repeating `per_cm` times a cm */
int  gx_ground(gx *g, const texture *t, float cx, float cz, float half, float per_cm, int parent);
/* write the scene. 0 on success */
int  gx_write_glb(gx *g, const char *path);

/* a JSON-safe copy of s (quotes, backslashes and control characters escaped) into out */
void gx_json_str(char *out, size_t n, const char *s);

#endif
