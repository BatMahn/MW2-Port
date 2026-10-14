/*
 * fx.h - weapon effects (impacts, explosions, muzzle flashes), as the 3D editions' engine
 * builds them.
 *
 * Game data:
 *   MW2_SHT1 (BWD)  OBJ + XPLO pairs, one per pool slot: the OBJ's model is a flat card
 *                   (LAS0_0, XPL0_0, PPC0_0, MZL0_0, DEATH0_0...); XPLO payload
 *                   { i16 slot index, i16 texture slot, i16 effect type, i16 0 }.
 *   mw2_xpl1 (BWD)  texture slots: a run of BMPJ { u16 cel id, name } lists the frames, then
 *                   BMID { u16 slot } + BSEC { u16 slot, u16 rate } per slot.
 * Engine (3dfx MW2.DLL):
 *   0x10045d00      projectile class (weapon table visual) + what was hit -> effect type;
 *                   per type at most 2 alive within 5 m; a free pool slot of the type
 *   0x1025b168      per type: duration (ticks), sound, light flag (28-byte entries)
 *   0x1002ce50      frame stepping: every `rate` ticks the next frame, looping (32 max)
 */
#ifndef MW2_FX_H
#define MW2_FX_H

#include <stdint.h>
#include "prj.h"

#define FX_TYPES   32
#define FX_LIVE    256
#define FX_FRAMES  32
#define FX_CARD_SLOTS 16   /* pool slots kept per type for the 3D cards (MZL0: 16) */

/* hit flags (engine projectile flags) */
#define FX_HIT_UNIT    0x100
#define FX_HIT_STRUCT  0x200
#define FX_HIT_GROUND  0x400
#define FX_HIT_PLAYER  0x2000

typedef struct {
    int   cel[FX_FRAMES];   /* texture archive (CEL) ids */
    int   frames, rate;     /* rate: ticks per frame */
} fx_anim;

typedef struct {
    int     slots[FX_TYPES];          /* pool slots of each type (XPLO count) */
    int     anim[FX_TYPES];           /* texture slot of the type's first pool object, -1 none */
    float   half_w[FX_TYPES], half_h[FX_TYPES];   /* card size (cm) */
    /* a colour type 3 card (0x3xxx, drawn by the billboard 0x1002a8e0): its foot (texel u 0, v > 0) and top (u 0, v 0)
     * corners, cm from the effect's position (x 0..2: game x, y, z; the pool objects are unrotated, 0x10045c30) */
    int     bb[FX_TYPES];
    float   bb_foot[FX_TYPES][3], bb_top[FX_TYPES][3];
    /* a card drawn as a 3D polygon (colour 0x8xxx: MZL / JET, 0xbxxx: DEATH - 0x1002a4b0 -> 0x10026e80 -> 0x1002c880, not
     * the billboard): each pool object's polygons (up to 2, 3-4 corners) x its OBJ scale, in the object's frame (game x,
     * y, z), texel u / v as 0-1 of the bitmap, and each polygon's normal as the engine holds it (the file's negated,
     * 0x10042140) - drawn when (corner 0 - eye) . normal < 0 (0x1002b9b0). Per pool slot (the type's XPLO entries in
     * MW2_SHT1 order, 0x1003ed70): the slots' models differ (MZL0_2 / MZL0_3: one polygon, the others two) */
    int     card_n[FX_TYPES][FX_CARD_SLOTS], card_nv[FX_TYPES][FX_CARD_SLOTS][2];
    float   card_v[FX_TYPES][FX_CARD_SLOTS][2][4][3], card_uv[FX_TYPES][FX_CARD_SLOTS][2][4][2], card_nrm[FX_TYPES][FX_CARD_SLOTS][2][3];
    fx_anim tex[512];                 /* texture slots (u16, engine 0x1002d060: < 0x200) */
} fx_defs;

typedef struct {
    int     type, alive;
    int     slot;                     /* the pool slot of its type: the first free one (0x10045d00) */
    float   pos[3];
    int32_t start, end;               /* ticks */
    int     has_rot;                  /* rot: the object's orientation (row-major, game coordinates) - a muzzle flash's or
                                       * jet flame's, its mount node's (0x10044a70); otherwise unrotated (0x10045c30) */
    float   rot[9];
} fx_live;

typedef struct {
    fx_live e[FX_LIVE];
} fx_world;

/* Load MW2_SHT1 and mw2_xpl1 from the models archive. 0 on success. */
int  fx_load(prj_archive *models, fx_defs *d);

/* engine 0x10045d00: effect type for a projectile of weapon visual `visual` ending with `flags`;
 * -1 for none. */
int  fx_impact_type(int visual, int flags);

/* Muzzle effect types of weapon w (engine weapon table 0x1025a6c0 +0x08 / +0x0c), k = 0 or 1;
 * -1 none. Only types with pool objects show (0x17 ballistic, 0x1a missile, 0x1e PPC). */
int  fx_muzzle(int weapon, int k);
#define FX_JET 0x19   /* jump-jet flame (engine 0x1001bf90, at the two jet mounts while jetting) */

/* engine table 0x1025b168 */
int  fx_duration(int type);   /* ticks */
int  fx_sound(int type);      /* SNDS index, -1 none */
int  fx_light(int type);      /* lights its surroundings */

/* Start an effect at pos (cm, game coordinates) at time `now` (ticks). Returns its index or -1
 * when the type has no free pool slot or two of it are already alive within 5 m. */
extern int fx_detail_low;   /* DISPLAY DETAIL low: no neighbouring effects of a type within 5 m */
int  fx_spawn(fx_world *w, const fx_defs *d, int type, const float pos[3], int32_t now);
void fx_update(fx_world *w, int32_t now);
/* The cel id an effect shows at `now` (looping its animation), -1 if none. */
int  fx_frame(const fx_defs *d, const fx_live *e, int32_t now);

#endif
