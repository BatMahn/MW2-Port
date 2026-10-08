/*
 * bwd.h - MechWarrior 2 world/mission descriptions (BWD resources).
 *
 * A BWD record is a flat list of IFF-style chunks:
 *
 *   "BWD\0" u32 total_size u32 ?
 *   then chunks: tag[4] u32 size (size includes the 8-byte header) payload
 *
 * Validated on all 1,436 records in MW2.PRJ: every byte is accounted for by
 * the chunk list; 54 chunk types occur. Every record begins with REV ("1.22")
 * and DTBL.
 *
 * Missions are trees rooted at a scene record (xxxxSCN1). INCL chunks pull in
 * the other parts:
 *   INCL   u16 resource_id  char name[10]
 *          resource_id is the BWD id in MW2.PRJ (all 1,671 match their names);
 *          0xFFFE means a file on disk instead (USERSTAR.BWD - your lance,
 *          EN0nSTAR.BWD, INSTMAP1.BWD - written by the game at runtime).
 *   MUSI   u16 ?  char name[14]        CD track to play, e.g. "track23"
 *   NAVP   i32 x, i32 y, i32 z, 16 bytes (unknown), char name[44]
 *          Every nav point named in the 49 fixed-map briefings resolves to one.
 *          138 have names (player nav points); 438 are unnamed and their role
 *          isn't confirmed yet.
 *   ORDR   briefing / debriefing text with \n \t \c \s formatting codes
 *   OBJ    (52 bytes) a node of an object's skeleton:
 *          i16 index, i16 parent (-1 = root), i16 ?,
 *          i32 scale[3] (1 = unit), i32 rotation[3] (units unconfirmed; 0 in rest pose),
 *          i32 offset[3] from the parent (model units), i16 ?, i16 ?, i16 ?,
 *          u16 model (POLY resource id; "DUMMY" for mount points), i16 ?
 *          A mech record (e.g. TIMBRWLF) holds 5 skeletons, one per REPR chunk:
 *          detail levels 1-4 and a reduced 5th set; sets 2-5 reuse set 1's root.
 *          Verified by assembling the Timber Wolf from its parts.
 *   BMPJ   u16 cel_id, char name[]   } in a mission's xxxxMAP1 record (3D editions):
 *   BMID   u8 slot, u8 bank, u16 0xFFFF } the texture for each slot. Bank 1 =
 *          surface textures; bank 0 = effect animation frames (with BSEC/BMEN). A polygon's
 *          texture is map[(colour & 0xFF) + offset]; the engine (3dfx MW2.DLL
 *          0x10042000) adds a per-object offset at slot 0x00 (camo, slots 0-7)
 *          and 0x14 (clan insignia). Verified: mech body = 0x00, decals = 0x14.
 * PTBL (paths) and TSK (object tasks) are decoded below (bwd_ptbl_decode, bwd_tsk_decode; type 5 tasks run in
 * mech3d.c world3d_paths). Other chunk types (GT, MTBL, STAR, ...) are carried as raw chunks until they're decoded.
 */
#ifndef MW2_BWD_H
#define MW2_BWD_H

#include <stddef.h>
#include <stdint.h>

#include "prj.h"

#define BWD_EXTERNAL_ID 0xFFFE

typedef struct {
    char           tag[5];
    const uint8_t *data;   /* payload, points into the record buffer */
    size_t         size;   /* payload size */
} bwd_chunk;

/* Walk the chunks of one record. Returns the chunk count, or -1 if the record
 * is malformed. Pass chunks = NULL to count only. */
int bwd_chunks(const uint8_t *rec, size_t len, bwd_chunk *chunks, int max);

typedef struct {
    int32_t x, y, z;
    float   heading;     /* degrees (16.16 at +12): a start point's facing */
    int     radius;      /* low 16 bits at +24; x100 game units assumed (CYANLEV1 5000 = 5 km boundary) */
    int     type;        /* high byte at +24: 0x20 boundary, 0x21 shown nav, 0x01 goal, 0x02 start (observed) */
    int     shown;       /* i16 at +16: nonzero = a nav point the player can select (3dfx 0x1001cea0 skips 0) */
    int     team;        /* i16 at +22: the side that can select it (compared with the unit's +8) */
    int     flags;       /* u16 at +18: engine nav flags (3dfx 0x1003ee80 -> 0x101e4254); 0x100 = shown as
                            "Unknown" in the target viewer until 0x20 (reached) is set */
    char    name[45];
} bwd_navpoint;

typedef struct {
    int16_t  index, parent;
    int32_t  scale[3], rotation[3], offset[3];
    uint16_t model;      /* POLY resource id */
    int      coll;          /* payload +4, i16: collision type (engine OBJ handler 0x10043270 -> 0x1000ffd0):
                                0 standable box, 1 XZ box, 4 none, 5 polygon terrain, 6/7 special, -1 none */
    uint16_t objtype;       /* payload +44 (engine OBJ handler 0x10043270: chunk +0x34 -> object +2); bits 0xf0 = class,
                               0xC0 = removed at OBJECT DENSITY LOW (0x1000de50), 0x70 = kept with TERRAIN TEXTURES off */
} bwd_obj;

/* Chunk decoders. Each returns 0 on success, -1 if the payload is malformed. */
int bwd_incl(const bwd_chunk *c, uint16_t *id, char name[11]);
int bwd_navp(const bwd_chunk *c, bwd_navpoint *out);
int bwd_musi(const bwd_chunk *c, int *cd_track); /* "track23" -> 23 */
int bwd_obj_decode(const bwd_chunk *c, bwd_obj *out);

/* GPS: one game piece to spawn (enemy/ally mechs, turrets, doors, the player's star).
 *   u16 mek id, u16 skeleton (BWD id, e.g. 30 = MADDOG), u8 state table index,
 *   ..., at 0x1C: skeleton name, loadout (MEK) name, display name, pilot name.
 * Its start position: the scene's MTBL[table] -> u16 record id at offset 102 ->
 * that record's NAVP (e.g. CYANST11). Table 0 is the player's star (start ST01).
 * Verified on CYAN, PINK and TNJ1: every spawn resolves, and names match
 * (turrets at STTn, doors at STDn). Pieces of one group share the start point;
 * their spacing comes from formation tables (FTBL), not yet decoded. */
typedef struct {
    uint16_t mek_id, skeleton_id;
    int      table;
    char     skeleton[10], loadout[10], name[33], pilot[33];
    int      skill;          /* payload +8 (low byte): AI fire interval base (engine +0x130 -> +0x158 = skill + 1) */
    int      ranges[3];      /* payload +10/+12/+14: AI ranges in metres (0 -> 250); codes -4, -1, -2 */
    int      pilot_level;    /* payload +16 (low byte): 1-4, 0 -> 1 (engine +0x159, 0x1001ee30) */
    int      leader;       /* +0x0d: lance leader */
    int      is_player;    /* +0x0e == 0: the human player (engine 0x1003f305); 2 = AI */
} bwd_gps;
int bwd_gps_decode(const bwd_chunk *c, bwd_gps *out);

/* PTBL: a named path (engine chunk handler 0x10042b00 -> path table 0x101e8e20, 0x744 bytes per path, at most 64
 * paths): char name[64], then up to 64 points of 7 i32 (read as floats):
 *   x, y, z (game units, in the moving node's parent frame), pitch, yaw, roll (degrees), ticks (1/182 s) the segment
 *   from this point to the next takes (the last point's: the closing segment back to point 0, used by "loop") */
#define BWD_PATH_MAX_POINTS 64
typedef struct { float p[3], pitch, yaw, roll; int32_t ticks; } bwd_path_point;
typedef struct {
    char           name[65];
    int            count;
    bwd_path_point pt[BWD_PATH_MAX_POINTS];
} bwd_path;
int bwd_ptbl_decode(const bwd_chunk *c, bwd_path *out);   /* -1: not a PTBL, or over 64 points (the engine skips it) */

/* TSK: a task on an object (engine dispatcher 0x1003f140 -> handler table 0x1025a630 by type: 0 spin 0x1000e880,
 * 1 colour frames 0x1000e6e0, 2 circling 0x1000ea50, 3 mech animation 0x1000dec0, 4 sound 0x1000ec90, 5 path 0x1000ee10;
 * types 0-2, 4, 5 run in mech3d.c world3d_paths):
 *   i16 type, u32 period (ticks between runs, 0 = every frame: 0x100083d0), char text[]: "node;params" - node is the
 *   OBJ index in the same record (atoi -> 0x10043250), the task runs on that object's list (0x10025010).
 * Type 5 params "mode,rotate,path" (sscanf "%[^,],%[^,],%[^,]"): mode "loop" / "repeat" / anything else (the data's
 * "oneshot"); "rotate" or not; the PTBL name, looked up among the paths loaded so far. */
typedef struct {
    int      type;
    uint32_t period;
    int      node;           /* -1: none (no ';') */
    char     params[128];
} bwd_tsk;
int bwd_tsk_decode(const bwd_chunk *c, bwd_tsk *out);

#define BWD_TEXMAP_SLOTS 256
typedef struct {
    int      present[BWD_TEXMAP_SLOTS];
    uint16_t cel_id[BWD_TEXMAP_SLOTS];
    char     name[BWD_TEXMAP_SLOTS][13];
    int      count;
    /* bank 0 (BMID +1 == 0): the effect / scrub bitmap table (engine 0x1003e463 -> 0x1002d060 bank 0) - the colour
     * word type 3 (0x3240: a billboard, 0x10026fc7) draws bitmap (w >> 4) & 0xff of it; the frame named last before the
     * BMID (single-frame entries such as TNJ1SCRI's tan_rk / rrock / cact_2) */
    int      b0_present[BWD_TEXMAP_SLOTS];
    char     b0_name[BWD_TEXMAP_SLOTS][13];
} bwd_texmap;

/* Read the slot -> texture table from a MAP record payload. Returns 0 on success. */
int bwd_texmap_decode(const uint8_t *rec, size_t len, bwd_texmap *out);

/* A mission: the scene record and everything it includes, loaded. */
typedef struct {
    char      name[17];       /* resource name, e.g. "CYANWLD1", or file stem */
    int       external;       /* came from disk (id 0xFFFE) */
    uint8_t  *data;
    size_t    size;
    bwd_chunk *chunks;
    int        chunk_count;
} bwd_record;

typedef struct {
    bwd_record  *records;
    int          record_count;
    char         missing[16][11]; /* includes that couldn't be found */
    int          missing_count;
    int          music_track;     /* CD track, 0 if none */
    bwd_navpoint *navs;
    int          nav_count;
} bwd_mission;

/* Load a mission by scene name (e.g. "CYANSCN1"). External includes are read
 * through the datapath (dp_read_file); missing ones are listed, not fatal.
 * Returns 0 on success. */
int  bwd_mission_load(prj_archive *a, const char *scene, bwd_mission *m);
void bwd_mission_free(bwd_mission *m);

/* All ORDR text in the mission, in include order, concatenated. malloc'd. */
char *bwd_mission_text(const bwd_mission *m, const char *record_suffix);

#endif
