/*
 * wtb.h - MechWarrior 2 polygon models (POLY resources, originally .WTB files).
 *
 * A record is one or more objects back to back (1-9; validated: all 3,798
 * records in MW2.PRJ parse to the exact byte, all 85,594 polygon vertex
 * references are in range). Each object:
 *
 *   0x00  "WTBO"
 *   0x04  u32 ?
 *   0x08  char name[16]   each byte negated, e.g. "tw1_hips"
 *   0x18  u16 vertex_count
 *   0x1A  u16 polygon_count
 *   0x1C  u16 flags       low bits ?, 0x1000 / 0x2000 seen on multi-object
 *                         records (meaning unconfirmed)
 *   0x1E  u16 ?
 *   0x20  vertex_count x { i32 x, i32 y, i32 z, u16 a, u16 b }   (a, b: unknown)
 *   then  polygon_count records, FIXED size by vertex count (this is how the
 *         engine walks them, MW2.EXE 0x50073):
 *           n <= 4: 12 bytes { u16 color, u16 n, u16 idx[4] }
 *           n 5-7:  18 bytes { u16 color, u16 n, u16 idx[7] }
 *         color: high byte = palette index of the polygon's base colour, shaded
 *         through a 16-level lighting table (see render.h); low byte = flags.
 *
 * Coordinates: y up, +z forward; same units as skeleton offsets.
 *
 * 3D-accelerated editions (3dfx, ATi, S3, PowerVR; 1996) use an extended
 * layout, detected automatically (all 6,574 models parse; one known odd
 * record excepted):
 *   name stored plainly (not negated)
 *   vertices 40 bytes: i32 x, y, z; double normal[3]; u16 u, v (texel coords)
 *   polygons: n <= 4: 40 bytes, n 5-7: 46 bytes
 *     { u16 color, u16 material, u16 ?, double normal[3], u16 n, u16 idx[n] }
 *     material 0x0Bxx = textured with the mech's camouflage
 *     (verified by rendering the 3dfx Timber Wolf with camo and insignia).
 */
#ifndef MW2_WTB_H
#define MW2_WTB_H

#include <stddef.h>
#include <stdint.h>

#define WTB_MAX_POLY_VERTS 7

typedef struct {
    int32_t  x, y, z;
    uint16_t a, b;         /* DOS: unknown; 3D editions: texel u, v */
    float    normal[3];    /* 3D editions only (smooth shading) */
} wtb_vertex;

typedef struct {
    uint16_t color;
    uint16_t material;     /* 3D editions only; 0x0Bxx = camo-textured */
    uint16_t unknown;
    uint16_t n;
    uint16_t idx[WTB_MAX_POLY_VERTS];
    float    normal[3];    /* 3D editions: bytes 6-29, three doubles (the engine loads them negated, 0x10042140) */
    int      has_normal;
} wtb_poly;

typedef struct {
    char        name[17];
    uint16_t    flags;
    wtb_vertex *verts;
    int         vert_count;
    wtb_poly   *polys;
    int         poly_count;
} wtb_object;

typedef struct {
    wtb_object *objects;
    int         object_count;
    int         extended;  /* 1 = 3D-edition layout */
} wtb_model;

/* Parse a POLY payload. Returns 0 on success, -1 if malformed. */
int  wtb_parse(const uint8_t *data, size_t len, wtb_model *out);
void wtb_free(wtb_model *m);

#endif
