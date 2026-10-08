/*
 * combat.h - weapons, damage and heat for mechs built from their MEK loadouts.
 *
 * FROM THE GAME (3dfx MW2.DLL; see ASSUMPTIONS.md):
 *   - weapon behaviour from the simulation's own weapon table (wtable.h): per-weapon state
 *     machine (recycle `refire` ticks -> ready -> volley of `per_volley` projectiles, one
 *     every `interval` ticks), damage and heat per projectile, range = speed x life
 *   - heat (0x10015f50): dissipation from heat sinking, doubled while shut down; warning at 65-80
 *     on a net gain (latched); at 80-100 a shutdown sequence starts and shuts the mech down 1086
 *     ticks later unless overridden, whatever the heat by then; restart at 65 or below 724 ticks after
 *     the shutdown; above 100 not overridden: destroyed 4525 ticks after the sequence began;
 *     overridden: each tick a 3-in-4000 chance of a cook-off - the DOS rule (0x24ec0): the first
 *     slot holding the first ammo bin's item takes a critical, destroyed if there is none (so
 *     within a roll or two); an invulnerable player skips all that
 *   - clock: 182 ticks per second
 * Armour, structure, weapons and their locations come from the MEK records.
 *
 *   - damage (0x100171d0): armour, then that location's structure; no transfer; a hit on the
 *     centre torso is redistributed at random to LT/CT/RT; rear armour (torsos only) when
 *     target heading - shot direction is within +-90 deg (0x10044f60)
 *   - destruction (0x10016640): side torso takes its arm; CT takes both side torsos, both
 *     arms and the head; head or CT = mech destroyed; first leg = immobile, second = destroyed
 *   - hit location from the part struck: OBJL groups; approximated here by each group's
 *     front-view area (mech3d_hit_weights)
 *
 *   - armour / structure are floats (0x100171d0): armour at or below 0 breaches, the rest goes into
 *     the structure; no damage outside locations 1-8
 *   - critical hits (0x100171d0): after a breach that leaves the location standing, rand(5) rolls;
 *     each rand(100) < 20 with entries in the location's item list is a critical: 12 = location
 *     destroyed, otherwise entry rand(count) takes it (0x100167b0) and leaves the list: a weapon is
 *     destroyed, equipment acts (speed -0.1, heat sink -0.000763, jets, cockpit, displays), endo /
 *     ferro pass it to another entry, an ammo bin explodes (its rounds x damage into the structure)
 *   - a destroyed location (0x10016640 -> 0x10016600) gives every entry a silent critical
 *   - ammunition: per-weapon bins drawn down a round at a time (0x10043cc0); the last round spends
 *     the weapon (state -1)
 *   - projectiles: fly at `speed` for `life` ticks and collide with the target's posed part
 *     geometry; the part's OBJL group is the location (msim.c; engine 0x10044f60 / 0x100103e0)
 *
 * STILL ASSUMED:
 *   - AI aim: the engine has no random scatter (0x100448e0: shots follow the shooter's aim
 *     line); here the AI torso is taken to track instantly within its MGEO twist limit, aiming
 *     at the target's current position (no lead). The GPS "250" values are AI ranges (m), not skill.
 *   - arm actuator / sensor criticals damage the player's cockpit displays (0x1001e670): no HUD yet
 *   - AI heat discipline (engine 0x100232b0): a weapon fires only if current heat + its heat
 *     per projectile < 65 (hold_fire_above = 65 for AI, 0 for a human pilot)
 */
#ifndef MW2_COMBAT_H
#define MW2_COMBAT_H

#include "mek.h"
#include "prj.h"
#include <stdint.h>

#define COMBAT_MAX_WEAPONS 10   /* the loader keeps the first 10 weapons (0x10041410) */
#define COMBAT_MAX_BINS 25      /* ammo bins (0x10041410 stops at 25) */
#define COMBAT_TICKS_PER_S 182.0f

/* weapon states (+0x08): 0 recycling, 1 ready, 2 firing a volley, 3 repeat wait (trigger held on a repeating
 * weapon: refire ticks, then the next volley), -1 destroyed or out of ammunition */
enum { CW_RECYCLE, CW_READY, CW_FIRING, CW_REPEAT };

typedef struct {
    int   weapon;       /* index into mek_weapons / sim_weapons */
    int   location;     /* MEK location index */
    int   state, shots, pulses_due;
    int   ammo;         /* projectiles left; -1 = energy weapon (no ammunition) */
    int   volley_start; /* the next projectile released is the volley's first (plays the sound) */
    uint32_t item;      /* MEK item id of this weapon instance */
    float t;            /* ticks into the current state */
    int   bin_first, bin_n, bin_cur;   /* its ammo bins (+0x34.., +0x30 count, +0x60 current) */
} combat_weapon;

/* an ammo bin (engine 0x16-byte record, 0x10041410): the ammo item id, the weapon it feeds, rounds left */
typedef struct { uint32_t id; int weapon, rounds; } combat_bin;

typedef struct {
    char          loadout[16];
    float         armor[MEK_LOC_COUNT], rear[MEK_LOC_COUNT], internal[MEK_LOC_COUNT];   /* floats, as the engine keeps them */
    int           armor_max[MEK_LOC_COUNT], rear_max[MEK_LOC_COUNT];   /* starting values (HUD damage levels) */
    combat_weapon weapons[COMBAT_MAX_WEAPONS];
    int           weapon_count;
    float         heat, sink_per_tick;
    int           shutdown, destroyed, damage_taken;
    float         shut_ticks, over_ticks, damage_carry;
    int           heat_warning;   /* one tick: "Heat level critical" (65-80 with a net gain; latched, 0x10015f50) */
    int           warn_latch;     /* engine DAT_1024bfd0: the warning was given; cleared below 65 after 724 ticks */
    float         warn_ticks, heat_mark, last_ticks;   /* ticks since the warning; heat after the last tick; last tick's length */
    int           auto_shutdown;  /* one tick: the sequence shut the mech down (message 0xd "Shutting down") */
    int           restarted;      /* one tick: the heat restart (state 3 -> 0): the start-up lock runs again (msim.c) */
    int           human;          /* a human pilot: edge-triggered fire, no range check (0x100437a0) */
    uint32_t      fire_held, fire_prev, fire_edge;   /* human: weapons whose trigger is held now / last tick / newly pressed */
    int           shutdown_pending;  /* engine flag 4: sequence started above 80, shuts down 1086 ticks later */
    int           override;          /* engine flag 8: the pilot overrode the shutdown */
    int           manual_down;       /* SHUTDOWN_MECH shut it down (informational: the heat restart still applies with flag 4) */
    /* Combat Variables (MW2DIF.CFG, engine block DAT_1024ac6c): the player's switches */
    int           unlimited_ammo;    /* +0: shots use no ammunition */
    int           invulnerable;      /* +1: no damage taken (0x10016280) */
    int           no_heat;           /* +4 heat tracking off: heat held at 0 (0x10015f50) */
    int           no_collision_damage;   /* +3 = 0: collision damage off (0x1000c160 / 0x1000c3c0) */
    float         seq_ticks;         /* ticks since the sequence / shutdown began (engine c[0x23]) */
    unsigned      rng;
    int           ammo_blown;        /* set when an overheat ammo explosion happened (for messages) */
    unsigned      breach_latch;      /* per location (bit l): its armour (front or rear) has been used up (location word
                                        flag 0x4000, 0x100171d0) */
    int           breach_sound;      /* a human pilot, up: a location's armour first used up - MECRDWA1 0xec (0x1001732e);
                                        the front end clears it */
    int           red_flash;         /* set (a human pilot) when the engine asks for the red palette flash (DAT_1024cce0):
                                        an ammunition bin exploding (0x100167b0) or more than 2 points of damage reaching the
                                        head / centre torso structure (0x100171d0); the front end clears it */
    int           display_hits;      /* criticals 5300-5450: cockpit displays damaged, 2 in 10 per instrument (0x1001e670) */
    int           display_hits5;     /* critical 5700: the same roll at 5 in 10 per instrument (0x1001e670 flag 1) */
    float         throttle_frac;     /* current throttle 0..1 of full, set by the mover: walking heats the mech by
                                        frac/64 x dissipation x 6.4 once per frame (0x1001aac8; 30 fps assumed) */
    float         hold_fire_above;  /* 0 = fire freely (a human pilot); AI: 65 (assumed) */
    int           ai_sel;           /* AI: the selected weapon slot (engine unit +0xac) */
    float         hit_w[8];         /* location weights (mech3d_hit_weights); all 0 = unknown */
    uint32_t      slots[8][MEK_SLOTS];  /* the location's item list (MEK slots; a critical removes its entry, 0x100167b0) */
    int           slot_count[8];        /* entries in the list (MEK +0x24; -1 per critical) */
    combat_bin    bins[COMBAT_MAX_BINS];
    int           bin_count;
    int           loc_gone[8];      /* location destroyed (engine flag 0x2000) */
    int           immobile;         /* lost a leg */
    float         speed_gain;       /* engine c[0x2c]: 1.0, -0.1 per leg actuator/hip/engine/gyro critical, floor 0.4 */
    int           no_jump;          /* gyro critical */
    /* jump jets (engine 0x10041410 / 0x1001a180 / 0x10019310) */
    int           tons;             /* MEK tonnage (controller +0xe4) */
    uint16_t      rating;           /* loadout rating, object +0x150 (0x10041c80; group member choice 0x10014920) */
    int           jets;             /* jump MP (c[0x31]) */
    float         jet_ddy;          /* thrust, cm/tick^2: jump MP / walk MP x 0.1184 (c[0x3b]) */
    float         jet_fuel;         /* ticks, max 1810 (c[0x30]); burns 1/tick, recharges 1/4 per tick */
    float         y, vy;            /* height (cm, absolute) and vertical speed (cm/tick) */
    float         ground;           /* terrain height under the unit (cm), set by the sim each step */
    float         landed_vy;        /* the vertical speed (cm/tick, < 0) it touched down with this step from a fall or jump, else 0 */
    int           blocked;          /* pressing against terrain (cleared by a free move): the contact count / impact-sound latch */
    int           jet_forward;      /* horizontal jet thrust on (input +0x20) */
    int           offline;          /* set by the sim: not in controller state 2 (starting up, powered down) - the fire
                                       routine 0x100437a0 runs only in state 2, so no trigger and no queued volley releases */
    /* ejection (controller state 5: EJECT 0x100174f0, automatic ejection 0x100167b0; resolved by 0x10016280) */
    int           ejected;          /* state 5 when 0x10016280 ran: 1 the pilot ejected (breathable planet - end status 2,
                                       a member of the player's group counts at career +0x36 instead of +0x34),
                                       2 automatic ejection aborted by a hostile atmosphere (end status 4, state 4) */
    int           intact_out;       /* state 5 kept: the mech is out of action but stands as it was - no death sequence,
                                       no wreck. The player after an ejection; any unit whose automatic ejection came on
                                       the explosion that had just destroyed it (state 4 -> 5 after 0x10016280 ran) */
} combat_unit;

/* The mission's planet (PLNT chunk, engine 0x1003de10): gravity in g, temperature, the jet
 * climb damping divisor, hostile atmosphere. Set before combat_init (dissipation depends on it). */
typedef struct { float gravity_g; int temperature; float jet_damping; int hostile; } combat_planet;
void combat_set_planet(const combat_planet *p);
/* engine DAT_1024c570: 0 from the mission start (0x1002388d) until the player first comes online (0x1001ac68); while 0,
 * 0x100171d0 applies no damage to anyone (weapon hits, splash, collisions, falls) and 0x10015e10 (BlowAmmo) does nothing.
 * The sim sets it; 1 by default (tools) */
extern int combat_damage_live;
/* TOGGLE_AUTOEJECT (engine DAT_1024bfbc, default off; one switch for every unit): an ammunition explosion (0x100167b0)
 * then puts the unit in state 5 and calls 0x10016280 - the player ejects (hostile planet: aborted, an ordinary death);
 * an AI mech (single player, DAT_1024ffcc == 0) goes on to state 4 there, an ordinary death */
void combat_set_auto_eject(int on);
int  combat_auto_eject(void);
int  combat_init(combat_unit *u, prj_archive *a, const char *loadout);
/* The engine's loadout rating of a MEK (0x10041c80, stored at object +0x150 by 0x10041410). */
uint16_t combat_mek_rating(const mek_def *d);
/* the player's heat dissipation by difficulty (MW2DIF +5; engine 0x10041410): per tick x heat sinking, normal /
 * colder than -30 / hotter than 50: Easy 0.001068 / 0.002136 / 0.001068, Medium 0.000763 / 0.001526 / 0.000687,
 * Hard 0.000687 / 0.001030 / 0.000549 (AI 'Mechs: Medium's) */
void combat_set_player_difficulty(combat_unit *u, int difficulty);
/* advance time: weapon cycles, heat, shutdown */
void combat_tick(combat_unit *u, float dt);
/* trigger ready weapons in range at `target` and resolve projectiles now due.
 * distance in game units (cm). Returns damage dealt (rounded). */
/* Trigger every ready weapon whose range covers `distance` (cm). Projectiles are then
 * released over time by combat_tick; collect them with combat_next_projectile. */
void combat_trigger(combat_unit *u, float distance);
/* As combat_trigger, for the weapons whose bit is set in mask (bit i = weapons[i]). */
void combat_trigger_mask(combat_unit *u, float distance, uint32_t mask);
/* AI fire (engine 0x100232b0): one weapon per decision - starting at the unit's selected weapon, each in turn is
 * tried: in its range window (+0x3c .. +0x40), heat + its heat under 65, ready, ammunition, and a roll of
 * rand(refire / 90 + 1) == 0; the first that passes fires (and stays selected), else the selection moves on.
 * rng(ctx, n) returns 0..n-1. Returns the weapon slot fired, -1 none */
int  combat_ai_fire(combat_unit *u, float distance, unsigned (*rng)(void *ctx, unsigned n), void *ctx);
/* Next projectile released and not yet launched: returns the weapon (sim_weapons index) or -1. */
int  combat_next_projectile(combat_unit *u);
/* As above; *first = 1 for a volley's first projectile (the engine plays the weapon sound then). */
int  combat_next_projectile_ex(combat_unit *u, int *first);
/* as combat_next_projectile_ex, also giving the weapon's MEK location (*loc, may be NULL) */
int  combat_next_projectile_at(combat_unit *u, int *first, int *loc);
float combat_weapon_damage(int w);
int  combat_weapon_kind(int w);     /* weapon table +0x00: projectile kind (0-2 lasers, 3 SRM/Narc, 4 LRM/Streak, 5 ballistic, 6 PPC, 7 Gauss/flamer) */
int  combat_weapon_has_object(int w);   /* weapon table +0x14: launches a projectile object (the flamer does not) */   /* damage per projectile (weapon table +0x30) */
int  combat_weapon_sound(int w);    /* SNDS index (+0x24) */
int  combat_weapon_visual(int w);   /* projectile object (+0x04) */
/* A projectile of weapon `w` struck location `loc` (1-8). rel_angle: target heading minus the
 * shot's direction of travel (degrees); within +-90 it strikes from behind (rear armour on
 * torsos). Returns the damage dealt. */
int  combat_hit(combat_unit *t, int w, int loc, float rel_angle, unsigned *rng);
float combat_weapon_speed(int w);   /* cm per tick */
float combat_weapon_range(int w);   /* table +0x40, cm */
/* JETTISON_AMMO (engine 0x10044220): the weapon's ammunition dumped - its count and the matching bins emptied.
 * Returns 1 if there was any. */
int  combat_jettison(combat_unit *u, int wi);
/* a critical on the item of the unit's first ammunition bin that can explode (rounds left, not LRM 20 ammunition;
 * 0x100167b0, not silent). Returns 0 when there is none. (Tests: provokes an ammunition explosion.) */
int  combat_crit_ammo(combat_unit *u);
/* MASC fitted: a critical slot holding an item 5000..5049 (engine loader 0x1004xxxx sets controller +0x43 bit 0x10) */
int  combat_has_masc(const combat_unit *u);
int   combat_weapon_life(int w);    /* ticks */
int   combat_weapon_drop(int w);    /* table +0x28: 1 = falls under gravity (MG, Streak, Narc, AMS) */
/* Old instant-hit helper (trigger + resolve with range odds); kept for tests. */
int  combat_fire(combat_unit *u, combat_unit *target, float distance, float rel_angle, unsigned *rng);
int  combat_health(const combat_unit *u);
/* The pilot overrides a pending shutdown (engine flag 8, message "Shutdown sequence overridden"). */
void combat_override(combat_unit *u);
/* Jump jets and vertical motion for dt seconds; `jetting` = jet input held. Gravity 0.0296
 * cm/tick^2; jetting: a = (ddy - g)(1 - 0.0362 vy) while rising; jet heat jets x 3/512 per
 * tick; landing faster than 16.154 cm/tick damages both legs by the excess x 1.5476.
 * Gravity = planet g x 0.0296; the damping divisor is the planet's (not a constant).
 * Returns 1 while airborne. */
int  combat_jets(combat_unit *u, int jetting, float dt, unsigned *rng);
/* Collision damage (engine 0x1000c3c0 -> 0x1000c1f0): speed in cm/tick against a surface whose normal is
 * (nx, ny, nz); rel_deg = the surface direction relative to the unit's facing. Nothing below 3.069 cm/tick. */
void combat_collision(combat_unit *u, float speed, float ny, float rel_deg, unsigned *rng);
/* A unit-unit contact (engine 0x1000c160 -> 0x1000c1f0): dv = the relative speed of the two (cm/tick, 3D); n = the
 * contact normal from the other unit's centre toward this one's (unit length, y up); facing = heading + torso twist;
 * other_tons scales a hit from above (head). Damages this unit only (each unit's own controller reports its contacts). */
void combat_collision_unit(combat_unit *u, float dv, const float n[3], float facing_deg, int other_tons, unsigned *rng);
/* forward jet thrust (input +0x20, 0x10019310): with the jets on, thrust goes along the heading
 * instead of up and the mech glides level (no fall while not climbing). Adds to *hvel (cm/tick)
 * the horizontal speed change for this step. */
void combat_jets_forward(combat_unit *u, int on, float dt, float *hvel);
int   combat_out_of_weapons(const combat_unit *u);   /* 0x10020880 */
float combat_max_range(const combat_unit *u);   /* game units (cm) */

#endif
