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
    int   dos_sky_smooth;    /* the port's dos_shading=smooth: the Gouraud polygons' and the haze band's colours blended, not
                              * stepped and dithered (0 = original) */
    int   pvr_veil;          /* PowerVR fog: the 40 % white, fogged plane at y 55555.55 over the sky (0x1005ED90) */
    int   solo;              /* the target viewer: black behind, no sky / ground / world layer / effects, no fog */
    float mono[4];           /* a > 0: the actor layer untextured in this colour, lit (the target viewer's solid mode); a = 2: in each
                              * part's own colour (mech3d_part.tint) */
    int   clip[4];           /* clip[2] > 0: drawn only inside this window rectangle (x, y from the bottom left, w, h) - a
                              * cockpit window opening or closing (3Dfx 0x10011aa0: the view keeps its scale, clipped) */
    float far_cull;          /* > 0: objects wholly beyond this distance (cm) are not drawn - the planet's VIEW far
                              * (DOS MW2.EXE 0x3f500 / 0x4e741, 3Dfx 0x1002fba0 / 0x1003dfbc); 0 = everything */
    int   mga_mip;           /* Matrox Mystique: the ground's mip level by view depth (2500 / 5000 / 15000 cm, MYSTIQUE.PAR) */
    /* the Titanium edition (mercsw.dll + its renderer plugins): its vertex fog ti_fog = {1 = on, Z (the full-fog depth,
     * 0 = none), the camera's factor}; the cloud layer's drift (slot 0x8c, U and V in texture repeats); effects
     * alpha-blended (Render type 8) instead of alpha-tested */
    float ti_fog[3];
    float ti_scroll[2];
    int   fx_blend;
    /* the Titanium lights (mercsw 0x1002ecaa, per vertex, one-sided; its SKYGND.PAR amb / wa / wb): ti_light on, the
     * ambient colour, two world lights {type 1 directional (unit vector) / 2 point (position), game coordinates}, all 0-1;
     * the ground's single colour (0x1005527e); the explosion lights (0x1002f238: point lights, each lighting the objects
     * that reach into its +-radius box, no falloff) */
    int   ti_light;
    float ti_amb[3], ti_lcol[2][3], ti_lvec[2][3], ti_gnd[3];
    int   ti_ltype[2];
#define GLR_TI_DYN 8
    int   ti_dyn_n;
    float ti_dyn[GLR_TI_DYN][7];     /* x, y, z (game), radius, r, g, b: the explosion and projectile lights */
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

/* forget the uploaded slot textures (the next glr_set_mech / glr_set_actors uploads again) and the sprites': new texture set */
void glr_flush_textures(glr *r);
/* an animated bank-1 slot (engine 0x1002ce50): draw it with frame t from now on (NULL: the slot's own texture again); the
 * texture must stay valid. Cheap when unchanged. glr_flush_textures forgets the frames. */
void glr_slot_frame(glr *r, int slot, const texture *t);
/* Average colour of a texture (0-1 RGB), as the PowerVR engine uses for fog. */
void glr_texture_average(const texture *t, float out[3]);

/* Textured sprites (weapon effects), game coordinates, drawn unlit after the scene with the texture's 1-bit alpha.
 * bb = 1: the engine's colour type 3 billboard (0x1002a8e0) - pos the card's foot, top its top: a screen square of
 * the projected edge's length centred on it; bb = 0: a camera-facing rectangle half_w x half_h around pos.
 * Copies up to GLR_MAX_SPRITES; the textures must stay valid. */
#define GLR_MAX_SPRITES 1024
/* Image enhancement (view.wire): a sprite is drawn as its card's polygon outline (DOS 0x38ae3 / 3Dfx 0x1002a4b0 colour
 * the object type picks) - wire_code 2 red (colour 0xb, type word 0x400: the projectile pool), else dark red (8: the
 * effect pool's type 0x20); has_tri: that triangle (world, game coordinates), else the card's rectangle. */
/* quad: a card drawn as the 3D polygon it is (colour 0x8xxx / 0xbxxx cards - muzzle flashes, jet flames, the death
 * explosion: 0x1002a4b0 -> 0x10026e80 -> 0x1002c880): its four corners qv (world, game coordinates) with texel
 * coordinates quv (0-1 of the bitmap), as they are - not turned to the camera. */
/* ti: a TEXTURES TITANIUM card drawn by mercsw's billboard 0x1004ef4c (mode 0): its corners and UVs (bb = 1) and the
 * texels x the vertex colour 0xc8 (200 / 255, 0x1004f33a..0x1004f356; D3D's default MODULATE). */
typedef struct { float pos[3]; float half_w, half_h; const texture *tex; float top[3]; int bb; int wire_code, has_tri; float tri[3][3];
                 int quad; float qv[4][3], quv[4][2]; int ti; } glr_sprite;
void glr_set_sprites(glr *r, const glr_sprite *s, int n);

/* The Titanium particles (mercsw particle.PID, 0x1004d7ba): screen squares of size_px pixels (at 640 x 480; scaled to the
 * window's height) from the projected point right and down, depth-tested, alpha-blended (Render type 7: SRCALPHA /
 * INVSRCALPHA, dx5draw 0x10004477) at `alpha` x the texel's alpha, unlit and unfogged (specular 0xff000000). Copied. */
typedef struct { float pos[3]; float size_px; const texture *tex; } glr_particle;
void glr_set_particles(glr *r, const glr_particle *p, int n, float alpha);

/* The bank-0 bitmap table (BMID bank 0: scrub rocks / cacti) for colour type 3 billboards; call before glr_set_mech.
 * The array is the caller's and must stay valid. */
void glr_set_bank0(glr *r, texture *const bank0[GLR_SLOTS]);
int  glr_billboard_count(void);   /* TEST ONLY: billboards in the last world layer built */

/* The Titanium edition's sky dome (its SKYDOME object, POLY TL1SKYDM: the cloud ceiling, the mountain ring, the sun and
 * the moon), drawn around the eye before the ground; each polygon's texture is slots[colour & 0xff], its texel UVs
 * divided by the texture's size - 1 (mercsw 0x10001477). NULL removes it. */
void glr_set_skydome(glr *r, const wtb_object *o, texture *const slots[GLR_SLOTS]);

/* The Titanium models' texture table (TEXTURES TITANIUM): parts flagged ti (mech3d_part.ti, or with a ti_model) are drawn
 * from it, their colour words read as the Titanium engine's (DOS-style: bits 12-14 the kind, 5-7 textured with bitmap =
 * low byte; UV = texel / (size - 1), mercsw 0x10001477) and the camouflage / insignia slots remapped per star (mercsw
 * 0x100748ab: an even slot 0-0xa with the star's CAMO set c: + c for c < 2, else base[c] + slot / 2; slot 0x3f + star).
 * slots NULL: off. The arrays are the caller's and must stay valid; call before glr_set_mech / glr_set_actors. */
void glr_set_titanium(glr *r, texture *const slots[GLR_SLOTS], const int camo[8]);

/* The rest of the Titanium models' look (call after glr_set_titanium, before glr_set_mech / glr_set_actors):
 * bank0 - its bank-0 bitmaps (texture indices below 0x100: the colour type 3 billboards' bitmap (w >> 4) & 0xff,
 *         mercsw 0x10053830 -> 0x1004eef0 -> 0x1004ef4c -> 0x10001375 without the + 0x100 of the mission's slots);
 * shade - the palette shade of its colour type 1 polygons (mercsw 0x1002f5e3): the planet's LITE (ambient, light
 *         vector, directional flag) and SKYGND.PAR's globalintensity. NULL: none (level 15 - 8 = the mid shade). */
typedef struct {
    int   on;          /* LITE read */
    int   light;       /* the light term on (0x10097cb4; off when LITE's vector is 999999 x 3) */
    int   dir;         /* LITE +22: directional (0x10248b00) */
    float amb;         /* LITE +20 ambient level (camera +0x2a -> 0x10248b0c) */
    float lvec[3];     /* LITE +8 / +12 / +16 (game coordinates, as stored) */
    int   gi;          /* globalintensity, clamped 0..128 (0x1002e8b7) */
    int   eye;         /* LITE vector 888888 x 3: a point light at the eye (0x100ad320) */
    float range;       /* LITE +24: the fog range (0x1009e99c; the shade less (depth / range) >> 4 when > 1e-7) */
} glr_ti_shade;
void glr_set_titanium_extra(glr *r, texture *const bank0[GLR_SLOTS], const glr_ti_shade *shade);

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
/* The 3D editions' palette fade (engine 0x1002ad50 -> DAT_1024e494, DAT_10160cf0): tint != NULL - every textured
 * fragment (objects, ground, sky, billboards, effects) times tint (slot 0xc INFRARED (0, 1, 8/255), slot 0x11 the red
 * flash (1, 8/255, 8/255)); pal / k - palette-coloured (untextured) polygons k of the way to their index's colour in pal
 * (k 0: off). Not used by the DOS look. */
void glr_set_world_fade(glr *r, const uint8_t pal[256][3], float k, const float tint[3]);
void glr_set_dos_palette(glr *r, const uint8_t pal[256][3]);
/* The 3D editions' damage shading (0x1002b9b0): a part whose mech3d_part.dmg_nib n is 1-15 (a mech location's damage,
 * the type-0x100 objects' nibble) is lit x (1 - n / 15), or x (1 + n / 15) (at most full) when sign (PLNT +52,
 * DAT_1024e428) is set. Not the DOS look, not Titanium models. */
void glr_set_damage_shade(glr *r, int sign);

#endif
