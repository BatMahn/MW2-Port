/*
 * mech3d.h - assemble a mech (or any object with a skeleton record) for drawing.
 *
 * Reads the object's BWD record (e.g. "TIMBRWLF"), picks one of its skeleton
 * sets (representation 0 = full detail ... 3 = lowest detail, 4 = reduced
 * set; sizes come from the record's DTBL), places each node by summing
 * offsets up the tree, and loads the POLY
 * model of every node that has one. Rest pose only: node rotations aren't
 * applied yet (they're zero in the stored pose).
 */
#ifndef MW2_MECH3D_H
#define MW2_MECH3D_H

#include "prj.h"
#include "wtb.h"

typedef struct {
    char      model_name[17];
    int32_t   pos[3];        /* world position of the part's origin */
    float     yaw;           /* rotation about the vertical axis, degrees (world objects) */
    int       has_rot;       /* 1: rot[] (row-major 3x3, game coords) replaces yaw */
    float     rot[9];
    int       node;          /* skeleton node this part hangs on (-1 for world objects) */
    int       group;         /* damage location 1-8 from the record's OBJL chunks (0 = none):
                                1 head, 2 RT, 3 CT, 4 LT, 5 RA, 6 LA, 7 RL, 8 LL (engine 0x100171d0; TW1_RARM is group 5) */
    wtb_model model;
    int       flat;          /* draw untextured, in each polygon's palette colour (cockpit frame) */
    int       objtype;       /* world objects: OBJ type word (bwd_obj.objtype); 0 for mech parts */
    int       parent_obj;    /* world objects: the OBJ parent's index in the same record (-1 none) */
    int       gt_debris;     /* world objects: GT flag 0x8000 - flies off as debris when its parent is destroyed */
    int       hidden;        /* not drawn and not solid (a building's destroyed variant before, its intact one after) */
    unsigned  tint;          /* 0x1RRGGBB: drawn in this colour (the target viewer's damage colours); 0 = its own */
    int       sparse;        /* world objects: an OBJECT DENSITY object (type 0xc0), hidden while the density is LOW */
    char      rec[11];       /* world objects: the record that placed it, and its OBJ node index there (GT A / B) */
    int       obj_index;
    int       tex_offset;    /* added to texture slots 0x00 (camo) and 0x14 (insignia): the owner star's STAR value
                                (engine 0x10042000: star record +0, set from the STAR chunk's first u32, 0x10042d40) */
    int       wire_hi;       /* image enhancement: drawn in the mechs' blue (an active building) */
    int       wire_dmg;      /* image enhancement: a mech location's damage colour, 0 unset, 1 blue, 2 red, 3 yellow, 4 colour 8 (dark red, a wreck) */
    int       shadow;        /* a flat shadow piece on the ground: drawn as a translucent darkening */
    int       coll;          /* world objects: OBJ collision type (0 box, 1 XZ box, 4 none, 5 terrain...); -1 none */
    /* collision hull (engine type 2, 0x1000d6b0): face planes in model space, built lazily */
    float    *hull;          /* plane_count x {nx, ny, nz, d}: inside when n.p <= d for all */
    int       hull_count;
    float     hull_center[3], hull_radius;   /* bounding sphere (model space), broad phase */
    int       sphere_ok;     /* hull_center / hull_radius computed */
    int       moving;        /* world objects: carried by a path task (PTBL, world3d_paths) - pos / rot change every step:
                                left out of the static world mesh and collision lists, drawn and hit-tested as they are */
} mech3d_part;

#define MECH3D_MAX_NODES 64
#define MECH3D_MAX_BINDS 32

typedef struct {
    mech3d_part *parts;
    int          part_count;
    int          node_count;     /* including mount points (DUMMY) */
    int          repr_count;     /* skeleton sets in the record */
    /* skeleton, kept for posing (mech3d_load only) */
    int          skel_count;
    int16_t      skel_index[MECH3D_MAX_NODES], skel_parent[MECH3D_MAX_NODES];
    int32_t      skel_offset[MECH3D_MAX_NODES][3];
    int          bind_count;     /* TSK "node;70,flags,track" */
    int16_t      bind_node[MECH3D_MAX_BINDS], bind_track[MECH3D_MAX_BINDS];
    int          anim_id;        /* ANIM resource id from the record's ANIM chunk, 0 if none */
    int          anim_rate;      /* TSK duration: ticks per key (x1.5 / x1 / x0.75 by gait, engine 0x1000e600) */
    int32_t      origin[3];      /* added to every posed part (placement in a world) */
    float        heading;        /* yaw of the whole mech, degrees */
    int          twist_node;     /* torso node (carries the *_HEAD part), -1 if none */
    float        twist;          /* torso twist relative to the legs, degrees */
    const void  *pose_anim;      /* the last mech3d_pose arguments (an anim_set *), for a level of detail posed alike */
    float        pose_t;
    int          eye_node;       /* EYEO chunk: the skeleton node of the pilot's eye (-1 if none) */
    float        eye_pos[3];     /* its world position after mech3d_pose */
    int          eye_ok;
    /* POFO {i16 node, i16 slot} (engine 0x1003eb09: object +0x68[slot]): mount points. Slots by the skeletons'
     * geometry (Timber Wolf, Mad Dog): 0 head, 1 right torso, 2 centre torso, 3 left torso, 4 right arm, 5 left arm,
     * 6 / 7 the jump jets (right / left) */
    int          pofo_node[8];   /* -1 = none */
    float        mount_pos[8][3];/* world positions after mech3d_pose */
    unsigned     mount_ok;       /* bit per slot */
    float        mount_local[8][3];   /* rest pose, heading 0, origin 0 (filled by mech3d_load) */
    float        mount_from[3];       /* the origin mount_pos was posed at */
    unsigned     mount_local_ok;
} mech3d;
/* the mount slot for a weapon in MEK location loc (H, LT, CT, RT, LA, RA, LL, RL), -1 if none */
int  mech3d_mount_slot(int loc);
/* a mount's world position for the mech placed at origin with heading: from its last mech3d_pose (animation and
 * torso twist included, moved to origin) when it has been posed, else the rest pose turned to heading; returns 0,
 * or -1 if the mech has no such mount */
int  mech3d_mount_world(const mech3d *m, int slot, const float origin[3], float heading, float out[3]);

void mech3d_use_skeleton(mech3d *dst, const mech3d *src);   /* dst's meshes posed on src's skeleton (the engine's LOD swap) */
int  mech3d_load(prj_archive *a, const char *record, int repr, mech3d *out);
/* Combat Variables DISPLAY DETAIL: 0 high, 1 low (engine 0x1002ff90: detail level 1 / 2). Loads asking for representation
 * 0 use this one instead when the model has it. */
extern int mech3d_lod;
/* Combat Variables OBJECT DENSITY LOW (MW2SND.CFG +0x20 = 0): world objects of class 0xC0 (OBJ type word & 0xf0) are left
 * out, as the engine's 0x1000de50 takes them off both the drawn and the collision lists (DOS MW2.EXE 0x1fbf0 the same).
 * In the data: gates, fences, lights, trees, spires, small buildings, some hills and craters. */
extern int world3d_density_low;

struct anim_set_s;
/* Pose the skeleton at animation time t (keys) and update every part's
 * position/rotation (rest pose if anim is NULL). */
#include "anim.h"
void mech3d_pose(mech3d *m, const anim_set *anim, float t);
/* Relative chance of each damage location (index 0-7 = location 1-8) being struck by a
 * shot aimed at the mech: each OBJL group's front-view area in the rest pose. */
void mech3d_hit_weights(const mech3d *m, float w[8]);
/* Cockpit point: the EYEO node's posed position (engine: EYEO -> object +0x44), else the centre
 * of the posed head-group parts (OBJL group 1: canopy / windshield) in world
 * coordinates. Returns 0, or -1 if the mech has no head group. */
int  mech3d_head_point(const mech3d *m, float out[3]);
/* Engine projectile collision (0x100103e0 / 0x10029db0 / 0x10010650): the segment o..o+d is
 * sampled at 4 points (k/4, k = 1..4); a hit is a sample inside a posed part's convex hull,
 * after a bounding-sphere check. Returns the hit part's OBJL group (3 if none), 0 for no hit,
 * and the sample fraction in *t. */
int  mech3d_hull_hit(mech3d *m, const float o[3], const float d[3], float *t);
/* Average forward body travel per key over keys [first, first+count): how far
 * the planted (lowest bound) foot moves backwards. Used to time the animation
 * to ground speed so feet don't slide. */
float mech3d_stride(const mech3d *m, const anim_set *anim, int first, int count);

/* Walking speed (units/s) for a loadout (MEK record, e.g. "TBR00STD"): walk MP
 * x 10.8 km/h (BattleTech scale) at ~1 cm per unit = MP x 300. And the matching
 * animation rate for walk sequence 0. Returns 0 on success. */
int   mech3d_walk_rate(prj_archive *a, const mech3d *m, const anim_set *anim, const char *loadout,
                       float *speed, float *keys_per_s);
/* Standard loadout for a skeleton record via the MTAB mech table ("TIMBRWLF" ->
 * "TBR00STD"). Returns 0 on success. */
int   mech3d_default_loadout(prj_archive *a, const char *record, char *out, size_t outlen);

/* World objects of a mission area record (e.g. "CYANARE1"): every OBJ node with
 * a model, placed by its parent chain. Rotations are degrees in 16.16 about the
 * vertical axis. Damaged/destroyed variants (model names "SA...", placed at the
 * same spot as the intact "S_..." building) are skipped unless with_damaged.
 * Parts are appended to `out` (call with a zeroed mech3d to start). */
int  world3d_append(prj_archive *a, const char *record, int with_damaged, mech3d *out);

/* Whole mission world from its scene record (e.g. "CYANSCN1"): every record in
 * the include tree that places objects (OBJ chunks) and isn't itself an object
 * definition (no REPR chunks: mechs, vehicles, turrets). Returns records used. */
int  world3d_mission(prj_archive *a, const char *scene, int with_damaged, mech3d *out);

/* Place the mission's spawned pieces (GPS records: mechs, turrets, doors, the
 * player's star) at their start nav points, appending their parts to `out`.
 * Group formation: the scene's STAR chunk names one per state table; it is
 * looked up in the mission's FTBL chunks, then FORMATNS. The leader stands at
 * the start point, follower n at offset n-1 with the formation's facing.
 * Returns the number of pieces placed. */
int  world3d_spawns(prj_archive *a, const char *scene, mech3d *out);
/* A follower slot (1..) of a named formation (FTBL in FORMATNS, e.g. "echelonl"), relative to the leader's facing
 * (the convention of world_actor.form_x / form_z). Returns 0, or -1 if there is no such slot. */
int  world3d_formation(prj_archive *a, const char *name, int slot, float *x, float *z);

/* Path tasks (TSK type 5, 3dfx MW2.DLL 0x1000ee10, the same in DOS): an OBJ node of a world record follows a PTBL path,
 * carrying its children. Created at load (0x10008280 runs the handler with mode 0: start = the load time, the position
 * filters at point 0, the angle filters at 0), run every `period` ticks while the object is active (0x10025040 skips
 * objects flagged 0x800). Each run: the segment i holding (now - start) by the points' tick counts, the target
 *   p[i] + (p[i+1] - p[i]) * f (the last point's segment closes back to point 0), angles (pitch, yaw, roll) = point i's;
 *   "rotate": yaw += atan2(dx, dz) of the segment (45 when both are 0, 0x10030120), pitch -= asin(dy / 65536) deg;
 * each value through a filter (0x1003b7e0 / 0x1003b900 for angles): value += dt * (target - value) / (0.3 x 181 ticks),
 * snapped when that overshoots; the node's local translation = the values, its local rotation Ry(yaw) Rx(pitch) Rz(roll)
 * (0x1000f820 / 0x1000f990 -> 0x1002f070). At the last point: oneshot ends the task (the node stays where it is),
 * "repeat" snaps back to point 0 and starts again (filters reset), "loop" runs the closing segment and wraps.
 * A destroyed intact object's tasks end (0x10025180 -> 0x100250a0, also its descendants: 0x10025310), and its destroyed
 * variant takes its world matrix (0x1000fce0 -> 0x1000fcc0).
 * The other object tasks (handler table 0x1025a630; type 3 0x1000dec0 is the mechs' animation binding, never in world
 * records) run on the same scheduler (0x100083d0: when now >= due, due = now + period; created at load, start 0):
 *   0 spin (0x1000e880) "a,b,c,s": the node's local rotation x= Rpyr(a, b, c) x elapsed / (s x 181) per run - in its own
 *     frame (0x1000f9d0 -> 0x1002ed60: local x M); s 0 -> 1 s. The training targets (TRNJ/TRNW TGT1-3), CIND, FUCH,
 *     PUCE, UMBE.
 *   1 colour frames (0x1000e6e0) "f0,f1,...": up to 16 palette indices; every polygon i of the node's own model gets the
 *     colour word f[(i + now / period) mod n] << 4 (type 0, flat palette colour: 0x10029970) - dropship exhausts,
 *     the targets' blinking, lights; 40 records.
 *   2 circling (0x1000ea50) "r,s,on[,x]": each run the node steps d = 2 pi r / s x dt (seconds) along (cos a, 0, -sin a)
 *     in its parent's frame (0x1000f850) and turns 360 / s x dt degrees of yaw in its own frame; a (from 0) adds the
 *     yaw - a circle of radius r every s seconds (birds, aircraft: BRON, BROW, CHED, GREY, JACK, SHEP, TEAL); on 0 ends it.
 *   4 sound (0x1000ec90) "range,name[,on]": a looping sound at the node (world3d_paths_sounds; the caller plays it). */
/* The engine's blown-off locations (0x10016640 -> 0x1000ff70 -> 0x10015530 / 0x10015410): destroying the head (1),
 * centre torso (3), an arm (5, 6) or a leg (7, 8) sends the node carrying that location's part flying, with everything
 * hanging under it; a side torso (2, 4) only takes its arm along and stays. Returns the location (1-8) in `mask` (bit
 * per location) whose node is part q's own or nearest such ancestor, 0 if none - the part is not drawn. */
#define MECH3D_FLY_LOCS ((1u << 1) | (1u << 3) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8))
int  mech3d_fly_loc(const mech3d *m, int q, unsigned mask);
/* the mask for a unit's destroyed locations (loc_gone[0..7]) */
unsigned mech3d_fly_mask(const int loc_gone[8]);

typedef struct world3d_paths world3d_paths;
/* The mission's path tasks over `world` (from world3d_mission of the same scene): marks the carried parts `moving`.
 * NULL if the mission has none. */
world3d_paths *world3d_paths_load(prj_archive *a, const char *scene, mech3d *world);
int  world3d_paths_tasks(const world3d_paths *p);
/* Run the tasks at time `now_ticks` (1/182 s since the mission start) and place the carried parts. Parts whose tasks
 * have all ended stay where they are and lose `moving` (fixed world objects again: the caller rebuilds its static mesh and
 * collision); returns how many did. */
int  world3d_paths_step(world3d_paths *p, mech3d *world, float now_ticks);
/* World part `intact` was destroyed: its tasks (and its descendants') end; when it was carried, world part `destroyed`
 * (-1 none) and its children are placed at its current world transform. Returns 1 if anything was moved. */
int  world3d_paths_destroyed(world3d_paths *p, mech3d *world, int intact, int destroyed);
void world3d_paths_free(world3d_paths *p);
/* The running sound tasks (TSK type 4, 0x1000ec90 -> 0x10032cc0): the node's world position now, the audible range (cm),
 * the SNDTABLE name, and a stable id (the task). Returns how many (at most max). */
typedef struct { float pos[3]; int range, task; char name[24]; } world3d_sound;
int  world3d_paths_sounds(const world3d_paths *p, world3d_sound *out, int max);

/* The same pieces as separate, posable actors (origin + heading set, rest pose). */
typedef struct {
    mech3d   mech;
    anim_set anim;
    int      have_anim;
    int      table;          /* state table (MTBL) index; 0 = the player's star */
    char     loadout[10];    /* MEK record, e.g. "mdg00std" */
    char     name[33];
    float    speed;          /* walking speed, units/s (from the loadout's walk MP) */
    float    keys_per_s;     /* animation rate that matches that speed (no foot sliding) */
    float    t;              /* current animation time, keys */
    int      leader;         /* first piece of its group */
    int      friendly;       /* on the player's side: from the player's lance file (USERSTAR, file 0), or its star's alliance 0 */
    int      alliance;       /* its star's alliance (STAR entry +4: 0 the player's side, 1 enemy, 2 neutral - e.g. the
                                training instructor, who doesn't fight unless provoked) */
    float    form_x, form_z, form_yaw;   /* formation slot relative to the leader */
    char     group[17];      /* the record that spawned it (e.g. "CYANENS1") */
    char     skel[17];       /* skeleton record (e.g. "MADDOG"), also the MGEO record name */
    int      ai_skill;       /* GPS: fire interval base */
    int      tex_offset;     /* its star's camo / insignia slot offset (STAR chunk entry +0; 0 = the player's star) */
    int      ai_level;       /* GPS pilot level 1-4 */
    float    ai_range[3];    /* GPS ranges, cm: [0] code -4, [1] code -1, [2] code -2 */
    unsigned obj_class;      /* GPS +0x18: the class the objective nodes attach the unit by (0x1003f38b / 0x1003ff60) */
    int      unit_class;     /* the skeleton record's GP chunk +2 (engine 0x1003e740: the class table 0x1024c330, 0x20 per
                                entry): 1 mechs, 2 hovercraft, 4 tanks, 5 aircraft, 8 dropships - all driven by the mech
                                controller / locomotion (0x1001a180 / 0x10019310); 3 turrets (0x10021a50 / 0x100219e0: fixed
                                in place, only twist and pitch turn) and 7 doors (0x10021f20 / 0x10021e60) have their own */
} world_actor;
int  world3d_actors(prj_archive *a, const char *scene, world_actor **actors, int *count);
void world3d_free_actors(world_actor *actors, int count);
/* Shallow-combine the actors' parts into one mech3d for drawing (models are
 * shared, so release with free(out->parts) only). */
int  world3d_compose(const world_actor *actors, int count, mech3d *out);
void mech3d_free(mech3d *m);

#endif
