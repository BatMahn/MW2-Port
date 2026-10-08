/* combat.c - see combat.h (what is from the game and what is assumed is listed there). */
#include "combat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wtable.h"

static unsigned rnd(unsigned *s) { *s = *s * 1103515245u + 12345u; return (*s >> 16) & 0x7FFF; }
static float frand(unsigned *s) { return (float)rnd(s) / 32768.0f; }

static combat_planet g_planet = {1.0f, 0, 100000.0f, 0};

void combat_set_planet(const combat_planet *p) { g_planet = *p; if (g_planet.jet_damping <= 0) g_planet.jet_damping = 100000.0f; }
static int g_auto_eject;   /* DAT_1024bfbc */
void combat_set_auto_eject(int on) { g_auto_eject = on != 0; }
int  combat_auto_eject(void) { return g_auto_eject; }

static int rand_n(unsigned *s, int n) { return n > 0 ? (int)(rnd(s) % (unsigned)n) : 0; }   /* engine 0x1003d910: table[i] % n */

static float range_cm(int w) { return (float)sim_weapons[w].range; }   /* table +0x40 */

/* The loadout rating (DOS 0x4cf20 -> object +0x14c; 3Dfx 0x10041c80 -> +0x150, called by 0x10041410 on the MEK
 * record): a 16-bit sum (wrapping), starting at the tonnage. For each of the 8 locations in turn: every
 * critical-slot id above 5000 counts in the band of the highest threshold below it (5000, 5050, .. 5900 in steps of
 * 50; 6000, 7000, 8000, 9000; table DOS 0x4c6d0 / 3Dfx built on the stack, same values); the running band counts
 * (never reset between locations) then add, for bands 0..20, count x mult x tonnage (mult >= 0) or count x -mult
 * (mult < 0); plus the running total of those slots (doubled once band 21, > 8000, has one), the running sum of
 * (armour + rear) x slot count, and the structure. Then, for the first item_count (at most 10) items, the short
 * table DOS 0x9eb2c / 3Dfx 0x10259f60 [id / 100]. The DOS rule is kept: the 3Dfx DLL reads the MEK's integer armour
 * and structure with flds before converting them (fildl after the call), so there those two terms are 0. */
static const short rating_mult[21] = { 1, 1, -60, -60, -50, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 0, 2, 3, 2, 1, 1 };
static const short rating_weapon[32] = {   /* 0x10259f60 */
    183, 137, 91, 46, 51, 34, 17, 74, 49, 25, 2, 228, 42, 82, 123, 157,
    74, 144, 247, 329, 4, 228, 166, 51, 21, 177, 74, 16, 50, 40, 0, 0 };
static int rating_band_min(int b) { return b <= 18 ? 5000 + 50 * b : 6000 + 1000 * (b - 19); }
uint16_t combat_mek_rating(const mek_def *d)
{
    int count[23] = {0}, total = 0, l, k, b, n;
    int32_t armor_sum = 0;
    int16_t r = (int16_t)d->tonnage, tons = (int16_t)d->tonnage;
    for (l = 0; l < MEK_LOC_COUNT; l++) {
        const mek_loc *L = &d->loc[l];
        int16_t nslots = (int16_t)L->slot_count;
        if (nslots > 0) {
            armor_sum += (int32_t)(L->armor + L->rear_armor) * nslots;
            for (k = 0; k < nslots && k < MEK_SLOTS; k++)
                for (b = 22; b >= 0; b--)
                    if (rating_band_min(b) < (int)L->slots[k]) { count[b]++; total++; break; }
        }
        for (b = 20; b >= 0; b--)
            if (count[b]) r = (int16_t)(rating_mult[b] < 0 ? r - rating_mult[b] * count[b] : r + rating_mult[b] * count[b] * tons);
        r = (int16_t)(r + (count[21] ? total * 2 : total));
        r = (int16_t)(r + (int16_t)armor_sum + (int16_t)L->internal);
    }
    n = d->item_count < 10 ? d->item_count : 10;
    for (k = 0; k < n; k++) {
        unsigned idx = d->items[k].id / 100;
        r = (int16_t)(r + (idx < 32 ? rating_weapon[idx] : 0));
    }
    return (uint16_t)r;
}

int combat_init(combat_unit *u, prj_archive *a, const char *loadout)
{
    mek_def d;
    int i, l, k, j;
    memset(u, 0, sizeof *u);
    snprintf(u->loadout, sizeof u->loadout, "%s", loadout);
    u->speed_gain = 1.0f;
    u->rng = 0x9E3779B9u;
    if (mek_load(a, loadout, &d) != 0) return -1;   /* the archive, else a user design MEK\\<name>.MEK */
    if (getenv("MW2_DEBUG_LOADOUT")) fprintf(stderr, "loadout %s: %d items, %d ammo bins, '%s'\n", loadout, d.item_count, d.link_count, d.config_name);
    /* engine 0x10041410: the MEK locations copied as they are (armour, rear, structure made floats; the item
     * list and its count +0x24 kept) */
    for (l = 0; l < MEK_LOC_COUNT; l++) {
        u->armor[l] = (float)d.loc[l].armor;
        u->rear[l] = (float)d.loc[l].rear_armor;
        u->internal[l] = (float)d.loc[l].internal;
        u->armor_max[l] = (int)d.loc[l].armor;
        u->rear_max[l] = (int)d.loc[l].rear_armor;
        u->slot_count[l] = d.loc[l].slot_count <= MEK_SLOTS ? d.loc[l].slot_count : MEK_SLOTS;
        for (k = 0; k < MEK_SLOTS; k++) u->slots[l][k] = d.loc[l].slots[k];
    }
    /* engine 0x10041410: heat sinking x 0.000763 per tick (AI mechs / normal difficulty) */
    /* engine 0x10041410 (AI / normal difficulty): colder than -30 doubles it, hotter than 50 lowers it */
    u->sink_per_tick = (float)d.heat_sinking *
                       (g_planet.temperature < -30 ? 0.00152587890625f : g_planet.temperature > 50 ? 0.0006866455078125f : 0.000762939453125f);
    u->rating = combat_mek_rating(&d);
    u->tons = (int)d.tonnage;   /* controller +0xe4 (MEK +0x00): the head-hit ratio in 0x1000c1f0 */
    u->jets = (int)d.jump_mp;
    u->jet_ddy = d.walk_mp ? (float)d.jump_mp / (float)d.walk_mp * 0.1184f : 0;
    u->jet_fuel = u->jets > 0 ? 1810.0f : 0;
    /* engine 0x10041410: the first 10 items are the weapons; for each, in order, every ammo link naming it becomes a
     * bin (at most 25) of per_volley x ammo_per_ton rounds, and the weapon's count is the sum (-1: no ammunition) */
    for (i = 0; i < d.item_count && u->weapon_count < COMBAT_MAX_WEAPONS; i++) {
        const mek_weapon *w = mek_weapon_for(d.items[i].id);
        combat_weapon *cw;
        int wi, loc = MEK_CT;
        if (!w) continue;
        wi = (int)(w - mek_weapons);
        if (wi < 0 || wi >= 31 || sim_weapons[wi].damage <= 0 || sim_weapons[wi].per_volley <= 0) continue;
        for (l = 0; l < MEK_LOC_COUNT; l++)
            for (k = 0; k < MEK_SLOTS; k++)
                if (d.loc[l].slots[k] == d.items[i].id) { loc = l; l = MEK_LOC_COUNT; break; }
        cw = &u->weapons[u->weapon_count];
        cw->weapon = wi;
        cw->item = d.items[i].id;
        cw->ammo = sim_weapons[wi].ammo_per_ton != -1 ? 0 : -1;
        cw->location = loc;
        cw->state = CW_READY;
        cw->bin_first = u->bin_count;
        for (j = d.item_count; j < d.item_count + d.link_count && j < MEK_MAX_ITEMS; j++) {
            combat_bin *b;
            if (u->bin_count >= COMBAT_MAX_BINS) break;
            if (d.items[j].linked_id != (int32_t)cw->item) continue;
            b = &u->bins[u->bin_count++];
            b->id = d.items[j].id;
            b->weapon = u->weapon_count;
            b->rounds = (int)(short)(sim_weapons[wi].per_volley * sim_weapons[wi].ammo_per_ton);
            if (cw->ammo != -1) cw->ammo += b->rounds;
            cw->bin_n++;
        }
        u->weapon_count++;
    }
    return 0;
}

static void destroy_location(combat_unit *t, int loc);
static void crit_item(combat_unit *t, int loc, int idx, int silent);
static void cook_off(combat_unit *u);

/* engine 0x10043cc0: one round leaves the weapon's count and its current bin (an empty bin moves the pointer on to
 * the next without taking a round); the count reaching 0 marks the weapon spent (state -1) and that projectile
 * still flies but adds no heat. Returns 0 then. Unlimited ammunition (Combat Variables +0) takes nothing. */
static int take_round(combat_unit *u, combat_weapon *cw)
{
    if (cw->ammo == -1 || u->unlimited_ammo) return 1;
    cw->ammo--;
    if (cw->bin_n > 0) {
        combat_bin *b = &u->bins[cw->bin_first + cw->bin_cur];
        if (b->rounds == 0) { if (cw->bin_cur + 1 < cw->bin_n) cw->bin_cur++; }
        else b->rounds--;
    }
    if (cw->ammo == 0) { cw->state = -1; return 0; }
    return 1;
}

/* engine 0x100437a0, state 2: release every projectile due (`interval` ticks apart, the time carried over) */
static void release_due(combat_unit *u, combat_weapon *cw)
{
    const sim_weapon *sw = &sim_weapons[cw->weapon];
    while (cw->state == CW_FIRING && cw->shots > 0 && cw->t >= (float)sw->interval) {
        int ok = take_round(u, cw);
        cw->pulses_due++;
        if (!ok) { if (cw->shots == sw->per_volley) cw->volley_start = 0; cw->shots = 0; break; }   /* last round: no heat, no sound */
        cw->t -= (float)sw->interval;
        cw->shots--;
        u->heat += sw->heat;
        if (cw->shots == 0 && sw->repeat && (u->fire_held & (1u << (cw - u->weapons)))) {
            /* a repeating weapon with its trigger still held waits refire ticks and fires again */
            cw->state = CW_REPEAT;
            cw->shots = sw->per_volley;
            cw->t = 0;
            break;
        }
    }
}

/* engine 0x100437a0 / 0x10044440: every trigger path starts the volley with the weapon's timer at 0, and the release
 * loop runs in the same frame - the first projectile leaves once `interval` ticks have passed, counting this frame */
static void start_volley(combat_unit *u, combat_weapon *cw)
{
    cw->state = CW_FIRING;
    cw->shots = sim_weapons[cw->weapon].per_volley;
    cw->t = u->last_ticks;
    cw->volley_start = 1;
    release_due(u, cw);
}

void combat_tick(combat_unit *u, float dt)
{
    float ticks = dt * COMBAT_TICKS_PER_S, net;
    int i;
    if (u->destroyed) return;
    u->last_ticks = ticks;
    u->fire_edge = u->fire_held & ~u->fire_prev;
    u->fire_prev = u->fire_held;
    u->heat_warning = 0;
    u->auto_shutdown = 0;
    u->restarted = 0;
    for (i = 0; i < u->weapon_count; i++) {
        combat_weapon *cw = &u->weapons[i];
        const sim_weapon *sw = &sim_weapons[cw->weapon];
        if (cw->state < 0) continue;   /* destroyed by a critical hit, or out of ammunition */
        /* 0x100437a0 runs only while the mech is up (state 2): a volley's timer stands still while shut down; the
         * recycle and repeat waits are measured from a start time, so they run on */
        if ((u->shutdown || u->offline) && cw->state == CW_FIRING) continue;
        cw->t += ticks;
        if (u->shutdown || u->offline) continue;
        if (cw->state == CW_RECYCLE && cw->t >= (float)sw->refire) { cw->state = CW_READY; cw->t = 0; }
        else if (cw->state == CW_REPEAT && cw->t >= (float)sw->refire) {
            /* still held: the next volley (counted from 0); released: ready */
            if (u->fire_held & (1u << i)) { cw->state = CW_FIRING; cw->volley_start = 1; } else cw->state = CW_READY;
            cw->t = 0;
        }
        else if (cw->state == CW_FIRING) {
            if (cw->shots < 1) { cw->state = CW_RECYCLE; cw->shots = 0; cw->t = 0; continue; }   /* the frame after the last */
            release_due(u, cw);
        }
    }
    /* walking heat, engine 0x1001aac8: (c13 - 1/64) x dissipation x 6.4 added once per frame (not per tick):
     * c13 - 1/64 = throttle / 64; 30 frames a second assumed */
    if (!u->shutdown && u->throttle_frac > 0)
        u->heat += u->throttle_frac * 0.015625f * u->sink_per_tick * 6.4f * (dt * 30.0f);
    /* engine 0x10015f50 */
    if (u->no_heat) { u->heat = 0; u->heat_mark = 0; return; }   /* Combat Variables: heat tracking off */
    net = (u->heat - u->heat_mark) - u->sink_per_tick * (u->shutdown ? 2.0f : 1.0f) * ticks;   /* this frame's gain */
    u->heat = u->heat_mark + net;
    if (u->heat < 0) u->heat = 0;
    u->seq_ticks += ticks;
    u->warn_ticks += ticks;
    /* flag 4 set and not overridden: shut down 1086 ticks after the sequence began, whatever the heat now */
    if (u->shutdown_pending && !u->override && !u->shutdown && u->seq_ticks > 1086.0f) {
        u->shutdown = 1;
        u->seq_ticks = 0;
        u->auto_shutdown = 1;              /* message 0xd "Shutting down" */
    }
    if (u->heat > 100.0f) {
        if (!u->invulnerable) {            /* an invulnerable player skips this branch */
            if (!u->override) {
                if (u->seq_ticks > 4525.0f) u->destroyed = 1;
            } else if (ticks > 0) {
                /* a roll per frame: 3D 0x100160c9 rand(4000 / dt) < 3 (3 in 4000 per tick); DOS 0x25115 rand(500 x dt)
                 * == 0, which depends on the frame rate (no fixed per-tick rate to port) - the 3Dfx rate is kept */
                u->rng = u->rng * 1103515245u + 12345u;
                if ((float)((u->rng >> 16) & 0x7FFF) / 32768.0f < 3.0f * ticks / 4000.0f) cook_off(u);
            }
        }
    } else if (u->heat > 80.0f) {
        if (!u->shutdown_pending) { u->shutdown_pending = 1; u->seq_ticks = 0; }   /* "Shutdown sequence initiated" */
    } else if (u->heat > 65.0f) {
        /* "Heat level critical": once, when this frame's net gain is positive; latched */
        if (!u->warn_latch && net > 0) { u->warn_latch = 1; u->warn_ticks = 0; u->heat_warning = 1; }
    } else if (!u->shutdown_pending) {
        if (u->warn_latch && u->warn_ticks > 724.0f) u->warn_latch = 0;
    } else {
        /* restart 724 ticks after c[0x23] (flag 4 cleared only here or by an override cooling down). Any shutdown
         * counts, a SHUTDOWN_MECH one too: the engine has no separate manual state (0x10015f50 tests flag 4 and state
         * 3 only), so a mech the pilot shut down during a sequence comes back up by itself - STARTUP_MECH is refused
         * while flag 4 is set without flag 8 (0x1001adf0), so nothing else could restart it */
        if (u->shutdown && u->seq_ticks > 724.0f) { u->shutdown = 0; u->shutdown_pending = 0; u->restarted = 1; u->manual_down = 0; }
        if (u->override) { u->override = 0; u->shutdown_pending = 0; }
    }
    u->heat_mark = u->heat;
}

/* An overridden overheat blows ammunition (DOS 0x24ec0, "BlowAmmo"; the 3Dfx DLL's 0x10015e10 differs, see below):
 * the first entry, in a standing location, holding the item id of the unit's FIRST ammo bin record (bins +6) takes a
 * critical (0x25960: the bin explodes if it still has rounds); no such entry left (or no bins at all): the mech is
 * destroyed (0x25340). So the second roll at the latest - more if that ammunition fills several slots - kills it
 * (DOSBox: overridden at full heat, the Timber Wolf blew up seconds later). The 3Dfx DLL instead takes the first ammo
 * entry whose weapon still has rounds and destroys the mech only once none is left, which with several ammo slots
 * (and bins already spent) lets an overridden mech run hot for minutes. */
static void cook_off(combat_unit *u)
{
    int l, k;
    if (u->bin_count > 0)
        for (l = 0; l < 8; l++) {
            if (u->loc_gone[l]) continue;
            for (k = 0; k < u->slot_count[l]; k++)
                if (u->slots[l][k] == u->bins[0].id) { crit_item(u, l + 1, k, 0); return; }
        }
    u->destroyed = 1;
}

/* engine location numbers 1-8 -> MEK index 0-7 (same order) */
static void destroy_location(combat_unit *t, int loc)   /* 1-8, engine 0x10016640 */
{
    int n;
    if (loc < 1 || loc > 8 || t->loc_gone[loc - 1]) return;
    t->loc_gone[loc - 1] = 1;
    t->internal[loc - 1] = 0;            /* only the structure is zeroed; the armour stays (display) */
    /* 0x10016600: every item in the list takes a silent critical (always entry 0: each one removes itself) */
    for (n = t->slot_count[loc - 1]; n > 0 && t->slot_count[loc - 1] > 0; n--) crit_item(t, loc, 0, 1);
    switch (loc) {
    case 2: destroy_location(t, 5); return;
    case 4: destroy_location(t, 6); return;
    case 5: case 6: return;
    case 3:
        destroy_location(t, 5); destroy_location(t, 2); destroy_location(t, 4); destroy_location(t, 6);
        destroy_location(t, 1);
        break;
    case 7: case 8:
        if (!t->immobile) { t->immobile = 1; t->speed_gain = 0; return; }   /* first leg: c[0x2c] = 0 */
        destroy_location(t, 1);
        destroy_location(t, 3);
        break;
    default:
        break;
    }
    t->destroyed = 1;
}

/* engine 0x100167b0: entry `idx` of location `loc`'s item list takes a critical (silent: a location being destroyed);
 * the entry is then removed from the list */
static void crit_item(combat_unit *t, int loc, int idx, int silent)
{
    int i = loc - 1, k;
    if (loc < 1 || loc > 8) return;
    for (;;) {
        uint32_t id, cls, eq;
        if (idx < 0 || idx >= t->slot_count[i]) return;
        id = t->slots[i][idx];
        cls = id / 100;
        if (cls < 50) {                                       /* a weapon (or an empty entry): that instance is gone */
            for (k = 0; k < t->weapon_count; k++)
                if (t->weapons[k].item == id) { t->weapons[k].state = -1; t->weapons[k].shots = 0; t->weapons[k].pulses_due = 0; break; }
            break;
        }
        if (cls > 100) {                                      /* ammunition (LRM 20 ammo, 10001-10099, is not: 0x1001680a) */
            int b;
            for (b = 0; b < t->bin_count; b++) {
                combat_bin *bn = &t->bins[b];
                combat_weapon *cw;
                if (bn->id != id) continue;
                cw = &t->weapons[bn->weapon];
                if (cw->ammo < 1) break;                       /* the weapon has none left: nothing more happens */
                cw->ammo -= bn->rounds;                        /* its rounds leave the weapon */
                if (cw->ammo == 0) { cw->state = -1; cw->shots = 0; }
                if (!silent && bn->rounds != 0) {
                    /* the bin explodes: rounds x per-round damage (bin +10 = table +0x30) into this location's structure */
                    t->internal[i] -= (float)bn->rounds * sim_weapons[cw->weapon].damage;
                    t->ammo_blown = 1;
                    if (t->human) t->red_flash = 1;   /* 0x10016b11: the player's bin - DAT_1024cce0 (the red flash) */
                    if (t->internal[i] < 0) {
                        t->internal[i] = 0;
                        t->slots[i][idx] = 0;
                        destroy_location(t, loc);
                    }
                    if (g_auto_eject) {
                        /* automatic ejection (0x100167b0, after the explosion's damage): state 5, then 0x10016280.
                         * Already destroyed by this explosion: 0x10016280 returns at once and the unit stays in state
                         * 5 - out of action, no death sequence. Else breathable: the player keeps state 5 (ejected),
                         * an AI mech is set to state 4 (an ordinary death, its pilot counted as ejected); hostile
                         * atmosphere: state 4 (the player: end status 4). Invulnerable player: 0x10016280 returns. */
                        if (t->destroyed) t->intact_out = 1;
                        else if (!(t->human && t->invulnerable)) {
                            t->ejected = g_planet.hostile ? 2 : 1;
                            t->intact_out = t->human && !g_planet.hostile;
                            t->destroyed = 1;
                        }
                    }
                }
                bn->rounds = 0;
            }
            break;
        }
        eq = (id / 10) * 10;                                   /* equipment */
        if (eq == 5500 || eq == 5550 || eq == 5600 || eq == 5650 || eq == 5800 || eq == 5850) {
            t->speed_gain -= 0.100006103515625f;               /* hip, leg actuators, gyro, engine */
            if (t->speed_gain < 0) t->speed_gain = 0;
            if (eq == 5800) { t->jet_fuel = -2; t->jet_ddy = 0; t->jets = 0; t->no_jump = 1; }   /* gyro: no jets */
        } else if (eq == 6000) {
            /* heat sink: 0.000763 less dissipation while it is positive (it can go below 0), else 0 */
            if (t->sink_per_tick > 0) t->sink_per_tick -= 0.000762939453125f; else t->sink_per_tick = 0;
        } else if (eq == 5750) t->destroyed = 1;              /* cockpit */
        else if (eq == 5900) { if (g_planet.hostile) t->destroyed = 1; }   /* life support */
        else if (eq == 7000) {                                 /* jump jet */
            if (t->jets < 1) t->jet_ddy = 0; else t->jet_ddy -= t->jet_ddy / (float)t->jets;
            if (t->jets > 0) t->jets--;
            if (t->jets == 0) t->jet_fuel = -2;
        }
        else if (eq >= 5300 && eq <= 5450) t->display_hits++;   /* cockpit displays: 0x1001e670(flag 0), each instrument 2 in 10 */
        else if (eq == 5700) t->display_hits5++;                /* 0x10016e33: 0x1001e670(flag 1), each instrument 5 in 10 */
        else if ((eq == 8000 || eq == 9000) && !silent) {      /* endo steel / ferro: another entry takes it, silently */
            idx = rand_n(&t->rng, t->slot_count[i]);
            silent = 1;
            continue;
        }
        if (fabsf(t->speed_gain) >= 1e-7f && t->speed_gain < 0.4f) t->speed_gain = 0.4f;   /* floor 0.4 (0x1001710b) */
        break;
    }
    if (t->slot_count[i] <= 0) return;                          /* already emptied (the location went with it) */
    for (k = idx; k < t->slot_count[i] - 1; k++) t->slots[i][k] = t->slots[i][k + 1];
    t->slots[i][k] = 0;
    t->slot_count[i]--;
}

static int hit_location(const combat_unit *t, unsigned *rng)
{
    float total = 0, r;
    int l;
    for (l = 0; l < 8; l++) total += t->hit_w[l];
    if (total <= 0) {
        static const float fallback[8] = {2, 14, 26, 14, 10, 10, 12, 12};   /* unknown geometry */
        int i;
        float ft = 0;
        for (i = 0; i < 8; i++) ft += fallback[i];
        r = frand(rng) * ft;
        for (i = 0; i < 8; i++) { if (r < fallback[i]) return i + 1; r -= fallback[i]; }
        return 3;
    }
    r = frand(rng) * total;
    for (l = 0; l < 8; l++) { if (r < t->hit_w[l]) return l + 1; r -= t->hit_w[l]; }
    return 3;
}

static void apply_damage_at(combat_unit *t, float dmg, int rear, int loc, unsigned *rng);

static void apply_damage(combat_unit *t, float dmg, int rear, unsigned *rng)
{
    apply_damage_at(t, dmg, rear, hit_location(t, rng), rng);
}

int combat_damage_live = 1;
static void apply_damage_at(combat_unit *t, float dmg, int rear, int loc, unsigned *rng)
{
    int i, r, n;
    float *arm;
    /* engine 0x100171d0: nothing for an invulnerable player, damage of 1e-7 or less, or a location outside 1-8 - and
     * nothing at all before the player's first start-up is done (DAT_1024c570; DOSBox WHITSCN1 / PLUMSCN1: the dropped
     * player lands hard, MECMTNHD, the damage display stays blue) */
    if (!combat_damage_live || t->invulnerable || !(dmg > 1e-7f) || loc < 1 || loc > 8) return;
    if (loc == 3) loc = 2 + rand_n(rng, 3);                  /* CT hit -> LT / CT / RT */
    i = loc - 1;
    t->damage_taken += (int)dmg;
    if (t->loc_gone[i]) return;                              /* already gone: no transfer (nothing more can happen) */
    arm = (rear && (loc == 2 || loc == 3 || loc == 4)) ? &t->rear[i] : &t->armor[i];
    *arm -= dmg;
    if (*arm >= 1e-7f) return;
    /* 0x1001732e: the first time a location's armour is used up (flag 0x4000, front and rear share it) the player, up
     * (state 2), hears MECRDWA1 0xec */
    if (!(t->breach_latch & (1u << i))) { t->breach_latch |= 1u << i; if (t->human && !t->shutdown && !t->destroyed) t->breach_sound = 1; }
    /* breached (armour at or below 0): the rest into this location's structure */
    t->internal[i] += *arm;
    *arm = 0;
    /* 0x10017383: the player's head (1) or centre torso (3, after the CT remap) breached by a hit of more than 2 points,
     * not yet destroyed (state 4): the red flash (DAT_1024cce0) */
    if (t->human && (loc == 1 || loc == 3) && !t->destroyed && dmg > 2.0f) t->red_flash = 1;
    if (t->internal[i] < 1e-7f) { destroy_location(t, loc); return; }
    /* critical hits: rand(5) rolls (re-drawn each pass); rand(100) < 20 with items in the list is one: 12 destroys the
     * location, else rand(count) picks the entry */
    for (n = 0; n < rand_n(rng, 5); n++) {
        r = rand_n(rng, 100);
        if (r < 20 && t->slot_count[i] > 0) {
            if (r == 12) { destroy_location(t, loc); return; }
            crit_item(t, loc, rand_n(rng, t->slot_count[i]), 0);
            if (t->destroyed || t->loc_gone[i]) return;
        }
    }
}

int combat_fire(combat_unit *u, combat_unit *t, float distance, float rel_angle, unsigned *rng)
{
    int rear;
    while (rel_angle > 180.0f) rel_angle -= 360.0f;
    while (rel_angle < -180.0f) rel_angle += 360.0f;
    rear = rel_angle > -90.0f && rel_angle < 90.0f;
    int i, dealt = 0;
    if (u->destroyed || t->destroyed) return 0;
    for (i = 0; i < u->weapon_count; i++) {
        combat_weapon *cw = &u->weapons[i];
        const sim_weapon *sw = &sim_weapons[cw->weapon];
        float range = range_cm(cw->weapon), p;
        /* trigger */
        if (cw->state == CW_READY && !u->shutdown && u->internal[cw->location] > 0 && distance <= range &&
            !(u->hold_fire_above > 0 && u->heat > u->hold_fire_above)) {
            start_volley(u, cw);
        }
        /* projectiles released since the last call (assumed: instant hit, range-based odds) */
        p = 0.85f - 0.35f * (distance / range);
        if (distance > range) p = 0;
        while (cw->pulses_due > 0) {
            cw->pulses_due--;
            if (frand(rng) < p) {
                apply_damage(t, sw->damage, rear, rng);
                dealt += (int)(sw->damage + 0.5f);
                t->heat += sw->target_heat;   /* PPC / flamer heat the target (engine 0x10044f60) */
            }
            if (t->destroyed) { cw->pulses_due = 0; break; }
        }
    }
    return dealt;
}

void combat_override(combat_unit *u)
{
    if (u->shutdown_pending && !u->shutdown) u->override = 1;
}

int combat_health(const combat_unit *u)
{
    int l;
    float h = 0;
    for (l = 0; l < MEK_LOC_COUNT; l++) h += u->armor[l] + u->rear[l] + u->internal[l];
    return (int)h;
}

int combat_out_of_weapons(const combat_unit *u)
{
    /* 0x10020880: 1 only when the unit HAS weapon slots (+0xa8 != 0) and none is usable (slot +8 != -1 and +0x20 != 0;
     * here a weapon left empty is state -1). An unarmed unit (a vehicle with no weapons, MARO's limousine) is not
     * "out of weapons" - it never takes the forced flee (0x1001ea40) */
    int i;
    if (u->weapon_count <= 0) return 0;
    for (i = 0; i < u->weapon_count; i++) if (u->weapons[i].state != -1) return 0;
    return 1;
}

float combat_max_range(const combat_unit *u)
{
    int i;
    float r = 0;
    for (i = 0; i < u->weapon_count; i++) if (range_cm(u->weapons[i].weapon) > r) r = range_cm(u->weapons[i].weapon);
    return r;
}

void combat_trigger(combat_unit *u, float distance) { combat_trigger_mask(u, distance, 0xFFFFFFFFu); }

void combat_trigger_mask(combat_unit *u, float distance, uint32_t mask)
{
    int i;
    if (u->destroyed || u->shutdown || u->offline) return;
    /* a human pilot (0x100437a0 / 0x10044440): a weapon fires on the press of its trigger, not while it is held (a
     * repeating weapon re-fires from the volley's end, see release_due), and with no range check */
    if (u->human) mask &= u->fire_edge;
    for (i = 0; i < u->weapon_count; i++) {
        combat_weapon *cw = &u->weapons[i];
        if (i < 32 && !(mask & (1u << i))) continue;
        const sim_weapon *sw = &sim_weapons[cw->weapon];
        if (cw->state == CW_READY && cw->ammo != 0 && u->internal[cw->location] > 0 && !u->loc_gone[cw->location] &&
            (u->human || distance <= range_cm(cw->weapon)) &&
            /* AI (0x100232b0): fire a weapon only if heat + its heat stays under 65 */
            !(u->hold_fire_above > 0 && u->heat + sw->heat >= u->hold_fire_above))
            start_volley(u, cw);
    }
}

int combat_ai_fire(combat_unit *u, float distance, unsigned (*rng)(void *ctx, unsigned n), void *ctx)
{
    int tries, n = u->weapon_count;
    if (u->destroyed || u->shutdown || u->offline || n <= 0) return -1;
    if (u->ai_sel < 0 || u->ai_sel >= n) u->ai_sel = 0;
    for (tries = 0; tries < n; tries++) {
        int i = u->ai_sel, k;
        combat_weapon *cw = &u->weapons[i];
        const sim_weapon *sw = &sim_weapons[cw->weapon];
        int usable = cw->state >= 0 && u->internal[cw->location] > 0 && !u->loc_gone[cw->location];
        /* 0x100151a0: 3 inside the minimum range (+0x3c), 2 inside the range, 1 at it, 0 beyond: fires on 1-2 */
        if (usable && distance >= (float)sw->field3c && distance <= range_cm(cw->weapon) &&
            sw->heat + u->heat < 65.0f && cw->state == CW_READY && cw->ammo != 0 &&
            rng(ctx, (unsigned)(sw->refire / 90 + 1)) == 0) {
            start_volley(u, cw);
            return i;
        }
        for (k = 1; k <= n; k++) {   /* 0x10043f80: the next weapon that is still there */
            int j = (i + k) % n;
            if (u->weapons[j].state >= 0 && !u->loc_gone[u->weapons[j].location] && u->internal[u->weapons[j].location] > 0) { u->ai_sel = j; break; }
        }
    }
    return -1;
}

int combat_next_projectile_ex(combat_unit *u, int *first) { return combat_next_projectile_at(u, first, NULL); }
int combat_next_projectile_at(combat_unit *u, int *first, int *loc)
{
    int i;
    for (i = 0; i < u->weapon_count; i++)
        if (u->weapons[i].pulses_due > 0) {
            u->weapons[i].pulses_due--;
            if (first) *first = u->weapons[i].volley_start;
            u->weapons[i].volley_start = 0;
            if (loc) *loc = u->weapons[i].location;
            return u->weapons[i].weapon;
        }
    return -1;
}

int combat_next_projectile(combat_unit *u) { return combat_next_projectile_ex(u, NULL); }
int combat_weapon_kind(int w) { return (w >= 0 && w < 31) ? sim_weapons[w].type : -1; }
int combat_weapon_has_object(int w) { return w >= 0 && w < 31 && !(sim_weapons[w].type == 7 && sim_weapons[w].visual < 0); }   /* +0x14: 0 only for the flamer (3D edition table) */
float combat_weapon_damage(int w) { return (w >= 0 && w < 31) ? sim_weapons[w].damage : 0; }
int combat_weapon_sound(int w) { return (w >= 0 && w < 31) ? sim_weapons[w].sound : 0; }
int combat_weapon_visual(int w) { return (w >= 0 && w < 31) ? sim_weapons[w].visual : -1; }

int combat_hit(combat_unit *t, int w, int loc, float rel_angle, unsigned *rng)
{
    int rear;
    if (t->destroyed || w < 0 || w >= 31) return 0;
    while (rel_angle > 180.0f) rel_angle -= 360.0f;
    while (rel_angle < -180.0f) rel_angle += 360.0f;
    rear = rel_angle > -90.0f && rel_angle < 90.0f;
    apply_damage_at(t, sim_weapons[w].damage, rear, loc, rng);
    t->heat += sim_weapons[w].target_heat;
    return (int)(sim_weapons[w].damage + 0.5f);
}

int combat_crit_ammo(combat_unit *u)
{
    int l, k, b;
    if (u->destroyed) return 0;
    for (b = 0; b < u->bin_count; b++) {
        if (u->bins[b].id / 100 <= 100 || u->bins[b].rounds < 1) continue;   /* LRM 20 ammunition never explodes (0x1001680a) */
        for (l = 0; l < 8; l++) {
            if (u->loc_gone[l]) continue;
            for (k = 0; k < u->slot_count[l]; k++)
                if (u->slots[l][k] == u->bins[b].id) { crit_item(u, l + 1, k, 0); return 1; }
        }
    }
    return 0;
}

int combat_jettison(combat_unit *u, int wi)
{
    combat_weapon *cw;
    if (!u || wi < 0 || wi >= u->weapon_count) return 0;
    cw = &u->weapons[wi];
    if (sim_weapons[cw->weapon].per_volley <= 0 || cw->ammo <= 0) return 0;   /* an energy weapon (-1) or empty */
    /* engine 0x10044220: the count goes to 0 and the weapon is spent (state -1); its bins are left as they are,
     * which is harmless - a bin feeding a weapon with no rounds neither explodes nor cooks off (0x100167b0, 0x10015e10) */
    cw->ammo = 0;
    cw->state = -1; cw->shots = 0;
    return 1;
}

int combat_has_masc(const combat_unit *u)
{
    int l, k;
    for (l = 0; l < 8; l++) for (k = 0; k < MEK_SLOTS; k++) if (u->slots[l][k] >= 5000 && u->slots[l][k] < 5050) return 1;
    return 0;
}

float combat_weapon_range(int w) { return (w >= 0 && w < 31) ? range_cm(w) : 0; }
float combat_weapon_speed(int w) { return (w >= 0 && w < 31) ? (float)sim_weapons[w].speed : 0; }
int   combat_weapon_life(int w) { return (w >= 0 && w < 31) ? sim_weapons[w].life : 0; }
int   combat_weapon_drop(int w) { return (w >= 0 && w < 31) ? sim_weapons[w].drop : 0; }

void combat_jets_forward(combat_unit *u, int on, float dt, float *hvel)
{
    u->jet_forward = on;
    if (on && u->jets > 0 && u->jet_fuel > 0 && !u->no_jump && !u->shutdown) *hvel += u->jet_ddy * dt * COMBAT_TICKS_PER_S;
}

int combat_jets(combat_unit *u, int jetting, float dt, unsigned *rng)
{
    const float g = 0.0296f * g_planet.gravity_g;
    float ticks = dt * COMBAT_TICKS_PER_S, a;
    /* engine 0x1001a58a: with the input held (and the mech up) the tank only drains - to 0, never refilling while
     * held; it refills a quarter tick per tick only with the input released; thrust (and jet heat) only while the
     * tank holds fuel after this frame's burn. A tank at -2 (no jets: gyro / last jet lost) does neither */
    int held = jetting && u->jets > 0 && !u->no_jump && !u->shutdown && !u->destroyed && u->jet_fuel >= 0;
    int jet;
    u->landed_vy = 0;
    if (u->jets > 0 && u->jet_fuel >= 0) {
        if (held) {
            u->jet_fuel -= ticks;
            if (u->jet_fuel <= 0) u->jet_fuel = 0;
        } else if (u->jet_fuel < 1810.0f) {
            u->jet_fuel += ticks * 0.25f;
            if (u->jet_fuel > 1810.0f) u->jet_fuel = 1810.0f;
        }
    }
    jet = held && u->jet_fuel > 0;
    if (jet) {
        u->heat += ticks * (float)u->jets * 0.005859375f;           /* 3/512 per jet per tick */
        if (u->no_heat) u->heat = 0;
    }
    /* standing / walking: follow the terrain up, and down by up to 10 m (the engine's ground window,
     * 0x10010080); a bigger drop starts a fall */
    if (!jet && fabsf(u->vy) < 1e-7f && u->y - u->ground <= 1000.0f) { u->y = u->ground; u->vy = 0; return 0; }
    a = -g;
    if (jet) a = u->jet_ddy - g + (u->vy > 1e-7f ? (g - u->jet_ddy) / g_planet.jet_damping * u->vy * 3620.0f : 0);
    if (jet && u->jet_forward) a = u->vy > 1e-7f ? -g : 0;   /* forward thrust: no lift, level glide */
    u->y += (0.5f * a * ticks + u->vy) * ticks;
    u->vy += a * ticks;
    if (u->y <= u->ground) {
        if (u->vy < -16.154f && !u->no_collision_damage) {               /* fall damage to both legs (0x10019310: only with
                                                                          * Collision Damage on, DAT_1024ac6c + 3) */
            float dmg = (u->vy + 16.154f) * -1.5476f;
            apply_damage_at(u, dmg, 0, 8, rng);
            apply_damage_at(u, dmg, 0, 7, rng);
        }
        u->landed_vy = u->vy;   /* the touch-down (0x10019e28..: sound and camera jolt when faster than 5.384 cm/tick) */
        u->y = u->ground;
        u->vy = 0;
        return 0;
    }
    return 1;
}

/* engine 0x1000c1f0: where a collision lands, by the contact normal (pointing from what was struck toward the unit, y up):
 * normal y below -0.866 (struck from above: the unit ran up into it) - the head, the damage x max(1, 2 x the other's
 * tonnage / its own) when the other is a unit; normal y above 0.7071 (ground-like, or on top of a unit) - half to each
 * leg; else by the bearing of the struck surface from the torso's facing: 45-135 degrees to a side that arm, else that
 * side's torso */
static void collide(combat_unit *u, float speed, float ny, float rel_deg, int other_tons, int latch, unsigned *rng)
{
    float dmg = (speed - 3.0693676f) * 0.050123077f * 3.0f;
    if (latch) u->blocked = 1;   /* terrain (0x10019d84 -> 0x1000c3c0) and a unit (0x10019d77 -> 0x1000c160) alike: every contact
                                  * frame - the impact cuts the velocity to a quarter (msim_world_impact), so pressing on stays
                                  * under the threshold; blocked = the contact (the player's impact-sound latch 0x1007c338) */
    if (dmg <= 0 || u->destroyed || u->invulnerable || u->no_collision_damage || getenv("MW2_NO_COLLISION_DAMAGE")) return;   /* Combat Variables */
    if (ny < -0.8660254f) {                       /* from above: the head (0x1000c212) */
        if (other_tons > 0) {
            float k = 2.0f * (float)other_tons / (float)(u->tons > 0 ? u->tons : 1);
            if (k > 1.0f) dmg *= k;
        }
        apply_damage_at(u, dmg, 0, 1, rng);
        return;
    }
    if (ny > 0.70710677f) {                       /* ground-like: half to each leg */
        apply_damage_at(u, dmg * 0.5f, 0, 7, rng);
        apply_damage_at(u, dmg * 0.5f, 0, 8, rng);
        return;
    }
    while (rel_deg > 180.0f) rel_deg -= 360.0f;
    while (rel_deg < -180.0f) rel_deg += 360.0f;
    if (rel_deg < 0) apply_damage_at(u, dmg, 0, (rel_deg > -135.0f && rel_deg < -45.0f) ? 6 : 4, rng);   /* left arm / left torso (groups 6 / 4) */
    else apply_damage_at(u, dmg, 0, (rel_deg > 45.0f && rel_deg < 135.0f) ? 5 : 2, rng);                 /* right arm / right torso (groups 5 / 2) */
}

void combat_collision(combat_unit *u, float speed, float ny, float rel_deg, unsigned *rng)
{
    collide(u, speed, ny, rel_deg, 0, 1, rng);
}

void combat_collision_unit(combat_unit *u, float dv, const float n[3], float facing_deg, int other_tons, unsigned *rng)
{
    collide(u, dv, n[1], atan2f(-n[0], -n[2]) * 57.29578f - facing_deg, other_tons, 0, rng);
}

void combat_set_player_difficulty(combat_unit *u, int difficulty)
{
    /* engine 0x10041410, the player's dissipation per tick x heat sinking, by difficulty: normal / colder than -30 /
     * hotter than 50 */
    static const float F[3][3] = {{0.0010681152f, 0.0021362305f, 0.0010681152f}, {0.000762939453f, 0.00152587891f, 0.000686645508f},
                                  {0.000686645508f, 0.00102996826f, 0.000549316406f}};
    int t = g_planet.temperature < -30 ? 1 : g_planet.temperature > 50 ? 2 : 0;
    if (difficulty < 0 || difficulty > 2) difficulty = 1;
    u->sink_per_tick = u->sink_per_tick / F[1][t] * F[difficulty][t];   /* the unit was set up at Medium */
}
