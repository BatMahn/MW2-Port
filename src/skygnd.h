/*
 * skygnd.h - per-mission sky/ground/fog settings (skygnd.par, 3D editions).
 *   mission=pink tile_sky=4.0 tile_ground=10680.6 anim_rate=0.0004 fog=0.000148
 * The 3dfx/ATi/S3 files have no fog field; only the PowerVR one does.
 *
 * How the 3dfx engine uses them (MW2.DLL 0x100277b0, 0x100278b0, 0x10027bb0):
 *   ground: quad at height 0, +-16,000,000 units, texture slot 0x08,
 *           texture repeats 0..tile_ground across it
 *   sky:    four sloped walls around the viewer (top edge at height 2.2M,
 *           64M out; bottom at -0.4M, 76.8M out), slot 0x8C, U 0..tile_sky,
 *           V 0..tile_sky*0.2, U scrolled by tile_sky*anim_rate every frame
 *   fog (PowerVR): density per SGL unit; SGL units = game units * 0.1
 */
#ifndef MW2_SKYGND_H
#define MW2_SKYGND_H

typedef struct {
    float tile_sky, tile_ground, anim_rate, fog;
    int   has_fog;
} skygnd;

/* defaults when a mission isn't listed: the 3dfx engine's own (4.0, 20000, 0.0001) */
void skygnd_defaults(skygnd *s);
/* Look up a mission (4-letter code, case-insensitive). Returns 0 if found. */
int  skygnd_lookup(const char *path, const char *mission, skygnd *out);

#define SKYGND_GROUND_SLOT 0x08
#define SKYGND_SKY_SLOT    0x8C
#define SKYGND_FOG_UNIT    0.1f   /* PowerVR density -> per game unit */

#endif
