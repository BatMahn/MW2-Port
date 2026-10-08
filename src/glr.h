/*
 * glr.h - OpenGL 3.3 core renderer for the enhanced (3D-edition) look.
 *
 * Window/context creation is the caller's job (SDL2 in glview, headless EGL
 * in glshot); this module only issues GL calls, so it runs the same way in
 * both. Any resolution and aspect ratio: the vertical field of view is fixed
 * and wider screens see more to the sides ("Hor+").
 *
 * Coordinates: the game's data is left-handed (+x is the mech's right, +y up,
 * +z forward; e.g. TW1DECLR sits at +x, TW1LULEG at -x). GL is right-handed,
 * so z is negated on upload. A mech faces -z in GL space; yaw 0 views its front.
 *
 * Shading model (matching what the 3D editions draw):
 *   - polygons with material 0x0Bxx sample the camo texture, decal parts
 *     sample the insignia (alpha-tested), others use their palette colour
 *   - per-vertex normals when the model has them (3D editions), face normals
 *     otherwise; two-sided diffuse + ambient
 *   - exponential fog: f = exp(-density * distance), mixed toward fog colour
 *     (curve per the PowerVR edition's density values; world scale still to
 *     be confirmed)
 */
#ifndef MW2_GLR_H
#define MW2_GLR_H

#include <stdint.h>

#include "mech3d.h"
#include "tex.h"

typedef struct glr glr;

typedef struct {
    float yaw, pitch;        /* degrees, orbit around the mech */
    float distance;          /* in mech radii; 0 = auto framing */
    float vfov;              /* vertical field of view, degrees */
    float light[3];
    float ambient;           /* 0-1 */
    int   point_light;       /* 1: the light sits at light_pos (a lit explosion, engine 0x10045d00) instead of shining along light */
    float light_pos[3];
    float fog_density;       /* per world unit; 0 = off */
    float fog_color[3];
    float sky_color[3];      /* clear colour */
    int   bilinear;          /* texture filtering on/off */
    float sky_scroll;        /* U offset of the sky texture (accumulates over time) */
    int   use_target;        /* 1: orbit target[] (game coords) at distance_abs instead of auto framing */
    float target[3], distance_abs;
    float ortho_w;           /* > 0: an orthographic view this wide (cm) - the satellite map */
    int   free_cam;          /* 1: first-person camera at eye[] (game coords) looking along look_yaw/look_pitch */
    float eye[3], look_yaw, look_pitch;
    int   wire;              /* image enhancement: black background, polygons as lines (world red, actors blue) */
    int   notex_world;       /* Combat Variables TERRAIN TEXTURES off: the world layer and the ground flat (engine render flag bit) */
    int   notex_actors;      /* OBJECT TEXTURES off: mechs and objects flat-shaded in their palette colours */
    int   flat_sky, flat_ground;   /* the in-game GRAPHICS menu's Textured Sky / Ground off: the texture's average colour
                                    * (3Dfx 0x10027c80 fills with palette 0xe0 / 0xef, set from 0x1002d330's average) */
    /* an edition's texture filtering (GL enums; 0 = from `bilinear`): the world / mech textures, and the sky / ground */
    int   tex_min, tex_mag, env_min, env_mag;
    /* the DOS edition's shading (glr_set_dos): the LITE light (position, or a direction when dos_dir), ambient level
     * (0-127 scale), the distance per darker shade level (0 = none) */
    float dos_lpos[3], dos_amb, dos_fogdist;
    int   dos_dir;
    float dos_band;          /* DOS haze band above the horizon (HRZM +4: pixels at 320 wide), 0 = none */
    int   pvr_veil;          /* PowerVR fog: the 40 % white, fogged plane at y 55555.55 over the sky (0x1005ED90) */
    int   solo;              /* the target viewer: black behind, no sky / ground / world layer / effects, no fog */
    float mono[4];           /* a > 0: the actor layer untextured in this colour, lit (the target viewer's solid mode); a = 2: in each
                              * part's own colour (mech3d_part.tint) */
    int   clip[4];           /* clip[2] > 0: drawn only inside this window rectangle (x, y from the bottom left, w, h) - a
                              * cockpit window opening or closing (3Dfx 0x10011aa0: the view keeps its scale, clipped) */
    float far_cull;          /* > 0: objects wholly beyond this distance (cm) are not drawn - the planet's VIEW far
                              * (DOS MW2.EXE 0x3f500 / 0x4e741, 3Dfx 0x1002fba0 / 0x1003dfbc); 0 = everything */
    int   mga_mip;           /* Matrox Mystique: the ground's mip level by view depth (2500 / 5000 / 15000 cm, MYSTIQUE.PAR) */
} glr_view;

/* Requires a current GL 3.3 core context. Returns NULL on failure (shader
 * compile errors are printed to stderr). */
glr *glr_create(void);
void glr_destroy(glr *r);

#define GLR_SLOTS 256
#define GLR_SLOT_CAMO 0x00      /* + camo index 0-7 */
#define GLR_SLOT_INSIGNIA 0x14  /* + clan index */

/* Upload a mech (replaces any previous one). Each polygon's texture is
 * slots[(colour & 0xFF) + offset], offset = camo for slot 0x00 and clan for
 * slot 0x14 (as the 3D editions' engine does). Missing slots, and DOS-format
 * models, fall back to palette[colour >> 8]. */
int  glr_set_mech(glr *r, const mech3d *m, const uint8_t palette[256][3],
                  texture *const slots[GLR_SLOTS], int camo, int clan);
/* Second layer for moving/posed actors drawn alongside the main mesh (world).
 * Cheap to call every frame; doesn't change the camera framing. */
int  glr_set_actors(glr *r, const mech3d *m, const uint8_t palette[256][3],
                    texture *const slots[GLR_SLOTS], int camo, int clan);

/* Sky and ground (3D-edition style; see skygnd.h). Either texture may be NULL
 * to disable that layer. Copies what it needs. */
void glr_set_environment(glr *r, const texture *ground, const texture *sky, float tile_sky, float tile_ground);

/* forget the uploaded slot textures (the next glr_set_mech / glr_set_actors uploads again): new texture set */
void glr_flush_textures(glr *r);
/* Average colour of a texture (0-1 RGB), as the PowerVR engine uses for fog. */
void glr_texture_average(const texture *t, float out[3]);

/* Camera-facing textured sprites (weapon effects), game coordinates, drawn unlit after the scene
 * with the texture's 1-bit alpha. Copies up to GLR_MAX_SPRITES; the textures must stay valid. */
#define GLR_MAX_SPRITES 1024
typedef struct { float pos[3]; float half_w, half_h; const texture *tex; } glr_sprite;
void glr_set_sprites(glr *r, const glr_sprite *s, int n);

/* The bank-0 bitmap table (BMID bank 0: scrub rocks / cacti) for colour type 3 billboards; call before glr_set_mech.
 * The array is the caller's and must stay valid. */
void glr_set_bank0(glr *r, texture *const bank0[GLR_SLOTS]);
int  glr_billboard_count(void);   /* TEST ONLY: billboards in the last world layer built */

/* Draw into the currently bound framebuffer, viewport w x h. */
void glr_draw(glr *r, const glr_view *v, int w, int h);
void glr_draw_rect(glr *r, const glr_view *v, int x0, int y0, int w, int h);

void glr_default_view(glr_view *v);

/* The DOS edition's colours (on with pal != NULL; call before uploading meshes): the polygon colour word is decoded the
 * DOS engine's way (bits 12-14 type, 8-11 ramp, 0-7 intensity or bitmap) and shaded per pixel into a palette index -
 * flat lit = ramp x 16 + level, textured = LUMA[level][texel] - with the level from the light, the ambient level and the
 * distance (MW2.EXE 0x38ae3, 0x3e086, 0x38ee0); luma: 16 x 256 (LUMA record), pal: the mission's PAL. */
void glr_set_dos(glr *r, const uint8_t pal[256][3], const uint8_t *luma);
/* replace only the DOS palette (the time-of-day fades) */
void glr_set_dos_palette(glr *r, const uint8_t pal[256][3]);

#endif
