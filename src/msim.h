/*
 * msim.h - a mission running: the MTBL interpreter driving the spawned groups.
 *
 * The engine's AI reads each group's current order node (0x10014520); this is a
 * simplified stand-in for that AI: a group leader turns toward the target of
 * its current "reach"/"leave" order (a nav point) or "destroy" order (another
 * group, or UserStar = the player) and walks at its loadout speed; followers
 * hold their formation slots. No combat, terrain or collision yet. Nav radius
 * defaults to 20,000 units as in the engine (0x10009b80).
 */
#ifndef MW2_MSIM_H
#define MW2_MSIM_H

#include "ai.h"
#include "combat.h"
#include "mech3d.h"
#include "mtbl.h"
#include "prj.h"

#define MSIM_MAX_NAVS 64

/* flying debris (engine 0x10015230 / 0x100152e0 / 0x100156f0): chunks thrown by explosions */
#define MSIM_MAX_DEBRIS 128
typedef struct {
    int     kind;            /* 0 / 1: CHUNKER1 / CHUNKER2; 2: a blown-off part (actor, part); 3: a world debris node (world part) */
    int     actor, part;     /* kind 2: actor index (-1 = the player) and its part index */
    float   p[3], v[3];      /* cm, cm/tick */
    float   ang[3], spin[3]; /* degrees, degrees/tick */
    int32_t until;           /* ticks */
    int     moving;
} msim_debris;

typedef struct {
    char  name[17];          /* the BWD record name (YELLNAV1): what MTBL orders refer to */
    char  label[22];         /* NAVP name ("Nav Epsilon"), 21 chars like the engine (0x1003ee80); "" = unnamed */
    int   shown;             /* NAVP +16 nonzero: a nav the player can reach / see (0x10009b80 tests it) */
    int   selectable;        /* the player can cycle to it: NAVP +16 nonzero and +22 == 0 (3dfx 0x1001cea0) */
    float x, y, z, radius;   /* y: the nav point's height (NAVP; the altitude tape's target marker) */
    unsigned cls;            /* NAVP +0x1a: the objective nodes attach it by this class (0x1003ff60) */
    int   visited;           /* the player has reached it (0x10009b80 flag 0x20: scan nodes) */
} msim_nav;

#define MSIM_MAX_SHOTS 768

typedef struct {
    float x, y, z, vx, vy, vz;   /* cm, cm per tick */
    float ay;                    /* vertical acceleration, cm/tick^2: -drop flag x planet gravity */
    float life;                  /* ticks left */
    int   weapon, owner;         /* owner: actor index, -1 = the player */
    int   homing;                /* guided at this actor (-1 the player, -2 none): engine 0x10045910 */
    float age;                   /* ticks flown (guidance starts after the first, +0xc0) */
    int   prox;                  /* +0x38 bit 0x8000: guidance found it within 1 m of the target's position (0x10045910);
                                  * the next tick it hits the target there (0x10044f60) */
    unsigned id;                 /* serial number, 1.. (shots are not kept in order: the camera follows one by id) */
    float aim_o[3], aim_d[3];    /* TEST ONLY: the player's aim line when it was fired (the eye, the reticle's direction) */
} msim_shot;

typedef struct {
    mtbl_logic   logic;
    world_actor *actors;
    int          actor_count;
    msim_nav     navs[MSIM_MAX_NAVS];
    int          nav_count;
    float        player[3];      /* the player's position (game units) */
    combat_unit *units;          /* one per actor (owned) */
    int         *armed;          /* actor has a loadout */
    combat_unit  player_unit;    /* the player's mech (TBR00STD unless set) */
    int          player_attacks; /* 1: the player fires (at player_target, or the nearest enemy in range) */
    uint32_t     player_fire_mask; /* weapons that fire (bit i = weapon i); 0 = all */
    int          player_target;  /* actor index the player aims at, -1 = nearest */
    float        player_heading; /* the start point's facing (degrees) */
    float        player_facing;  /* the player's current heading (updated by the front end) */
    unsigned     rng;
    msim_shot    shots[MSIM_MAX_SHOTS];
    int          shot_count;
    mech3d       pl_mech;        /* the player's collision model (Timber Wolf) */
    int          pl_mech_ok;
    float       *height;         /* per actor: model height (cm), for muzzle and aim points */
    int32_t     *wreck_until;    /* per actor (+1 for the player, last): the death sequence's end, ticks */
    float       *twist_limit;    /* per actor: MGEO +0x14 (deg); 361 = turrets turn freely */
    int          hits, misses;   /* projectile statistics */
    ai_library   ai;             /* the AIT programs */
    float      (*obstacles)[4];  /* building footprints: min x, max x, min z, max z (cm) - broad phase */
    int         *obstacle_part;  /* world part index for each footprint (faces tested) */
    const mech3d *world;         /* the mission's world (not owned; must outlive the sim) */
    int          obstacle_count;
    int          flank_slots[8]; /* attackers per slot around the player */
    float       *avoid_turn, *avoid_thr, *avoid_next;   /* per actor: held avoidance output */
    float       *air_speed;      /* per actor: horizontal speed carried while airborne (cm/s) */
    int         *avoid_side;
    ai_mind     *minds;          /* per actor */
    int          ai_ok;
    combat_planet planet;        /* the mission's PLNT values */
    /* called for every projectile launched; first = the volley's first (weapon sound) */
    void       (*on_shot)(void *user, int weapon, int owner, int first, float x, float z);
    /* weapon effects (fx.h): type and position (cm, game coordinates) - impacts on mechs and the
     * ground (engine 0x10045d00), and a mech's death explosion */
    void       (*on_effect)(void *user, int type, const float pos[3]);
    int          fx_refused;      /* set by on_effect when the effect pool refused the effect (0x10045d00: a neighbour of the
                                  * type within 5 m, or no free slot) - then neither the effect nor its chunks */
    void       (*on_sound)(void *user, int sound, const float pos[3]);   /* a positional sound at pos (footsteps ...); -1 = the collision clang; pos NULL = a cockpit sound at volume 0x32 */
    void       (*on_radio)(void *user, const char *sound);   /* a mission radio message (SNDS name), voice channel */
    int         *step_key;        /* per actor: the walk-cycle key last seen (footsteps) */
    int         *anim_seq;        /* per actor: the sequence playing (walk / run / reverse); a change restarts it */
    int         *bump_with;       /* per actor: the mech it is in contact with (+2; 1 = the player), 0 none */
    int          no_player_muzzle;   /* the viewer's cockpit: the player's own flashes would fill the view */
    void        *shot_user;
    unsigned     player_class;   /* USERSTAR GPS +0x18: UserStar's class for the objective nodes */
    char         player_skel[17], player_loadout[17];   /* from USERSTAR.BWD (shell), else Timber Wolf */
    int32_t      now;            /* ms */
    /* mission end (engine 0x10009f50 / 0x100374e0): the player's star (table 0) result or the player's
     * destruction starts a 1820-tick (10 s) end sequence, then the mission is over */
    int          outcome;        /* 0 running / none, 2 successful, 3 failed, 4 time exceeded */
    int          ending, over, player_lost;
    int          debug_invulnerable;   /* tests only: the player's destruction is ignored */
    int32_t      end_at, ended_at;   /* ms */
    /* ejection (player_unit.ejected / intact_out, combat.h): state 5 holds the end back 0x389 ticks - the controller
     * (0x1001adf0) sets the end flag DAT_1024c568 only then, the end sequence (0x100374e0) follows */
    int32_t      eject_at;       /* sim ms the player went out in state 5 (0 none) */
    int          player_counted; /* the player's own loss is in the career counters (0x10016280: +0x34 / +0x36) */
    unsigned     shot_serial;    /* the last msim_shot id given */
    unsigned     player_last_shot;   /* id of the player's newest projectile (engine DAT_1025a69c when a missile) */
    void (*log)(void *user, const char *line);
    void        *log_user;
    int          player_vertical; /* 1: msim_step stands the player on the terrain (headless tools) */
    uint8_t      career[80];   /* MW2CAR.CFG counters */
    int          music_track;
    int          chunky;       /* Combat Variables CHUNKY EXPLOSIONS (MW2SND.CFG +0x24): debris chunks; 0 = none (ASSUMED meaning) */
    int          collision_damage, difficulty;   /* Combat Variables (MW2DIF.CFG) */  /* the mission's CD track (MUSI -> MUS "trackNN" = "NN 0"), 0 = none */
    msim_debris  debris[MSIM_MAX_DEBRIS];
    int          debris_count;
    void        *terrain;        /* walkable surfaces from the world (types 0 and 5), see msim_ground */
    /* Lancemate orders (engine 0x10014390 / 0x10014290; the 3D edition's COMMAND COMPUTER, 0x10260950..) */
    prj_archive *arch;
    float       *online_at;      /* per actor: sim ms the mech comes online (start-up, engine 0x1001a180 state 0 -> 2) */
    float        player_online_at;
    unsigned    *fly_seen;       /* [actor + 1] (0 the player): the blown-off locations already sent flying (blow_offs) */
    float       *radius;         /* per actor: contact radius, cm (MGEO int[6]; engine 0x1000ba20 touches at r1 + r2) */
    float        player_radius;
    float        player_vel[3];  /* the player's velocity, cm/tick (x, y, z; set by the front end): mech contacts use the
                                  * relative speed (engine 0x1000c160) */
    int          fx_light_ok;
    int         *ai_focus;       /* per actor: the attacker it turned on (0x10012520): -1 the player, -2 none */
    int         *ai_who;
    int         *powered_down;   /* per actor: shut down by a rest / hold node (states 10 / 11, mech +0xa0 |= 3) */
    int          sd_at_once;     /* the DOS rule for input +0x43 (self-destruct): destroyed at once (MW2.EXE 0x27785 ->
                                    0x265f0); else the 3D editions' state 7, 0x16a ticks first (0x1001a180) */
    int32_t     *sd_at;          /* per actor: the self-destruct's time (sim ms; 0 none) */
    int         *seen_node;      /* per actor: the star's node last applied (0x10014520 runs on a change) */         /* per actor: whom it attacks (state 3), -1 the player, -2 none */    /* PLNT payload +0x38 == 0: explosions may take the scene light (0x1003de48 -> DAT_1025a6a8) */
    float       *centre_h;       /* per actor: centre above the feet, cm (MGEO int[0]: the climbable rise) */
    float        player_centre_h;
    int         *cmd;            /* per actor: commanded AI state (2 attack, 5 follow, 7 patrol, 8 godirect, 11 shutdown), 0 none */
    int         *cmd_target;     /* per actor: the commanded target actor (attack / defend), -1 none */
    int         *engage;         /* per actor: engine +0x154 "engage at will" (0x100124a0: 0 in the player's star, 1 elsewhere) */
    int         *mate_target;    /* per actor: the enemy it was last fighting (engine condition 6, 0x10013090), -1 none */
    int         *defend_leg;     /* per actor: Defend patrol point 0-3 around the defended mech (0x10013f00), +4 once one is reached */
    int          star_post;      /* a starmate's sighting posted to the player (its leader): target actor + 2, 0 = none */
    int          player_victim;  /* the unit the player last hit (engine 0x10014110: the player's record "attacking" it), -1 */
    int          radio_busy;     /* set by the front end while mission radio is queued or playing (DAT_1024f6bc) */
    int32_t      end_flush_at;   /* the end sequence's radio flush time (0x10031640, 264 ticks in), ms */      /* a starmate posted a sighting to its leader, the player (0x100141e0), this step */
    int          player_nav;     /* the player's selected nav point (index into navs), -1 = none: Disengage's goal */
    int          star_formation; /* the player's star formation (0..5 echelon left .. wedge), -1 = as spawned */
    int          last_cmd[8];    /* per point (0 = all): the last command's menu index (0x10264468; 7 = "No Cmd") */
    void       (*on_message)(void *user, const char *text, int voice);   /* order acknowledgements (0x10031810) */
    /* destructible world objects: GT chunks {i16 intact node, i16 destroyed node, .., +16 i16 hit points, +18 type,
     * +20 flags, +24 name[22], +46 sub-name[22]} (engine OBJ/GT handler 0x1003eb5c -> thing table 0x101e0280:
     * +0 flags, +8 hit points; damage 0x10046c20; destroyed: explosion 0x10045c30, swap 0x10046a90) */
    struct msim_building { int intact, destroyed, hp, type, inspected; char name[23], sub[23]; float mn[3], mx[3]; } *bld;
    int          bld_count;
    int          world_dirty;    /* a building changed: the front end re-uploads the world mesh */
    struct world3d_paths *paths; /* path tasks (PTBL): world parts flagged `moving`, placed every step (NULL: none) */
    char         scene[17];
    int          world_rebuild;  /* the collision surfaces are rebuilt at the next step */
    int          buildings_destroyed;
    uint8_t     *inspected;
    /* missile lock (engine 0x10044560): set by the front end - the selected weapon (player_unit slot) and the aim
     * (torso facing / pitch, degrees); kept here - player_lock: bit 0x8000 target in the 16-degree window and range,
     * 0x40 acquiring, 0x80 locked (after 0x16a ticks) */
    int          player_sel_w, player_lock_target;   /* -1: none */
    int          player_aim_free;  /* 1: with no target the player fires along the aim (the front end; missionsim keeps the nearest enemy) */
    float        player_aim_yaw, player_aim_pitch;
    float        player_eye[3];    /* the aim ray's origin: the player's eye object (unit +0x44, the EYEO node - the camera's), */
    float        player_conv;      /* the convergence range last set by the aim ray (unit +0xb0; 0 = never: 150000) */
    int          player_eye_ok;    /* world cm, set by the front end each frame (0x10044950); 0: the body point pl_h x 0.7 */
    int          player_lock;
    float        player_lock_ticks;
    float       *ai_lock;         /* unused: the AI's missile lock is ai_mind.lock / lock_ticks (engine 0x10044560) */
    float       *provoked;        /* per actor: seconds left of "my group is under attack" (engine 0x10014110 / 0x10012de0) */         /* per actor: ticks its target has been held (AI lock, ASSUMED: the same 2 s) */
    mech3d       pl_shown;        /* the player's mech as drawn (posed with animation and twist): its mounts */
    int          pl_shown_ok;      /* per actor: inspected by the player's side */
    void        *message_user;
} msim;

/* Lancemate orders. point 0 = all points, 1 = Point 2, 2 = Point 3 ... (the lance position, engine object +0xc).
 * order = the AI state the engine commands: MSIM_CMD_*; target = the player's selected target (actor index, -1 none).
 * Returns 1 if the order was given, 0 if not (no such point, or attack/defend without a target). */
enum { MSIM_CMD_ATTACK = 2, MSIM_CMD_ENGAGE = 3, MSIM_CMD_JOIN = 5, MSIM_CMD_DEFEND = 7, MSIM_CMD_DISENGAGE = 8,
       MSIM_CMD_SHUTDOWN = 11 };
int  msim_command(msim *s, int point, int order, int target);
/* actor index of a lance point (1 = Point 2 ...), -1 if there is none or it is destroyed */
int  msim_point_actor(const msim *s, int point);
/* the COMMAND COMPUTER "Status:" index (engine 0x10064570: AI state + 1; 13 Destroyed), 0 = none */
int  msim_point_status(const msim *s, int point);
/* Change Formation (engine 0x10064540 -> 0x10018c70): 0 echelon left, 1 echelon right, 2 line abreast,
 * 3 line astern, 4 vee, 5 wedge (FORMATNS FTBL echelonl .. wedge). Returns 0 on success. */
int  msim_set_formation(msim *s, int formation);
/* INSPECT_TARGET (engine action 0x12 -> input +0x3a, handled at 0x1001d5xx): the player's side inspects the target -
 * a mech (actor >= 0) or a structure (thing >= 0, index into bld) - when within its radius + 200 m; marks it for the
 * scan objectives (MTBL_K_SCAN). Returns 1 "Inspection successful", 2 "Target is beyond inspection radius", 0 nothing
 * (no target, or already inspected). */
int  msim_inspect(msim *s, int actor, int thing);
/* the structure nearest the aim line within cone degrees (for TARGET_AT_RETICLE), -1 none */
int  msim_aim_thing(const msim *s, float yaw_deg, float cone_deg, float *distance);
/* the engine's weapon table +0x18: the weapon is guided (LRMs, Streak SRMs, Narc) */
int  msim_weapon_guided(int weapon);
/* tests: damage destructible object b (index into bld) */
void msim_damage_building(msim *s, int b, float dmg);
extern const char *const MSIM_STATUS_TEXT[14];   /* "None " .. "Destroyed " (0x102609e8) */
extern const char *const MSIM_FORMATION_TEXT[7]; /* "Echelon Left" .. "No Formation" (0x10260958) */

/* actors from world3d_actors(); the sim does not own them. */
int  msim_init(msim *s, prj_archive *a, const char *scene, world_actor *actors, int count);
void msim_step(msim *s, int32_t dt_ms);
/* STARTUP_MECH on a shut-down player (engine 0x1001b011: state 3 -> 0 when flag 4 is clear or overridden, "Powering
 * up..."): the mech is up again and the start-up runs - online 0x43e + rand(0x16a) ticks later (0x1001ab71). Returns 0
 * (nothing done) while a heat shutdown is still pending and not overridden. */
int  msim_player_restart(msim *s);
/* Skip the end-sequence wait (the engine's key during the ending), or abort a running mission. */
void msim_end_now(msim *s);
/* "Mission successful" / "Mission failed" / "Mission time exceeded" / NULL (engine strings). */
const char *msim_outcome_text(const msim *s);
/* MW2MSN.CFG for the shell (engine 0x1000aba0, 0x9d4 bytes): returns 0 on success. */
int  msim_write_results(const msim *s, const char *path);
/* MW2CAR.CFG for the debriefing (DOS engine 0x00051d20: 80 bytes of u16 counters, each a triple enemy / neutral /
 * friendly - engine 0x102441d5 / d7 / d9): +0x07/09/0B mechs the player destroyed, +0x13 the player's shots,
 * +0x15/17/19 the player's hits, +0x1D end status (DAT_102441dd: 1 normal, 2 ejected, 4 automatic ejection aborted
 * by a hostile atmosphere - the shell never reads it; the engine picks its exit palette 0x10 / 0x11 by bit 4),
 * +0x1E/20/22 mechs destroyed by anyone, +0x34 friendly mechs lost (+0x36 their pilots ejected; the player's own
 * loss counts too, 0x10016280), +0x44/46/48 and +0x4A/4C/4E the same two for vehicles. */
int  msim_write_career(const msim *s, const char *path);
void msim_free(msim *s);
/* obstacles for avoidance (engine 0x100201c0): rays are tested against the world's faces;
 * `world` must stay alive while the sim runs */
void msim_set_world(msim *s, const mech3d *world);
/* index of the nearest living armed enemy actor within the player's weapon range, -1 if none */
int  msim_nearest_enemy(const msim *s, float *distance);
/* Terrain height under (x, z) for something at height y (engine 0x10010080): over the world's type 5
 * polygon terrain (upward faces) and type 0 boxes (their tops), the highest surface within 10 m of y,
 * else the highest below y, else 0. */
float msim_ground(const msim *s, float x, float z, float y);
/* jump-jet flames for a unit jetting at (x, y, z) facing heading (via on_effect) */
void msim_jet_flames(msim *s, float x, float y, float z, float heading);
/* the same at the mech's jet mounts (POFO slots 6 / 7) when it has them */
void msim_jet_flames_at(msim *s, const mech3d *m, float x, float y, float z, float heading);
/* As msim_ground, also giving the chosen surface's normal (n[3]; up for boxes and the flat plane). */
float msim_ground_n(const msim *s, float x, float z, float y, float *n);
/* Locomotion (engine 0x10019310): a step from (x0, z0) to (x, z) at height y is refused when steep
 * ground (normal y < 0.707) within 1 m ahead rises more than the step height (1 m), when the ground is
 * outside the window (more than 10 m up / 100 m down), or inside a type 1 obstacle. */
int  msim_can_step(const msim *s, float x0, float z0, float x, float z, float y);
/* Mech-mech contact (engine 0x1000ba20): every unit is a sphere (MGEO radius, centred centre_h above the feet). A move of
 * unit `self` (-1 the player) from centre p0 to centre p1 touches another unit when the centres come closer than
 * r1 + r2 and closer than before. Returns the unit touched (-1 the player, else an actor; -2 none) with the contact normal
 * n (from its centre toward p1, unit length), its centre c, r1 + r2 (*rr) and its velocity ov (cm/tick). */
int  msim_unit_contact(const msim *s, int self, const float p0[3], const float p1[3], float n[3], float c[3], float *rr, float ov[3]);
int  msim_can_step_hr(const msim *s, float x0, float z0, float x, float z, float y, float H, float R);   /* H centre height, R contact radius (MGEO) */
/* the same sphere test without the closing rule, against unit `other` only (-1 the player): is p1 inside its sphere?
 * (the engine's push-out of a unit set down inside another, 0x1000ba20) */
int  msim_unit_inside(const msim *s, int self, int other, const float p1[3], float n[3], float c[3], float *rr, float ov[3]);
/* A move (x0, z0) -> (*x, *z) refused by msim_can_step_hr with the surface normal n (engine 0x1000b5e0 on a world hit,
 * then 0x10019310's second try): the unit ends a quarter of the untravelled distance off the touching point along the
 * move reflected by the surface, or stays if that is refused too; *speed (cm/s along heading_deg, signed) becomes the
 * reflected velocity x -0.25 (x -0.5 when it stayed), along the heading. Returns 1 when it stayed. */
void msim_block_normal(float n[3]);   /* the normal of the surface that refused the last msim_can_step_hr move */
int  msim_world_impact(const msim *s, float x0, float z0, float *x, float *z, float y, float H, float R, const float n[3],
                       float heading_deg, float *speed);
/* Inside a type 1 obstacle's XZ box (walls, buildings: engine 0x1000d520, any height)? n = its face normal. */
int  msim_wall_at(const msim *s, float x, float z, float n[3]);
/* the living armed enemy closest to the aim direction (degrees, game yaw) within
 * `cone` degrees and weapon range; -1 if none */
int  msim_aim_target(const msim *s, float aim_yaw, float cone, float *distance);
/* the engine's throttle-to-speed chain (0x1001a180 / 0x100190d0), one frame of `ticks` (182 Hz): *thr = c[0x13] (fraction
 * of full, signed for the player's reverse) servoed toward thr_target; *spd = c[0xb] servoed toward the drive c[10];
 * *vel = the ground velocity along the heading (cm/s) approaching *spd. rev = -0.5 for an AI reversing (input +0x2f),
 * else 1; drive 0 = not up / immobile (c[10] cleared); slow: a walk key flagged 0x10 has run (servo T 90.5 ticks, else
 * 36.2). top = full-throttle speed, cm/s */
void msim_drive_step(float *thr, float *spd, float *vel, float thr_target, float rev, float top, int drive, int slow, float ticks);
/* a mech's full-throttle speed c[0x22] / 64 (cm/s) on a planet of gravity_g, in DOS MW2.EXE's integer steps */
float msim_top_speed(int walk_mp, float gravity_g);
/* the player's throttle preset n (0 = STOP .. 9 = FULL) as a fraction of full: DOS n x 113 / 0x400 */
float msim_throttle_preset(int n);

#endif
