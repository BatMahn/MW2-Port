/* msim.c - see msim.h. */
#include "development.h"
#include "msim.h"
#include "fx.h"
#include "datapath.h"
#include "dos_matrix.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <limits.h>

#include "bwd.h"

/* The native DOS controller first runs about 168 ticks into the mission clock. */
#define DOS_FIRST_CONTROL_TICK 168
#define DOS_STARTUP_BASE_TICKS 0x445

static unsigned rnd_ms(unsigned *x);
static unsigned ms_roll(void *ctx, unsigned n);
static int32_t dos_gravity(float gravity_g);
static int32_t dos_projectile_mul(int32_t a, int32_t b);

static void dos_random_init(msim *s)
{
    unsigned seed = 0;
    for (size_t i = 0; i < sizeof s->dos_random / sizeof s->dos_random[0]; i++)
        s->dos_random[i] = (uint16_t)rnd_ms(&seed);
    s->dos_random_index = 0;
    /* MW2.EXE 0x49fe0 fills Gaussian entries 1..127 after the uniform table.
     * The Gaussian cursor at 0x4a0b0 advances before it reads and wraps afterward. */
    const double root = sqrt(90.0);
    const double scale = root * 2.0 * 1.0172836491999064e-6;
    s->dos_gaussian[0] = 0;
    for (size_t i = 1; i < sizeof s->dos_gaussian / sizeof s->dos_gaussian[0]; i++) {
        unsigned sum = 0;
        for (int j = 0; j < 30; j++) sum += rnd_ms(&seed);
        s->dos_gaussian[i] = (int32_t)((sum * scale - root) * 1024.0);
    }
    s->dos_gaussian_index = 0;
}

static unsigned startup_ticks(msim *s)
{
    if (!msim_dos_int) return 0x43e + ms_roll(s, 0x16a);
    return DOS_STARTUP_BASE_TICKS + ms_roll(s, 0x16c);   /* 0x2823f waits through the deadline. */
}

static float startup_deadline(const msim *s, unsigned delay, int initial)
{
    if (!msim_dos_int) return (float)s->now + (float)delay * 1000.0f / 182.0f;
    int start = msim_dos_frame_ticks(0, s->now);
    if (initial) start += s->dos_boot_ticks;
    return (float)(delay + (unsigned)start) * 1000.0f / 182.0f;
}

static float g_block_n[3] = {0, 1, 0};   /* see refuse() */
static const char *g_block_what;   /* TEST ONLY (MW2_BLOCK_TRACE): what refused the last step */
#define MSIM_CONTACT_HOLD 10   /* 1/30 s moves without a touch before contact mode ends (sliding round a mech: TNJ1 at most 3 between touches) */
static int g_block_id = -1;        /* what refused the last msim_can_step_hr move: the world part (a terrain piece too), -1 none */
static int g_hit_kind;             /* the last refusal: 1 the segment hit (g_hit_t along it), 2 a type 6 sphere (g_push) */
static float g_hit_t, g_push[3], g_hit_at[4];   /* the hit's length along the extended segment; the pushed centre; the object's sphere */
static float g_hit_ext;            /* the extension of the last tested move (R walking; msim_can_move) */
static int g_impact_part = -1;     /* the object of the last msim_world_impact (its first hit) and its centre */
static float g_impact_at[4];
static const msim_nav *find_nav(const msim *s, const char *name)
{
    int i;
    for (i = 0; i < s->nav_count; i++) if (strcasecmp(s->navs[i].name, name) == 0) return &s->navs[i];
    return NULL;
}

static world_actor *group_leader(msim *s, int table)
{
    /* the first living member leads (the original leader, or its successor once it is destroyed) */
    int i;
    for (i = 0; i < s->actor_count; i++)
        if (s->actors[i].table == table && s->actors[i].leader && !(s->armed && s->armed[i] && s->units[i].destroyed)) return &s->actors[i];
    for (i = 0; i < s->actor_count; i++)
        if (s->actors[i].table == table && !(s->armed && s->armed[i] && s->units[i].destroyed)) return &s->actors[i];
    return NULL;
}

/* ---- lancemate orders (engine 0x10014390 / 0x10014290 / 0x10031810) ---- */
const char *const MSIM_STATUS_TEXT[14] = {"None ", "Idle ", "Avoiding ", "Targeting ", "Engaging ", "Disengaging ",
    "In Formation ", "Reconning ", "Defending ", "En Route ", "Disengaging ", "Silent ", "Shutdown ", "Destroyed "};
const char *const MSIM_FORMATION_TEXT[7] = {"Echelon Left", "Echelon Right", "Line Abreast", "Line Astern", "V Form",
    "Wedge", "No Formation"};
/* the acknowledgement table 0x1024ec58: {voice id, text}; the prefix table 0x1024eb40 {voice, "All points" ..} */
static const struct { int voice; const char *text; } ORDER_MSG[11] = {
    {0x00, "cannot do this"}, {0x0a, "attacking your target"}, {0xb4, "joining formation"},
    {0x10a, "patrolling your target"}, {0x71, "running awayyyyyy"}, {0x22, "reports target destroyed"},
    {0x23, "destroyed"}, {0x36, "critical hit"}, {0x0c, "ejecting"}, {0x0d, "reports system shutdown"},
    {0x2a, "reports task complete. Rejoining formation"}};
static const char *const POINT_TEXT[3] = {"All points", "Point 2", "Point 3"};
static const int POINT_VOICE[3] = {2, 264, 265};   /* 0x1024eb40 {voice, text}: ALLPOINT, Point 2, Point 3 */

/* the player's starmates in lance order: point 1 = the first */
static int star_member(const msim *s, int point)
{
    int i, n = 0;
    for (i = 0; i < s->actor_count; i++)
        if (s->actors[i].friendly && s->actors[i].table == 0 && ++n == point) return i;
    return -1;
}
/* the units an order to all points reaches: the star (the player's group); with the cheat "lairdo" every unit on the
 * player's side (NextTarget's c_aiTargetFriendly, Win95 0x10054043: also any player of side 0) */
static int order_member(const msim *s, int point)
{
    int i, n = 0;
    if (!s->lairdo) return star_member(s, point);
    for (i = 0; i < s->actor_count; i++)
        if (s->actors[i].friendly && ++n == point) return i;
    return -1;
}
static int star_point(const msim *s, int actor)
{
    int i, n = 0;
    for (i = 0; i < s->actor_count; i++)
        if (s->actors[i].friendly && s->actors[i].table == 0) { n++; if (i == actor) return n; }
    return 0;
}
static void msim_order_message(msim *s, int point, int msg)
{
    char line[96];
    if (msg < 0 || msg > 10) return;
    /* 0x10031810(msg, point): the point's entry first (table 0x1024eb40), the message's second (0x1024ec58) - both
     * voices said in turn, the texts joined by a space (0x10031150); no point (-1): the message alone */
    if (point >= 0 && point < 3) snprintf(line, sizeof line, "%s %s", POINT_TEXT[point], ORDER_MSG[msg].text);
    else snprintf(line, sizeof line, "%s", ORDER_MSG[msg].text);
    if (s->on_message) s->on_message(s->message_user, line, point >= 0 && point < 3 ? POINT_VOICE[point] | ORDER_MSG[msg].voice << 16 : ORDER_MSG[msg].voice);
    if (s->log) s->log(s->log_user, line);
}

int msim_point_actor(const msim *s, int point)
{
    int i = star_member(s, point);
    return i >= 0 && !(s->armed[i] && s->units[i].destroyed) ? i : -1;
}

int msim_point_status(const msim *s, int point)
{
    int i = star_member(s, point);
    if (i < 0) return 0;
    if (s->armed[i] && s->units[i].destroyed) return 13;
    return s->minds ? s->minds[i].state + 1 : 0;
}

static int thing_point(const msim *s, int b, float p[3]);
static void guard_drop(ai_mind *m);
static void guard_enter(const msim *s, ai_mind *m, int code);
int msim_command(msim *s, int point, int order, int target)
{
    int i, first = point ? point : 1, last = point ? point : 64, msg = -1, given = 0;
    static const int MENU_INDEX[12] = {7, 7, 2, 1, 7, 3, 7, 4, 5, 7, 7, 6};   /* 0x10264468 values per order */
    if (!s->cmd || !s->minds) return 0;
    if (point && msim_point_actor(s, point) < 0) return 0;          /* 0x10014220: no such point */
    switch (order) {
    case MSIM_CMD_ATTACK: msg = 1; break;
    case MSIM_CMD_JOIN: msg = 2; break;
    case MSIM_CMD_DEFEND: msg = 3; break;
    case MSIM_CMD_DISENGAGE: msg = 4; break;
    case MSIM_CMD_SHUTDOWN: msg = 9; break;
    case MSIM_CMD_ENGAGE: break;
    default: return 0;
    }
    if (order == MSIM_CMD_ATTACK && MSIM_IS_THING(target)) {
        float p[3];
        if (!thing_point(s, MSIM_THING_OF(target), p)) return 0;
    } else if ((order == MSIM_CMD_ATTACK || order == MSIM_CMD_DEFEND) &&
               (target < 0 || target >= s->actor_count || !s->armed[target] || s->units[target].destroyed)) return 0;
    for (i = first; i <= last; i++) {
        int a = point ? star_member(s, i) : order_member(s, i);
        ai_mind *m;
        if (a < 0) break;
        if (s->armed[a] && s->units[a].destroyed) continue;            /* state 12: cannot */
        m = &s->minds[a];
        if (a == target) continue;
        if (order == MSIM_CMD_ENGAGE) {                                /* toggles engage; the command flags clear */
            s->engage[a] = !s->engage[a];
            s->cmd[a] = 0; s->cmd_target[a] = -1;
            given = 1;
            continue;
        }
        if (s->ai_ok && !ai_allows(&s->ai, m, order)) continue;       /* 0x10013d90: the program lacks the state */
        s->cmd[a] = order;
        s->cmd_target[a] = order == MSIM_CMD_ATTACK || order == MSIM_CMD_DEFEND ? target : -1;
        if (order == MSIM_CMD_JOIN) s->engage[a] = 0;
        if (m->state == AI_FOLLOW || m->state == AI_PATROL) guard_drop(m);   /* exit 0x10013680 (5 / 7) -> 0x10013e90 */
        m->state = order; m->target = order == MSIM_CMD_ATTACK;
        if (s->defend_leg) s->defend_leg[a] = 0;
        if (order == MSIM_CMD_DEFEND) guard_enter(s, m, 0x20000 | (target + 1));   /* entry case 7 -> 0x10013f00 */
        given = 1;
    }
    s->last_cmd[point < 8 ? point : 7] = MENU_INDEX[order];
    /* Engage at Will sends no message: 0x10014390 leaves the message index at -1 for it (0x10031870's
     * engaged / disengaged lines belong to the systems: MASC, autopilot, chain fire ...) */
    if (order != MSIM_CMD_ENGAGE) msim_order_message(s, point, msg);
    (void)given;
    return 1;
}

int msim_set_formation(msim *s, int formation)
{
    static const char *const FTBL[6] = {"echelonl", "echelonr", "lineabreast", "lineastern", "vform", "wedge"};
    static const char *const SAID[6] = {"echelon left", "echelon right", "line abreast", "line astern", "vee form", "wedge"};
    char line[64];
    int p, a;
    if (formation < 0 || formation > 5 || !s->arch) return -1;
    for (p = 1; (a = star_member(s, p)) >= 0; p++) {
        float x, z;
        if (world3d_formation(s->arch, FTBL[formation], p, &x, &z) == 0) { s->actors[a].form_x = x; s->actors[a].form_z = z; }
    }
    s->star_formation = formation;
    s->last_cmd[0] = 0;
    snprintf(line, sizeof line, "Formation change to %s", SAID[formation]);   /* 0x10031850, table 0x1024ed40 */
    {   /* table 0x1024ed40 {voice, text}: "Formation change to" 11, then the name's own line (79 / 81 / 82 / 83 / 86 / 87),
         * passed as voice | name << 16 (the front end queues the second) */
        static const int NV[6] = {79, 81, 82, 83, 86, 87};
        if (s->on_message) s->on_message(s->message_user, line, 0x0b | NV[formation] << 16);
    }
    if (s->log) s->log(s->log_user, line);
    return 0;
}

/* position of a named target: nav point, a group (by its spawn record), or the player */
/* a node's target naming the player: "UserStar" (the shell's star), or the record its GPS came from - a trial's own star
 * record (JENNSTAR: Trial 1's stars are ordered to destroy "jennstar", which had matched nothing, so they stood idle until
 * they saw the player - user report), Cadet Training's TNx#USS1 */
static int is_player_rec(const msim *s, const char *t)
{
    return t && (strcasecmp(t, "UserStar") == 0 || (s->player_rec[0] && strcasecmp(t, s->player_rec) == 0));
}

static int load_nav_chunks(msim *s, const char *record, const bwd_chunk *chunks, int count, int player_table)
{
    for (int k = 0; k < count && s->nav_count < MSIM_MAX_NAVS; k++) {
        bwd_navpoint nv;
        if (bwd_navp(&chunks[k], &nv) != 0) continue;
        msim_nav *nav = &s->navs[s->nav_count++];
        snprintf(nav->name, sizeof nav->name, "%s", record);
        snprintf(nav->label, sizeof nav->label, "%.21s", nv.name);
        nav->selectable = nv.shown != 0 && nv.team == player_table;
        nav->shown = nv.shown != 0;
        nav->x = (float)nv.x;
        nav->y = (float)nv.y;
        nav->z = (float)nv.z;
        nav->radius = nv.radius > 0 ? (float)nv.radius * 100.0f : 20000.0f;
        nav->cls = (unsigned)(chunks[k].data[26] | chunks[k].data[27] << 8);
        return 1;
    }
    return 0;
}
static int target_pos(msim *s, const char *name, float *x, float *z, float *radius)
{
    const msim_nav *n = find_nav(s, name);
    int i;
    *radius = 20000.0f;
    /* objects and groups: 200 m (engine 0x10009b80 compares squared distance with 4e8) */
    if (is_player_rec(s, name)) { *x = s->player[0]; *z = s->player[2]; *radius = 20000.0f; return 0; }
    if (n) { *x = n->x; *z = n->z; *radius = n->radius; return 0; }
    for (i = 0; i < s->actor_count; i++)
        if (s->actors[i].leader && strcasecmp(s->actors[i].group, name) == 0) {
            *x = (float)s->actors[i].mech.origin[0]; *z = (float)s->actors[i].mech.origin[2]; *radius = 20000.0f;
            return 0;
        }
    return -1;
}

/* turrets (class 3) and doors (class 7) have their own controller and locomotion (0x10021a50 / 0x100219e0, 0x10021f20 /
 * 0x10021e60): they never walk, turn their body or fall - a turret stays where it was placed, turning only its twist */
/* High-altitude fighters have a ground reference and a separate model
 * altitude. Aiming uses the model, while floor lookup uses the reference. */
static float high_flight_lift(const msim *s, int i)
{
    return s->draw_lift && s->draw_lift[i] > 20000.0f ? s->draw_lift[i] : 0.0f;
}

static float actor_aim_base(const msim *s, int i)
{
    return s->units[i].y + high_flight_lift(s, i);
}

static int fixed_unit(const world_actor *ac) { return ac->unit_class == 3 || ac->unit_class == 7; }
static unsigned class_widen(unsigned c) { if (c & 0x730) c |= 0x730; if (c & 3) c |= 3; return c; }   /* 0x1003ddf0 */
/* A node's targets and their test (engine 0x1003ff60 attach, 0x1000a1d0 / 0x10009a50 / 0x10009ab0 / 0x10009b80), see
 * mtbl_world.targets. Attached: units of the record (GPS class +0x18; the player for UserStar, its USERSTAR GPS), nav
 * points of the record (NAVP class +0x1a) and things of the record (GT class +0x12), each when its class widened by
 * 0x1003ddf0 shares a bit with the node kind - at most 40 (0x28). Per target:
 *   destroyed (kinds 1 / 2 / 4): a unit's flags & 0xe, a thing's flags & 0xe, a nav point never
 *   visited (8 / 0x20): a unit inspected by the side, a nav point reached by the player (flag 0x20, set by the reach
 *     test below), a thing inspected
 *   within (0x100 / 0x2000), from the table's unit (table 0: the player; else the group's leader): a nav point within
 *     its radius (3D; radius < 1 -> 200 m), a unit or a thing within 200 m (3D, squared distance <= 4e8, _DAT_1024710c) */
static int tgt_within(msim *s, int table, int type, int idx)
{
    float ux, uy, uz, tx, ty, tz, r = 20000.0f;
    if (table == 0) { ux = s->player[0]; uy = s->player_unit.y; uz = s->player[2]; }
    else {
        world_actor *l = group_leader(s, table);
        if (!l) return 0;
        ux = (float)l->mech.origin[0]; uy = s->units[l - s->actors].y; uz = (float)l->mech.origin[2];
    }
    if (type == 1) {
        msim_nav *n = &s->navs[idx];
        tx = n->x; ty = n->y; tz = n->z; r = n->radius >= 1.0f ? n->radius : 20000.0f;
    } else if (type == 2) {
        if (idx < 0) { tx = s->player[0]; ty = s->player_unit.y; tz = s->player[2]; }
        else { tx = (float)s->actors[idx].mech.origin[0]; ty = actor_aim_base(s, idx); tz = (float)s->actors[idx].mech.origin[2]; }
    } else {
        const mech3d_part *p = &s->world->parts[s->bld[idx].intact];
        tx = (float)p->pos[0]; ty = (float)p->pos[1]; tz = (float)p->pos[2];
    }
    if ((ux - tx) * (ux - tx) + (uy - ty) * (uy - ty) + (uz - tz) * (uz - tz) > r * r) return 0;
    if (type == 1 && table == 0 && s->navs[idx].shown) s->navs[idx].visited = 1;   /* 0x10009b80: the player's side has been there (0x20;
                                                     * the player's unit, a nav with NAVP +16 set - DOS 0x16190 the same) */
    return 1;
}
static int tgt_test(msim *s, int table, uint32_t kind, int type, int idx)
{
    if (kind & (MTBL_K_DESTROY1 | MTBL_K_DESTROY2 | MTBL_K_PROTECT)) {
        if (type == 1) return 0;
        if (type == 2) return idx < 0 ? s->player_unit.destroyed : (s->armed[idx] && s->units[idx].destroyed);
        return s->bld[idx].hp <= 0;
    }
    if (kind & (MTBL_K_SCAN | MTBL_K_ALLMAIN)) {
        if (type == 1) return s->navs[idx].visited;
        if (type == 2) return idx >= 0 && s->inspected && s->inspected[idx];
        return s->bld[idx].inspected;
    }
    return tgt_within(s, table, type, idx);
}
static int cb_targets(void *u, int table, int node, uint32_t kind, const char *t, int *n_ok)
{
    msim *s = u;
    int i, n = 0, ok = 0;
    (void)node;
    *n_ok = 0;
    if (!t) return 0;
    if (is_player_rec(s, t) && (class_widen(s->player_class) & kind)) { n++; ok += tgt_test(s, table, kind, 2, -1); }
    for (i = 0; i < s->actor_count && n < 40; i++)
        if (strcasecmp(s->actors[i].group, t) == 0 && (class_widen(s->actors[i].obj_class) & kind)) { n++; ok += tgt_test(s, table, kind, 2, i); }
    for (i = 0; i < s->nav_count && n < 40; i++)
        if (strcasecmp(s->navs[i].name, t) == 0 && (class_widen(s->navs[i].cls) & kind)) { n++; ok += tgt_test(s, table, kind, 1, i); }
    for (i = 0; s->world && i < s->bld_count && n < 40; i++)
        if (strcasecmp(s->world->parts[s->bld[i].intact].rec, t) == 0 && (class_widen((unsigned)s->bld[i].type) & kind)) { n++; ok += tgt_test(s, table, kind, 4, i); }
    *n_ok = ok;
    return n;
}

/* the nodes' radio messages (the training instructor, the campaign's star-mates and command): the start message when a
 * node becomes active, the finish message when it succeeds - SNDS records by name, on the voice channel */
/* the node radio lines are its SUCCESS and FAILURE messages (the "S" and "F" names): S when the node succeeds, F
 * when it fails - nothing when a node is merely armed. So the start node's line (yell00bS, the opener) plays as the
 * mission starts, a training step's instruction (trn1_02S) once the previous step completes, a LEAVE warning
 * (genetr2S) when the player strays, and an objective's report when it is actually achieved */
static void cb_started(void *u, int t, int i) { (void)u; (void)t; (void)i; }
static void cb_finished(void *u, int t, int i, int ok)
{
    msim *s = u;
    char line[160];
    const mtbl_node *n = &s->logic.tables[t].nodes[i];
    const char *say = ok ? n->sound_start : n->sound_finish;
    /* engine 0x10009d60: only the player's star (table 0), once per node index (announce flags 0x10208040). (Its
     * "%s successful" / "%s failed" text goes to 0x100086d0, which appends it to a log file - not the screen.)
     * Nothing is queued once the end sequence has flushed the radio (0x10031640 sets DAT_1024f6c4) */
    if (t == 0 && !s->logic.announced[i] && !s->logic.silent && !(s->end_flush_at && s->now >= s->end_flush_at)) {
        if (say[0] && strcmp(say, "NULL") != 0 && s->on_radio) { if (mw2_development_env("MW2_RADIO_TRACE")) fprintf(stderr, "radio node %d at %d ms\n", i, (int)s->now); s->on_radio(s->shot_user, say); }   /* TEST ONLY trace */
        s->logic.announced[i] = 1;
    }
    if (!s->log) return;
    snprintf(line, sizeof line, "%6.1fs table %d node %d %s %s%s%s%s", (double)s->now / 1000.0, t, i, mtbl_kind_name(n->kind),
             ok ? "ok" : "FAILED", n->target[0] ? " target " : "", n->target, n->text[0] ? "" : "");
    s->log(s->log_user, line);
    if (n->text[0]) {
        snprintf(line, sizeof line, "        objective \"%s\" %s", n->text, ok ? "complete" : "failed");
        s->log(s->log_user, line);
    }
}

static int titanium_16bit_requested;

void msim_use_titanium_16bit(int enabled)
{
    titanium_16bit_requested = enabled;
}

void msim_set_dos(int dos)
{
    combat_dos = msim_dos_int = ai_dos_int = combat_jet_refill_dos = mech3d_dos_hips = dos != 0;
    combat_ct_split = dos ? 2 : 3;
    combat_jet_fuel_max = dos ? 1820.0f : 1810.0f;
}

/* The DOS BTHG handler (0x4f326) reserves pool slots in record order. */
static void dos_shot_pool_load(msim *s, prj_archive *a)
{
    prj_record r;
    if (prj_read_named(a, "BWD", "MW2_SHT1", &r) != PRJ_OK) return;
    int n = bwd_chunks(r.data, r.size, NULL, 0);
    bwd_chunk *chunks = n > 0 ? calloc((size_t)n, sizeof *chunks) : NULL;
    if (chunks) {
        bwd_chunks(r.data, r.size, chunks, n);
        for (int i = 0; i < n && s->dos_shot_slots < MSIM_DOS_MAX_SHOTS; i++) {
            const bwd_chunk *c = &chunks[i];
            if (strcmp(c->tag, "BTHG") == 0 && c->size >= 4) {
                int kind = c->data[2] | c->data[3] << 8;
                s->dos_shot_kinds[s->dos_shot_slots++] = kind <= 254 ? (uint8_t)kind : 255;
            }
        }
        free(chunks);
    }
    prj_record_free(&r);
}

static int actor_has_animation(const world_actor *actor)
{
    /* DOS vehicle sets such as CHPRANIM have one track but carry drive flags. */
    return msim_dos_int ? actor->anim.key_count > 0 : actor->have_anim;
}

static void dos_init_actor_anim(msim *s, int actor)
{
    if (s->dos_anim_order) s->dos_anim_order[s->dos_anim_count++] = actor;
    world_actor *ac = &s->actors[actor];
    anim_player *player = s->anim_pl ? &s->anim_pl[actor] : NULL;
    mech3d_dos_anim_init(&ac->mech, player && actor_has_animation(ac) ? &ac->anim : NULL, player, &s->dos_anim_phase);
}

struct msim_spin_seed {
    char record[11];
    int node;
    unsigned period;
    int32_t yaw;
};

static void dos_init_spin(msim *s, const bwd_record *record, const bwd_obj *obj)
{
    for (int i = 0; i < record->chunk_count; i++) {
        bwd_tsk task;
        if (bwd_tsk_decode(&record->chunks[i], &task) != 0 || (task.type != 0 && task.type != 2) || task.node != obj->index) continue;
        struct msim_spin_seed *seeds = realloc(s->dos_spin_seeds, (size_t)(s->dos_spin_count + 1) * sizeof *seeds);
        if (!seeds) return;
        s->dos_spin_seeds = seeds;
        struct msim_spin_seed *seed = &seeds[s->dos_spin_count++];
        snprintf(seed->record, sizeof seed->record, "%.10s", record->name);
        seed->node = obj->index;
        seed->period = s->dos_anim_phase;
        seed->yaw = obj->rotation[1];
        return;
    }
}

static void dos_init_chunk(msim *s, const bwd_record *record, int obj_index, const bwd_obj *obj)
{
    /* MW2_SHT1 pairs each pooled chunk's OBJ with an XPLO declaration of type 11. */
    for (int k = obj_index + 1; k < record->chunk_count; k++) {
        const bwd_chunk *c = &record->chunks[k];
        if (!strcmp(c->tag, "OBJ")) break;
        if (!strcmp(c->tag, "XPLO") && c->size >= 6 && c->data[4] == 11 && c->data[5] == 0 &&
            s->dos_chunk_count < MSIM_DOS_CHUNK_COUNT) {
            int slot = s->dos_chunk_count++;
            const dos_matrix rotation = dos_matrix_rotation(obj->rotation[0], obj->rotation[1], obj->rotation[2]);
            s->dos_chunk_left[slot] = s->dos_anim_phase;
            s->dos_chunk_kind[slot] = obj->model;
            memcpy(s->dos_chunk_rot[slot], rotation.r, sizeof rotation.r);
            break;
        }
    }
}

/* INCL recurses in place in DOS. GPS copies a unit skeleton at that point,
 * interleaved with allocations for scenery and the projectile/effect pool. */
static void dos_init_anim_record(msim *s, const bwd_mission *mission, int index,
                                 unsigned char *seen, unsigned char *spawned, int *player_seen)
{
    if (seen[index]) return;
    seen[index] = 1;
    const bwd_record *record = &mission->records[index];
    int pool = strcasecmp(record->name, "MW2_SHT1") == 0;
    int has_spin = 0;
    for (int k = 0; k < record->chunk_count && !has_spin; k++) {
        bwd_tsk task;
        has_spin = bwd_tsk_decode(&record->chunks[k], &task) == 0 && (task.type == 0 || task.type == 2);
    }
    for (int k = 0; k < record->chunk_count; k++) {
        const bwd_chunk *chunk = &record->chunks[k];
        uint16_t id;
        char name[11];
        bwd_obj obj;
        bwd_gps gps;
        if (bwd_incl(chunk, &id, name) == 0) {
            for (int i = 0; i < mission->record_count; i++) {
                if (strcasecmp(mission->records[i].name, name) != 0) continue;
                dos_init_anim_record(s, mission, i, seen, spawned, player_seen);
                break;
            }
        } else if (bwd_obj_decode(chunk, &obj) == 0 && (pool || obj.parent != -2)) {
            if (has_spin) dos_init_spin(s, record, &obj);
            if (pool) dos_init_chunk(s, record, k, &obj);
            s->dos_anim_phase = 64 + ((s->dos_anim_phase + 7) & 127);
        } else if (bwd_gps_decode(chunk, &gps) == 0) {
            if (gps.is_player) {
                if (*player_seen || !s->pl_mech_ok) continue;
                *player_seen = 1;
                s->dos_player_anim_phase = s->dos_anim_phase;
                mech3d_dos_anim_init(&s->pl_mech, NULL, NULL, &s->dos_anim_phase);
                continue;
            }
            for (int i = 0; i < s->actor_count; i++) {
                const world_actor *ac = &s->actors[i];
                if (spawned[i] || strcasecmp(ac->group, record->name) || strcasecmp(ac->name, gps.name)) continue;
                spawned[i] = 1;
                dos_init_actor_anim(s, i);
                break;
            }
        }
    }
}

static void dos_init_anim_order(msim *s, const bwd_mission *mission)
{
    size_t records = (size_t)mission->record_count, actors = (size_t)s->actor_count;
    unsigned char *seen = calloc(records + actors + 1, 1);
    unsigned char *spawned = seen ? seen + records : NULL;
    int player_seen = 0;
    s->dos_anim_order = calloc(actors, sizeof *s->dos_anim_order);
    if (seen && records) dos_init_anim_record(s, mission, 0, seen, spawned, &player_seen);
    if (!player_seen && s->pl_mech_ok) {
        s->dos_player_anim_phase = s->dos_anim_phase;
        mech3d_dos_anim_init(&s->pl_mech, NULL, NULL, &s->dos_anim_phase);
    }
    for (int i = 0; i < s->actor_count; i++)
        if (!spawned || !spawned[i]) dos_init_actor_anim(s, i);
    free(seen);

    int type = prj_find_type(s->arch, "POLY");
    int models[2] = {prj_find_id(s->arch, type, "CHUNKER1"), prj_find_id(s->arch, type, "CHUNKER2")};
    for (int slot = 0; slot < s->dos_chunk_count; slot++) s->dos_chunk_kind[slot] = s->dos_chunk_kind[slot] == models[1];
    for (int kind = 0; kind < 2; kind++) {
        prj_record model;
        if (prj_read(s->arch, type, models[kind], &model) != PRJ_OK) continue;
        wtb_model mesh;
        if (wtb_parse(model.data, model.size, &mesh) == 0) {
            int32_t sphere[4];
            if (mesh.object_count > 0) {
                mech3d_dos_model_sphere(&mesh.objects[0], sphere);
                s->dos_chunk_radius[kind] = sphere[3] * 2;   /* DOS pool OBJ scale */
            }
            wtb_free(&mesh);
        }
        prj_record_free(&model);
    }
}

int msim_init(msim *s, prj_archive *a, const char *scene, world_actor *actors, int count)
{
    /* MWK identifies the Titanium archives. The 16-bit archive has
     * extended explosion frames, including A9EXBR32. Rendition can also
     * select the 16-bit engine when the mission uses an older archive. */
    combat_titanium = TITANIUM_COMBAT_NONE;
    if (!msim_dos_int) {
        if (titanium_16bit_requested) combat_titanium = TITANIUM_COMBAT_MERCSW;
        else if (prj_find_type(a, "MWK") >= 0) {
            const int cel = prj_find_type(a, "CEL");
            combat_titanium = cel >= 0 && prj_find_id(a, cel, "A9EXBR32") >= 0
                              ? TITANIUM_COMBAT_MERCSW : TITANIUM_COMBAT_BATTLE;
        }
    }
    int player_table;
    bwd_mission m;
    prj_record rec;
    int i, k;
    const char *pl_rec;
    memset(s, 0, sizeof *s);
    s->dos_anim_phase = 100;
    s->dos_end = s->sd_at_once = msim_dos_int;
    snprintf(s->scene, sizeof s->scene, "%s", scene);
    s->actors = actors;
    s->actor_count = count;
    s->rng = 12345u;
    if (msim_dos_int) dos_random_init(s);
    if (msim_dos_int) dos_shot_pool_load(s, a);
    {   /* the planet (PLNT chunk) before any unit is built */
        bwd_mission bm;
        combat_planet pl = {1.0f, 0, 100000.0f, 0};
        s->fx_light_ok = 1;
        int r, c;
        if (bwd_mission_load(a, scene, &bm) == 0) {
            for (r = 0; r < bm.record_count; r++)
                for (c = 0; c < bm.records[r].chunk_count; c++) {
                    const bwd_chunk *ch = &bm.records[r].chunks[c];
                    if (strcmp(ch->tag, "PLNT") != 0 || ch->size < 56) continue;
                    pl.gravity_g = (float)(int32_t)(ch->data[0] | (ch->data[1] << 8) | (ch->data[2] << 16) | ((uint32_t)ch->data[3] << 24)) / 65536.0f;
                    pl.temperature = (int32_t)(ch->data[28] | (ch->data[29] << 8) | (ch->data[30] << 16) | ((uint32_t)ch->data[31] << 24));
                    pl.jet_damping = (float)(int32_t)(ch->data[32] | (ch->data[33] << 8) | (ch->data[34] << 16) | ((uint32_t)ch->data[35] << 24));
                    pl.hostile = (ch->data[48] | ch->data[49] | ch->data[50] | ch->data[51]) == 0;
                    s->fx_light_ok = ch->size < 60 || (ch->data[56] | ch->data[57] | ch->data[58] | ch->data[59]) == 0;
                }
            bwd_mission_free(&bm);
        }
        {   /* the mission configuration's overrides (0x1003de10 PLNT handler): DAT_1024ac6c is MW2DIF.CFG read into 0x17
             * zeroed bytes (0x10008ae3 -> 0x100408a0; MW2NET.CFG in a network game): +0xf (16.16) replaces the gravity, +0x13
             * the temperature, when not 0. The shell writes 8 bytes (Combat Variables, FUN_000258e0: fwrite 8 x 1), so a
             * single-player game never has them - only a longer file would */
            unsigned char *dif = NULL;
            size_t dlen = 0;
            dp_add_roots_from_env();
            if (dp_read_file("MW2DIF.CFG", &dif, &dlen) == 0) {
                if (dlen >= 0x17) {
                    int32_t gv = (int32_t)(dif[0xf] | dif[0x10] << 8 | dif[0x11] << 16 | (uint32_t)dif[0x12] << 24);
                    int32_t tv = (int32_t)(dif[0x13] | dif[0x14] << 8 | dif[0x15] << 16 | (uint32_t)dif[0x16] << 24);
                    if (gv) pl.gravity_g = (float)gv / 65536.0f;
                    if (tv) pl.temperature = tv;
                }
                free(dif);
            }
        }
        if (pl.gravity_g <= 0) pl.gravity_g = 1.0f;
        if (mw2_development_env("MW2_DEBUG_PLANET")) fprintf(stderr, "planet %s: %.4f g, temp %d, jet damping %.0f\n", scene, pl.gravity_g, (int)pl.temperature, pl.jet_damping);
        combat_set_planet(&pl);
        s->planet = pl;
        /* top speed falls with the planet's gravity: DOSBox, the Timber Wolf at full throttle reads 75 kph on JACK (1.1 g)
         * and 90 on YELL (0.9 g) - YELL timed by its nav range: 34.6 m per 2.135 s = 58.3 km/h; both fit walk MP x
         * 300 cm/s / g (54 / 1.1 = 49, 54 / 0.9 = 60). Engine 0x100190d0: controller +0x88 /= g x 0.0296 x 33.784; DOS
         * in integers (msim_top_speed) */
        for (i = 0; i < count; i++)
            if (actors[i].speed > 0) actors[i].speed = msim_top_speed((int)lrintf(actors[i].speed / 300.0f), pl.gravity_g);
    }
    s->units = calloc((size_t)(count > 0 ? count : 1), sizeof *s->units);
    /* the start-up (engine 0x1001a180 state 0, 0x1001ab71): every mech, the player's too, comes online after
     * 0x43e + rand(0x16a) ticks = 5.97-7.95 s; until then no throttle, turning, jets or weapons */
    s->online_at = calloc((size_t)(count > 0 ? count : 1), sizeof *s->online_at);
    s->unit_flank = calloc((size_t)(count > 0 ? count : 1), sizeof *s->unit_flank);
    s->ai_focus = malloc((size_t)(count > 0 ? count : 1) * sizeof *s->ai_focus);
    s->ai_who = malloc((size_t)(count > 0 ? count : 1) * sizeof *s->ai_who);
    for (i = 0; s->ai_focus && s->ai_who && i < count; i++) s->ai_focus[i] = s->ai_who[i] = -2;
    s->powered_down = calloc((size_t)(count > 0 ? count : 1), sizeof *s->powered_down);
    s->sd_at = calloc((size_t)(count > 0 ? count : 1), sizeof *s->sd_at);
    s->seen_node = malloc((size_t)(count > 0 ? count : 1) * sizeof *s->seen_node);
    for (i = 0; s->seen_node && i < count; i++) s->seen_node[i] = -2;
    for (i = 0; s->online_at && i < count; i++)
        s->online_at[i] = msim_dos_int ? -1.0f : (float)startup_ticks(s) * 1000.0f / 182.0f;
    if (msim_dos_int) s->dos_boot_ticks = DOS_FIRST_CONTROL_TICK;
    /* DOS chooses the delay in the first controller pass. Until then the
     * base delay keeps the player's controls in the startup state. */
    s->player_online_at = startup_deadline(s, msim_dos_int ? DOS_STARTUP_BASE_TICKS : startup_ticks(s), 1);
    combat_damage_live = mw2_development_env("MW2_NO_STARTUP") != NULL;   /* DAT_1024c570 = 0 at the mission start (0x1002388d) */
    s->armed = calloc((size_t)(count > 0 ? count : 1), sizeof *s->armed);
    if (!s->units || !s->armed) return -1;
    for (i = 0; i < count; i++) {
        char up[16];
        size_t q;
        for (q = 0; actors[i].loadout[q] && q < sizeof up - 1; q++) up[q] = (char)toupper((unsigned char)actors[i].loadout[q]);
        up[q] = '\0';
        s->armed[i] = combat_init(&s->units[i], a, up) == 0;
        if (msim_dos_int) {
            s->units[i].dos_roll = ms_roll;
            s->units[i].dos_roll_context = s;
        }
        s->units[i].hold_fire_above = 65.0f;     /* AI heat discipline: assumed */
        /* one position for drawing, collision and simulation (engine object +0x50..+0x58): the unit starts at its spawn
         * point's height (NAVP y) - a turret on a cliff top fires from there, a unit above the ground falls to it */
        s->units[i].y = s->units[i].ground = (float)actors[i].mech.origin[1];
        mech3d_hit_weights(&actors[i].mech, s->units[i].hit_w);
    }
    /* the player's mech: the shell writes it to USERSTAR.BWD (first GPS) before launching */
    snprintf(s->player_skel, sizeof s->player_skel, "TIMBRWLF");
    snprintf(s->player_loadout, sizeof s->player_loadout, "TBR00STD");
    s->player_class = 6;   /* the usual GPS class (destroy / protect) when there is no USERSTAR */
    snprintf(s->player_rec, sizeof s->player_rec, "UserStar");
    player_table = 0;
    {
        unsigned char *d = NULL;
        size_t len = 0, o = 12;
        static int roots;
        if (!roots) { dp_add_roots_from_env(); roots = 1; }
        int from_tree = 0;
        {   /* the player is the mission tree's GPS with +0x0e == 0 (engine 0x1003f36b sets 0x10259c74 for it, whichever
             * record holds it): the shell's USERSTAR in the campaign / Instant Action (an external part of the tree),
             * the mission's own TNx#USS1 in Cadet Training - the training launch writes no star file (shell
             * FUN_000375a0 skips FUN_0003abb0 -> FUN_0001d540 for screen 14), so a USERSTAR.BWD on disk is stale there */
            bwd_mission pm;
            int r, c, k;
            if (bwd_mission_load(a, scene, &pm) == 0) {
                for (r = 0; r < pm.record_count && !from_tree; r++)
                    for (c = 0; c < pm.records[r].chunk_count && !from_tree; c++) {
                        const bwd_chunk *pc = &pm.records[r].chunks[c];
                        bwd_gps g;
                        if (bwd_gps_decode(pc, &g) != 0 || !g.is_player || !g.skeleton[0] || !g.loadout[0]) continue;
                        if (pc->size >= 26) s->player_class = (unsigned)(pc->data[24] | pc->data[25] << 8);
                        player_table = g.table >= 0 && g.table < 0x10 ? g.table : 0;   /* GPS +4 -> unit +8 (0x1003f34b) */
                        snprintf(s->player_skel, sizeof s->player_skel, "%s", g.skeleton);
                        snprintf(s->player_loadout, sizeof s->player_loadout, "%s", g.loadout);
                        for (k = 0; s->player_skel[k]; k++) s->player_skel[k] = (char)toupper((unsigned char)s->player_skel[k]);
                        for (k = 0; s->player_loadout[k]; k++) s->player_loadout[k] = (char)toupper((unsigned char)s->player_loadout[k]);
                        snprintf(s->player_rec, sizeof s->player_rec, "%s", pm.records[r].name);
                        from_tree = 1;
                    }
                bwd_mission_free(&pm);
            }
        }
        if (!from_tree && dp_read_file("USERSTAR.BWD", &d, &len) == 0) {
            while (o + 8 <= len) {
                uint32_t sz = (uint32_t)(d[o + 4] | (d[o + 5] << 8) | (d[o + 6] << 16) | ((uint32_t)d[o + 7] << 24));
                if (sz < 8 || o + sz > len) break;
                if (memcmp(d + o, "GPS", 3) == 0) {
                    bwd_chunk c;
                    bwd_gps g;
                    memset(&c, 0, sizeof c);
                    memcpy(c.tag, "GPS", 4);
                    c.data = d + o + 8;
                    c.size = sz - 8;
                    if (c.size >= 26) s->player_class = (unsigned)(c.data[24] | c.data[25] << 8);   /* GPS +0x18 (0x1003f38b) */
                    if (bwd_gps_decode(&c, &g) == 0 && g.skeleton[0] && g.loadout[0]) {
                        int k;
                        player_table = g.table >= 0 && g.table < 0x10 ? g.table : 0;   /* GPS +4 -> unit +8 (0x1003f34b) */
                        snprintf(s->player_skel, sizeof s->player_skel, "%s", g.skeleton);
                        snprintf(s->player_loadout, sizeof s->player_loadout, "%s", g.loadout);
                        for (k = 0; s->player_skel[k]; k++) s->player_skel[k] = (char)toupper((unsigned char)s->player_skel[k]);
                        for (k = 0; s->player_loadout[k]; k++) s->player_loadout[k] = (char)toupper((unsigned char)s->player_loadout[k]);
                    }
                    break;
                }
                o += sz;
            }
            free(d);
        }
    }
    if (mw2_development_env("MW2_TEST_PLAYER_MECH")) {   /* TEST ONLY: "SKELETON,LOADOUT" for the player (e.g. MADDOG,MDG00STD) */
        char sk[32] = "", lo[32] = "";
        if (sscanf(mw2_development_env("MW2_TEST_PLAYER_MECH"), "%31[^,],%31s", sk, lo) == 2) {
            snprintf(s->player_skel, sizeof s->player_skel, "%s", sk);
            snprintf(s->player_loadout, sizeof s->player_loadout, "%s", lo);
        }
    }
    if (mw2_development_env("MW2_DEBUG_LOADOUT")) fprintf(stderr, "player loadout %s\n", s->player_loadout);   /* tests */
    if (combat_init(&s->player_unit, a, s->player_loadout) != 0) {
        if (mw2_development_env("MW2_DEBUG_LOADOUT")) fprintf(stderr, "loadout %s failed: TBR00STD\n", s->player_loadout);
        snprintf(s->player_skel, sizeof s->player_skel, "TIMBRWLF");
        snprintf(s->player_loadout, sizeof s->player_loadout, "TBR00STD");
        combat_init(&s->player_unit, a, s->player_loadout);
    }
    if (msim_dos_int) {
        s->player_unit.dos_roll = ms_roll;
        s->player_unit.dos_roll_context = s;
    }
    {   /* Combat Variables: MW2DIF.CFG (the install's, via the datapath roots): +0 unlimited ammo, +1 invulnerability,
         * +3 collision damage, +4 heat tracking, +5 difficulty (defaults: a fresh install's 0 0 1 1 1 1) */
        unsigned char *dif = NULL;
        size_t dlen = 0;
        unsigned char v[8] = {0, 0, 1, 1, 1, 1, 0, 0};
        if (dp_read_file("MW2DIF.CFG", &dif, &dlen) == 0) { memcpy(v, dif, dlen < 8 ? dlen : 8); free(dif); }
        s->player_unit.unlimited_ammo = v[0] != 0;
        s->player_unit.invulnerable = v[1] != 0;
        s->collision_damage = v[3] != 0;
        s->splash = v[2] != 0;   /* SPLASH DAMAGE ("Splash Damage ON / OFF", MW2.EXE 0x46e90 strings), default on */
        for (i = 0; i < count; i++) s->units[i].no_collision_damage = v[3] == 0;   /* a global switch (DAT_1024ac6c + 3): falls too */
        s->player_unit.no_heat = v[4] == 0;
        s->player_unit.no_collision_damage = v[3] == 0;
        combat_set_player_difficulty(&s->player_unit, v[5]);
        s->difficulty = v[5];
        /* the loader's armour factor by difficulty (engine 0x10041410, DOS 0x4c760): the player x 4, the player's side
         * (alliance 0) x 3 / 3 / 4, others x 1 / 3 / 4 - armour and rear armour only */
        combat_set_armor_mult(&s->player_unit, combat_armor_mult(1, 0, v[5]));
        s->player_unit.dos_shade_divisor = 4;
        for (i = 0; i < count; i++)
            if (s->armed[i]) {
                combat_set_armor_mult(&s->units[i], combat_armor_mult(0, actors[i].friendly ? 0 : actors[i].alliance, v[5]));
                s->units[i].dos_shade_divisor = (int)combat_armor_mult(0, 1, v[5]);
            }
        {   /* MW2SND.CFG +0x24: chunky explosions (default on) */
            unsigned char *snd = NULL;
            size_t sl = 0;
            s->chunky = 1;
            if (dp_read_file("MW2SND.CFG", &snd, &sl) == 0) { if (sl >= 0x28) s->chunky = (snd[0x24] | snd[0x25] << 8 | snd[0x26] << 16 | snd[0x27] << 24) != 0; free(snd); }
        }
        if (mw2_development_env("MW2_DEBUG_LOADOUT")) fprintf(stderr, "MW2DIF: unlimited ammo %d, invulnerable %d, collision damage %d, heat tracking %d, difficulty %d\n", v[0], v[1], v[3], v[4], v[5]);
    }
    s->player_unit.hold_fire_above = 65.0f;      /* headless test player; glview sets 0 (you decide) */
    pl_rec = s->player_skel;
    if (mech3d_load(a, s->player_skel, 0, &s->pl_mech) == 0 || (pl_rec = "TIMBRWLF", mech3d_load(a, pl_rec, 0, &s->pl_mech) == 0)) {
        s->pl_mech_ok = 1;
        mech3d_hit_weights(&s->pl_mech, s->player_unit.hit_w);
        /* in the cockpit view the player's object is switched to level of detail 4 (0x100267d0: camera mode 0,
         * 0x1002df50) and the swap (0x10026330 -> 0x10026430) takes the old pieces out of the collision list and
         * puts the new representation's in (0x10029a00, 0x1000db80): the shots meet the cockpit set - head, arms,
         * upper and lower legs (TIMBRWLF: TW5_HEAD 3.5 m wide against TW1_HEAD's 7.3, no hips) */
        s->pl_pit_ok = mech3d_load(a, pl_rec, 4, &s->pl_pit) == 0 && s->pl_pit.part_count > 0;
        if (s->pl_pit_ok) mech3d_use_skeleton(&s->pl_pit, &s->pl_mech);
    }
    s->player_cockpit = 1;
    {   /* each effect type's card radius (its geometry's +0x40: the farthest vertex, 0x100291fd) for the area damage */
        fx_defs *fd = calloc(1, sizeof *fd);
        if (fd && fx_load(a, fd) == 0)
            for (i = 0; i < 32; i++) s->fx_radius[i] = sqrtf(fd->half_w[i] * fd->half_w[i] + fd->half_h[i] * fd->half_h[i]);
        free(fd);
    }
    s->ai_ok = ai_load(&s->ai, a) == 0;
    s->minds = calloc((size_t)(count ? count : 1), sizeof *s->minds);
    s->arch = a;
    s->col_repr = calloc((size_t)(count ? count : 1), 1);
    if (msim_dos_int) {
        s->dos_hulls = calloc((size_t)count + 1, sizeof *s->dos_hulls);
        if (s->dos_hulls)
            for (int q = 0; q <= count; q++) s->dos_hulls[q].unit = q - 1;
    }
    s->col_lod = calloc((size_t)count + 1, sizeof *s->col_lod);
    s->col_lod_ok = calloc((size_t)count + 1, sizeof *s->col_lod_ok);
    s->no_col_lod = mw2_development_env("MW2_NO_COL_LOD") != NULL || mw2_development_env("MW2_NO_LOD") != NULL;   /* TEST ONLY */
    s->cmd = calloc((size_t)(count ? count : 1), sizeof *s->cmd);
    s->cmd_target = calloc((size_t)(count ? count : 1), sizeof *s->cmd_target);
    s->engage = calloc((size_t)(count ? count : 1), sizeof *s->engage);
    s->player_victim = -1;
    s->mate_target = calloc((size_t)(count ? count : 1), sizeof *s->mate_target);
    s->defend_leg = calloc((size_t)(count ? count : 1), sizeof *s->defend_leg);
    s->ai_lock = calloc((size_t)(count ? count : 1), sizeof *s->ai_lock);
    s->provoked = calloc((size_t)(count ? count : 1), sizeof *s->provoked);
    s->anim_pl = calloc((size_t)(count ? count : 1), sizeof *s->anim_pl);
    for (i = 0; s->anim_pl && i < count; i++)
        anim_player_init(&s->anim_pl[i], actors[i].mech.anim_rate > 0 ? actors[i].mech.anim_rate : 70);
    s->bump_with = calloc((size_t)(count ? count : 1), sizeof *s->bump_with);
    s->aim = calloc((size_t)(count ? count : 1), sizeof *s->aim);
    memset(&s->player_aim, 0, sizeof s->player_aim);
    s->reticle_actor = s->reticle_bld = -1;
    s->contact_frac = calloc((size_t)(count ? count : 1), sizeof *s->contact_frac);
    s->contact_acc = calloc((size_t)(count ? count : 1), sizeof *s->contact_acc);
    s->across = calloc((size_t)(count ? count : 1), sizeof *s->across);
    s->contact_hold = calloc((size_t)(count ? count : 1), sizeof *s->contact_hold);
    s->player_sel_w = 0;
    s->player_lock_target = -1;
    s->dos_warning_target = -1;
    s->inspected = calloc((size_t)(count ? count : 1), 1);
    s->player_nav = -1;
    s->star_formation = -1;
    for (i = 0; i < 8; i++) s->last_cmd[i] = 7;                       /* "No Cmd" */
    for (i = 0; i < count && s->engage && s->cmd_target; i++) {
        s->cmd_target[i] = -1;
        if (s->mate_target) s->mate_target[i] = -1;
        s->engage[i] = !(actors[i].friendly && actors[i].table == 0);   /* 0x100124a0: the player's group 0 */
    }
    const char *ai_seed = mw2_development_env("MW2_AI_SEED");   /* TEST ONLY: sample the AI's random choices */
    for (i = 0; s->minds && i < count; i++) {
        ai_init(&s->minds[i], actors[i].leader ? AIP_LDEFEND : AIP_FDEFEND);
        if (ai_seed) s->minds[i].rng ^= (unsigned)strtoul(ai_seed, NULL, 0) * 2654435761u;
        else if (msim_dos_int) {
            s->minds[i].random_roll = ms_roll;
            s->minds[i].random_context = s;
        }
    }
    s->air_speed = calloc((size_t)(count ? count : 1), sizeof *s->air_speed);
    s->avoid_turn = calloc((size_t)(count ? count : 1), sizeof *s->avoid_turn);
    s->avoid_thr = calloc((size_t)(count ? count : 1), sizeof *s->avoid_thr);
    s->avoid_next = calloc((size_t)(count ? count : 1), sizeof *s->avoid_next);
    s->avoid_side = calloc((size_t)(count ? count : 1), sizeof *s->avoid_side);
    s->avoid_len = calloc((size_t)(count ? count : 1), sizeof *s->avoid_len);
    s->height = calloc((size_t)(count ? count : 1), sizeof *s->height);
    s->wreck_until = calloc((size_t)count + 1, sizeof *s->wreck_until);
    s->msg_seen = calloc((size_t)(count ? count : 1), sizeof *s->msg_seen);
    s->twist_limit = calloc((size_t)(count ? count : 1), sizeof *s->twist_limit);
    s->radius = calloc((size_t)(count > 0 ? count : 1), sizeof *s->radius);
    s->fly_seen = calloc((size_t)count + 1, sizeof *s->fly_seen);
    s->centre_h = calloc((size_t)(count > 0 ? count : 1), sizeof *s->centre_h);
    s->draw_lift = calloc((size_t)(count > 0 ? count : 1), sizeof *s->draw_lift);
    for (i = 0; s->twist_limit && s->radius && s->centre_h && i < count; i++) {
        prj_record mg;
        s->twist_limit[i] = 90.0f;
        s->radius[i] = 450.0f;
        s->centre_h[i] = 550.0f;
        if (prj_read_named(a, "MGEO", actors[i].skel, &mg) == PRJ_OK) {
            if (mg.size >= 24)
                s->twist_limit[i] = (float)(int32_t)(mg.data[20] | (mg.data[21] << 8) | (mg.data[22] << 16) | ((uint32_t)mg.data[23] << 24)) / 65536.0f;
            if (mg.size >= 28) {   /* int[6]: Timber Wolf 545, Kit Fox 450, Mad Dog 500, Nova 600, Elemental 170 */
                int32_t r = (int32_t)(mg.data[24] | (mg.data[25] << 8) | (mg.data[26] << 16) | ((uint32_t)mg.data[27] << 24));
                int32_t hc = (int32_t)(mg.data[0] | (mg.data[1] << 8) | (mg.data[2] << 16) | ((uint32_t)mg.data[3] << 24));
                if (r > 0 && r < 5000) s->radius[i] = (float)r;
                const uint8_t *p = mg.data + 8;
                int32_t below = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
                int air = actors[i].unit_class == 5 || actors[i].unit_class == 8;
                /* The high-altitude fighter variants use class 2 and store
                 * their entire flight offset in MGEO (+0, with -offset at +8).
                 * AIRCRF1D: 3950 m. SHILONEA and CHIPPEWB: 2130 m. */
                int high_fighter = actors[i].unit_class == 2 && hc > 20000 && below == -hc;
                /* aircraft and dropships: int[0] is their flying height (AIRCRFT* 30-100 m, DROPA 75 m) */
                /* 0 counts (doors and turrets: DOORA / B, TURRET*, the centre at their base) - it had fallen back to 5.5 m,
                 * and the target viewer, framed on the centre (DOS: the object's position), showed Scorching Sand's
                 * turrets at the box's lower edge (user report) */
                if (high_fighter || (hc >= 0 && (hc < 5000 || (air && hc <= 20000)))) s->centre_h[i] = (float)hc;
                if ((air || high_fighter) && s->draw_lift) s->draw_lift[i] = s->centre_h[i];
            }
            prj_record_free(&mg);
        }
    }
    {
        prj_record mg;
        s->player_radius = 545.0f; s->player_centre_h = 550.0f;
        if (prj_read_named(a, "MGEO", s->player_skel, &mg) == PRJ_OK) {
            if (mg.size >= 28) {
                int32_t r = (int32_t)(mg.data[24] | (mg.data[25] << 8) | (mg.data[26] << 16) | ((uint32_t)mg.data[27] << 24));
                int32_t hc = (int32_t)(mg.data[0] | (mg.data[1] << 8) | (mg.data[2] << 16) | ((uint32_t)mg.data[3] << 24));
                if (r > 0 && r < 5000) s->player_radius = (float)r;
                if (hc > 0 && hc < 5000) s->player_centre_h = (float)hc;
            }
            prj_record_free(&mg);
        }
    }
    for (i = 0; s->height && i < count; i++) {
        int p2, v;
        float top = 0;
        for (p2 = 0; p2 < actors[i].mech.part_count; p2++) {
            const mech3d_part *pp = &actors[i].mech.parts[p2];
            if (pp->model.object_count < 1) continue;
            for (v = 0; v < pp->model.objects[0].vert_count; v++) {
                float y = (float)(pp->model.objects[0].verts[v].y + pp->pos[1] - actors[i].mech.origin[1]);
                if (y > top) top = y;
            }
        }
        s->height[i] = top > 100 ? top : 600;
    }
    for (i = 0; s->draw_lift && i < count; i++)   /* hung from the centre from the start (msim_draw_y) */
        if (s->draw_lift[i] != 0.0f) {
            actors[i].mech.origin[1] = (int32_t)(s->units[i].y + s->draw_lift[i]);
            mech3d_pose(&actors[i].mech, NULL, 0);
        }
    if (prj_read_named(a, "BWD", scene, &rec) != PRJ_OK) return -1;
    mtbl_logic_init(&s->logic, rec.data, rec.size);
    prj_record_free(&rec);
    /* nav points: every NAVP in the mission tree, keyed by its record name */
    if (bwd_mission_load(a, scene, &m) == 0) {
        s->music_track = m.music_track;
        if (msim_dos_int) dos_init_anim_order(s, &m);
        for (i = 0; i < m.record_count; i++)   /* the player's star: STAR (24-byte entries per table, +0 the slot offset) */
            for (k = 0; k < m.records[i].chunk_count; k++) {
                const bwd_chunk *sc = &m.records[i].chunks[k];
                if (strcmp(sc->tag, "STAR") == 0 && (size_t)(player_table + 1) * 24 <= sc->size) {
                    const uint8_t *q = sc->data + player_table * 24;
                    int v = (int)((uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24);
                    s->player_tex = v >= 0 && v < 16 ? v : 0;
                }
            }
        /* The engine only shows navs on the player's table (0x1001cea0). */
        for (i = 0; i < m.record_count && s->nav_count < MSIM_MAX_NAVS; i++)
            load_nav_chunks(s, m.records[i].name, m.records[i].chunks, m.records[i].chunk_count, player_table);
        /* ZACK's return objective names BRAN's drop site in the raw archive.
         * The Airlock is the ZACKGOU1 nav included in this scene. */
        if (strcasecmp(scene, "ZACKSCN1") == 0) {
            mtbl_table *player = &s->logic.tables[0];
            const msim_nav *airlock = find_nav(s, "ZACKGOU1");
            if (airlock && strcasecmp(airlock->label, "Airlock") == 0)
                for (k = 0; k < player->node_count; k++) {
                    mtbl_node *node = &player->nodes[k];
                    if (node->kind == MTBL_K_REACH &&
                        strcasecmp(node->target, "BRANST01") == 0 &&
                        strcasecmp(node->text, airlock->label) == 0)
                        memcpy(node->target, "ZACKGOU1", sizeof "ZACKGOU1");
                }
        }
        /* A few repacked scenes omit INCL for navs still present in their mission data. */
        for (i = 0; i < s->logic.table_count; i++)
            for (k = 0; k < s->logic.tables[i].node_count && s->nav_count < MSIM_MAX_NAVS; k++) {
                const mtbl_node *node = &s->logic.tables[i].nodes[k];
                if ((node->kind != MTBL_K_REACH && node->kind != MTBL_K_LEAVE) ||
                    strncasecmp(node->target, scene, 4) != 0 || find_nav(s, node->target)) continue;
                prj_record nav;
                if (prj_read_named(a, "BWD", node->target, &nav) != PRJ_OK) continue;
                bwd_chunk chunks[4];
                int count = bwd_chunks(nav.data, nav.size, chunks, 4);
                if (count > 0 && count <= 4 && strcasecmp(nav.name, node->target) == 0)
                    load_nav_chunks(s, nav.name, chunks, count, player_table);
                prj_record_free(&nav);
            }
        bwd_mission_free(&m);
    }
    /* the player starts at table 0's start node target (e.g. CYANST01) */
    if (s->logic.table_count > 0) {
        const msim_nav *st = find_nav(s, s->logic.tables[0].nodes[0].target);
        if (st) { s->player[0] = st->x; s->player[2] = st->z; s->player_unit.y = st->y; if (mw2_development_env("MW2_DBG_START")) fprintf(stderr, "START %s y=%.0f ground=%.0f\n", st->name, (double)st->y, (double)msim_ground(s, st->x, st->z, 1e9f)); }
    }
    s->player_target = -1;
    s->player_facing = 0;
    {   /* the start point's facing */
        bwd_mission bm;
        int r, c;
        if (s->logic.table_count > 0 && bwd_mission_load(a, scene, &bm) == 0) {
            for (r = 0; r < bm.record_count; r++)
                if (strcasecmp(bm.records[r].name, s->logic.tables[0].nodes[0].target) == 0)
                    for (c = 0; c < bm.records[r].chunk_count; c++) {
                        bwd_navpoint nv;
                        if (bwd_navp(&bm.records[r].chunks[c], &nv) == 0) { s->player_heading = s->player_facing = nv.heading; break; }
                    }
            bwd_mission_free(&bm);
        }
    }
    return 0;
}


/* Each AI unit's enemy: friendlies (the player's lance) fight the nearest live hostile mech; hostiles
 * fight the nearest of the player and the friendlies. Returns 0 if there is none.
 * *who = actor index, or -1 for the player. */
/* power down (0x10014520 states 10 / 11: object +0x14 |= 0x10, mech +0xa0 |= 3) and wake (0x10013790: a new state entry
 * clears it and the start-up runs again - 0x43e + rand(0x16a) ticks; NONPSTRT 0x104 at the mech once the player is up) */
static void ai_power_down(msim *s, int i) { if (s->powered_down) s->powered_down[i] = 1; }
static void startup_sound(msim *s, int sound, const float pos[3])
{
    if (s->on_sound) {
        s->on_sound(s->shot_user, sound, pos);
    } else if (msim_dos_int) {
        /* With no audio client, use the player's centre as the listener. The
         * original in-range pitch draw happens even if samples cannot play. */
        int pan, delay;
        msim_dos_sound_at(s, (int32_t)lrintf(pos[0] - s->player[0]),
                          (int32_t)lrintf(pos[1] - s->player_unit.y - s->player_centre_h),
                          (int32_t)lrintf(pos[2] - s->player[2]), 0, 0, 1, &pan, &delay);
    }
}

static void dos_player_startup_sound(msim *s)
{
    const float pos[3] = {s->player[0], s->player_unit.y + s->player_centre_h, s->player[2]};
    startup_sound(s, 0xf7, pos);
}

/* a unit's state 0 -> 1 (0x1001ab71): an enemy (0x10019040 == 1) NONPSTRT 0x104 only once the player is up (+0x15 &
 * 0x20), any other MECTURX2 0xf7 - both at the mech */
static void unit_startup(msim *s, int i)
{
    float p3[3] = {(float)s->actors[i].mech.origin[0], s->units[i].y, (float)s->actors[i].mech.origin[2]};
    const world_actor *ac = &s->actors[i];
    int enemy = !ac->friendly && ac->alliance == 1;
    if (!s->armed[i] || s->units[i].destroyed || (msim_dos_int && fixed_unit(ac))) return;
    if (msim_dos_int && s->centre_h) p3[1] += s->centre_h[i];
    if (!enemy) {
        startup_sound(s, 0xf7, p3);
        return;
    }
    if (s->player_unit.destroyed || (s->now < s->player_online_at && !mw2_development_env("MW2_NO_STARTUP"))) return;
    startup_sound(s, 0x104, p3);
    /* The radar contact appears as the sequence begins. Artillery and doors have no voice cue. */
    if (s->on_message && ac->unit_class != 3 && ac->unit_class != 7)
        s->on_message(s->message_user, "Enemy power-up detected", 74);
}
static void ai_wake(msim *s, int i)
{
    if (!s->powered_down || !s->powered_down[i]) return;
    s->powered_down[i] = 0;
    if (msim_dos_int && s->online_at && s->online_at[i] < 0.0f) return;
    if (s->online_at) s->online_at[i] = startup_deadline(s, startup_ticks(s), 0);
    unit_startup(s, i);
}
/* TEST ONLY (glview MW2_TEST_APOWER): power actor i down (up = 0) or wake it (up = 1) as a mission node would */
void msim_test_power(msim *s, int i, int up)
{
    if (i < 0 || i >= s->actor_count) return;
    if (up) ai_wake(s, i); else ai_power_down(s, i);
}
static int task_pos(const msim *s, int code, float p[3], float *range);
static int pick_enemy(const msim *s, int i, float *ex, float *ez, float *ey, float *eface, float *eh, int *who)
{
    const world_actor *ac = &s->actors[i];
    float best = 1e30f, x0 = (float)ac->mech.origin[0], z0 = (float)ac->mech.origin[2];
    int k, found = 0;
    /* DOS 0x22fc1..0x22fd3 excludes candidates on the same side or
     * with alliance 2. A neutral observer can still pick another side.
     * An explicit target or attacker takes priority in both editions. */
    int self_al = ac->friendly ? 0 : ac->alliance, f = s->ai_focus ? s->ai_focus[i] : -2;
    if (ac->friendly && s->cmd && s->cmd[i] == MSIM_CMD_ATTACK) {
        f = s->cmd_target[i];
        if (MSIM_IS_THING(f)) {
            float p[3];
            if (!thing_point(s, MSIM_THING_OF(f), p)) return 0;
            *ex = p[0]; *ez = p[2]; *ey = p[1]; *eface = 0; *eh = 0; *who = f;
            return 1;
        }
        if (f < 0 || f >= s->actor_count || !s->armed[f] || s->units[f].destroyed) return 0;
    }
    if (f == -1 && !s->player_unit.destroyed) {
        *ex = s->player[0]; *ez = s->player[2]; *ey = s->player_unit.y; *eface = s->player_facing; *eh = 0; *who = -1;
        return 1;
    }
    if (f >= 0 && f < s->actor_count && s->armed[f] && !s->units[f].destroyed) {
        *ex = (float)s->actors[f].mech.origin[0]; *ez = (float)s->actors[f].mech.origin[2];
        *ey = actor_aim_base(s, f); *eface = s->actors[f].mech.heading; *eh = s->height ? s->height[f] : 0; *who = f;
        return 1;
    }
    if (s->minds && s->minds[i].tgt != -2) {   /* the target condition 7 / the group assignment gave it (engine +0x14e) */
        int t = s->minds[i].tgt;
        if (t == -1 && !s->player_unit.destroyed) {
            *ex = s->player[0]; *ez = s->player[2]; *ey = s->player_unit.y; *eface = s->player_facing; *eh = 0; *who = -1;
            return 1;
        }
        if (t >= 0 && t < s->actor_count && s->armed[t] && !s->units[t].destroyed) {
            *ex = (float)s->actors[t].mech.origin[0]; *ez = (float)s->actors[t].mech.origin[2];
            *ey = actor_aim_base(s, t); *eface = s->actors[t].mech.heading; *eh = s->height ? s->height[t] : 0; *who = t;
            return 1;
        }
    }
    if (s->minds && s->minds[i].tgt == -2 && (s->minds[i].state == AI_TARGET || s->minds[i].state == AI_ATTACK) &&
        ((s->minds[i].node_tgt >> 16) == 1 || (s->minds[i].node_tgt >> 16) == 4)) {
        /* a node task's nav point or thing as the target +0x14e (0x10014ba0 kinds 1 / 2 / 8): aimed at and fired on like a
         * unit (0x100231a0, 0x100232b0's window by its distance); destroyed -> none (condition 6 / 0x10013c40 flag 4).
         * A nav point uses -3. A structure carries its thing index through aiming, line of fire and lock. */
        float p[3], r;
        const int target = s->minds[i].node_tgt;
        if (!task_pos(s, target, p, &r)) return 0;
        *ex = p[0]; *ez = p[2]; *ey = p[1]; *eface = 0; *eh = 0;
        *who = (target >> 16) == 4 ? MSIM_TGT_THING(target & 0xffff) : -3;
        return 1;
    }
    if (self_al != 0 && !s->player_unit.destroyed && (msim_dos_int || self_al != 2 || s->player_victim == i || (s->provoked && s->provoked[i] > 0))) {
        float dx = s->player[0] - x0, dz = s->player[2] - z0;
        best = dx * dx + dz * dz;
        *ex = s->player[0]; *ez = s->player[2]; *ey = s->player_unit.y; *eface = s->player_facing; *eh = 0; *who = -1;
        found = 1;
    }
    for (k = 0; k < s->actor_count; k++) {
        float dx, dz, d;
        int al = s->actors[k].friendly ? 0 : s->actors[k].alliance;
        if (k == i || !s->armed[k] || s->units[k].destroyed || al == self_al || al == 2 || (!msim_dos_int && self_al == 2)) continue;
        dx = (float)s->actors[k].mech.origin[0] - x0; dz = (float)s->actors[k].mech.origin[2] - z0;
        d = dx * dx + dz * dz;
        if (d < best) {
            best = d; found = 1; *who = k;
            *ex = (float)s->actors[k].mech.origin[0]; *ez = (float)s->actors[k].mech.origin[2];
            *ey = actor_aim_base(s, k); *eface = s->actors[k].mech.heading; *eh = s->height ? s->height[k] : 0;
        }
    }
    return found;
}

/* condition 7's pick (engine 0x10012fd0 -> 0x10012de0): the enemies in iterator order (the player, then the units),
 * keeping each one nearer than all before it; a mech already attacked by a groupmate (state 2 / 3 with it as target)
 * is passed over unless the group's node kind is 4. *nearest = the nearest enemy's distance (the in-range test
 * 0x10012b40). Returns the pick (-1 the player, an actor) or -2 none. */
static int c7_pick(const msim *s, int i, float *nearest)
{
    const world_actor *ac = &s->actors[i];
    float best = 1e30f, x0 = (float)ac->mech.origin[0], z0 = (float)ac->mech.origin[2];
    int self_al = ac->friendly ? 0 : ac->alliance, pick = -2, k, q;
    int cur = ac->table < s->logic.table_count ? s->logic.current[ac->table] : -1;
    int kind4 = cur >= 0 && s->logic.tables[ac->table].nodes[cur].kind == 4;
    *nearest = 1e30f;
    for (k = -1; k < s->actor_count; k++) {
        float dx, dz, d;
        int claimed = 0;
        if (k < 0) {
            if (self_al == 0 || s->player_unit.destroyed || (!msim_dos_int && self_al == 2 && s->player_victim != i && !(s->provoked && s->provoked[i] > 0))) continue;
            dx = s->player[0] - x0; dz = s->player[2] - z0;
        } else {
            int al = s->actors[k].friendly ? 0 : s->actors[k].alliance;
            if (k == i || !s->armed[k] || s->units[k].destroyed || al == self_al || al == 2 || (!msim_dos_int && self_al == 2)) continue;
            dx = (float)s->actors[k].mech.origin[0] - x0; dz = (float)s->actors[k].mech.origin[2] - z0;
        }
        d = sqrtf(dx * dx + dz * dz);
        if (d >= best) continue;
        best = d;
        for (q = 0; q < s->actor_count && !kind4 && s->minds; q++) {
            const ai_mind *mq = &s->minds[q];
            float a1, a2, a3, a4, a5;
            int wq = -2;
            if (q == i || s->actors[q].table != ac->table || !s->armed[q] || s->units[q].destroyed) continue;
            if (mq->state != AI_TARGET && mq->state != AI_ATTACK) continue;
            if (!pick_enemy(s, q, &a1, &a2, &a3, &a4, &a5, &wq)) continue;
            if (wq == k) { claimed = 1; break; }
        }
        if (!claimed) pick = k;
    }
    *nearest = best;
    return best < 1e30f ? pick : -2;
}

/* group target sharing (0x10011c50 -> 0x10014ba0 -> 0x10014920): a sighting posted to the group's leader (condition 7
 * with its target, +0x144 / +0x146) sends ONE member at the target per event - not the leader, not commanded
 * (+0x152 & 3), not in state 2 / 3, with weapons left (0x10020880) and the engage flag +0x154 set; the leader only if no
 * member qualifies. The member is the one whose loadout rating +0x150 (combat_mek_rating, 0x10041c80) is closest to
 * the target's (|difference| as ints of the two u16s; the first in lance order on a tie; a difference of 0x7fff or more
 * never wins). leader: actor index, -1 = the player leads (the player's star). */
static int assign_ok(const msim *s, int q)
{
    const ai_mind *m = &s->minds[q];
    if (!s->armed[q] || s->units[q].destroyed || (s->cmd && s->cmd[q]) || m->fled || m->lock_bits) return 0;
    if (m->state == AI_TARGET || m->state == AI_ATTACK || (s->engage && !s->engage[q])) return 0;
    if (combat_max_range(&s->units[q]) <= 0) return 0;
    return ai_allows(&s->ai, m, AI_TARGET);   /* 0x10013e00 -> 0x10013d90 */
}
static void ai_wake(msim *s, int i);

static void target_fire_delay(msim *s, ai_mind *m)
{
    const unsigned delay = ms_roll(s, 10);
    if (msim_dos_int) m->dos_next_fire = msim_dos_frame_ticks(0, s->now) + (int)delay * 18;
    else m->next_fire = (float)(delay * 22u);
}
static void group_assign(msim *s, int table, int leader, int who)
{
    if (mw2_development_env("MW2_POST_TRACE")) fprintf(stderr, "group_assign %.2f table %d leader %d who %d ok %d\n", s->now / 1000.0, table, leader, who, leader >= 0 ? assign_ok(s, leader) : -1);   /* TEST ONLY */
    int q, pick = -1, best = 0x7fff;
    int want = who == -1 ? s->player_unit.rating : who >= 0 && who < s->actor_count ? s->units[who].rating : 0;
    for (q = 0; q < s->actor_count; q++) {
        int d;
        if (s->actors[q].table != table || q == leader || (leader < 0 && !s->actors[q].friendly)) continue;
        if (!assign_ok(s, q)) continue;
        d = abs((int)s->units[q].rating - want);
        if (d < best) { best = d; pick = q; }
    }
    if (pick < 0 && leader >= 0 && assign_ok(s, leader)) pick = leader;
    if (pick < 0) return;
    {
        ai_mind *m = &s->minds[pick];
        m->prev_state = m->state; m->state = AI_TARGET; m->target = 1; m->tgt = who;
        if (msim_dos_int) m->counter = 1;   /* DOS: the assignment pushes (state, target), depth 1 (the GBL port's audit) */
        target_fire_delay(s, m);
        ai_wake(s, pick);
    }
}

/* the node's targets (0x10014ba0), each tick the group's leader runs it for its star - never the player's star: the node
 * kind (+0x3c) must be non-zero and not 0x10 / 0x200 / 0x400 / 0x800 (mask 0xe10). The node's attached targets
 * (+0x128 count, +0x129 ids; cb_targets' order and class test) are taken in order; a target is skipped once done
 * (0x10013c40: kinds 1 / 2 / 4 / 0x1000 destroyed, 8 / 0x100 inspected / visited) or once covered: the node's count
 * (+0xb3; 0 = every member, leader included, not powered down) of members already on it (+0x14e) in the kind's state
 * (1 / 2: 2 or 3; 4: 7; 8: 2; 0x20 / 0x100 / 0x400: 8; 0x1000: any; 0x2000: 4). The first target left gets ONE member:
 * the first in lance order, not the leader, idle (0) or following (5) with weapons left (0x10020880); else the leader if
 * it idles with weapons; it enters the kind's state on the target (0x10013d40 -> 0x10013e00; while it has a state pushed
 * (+0x140), that pushed state is replaced instead) and takes the node's lock bits (0x100146e0) | 0x40 in +0x152.
 * The state by kind (0x10014e63..0x10014f30): 1 / 2 / 8 -> 2 (target), 4 -> 7 (patrol round it, 0x10013f00), 0x20 / 0x100 ->
 * 8 (go to it), 0x2000 -> 4 (flee it), 0x1000 -> 0 after the nuke at the member (0x10047020) - never reached: a 0x1000
 * node succeeds the moment it is ready (0x1000a1d0 LAB_1000a621 with its test flag still true) before 0x1000a9b0 picks
 * the current node, and kinds 0x20 / 0x2000 test an uninitialised flag in 0x10013c40 (taken as done, below). The target
 * code goes to +0x14c and +0x14e alike (0x10013e00 -> 0x10012a00): the movement (0x100130f0) heads for it, the aim
 * (0x100231a0) and the weapons' window (0x100232b0) take it - a nav point or a thing as much as a unit (pick_enemy). */
/* a task target (the node's code: 0x20000 | unit + 1 (0 the player), 0x10000 | nav point, 0x40000 | thing): where it is
 * (a thing: its box's centre - the engine's object +0x34 is its world bounding centre, 0x10029c10) and the reference
 * range 0x10013450 gives it: a nav point its radius (at least 100 cm), a unit or a thing 100 m. 0: gone (a destroyed
 * unit or thing, or none) */
/* a thing's point (engine 0x100257a0 / DOS 0x367f0, from the radar 0x10003960 / DOS 0x11cce, the target update
 * 0x1001d2d0 and the guidance 0x10045910): its OBJ entry's object (+0x1c, 0x101be5fc / DOS 0x101e90 +0x1c: set for every
 * object loaded, freed only while hidden, 0x10025310 -> 0x10025730) gives its world bounding centre +0x34..+0x3c - the
 * local vertex box's middle (0x10029060) through the node's placement (0x10029c10); only without an object the node's
 * +0x60..+0x68 (0x1000f7e0 / DOS 0x1df60), the model's origin. (The port had taken the origin: YELLSCN1's Wolf Dropship
 * waited 1-2 px from DOS's blip on the radar.) 0: no such thing, destroyed (flag 4) or hidden */
static int thing_point(const msim *s, int b, float p[3])
{
    const mech3d_part *pp;
    if (!s->world || b < 0 || b >= s->bld_count || s->bld[b].hp <= 0) return 0;
    if (s->bld[b].intact < 0 || s->bld[b].intact >= s->world->part_count) return 0;
    pp = &s->world->parts[s->bld[b].intact];
    if (pp->hidden) return 0;
    p[0] = (float)pp->pos[0]; p[1] = (float)pp->pos[1]; p[2] = (float)pp->pos[2];
    if (pp->model.object_count > 0 && pp->model.objects[0].vert_count > 0) {
        const wtb_object *o = &pp->model.objects[0];
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        float c[3];
        int v, j;
        for (v = 0; v < o->vert_count; v++) {
            const float q[3] = {(float)o->verts[v].x, (float)o->verts[v].y, (float)o->verts[v].z};
            for (j = 0; j < 3; j++) { if (q[j] < mn[j]) mn[j] = q[j]; if (q[j] > mx[j]) mx[j] = q[j]; }
        }
        for (j = 0; j < 3; j++) c[j] = (mn[j] + mx[j]) * 0.5f;
        if (pp->has_rot) {   /* (as part_vertex) */
            const float *R = pp->rot;
            for (j = 0; j < 3; j++) p[j] = R[j * 3] * c[0] + R[j * 3 + 1] * c[1] + R[j * 3 + 2] * c[2] + (float)pp->pos[j];
        } else {
            const float cy = cosf(pp->yaw * 3.14159265f / 180.0f), sy = sinf(pp->yaw * 3.14159265f / 180.0f);
            p[0] = cy * c[0] + sy * c[2] + (float)pp->pos[0];
            p[1] = c[1] + (float)pp->pos[1];
            p[2] = -sy * c[0] + cy * c[2] + (float)pp->pos[2];
        }
    }
    return 1;
}
int msim_thing_point(const msim *s, int b, float p[3]) { return thing_point(s, b, p); }
static int task_pos(const msim *s, int code, float p[3], float *range)
{
    int type = code >> 16, idx = code & 0xffff;
    *range = 10000.0f;
    if (type == 2) {
        idx--;
        if (idx < 0) { if (s->player_unit.destroyed) return 0; p[0] = s->player[0]; p[1] = s->player_unit.y; p[2] = s->player[2]; return 1; }
        if (idx >= s->actor_count || !s->armed[idx] || s->units[idx].destroyed) return 0;
        p[0] = (float)s->actors[idx].mech.origin[0]; p[1] = s->units[idx].y; p[2] = (float)s->actors[idx].mech.origin[2];
        return 1;
    }
    if (type == 1) {
        if (idx >= s->nav_count) return 0;
        p[0] = s->navs[idx].x; p[1] = s->navs[idx].y; p[2] = s->navs[idx].z;
        *range = s->navs[idx].radius >= 100.0f ? s->navs[idx].radius : 100.0f;
        return 1;
    }
    if (type == 4) {
        if (!thing_point(s, idx, p)) return 0;
        if (msim_dos_int || !s->world->parts[s->bld[idx].intact].model.object_count) return 1;
        int k;
        for (k = 0; k < 3; k++) p[k] = (s->bld[idx].mn[k] + s->bld[idx].mx[k]) * 0.5f;
        return 1;
    }
    return 0;
}
/* the unit's own temporary nav points (ai_mind.guard). Leaving state 5 or 7 (0x10013680 -> 0x10013e90) deletes them
 * walking the nav table by index: each deletion (0x1001cb40) shifts the rest down while the index still steps on, so
 * only every other one goes - the 1st, 3rd, 5th ... (the port keeps a unit's points apart from the other units': in the
 * engine's one table another unit's point between two of them takes the skip instead - ASSUMED none) */
static void guard_drop(ai_mind *m)
{
    int r, w = 0;
    for (r = 1; r < m->guard_n; r += 2, w++) { m->guard[w][0] = m->guard[r][0]; m->guard[w][1] = m->guard[r][1]; }
    m->guard_n = w;
}
/* entering patrol (0x10013790 case 7 -> 0x10013f00): four 30 m nav points (0x1001cab0) at x - R, z - R, x + R, z + R
 * round the target (R = 0x10013450's range; a thing 250 m, 0x46c35000), appended to the table in that order; the goal is
 * the first own point in the table (0x10014050(0x100) -> 0x1001dac0(1) -> 0x1001cca0 from index 1). On arriving (DEFLT
 * state 7: condition 3, the goal within its radius) the rule re-enters state 7 with the target (ref2 0x40 -> +0x14e):
 * exit (every other point deleted) and entry again - so the goal walks -x, -z, +z, -z of the next ring and then stays
 * on the oldest -x point left: a guard round a fixed target ends standing at its x - R point */
static void guard_enter(const msim *s, ai_mind *m, int code)
{
    static const float PX[4] = {-1, 0, 1, 0}, PZ[4] = {0, -1, 0, 1};
    float c[3], r;
    int k;
    if (!task_pos(s, code, c, &r)) return;   /* 0x10013f00: no position, no points */
    if ((code >> 16) == 4) r = 25000.0f;
    for (k = 0; k < 4 && m->guard_n < 16; k++, m->guard_n++) {   /* 0x1001cab0 refuses past 0x80 nav points */
        m->guard[m->guard_n][0] = c[0] + PX[k] * r;
        m->guard[m->guard_n][1] = c[2] + PZ[k] * r;
    }
}
static int node_target_list(const msim *s, const mtbl_node *n, int *code, int max)
{
    int i, c = 0;
    const char *t = n->target;
    if (!t[0] || strcasecmp(t, "NULL") == 0) return 0;
    if (is_player_rec(s, t) && (class_widen(s->player_class) & n->kind) && c < max) code[c++] = 0x20000;
    for (i = 0; i < s->actor_count && c < max; i++)
        if (strcasecmp(s->actors[i].group, t) == 0 && (class_widen(s->actors[i].obj_class) & n->kind)) code[c++] = 0x20000 | (i + 1);
    for (i = 0; i < s->nav_count && c < max; i++)
        if (strcasecmp(s->navs[i].name, t) == 0 && (class_widen(s->navs[i].cls) & n->kind)) code[c++] = 0x10000 | i;
    for (i = 0; s->world && i < s->bld_count && c < max; i++)
        if (strcasecmp(s->world->parts[s->bld[i].intact].rec, t) == 0 && (class_widen((unsigned)s->bld[i].type) & n->kind)) code[c++] = 0x40000 | i;
    return c;
}
static int node_target_done(msim *s, int table, uint32_t kind, int code)   /* 0x10013c40 */
{
    int type = code >> 16, idx = code & 0xffff;
    if (type == 2) idx--;   /* -1 = the player */
    if (kind == 1 || kind == 2 || kind == 4 || kind == 0x1000) return type == 1 ? 0 : tgt_test(s, table, MTBL_K_DESTROY2, type, idx);
    if (kind == 8 || kind == 0x100) return tgt_test(s, table, MTBL_K_SCAN, type, idx);
    return 1;   /* other kinds: the engine's test bit is uninitialised (no enemy star's node uses them) */
}
static int node_state(uint32_t kind, int st)   /* the kind's state test in the count (0x10014ba0) */
{
    switch (kind) {
    case 1: case 2: return st == AI_TARGET || st == AI_ATTACK;
    case 4: return st == AI_PATROL;
    case 8: return st == AI_TARGET;
    case 0x20: case 0x100: case 0x200: case 0x400: return st == AI_GODIRECT;
    case 0x1000: return 1;
    case 0x2000: return st == AI_FLEE;
    default: return 0;
    }
}
static int on_target(const msim *s, int q, int code)   /* +0x14e == the target */
{
    const ai_mind *m = &s->minds[q];
    if ((code >> 16) == 2) return (m->state == AI_TARGET || m->state == AI_ATTACK) ? m->tgt == (code & 0xffff) - 1 : m->node_tgt == code;
    return m->node_tgt == code;
}

static void sync_unit_node(msim *s, int i, int cur)
{
    if (!s->seen_node || s->seen_node[i] == cur) return;
    const world_actor *ac = &s->actors[i];
    ai_mind *m = &s->minds[i];
    const mtbl_node *node = cur >= 0 ? &s->logic.tables[ac->table].nodes[cur] : NULL;
    const uint32_t kind = node ? node->kind : 0;
    s->seen_node[i] = cur;
    m->lock_bits = node && node->lock == 1 ? 2 : node && node->lock == 2 ? 1 : 0;
    /* DOS 0x23c34: a node change leaves a pushed state alone. */
    if (msim_dos_int && m->counter) return;
    if (kind & 0x400) {
        m->state = AI_REST;
        m->target = 0;
        ai_power_down(s, i);
    } else if (kind & (0x800 | 0x10)) {
        m->state = AI_SHUTDOWN;
        m->target = 0;
        ai_power_down(s, i);
    } else {
        ai_wake(s, i);
        if (m->state == AI_FOLLOW || m->state == AI_PATROL) guard_drop(m);
        m->state = ac->leader ? AI_IDLE : AI_FOLLOW;
        m->target = 0;
        m->node_tgt = 0;
    }
}

static void node_assign(msim *s, int table, int leader)
{
    int cur = table < s->logic.table_count ? s->logic.current[table] : -1;
    const mtbl_node *n;
    int code[40], nt, members = 0, q, k, pick = -1, tcode = 0;
    float nearest = 1e30f;
    uint32_t kind;
    if (cur < 0) return;
    n = &s->logic.tables[table].nodes[cur];
    kind = n->kind;
    if (kind == 0 || (kind & 0xe10)) return;
    for (q = 0; q < s->actor_count; q++)
        if (s->actors[q].table == table && s->armed[q] && !s->units[q].destroyed && !(s->powered_down && s->powered_down[q])) members++;
    if (!members) return;
    nt = node_target_list(s, n, code, 40);
    for (k = 0; k < nt; k++) {
        int left = n->need ? n->need : members;
        if (node_target_done(s, table, kind, code[k])) continue;
        for (q = 0; q < s->actor_count && left; q++) {
            if (s->actors[q].table != table || !s->armed[q] || s->units[q].destroyed || (s->powered_down && s->powered_down[q])) continue;
            if (on_target(s, q, code[k]) && node_state(kind, s->minds[q].state)) left--;
        }
        if (!left) continue;
        if ((kind == 1 || kind == 2) && (code[k] >> 16) == 4) {
            const struct msim_building *b = &s->bld[code[k] & 0xffff];
            /* The DOS convoy order follows the attached moving targets in record order. */
            if (msim_dos_int && s->world->parts[b->intact].moving) {
                tcode = code[k];
                break;
            }
            const float dx = (b->mn[0] + b->mx[0]) * 0.5f - (float)s->actors[leader].mech.origin[0];
            const float dz = (b->mn[2] + b->mx[2]) * 0.5f - (float)s->actors[leader].mech.origin[2];
            const float distance = dx * dx + dz * dz;
            /* A record can place several equivalent targets, such as separate hangar doors.
             * Attach the nearest one to the star so each star can clear its own exit. */
            if (distance < nearest) { nearest = distance; tcode = code[k]; }
        } else {
            if (!tcode) tcode = code[k];
            break;
        }
    }
    if (!tcode) return;
    for (q = 0; q < s->actor_count && pick < 0; q++) {
        const ai_mind *m = &s->minds[q];
        if (q == leader || s->actors[q].table != table || !s->armed[q] || s->units[q].destroyed || (s->powered_down && s->powered_down[q])) continue;
        if (m->state == AI_IDLE || (m->state == AI_FOLLOW && !combat_out_of_weapons(&s->units[q]))) pick = q;
    }
    if (pick < 0) {
        if (s->minds[leader].state != AI_IDLE || combat_out_of_weapons(&s->units[leader])) return;
        pick = leader;
    }
    {
        ai_mind *m = &s->minds[pick];
        int unit = (tcode >> 16) == 2, st = (kind == 1 || kind == 2 || kind == 8) ? AI_TARGET : kind == 4 ? AI_PATROL :
                                             (kind == 0x100 || kind == 0x20) ? AI_GODIRECT : -1;   /* (0x2000 -> 4 and
                                                                                   * 0x1000 never get here: above) */
        if (st < 0) return;
        /* A neutral instructor acquires its player reference after startup begins. */
        if (msim_dos_int && tcode == 0x20000 && s->actors[pick].alliance == 2 &&
            s->online_at && s->online_at[pick] < 0.0f) return;
        m->lock_bits |= n->lock == 1 ? 2 : n->lock == 2 ? 1 : 0;   /* +0x152 |= 0x100146e0(+0xb7) | 0x40, whatever follows */
        /* 0x10013d40: with a state pushed (+0x140 = 1) the (state, target) pair is written to slot +0x140 (+0x12c) - the
         * pop reads slot +0x140 - 1 (+0x128): the assignment is lost */
        if (m->counter) return;
        if (!ai_allows(&s->ai, m, st)) return;                      /* 0x10013e00 -> 0x10013d90 */
        if (mw2_development_env("MW2_DEBUG_NODE_TASKS")) {   /* TEST ONLY */
            float tp[3] = {0, 0, 0}, tr = 0;
            task_pos(s, tcode, tp, &tr);
            fprintf(stderr, "node task %.2f table %d kind %#x actor %d (%s) state %d -> %d target %#x %s at %.0f,%.0f,%.0f range %.0f\n", s->now / 1000.0,
                    table, (unsigned)kind, pick, s->actors[pick].name, m->state, st, (unsigned)tcode, n->target, tp[0], tp[1], tp[2], tr);
        }
        if (m->state == AI_FOLLOW || m->state == AI_PATROL) guard_drop(m);   /* exit 0x10013680 cases 5 / 7 -> 0x10013e90 */
        m->node_tgt = tcode;
        m->prev_state = m->state; m->state = st; m->target = st == AI_TARGET; m->tgt = unit && st == AI_TARGET ? (tcode & 0xffff) - 1 : -2;
        m->to_navref = 0;
        if (st == AI_TARGET) target_fire_delay(s, m);
        if (st == AI_PATROL) guard_enter(s, m, tcode);                       /* entry case 7 -> 0x10013f00 */
        ai_wake(s, pick);
    }
}

static float wrap180(float a)
{
    while (a > 180) a -= 360;
    while (a < -180) a += 360;
    return a;
}

static void combat_step(msim *s, int32_t dt_ms, int dos_ticks);
static void projectile_step(msim *s, float ticks, int dos_ticks);
static void terrain_release(void *t);
static float wrap180(float a);
static int los_blocked(const msim *s, float x0, float y0, float z0, float x1, float y1, float z1);
static int dos_clear_to(msim *s, int self, int target_unit, int target_thing, const float from[3], const float to[3]);
static void dos_ai_fire_decision(msim *s, int actor, const ai_view *view, int target_unit);
static void dos_ai_weapon_step(msim *s, int i, float dt, int ticks, int starting);
static float drive_servo(float cur, float tgt, float T, float ticks);
/* MW2.EXE's vector length (the normalise 0x40500, the collisions 0x20890 / 0x20a50): the octagonal estimate (4 max + mid +
 * min) / 4 of the axis offsets (DOS integers >> 2; here as floats) */
static float oct_len(float x, float y, float z)
{
    float a = fabsf(x), b = fabsf(y), c = fabsf(z), t;
    if (a < b) { t = a; a = b; b = t; }
    if (a < c) { t = a; a = c; c = t; }
    return (a * 4.0f + b + c) * 0.25f;
}
float msim_dos_len(float x, float y, float z) { return oct_len(x, y, z); }
static int avoid_obstacles(msim *s, int i, ai_mind *m, float goal_bearing, float ticks);
struct ai_avoid_context { msim *s; ai_mind *mind; int actor, target_unit; float ticks; };

static int dos_avoid_attack(void *context, float bearing)
{
    const struct ai_avoid_context *c = context;
    int avoided = avoid_obstacles(c->s, c->actor, c->mind, bearing, c->ticks);
    return avoided && !(c->mind->man == 6 && c->target_unit >= -1 && c->target_unit == c->mind->dos_avoid_unit);
}
static unsigned rnd_ms(unsigned *x) { *x = *x * 1103515245u + 12345u; return (*x >> 16) & 0x7FFF; }
static float frand_ms(unsigned *x) { return (float)rnd_ms(x) / 32768.0f; }
static unsigned ms_roll(void *ctx, unsigned n)
{
    msim *s = ctx;
    if (!n) return 0;
    if (!msim_dos_int) return rnd_ms(&s->rng) % n;
    unsigned value = s->dos_random[s->dos_random_index];
    s->dos_random_index = (s->dos_random_index + 1) % (sizeof s->dos_random / sizeof s->dos_random[0]);
    return value % n;
}

int msim_dos_sound_pitch(msim *s)
{
    static const int rates[10] = {9922, 10143, 10364, 10584, 10804, 11025, 11246, 11466, 11686, 11907};
    return rates[ms_roll(s, 10)];
}

int msim_dos_sound_at(msim *s, int32_t x, int32_t y, int32_t z, int32_t listener,
                      int half, int account_pitch, int *pan, int *delay)
{
    const int32_t bearing = dos_atan2(x, z);
    const int32_t distance = dos_slant_distance(dos_level_distance(x, z), y);
    if (distance >= 50000) return 0;
    const int metres = distance / 100;
    const int volume = 100 - metres * metres * 100 / 250000;
    const int p = 64 + (dos_sin(listener - bearing) >> 23);
    *pan = p < 15 ? 15 : p > 111 ? 111 : p;
    *delay = (distance * 0x168 + 0x8000) >> 16;
    /* 0x40c90 draws before the mixer tries to allocate a channel. 0x409b0
     * chooses the sample's configured rate, so only the random cursor changes. */
    if (account_pitch) msim_dos_sound_pitch(s);
    return half ? volume >> 1 : volume;
}

/* engine death state 4 (0x1001a180 / 0x10021f20 -> 0x1001be40): the death explosion (0x0d) at once,
 * then for 0x712 ticks, each tick a 21% chance of an explosion (type 3, 60%) or debris (0x10b, 40%)
 * at a Gaussian offset (sd 512 cm) around the wreck */
static float gauss(unsigned *rng)
{
    float g = 0;
    int k;
    for (k = 0; k < 30; k++) g += frand_ms(rng);
    return (g - 15.0f) / 1.5811388f;   /* the engine's table: sum of 30 rand(), unit spread */
}


static int32_t dos_gauss(msim *s)
{
    unsigned i = s->dos_gaussian_index + 1;
    s->dos_gaussian_index = i % (sizeof s->dos_gaussian / sizeof s->dos_gaussian[0] - 1);
    return s->dos_gaussian[i];
}

static void debris_start_motion(msim *s, msim_debris *d, int32_t radius, const mech3d_part *part)
{
    d->moving = 1;
    memset(d->ang, 0, sizeof d->ang);
    if (msim_dos_int) {
        int gravity = dos_gravity(s->planet.gravity_g);
        int divisor = (int)((int64_t)gravity * 1024 / 0x794);
        if (divisor < 1) divisor = 1;
        unsigned occupied[MSIM_MAX_DEBRIS] = {0};
        for (int i = 0; i < s->debris_count; i++) {
            const msim_debris *other = &s->debris[i];
            if (other != d && other->moving) occupied[other->dos_body] = 1;
        }
        for (d->dos_body = 0; occupied[d->dos_body]; d->dos_body++) {}
        if (!d->dos_chunk) {
            const dos_matrix base = dos_matrix_rotation(0, part ? (int32_t)lroundf(part->yaw * 65536.0f) : 0, 0);
            memcpy(d->dos_rot, base.r, sizeof d->dos_rot);
            if (part && part->has_dos_rot) memcpy(d->dos_rot, part->dos_rot, sizeof d->dos_rot);
            else if (part && part->has_rot)
                for (int k = 0; k < 9; k++) d->dos_rot[k] = (int32_t)llround(part->rot[k] * 536870912.0);
            d->dos_rot_left = 100;
        }
        for (int j = 0; j < 3; j++) {
            int axis = (const int[]){0, 2, 1}[j];
            int32_t q = dos_gauss(s) + (axis == 1 ? 1024 : 0);
            d->dos_velocity[axis] = dos_projectile_mul(q * 65536 / divisor, 0x57e98);
            d->v[axis] = (float)d->dos_velocity[axis] / 65536.0f;
        }
        for (int axis = 0; axis < 3; axis++) d->dos_spin[axis] = dos_gauss(s) * 0x7e98 / 1024;
        d->dos_bottom = radius / 2;
        if (d->dos_chunk && mw2_development_env("MW2_DEBRIS_TRACE") && atoi(mw2_development_env("MW2_DEBRIS_TRACE")) > 1)
            fprintf(stderr, "chunk %d body %d\n", msim_dos_frame_ticks(0, s->now), d->dos_body);
        return;
    }
    d->v[0] = gauss(&s->rng) * 5.5f;
    d->v[1] = (gauss(&s->rng) + 1.0f) * 5.5f;
    d->v[2] = gauss(&s->rng) * 5.5f;
    for (int axis = 0; axis < 3; axis++) d->spin[axis] = gauss(&s->rng) * 0.5f;
}

/* debris: launched with Gaussian velocities (5.5 cm/tick sd sideways, (g + 1) x 5.5 up) and spins
 * (0.5 / tick sd); gravity 0.0296 cm/tick^2; on landing vy = -vy / 4, and at random the horizontal
 * speed and spin halve and reflect; at rest below 0.5525 cm/tick (engine 0x100152e0, 0x100156f0) */
static void spawn_debris(msim *s, int count, const float p[3])
{
    /* (not gated by CHUNKY EXPLOSIONS: the engine reads that only when instantiating GT 0x8000 debris nodes, 0x100254ad) */
    int32_t now = (int32_t)((int64_t)s->now * 182 / 1000);
    int chunks = 0, k;
    uint32_t occupied = 0;
    for (k = 0; k < s->debris_count; k++) {
        chunks += s->debris[k].kind < 2;
        if (s->debris[k].dos_chunk) occupied |= UINT32_C(1) << (s->debris[k].dos_chunk - 1);
    }
    /* MW2_SHT1 has 32 chunk (type 0x0b) objects - the chunks' own pool; the physics bodies (128) are shared with the
     * blown-off parts (0x10015230) */
    while (count-- > 0 && chunks < MSIM_DOS_CHUNK_COUNT && s->debris_count < MSIM_MAX_DEBRIS) {
        msim_debris *d = &s->debris[s->debris_count++];
        d->kind = chunks++ & 1;
        d->dos_chunk = 0;
        for (int i = 0; i < s->dos_chunk_count; i++) {
            uint32_t slot = UINT32_C(1) << i;
            if (occupied & slot) continue;
            occupied |= slot;
            d->dos_chunk = i + 1;
            d->kind = s->dos_chunk_kind[i];
            memcpy(d->dos_rot, s->dos_chunk_rot[i], sizeof d->dos_rot);
            break;
        }
        memcpy(d->p, p, sizeof d->p);
        debris_start_motion(s, d, s->dos_chunk_radius[d->kind], NULL);
        d->until = now + (msim_dos_int ? 1820 : 1810);
    }
}

/* an effect through 0x10045d00: types 3 and 4 (the missile family - every sub-type: ground 0x0c, player 0x11, structure
 * 0x13 / 0x14) also throw a 0x10b, four chunks (0x10045dcc / 0x10045e1c set the follow-up, 0x100463de), but only when
 * the effect itself was placed (a refused one - a neighbour within 5 m, a full pool - returns before it) */
/* TEST ONLY (MW2_AIM_TRACE): a player shot's impact against the reticle's line (the eye along the aim): the angle off
 * it seen from the eye (deg), and the miss distance from the line (cm) */
static void aim_trace(const msim *s, const msim_shot *sh, const char *what, const float p[3])
{
    const float *d = sh->aim_d;
    float ay = atan2f(d[0], d[2]), v[3], t, m[3], ml, vl;
    int k;
    if (!mw2_development_env("MW2_AIM_TRACE") || !s->player_eye_ok) return;
    for (k = 0; k < 3; k++) v[k] = p[k] - sh->aim_o[k];
    t = v[0] * d[0] + v[1] * d[1] + v[2] * d[2];
    for (k = 0; k < 3; k++) m[k] = v[k] - d[k] * t;
    ml = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]); vl = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    printf("AIMHIT now=%d %s range=%.0f off_deg=%.3f miss_cm=%.0f right=%.0f up=%.0f conv=%.0f w=%d\n", (int)s->now, what, t, vl > 0 ? asinf(ml / vl) * 180.0f / 3.14159265f : 0.0f, ml,
           m[0] * cosf(ay) - m[2] * sinf(ay), m[1], s->player_conv, sh->weapon);
}
static int missile_fx(int weapon) { int v = combat_weapon_visual(weapon); return v >= 0 && ((v & 0xff) == 3 || (v & 0xff) == 4); }
/* effect types whose live effect damages the units round it (0x1025b180: +0x18 of the 0x1025b168 rows) */
static const unsigned char AREA_TYPE[32] = {0,0,0,1, 1,0,1,1, 0,0,0,0, 1,1,0,0, 0,1,0,1, 1,1,1,0, 0,0,0,0, 0,0,0,0};
static void blow_off_unit(msim *s, int unit);
static void effect(msim *s, int type, const float p[3], int chunks)
{
    s->fx_refused = 0;
    if (s->on_effect) s->on_effect(s->shot_user, type, p);
    if (chunks && !s->fx_refused) spawn_debris(s, 4, p);
    if (!s->fx_refused && type >= 0 && type < 32 && AREA_TYPE[type] && s->area_count < (int)(sizeof s->area / sizeof s->area[0])) {
        int index = s->area_count, slot = 0;
        if (msim_dos_int) {
            /* MW2_SHT1 groups damaging effects by type. 0x51153 chooses
             * the first free slot of that type, and 0x515e5 visits slots in order. */
            index = 0;
            while (index < s->area_count && s->area[index].type < type) index++;
            while (index < s->area_count && s->area[index].type == type && s->area[index].dos_slot == slot) {
                index++;
                slot++;
            }
            memmove(s->area + index + 1, s->area + index, (size_t)(s->area_count - index) * sizeof s->area[0]);
        }
        s->area_count++;
        struct msim_area *a = &s->area[index];
        a->dos_slot = slot;
        a->type = type; memcpy(a->pos, p, sizeof a->pos); a->r = s->fx_radius[type];
        a->dos_ticks_left = fx_duration(type, msim_dos_int);
        a->end = msim_dos_frame_ticks(0, s->now) + a->dos_ticks_left;
        /* At 0x5137e DOS adds the creating frame's ticks to the lifetime. */
        if (msim_dos_int) a->dos_ticks_left += s->dos_frame_ticks;
    }
}
/* 0x10046490, every frame: each live effect of a damaging type, with SPLASH DAMAGE on (DAT_1024ac6c + 2), calls
 * 0x10046700(pos, radius, 1/256): every unit whose position (object +0x50..+0x58: its centre, the feet + the MGEO centre
 * height controller +0xcc - 0x10019fad / 0x10019db8, as the missile guidance's aim point) is nearer the effect than its contact
 * radius (controller +0xe8) + the effect's radius takes ticks / 256 on each location 2-8 and half that on the head
 * (location 1; DOS MW2.EXE 0x517c0, the same loop over 0x26270: a quarter, and the distance as the octagonal estimate
 * (4 x max + mid + min) / 4 of the axis offsets) through 0x100171d0 (so a CT share spreads as a hit's); a unit in
 * state 4 (destroyed: its own death explosion, the wreck's blasts) twice that (x DAT_102485f8 = 2) - 0x100171d0 has no state test, so a wreck loses
 * locations (blown off) and its bins cook off. Skipped: controller +0x10c bit 0x100 (a wreck blown apart on landing,
 * 0x1000c4d0 - not modelled) and state 5 counts once (an ejected unit stands intact). Then the effect's life runs down
 * by the ticks (it ends at <= 0) */
/* the units within r (+ each one's contact radius) of pos take base on each location 2-8 and half (DOS a quarter) on the
 * head, a wreck twice that - 0x10046700's loop (see area_damage) */
static void units_in_radius(msim *s, const float pos[3], float r, float base)
{
    for (int slot = 0; slot <= s->actor_count; slot++) {
        int u = slot - 1;
        if (msim_dos_int) {
            const int index = s->actor_count - slot - 1;
            u = index >= 0 && s->dos_anim_order ? s->dos_anim_order[index] : index;
        }
        combat_unit *cu = u < 0 ? &s->player_unit : &s->units[u];
        float x = u < 0 ? s->player[0] : (float)s->actors[u].mech.origin[0], z = u < 0 ? s->player[2] : (float)s->actors[u].mech.origin[2];
        float y = cu->y + (u < 0 ? s->player_centre_h : s->centre_h ? s->centre_h[u] : 550.0f), rr = u < 0 ? s->player_radius : (s->radius ? s->radius[u] : 450.0f), dx = x - pos[0], dy = y - pos[1], dz = z - pos[2];
        float d = base * (cu->destroyed && !cu->intact_out ? 2.0f : 1.0f);
        if (u >= 0 && !s->armed[u]) continue;
        {   /* the octagonal estimate (4 x max + mid + min) / 4 of |dx|, |dy|, |dz| - DOS 0x517c0 (>> 2) and the 3D
             * editions' ApproximateVectorLength alike (3Dfx 0x10030330, called by 0x10046700: demech2's reading - the
             * port had the exact distance there) */
            float p0 = fabsf(dx), p1 = fabsf(dy), p2 = fabsf(dz), q;
            if (p0 < p1) { q = p0; p0 = p1; p1 = q; }
            if (p0 < p2) { q = p0; p0 = p2; p2 = q; }
            const float distance = (p0 * 4.0f + p1 + p2) * 0.25f;
            if (msim_dos_int ? (int32_t)distance >= (int32_t)rr + (int32_t)r : distance >= rr + r) continue;
        }
        /* the head: half (3Dfx 0x10046700: / DAT_102485f8 = 2.0); a quarter in DOS (MW2.EXE 0x517c0: 0x26270(iVar4 >> 2, 1)) */
        combat_damage_at(cu, msim_dos_int ? d / 4.0f : d / 2.0f, 1, &s->rng);
        for (int l = 2; l <= 8; l++) combat_damage_at(cu, d, l, &s->rng);
        if (msim_dos_int) blow_off_unit(s, u);
    }
}
static void part_vertex(const mech3d_part *p, const wtb_vertex *v, float out[3]);
static int part_box(const mech3d_part *p, float mn[3], float mx[3]);
/* a part's bounding sphere as placed now (ComputeShapeBounds 0x10029890: the vertex box's centre, the farthest vertex) */
static int part_sphere(const mech3d_part *p, float c[3], float *r)
{
    float mn[3], mx[3], best = 0;
    int v, j;
    if (part_box(p, mn, mx) != 0) return -1;
    for (j = 0; j < 3; j++) c[j] = (mn[j] + mx[j]) * 0.5f;
    for (v = 0; v < p->model.objects[0].vert_count; v++) {
        float q[3], d;
        part_vertex(p, &p->model.objects[0].verts[v], q);
        d = (q[0] - c[0]) * (q[0] - c[0]) + (q[1] - c[1]) * (q[1] - c[1]) + (q[2] - c[2]) * (q[2] - c[2]);
        if (d > best) best = d;
    }
    *r = sqrtf(best);
    return 0;
}
/* HeatMechsNearFires (Win95 0x1006c362, 3Dfx 0x10046e70; with heat tracking on, every frame before the effects): each
 * fire / smoke card in the scene (shape kind 0xf0 == 0x10: the OBJ type word's class - 73 objects in the missions, the
 * YELLSMO1 smoke, the burning wrecks of TEAL / TAUP / PLUM / PINK ...) heats every mech within twice its bounding radius
 * r plus the mech's own: (r - r d / reach) / 8 (1.1, 16.16 heat) or x 0.25 (the 3D editions' floats) a tick, d the
 * octagonal distance estimate from the mech's position (its feet) to the card's centre. A port addition until now
 * (missing): traced from the demech2 decompilation's reading of these addresses */
static void fires_heat(msim *s, float ticks)
{
    int q, u;
    if (!s->world || ticks <= 0) return;
    for (q = 0; q < s->world->part_count; q++) {
        const mech3d_part *p = &s->world->parts[q];
        float c[3], r;
        if ((p->objtype & 0xf0) != 0x10 || p->hidden || part_sphere(p, c, &r) != 0) continue;
        r *= 2.0f;
        if (mw2_development_env("MW2_FIRE_TRACE")) { static int said; if (said++ < 10) { fprintf(stderr, "fire %d %s at %.0f,%.0f,%.0f r %.0f\n", q, p->model_name, c[0], c[1], c[2], r); } }   /* TEST ONLY */
        for (u = -1; u < s->actor_count; u++) {
            combat_unit *cu = u < 0 ? &s->player_unit : &s->units[u];
            float x = u < 0 ? s->player[0] : (float)s->actors[u].mech.origin[0], z = u < 0 ? s->player[2] : (float)s->actors[u].mech.origin[2];
            float rr = u < 0 ? s->player_radius : (s->radius ? s->radius[u] : 450.0f), reach = rr + r;
            float p0 = fabsf(x - c[0]), p1 = fabsf(cu->y - c[1]), p2 = fabsf(z - c[2]), t, d;
            if ((u >= 0 && !s->armed[u]) || cu->destroyed || cu->no_heat) continue;
            if (p0 < p1) { t = p0; p0 = p1; p1 = t; }
            if (p0 < p2) { t = p0; p0 = p2; p2 = t; }
            d = (p0 * 4.0f + p1 + p2) * 0.25f;
            if (d >= reach) continue;
            cu->heat += (r - r * d / reach) * (msim_dos_int ? 0.125f : 0.25f) / 65536.0f * ticks;
        }
    }
}
static void area_damage(msim *s, float ticks)
{
    int32_t now = (int32_t)((int64_t)s->now * 182 / 1000);
    int k;
    for (k = 0; k < s->area_count; ) {
        struct msim_area *a = &s->area[k];
        if (!msim_dos_int && now >= a->end) { s->area[k] = s->area[--s->area_count]; continue; }
        if (s->splash) units_in_radius(s, a->pos, a->r, ticks / 256.0f);
        /* DOS 0x51684 subtracts elapsed ticks after applying this frame's damage. */
        if (msim_dos_int && (a->dos_ticks_left -= (int32_t)ticks) <= 0) {
            s->area_count--;
            memmove(a, a + 1, (size_t)(s->area_count - k) * sizeof *a);
            continue;
        }
        k++;
    }
}

/* a bin explosion (0x100167b0, the count not 0): explosion 7 at the location's node (0x1000fec0 / 0x1000f760 ->
 * 0x10045c30) - a damaging effect type (0x1025b180), so it hurts every location of the units round it, the exploding
 * one too (area_damage): the node 0x1000fec0 finds - the first in the tree whose model's group (+0x16) is the
 * location, else the centre torso's - at its object's world bounding centre (0x1000f760 -> 0x10029890: the box middle of the part's vertices placed
 * by the node, mech3d_group_point) */
static void ammo_booms(msim *s)
{
    int u;
    for (u = -1; u < s->actor_count; u++) {
        combat_unit *cu = u < 0 ? &s->player_unit : &s->units[u];
        const mech3d *m = u < 0 ? &s->pl_mech : msim_unit_hull(s, u);   /* its node objects: the drawn representation's */
        float o[3], p[3], h;
        int loc = cu->ammo_boom_loc;
        if (!loc) continue;
        cu->ammo_boom_loc = 0;
        if (u >= 0 && !s->armed[u]) continue;
        o[0] = u < 0 ? s->player[0] : (float)s->actors[u].mech.origin[0];
        o[1] = cu->y;
        o[2] = u < 0 ? s->player[2] : (float)s->actors[u].mech.origin[2];
        h = u < 0 ? s->player_facing : s->actors[u].mech.heading;
        if (mech3d_group_point(m, loc, o, h, p) != 0) continue;   /* no such node: no explosion (0x100167b0) */
        effect(s, 7, p, 0);
    }
}

/* a destroyed location's parts fly off (engine 0x10015410: same physics, 0xe24 ticks, pool of 128) */
/* the parts a newly destroyed location sends flying (engine 0x10016640 -> 0x1000ff70 -> 0x10015530 / 0x10015410: the
 * location's node and everything under it - the centre torso carries the hips, so all of a mech; same physics as the
 * chunks, 0xe24 ticks, 128 bodies), each part once: those under a location in `mask` and under none in `old` */
static void blow_off(msim *s, int unit, unsigned mask, unsigned old)
{
    if (msim_dos_int && unit >= 0 && (mask & ~old)) {
        /* Movement has already updated the root and servos. Keep the current
         * local animation keys and detach from their new world transform. */
        mech3d *mech = &s->actors[unit].mech;
        mech3d_pose(mech, (const anim_set *)mech->pose_anim, mech->pose_t);
    }
    const mech3d *m = unit < 0 ? &s->pl_mech : &s->actors[unit].mech;
    int32_t now = (int32_t)((int64_t)s->now * 182 / 1000);
    int q, k;
    for (q = 0; q < m->part_count && s->debris_count < MSIM_MAX_DEBRIS; q++) {
        msim_debris *d;
        if (m->parts[q].model.object_count < 1 || !mech3d_fly_loc(m, q, mask) || mech3d_fly_loc(m, q, old)) continue;
        d = &s->debris[s->debris_count++];
        d->dos_chunk = 0;
        d->kind = 2; d->actor = unit; d->part = q;
        for (k = 0; k < 3; k++) d->p[k] = (float)m->parts[q].pos[k];
        debris_start_motion(s, d, m->parts[q].dos_sphere[3], &m->parts[q]);
        if (msim_dos_int) {
            unsigned countdown = mech3d_dos_part_countdown(m, (const anim_set *)m->pose_anim, m->parts[q].node);
            if (countdown) d->dos_rot_left = countdown;
        }
        d->until = now + (msim_dos_int ? 0xe38 : 0xe24);
    }
}
static void blow_off_unit(msim *s, int unit)
{
    if (!s->fly_seen || (unit >= 0 && !s->armed[unit])) return;
    const combat_unit *cu = unit < 0 ? &s->player_unit : &s->units[unit];
    const unsigned mask = mech3d_fly_mask(cu->loc_gone);
    unsigned *seen = &s->fly_seen[unit + 1];
    if (mask != *seen) {
        blow_off(s, unit, mask, *seen);
        *seen = mask;
    }
}

static void blow_offs(msim *s)
{
    for (int u = -1; u < s->actor_count; u++) blow_off_unit(s, u);
}

/* DOS 0x2b930: integrate the bottom of the object's sphere in whole centimetres,
 * then use rounded horizontal deltas and the fixed-point landing response. */
static int dos_debris_step(msim *s, msim_debris *d, int ticks, int32_t angles[3])
{
    int32_t *v = d->dos_velocity;
    int32_t old_vy = v[1], dv = -dos_gravity(s->planet.gravity_g) * ticks;
    int32_t bottom = (int32_t)d->p[1] - d->dos_bottom;
    int32_t y = bottom + (int32_t)(((int64_t)(old_vy + (dv >> 1)) * ticks) >> 16);
    int rest = 0;
    v[1] += dv;
    if (v[1] <= 0) {
        int32_t floor = (int32_t)msim_ground(s, d->p[0], d->p[2], (float)bottom);
        if (y <= floor) {
            y = floor;
            rest = old_vy > 0 || v[1] > -36008;
            v[1] = -(v[1] >> 2);
            if (ms_roll(s, 2)) {
                v[1] >>= 1;
                v[0] = -(v[0] >> 1);
                v[2] = -(v[2] >> 1);
            }
            for (int axis = 0; axis < 3; axis++) d->dos_spin[axis] = -(d->dos_spin[axis] >> 1);
        }
    }
    d->p[0] += (float)dos_projectile_mul(v[0], ticks);
    d->p[1] = (float)(y + d->dos_bottom);
    d->p[2] += (float)dos_projectile_mul(v[2], ticks);
    for (int axis = 0; axis < 3; axis++) {
        angles[axis] = d->dos_spin[axis] * ticks;
        d->v[axis] = rest ? 0.0f : (float)v[axis] / 65536.0f;
    }
    return !rest;
}

static void debris_step(msim *s, float ticks)
{
    int32_t now = (int32_t)((int64_t)s->now * 182 / 1000);
    int i, k;
    if (msim_dos_int) {
        /* The body slots are shared by chunks and detached nodes (0x2b740). */
        msim_debris *bodies[MSIM_MAX_DEBRIS] = {0};
        for (i = 0; i < s->debris_count; i++)
            if (s->debris[i].moving) bodies[s->debris[i].dos_body] = &s->debris[i];
        for (i = 0; i < MSIM_MAX_DEBRIS; i++) {
            msim_debris *d = bodies[i];
            if (!d) continue;
            int32_t step[3];
            unsigned *left = d->dos_chunk ? &s->dos_chunk_left[d->dos_chunk - 1] : &d->dos_rot_left;
            d->moving = dos_debris_step(s, d, (int)ticks, step);
            mech3d_dos_rotate(d->dos_rot, left, &s->dos_anim_phase, step[0], step[1], step[2]);
        }
    }
    for (i = 0; i < s->debris_count; i++) {
        msim_debris *d = &s->debris[i];
        if (now >= d->until) {
            if (d->dos_chunk) memcpy(s->dos_chunk_rot[d->dos_chunk - 1], d->dos_rot, sizeof d->dos_rot);
            if (i < --s->debris_count) s->debris[i] = s->debris[s->debris_count];
            i--;
            continue;
        }
        if (!d->moving || msim_dos_int) continue;
        {   /* 0x100156f0, in its order: the new vertical speed and height; touching the ground (vy under 1e-7 and the height
             * at or under it) - at rest if the speed was under 0.5525 cm/tick (0xbf0d6fbc) or it had been rising, the
             * height the ground's, vy = -vy / 4, and on rand-table % 2 (0x1003d910 with 2: 1 in 2) vy halved again and
             * the horizontal speed halved and reflected; the spin always halved and reflected (degrees a tick: the
             * angles go x pi / 180 into 0x1002f070); then the move and the turn at the new speeds; at rest the slot's
             * motion is cleared (0x10015a50) and the piece stays */
            float v0 = d->v[1], vy = v0 - 0.0296f * ticks, ny = d->p[1] + (v0 - 0.5f * 0.0296f * ticks) * ticks, g;
            int rest = 0;
            if (vy < 1e-7f) {
                g = msim_ground(s, d->p[0], d->p[2], d->p[1]);
                if (ny <= g) {
                    rest = vy > -0.5525f || v0 > 1e-7f;
                    ny = g;
                    vy = -vy * 0.25f;
                    if (rnd_ms(&s->rng) % 2) { vy *= 0.5f; d->v[0] *= -0.5f; d->v[2] *= -0.5f; }
                    for (k = 0; k < 3; k++) d->spin[k] *= -0.5f;
                }
            }
            d->p[0] += d->v[0] * ticks; d->p[2] += d->v[2] * ticks;
            for (k = 0; k < 3; k++) d->ang[k] += d->spin[k] * ticks;
            d->p[1] = ny; d->v[1] = vy;
            if (rest) { d->moving = 0; d->v[0] = d->v[1] = d->v[2] = 0; for (k = 0; k < 3; k++) d->spin[k] = 0; }
        }
    }
}

static void car_inc(msim *s, int off);
static void wrecks(msim *s, int32_t dt_ms)
{
    int32_t now = (int32_t)((int64_t)s->now * 182 / 1000);
    int64_t f;
    if (!s->wreck_until) return;
    for (int slot = 0; slot <= s->actor_count; slot++) {
        int i = msim_dos_int ? (slot == 0 ? s->actor_count : slot - 1) : slot;
        const combat_unit *u = i < s->actor_count ? &s->units[i] : &s->player_unit;
        float x, y, z;
        if (i < s->actor_count && !s->armed[i]) continue;
        if (!u->destroyed || u->intact_out) continue;   /* state 5 has no death sequence (0x1001a180 case 4 only) */
        if (i < s->actor_count) {
            x = (float)s->actors[i].mech.origin[0]; z = (float)s->actors[i].mech.origin[2];
            y = u->y + (msim_dos_int && s->centre_h ? s->centre_h[i] : s->height[i] * 0.5f);
        } else {
            x = s->player[0]; z = s->player[2];
            y = u->y + (msim_dos_int ? s->player_centre_h : 500.0f);
        }
        if (s->wreck_until[i] == 0) {
            float p[3] = {x, y, z};
            if (i == s->actor_count && s->on_player_down) s->on_player_down(s->shot_user);
            effect(s, 0x0d, p, 0);   /* 0x1001be40 (+0x8c == 0): a damaging type - area damage round the wreck (0x10046490) */
            s->wreck_until[i] = now + (msim_dos_int ? 0x71c : 0x712);
            continue;
        }
        if (msim_dos_int) {
            if (now < s->wreck_until[i] && ms_roll(s, 100) <= 20) {
                float p[3] = {x + (float)(dos_gauss(s) / 2), y, z + (float)(dos_gauss(s) / 2)};
                if (ms_roll(s, 100) < 60) effect(s, 3, p, 1);
                else spawn_debris(s, 4, p);
            }
            continue;
        }
        /* once per controller update - per FRAME, not per tick (0x1001a180 runs once a frame, its timers in ticks;
         * DOSBox: two or three explosions at a time, where 21 % of 182 ticks a second gave a wall of them); the frame
         * rate taken as 30 a second (ASSUMED, as the display damage) */
        for (f = (int64_t)(s->now - dt_ms) * 30 / 1000 + 1; f <= (int64_t)s->now * 30 / 1000 && (int32_t)(f * 1000 / 30 * 182 / 1000) < s->wreck_until[i]; f++) {
            if ((int)(frand_ms(&s->rng) * 100.0f) < 21) {
                float p[3] = {x + gauss(&s->rng) * 512.0f, y, z + gauss(&s->rng) * 512.0f};
                if ((int)(frand_ms(&s->rng) * 100.0f) < 60) effect(s, 3, p, 1);   /* + its four chunks */
                else spawn_debris(s, 4, p);   /* 0x10b: (0x100 >> 8) & 1 -> four chunks (0x10045f06 -> 0x10046d40) */
            }
        }
    }
}

static void part_vertex(const mech3d_part *p, const wtb_vertex *v, float out[3]);
/* the world box of a part as placed now */
static int part_box(const mech3d_part *p, float mn[3], float mx[3])
{
    int v, j;
    for (j = 0; j < 3; j++) { mn[j] = 1e30f; mx[j] = -1e30f; }
    if (p->model.object_count < 1 || p->model.objects[0].vert_count < 1) return -1;
    for (v = 0; v < p->model.objects[0].vert_count; v++) {
        float q[3];
        part_vertex(p, &p->model.objects[0].verts[v], q);
        for (j = 0; j < 3; j++) { if (q[j] < mn[j]) mn[j] = q[j]; if (q[j] > mx[j]) mx[j] = q[j]; }
    }
    return 0;
}
/* a thing's bounding sphere (its collision object's +0x34..+0x40, 0x10029890: the vertex box's centre, the farthest
 * vertex), as the target bracket draws it (0x10022ea0); 0 when gone */
int msim_thing_sphere(const msim *s, int b, float c[3], float *r)
{
    const mech3d_part *pp;
    float mn[3], mx[3], best = 0;
    int v;
    if (!thing_point(s, b, c)) return 0;
    pp = &s->world->parts[s->bld[b].intact];
    if (part_box(pp, mn, mx) != 0) return 0;
    for (v = 0; v < 3; v++) c[v] = (mn[v] + mx[v]) * 0.5f;
    for (v = 0; v < pp->model.objects[0].vert_count; v++) {
        float q[3], d;
        part_vertex(pp, &pp->model.objects[0].verts[v], q);
        d = (q[0] - c[0]) * (q[0] - c[0]) + (q[1] - c[1]) * (q[1] - c[1]) + (q[2] - c[2]) * (q[2] - c[2]);
        if (d > best) best = d;
    }
    *r = sqrtf(best);
    return 1;
}

/* path tasks (world3d_paths): move the carried world parts; a carried structure's box follows it */
static void mov_refresh(msim *s);
static void paths_step(msim *s)
{
    int b;
    if (world3d_paths_step(s->paths, (mech3d *)s->world, (float)((double)s->now * 182.0 / 1000.0),
                           msim_dos_int ? &s->dos_anim_phase : NULL) > 0)
        s->world_dirty = s->world_rebuild = 1;   /* parts whose paths ended: back in the static mesh and collision */
    for (b = 0; b < s->bld_count; b++) {
        struct msim_building *g = &s->bld[b];
        if (g->hp > 0 && g->intact >= 0 && g->intact < s->world->part_count && s->world->parts[g->intact].moving)
            part_box(&s->world->parts[g->intact], g->mn, g->mx);
    }
    mov_refresh(s);
}

/* 0x100174f0(c, 0) -> 0x10016280 with the unit itself as the killer (+0x114 = its own id): the ordinary death (state 4,
 * the death sequence: wrecks), the destroyed counters (MW2CAR.CFG, as for a kill: units destroyed, a wingman lost),
 * no kill for the player */
static void self_destruct(msim *s, int i)
{
    combat_unit *u = &s->units[i];
    char line[128];
    if (u->destroyed) return;
    u->destroyed = 1;
    {
        static const int KILLED[3] = {0x1e, 0x20, 0x22};
        int side = s->actors[i].friendly ? 2 : s->actors[i].alliance == 2 ? 1 : 0;
        car_inc(s, KILLED[side]);
        if (side == 2) car_inc(s, 0x34);
    }
    if (s->log) {
        snprintf(line, sizeof line, "%6.1fs %s (%s) self-destructs - DESTROYED", (double)s->now / 1000.0, s->actors[i].name, s->actors[i].group);
        s->log(s->log_user, line);
    }
}

/* The messages a unit's state change posts (0x10031870, message table 0x1024f140):
 *  - a death (0x10016280, run once per unit for any cause - weapons, area damage, collisions, ammo, heat, self-destruct,
 *    ejection - and whoever the killer, the player, a starmate or nobody): a unit of the enemy alliance (its group's +0,
 *    0x10019040 == 1) by its class (unit +0, jump table 0x100165d8): 1 mech -> 0x15 "Enemy mech destroyed" (voice 70),
 *    2 hovercraft / 4 tank -> 0x16 "Enemy vehicle destroyed" (71), 3 turret -> 0x19 "Enemy target destroyed" (73),
 *    5 aircraft / 8 dropship -> 0x17 "Enemy aircraft destroyed" (72), 6 / 7 (doors) nothing; and a starmate (its group
 *    led by the player, 0x10018f90) -> 0x10031810(6, its point): "Point n" "destroyed" */
static void unit_messages(msim *s)
{
    int i;
    if (!s->msg_seen || !s->armed) return;
    for (i = 0; i < s->actor_count; i++) {
        const world_actor *ac = &s->actors[i];
        const combat_unit *u = &s->units[i];
        int enemy = (ac->friendly ? 0 : ac->alliance) == 1;
        if (!s->armed[i] || (s->msg_seen[i] & 1)) continue;
        if (u->destroyed) {
            static const char *const KILL[9] = {NULL, "Enemy mech destroyed", "Enemy vehicle destroyed", "Enemy target destroyed",
                                                "Enemy vehicle destroyed", "Enemy aircraft destroyed", NULL, NULL, "Enemy aircraft destroyed"};
            static const int KILL_VOICE[9] = {0, 70, 71, 73, 71, 72, 0, 0, 72};
            int c = ac->unit_class;
            s->msg_seen[i] |= 1;
            if (enemy && c >= 0 && c < 9 && KILL[c] && s->on_message) s->on_message(s->message_user, KILL[c], KILL_VOICE[c]);
            if (ac->friendly && ac->table == 0) msim_order_message(s, star_point(s, i), 6);   /* "Point n destroyed" */
        }
    }
}

static void collision_lods(msim *s);
static void dos_sync_hull_order(msim *s);
static void dos_prepare_scenery(msim *s, const msim_camera *camera);
static int test_floor(const msim *s, float x, float z, float *lo, float *hi);
static void mission_status_step(msim *s, const mtbl_world *w)
{
    unit_messages(s);   /* deaths so far: announced before the mission tables look at them (0x10016280 posts at once) */
    mtbl_logic_step(&s->logic, s->now, w);
    /* mission end (engine 0x10009f50 for the player's star; 0x100374e0 end sequence: 0x71c ticks) */
    if (!s->ending) {
        if (s->logic.result[0]) s->outcome = s->logic.result[0];
        if (s->debug_invulnerable && s->player_unit.destroyed) s->player_unit.destroyed = 0;
        if (s->player_unit.destroyed && !s->player_counted) {   /* 0x10016280: the player's group, the player included */
            s->player_counted = 1;
            car_inc(s, s->player_unit.ejected == 1 ? 0x36 : 0x34);
        }
        if (s->player_unit.intact_out && !s->eject_at) s->eject_at = s->now > 0 ? s->now : 1;
        /* engine 0x1000a9b0: the mission's end waits until the radio queue is empty (DAT_1024f6bc), so the last lines
         * play out first; a destroyed player ends at once (0x10019131); an ejected one (state 5) 0x389 ticks later
         * (0x1001adf0 - DOSBox: "Press CTRL-Q to exit..." 5-6 s after Ctrl+Alt+e) */
        if (s->eject_at && s->now - s->eject_at < 0x389 * 1000 / 182) { /* the ejection camera runs */ }
        else if ((s->logic.result[0] && !s->radio_busy && !s->hang_around) || s->player_unit.destroyed) {
            char line[96];
            /* DOS MW2.EXE 0x16e80 (the star results): with the end flag set (DAT_0009620c: the player destroyed, state 4,
             * at once; ejected, state 5, 0x38e ticks on) the player's star still unresolved fails - "Mission failed" and
             * the table's failure line (0x164f0), DOSBox: shown 0.5 s after Ctrl+Alt+x. (The 3Dfx DLL's 0x1000a9b0 lacks
             * the branch; kept per the user's report - docs/reference/dos_selfdestruct_mission_failed.png) */
            if (s->player_unit.destroyed && !s->logic.result[0]) {
                s->logic.result[0] = 3;
                s->logic.result_time[0] = (int32_t)s->now;
                s->outcome = 3;
            }
            s->ending = 1;
            s->player_lost = s->player_unit.destroyed;
            s->end_flush_at = s->now + 0x108 * 1000 / 182;   /* 0x100374e0: radio flushed 264 ticks in */
            s->end_at = s->now + 0x71c * 1000 / 182;         /* mission over 1820 ticks in */
            snprintf(line, sizeof line, "%6.1fs %s", s->now / 1000.0, s->outcome ? msim_outcome_text(s) : "player destroyed: mission over");
            if (s->log) s->log(s->log_user, line);
            /* the star's outcome line (0x10009f50): MTBL header +0x12 on success, +0x1d on failure, BET68 for time
             * exceeded - queued at priority 0x50, so it survives the flush */
            if (s->outcome && s->on_radio) {
                const char *o = s->outcome == 2 ? s->logic.tables[0].win_sound : s->outcome == 3 ? s->logic.tables[0].lose_sound : "BET68";
                if (o[0]) s->on_radio(s->shot_user, o);
            }
            /* "Press CTRL-Q to exit..." on the message bar: 0x1536 ticks at priority 100 (0x10002f20) */
            if (s->dos_end) s->dos_resolved_s = (s->logic.result_time[0] > 0 ? s->logic.result_time[0] : s->now) / 1000;   /* star +8 */
            else if (s->on_message) s->on_message(s->message_user, "Press CTRL-Q to exit...", -0x1536);
        }
    } else if (!s->over && s->dos_end) {
        /* DOS 0x16fbe..0x1703b: with no radio line playing (DAT_00097ddc), past 3 s the prompt (0x11020: 0x1554 ticks,
         * priority 100) once; past 0x14 s the loop's end (DAT_00095864 += 2) */
        int el = (int)(s->now / 1000) - (int)s->dos_resolved_s;
        if (!s->radio_busy) {
            if (!s->dos_prompted && el > 3) {
                s->dos_prompted = 1;
                if (s->on_message) s->on_message(s->message_user, "Press any key to exit...", -0x1554);
            } else if (el > 0x14) {
                s->over = 1;
                s->ended_at = s->now;
            }
        }
    } else if (!s->over && s->now >= s->end_at) {
        s->over = 1;
        s->ended_at = s->now;
    }
}

static void actor_wreck_move(msim *s, int i, float dt, int dos_ticks)
{
    world_actor *ac = &s->actors[i];
    ai_mind *m = &s->minds[i];
    if (s->armed[i] && s->units[i].destroyed && !fixed_unit(ac)) {
        float h = ac->mech.heading * 3.14159265f / 180.0f;
        float across = s->across ? s->across[i] : 0.0f;
        msim_wreck_step(s, i, ac->mech.origin, sinf(h) * m->speed + cosf(h) * across,
                        cosf(h) * m->speed - sinf(h) * across, dt, dos_ticks);
        if (s->units[i].wreck_stopped) {
            m->speed = 0;
            if (s->air_speed) s->air_speed[i] = 0;
            if (s->across) s->across[i] = 0;
        }
        ac->mech.origin[1] = (int32_t)msim_draw_y(s, i);
    }
}

static void actor_move(msim *s, int i, float dt, int dos_ticks, int starting, int jets_installed)
{
    world_actor *ac = &s->actors[i];
    ai_mind *m = &s->minds[i];
    {
        float lim = s->twist_limit ? s->twist_limit[i] : 90.0f, tw = m->twist_demand;
        if (lim < 360.0f) { if (tw > lim) tw = lim; if (tw < -lim) tw = -lim; }
        if (msim_dos_int) {
            /* DOS 0x26733 / 0x26772: mech twist and pitch take 109 and 36 ticks; turret servos both take
             * 145 ticks (0x2eb1f / 0x2eb38). The original steps each over the whole ticks since its last update. */
            int turret = ac->unit_class == 3;
            int twist_tau = turret ? 145 : 109;
            /* The offline controller clears both servo demands (0x280af, 0x2ee36). */
            int offline = starting || s->units[i].shutdown;
            float pitch = offline ? 0.0f : m->pitch_demand;
            if (offline) tw = 0.0f;
            m->twist = lim >= 360.0f ? msim_twist_round(m->twist, tw, twist_tau, dos_ticks)
                                     : msim_dos_aim_servo(m->twist, tw, twist_tau, dos_ticks);
            m->pitch = msim_dos_aim_servo(m->pitch, pitch, turret ? 145 : 36, dos_ticks);
        } else {
            float ticks = dt * 182.0f;
            m->twist = drive_servo(m->twist, tw, 108.6f, ticks);
            m->pitch = drive_servo(m->pitch, m->pitch_demand, 36.2f, ticks);
        }
        ac->mech.twist = m->twist;   /* drawn, and the weapon mounts follow it */
        /* the pitch object (the record's second THNG node: arms, a turret's guns) turned to the aim's pitch
         * (0x10019310: -asin of the aim ray's rise from it, less the torso's pitch - left at 0 here, the torso's
         * own pitch taken for the player only) */
        ac->mech.pitch = -m->pitch;
    }
    {
        float kmh, rate, h, spd, top = ac->speed * s->units[i].speed_gain;
        /* the throttle -> speed chain (msim_drive_step: servos +0x44 / +0x24 and the velocity approach, 0x1001a180 /
         * 0x100190d0), the player's rule; reverse (input +0x2f, manoeuvres 8 / 10) only asks the gait code for the
         * reverse sequence (+0x88 = 2, 0x1000b3a0) and the drive turns x -0.5 once that sequence PLAYS (+0x84 == 2,
         * 0x1001a90a), as the player's (glview pl_move): from a run the stride goes run -> walk -> reverse first.
         * Class 4 units take the asked sequence at once (0x1000b3a0: object +0 == 4 -> +0x84 = +0x88); other
         * units without a walk animation never play one (+0x84 stays -1, 0x1000b5a0) - no reverse drive. The
         * throttle itself stays positive. AI mechs get no 12.5% turning creep (that is for object +0x10 != 2, the
         * player); 0x1001f910's 10% is set by ai.c */
        /* the moves: in contact with a mech or a wall in 1/30 s steps, the remainder carried, until MSIM_CONTACT_HOLD
         * steps in a row have touched nothing - the engine's contact responses are per frame and frame-rate dependent
         * (0x1000ba20, 0x1000b5e0), the port takes the 30 fps reference (as the player's, glview pl_locomote); out of
         * contact the frame is one move */
        float rest = dt, sdt;
        int steep_contact = 0;
        /* the frame's vertical move goes into the move test with the horizontal one (engine 0x10019310 -> 0x1000b5e0:
         * one 3D segment; walking it is 0): combat_jets makes it below unless a move was refused - then the 3D
         * impact's vertical move and reflected vertical velocity stand (dy_done, vy3) */
        /* the jets firing (input +0x1d, fuel c[0x30] > 0, state 2: 0x10019310): with the legs turning (input +0xc not 0)
         * or a directional jet held (+0x1e / +0x1f / +0x20 / +0x21) no lift - the vertical acceleration 0 (rising: -g),
         * 0x100196cc..0x10019711; else lift (combat_jets). The flag is combat_unit.jet_forward (combat_jets_dy /
         * combat_jets) */
        int firing = m->jet && s->units[i].jets > 0 && s->units[i].jet_fuel > 0 && !s->units[i].no_jump && !s->units[i].shutdown;
        int jdir = m->jet_side < 0 ? 1 : m->jet_side > 0 ? 2 : m->jet_forward ? 3 : m->jet_back ? 4 : 0;   /* the first of
                                                                               * left, right, forward, back */
        if (!firing || m->turn != 0.0f) jdir = 0;
        s->units[i].jet_forward = firing && (m->turn != 0.0f || jdir);
        combat_motion lift = {0};
        if (msim_dos_int && !fixed_unit(ac)) lift = msim_dos_lift(&s->units[i], &s->planet, firing, dos_ticks, 0);
        float dy_frame = msim_dos_int ? lift.dy : fixed_unit(ac) ? 0.0f : combat_jets_dy(&s->units[i], m->jet, dt);
        float dy_done = 0.0f, vy3 = 0.0f;
        int imp3 = 0, land3 = 0;
        int cm = !msim_dos_int && s->contact_acc && s->contact_hold && s->contact_hold[i] > 0;
        if (cm) s->contact_acc[i] += dt;
        else if (s->contact_acc) s->contact_acc[i] = 0;
        for (;;) {
            int touched = 0;
            if (cm) {
                if (s->contact_acc[i] < 1.0f / 30.0f - 1e-5f) break;
                s->contact_acc[i] -= 1.0f / 30.0f;
                sdt = 1.0f / 30.0f;
            } else sdt = rest;
            /* ONE velocity (engine 0x10019310, +0xf4 / +0xfc), kept here as m->speed along the heading and across[i]
             * across it (+ right): on the ground with the jets not firing it approaches heading x c[0xb] (m->speed by
             * msim_drive_step, across at 1/45 per tick to 0); airborne or with the jets firing it is HELD - the servos
             * run, a turn does not swing it - and only the directional jets' thrust and drag act, on all of it (the
             * player's rule, glview: "Jet terminal speed", "Airborne velocity held"). The port had restarted every
             * grounded frame from the walking speed, so an AI unit's ground thrust never built up */
            int air = s->units[i].y > s->units[i].ground + (msim_dos_int ? 0.0f : 1.0f), freev = air || firing;
            float v0 = m->speed, a0 = s->across ? s->across[i] : 0.0f, hd0 = ac->mech.heading;
            int32_t dos_move[2] = {0};
            int revp = ac->unit_class == 4 ? m->reverse : (s->anim_pl && actor_has_animation(ac) && s->anim_pl[i].cur == 2);
            float throttle = msim_dos_int ? m->throttle * s->units[i].speed_gain : m->throttle;
            float dos_steer_speed = m->spd_servo;   /* 0x27ce3 reads speed before 0x26a51 steps it. */
            msim_drive_step(&m->thr_servo, &m->spd_servo, &m->speed, throttle, revp ? -0.5f : 1.0f,
                            msim_dos_int ? ac->speed : top,
                            !(s->units[i].shutdown || s->units[i].immobile), m->drive_slow,
                            msim_dos_int ? (float)dos_ticks : sdt * 182.0f);
            if (freev) m->speed = v0;
            kmh = fabsf(m->spd_servo) * 0.036f;
            rate = m->turn / 1024.0f * 90.0f * cosf((kmh < 80.0f ? kmh : 80.0f) * 3.14159265f / 180.0f);   /* by c[0xb] x 6.516 (0x1001a180) */
            if (m->jet && s->units[i].jets > 0 && !s->units[i].no_jump && !s->units[i].shutdown && s->units[i].jet_fuel >= 0)
                rate = combat_jets_firing(&s->units[i], m->jet, sdt) ? m->turn / 1024.0f * 90.0f * 2.0f : 0.0f;   /* the jets held (a
                               * tank, state 2): x2 (0x102473d4) while fuel is left after the burn, else no turn (0x1001a180) */
            if (s->units[i].immobile && !(m->jet && s->units[i].jets > 0 && !s->units[i].no_jump && !s->units[i].shutdown && s->units[i].jet_fuel >= 0)) rate = 0.0f;   /* a leg gone: no leg turn (user report against the
                               * original: a legged mech cannot come about - circle behind it; it still twists its torso) */
            if (msim_dos_int) {
                /* DOS MW2.EXE 0x27ce3..0x27d5a: kv = c[0xb] (16.16 cm/tick, +0x2c) / 10000 by idiv (toward zero, ~km/h);
                 * a computer-run unit at kv 0 does not turn on the spot; airborne (object +0x80 bit 4, the feet more
                 * than the centre height over the ground) no leg turn unless the jets are held (above). The heading then
                 * turns by the leg turn SERVO's value (0x27561: object +0x60 += +0x3c x ticks / 182), servo +0x34 easing
                 * toward the demand with T = chop(0.3 x 182) = 54 ticks (0x2675c): a turn builds up and runs on ~0.3 s.
                 * Found by the GBL port's emulated comparison (GBL 0x28054 / 0x278e1; MW2.EXE the same code) */
                int jets_held = m->jet && s->units[i].jets > 0 && !s->units[i].no_jump && !s->units[i].shutdown && s->units[i].jet_fuel >= 0;
                if (!jets_held) {
                    int airborne = s->units[i].y - s->units[i].ground > (s->centre_h ? s->centre_h[i] : 550.0f);
                    rate = msim_leg_demand(starting ? 0.0f : m->turn, dos_steer_speed, 0,
                                           s->units[i].speed_gain, airborne, 0, 0.0f);
                    if (s->units[i].immobile) rate = 0.0f;
                }
                msim_dos_walk(&s->units[i], hd0, m->spd_servo, ac->speed, freev, jdir, dos_ticks, dos_move);
                msim_dos_leg_step(&m->leg_srv, rate, &ac->mech.heading, dos_ticks);
                msim_dos_project_velocity(&s->units[i], ac->mech.heading, &m->speed, s->across ? &s->across[i] : NULL);
            } else ac->mech.heading = wrap180(ac->mech.heading + rate * sdt);
            if (!msim_dos_int && freev && s->across && ac->mech.heading != hd0) {   /* held: the world vector stays as the heading turns */
                float d = (ac->mech.heading - hd0) * 3.14159265f / 180.0f, cd = cosf(d), sd = sinf(d);
                m->speed = v0 * cd + a0 * sd; s->across[i] = a0 * cd - v0 * sd;
            }
            s->units[i].throttle_frac = fabsf(m->thr_servo) > 1.0f ? 1.0f : fabsf(m->thr_servo);   /* walking heat: c[0x13] - 1/64 (0x1001aac8) */
            if (!msim_dos_int) {   /* the directional jets (0x10019310 / DOS 0x26ba2): v += ticks x (T dir + drag(v)), the drag f = k S + B of
                 * the whole velocity at the start of the step (msim_jet_drag, S the DOS (4 max + min) >> 2 of its x / z):
                 * terminal L = 2.5 x the top speed; world components under 0.0625 cm/tick snap to 0 */
                float dv = 0;
                combat_jets_forward(&s->units[i], jdir != 0, sdt, &dv);
                s->units[i].jet_forward = firing && (m->turn != 0.0f || jdir);   /* the no-lift flag (above) */
                if (dv > 0) {
                    float hh = ac->mech.heading * 3.14159265f / 180.0f, sh = sinf(hh), ch = cosf(hh), ticks = sdt * 182.0f;
                    float ac0 = s->across ? s->across[i] : 0.0f;
                    float vx = (sh * m->speed + ch * ac0) / 182.0f, vz = (ch * m->speed - sh * ac0) / 182.0f;   /* cm/tick */
                    float dx = jdir == 3 ? sh : jdir == 4 ? -sh : jdir == 2 ? ch : -ch;
                    float dz = jdir == 3 ? ch : jdir == 4 ? -ch : jdir == 2 ? -sh : sh;
                    float ax = fabsf(vx), az = fabsf(vz), S = ax > az ? ax + az * 0.25f : az + ax * 0.25f, kq, B, nvx, nvz;
                    msim_jet_drag(s->units[i].jet_ddy, ac->speed, &kq, &B);
                    nvx = vx + dx * dv; nvz = vz + dz * dv;
                    if (S > 1e-7f && (kq != 0 || B != 0)) { float f = kq * S + B; nvx -= vx * f * ticks; nvz -= vz * f * ticks; }
                    if (fabsf(nvx) < 0.0625f) nvx = 0;
                    if (fabsf(nvz) < 0.0625f) nvz = 0;
                    m->speed = (sh * nvx + ch * nvz) * 182.0f;
                    if (s->across) s->across[i] = (ch * nvx - sh * nvz) * 182.0f;
                }
            }
            spd = m->speed;
            if (s->air_speed) s->air_speed[i] = spd;
            h = ac->mech.heading * 3.14159265f / 180.0f;
            {
                float acr = 0.0f, nx, nz;
                if (msim_dos_int) {
                    acr = s->across ? s->across[i] : 0.0f;
                    nx = (float)ac->mech.origin[0] + (float)dos_move[0];
                    nz = (float)ac->mech.origin[2] + (float)dos_move[1];
                } else {
                    if (s->across && s->across[i] != 0.0f) {   /* the velocity across the heading (a contact / impact, the side
                                                                 * jets): x (1 - ticks / 45) on the ground with the jets not
                                                                 * firing, held otherwise; zero under 0.0625 cm/tick (0x10019310,
                                                                 * 0x102473b4 / 0x102473d0), the move by the mean */
                        float kq = sdt * 182.0f / 45.0f, a1 = s->across[i];
                        if (!freev) s->across[i] *= kq < 1.0f ? 1.0f - kq : 0.0f;
                        acr = 0.5f * (a1 + s->across[i]);
                        if (fabsf(s->across[i]) < 0.0625f * 182.0f) s->across[i] = 0;
                    }
                    nx = (float)ac->mech.origin[0] + (sinf(h) * spd + cosf(h) * acr) * sdt;
                    nz = (float)ac->mech.origin[2] + (cosf(h) * spd - sinf(h) * acr) * sdt;
                }
                if (!msim_dos_int && s->contact_frac) {   /* the sub-cm rest of the last contact projection */
                    nx += s->contact_frac[i][0]; nz += s->contact_frac[i][1];
                    s->contact_frac[i][0] = s->contact_frac[i][1] = 0;
                }
                {   /* mech-mech contact, in 3D (engine 0x1000b5e0 -> 0x1000ba20 spheres, 0x10019310): the unit touched, the
                     * normal from its centre to this one's, the relative speed. On top of it (normal y above 0.7071) with
                     * the feet within 10 m of the terrain: landed - no damage to either, the mover stood on the terrain at its
                     * projection onto the sphere and pushed out of the sphere by the next frame's move; else this one
                     * takes collision damage by the normal (0x1000c160 / 0x1000c1f0: legs from below, head from above x
                     * the tonnage ratio, else arm / torso) on every contact tick - only the mover: the other takes damage
                     * only from its own movement's contacts (0x10019d77) - and a clang once per contact (on_sound -1). Walking or airborne, the move's end is projected onto the r1 + r2 sphere and
                     * it bounces off along the normal at half the relative speed (0x1000ba20 +0xf4 = n x max(1, dv / 2)) -
                     * pressing on, it slides round the other (the port had pushed a walker back 60 cm at -30 % speed). */
                    float x0 = (float)ac->mech.origin[0], z0 = (float)ac->mech.origin[2], ticks = sdt * 182.0f;
                    float ch = s->centre_h ? s->centre_h[i] : 550.0f, air = s->units[i].y > s->units[i].ground + 1.0f;
                    float vy = air ? s->units[i].vy : 0.0f;
                    float p0[3] = {x0, s->units[i].y + ch, z0};
                    float p1[3] = {nx, s->units[i].y + ch + (msim_dos_int ? dy_frame : vy * ticks), nz};
                    float n[3], c[3], rr = 0, ov[3], sv[3] = {sinf(h) * spd / 182.0f, vy, cosf(h) * spd / 182.0f};
                    if (msim_dos_int) {
                        sv[0] = s->units[i].dos_vel[0] / 65536.0f;
                        sv[1] = lift.vy;
                        sv[2] = s->units[i].dos_vel[1] / 65536.0f;
                        memcpy(s->units[i].dos_attempted, sv, sizeof sv);
                        s->units[i].dos_contact_unit = 0;
                    }
                    int hit = msim_unit_contact(s, i, p0, p1, n, c, &rr, ov), pushout = 0;
                    /* set down inside the sphere of the unit it landed on (bump_with < 0): this frame's move ends inside it
                     * whatever its direction - the engine's push-out (0x1000ba20 tests only that the end is inside) */
                    if (hit == -2 && s->bump_with && s->bump_with[i] < 0 && msim_unit_inside(s, i, -s->bump_with[i] - 2, p1, n, c, &rr, ov)) {
                        hit = -s->bump_with[i] - 2; pushout = 1;
                    }
                    if (hit != -2) {
                        /* the second try (0x10019a61, DOS 0x27109): the move to the end projected onto the sphere, tested
                         * again - refused by rounding (msim_contact_retest), the velocity is doubled (0x102473d4, DOS
                         * 0x2713b) and the projected end kept (the same object: a unit's id output is 0 both times) */
                        const float vx2 = !msim_dos_int && msim_contact_retest(p0, p1, c, rr) ? 2.0f : 1.0f;
                        touched = 1;
                        /* the engine's speeds (0x1000ba20): it copies the other's z velocity over its own (+0x108) before
                         * 0x1000c160 takes the difference, so the damage speed is |(dvx, dvy)|; the bounce is half of
                         * |(dvx, dvy, the other's vz)|, at least 1 cm/tick */
                        float dvx = sv[0] - ov[0], dvy = sv[1] - ov[1], dv = msim_dos_int ? oct_len(dvx, dvy, 0.0f) : sqrtf(dvx * dvx + dvy * dvy);   /* DOS 0x20890: the DOS length */
                        float bs = 0.5f * sqrtf(dvx * dvx + dvy * dvy + ov[2] * ov[2]);
                        float py = c[1] + n[1] * rr - ch, hl = sqrtf(n[0] * n[0] + n[2] * n[2]);   /* projected onto its sphere */
                        float gnd = msim_ground(s, c[0] + n[0] * rr, c[2] + n[2] * rr, py + ch);   /* 0x10010080 at the centre */
                        if (mw2_development_env("MW2_CONTACT_TRACE")) fprintf(stderr, "contact %.2fs actor %d -> %d n %.2f %.2f %.2f dv %.2f feet %.0f above %.0f air %d\n", s->now / 1000.0, i, hit, n[0], n[1], n[2], dv, py, py - gnd, (int)air);
                        if (msim_dos_int) {
                            float end[3], bounce[3];
                            int retry_hit = msim_dos_unit_impact(s, i, p0, ch, s->radius[i], hit, c, n, rr, sv, ov, end, bounce);
                            ac->mech.origin[0] = (int32_t)end[0];
                            ac->mech.origin[2] = (int32_t)end[2];
                            dy_done = end[1] - p0[1];
                            vy3 = bounce[1];
                            imp3 = 1;
                            land3 = !retry_hit && n[1] > 0.70710677f && py - gnd < 1000.0f && py - gnd > -10000.0f;
                            s->units[i].dos_vel[0] = (int32_t)lrintf(bounce[0] * 65536.0f);
                            s->units[i].dos_vel[1] = (int32_t)lrintf(bounce[2] * 65536.0f);
                            msim_dos_project_velocity(&s->units[i], ac->mech.heading, &m->speed, s->across ? &s->across[i] : NULL);
                            if (s->air_speed) s->air_speed[i] = m->speed;
                        }
                        if (land3 || (!msim_dos_int && n[1] > 0.70710677f && py - gnd < 1000.0f && py - gnd > -10000.0f)) {   /* landed on it (0x10019c4e) */
                            /* the engine: the position projected onto the sphere (0x1000ba20), the feet taken as 1 cm under
                             * the terrain there (local_5c = -1) - so it stands on the terrain at that point, inside the
                             * sphere - vy 0, the velocity n x max(1, dv / 2) across the ground, the contact count cleared;
                             * the next frame's move pushes it out (above) */
                            if (msim_dos_int) { if (s->bump_with) s->bump_with[i] = -(hit + 2); break; }
                            float gx = c[0] + n[0] * rr, gz = c[2] + n[2] * rr;
                            if (bs < 1.0f) bs = 1.0f;
                            bs *= vx2;
                            (void)hl;
                            ac->mech.origin[0] = (int32_t)lrintf(gx); ac->mech.origin[2] = (int32_t)lrintf(gz);
                            s->units[i].ground = gnd;
                            s->units[i].y = s->units[i].ground; s->units[i].vy = 0;
                            if (s->air_speed) s->air_speed[i] = 0;
                            m->speed = spd = (n[0] * sinf(h) + n[2] * cosf(h)) * bs * 182.0f;   /* along the heading */
                            if (s->bump_with) s->bump_with[i] = -(hit + 2);
                        } else {
                            if (s->collision_damage)   /* every contact tick, the mover only (0x10019d6a -> 0x1000c160) */
                                combat_collision_unit(&s->units[i], dv, n, ac->mech.heading + m->twist, hit == -1 ? s->player_unit.tons : s->units[hit].tons, &s->rng);
                            if (s->bump_with && s->bump_with[i] != hit + 2) {
                                if (s->on_sound && dv > 0.5f) { float p3[3] = {(x0 + c[0]) * 0.5f, s->units[i].y, (z0 + c[2]) * 0.5f}; s->on_sound(s->shot_user, -1, p3); }
                                s->bump_with[i] = hit + 2;
                            }
                            if (msim_dos_int) break;
                            {   /* bounced off / pushed out: to the sphere's surface, moving away along the normal (walking too) */
                                if (bs < 1.0f) bs = 1.0f;
                                bs *= vx2;
                                float rp = air || pushout ? rr + 1.0f : rr;   /* walking: onto the sphere itself (0x1000ba20) */
                                float gx = c[0] + n[0] * rp, gz = c[2] + n[2] * rp;
                                ac->mech.origin[0] = (int32_t)lrintf(gx); ac->mech.origin[2] = (int32_t)lrintf(gz);
                                if (s->contact_frac) { s->contact_frac[i][0] = gx - (float)ac->mech.origin[0]; s->contact_frac[i][1] = gz - (float)ac->mech.origin[2]; }
                                /* the projection is in 3D (0x1000ba20): walking too the centre goes to the sphere's surface,
                                 * lifted by n.y x the penetration (or lowered: back on the ground), and the velocity n x
                                 * max(1, dv / 2) carries n.y up - a small hop when the other's centre is lower */
                                s->units[i].y = c[1] + n[1] * rp - ch;
                                s->units[i].ground = msim_ground(s, gx, gz, s->units[i].y + ch);
                                if (s->units[i].y < s->units[i].ground) s->units[i].y = s->units[i].ground;
                                s->units[i].vy = n[1] * bs;
                                spd = (n[0] * sinf(h) + n[2] * cosf(h)) * bs * 182.0f;   /* along the heading */
                                if (s->air_speed) s->air_speed[i] = spd;
                                if (s->across) s->across[i] = (n[0] * cosf(h) - n[2] * sinf(h)) * bs * 182.0f;   /* and across it */
                                m->speed = spd;   /* the one velocity (msim_step's drive) */
                                if (s->units[i].y <= s->units[i].ground + 1.0f) s->units[i].vy = 0;   /* on the ground */
                            }
                        }
                        nx = (float)ac->mech.origin[0]; nz = (float)ac->mech.origin[2];
                    } else if (s->bump_with) s->bump_with[i] = 0;
                }
                float dy_sub = imp3 ? vy3 * sdt * 182.0f : dt > 0.0f ? dy_frame * sdt / dt : 0.0f;   /* this move's share */
                if (msim_can_move(s, (float)ac->mech.origin[0], (float)ac->mech.origin[2], nx, nz, s->units[i].y + dy_done, dy_sub, s->centre_h[i], s->radius[i])) {
                    /* airborne too, in 3D (0x10019310: every move) */
                    dy_done += dy_sub;
                    /* the contact count (controller +0xa4, 0x1000b5e0) clears on a free move; a unit that does not move
                     * keeps it (and counts on) */
                    if ((int32_t)lrintf(nx) != ac->mech.origin[0] || (int32_t)lrintf(nz) != ac->mech.origin[2]) s->units[i].blocked = 0;
                    ac->mech.origin[0] = (int32_t)lrintf(nx);
                    ac->mech.origin[2] = (int32_t)lrintf(nz);
                } else {   /* walked into the world (engine 0x10019310 -> 0x1000b5e0): the move cut back to the touching
                            * point and the velocity reflected and cut to a quarter (msim_world_impact) - so pressing on, the
                            * speed stays under the damage threshold; unless harmless (a climbable face, the feet in the
                            * ground window: 0x10019c4e), collision damage at the attempted speed on every contact frame
                            * (0x10019d84 -> 0x1000c3c0 / 0x1000c1f0) and the object walked into takes its share */
                    float wn[3], bx = nx, bz = nz, sp3 = sqrtf(spd * spd + acr * acr) / 182.0f;
                    if (msim_dos_int) {   /* DOS MW2.EXE 0x20a50: the DOS length (4 max + mid + min) / 4 of the velocity (world x / y / z) */
                        float hh3 = ac->mech.heading * 3.14159265f / 180.0f;
                        sp3 = oct_len((sinf(hh3) * spd + cosf(hh3) * acr) / 182.0f, s->units[i].vy, (cosf(hh3) * spd - sinf(hh3) * acr) / 182.0f);
                    }
                    float sacr = s->across ? s->across[i] : 0.0f;
                    /* the attempted velocity is the frame's end one (0x10019310: v + a t before the move) */
                    float dyi = dy_sub, vyi = msim_dos_int ? lift.vy :
                        dy_sub == 0.0f ? 0.0f : imp3 ? vy3 : dt > 0.0f ? 2.0f * dy_frame / (dt * 182.0f) - s->units[i].vy : s->units[i].vy;
                    int imp = msim_world_impact3(s, (float)ac->mech.origin[0], (float)ac->mech.origin[2], &bx, &bz, s->units[i].y + dy_done, &dyi, s->centre_h[i], s->radius[i],
                                                 ac->mech.heading, &m->speed, &sacr, &vyi, s->units[i].dos_vel);
                    if (msim_dos_int || dy_sub != 0.0f) { imp3 = 1; vy3 = vyi; land3 = imp & 2; }   /* an airborne move: the 3D impact's (walking stays level) */
                    if (s->air_speed) s->air_speed[i] = m->speed;   /* one velocity (+0xf4..0xfc): the airborne momentum is the reflected one too */
                    dy_done += dyi;
                    if (s->across) s->across[i] = sacr;
                    wn[0] = g_block_n[0]; wn[1] = g_block_n[1]; wn[2] = g_block_n[2];   /* the hit's normal (the second try's when refused) */
                    ac->mech.origin[0] = (int32_t)lrintf(bx); ac->mech.origin[2] = (int32_t)lrintf(bz);
                    if (s->contact_frac) { s->contact_frac[i][0] = bx - (float)ac->mech.origin[0]; s->contact_frac[i][1] = bz - (float)ac->mech.origin[2]; }   /* a wall slide's sub-cm moves */
                    /* the contact the AI reads (+0xa4, 0x10020e00) counts whatever the Collision Damage setting: 0x1000b5e0
                     * adds one for every refused move (0x10010530 hit), a mech or a sphere in the way; a harmless one
                     * clears it */
                    s->units[i].blocked = !(imp & 2);
                    if (!(imp & 2)) {
                        if (ac->unit_class == 1 && wn[1] < 0.70710677f &&
                            (msim_dos_int ? wn[1] > 0.0f : m->state == AI_GODIRECT && wn[1] >= 0.0f)) {
                            steep_contact = 1;
                            m->terrain_normal[0] = wn[0];
                            m->terrain_normal[1] = wn[2];
                        }
                        if (s->collision_damage)   /* Combat Variables: a global switch in the engine */
                            combat_collision(&s->units[i], sp3, wn[1], atan2f(-wn[0], -wn[2]) * 57.29578f - (ac->mech.heading + m->twist), &s->rng);   /* off heading + twist (DOS 0x209ab..0x209b9: object +0x60 + +0x6c), as the player's */
                        msim_impact_damage(s, i, sp3);
                    }
                touched = 1;
                    if (mw2_development_env("MW2_BLOCK_TRACE") && atoi(mw2_development_env("MW2_BLOCK_TRACE")) == i)   /* TEST ONLY */
                        fprintf(stderr, "block %.2fs actor %d at %d,%d by %s n %.2f %.2f %.2f\n", s->now / 1000.0, i, ac->mech.origin[0], ac->mech.origin[2],
                                g_block_what ? g_block_what : "terrain", wn[0], wn[1], wn[2]);
                }
            }
            if (touched && s->contact_hold) s->contact_hold[i] = MSIM_CONTACT_HOLD;
            if (!cm) break;
            if (!touched && --s->contact_hold[i] <= 0) {   /* out of contact: the rest of the frame as one move */
                cm = 0; rest = s->contact_acc[i]; s->contact_acc[i] = 0;
                if (rest <= 1e-5f) break;
            }
        }
        if (ac->unit_class == 1) {
            /* A sliding or bouncing contact can have clear frames between impacts.
             * In 3D, short clear moves between wall bounces decay the stall more slowly. */
            int decay = msim_dos_int ? dos_ticks : (dos_ticks + 3) / 4;
            int ticks = m->terrain_block_ticks + (steep_contact && fabsf(m->speed) < 200.0f ? dos_ticks : -decay);
            m->terrain_block_ticks = ticks < 0 ? 0 : ticks > 182 ? 182 : ticks;
        }
        if (fixed_unit(ac)) s->units[i].ground = s->units[i].y;   /* 0x100219e0: no translation, no gravity */
        else {
            float probe_h = s->centre_h ? s->centre_h[i] : 550.0f;
            s->units[i].ground = msim_ground(s, (float)ac->mech.origin[0], (float)ac->mech.origin[2],
                                             s->units[i].y + probe_h - high_flight_lift(s, i));
            float y3 = s->units[i].y + dy_done;
            /* falling fast (off a mesa, a cliff): an AI unit with jets fires them to slow the drop before it would land
             * hard enough to hurt its legs (fall damage beyond 16.154 cm/tick, combat_land). User report against DOS,
             * Scorching Sand: the two Jenners that start on the mesa jet as they drop off it (the plumes show) and land
             * unhurt; the port's dropped like stones and both red-legged. INFERRED from that observation (the engine's
             * code not traced): the jets on while over 2 m up and falling faster than 10 cm/tick */
            if ((!msim_dos_int || m->man != 9) && !s->units[i].destroyed && !s->units[i].shutdown &&
                jets_installed && s->units[i].jet_fuel > 0 && s->units[i].y - s->units[i].ground > 200.0f && s->units[i].vy < -10.0f && !mw2_development_env("MW2_NO_FALL_BRAKE")) m->jet = 1;   /* (tests: MW2_NO_FALL_BRAKE) */
            combat_motion impact = {dy_done, vy3, dos_ticks};
            combat_jets_motion(&s->units[i], m->jet, dt, &s->rng, msim_dos_int ? (imp3 ? &impact : &lift) : NULL);
            if (msim_dos_int) {
                if (land3 && s->units[i].y > s->units[i].ground) combat_land(&s->units[i], vy3, &s->rng);
            } else if (imp3 && land3)   /* a harmless impact (a ground-like face, the feet in the window) lands it: 0x10019c4e sets
                                  * the feet's height over the ground to -1, so 0x10019e41 stands it on the ground with the
                                  * touch-down's sound and fall damage at the impact's vertical speed, vy 0 */
                combat_land(&s->units[i], vy3, &s->rng);
            else if (imp3) {   /* refused in 3D: where the impact left it, with the reflected vertical velocity */
                s->units[i].y = y3; s->units[i].vy = vy3; s->units[i].landed_vy = 0;
                if (s->units[i].y <= s->units[i].ground) combat_land(&s->units[i], vy3, &s->rng);
            }
            if (s->units[i].landed_vy < -5.384f && !s->units[i].destroyed && s->on_sound) {   /* the touch-down (0x10019e28 ->
                                                                                                * 0x1001c0a0): MECMTNSF 0xe6 up to 16.154 cm/tick, MECMTNHD 0xe5 faster, at the mech */
                float p3[3] = {(float)ac->mech.origin[0], s->units[i].y, (float)ac->mech.origin[2]};
                s->on_sound(s->shot_user, s->units[i].landed_vy >= -16.154f ? 0xe6 : 0xe5, p3);
            }
        }
        if (s->units[i].jet_on && s->on_effect) msim_jet_flames_at(s, &ac->mech, (float)ac->mech.origin[0], s->units[i].y, (float)ac->mech.origin[2], ac->mech.heading);
        ac->mech.origin[1] = (int32_t)msim_draw_y(s, i);
    }
    if (mw2_development_env("MW2_TEST_LEG") && s->now >= atoi(mw2_development_env("MW2_TEST_LEG")) && s->armed[i] && !s->units[i].loc_gone[7]) {   /* tests: the left leg shot off */
        int q; for (q = 0; q < 80 && !s->units[i].loc_gone[7]; q++) combat_hit(&s->units[i], 11, 8, 180.0f, &s->rng);
    }
}

/* Keep driving along the face after getting free, so the normal controller
 * does not immediately steer back into the same obstruction. */
static void ai_escape_terrain(ai_mind *m, float heading, float goal, int ticks, int dos)
{
    if (m->terrain_escape_ticks <= 0) {
        if (m->terrain_block_ticks < 182 || m->throttle <= 0.0f) return;
        const float downhill = atan2f(m->terrain_normal[0], m->terrain_normal[1]) * 57.29578f;
        const float side = wrap180((dos ? heading : goal) - downhill);
        /* Scripted 3D routes need longer and closer to parallel to pass a wall. */
        const float angle = dos ? 60.0f : 85.0f;
        m->terrain_escape_bearing = downhill + (side < 0.0f ? -angle : angle);
        m->terrain_escape_ticks = (dos ? 3 : 5) * 182;
        m->terrain_block_ticks = 0;
    } else if (dos && m->terrain_block_ticks >= 182 &&
               fabsf(wrap180(m->terrain_escape_bearing - heading)) < 20.0f) {
        const float downhill = atan2f(m->terrain_normal[0], m->terrain_normal[1]) * 57.29578f;
        if (fabsf(wrap180(m->terrain_escape_bearing - downhill)) > 20.0f) {
            /* A shallow escape can stay pinned at the join between slope faces. */
            m->terrain_escape_bearing = downhill;
            m->terrain_escape_ticks = 3 * 182;
            m->terrain_block_ticks = 0;
        }
    }
    const float err = wrap180(m->terrain_escape_bearing - heading);
    m->reverse = 0;
    m->turn = fminf(819.2f, fmaxf(-819.2f, 24.0f * err));
    m->throttle = fabsf(err) > 30.0f ? 0.25f : 0.75f;
    m->terrain_escape_ticks -= ticks;
}

static void actors_animate(msim *s, float dt, int dos_ticks)
{
    /* pose everyone: the gait code (engine 0x1000b3a0) asks for a sequence by the current throttle c[0x13] and the key
     * player (0x1000dec0, anim_player_step) walks the keys toward it - a stopping mech finishes its stride and comes to
     * the standing pose (the old code froze the keys mid-stride whenever the ground speed fell under 1 cm/s) */
    /* DOS animation tasks run newest first. The player gait tasks follow the other units,
     * then scenery tasks consume the next normalization periods. */
    for (int k = 0; k < s->actor_count; k++) {
        int i = !msim_dos_int ? k : s->dos_anim_order ? s->dos_anim_order[s->actor_count - k - 1] : s->actor_count - k - 1;
        world_actor *ac = &s->actors[i];
        /* no walk animation (vehicles, turrets, dropships): the rest pose, placed at the unit's position and heading -
         * the model drawn and hit is where the unit is */
        if (!actor_has_animation(ac)) { mech3d_pose(&ac->mech, NULL, 0); continue; }
        /* destroyed (state 4): the mech stands as it was struck (the engine's state 4 runs no gait; DOSBox shows the
         * self-destructed Mad Dog mid-stride - ASSUMPTIONS "Standing wrecks"); only the dead are held */
        if (s->armed[i] && s->units[i].destroyed) continue;
        int held = 0;
        if (s->anim_pl) {
            /* airborne - the feet more than the centre height c[0x33] over the ground (0x10019310 0x10019df2) - or the
             * jets firing with fuel left (0x1001a180 0x1001a688): object +0x80 bit 0 cleared every frame, so the key
             * player holds its key and pose, drops the frame's time and starts no key (no footstep) until it lands; the
             * gait request runs on (as the player's, tools/glview.c). Was: held whenever 1 cm off the ground (ASSUMED) */
            held = s->units[i].jet_on || s->units[i].y - s->units[i].ground > (s->centre_h ? s->centre_h[i] : 550.0f);
            if (held) anim_player_hold(&s->anim_pl[i]);
            /* 0x1000b3a0: c[0x13] over the stop level (1/64) by more than 1/2048 (1/32 of full: 0x3c840001) walks -
             * gait 1 under 1/4 of full, 2 under 3/4, else 3; sequence 0 (walk) below gait 3, 1 (run) at it, 2 (reverse)
             * while reversing (input +0x2f; the AI's throttle stays positive), the gait then at most 2; at or under it
             * the request is "stop" (-1, gait 0: 0x1000b350) */
            const ai_mind *m = s->minds ? &s->minds[i] : NULL;
            float fr = m ? fabsf(m->thr_servo) : 0.0f;
            int req = -1, gait = 0, ev;
            int rate = ac->mech.anim_rate > 0 ? ac->mech.anim_rate : 70;   /* the TSK key time */
            if (m && fr > 1.0f / 32.0f) {
                gait = fr < 0.25f ? 1 : fr < 0.75f ? 2 : 3;
                req = gait == 3 ? 1 : 0;
                if (s->anim_pl[i].cur == 2 && gait > 2) gait = 2;
                if (m->reverse) req = 2;
            }
            ev = anim_player_step(&ac->anim, &s->anim_pl[i], req, gait, rate,
                                  msim_dos_int ? (float)dos_ticks : dt * 182.0f, msim_dos_int);
            if (msim_dos_int) mech3d_dos_anim_step(&ac->mech, &ac->anim, &s->anim_pl[i], &s->dos_anim_phase);
            if (!msim_dos_int && (ev & ANIM_EV_STEP) && ac->have_anim && !s->units[i].destroyed && s->on_sound) {
                float p3[3] = {(float)ac->mech.origin[0], s->units[i].y, (float)ac->mech.origin[2]};
                s->on_sound(s->shot_user, 0x103, p3);
            }
            /* a key flagged 0x10 (object +0x80 bit 1): the drive servo's T becomes 90.5 ticks for good (0x1001a180) */
            if (!msim_dos_int && (ev & ANIM_EV_SLOW) && m && fabsf(m->speed) > 1.0f) s->minds[i].drive_slow = 1;
            ac->t = s->anim_pl[i].t;
        }
        if (mw2_development_env("MW2_ANIM_TRACE") && i == atoi(mw2_development_env("MW2_ANIM_TRACE")))   /* tests: one actor every step */
            fprintf(stderr, "anim %.2fs actor %d held %d vy %.2f fuel %.0f jet %d y %.0f ground %.0f throttle %.2f key %.2f (seq %d)\n", s->now / 1000.0, i, held, s->units[i].vy, s->units[i].jet_fuel, s->minds ? s->minds[i].jet : -1, s->units[i].y, s->units[i].ground, s->minds ? s->minds[i].thr_servo : 0.0f, ac->t, s->anim_pl ? s->anim_pl[i].cur : -1);
        const anim_player *ap = s->anim_pl && !mw2_development_env("MW2_NO_POSE_OFF") ? &s->anim_pl[i] : NULL;
        ac->mech.pose_off = ap && !msim_dos_int ? ap->off : NULL;
        ac->mech.pose_dos = msim_dos_int ? ap : NULL;
        mech3d_pose(&ac->mech, &ac->anim, ac->t);
    }
}

void msim_step(msim *s, int32_t dt_ms)
{
    if (dt_ms <= 0) return;   /* a sub-millisecond frame: its time is carried to the next one (glview sim_ms) */
    if (s->world_rebuild && s->world) { s->world_rebuild = 0; msim_set_world(s, s->world); }
    mtbl_world w;
    float dt = (float)dt_ms / 1000.0f;
    int dos_ticks = msim_dos_frame_ticks(s->now, dt_ms);
    s->dos_frame_ticks = dos_ticks;
    int first_frame = s->now == 0;
    int i;
    w.user = s; w.targets = cb_targets; w.finished = cb_finished; w.started = cb_started;
    s->now += dt_ms;
    if (!msim_dos_int && s->paths && s->world) paths_step(s);
    if (s->player_vertical) {   /* tools without their own player physics: stand the player on the terrain */
        s->player_unit.ground = msim_ground(s, s->player[0], s->player[2], s->player_unit.y + s->player_centre_h);
        combat_jets(&s->player_unit, 0, dt, &s->rng);
    }
    if (msim_dos_int && first_frame) {
        /* A replay may supply the time already spent loading. The estimated
         * boot interval fills only the part that isn't on the mission clock. */
        s->dos_boot_ticks = dos_ticks < DOS_FIRST_CONTROL_TICK ? DOS_FIRST_CONTROL_TICK - dos_ticks : 0;
        /* The player's first movement can land before the startup roll. */
        s->player_online_at = startup_deadline(s, startup_ticks(s), 1);
        dos_player_startup_sound(s);
    }
    if (msim_dos_int) {
        /* DOS runs animation tasks at 0x15d7f, before movement and the controller.
         * Newly entered keys are already available to both passes. */
        actors_animate(s, dt, dos_ticks);
        s->dos_player_anim_event = 0;
        if (s->dos_player_anim && !s->player_unit.destroyed) {
            anim_player *player = s->dos_player_anim;
            if (s->dos_player_anim_held) anim_player_hold(player);
            s->dos_player_anim_event = anim_player_step(s->dos_player_anim_set, player, s->dos_player_anim_seq,
                                                        s->dos_player_gait, s->dos_player_anim_rate, (float)dos_ticks, 1);
            mech3d_dos_anim_step(s->dos_player_model, s->dos_player_anim_set, player, &s->dos_anim_phase);
        }
    }
    if (msim_dos_int && !first_frame) {
        /* DOS 0x15d84 calls 0x24810. Movement consumes the state left by
         * the previous controller pass (0x15d9d), including startup and AI inputs. */
        for (i = 0; s->minds && s->ai_ok && i < s->actor_count; i++) {
            if (!s->armed[i]) continue;
            if (s->units[i].destroyed) actor_wreck_move(s, i, dt, dos_ticks);
            else actor_move(s, i, dt, dos_ticks, s->units[i].offline,
                            s->units[i].jets > 0 && !s->units[i].no_jump);
        }
    }
    combat_step(s, dt_ms, dos_ticks);
    if (msim_dos_int && s->dos_player_anim && s->on_dos_player_gait && !s->player_unit.destroyed)
        s->on_dos_player_gait();
    if (mw2_development_env("MW2_TEST_FALLSIM")) {   /* TEST ONLY: units (the player -1, wrecks too) under every type 5 surface at their x, z,
                                         * every 0.25 s: "FALLSIM t= unit= skel [WRECK] x= z= y= vy= ground= floor=lo..hi" */
        static int32_t fl_t = -100000;
        int q, d4;
        if (s->now - fl_t >= 250) {
            fl_t = s->now;
            for (q = -1; q < s->actor_count; q++) {
                const combat_unit *u = q < 0 ? &s->player_unit : &s->units[q];
                float x = q < 0 ? s->player[0] : (float)s->actors[q].mech.origin[0], z = q < 0 ? s->player[2] : (float)s->actors[q].mech.origin[2];
                float lo = 0, hi = 0, l2, h2;
                int nf = 0;
                if (q >= 0 && (!s->armed[q] || fixed_unit(&s->actors[q]))) continue;
                for (d4 = 0; d4 < 5; d4++)   /* (and 3 cm round it: a point in a quadtree seam has no polygon) */
                    if (test_floor(s, x + (d4 == 1 ? 3.0f : d4 == 2 ? -3.0f : 0.0f), z + (d4 == 3 ? 3.0f : d4 == 4 ? -3.0f : 0.0f), &l2, &h2)) {
                        if (!nf || l2 < lo) lo = l2;
                        if (!nf || h2 > hi) hi = h2;
                        nf++;
                    }
                if (nf && u->y < lo - 300.0f)
                    fprintf(stderr, "FALLSIM t=%.2f unit=%d %s%s x=%.0f z=%.0f y=%.0f vy=%.2f ground=%.0f floor=%.0f..%.0f\n", s->now / 1000.0, q,
                            q < 0 ? s->player_skel : s->actors[q].skel, u->destroyed ? " WRECK" : "", x, z, u->y, u->vy, u->ground, lo, hi);
            }
        }
    }
    if (!msim_dos_int) {
        wrecks(s, dt_ms);
        blow_offs(s);
        debris_step(s, (float)dt_ms * 0.182f);
        mission_status_step(s, &w);
    } else if (first_frame) {
        /* Seed the initial orders before the first controller update. */
        mtbl_logic_step(&s->logic, s->now, &w);
    }

    /* AI (src/ai.c): every unit runs its AIT programs; the group's mission order picks the
     * order program (mapping assumed: destroy -> DESTROY, reach -> RECON, leave -> LEAVE,
     * otherwise DEFEND), leader or follower version. */
    if (s->star_post && s->minds && s->engage && s->cmd) group_assign(s, 0, -1, s->star_post - 2);   /* the player leads: its tick */
    s->star_post = 0;
    if (!msim_dos_int && s->minds && s->ai_ok && s->seen_node) {   /* 0x10014ba0 in each leader's tick (0x10011c50), not the player's star */
        int t, k, n = (int)(dt * 182.0f + 0.5f);
        for (t = 0; t < s->logic.table_count; t++) {
            world_actor *l = group_leader(s, t);
            int li = l ? (int)(l - s->actors) : -1, cur = s->logic.current[t];
            if (li < 0 || (l->friendly && t == 0) || !s->armed[li] || s->units[li].destroyed) continue;
            if (s->seen_node[li] != cur) continue;   /* its node change (0x10014520) is applied first, below */
            for (k = 0; k < (n > 0 ? n : 1); k++) node_assign(s, t, li);
        }
    }
    for (i = 0; s->minds && s->ai_ok && i < s->actor_count; i++) {
        world_actor *ac = &s->actors[i];
        ai_mind *m = &s->minds[i];
        ai_view v;
        int cur = ac->table < s->logic.table_count ? s->logic.current[ac->table] : -1, prog;
        world_actor *l = group_leader(s, ac->table);
        float dx, dz;
        ai_set_class(m, ac->unit_class, s->units[i].tons);   /* the manoeuvre record (DOS 0x2c880: by class, the 1-ton jumper) */
        int enemy_who = -1, in_star = ac->friendly && ac->table == 0 && !s->player_unit.destroyed;
        if (!msim_dos_int) actor_wreck_move(s, i, dt, dos_ticks);
        if (!s->armed[i] || s->units[i].destroyed) continue;
        if (l == ac && m->event_cond == 7) {   /* the leader's tick: its posted sighting (0x10011c50 -> 0x10014ba0) */
            group_assign(s, ac->table, i, m->event_who);
            m->event_cond = 0;
        }
        /* order program: engine 0x10014520 / 0x10014700. order = lowest set bit of the node kind
         * + 1 (no node = 0x200); the per-class tables (identical for classes 1-8) give: */
        {
            static const int LEAD[15] = {AIP_DEFLT, AIP_LDESTROY, AIP_LDESTROY, AIP_LDEFEND, AIP_LRECON, AIP_DEFLT, AIP_LRECON,
                                         AIP_DEFLT, AIP_DEFLT, AIP_LRECON, AIP_DEFLT, AIP_DEFLT, AIP_DEFLT, AIP_DEFLT, AIP_LLEAVE};
            static const int FOLL[15] = {AIP_DEFLT, AIP_FDESTROY, AIP_FDESTROY, AIP_FDEFEND, AIP_FRECON, AIP_DEFLT, AIP_FRECON,
                                         AIP_DEFLT, AIP_DEFLT, AIP_FRECON, AIP_DEFLT, AIP_DEFLT, AIP_DEFLT, AIP_DEFLT, AIP_FLEAVE};
            uint32_t k = cur >= 0 ? s->logic.tables[ac->table].nodes[cur].kind : 0x200;
            int order = 0, b;
            for (b = 0; b < 16; b++) if (k & (1u << b)) { order = b + 1; break; }
            if (order > 14) order = 0;
            prog = ac->leader ? LEAD[order] : FOLL[order];
            if (prog == AIP_DEFLT) prog = -1;          /* DEFLT already in slot 0 */
        }
        if (!in_star && msim_dos_int && l == ac && s->seen_node && s->seen_node[i] != cur) {
            /* The whole star enters its new node before the leader dispatches
             * the first order. A later controller must retain that order. */
            for (int member = 0; member < s->actor_count; member++)
                if (s->actors[member].table == ac->table && s->armed[member] && !s->units[member].destroyed)
                    sync_unit_node(s, member, cur);
        }
        if (!in_star) sync_unit_node(s, i, cur);
        if (m->progs[1] != prog) {
            m->progs[1] = prog;
            /* engine 0x10014520 on a node change: kind 0x400 -> rest (10), 0x800 -> shutdown (11); otherwise the leader
             * idles (0) and the members follow (5). The player's star stops there - its mates stay in formation on the
             * player (node targets are never assigned to it; the bug where JACK's mates walked off to en01Star at the
             * start). Other stars then get the node's targets one member a tick (0x10014ba0, node_assign). */
            uint32_t kk = cur >= 0 ? s->logic.tables[ac->table].nodes[cur].kind : 0;
            if (in_star) { if (!(s->cmd && s->cmd[i]) && m->state != AI_REST && m->state != AI_SHUTDOWN) { m->state = AI_FOLLOW; m->target = 0; } }
            (void)kk;   /* 0x400 / 0x800 power-down (states 10 / 11) not applied yet: the port's AI has no rest / wake path */
        }
        if (msim_dos_int && first_frame) {
            s->units[i].offline = 1;
            continue;
        }
        /* Each DOS leader assigns one order in its controller visit. Its
         * target-entry draw precedes this controller's startup draw. */
        if (msim_dos_int && l == ac && !(ac->friendly && ac->table == 0) && s->seen_node && s->seen_node[i] == cur)
            node_assign(s, ac->table, i);
        memset(&v, 0, sizeof v);
        v.heading = ac->mech.heading;
        v.heat = s->units[i].heat;
        v.range_c = ac->ai_range[0]; v.range_a = ac->ai_range[1]; v.range_b = ac->ai_range[2];   /* GPS (engine 0x10012000) */
        v.skill = ac->ai_skill;
        if (msim_dos_int) v.dos_clock = msim_dos_frame_ticks(0, s->now);
        v.twist_limit = s->twist_limit ? s->twist_limit[i] : 90.0f;
        v.self_x = (float)ac->mech.origin[0]; v.self_z = (float)ac->mech.origin[2];
        /* the attacker reaction (0x10012520, at the start of every rule pass unless lock bit 1): a live unit of another
         * alliance attacking this one (state 3 with this one as its target - the player only while firing at an
         * alliance-1 target, 0x10011c50 -> 0x10014110) makes this one target it */
        if (s->ai_focus && s->ai_who && !(s->cmd && s->cmd[i]) && !(in_star ? cur >= 0 && s->logic.tables[ac->table].nodes[cur].lock == 2 : (m->lock_bits & 1))) {
            int self_al = ac->friendly ? 0 : ac->alliance, att = -2, q3;
            /* the player is "attacking" a unit only once one of its shots has HIT it (shots.c -> RecordAttack 0x10014110:
             * the shooter's goal and state 3, which FindAttacker 0x10013370 looks for) - not for firing at it while it is
             * targeted: the port had woken a resting unit for a missile passing over it (user report, Trial 1's second
             * star: they wake only when hit) */
            if (self_al != 0 && s->player_victim == i && !s->player_unit.destroyed) att = -1;
            for (q3 = 0; q3 < s->actor_count && att == -2; q3++) {
                int al = s->actors[q3].friendly ? 0 : s->actors[q3].alliance;
                if (q3 != i && s->armed[q3] && !s->units[q3].destroyed && s->ai_who[q3] == i && al != self_al) att = q3;
            }
            if (att != -2 && s->ai_focus[i] != att) {
                s->ai_focus[i] = att;
                m->prev_state = m->state;
                if (msim_dos_int) {
                    m->counter = 1;
                    target_fire_delay(s, m);
                }
                m->state = AI_TARGET;
                m->target = 1;
                ai_wake(s, i);
            }
        }
        if (s->ai_focus && s->ai_focus[i] >= 0 && (!s->armed[s->ai_focus[i]] || s->units[s->ai_focus[i]].destroyed)) s->ai_focus[i] = -2;
        if (s->ai_focus && s->ai_focus[i] == -1 && s->player_unit.destroyed) s->ai_focus[i] = -2;
        {
            float ex = 0, ez = 0, ey = 0, ef = 0, eh = 0;
            int who = -1;
            v.has_enemy = pick_enemy(s, i, &ex, &ez, &ey, &ef, &eh, &who);
            enemy_who = v.has_enemy ? who : -1;
            v.enemy_is_player = v.has_enemy && who == -1;
            v.target_not_unit = v.has_enemy && who < -1;
            v.enemy_x = ex; v.enemy_z = ez; v.enemy_heading = ef; v.enemy_height = ey - s->units[i].ground;   /* against my own height above my ground */
        }
        if (s->provoked && s->provoked[i] > 0) {   /* MW2_PROVOKE=1 only (a port option, off by default) */
            ai_mind *pm = &s->minds[i];
            v.provoked = 1; s->provoked[i] -= dt;
            /* turn on the attacker: the engine's provoked mechs take the attacker as their target and attack (0x10014110
             * marks the attacker attacking; 0x10012de0 picks it) - whatever their program was doing */
            if (pm->state != AI_TARGET && pm->state != AI_ATTACK) { pm->prev_state = pm->state; pm->state = AI_ATTACK; pm->target = 1; }
        }
        {   /* condition references and the manoeuvre tests (see ai.h) */
            int c7 = c7_pick(s, i, &v.c7_nearest);
            v.c7_ok = c7 != -2;
            m->post_who = c7;
            v.commanded = (s->cmd && s->cmd[i] != 0) || m->lock_bits;
            v.unit_led = !in_star;
            if (m->nav_ok) {
                float nx = m->nav_x - (float)ac->mech.origin[0], nz = m->nav_z - (float)ac->mech.origin[2];
                v.navref_ok = 1; v.navref_radius = 3000.0f;
                v.navref_dist = sqrtf(nx * nx + nz * nz); v.navref_bearing = atan2f(nx, nz) * 180.0f / 3.14159265f;
            }
            if (l && l != ac) {
                float lx = (float)l->mech.origin[0] - (float)ac->mech.origin[0], lz = (float)l->mech.origin[2] - (float)ac->mech.origin[2];
                v.leader_ok = 1; v.leader_dist = sqrtf(lx * lx + lz * lz);
            } else if (in_star) {
                float lx = s->player[0] - (float)ac->mech.origin[0], lz = s->player[2] - (float)ac->mech.origin[2];
                v.leader_ok = 1; v.leader_dist = sqrtf(lx * lx + lz * lz);
            }
            v.contact = (s->bump_with && s->bump_with[i] > 0) || s->units[i].blocked;   /* controller +0xa4 (cleared on landing on a unit) */
            v.contact_unit = s->bump_with && s->bump_with[i] > 0;
            v.grounded = s->units[i].y <= s->units[i].ground + 1.0f;
            v.out_of_weapons = combat_out_of_weapons(&s->units[i]);                  /* 0x10020880 */
            v.moving = ac->speed > 0 && !fixed_unit(ac);   /* controller +0x88: the top speed (0x10041410; 0 for turrets, 0x100219c9) */
        }
        v.can_jump = s->units[i].jets > 0 && !s->units[i].no_jump && s->units[i].jet_fuel > 5.0f;
        v.jet_fuel = s->units[i].jet_fuel;
        v.jets_installed = s->units[i].jets > 0 && !s->units[i].no_jump;
        v.vertical_speed = s->units[i].vy;
        v.height = s->units[i].y - s->units[i].ground;   /* above the terrain (ai.h): y is absolute - on raised ground the
                                                          * jump manoeuvres never saw a landing */
        v.pilot_level = ac->ai_level >= 1 && ac->ai_level <= 4 ? ac->ai_level : 1;   /* 0x1001ee30: outside 1-4 -> 1 */
        v.weapon_range = combat_max_range(&s->units[i]);
        v.is_leader = ac->leader;
        dx = v.enemy_x - (float)ac->mech.origin[0];
        dz = v.enemy_z - (float)ac->mech.origin[2];
        v.enemy_dist = sqrtf(dx * dx + dz * dz);
        v.enemy_bearing = atan2f(dx, dz) * 180.0f / 3.14159265f;
        if (!msim_dos_int && (ac->unit_class == 3 || ac->unit_class == 8) && ac->mech.eye_ok) {
            /* turrets and the turret-class dropships aim (the twist demand) from their eye, as the pitch below: the
             * aim ray starts there (0x10044810), and a dropship's eye stands 17 m out on its nose, so a bearing from the
             * origin sent every shot past the player to one side off the centre line (user report, Sable Flame: the
             * Falcon dropships' fire missed left or right on every difficulty) */
            float ex2 = v.enemy_x - (float)ac->mech.origin[0] - (ac->mech.eye_pos[0] - ac->mech.mount_from[0]);
            float ez2 = v.enemy_z - (float)ac->mech.origin[2] - (ac->mech.eye_pos[2] - ac->mech.mount_from[2]);
            v.enemy_bearing = atan2f(ex2, ez2) * 180.0f / 3.14159265f;
        }
        {   /* the player's star (engine: the player leads it; "my leader" 0x2201 = the player) and its orders */
            int star = ac->friendly && ac->table == 0 && !s->player_unit.destroyed;
            int c = s->cmd ? s->cmd[i] : 0, t = s->cmd_target ? s->cmd_target[i] : -1;
            if (star && s->mate_target) {
                /* condition 6 (0x10013090): the mate's target is destroyed -> "Point N reports target destroyed"
                 * (message 5) for units the player leads. A commanded attacker then leaves its state through the
                 * exit hook 0x10013680: message 10 ("task complete") only if it was still closing (state 2), and
                 * either way it goes back to the formation (action 3 pop / the rule's state 5) - still commanded
                 * (+0x152 & 3 is never cleared there), so it picks no targets of its own until Engage at Will. */
                int mt = s->mate_target[i];
                if ((mt >= 0 && mt < s->actor_count && s->armed[mt] && s->units[mt].destroyed) ||
                    (MSIM_IS_THING(mt) && MSIM_THING_OF(mt) < s->bld_count && s->bld[MSIM_THING_OF(mt)].hp <= 0)) {
                    msim_order_message(s, star_point(s, i), 5);
                    if (c == MSIM_CMD_ATTACK && mt == t) {
                        if (m->state == AI_TARGET) msim_order_message(s, star_point(s, i), 10);
                        s->cmd[i] = c = MSIM_CMD_JOIN; s->cmd_target[i] = t = -1;
                        m->state = AI_FOLLOW; m->target = 0;
                    } else if (!c && (m->state == AI_TARGET || m->state == AI_ATTACK)) { m->state = AI_FOLLOW; m->target = 0; }
                    s->mate_target[i] = -1;
                }
                if (c == MSIM_CMD_ATTACK && !v.has_enemy) {   /* target destroyed or no longer available */
                    if (m->state == AI_TARGET) msim_order_message(s, star_point(s, i), 10);
                    s->cmd[i] = c = MSIM_CMD_JOIN; s->cmd_target[i] = t = -1;
                    m->state = AI_FOLLOW; m->target = 0;
                }
                if (c == MSIM_CMD_DEFEND && t >= 0 && t < s->actor_count && s->units[t].destroyed && s->defend_leg &&
                    s->defend_leg[i] < 4) {
                    /* the defended mech died before the first patrol point was reached: message 10 (exit hook, state 7,
                     * target flag 4) and back to the formation; after that the mate keeps patrolling the wreck */
                    msim_order_message(s, star_point(s, i), 10);
                    s->cmd[i] = c = MSIM_CMD_JOIN; s->cmd_target[i] = t = -1;
                    m->state = AI_FOLLOW; m->target = 0;
                }
            }
            if (star && c) {
                /* a commanded mate keeps its task: the target choice is off (condition 7, 0x10012fd0, returns 0 while +0x152 & 3) */
                if (c == MSIM_CMD_ATTACK) {
                    if (t >= 0) v.enemy_height = s->units[t].y - s->units[i].ground;
                    if (m->state != AI_TARGET && m->state != AI_ATTACK) { m->state = AI_TARGET; m->target = 1; }
                } else v.has_enemy = 0;
                dx = v.enemy_x - (float)ac->mech.origin[0];
                dz = v.enemy_z - (float)ac->mech.origin[2];
                v.enemy_dist = sqrtf(dx * dx + dz * dz);
                v.enemy_bearing = atan2f(dx, dz) * 180.0f / 3.14159265f;
            }
            if (star && (c == MSIM_CMD_DEFEND || c == MSIM_CMD_DISENGAGE)) {
                float tx = 0, tz = 0, r = 10000.0f;
                int ok = 0;
                if (c == MSIM_CMD_DEFEND && t >= 0 && t < s->actor_count && s->defend_leg) {
                    /* engine 0x10013f00: four 30 m patrol points at x - R, z - R, x + R, z + R round the defended mech, R
                     * = its reference range (0x10013450: a unit 100 m); the goal is the first own point in the nav table;
                     * DEFLT state 7 re-enters on reaching it (cond 3, ref 0x2a; ref2 0x40 = the mech), every other point
                     * deleted and four more made round the mech's new position (guard_drop / guard_enter). No engagement:
                     * condition 7 is blocked while commanded and the group assignment skips commanded units */
                    if (m->guard_n == 0 && m->state == AI_PATROL) guard_enter(s, m, 0x20000 | (t + 1));
                    if (m->guard_n > 0) { tx = m->guard[0][0]; tz = m->guard[0][1]; r = 3000.0f; ok = 1; }
                }
                if (c == MSIM_CMD_DISENGAGE && s->player_nav >= 0 && s->player_nav < s->nav_count) {
                    tx = (float)s->navs[s->player_nav].x; tz = (float)s->navs[s->player_nav].z; r = (float)s->navs[s->player_nav].radius; ok = 1;
                }
                if (ok) {
                    dx = tx - (float)ac->mech.origin[0]; dz = tz - (float)ac->mech.origin[2];
                    v.has_goal = 1; v.goal_radius = r;
                    v.goal_dist = sqrtf(dx * dx + dz * dz); v.goal_bearing = atan2f(dx, dz) * 180.0f / 3.14159265f;
                }
            } else if (star && c != MSIM_CMD_ATTACK && c != MSIM_CMD_SHUTDOWN) {
                /* follow the player: the formation slot relative to the player's facing */
                float h = s->player_facing * 3.14159265f / 180.0f;
                float sx = s->player[0] + cosf(h) * ac->form_x + sinf(h) * ac->form_z;
                float sz = s->player[2] - sinf(h) * ac->form_x + cosf(h) * ac->form_z;
                dx = sx - (float)ac->mech.origin[0]; dz = sz - (float)ac->mech.origin[2];
                v.has_goal = 1; v.goal_radius = 3000.0f;   /* formation slots are 30 m nav points (0x1001cab0) */
                v.goal_dist = sqrtf(dx * dx + dz * dz); v.goal_bearing = atan2f(dx, dz) * 180.0f / 3.14159265f;
                if (m->state == AI_IDLE || m->state == AI_PATROL) m->state = AI_FOLLOW;
                l = NULL;                                  /* the leader is the player, not an actor */
            }
            if (star) goto star_done;
        }
        if (!ac->leader && l && l != ac) {
            /* follower: its formation slot behind the leader */
            float h = l->mech.heading * 3.14159265f / 180.0f;
            float sx = (float)l->mech.origin[0] + cosf(h) * ac->form_x + sinf(h) * ac->form_z;
            float sz = (float)l->mech.origin[2] - sinf(h) * ac->form_x + cosf(h) * ac->form_z;
            dx = sx - (float)ac->mech.origin[0]; dz = sz - (float)ac->mech.origin[2];
            v.has_goal = 1; v.goal_radius = 3000.0f;   /* formation slots are 30 m nav points (0x1001cab0) */
            v.goal_dist = sqrtf(dx * dx + dz * dz); v.goal_bearing = atan2f(dx, dz) * 180.0f / 3.14159265f;
            if (m->state == AI_IDLE || (m->state == AI_PATROL && !m->node_tgt)) m->state = AI_FOLLOW;   /* (a node's guard: kept) */
        } else if (cur >= 0) {
            const mtbl_node *n = &s->logic.tables[ac->table].nodes[cur];
            float tx, tz, r;
            if (is_player_rec(s, n->target) && !s->player_unit.destroyed) {
                /* attack order: the goal is the player */
                v.has_goal = 1; v.goal_radius = 10000.0f; v.goal_dist = v.enemy_dist; v.goal_bearing = v.enemy_bearing;
            } else if (target_pos(s, n->target, &tx, &tz, &r) == 0) {
                dx = tx - (float)ac->mech.origin[0]; dz = tz - (float)ac->mech.origin[2];
                v.has_goal = 1; v.goal_radius = r;
                v.goal_dist = sqrtf(dx * dx + dz * dz); v.goal_bearing = atan2f(dx, dz) * 180.0f / 3.14159265f;
            }
        }
        if (m->node_tgt && (m->state == AI_PATROL || (m->state == AI_GODIRECT && !m->to_navref && !m->target))) {
            /* a node task's goal +0x14c (0x10014ba0): patrol (kind 4) heads for its first own 30 m point (0x10013f00), go to
             * (0x100) for the target itself within 0x10013450's range (a nav point's radius, else 100 m) - the movement of
             * states 6-8 (0x100130f0: throttle 0x100134c0 by that range, steering 0x10013600) */
            float p[3], r = 3000.0f;
            int ok = 0;
            if (m->state == AI_PATROL && m->guard_n > 0) { p[0] = m->guard[0][0]; p[2] = m->guard[0][1]; ok = 1; }
            else if (m->state == AI_GODIRECT) ok = task_pos(s, m->node_tgt, p, &r);
            if (ok) {
                dx = p[0] - (float)ac->mech.origin[0]; dz = p[2] - (float)ac->mech.origin[2];
                v.has_goal = 1; v.goal_radius = r;
                v.goal_dist = sqrtf(dx * dx + dz * dz); v.goal_bearing = atan2f(dx, dz) * 180.0f / 3.14159265f;
            }
        }
    star_done:
        m->targets_player = v.has_enemy && v.enemy_is_player;
        {   /* DOS 0x2da50 and 0x2e150 test from the unit centre and again at the target's elevation. */
            const float own_y = msim_dos_int && s->centre_h ? s->units[i].y + s->centre_h[i] : actor_aim_base(s, i);
            const float target_h = !msim_dos_int || !v.has_enemy ? 0.0f :
                                   enemy_who == -1 ? s->player_centre_h :
                                   enemy_who >= 0 && s->centre_h ? s->centre_h[enemy_who] : 0.0f;
            const float target_y = v.enemy_height + s->units[i].ground + target_h;
            const float dy = target_y - own_y;
            v.world_height = own_y;
            v.target_world_height = target_y;
            if (msim_dos_int && v.has_enemy) {
                int32_t x = (int32_t)lroundf(v.enemy_x) - ac->mech.origin[0];
                int32_t z = (int32_t)lroundf(v.enemy_z) - ac->mech.origin[2];
                int32_t level = dos_level_distance(x, z);
                v.enemy_dist = (float)level;
                v.enemy_dist3 = (float)dos_slant_distance(level, (int32_t)lroundf(target_y) - (int32_t)lroundf(own_y));
                if (ac->unit_class == 3 || ac->unit_class == 8)
                    v.enemy_bearing = (float)dos_atan2(x, z) / 65536.0f;
            } else v.enemy_dist3 = hypotf(v.enemy_dist, dy);
            if (v.has_enemy && !v.target_not_unit && ac->unit_class == 1 && dy > 800.0f) {
                float from[3] = {(float)ac->mech.origin[0], own_y, (float)ac->mech.origin[2]};
                const float to[3] = {v.enemy_x, target_y, v.enemy_z};
                if (msim_dos_int) {
                    v.jump_low = !dos_clear_to(s, i, enemy_who, -1, from, to);
                    from[1] = target_y;
                    v.jump_view = v.jump_low && dos_clear_to(s, i, enemy_who, -1, from, to);
                } else {
                    v.jump_low = los_blocked(s, from[0], from[1], from[2], to[0], to[1], to[2]);
                    v.jump_view = v.jump_low && !los_blocked(s, from[0], to[1], from[2], to[0], to[1], to[2]);
                }
            }
            v.flank_slots = !v.has_enemy ? NULL : enemy_who == -1 ? s->flank_slots : enemy_who >= 0 && s->unit_flank ? s->unit_flank[enemy_who] : NULL;
            v.own_flank = s->unit_flank ? s->unit_flank[i] : NULL;
        }
        {
            float eye_up = 0.0f, aim_up = 0.0f, hd = v.enemy_dist;
            int starmate = ac->friendly && ac->table == 0;
            if (starmate || (!msim_dos_int && (ac->unit_class == 3 || ac->unit_class == 8) && ac->mech.eye_ok)) {
                /* The firing ray starts at the eye. Aim at the target's body even when both units are on level ground. */
                float dx_eye = v.enemy_x - (float)ac->mech.origin[0];
                float dz_eye = v.enemy_z - (float)ac->mech.origin[2];
                if (ac->mech.eye_ok) {
                    dx_eye -= ac->mech.eye_pos[0] - ac->mech.mount_from[0];
                    dz_eye -= ac->mech.eye_pos[2] - ac->mech.mount_from[2];
                    eye_up = ac->mech.eye_pos[1] - ac->mech.mount_from[1];
                } else eye_up = s->height[i] * 0.7f;
                if (enemy_who == -1) aim_up = s->player_centre_h;
                else if (enemy_who >= 0) {
                    aim_up = starmate ? msim_draw_y(s, enemy_who) - s->units[enemy_who].y + s->height[enemy_who] * 0.55f
                                      : s->centre_h[enemy_who];
                }
                hd = hypotf(dx_eye, dz_eye);
                if (starmate && v.has_enemy) v.enemy_bearing = atan2f(dx_eye, dz_eye) * 180.0f / 3.14159265f;
            }
            if (msim_dos_int && !starmate && (ac->unit_class == 3 || ac->unit_class == 5 || ac->unit_class == 8)) {
                /* 0x22850 / 0x229a0 use object centres for the controller's aim.
                 * The projectile convergence ray still starts at the eye. */
                int32_t target_y = (int32_t)lroundf(v.target_world_height);
                int32_t own_y = (int32_t)lroundf(v.world_height);
                v.enemy_elev = (float)dos_atan2(target_y - own_y, (int32_t)v.enemy_dist) / 65536.0f;
            } else {
                v.enemy_elev = atan2f(v.enemy_height + aim_up + s->units[i].ground - actor_aim_base(s, i) - eye_up,
                                      fmaxf(hd, 1.0f)) * 180.0f / 3.14159265f;
            }
        }
        v.speed_kmh = fabsf(m->speed) * 0.036f;   /* actual speed */
        /* powered down: a shut-down unit (state 11) runs no rules - it waits for an event; a RESTING one (state 10, a hold node)
         * runs its state's rules as any other (RunAIRules 0x10011d20 for every AI player): DEFLT's rest rule posts a sighting
         * within the unit's rest alert range (condition 7, range -1) to its leader, whose group assignment wakes it. The
         * port ran no rules for either, so a resting unit woke only by its star's next node (user report: Sable Flame's
         * dropships, rest alert 1000 m, woke at their 750 m zone) */
        /* DOS MW2.EXE 0x21510: the rule pass 0x215f0 runs for every computer-run unit in every AI state but 12 - shut down
         * (11) included; any state entered then goes through the entry 0x22bd0, which restarts a powered-down unit
         * (the start-up again) unless the state entered is 10 / 11 itself (the GBL port's audit; MW2.EXE the same code) */
        if (s->powered_down && s->powered_down[i] && m->state != AI_REST && !(msim_dos_int && m->state == AI_SHUTDOWN)) { m->throttle = 0; m->turn = 0; m->wants_fire = 0; }
        else {
            int was_down = s->powered_down && s->powered_down[i];
            struct ai_avoid_context avoid = {s, m, i, v.target_not_unit ? -2 : enemy_who, (float)dos_ticks};
            if (msim_dos_int && s->avoid_turn && v.moving) {
                v.dos_avoid = dos_avoid_attack;
                v.avoid_context = &avoid;
            }
            ai_step(&s->ai, m, &v, dt, dos_ticks);
            if (msim_dos_int && !m->rule_fired)
                m->dos_warning_unit = v.has_enemy && (m->state == AI_TARGET || m->state == AI_ATTACK ||
                                        (m->state == AI_GODIRECT && m->target)) ? enemy_who : -2;
            if (msim_dos_int && m->entered && was_down && m->state != AI_REST && m->state != AI_SHUTDOWN) ai_wake(s, i);
            else if (msim_dos_int && m->entered && !was_down && (m->state == AI_REST || m->state == AI_SHUTDOWN)) ai_power_down(s, i);
            if (s->powered_down && s->powered_down[i]) { m->throttle = 0; m->turn = 0; m->wants_fire = 0; m->jet = 0; }   /* (still down until woken) */
            if (m->entered) {   /* 0x10013e00: the exit (0x10013680 cases 5 / 7 -> 0x10013e90) and the entry (0x10013790 case 7
                                 * -> 0x10013f00 round the target +0x14e: a node's guard target, a Defend order's mech) */
                int c = s->cmd ? s->cmd[i] : 0, t = s->cmd_target ? s->cmd_target[i] : -1;
                if (m->entered_from == AI_FOLLOW || m->entered_from == AI_PATROL) guard_drop(m);
                if (m->state == AI_PATROL) {
                    if (in_star && c == MSIM_CMD_DEFEND && t >= 0) {
                        if (m->entered_from == AI_PATROL && s->defend_leg) s->defend_leg[i] |= 4;   /* a patrol point reached */
                        guard_enter(s, m, 0x20000 | (t + 1));
                    }
                    else if (!in_star && m->node_tgt) guard_enter(s, m, m->node_tgt);
                }
            }
        }
        if (mw2_development_env("MW2_TEST_HOLD_ACTOR") && atoi(mw2_development_env("MW2_TEST_HOLD_ACTOR")) == i) {   /* TEST hook: this unit stands still
                                                                                               * (no walking, turning, jets, dodges or fire) */
            m->throttle = 0; m->turn = 0; m->jet = 0; m->jet_forward = 0; m->jet_back = 0; m->jet_side = 0; m->wants_fire = 0; m->reverse = 0;
        }
        if (s->sd_at && m->self_destruct) {
            /* input +0x43 (manoeuvre 6's charge ended within 20 m, 0x1001fb90). 3D editions (0x1001a180): taken only with
             * the mech up (controller state 2) and cleared: state 7 with a deadline 0x16a ticks on - no fire meanwhile
             * (0x100437a0 runs in state 2 only; it has no weapons left anyway), the AI and the legs carry on; then
             * 0x100174f0(c, 0). The DOS game (0x27785 -> 0x265f0; its charge 0x2d4a0) destroys it at once. No message
             * or sound for an AI unit (message 4 is the player's) */
            int up = !s->units[i].shutdown && !(s->online_at && s->now < s->online_at[i]);
            if (s->log && (s->sd_at_once || (up && !s->sd_at[i]))) {
                char line[128];
                snprintf(line, sizeof line, "%6.1fs %s (%s) self-destruct (input +0x43)", (double)s->now / 1000.0, ac->name, ac->group);
                s->log(s->log_user, line);
            }
            if (s->sd_at_once) { m->self_destruct = 0; self_destruct(s, i); }
            else if (up) { m->self_destruct = 0; if (!s->sd_at[i]) s->sd_at[i] = s->now + 0x16a * 1000 / 182; }
        }
        if (s->sd_at && s->sd_at[i] && s->now > s->sd_at[i]) { s->sd_at[i] = 0; self_destruct(s, i); }
        if (s->ai_who) s->ai_who[i] = m->state == AI_ATTACK && v.has_enemy ? enemy_who : -2;
        if (s->cmd && s->cmd[i] == MSIM_CMD_SHUTDOWN) { m->state = AI_SHUTDOWN; m->throttle = 0; m->turn = 0; m->wants_fire = 0; m->target = 0; }
        /* action 4 (0x100141e0 -> 0x10014270): (condition, the pick) to the leader - itself when it leads - if it has none
         * pending; the player's star posts to the player (its assignment runs at the top of the next step) */
        if (m->post_cond && in_star) { if (!s->star_post) s->star_post = m->post_who + 2; }
        else if (m->post_cond) {
            ai_mind *lm = l ? &s->minds[l - s->actors] : m;
            if (mw2_development_env("MW2_POST_TRACE")) fprintf(stderr, "post %.2f actor %d cond %d who %d leader %d pending %d\n", s->now / 1000.0, i, m->post_cond, m->post_who, l ? (int)(l - s->actors) : -1, lm->event_cond);   /* TEST ONLY */
            if (lm->event_cond == 0) { lm->event_cond = m->post_cond; lm->event_who = m->post_who; }
        }
        if (m->c7_fired && (m->state == AI_TARGET || m->state == AI_ATTACK)) m->tgt = m->post_who;   /* ref2 0x25: the pick */
        if (m->state != AI_TARGET && m->state != AI_ATTACK && !(m->state == AI_GODIRECT && m->target)) m->tgt = -2;
        if (in_star && s->mate_target && (m->state == AI_TARGET || m->state == AI_ATTACK))
            s->mate_target[i] = s->cmd && s->cmd[i] == MSIM_CMD_ATTACK ? s->cmd_target[i] : enemy_who;
        m->post_cond = 0;
        /* every class but turrets and doors drives with the mech controller (0x1001a180 / 0x10019310): vehicles too, at
         * their MEK speed (they have no walk animation; the old !have_anim test kept every tank, hovercraft and gunship
         * at its spawn point) */
        if (s->units[i].shutdown || s->units[i].immobile || fixed_unit(ac)) m->throttle = 0;
        if (fixed_unit(ac)) { m->turn = 0; m->jet = 0; m->jet_forward = 0; m->jet_back = 0; m->jet_side = 0; }
        /* The DOS controller promotes state 1 to state 2 at the end of its
         * update (0x28249). It first consumes input on the following update. */
        int32_t control_time = msim_dos_int ? s->now - dt_ms : s->now;
        int starting = s->online_at && (s->online_at[i] < 0.0f || control_time < s->online_at[i]);
        if (starting) {
            /* DOS 0x280b6 clears the leg servo's demand while 0x280c9 clears
             * the throttle input. The last steering input survives for 0x228d0. */
            m->throttle = 0;
            if (!msim_dos_int) m->turn = 0;
            m->wants_fire = 0; m->jet = 0; m->jet_forward = 0; m->jet_back = 0;
        }
        if (msim_dos_int) {
            dos_ai_fire_decision(s, i, &v, enemy_who);
            dos_ai_weapon_step(s, i, dt, dos_ticks, starting);
            /* DOS 0x27f1a samples the key's drive flag after this frame's move. */
            if (s->anim_pl && s->anim_pl[i].dos_drive_slow) m->drive_slow = 1;
            /* 0x27b5d consumes the footstep flag set in this frame’s task pass. */
            if (s->anim_pl && s->anim_pl[i].dos_footstep) {
                s->anim_pl[i].dos_footstep = 0;
                if (ac->have_anim && !s->units[i].destroyed && s->on_sound) {
                    float p3[3] = {(float)ac->mech.origin[0], s->units[i].y + (s->centre_h ? s->centre_h[i] : 0.0f),
                                   (float)ac->mech.origin[2]};
                    s->on_sound(s->shot_user, 0x103, p3);
                }
            }
            /* Native non-player controllers start on their second visit.
             * The startup draw follows that unit's AI pass (0x27812, 0x28187). */
            if (!first_frame && s->online_at && s->online_at[i] < 0.0f &&
                !(s->powered_down && s->powered_down[i])) {
                s->online_at[i] = startup_deadline(s, startup_ticks(s), 1);
                unit_startup(s, i);
            }
        }
        if (s->avoid_turn && m->state != AI_ATTACK) {   /* 0x100201c0 runs in states 2, 4-8, not 3 */
            float gb = m->state == AI_ATTACK || m->state == AI_TARGET ? v.enemy_bearing : (v.has_goal ? v.goal_bearing : ac->mech.heading);
            avoid_obstacles(s, i, m, gb, msim_dos_int ? (float)dos_ticks : dt * 182.0f);
        }
        if (ac->unit_class == 1 && (msim_dos_int || (m->state == AI_GODIRECT && v.has_goal)) &&
            !starting && !s->units[i].offline && !s->units[i].shutdown &&
            !s->units[i].immobile && !m->jet && m->state >= AI_TARGET && m->state <= AI_GODIRECT)
            ai_escape_terrain(m, ac->mech.heading, v.goal_bearing, dos_ticks, msim_dos_int);
        if (!msim_dos_int) actor_move(s, i, dt, dos_ticks, starting, v.jets_installed);
        if (mw2_development_env("MW2_AI_TRACE") && i == atoi(mw2_development_env("MW2_AI_TRACE")) && (s->now / dt_ms) % 10 == 0) {   /* tests: one AI every 10 steps */
            fprintf(stderr, "ai %.2fs actor %d at %d,%d y %.0f immobile %d heading %.1f turn %.0f avoid %.0f/%d goal %.1f\n", s->now / 1000.0, i, ac->mech.origin[0], ac->mech.origin[2], s->units[i].y, s->units[i].immobile,
                    ac->mech.heading, m->turn, s->avoid_turn ? s->avoid_turn[i] : 0.0f, s->avoid_side ? s->avoid_side[i] : 0, v.has_goal ? v.goal_bearing : -999.0f);
            fprintf(stderr, "ai %.2fs actor %d %s man %d (prev %d) thr %.2f%s speed %.0f twist %.1f->%.1f pitch %.1f->%.1f lock %#x tgt %d fire %d heat %.1f\n",
                    s->now / 1000.0, i, ai_state_name(m->state), m->man, m->prev_man, m->throttle, m->reverse ? " REV" : "", m->speed,
                    m->twist, m->twist_demand, m->pitch, m->pitch_demand, m->lock, m->tgt, m->wants_fire, s->units[i].heat);
        }
    }

    if (!msim_dos_int) actors_animate(s, dt, dos_ticks);
    if (msim_dos_int) {
        if (s->paths && s->world) paths_step(s);
        if (s->on_dos_world_sound) s->on_dos_world_sound();
        debris_step(s, (float)dos_ticks);
        /* The DOS mission evaluator at 0x15dd5 sees this frame's hits. */
        mission_status_step(s, &w);
    }
    if (!msim_dos_int || !s->cam_ok) {
        collision_lods(s);
        dos_sync_hull_order(s);
    }
}

void msim_free(msim *s)
{
    free(s->units);
    free(s->armed);
    free(s->height);
    free(s->wreck_until); s->wreck_until = NULL;
    free(s->msg_seen); s->msg_seen = NULL;
    free(s->twist_limit);
    free(s->minds);
    free(s->cmd); free(s->unit_flank); s->unit_flank = NULL; free(s->online_at); free(s->radius); free(s->fly_seen); free(s->centre_h); free(s->draw_lift); free(s->ai_focus); free(s->ai_who); free(s->powered_down); free(s->seen_node); free(s->sd_at); free(s->cmd_target); free(s->engage); free(s->mate_target); free(s->defend_leg);
    free(s->bld);
    free(s->inspected);
    free(s->ai_lock);
    free(s->provoked);
    free(s->anim_pl);
    free(s->bump_with);
    free(s->aim); s->aim = NULL;
    free(s->contact_frac); s->contact_frac = NULL;
    free(s->contact_acc); s->contact_acc = NULL;
    free(s->across); s->across = NULL;
    free(s->contact_hold); s->contact_hold = NULL;
    s->minds = NULL;
    free(s->air_speed); s->air_speed = NULL;
    free(s->obstacle_part); s->obstacle_part = NULL; s->world = NULL;
    free(s->los_terrain); free(s->los_terrain_box); s->los_terrain = NULL; s->los_terrain_box = NULL; s->los_terrain_count = 0;
    world3d_paths_free(s->paths); s->paths = NULL;
    free(s->dos_anim_order);
    free(s->dos_spin_seeds); s->dos_spin_seeds = NULL; s->dos_spin_count = 0;
    terrain_release(s->terrain); s->terrain = NULL;
    free(s->clist_seq); s->clist_seq = NULL; s->clist_seq_n = 0;
    free(s->dos_hulls); s->dos_hulls = NULL;
    free(s->obstacles); free(s->avoid_turn); free(s->avoid_thr); free(s->avoid_next); free(s->avoid_side); free(s->avoid_len);
    s->obstacles = NULL; s->avoid_turn = s->avoid_thr = s->avoid_next = s->avoid_len = NULL; s->avoid_side = NULL;
    s->twist_limit = NULL;
    if (s->pl_mech_ok) mech3d_free(&s->pl_mech);
    if (s->pl_pit_ok) mech3d_free(&s->pl_pit);
    if (s->col_lod) {
        int k, r;
        for (k = 0; k <= s->actor_count; k++) for (r = 0; r < 3; r++) if (s->col_lod_ok[k][r] == 1) mech3d_free(&s->col_lod[k][r]);
    }
    free(s->col_lod); s->col_lod = NULL; free(s->col_lod_ok); s->col_lod_ok = NULL; free(s->col_repr); s->col_repr = NULL;
    s->pl_pit_ok = 0;
    s->units = NULL;
    s->armed = NULL;
    s->height = NULL;
    s->pl_mech_ok = 0;
}

int msim_nearest_enemy(const msim *s, float *distance)
{
    int i, best = -1;
    float bd = combat_max_range(&s->player_unit);
    for (i = 0; i < s->actor_count; i++) {
        float dx, dz, d;
        if (!s->armed[i] || s->units[i].destroyed || s->units[i].weapon_count == 0 || s->actors[i].friendly) continue;
        dx = (float)s->actors[i].mech.origin[0] - s->player[0];
        dz = (float)s->actors[i].mech.origin[2] - s->player[2];
        d = sqrtf(dx * dx + dz * dz);
        if (d <= bd) { bd = d; best = i; }
    }
    if (distance) *distance = bd;
    return best;
}

/* world position of a vertex of a posed part */
static void part_vertex(const mech3d_part *p, const wtb_vertex *v, float out[3])
{
    if (p->has_rot) {
        const float *R = p->rot;
        out[0] = R[0] * v->x + R[1] * v->y + R[2] * v->z + (float)p->pos[0];
        out[1] = R[3] * v->x + R[4] * v->y + R[5] * v->z + (float)p->pos[1];
        out[2] = R[6] * v->x + R[7] * v->y + R[8] * v->z + (float)p->pos[2];
    } else {
        float c = cosf(p->yaw * 3.14159265f / 180.0f), sn = sinf(p->yaw * 3.14159265f / 180.0f);
        out[0] = c * v->x + sn * v->z + (float)p->pos[0];
        out[1] = (float)v->y + (float)p->pos[1];
        out[2] = -sn * v->x + c * v->z + (float)p->pos[2];
    }
}

/* segment o + t*d, t in [0,1] against triangle (Moller-Trumbore); returns t or -1 */
static float seg_tri(const float o[3], const float d[3], const float a[3], const float b[3], const float c[3])
{
    float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    float pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2], inv, tv[3], u, v, qv[3], t;
    if (fabsf(det) < 1e-6f) return -1;
    inv = 1.0f / det;
    tv[0] = o[0] - a[0]; tv[1] = o[1] - a[1]; tv[2] = o[2] - a[2];
    u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
    if (u < 0 || u > 1) return -1;
    qv[0] = tv[1] * e1[2] - tv[2] * e1[1]; qv[1] = tv[2] * e1[0] - tv[0] * e1[2]; qv[2] = tv[0] * e1[1] - tv[1] * e1[0];
    v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * inv;
    if (v < 0 || u + v > 1) return -1;
    t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
    return (t >= 0 && t <= 1) ? t : -1;
}

/* nearest part of `m` hit by the segment; returns its OBJL group (0 if none hit) and t */
static int seg_mech(const mech3d *m, const float o[3], const float d[3], float *tbest)
{
    int i, k, j, group = 0;
    *tbest = 2;
    for (i = 0; i < m->part_count; i++) {
        const mech3d_part *p = &m->parts[i];
        const wtb_object *ob;
        if (p->model.object_count < 1) continue;
        ob = &p->model.objects[0];
        for (k = 0; k < ob->poly_count; k++) {
            const wtb_poly *q = &ob->polys[k];
            float A[3], B[3], C[3], t;
            if (q->n < 3) continue;
            part_vertex(p, &ob->verts[q->idx[0]], A);
            for (j = 1; j + 1 < q->n; j++) {
                part_vertex(p, &ob->verts[q->idx[j]], B);
                part_vertex(p, &ob->verts[q->idx[j + 1]], C);
                t = seg_tri(o, d, A, B, C);
                if (t >= 0 && t < *tbest) { *tbest = t; group = p->group > 0 ? p->group : 3; }
            }
        }
    }
    return *tbest <= 1 ? group : 0;
}

int msim_roof_overhead(const msim *s, float x, float y, float z, int sky_root)
{
    const float from[3] = {x, y + 1.0f, z}, up[3] = {0, 2000000.0f, 0};
    if (!s->world) return 0;
    for (int i = 0; i < s->obstacle_count; i++) {
        const float *box = s->obstacles[i];
        if (x < box[0] || x > box[1] || z < box[2] || z > box[3]) continue;
        const int index = s->obstacle_part[i];
        const mech3d_part *part = &s->world->parts[index];
        if (part->hidden || part->facing || part->shadow || part->coll == 5 || mech3d_is_force_shield(part) ||
            mech3d_part_in_tree(s->world, index, sky_root)) continue;
        mech3d roof = {0};
        float t;
        roof.parts = (mech3d_part *)part;
        roof.part_count = 1;
        if (seg_mech(&roof, from, up, &t)) return 1;
    }
    return 0;
}


/* the volley's muzzle flashes at the weapon (engine 0x10043aa8: the weapon table's two muzzle types) */
static void muzzle(msim *s, int weapon, const float from[3], const mech3d *m, int slot, float heading)
{
    int k;
    if (!s->on_effect) return;
    /* the flash takes the mount node's world orientation (0x10045cc0 -> 0x10045d00 -> 0x10044a70) */
    if (m) { mech3d_mount_rot(m, slot, heading, s->fx_rot); s->fx_rot_ok = 1; }
    for (k = 0; k < 2; k++) {
        int t = fx_muzzle(weapon, k);
        if (t >= 0) s->on_effect(s->shot_user, t, from);
    }
    s->fx_rot_ok = 0;
}

/* weapon table +0x18 (3dfx MW2.DLL 0x1025a6d8 + 0x58 i): LRM 20/15/10/5, Streak SRM-6/4/2, Narc */
static const unsigned char GUIDED[COMBAT_WEAPON_COUNT] = {
    [0] = 1, [1] = 1, [2] = 1, [3] = 1, [7] = 1, [8] = 1, [9] = 1, [28] = 1,
    [32] = 1, [33] = 1, [34] = 1, [35] = 1, [36] = 1, [37] = 1, [38] = 1,
    [42] = 1, [61] = 1, [64] = 1, [65] = 1,
};
int msim_weapon_guided(int w) { return w >= 0 && w < COMBAT_WEAPON_COUNT && GUIDED[w]; }
static void car_inc(msim *s, int off);

static mech3d *player_hull(msim *s)
{
    if (s->pl_pit_ok && s->player_cockpit) return &s->pl_pit;
    if (s->player_external && s->player_col_repr && s->col_lod)
        return &s->col_lod[s->actor_count][s->player_col_repr - 1];
    return &s->pl_mech;
}
/* an actor's pieces in the collision list: those of the representation it is drawn at (3Dfx 0x100267d0 picks it each frame,
 * 0x10026330 -> 0x10026430 swaps the node objects: 0x10029a00 takes the old pieces out of the list, 0x1000db80 /
 * 0x1000ffd0(6) puts the new ones in; DOS 0x37580 -> 0x37060 -> 0x37190 alike) */
int msim_unit_repr(const msim *s, int k)
{
    return s->col_repr && k >= 0 && k < s->actor_count ? s->col_repr[k] : 0;
}
const mech3d *msim_unit_hull(const msim *s, int k)
{
    int r = msim_unit_repr(s, k);
    return r > 0 && s->col_lod && s->col_lod_ok[k][r - 1] == 1 ? &s->col_lod[k][r - 1] : &s->actors[k].mech;
}

static int dos_hull_newest_first(const void *a, const void *b)
{
    int x = ((const struct msim_dos_hull *)a)->sequence, y = ((const struct msim_dos_hull *)b)->sequence;
    return (x < y) - (x > y);
}

/* 0x37060 replaces a unit's piece objects when its representation changes.
 * Their replacements enter at the head (0x373c8 -> 0x1f950). */
static void dos_sync_hull_order(msim *s)
{
    if (!s->dos_hulls) return;
    int changed = 0;
    for (int q = 0; q <= s->actor_count; q++) {
        struct msim_dos_hull *h = &s->dos_hulls[q];
        const int i = h->unit;
        const mech3d *repr = i < 0 ? (s->pl_mech_ok ? player_hull(s) : NULL)
                                  : (s->armed[i] ? msim_unit_hull(s, i) : NULL);
        if (repr == h->repr) continue;
        h->repr = repr;
        h->sequence = s->clist_next + i + 2;
        changed = 1;
    }
    if (changed) {
        /* 0x37798 applies all changes in unit-table order. */
        s->clist_next += s->actor_count + 1;
        qsort(s->dos_hulls, (size_t)s->actor_count + 1, sizeof *s->dos_hulls, dos_hull_newest_first);
    }
}
static int32_t dos_lod_vector(const msim *s, int unit, const int32_t camera[3], int32_t *dy)
{
    if (unit < 0) {
        *dy = (int32_t)lroundf(s->player_unit.y + s->player_centre_h) - camera[1];
        return dos_level_distance((int32_t)lroundf(s->player[0]) - camera[0],
                                  (int32_t)lroundf(s->player[2]) - camera[2]);
    }
    const int32_t *p = s->actors[unit].mech.origin;
    float centre = s->centre_h ? s->centre_h[unit] : (float)s->actors[unit].mech.centre_h;
    *dy = (int32_t)lroundf(s->units[unit].y + centre) - camera[1];
    return dos_level_distance(p[0] - camera[0], p[2] - camera[2]);
}

/* DOS 0x37580. The first pass tracks successive horizontal distance records.
 * The second pass uses slant distance, with the cockpit and wrecks assigned separately. */
static void dos_select_lods(msim *s, const float cam[3], float a0)
{
    const int32_t scale = (int32_t)(a0 * 65536.0f);
    const int32_t t0 = (int32_t)(((int64_t)scale * 3600 + 32768) >> 16);
    const int32_t t1 = (int32_t)(((int64_t)scale * 8500 + 32768) >> 16);
    const int32_t t2 = (int32_t)(((int64_t)scale * 22500 + 32768) >> 16);
    const int32_t camera[3] = {(int32_t)lroundf(cam[0]), (int32_t)lroundf(cam[1]), (int32_t)lroundf(cam[2])};
    const int first = s->player_external && s->pl_mech_ok ? -1 : 0;
    int n1 = -2, n2 = -2, n3 = -2, count1 = 0;
    int32_t nearest = t0;
    for (int k = first; k < s->actor_count; k++) {
        unsigned char *r = k < 0 ? &s->player_col_repr : &s->col_repr[k];
        const combat_unit *u = k < 0 ? &s->player_unit : &s->units[k];
        if ((k < 0 || s->armed[k]) && u->destroyed && !u->intact_out) {
            *r = k >= 0;
            continue;
        }
        if (s->no_col_lod) {
            *r = 0;
            continue;
        }
        int32_t dy;
        int32_t level = dos_lod_vector(s, k, camera, &dy);
        *r = 0xfe;
        if (level < nearest) {
            nearest = level;
            n3 = n2;
            n2 = n1;
            n1 = k;
        }
    }
    for (int k = first; k < s->actor_count; k++) {
        unsigned char *r = k < 0 ? &s->player_col_repr : &s->col_repr[k];
        if (*r != 0xfe) continue;
        int32_t dy;
        int32_t level = dos_lod_vector(s, k, camera, &dy);
        int32_t d = dos_slant_distance(level, dy);
        if (k == n1 && d < t0) *r = 0;
        else if (k == n2 || (k == n3 && d < t1) || ((n2 == -2 || n3 == -2) && d < t1 && count1 < 3)) {
            *r = 1;
            count1++;
        } else *r = d >= t2 ? 3 : 2;
    }
}

static unsigned char pose_collision_lod(msim *s, int slot, unsigned char repr, const char *skel, const mech3d *base)
{
    while (repr > 0) {
        mech3d *m = &s->col_lod[slot][repr - 1];
        unsigned char *ready = &s->col_lod_ok[slot][repr - 1];
        if (!*ready && s->arch) {
            *ready = mech3d_load(s->arch, skel, repr, m) == 0 && m->part_count > 0 ? 1 : 2;
            if (*ready == 1) mech3d_use_skeleton(m, base);
            else mech3d_free(m);
        }
        if (*ready == 1) {
            mech3d_pose_like(m, base);
            break;
        }
        repr--;
    }
    return repr;
}

static void collision_lods(msim *s)
{
    float cam[3], a0 = s->cam_ok && s->cam_a0 > 0 ? s->cam_a0 : 2.0f;
    if (!s->col_repr) return;
    if (s->cam_ok) memcpy(cam, s->cam_eye, sizeof cam);
    else if (s->player_eye_ok) memcpy(cam, s->player_eye, sizeof cam);
    else { cam[0] = s->player[0]; cam[1] = s->player_unit.y + s->player_centre_h; cam[2] = s->player[2]; }
    s->player_col_repr = 0;
    if (msim_dos_int) dos_select_lods(s, cam, a0);
    else for (int k = 0; k < s->actor_count; k++) {
        const world_actor *ac = &s->actors[k];
        if (s->armed[k] && s->units[k].destroyed && !s->units[k].intact_out) { s->col_repr[k] = 1; continue; }
        if (s->no_col_lod) { s->col_repr[k] = 0; continue; }
        float dx = (float)ac->mech.origin[0] - cam[0], dy = s->units[k].y - cam[1], dz = (float)ac->mech.origin[2] - cam[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        s->col_repr[k] = d < a0 * 3600.0f ? 0 : d < a0 * 8500.0f ? 1 : d < a0 * 22500.0f ? 2 : 3;
    }
    if (!s->col_lod || !s->col_lod_ok) return;
    for (int k = 0; k < s->actor_count; k++) {
        world_actor *ac = &s->actors[k];
        s->col_repr[k] = pose_collision_lod(s, k, s->col_repr[k], ac->skel, &ac->mech);
    }
    if (s->player_col_repr)
        s->player_col_repr = pose_collision_lod(s, s->actor_count, s->player_col_repr, s->player_skel, &s->pl_mech);
}
void msim_camera_lods(msim *s, const msim_camera *camera)
{
    memcpy(s->cam_eye, camera->eye, sizeof s->cam_eye);
    s->cam_a0 = camera->lod_scale;
    s->cam_ok = 1;
    /* DOS 0x15da7 -> 0x35870 picks the draw representations after projectile upkeep. */
    if (msim_dos_int) {
        collision_lods(s);
        dos_sync_hull_order(s);
        dos_prepare_scenery(s, camera);
    }
}
/* the engine's tick clock (DAT_1024ab44) */
static int32_t sim_ticks(const msim *s) { return (int32_t)((int64_t)s->now * 182 / 1000); }
/* 0x1003b740: an integer servo step - the 32-bit product (imul), divided toward zero (idiv), snapped to the target
 * when the step reaches or passes it */
static int32_t iservo_step(msim_iservo *v, int32_t now)
{
    int32_t d = v->target - v->value, st;
    if (d != 0) {
        st = (int32_t)((uint32_t)(now - v->t) * (uint32_t)d) / (v->tau > 0 ? v->tau : 1);
        if ((d > 0 && st >= d) || (d < 0 && st <= d)) v->value = v->target;
        else v->value += st;
    }
    v->t = now;
    return v->value;
}
/* 0x10044810: one aim range update. The +0xa8 servo first (its value +0xb0, the hit range, back toward 50000 with tau
 * 20 s); then the aim ray: a unit struck (object class 0x300) sets +0xb0 to the distance, 2000 at the least
 * (0x10248598; nothing struck, or something else nearer: +0xb0 keeps its value); the +0x98 servo (value +0xa0) then
 * follows +0xb0 with tau 0.2 s. Both set up at 50000 (0x10018a20 -> 0x1003b700: tau = seconds x 181). Returns +0xa0. */
static float aim_range_step(msim *s, msim_aim *a, int hit, float dist)
{
    int32_t now = sim_ticks(s);
    if (!a->ok) {
        a->range.t = a->conv.t = 0;   /* set up with the unit at the mission's start (0x10018a20) */
        a->range.target = a->range.value = a->conv.target = a->conv.value = 50000;
        a->range.tau = msim_dos_int ? 3640 : 3620; a->conv.tau = 36;   /* 20 s / 0.2 s: DOS 0x24bf5 chop(20 x 182) = 3640 */
        a->ok = 1;
    }
    iservo_step(&a->range, now);
    if (hit) a->range.value = dist <= 2000.0f ? 2000 : (int32_t)dist;   /* _ftol */
    a->conv.target = a->range.value;
    return (float)iservo_step(&a->conv, now);
}
static int shot_objects(msim *s, const float o[3], const float dv[3], float *tb, int *part);
static int dos_shot_contact(msim *s, int owner, const float o[3], const float dv[3],
                            int *unit, int *group, int *part, float contact[3]);
static int dos_aim_contact(msim *s, int owner, const float o[3], const float direction[3],
                           int *unit, int *part, float *distance);
static void dos_impact_effect(const float dv[3], const float hit[3], float out[3]);
static int shot_thing(const msim *s, int part);
/* The aim ray (DOS 0x53190 -> 0x1c3f0, 3Dfx 0x10044950 -> 0x100103e0) traces 150000 cm from the eye.
 * Returns 1 with the distance to the nearest unit or thing, 0 when blocked by scenery or when nothing was hit. */
static int aim_ray(msim *s, int self, const float from[3], const float dir[3], float *dist)
{
    float best = 150000.0f, tb = 2.0f;
    int q, hit = 0, part = -1, hitq = -1, hitb = -1;
    if (msim_dos_int) {
        int unit = -2;
        if (!dos_aim_contact(s, self, from, dir, &unit, &part, dist)) return 0;
        hitb = part >= 0 ? shot_thing(s, part) : -1;
        if (unit == -2 && hitb < 0) return 0;
        if (self == -1 && (unit >= 0 || hitb >= 0)) {
            s->reticle_actor = unit >= 0 ? unit : -1;
            s->reticle_bld = hitb;
        }
        return 1;
    }
    for (q = -1; q < s->actor_count; q++) {
        float c[3], t, px, py, pz, rr, fraction;
        int group;
        mech3d *mq;
        if (q == self) continue;
        if (q < 0) {
            if (!s->pl_mech_ok || s->player_unit.destroyed) continue;
            mq = player_hull(s);
            c[0] = s->player[0] - from[0]; c[1] = s->player_unit.y + (s->player_centre_h > 0 ? s->player_centre_h : 475.0f) - from[1]; c[2] = s->player[2] - from[2];
            rr = s->player_radius > 1200.0f ? s->player_radius : 1200.0f;
        } else {
            if (!s->armed[q] || s->units[q].destroyed) continue;
            mq = (mech3d *)msim_unit_hull(s, q);
            c[0] = (float)mq->origin[0] - from[0];
            c[1] = s->units[q].y + (s->centre_h ? s->centre_h[q] : s->height[q] * 0.5f) - from[1];
            c[2] = (float)mq->origin[2] - from[2];
            rr = s->radius ? s->radius[q] : 450.0f;
            rr = rr > s->height[q] * 0.75f ? rr : s->height[q] * 0.75f;   /* the whole mech inside */
        }
        t = c[0] * dir[0] + c[1] * dir[1] + c[2] * dir[2];
        px = c[0] - dir[0] * t; py = c[1] - dir[1] * t; pz = c[2] - dir[2] * t;
        if (t <= 0 || px * px + py * py + pz * pz > rr * rr) continue;
        /* Trace from the eye to the current closest contact. Splitting the ray into short
         * segments could move a hit on a segment boundary to the end of the following segment. */
        const float ray[3] = {dir[0] * best, dir[1] * best, dir[2] * best};
        if (mech3d_hull_hit(mq, from, ray, mech3d_fly_mask(q < 0 ? s->player_unit.loc_gone : s->units[q].loc_gone),
                           &fraction, &group) && fraction < 1.0f) {
            best *= fraction;
            hit = 1;
            hitq = q;
        }
    }
    {   /* world objects nearer: a thing's object (0x200) is a unit struck, anything else stops the ray */
        float dv[3] = {dir[0] * best, dir[1] * best, dir[2] * best};
        if (s->world && s->terrain && shot_objects(s, from, dv, &tb, &part) && tb < 1.0f) {
            hitb = shot_thing(s, part);
            hit = hitb >= 0; hitq = -1;
            best *= tb;
        }
    }
    if (hit && los_blocked(s, from[0], from[1], from[2], from[0] + dir[0] * best, from[1] + dir[1] * best, from[2] + dir[2] * best)) hit = 0;   /* the ground nearer */
    if (hit && self == -1 && !(hitq < 0 && hitb < 0)) {   /* the player's: the unit struck becomes the reticle's (0x1004489f) */
        s->reticle_actor = hitq >= 0 ? hitq : -1;
        s->reticle_bld = hitq >= 0 ? -1 : hitb;
    }
    *dist = best;
    return hit;
}

/* DOS 0x3fa50: use the octagonal length, rounded down, then divide toward zero into 16.16. */
static int32_t dos_normalize(int32_t v[3])
{
    int64_t x = llabs(v[0]), y = llabs(v[1]), z = llabs(v[2]);
    int64_t max = x > y ? x : y;
    if (z > max) max = z;
    int32_t len = (int32_t)((3 * max + x + y + z) / 4);
    if (len)
        for (int i = 0; i < 3; i++) v[i] = (int32_t)((int64_t)v[i] * 65536 / len);
    return len;
}

static int32_t dos_projectile_mul(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b + 32768) >> 16);
}

static int32_t dos_gravity(float gravity_g)
{
    return dos_projectile_mul((int32_t)lrintf(gravity_g * 65536.0f), 0x794);   /* PLNT 0x4e62e */
}

/* DOS 0x5345c..0x53553: one random offset along each of the two launch-plane directions. */
static void dos_missile_offset(msim_shot *sh, const int32_t d[3], int32_t off)
{
    int32_t p[3] = {-d[2], 0, d[0]};
    dos_normalize(p);
    sh->x += (float)(dos_projectile_mul(off, p[0]) +
                    dos_projectile_mul(off, dos_projectile_mul(d[1], p[2])));
    sh->y += (float)dos_projectile_mul(off, dos_projectile_mul(d[2], p[0]) + dos_projectile_mul(d[0], p[2]));
    sh->z += (float)(dos_projectile_mul(off, p[2]) +
                    dos_projectile_mul(off, -dos_projectile_mul(d[1], p[0])));
}

/* DOS 0x52710 chooses the first free slot of the requested kind. The dense array stays sorted
 * by those slots so that the projectile update resolves simultaneous impacts in the same order. */
static msim_shot *alloc_shot(msim *s, int kind)
{
    int pos = s->shot_count, slot = -1;
    if (s->shot_count >= MSIM_MAX_SHOTS) return NULL;
    if (msim_dos_int && s->dos_shot_slots) {
        for (slot = 0, pos = 0; slot < s->dos_shot_slots; slot++) {
            if (pos < s->shot_count && s->shots[pos].dos_slot == slot) pos++;
            else if (s->dos_shot_kinds[slot] == kind) break;
        }
        if (slot == s->dos_shot_slots) return NULL;
        memmove(s->shots + pos + 1, s->shots + pos, (size_t)(s->shot_count - pos) * sizeof *s->shots);
    }
    s->shot_count++;
    s->shots[pos].dos_slot = slot;
    return &s->shots[pos];
}

static void remove_shot(msim *s, int at)
{
    s->shot_count--;
    if (msim_dos_int && s->dos_shot_slots)
        memmove(s->shots + at, s->shots + at + 1, (size_t)(s->shot_count - at) * sizeof *s->shots);
    else
        s->shots[at] = s->shots[s->shot_count];
}

static msim_shot *spawn_shot_h(msim *s, int weapon, int owner, const float from[3], const float to[3], float scatter, int homing)
{
    msim_shot *sh;
    float d[3], len, sp = combat_weapon_speed(weapon);
    int32_t dos_dir[3];
    int kind = combat_weapon_kind(weapon);
    if (owner == -1 && msim_dos_int) car_inc(s, 0x13);   /* 0x52671: counts before testing for a free projectile */
    if (sp <= 0 || (msim_dos_int && !combat_weapon_has_object(weapon))) return NULL;
    sh = alloc_shot(s, kind);
    if (!sh) return NULL;
    if (msim_dos_int) {
        for (int i = 0; i < 3; i++) dos_dir[i] = (int32_t)to[i] - (int32_t)from[i];
        dos_normalize(dos_dir);   /* 0x527dc, after subtracting the mount position from the convergence point */
        for (int i = 0; i < 3; i++) sh->dos_vel[i] = dos_dir[i] * (int32_t)sp;
        sh->vx = (float)sh->dos_vel[0] / 65536.0f;
        sh->vy = (float)sh->dos_vel[1] / 65536.0f;
        sh->vz = (float)sh->dos_vel[2] / 65536.0f;
        sh->x = (float)(int32_t)from[0]; sh->y = (float)(int32_t)from[1]; sh->z = (float)(int32_t)from[2];
        if (kind == 3 || kind == 4) dos_missile_offset(sh, dos_dir, (int32_t)ms_roll(s, 200) - 100);
    } else {
        d[0] = to[0] - from[0] + (frand_ms(&s->rng) - 0.5f) * 2.0f * scatter;
        d[1] = to[1] - from[1] + (frand_ms(&s->rng) - 0.5f) * 2.0f * scatter;
        d[2] = to[2] - from[2] + (frand_ms(&s->rng) - 0.5f) * 2.0f * scatter;
        len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 1) len = 1;
        sh->x = from[0]; sh->y = from[1]; sh->z = from[2];
        sh->vx = d[0] / len * sp; sh->vy = d[1] / len * sp; sh->vz = d[2] / len * sp;
        /* 0x10044ac0: move a missile's start off the mount in the plane perpendicular to its direction. */
        if (kind == 3 || kind == 4) {
            float ux = d[0] / len, uy = d[1] / len, uz = d[2] / len, hl = sqrtf(ux * ux + uz * uz);
            float off = (float)((int)(rnd_ms(&s->rng) % 200u) - 100);
            if (hl > 0) {
                float px = -uz / hl, pz = ux / hl;
                sh->x += off * (px + uy * pz);
                sh->y += off * (ux * pz + uz * px);
                sh->z += off * (pz - uy * px);
            }
        }
    }
    sh->life = (float)combat_weapon_life(weapon);
    sh->weapon = weapon;
    sh->owner = owner;
    if (owner == -1 && !msim_dos_int) car_inc(s, 0x13);
    sh->ax = sh->az = 0;
    sh->ay = -(float)combat_weapon_drop(weapon) *
             (msim_dos_int ? (float)dos_gravity(s->planet.gravity_g) / 65536.0f : 0.0296f * s->planet.gravity_g);
    sh->homing = msim_weapon_guided(weapon) ? homing : -2;   /* 0x100437a0: the mech's target when guided and locked */
    sh->age = 0;
    sh->prox = 0;
    sh->first = 0;
    sh->id = ++s->shot_serial;
    /* the player's newest MISSILE (projectile type 3 / 4) - the one F10 / F9 follow (0x10044xxx: DAT_1025a69c, kept only for
     * a missile): every shot had counted, so after a laser or a gun F10 had nothing to follow (the GBL port's DOSBox check:
     * an LRM fired, then the machine gun, then F10 - the camera takes the missile) */
    if (owner == -1 && (combat_weapon_kind(weapon) == 3 || combat_weapon_kind(weapon) == 4)) s->player_last_shot = sh->id;
    return sh;
}

static void car_inc(msim *s, int off)
{
    unsigned v = (unsigned)(s->career[off] | s->career[off + 1] << 8) + 1;
    s->career[off] = (uint8_t)v; s->career[off + 1] = (uint8_t)(v >> 8);
}
int msim_write_career(const msim *s, const char *path)
{
    FILE *f = fopen(path, "wb");
    uint8_t b[80];
    if (!f) return -1;
    memcpy(b, s->career, sizeof b);
    /* end status DAT_102441dd (0x10016280): 1 normal, 2 ejected, 4 automatic ejection aborted (hostile atmosphere) */
    b[0x1d] = s->player_unit.ejected == 1 ? 2 : s->player_unit.ejected == 2 ? 4 : 1;
    fwrite(b, 1, sizeof b, f);
    fclose(f);
    return 0;
}

static int order_attacks_player(const msim *s, int i)
{
    int cur = s->actors[i].table < s->logic.table_count ? s->logic.current[s->actors[i].table] : -1;
    const mtbl_node *n;
    if (cur < 0) return 0;
    n = &s->logic.tables[s->actors[i].table].nodes[cur];
    return (n->kind == MTBL_K_DESTROY1 || n->kind == MTBL_K_DESTROY2) && is_player_rec(s, n->target);
}

/* world part q is the intact object of a destroyed thing: its object entry carries 0x200 (0x10025180: +0xc |= 0xa00),
 * which 0x10025310 (DOS 0x362d0) never brings back - only hidden entries WITHOUT 0x200 are created again under a
 * shown parent */
static int part_destroyed(const msim *s, int q)
{
    int b;
    for (b = 0; b < s->bld_count; b++) if (s->bld[b].intact == q && s->bld[b].hp <= 0) return 1;
    return 0;
}

/* the swap of a destroyed object (engine 0x10025180): its thing is flagged destroyed (+0 |= 4, what the objective
 * nodes test, 0x10009a50), the intact object and its children hidden, the destroyed variant shown */
static void building_swap(msim *s, int b)
{
    struct msim_building *bd = &s->bld[b];
    mech3d_part *pi, *pd;
    bd->hp = 0;
    pi = (mech3d_part *)&s->world->parts[bd->intact];
    pd = bd->destroyed >= 0 ? (mech3d_part *)&s->world->parts[bd->destroyed] : NULL;
    /* a carried object (path task): its tasks end, and the destroyed variant takes its current world matrix
     * (0x10025180: 0x1000fce0 -> 0x1000fcc0) - placed before its debris children are launched below */
    if (s->paths) world3d_paths_destroyed(s->paths, (mech3d *)s->world, bd->intact, bd->destroyed);
    pi->hidden = 1;
    {   /* the intact object's own children go with it (0x10025310) */
        int q, changed = 1;
        while (changed) {
            changed = 0;
            for (q = 0; q < s->world->part_count; q++) {
                mech3d_part *c = (mech3d_part *)&s->world->parts[q];
                int j;
                if (c->hidden || c->parent_obj < 0 || strcmp(c->rec, pi->rec) != 0) continue;
                for (j = 0; j < s->world->part_count; j++)
                    if (s->world->parts[j].obj_index == c->parent_obj && strcmp(s->world->parts[j].rec, c->rec) == 0) {
                        if (s->world->parts[j].hidden && s->world->parts[j].gt_debris != 2 && (j == bd->intact || &s->world->parts[j] != pd)) {
                            if (j == bd->intact || s->world->parts[j].hidden) { c->hidden = 1; changed = 1; }
                        }
                        break;
                    }
            }
        }
    }
    if (pd) {   /* the destroyed variant appears (0x10025450) with its children (0x10025310): GT 0x8000 ones fly off as debris -
                 * only with CHUNKY EXPLOSIONS on and room in the 128-piece pool (0x100254ad) - the rest just show */
        int q, changed = 1;
        int32_t now = (int32_t)((int64_t)s->now * 182 / 1000);
        pd->hidden = 0;
        if (s->clist_seq && bd->destroyed < s->clist_seq_n) s->clist_seq[bd->destroyed] = ++s->clist_next;   /* created now: the
                                                                         * head of the collision list (0x10025450 -> 0x1000dc60) */
        while (changed) {
            changed = 0;
            for (q = 0; q < s->world->part_count; q++) {
                mech3d_part *c = (mech3d_part *)&s->world->parts[q];
                int j, parent_shown = 0;
                if (!c->hidden || c->parent_obj < 0 || c->gt_debris == 2 || (c->sparse && world3d_density_low) ||
                    !mech3d_part_in_tree(s->world, q, bd->destroyed)) continue;
                /* 0x10025310: an entry flagged 0x200 - a destroyed thing's intact object, this one too - stays gone,
                 * even under a parent still standing (ORANARE1's pump-station tanks S_BPUMPE hang on S_BPUMPB) */
                if (part_destroyed(s, q)) continue;
                for (j = 0; j < s->world->part_count; j++)
                    if (s->world->parts[j].obj_index == c->parent_obj && strcmp(s->world->parts[j].rec, c->rec) == 0) {
                        parent_shown = !s->world->parts[j].hidden || s->world->parts[j].gt_debris == 2;
                        break;
                    }
                if (!parent_shown || j == bd->intact) continue;
                if (c->gt_debris) {
                    c->gt_debris = 2;   /* spent: launched (or dropped) once */
                    /* Split wreck models (such as VCDHELOA and VCDDROPX) flag their pieces by WTB layout. */
                    unsigned split_flag = c->model.extended ? 0x1000u : 0x2000u;
                    int pieces = c->model.object_count;
                    if (pieces > 0 && !(c->model.objects[0].flags & split_flag)) pieces = 1;
                    for (int piece = 0; s->chunky && piece < pieces && s->debris_count < MSIM_MAX_DEBRIS; piece++) {
                        msim_debris *dd = &s->debris[s->debris_count++];
                        int k;
                        dd->dos_chunk = 0;
                        dd->kind = 3; dd->actor = -2; dd->part = q; dd->object = piece;
                        for (k = 0; k < 3; k++) dd->p[k] = (float)c->pos[k];
                        int32_t sphere[4] = {0};
                        if (msim_dos_int) mech3d_dos_model_sphere(&c->model.objects[piece], sphere);
                        debris_start_motion(s, dd, sphere[3], c);
                        dd->until = now + (msim_dos_int ? 0xe38 : 0xe24);
                    }
                } else {
                    c->hidden = 0;
                    if (s->clist_seq && q < s->clist_seq_n) s->clist_seq[q] = ++s->clist_next;
                }
                changed = 1;
            }
        }
    }
}

/* engine 0x10046c20: hit points minus the damage; at 0 an explosion at the object (0x10045c30) and the swap to its
 * destroyed variant (0x10046a90 -> 0x10025290 -> 0x10025180). The swap marks the object entry destroyed (+0xc |= 0x200)
 * and 0x10025310 then runs 0x10025180 on every object whose PARENT entry carries 0x200: a structure placed as the child
 * of another (YELLARE6: the chemical plant's Exhaust-Transferal, Exhaust-Piping and Coolant-Piping hang on the
 * Processor) is destroyed with it - its thing flagged destroyed (no explosion, no hit points taken, no kill credit),
 * its destroyed variant shown. Objects without a thing only hide (0x800). */
static void building_damage(msim *s, int b, float dmg, int owner, const float at[3])
{
    struct msim_building *bd = &s->bld[b];
    mech3d_part *pi;
    char line[96];
    if (mw2_development_env("MW2_DAMAGE_LOG"))   /* TEST ONLY: who damages which structure */
        printf("damage structure %s by %s %.3f\n", bd->name, owner < 0 ? (owner == -1 ? "player" : "none") : s->actors[owner].group, (double)dmg);
    int changed = 1;
    (void)owner;
    if (bd->hp <= 0) return;
    if (mw2_development_env("MW2_BLD_TRACE")) fprintf(stderr, "bld hit %s (%s) %.1f -> hp %d at %.0f,%.0f,%.0f by %d\n", bd->name, bd->sub, (double)dmg, bd->hp - (int)dmg, at ? (double)at[0] : 0.0, at ? (double)at[1] : 0.0, at ? (double)at[2] : 0.0, owner);
    bd->hp -= (int)dmg;   /* __ftol: truncated */
    if (bd->hp > 0) return;
    building_swap(s, b);
    pi = (mech3d_part *)&s->world->parts[bd->intact];
    while (changed) {   /* 0x10025310: children of destroyed entries */
        int c;
        changed = 0;
        for (c = 0; c < s->bld_count; c++) {
            const mech3d_part *ci = &s->world->parts[s->bld[c].intact];
            int p;
            if (s->bld[c].hp <= 0 || ci->parent_obj < 0) continue;
            for (p = 0; p < s->bld_count; p++) {
                const mech3d_part *pp = &s->world->parts[s->bld[p].intact];
                if (p == c || s->bld[p].hp > 0 || pp->obj_index != ci->parent_obj || strcmp(pp->rec, ci->rec) != 0) continue;
                building_swap(s, c);
                snprintf(line, sizeof line, "%6.1fs %s (%s) destroyed with %s", s->now / 1000.0, s->bld[c].name, s->bld[c].sub, s->bld[p].sub);
                if (s->log) s->log(s->log_user, line);
                changed = 1;
                break;
            }
        }
    }
    {   /* at the impact point (0x10046c20): class 0xb0 things one explosion 0x0d, everything else effect 3 and a debris
         * chunk (0x20b -> 0x10045d00 case 0xb -> 0x10046d40) */
        float p[3] = {at ? at[0] : (float)pi->pos[0], at ? at[1] : (float)pi->pos[1] + 200.0f, at ? at[2] : (float)pi->pos[2]};
        if ((bd->type & 0xf0) == 0xb0 || (pi->objtype & 0xf0) == 0xb0) effect(s, 0x0d, p, 1);
        else { effect(s, 3, p, 1); spawn_debris(s, 4, p); }   /* 0x20b: (0x200 >> 8) & 2 -> four chunks */
    }
    s->buildings_destroyed++;
    s->world_dirty = 1;
    s->world_rebuild = 1;                        /* the collision surfaces without it: rebuilt once per step */
    snprintf(line, sizeof line, "%6.1fs %s (%s) destroyed", s->now / 1000.0, bd->name, bd->sub);
    if (s->log) s->log(s->log_user, line);
    if (mw2_development_env("MW2_BLD_TRACE")) {   /* TEST ONLY: what of the record stands after the swap (model/OBJ index) */
        int q;
        fprintf(stderr, "bld shown %s after %d:", pi->rec, b);
        for (q = 0; q < s->world->part_count; q++)
            if (!s->world->parts[q].hidden && strcmp(s->world->parts[q].rec, pi->rec) == 0) fprintf(stderr, " %s/%d", s->world->parts[q].model_name, s->world->parts[q].obj_index);
        fprintf(stderr, "\n");
    }
}

int msim_aim_thing(const msim *s, float yaw_deg, float cone_deg, float *distance)
{
    int b, best = -1;
    float bd = 1e30f;
    for (b = 0; b < s->bld_count; b++) {
        const struct msim_building *g = &s->bld[b];
        float cx = (g->mn[0] + g->mx[0]) * 0.5f - s->player[0], cz = (g->mn[2] + g->mx[2]) * 0.5f - s->player[2];
        float d = sqrtf(cx * cx + cz * cz), off = atan2f(cx, cz) * 180.0f / 3.14159265f - yaw_deg;
        if (g->hp <= 0 || (s->world && s->world->parts[g->intact].hidden)) continue;
        while (off > 180) off -= 360;
        while (off < -180) off += 360;
        if (fabsf(off) > cone_deg || d > 182000.0f) continue;
        if (d < bd) { bd = d; best = b; }
    }
    if (distance) *distance = bd;
    return best;
}

/* DOS MW2.EXE: at the load every thing record's +0x10 = its object's model +0x40 (0x4fd5f..0x4fdad: thing +4 -> 0x36790
 * -> +0x6c -> +0x40), the model's bounding radius from 0x37f60: the centre of the vertex box ((min + max) >> 1, integers),
 * the farthest vertex from it, its square root truncated (0x38086 fsqrt, 0x5c3c2 frndint with RC = chop, fistpl) - of the
 * vertices as loaded (OBJ scale applied, 0x4fef0); cm */
static int thing_model_radius(const mech3d_part *p)
{
    if (p->model.object_count < 1) return 0;
    int32_t sphere[4];
    mech3d_dos_model_sphere(&p->model.objects[0], sphere);
    return sphere[3];
}

int msim_inspect(msim *s, int actor, int thing)
{
    float dx, dz, r;
    /* engine 0x1001d510 (DOS 0x2a7xx): the horizontal distance (+0xc0, 0x1001d970) against the radius + 200 m; 1 inspected,
     * 2 out of range, 3 inspected before */
    if (actor >= 0 && actor < s->actor_count) {
        if (!s->inspected) return 0;
        if (s->inspected[actor]) return 3;                                 /* inspected before (DAT_1024c8ac = 3) */
        dx = (float)s->actors[actor].mech.origin[0] - s->player[0]; dz = (float)s->actors[actor].mech.origin[2] - s->player[2];
        r = (s->radius ? s->radius[actor] : 450.0f) + 20000.0f;           /* the unit's radius +0xe8 (MGEO int[6]) + 200 m (0x102474d0) */
        if (dx * dx + dz * dz >= r * r) return 2;                          /* inspected only while distance < radius + 200 m */
        s->inspected[actor] = 1;
        return 1;
    }
    if (thing >= 0 && thing < s->bld_count) {
        struct msim_building *g = &s->bld[thing];
        if (g->inspected) return 3;
        dx = (g->mn[0] + g->mx[0]) * 0.5f - s->player[0]; dz = (g->mn[2] + g->mx[2]) * 0.5f - s->player[2];
        r = 20000.0f;   /* the thing record's +0x10 + 200 m: the 3Dfx DLL never sets +0x10 (0; the loader 0x1003eb5c and the clear
                         * 0x10018920 leave it), so 200 m from its point; DOS sets it to the model's bounding radius (0x4fdad) */
        if (msim_dos_int && s->world && g->intact >= 0 && g->intact < s->world->part_count)
            r += (float)thing_model_radius(&s->world->parts[g->intact]);   /* DOS 0x2a8d2: +0x10 + 0x4e20 against +0xc0 */
        if (mw2_development_env("MW2_STATE_DUMP")) printf("INSPECT_THING d=%.0f r=%.0f\n", (double)sqrtf(dx * dx + dz * dz), (double)r);   /* TEST hook (tests/sequences) */
        if (dx * dx + dz * dz >= r * r) return 2;   /* DOS 0x2a8e3: out of range when radius + 200 m <= the distance */
        g->inspected = 1;
        return 1;
    }
    return 0;
}

void msim_damage_building(msim *s, int b, float dmg) { float p[3] = {0, 0, 0}; if (b >= 0 && b < s->bld_count) building_damage(s, b, dmg, -1, p); }

/* a 50 m sidestep (0x10020ac0 -> 0x100204e0 probes, 0x100103e0): clear when the probe hits nothing - world or unit */
static int probe_engine(const msim *s, int self, const float p0[3], const float p1[3], int *climb, float *ox, float *oz);
struct side_ctx { const msim *s; int i; };
static int side_free(void *ctx, int side)
{
    const struct side_ctx *c = ctx;
    const world_actor *ac = &c->s->actors[c->i];
    float h = (ac->mech.heading + 90.0f * (float)side) * 3.14159265f / 180.0f, ox, oz;
    float yc = c->s->units[c->i].y + (c->s->centre_h ? c->s->centre_h[c->i] : 550.0f);
    float p0[3] = {(float)ac->mech.origin[0], yc, (float)ac->mech.origin[2]}, p1[3] = {p0[0] + sinf(h) * 5000.0f, yc, p0[2] + cosf(h) * 5000.0f};
    int climb;
    return !probe_engine(c->s, c->i, p0, p1, &climb, &ox, &oz);
}
/* a guided launch at AI mech t by a shooter of pilot level lv (the player 1): engine 0x10020ac0 */
static void missile_warning(msim *s, int t, int lv, float sx, float sz, int in_view)
{
    ai_view v;
    struct side_ctx c;
    float rel;
    if (!s->minds || !s->ai_ok || t < 0 || t >= s->actor_count || !s->armed[t] || s->units[t].destroyed) return;
    rel = atan2f(sx - (float)s->actors[t].mech.origin[0], sz - (float)s->actors[t].mech.origin[2]) * 57.29578f - s->actors[t].mech.heading;   /* the shooter off the target's heading (DOS 0x2e5ec..) */
    memset(&v, 0, sizeof v);
    v.can_jump = s->units[t].jets > 0 && !s->units[t].no_jump && s->units[t].jet_fuel > 5.0f;
    v.heat = s->units[t].heat;
    v.pilot_level = s->actors[t].ai_level >= 1 && s->actors[t].ai_level <= 4 ? s->actors[t].ai_level : 1;
    c.s = s; c.i = t;
    if (msim_dos_int) ai_dos_missile_react(&s->minds[t], &v, in_view, rel, side_free, &c);
    else ai_missile_warning(&s->minds[t], &v, lv, in_view, rel, side_free, &c);
}

/* DOS 0x2e450 consumes the shooter's roll before testing the weapon and
 * resolving the tracked unit. A shot without a warning recipient still uses the roll. */
static int dos_warning_roll(msim *s, int weapon, int level)
{
    return level <= 2 && ms_roll(s, 3) != 0 && (msim_weapon_guided(weapon) || weapon == 0x15);
}

/* DOS 0x2ea8d: selection can grant a lock before the line-of-fire test.
 * The guided weapon's warning also plays before that test. */
static int dos_weapon_selection_effects(msim *s, int weapon, int skill, int target)
{
    if (!msim_weapon_guided(weapon)) return 0;
    int range_byte = (skill > 0 ? skill : 1) + 1;
    int locked = range_byte <= 4 && ms_roll(s, (unsigned)range_byte + 1u) == 0;
    if (target == -1 && s->on_sound) s->on_sound(s->shot_user, 0x6f, NULL);
    return locked;
}

/* 0x2e860 picks the weapon and writes the trigger in the AI update.
 * The DOS weapon controller consumes the trigger immediately after this decision. */
static void dos_ai_fire_decision(msim *s, int actor, const ai_view *view, int target_unit)
{
    ai_mind *mind = &s->minds[actor];
    combat_unit *unit = &s->units[actor];
    const world_actor *ac = &s->actors[actor];
    if (mind->rule_fired) return;
    unit->ai_trigger = 0;
    if (!mind->wants_fire || !view->has_enemy) return;

    const int thing = MSIM_IS_THING(target_unit) ? MSIM_THING_OF(target_unit) : -1;
    const int target = target_unit >= -1 || thing >= 0 ? target_unit : -2;
    unit->lock_target = mind->lock & 0x80 ? target : -2;
    int slot = combat_ai_select(unit, view->enemy_dist, ms_roll, s);
    if (slot < 0) return;
    if (dos_weapon_selection_effects(s, unit->weapons[slot].weapon, ac->ai_skill, target_unit))
        unit->lock_target = target;

    const float centre[3] = {(float)ac->mech.origin[0],
                             unit->y + (s->centre_h ? s->centre_h[actor] : s->height[actor] * 0.7f),
                             (float)ac->mech.origin[2]};
    float aim[3] = {view->enemy_x, view->target_world_height, view->enemy_z};
    if (thing >= 0) thing_point(s, thing, aim);
    const int32_t torso = (int32_t)llround((ac->mech.heading + mind->twist) * 65536.0);
    const int32_t bearing = dos_atan2((int32_t)(aim[0] - centre[0]), (int32_t)(aim[2] - centre[2]));
    int32_t angle = (torso - bearing) % 0x1680000;
    if (angle < 0) angle += 0x1680000;
    unit->ai_trigger = (angle <= 0x5a0000 || angle >= 0x10e0000) &&
                       dos_clear_to(s, actor, target_unit, thing, centre, aim);
}

static void dos_player_warning(msim *s, const float aim[3], int react)
{
    int t = s->dos_warning_target;
    if (!s->dos_warning_view || t < 0 || t >= s->actor_count || !s->armed[t] || s->units[t].destroyed) return;
    const int32_t *origin = s->actors[t].mech.origin;
    const float target[3] = {(float)origin[0], s->units[t].y + (s->centre_h ? s->centre_h[t] : 550.0f), (float)origin[2]};
    const float shooter[3] = {s->player[0], s->player_unit.y + s->player_centre_h, s->player[2]};
    int in_view = s->dos_warning_view(s->shot_user, aim, target, shooter, s->player_facing);
    if (mw2_development_env("MW2_MISSILE_TRACE")) fprintf(stderr, "mwarn unit %d view %d\n", t, in_view);
    if (react) missile_warning(s, t, 1, shooter[0], shooter[2], in_view);
}

/* the units a shot's step o..o+dv enters (0x10044f60 -> 0x100103e0): the player and actors near the segment, the
 * shooter's own skipped (+0x14). A unit's pieces stay in the collision list when it is destroyed (only blown-off ones
 * leave, 0x100156f0 -> 0x10015be0): a standing wreck stops shots (no damage). The nearest piece entered nearer than
 * *tbest (a fraction of dv) sets *tbest, *unit and *group (its location, 0 none) */
static void shot_units(msim *s, int owner, const float o[3], const float dv[3], float *tbest, int *unit, int *group)
{
    int i, g;
    float t;
    for (i = -1; i < s->actor_count; i++) {
        const mech3d *m;
        float cx, cz, ex, ez, lx, lz, len2, u, px, pz;
        if (i == owner) continue;
        if (i == -1) { if (!s->pl_mech_ok || s->player_unit.destroyed) continue; m = player_hull(s); }
        else { if (!s->armed[i]) continue; m = msim_unit_hull(s, i); }
        cx = (float)m->origin[0]; cz = (float)m->origin[2];
        /* coarse: horizontal distance from the segment to the unit < 20 m */
        ex = dv[0]; ez = dv[2]; lx = cx - o[0]; lz = cz - o[2];
        len2 = ex * ex + ez * ez;
        u = len2 > 0 ? (lx * ex + lz * ez) / len2 : 0;
        if (u < 0) u = 0;
        if (u > 1) u = 1;
        px = o[0] + ex * u - cx; pz = o[2] + ez * u - cz;
        {   /* the port's coarse cull (the engine tests the whole collision list): 20 m, or as far as the unit's pieces reach
             * - RED_SCN1's dropship (TURRETE) hangs its hull 93 m off the unit's origin (its OBJ 1: -8382, 1805, 4020),
             * which a fixed 20 m left unhittable (GitHub report: the lasers passed through it) */
            float reach = 2000.0f;
            int q;
            for (q = 0; q < m->part_count; q++) {
                const mech3d_part *pp = &m->parts[q];
                float qx = (float)pp->pos[0] - cx, qz = (float)pp->pos[2] - cz, rq = sqrtf(qx * qx + qz * qz) + (pp->sphere_ok ? pp->hull_radius : 1500.0f);
                if (rq > reach) reach = rq;
            }
            if (px * px + pz * pz > reach * reach) continue;
        }
        /* engine 0x100103e0: the pieces' spheres (type 6, no test of their own), the nearest entry; the piece's
         * location (+0x16) takes the damage */
        if (mech3d_hull_hit((mech3d *)m, o, dv, mech3d_fly_mask(i < 0 ? s->player_unit.loc_gone : s->units[i].loc_gone), &t, &g) && t < *tbest) {
            *tbest = t; *unit = i; *group = g;
        }
    }
}
/* engine 0x10015090 (the fire decision 0x100231a0, AI pilots below level 5): the line from the unit to its target runs
 * against the whole collision list (0x100103e0, the unit itself skipped) - units and world objects together; the
 * nearest object struck must be the target (object +2 bit 0x100 with controller +0xdc bit 0x200: a unit's piece whose
 * owner +0x14 is the target; bit 0x200 with +0xdc bit 0x400: the target thing), anything else holds the fire (return
 * 0); nothing struck, fire. los_blocked covers the world objects; this, a unit nearer than the target */
static int los_unit_first(msim *s, int self, int who, float x0, float y0, float z0, float x1, float y1, float z1)
{
    float o[3] = {x0, y0, z0}, dv[3] = {x1 - x0, y1 - y0, z1 - z0}, t = 1.0f;
    int unit = -2, group = 0;
    shot_units(s, self, o, dv, &t, &unit, &group);
    return unit != -2 && unit != who;
}

/* For a scripted structure target, hitting its front face is a clear shot.
 * Use the same collision list and first-hit ordering as the projectile. */
static int los_thing_clear(msim *s, int self, int thing, const float from[3], const float to[3])
{
    const float ray[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    float dist = 1.0f;
    int unit = -2, group = 0, part = -1;
    shot_units(s, self, from, ray, &dist, &unit, &group);
    if (shot_objects(s, from, ray, &dist, &part)) return shot_thing(s, part) == thing;
    return unit == -2;
}
int msim_test_fire_line(msim *s, int k)   /* TEST ONLY: the fire decision's line test (combat_step) at the player */
{
    float x0, y0, z0, y1;
    if (k < 0 || k >= s->actor_count || !s->armed[k]) return -2;
    x0 = (float)s->actors[k].mech.origin[0]; y0 = s->units[k].y + s->height[k] * 0.7f; z0 = (float)s->actors[k].mech.origin[2];
    y1 = s->player_unit.y + 950.0f * 0.55f;
    if (los_blocked(s, x0, y0, z0, s->player[0], y1, s->player[2])) return -1;
    return !los_unit_first(s, k, -1, x0, y0, z0, s->player[0], y1, s->player[2]);
}
int msim_test_shot(msim *s, const float o[3], const float dv[3], int *unit, int *group, float *t)   /* TEST ONLY */
{
    *t = 2.0f; *unit = -2; *group = 0;
    shot_units(s, -2, o, dv, t, unit, group);
    return *unit != -2;
}
/* DOS 0x52ea0 uses whole-centimetre position deltas and the 16.16
 * bearing and elevation from 0x2ab60. A zero low word of the 64-bit
 * range difference is treated as equality. */
static int dos_lock_in_window(int weapon, int32_t dx, int32_t dy, int32_t dz,
                              int32_t yaw, int32_t pitch)
{
    const uint64_t range2 = (uint64_t)((int64_t)dx * dx) + (uint64_t)((int64_t)dy * dy) +
                            (uint64_t)((int64_t)dz * dz);
    const uint64_t minimum = (uint64_t)combat_weapon_min_range(weapon);
    const uint64_t maximum = (uint64_t)combat_weapon_range(weapon);
    const uint64_t min2 = minimum * minimum, max2 = maximum * maximum;
    if (range2 < min2 || (uint32_t)(range2 - min2) == 0) return 0;
    if (range2 > max2 && (uint32_t)(range2 - max2) != 0) return 0;

    const int32_t turn = 360 << 16, half_turn = 180 << 16, cone = 16 << 16;
    const int32_t bearing = dos_atan2(dx, dz);
    const int32_t elevation = dos_atan2(dy, dos_level_distance(dx, dz));
    int32_t azimuth_error = (bearing - yaw) % turn;
    if (azimuth_error > half_turn) azimuth_error -= turn;
    if (azimuth_error < -half_turn) azimuth_error += turn;
    const int32_t elevation_error = (elevation - pitch) % turn;
    return azimuth_error > -cone && azimuth_error < cone &&
           elevation_error > -cone && elevation_error < cone;
}

/* missile lock (engine 0x10044560, per tick for the selected weapon): a guided weapon, a selected target within
 * the weapon's range window (+0x3c .. +0x40) and within 16 degrees of the torso's aim both ways: on target (0x8000);
 * held for 0x16a ticks (2 s) -> locked (0x80); any break restarts it */
static void player_lock_step(msim *s, float pl_h, float ticks)
{
    int e = s->player_lock_target, sw = s->player_sel_w, wid = -1, in = 0;
    s->player_lock &= ~0x80;
    if (sw >= 0 && sw < s->player_unit.weapon_count) wid = s->player_unit.weapons[sw].weapon;
    float tp[3];
    /* a thing (kind 0x400) locks like a unit - 0x10044560 refuses only no target and kinds 0x100 (a nav point) /
     * 0x1000: its point is its object's position (0x1001d2d0 -> 0x100257a0), taken from the player's feet (+0x50) */
    int th = MSIM_IS_THING(e) && !s->player_unit.destroyed && thing_point(s, MSIM_THING_OF(e), tp);
    if (wid >= 0 && msim_weapon_guided(wid) && (th || (e >= 0 && e < s->actor_count && s->armed[e] && !s->units[e].destroyed && !s->player_unit.destroyed))) {
        float dx = th ? tp[0] - s->player[0] : (float)s->actors[e].mech.origin[0] - s->player[0];
        float dz = th ? tp[2] - s->player[2] : (float)s->actors[e].mech.origin[2] - s->player[2];
        /* DOS 0x52f14 compares the positions stored by target selection, measured from each unit's feet. */
        float dy = th ? tp[1] - s->player_unit.y : msim_dos_int ? s->units[e].y - s->player_unit.y :
                   (actor_aim_base(s, e) + s->height[e] * 0.5f) - (s->player_unit.y + pl_h * 0.7f);
        if (msim_dos_int) {
            in = dos_lock_in_window(wid, (int32_t)dx, (int32_t)dy, (int32_t)dz,
                                    (int32_t)llround(s->player_aim_yaw * 65536.0),
                                    (int32_t)llround(s->player_aim_pitch * 65536.0));
        } else {
            float d = sqrtf(dx * dx + dz * dz + dy * dy);
            const float max_range = combat_weapon_range(wid);
            if (d > (float)combat_weapon_min_range(wid) && (d < max_range || (th && d == max_range))) {
                float yaw = wrap180(atan2f(dx, dz) * 180.0f / 3.14159265f - s->player_aim_yaw);
                float pit = atan2f(dy, sqrtf(dx * dx + dz * dz)) * 180.0f / 3.14159265f - s->player_aim_pitch;
                in = fabsf(yaw) < 16.0f && fabsf(pit) < 16.0f;
            }
        }
    }
    if (in) {
        s->player_lock |= 0x8000;
        if (!(s->player_lock & 0x40)) { s->player_lock |= 0x40; s->player_lock_ticks = msim_dos_int ? 364.0f : 362.0f; }   /* DOS 0x52ea0 (also the player's, 0x27a55): 0x16c */
        else if (s->player_lock_ticks < 1.0f) { s->player_lock_ticks = 0; s->player_lock |= 0x80; }
        else s->player_lock_ticks -= ticks;
    } else if (wid >= 0 && msim_weapon_guided(wid) && (e >= 0 || MSIM_IS_THING(e))) {
        /* off target / out of the window (0x10044560): 0x8000 and 0x80 cleared, the timer counts back up by dt and
         * 0x40 goes at 0x16a - so a brief break resumes where it was */
        s->player_lock &= ~(0x8000 | 0x80);
        if (s->player_lock & 0x40) { s->player_lock_ticks += ticks; if (s->player_lock_ticks >= (msim_dos_int ? 364.0f : 362.0f)) s->player_lock &= ~0x40; }
    } else s->player_lock = 0;   /* no guided weapon or no target: everything cleared */
    s->player_unit.lock_target = (s->player_lock & 0x80) ? e : -2;
}

/* DOS 0x507da..0x5082f. Average the 16.16 velocity over the frame and round the displacement down to whole cm. */
static int32_t dos_projectile_axis(int32_t velocity, int32_t acceleration, int ticks, int32_t *next)
{
    int64_t change = (int64_t)acceleration * ticks;
    *next = (int32_t)(velocity + change);
    return (int32_t)(((velocity + (change >> 1)) * ticks) >> 16);
}

/* DOS 0x50e24..0x50ee7: guide from the new position using the velocity that began the frame. */
static void dos_projectile_guide(msim_shot *sh, int32_t delta[3])
{
    int32_t velocity[3] = {sh->dos_vel[0], sh->dos_vel[1], sh->dos_vel[2]};
    sh->prox = dos_normalize(delta) <= 100;
    dos_normalize(velocity);
    sh->ax = (float)(delta[0] - velocity[0]) / 65536.0f;
    sh->ay = (float)(delta[1] - velocity[1]) / 65536.0f;
    sh->az = (float)(delta[2] - velocity[2]) / 65536.0f;
}

static void ai_missile_lock(msim *s, int i, ai_mind *m, int has_target, int who,
                            float dx, float ey, float dz, float lock_ticks)
{
    if (has_target) {   /* AI missile lock (0x10044560, every live mech, for the AI's selected weapon +0xac = combat_unit.ai_sel):
                * guided, origin distance (3D) in the weapon's window (+0x3c < d <= +0x40), within 16 degrees of the aim
                * (bearing off heading + lagged twist; pitch + elevation): 0x8000 on target; 0x40 + a 0x16a-tick timer
                * when first on, counted down by dt, locked (0x80) at <= 0. Off target: 0x8000 / 0x80 cleared and the
                * timer counts back UP by dt, 0x40 cleared at 0x16a. No guided weapon selected: all cleared. */
        int sw = s->units[i].ai_sel, wid = sw >= 0 && sw < s->units[i].weapon_count ? s->units[i].weapons[sw].weapon : -1;
        /* A thing (kind 0x400) locks at its object's position (0x1001d2d0 -> 0x100257a0).
         * A nav point (kind 0x100) never locks (0x10044560 tests +0xdc & 0x1100). */
        float tq[3], lx = dx, lz = dz, ly = ey;
        if (MSIM_IS_THING(who) && thing_point(s, MSIM_THING_OF(who), tq)) {
            lx = tq[0] - (float)s->actors[i].mech.origin[0]; lz = tq[2] - (float)s->actors[i].mech.origin[2]; ly = tq[1];
        } else if (who == -3) wid = -1;
        m->lock &= ~0x80;
        if (wid >= 0 && msim_weapon_guided(wid)) {
            float dy = ly - actor_aim_base(s, i);
            int on = 0;
            if (msim_dos_int) {
                on = dos_lock_in_window(wid, (int32_t)lx, (int32_t)dy, (int32_t)lz,
                                        (int32_t)llround((s->actors[i].mech.heading + m->twist) * 65536.0),
                                        (int32_t)llround(m->pitch * 65536.0));
            } else {
                float d3 = sqrtf(lx * lx + lz * lz + dy * dy);
                if (d3 > (float)combat_weapon_min_range(wid) && d3 <= combat_weapon_range(wid)) {
                    float yaw = wrap180(atan2f(lx, lz) * 180.0f / 3.14159265f - s->actors[i].mech.heading - m->twist);
                    float el = atan2f(dy, sqrtf(lx * lx + lz * lz)) * 180.0f / 3.14159265f;
                    on = fabsf(yaw) < 16.0f && fabsf(m->pitch - el) < 16.0f;
                }
            }
            if (on) {
                m->lock |= 0x8000;
                if (!(m->lock & 0x40)) { m->lock |= 0x40; m->lock_ticks = msim_dos_int ? 364.0f : 362.0f; }   /* DOS 0x530d4: 0x16c */
                else if (m->lock_ticks < 1.0f) { m->lock_ticks = 0; m->lock |= 0x80; }
                else m->lock_ticks -= lock_ticks;
            } else {
                m->lock &= ~(0x8000 | 0x80);
                if (m->lock & 0x40) { m->lock_ticks += lock_ticks; if (m->lock_ticks >= (msim_dos_int ? 364.0f : 362.0f)) m->lock &= ~0x40; }
            }
        } else m->lock = 0;
    } else m->lock = 0;   /* DOS 0x52f03: no target clears the timed lock */
}

static void combat_actor_shots(msim *s, int i, float lock_ticks)
{
    const float pl_h = 950.0f;
    float dx, dz, from[3], to[3], ex = 0, ez = 0, ey = 0, ef = 0, eh = 0, aim_h;
    int who = -2;
    ai_mind *m = s->minds && s->ai_ok ? &s->minds[i] : NULL;
    /* the AI's trigger (+0x25): cleared by the state's movement (0x100130f0 case 2, 0x10013290), which does not
     * run when a rule fired (0x10011c50) - then it holds */
    if (!msim_dos_int && m && !m->rule_fired) s->units[i].ai_trigger = 0;
    if (!s->armed[i] || s->units[i].destroyed) return;
    /* DOS 0x5235a: an online unit releases the rest of a volley even after losing its target. */
    const int has_target = pick_enemy(s, i, &ex, &ez, &ey, &ef, &eh, &who);
    (void)ef;
    const int starmate = s->actors[i].friendly && s->actors[i].table == 0;
    const int thing = MSIM_IS_THING(who) ? MSIM_THING_OF(who) : -1;
    aim_h = who == -1 ? s->player_unit.y + pl_h * 0.55f : ey + eh * 0.55f;   /* (a task's nav point / thing: its point) */
    dx = ex - (float)s->actors[i].mech.origin[0];
    dz = ez - (float)s->actors[i].mech.origin[2];
    from[0] = (float)s->actors[i].mech.origin[0]; from[1] = actor_aim_base(s, i) + s->height[i] * 0.7f; from[2] = (float)s->actors[i].mech.origin[2];
    if (starmate && (who >= 0 || thing >= 0)) {
        const mech3d *mech = &s->actors[i].mech;
        if (who >= 0) aim_h = msim_draw_y(s, who) + eh * 0.55f;
        if (mech->eye_ok)
            for (int axis = 0; axis < 3; axis++)
                from[axis] = mech->eye_pos[axis];
    }
    if (!msim_dos_int && m) ai_missile_lock(s, i, m, has_target, who, dx, ey, dz, lock_ticks);
    if (!msim_dos_int && m && has_target) {
        /* set by the fire decision (0x100231a0) when 0x100232b0 chooses a weapon (it stays selected; ENEMYFIR
         * 0x10023428) and the line to the target is clear (0x10015090). The weapon step fires on the press, one
         * weapon, and moves on at the release (combat_ai_trigger) */
        combat_unit *cu = &s->units[i];
        if (!m->rule_fired) {
            if (m->wants_fire) {
                int fw;
                s->units[i].lock_target = (m->lock & 0x80) && (who >= -1 || thing >= 0) ? who : -2;
                fw = combat_ai_select(cu, sqrtf(dx * dx + dz * dz), ms_roll, s);
                if (fw >= 0 && who == -1 && msim_weapon_guided(cu->weapons[fw].weapon) && s->on_sound)
                    s->on_sound(s->shot_user, 0x6f, NULL);
                if (fw >= 0 && thing >= 0) {
                    const float target[3] = {ex, aim_h, ez};
                    cu->ai_trigger = los_thing_clear(s, i, thing, from, target);
                } else {
                    cu->ai_trigger = fw >= 0 && !los_blocked(s, from[0], from[1], from[2], ex, aim_h, ez) &&
                                     !los_unit_first(s, i, who, from[0], from[1], from[2], ex, aim_h, ez);
                }
            }
        }
    } else if (!m && has_target && who == -1 && order_attacks_player(s, i) &&
        !los_blocked(s, (float)s->actors[i].mech.origin[0], s->units[i].y + s->height[i] * 0.7f, (float)s->actors[i].mech.origin[2],
                     ex, aim_h, ez) &&
        !los_unit_first(s, i, who, (float)s->actors[i].mech.origin[0], s->units[i].y + s->height[i] * 0.7f, (float)s->actors[i].mech.origin[2],
                        ex, aim_h, ez)) {
        int fw;
        /* homing only if locked when triggered (0x100437a0 / 0x100443a0): the volley latches the target in its slot */
        s->units[i].lock_target = -2;
        fw = combat_ai_fire(&s->units[i], sqrtf(dx * dx + dz * dz), ms_roll, s);   /* one weapon per decision (0x100232b0) */
        /* 0x10023428: a guided weapon (weapon table +0x18) fired at the player (+0x14e == player | 0x200): ENEMYFIR
         * 0x6f, volume 0x32, not positional (pos NULL) */
        if (fw >= 0 && who == -1 && msim_weapon_guided(s->units[i].weapons[fw].weapon) && s->on_sound) s->on_sound(s->shot_user, 0x6f, NULL);
    }
    int first = 0, loc = -1, hom = -2;
    int w = combat_next_projectile_h(&s->units[i], &first, &loc, &hom);
    if (w < 0) goto refresh_lock;
    {   /* engine 0x100448e0: the shot follows the shooter's aim line (no random scatter): the legs' heading + the
         * LAGGED torso twist (clamped to MGEO) and the lagged pitch, from the aim origin; aimed at the ray's point at
         * the target's distance (no AI programs; with them the aim ray below, per projectile) */
        float lim = s->twist_limit ? s->twist_limit[i] : 90.0f, rel = 0.0f, yaw, pit = 0.0f, d3;
        float tx = ex - from[0], ty = aim_h - from[1], tz = ez - from[2];
        if (m) {
            rel = m->twist;
            pit = m->pitch;
        } else if (has_target) {   /* no AI programs: straight at the target within the twist limit */
            rel = wrap180(atan2f(dx, dz) * 180.0f / 3.14159265f - s->actors[i].mech.heading);
            if (lim < 360.0f) { if (rel > lim) rel = lim; if (rel < -lim) rel = -lim; }
            pit = atan2f(ty, sqrtf(tx * tx + tz * tz)) * 180.0f / 3.14159265f;
        }
        yaw = (s->actors[i].mech.heading + rel) * 3.14159265f / 180.0f;
        pit = pit * 3.14159265f / 180.0f;
        d3 = has_target ? sqrtf(tx * tx + ty * ty + tz * tz) : 50000.0f;
        to[0] = from[0] + sinf(yaw) * cosf(pit) * d3; to[1] = from[1] + sinf(pit) * d3; to[2] = from[2] + cosf(yaw) * cosf(pit) * d3;
    }
    const mech3d *shot_pose = &s->actors[i].mech;
    mech3d moved_pose;
    if (msim_dos_int) {
        /* Movement already changed the root and servo angles. Solve the eye
         * and mounts using the animation keys still active in this pass. */
        moved_pose = *shot_pose;
        moved_pose.part_count = 0;
        mech3d_pose(&moved_pose, (const anim_set *)shot_pose->pose_anim, shot_pose->pose_t);
        shot_pose = &moved_pose;
    }
    do {
        /* from the weapon's mount (POFO slot by its location), else the body point */
        float o[3] = {(float)s->actors[i].mech.origin[0], s->units[i].y, (float)s->actors[i].mech.origin[2]}, mf[3];
        const float *src = mech3d_mount_world(shot_pose, mech3d_mount_slot(loc), o, shot_pose->heading, mf) == 0 ? mf : from;
        if (m && s->aim) {
            /* every projectile as it leaves (0x10043cc0 -> 0x10044810, AI units only): the aim ray from the EYE
             * (unit +0x44, the EYEO node as posed: walk, lagged twist; no torso sway, mech3d.c) along the eye
             * object's world yaw and the controller's lagged pitch (0x100449d0: +0x20 -> +0x1c replaces the
             * eye's own pitch) sets the aim range (aim_range_step); the shot heads from its mount for the
             * point at that range on the ray (0x100448e0) - so it lands where the eye ray meets the target, not
             * on the target's centre */
            const mech3d *am = shot_pose;
            float eye[3], dir[3], ey2, pr = m->pitch * 3.14159265f / 180.0f, dist, r;
            int hit;
            if (am->eye_ok) {
                eye[0] = o[0] + am->eye_pos[0] - am->mount_from[0];
                eye[1] = o[1] + am->eye_pos[1] - am->mount_from[1];
                eye[2] = o[2] + am->eye_pos[2] - am->mount_from[2];
                ey2 = atan2f(am->eye_rot[2], am->eye_rot[8]);
            } else {
                memcpy(eye, from, sizeof eye);
                ey2 = (am->heading + m->twist) * 3.14159265f / 180.0f;
            }
            float target_range = 0.0f;
            if (starmate && who >= 0) {
                /* Account for the latest eye pose while retaining the twist and pitch servo lag. */
                float tx = ex - eye[0], ty = aim_h - eye[1], tz = ez - eye[2];
                target_range = sqrtf(tx * tx + ty * ty + tz * tz);
                ey2 = atan2f(tx, tz) + wrap180(m->twist - m->twist_demand) * 3.14159265f / 180.0f;
                pr = atan2f(ty, hypotf(tx, tz)) + (m->pitch - m->pitch_demand) * 3.14159265f / 180.0f;
            }
            dir[0] = sinf(ey2) * cosf(pr); dir[1] = sinf(pr); dir[2] = cosf(ey2) * cosf(pr);
            hit = aim_ray(s, i, eye, dir, &dist);
            r = aim_range_step(s, &s->aim[i], hit, dist);
            /* Converge at the selected target so a miss from the eye cannot send shots from
             * high mounts over a low turret by converging well behind it. */
            if (target_range > 0.0f) r = target_range;
            to[0] = eye[0] + dir[0] * r; to[1] = eye[1] + dir[1] * r; to[2] = eye[2] + dir[2] * r;
            if (mw2_development_env("MW2_AI_AIM_TRACE"))   /* TEST ONLY */
                fprintf(stderr, "aiaim %.2f actor %d w %d hit %d dist %.0f range %d conv %.0f eye %.0f %.0f %.0f yaw %.2f body %.2f pitch %.2f\n",
                        s->now / 1000.0, i, w, hit, dist, s->aim[i].range.value, r, eye[0], eye[1], eye[2], ey2 * 180.0f / 3.14159265f,
                        wrap180(am->heading + m->twist), pr * 180.0f / 3.14159265f);
        }
        {
            msim_shot *shot = spawn_shot_h(s, w, i, src, to, 0.0f, hom);
            if (shot) {
                shot->first = first;
                shot->life -= combat_last_age;   /* lifetime less the carry already spent in the weapon slot */
                if (mw2_development_env("MW2_AI_AIM_TRACE"))
                    fprintf(stderr, "aishot %u actor %d w %d carry %.0f\n", shot->id, i, w, combat_last_age);
            }
        }
        if (!msim_dos_int && who >= 0 && (msim_weapon_guided(w) || w == 0x15)) {   /* 0x10020ac0 from 0x1004397b */
            int lv = s->actors[i].ai_level >= 1 && s->actors[i].ai_level <= 4 ? s->actors[i].ai_level : 1;
            missile_warning(s, who, lv, (float)s->actors[i].mech.origin[0], (float)s->actors[i].mech.origin[2], 1);
        }
        if (s->on_shot) s->on_shot(s->shot_user, w, i, first, src[0], src[2]);
        if (first) muzzle(s, w, src, src == mf ? shot_pose : NULL, mech3d_mount_slot(loc), shot_pose->heading);
    } while ((w = combat_next_projectile_h(&s->units[i], &first, &loc, &hom)) >= 0);
refresh_lock:
    if (msim_dos_int && m)
        ai_missile_lock(s, i, m, has_target, who, dx, ey, dz, lock_ticks);
}

static void dos_ai_weapon_step(msim *s, int i, float dt, int ticks, int starting)
{
    combat_unit *unit = &s->units[i];
    unit->offline = starting || (s->powered_down && s->powered_down[i]);
    int started = combat_ai_trigger(unit);
    if (started >= 0) {
        int who = s->minds[i].dos_warning_unit;
        int level = s->actors[i].ai_level;
        if (level < 1 || level > 4) level = 1;
        if (dos_warning_roll(s, started, level) && who >= 0)
            missile_warning(s, who, level, (float)s->actors[i].mech.origin[0],
                            (float)s->actors[i].mech.origin[2], 1);
    }
    combat_tick(unit, dt, ticks);
    if (unit->restarted && s->online_at) {
        s->online_at[i] = startup_deadline(s, startup_ticks(s), 0);
        unit_startup(s, i);
    }
    combat_actor_shots(s, i, (float)ticks);
}

static void combat_step(msim *s, int32_t dt_ms, int dos_ticks)
{
    int i, w;
    float dt = (float)dt_ms / 1000.0f;
    /* BATTLE.DLL registers the 182 Hz timer with a PIT divisor of 6556. */
    float ticks = combat_titanium ? (float)dos_ticks : dt * COMBAT_TICKS_PER_S;
    float lock_ticks = msim_dos_int ? (float)dos_ticks : ticks;
    const float pl_h = 950.0f;          /* Timber Wolf muzzle / aim height, cm (approx.) */

    /* the player's collision model follows the player */
    if (s->pl_mech_ok) {
        s->pl_mech.origin[0] = (int32_t)s->player[0];
        s->pl_mech.origin[1] = (int32_t)s->player_unit.y;
        s->pl_mech.origin[2] = (int32_t)s->player[2];
        s->pl_mech.heading = s->player_facing;
        s->pl_mech.twist = s->player_twist;
        const anim_set *anim = NULL;
        float pose_time = 0;
        if (msim_dos_int) {
            s->pl_mech.tilt = s->pl_mech.pitch = -0.5f * s->player_aim_pitch;
            if (s->pl_shown_ok) {
                anim = (const anim_set *)s->pl_shown.pose_anim;
                pose_time = s->pl_shown.pose_t;
                s->pl_mech.pose_off = s->pl_shown.pose_off;
                s->pl_mech.pose_dos = s->pl_shown.pose_dos;
            }
        }
        mech3d_pose(&s->pl_mech, anim, pose_time);
        if (s->pl_pit_ok) mech3d_pose_like(&s->pl_pit, &s->pl_mech);
        if (s->player_external && s->player_col_repr && s->col_lod && s->col_lod_ok)
            s->player_col_repr = pose_collision_lod(s, s->actor_count, s->player_col_repr, s->player_skel, &s->pl_mech);
    }
    dos_sync_hull_order(s);
    if (msim_dos_int) {
        /* DOS 0x15d8e advances existing projectiles before the controllers
         * fire at 0x15d9d. New projectiles first move on the next update. */
        projectile_step(s, (float)dos_ticks, dos_ticks);
        wrecks(s, dt_ms);
        blow_offs(s);
    }
    /* the fire routine (0x100437a0) runs from the controller only in state 2: not while starting up (states 0 / 1,
     * 0x1001a180 / 0x10021a50) or powered down - a queued volley waits, nothing is triggered */
    s->player_unit.offline = (float)s->now < s->player_online_at && !mw2_development_env("MW2_NO_STARTUP");   /* the front end's test hook */
    if (!s->player_unit.offline && !s->player_unit.shutdown) combat_damage_live = 1;   /* DAT_1024c570: the player's first start-up done (0x1001ac68) */
    for (i = 0; i < s->actor_count; i++)
        s->units[i].offline = (s->online_at && (float)s->now < s->online_at[i]) || (s->powered_down && s->powered_down[i]);
    combat_tick(&s->player_unit, dt, dos_ticks);
    if (!msim_dos_int || !s->minds || !s->ai_ok)
        for (i = 0; i < s->actor_count; i++) if (s->armed[i]) {
            if (s->minds && s->ai_ok) combat_ai_trigger(&s->units[i]);
            combat_tick(&s->units[i], dt, dos_ticks);
        }
    if (s->cheat_kill > 0 && s->cheat_kill <= s->actor_count) {   /* the cheat codes "gankem" / "enolagay" */
        int u = s->cheat_kill - 1;
        combat_unit *cu = &s->units[u];
        if (s->cheat_nuke) {
            /* StartNuke (Win95 0x1006c4e2, 3Dfx 0x10047020): the effect 0x16 (NUKE) at the target's position and a blast
             * that grows 200 cm a tick to 4 km - or 100 x the shape radius of a 0x16 effect already burning - for twice the
             * effect's life, every unit inside taking ticks / 16 a location (UpdateNuke 0x10047130 -> 0x10046700). Its
             * structures and debris passes (rate 0x1000 x ticks, truncated to whole points) take nothing at a frame's
             * ticks; its palette flash (slot 0x12) changes no colour in the 3D editions (0x1002ad50's fog colour only
             * for slots 0xc / 0x10 / 0x11) - neither is drawn */
            int k;
            s->nuke_max = 400000.0f;
            for (k = 0; k < s->area_count; k++) if (s->area[k].type == 0x16) { s->nuke_max = s->fx_radius[0x16] * 100.0f; break; }
            s->nuke_pos[0] = (float)s->actors[u].mech.origin[0]; s->nuke_pos[1] = cu->y; s->nuke_pos[2] = (float)s->actors[u].mech.origin[2];
            s->nuke_left = (float)fx_duration(0x16, msim_dos_int) * 2.0f;
            s->nuke_r = 0;
            effect(s, 0x16, s->nuke_pos, 0);
        } else if (!cu->destroyed && s->armed[u]) {
            /* KillMech(player, its mech) (0x10016280): destroyed, the kill counted for the player */
            int side = s->actors[u].friendly ? 2 : s->actors[u].alliance == 2 ? 1 : 0;
            static const int KILLED_BY_PLAYER[3] = {0x07, 0x09, 0x0b}, KILLED[3] = {0x1e, 0x20, 0x22};
            cu->destroyed = 1;
            car_inc(s, KILLED_BY_PLAYER[side]);
            car_inc(s, KILLED[side]);
            if (side == 2) car_inc(s, 0x34);
        }
    }
    s->cheat_kill = 0; s->cheat_nuke = 0;
    if (s->nuke_left > 0) {   /* UpdateNuke */
        s->nuke_r += ticks * 200.0f;
        if (s->nuke_r > s->nuke_max) s->nuke_r = s->nuke_max;
        units_in_radius(s, s->nuke_pos, s->nuke_r, ticks / 16.0f);
        s->nuke_left -= ticks;
    }
    fires_heat(s, ticks);
    area_damage(s, ticks);
    ammo_booms(s);
    /* the heat restart sets state 0 (0x10016201), which runs the start-up again (0x1001ab71: online 0x43e + rand(0x16a)
     * ticks later; no throttle, turning, jets or weapons until then) */
    if (s->player_unit.restarted) {
        unsigned d = startup_ticks(s);
        s->player_online_at = startup_deadline(s, d, 0);
        if (msim_dos_int) dos_player_startup_sound(s);
        s->player_unit.seq_ticks = -(float)d;   /* state 0 sets c[0x23] to the online time (0x1001ab71): the overheat timer counts from it */
    }
    for (i = 0; (!msim_dos_int || !s->minds || !s->ai_ok) && s->online_at && i < s->actor_count; i++)
        if (s->armed[i] && s->units[i].restarted) {
            s->online_at[i] = startup_deadline(s, startup_ticks(s), 0);
            unit_startup(s, i);   /* state 0 again (0x10015f50 -> 0x1001ab71) */
        }

    /* In DOS, AI controllers launch from their own update. Keep the fallback for missions without AI programs. */
    if (!msim_dos_int || !s->minds || !s->ai_ok)
        for (i = 0; i < s->actor_count; i++) combat_actor_shots(s, i, lock_ticks);
    if (!msim_dos_int) player_lock_step(s, pl_h, ticks);
    /* the player fires at the crosshair target, or with none along the aim line (engine 0x100448e0: shots follow the
     * aim line; the trigger needs no target) */
    if (!s->player_unit.destroyed) {
        float d;
        int e = s->player_target >= 0 ? s->player_target : (s->player_aim_free ? -1 : msim_nearest_enemy(s, &d));
        int tgt_ok = e >= 0 && s->armed[e] && !s->units[e].destroyed;
        if (tgt_ok || s->player_aim_free) {
            float ay = s->player_aim_yaw * 3.14159265f / 180.0f, ap = s->player_aim_pitch * 3.14159265f / 180.0f;
            float dx = tgt_ok ? (float)s->actors[e].mech.origin[0] - s->player[0] : sinf(ay) * cosf(ap) * 100000.0f;
            float dz = tgt_ok ? (float)s->actors[e].mech.origin[2] - s->player[2] : cosf(ay) * cosf(ap) * 100000.0f;
            float from[3] = {s->player[0], s->player_unit.y + pl_h * 0.7f, s->player[2]};
            float to[3];
            if (s->player_aim_free) {
                /* engine 0x10043cc0 -> 0x100448e0: every projectile as it leaves, toward a point on the CURRENT aim line
                 * (the torso's heading and tilt - the reticle) at the convergence range: the ray's hit on a mech (flags
                 * 0x300) no nearer than 2000 (0x10044810, threshold 0x10248598), else the ray's 150000 (0x10044999).
                 * So a volley still leaving while the reticle moves fans out after it (LRMs raised mid-volley arc up). */
                /* the ray starts at the EYE (0x10044950: the unit's +0x44 object, the camera's - plus the controller's
                 * +0xc4 offset, which the camera adds too, 0x1002e15e), so its line is the reticle's: the body point
                 * (6.65 m) put every converged shot 1.5-3 m under the reticle on a mech (user report). The hit range
                 * (0x10044810 -> 0x100103e0) is where the ray first enters a mech, here its parts' hulls (the shots' own
                 * test), found within the mech's bounding sphere */
                float dir[3] = {sinf(ay) * cosf(ap), sinf(ap), cosf(ay) * cosf(ap)}, best = 150000.0f;
                int hit = 0;
                if (s->player_eye_ok && !mw2_development_env("MW2_AIM_BODY")) memcpy(from, s->player_eye, sizeof from);   /* TEST ONLY: MW2_AIM_BODY the old ray */
                /* every frame for the player (0x10019310 -> 0x1001a021): the ray's hit range (aim_ray) through the
                 * servos (aim_range_step): with nothing struck the range drifts back toward 50000 (tau 20 s) and the
                 * convergence follows the range with tau 0.2 s */
                hit = aim_ray(s, -1, from, dir, &best);
                best = aim_range_step(s, &s->player_aim, hit, best);
                s->player_conv = best;
                to[0] = from[0] + dir[0] * best; to[1] = from[1] + dir[1] * best; to[2] = from[2] + dir[2] * best;
            }
            else if (tgt_ok) { to[0] = (float)s->actors[e].mech.origin[0]; to[1] = actor_aim_base(s, e) + s->height[e] * 0.55f; to[2] = (float)s->actors[e].mech.origin[2]; }
            else { to[0] = from[0] + dx; to[1] = from[1] + sinf(ap) * 100000.0f; to[2] = from[2] + dz; }
            /* a volley triggered now latches the target if locked (0x100437a0 / 0x10044440: slot +0x18 / +0x1c from the
             * mech's +0xdc when the controller's 0x80 is set) - its later missiles home even if the lock breaks */
            if (s->player_attacks) {
                uint32_t started = combat_trigger_mask(&s->player_unit, tgt_ok ? sqrtf(dx * dx + dz * dz) : 0.0f,
                                                       s->player_fire_mask ? s->player_fire_mask : 0xFFFFFFFFu);
                /* DOS 0x522c6 / 0x52dc8: a missile/PPC warning is sent as the volley enters state 2. */
                if (msim_dos_int)
                    for (int slot = 0; slot < s->player_unit.weapon_count && slot < 32; slot++) {
                        int weapon = s->player_unit.weapons[slot].weapon;
                        if (!(started & (1u << slot))) continue;
                        int react = dos_warning_roll(s, weapon, 1);
                        if (msim_weapon_guided(weapon) || weapon == 0x15) dos_player_warning(s, to, react);
                    }
            }
            {
                int first = 0;
                int loc = -1, hom = -2;
                while ((w = combat_next_projectile_h(&s->player_unit, &first, &loc, &hom)) >= 0) {
                    float o[3] = {s->player[0], s->player_unit.y, s->player[2]}, mf[3];
                    const float *src = (s->pl_shown_ok && mech3d_mount_world(&s->pl_shown, mech3d_mount_slot(loc), o, s->player_facing, mf) == 0) ||
                                       (s->pl_mech_ok && mech3d_mount_world(&s->pl_mech, mech3d_mount_slot(loc), o, s->player_facing, mf) == 0) ? mf : from;
                    if (mw2_development_env("MW2_MUZZLE_TRACE")) {   /* tests: each player shot's start in the mech's frame (right, up, forward) */
                        float hh = s->player_facing * 3.14159265f / 180.0f, dx2 = src[0] - s->player[0], dz2 = src[2] - s->player[2];
                        fprintf(stderr, "muzzle w %d loc %d slot %d: right %.0f up %.0f fwd %.0f (%s) eye %.0f %.0f %.0f to %.0f %.0f %.0f\n", w, loc, mech3d_mount_slot(loc), dx2 * cosf(hh) - dz2 * sinf(hh), src[1] - s->player_unit.y, dx2 * sinf(hh) + dz2 * cosf(hh), src == from ? "body" : "mount", from[0], from[1], from[2], to[0], to[1], to[2]);
                    }
                    msim_shot *ns = spawn_shot_h(s, w, -1, src, to, 0.0f, hom);
                    if (ns) {
                        ns->first = first;
                        ns->life -= combat_last_age;
                        /* TEST ONLY: the aim line */
                        float ay2 = s->player_aim_yaw * 3.14159265f / 180.0f, ap2 = s->player_aim_pitch * 3.14159265f / 180.0f;
                        memcpy(ns->aim_o, s->player_eye_ok ? s->player_eye : from, sizeof ns->aim_o);
                        ns->aim_d[0] = sinf(ay2) * cosf(ap2); ns->aim_d[1] = sinf(ap2); ns->aim_d[2] = cosf(ay2) * cosf(ap2);
                    }
                    if (!msim_dos_int && tgt_ok && (msim_weapon_guided(w) || w == 0x15)) {
                        /* 0x10020ac0: within 15 degrees of torso aim */
                        float off = wrap180(atan2f(dx, dz) * 180.0f / 3.14159265f - s->player_aim_yaw);
                        if (fabsf(off) < 15.0f) missile_warning(s, e, 1, s->player[0], s->player[2], 1);
                    }
                    s->shot_loc = loc;
                    if (s->on_shot) s->on_shot(s->shot_user, w, -1, first, src[0], src[2]);
                    if (first && !s->no_player_muzzle)
                        muzzle(s, w, src, src != mf ? NULL : s->pl_shown_ok && (s->pl_shown.mount_ok & (1u << mech3d_mount_slot(loc))) ? &s->pl_shown : s->pl_mech_ok ? &s->pl_mech : NULL, mech3d_mount_slot(loc), s->player_facing);
                }
            }
        } else {
            while (combat_next_projectile(&s->player_unit) >= 0) {}
        }
    }

    /* DOS 0x27780 calls weapon dispatch (0x27836) before target upkeep and lock (0x27a55). */
    if (msim_dos_int) {
        player_lock_step(s, pl_h, lock_ticks);
        s->dos_warning_target = s->player_lock_target;
    }

    if (!msim_dos_int) projectile_step(s, ticks, dos_ticks);
}

static void projectile_step(msim *s, float ticks, int dos_ticks)
{
    char line[200];
    int k;
    /* fly and collide */
    for (k = 0; k < s->shot_count; k++) {
        msim_shot *sh = &s->shots[k];
        float total = msim_dos_int ? (float)dos_ticks : fminf(ticks, sh->life);
        float step, o[3], dv[3], dos_unit_contact[3];
        float tbest = 2;
        int hit_unit = -2, hit_group = 0, gone = 0, sub, nsub = msim_dos_int ? 1 : (int)ceilf(total);
        if (msim_dos_int && !dos_ticks) continue;
        if (nsub < 1) nsub = 1;
        step = total / (float)nsub;
        /* DOS checks the frame's full segment, including the last frame before retirement (0x506f0 / 0x50bc6). */
        for (sub = 0; sub < nsub && hit_unit == -2; sub++) {
            const float ov[3] = {sh->vx, sh->vy, sh->vz};
            float vnew[3];
            int32_t dos_next[3];
            o[0] = sh->x; o[1] = sh->y; o[2] = sh->z;
            if (sh->prox && sh->homing != -2) {
                /* 0x10044f60: a projectile flagged by the guidance (+0x38 & 0x8000) skips the collision test and hits its
                 * target where it is (the unit's own object, 0x1000f760: its root part's location) */
                int tg = sh->homing;
                float tq[3];
                if (MSIM_IS_THING(tg) && thing_point(s, MSIM_THING_OF(tg), tq)) {
                    /* a thing (+0x34 == 0x400): its object (0x10025780) takes the hit there - a thing's object (+2 flags
                     * 0x200) the damage (0x10046c20) at the projectile's point */
                    int ft = fx_impact_type(combat_weapon_visual(sh->weapon), FX_HIT_GROUND);
                    float hp3[3] = {sh->x, sh->y, sh->z};
                    if (ft >= 0) effect(s, ft, hp3, missile_fx(sh->weapon));
                    building_damage(s, MSIM_THING_OF(tg), combat_weapon_damage(sh->weapon), sh->owner, hp3);
                    gone = 1;
                    sh->life = 0;
                    break;
                }
                if (!MSIM_IS_THING(tg) && !(tg == -1 ? s->player_unit.destroyed || !s->pl_mech_ok : (tg < 0 || tg >= s->actor_count || !s->armed[tg] || s->units[tg].destroyed))) {
                    const mech3d *tm = tg == -1 ? &s->pl_mech : &s->actors[tg].mech;
                    hit_unit = tg; hit_group = tm->part_count > 0 && tm->parts[0].group > 0 ? tm->parts[0].group : 3; tbest = 0;
                    dv[0] = dv[1] = dv[2] = 0;
                    memcpy(dos_unit_contact, o, sizeof dos_unit_contact);
                    break;
                }
                sh->prox = 0;
            }
            sh->age += step;
            {
                const float accel[3] = {sh->ax, sh->ay, sh->az};
                for (int axis = 0; axis < 3; axis++) {
                    if (msim_dos_int) {
                        o[axis] = (float)(int32_t)o[axis];
                        dv[axis] = (float)dos_projectile_axis(sh->dos_vel[axis],
                                                           (int32_t)lrintf(accel[axis] * 65536.0f), dos_ticks, &dos_next[axis]);
                        vnew[axis] = (float)dos_next[axis] / 65536.0f;
                    } else {
                        dv[axis] = ov[axis] * step + 0.5f * accel[axis] * step * step;
                        vnew[axis] = ov[axis] + accel[axis] * step;
                    }
                }
            }
            int part = -1, world_hit;
            float hp3[3] = {0}, tb = 2.0f;
            if (msim_dos_int) {
                world_hit = dos_shot_contact(s, sh->owner, o, dv, &hit_unit, &hit_group, &part, hp3) && part >= 0;
                memcpy(dos_unit_contact, hp3, sizeof dos_unit_contact);
            } else {
                shot_units(s, sh->owner, o, dv, &tbest, &hit_unit, &hit_group);
                world_hit = s->world && s->terrain && shot_objects(s, o, dv, &tb, &part) && tb < tbest;
            }
            if (world_hit) {
                /* world objects (engine 0x10044f60 -> 0x100103e0): the NEAREST object of the collision list the step
                 * crosses - the bounding sphere first (0x10029db0), then the type's segment test (0x10010650, table
                 * 0x1024b3a8): 0 / 1 the world box (0x1000d310, a slab test - a pipe or a tank is hit anywhere in its box),
                 * 2 / 7 the faces (0x1000d7b0), 3 / 6 / 8 / 9 nothing more (no entry: 0x100107b0 - the sphere's crossing
                 * is the hit; the GOLD shield dome, the TNJ3 target spheres). A thing's object (+2 flags 0x200) takes the
                 * damage (0x10046c20); any other just stops the shot (0x400: an impact). Hidden objects (destroyed, or
                 * a destroyed object's children) are out of the list (0x10025730) */
                hit_unit = -2;
                if (!msim_dos_int)
                    for (int axis = 0; axis < 3; axis++) hp3[axis] = o[axis] + dv[axis] * tb;
                /* A thing (+2 & 0x200) uses a structure impact; regular scenery uses the ground effect. */
                const int b2 = shot_thing(s, part);
                const int ft = fx_impact_type(combat_weapon_visual(sh->weapon), b2 >= 0 || (s->world->parts[part].objtype & 0x600) ? FX_HIT_STRUCT : FX_HIT_GROUND);
                if (ft >= 0) effect(s, ft, hp3, missile_fx(sh->weapon));
                if (b2 >= 0) building_damage(s, b2, combat_weapon_damage(sh->weapon), sh->owner, hp3);
                if (sh->owner == -1 && sh->homing != -2 && mw2_development_env("MW2_MISSILE_TRACE"))   /* TEST ONLY: a guided shot's end */
                    fprintf(stderr, "mhit %.3f object %s %.0f %.0f %.0f\n", s->now / 1000.0, s->world->parts[part].model_name, hp3[0], hp3[1], hp3[2]);
                gone = 1;
                sh->life = 0;
                break;
            }
            if (hit_unit == -2) {
                sh->x = o[0] + dv[0]; sh->y = o[1] + dv[1]; sh->z = o[2] + dv[2];
                sh->vx = vnew[0]; sh->vy = vnew[1]; sh->vz = vnew[2];
                sh->life -= step;
                if (!msim_dos_int && sh->life <= 0) break;
                if (sh->life > 0 && sh->homing != -2 && sh->age > 0) {
                    /* guidance (0x10045910, from 0x10044f60 after the move, with the velocity the move used): the
                     * acceleration (+0x14..+0x1c) BECOMES unit(target - p) - unit(v) - no gravity while guided - aimed at
                     * the target OBJECT's position (+0x50..+0x58; DOS 0x50d50 the same). The object's height is NOT the
                     * feet: the move 0x10019310 stores object y (0x10019fad) = ground (+0x74) + controller +0xcc (0x10019db8), the
                     * MGEO int[0] centre height (loader 0x100400b0; the altitude tape 0x100222e0 subtracts it back) - so the
                     * volley steers at the mech's centre (Mad Dog 5 m up), not its feet; within 100 cm (0x102485c4) it is
                     * flagged to hit it next tick. Speed is not held: turning costs speed. A target gone (unit flags 2 /
                     * 4): acceleration 0 and +0x34 = 0 - it flies on straight, gravity no longer applied */
                    int tg = sh->homing;
                    float tx, ty, tz, tl, vl;
                    float tq[3];
                    int th = MSIM_IS_THING(tg) && thing_point(s, MSIM_THING_OF(tg), tq);   /* +0x34 == 0x400: 0x100257a0, the
                                                                                             * thing's object position; flag 4 (destroyed) -> lost */
                    if (MSIM_IS_THING(tg) ? !th : tg == -1 ? s->player_unit.destroyed : (tg < 0 || tg >= s->actor_count || !s->armed[tg] || s->units[tg].destroyed)) {
                        sh->homing = -2; sh->ax = sh->ay = sh->az = 0;
                    } else {
                        if (th) { tx = tq[0]; ty = tq[1]; tz = tq[2]; }
                        else if (tg == -1) { tx = s->player[0]; ty = s->player_unit.y + s->player_centre_h; tz = s->player[2]; }
                        else { tx = (float)s->actors[tg].mech.origin[0]; ty = s->units[tg].y + (s->centre_h ? s->centre_h[tg] : 550.0f); tz = (float)s->actors[tg].mech.origin[2]; }
                        if (msim_dos_int) {
                            int32_t delta[3] = {(int32_t)tx - (int32_t)sh->x, (int32_t)ty - (int32_t)sh->y,
                                                (int32_t)tz - (int32_t)sh->z};
                            dos_projectile_guide(sh, delta);
                        } else {
                            tx -= sh->x; ty -= sh->y; tz -= sh->z;
                            tl = sqrtf(tx * tx + ty * ty + tz * tz); vl = sqrtf(ov[0] * ov[0] + ov[1] * ov[1] + ov[2] * ov[2]);
                            if (tl <= 100.0f) sh->prox = 1;
                            if (tl > 0 && vl > 0) { sh->ax = tx / tl - ov[0] / vl; sh->ay = ty / tl - ov[1] / vl; sh->az = tz / tl - ov[2] / vl; }
                        }
                    }
                }
                if (msim_dos_int) memcpy(sh->dos_vel, dos_next, sizeof dos_next);
                {
                    /* DOS 0x50b4e: the height query is only the fallback for a negative endpoint.
                     * Ground above zero is checked as terrain polygons along the segment. */
                    float gy = msim_dos_int && sh->y >= 0.0f ? sh->y : msim_ground(s, sh->x, sh->z, sh->y);
                    if (sh->y < gy) {
                        if (s->on_effect) {
                            float gp[3] = {sh->x, gy, sh->z};
                            int ft = fx_impact_type(combat_weapon_visual(sh->weapon), FX_HIT_GROUND);
                            if (ft >= 0) effect(s, ft, gp, missile_fx(sh->weapon));
                            if (sh->owner == -1) aim_trace(s, sh, "ground", gp);   /* TEST ONLY */
                            if (sh->owner == -1 && sh->homing != -2 && mw2_development_env("MW2_MISSILE_TRACE"))   /* TEST ONLY: a guided shot's end */
                                fprintf(stderr, "mhit %.3f ground %.0f %.0f %.0f\n", s->now / 1000.0, gp[0], gp[1], gp[2]);
                        }
                        sh->life = 0;
                        break;
                    }
                }
            }
        }
        if (hit_unit != -2) {
            combat_unit *tu = hit_unit == -1 ? &s->player_unit : &s->units[hit_unit];
            float hp[3];
            for (int axis = 0; axis < 3; axis++) hp[axis] = msim_dos_int ? dos_unit_contact[axis] : o[axis] + dv[axis] * tbest;
            float th = hit_unit == -1 ? s->player_facing : s->actors[hit_unit].mech.heading;
            float dir = atan2f(sh->vx, sh->vz) * 180.0f / 3.14159265f;
            int was = tu->destroyed, dealt;
            if (hit_unit == -1 && mw2_development_env("MW2_TEST_PLHIT_GROUP")) hit_group = atoi(mw2_development_env("MW2_TEST_PLHIT_GROUP"));   /* TEST ONLY: the location struck */
            if (hit_unit == -1 && mw2_development_env("MW2_AI_AIM_TRACE")) {   /* TEST ONLY: where the player was struck (right, up, forward of the player) */
                float hx = hp[0] - s->player[0], hz = hp[2] - s->player[2], fh = s->player_facing * 3.14159265f / 180.0f;
                fprintf(stderr, "plhit %.2f from %d w %d group %d right %.0f up %.0f fwd %.0f age %.0f shot %u\n", s->now / 1000.0, sh->owner, sh->weapon, hit_group,
                        hx * cosf(fh) - hz * sinf(fh), hp[1] - s->player_unit.y, hx * sinf(fh) + hz * cosf(fh), sh->age, sh->id);
            }
            dealt = combat_hit(tu, sh->weapon, hit_group, th - dir, &s->rng);
            /* DOS 0x25740 detaches destroyed locations in the damage call,
             * before the impact effect can reserve its own body slots. */
            if (msim_dos_int) blow_off_unit(s, hit_unit);
            s->hits++;
            if (hit_unit >= 0) {   /* career counters (MW2CAR.CFG) */
                /* each counter is a triple: enemy, neutral, friendly (engine 0x102441d5 / d7 / d9) */
                int side = s->actors[hit_unit].friendly ? 2 : s->actors[hit_unit].alliance == 2 ? 1 : 0;
                static const int HIT[3] = {0x15, 0x17, 0x19}, KILLED_BY_PLAYER[3] = {0x07, 0x09, 0x0b}, KILLED[3] = {0x1e, 0x20, 0x22};
                if (sh->owner == -1) s->player_victim = hit_unit;   /* 0x10014110 */
                if (sh->owner == -1) car_inc(s, HIT[side]);
                if (s->provoked && !s->actors[hit_unit].friendly) {
                    /* engine 0x10014110: the attacker's record says "attacking <victim>" (state 3, target); the victim's
                     * group sees it through its target choice (0x10012de0 counts enemies attacking a member) and
                     * turns on the attacker - here every armed member of the hit mech's group is provoked for 30 s */
                    int j;
                    /* a port addition (requested): the engine has no such reaction (0x10014110 records attackers only on
                     * the player's record) - off by default (the original); MW2_PROVOKE=1 turns it on */
                    for (j = 0; j < s->actor_count && mw2_development_env("MW2_PROVOKE"); j++) {
                        float gx = (float)(s->actors[j].mech.origin[0] - s->actors[hit_unit].mech.origin[0]);
                        float gz = (float)(s->actors[j].mech.origin[2] - s->actors[hit_unit].mech.origin[2]);
                        if (s->armed[j] && !s->units[j].destroyed && !s->actors[j].friendly &&
                            (strcasecmp(s->actors[j].group, s->actors[hit_unit].group) == 0 || gx * gx + gz * gz < 60000.0f * 60000.0f))
                            s->provoked[j] = 30.0f;   /* its group, and any enemy mech within 600 m that saw it (ASSUMED radius) */
                    }
                }
                if (!was && tu->destroyed) {
                    if (sh->owner == -1) car_inc(s, KILLED_BY_PLAYER[side]);
                    car_inc(s, KILLED[side]);
                    if (side == 2) car_inc(s, tu->ejected == 1 ? 0x36 : 0x34);   /* a wingman lost (0x10016280: +0x36 if it ejected) */
                }
            }
            if (s->on_effect) {   /* impact effect at the hit point; the death explosion when it kills */
                /* the flags (0x100453xx): 0x100 any unit; for the player still alive after the hit (+0x14 & 2 clear) 0x4000,
                 * and 0x2000 only when the part struck is in location 1-4 (head, right / centre / left torso: the hit
                 * part's +0x16) - an arm or a leg hit shows the plain unit effect */
                int ph = hit_unit == -1 && !tu->destroyed && hit_group >= 1 && hit_group <= 4;
                int visual = combat_weapon_visual(sh->weapon);
                int ft = fx_impact_type(visual, FX_HIT_UNIT | (ph ? FX_HIT_PLAYER : 0));
                s->fx_player_hit = hit_unit == -1 && !tu->destroyed;   /* flag 0x4000 */
                if (ft >= 0) {
                    float at[3];
                    if (msim_dos_int && visual >= 0 && (visual & 0xff) == 5) {
                        dos_impact_effect(dv, hp, at);
                        effect(s, ft, at, missile_fx(sh->weapon));
                    } else {
                        effect(s, ft, hp, missile_fx(sh->weapon));
                    }
                }
                s->fx_player_hit = 0;
                if (sh->owner == -1) aim_trace(s, sh, "unit", hp);   /* TEST ONLY */
                if (sh->owner == -1 && sh->homing != -2 && mw2_development_env("MW2_MISSILE_TRACE"))   /* TEST ONLY: a guided shot's end */
                    fprintf(stderr, "mhit %.3f unit %d group %d prox %d up %.0f id %u\n", s->now / 1000.0, hit_unit, hit_group, tbest == 0,
                            hp[1] - (hit_unit == -1 ? s->player_unit.y : s->units[hit_unit].y), sh->id);
                if (mw2_development_env("MW2_FX_TRACE") && hit_unit == -1) fprintf(stderr, "plfx %.2f group %d type %d\n", s->now / 1000.0, hit_group, ft);   /* TEST ONLY */
                (void)was;   /* the death explosion comes from the death sequence (wrecks) */
            }
            if (s->log) {
                static const char *LOC[9] = {"?", "head", "right torso", "centre torso", "left torso", "right arm", "left arm", "right leg", "left leg"};
                if (hit_unit == -1)
                    snprintf(line, sizeof line, "%6.1fs %s hits the player's %s for %d (player %d left)%s", (double)s->now / 1000.0,
                             s->actors[sh->owner].name, LOC[hit_group], dealt, combat_health(tu), tu->destroyed ? " - PLAYER DESTROYED" : "");
                else
                    snprintf(line, sizeof line, "%6.1fs %s hits %s (%s) %s for %d (%d left)%s", (double)s->now / 1000.0,
                             sh->owner == -1 ? "player" : s->actors[sh->owner].name, s->actors[hit_unit].name, s->actors[hit_unit].group,
                             LOC[hit_group], dealt, combat_health(tu), tu->destroyed ? " - DESTROYED" : "");
                s->log(s->log_user, line);
            }
            gone = 1;
        } else if (sh->life <= 0) {   /* Below-zero world height is valid; impact or expiry ends flight. */
            gone = 1;
            s->misses++;
            if (mw2_development_env("MW2_BLD_TRACE") && sh->owner == -1)
                fprintf(stderr, "miss w %d at %.0f,%.0f,%.0f life %.1f\n", sh->weapon, sh->x, sh->y, sh->z, sh->life);
        }
        if (gone) { remove_shot(s, k); k--; }
    }
}

int msim_aim_target(const msim *s, float aim_yaw, float cone, float *distance)
{
    int i, best = -1;
    /* a narrow cone (TARGET_AT_RETICLE, 5 degrees) reaches any distance; the wide one (the reticle's on-target test)
     * stops at weapon range */
    float range = cone <= 5.0f ? 1e9f : combat_max_range(&s->player_unit), bestoff = cone;
    for (i = 0; i < s->actor_count; i++) {
        float dx, dz, d, off;
        if (!s->armed[i] || s->units[i].destroyed || s->units[i].weapon_count == 0 || s->actors[i].friendly) continue;
        dx = (float)s->actors[i].mech.origin[0] - s->player[0];
        dz = (float)s->actors[i].mech.origin[2] - s->player[2];
        d = sqrtf(dx * dx + dz * dz);
        if (d > range || d < 1) continue;
        off = fabsf(wrap180(atan2f(dx, dz) * 180.0f / 3.14159265f - aim_yaw));
        if (off <= bestoff) { bestoff = off; best = i; if (distance) *distance = d; }
    }
    return best;
}


/* ---------------- terrain (collision types 0 and 5) ---------------- */
struct cobj; struct cpoly; struct qnode;
typedef struct { float minx, maxx, minz, maxz, top; } box_t;
typedef struct { int *idx; int n, cap; } cell_t;
typedef struct {
    box_t *wall; int nwall;    /* type 1: XZ boxes, no height limit (engine 0x1000d520) */
    float x0, z0, cs; int gw, gh;
    int nsolid;                /* fixed objects of collision types 0, 1, 2, 5, 7 */
    /* projectiles (0x100103e0 walks the whole collision list): every object of collision type 0-3 or 6-9 (5, the polygon
     * terrain, is the ground test; 4 never hits; -1 objects are not in the list) - part index, world box, bounding
     * sphere (0x10028xxx: the vertex box's centre, the farthest vertex), per 50 m cell */
    int *obj; float (*obj_box)[6]; float (*obj_sph)[4]; int nobj;
    cell_t *ocell;
    int *thing_of;             /* per world part: the bld entry it is the intact object of, -1 */
    int *stamp; int stamp_now; int nparts;
    /* parts carried / animated by object tasks (`moving`): the same collision list in the engine - each object's collision
     * entry takes its new world matrix whenever the node moves (0x1000fbe0 -> 0x10029c10) - so they block walking,
     * carry the ground (type 0 tops) and the avoidance probes where they are now: part, world box (refreshed each step) */
    int *mov; float (*mov_box)[6]; int nmov;
    /* the engine's collision list (clist_build): entries in the engine's list order (clist_seq), the faces of types 2 / 5 / 7, the type 5
     * quadtrees, the fixed entries per 50 m cell */
    struct cobj *co; int nco;
    struct cpoly *cp; int ncp, capcp;
    struct scenery_state *dos_scenery;
    struct scenery_faces *scenery_faces;
    int scenery_count, scenery_capacity;
    struct qnode *qn; int nqn, capqn;
    int *qidx; int nqidx, capqidx;
    cell_t *ccell;
} terrain_t;

static void cell_add(cell_t *c, int v)
{
    if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 8; c->idx = realloc(c->idx, (size_t)c->cap * sizeof *c->idx); }
    c->idx[c->n++] = v;
}

static void terrain_free(terrain_t *t)
{
    int i;
    if (!t) return;
    for (i = 0; t->ocell && i < t->gw * t->gh; i++) free(t->ocell[i].idx);
    free(t->wall);
    free(t->obj); free(t->obj_box); free(t->obj_sph); free(t->ocell); free(t->thing_of); free(t->stamp);
    for (i = 0; t->ccell && i < t->gw * t->gh; i++) free(t->ccell[i].idx);
    free(t->ccell); free(t->co); free(t->cp); free(t->qn); free(t->qidx);
    free(t->dos_scenery); free(t->scenery_faces);
    free(t->mov); free(t->mov_box); free(t);
}

static void terrain_release(void *t) { terrain_free((terrain_t *)t); }

static void clist_build(terrain_t *t, const mech3d *w, const int *seq, int nseq);
static terrain_t *terrain_build(const mech3d *w, const int *seq, int nseq)
{
    terrain_t *t = calloc(1, sizeof *t);
    int i, k, j, capw = 0;
    float minx = 1e30f, maxx = -1e30f, minz = 1e30f, maxz = -1e30f;
    if (!t) return NULL;
    for (i = 0; i < w->part_count; i++) {
        const mech3d_part *p = &w->parts[i];
        const wtb_object *o;
        float bmn[3] = {1e30f, 1e30f, 1e30f}, bmx[3] = {-1e30f, -1e30f, -1e30f};
        if ((p->coll != 0 && p->coll != 1 && p->coll != 2 && p->coll != 5 && p->coll != 7) || p->model.object_count < 1 || p->hidden || p->moving) continue;
        o = &p->model.objects[0];
        for (k = 0; k < o->vert_count; k++) {
            float q[3];
            part_vertex(p, &o->verts[k], q);
            for (j = 0; j < 3; j++) { if (q[j] < bmn[j]) bmn[j] = q[j]; if (q[j] > bmx[j]) bmx[j] = q[j]; }
        }
        if (bmn[0] > bmx[0]) continue;
        t->nsolid++;   /* counted: the collision list's fixed entries (clist_build) */
        if (p->coll == 1) {   /* engine type 1 (0x1000d520): an XZ box (msim_wall_at, tools) */
            if (t->nwall == capw) { capw = capw ? capw * 2 : 64; t->wall = realloc(t->wall, (size_t)capw * sizeof *t->wall); }
            t->wall[t->nwall].minx = bmn[0]; t->wall[t->nwall].maxx = bmx[0]; t->wall[t->nwall].minz = bmn[2]; t->wall[t->nwall].maxz = bmx[2];
            t->wall[t->nwall].top = bmx[1];
            t->nwall++;
        }
        if (bmn[0] < minx) minx = bmn[0];
        if (bmx[0] > maxx) maxx = bmx[0];
        if (bmn[2] < minz) minz = bmn[2];
        if (bmx[2] > maxz) maxz = bmx[2];
    }
    for (i = 0; i < w->part_count; i++) {   /* the projectiles' objects */
        const mech3d_part *p = &w->parts[i];
        const wtb_object *o;
        float bmn[3] = {1e30f, 1e30f, 1e30f}, bmx[3] = {-1e30f, -1e30f, -1e30f}, c[3], r2 = 0;
        if (p->coll < 0 || p->coll > 9 || p->coll == 4 || p->coll == 5 || p->model.object_count < 1 || p->hidden || p->moving) continue;
        o = &p->model.objects[0];
        if (o->vert_count < 1) continue;
        for (k = 0; k < o->vert_count; k++) {
            float q[3];
            part_vertex(p, &o->verts[k], q);
            for (j = 0; j < 3; j++) { if (q[j] < bmn[j]) bmn[j] = q[j]; if (q[j] > bmx[j]) bmx[j] = q[j]; }
        }
        for (j = 0; j < 3; j++) c[j] = (bmn[j] + bmx[j]) * 0.5f;
        for (k = 0; k < o->vert_count; k++) {
            float q[3], d2;
            part_vertex(p, &o->verts[k], q);
            d2 = (q[0] - c[0]) * (q[0] - c[0]) + (q[1] - c[1]) * (q[1] - c[1]) + (q[2] - c[2]) * (q[2] - c[2]);
            if (d2 > r2) r2 = d2;
        }
        t->obj = realloc(t->obj, (size_t)(t->nobj + 1) * sizeof *t->obj);
        t->obj_box = realloc(t->obj_box, (size_t)(t->nobj + 1) * sizeof *t->obj_box);
        t->obj_sph = realloc(t->obj_sph, (size_t)(t->nobj + 1) * sizeof *t->obj_sph);
        if (!t->obj || !t->obj_box || !t->obj_sph) { terrain_free(t); return NULL; }
        t->obj[t->nobj] = i;
        for (j = 0; j < 3; j++) { t->obj_box[t->nobj][j] = bmn[j]; t->obj_box[t->nobj][3 + j] = bmx[j]; t->obj_sph[t->nobj][j] = c[j]; }
        t->obj_sph[t->nobj][3] = sqrtf(r2);
        t->nobj++;
        if (bmn[0] < minx) minx = bmn[0];
        if (bmx[0] > maxx) maxx = bmx[0];
        if (bmn[2] < minz) minz = bmn[2];
        if (bmx[2] > maxz) maxz = bmx[2];
    }
    for (i = 0; i < w->part_count; i++) {   /* the moving parts in the collision list (boxes filled by mov_refresh) */
        const mech3d_part *p = &w->parts[i];
        float bmn[3], bmx[3];
        if (!p->moving || p->coll < 0 || p->coll > 9 || p->coll == 4 || p->model.object_count < 1 || p->model.objects[0].vert_count < 1) continue;
        if (!(t->mov = realloc(t->mov, (size_t)(t->nmov + 1) * sizeof *t->mov))
            || !(t->mov_box = realloc(t->mov_box, (size_t)(t->nmov + 1) * sizeof *t->mov_box))) { terrain_free(t); return NULL; }
        t->mov[t->nmov++] = i;
        if (part_box(p, bmn, bmx) == 0) {
            if (bmn[0] < minx) minx = bmn[0];
            if (bmx[0] > maxx) maxx = bmx[0];
            if (bmn[2] < minz) minz = bmn[2];
            if (bmx[2] > maxz) maxz = bmx[2];
        }
    }
    t->nparts = w->part_count;
    if (!t->nwall && !t->nsolid && !t->nobj && !t->nmov) { terrain_free(t); return NULL; }
    t->cs = 5000.0f;
    t->x0 = minx; t->z0 = minz;
    if (!(maxx >= minx) || !(maxz >= minz) || maxx - minx > 4.0e8f || maxz - minz > 4.0e8f) { terrain_free(t); return NULL; }
    t->gw = (int)((maxx - minx) / t->cs) + 1; t->gh = (int)((maxz - minz) / t->cs) + 1;
    if (t->gw > 4096) t->gw = 4096;
    if (t->gh > 4096) t->gh = 4096;
    t->ocell = calloc((size_t)t->gw * (size_t)t->gh, sizeof *t->ocell);
    t->stamp = calloc((size_t)(w->part_count > 0 ? w->part_count : 1), sizeof *t->stamp);
    if (!t->ocell || !t->stamp) { terrain_free(t); return NULL; }
    for (i = 0; i < t->nobj; i++) {   /* each object in the cells its sphere's square covers */
        const float *sp = t->obj_sph[i];
        int cx0 = (int)((sp[0] - sp[3] - t->x0) / t->cs), cx1 = (int)((sp[0] + sp[3] - t->x0) / t->cs);
        int cz0 = (int)((sp[2] - sp[3] - t->z0) / t->cs), cz1 = (int)((sp[2] + sp[3] - t->z0) / t->cs), cx, cz;
        if (cx0 < 0) cx0 = 0;
        if (cz0 < 0) cz0 = 0;
        if (cx1 >= t->gw) cx1 = t->gw - 1;
        if (cz1 >= t->gh) cz1 = t->gh - 1;
        for (cz = cz0; cz <= cz1; cz++)
            for (cx = cx0; cx <= cx1; cx++) cell_add(&t->ocell[cz * t->gw + cx], i);
    }
    clist_build(t, w, seq, nseq);
    return t;
}


/* ---------------- the engine's collision list (0x1024b320): walking, the ground and the AI probes ----------------
 * Every world object of collision type 0-3 or 5-9 is one entry, in the engine's order - the last created first (msim
 * clist_seq: 0x1000db80 / 0x1000dc60 insert at the head) - the moving ones where they are now: a moving object keeps its
 * place, 0x1000fbe0 -> 0x10029c10 only re-centres its sphere (+0x34..0x3c from the matrix) and bumps its version +0x48.
 * Per type (table 0x1024b3a8: point test / segment test / ground): 0 the world box (0x1000d1b0 / 0x1000d310 / its top
 * 0x1000d1f0), 1 the box (0x1000d520 / 0x1000d310 / none), 2 and 7 the faces (0x1000d6b0 / 0x1000d7b0 / none), 4 nothing
 * (0x1000d580 / 0x1000d590), 5 the polygon terrain through a quadtree (0x1000d640 / 0x1000d680 -> 0x1000ccf0 /
 * 0x1000d690 -> 0x1000cf80), 3, 6, 8, 9 the bounding sphere (no entries: 0x100107b0). A face is the polygon with the
 * engine's normal (from its vertices as built, 0x10029210 - see eng_poly_normal) and its first vertex. */
typedef struct cpoly { float v[WTB_MAX_POLY_VERTS][3]; int n; float nrm[3]; float mn[3], mx[3]; } cpoly_t;
typedef struct qnode { float r[6]; int ch[4]; int np, p0; } qnode_t;   /* r: x0 x1 y0 y1 z0 z1; np > 0: a leaf */
typedef struct cobj {
    int part, type, moving;
    float box[6], sph[4], lsph[4];
    int p0, np, root;
    int unposed_first, unposed_count, lod_first, lod_count;
} cobj_t;

typedef struct scenery_faces { int first, count, distance; } scenery_faces;
typedef struct scenery_state { int model; int in_range; } scenery_state;

/* the engine's face normal through three model vertices (0x10030410): -((P2 - P1) x (P3 - P2)), normalised; returns the
 * cross product's length as an int (ftol: the x87's 0x80000000 from 2^31 up), *ok 0 when it is 0 (left unchanged) */
static int eng_cross(const wtb_vertex *P1, const wtb_vertex *P2, const wtb_vertex *P3, float n[3], int *ok)
{
    float a[3] = {(float)P2->x - (float)P1->x, (float)P2->y - (float)P1->y, (float)P2->z - (float)P1->z};
    float b[3] = {(float)P3->x - (float)P2->x, (float)P3->y - (float)P2->y, (float)P3->z - (float)P2->z}, l;
    n[0] = -(a[1] * b[2] - a[2] * b[1]); n[1] = -(a[2] * b[0] - a[0] * b[2]); n[2] = -(a[0] * b[1] - a[1] * b[0]);
    l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    *ok = l > 0.0f;
    if (!*ok) return 0;
    n[0] /= l; n[1] /= l; n[2] /= l;
    return l < 2147483648.0f ? (int)l : INT_MIN;
}

/* the engine's normal of polygon pg of part wp (world), 0 when degenerate. The model loader stores the file's normals
 * (negated, 0x10042140) but then recomputes every polygon's normal from its vertices - as built, i.e. with the OBJ scale
 * (0x10042140 -> 0x100298c0 -> 0x10029210 per polygon, overwriting +0x08 and +0x14): a triangle through its three
 * vertices (0x10030410); a larger polygon from its longest edge (edges idx[0]-idx[n-1], idx[n-1]-idx[n-2] ... idx[1]-idx[0],
 * the first longest) and its other vertices from the last: the first whose cross product is at least 17 long (ftol),
 * else the longest one; an edge shorter than 1e-7 (0x102476f8) leaves (1, 0, 0). The ground (0x1000fff0), the quadtree's
 * "normal y above 0" (0x1000cf80) and the face tests use it. The 3Dfx archive's stored normals are those of the unscaled
 * model (PINKMTN2's hills, y x 2 / x z x 2: their slope halved or doubled) and some differ anyway (PINKMTN1 TLAMTFSA) */
static int eng_poly_normal(const mech3d_part *wp, const wtb_poly *pg, float nw[3])
{
    const wtb_object *o = &wp->model.objects[0];
    float n[3] = {1.0f, 0.0f, 0.0f};
    int j, cnt = pg->n > WTB_MAX_POLY_VERTS ? WTB_MAX_POLY_VERTS : pg->n;
    for (j = 0; j < cnt; j++) if (pg->idx[j] >= o->vert_count) return 0;
    if (cnt < 3) return 0;
    if (cnt == 3) {
        int ok;
        eng_cross(&o->verts[pg->idx[0]], &o->verts[pg->idx[1]], &o->verts[pg->idx[2]], n, &ok);
        if (!ok) return 0;   /* (the engine divides by 0) */
    } else {
        const wtb_vertex *prev, *cur = &o->verts[pg->idx[0]], *e0 = NULL, *e1 = NULL;
        float best = 0.0f;
        int k, i0 = -1, i1 = -1, kbest = -1;
        for (k = cnt - 1; k >= 0; k--) {
            float d[3], l;
            int ip = k == cnt - 1 ? 0 : k + 1;
            prev = cur; cur = &o->verts[pg->idx[k]];
            d[0] = (float)prev->x - (float)cur->x; d[1] = (float)prev->y - (float)cur->y; d[2] = (float)prev->z - (float)cur->z;
            l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (best < l) { best = l; e0 = cur; e1 = prev; i0 = k; i1 = ip; }
        }
        if (best >= 1e-7f && e0 && e1) {
            for (k = cnt - 1; k >= 0; k--) {
                float m[3];
                int len, ok;
                if (k == i0 || k == i1 || pg->idx[k] == pg->idx[i0] || pg->idx[k] == pg->idx[i1]) continue;
                len = eng_cross(e0, e1, &o->verts[pg->idx[k]], m, &ok);
                if (kbest < len && ok) { memcpy(n, m, sizeof n); kbest = len; }
                if (len >= 0x11) break;
            }
        }
    }
    if (wp->has_rot) {   /* the object's turn (part_vertex without the offset) */
        const float *R = wp->rot;
        for (j = 0; j < 3; j++) nw[j] = R[3 * j] * n[0] + R[3 * j + 1] * n[1] + R[3 * j + 2] * n[2];
    } else {
        float c = cosf(wp->yaw * 3.14159265f / 180.0f), sn = sinf(wp->yaw * 3.14159265f / 180.0f);
        nw[0] = c * n[0] + sn * n[2]; nw[1] = n[1]; nw[2] = -sn * n[0] + c * n[2];
    }
    return 1;
}

/* a part's polygons as engine faces (world vertices, normal, extent) */
static void cpoly_fill(cpoly_t *c, const mech3d_part *wp, const wtb_poly *pg, const float nrm[3])
{
    int j, a;
    c->n = pg->n > WTB_MAX_POLY_VERTS ? WTB_MAX_POLY_VERTS : pg->n;
    for (a = 0; a < 3; a++) { c->mn[a] = 3.4e38f; c->mx[a] = -3.4e38f; c->nrm[a] = nrm[a]; }
    for (j = 0; j < c->n; j++) {
        part_vertex(wp, &wp->model.objects[0].verts[pg->idx[j]], c->v[j]);
        for (a = 0; a < 3; a++) { if (c->v[j][a] < c->mn[a]) c->mn[a] = c->v[j][a]; if (c->v[j][a] > c->mx[a]) c->mx[a] = c->v[j][a]; }
    }
}

/* Projected face test: DOS 0x1ca00, 3Dfx 0x10010a90, Titanium 16-bit
 * 0x1001941a. The bounds test uses [min, max) on each axis. Visit the
 * edges in reverse order, stopping at the first crossing on each side
 * or rejecting a second crossing on the same side. */
static int cpoly_in(const cpoly_t *p, int a, int b, float x, float y)
{
    int k, bits = 0, left = 0, right = 0;
    const float *Q;
    if (p->n < 3) return 0;
    for (k = p->n - 1; k >= 0 && bits != 0xf; k--) {
        bits |= x < p->v[k][a] ? 2 : 1;
        bits |= y < p->v[k][b] ? 8 : 4;
    }
    if (bits != 0xf) return 0;
    Q = p->v[0];
    for (k = p->n - 1; k >= 0; Q = p->v[k], k--) {
        const float *P = p->v[k];
        int side;   /* -1 left, 1 right */
        if (!((y <= P[b] || y <= Q[b]) && (P[b] <= y || Q[b] <= y))) continue;
        if (y == P[b] && P[b] == Q[b]) {
            if (!msim_dos_int)
                return fminf(P[a], Q[a]) <= x && x <= fmaxf(P[a], Q[a]);
            if (P[a] < x && Q[a] < x) return 0;
            if (P[a] <= x) return 1;
            return x < Q[a] ? 0 : 1;
        }
        if (y == P[b]) continue;
        if (P[a] < x && Q[a] < x) side = -1;
        else if (x < P[a] && x < Q[a]) side = 1;
        else {
            float delta;
            if (msim_dos_int) {
                float xi = truncf((Q[a] - P[a]) * (y - P[b]) / (Q[b] - P[b])) + P[a];
                delta = xi - x;
            } else {
                /* Store the signed distance once, after the interpolation. */
                delta = (float)((((double)Q[a] - P[a]) * ((double)y - P[b])) /
                                ((double)Q[b] - P[b]) + ((double)P[a] - x));
            }
            if (delta == 0.0f) return 1;
            side = delta < 0.0f ? -1 : 1;
        }
        if (side < 0) { if (left) return 0; if (right) return 1; left = 1; }
        else { if (right) return 0; if (left) return 1; right = 1; }
    }
    return 0;
}

/* the quadtree of a type 5 object (0x1000c550 / 0x1000c640): the root is the vertex box and always splits; a child takes
 * the lower half of its parent's x / z range [lo, mid] or the upper [mid + 1, hi] (bits 0 / 1 of its index), exists only
 * when both ranges are non-empty, and is a leaf of the polygons whose extent touches its rectangle (in polygon order,
 * their y range; none: y 0 - 0) unless more than 25 do: then it splits again (y range from its children) */
static int qt_new(terrain_t *t)
{
    if (t->nqn == t->capqn) {
        qnode_t *q = realloc(t->qn, (size_t)(t->capqn ? t->capqn * 2 : 256) * sizeof *q);
        if (!q) return -1;
        t->qn = q; t->capqn = t->capqn ? t->capqn * 2 : 256;
    }
    memset(&t->qn[t->nqn], 0, sizeof t->qn[0]);
    t->qn[t->nqn].ch[0] = t->qn[t->nqn].ch[1] = t->qn[t->nqn].ch[2] = t->qn[t->nqn].ch[3] = -1;
    return t->nqn++;
}
static int qt_child(terrain_t *t, const cobj_t *o, int parent, int b)
{
    float r[6], ylo = 3.4e38f, yhi = -3.4e38f, pr[6];
    int k, cnt = 0, list[25], ni, c;
    memcpy(pr, t->qn[parent].r, sizeof pr);
    if (!(b & 1)) { r[0] = pr[0]; r[1] = (pr[1] + pr[0]) * 0.5f; } else { r[1] = pr[1]; r[0] = (pr[0] + pr[1]) * 0.5f + 1.0f; }
    if (!(b & 2)) { r[4] = pr[4]; r[5] = (pr[5] + pr[4]) * 0.5f; } else { r[5] = pr[5]; r[4] = (pr[4] + pr[5]) * 0.5f + 1.0f; }
    if (!(r[0] < r[1] && r[4] < r[5])) return -1;
    for (k = 0; k < o->np; k++) {
        const cpoly_t *p = &t->cp[o->p0 + k];
        if (!(p->mn[0] <= r[1] && r[0] <= p->mx[0] && p->mn[2] <= r[5] && r[4] <= p->mx[2])) continue;
        if (++cnt > 25) break;
        list[cnt - 1] = k;
        if (p->mn[1] < ylo) ylo = p->mn[1];
        if (p->mx[1] > yhi) yhi = p->mx[1];
    }
    if ((ni = qt_new(t)) < 0) return -1;
    t->qn[ni].r[0] = r[0]; t->qn[ni].r[1] = r[1]; t->qn[ni].r[4] = r[4]; t->qn[ni].r[5] = r[5];
    if (cnt > 25) {
        ylo = 3.4e38f; yhi = -3.4e38f;
        for (c = 0; c < 4; c++) {
            int ch = qt_child(t, o, ni, c);
            t->qn[ni].ch[c] = ch;
            if (ch < 0) continue;
            if (t->qn[ch].r[3] > yhi) yhi = t->qn[ch].r[3];
            if (t->qn[ch].r[2] < ylo) ylo = t->qn[ch].r[2];
        }
        if (ylo == 3.4e38f) ylo = yhi = 0.0f;
        t->qn[ni].r[2] = ylo; t->qn[ni].r[3] = yhi;
        return ni;
    }
    if (cnt == 0) ylo = yhi = 0.0f;
    t->qn[ni].r[2] = ylo; t->qn[ni].r[3] = yhi;
    if (t->nqidx + cnt > t->capqidx) {
        int nc = t->capqidx ? t->capqidx * 2 : 1024, *q;
        while (nc < t->nqidx + cnt) nc *= 2;
        if (!(q = realloc(t->qidx, (size_t)nc * sizeof *q))) return ni;
        t->qidx = q; t->capqidx = nc;
    }
    t->qn[ni].p0 = t->nqidx; t->qn[ni].np = cnt;
    for (k = 0; k < cnt; k++) t->qidx[t->nqidx++] = o->p0 + list[k];
    return ni;
}

static int clist_append_faces(terrain_t *t, const mech3d_part *part)
{
    const wtb_object *model = &part->model.objects[0];
    int first = t->ncp;
    for (int k = 0; k < model->poly_count; k++) {
        const wtb_poly *poly = &model->polys[k];
        float normal[3];
        if (!eng_poly_normal(part, poly, normal)) continue;
        if (t->ncp == t->capcp) {
            int capacity = t->capcp ? t->capcp * 2 : 1024;
            cpoly_t *next = realloc(t->cp, (size_t)capacity * sizeof *next);
            if (!next) break;
            t->cp = next;
            t->capcp = capacity;
        }
        cpoly_fill(&t->cp[t->ncp++], part, poly, normal);
    }
    return t->ncp - first;
}

static void clist_scenery_faces(terrain_t *t, const mech3d *w, cobj_t *c, const mech3d_part *part)
{
    const int count = part->model.object_count;
    for (int i = 0; i < count; i++)
        if (!(part->model.objects[i].flags & 0x1000)) return;

    if (!t->dos_scenery)
        t->dos_scenery = calloc((size_t)w->part_count, sizeof *t->dos_scenery);
    if (!t->dos_scenery) return;
    if (t->scenery_count + count > t->scenery_capacity) {
        int capacity = t->scenery_count + count + 64;
        scenery_faces *faces = realloc(t->scenery_faces, (size_t)capacity * sizeof *faces);
        if (!faces) return;
        t->scenery_faces = faces;
        t->scenery_capacity = capacity;
    }

    c->lod_first = t->scenery_count;
    c->lod_count = count;
    mech3d_part pose = *part;
    for (int i = 0; i < count; i++) {
        pose.model.objects = &part->model.objects[i];
        const char *suffix = strchr(pose.model.objects->name, '_');
        const long distance = suffix && isdigit((unsigned char)suffix[1]) ? strtol(suffix + 1, NULL, 10) : 0;
        scenery_faces *faces = &t->scenery_faces[t->scenery_count++];
        faces->distance = distance <= INT_MAX ? (int)distance : INT_MAX;
        faces->first = i == 0 ? c->p0 : t->ncp;
        faces->count = i == 0 ? c->np : clist_append_faces(t, &pose);
    }

    /* 0x383bb leaves the last loaded model selected until 0x3cda0 poses it. */
    memset(pose.pos, 0, sizeof pose.pos);
    pose.has_rot = pose.has_dos_rot = 0;
    pose.yaw = 0;
    c->unposed_first = t->ncp;
    c->unposed_count = clist_append_faces(t, &pose);
}

static void clist_build(terrain_t *t, const mech3d *w, const int *seq, int nseq)
{
    int i, k, j;
    t->co = calloc((size_t)(w->part_count > 0 ? w->part_count : 1), sizeof *t->co);
    if (!t->co) return;
    for (i = 0; i < w->part_count; i++) {
        const mech3d_part *p = &w->parts[i];
        const wtb_object *o;
        cobj_t *c;
        float bmn[3] = {3.4e38f, 3.4e38f, 3.4e38f}, bmx[3] = {-3.4e38f, -3.4e38f, -3.4e38f}, lmn[3] = {3.4e38f, 3.4e38f, 3.4e38f}, lmx[3] = {-3.4e38f, -3.4e38f, -3.4e38f}, r2 = 0, l2 = 0;
        if (p->coll < 0 || p->coll > 9 || p->coll == 4 || p->hidden || p->model.object_count < 1 || p->model.objects[0].vert_count < 1) continue;
        o = &p->model.objects[0];
        c = &t->co[t->nco];
        memset(c, 0, sizeof *c);
        c->part = i; c->type = p->coll; c->moving = p->moving; c->root = -1;
        for (k = 0; k < o->vert_count; k++) {
            float q[3], lq[3] = {(float)o->verts[k].x, (float)o->verts[k].y, (float)o->verts[k].z};
            part_vertex(p, &o->verts[k], q);
            for (j = 0; j < 3; j++) {
                if (q[j] < bmn[j]) bmn[j] = q[j];
                if (q[j] > bmx[j]) bmx[j] = q[j];
                if (lq[j] < lmn[j]) lmn[j] = lq[j];
                if (lq[j] > lmx[j]) lmx[j] = lq[j];
            }
        }
        for (j = 0; j < 3; j++) { c->box[2 * j] = bmn[j]; c->box[2 * j + 1] = bmx[j]; c->sph[j] = (bmn[j] + bmx[j]) * 0.5f; c->lsph[j] = (lmn[j] + lmx[j]) * 0.5f; }
        for (k = 0; k < o->vert_count; k++) {   /* the bounding sphere (+0x34 / +0x40): the box centre, the farthest vertex */
            float q[3], d2 = 0, e2 = 0, lq[3] = {(float)o->verts[k].x, (float)o->verts[k].y, (float)o->verts[k].z};
            part_vertex(p, &o->verts[k], q);
            for (j = 0; j < 3; j++) { d2 += (q[j] - c->sph[j]) * (q[j] - c->sph[j]); e2 += (lq[j] - c->lsph[j]) * (lq[j] - c->lsph[j]); }
            if (d2 > r2) r2 = d2;
            if (e2 > l2) l2 = e2;
        }
        c->sph[3] = sqrtf(r2); c->lsph[3] = sqrtf(l2);
        if (!c->moving && (c->type == 2 || c->type == 5 || c->type == 7)) {
            c->p0 = t->ncp;
            c->np = clist_append_faces(t, p);
            if (msim_dos_int && c->type == 7 && !p->model.extended && p->model.object_count > 1)
                clist_scenery_faces(t, w, c, p);
        }
        t->nco++;
    }
    if (seq && nseq == w->part_count) {   /* list order: the last created first (msim clist_seq; ties keep world order) */
        int a, b;
        for (a = 1; a < t->nco; a++) {
            cobj_t q = t->co[a];
            int sq = seq[q.part];
            for (b = a; b > 0 && seq[t->co[b - 1].part] < sq; b--) t->co[b] = t->co[b - 1];
            t->co[b] = q;
        }
    }
    for (i = 0; i < t->nco; i++) {   /* the type 5 quadtrees (after the polygon array stops moving) */
        cobj_t *c = &t->co[i];
        int r, b;
        if (c->type != 5 || c->moving) continue;
        if ((r = qt_new(t)) < 0) break;
        t->qn[r].r[0] = c->box[0]; t->qn[r].r[1] = c->box[1]; t->qn[r].r[2] = c->box[2];
        t->qn[r].r[3] = c->box[3]; t->qn[r].r[4] = c->box[4]; t->qn[r].r[5] = c->box[5];
        c->root = r;
        for (b = 0; b < 4; b++) { int ch = qt_child(t, c, r, b); t->qn[r].ch[b] = ch; }
    }
    t->ccell = calloc((size_t)t->gw * (size_t)t->gh, sizeof *t->ccell);
    for (i = 0; t->ccell && i < t->nco; i++) {   /* each fixed entry in the cells its box covers (in list order) */
        const cobj_t *c = &t->co[i];
        int cx0, cx1, cz0, cz1, cx, cz;
        if (c->moving) continue;
        cx0 = (int)((c->box[0] - t->x0) / t->cs); cx1 = (int)((c->box[1] - t->x0) / t->cs);
        cz0 = (int)((c->box[4] - t->z0) / t->cs); cz1 = (int)((c->box[5] - t->z0) / t->cs);
        if (cx0 < 0) cx0 = 0;
        if (cz0 < 0) cz0 = 0;
        if (cx1 >= t->gw) cx1 = t->gw - 1;
        if (cz1 >= t->gh) cz1 = t->gh - 1;
        for (cz = cz0; cz <= cz1; cz++)
            for (cx = cx0; cx <= cx1; cx++) cell_add(&t->ccell[cz * t->gw + cx], i);
    }
}

/* 0x3f500 compares the centre against the clip planes, expanded by the sphere radius. */
static int dos_scenery_visible(const int32_t point[3], int32_t radius, int32_t near, int32_t far,
                               const int32_t focal[2], const int32_t normal[2])
{
    const int32_t depth = point[2];
    if (depth + radius < near || (far > 0 && depth - radius > far)) return 0;
    for (int axis = 0; axis < 2; axis++) {
        int64_t distance = (int64_t)abs(point[axis]) * focal[axis] - (int64_t)depth * 65536;
        if (distance / normal[axis] > radius) return 0;
    }
    return 1;
}

/* 0x3cda0 walks the thresholds in ascending order. Equality selects the new model. */
static int dos_scenery_level(const scenery_faces *models, int count, int32_t depth, int32_t scale)
{
    int first = 0, selected = -1;
    for (int i = 0; i < count; i++) {
        if (models[i].distance < models[first].distance) first = i;
        if (models[i].distance == 0) continue;
        int64_t threshold = ((int64_t)models[i].distance * scale + 32768) >> 16;
        if (depth >= threshold && (selected < 0 || models[i].distance >= models[selected].distance))
            selected = i;
    }
    if (models[first].distance == 0 || selected < 0) return first;
    return selected;
}

static void dos_prepare_scenery(msim *s, const msim_camera *camera)
{
    terrain_t *t = (terrain_t *)s->terrain;
    if (!t || !t->dos_scenery) return;
    const dos_matrix axes = dos_matrix_rotation(
        (int32_t)lrint(-(double)camera->pitch * 65536.0),
        (int32_t)lrint((double)camera->yaw * 65536.0), 0);
    const int32_t focal[2] = {camera->focal_x, camera->focal_y};
    const int32_t normal[2] = {
        (int32_t)llround(hypot(65536.0, focal[0])), (int32_t)llround(hypot(65536.0, focal[1]))
    };
    int32_t eye[3];
    for (int axis = 0; axis < 3; axis++) eye[axis] = (int32_t)lrintf(camera->eye[axis]);
    const int32_t far = (int32_t)camera->far_clip;
    const int32_t scale = (int32_t)(camera->lod_scale * 65536.0f);
    for (int i = 0; i < t->nco; i++) {
        const cobj_t *c = &t->co[i];
        if (!c->lod_count) continue;
        int32_t delta[3], point[3];
        int32_t radius = (int32_t)lrintf(c->sph[3]);
        int64_t distance2 = 0;
        for (int axis = 0; axis < 3; axis++) {
            delta[axis] = (int32_t)lrintf(c->sph[axis]) - eye[axis];
            distance2 += (int64_t)delta[axis] * delta[axis];
        }
        const int64_t limit = (int64_t)far + radius;
        if (far > 0 && distance2 > limit * limit) continue;
        scenery_state *state = &t->dos_scenery[c->part];
        state->in_range = 1;
        for (int axis = 0; axis < 3; axis++) {
            int64_t dot = 0;
            for (int j = 0; j < 3; j++) dot += (int64_t)axes.r[j * 3 + axis] * delta[j];
            point[axis] = (int32_t)((dot + (1 << 28)) >> 29);
        }
        if (!dos_scenery_visible(point, radius, camera->near_clip, far, focal, normal)) continue;
        state->model = 1 + dos_scenery_level(&t->scenery_faces[c->lod_first], c->lod_count, point[2], scale);
    }
}

/* The renderer keeps a static world mesh, so only rebuild it when a
 * visible object's depth crosses a detail threshold. */
int msim_scenery_draw_lods(const msim *s, mech3d *world)
{
    const terrain_t *t = (const terrain_t *)s->terrain;
    int changed = 0;
    if (s->world != world || !t || !t->dos_scenery) return 0;
    for (int i = 0; i < t->nco; i++) {
        const cobj_t *c = &t->co[i];
        const int model = t->dos_scenery[c->part].model;
        if (!c->lod_count || !model) continue;
        mech3d_part *p = &world->parts[c->part];
        if (model > p->model.object_count || p->dos_draw_object == model - 1) continue;
        p->dos_draw_object = model - 1;
        changed = 1;
    }
    return changed;
}

static scenery_faces clist_faces(const msim *s, const terrain_t *t, const cobj_t *c, int movement)
{
    if (s->cam_ok && c->lod_count) {
        const scenery_state *state = &t->dos_scenery[c->part];
        /* Nearby walls must keep their movement collision even when behind the camera. */
        const int lod = movement ? state->in_range : state->model;
        if (!lod) return (scenery_faces){c->unposed_first, c->unposed_count, 0};
        if (lod <= c->lod_count) return t->scenery_faces[c->lod_first + lod - 1];
    }
    return (scenery_faces){c->p0, c->np, 0};
}

/* A moving entry uses its current world box and bounding sphere. */
static int cobj_now(const msim *s, const cobj_t *c, float box[6], float sph[4])
{
    const terrain_t *t = (const terrain_t *)s->terrain;
    const mech3d_part *p;
    int k;
    if (!c->moving) { memcpy(box, c->box, sizeof c->box); memcpy(sph, c->sph, sizeof c->sph); return 1; }
    if (!s->world || c->part >= s->world->part_count) return 0;
    p = &s->world->parts[c->part];
    if (p->hidden) return 0;
    for (k = 0; k < t->nmov; k++) if (t->mov[k] == c->part) break;
    if (k == t->nmov || t->mov_box[k][0] < -1e29f) return 0;
    box[0] = t->mov_box[k][0]; box[1] = t->mov_box[k][3]; box[2] = t->mov_box[k][1];
    box[3] = t->mov_box[k][4]; box[4] = t->mov_box[k][2]; box[5] = t->mov_box[k][5];
    {
        wtb_vertex v;
        memset(&v, 0, sizeof v);
        v.x = (int32_t)lrintf(c->lsph[0]); v.y = (int32_t)lrintf(c->lsph[1]); v.z = (int32_t)lrintf(c->lsph[2]);
        part_vertex(p, &v, sph);
        sph[3] = c->lsph[3];
    }
    return 1;
}

/* the list entries that may hold point / segment (x0, z0) - (x1, z1): the fixed ones of the cells it crosses and the
 * moving ones, in list order; returns the count (out has room for n) */
static int clist_near(const msim *s, float x0, float z0, float x1, float z1, int *out, int n)
{
    terrain_t *t = (terrain_t *)s->terrain;
    int cnt = 0, cx0, cx1, cz0, cz1, cx, cz, k, a, b;
    if (!t || !t->co) return 0;
    if (x1 < x0) { float q = x0; x0 = x1; x1 = q; }
    if (z1 < z0) { float q = z0; z0 = z1; z1 = q; }
    t->stamp_now++;
    if (t->ccell) {
        cx0 = (int)floorf((x0 - t->x0) / t->cs); cx1 = (int)floorf((x1 - t->x0) / t->cs);
        cz0 = (int)floorf((z0 - t->z0) / t->cs); cz1 = (int)floorf((z1 - t->z0) / t->cs);
        if (cx0 < 0) cx0 = 0;
        if (cz0 < 0) cz0 = 0;
        if (cx1 >= t->gw) cx1 = t->gw - 1;
        if (cz1 >= t->gh) cz1 = t->gh - 1;
        for (cz = cz0; cz <= cz1; cz++)
            for (cx = cx0; cx <= cx1; cx++) {
                const cell_t *c = &t->ccell[cz * t->gw + cx];
                for (k = 0; k < c->n; k++) {
                    int ci = c->idx[k], pi = t->co[ci].part;
                    if (t->stamp[pi] == t->stamp_now) continue;
                    t->stamp[pi] = t->stamp_now;
                    if (cnt < n) out[cnt++] = ci;
                }
            }
    }
    for (k = 0; k < t->nco; k++)   /* the moving ones, and every sphere (its reach can pass its box) */
        if ((t->co[k].moving || t->co[k].type == 6) && t->stamp[t->co[k].part] != t->stamp_now && cnt < n) {
            t->stamp[t->co[k].part] = t->stamp_now;
            out[cnt++] = k;
        }
    for (a = 1; a < cnt; a++)   /* list order */
        for (b = a; b > 0 && out[b - 1] > out[b]; b--) { int q = out[b]; out[b] = out[b - 1]; out[b - 1] = q; }
    return cnt;
}

/* engine 0x1000cf80 on a type 5 object's quadtree: inside the root rectangle, the first polygon (in polygon order) of
 * the leaf with a normal y above 0 (the float's bits as an int > 0) that holds (x, z) gives the height on its plane through
 * its first vertex (0x1000fff0); none - a hole, or a point in the 1 cm seam between two children - gives 0 */
static int qt_ground(const terrain_t *t, int ni, float x, float z, float *h, const float **nrm)
{
    const qnode_t *q;
    int k;
    if (ni < 0) return 0;
    q = &t->qn[ni];
    if (x < q->r[0] || x > q->r[1] || z < q->r[4] || z > q->r[5]) return 0;
    if (q->np > 0) {
        for (k = 0; k < q->np; k++) {
            const cpoly_t *p = &t->cp[t->qidx[q->p0 + k]];
            if (p->nrm[1] > 0.0f && cpoly_in(p, 0, 2, x, z)) {
                *h = p->v[0][1] - ((x - p->v[0][0]) * p->nrm[0] + (z - p->v[0][2]) * p->nrm[2]) / p->nrm[1];
                *nrm = p->nrm;
                return 1;
            }
        }
        *h = 0.0f; *nrm = NULL;
        return 1;
    }
    for (k = 0; k < 4; k++) if (qt_ground(t, q->ch[k], x, z, h, nrm)) return 1;
    *h = 0.0f; *nrm = NULL;
    return 1;
}

/* ---- segments (0x1000ae00: start, end, end - start, the unit direction, the length) ---- */
typedef struct { float p[3], e[3], d[3], u[3], len; } eseg_t;
static void eseg_set(eseg_t *g, const float p[3], const float e[3])
{
    int a;
    for (a = 0; a < 3; a++) { g->p[a] = p[a]; g->e[a] = e[a]; g->d[a] = e[a] - p[a]; }
    g->len = sqrtf(g->d[0] * g->d[0] + g->d[1] * g->d[1] + g->d[2] * g->d[2]);
    for (a = 0; a < 3; a++) g->u[a] = g->len > 0 ? g->d[a] / g->len : 0.0f;
}
static void eseg_len(eseg_t *g, float L)   /* 0x1000b060 / 0x1000b160: the end moved to length L along the direction */
{
    int a;
    g->len = L;
    for (a = 0; a < 3; a++) { g->e[a] = g->p[a] + g->u[a] * L; g->d[a] = g->e[a] - g->p[a]; }
}

/* the slab test (0x1000b230 per axis; 0x1000d310 / 0x1000ccf0): the latest entry (x, then y, then z on a tie), the
 * earliest exit; an entry before the start counts as 0 (a segment starting inside the box hits it at once); the exit
 * no later than the length. *axis: the entry's axis */
static int eseg_slab(const eseg_t *g, const float b[6], float *tin, int *axis)
{
    float tn = -3.4e38f, tx = 3.4e38f;
    int a, ax = 0;
    for (a = 0; a < 3; a++) {
        float lo = b[2 * a], hi = b[2 * a + 1], en, ex;
        if (fabsf(g->u[a]) < 1e-7f) {
            if (g->p[a] < lo || g->p[a] > hi) return 0;
            en = -3.4e38f; ex = 3.4e38f;
        } else {
            en = (lo - g->p[a]) / g->u[a]; ex = (hi - g->p[a]) / g->u[a];
            if (ex < en) { float q = en; en = ex; ex = q; }
        }
        if (a == 0) { tn = en; tx = ex; }
        else { if (tn < en) { tn = en; ax = a; } if (ex < tx) tx = ex; }
    }
    if (tx < tn) return 0;
    if (tn < 0) tn = 0;
    if (g->len < tx) tx = g->len;
    if (tx < tn) return 0;
    *tin = tn; *axis = ax;
    return 1;
}

/* a face (0x10010830): crossed from its front (direction . normal < 0, the start more than 1e-7 in front of it) within
 * the segment's present length, the crossing inside the polygon (projected on the plane of the normal's largest
 * component, 0x100109e0); the segment then ends there */
static int eseg_poly(const cpoly_t *p, eseg_t *g, float n[3])
{
    float dn = p->nrm[0] * g->u[0] + p->nrm[1] * g->u[1] + p->nrm[2] * g->u[2], f, t, Q[3];
    if (!(dn < 0.0f) || p->n < 3) return 0;
    f = p->nrm[0] * (g->p[0] - p->v[0][0]) + p->nrm[1] * (g->p[1] - p->v[0][1]) + p->nrm[2] * (g->p[2] - p->v[0][2]);
    if (f < 1e-7f) return 0;
    t = -(f / dn);
    if (!(t >= 1e-7f) || t > g->len) return 0;
    Q[0] = g->p[0] + g->u[0] * t; Q[1] = g->p[1] + g->u[1] * t; Q[2] = g->p[2] + g->u[2] * t;
    {
        float ax = fabsf(p->nrm[0]), ay = fabsf(p->nrm[1]), az = fabsf(p->nrm[2]);
        int in = ax < ay && az < ay ? cpoly_in(p, 0, 2, Q[0], Q[2]) : az < ax ? cpoly_in(p, 1, 2, Q[1], Q[2]) : cpoly_in(p, 0, 1, Q[0], Q[1]);
        if (!in) return 0;
    }
    eseg_len(g, t);
    n[0] = p->nrm[0]; n[1] = p->nrm[1]; n[2] = p->nrm[2];
    return 1;
}

/* a type 5 quadtree (0x1000ccf0 / 0x1000ced0): node boxes by the slab test; a leaf - its first polygon crossed; an inner
 * node - the nearest of its children's hits (each tried with the whole segment). The normal left is the last face
 * crossed (the engine's DAT_1024b408), not necessarily the nearest */
static int qt_seg(const terrain_t *t, int ni, eseg_t *g, float n[3])
{
    const qnode_t *q;
    float tn;
    int ax, k;
    if (ni < 0) return 0;
    q = &t->qn[ni];
    if (!eseg_slab(g, q->r, &tn, &ax)) return 0;
    if (q->np > 0) {
        for (k = 0; k < q->np; k++) if (eseg_poly(&t->cp[t->qidx[q->p0 + k]], g, n)) return 1;
        return 0;
    }
    {
        float best = 3.4e38f;
        eseg_t keep = *g;
        for (k = 0; k < 4; k++) {
            eseg_t gc = *g;
            if (qt_seg(t, q->ch[k], &gc, n) && gc.len < best) { best = gc.len; keep = gc; }
        }
        if (best < 2147483648.0f) { *g = keep; return 1; }
        return 0;
    }
}

/* 0x10029db0: the bounding sphere's entry along the segment - 10 when the start is inside, 3.4e38 when it misses or
 * lies past the end, 0 when the projection is shorter than the radius */
static float eseg_sphere(const float sp[4], const eseg_t *g)
{
    float v[3] = {sp[0] - g->p[0], sp[1] - g->p[1], sp[2] - g->p[2]}, c2, b, tp, e;
    if (!(sp[3] >= 1e-7f)) return 3.4e38f;
    c2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] - sp[3] * sp[3];
    if (c2 < 0) return 10.0f;
    b = v[0] * g->d[0] + v[1] * g->d[1] + v[2] * g->d[2];
    if (b < 0) return 3.4e38f;
    tp = b / g->len;
    if (tp * tp - c2 < 0) return 3.4e38f;
    e = tp - sp[3];
    if (e < 0) return 0.0f;
    if (g->len <= e) return 3.4e38f;
    return e;
}

/* one entry against the segment (0x10010650 with its type's segment test); the segment ends at the hit, n the hit's
 * normal (DAT_1024b408..410) */
static int cobj_seg(const msim *s, const cobj_t *c, const float box[6], const float sph[4],
                    float ent, eseg_t *g, float n[3], int movement)
{
    const terrain_t *t = (const terrain_t *)s->terrain;
    if (!(g->len > 0.0f)) return 1;
    switch (c->type) {
    case 0: case 1: {   /* 0x1000d310: the box; the normal on the entry axis against the move */
        float tn;
        int ax;
        if (!eseg_slab(g, box, &tn, &ax)) return 0;
        eseg_len(g, tn);
        n[0] = n[1] = n[2] = 0;
        n[ax] = g->d[ax] > 0.0f ? -1.0f : 1.0f;   /* end - start after the cut (so +1 for a hit at the start itself) */
        return 1;
    }
    case 2: case 7: {   /* 0x1000d7b0: the first face crossed, in polygon order */
        int k;
        if (c->moving) {   /* where the path task has it now */
            const mech3d_part *p = &s->world->parts[c->part];
            const wtb_object *o = &p->model.objects[0];
            for (k = 0; k < o->poly_count; k++) {
                cpoly_t cp;
                float nrm[3];
                if (o->polys[k].n < 3 || !eng_poly_normal(p, &o->polys[k], nrm)) continue;
                cpoly_fill(&cp, p, &o->polys[k], nrm);
                if (eseg_poly(&cp, g, n)) return 1;
            }
            return 0;
        }
        const scenery_faces faces = clist_faces(s, t, c, movement);
        for (k = 0; k < faces.count; k++) if (eseg_poly(&t->cp[faces.first + k], g, n)) return 1;
        return 0;
    }
    case 5:   /* 0x1000d680 -> 0x1000ccf0 */
        return c->root >= 0 && qt_seg(t, c->root, g, n);
    default: {   /* 3, 6, 8, 9: 0x100107b0 - the sphere: hit at its entry (the end stays when the entry is 0), the normal
                  * from its centre to the start (entry 10: the start inside) or to the end */
        const float *q;
        float l;
        if (ent > 0.0f) eseg_len(g, ent);
        q = ent == 10.0f ? g->p : g->e;
        n[0] = q[0] - sph[0]; n[1] = q[1] - sph[1]; n[2] = q[2] - sph[2];
        l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 0) { n[0] /= l; n[1] /= l; n[2] /= l; }
        return 1;
    }
    }
}

typedef enum { CLIST_PROBE, CLIST_MOVE } clist_query;

/* Moves can replace a hit with a farther face when the new object's sphere is entered first (0x10010530).
 * Probes and projectiles keep the nearest face. Movement handles type 6 separately. */
static int clist_seg(const msim *s, const float p0[3], const float p1[3], clist_query query,
                     float *t, float n[3], int *ci)
{
    const terrain_t *tt = (const terrain_t *)s->terrain;
    eseg_t g0;
    int cand[4096], nc, k, hit = -1;
    float best = 3.4e38f;
    if (!tt || !tt->co) return 0;
    eseg_set(&g0, p0, p1);
    if (!(g0.len > 0.0f)) return 0;
    nc = clist_near(s, p0[0], p0[2], p1[0], p1[2], cand, 4096);
    for (k = 0; k < nc; k++) {
        const cobj_t *c = &tt->co[cand[k]];
        float box[6], sph[4], e, nn[3] = {0, 1, 0};
        eseg_t g;
        if (query == CLIST_MOVE && c->type == 6) continue;
        if (!cobj_now(s, c, box, sph)) continue;
        if (tt->thing_of && c->part < tt->nparts && tt->thing_of[c->part] >= 0 && s->bld[tt->thing_of[c->part]].hp <= 0) continue;
        e = eseg_sphere(sph, &g0);
        if (!(e < best)) continue;
        g = g0;
        if (!cobj_seg(s, c, box, sph, e, &g, nn, query == CLIST_MOVE)) continue;
        if (query != CLIST_MOVE && !(g.len < best)) continue;
        best = g.len; hit = cand[k];
        n[0] = nn[0]; n[1] = nn[1]; n[2] = nn[2];
    }
    if (hit < 0) return 0;
    *t = best; *ci = hit;
    return 1;
}

typedef struct {
    int32_t start[3], delta[3], dir[3], length;   /* integer cm and 16.16 direction */
    int long_step;
} dos_shot_segment;

static dos_shot_segment dos_shot_segment_make(const float o[3], const float dv[3], int use_long_steps)
{
    dos_shot_segment g;
    for (int i = 0; i < 3; i++) {
        g.start[i] = (int32_t)o[i];
        g.dir[i] = g.delta[i] = (int32_t)(o[i] + dv[i]) - g.start[i];
    }
    g.length = dos_normalize(g.dir);
    g.long_step = use_long_steps && g.length > 3000;
    if (g.long_step) {   /* 0x5088f: 0x1d9f0 for a long step, 0x1d960 otherwise */
        const int32_t *d = g.delta;
        double len = sqrt((double)d[0] * d[0] + (double)d[1] * d[1] + (double)d[2] * d[2]);
        double inv = 1.0 / len;
        g.length = (int32_t)(len + 0.5);
        for (int i = 0; i < 3; i++) g.dir[i] = (int32_t)((d[i] * inv) * 65536.0);
    }
    return g;
}

/* 0x5093c derives a second position 100 cm back along the ray. The unit bullet impact
 * at 0x510c1 uses the backed X and Z with the actual contact height. */
static void dos_impact_effect(const float dv[3], const float hit[3], float out[3])
{
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    const dos_shot_segment g = dos_shot_segment_make(origin, dv, 1);
    for (int i = 0; i < 3; i++)
        out[i] = i == 1 ? hit[i] : (float)((int32_t)lroundf(hit[i]) - dos_projectile_mul(g.dir[i], 100));
}

/* DOS 0x3a745 tests the integer sphere, including the carry-out rejection on the final dot product add. */
static int32_t dos_shot_sphere(const dos_shot_segment *g, const float sph[4], int reject_carry)
{
    int64_t dx = (int32_t)lroundf(sph[0]) - g->start[0];
    int64_t dy = (int32_t)lroundf(sph[1]) - g->start[1];
    int64_t dz = (int32_t)lroundf(sph[2]) - g->start[2];
    int32_t r = (int32_t)lroundf(sph[3]);
    if (r <= 0) return INT32_MAX;
    int64_t offset2 = dx * dx + dy * dy + dz * dz - (int64_t)r * r;
    if (offset2 < 0) return 10;
    int64_t partial = dx * g->delta[0] + dy * g->delta[1], last = dz * g->delta[2];
    if (reject_carry && (uint64_t)partial + (uint64_t)last < (uint64_t)partial) return INT32_MAX;
    const int64_t dot = partial + last;
    if (!reject_carry && dot <= 0) return INT32_MAX;
    int64_t projected = dot / g->length;
    if (projected * projected < offset2) return INT32_MAX;
    int64_t entry = projected - r;
    return entry >= g->length ? INT32_MAX : entry > 0 ? (int32_t)entry : 0;
}

/* DOS 0x1dca0 and 0x1ec60: clipped intersection distances are integer cm along the fixed-point ray. */
static int32_t dos_shot_box(const dos_shot_segment *g, const float box[6])
{
    int32_t start = 0, end = g->length;
    for (int axis = 0; axis < 3; axis++) {
        int64_t lo = (int32_t)lroundf(box[2 * axis]) - g->start[axis];
        int64_t hi = (int32_t)lroundf(box[2 * axis + 1]) - g->start[axis];
        int32_t d = g->dir[axis];
        if (d == 0 || lo / d > 32767 || lo / d < -32768 || hi / d > 32767 || hi / d < -32768) {
            if (lo > 0 || hi < 0) return INT32_MAX;
            continue;
        }
        int32_t a = (int32_t)(lo * 65536 / d), b = (int32_t)(hi * 65536 / d);
        if (a > b) { int32_t tmp = a; a = b; b = tmp; }
        if (a > start) start = a;
        if (b < end) end = b;
        if (start > end) return INT32_MAX;
    }
    return start;
}

static void dos_shot_point(const dos_shot_segment *g, int32_t entry, float out[3])
{
    for (int i = 0; i < 3; i++) out[i] = (float)(g->start[i] + dos_projectile_mul(g->dir[i], entry));
}

/* Pieces are inserted in load order, so the last solid piece has first refusal. */
static int dos_shot_hull(const dos_shot_segment *g, const mech3d *m, unsigned gone, int reject_carry,
                         int32_t *best, int *group, float contact[3], float *centre)
{
    int hit = 0;
    for (int j = m->part_count - 1; j >= 0; j--) {
        float sphere[4];
        if (!mech3d_dos_hull_sphere(m, j, gone, sphere)) continue;
        int32_t broad = dos_shot_sphere(g, sphere, reject_carry);
        int32_t entry = broad ? broad : g->length;
        if (broad >= *best || entry >= *best) continue;
        *best = entry;
        *group = m->parts[j].group;
        hit = 1;
        if (centre) memcpy(centre, sphere, 3 * sizeof *centre);
        if (broad == 0) {
            for (int i = 0; i < 3; i++) contact[i] = (float)(g->start[i] + g->delta[i]);
        } else dos_shot_point(g, entry, contact);
    }
    return hit;
}

/* DOS 0x1c7a0 rounds each term of the plane test separately with a 3.29 normal.
 * After projecting onto the face it evaluates the crossing in integer centimetres (0x1c970). */
static int32_t dos_shot_face(const dos_shot_segment *g, const cpoly_t *p, float out[3], int32_t *normal_y)
{
    if (p->n < 3) return INT32_MAX;
    int32_t n[3], dn = 0, front = 0;
    for (int i = 0; i < 3; i++) {
        n[i] = (int32_t)llround((double)p->nrm[i] * 536870912.0);
        int32_t v = (int32_t)lroundf(p->v[0][i]);
        dn += (int32_t)(((int64_t)n[i] * g->dir[i] + (1 << 28)) >> 29);
        front += (int32_t)(((int64_t)n[i] * g->start[i] + (1 << 28)) >> 29)
               - (int32_t)(((int64_t)n[i] * v + (1 << 28)) >> 29);
    }
    if (dn >= 0 || front <= 0) return INT32_MAX;
    int64_t entry = -(int64_t)front * 65536 / dn;
    if (entry <= 0 || entry > g->length) return INT32_MAX;
    dos_shot_point(g, (int32_t)entry, out);
    int ax = abs(n[0]), ay = abs(n[1]), az = abs(n[2]);
    int drop = ax < ay && az < ay ? 1 : az < ax ? 0 : 2;
    int a = drop == 0 ? 1 : 0;
    int b = drop == 2 ? 1 : 2;
    cpoly_t rounded = {.n = p->n};
    for (int i = 0; i < p->n; i++) {
        rounded.v[i][a] = roundf(p->v[i][a]);
        rounded.v[i][b] = roundf(p->v[i][b]);
    }
    if (!cpoly_in(&rounded, a, b, out[a], out[b])) return INT32_MAX;
    *normal_y = n[1] >> 13;
    /* 0x1dbd0 measures again from the rounded point in the segment's original normalization mode. */
    int32_t d[3];
    for (int i = 0; i < 3; i++) d[i] = (int32_t)out[i] - g->start[i];
    if (!g->long_step) return dos_normalize(d);
    return (int32_t)(sqrt((double)d[0] * d[0] + (double)d[1] * d[1] + (double)d[2] * d[2]) + 0.5);
}

static int32_t dos_shot_tree(const terrain_t *t, int node, const dos_shot_segment *g, float out[3], int32_t *normal_y)
{
    if (node < 0) return INT32_MAX;
    const qnode_t *q = &t->qn[node];
    if (dos_shot_box(g, q->r) == INT32_MAX) return INT32_MAX;
    if (q->np > 0) {
        for (int i = 0; i < q->np; i++) {
            int32_t entry = dos_shot_face(g, &t->cp[t->qidx[q->p0 + i]], out, normal_y);
            if (entry != INT32_MAX) return entry;
        }
        return INT32_MAX;
    }
    int32_t best = INT32_MAX;
    for (int i = 0; i < 4; i++) {
        float hit[3];
        int32_t y;
        int32_t entry = dos_shot_tree(t, q->ch[i], g, hit, &y);
        if (entry >= best) continue;
        best = entry;
        *normal_y = y;
        memcpy(out, hit, sizeof hit);
    }
    return best;
}

typedef struct {
    float centre[3];
    int climbable, recover_terrain;
} dos_probe_hit;

/* 0x1c3f0 walks scenery and unit pieces together, newest first. A candidate
 * must beat the current distance on the broad test and on its actual contact. */
static int32_t dos_scan_segment(const msim *s, int owner, const dos_shot_segment *g,
                                int *unit, int *group, int *part, float contact[3], dos_probe_hit *probe)
{
    const terrain_t *t = (const terrain_t *)s->terrain;
    const float end[3] = {(float)(g->start[0] + g->delta[0]), (float)(g->start[1] + g->delta[1]),
                          (float)(g->start[2] + g->delta[2])};
    int candidates[4096];
    int count = clist_near(s, (float)g->start[0], (float)g->start[2], end[0], end[2], candidates, 4096);
    int hull_count = s->dos_hulls ? s->actor_count + 1 : 0;
    int32_t best = INT32_MAX;
    for (int i = 0, j = 0; i < count || j < hull_count;) {
        const cobj_t *c = i < count ? &t->co[candidates[i]] : NULL;
        int sequence = c && s->clist_seq && c->part < s->clist_seq_n ? s->clist_seq[c->part] : 0;
        if (j < hull_count && (!c || s->dos_hulls[j].sequence > sequence)) {
            const struct msim_dos_hull *h = &s->dos_hulls[j++];
            if (!h->repr || h->unit == owner) continue;
            unsigned gone = mech3d_fly_mask(h->unit < 0 ? s->player_unit.loc_gone : s->units[h->unit].loc_gone);
            const int cls = h->unit < 0 ? (int)s->player_class : s->actors[h->unit].unit_class;
            /* Player fire and its sight ray must reach the offset hull pieces of dropships and turrets. */
            const int reject_carry = owner != -1 || probe || (cls != 3 && cls != 8);
            if (dos_shot_hull(g, h->repr, gone, reject_carry, &best, group, contact,
                              probe ? probe->centre : NULL)) {
                if (probe) probe->climbable = 0;
                *unit = h->unit;
                *part = -1;
            }
            continue;
        }
        i++;
        float box[6], sph[4], point[3];
        int32_t normal_y = 0;
        if (c->type == 4) continue;    /* DOS segment handler 0x1ee70 always rejects. */
        if (!cobj_now(s, c, box, sph)) continue;
        if (t->thing_of && c->part < t->nparts && t->thing_of[c->part] >= 0 &&
            s->bld[t->thing_of[c->part]].hp <= 0) continue;
        /* A mech already blocked by a slope needs to see the same terrain
         * face its movement hit, even when the DOS sphere test drops a carry. */
        int32_t entry = dos_shot_sphere(g, sph, !(probe && probe->recover_terrain && c->type == 5));
        if (entry >= best) continue;
        if (c->type == 0 || c->type == 1) {
            entry = dos_shot_box(g, box);
            if (entry == INT32_MAX) continue;
            dos_shot_point(g, entry, point);
        } else if (c->type == 5) {
            entry = dos_shot_tree(t, c->root, g, point, &normal_y);
        } else if (c->type == 2 || c->type == 7) {
            entry = INT32_MAX;
            if (c->moving) {
                const mech3d_part *p = &s->world->parts[c->part];
                const wtb_object *model = &p->model.objects[0];
                for (int j = 0; j < model->poly_count && entry == INT32_MAX; j++) {
                    cpoly_t face;
                    float normal[3];
                    if (!eng_poly_normal(p, &model->polys[j], normal)) continue;
                    cpoly_fill(&face, p, &model->polys[j], normal);
                    entry = dos_shot_face(g, &face, point, &normal_y);
                }
            } else {
                /* A cockpit, orbit or weapon camera must leave player fire and convergence
                 * testing the same placed wall. Camera poses still feed the native AI scan. */
                const scenery_faces faces = owner == -1 && !probe
                    ? (scenery_faces){c->p0, c->np, 0} : clist_faces(s, t, c, 0);
                for (int j = 0; j < faces.count && entry == INT32_MAX; j++)
                    entry = dos_shot_face(g, &t->cp[faces.first + j], point, &normal_y);
            }
        } else if (entry == 0) {
            entry = g->length;   /* 0x1c720 leaves the endpoint alone at a zero sphere entry */
            memcpy(point, end, sizeof point);
        } else {
            dos_shot_point(g, entry, point);
        }
        if (entry >= best) continue;
        best = entry;
        *part = c->part;
        *unit = -2;
        memcpy(contact, point, sizeof point);
        if (probe) {
            memcpy(probe->centre, sph, sizeof probe->centre);
            probe->climbable = (c->type == 5 && normal_y >= 0xc41b) ||
                               (s->world && (s->world->parts[c->part].objtype & 0xf0) == 0x50);
        }
    }
    return best;
}

static int dos_shot_contact(msim *s, int owner, const float o[3], const float dv[3],
                            int *unit, int *group, int *part, float contact[3])
{
    const dos_shot_segment g = dos_shot_segment_make(o, dv, 1);
    return g.length > 0 && dos_scan_segment(s, owner, &g, unit, group, part, contact, NULL) != INT32_MAX;
}

/* The eye passes its direction and the fixed 1.5 km length to 0x1d8a0. */
static int dos_aim_contact(msim *s, int owner, const float o[3], const float direction[3],
                           int *unit, int *part, float *distance)
{
    dos_shot_segment g = {.length = 150000};
    for (int i = 0; i < 3; i++) {
        g.start[i] = (int32_t)lroundf(o[i]);
        g.dir[i] = (int32_t)lroundf(direction[i] * 65536.0f);
        g.delta[i] = dos_projectile_mul(g.dir[i], g.length);
    }
    int group = 0;
    float contact[3];
    int32_t best = dos_scan_segment(s, owner, &g, unit, &group, part, contact, NULL);
    *distance = best == INT32_MAX ? (float)g.length : (float)best;
    return best != INT32_MAX;
}

/* The AI's direct line tests (0x2bf20/0x2e150) use the octagonal segment length. */
static int dos_clear_to(msim *s, int self, int target_unit, int target_thing, const float from[3], const float to[3])
{
    const float ray[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    const dos_shot_segment g = dos_shot_segment_make(from, ray, 0);
    int unit = -2, group = 0, part = -1;
    float contact[3];
    if (g.length <= 0 || dos_scan_segment(s, self, &g, &unit, &group, &part, contact, NULL) == INT32_MAX) return 1;
    if (unit != -2) return unit == target_unit;
    return target_thing >= 0 && shot_thing(s, part) == target_thing;
}


/* engine 0x1000be60: before the move's segment, the unit's new centre against the type 6 entries (spheres - vehicles,
 * dropships, towers): closer than the two radii (the unit's +0xe8 and the sphere's +0x40) it is set on the sphere's
 * surface along the line from its centre (no direction: +z), the first such in list order. (Class 0x50 spheres are
 * knocked away instead, 0x10015230 / 0x10015920, and do not stop it - no world object has class 0x50.) */
static int sphere_push(const msim *s, const float start[3], const float c[3], float R, float out[3], float n[3], int *ci)
{
    const terrain_t *t = (const terrain_t *)s->terrain;
    int k;
    if (!t || !t->co) return 0;
    for (k = 0; k < t->nco; k++) {
        const cobj_t *o = &t->co[k];
        float box[6], sph[4], d[3], dl, rr;
        if (o->type != 6 || !cobj_now(s, o, box, sph)) continue;
        if (t->thing_of && o->part < t->nparts && t->thing_of[o->part] >= 0 && s->bld[t->thing_of[o->part]].hp <= 0) continue;
        d[0] = c[0] - sph[0]; d[1] = c[1] - sph[1]; d[2] = c[2] - sph[2];
        dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); rr = R + sph[3];
        if (!(dl < rr)) continue;
        const mech3d_part *part = &s->world->parts[o->part];
        if ((part->objtype & 0xf0) == 0x50) return 0;
        if (mech3d_is_force_shield(part)) {
            /* Golden Spade starts three defenders inside the shield. A move originating inside
             * must not eject them from their mission-defined positions as soon as physics runs. */
            float dx = start[0] - sph[0], dy = start[1] - sph[1], dz = start[2] - sph[2];
            if (dx * dx + dy * dy + dz * dz < sph[3] * sph[3]) continue;
        }
        if (dl < 1e-7f) { n[0] = 0; n[1] = 0; n[2] = 1.0f; }
        else { n[0] = d[0] / dl; n[1] = d[1] / dl; n[2] = d[2] / dl; }
        out[0] = sph[0] + n[0] * rr; out[1] = sph[1] + n[1] * rr; out[2] = sph[2] + n[2] * rr;
        *ci = k;
        return 1;
    }
    return 0;
}

/* the servo 0x1003b7e0: value += elapsed ticks x (target - value) / T, never past the target (snaps within 1e-7 of it) */
static float drive_servo(float cur, float tgt, float T, float ticks)
{
    float d = tgt - cur, inc = ticks * d / T;
    if (fabsf(d) < 1e-7f || fabsf(inc) >= fabsf(d)) return tgt;
    return cur + inc;
}
float msim_servo(float cur, float tgt, float T, float ticks) { return drive_servo(cur, tgt, T, ticks); }
/* the DOS leg turn demand c[0xe] walking (MW2.EXE 0x27cd4..0x27d6d): input on the 1024 scale, spd = c[0xb] in cm/s; kv =
 * c[0xb] (16.16 cm/tick) / 10000 by idiv (toward zero; ~km/h): 0 when kv is 0 for a computer-run unit, or for the player
 * with the throttle gain c[0x2d] at 0 (immobile); else (input / 1024, toward zero in 16.16) x 90 x cos(min(80, kv) deg).
 * Airborne (object +0x80 bit 4): the autopilot on (+0xbc) keeps the last demand, else 0 */
float msim_leg_demand(float input, float spd, int player, float gain, int airborne, int autopilot, float prev)
{
    int kv = (int)(spd * (65536.0f / 182.0f)) / 10000;
    if (airborne) return autopilot ? prev : 0.0f;
    if (kv == 0 && (!player || gain <= 0.0f)) return 0.0f;
    if (kv > 80) kv = 80;
    /* only the top is clamped (0x27d07): reversing faster than 80 takes cos(kv) of the negative kv as it is */
    return input / 1024.0f * 90.0f * cosf((float)kv * 3.14159265f / 180.0f);
}
/* a whole-turn twist (a turret, DOS 0x2edd1..0x2edfc then the servo): the value taken round to within 180 degrees of the
 * demand, then stepped toward it; returned within +-180 */
float msim_twist_round(float cur, float demand, int tau, int ticks)
{
    cur = demand - wrap180(demand - cur);
    return wrap180(msim_dos_aim_servo(cur, demand, tau, ticks));
}
/* The engine's throttle-to-speed chain (traced session 12; was a DOS-fitted ramp). Every mech, player and AI alike:
 *  - 0x1001a180: throttle target c[0x12] = 1/64 + input +8 x c[0x2c] (stop 1/64, full 2/64; 1/64 + 1/512 turning on the
 *    spot); state != 2 (starting / shut down): c[0x12] = 1/64 and the drive c[10] = 0;
 *  - servo +0x44 (init 0x100190d0: 1/64, T = 0.2 s x 181 = 36.2 ticks, the same for every mech; stepped once a frame in
 *    0x100190d0 by 0x1003b7e0): c[0x13] follows c[0x12];
 *  - drive c[10] = (c[0x13] - 1/64) x c[0x22] (walk MP x 105.49 cm/tick at full), 0 below 1/2048 over the stop level
 *    (1/32 of full), x -0.5 in the reverse sequence (+0x84 == 2, input +0x2f), x 1.5 with MASC (top passed in);
 *  - servo +0x24 (T 36.2 ticks at init, 0x100190d0 / reset 0x10018a20): c[0xb] follows c[10]; 0x1001a180 sets its T to
 *    90.5 ticks (DOS 0x5b) while the mech's animation key carries flag 0x10 (object +0x80 bit 2, set on entering such a
 *    key, 0x1000dec0 area; Timber Wolf walk keys 6 / 9, run 16 / 19) and nothing sets it back: the first acceleration of
 *    a mission is quick, later ones slower;
 *  - 0x10019310 grounded: the velocity c[0x3d] / c[0x3f] approaches heading x c[0xb] by (target - v) / max(45, frame
 *    ticks) per tick, i.e. v += min(ticks, 45) / 45 x (target - v) per frame (DOS 0x26bxx the same, / 0x2d).
 * DOSBox (YELLSCN1, Timber Wolf from rest, FULL preset; 14-tick frames, ZMBV capture): 10 / 25 / 46 / 64 / 73 / 81 / 85 / 88
 * kph at 0.26 / 0.39 / 0.56 / 0.71 / 0.84 / 1.0 / 1.19 / 1.36 s after the throttle bar moved, top 90 - the port with this
 * rule at 30 fps: 14 / 27 / 50 / 63 / 72 / 80 / 86 / 88 (T 90.5 throughout would reach only ~64 at 1 s). The velocity is
 * kept along the heading
 * (the engine's vector lags the heading by the same 45-tick rule in a turn - not modelled). */
int msim_dos_int = 0;
/* DOS 0x49b80. The division toward zero leaves the last few 16.16 units in the servo when it settles. */
static int32_t dos_drive_servo(int32_t value, int32_t target, int elapsed, int tau)
{
    int32_t diff = (int32_t)((uint32_t)target - (uint32_t)value);
    int32_t step = (int32_t)((uint32_t)diff * (uint32_t)elapsed) / tau;
    if ((diff < 0 && step <= diff) || (diff >= 0 && step >= diff)) return target;
    return (int32_t)((uint32_t)value + (uint32_t)step);
}

float msim_dos_aim_servo(float cur, float demand, int tau, int ticks)
{
    int32_t value = (int32_t)lrintf(cur * 65536.0f), target = (int32_t)lrintf(demand * 65536.0f);
    return (float)dos_drive_servo(value, target, ticks, tau) / 65536.0f;
}

/* DOS 0x2675c / 0x49b80 update the turn servo. 0x27561 then integrates
 * the new value with a signed, whole-tick division toward zero. */
float msim_dos_leg_step(float *servo, float demand, float *heading, int ticks)
{
    int32_t rate = (int32_t)lrintf(*servo * 65536.0f);
    rate = dos_drive_servo(rate, (int32_t)lrintf(demand * 65536.0f), ticks, 54);
    int32_t step = (int32_t)((uint32_t)rate * (uint32_t)ticks) / 182;
    int32_t angle = ((int32_t)lrintf(*heading * 65536.0f) + step) % (360 * 65536);
    if (angle > 180 * 65536) angle -= 360 * 65536;
    if (angle < -180 * 65536) angle += 360 * 65536;
    *servo = rate / 65536.0f;
    *heading = angle / 65536.0f;
    return step / 65536.0f;
}

void msim_drive_step(float *thr, float *spd, float *vel, float thr_target, float rev, float top, int drive, int slow, float ticks)
{
    if (!drive) thr_target = 0;
    float demand;
    if (msim_dos_int) {
        /* The controller reads the previous throttle (0x27ecf) before movement
         * advances the speed and throttle servos (0x26a51 and 0x26a5c). */
        const double scale = 65536.0 / 182.0;
        const int32_t previous = (int32_t)lroundf(*thr * 1024.0f);
        const int32_t throttle = (int32_t)lroundf(thr_target * 1024.0f);
        const int32_t factor = (int32_t)lround(top * scale / 1024.0);
        const int elapsed = (int)lrintf(ticks);
        int32_t target = drive && previous >= 32 ? previous * factor : 0;
        if (rev < 0) target = -(target >> 1);
        const int32_t value = dos_drive_servo((int32_t)llrint(*spd * scale), target, elapsed, slow ? 91 : 36);
        *spd = (float)(value / scale);
        *thr = dos_drive_servo(previous, throttle, elapsed, 36) / 1024.0f;
        /* 0x26ae0. The signed servo still drives the HUD, but values strictly inside this band cannot move a mech. */
        demand = value > -32 && value < 32 ? 0.0f : *spd;
    } else {
        *thr = drive_servo(*thr, thr_target, 36.2f, ticks);
        float c10 = (!drive || fabsf(*thr) < 1.0f / 32.0f) ? 0.0f : *thr * top * (*thr < 0 ? 0.5f : 1.0f) * rev;
        *spd = drive_servo(*spd, c10, slow ? 90.5f : 36.2f, ticks);
        demand = *spd;
    }
    *vel += (demand - *vel) * (ticks < 45.0f ? ticks / 45.0f : 1.0f);
}
/* DOS MW2.EXE keeps c[0x22] as an integer (cm/tick per 1/65536 of throttle over the stop level 0x400): walk MP x
 * 0x697e98 (105.49 in 16.16) rounded (0x4cd46), then (c22 << 16) / the gravity ratio 0x957b4, truncated (0x26973); the
 * ratio is (0x957b0 << 16) / 0x794 (0x14f67), 0x957b0 = PLNT g (16.16) x 0x794 (0.0296) rounded (PLNT handler). Full
 * throttle (0x400 over the stop level) = c22 / 64 cm/tick. Timber Wolf (5 MP) on YELL (0.9 g): 527 -> 585 = 9.1406
 * cm/tick (the 3Dfx DLL's floats: 9.1575). No clamp either way (low gravity speeds mechs up): DOSBox ORANSCN1 (0.6 g)
 * FULL at heading 180 - Timber Wolf (5 MP) 133 kph, Nova NVA00STD (6 MP) 160; Nova THROTTLE_8 124, THROTTLE_7 106 */
float msim_top_speed(int walk_mp, float gravity_g)
{
    int64_t grav = dos_gravity(gravity_g);
    int64_t ratio = grav > 0 ? (grav << 16) / 0x794 : 65536, c22 = ((int64_t)walk_mp * 0x697e98 + 0x8000) >> 16;
    if (ratio <= 0) ratio = 65536;
    c22 = (c22 << 16) / ratio;
    return (float)c22 / 64.0f * 182.0f;
}
/* DOS 0x4684c: THROTTLE_STOP .. _FULL (actions 0x1a + n) set the input +8 to n x 113 (16.16), so FULL is 1017 / 1024 of
 * the 0x400 full scale - 0.68 % short; the 3Dfx DLL (0x10038133) uses n / 576 exactly. DOSBox YELLSCN1 confirms the DOS
 * value: the Timber Wolf's FULL top reads 90 / 84 / 82 / 91 kph at headings 158 / 142 / 128 / 193 degrees, which only
 * 1017 x 585 / 65536 = 9.078 cm/tick (59.5 km/h) gives through the readout's estimate (60 km/h gives 91 / 84 / 84 / 91) */
float msim_throttle_preset(int n) { return (float)(n * 113) / 1024.0f; }
/* The directional jets' drag (DOS 0x26ba2..0x26cxx; 3Dfx 0x100194fa..0x10019618 the same in floats): L = c[0x22] x 0x1400 /
 * 2 (2.5 x the top walking speed, 16.16), L2 = L^2 >> 16 rounded, k = ((T - ((T << 16) / L) / 3) << 16) / (L2 - (3 L >> 2)),
 * B = ((T - (k L2 >> 16 rounded)) << 16) / L, all truncating; drag = (k S >> 16 + B) S >> 16 - so the terminal speed is
 * ~L as in the floats, but k is a few units (Nova on ORAN: k = 3 for the floats' 3.85, B = +33 for -1.6), which slows
 * the middle of the climb: DOSBox ORANSCN1 Nova, backward jets from rest, 144 / 207 / 267 kph at 0.92 / 1.42 / 1.92 s -
 * the integers give 149 / 215 / 267, the floats 154 / 223 / 277 */
static void dos_jet_drag_q16(int32_t thrust, int32_t top, int32_t *k, int32_t *b)
{
    int64_t limit = (int64_t)top * 0x1400 / 2;
    *k = *b = 0;
    if (thrust < 1 || limit < 1) return;
    int32_t square = dos_projectile_mul((int32_t)limit, (int32_t)limit);
    int64_t den = square - (limit * 3) / 4;
    if (den == 0) return;
    int64_t t = thrust - ((int64_t)thrust * 65536 / limit) / 3;
    *k = (int32_t)(t * 65536 / den);
    *b = (int32_t)(((int64_t)thrust - dos_projectile_mul(*k, square)) * 65536 / limit);
}

void msim_jet_drag(float thrust, float top_cm_s, float *k, float *B)
{
    int32_t kq, bq;
    dos_jet_drag_q16((int32_t)lrintf(thrust * 65536.0f), (int32_t)lrintf(top_cm_s * 64.0f / 182.0f), &kq, &bq);
    *k = kq / 65536.0f;
    *B = bq / 65536.0f;
}

/* DOS 0x26f98: add acceleration, then shift the signed displacement down
 * to centimetres. The horizontal dead zone is applied after the move. */
static int32_t dos_motion_axis(int32_t *velocity, int32_t accel, int ticks)
{
    int64_t dv = (int64_t)accel * ticks;
    int32_t move = (int32_t)(((int64_t)*velocity + (dv >> 1)) * ticks >> 16);
    *velocity += (int32_t)dv;
    return move;
}

/* 0x26d01 .. 0x26f8a: a vertical jet damps upward motion only. A turn
 * or a directional jet spends fuel without providing lift. */
static int32_t dos_motion_lift(int32_t *velocity, int32_t gravity, int32_t thrust,
                               int32_t damping, int grounded, int mode, int ticks)
{
    int32_t accel = -gravity;
    if (mode == 1) {
        accel += thrust;
        if (*velocity > 0 && damping > 0)
            accel += dos_projectile_mul((gravity - thrust) * 3640, *velocity) / damping;
    } else if (*velocity == 0 ? (grounded || mode == 2) : (*velocity < 0 && mode == 2)) accel = 0;
    return dos_motion_axis(velocity, accel, ticks);
}

combat_motion msim_dos_lift(const combat_unit *u, const combat_planet *planet, int jet, int ticks, int first_move)
{
    int32_t speed = (int32_t)lrintf(u->vy * 65536.0f);
    int mode = jet ? (u->jet_forward ? 2 : 1) : 0;
    /* A freshly placed player has yet to register a terrain contact. */
    int grounded = !first_move && u->y <= u->ground;
    int32_t move = dos_motion_lift(&speed, dos_gravity(planet->gravity_g),
                                  (int32_t)lrintf(u->jet_ddy * 65536.0f),
                                  (int32_t)lrintf(planet->jet_damping), grounded, mode, ticks);
    return (combat_motion){(float)move, speed / 65536.0f, ticks};
}

static void dos_motion_plane(int32_t velocity[2], int32_t drive, int32_t sa, int32_t ca, int32_t thrust,
                             int32_t top, int freev, int jet_dir, int ticks, int32_t move[2])
{
    int32_t accel[2] = {0};
    if (!freev) {
        if (drive > -32 && drive < 32) drive = 0;
        int divide = ticks > 45 ? ticks : 45;
        accel[0] = (dos_projectile_mul(drive, sa) - velocity[0]) / divide;
        accel[1] = (dos_projectile_mul(drive, ca) - velocity[1]) / divide;
    } else if (jet_dir) {
        int32_t k, b, x = abs(velocity[0]), z = abs(velocity[1]);
        int32_t speed = x > z ? x + (z >> 2) : z + (x >> 2);
        dos_jet_drag_q16(thrust, top, &k, &b);
        int32_t tx = dos_projectile_mul(thrust, sa), tz = dos_projectile_mul(thrust, ca);
        accel[0] = jet_dir == 1 ? -tz : jet_dir == 2 ? tz : jet_dir == 3 ? tx : -tx;
        accel[1] = jet_dir == 1 ? tx : jet_dir == 2 ? -tx : jet_dir == 3 ? tz : -tz;
        if (speed > 0) {
            int32_t drag = dos_projectile_mul(dos_projectile_mul(k, speed) + b, speed);
            for (int axis = 0; axis < 2; axis++)
                accel[axis] -= (int32_t)((int64_t)drag * velocity[axis] / speed);
        }
    }
    for (int axis = 0; axis < 2; axis++) {
        move[axis] = dos_motion_axis(&velocity[axis], accel[axis], ticks);
        if (velocity[axis] > -4096 && velocity[axis] < 4096) velocity[axis] = 0;
    }
}

void msim_dos_walk(combat_unit *u, float heading, float drive, float top, int freev, int jet_dir,
                   int ticks, int32_t move[2])
{
    const double scale = 65536.0 / 182.0;
    int32_t yaw = (int32_t)lrintf(heading * 65536.0f);
    dos_motion_plane(u->dos_vel, (int32_t)lrint(drive * scale), dos_sin(yaw) >> 13, dos_cos(yaw) >> 13,
                     (int32_t)lrintf(u->jet_ddy * 65536.0f), (int32_t)lrintf(top * 64.0f / 182.0f),
                     freev, jet_dir, ticks, move);
}

/* DOS 0x269ca and 0x2717c, 3D 0x10019ae1: a wreck stops at the last
 * position if either a wall or a unit refuses the proposed airborne move. */
void msim_wreck_step(msim *s, int actor, int32_t origin[3], float vx, float vz, float dt, int dos_ticks)
{
    combat_unit *u = actor < 0 ? &s->player_unit : &s->units[actor];
    if (dt <= 0.0f || (msim_dos_int && !dos_ticks)) return;
    u->jet_on = 0;
    if (u->wreck_stopped) return;
    if (u->y <= u->ground) {
        u->wreck_stopped = 1;
        return;
    }

    float h = actor < 0 ? s->player_centre_h : s->centre_h ? s->centre_h[actor] : 550.0f;
    float r = actor < 0 ? s->player_radius : s->radius ? s->radius[actor] : 450.0f;
    float x = (float)origin[0] + vx * dt, z = (float)origin[2] + vz * dt;
    combat_motion lift;
    if (msim_dos_int) {
        int32_t delta[2];
        dos_motion_plane(u->dos_vel, 0, 0, 0, 0, 0, 1, 0, dos_ticks, delta);
        x = (float)(origin[0] + delta[0]);
        z = (float)(origin[2] + delta[1]);
        lift = msim_dos_lift(u, &s->planet, 0, dos_ticks, 0);
    } else lift = (combat_motion){.dy = combat_jets_dy(u, 0, dt)};
    float before[3] = {(float)origin[0], u->y + h, (float)origin[2]};
    float after[3] = {x, u->y + h + lift.dy, z}, normal[3], centre[3], velocity[3], radius;
    if (msim_unit_contact(s, actor, before, after, normal, centre, &radius, velocity) != -2 ||
        !msim_can_move(s, before[0], before[2], x, z, u->y, lift.dy, h, r)) {
        u->wreck_stopped = 1;
        u->vy = 0;
        return;
    }
    origin[0] = (int32_t)lrintf(x);
    origin[2] = (int32_t)lrintf(z);
    u->ground = msim_ground(s, x, z, u->y + lift.dy + h);
    combat_jets_motion(u, 0, dt, &s->rng, msim_dos_int ? &lift : NULL);
    u->wreck_stopped = u->y <= u->ground;
}

void msim_dos_project_velocity(const combat_unit *u, float heading, float *along, float *across)
{
    const float scale = 182.0f / 65536.0f;
    float h = heading * 3.14159265f / 180.0f, sn = sinf(h), cs = cosf(h);
    *along = (sn * u->dos_vel[0] + cs * u->dos_vel[1]) * scale;
    if (across) *across = (cs * u->dos_vel[0] - sn * u->dos_vel[1]) * scale;
}

float msim_ground(const msim *s, float x, float z, float y) { return msim_ground_n(s, x, z, y, NULL); }
float msim_draw_y(const msim *s, int i)
{
    if (i < 0 || i >= s->actor_count) return 0.0f;
    return s->units[i].y + (s->draw_lift ? s->draw_lift[i] : 0.0f);
}

int msim_wall_at(const msim *s, float x, float z, float n[3])
{
    const terrain_t *t = s ? (const terrain_t *)s->terrain : NULL;
    int k;
    if (!t) return 0;
    for (k = 0; k < t->nwall; k++) {
        const box_t *b = &t->wall[k];
        if (x < b->minx || x > b->maxx || z < b->minz || z > b->maxz) continue;
        if (n) {   /* the face nearest the point: its outward normal */
            float dl = x - b->minx, dr = b->maxx - x, dn = z - b->minz, df = b->maxz - z, m = fminf(fminf(dl, dr), fminf(dn, df));
            n[0] = m == dl ? -1.0f : m == dr ? 1.0f : 0.0f; n[1] = 0; n[2] = m == dn ? -1.0f : m == df ? 1.0f : 0.0f;
        }
        return 1;
    }
    return 0;
}

/* the projectile segment o..o+dv against the world objects (see combat_step): the nearest hit's fraction and part */
/* one collision-list object against the step o..o+dv (0x100103e0): its bounding sphere sp, world box bx; *tt the hit */
static int shot_one(msim *s, int pi, const float *sp, const float *bx, const float o[3], const float dv[3], float len, float *tt)
{
    const terrain_t *t = (const terrain_t *)s->terrain;
    const mech3d_part *bp = &s->world->parts[pi];
    float cxs, cys, czs, c2, pr, ent, t0 = 0.0f, t1 = 1.0f;
    int j, out = 0;
    if (bp->hidden) return 0;
    if (t && t->thing_of && pi < t->nparts && t->thing_of[pi] >= 0 && s->bld[t->thing_of[pi]].hp <= 0) return 0;
    /* 0x10029db0: inside -> 10 cm, else (projection - radius), past the step's end -> no hit */
    cxs = sp[0] - o[0]; cys = sp[1] - o[1]; czs = sp[2] - o[2];
    c2 = cxs * cxs + cys * cys + czs * czs - sp[3] * sp[3];
    if (sp[3] < 1e-7f) return 0;
    if (c2 < 0) ent = 10.0f;
    else {
        pr = (cxs * dv[0] + cys * dv[1] + czs * dv[2]) / len;
        if (pr < 0 || pr * pr - c2 < 0) return 0;
        ent = pr - sp[3];
        if (ent < 0) ent = 0;
        if (ent >= len) return 0;
    }
    if (bp->coll == 3 || bp->coll == 6 || bp->coll == 8 || bp->coll == 9) { *tt = ent / len < 1.0f ? ent / len : 1.0f; return 1; }
    for (j = 0; j < 3 && !out; j++) {   /* the world box */
        if (fabsf(dv[j]) < 1e-6f) { if (o[j] < bx[j] || o[j] > bx[3 + j]) out = 1; continue; }
        {
            float ta = (bx[j] - o[j]) / dv[j], tc = (bx[3 + j] - o[j]) / dv[j];
            if (ta > tc) { float tq = ta; ta = tc; tc = tq; }
            if (ta > t0) t0 = ta;
            if (tc < t1) t1 = tc;
            if (t0 > t1) out = 1;
        }
    }
    if (out) return 0;
    if (bp->coll == 0 || bp->coll == 1) { *tt = t0; return 1; }
    {
        mech3d one;
        memset(&one, 0, sizeof one);
        one.parts = (mech3d_part *)bp;
        one.part_count = 1;
        return seg_mech(&one, o, dv, tt);
    }
}

static int shot_objects(msim *s, const float o[3], const float dv[3], float *tb, int *part)
{
    terrain_t *t = (terrain_t *)s->terrain;
    float len = sqrtf(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]), lo[2], hi[2];
    int cx0, cx1, cz0, cz1, cx, cz, found = 0;
    if (!s->world || len <= 0) return 0;
    if (s->paths) {   /* carried parts (path tasks): their sphere and box as they are now */
        int q;
        for (q = 0; q < s->world->part_count; q++) {
            const mech3d_part *p = &s->world->parts[q];
            float bx[6], sp[4], r2 = 0, tt;
            int k, j;
            if (!p->moving || p->hidden || p->coll < 0 || p->coll > 9 || p->coll == 4 || p->coll == 5) continue;
            if (part_box(p, bx, bx + 3) != 0) continue;
            for (j = 0; j < 3; j++) sp[j] = (bx[j] + bx[3 + j]) * 0.5f;
            if ((sp[0] - o[0]) * (sp[0] - o[0]) + (sp[2] - o[2]) * (sp[2] - o[2]) > (len + 30000.0f) * (len + 30000.0f)) continue;
            for (k = 0; k < p->model.objects[0].vert_count; k++) {
                float w[3], d2;
                part_vertex(p, &p->model.objects[0].verts[k], w);
                d2 = (w[0] - sp[0]) * (w[0] - sp[0]) + (w[1] - sp[1]) * (w[1] - sp[1]) + (w[2] - sp[2]) * (w[2] - sp[2]);
                if (d2 > r2) r2 = d2;
            }
            sp[3] = sqrtf(r2);
            if (shot_one(s, q, sp, bx, o, dv, len, &tt) && tt < *tb) { *tb = tt; *part = q; found = 1; }
        }
    }
    if (!t || !t->ocell) return found;
    if (++t->stamp_now == 0x7fffffff) { memset(t->stamp, 0, (size_t)(t->nparts > 0 ? t->nparts : 1) * sizeof *t->stamp); t->stamp_now = 1; }
    lo[0] = fminf(o[0], o[0] + dv[0]); hi[0] = fmaxf(o[0], o[0] + dv[0]);
    lo[1] = fminf(o[2], o[2] + dv[2]); hi[1] = fmaxf(o[2], o[2] + dv[2]);
    cx0 = (int)floorf((lo[0] - t->x0) / t->cs); cx1 = (int)floorf((hi[0] - t->x0) / t->cs);
    cz0 = (int)floorf((lo[1] - t->z0) / t->cs); cz1 = (int)floorf((hi[1] - t->z0) / t->cs);
    if (cx1 < 0 || cz1 < 0 || cx0 >= t->gw || cz0 >= t->gh) return found;
    if (cx0 < 0) cx0 = 0;
    if (cz0 < 0) cz0 = 0;
    if (cx1 >= t->gw) cx1 = t->gw - 1;
    if (cz1 >= t->gh) cz1 = t->gh - 1;
    for (cz = cz0; cz <= cz1; cz++)
        for (cx = cx0; cx <= cx1; cx++) {
            const cell_t *c = &t->ocell[cz * t->gw + cx];
            int k;
            for (k = 0; k < c->n; k++) {
                int oi = c->idx[k], pi = t->obj[oi];
                float tt;
                if (t->stamp[pi] == t->stamp_now) continue;
                t->stamp[pi] = t->stamp_now;
                if (shot_one(s, pi, t->obj_sph[oi], t->obj_box[oi], o, dv, len, &tt) && tt < *tb) { *tb = tt; *part = pi; found = 1; }
            }
        }
    return found;
}
int msim_shot_obstruction(msim *s, const float from[3], const float to[3])
{
    const float dv[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    float t = 1.0f;
    int part = -1;
    shot_objects(s, from, dv, &t, &part);
    return part;
}

static int shot_thing(const msim *s, int part)
{
    const terrain_t *t = (const terrain_t *)s->terrain;
    return t && t->thing_of && part >= 0 && part < t->nparts ? t->thing_of[part] : -1;
}

/* the moving parts' world boxes now (terrain_t mov), and their avoidance footprints (msim_set_world adds them last) */
static void mov_refresh(msim *s)
{
    terrain_t *t = (terrain_t *)s->terrain;
    int k;
    if (!s->world) return;
    for (k = 0; t && k < t->nmov; k++) {
        float *b = t->mov_box[k];
        int pi = t->mov[k];
        if (pi >= s->world->part_count || s->world->parts[pi].hidden || part_box(&s->world->parts[pi], b, b + 3) != 0) {
            b[0] = b[1] = b[2] = b[3] = b[4] = b[5] = -1e30f;   /* gone: a point far away, never hit */
        }
    }
    for (k = 0; s->obstacles && s->obstacle_part && k < s->obstacle_count; k++) {
        float mn[3], mx[3];
        int pi = s->obstacle_part[k];
        if (pi < 0 || pi >= s->world->part_count || !s->world->parts[pi].moving) continue;
        if (s->world->parts[pi].hidden || part_box(&s->world->parts[pi], mn, mx) != 0) mn[0] = mn[2] = mx[0] = mx[2] = -1e30f;
        s->obstacles[k][0] = mn[0]; s->obstacles[k][1] = mx[0]; s->obstacles[k][2] = mn[2]; s->obstacles[k][3] = mx[2];
    }
}

int msim_can_step(const msim *s, float x0, float z0, float x, float z, float y)
{
    return msim_can_step_hr(s, x0, z0, x, z, y, 550.0f, 545.0f);
}
/* the normal of the surface that refused the last msim_can_step_hr step (the engine's hit normal DAT_1024b420..428,
 * which 0x1000c1f0 takes for the collision damage: normal y over cos 45 -> the legs, else a side by direction) */
static int refuse(const float n[3]) { g_block_n[0] = n[0]; g_block_n[1] = n[1]; g_block_n[2] = n[2]; return 0; }
/* H = the mech's centre height above its feet (MGEO int[0]), R = its contact radius (int[6]) */
/* unit k's contact sphere (centre q, radius *r2) and velocity ov (cm/tick); 0 when it has none (destroyed) */
static int unit_sphere(const msim *s, int k, float q[3], float *r2, float ov[3])
{
    if (k < 0) {
        if (s->player_unit.destroyed) return 0;
        q[0] = s->player[0]; q[1] = s->player_unit.y + s->player_centre_h; q[2] = s->player[2]; *r2 = s->player_radius;
        memcpy(ov, msim_dos_int ? s->player_unit.dos_attempted : s->player_vel, 3 * sizeof *ov);
    } else {
        int air;
        float sp, hh;
        if (s->units[k].destroyed) return 0;
        q[0] = (float)s->actors[k].mech.origin[0]; q[1] = s->units[k].y + (s->centre_h ? s->centre_h[k] : 550.0f);
        q[2] = (float)s->actors[k].mech.origin[2]; *r2 = s->radius ? s->radius[k] : 450.0f;
        air = s->units[k].y > s->units[k].ground + 1.0f;
        sp = (air && s->air_speed ? s->air_speed[k] : s->minds ? s->minds[k].speed : 0.0f) / 182.0f;
        hh = s->actors[k].mech.heading * 3.14159265f / 180.0f;
        ov[0] = sinf(hh) * sp; ov[1] = air ? s->units[k].vy : 0.0f; ov[2] = cosf(hh) * sp;
        if (msim_dos_int) memcpy(ov, s->units[k].dos_attempted, 3 * sizeof *ov);
    }
    return 1;
}
static int32_t approx_len3_dos(int32_t a, int32_t b, int32_t c)
{
    int32_t x = a < 0 ? -a : a, y = b < 0 ? -b : b, z = c < 0 ? -c : c, t;
    if (x < y) { t = x; x = y; y = t; }
    if (x < z) { t = x; x = z; z = t; }
    return (x * 4 + y + z) >> 2;
}

/* 0x200d0 measures contact in whole centimetres with the octagonal
 * length estimate, both when finding a hit and when projecting out of it. */
static int32_t dos_contact_delta(const float p[3], const float q[3], int32_t d[3])
{
    for (int k = 0; k < 3; k++) d[k] = (int32_t)lrintf(p[k]) - (int32_t)lrintf(q[k]);
    return approx_len3_dos(d[0], d[1], d[2]);
}

static void contact_normal(const float p[3], const float q[3], float d, float n[3])
{
    if (msim_dos_int && d > 0.0f) {
        int32_t delta[3], len = dos_contact_delta(p, q, delta);
        for (int k = 0; k < 3; k++) n[k] = (int32_t)((int64_t)delta[k] * 65536 / len) / 65536.0f;
    } else if (d > 1e-3f) { n[0] = (p[0] - q[0]) / d; n[1] = (p[1] - q[1]) / d; n[2] = (p[2] - q[2]) / d; }
    else { n[0] = 0; n[1] = 0; n[2] = 1.0f; }
}

/* 0x1fd0c checks unit contact only for an online mover or a falling wreck.
 * A powered-down or starting unit remains eligible as the other participant. */
static int dos_unit_contact_enabled(const combat_unit *mover)
{
    return !mover->intact_out && (mover->destroyed || (!mover->offline && !mover->shutdown));
}

int msim_unit_contact(const msim *s, int self, const float p0[3], const float p1[3], float n[3], float c[3], float *rr, float ov[3])
{
    if (msim_dos_int && !dos_unit_contact_enabled(self < 0 ? &s->player_unit : &s->units[self])) return -2;
    float r1 = self >= 0 ? (s->radius ? s->radius[self] : 450.0f) : s->player_radius, best = 1e30f;
    int k, hit = -2;
    for (k = -1; k < s->actor_count; k++) {
        float q[3], r2, d1, d0, rs, v[3];
        if (k == self || !unit_sphere(s, k, q, &r2, v)) continue;
        rs = r1 + r2;
        d1 = sqrtf((p1[0] - q[0]) * (p1[0] - q[0]) + (p1[1] - q[1]) * (p1[1] - q[1]) + (p1[2] - q[2]) * (p1[2] - q[2]));
        d0 = sqrtf((p0[0] - q[0]) * (p0[0] - q[0]) + (p0[1] - q[1]) * (p0[1] - q[1]) + (p0[2] - q[2]) * (p0[2] - q[2]));
        if (msim_dos_int) {
            int32_t delta[3];
            d1 = (float)dos_contact_delta(p1, q, delta);
        }
        /* 0x20141: a unit already touched by an earlier mover responds
         * on its turn even if the attempted endpoint is outside that sphere. */
        int reciprocal = msim_dos_int && k < self &&
                         (k < 0 ? s->player_unit.dos_contact_unit : s->units[k].dos_contact_unit) == self + 2;
        if (reciprocal || (d1 < rs && (msim_dos_int || (d1 < d0 && d1 < best)))) {
            best = d1; hit = k; *rr = rs;
            c[0] = q[0]; c[1] = q[1]; c[2] = q[2];
            contact_normal(p1, q, d1, n);
            ov[0] = v[0]; ov[1] = v[1]; ov[2] = v[2];
            if (msim_dos_int) break;
        }
    }
    return hit;
}
int msim_unit_inside(const msim *s, int self, int other, const float p1[3], float n[3], float c[3], float *rr, float ov[3])
{
    if (msim_dos_int && !dos_unit_contact_enabled(self < 0 ? &s->player_unit : &s->units[self])) return 0;
    float r1 = self >= 0 ? (s->radius ? s->radius[self] : 450.0f) : s->player_radius, q[3], r2, d;
    if (other == self || other < -1 || other >= s->actor_count || !unit_sphere(s, other, q, &r2, ov)) return 0;
    d = sqrtf((p1[0] - q[0]) * (p1[0] - q[0]) + (p1[1] - q[1]) * (p1[1] - q[1]) + (p1[2] - q[2]) * (p1[2] - q[2]));
    if (msim_dos_int) {
        int32_t delta[3];
        d = (float)dos_contact_delta(p1, q, delta);
    }
    if (d >= r1 + r2) return 0;   /* 0x1000ba20: dist >= r1 + r2 -> no contact */
    *rr = r1 + r2; c[0] = q[0]; c[1] = q[1]; c[2] = q[2];
    contact_normal(p1, q, d, n);
    return 1;
}

/* 0x10030330 (3D) / DOS 0x200d0: the length estimate (4 max + mid + min) / 4 - x87 extended in the DLL, >> 2 in DOS */
static long double approx_len3(float a, float b, float c)
{
    float x = fabsf(a), y = fabsf(b), z = fabsf(c), t;
    if (x < z) { t = x; x = z; z = t; }
    if (x < y) { t = x; x = y; y = t; }
    return ((long double)x * 4.0L + (long double)z + (long double)y) * 0.25L;
}


/* 0x2043a: round the projected position to centimetres and use the
 * other mech's z speed in the bounce magnitude (0x20499). */
static void dos_unit_bounce(const float centre[3], const float normal[3], float radius,
                            const float vel[3], const float other[3], float end[3], float bounced[3])
{
    int32_t nx[3];
    for (int k = 0; k < 3; k++) nx[k] = (int32_t)lrintf(normal[k] * 65536.0f);
    int32_t dx = (int32_t)lrintf(vel[0] * 65536.0f) - (int32_t)lrintf(other[0] * 65536.0f);
    int32_t dy = (int32_t)lrintf(vel[1] * 65536.0f) - (int32_t)lrintf(other[1] * 65536.0f);
    int32_t speed = approx_len3_dos(dx, dy, (int32_t)lrintf(other[2] * 65536.0f)) >> 1;
    if (speed < 65536) speed = 65536;
    for (int k = 0; k < 3; k++) {
        end[k] = (float)((int32_t)lrintf(centre[k]) + dos_projectile_mul(nx[k], (int32_t)radius));
        bounced[k] = dos_projectile_mul(nx[k], speed) / 65536.0f;
    }
}

/* DOS 0x27109 tests the projected end against the same collision list.
 * A second unit hit keeps the projected end. A world hit restores the start. */
int msim_dos_unit_impact(msim *s, int self, const float start[3], float height, float radius, int hit,
                         const float centre[3], const float normal[3], float combined_radius,
                         const float vel[3], const float other[3], float end[3], float bounce[3])
{
    float n[3], q[3], v[3], rr;
    combat_unit *mover = self < 0 ? &s->player_unit : &s->units[self];
    dos_unit_bounce(centre, normal, combined_radius, vel, other, end, bounce);
    mover->dos_contact_unit = hit + 2;
    mover->dos_attempted[2] = other[2];
    int retry = msim_unit_contact(s, self, start, end, n, q, &rr, v);
    if (retry == -2) {
        if (msim_can_move(s, start[0], start[2], end[0], end[2], start[1] - height, end[1] - start[1], height, radius))
            return 0;
        memcpy(end, start, 3 * sizeof *end);
    } else {
        mover->dos_contact_unit = retry + 2;
        mover->dos_attempted[2] = v[2];
    }
    for (int k = 0; k < 3; k++) bounce[k] *= 2.0f;
    return 1;
}

int msim_contact_retest(const float p0[3], const float p1[3], const float q[3], float rr)
{
    int k;
    if (msim_dos_int) {
        /* DOS 0x200d0 on whole cm: n = d << 16 / est (idiv, toward 0); the end = q + (n x R) >> 16 rounded at bit 15; the
         * second try (DOS 0x26990) tests the same end: est(end - q) < R refuses it */
        int32_t d[3], e[3], R = (int32_t)lrintf(rr), L;
        for (k = 0; k < 3; k++) d[k] = (int32_t)lrintf(p1[k]) - (int32_t)lrintf(q[k]);
        L = approx_len3_dos(d[0], d[1], d[2]);
        for (k = 0; k < 3; k++) {
            int64_t n16 = L == 0 ? (k == 2 ? 0x10000 : 0) : ((int64_t)d[k] * 65536) / L, pr = n16 * R;
            e[k] = (int32_t)(pr >> 16) + (int32_t)((pr >> 15) & 1);
        }
        return approx_len3_dos(e[0], e[1], e[2]) < R;
    } else {
        /* 0x1000ba20 in floats: n = d / est(d) (float), the end = n x R + q (float); 0x1000b5e0 rebuilds the second try's
         * end as old + (end - old) (0x1000b5e0: local_78 = delta + old), and 0x1000ba20 refuses it when est(end - q) < R */
        float d[3], n[3], e[3], L;
        for (k = 0; k < 3; k++) d[k] = p1[k] - q[k];
        L = (float)approx_len3(d[0], d[1], d[2]);
        for (k = 0; k < 3; k++) {
            float t, end, dl;
            n[k] = L <= 1e-7f ? (k == 2 ? 1.0f : 0.0f) : (float)((long double)d[k] / (long double)L);
            t = n[k] * rr; end = t + q[k]; dl = end - p0[k]; end = dl + p0[k];
            e[k] = end - q[k];
        }
        return approx_len3(e[0], e[1], e[2]) < (long double)rr;
    }
}

void msim_block_normal(float n[3]) { n[0] = g_block_n[0]; n[1] = g_block_n[1]; n[2] = g_block_n[2]; }
const char *msim_block_what(void) { return g_block_what; }
int msim_world_impact(const msim *s, float x0, float z0, float *x, float *z, float y, float H, float R, const float n_in[3],
                      float heading_deg, float *speed, float *across)
{
    float dy = 0.0f, vy = 0.0f;
    (void)n_in;
    return msim_world_impact3(s, x0, z0, x, z, y, &dy, H, R, heading_deg, speed, across, &vy, NULL);
}

/* DOS 0x1fee6 .. 0x2007d. A face hit uses the length estimate both for
 * the incoming segment and for the attempted end velocity. */
static void dos_face_response(const int32_t dir[3], const int32_t normal[3],
                              int32_t velocity[3], int32_t untravelled, int32_t rebound[3])
{
    int32_t twice_dot = 0;
    for (int i = 0; i < 3; i++) twice_dot += dos_projectile_mul(normal[i] * 2, dir[i]);
    int32_t quarter = -approx_len3_dos(velocity[0], velocity[1], velocity[2]) >> 2;
    for (int i = 0; i < 3; i++) {
        int32_t axis = dir[i] - dos_projectile_mul(normal[i], twice_dot);
        velocity[i] = dos_projectile_mul(axis, quarter);
        rebound[i] = untravelled > 0 ? dos_projectile_mul(axis, untravelled) >> 2 : 0;
    }
}

int msim_world_impact3(const msim *s, float x0, float z0, float *x, float *z, float y, float *dy, float H, float R,
                       float heading_deg, float *speed, float *across, float *vy, int32_t dos_vel[2])
{
    /* engine 0x1000b5e0, the world hit (0x10010530): the move's segment (centre to centre, extended R ahead) ends at the
     * hit; the touching point is that hit less R along the move - behind the start when the hit is nearer than R
     * (0x1000b060(hit - extension)); d = the move's unit direction, r = d - 2 (d . n) n its reflection by the hit normal
     * (DAT_1024b420..428, 0x10247140 2.0); the position = the touching point + r x the untravelled distance x 0.25
     * (0x10247148) when any is left, the velocity +0xf4..0xfc = r x |attempted velocity +0x100..0x108| x -0.25
     * (0x10247144) - a vector (its part across the heading returned in *across). A type 6 sphere (0x1000be60) instead
     * sets the unit on its surface with the velocity n x |v| x 0.25 x 0.5 (x -0.5 above 8 cm/tick, 0x10247150..158).
     * 0x10019310 then tries the move from where the unit stood to that position again (second 0x1000b5e0 call,
     * 0x10019a61; its own result discarded): refused, the velocity is doubled (0x102473d4) and the unit keeps the new
     * position when the object hit is the same as the first time (0x10019a6a; a terrain piece is an object), else it
     * stays where it stood (0x10019a76). Returns bit 0: it stayed; bit 1: harmless - the normal left (the second try's
     * when refused) has y above cos 45 (0x3f350481) and the feet are within 10 m above / 100 m below the ground there
     * (0x10019c30): no damage, no sound, the contact count cleared (0x10019c4e) */
    /* In 3D (the move's dy: airborne, 0x10019310 passes the vertical part with the horizontal): the segment, the touching
     * point (hit - the extension, msim_can_move), the reflection and the untravelled part are the 3D ones, the attempted
     * velocity |(vh, vy)|; *dy returns the vertical move made, *vy the new vertical velocity (cm/tick: the reflected one's
     * y; the engine's +0xf8). */
    float dx = *x - x0, dz = *z - z0, len = sqrtf(dx * dx + *dy * *dy + dz * dz), n[3], bx, by, bz, v, hh, vel[3], g, ext;
    int kind, id1, ret = 0, k;
    if (msim_can_move(s, x0, z0, *x, *z, y, *dy, H, R)) return 0;
    kind = g_hit_kind; id1 = g_block_id; ext = g_hit_ext;
    memcpy(n, g_block_n, sizeof n);
    g_impact_part = id1;
    memcpy(g_impact_at, g_hit_at, sizeof g_impact_at);
    hh = heading_deg * 3.14159265f / 180.0f;
    v = (across ? sqrtf(*speed * *speed + *across * *across) : fabsf(*speed)) / 182.0f;   /* |the attempted velocity|, cm/tick */
    v = sqrtf(v * v + *vy * *vy);
    int32_t raw[3] = {0};
    if (msim_dos_int) {
        float sn = sinf(hh), cs = cosf(hh), side = across ? *across : 0.0f;
        raw[0] = dos_vel ? dos_vel[0] : (int32_t)lrintf((sn * *speed + cs * side) * (65536.0f / 182.0f));
        raw[1] = (int32_t)lrintf(*vy * 65536.0f);
        raw[2] = dos_vel ? dos_vel[1] : (int32_t)lrintf((cs * *speed - sn * side) * (65536.0f / 182.0f));
        v = approx_len3_dos(raw[0], raw[1], raw[2]) / 65536.0f;
    }
    if (kind == 2) {
        float m = v * 0.25f, kk = m > 8.0f ? -0.5f : 0.5f;
        bx = g_push[0]; by = *dy == 0.0f ? y : g_push[1] - H; bz = g_push[2];
        for (k = 0; k < 3; k++) vel[k] = n[k] * m * kk;
    } else if (msim_dos_int) {
        int32_t dir[3] = {(int32_t)lrintf(dx), (int32_t)lrintf(*dy), (int32_t)lrintf(dz)};
        int32_t length = dos_normalize(dir);
        int32_t normal[3], rebound[3];
        const float start[3] = {x0, y, z0};
        float pos[3];
        int32_t touch = (int32_t)lrintf(g_hit_t) - (int32_t)ext;
        for (int i = 0; i < 3; i++) normal[i] = (int32_t)lrintf(n[i] * 65536.0f);
        dos_face_response(dir, normal, raw, length - touch, rebound);
        for (int i = 0; i < 3; i++) {
            pos[i] = (float)((int32_t)lrintf(start[i]) + dos_projectile_mul(dir[i], touch) + rebound[i]);
            vel[i] = raw[i] / 65536.0f;
        }
        bx = pos[0]; by = pos[1]; bz = pos[2];
    } else {
        float ux = dx / len, uy = *dy / len, uz = dz / len, dn = ux * n[0] + uy * n[1] + uz * n[2], r[3], tch = g_hit_t - ext, u = len - tch;
        r[0] = ux - 2.0f * dn * n[0]; r[1] = uy - 2.0f * dn * n[1]; r[2] = uz - 2.0f * dn * n[2];
        bx = x0 + ux * tch; by = y + uy * tch; bz = z0 + uz * tch;
        if (u > 0.0f) {
            bx += r[0] * u * 0.25f; bz += r[2] * u * 0.25f;
            if (*dy != 0.0f) by += r[1] * u * 0.25f;   /* walking stays level (the port's walking move has no y part) */
        }
        for (k = 0; k < 3; k++) vel[k] = r[k] * v * -0.25f;
    }
    if (!msim_can_move(s, x0, z0, bx, bz, y, by - y, H, R)) {   /* the second try (0x10019a61: new - old, in 3D) */
        for (k = 0; k < 3; k++) vel[k] *= 2.0f;
        if (g_block_id != id1) { bx = x0; by = y; bz = z0; ret |= 1; }
    } else memcpy(g_block_n, n, sizeof n);
    if (msim_dos_int && dos_vel) {
        dos_vel[0] = (int32_t)lrintf(vel[0] * 65536.0f);
        dos_vel[1] = (int32_t)lrintf(vel[2] * 65536.0f);
    }
    *x = bx; *z = bz; *dy = by - y; *vy = vel[1];
    *speed = (vel[0] * sinf(hh) + vel[2] * cosf(hh)) * 182.0f;
    if (across) *across = (vel[0] * cosf(hh) - vel[2] * sinf(hh)) * 182.0f;
    g = by - msim_ground(s, bx, bz, by + H);   /* the feet over the ground there (0x10010080 at the centre) */
    if (g_block_n[1] > 0.70710677f && g < 1000.0f && g > -10000.0f) ret |= 2;
    return ret;
}

/* collision damage to the object walked into (engine 0x1000c3c0, the impact of a unit that touched no unit): its thing
 * takes (|v| - 3.0694) x 0.050123 x 3 (0x1024715c..164) at the object's centre (0x10046c20) whatever the Collision
 * Damage setting (the unit's own share is that setting's, 0x1000c1f0); the class 0xb0 branch before it (0x1000c3cb:
 * 50 to each leg, an explosion at the object, destroyed) runs only with DAT_1024ffcc set - the network game (SimMain's
 * session argument, 0x10008a8d; switch /N) */
void msim_impact_damage(msim *s, int owner, float speed)
{
    const terrain_t *t = (const terrain_t *)s->terrain;
    int pi = g_impact_part;
    if (!(speed > 3.0693676f) || !t || !t->thing_of || pi < 0 || pi >= t->nparts || t->thing_of[pi] < 0) return;
    building_damage(s, t->thing_of[pi], (speed - 3.0693676f) * 0.050123077f * 3.0f, owner, g_impact_at);
}
/* the object of the last impact is destructible (its thing; the impact sound 0xc8 MECBLDC1, else 0xe5 MECMTNHD:
 * 0x10019cf8 tests the object's +3 bit 1) */
int msim_impact_destructible(const msim *s)
{
    const terrain_t *t = (const terrain_t *)s->terrain;
    int pi = g_impact_part;
    return t && t->thing_of && pi >= 0 && pi < t->nparts && t->thing_of[pi] >= 0;
}

int msim_can_step_hr(const msim *s, float x0, float z0, float x, float z, float y, float H, float R)
{
    return msim_can_move(s, x0, z0, x, z, y, 0.0f, H, R);
}

int msim_can_move(const msim *s, float x0, float z0, float x, float z, float y, float dy, float H, float R)
{
    /* engine 0x1000b5e0 (DOS 0x1fca0) for a unit's move (x0, y + H, z0) -> (x, y + dy + H, z), its centre: the new
     * centre against the type 6 spheres (0x1000be60), then the segment from the old centre to the new one extended along
     * the move by (1 - |u.y|) x R + |u.y| x H (0x1000b7b9, u its unit direction; walking: R) against the collision list
     * (0x10010530): types 0 and 1 by their world box (a box below or above the centre's height is not touched; one the
     * centre starts inside stops it at once), 2 / 7 / 5 by the faces it crosses from the front - the terrain too: any
     * face crossed refuses the move, a climbable one (normal y above cos 45) only spares the damage (msim_world_impact).
     * dy: the frame's vertical move (airborne: falling, jumping - 0x10019310 passes the whole (v + a t / 2) t; walking
     * 0). A move of nothing tests nothing. */
    float p0[3] = {x0, y + H, z0}, p1[3] = {x, y + dy + H, z}, dx = x - x0, dz = z - z0, dl = sqrtf(dx * dx + dy * dy + dz * dz), t, n[3], ext;
    const terrain_t *tt = (const terrain_t *)s->terrain;
    int ci;
    g_block_what = NULL;
    g_block_id = -1;
    g_hit_kind = 0;
    if (!tt || !tt->co) return 1;
    if (sphere_push(s, p0, p1, R, g_push, n, &ci)) {
        g_hit_kind = 2; g_block_id = tt->co[ci].part; g_block_what = s->world->parts[g_block_id].model_name;
        { float box[6]; cobj_now(s, &tt->co[ci], box, g_hit_at); }
        return refuse(n);
    }
    if (!(dl > 0.0f)) return 1;
    if (msim_dos_int) {
        int32_t dir[3] = {(int32_t)lrintf(dx), (int32_t)lrintf(dy), (int32_t)lrintf(dz)};
        int32_t length = dos_normalize(dir);
        if (!length) return 1;
        int32_t tilt = abs(dir[1]);
        int32_t reach = dos_projectile_mul((int32_t)lrintf(H), tilt) +
                        dos_projectile_mul((int32_t)lrintf(R), 65536 - tilt);
        ext = (float)reach;
        for (int i = 0; i < 3; i++) p1[i] = p0[i] + (float)dos_projectile_mul(dir[i], length + reach);
    } else {
        ext = (1.0f - fabsf(dy / dl)) * R + fabsf(dy / dl) * H;
        p1[0] += dx / dl * ext; p1[1] += dy / dl * ext; p1[2] += dz / dl * ext;
    }
    g_hit_ext = ext;
    if (clist_seg(s, p0, p1, CLIST_MOVE, &t, n, &ci)) {
        if (msim_dos_int) {
            float rx = p1[0] - p0[0], ry = p1[1] - p0[1], rz = p1[2] - p0[2];
            float ray_length = sqrtf(rx * rx + ry * ry + rz * rz);
            float f = ray_length > 0.0f ? t / ray_length : 0.0f;
            t = (float)approx_len3_dos((int32_t)lrintf(rx * f), (int32_t)lrintf(ry * f), (int32_t)lrintf(rz * f));
        }
        g_hit_kind = 1; g_hit_t = t; g_block_id = tt->co[ci].part; g_block_what = s->world->parts[g_block_id].model_name;
        { float box[6]; cobj_now(s, &tt->co[ci], box, g_hit_at); }
        return refuse(n);
    }
    return 1;
}

float msim_ground_n(const msim *s, float x, float z, float y, float *nout)
{
    /* engine 0x10010080 over the collision list: the type 5 terrain (its quadtree, qt_ground) and the type 0 boxes (their
     * top inside their XZ extent, 0x1000d1f0); the highest surface within 10 m of y (y < h + 10 m and h - 10 m < y) wins,
     * else (while none is within) the highest below y, else 0. The engine asks with the unit's CENTRE height (object
     * +0x54 = the feet + H: 0x10019b33 / 0x10019dbf) */
    const terrain_t *t = s ? (const terrain_t *)s->terrain : NULL;
    static const float UP[3] = {0, 1, 0};
    float win = -3.4e38f, below = -3.4e38f;
    const float *nwin = UP, *nbelow = UP;
    int cand[4096], nc, k;
    if (nout) memcpy(nout, UP, sizeof UP);
    if (!t || !t->co) return 0.0f;
    nc = clist_near(s, x, z, x, z, cand, 4096);
    for (k = 0; k < nc; k++) {
        const cobj_t *c = &t->co[cand[k]];
        const float *ny = UP;
        float h, box[6], sph[4];
        if (c->type == 5) {
            const float *pn = NULL;
            if (c->moving || !qt_ground(t, c->root, x, z, &h, &pn)) continue;
            if (pn) ny = pn;
        } else if (c->type == 0) {
            if (!cobj_now(s, c, box, sph) || x < box[0] || x > box[1] || z < box[4] || z > box[5]) continue;
            h = box[3];
        } else continue;
        if (win == -3.4e38f && h < y && h > below) { below = h; nbelow = ny; }
        if (y < h + 1000.0f && h - 1000.0f < y && h > win) { win = h; nwin = ny; }
    }
    if (win > -3.4e38f) { if (nout) memcpy(nout, nwin, sizeof UP); return win; }
    if (below > -3.4e38f) { if (nout) memcpy(nout, nbelow, sizeof UP); return below; }
    return 0.0f;
}

/* TEST ONLY (MW2_TEST_FALLSIM): the lowest and highest type 5 surface at (x, z) (holes / seams left out); 0 when none */
static int test_floor(const msim *s, float x, float z, float *lo, float *hi)
{
    const terrain_t *t = s ? (const terrain_t *)s->terrain : NULL;
    int cand[4096], nc, k, n = 0;
    *lo = 3.4e38f; *hi = -3.4e38f;
    if (!t || !t->co) return 0;
    nc = clist_near(s, x, z, x, z, cand, 4096);
    for (k = 0; k < nc; k++) {
        const cobj_t *c = &t->co[cand[k]];
        const float *pn = NULL;
        float h;
        if (c->type != 5 || c->moving || !qt_ground(t, c->root, x, z, &h, &pn) || !pn) continue;
        if (h < *lo) *lo = h;
        if (h > *hi) *hi = h;
        n++;
    }
    return n;
}

static void part_vertex(const mech3d_part *p, const wtb_vertex *v, float out[3]);
static void buildings_init(msim *s, const mech3d *w)
{
    int i, k, q;
    int team_all[64];   /* the scene's STAR entries {u32 team, u32 alliance, name[16]} (0x10042d40: DAT_101e0170[team] = alliance) */
    free(s->bld); s->bld = NULL; s->bld_count = 0;
    if (!s->arch) return;
    bwd_mission mission;
    if (bwd_mission_load(s->arch, s->scene, &mission) != 0) return;
    memset(team_all, 0, sizeof team_all);
    const bwd_record *scene = &mission.records[0];
    for (k = 0; k < scene->chunk_count; k++) {
        const bwd_chunk *star = &scene->chunks[k];
        if (strcmp(star->tag, "STAR") != 0) continue;
        for (q = 0; (size_t)(q + 1) * 24 <= star->size; q++) {
            const uint8_t *e = star->data + q * 24;
            /* +0 the star's affiliation, +4 its alliance (0x10042d40). */
            uint32_t team = (uint32_t)e[0] | (uint32_t)e[1] << 8 | (uint32_t)e[2] << 16 | (uint32_t)e[3] << 24;
            if (team < 64) team_all[team] = (int)((uint32_t)e[4] | (uint32_t)e[5] << 8 | (uint32_t)e[6] << 16 | (uint32_t)e[7] << 24);
        }
    }
    for (i = 0; i < w->part_count; i++) {
        const char *name = w->parts[i].rec;
        const bwd_record *record = NULL;
        int seen = 0, affl = -1;   /* AFFL sets the team of the GT records that follow (0x1003de10). */
        if (!name[0]) continue;
        for (k = 0; k < i && !seen; k++) if (strcmp(w->parts[k].rec, name) == 0) seen = 1;
        if (seen) continue;
        for (k = 0; k < mission.record_count; k++)
            if (strcmp(mission.records[k].name, name) == 0) { record = &mission.records[k]; break; }
        if (!record) continue;
        const bwd_chunk *c = record->chunks;
        for (k = 0; k < record->chunk_count; k++) {
            const uint8_t *d = c[k].data;
            int a, b, hp, ia = -1, ib = -1;
            if (strcmp(c[k].tag, "AFFL") == 0 && c[k].size >= 4) { affl = (int32_t)((uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24); continue; }
            if (strcmp(c[k].tag, "GT") != 0 || c[k].size < 68) continue;
            a = (int16_t)(d[0] | d[1] << 8); b = (int16_t)(d[2] | d[3] << 8); hp = (int16_t)(d[16] | d[17] << 8);
            /* the destroyed variant (b) is not created until the swap (the thing table's entries, 0x10024fe0 -> 0x10025450:
             * flagged 0x800, they wait), nor are its children: hidden here whatever its model is called - the port hid
             * only the SA* models, so GREYSCN1's wrecked Wolf dropship (VA1DROPA / VA1DRPWS, the destroyed variants of
             * the V_B* dropship that GREYDRP1 carries to the drop site) and GREYFLF1's crashed Aerotechs stood at the
             * records' own origin, mid-map, from the start (user report: a disassembled dropship one walks through) */
            if (b >= 0 && b != a && w->part_count > 0) {
                char *mark = calloc((size_t)w->part_count, 1);   /* the variant and everything under it */
                int changed = 1;
                if (mark) {
                    /* by object index (a dummy node in between - GREYFLF1's 0x70 nodes - has no part of its own) */
                    int objs[64], no = 0, j;
                    objs[no++] = b;
                    while (changed) {
                        changed = 0;
                        for (q = 0; q < w->part_count; q++) {
                            if (mark[q] || strcmp(w->parts[q].rec, w->parts[i].rec) != 0) continue;
                            for (j = 0; j < no; j++)
                                if (w->parts[q].obj_index == objs[j] || w->parts[q].parent_obj == objs[j]) {
                                    mark[q] = 1; changed = 1;
                                    if (no < 64) objs[no++] = w->parts[q].obj_index;
                                    break;
                                }
                        }
                    }
                    for (q = 0; q < w->part_count; q++) if (mark[q]) ((mech3d_part *)&w->parts[q])->hidden = 1;
                    free(mark);
                }
            }
            if (((d[20] | d[21] << 8) & 0x8000) && (d[18] & 0xf0) != 0x70)   /* a debris node (0x10025450 / 0x10015530) */
                for (q = 0; q < w->part_count; q++)
                    if (w->parts[q].obj_index == a && strcmp(w->parts[q].rec, w->parts[i].rec) == 0) w->parts[q].gt_debris = 1;
            /* 0x10046c20 takes no damage at 0 hit points or with flag 0x0004 (+20, the runtime "destroyed" bit);
             * flag 0x0400 (dropships, gunships, spheres) is only a type selector, not tested there */
            /* things that take no damage (0 hit points, or flag 4) stay when they have a class: the objective nodes
             * attach them (0x1003ff60) - an indestructible protect target never fails, a scan target is inspected */
            if ((hp <= 0 || (d[20] & 0x04)) && !(d[18] | d[19])) continue;
            for (q = 0; q < w->part_count; q++)
                if (strcmp(w->parts[q].rec, w->parts[i].rec) == 0) {
                    if (w->parts[q].obj_index == a) ia = q;
                    if (w->parts[q].obj_index == b) ib = q;
                }
            if (ia < 0) continue;
            {
                struct msim_building *g = realloc(s->bld, (size_t)(s->bld_count + 1) * sizeof *g);
                if (!g) break;
                s->bld = g;
                g = &s->bld[s->bld_count++];
                g->intact = ia; g->destroyed = ib; g->hp = (hp <= 0 || (d[20] & 0x04)) ? 0x7fffffff : hp; g->inspected = 0;
                g->type = d[18] | d[19] << 8;   /* +18: the thing's class bits (0x1003ddf0 widens them) */
                g->flags = d[20] | d[21] << 8;   /* +20: the thing record's flags word (0x1003eb97): targeting 0x400 / 0x800 / 0x1000 (0x1001d2d0) */
                g->side = affl < 0 || affl >= 64 ? 2 : team_all[affl];   /* 0x10019060: no team -> 2, else the team's alliance */
                const mech3d_part *part = &w->parts[ia];
                if (part_box(part, g->mn, g->mx) != 0)
                    for (int axis = 0; axis < 3; axis++)
                        g->mn[axis] = g->mx[axis] = (float)part->pos[axis];
                snprintf(g->name, sizeof g->name, "%.22s", (const char *)d + 24);
                snprintf(g->sub, sizeof g->sub, "%.22s", (const char *)d + 46);
            }
        }
    }
    bwd_mission_free(&mission);
}

/* the engine's collision-list insertion order (msim clist_seq): the loader 0x1003de10 handles a record's chunks in
 * order and an INCL (0x10042c60) loads the included record there and then (depth first); an OBJ outside any BLK..ENDB
 * block is created at once (0x10043270 -> 0x1000db80 / 0x1000ffd0 -> 0x1000dc60: the entry goes to the HEAD of the
 * list), one inside a block (BLK 0x10024ba0 sets the block DAT_1024dac8) is only registered in the thing table
 * (0x10024aa0) and created after the load, table entry by entry (0x10008ccf -> 0x10024fe0 -> 0x10025450 - the destroyed
 * variants, flagged 0x800, wait for their swap). So the list runs from the last object created to the first. */
static void corder_visit(const bwd_mission *m, const char *name, int *done, int *depth, int pass, int *seq, const mech3d *w, int *out)
{
    int r, k;
    for (r = 0; r < m->record_count; r++) if (strcasecmp(m->records[r].name, name) == 0) break;
    if (r == m->record_count || done[r] == pass) return;
    done[r] = pass;
    for (k = 0; k < m->records[r].chunk_count; k++) {
        const bwd_chunk *c = &m->records[r].chunks[k];
        if (strcmp(c->tag, "BLK") == 0) (*depth)++;
        else if (strcmp(c->tag, "ENDB") == 0) { if (*depth > 0) (*depth)--; }
        else if (strcmp(c->tag, "INCL") == 0) {
            uint16_t id;
            char nm[11];
            if (bwd_incl(c, &id, nm) == 0) corder_visit(m, nm, done, depth, pass, seq, w, out);
        } else if (strcmp(c->tag, "OBJ") == 0) {
            bwd_obj o;
            int q;
            if (bwd_obj_decode(c, &o) != 0 || (*depth > 0) != (pass == 2)) continue;
            ++*seq;
            for (q = 0; q < w->part_count; q++)
                if (w->parts[q].obj_index == o.index && strcasecmp(w->parts[q].rec, m->records[r].name) == 0 && !out[q]) out[q] = *seq;
        }
    }
}
static void clist_order(msim *s, const mech3d *w)
{
    bwd_mission m;
    int *done, depth, pass, seq = 0;
    free(s->clist_seq);
    s->clist_seq = calloc((size_t)(w->part_count > 0 ? w->part_count : 1), sizeof *s->clist_seq);
    s->clist_seq_n = s->clist_seq ? w->part_count : 0;
    s->clist_next = 0;
    if (!s->clist_seq || !s->arch || !s->scene[0] || bwd_mission_load(s->arch, s->scene, &m) != 0) return;
    if ((done = calloc((size_t)(m.record_count > 0 ? m.record_count : 1), sizeof *done))) {
        for (pass = 1; pass <= 2; pass++) { depth = 0; corder_visit(&m, s->scene, done, &depth, pass, &seq, w, s->clist_seq); }   /* outside, then inside blocks */
        free(done);
    }
    s->clist_next = seq;
    if (mw2_development_env("MW2_CLIST_TRACE")) {   /* TEST ONLY: the insertion order found */
        int q, z = 0;
        for (q = 0; q < w->part_count; q++) z += s->clist_seq[q] == 0;
        fprintf(stderr, "clist order %s: %d parts, %d objects seen, %d parts unplaced\n", s->scene, w->part_count, seq, z);
    }
    bwd_mission_free(&m);
}

void msim_set_world(msim *s, const mech3d *world)
{
    if (s->world != world || s->clist_seq_n != world->part_count) clist_order(s, world);
    if (s->world != world) {
        /* path tasks first: the carried parts are flagged `moving` and kept out of the static lists below */
        world3d_paths_free(s->paths);
        s->paths = s->arch && s->scene[0] ? world3d_paths_load(s->arch, s->scene, (mech3d *)world) : NULL;
        for (int k = 0; k < s->dos_spin_count; k++) {
            const struct msim_spin_seed *seed = &s->dos_spin_seeds[k];
            world3d_paths_dos_seed(s->paths, seed->record, seed->node, seed->period, seed->yaw);
        }
        buildings_init(s, world);
    }
    terrain_t *previous = (terrain_t *)s->terrain;
    terrain_t *terrain = terrain_build(world, s->clist_seq, s->clist_seq_n);
    if (s->world == world && previous && terrain && previous->dos_scenery && terrain->dos_scenery) {
        const int count = previous->nparts < terrain->nparts ? previous->nparts : terrain->nparts;
        memcpy(terrain->dos_scenery, previous->dos_scenery, (size_t)count * sizeof *terrain->dos_scenery);
    }
    terrain_free(previous);
    s->terrain = terrain;
    if (mw2_development_env("MW2_CLIST_TRACE") && s->terrain && ((terrain_t *)s->terrain)->nco > 0) {   /* TEST ONLY: the list's head */
        const terrain_t *tt = (const terrain_t *)s->terrain;
        int q;
        fprintf(stderr, "clist head at %.2fs:", s->now / 1000.0);
        for (q = 0; q < tt->nco && q < 3; q++)
            fprintf(stderr, " %s(%s/%d seq %d)", world->parts[tt->co[q].part].model_name, world->parts[tt->co[q].part].rec, world->parts[tt->co[q].part].obj_index,
                    s->clist_seq && tt->co[q].part < s->clist_seq_n ? s->clist_seq[tt->co[q].part] : -1);
        fprintf(stderr, " of %d\n", tt->nco);
    }
    int i, k;
    if (s->terrain) {
        terrain_t *tt = (terrain_t *)s->terrain;
        tt->thing_of = malloc((size_t)(world->part_count > 0 ? world->part_count : 1) * sizeof *tt->thing_of);
        for (i = 0; tt->thing_of && i < world->part_count; i++) tt->thing_of[i] = -1;
        for (i = 0; tt->thing_of && i < s->bld_count; i++) if (s->bld[i].intact >= 0 && s->bld[i].intact < world->part_count) tt->thing_of[s->bld[i].intact] = i;
    }
    free(s->obstacles);
    free(s->obstacle_part);
    s->world = world;
    s->obstacles = calloc((size_t)(world->part_count ? world->part_count : 1), sizeof *s->obstacles);
    s->obstacle_part = calloc((size_t)(world->part_count ? world->part_count : 1), sizeof *s->obstacle_part);
    s->obstacle_count = 0;
    free(s->los_terrain); free(s->los_terrain_box);
    s->los_terrain = calloc((size_t)(world->part_count ? world->part_count : 1), sizeof *s->los_terrain);
    s->los_terrain_box = calloc((size_t)(world->part_count ? world->part_count : 1), sizeof *s->los_terrain_box);
    s->los_terrain_count = 0;
    if (!s->obstacles || !s->obstacle_part) return;
    for (i = 0; i < world->part_count; i++) {
        const mech3d_part *p = &world->parts[i];
        const wtb_object *o;
        float mn[2] = {1e30f, 1e30f}, mx[2] = {-1e30f, -1e30f}, top = -1e30f;
        if (p->model.object_count < 1 || p->hidden || p->moving) continue;   /* carried parts: not a fixed footprint */
        o = &p->model.objects[0];
        for (k = 0; k < o->vert_count; k++) {
            float w[3];
            part_vertex(p, &o->verts[k], w);
            if (w[0] < mn[0]) mn[0] = w[0];
            if (w[0] > mx[0]) mx[0] = w[0];
            if (w[2] < mn[1]) mn[1] = w[2];
            if (w[2] > mx[1]) mx[1] = w[2];
            if (w[1] > top) top = w[1];
        }
        /* candidates: anything reaching probe height (2 m), excluding terrain-sized objects (> 300 m) - those go to the
         * line-of-fire list (hills, the ice walls, mesas: a shot's line through them is blocked, below) */
        if (top >= 200.0f && (mx[0] - mn[0] > 30000.0f || mx[1] - mn[1] > 30000.0f) && p->coll != 4 && s->los_terrain && s->los_terrain_box) {
            s->los_terrain[s->los_terrain_count] = i;
            s->los_terrain_box[s->los_terrain_count][0] = mn[0]; s->los_terrain_box[s->los_terrain_count][1] = mx[0];
            s->los_terrain_box[s->los_terrain_count][2] = mn[1]; s->los_terrain_box[s->los_terrain_count][3] = mx[1];
            s->los_terrain_count++;
        }
        if (top < 200.0f || mx[0] - mn[0] > 30000.0f || mx[1] - mn[1] > 30000.0f) continue;
        s->obstacle_part[s->obstacle_count] = i;
        s->obstacles[s->obstacle_count][0] = mn[0]; s->obstacles[s->obstacle_count][1] = mx[0];
        s->obstacles[s->obstacle_count][2] = mn[1]; s->obstacles[s->obstacle_count][3] = mx[1];
        s->obstacle_count++;
    }
    for (i = 0; i < world->part_count; i++) {   /* moving parts in the collision list: footprints refreshed every step */
        const mech3d_part *p = &world->parts[i];
        if (!p->moving || p->hidden || p->model.object_count < 1 || p->coll < 0 || p->coll > 9 || p->coll == 4 || p->coll == 5) continue;
        s->obstacle_part[s->obstacle_count] = i;
        s->obstacles[s->obstacle_count][0] = s->obstacles[s->obstacle_count][1] = -1e30f;
        s->obstacles[s->obstacle_count][2] = s->obstacles[s->obstacle_count][3] = -1e30f;
        s->obstacle_count++;
    }
    mov_refresh(s);
}


/* engine 0x100201c0: probe ahead at 0, 26.6, 45, 63.4 degrees toward the avoid side; each blocked
 * probe adds 204.8 turn; deflecting drops throttle to 1/4; re-checked every 90 / 181 ticks */
/* One probe (0x100204e0 -> 0x100103e0): a ray at the unit's centre height (object +0x54) from its centre - or, for the
 * check back toward the goal, from its side R - 1 cm out (0x100204e0 param 6) - against the whole collision list: the
 * NEAREST hit of the world entries (the terrain pieces too, whatever their size; type 6 spheres too) and the other
 * units' pieces (spheres; the unit's own skipped).
 * Returns 0 none, 1 an entry, 2 a unit; *climb: 0x100204b0 - an entry whose type has a ground handler (0 / 5, column 2
 * of 0x1024b3a8, 0x100102c0) hit on a face with normal y above 0.766 (0x3f441892), or of class 0x50; (*ox, *oz) the
 * thing's centre (its bounding sphere +0x34 / the unit), which 0x10020670 turns away from */
/* a unit piece's bounding sphere in the world as posed (0x10029c10: the model's centre +0x28 - its vertex box's middle -
 * through the world matrix, the radius +0x40 the farthest vertex from it); 0 for a part without vertices */
static int piece_sphere(const mech3d_part *p, float sph[4])
{
    const wtb_object *o;
    float mn[3] = {3.4e38f, 3.4e38f, 3.4e38f}, mx[3] = {-3.4e38f, -3.4e38f, -3.4e38f}, r2 = 0;
    wtb_vertex c;
    int k, j;
    if (p->model.object_count < 1 || (o = &p->model.objects[0])->vert_count < 1) return 0;
    for (k = 0; k < o->vert_count; k++) {
        const float v[3] = {(float)o->verts[k].x, (float)o->verts[k].y, (float)o->verts[k].z};
        for (j = 0; j < 3; j++) { if (v[j] < mn[j]) mn[j] = v[j]; if (v[j] > mx[j]) mx[j] = v[j]; }
    }
    for (k = 0; k < o->vert_count; k++) {
        float dx = (float)o->verts[k].x - (mn[0] + mx[0]) * 0.5f, dy = (float)o->verts[k].y - (mn[1] + mx[1]) * 0.5f, dz = (float)o->verts[k].z - (mn[2] + mx[2]) * 0.5f;
        if (dx * dx + dy * dy + dz * dz > r2) r2 = dx * dx + dy * dy + dz * dz;
    }
    memset(&c, 0, sizeof c);
    c.x = (int32_t)lrintf((mn[0] + mx[0]) * 0.5f); c.y = (int32_t)lrintf((mn[1] + mx[1]) * 0.5f); c.z = (int32_t)lrintf((mn[2] + mx[2]) * 0.5f);
    part_vertex(p, &c, sph);
    sph[3] = sqrtf(r2);
    return 1;
}

static int dos_probe(const msim *s, int self, const float p0[3], const float p1[3], int *climb, float *ox, float *oz, int *hit_unit)
{
    const float ray[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    const dos_shot_segment g = dos_shot_segment_make(p0, ray, 0);
    dos_probe_hit probe = {0};
    int unit = -2, group = 0, part = -1;
    float contact[3];
    if (self >= 0 && self < s->actor_count && s->actors[self].unit_class == 1) {
        const combat_unit *u = &s->units[self];
        probe.recover_terrain = u->blocked && u->y <= u->ground + 1.0f;
    }
    *climb = 0;
    if (g.length <= 0 || dos_scan_segment(s, self, &g, &unit, &group, &part, contact, &probe) == INT32_MAX) return 0;
    *climb = probe.climbable;
    *ox = probe.centre[0];
    *oz = probe.centre[2];
    if (hit_unit) *hit_unit = unit;
    return unit == -2 ? 1 : 2;
}

static int probe_engine(const msim *s, int self, const float p0[3], const float p1[3], int *climb, float *ox, float *oz)
{
    if (msim_dos_int) return dos_probe(s, self, p0, p1, climb, ox, oz, NULL);
    const terrain_t *t = (const terrain_t *)s->terrain;
    float tt = 3.4e38f, n[3] = {0, 1, 0}, dx = p1[0] - p0[0], dz = p1[2] - p0[2], l2 = dx * dx + dz * dz, len = sqrtf(l2);
    int ci, hit = 0, k;
    *climb = 0;
    if (t && clist_seg(s, p0, p1, CLIST_PROBE, &tt, n, &ci)) {
        const cobj_t *c = &t->co[ci];
        float box[6], sph[4];
        hit = 1;
        *climb = ((c->type == 0 || c->type == 5) && n[1] > 0.7660444f) || (s->world->parts[c->part].objtype & 0xf0) == 0x50;
        cobj_now(s, c, box, sph);
        *ox = sph[0]; *oz = sph[2];
    }
    if (len > 0) {   /* the units: their collision pieces in the same list - each piece a type 6 sphere (the unit's LOD objects,
                      * 0x10026430 -> 0x1000ffd0(6); DUMMY nodes, class 0x70, are left out), the probe's own unit skipped by
                      * its id (object +0x14); a sphere entered nearer wins (0x10029db0 -> 0x100107b0: a start inside hits at
                      * 10 cm). Destroyed units keep their pieces (only blown-off parts leave, 0x100156f0 -> 0x10015be0) */
        eseg_t g0;
        eseg_set(&g0, p0, p1);
        for (k = -1; k < s->actor_count; k++) {
            const mech3d *mq;
            int q;
            if (k == self) continue;
            if (k < 0) { if (!s->pl_mech_ok) continue; mq = s->pl_pit_ok && s->player_cockpit ? &s->pl_pit : &s->pl_mech; }
            else mq = msim_unit_hull(s, k);
            for (q = 0; q < mq->part_count; q++) {
                const mech3d_part *pp = &mq->parts[q];
                float sph[4], e, hl;
                if (pp->hidden || !pp->objl || !piece_sphere(pp, sph)) continue;   /* no OBJL entry: out of the list (mech3d.h) */
                e = eseg_sphere(sph, &g0);
                hl = e > 0.0f ? e : g0.len;   /* 0x100107b0: the end stays when the entry is 0 */
                if (!(e < tt) || !(hl < tt)) continue;
                tt = hl;
                hit = 2; *climb = 0; *ox = sph[0]; *oz = sph[2];
                if (mw2_development_env("MW2_PROBE_TRACE")) fprintf(stderr, "probe %.2fs actor %d: unit %d piece %s r %.0f at %.0f of %.0f\n", s->now / 1000.0, self, k, pp->model_name, sph[3], hl, g0.len);   /* TEST ONLY */
            }
        }
    }
    return hit;
}

int msim_test_probe(const msim *s, const float p0[3], const float p1[3], float *ox, float *oz)   /* TEST ONLY */
{
    int climb;
    return probe_engine(s, -2, p0, p1, &climb, ox, oz);
}

/* 0x2df30 divides by the local X/Z table and transforms the endpoint from the
 * unit's centre. The side probe moves the origin only. */
static void dos_probe_points(const float centre[3], float heading, int step, int side, int length,
                             int lateral, float from[3], float to[3])
{
    static const int xdiv[] = {0, 2, 1, 1}, zdiv[] = {1, 1, 1, 2};
    dos_matrix pose = dos_matrix_rotation(0, (int32_t)lrint((double)heading * 65536.0), 0);
    int32_t a[3], b[3];
    for (int j = 0; j < 3; j++) pose.p[j] = (int32_t)lroundf(centre[j]);
    const int sign = side < 0 ? -1 : 1;
    const int32_t local_from[3] = {sign * lateral, 0, 0};
    const int32_t local_to[3] = {step == 0 ? 0 : sign * (length / xdiv[step]), 0, length / zdiv[step]};
    dos_matrix_point(&pose, local_from, a);
    dos_matrix_point(&pose, local_to, b);
    for (int j = 0; j < 3; j++) { from[j] = (float)a[j]; to[j] = (float)b[j]; }
}

static float dos_probe_scale(float vx, float vz, int mech)
{
    int32_t x = (int32_t)lroundf(vx * 65536.0f), z = (int32_t)lroundf(vz * 65536.0f);
    int32_t max = x > z ? x : z, min = x < z ? x : z;
    int32_t f = (int32_t)((((int64_t)max * 4 + min) >> 2) * 65536 / 500000);
    if (!mech) f >>= 1;
    return (float)(f < 19660 ? 19660 : f) / 65536.0f;
}

static int avoid_obstacles(msim *s, int i, ai_mind *m, float goal_bearing, float ticks)
{
    static const float ANG[4] = {0.0f, 26.565f, 45.0f, 63.435f}, LEN[4] = {1.0f, 1.118f, 1.414f, 1.118f};
    world_actor *ac = &s->actors[i];
    float x = (float)ac->mech.origin[0], z = (float)ac->mech.origin[2], len, accum = 0;
    int step, side = s->avoid_side[i];
    if ((!side && msim_dos_int && m->throttle <= 0) || (msim_dos_int && m->reverse) ||
        (!msim_dos_int && m->throttle <= 0)) {
        s->avoid_turn[i] = 0;
        return 0;
    }
    float now = 0;
    if (msim_dos_int) {
        now = (float)sim_ticks(s);
        if (now < s->avoid_next[i]) {
            if (s->avoid_turn[i] == 0) return 0;
            m->turn = s->avoid_turn[i];
            m->throttle = s->avoid_thr[i];
            return 1;
        }
    } else {
        s->avoid_next[i] -= ticks;
        if (s->avoid_next[i] > 0) {
            if (s->avoid_turn[i] != 0) { m->turn = s->avoid_turn[i]; m->throttle = s->avoid_thr[i]; }
            return s->avoid_turn[i] != 0;
        }
    }
    if (side == 0) {
        /* the length factor +0x192, recomputed only while no side is set: the unit's VELOCITY (physics +0xf4 / +0xfc, cm/tick)
         * as (4 max + mid + min) / 4 (0x10030330) x 0.12987 (0x10247534; DOS 0x2db80: << 16 / 500000), at least 0.3
         * (0x3e99999a) - not its top speed x throttle: a unit starting from rest probes 0.3 x 5000 cm = 15 m (the port's
         * 40 m probe from a standstill turned ORANSCN1's Trainer Maddog away from an object 37 m ahead into a wide loop
         * west; DOSBox: it turns tightly and heads off south-east) */
        float h = ac->mech.heading * 3.14159265f / 180.0f, a = s->across ? s->across[i] : 0.0f;
        float vx = fabsf((sinf(h) * m->speed + cosf(h) * a) / 182.0f), vz = fabsf((cosf(h) * m->speed - sinf(h) * a) / 182.0f);
        if (msim_dos_int) s->avoid_len[i] = dos_probe_scale(vx, vz, ac->unit_class == 1);
        else {
            float f = ((vx > vz ? vx : vz) * 4.0f + (vx > vz ? vz : vx)) * 0.25f * 0.12987013f;
            if (ac->unit_class != 1) f *= 0.5f;
            s->avoid_len[i] = fmaxf(f, 0.3f);
        }
    }
    len = s->avoid_len[i] * 5000.0f;
    {
        float yc = s->units[i].y + (s->centre_h ? s->centre_h[i] : 550.0f), R = s->radius ? s->radius[i] : 450.0f;
        const float centre[3] = {x, yc, z};
        for (step = 0; step < 4; step++) {
            float a = (ac->mech.heading + (float)side * ANG[step]) * 3.14159265f / 180.0f, L = len * LEN[step];
            float p0[3] = {x, yc, z}, p1[3] = {x + sinf(a) * L, yc, z + cosf(a) * L}, ox = 0, oz = 0;
            if (msim_dos_int) dos_probe_points(centre, ac->mech.heading, step, side, (int)len, 0, p0, p1);
            /* the DOS edition's probe (DOS 0x2db80 serves the AI and the player alike: 0x2df30 -> 0x1c3f0 -> 0x3a745) has the
             * DOS integer collision scan. The 3D editions use the float scan (0x100201c0 -> 0x100103e0 -> 0x10029db0). */
            int climb, unit = -2;
            int hit = msim_dos_int ? dos_probe(s, i, p0, p1, &climb, &ox, &oz, &unit)
                                   : probe_engine(s, i, p0, p1, &climb, &ox, &oz);
            if (!hit) {
                if (step == 0 && fabsf(wrap180(goal_bearing - ac->mech.heading)) <= 1.0f) side = 0;
                if (step == 0 && side != 0) {
                    /* ahead is clear but the goal is still off to the other side: the engine probes 26.6 degrees back toward
                     * it from the unit's side (0x100201c0: 0x100204e0(-side, 1, ..): R - 1 cm out on that side) and, while
                     * that ray hits something not climbable, keeps turning away (+side x 409.6, 0x10247594) at full throttle -
                     * it follows the wall instead of steering straight back into it */
                    float bh = (ac->mech.heading - (float)side * ANG[1]) * 3.14159265f / 180.0f, Lb = len * LEN[1];
                    float hr = ac->mech.heading * 3.14159265f / 180.0f, off = (float)(-side) * (R - 1.0f);
                    float q0[3] = {x + cosf(hr) * off, yc, z - sinf(hr) * off}, q1[3];
                    int bclimb;
                    q1[0] = q0[0] + sinf(bh) * Lb; q1[1] = yc; q1[2] = q0[2] + cosf(bh) * Lb;
                    if (msim_dos_int) dos_probe_points(centre, ac->mech.heading, 1, -side, (int)len, (int)R - 1, q0, q1);
                    if (probe_engine(s, i, q0, q1, &bclimb, &ox, &oz) && !bclimb) accum += (float)side * 409.6f;
                }
                break;
            }
            if (climb) break;   /* 0x100204b0: a climbable / class 0x50 hit ends the scan - no turn, no wall follow */
            if (side == 0) {   /* 0x10020670: turn away from the thing hit (its centre: a terrain piece's too) */
                float ob = atan2f(ox - x, oz - z) * 180.0f / 3.14159265f;
                side = wrap180(ob - ac->mech.heading) < 0 ? 1 : -1;
                m->dos_avoid_unit = unit;
            }
            accum += (float)side * 204.8f;
        }
    }
    s->avoid_side[i] = side;
    if (accum != 0) {
        if (accum > 819.2f) accum = 819.2f;
        if (accum < -819.2f) accum = -819.2f;
        s->avoid_turn[i] = accum;
        s->avoid_thr[i] = step != 0 ? 0.25f : 1.0f;   /* 0x100201c0: 1/256 (a quarter) when deflecting, else full (1/64) */
        if (ac->unit_class == 6) s->avoid_turn[i] = s->avoid_thr[i] = 0;   /* 0x100201c0: class 6 zeroes the output */
        m->turn = s->avoid_turn[i];
        m->throttle = s->avoid_thr[i];
    } else {
        s->avoid_turn[i] = 0;
    }
    s->avoid_next[i] = msim_dos_int ? now + (side == 0 ? 182.0f : 91.0f) : side == 0 ? 181.0f : 90.0f;
    return s->avoid_turn[i] != 0;
}

/* The player's autopilot obstacle avoidance: DOS 0x2db80 (from the autopilot 0x24ca0), the same scan as the AI's
 * (avoid_obstacles, 3Dfx 0x100201c0) with the player's differences - and the DOS integer collision scan:
 *  - checked every 10 ticks (DAT_0009581c + 10; AI 0x5b / 0xb6); between checks it returns "+0x182 != 0": the inputs it
 *    (or the nav steering) wrote last are held;
 *  - exits at once (0) when the unit cannot move (physics +0x88 == 0), or the throttle input is 0 with no side set, or the
 *    reverse input +0x2f is set;
 *  - the probe length factor +0x18c (recomputed only while no side is set): the velocity's size as the engine's
 *    (4 max + mid + min) / 4 of |+0xf4|, 0, |+0xfc| (16.16 cm/tick), << 16 / 500000 (0x7a120), a mech (class 1) not
 *    halved, at least 0x4ccc (0.3, the compare against 19660.8 at 0x90af4); the probe x 5000 cm (0x13880000);
 *  - probes 0 / 26.6 / 45 / 63.4 degrees toward the side (table 0x96374: local (x, z) = len / (0,1), (2,1), (1,1),
 *    (1,2) through the unit object's world matrix - DOSBox: the matrix is the player's own pose, translation = +0x50 /
 *    +0x58); a probe hit that is not climbable (0x1c2e0: a ground-type entry hit on a face with normal y >= 0xc41b, or
 *    class 0x50) sets the side away from the thing hit (0x2e060) and adds side x 204.8 (0x90afc x 0.25); a free step-0
 *    probe with the bearing error to the nav within 1 degree (0x230d0 < 0x10001) clears the side; with a side set and
 *    step 0 free, the wall follow: a probe 26.6 degrees back toward the goal from the unit's side R - 1 cm out
 *    (0x2df30 param 6) that hits something not climbable adds side x 409.6 (0x90b0c);
 *  - turn clamped to +-819.2 (0x3333333), throttle input 0x100 (a quarter of full) when a probe was blocked, else the
 *    saved +0x17a (the pilot's throttle) for the wall follow; input +0xc = turn.
 * Because of the DOS broad phase the probes see an object only at contact (its sphere holding the probe's start) unless
 * its centre lies ahead in both x and z: DOSBox TNJ4 the autopilot walks straight into S_1BQDAS, is pinned ~1-2 s, turns
 * away, follows the face and resumes toward the nav */
int msim_player_avoid(msim *s, float heading, float along, float across, float throttle, int reverse, int immobile,
                      float goal_err, float saved_thr, int *wrote, float *turn, float *thr)
{
    float now = (float)sim_ticks(s), x = s->player[0], z = s->player[2], accum = 0, len;
    float yc = s->player_unit.y + s->player_centre_h, R = s->player_radius;
    const float centre[3] = {x, yc, z};
    int step, side = s->pav_side;
    *wrote = 0;
    if (!s->terrain || immobile || (side == 0 && throttle == 0) || reverse) return 0;
    if (now < s->pav_next) return s->pav_hit != 0;
    if (side == 0) {   /* 0x2dbea-0x2dc5e */
        float hr = heading * 3.14159265f / 180.0f;
        float vx = fabsf((along * sinf(hr) + across * cosf(hr)) / 182.0f), vz = fabsf((along * cosf(hr) - across * sinf(hr)) / 182.0f);
        s->pav_len = dos_probe_scale(vx, vz, 1);
    }
    len = s->pav_len * 5000.0f;
    for (step = 0; step < 4; step++) {
        float p0[3], p1[3], ox = 0, oz = 0;
        dos_probe_points(centre, heading, step, side, (int)len, 0, p0, p1);
        int climb, hit = dos_probe(s, -1, p0, p1, &climb, &ox, &oz, NULL);
        if (!hit) {
            if (step == 0) {
                if (fabsf(goal_err) <= 1.0f) { s->pav_hit = 0; side = 0; }   /* on course: the side cleared */
                if (side != 0) {   /* the wall follow: 26.6 degrees back toward the goal from the side R - 1 cm out */
                    float q0[3], q1[3];
                    int bclimb;
                    dos_probe_points(centre, heading, 1, -side, (int)len, (int)R - 1, q0, q1);
                    if (dos_probe(s, -1, q0, q1, &bclimb, &ox, &oz, NULL) && !bclimb) accum += (float)side * 409.6f;
                }
            }
            break;
        }
        if (climb) break;   /* 0x2dce5 */
        if (side == 0) {   /* 0x2e060: away from the thing hit; +0x182 = it */
            float ob = atan2f(ox - x, oz - z) * 180.0f / 3.14159265f;
            side = wrap180(ob - heading) < 0 ? 1 : -1;
            s->pav_hit = 1;
        }
        accum += (float)side * 204.8f;
    }
    s->pav_side = side;
    if (accum != 0) {
        if (accum > 819.2f) accum = 819.2f;
        if (accum < -819.2f) accum = -819.2f;
        *wrote = 1; *turn = accum;
        *thr = step != 0 ? 0.25f : saved_thr;   /* 0x100 of the 0x400 full scale */
    }
    s->pav_next = now + 10.0f;
    if (mw2_development_env("MW2_PAV_TRACE")) fprintf(stderr, "pav %.2fs side %d hit %d steps %d turn %.1f len %.0f\n", s->now / 1000.0, side, s->pav_hit, step, (double)accum, (double)len);   /* TEST ONLY */
    return accum != 0;
}

/* engine 0x10015090 (AI pilots below level 5): no fire when the line to the target hits the world */
static int los_blocked(const msim *s, float x0, float y0, float z0, float x1, float y1, float z1)
{
    int i;
    if (!s->world || !s->obstacle_part) return 0;
    for (i = 0; i < s->obstacle_count; i++) {
        const float *b = s->obstacles[i];
        float lo_x = x0 < x1 ? x0 : x1, hi_x = x0 > x1 ? x0 : x1, lo_z = z0 < z1 ? z0 : z1, hi_z = z0 > z1 ? z0 : z1;
        float o3[3] = {x0, y0, z0}, d3[3] = {x1 - x0, y1 - y0, z1 - z0}, t;
        mech3d one;
        if (b[1] < lo_x || b[0] > hi_x || b[3] < lo_z || b[2] > hi_z) continue;
        if (x0 >= b[0] && x0 <= b[1] && z0 >= b[2] && z0 <= b[3]) continue;
        memset(&one, 0, sizeof one);
        one.parts = (mech3d_part *)&s->world->parts[s->obstacle_part[i]];
        one.part_count = 1;
        if (seg_mech(&one, o3, d3, &t)) return 1;
    }
    /* the terrain (user report, Burning Chrome: the Mad Dog and the Summoner fired LRMs into the ice walls in front of
     * them and the ledge turret shot at its ice wall - the line to the player passed through the wall, the port only
     * tested buildings) */
    {   /* (from 3 m out: a turret on a ledge stands on the terrain it would otherwise "hit" at once) */
        const float len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0));
        if (len > 1000.0f) { const float k = 300.0f / len; x0 += (x1 - x0) * k; y0 += (y1 - y0) * k; z0 += (z1 - z0) * k; }
    }
    for (i = 0; i < s->los_terrain_count; i++) {
        const float *b = s->los_terrain_box[i];
        float lo_x = x0 < x1 ? x0 : x1, hi_x = x0 > x1 ? x0 : x1, lo_z = z0 < z1 ? z0 : z1, hi_z = z0 > z1 ? z0 : z1;
        float o3[3] = {x0, y0, z0}, d3[3] = {x1 - x0, y1 - y0, z1 - z0}, t;
        mech3d one;
        if (b[1] < lo_x || b[0] > hi_x || b[3] < lo_z || b[2] > hi_z) continue;
        memset(&one, 0, sizeof one);
        one.parts = (mech3d_part *)&s->world->parts[s->los_terrain[i]];
        one.part_count = 1;
        if (seg_mech(&one, o3, d3, &t) && t < 0.995f) {   /* (not the target's own footing at the far end) */
            if (mw2_development_env("MW2_LOS_TRACE")) { static int said; if (said++ < 40) fprintf(stderr, "los: %.2fs blocked by %s at t %.2f from %.0f,%.0f\n", s->now / 1000.0, s->world->parts[s->los_terrain[i]].model_name, (double)t, x0, z0); }   /* TEST ONLY */
            return 1;
        }
    }
    return 0;
}

int msim_player_restart(msim *s)
{
    combat_unit *u = &s->player_unit;
    if (!u->shutdown || u->destroyed || (u->shutdown_pending && !u->override)) return 0;
    u->shutdown = 0; u->manual_down = 0;
    s->player_online_at = startup_deadline(s, startup_ticks(s), 0);
    if (msim_dos_int) dos_player_startup_sound(s);
    return 1;
}

void msim_end_now(msim *s)
{
    if (!s->ending) { s->ending = 1; s->player_lost = s->player_unit.destroyed; }
    s->over = 1;
    s->ended_at = s->now;
}

const char *msim_outcome_text(const msim *s)
{
    switch (s->outcome) {
    case 2: return "Mission successful";
    case 3: return "Mission failed";
    case 4: return "Mission time exceeded";
    default: return NULL;
    }
}

static void put32le(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }

int msim_write_results(const msim *s, const char *path)
{
    /* engine 0x1000aba0: [0] "MW2M", [1] entries, [2] the star's start (s), [3] when its result was set (s, -1 none;
     * the port: the end), [4] its status; per visible table-0 node 13 ints: [0] succeeded, [1] the raw priority byte
     * (L-6), [2] armed (s), [3] resolved (s, -1), [4] main objective ('M'), text from [5] (node +0xad) - as the
     * original's own file (YELLSCN1: types 1, 1, 2, 4; [4] 1 for the two M nodes) */
    unsigned char b[0x9d4];
    const mtbl_table *t0 = &s->logic.tables[0];
    int i, n = 0;
    FILE *f;
    memset(b, 0, sizeof b);
    put32le(b + 0, 0x4d32574d);   /* "MW2M", as the original's file */
    put32le(b + 8, 0);
    put32le(b + 12, (uint32_t)((s->ended_at ? s->ended_at : s->now) / 1000));
    put32le(b + 16, (uint32_t)s->outcome);
    for (i = 0; i < t0->node_count && 20 + (n + 1) * 52 <= (int)sizeof b; i++) {
        const mtbl_node *nd = &t0->nodes[i];
        unsigned char *e = b + 20 + n * 52;
        int32_t st = s->logic.started[0][i], en = s->logic.ended[0][i];
        if (!s->logic.shown[0][i]) continue;
        put32le(e, s->logic.state[0][i] == MTBL_OK);
        put32le(e + 4, (uint32_t)nd->priority);
        put32le(e + 8, st >= 0 ? (uint32_t)(st / 1000) : 0xffffffffu);
        put32le(e + 12, en >= 0 ? (uint32_t)(en / 1000) : 0xffffffffu);
        put32le(e + 16, nd->objective == 'M' ? 1u : 0u);
        memcpy(e + 20, nd->text, strlen(nd->text) < 31 ? strlen(nd->text) : 31);
        n++;
    }
    put32le(b + 4, (uint32_t)n);
    f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(b, 1, sizeof b, f) != sizeof b) { fclose(f); return -1; }
    return fclose(f);
}

void msim_jet_flames_at(msim *s, const mech3d *m, float x, float y, float z, float heading)
{
    float o[3] = {x, y, z}, p[3];
    int q, n = 0;
    if (!s->on_effect) return;
    for (q = 6; q <= 7 && m; q++)
        if (mech3d_mount_world(m, q, o, heading, p) == 0) {
            mech3d_mount_rot(m, q, heading, s->fx_rot); s->fx_rot_ok = 1;   /* the jet node's orientation (0x1001bf90 -> 0x10045cc0) */
            s->on_effect(s->shot_user, FX_JET, p); n++;
            s->fx_rot_ok = 0;
        }
    if (!n) msim_jet_flames(s, x, y, z, heading);
}

void msim_jet_flames(msim *s, float x, float y, float z, float heading)
{
    /* engine 0x1001bf90: a flame at each of the two jet mounts every tick while jetting; the mounts
     * are taken as 1.2 m either side of the feet (port) */
    float h = heading * 3.14159265f / 180.0f, side;
    if (!s->on_effect) return;
    s->fx_rot[0] = cosf(h); s->fx_rot[1] = 0; s->fx_rot[2] = sinf(h);   /* the heading's yaw (no jet nodes here) */
    s->fx_rot[3] = 0; s->fx_rot[4] = 1; s->fx_rot[5] = 0;
    s->fx_rot[6] = -sinf(h); s->fx_rot[7] = 0; s->fx_rot[8] = cosf(h);
    s->fx_rot_ok = 1;
    for (side = -1; side <= 1; side += 2) {
        float p[3] = {x + cosf(h) * 120.0f * side, y + 50.0f, z - sinf(h) * 120.0f * side};
        s->on_effect(s->shot_user, FX_JET, p);
    }
    s->fx_rot_ok = 0;
}
