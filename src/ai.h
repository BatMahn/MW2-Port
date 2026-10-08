/*
 * ai.h - MechWarrior 2's AI: AIT rule programs run per mech (3dfx MW2.DLL, see ASSUMPTIONS.md).
 *
 * FROM THE ENGINE: states (idle 0, avoid 1, target 2, attack 3, flee 4, follow 5, recon 6,
 * patrol 7, godirect 8, rest 10, shutdown 11, dead 12); program layout (header of (state, rule
 * count), 12-byte rules); rule evaluation (0x10011d20: condition -> action -> new state);
 * condition and action meanings; movement per state (0x100130f0): throttle by distance
 * (full beyond 2R, 80% between R and 2R, stop inside R; R = nav radius or 100 m), slow down
 * for turns over 45 deg (to 10%), halve throttle above 65 heat; steering = heading error x
 * 18.2 capped at +-819.2 (turn = input/1024 x 90 x cos(min(80, km/h)) deg/s); patrol torso
 * sweep +-45 in 5 deg steps every 32 ticks.
 *
 * Conditions 1/2 = nearest enemy closer than R / farthest enemy beyond R (0x10012b40 / 0x10012bf0:
 * the "score" is the distance at +0xc0); candidates exclude destroyed units and, for the player,
 * anything beyond 1750 m (0x1001d050).
 * Program choice per order: engine tables 0x1024baa8.. (identical for classes 1-8), applied in msim.c.
 * Firing (0x100231a0): only while the LAGGED torso twist is within +-10 deg of the target's bearing off the legs,
 * at intervals of rand(+0x158) x 22 ticks, +0x158 = max(GPS skill, 1) + 1 (0x10012000). The twist and aim pitch
 * follow the AI's demands through servos (ai_mind.twist / pitch; stepped by msim.c).
 * Events (0x10014270): action 4 posts (condition, target) to the leader; a posted event makes
 * the matching rule fire without its condition (0x10011d20).
 * Attack manoeuvres (0x1001ea40, chooser 0x1001f030 over the table at 0x1024cdf0): see ai.c.
 * Manoeuvre 1 (0x10020d00 / 0x1001f630): flank to a point 150 m from the target in one of 8
 * 45-degree slots around its facing, free slots preferred; re-picked every 543 ticks.
 * Obstacle avoidance (0x100201c0) is applied by msim.c, which has the world geometry.
 * Jump manoeuvres: 4 (0x1001fdd0) jet until 20 m above the start; 9 (0x1001fcc0) jet while the
 * target is > 8 m above and height < 40 m, then (1 in 4: queue 5) steer to it 181 ticks and finish
 * on landing; 5 (0x1001fe30) jet at the target with forward thrust, cut inside 10 m, finish on
 * landing. Jump-to-turn (0x10020950): pilot level < 4, target outside the twist limit -> jets on.
 * INFERRED: 0x10020a30's compared value read as height in metres; manoeuvre 5's forward-thrust
 * flag taken as "target ahead".
 * The chooser skips 4/5/9 for mechs without jets or fuel, as the engine's checks do.
 * attack-state movement (taken as target movement); target score / visibility simplified;
 * posted events (condition 5) not generated; the AI runs every frame.
 */
#ifndef MW2_AI_H
#define MW2_AI_H

#include <stdint.h>

#include "prj.h"

enum { AI_IDLE = 0, AI_AVOID, AI_TARGET, AI_ATTACK, AI_FLEE, AI_FOLLOW, AI_RECON, AI_PATROL, AI_GODIRECT,
       AI_REST = 10, AI_SHUTDOWN = 11, AI_DEAD = 12 };

typedef struct {
    uint16_t cond, cond_ref;
    int16_t  range;          /* -1/-2 stored AI ranges, -3 unlimited, else metres */
    uint8_t  action, next_state;
    uint8_t  b8;
    uint16_t ref;            /* bytes 9-10 */
} ai_rule;

typedef struct {
    char    name[10];
    int     state_count;
    int16_t states[16], rule_count[16], first_rule[16];
    ai_rule rules[48];
    int     total_rules;
} ai_program;

typedef struct {
    ai_program progs[9];     /* DEFLT, FDEFEND, FDESTROY, FLEAVE, FRECON, LDEFEND, LDESTROY, LLEAVE, LRECON */
    int        count;
} ai_library;

enum { AIP_DEFLT, AIP_FDEFEND, AIP_FDESTROY, AIP_FLEAVE, AIP_FRECON, AIP_LDEFEND, AIP_LDESTROY, AIP_LLEAVE, AIP_LRECON };

int  ai_load(ai_library *lib, prj_archive *a);
const char *ai_state_name(int s);

/* The world as one AI mech sees it. Distances in cm, angles in degrees (game yaw). */
typedef struct {
    int   has_enemy;         /* an enemy (the player) exists and is alive */
    int   provoked;          /* a member of this mech's group was just hit: the enemy counts as in range */
    float enemy_dist, enemy_bearing;
    int   has_goal;          /* a goal point from the mission order (nav / leader slot) */
    float goal_dist, goal_bearing, goal_radius;
    float range_a, range_b, range_c;  /* stored AI ranges (GPS 2nd, 3rd, 1st value), cm */
    float weapon_range;      /* cm */
    float heading, speed_kmh, heat;
    float twist_limit;       /* MGEO, degrees */
    int   is_leader;
    int   skill;             /* GPS fire interval base */
    int   enemy_is_player;   /* the enemy is the player (0x100231a0: other targets need rand(3) == 0) */
    float self_x, self_z, enemy_x, enemy_z, enemy_heading;
    int  *flank_slots;       /* attackers per 45-degree slot around the enemy (engine target +0x1a2) */
    int   can_jump;          /* has jump jets and fuel (engine 0x10020a30) */
    float height;            /* current height above ground, cm */
    float enemy_height;      /* target's height, cm (for "target above me", 0x10020090) */
    int   pilot_level;       /* GPS 1-4 (engine +0x159); < 4 enables jump-to-turn (0x10020950) */
    /* condition references (engine 0x10011d20 -> 0x10013400 / 0x10013a50: the rule's cond_ref is an iterator) */
    int   commanded;         /* +0x152 & 3: a commanded mate - condition 7 returns 0 (0x10012fd0) */
    int   c7_ok;             /* condition 7 (0x10012fd0 -> 0x10012de0): a pick exists (nearest enemy not already attacked
                              * by a groupmate in state 2/3, unless the node kind is 4) */
    float c7_nearest;        /* distance to the nearest enemy (the in-range test 0x10012b40), cm */
    int   navref_ok;         /* the assigned nav +0x16a (ref 0x2101): 0 = none (0x1000, excluded by 0x10013c40) */
    float navref_dist, navref_bearing, navref_radius;
    int   unit_led;          /* the group's leader is not the player (0x10018f90 != 0) */
    int   leader_ok;         /* ref 0x2201: my leader (another unit) */
    float leader_dist;
    float enemy_elev;        /* elevation from my origin to the target's origin, degrees (up +) */
    int   contact;           /* touching another mech (controller +0xa4, 0x10020e00) */
    int   out_of_weapons;    /* 0x10020880: no weapon with ammunition / working */
    int   moving;            /* controller +0x88 != 0 (0x1001ea40's out-of-weapons test) */
} ai_view;

typedef struct {
    int   state, prev_state;
    int   target;            /* 1 = engaged with the enemy, 0 = none */
    int   counter;           /* engine +0x140 */
    int   progs[3];          /* slot 0 default, 1 order program, 2 unused */
    float throttle;          /* 0 .. 1 (fraction of full = walk speed) */
    float turn;              /* input units, +-1024 = full */
    float torso;             /* patrol sweep, degrees */
    int   sweep_dir;
    float sweep_ticks;
    int   wants_fire;
    int   event_cond;        /* posted event (engine +0x144), 0 = none */
    int   post_cond;         /* set by action 4: event for the group leader (msim delivers it) */
    float next_fire;         /* ticks until the next fire decision (engine +0x15a) */
    float weave, weave_ticks;/* attack manoeuvre 3 */
    int   man, prev_man, forced_man;   /* attack manoeuvre (engine +0x170 / +0x172 / +0x174), -1 none */
    float man_ticks, man_limit;
    int   flank_slot;        /* manoeuvre 1: slot taken (-1 none) */
    float flank_x, flank_z, flank_retarget;
    int   jet;               /* jet input this tick */
    int   jet_forward;       /* horizontal jet thrust input (+0x20) */
    int   man_phase;         /* manoeuvre 5/9 phase (engine +0x180) */
    int   turn_jet;          /* jump-to-turn jets latched (0x10020950); cleared when a manoeuvre ends */
    float jump_from;         /* manoeuvre 4: height when it started */
    float last_dist;         /* manoeuvres 2/3/5: previous distance (closing speed, 0x100208f0; +0x19a, 0 at a start) */
    /* aim servos (0x100190d0 -> 0x1003b7a0, stepped by 0x1003b7e0 in 0x10019310): value += dt x (target - value) / T,
     * no overshoot. Twist: controller +4 servo, T = 0.6 s for AI mechs (0.2 s the player) = 108.6 ticks at the servo's
     * 181 ticks/s; target = input +4 clamped to the MGEO limit (0x1001a180). Pitch: +0x14 servo, T = 0.2 s = 36.2 ticks,
     * target = input +0 = -clamp(elevation, +-60) (0x100135d0). Stored here in game sign: up +. */
    float twist, pitch;      /* current (lagged) torso twist relative to the legs / aim pitch, degrees */
    float twist_demand, pitch_demand;   /* the AI's inputs this step (0x100231a0: twist demand = param_2) */
    int   reverse;           /* input +0x2f (manoeuvres 8 / 10): drive backwards at half speed (0x1001ff70 / 0x1001fc00) */
    int   jet_side;          /* manoeuvre 11: lateral jets (input +0x1e) */
    float speed;             /* actual ground speed, cm/s (signed; the velocity c[0x3d..0x3f], see msim_drive_step) */
    float thr_servo;         /* current throttle c[0x13] as a fraction of full (servo +0x44, T 36.2 ticks) */
    float spd_servo;         /* servoed drive speed c[0xb], cm/s (servo +0x24 toward c[10]) */
    int   drive_slow;        /* servo +0x24's T: 0 = 36.2 ticks (0x100190d0) until a walk key flagged 0x10 runs, then 90.5 */
    int   targets_player;    /* this step's enemy is the player (+0x14e == player | 0x200): the MISLTRCK check (0x10031e30) */
    int   lock;              /* missile lock flags (0x10044560): 0x8000 on target, 0x40 acquiring, 0x80 locked */
    float lock_ticks;
    int   tgt;               /* the target picked by condition 7 / the group assignment (actor, -1 player, -2 none) */
    int   c7_fired;          /* a condition-7 rule committed this step (msim takes the pick as the target) */
    int   nav_ok;            /* the assigned nav +0x16a: a 30 m point made where the mech left patrol for a target
                              * (entry 0x10013790 case 2, groups not led by the player), released on reaching it */
    float nav_x, nav_z;
    int   to_navref;         /* godirect to the assigned nav (+0x16a, rule cond 2 ref 0x2101 -> ref2 0x25) */
    int   rule_fired;        /* a rule fired this step: no movement (0x10011c50) */
    int   event_who;         /* posted event's target (+0x146) */
    int   post_who;
    int   queue5;            /* +0x184 = 1: the out-of-weapons jump (4) is followed by 5 (0x1001f030) */
    int   fled;              /* out of weapons -> flee (0x1001f030 forced -2): +0x152 = 1, no more target choice */
    int   fire_locked;       /* the lock (0x80) when the weapon now firing was triggered: its missiles home */
    unsigned rng;
} ai_mind;

void ai_init(ai_mind *m, int order_prog);
/* whether one of the mind's programs lists the state (engine 0x10013d90): a command for a state it lacks fails */
int  ai_allows(const ai_library *lib, const ai_mind *m, int state);
/* post an event (condition code) to a mind, if it has none pending (engine 0x10014270) */
void ai_post(ai_mind *m, int cond);
/* the target of a guided launch reacts (engine 0x10020ac0): shooter_level = the shooter's pilot level (player 1).
 * side_free(ctx, side) reports whether a 50 m sidestep that way (-1 left, +1 right) is clear. */
void ai_missile_warning(ai_mind *m, const ai_view *v, int shooter_level, int (*side_free)(void *ctx, int side), void *ctx);
/* one AI step: evaluate the current state's rules, then move per state. dt in seconds. */
void ai_step(const ai_library *lib, ai_mind *m, const ai_view *v, float dt);

#endif
