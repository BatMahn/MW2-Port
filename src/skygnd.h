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

/* The Titanium edition's 16-bit SKYGND.PAR (its engine mercsw.dll reads it at 0x10053b00 with one sscanf,
 * 0x1009e9e0): "mission=%4s globalintensity=%d tile_ground=%f grounddelta=%f ground_max=%f shadowtable=%d
 * gclip=%f,%f,%f amb=%d,%d,%d wa=%d,%d,%d,%c,%f,%f,%f wb=... fog=%d,%d,%d density=%f,%f,%f sky=%d,%d,%d skyAnim=%f,%f".
 * The text from a '#' on is dropped; the first matching line wins. The values as stored, scaled as the engine does
 * right after the read (0x10053d1c..0x10054097): */
typedef struct {
    int   found;
    int   intensity;                 /* globalintensity */
    float tile_ground;               /* tile_ground / 128 (0x10053d34): texture repeats across the ground quad's half */
    float ground_max;                /* ground_max x 2 (0x10053dbf): the ground quad's half size is this x 2 (0x1005548f) */
    int   fog[3];                    /* fog=R,G,B (0-255) */
    float dens_y, dens_m, dens_z;    /* density=Y,M,Z; Z x 10 (0x10053d22): the distance fog's full-fog depth */
    int   sky[3];                    /* sky=R,G,B: the clear colour */
    float anim[2];                   /* skyAnim / 181 (0x1005407f): the cloud layer's U, V drift per engine tick */
    int   amb[3];                    /* amb=R,G,B */
} skygnd_ti;
/* Look up a mission (4-letter code) in a Titanium SKYGND.PAR. Fields a line lacks (or that follow one sscanf could not
 * read) are 0; the engine keeps whatever the previous mission set there. Returns 0 if found. */
int  skygnd_ti_lookup(const char *path, const char *mission, skygnd_ti *out);

#define SKYGND_GROUND_SLOT 0x08
#define SKYGND_SKY_SLOT    0x8C
#define SKYGND_FOG_UNIT    0.1f   /* PowerVR density -> per game unit */

#endif
