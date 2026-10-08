/* msim.c - see msim.h. */
#include "msim.h"
static unsigned rnd_ms(unsigned *x);
#include "fx.h"
#include "datapath.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "bwd.h"

static float g_block_n[3] = {0, 1, 0};   /* see refuse() */
static const char *g_block_what;   /* TEST ONLY (MW2_BLOCK_TRACE): what refused the last step */
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

/* the player's starmates in lance order: point 1 = the first */
static int star_member(const msim *s, int point)
{
    int i, n = 0;
    for (i = 0; i < s->actor_count; i++)
        if (s->actors[i].friendly && s->actors[i].table == 0 && ++n == point) return i;
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
    if (point >= 0 && point < 3) snprintf(line, sizeof line, "%s %s", POINT_TEXT[point], ORDER_MSG[msg].text);
    else snprintf(line, sizeof line, "%s", ORDER_MSG[msg].text);
    if (s->on_message) s->on_message(s->message_user, line, ORDER_MSG[msg].voice);
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
    if ((order == MSIM_CMD_ATTACK || order == MSIM_CMD_DEFEND) &&
        (target < 0 || target >= s->actor_count || !s->armed[target] || s->units[target].destroyed)) return 0;   /* 0x1000: no target */
    for (i = first; i <= last; i++) {
        int a = star_member(s, i);
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
        m->state = order; m->target = order == MSIM_CMD_ATTACK;
        if (s->defend_leg) s->defend_leg[a] = 0;
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
static int target_pos(msim *s, const char *name, float *x, float *z, float *radius)
{
    const msim_nav *n = find_nav(s, name);
    int i;
    *radius = 20000.0f;
    /* objects and groups: 200 m (engine 0x10009b80 compares squared distance with 4e8) */
    if (strcasecmp(name, "UserStar") == 0) { *x = s->player[0]; *z = s->player[2]; *radius = 20000.0f; return 0; }
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
        else { tx = (float)s->actors[idx].mech.origin[0]; ty = s->units[idx].y; tz = (float)s->actors[idx].mech.origin[2]; }
    } else {
        const mech3d_part *p = &s->world->parts[s->bld[idx].intact];
        tx = (float)p->pos[0]; ty = (float)p->pos[1]; tz = (float)p->pos[2];
    }
    if ((ux - tx) * (ux - tx) + (uy - ty) * (uy - ty) + (uz - tz) * (uz - tz) > r * r) return 0;
    if (type == 1 && table == 0) s->navs[idx].visited = 1;   /* 0x10009b80: the player's side has been there (0x20) */
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
    if (strcasecmp(t, "UserStar") == 0 && (class_widen(s->player_class) & kind)) { n++; ok += tgt_test(s, table, kind, 2, -1); }
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
        if (say[0] && strcmp(say, "NULL") != 0 && s->on_radio) { if (getenv("MW2_RADIO_TRACE")) fprintf(stderr, "radio node %d at %d ms\n", i, (int)s->now); s->on_radio(s->shot_user, say); }   /* TEST ONLY trace */
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

int msim_init(msim *s, prj_archive *a, const char *scene, world_actor *actors, int count)
{
    bwd_mission m;
    prj_record rec;
    int i, k;
    memset(s, 0, sizeof *s);
    snprintf(s->scene, sizeof s->scene, "%s", scene);
    s->actors = actors;
    s->actor_count = count;
    s->rng = 12345u;
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
        if (pl.gravity_g <= 0) pl.gravity_g = 1.0f;
        if (getenv("MW2_DEBUG_PLANET")) fprintf(stderr, "planet %s: %.4f g, temp %d, jet damping %.0f\n", scene, pl.gravity_g, (int)pl.temperature, pl.jet_damping);
        combat_set_planet(&pl);
        s->planet = pl;
        /* top speed falls with the planet's gravity: DOSBox, the Timber Wolf at full throttle reads 75 kph on JACK (1.1 g)
         * and 90 on YELL (0.9 g) - YELL timed by its nav range: 34.6 m per 2.135 s = 58.3 km/h; both fit walk MP x
         * 300 cm/s / g (54 / 1.1 = 49, 54 / 0.9 = 60). Engine 0x100190d0: controller +0x88 /= g x 0.0296 x 33.784. */
        for (i = 0; i < count; i++) actors[i].speed /= pl.gravity_g;
    }
    s->units = calloc((size_t)(count > 0 ? count : 1), sizeof *s->units);
    /* the start-up (engine 0x1001a180 state 0, 0x1001ab71): every mech, the player's too, comes online after
     * 0x43e + rand(0x16a) ticks = 5.97-7.95 s; until then no throttle, turning, jets or weapons */
    s->online_at = calloc((size_t)(count > 0 ? count : 1), sizeof *s->online_at);
    s->ai_focus = malloc((size_t)(count > 0 ? count : 1) * sizeof *s->ai_focus);
    s->ai_who = malloc((size_t)(count > 0 ? count : 1) * sizeof *s->ai_who);
    for (i = 0; s->ai_focus && s->ai_who && i < count; i++) s->ai_focus[i] = s->ai_who[i] = -2;
    s->powered_down = calloc((size_t)(count > 0 ? count : 1), sizeof *s->powered_down);
    s->seen_node = malloc((size_t)(count > 0 ? count : 1) * sizeof *s->seen_node);
    for (i = 0; s->seen_node && i < count; i++) s->seen_node[i] = -2;
    for (i = 0; s->online_at && i < count; i++) s->online_at[i] = (float)(0x43e + rnd_ms(&s->rng) % 0x16a) * 1000.0f / 182.0f;
    s->player_online_at = (float)(0x43e + rnd_ms(&s->rng) % 0x16a) * 1000.0f / 182.0f;
    combat_damage_live = getenv("MW2_NO_STARTUP") != NULL;   /* DAT_1024c570 = 0 at the mission start (0x1002388d) */
    s->armed = calloc((size_t)(count > 0 ? count : 1), sizeof *s->armed);
    if (!s->units || !s->armed) return -1;
    for (i = 0; i < count; i++) {
        char up[16];
        size_t q;
        for (q = 0; actors[i].loadout[q] && q < sizeof up - 1; q++) up[q] = (char)toupper((unsigned char)actors[i].loadout[q]);
        up[q] = '\0';
        s->armed[i] = combat_init(&s->units[i], a, up) == 0;
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
                        snprintf(s->player_skel, sizeof s->player_skel, "%s", g.skeleton);
                        snprintf(s->player_loadout, sizeof s->player_loadout, "%s", g.loadout);
                        for (k = 0; s->player_skel[k]; k++) s->player_skel[k] = (char)toupper((unsigned char)s->player_skel[k]);
                        for (k = 0; s->player_loadout[k]; k++) s->player_loadout[k] = (char)toupper((unsigned char)s->player_loadout[k]);
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
    if (getenv("MW2_DEBUG_LOADOUT")) fprintf(stderr, "player loadout %s\n", s->player_loadout);   /* tests */
    if (combat_init(&s->player_unit, a, s->player_loadout) != 0) {
        if (getenv("MW2_DEBUG_LOADOUT")) fprintf(stderr, "loadout %s failed: TBR00STD\n", s->player_loadout);
        snprintf(s->player_skel, sizeof s->player_skel, "TIMBRWLF");
        snprintf(s->player_loadout, sizeof s->player_loadout, "TBR00STD");
        combat_init(&s->player_unit, a, s->player_loadout);
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
        for (i = 0; i < count; i++) s->units[i].no_collision_damage = v[3] == 0;   /* a global switch (DAT_1024ac6c + 3): falls too */
        s->player_unit.no_heat = v[4] == 0;
        s->player_unit.no_collision_damage = v[3] == 0;
        combat_set_player_difficulty(&s->player_unit, v[5]);
        s->difficulty = v[5];
        {   /* MW2SND.CFG +0x24: chunky explosions (default on) */
            unsigned char *snd = NULL;
            size_t sl = 0;
            s->chunky = 1;
            if (dp_read_file("MW2SND.CFG", &snd, &sl) == 0) { if (sl >= 0x28) s->chunky = (snd[0x24] | snd[0x25] << 8 | snd[0x26] << 16 | snd[0x27] << 24) != 0; free(snd); }
        }
        if (getenv("MW2_DEBUG_LOADOUT")) fprintf(stderr, "MW2DIF: unlimited ammo %d, invulnerable %d, collision damage %d, heat tracking %d, difficulty %d\n", v[0], v[1], v[3], v[4], v[5]);
    }
    s->player_unit.hold_fire_above = 65.0f;      /* headless test player; glview sets 0 (you decide) */
    if (mech3d_load(a, s->player_skel, 0, &s->pl_mech) == 0 || mech3d_load(a, "TIMBRWLF", 0, &s->pl_mech) == 0) {
        s->pl_mech_ok = 1;
        mech3d_hit_weights(&s->pl_mech, s->player_unit.hit_w);
    }
    s->ai_ok = ai_load(&s->ai, a) == 0;
    s->minds = calloc((size_t)(count ? count : 1), sizeof *s->minds);
    s->arch = a;
    s->cmd = calloc((size_t)(count ? count : 1), sizeof *s->cmd);
    s->cmd_target = calloc((size_t)(count ? count : 1), sizeof *s->cmd_target);
    s->engage = calloc((size_t)(count ? count : 1), sizeof *s->engage);
    s->player_victim = -1;
    s->mate_target = calloc((size_t)(count ? count : 1), sizeof *s->mate_target);
    s->defend_leg = calloc((size_t)(count ? count : 1), sizeof *s->defend_leg);
    s->ai_lock = calloc((size_t)(count ? count : 1), sizeof *s->ai_lock);
    s->provoked = calloc((size_t)(count ? count : 1), sizeof *s->provoked);
    s->step_key = calloc((size_t)(count ? count : 1), sizeof *s->step_key);
    s->anim_seq = calloc((size_t)(count ? count : 1), sizeof *s->anim_seq);
    s->bump_with = calloc((size_t)(count ? count : 1), sizeof *s->bump_with);
    s->player_sel_w = 0;
    s->player_lock_target = -1;
    s->inspected = calloc((size_t)(count ? count : 1), 1);
    s->player_nav = -1;
    s->star_formation = -1;
    for (i = 0; i < 8; i++) s->last_cmd[i] = 7;                       /* "No Cmd" */
    for (i = 0; i < count && s->engage && s->cmd_target; i++) {
        s->cmd_target[i] = -1;
        if (s->mate_target) s->mate_target[i] = -1;
        s->engage[i] = !(actors[i].friendly && actors[i].table == 0);   /* 0x100124a0: the player's group 0 */
    }
    for (i = 0; s->minds && i < count; i++) ai_init(&s->minds[i], actors[i].leader ? AIP_LDEFEND : AIP_FDEFEND);
    s->air_speed = calloc((size_t)(count ? count : 1), sizeof *s->air_speed);
    s->avoid_turn = calloc((size_t)(count ? count : 1), sizeof *s->avoid_turn);
    s->avoid_thr = calloc((size_t)(count ? count : 1), sizeof *s->avoid_thr);
    s->avoid_next = calloc((size_t)(count ? count : 1), sizeof *s->avoid_next);
    s->avoid_side = calloc((size_t)(count ? count : 1), sizeof *s->avoid_side);
    s->height = calloc((size_t)(count ? count : 1), sizeof *s->height);
    s->wreck_until = calloc((size_t)count + 1, sizeof *s->wreck_until);
    s->twist_limit = calloc((size_t)(count ? count : 1), sizeof *s->twist_limit);
    s->radius = calloc((size_t)(count > 0 ? count : 1), sizeof *s->radius);
    s->fly_seen = calloc((size_t)count + 1, sizeof *s->fly_seen);
    s->centre_h = calloc((size_t)(count > 0 ? count : 1), sizeof *s->centre_h);
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
                if (hc > 0 && hc < 5000) s->centre_h[i] = (float)hc;
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
    if (prj_read_named(a, "BWD", scene, &rec) != PRJ_OK) return -1;
    mtbl_logic_init(&s->logic, rec.data, rec.size);
    prj_record_free(&rec);
    /* nav points: every NAVP in the mission tree, keyed by its record name */
    if (bwd_mission_load(a, scene, &m) == 0) {
        s->music_track = m.music_track;
        for (i = 0; i < m.record_count && s->nav_count < MSIM_MAX_NAVS; i++)
            for (k = 0; k < m.records[i].chunk_count; k++) {
                bwd_navpoint nv;
                if (bwd_navp(&m.records[i].chunks[k], &nv) != 0) continue;
                snprintf(s->navs[s->nav_count].name, sizeof s->navs[0].name, "%s", m.records[i].name);
                snprintf(s->navs[s->nav_count].label, sizeof s->navs[0].label, "%.21s", nv.name);
                /* engine 0x1001cea0: skipped when NAVP +16 is 0 or +22 differs from the unit's +8
                   (the player's group, assumed 0: 115 of 138 named nav points carry 0) */
                s->navs[s->nav_count].selectable = nv.shown != 0 && nv.team == 0;
                s->navs[s->nav_count].shown = nv.shown != 0;
                s->navs[s->nav_count].x = (float)nv.x;
                s->navs[s->nav_count].y = (float)nv.y;
                s->navs[s->nav_count].z = (float)nv.z;
                /* NAVP radius x100 (assumed metres -> cm); engine default 20,000 when unset */
                s->navs[s->nav_count].radius = nv.radius > 0 ? (float)nv.radius * 100.0f : 20000.0f;
                {   /* NAVP +0x1a: the class the objective nodes attach it by (0x1003ef3a) */
                    const bwd_chunk *nc = &m.records[i].chunks[k];
                    s->navs[s->nav_count].cls = nc->size >= 28 ? (unsigned)(nc->data[26] | nc->data[27] << 8) : 0x100u;
                }
                s->nav_count++;
                break;
            }
        bwd_mission_free(&m);
    }
    /* the player starts at table 0's start node target (e.g. CYANST01) */
    if (s->logic.table_count > 0) {
        const msim_nav *st = find_nav(s, s->logic.tables[0].nodes[0].target);
        if (st) { s->player[0] = st->x; s->player[2] = st->z; s->player_unit.y = st->y; if (getenv("MW2_DBG_START")) fprintf(stderr, "START %s y=%.0f ground=%.0f\n", st->name, (double)st->y, (double)msim_ground(s, st->x, st->z, 1e9f)); }
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
static unsigned rnd_ms(unsigned *x);
static void ai_power_down(msim *s, int i) { if (s->powered_down) s->powered_down[i] = 1; }
static void ai_wake(msim *s, int i)
{
    if (!s->powered_down || !s->powered_down[i]) return;
    s->powered_down[i] = 0;
    if (s->online_at) s->online_at[i] = (float)s->now + (float)(0x43e + rnd_ms(&s->rng) % 0x16a) * 1000.0f / 182.0f;
    if (s->on_sound && s->now >= s->player_online_at) {
        float p3[3] = {(float)s->actors[i].mech.origin[0], s->units[i].y, (float)s->actors[i].mech.origin[2]};
        s->on_sound(s->shot_user, 0x104, p3);
    }
}
static int pick_enemy(const msim *s, int i, float *ex, float *ez, float *ey, float *eface, float *eh, int *who)
{
    const world_actor *ac = &s->actors[i];
    float best = 1e30f, x0 = (float)ac->mech.origin[0], z0 = (float)ac->mech.origin[2];
    int k, found = 0;
    /* the enemy iterator (0x10013a50): units of another alliance, never neutrals (2); a neutral itself picks nobody -
     * DOSBox (TNW1: the instructor, alliance 2, never fires at the player unprovoked over 40 s; session 11's trace) -
     * until the player hits it (0x10014110 -> 0x10013370, s->player_victim). A unit turned on its attacker
     * (0x10012520, s->ai_focus) takes that one. */
    int self_al = ac->friendly ? 0 : ac->alliance, f = s->ai_focus ? s->ai_focus[i] : -2;
    if (f == -1 && !s->player_unit.destroyed) {
        *ex = s->player[0]; *ez = s->player[2]; *ey = s->player_unit.y; *eface = s->player_facing; *eh = 0; *who = -1;
        return 1;
    }
    if (f >= 0 && f < s->actor_count && s->armed[f] && !s->units[f].destroyed) {
        *ex = (float)s->actors[f].mech.origin[0]; *ez = (float)s->actors[f].mech.origin[2];
        *ey = s->units[f].y; *eface = s->actors[f].mech.heading; *eh = s->height ? s->height[f] : 0; *who = f;
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
            *ey = s->units[t].y; *eface = s->actors[t].mech.heading; *eh = s->height ? s->height[t] : 0; *who = t;
            return 1;
        }
    }
    if (self_al != 0 && !s->player_unit.destroyed && (self_al != 2 || s->player_victim == i || (s->provoked && s->provoked[i] > 0))) {
        float dx = s->player[0] - x0, dz = s->player[2] - z0;
        best = dx * dx + dz * dz;
        *ex = s->player[0]; *ez = s->player[2]; *ey = s->player_unit.y; *eface = s->player_facing; *eh = 0; *who = -1;
        found = 1;
    }
    for (k = 0; k < s->actor_count; k++) {
        float dx, dz, d;
        int al = s->actors[k].friendly ? 0 : s->actors[k].alliance;
        if (k == i || !s->armed[k] || s->units[k].destroyed || al == self_al || al == 2 || self_al == 2) continue;
        dx = (float)s->actors[k].mech.origin[0] - x0; dz = (float)s->actors[k].mech.origin[2] - z0;
        d = dx * dx + dz * dz;
        if (d < best) {
            best = d; found = 1; *who = k;
            *ex = (float)s->actors[k].mech.origin[0]; *ez = (float)s->actors[k].mech.origin[2];
            *ey = s->units[k].y; *eface = s->actors[k].mech.heading; *eh = s->height ? s->height[k] : 0;
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
            if (self_al == 0 || s->player_unit.destroyed || (self_al == 2 && s->player_victim != i && !(s->provoked && s->provoked[i] > 0))) continue;
            dx = s->player[0] - x0; dz = s->player[2] - z0;
        } else {
            int al = s->actors[k].friendly ? 0 : s->actors[k].alliance;
            if (k == i || !s->armed[k] || s->units[k].destroyed || al == self_al || al == 2 || self_al == 2) continue;
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
 * member qualifies. The engine takes the member whose +0x150 (a loadout rating, 0x10041c80 - not decoded) is closest to
 * the target's: the first in lance order here. leader: actor index, -1 = the player leads (the player's star). */
static int assign_ok(const msim *s, int q)
{
    const ai_mind *m = &s->minds[q];
    if (!s->armed[q] || s->units[q].destroyed || (s->cmd && s->cmd[q]) || m->fled) return 0;
    if (m->state == AI_TARGET || m->state == AI_ATTACK || (s->engage && !s->engage[q])) return 0;
    if (combat_max_range(&s->units[q]) <= 0) return 0;
    return ai_allows(&s->ai, m, AI_TARGET);   /* 0x10013e00 -> 0x10013d90 */
}
static void ai_wake(msim *s, int i);
static void group_assign(msim *s, int table, int leader, int who)
{
    int q, pick = -1;
    for (q = 0; q < s->actor_count && pick < 0; q++) {
        if (s->actors[q].table != table || q == leader || (leader < 0 && !s->actors[q].friendly)) continue;
        if (assign_ok(s, q)) pick = q;
    }
    if (pick < 0 && leader >= 0 && assign_ok(s, leader)) pick = leader;
    if (pick < 0) return;
    {
        ai_mind *m = &s->minds[pick];
        m->prev_state = m->state; m->state = AI_TARGET; m->target = 1; m->tgt = who;
        m->next_fire = (float)(rnd_ms(&s->rng) % 10u * 22u);   /* entry 0x10013790 case 2 */
        ai_wake(s, pick);
    }
}

static float wrap180(float a)
{
    while (a > 180) a -= 360;
    while (a < -180) a += 360;
    return a;
}

static void combat_step(msim *s, float dt);
static void terrain_release(void *t);
static float wrap180(float a);
static int los_blocked(const msim *s, float x0, float y0, float z0, float x1, float y1, float z1);
static void avoid_obstacles(msim *s, int i, ai_mind *m, float goal_bearing, float dt);
static unsigned rnd_ms(unsigned *x);
static unsigned rnd_ms(unsigned *x) { *x = *x * 1103515245u + 12345u; return (*x >> 16) & 0x7FFF; }
static float frand_ms(unsigned *x) { return (float)rnd_ms(x) / 32768.0f; }
static unsigned ms_roll(void *ctx, unsigned n) { return n ? (unsigned)rnd_ms((unsigned *)ctx) % n : 0; }


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


/* debris: launched with Gaussian velocities (5.5 cm/tick sd sideways, (g + 1) x 5.5 up) and spins
 * (0.5 / tick sd); gravity 0.0296 cm/tick^2; on landing vy = -vy / 4, and at random the horizontal
 * speed and spin halve and reflect; at rest below 0.5525 cm/tick (engine 0x100152e0, 0x100156f0) */
static void spawn_debris(msim *s, int count, const float p[3])
{
    /* (not gated by CHUNKY EXPLOSIONS: the engine reads that only when instantiating GT 0x8000 debris nodes, 0x100254ad) */
    int32_t now = (int32_t)((int64_t)s->now * 182 / 1000);
    int chunks = 0, k;
    for (k = 0; k < s->debris_count; k++) chunks += s->debris[k].kind < 2;
    /* MW2_SHT1 has 32 chunk (type 0x0b) objects - the chunks' own pool; the physics bodies (128) are shared with the
     * blown-off parts (0x10015230) */
    while (count-- > 0 && chunks < 32 && s->debris_count < MSIM_MAX_DEBRIS) {
        msim_debris *d = &s->debris[s->debris_count++];
        d->kind = chunks++ & 1;
        memcpy(d->p, p, sizeof d->p);
        d->v[0] = gauss(&s->rng) * 5.5f;
        d->v[1] = (gauss(&s->rng) + 1.0f) * 5.5f;
        d->v[2] = gauss(&s->rng) * 5.5f;
        for (k = 0; k < 3; k++) { d->ang[k] = 0; d->spin[k] = gauss(&s->rng) * 0.5f; }
        d->until = now + 1810;   /* the debris type's duration (0x1025b168, type 0x0b) */
        d->moving = 1;
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
    if (!getenv("MW2_AIM_TRACE") || !s->player_eye_ok) return;
    for (k = 0; k < 3; k++) v[k] = p[k] - sh->aim_o[k];
    t = v[0] * d[0] + v[1] * d[1] + v[2] * d[2];
    for (k = 0; k < 3; k++) m[k] = v[k] - d[k] * t;
    ml = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]); vl = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    printf("AIMHIT now=%d %s range=%.0f off_deg=%.3f miss_cm=%.0f right=%.0f up=%.0f conv=%.0f\n", (int)s->now, what, t, vl > 0 ? asinf(ml / vl) * 180.0f / 3.14159265f : 0.0f, ml,
           m[0] * cosf(ay) - m[2] * sinf(ay), m[1], s->player_conv);
}
static int missile_fx(int weapon) { int v = combat_weapon_visual(weapon); return v >= 0 && ((v & 0xff) == 3 || (v & 0xff) == 4); }
static void effect(msim *s, int type, const float p[3], int chunks)
{
    s->fx_refused = 0;
    if (s->on_effect) s->on_effect(s->shot_user, type, p);
    if (chunks && !s->fx_refused) spawn_debris(s, 4, p);
}

/* a destroyed location's parts fly off (engine 0x10015410: same physics, 0xe24 ticks, pool of 128) */
/* the parts a newly destroyed location sends flying (engine 0x10016640 -> 0x1000ff70 -> 0x10015530 / 0x10015410: the
 * location's node and everything under it - the centre torso carries the hips, so all of a mech; same physics as the
 * chunks, 0xe24 ticks, 128 bodies), each part once: those under a location in `mask` and under none in `old` */
static void blow_off(msim *s, int unit, unsigned mask, unsigned old)
{
    const mech3d *m = unit < 0 ? &s->pl_mech : &s->actors[unit].mech;
    int32_t now = (int32_t)((int64_t)s->now * 182 / 1000);
    int q, k;
    for (q = 0; q < m->part_count && s->debris_count < MSIM_MAX_DEBRIS; q++) {
        msim_debris *d;
        if (m->parts[q].model.object_count < 1 || !mech3d_fly_loc(m, q, mask) || mech3d_fly_loc(m, q, old)) continue;
        d = &s->debris[s->debris_count++];
        d->kind = 2; d->actor = unit; d->part = q;
        for (k = 0; k < 3; k++) d->p[k] = (float)m->parts[q].pos[k];
        d->v[0] = gauss(&s->rng) * 5.5f;
        d->v[1] = (gauss(&s->rng) + 1.0f) * 5.5f;
        d->v[2] = gauss(&s->rng) * 5.5f;
        for (k = 0; k < 3; k++) { d->ang[k] = 0; d->spin[k] = gauss(&s->rng) * 0.5f; }
        d->until = now + 0xe24;
        d->moving = 1;
    }
}
static void blow_offs(msim *s)
{
    int u;
    if (!s->fly_seen) return;
    for (u = -1; u < s->actor_count; u++) {
        const combat_unit *cu = u < 0 ? &s->player_unit : &s->units[u];
        unsigned mask = mech3d_fly_mask(cu->loc_gone);
        if (u >= 0 && !s->armed[u]) continue;
        if (mask != s->fly_seen[u + 1]) { blow_off(s, u, mask, s->fly_seen[u + 1]); s->fly_seen[u + 1] = mask; }
    }
}

static void debris_step(msim *s, float ticks)
{
    int32_t now = (int32_t)((int64_t)s->now * 182 / 1000);
    int i, k;
    for (i = 0; i < s->debris_count; i++) {
        msim_debris *d = &s->debris[i];
        if (now >= d->until) { s->debris[i--] = s->debris[--s->debris_count]; continue; }
        if (!d->moving) continue;
        {
            float vy = d->v[1] - 0.0296f * ticks, ny = d->p[1] + (d->v[1] - 0.5f * 0.0296f * ticks) * ticks, g;
            d->p[0] += d->v[0] * ticks; d->p[2] += d->v[2] * ticks;
            for (k = 0; k < 3; k++) d->ang[k] += d->spin[k] * ticks;
            g = msim_ground(s, d->p[0], d->p[2], d->p[1]);
            if (vy < 0 && ny <= g) {   /* landed: bounce */
                ny = g;
                vy = -vy * 0.25f;
                if (frand_ms(&s->rng) < 0.5f) { d->v[0] *= -0.5f; d->v[2] *= -0.5f; }   /* "at random" (odds not decoded) */
                for (k = 0; k < 3; k++) d->spin[k] *= -0.5f;
                if (vy < 0.5525f) { d->moving = 0; vy = 0; }
            }
            d->p[1] = ny; d->v[1] = vy;
        }
    }
}

static void car_inc(msim *s, int off);
static void wrecks(msim *s, int32_t dt_ms)
{
    int32_t now = (int32_t)((int64_t)s->now * 182 / 1000);
    int64_t f;
    int i;
    if (!s->wreck_until) return;
    for (i = 0; i <= s->actor_count; i++) {
        const combat_unit *u = i < s->actor_count ? &s->units[i] : &s->player_unit;
        float x, y, z;
        if (i < s->actor_count && !s->armed[i]) continue;
        if (!u->destroyed || u->intact_out) continue;   /* state 5 has no death sequence (0x1001a180 case 4 only) */
        if (i < s->actor_count) { x = (float)s->actors[i].mech.origin[0]; z = (float)s->actors[i].mech.origin[2]; y = u->y + s->height[i] * 0.5f; }
        else { x = s->player[0]; z = s->player[2]; y = u->y + 500.0f; }
        if (s->wreck_until[i] == 0) {
            float p[3] = {x, y, z};
            if (s->on_effect) s->on_effect(s->shot_user, 0x0d, p);
            s->wreck_until[i] = now + 0x712;
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

/* path tasks (world3d_paths): move the carried world parts; a carried structure's box follows it */
static void mov_refresh(msim *s);
static void paths_step(msim *s)
{
    int b;
    if (world3d_paths_step(s->paths, (mech3d *)s->world, (float)((double)s->now * 182.0 / 1000.0)) > 0)
        s->world_dirty = s->world_rebuild = 1;   /* parts whose paths ended: back in the static mesh and collision */
    for (b = 0; b < s->bld_count; b++) {
        struct msim_building *g = &s->bld[b];
        if (g->hp > 0 && g->intact >= 0 && g->intact < s->world->part_count && s->world->parts[g->intact].moving)
            part_box(&s->world->parts[g->intact], g->mn, g->mx);
    }
    mov_refresh(s);
}

void msim_step(msim *s, int32_t dt_ms)
{
    if (dt_ms <= 0) return;   /* a sub-millisecond frame: its time is carried to the next one (glview sim_ms) */
    if (s->world_rebuild && s->world) { s->world_rebuild = 0; msim_set_world(s, s->world); }
    mtbl_world w;
    float dt = (float)dt_ms / 1000.0f;
    int i;
    w.user = s; w.targets = cb_targets; w.finished = cb_finished; w.started = cb_started;
    s->now += dt_ms;
    if (s->paths && s->world) paths_step(s);
    if (s->player_vertical) {   /* tools without their own player physics: stand the player on the terrain */
        s->player_unit.ground = msim_ground(s, s->player[0], s->player[2], s->player_unit.y);
        combat_jets(&s->player_unit, 0, dt, &s->rng);
    }
    combat_step(s, dt);
    wrecks(s, dt_ms);
    blow_offs(s);   /* locations destroyed since the last step: their parts fly */
    debris_step(s, (float)dt_ms * 0.182f);
    mtbl_logic_step(&s->logic, s->now, &w);
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
        else if ((s->logic.result[0] && !s->radio_busy) || s->player_unit.destroyed) {
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
            if (s->on_message) s->on_message(s->message_user, "Press CTRL-Q to exit...", -0x1536);
        }
    } else if (!s->over && s->now >= s->end_at) {
        s->over = 1;
        s->ended_at = s->now;
    }

    /* AI (src/ai.c): every unit runs its AIT programs; the group's mission order picks the
     * order program (mapping assumed: destroy -> DESTROY, reach -> RECON, leave -> LEAVE,
     * otherwise DEFEND), leader or follower version. */
    if (s->star_post && s->minds && s->engage && s->cmd) group_assign(s, 0, -1, s->star_post - 2);   /* the player leads: its tick */
    s->star_post = 0;
    for (i = 0; s->minds && s->ai_ok && i < s->actor_count; i++) {
        world_actor *ac = &s->actors[i];
        ai_mind *m = &s->minds[i];
        ai_view v;
        int cur = ac->table < s->logic.table_count ? s->logic.current[ac->table] : -1, prog;
        world_actor *l = group_leader(s, ac->table);
        float dx, dz;
        int enemy_who = -1, in_star = ac->friendly && ac->table == 0 && !s->player_unit.destroyed;
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
        if (!in_star && s->seen_node && cur != s->seen_node[i]) {   /* 0x10014520 on the star's node change */
            uint32_t kn = cur >= 0 ? s->logic.tables[ac->table].nodes[cur].kind : 0;
            int destroy = (kn & 0xbu) != 0;   /* kinds 1 / 2 / 8 -> state 2 */
            s->seen_node[i] = cur;
            if (kn & 0x400) { m->state = AI_REST; m->target = 0; ai_power_down(s, i); }
            else if (kn & (0x800 | 0x10)) { m->state = AI_SHUTDOWN; m->target = 0; ai_power_down(s, i); }
            else {
                ai_wake(s, i);
                if (!destroy) { m->state = ac->leader ? AI_IDLE : AI_FOLLOW; m->target = 0; }
                else m->progs[1] = -99;   /* the destroy assignment below runs again */
            }
        }
        if (m->progs[1] != prog) {
            m->progs[1] = prog;
            /* engine 0x10014520 on a node change: kind 0x400 -> rest (10), 0x800 -> shutdown (11); otherwise the leader
             * idles (0) and the members follow (5). The player's star stops there - its mates stay in formation on the
             * player (node targets are never assigned to it; the bug where JACK's mates walked off to en01Star at the
             * start). Other stars then get the node's targets (0x10014ba0: one member a tick until the node's count is
             * covered, 0 = all) - here all at once, the target being the enemy pick_enemy finds. */
            uint32_t kk = cur >= 0 ? s->logic.tables[ac->table].nodes[cur].kind : 0;
            if (in_star) { if (!(s->cmd && s->cmd[i]) && m->state != AI_REST && m->state != AI_SHUTDOWN) { m->state = AI_FOLLOW; m->target = 0; } }
            else if ((prog == AIP_LDESTROY || prog == AIP_FDESTROY) && !s->player_unit.destroyed) {
                /* 0x10014520 then 0x10014ba0: the leader idles (0), the members follow (5); the node's targets are then
                 * handed out - members first (lance order), the leader last - until the node's count (L-4, 0 = all) is
                 * covered. Those beyond the count keep following */
                int need = cur >= 0 ? s->logic.tables[ac->table].nodes[cur].need : 0, rank = 0, q2, members = 0;
                for (q2 = 0; q2 < s->actor_count; q2++) {
                    if (s->actors[q2].table != ac->table || !s->armed[q2] || s->units[q2].destroyed) continue;
                    if (!s->actors[q2].leader) members++;
                    if (q2 < i && !s->actors[q2].leader && !ac->leader) rank++;
                }
                if (ac->leader) rank = members;
                if (need <= 0 || rank < need) { m->state = AI_TARGET; m->target = 1; }
                else { m->state = ac->leader ? AI_IDLE : AI_FOLLOW; m->target = 0; }
            }
            (void)kk;   /* 0x400 / 0x800 power-down (states 10 / 11) not applied yet: the port's AI has no rest / wake path */
        }
        memset(&v, 0, sizeof v);
        v.heading = ac->mech.heading;
        v.heat = s->units[i].heat;
        v.range_c = ac->ai_range[0]; v.range_a = ac->ai_range[1]; v.range_b = ac->ai_range[2];   /* GPS (engine 0x10012000) */
        v.skill = ac->ai_skill;
        v.twist_limit = s->twist_limit ? s->twist_limit[i] : 90.0f;
        v.self_x = (float)ac->mech.origin[0]; v.self_z = (float)ac->mech.origin[2];
        {
            float ex = 0, ez = 0, ey = 0, ef = 0, eh = 0;
            int who = -1;
            v.has_enemy = pick_enemy(s, i, &ex, &ez, &ey, &ef, &eh, &who);
            enemy_who = v.has_enemy ? who : -1;
            v.enemy_is_player = v.has_enemy && who < 0;
            m->targets_player = v.enemy_is_player;
            v.enemy_x = ex; v.enemy_z = ez; v.enemy_heading = ef; v.enemy_height = ey - s->units[i].ground;   /* against my own height above my ground */
        }
        v.flank_slots = s->flank_slots;
        /* the attacker reaction (0x10012520, at the start of every rule pass unless lock bit 1): a live unit of another
         * alliance attacking this one (state 3 with this one as its target - the player only while firing at an
         * alliance-1 target, 0x10011c50 -> 0x10014110) makes this one target it */
        if (s->ai_focus && s->ai_who && !(s->cmd && s->cmd[i]) && !(cur >= 0 && s->logic.tables[ac->table].nodes[cur].lock == 2)) {
            int self_al = ac->friendly ? 0 : ac->alliance, att = -2, q3;
            int pt = s->player_target;
            if (s->player_attacks && pt == i && self_al == 1 && !s->player_unit.destroyed) att = -1;
            if (self_al == 2 && s->player_victim == i && !s->player_unit.destroyed) att = -1;   /* a neutral the player hit */
            for (q3 = 0; q3 < s->actor_count && att == -2; q3++) {
                int al = s->actors[q3].friendly ? 0 : s->actors[q3].alliance;
                if (q3 != i && s->armed[q3] && !s->units[q3].destroyed && s->ai_who[q3] == i && al != self_al) att = q3;
            }
            if (att != -2 && s->ai_focus[i] != att) { s->ai_focus[i] = att; m->prev_state = m->state; m->state = AI_TARGET; m->target = 1; ai_wake(s, i); }
        }
        if (s->ai_focus && s->ai_focus[i] >= 0 && (!s->armed[s->ai_focus[i]] || s->units[s->ai_focus[i]].destroyed)) s->ai_focus[i] = -2;
        if (s->ai_focus && s->ai_focus[i] == -1 && s->player_unit.destroyed) s->ai_focus[i] = -2;
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
            v.commanded = s->cmd && s->cmd[i] != 0;
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
            v.contact = (s->bump_with && s->bump_with[i]) || s->units[i].blocked;   /* controller +0xa4 */
            v.out_of_weapons = combat_out_of_weapons(&s->units[i]);                  /* 0x10020880 */
        }
        v.can_jump = s->units[i].jets > 0 && !s->units[i].no_jump && s->units[i].jet_fuel > 5.0f;
        v.height = s->units[i].y - s->units[i].ground;   /* above the terrain (ai.h): y is absolute - on raised ground the
                                                          * jump manoeuvres never saw a landing */
        v.pilot_level = ac->ai_level >= 1 && ac->ai_level <= 4 ? ac->ai_level : 1;   /* 0x1001ee30: outside 1-4 -> 1 */
        v.weapon_range = combat_max_range(&s->units[i]);
        v.is_leader = ac->leader;
        dx = v.enemy_x - (float)ac->mech.origin[0];
        dz = v.enemy_z - (float)ac->mech.origin[2];
        v.enemy_dist = sqrtf(dx * dx + dz * dz);
        v.enemy_bearing = atan2f(dx, dz) * 180.0f / 3.14159265f;
        /* elevation between the unit origins (the pitch demand, 0x100231a0 -> 0x100135d0) */
        v.enemy_elev = atan2f(v.enemy_height + s->units[i].ground - s->units[i].y, v.enemy_dist > 1.0f ? v.enemy_dist : 1.0f) * 180.0f / 3.14159265f;
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
                if (mt >= 0 && mt < s->actor_count && s->armed[mt] && s->units[mt].destroyed) {
                    msim_order_message(s, star_point(s, i), 5);
                    if (c == MSIM_CMD_ATTACK && mt == t) {
                        if (m->state == AI_TARGET) msim_order_message(s, star_point(s, i), 10);
                        s->cmd[i] = c = MSIM_CMD_JOIN; s->cmd_target[i] = t = -1;
                        m->state = AI_FOLLOW; m->target = 0;
                    } else if (!c && (m->state == AI_TARGET || m->state == AI_ATTACK)) { m->state = AI_FOLLOW; m->target = 0; }
                    s->mate_target[i] = -1;
                }
                if (c == MSIM_CMD_ATTACK && t >= 0 && t < s->actor_count && s->units[t].destroyed) {   /* killed by someone else */
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
                    const world_actor *e = &s->actors[t];
                    v.has_enemy = 1;
                    v.enemy_x = (float)e->mech.origin[0]; v.enemy_z = (float)e->mech.origin[2];
                    v.enemy_heading = e->mech.heading; v.enemy_height = s->units[t].y - s->units[i].ground;
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
                    /* engine 0x10013f00: four patrol points at x +- R, z +- R around the defended mech, R = its reference
                     * range (0x10013450: an object = 100 m); DEFLT state 7 re-enters on reaching one (cond 3, ref 0x2a),
                     * rebuilding them round the mech's new position. No engagement: condition 7 is blocked while
                     * commanded and the group assignment skips commanded units */
                    static const float PX[4] = {1, 0, -1, 0}, PZ[4] = {0, 1, 0, -1};
                    int leg = s->defend_leg[i] & 3;
                    float ddx, ddz;
                    tx = (float)s->actors[t].mech.origin[0] + PX[leg] * 10000.0f;
                    tz = (float)s->actors[t].mech.origin[2] + PZ[leg] * 10000.0f;
                    ddx = tx - (float)ac->mech.origin[0]; ddz = tz - (float)ac->mech.origin[2];
                    if (ddx * ddx + ddz * ddz < 3000.0f * 3000.0f) {
                        s->defend_leg[i] = 4 | ((leg + 1) & 3);
                        tx = (float)s->actors[t].mech.origin[0] + PX[(leg + 1) & 3] * 10000.0f;
                        tz = (float)s->actors[t].mech.origin[2] + PZ[(leg + 1) & 3] * 10000.0f;
                    }
                    r = 3000.0f; ok = 1;
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
            if (m->state == AI_IDLE || m->state == AI_PATROL) m->state = AI_FOLLOW;
        } else if (cur >= 0) {
            const mtbl_node *n = &s->logic.tables[ac->table].nodes[cur];
            float tx, tz, r;
            if (strcasecmp(n->target, "UserStar") == 0 && !s->player_unit.destroyed) {
                /* attack order: the goal is the player */
                v.has_goal = 1; v.goal_radius = 10000.0f; v.goal_dist = v.enemy_dist; v.goal_bearing = v.enemy_bearing;
            } else if (target_pos(s, n->target, &tx, &tz, &r) == 0) {
                dx = tx - (float)ac->mech.origin[0]; dz = tz - (float)ac->mech.origin[2];
                v.has_goal = 1; v.goal_radius = r;
                v.goal_dist = sqrtf(dx * dx + dz * dz); v.goal_bearing = atan2f(dx, dz) * 180.0f / 3.14159265f;
                if (m->state == AI_IDLE && n->kind == MTBL_K_REACH) m->state = AI_PATROL;
            }
        }
    star_done:
        v.speed_kmh = fabsf(m->speed) * 0.036f;   /* actual speed */
        if (s->powered_down && s->powered_down[i]) { m->throttle = 0; m->turn = 0; m->wants_fire = 0; }   /* shut down: no rules run */
        else ai_step(&s->ai, m, &v, dt);
        if (getenv("MW2_TEST_HOLD_ACTOR") && atoi(getenv("MW2_TEST_HOLD_ACTOR")) == i) {   /* TEST hook: this unit stands still
                                                                                               * (no walking, turning, jets, dodges or fire) */
            m->throttle = 0; m->turn = 0; m->jet = 0; m->jet_forward = 0; m->jet_side = 0; m->wants_fire = 0; m->reverse = 0;
        }
        if (s->ai_who) s->ai_who[i] = m->state == AI_ATTACK && v.has_enemy ? enemy_who : -2;
        if (s->cmd && s->cmd[i] == MSIM_CMD_SHUTDOWN) { m->state = AI_SHUTDOWN; m->throttle = 0; m->turn = 0; m->wants_fire = 0; m->target = 0; }
        /* action 4 (0x100141e0 -> 0x10014270): (condition, the pick) to the leader - itself when it leads - if it has none
         * pending; the player's star posts to the player (its assignment runs at the top of the next step) */
        if (m->post_cond && in_star) { if (!s->star_post) s->star_post = m->post_who + 2; }
        else if (m->post_cond) {
            ai_mind *lm = l ? &s->minds[l - s->actors] : m;
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
        if (fixed_unit(ac)) { m->turn = 0; m->jet = 0; m->jet_forward = 0; m->jet_side = 0; }
        if (s->online_at && s->now < s->online_at[i]) { m->throttle = 0; m->turn = 0; m->wants_fire = 0; m->jet = 0; m->jet_forward = 0; }   /* still starting up: no jets either (0x1001a180) */
        if (s->avoid_turn && m->state != AI_ATTACK) {   /* 0x100201c0 runs in states 2, 4-8, not 3 */
            float gb = m->state == AI_ATTACK || m->state == AI_TARGET ? v.enemy_bearing : (v.has_goal ? v.goal_bearing : ac->mech.heading);
            avoid_obstacles(s, i, m, gb, dt);
        }
        {   /* aim servos (0x1003b7e0, see ai.h): twist T = 0.6 s x 181 = 108.6 ticks toward the demand clamped to the MGEO
             * limit (0x1001a180), pitch T = 0.2 s = 36.2 ticks; value += dt x (target - value) / T, no overshoot */
            float lim = s->twist_limit ? s->twist_limit[i] : 90.0f, tw = m->twist_demand, ticks = dt * 182.0f;
            float kt = ticks / 108.6f, kp = ticks / 36.2f;
            if (lim < 360.0f) { if (tw > lim) tw = lim; if (tw < -lim) tw = -lim; }
            m->twist += (tw - m->twist) * (kt < 1.0f ? kt : 1.0f);
            m->pitch += (m->pitch_demand - m->pitch) * (kp < 1.0f ? kp : 1.0f);
            ac->mech.twist = m->twist;   /* drawn, and the weapon mounts follow it */
        }
        {
            float kmh, rate, h, spd, top = ac->speed * s->units[i].speed_gain;
            /* the throttle -> speed chain (msim_drive_step: servos +0x44 / +0x24 and the velocity approach, 0x1001a180 /
             * 0x100190d0), the player's rule; reverse (input +0x2f, manoeuvres 8 / 10): the drive x -0.5 at once, the
             * throttle itself stays positive. AI mechs get no 12.5% turning creep (that is for object +0x10 != 2, the
             * player); 0x1001f910's 10% is set by ai.c */
            msim_drive_step(&m->thr_servo, &m->spd_servo, &m->speed, m->throttle, m->reverse ? -0.5f : 1.0f, top,
                            !(s->units[i].shutdown || s->units[i].immobile), m->drive_slow, dt * 182.0f);
            kmh = fabsf(m->spd_servo) * 0.036f;
            rate = m->turn / 1024.0f * 90.0f * cosf((kmh < 80.0f ? kmh : 80.0f) * 3.14159265f / 180.0f);   /* by c[0xb] x 6.516 (0x1001a180) */
            if (m->jet && s->units[i].jet_fuel >= 1.0f) rate = m->turn / 1024.0f * 90.0f * 2.0f;   /* jetting: x2 (0x1001a180) */
            ac->mech.heading = wrap180(ac->mech.heading + rate * dt);
            spd = m->speed;
            s->units[i].throttle_frac = fabsf(m->thr_servo) > 1.0f ? 1.0f : fabsf(m->thr_servo);   /* walking heat: c[0x13] - 1/64 (0x1001aac8) */
            if (s->units[i].y > s->units[i].ground + 1.0f) spd = s->air_speed ? s->air_speed[i] : spd;     /* airborne (y is absolute: above the ground): keep momentum */
            {
                float hv = spd / 182.0f;                                          /* cm/tick */
                int thrust = m->jet && (m->jet_forward || m->jet_side);           /* +0x20 forward, +0x1e / +0x1f sideways */
                combat_jets_forward(&s->units[i], thrust, dt, &hv);
                if (thrust) spd = hv * 182.0f;
                if (s->air_speed) s->air_speed[i] = spd;
            }
            h = (ac->mech.heading + (m->jet_side && s->units[i].y > s->units[i].ground + 1.0f ? 90.0f * (float)m->jet_side : 0.0f)) * 3.14159265f / 180.0f;
            {
                float nx = (float)ac->mech.origin[0] + sinf(h) * spd * dt, nz = (float)ac->mech.origin[2] + cosf(h) * spd * dt;
                {   /* mech-mech contact, in 3D (engine 0x1000b5e0 -> 0x1000ba20 spheres, 0x10019310): the unit touched, the
                     * normal from its centre to this one's, the relative speed. On top of it (normal y above 0.7071) with
                     * the feet within 10 m of the terrain: landed - no damage to either, the mover set down beside it (the
                     * engine stands it on the terrain there and pushes it out of the sphere the next tick); else this one
                     * takes collision damage by the normal (0x1000c160 / 0x1000c1f0: legs from below, head from above x
                     * the tonnage ratio, else arm / torso) on every contact tick - only the mover: the other takes damage
                     * only from its own movement's contacts (0x10019d77) - and a clang once per contact (on_sound -1). Airborne, it bounces off along the normal at half the relative speed (0x1000ba20
                     * +0xf4 = n x max(1, dv / 2)); walking, it is pushed back 60 cm at -30 % speed. */
                    float x0 = (float)ac->mech.origin[0], z0 = (float)ac->mech.origin[2], ticks = dt * 182.0f;
                    float ch = s->centre_h ? s->centre_h[i] : 550.0f, air = s->units[i].y > s->units[i].ground + 1.0f;
                    float vy = air ? s->units[i].vy : 0.0f;
                    float p0[3] = {x0, s->units[i].y + ch, z0}, p1[3] = {nx, s->units[i].y + ch + vy * ticks, nz};
                    float n[3], c[3], rr = 0, ov[3], sv[3] = {sinf(h) * spd / 182.0f, vy, cosf(h) * spd / 182.0f};
                    int hit = msim_unit_contact(s, i, p0, p1, n, c, &rr, ov);
                    if (hit != -2) {
                        /* the engine's speeds (0x1000ba20): it copies the other's z velocity over its own (+0x108) before
                         * 0x1000c160 takes the difference, so the damage speed is |(dvx, dvy)|; the bounce is half of
                         * |(dvx, dvy, the other's vz)|, at least 1 cm/tick */
                        float dvx = sv[0] - ov[0], dvy = sv[1] - ov[1], dv = sqrtf(dvx * dvx + dvy * dvy);
                        float bs = 0.5f * sqrtf(dvx * dvx + dvy * dvy + ov[2] * ov[2]);
                        float py = c[1] + n[1] * rr - ch, hl = sqrtf(n[0] * n[0] + n[2] * n[2]);   /* projected onto its sphere */
                        float gnd = msim_ground(s, c[0] + n[0] * rr, c[2] + n[2] * rr, py);
                        if (getenv("MW2_CONTACT_TRACE")) fprintf(stderr, "contact %.2fs actor %d -> %d n %.2f %.2f %.2f dv %.2f feet %.0f above %.0f air %d\n", s->now / 1000.0, i, hit, n[0], n[1], n[2], dv, py, py - gnd, (int)air);
                        if (n[1] > 0.70710677f && py - gnd < 1000.0f && py - gnd > -10000.0f) {   /* landed on it (0x10019c4e) */
                            float dx = hl > 1e-3f ? n[0] / hl : -sinf(h), dz = hl > 1e-3f ? n[2] / hl : -cosf(h);
                            float rh = rr, gx, gz;
                            gx = c[0] + dx * (rh + 1.0f); gz = c[2] + dz * (rh + 1.0f);
                            ac->mech.origin[0] = (int32_t)lrintf(gx); ac->mech.origin[2] = (int32_t)lrintf(gz);
                            s->units[i].ground = msim_ground(s, gx, gz, s->units[i].y);
                            s->units[i].y = s->units[i].ground; s->units[i].vy = 0;
                            if (s->air_speed) s->air_speed[i] = 0;
                            m->speed = 0; spd = 0;
                            if (s->bump_with) s->bump_with[i] = hit + 2;
                        } else {
                            if (s->collision_damage)   /* every contact tick, the mover only (0x10019d6a -> 0x1000c160) */
                                combat_collision_unit(&s->units[i], dv, n, ac->mech.heading + m->twist, hit == -1 ? s->player_unit.tons : s->units[hit].tons, &s->rng);
                            if (s->bump_with && s->bump_with[i] != hit + 2) {
                                if (s->on_sound && dv > 0.5f) { float p3[3] = {(x0 + c[0]) * 0.5f, s->units[i].y, (z0 + c[2]) * 0.5f}; s->on_sound(s->shot_user, -1, p3); }
                                s->bump_with[i] = hit + 2;
                            }
                            if (air) {   /* bounced off: out to the sphere's surface, moving away along the normal */
                                if (bs < 1.0f) bs = 1.0f;
                                float gx = c[0] + n[0] * (rr + 1.0f), gz = c[2] + n[2] * (rr + 1.0f);
                                ac->mech.origin[0] = (int32_t)lrintf(gx); ac->mech.origin[2] = (int32_t)lrintf(gz);
                                s->units[i].y = c[1] + n[1] * (rr + 1.0f) - ch;
                                s->units[i].ground = msim_ground(s, gx, gz, s->units[i].y);
                                if (s->units[i].y < s->units[i].ground) s->units[i].y = s->units[i].ground;
                                s->units[i].vy = n[1] * bs;
                                spd = (n[0] * sinf(h) + n[2] * cosf(h)) * bs * 182.0f;   /* along the heading */
                                if (s->air_speed) s->air_speed[i] = spd;
                            } else {
                                float bx = x0 - c[0], bz = z0 - c[2], bl = sqrtf(bx * bx + bz * bz);
                                if (bl > 1.0f) { ac->mech.origin[0] += (int32_t)lrintf(bx / bl * 60.0f); ac->mech.origin[2] += (int32_t)lrintf(bz / bl * 60.0f); }
                                m->speed *= -0.3f;   /* bounced back, as the player (glview) */
                            }
                        }
                        nx = (float)ac->mech.origin[0]; nz = (float)ac->mech.origin[2];
                    } else if (s->bump_with) s->bump_with[i] = 0;
                }
                if (s->units[i].y > s->units[i].ground + 1.0f || msim_can_step_hr(s, (float)ac->mech.origin[0], (float)ac->mech.origin[2], nx, nz, s->units[i].y, s->centre_h[i], s->radius[i])) {   /* airborne: no ground test */
                    /* the contact count (controller +0xa4, 0x1000b5e0) clears on a free move; a unit that does not move
                     * keeps it (and counts on) */
                    if ((int32_t)lrintf(nx) != ac->mech.origin[0] || (int32_t)lrintf(nz) != ac->mech.origin[2]) s->units[i].blocked = 0;
                    ac->mech.origin[0] = (int32_t)lrintf(nx);
                    ac->mech.origin[2] = (int32_t)lrintf(nz);
                } else {   /* blocked by the terrain: collision damage at speed (engine 0x1000c3c0) */
                    float wn[3] = {g_block_n[0], g_block_n[1], g_block_n[2]};   /* the refusing surface's normal */
                    if (s->collision_damage)   /* Combat Variables: a global switch in the engine */
                        combat_collision(&s->units[i], spd / 182.0f, wn[1], atan2f(-wn[0], -wn[2]) * 57.29578f - ac->mech.heading, &s->rng);
                    /* the contact the AI reads (+0xa4, 0x10020e00) counts whatever the Collision Damage setting: 0x1000b5e0
                     * adds one for every refused move (0x10010530 hit), a mech or a sphere in the way */
                    s->units[i].blocked = 1;
                    if (getenv("MW2_BLOCK_TRACE") && atoi(getenv("MW2_BLOCK_TRACE")) == i)   /* TEST ONLY */
                        fprintf(stderr, "block %.2fs actor %d at %d,%d by %s n %.2f %.2f %.2f\n", s->now / 1000.0, i, ac->mech.origin[0], ac->mech.origin[2],
                                g_block_what ? g_block_what : "terrain", wn[0], wn[1], wn[2]);
                }
            }
            if (fabsf(spd) > 1.0f && s->units[i].y <= s->units[i].ground + 1.0f) {   /* on the ground (y is absolute - the old y <= 0 test froze every
                                                                               * stride on terrain above zero); engine key timing: TSK duration x gait factor (0x1000e600) */
                static const float MOD[3] = {1.5f, 1.0f, 0.75f};
                float fr = fabsf(m->thr_servo);   /* gait by the current throttle c[0x13] (0x1000b3a0: param_1[8] + 0x4c) */
                int g = fr < 0.25f ? 0 : fr < 0.75f ? 1 : 2;
                {   int r0 = ac->mech.anim_rate > 0 ? ac->mech.anim_rate : 70;   /* key time 0x1000e600 (integer) */
                    ac->t += dt * 182.0f / (float)(g == 0 ? r0 + (r0 >> 1) : g == 1 ? r0 : r0 - (r0 >> 2)); (void)MOD; }
            }
            if (fixed_unit(ac)) s->units[i].ground = s->units[i].y;   /* 0x100219e0: no translation, no gravity */
            else {
                s->units[i].ground = msim_ground(s, (float)ac->mech.origin[0], (float)ac->mech.origin[2], s->units[i].y);
                combat_jets(&s->units[i], m->jet, dt, &s->rng);           /* jump jets / falling, on the terrain */
                if (s->units[i].landed_vy < -5.384f && !s->units[i].destroyed && s->on_sound) {   /* the touch-down (0x10019e28 ->
                                                                                                    * 0x1001c0a0): MECMTNSF 0xe6 up to 16.154 cm/tick, MECMTNHD 0xe5 faster, at the mech */
                    float p3[3] = {(float)ac->mech.origin[0], s->units[i].y, (float)ac->mech.origin[2]};
                    s->on_sound(s->shot_user, s->units[i].landed_vy >= -16.154f ? 0xe6 : 0xe5, p3);
                }
            }
            if (m->jet && s->on_effect) msim_jet_flames_at(s, &ac->mech, (float)ac->mech.origin[0], s->units[i].y, (float)ac->mech.origin[2], ac->mech.heading);
            ac->mech.origin[1] = (int32_t)s->units[i].y;
        }
        if (getenv("MW2_TEST_LEG") && s->now >= atoi(getenv("MW2_TEST_LEG")) && s->armed[i] && !s->units[i].loc_gone[7]) {   /* tests: the left leg shot off */
            int q; for (q = 0; q < 80 && !s->units[i].loc_gone[7]; q++) combat_hit(&s->units[i], 11, 8, 180.0f, &s->rng);
        }
        if (getenv("MW2_AI_TRACE") && i == atoi(getenv("MW2_AI_TRACE")) && (s->now / dt_ms) % 10 == 0) {   /* tests: one AI every 10 steps */
            fprintf(stderr, "ai %.2fs actor %d at %d,%d y %.0f immobile %d heading %.1f turn %.0f avoid %.0f/%d goal %.1f\n", s->now / 1000.0, i, ac->mech.origin[0], ac->mech.origin[2], s->units[i].y, s->units[i].immobile,
                    ac->mech.heading, m->turn, s->avoid_turn ? s->avoid_turn[i] : 0.0f, s->avoid_side ? s->avoid_side[i] : 0, v.has_goal ? v.goal_bearing : -999.0f);
            fprintf(stderr, "ai %.2fs actor %d %s man %d (prev %d) thr %.2f%s speed %.0f twist %.1f->%.1f pitch %.1f->%.1f lock %#x tgt %d fire %d\n",
                    s->now / 1000.0, i, ai_state_name(m->state), m->man, m->prev_man, m->throttle, m->reverse ? " REV" : "", m->speed,
                    m->twist, m->twist_demand, m->pitch, m->pitch_demand, m->lock, m->tgt, m->wants_fire);
        }
    }

    /* pose everyone: gait by throttle (engine 0x1000b3a0: < 25% seq 0, 25-75% seq 1, >= 75% seq 2) */
    for (i = 0; i < s->actor_count; i++) {
        world_actor *ac = &s->actors[i];
        int first = 0, cnt = 12;
        /* no walk animation (vehicles, turrets, dropships): the rest pose, placed at the unit's position and heading -
         * the model drawn and hit is where the unit is */
        if (!ac->have_anim) { mech3d_pose(&ac->mech, NULL, 0); continue; }
        /* destroyed (state 4): the mech stands as it was struck (ASSUMED: the stride stops - the engine's state 4 runs
         * no gait; DOSBox shows the self-destructed player's mech standing) */
        if (s->armed[i] && s->units[i].destroyed) continue;
        {
            /* by the current throttle c[0x13] (0x1000b3a0); reversing (input +0x2f, ai_mind.reverse) -> sequence 2 */
            float thr = s->minds ? (s->minds[i].reverse ? -s->minds[i].thr_servo : s->minds[i].thr_servo) : 0;
            int seq;
            if (s->minds && s->minds[i].reverse && thr > -0.02f && s->minds[i].throttle > 0) thr = -0.02f;
            seq = thr < -0.01f ? 2 : thr < 0.75f ? 0 : 1;   /* engine 0x1000b3a0: the run from gait 3 (3/4 up); 2 = reverse walk */
            if (anim_sequence(&ac->anim, seq, &first, &cnt) != 0) { seq = 0; anim_sequence(&ac->anim, 0, &first, &cnt); }
            if (s->anim_seq && s->anim_seq[i] != seq + 1) { s->anim_seq[i] = seq + 1; ac->t = 0; }   /* a new sequence from its start (as the player's) */
        }
        {   /* the stride loops left - right - left (anim_loop_range); the lead-in plays once */
            int ls, le;
            anim_loop_range(&ac->anim, first, cnt, &ls, &le);
            if (ac->t >= (float)le) ac->t = (float)ls + fmodf(ac->t - (float)le, (float)(le - ls > 0 ? le - ls : 1));
        }
        if (s->step_key && !s->units[i].destroyed) {   /* footsteps (0x1000e600 / 0x1000b3a0): a key flagged 0x800 -> 0x103 at the mech */
            int key = first + (int)ac->t;
            if (key != s->step_key[i] && key >= 0 && key < ac->anim.key_count && (ac->anim.key_flags[key] & 0x800) && s->on_sound) {
                float p3[3] = {(float)ac->mech.origin[0], s->units[i].y, (float)ac->mech.origin[2]};
                s->on_sound(s->shot_user, 0x103, p3);
            }
            s->step_key[i] = key;
        }
        if (s->minds) {   /* a key flagged 0x10 (object +0x80 bit 2): the drive servo's T becomes 90.5 ticks for good (0x1001a180) */
            int key = first + (int)ac->t;
            if (key >= 0 && key < ac->anim.key_count && (ac->anim.key_flags[key] & 0x10) && fabsf(s->minds[i].speed) > 1.0f) s->minds[i].drive_slow = 1;
        }
        if (getenv("MW2_ANIM_TRACE") && i == atoi(getenv("MW2_ANIM_TRACE")))   /* tests: one actor every step */
            fprintf(stderr, "anim %.2fs actor %d vy %.2f fuel %.0f jet %d y %.0f ground %.0f throttle %.2f key %.2f (first %d)\n", s->now / 1000.0, i, s->units[i].vy, s->units[i].jet_fuel, s->minds ? s->minds[i].jet : -1, s->units[i].y, s->units[i].ground, s->minds ? s->minds[i].throttle : 0.0f, ac->t, first);
        mech3d_pose(&ac->mech, &ac->anim, (float)first + ac->t);
    }
}

void msim_free(msim *s)
{
    free(s->units);
    free(s->armed);
    free(s->height);
    free(s->wreck_until); s->wreck_until = NULL;
    free(s->twist_limit);
    free(s->minds);
    free(s->cmd); free(s->online_at); free(s->radius); free(s->fly_seen); free(s->centre_h); free(s->ai_focus); free(s->ai_who); free(s->powered_down); free(s->seen_node); free(s->cmd_target); free(s->engage); free(s->mate_target); free(s->defend_leg);
    free(s->bld);
    free(s->inspected);
    free(s->ai_lock);
    free(s->provoked);
    free(s->step_key);
    free(s->anim_seq);
    free(s->bump_with);
    s->minds = NULL;
    free(s->air_speed); s->air_speed = NULL;
    free(s->obstacle_part); s->obstacle_part = NULL; s->world = NULL;
    world3d_paths_free(s->paths); s->paths = NULL;
    terrain_release(s->terrain); s->terrain = NULL;
    free(s->obstacles); free(s->avoid_turn); free(s->avoid_thr); free(s->avoid_next); free(s->avoid_side);
    s->obstacles = NULL; s->avoid_turn = s->avoid_thr = s->avoid_next = NULL; s->avoid_side = NULL;
    s->twist_limit = NULL;
    if (s->pl_mech_ok) mech3d_free(&s->pl_mech);
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


/* the volley's muzzle flashes at the weapon (engine 0x10043aa8: the weapon table's two muzzle types) */
static void muzzle(msim *s, int weapon, const float from[3])
{
    int k;
    if (!s->on_effect) return;
    for (k = 0; k < 2; k++) {
        int t = fx_muzzle(weapon, k);
        if (t >= 0) s->on_effect(s->shot_user, t, from);
    }
}

/* weapon table +0x18 (3dfx MW2.DLL 0x1025a6d8 + 0x58 i): LRM 20/15/10/5, Streak SRM-6/4/2, Narc */
static const unsigned char GUIDED[31] = {1,1,1,1, 0,0,0, 1,1,1, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 1, 0,0};
int msim_weapon_guided(int w) { return w >= 0 && w < 31 && GUIDED[w]; }
/* weapon table +0x3c / +0x40: the lock's range window, cm */
static const int LOCK_MIN[31] = {7500,7500,7500,7500, 0,0,0, 25,25,25, 0,0,0,0,0,0,0,0,0,0,1000,0,0,0,0,0,0,0, 0, 0,0};
static void car_inc(msim *s, int off);
static void spawn_shot_h(msim *s, int weapon, int owner, const float from[3], const float to[3], float scatter, int homing);
static void spawn_shot_h(msim *s, int weapon, int owner, const float from[3], const float to[3], float scatter, int homing)
{
    msim_shot *sh;
    float d[3], len, sp = combat_weapon_speed(weapon);
    if (s->shot_count >= MSIM_MAX_SHOTS || sp <= 0) return;
    sh = &s->shots[s->shot_count++];
    d[0] = to[0] - from[0] + (frand_ms(&s->rng) - 0.5f) * 2.0f * scatter;
    d[1] = to[1] - from[1] + (frand_ms(&s->rng) - 0.5f) * 2.0f * scatter;
    d[2] = to[2] - from[2] + (frand_ms(&s->rng) - 0.5f) * 2.0f * scatter;
    len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1) len = 1;
    sh->x = from[0]; sh->y = from[1]; sh->z = from[2];
    sh->vx = d[0] / len * sp; sh->vy = d[1] / len * sp; sh->vz = d[2] / len * sp;
    sh->life = (float)combat_weapon_life(weapon);
    sh->weapon = weapon;
    sh->owner = owner;
    if (owner == -1) car_inc(s, 0x13);   /* the player's shots */
    sh->ay = -(float)combat_weapon_drop(weapon) * 0.0296f * s->planet.gravity_g;   /* 0x10043cc0: +0x28 x gravity */
    sh->homing = msim_weapon_guided(weapon) ? homing : -2;   /* 0x100437a0: the mech's target when guided and locked */
    sh->age = 0;
    sh->prox = 0;
    sh->id = ++s->shot_serial;
    if (owner == -1) s->player_last_shot = sh->id;   /* 0x10044xxx: DAT_1025a69c (kept only for a missile) */
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
    return (n->kind == MTBL_K_DESTROY1 || n->kind == MTBL_K_DESTROY2) && strcasecmp(n->target, "UserStar") == 0;
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
        while (changed) {
            changed = 0;
            for (q = 0; q < s->world->part_count; q++) {
                mech3d_part *c = (mech3d_part *)&s->world->parts[q];
                int j, parent_shown = 0;
                if (!c->hidden || c->parent_obj < 0 || strcmp(c->rec, pd->rec) != 0 || c->gt_debris == 2 || (c->sparse && world3d_density_low)) continue;
                for (j = 0; j < s->world->part_count; j++)
                    if (s->world->parts[j].obj_index == c->parent_obj && strcmp(s->world->parts[j].rec, c->rec) == 0) {
                        parent_shown = !s->world->parts[j].hidden || s->world->parts[j].gt_debris == 2;
                        break;
                    }
                if (!parent_shown || j == bd->intact) continue;
                if (c->gt_debris) {
                    c->gt_debris = 2;   /* spent: launched (or dropped) once */
                    if (s->chunky && s->debris_count < MSIM_MAX_DEBRIS && c->model.object_count > 0) {
                        msim_debris *dd = &s->debris[s->debris_count++];
                        int k;
                        dd->kind = 3; dd->actor = -2; dd->part = q;
                        for (k = 0; k < 3; k++) dd->p[k] = (float)c->pos[k];
                        dd->v[0] = gauss(&s->rng) * 5.5f;
                        dd->v[1] = (gauss(&s->rng) + 1.0f) * 5.5f;
                        dd->v[2] = gauss(&s->rng) * 5.5f;
                        for (k = 0; k < 3; k++) { dd->ang[k] = 0; dd->spin[k] = gauss(&s->rng) * 0.5f; }
                        dd->until = now + 0xe24;
                        dd->moving = 1;
                    }
                } else c->hidden = 0;
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
    int changed = 1;
    (void)owner;
    if (bd->hp <= 0) return;
    if (getenv("MW2_BLD_TRACE")) fprintf(stderr, "bld hit %s (%s) %.1f -> hp %d at %.0f,%.0f,%.0f by %d\n", bd->name, bd->sub, (double)dmg, bd->hp - (int)dmg, at ? (double)at[0] : 0.0, at ? (double)at[1] : 0.0, at ? (double)at[2] : 0.0, owner);
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
        if ((bd->type & 0xf0) == 0xb0) { if (s->on_effect) s->on_effect(s->shot_user, 0x0d, p); }
        else { effect(s, 3, p, 1); spawn_debris(s, 4, p); }   /* 0x20b: (0x200 >> 8) & 2 -> four chunks */
    }
    s->buildings_destroyed++;
    s->world_dirty = 1;
    s->world_rebuild = 1;                        /* the collision surfaces without it: rebuilt once per step */
    snprintf(line, sizeof line, "%6.1fs %s (%s) destroyed", s->now / 1000.0, bd->name, bd->sub);
    if (s->log) s->log(s->log_user, line);
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

int msim_inspect(msim *s, int actor, int thing)
{
    float dx, dz, r;
    if (actor >= 0 && actor < s->actor_count) {
        if (!s->inspected || s->inspected[actor]) return 0;
        dx = (float)s->actors[actor].mech.origin[0] - s->player[0]; dz = (float)s->actors[actor].mech.origin[2] - s->player[2];
        r = (s->height ? s->height[actor] * 0.5f : 500.0f) + 20000.0f;   /* the mech's radius (+0xe8, ASSUMED half its height) + 200 m */
        if (dx * dx + dz * dz > r * r) return 2;
        s->inspected[actor] = 1;
        return 1;
    }
    if (thing >= 0 && thing < s->bld_count) {
        struct msim_building *g = &s->bld[thing];
        float hx = (g->mx[0] - g->mn[0]) * 0.5f, hz = (g->mx[2] - g->mn[2]) * 0.5f;
        if (g->inspected) return 0;
        dx = (g->mn[0] + g->mx[0]) * 0.5f - s->player[0]; dz = (g->mn[2] + g->mx[2]) * 0.5f - s->player[2];
        r = sqrtf(hx * hx + hz * hz) + 20000.0f;                           /* its radius (half the box diagonal, ASSUMED) + 200 m */
        if (dx * dx + dz * dz > r * r) return 2;
        g->inspected = 1;
        return 1;
    }
    return 0;
}

void msim_damage_building(msim *s, int b, float dmg) { float p[3] = {0, 0, 0}; if (b >= 0 && b < s->bld_count) building_damage(s, b, dmg, -1, p); }

/* a 50 m sidestep (0x10020ac0 -> 0x100204e0 probes): clear of obstacles and mechs */
static int probe_hits(const msim *s, float x0, float z0, float x1, float z1);
static int mech_probe(const msim *s, int self, float x0, float z0, float x1, float z1, float *hx, float *hz);
struct side_ctx { const msim *s; int i; };
static int side_free(void *ctx, int side)
{
    const struct side_ctx *c = ctx;
    const world_actor *ac = &c->s->actors[c->i];
    float h = (ac->mech.heading + 90.0f * (float)side) * 3.14159265f / 180.0f, hx, hz;
    float x0 = (float)ac->mech.origin[0], z0 = (float)ac->mech.origin[2], x1 = x0 + sinf(h) * 5000.0f, z1 = z0 + cosf(h) * 5000.0f;
    return !probe_hits(c->s, x0, z0, x1, z1) && !mech_probe(c->s, c->i, x0, z0, x1, z1, &hx, &hz);
}
/* a guided launch at AI mech t by a shooter of pilot level lv (the player 1): engine 0x10020ac0 */
static void missile_warning(msim *s, int t, int lv)
{
    ai_view v;
    struct side_ctx c;
    if (!s->minds || !s->ai_ok || t < 0 || t >= s->actor_count || !s->armed[t] || s->units[t].destroyed) return;
    memset(&v, 0, sizeof v);
    v.can_jump = s->units[t].jets > 0 && !s->units[t].no_jump && s->units[t].jet_fuel > 5.0f;
    v.heat = s->units[t].heat;
    v.pilot_level = s->actors[t].ai_level >= 1 && s->actors[t].ai_level <= 4 ? s->actors[t].ai_level : 1;
    c.s = s; c.i = t;
    ai_missile_warning(&s->minds[t], &v, lv, side_free, &c);
}

static int shot_objects(msim *s, const float o[3], const float dv[3], float *tb, int *part);
static int shot_thing(const msim *s, int part);
static void combat_step(msim *s, float dt)
{
    int i, k, w;
    float ticks = dt * COMBAT_TICKS_PER_S;
    char line[200];
    const float pl_h = 950.0f;          /* Timber Wolf muzzle / aim height, cm (approx.) */

    /* the player's collision model follows the player */
    if (s->pl_mech_ok) {
        s->pl_mech.origin[0] = (int32_t)s->player[0];
        s->pl_mech.origin[1] = (int32_t)s->player_unit.y;
        s->pl_mech.origin[2] = (int32_t)s->player[2];
        s->pl_mech.heading = s->player_facing;
        mech3d_pose(&s->pl_mech, NULL, 0);
    }
    /* the fire routine (0x100437a0) runs from the controller only in state 2: not while starting up (states 0 / 1,
     * 0x1001a180 / 0x10021a50) or powered down - a queued volley waits, nothing is triggered */
    s->player_unit.offline = (float)s->now < s->player_online_at && !getenv("MW2_NO_STARTUP");   /* the front end's test hook */
    if (!s->player_unit.offline && !s->player_unit.shutdown) combat_damage_live = 1;   /* DAT_1024c570: the player's first start-up done (0x1001ac68) */
    for (i = 0; i < s->actor_count; i++)
        s->units[i].offline = (s->online_at && (float)s->now < s->online_at[i]) || (s->powered_down && s->powered_down[i]);
    combat_tick(&s->player_unit, dt);
    for (i = 0; i < s->actor_count; i++) if (s->armed[i]) combat_tick(&s->units[i], dt);
    /* the heat restart sets state 0 (0x10016201), which runs the start-up again (0x1001ab71: online 0x43e + rand(0x16a)
     * ticks later; no throttle, turning, jets or weapons until then) */
    if (s->player_unit.restarted) {
        unsigned d = 0x43e + rnd_ms(&s->rng) % 0x16a;
        s->player_online_at = (float)s->now + (float)d * 1000.0f / 182.0f;
        s->player_unit.seq_ticks = -(float)d;   /* state 0 sets c[0x23] to the online time (0x1001ab71): the overheat timer counts from it */
    }
    for (i = 0; s->online_at && i < s->actor_count; i++)
        if (s->armed[i] && s->units[i].restarted) s->online_at[i] = (float)s->now + (float)(0x43e + rnd_ms(&s->rng) % 0x16a) * 1000.0f / 182.0f;

    /* triggers and launches: the AI's fire decisions */
    for (i = 0; i < s->actor_count; i++) {
        float dx, dz, from[3], to[3], ex = 0, ez = 0, ey = 0, ef = 0, eh = 0, aim_h;
        int who = -1;
        ai_mind *m = s->minds && s->ai_ok ? &s->minds[i] : NULL;
        if (!s->armed[i] || s->units[i].destroyed || !pick_enemy(s, i, &ex, &ez, &ey, &ef, &eh, &who)) continue;
        (void)ef;
        aim_h = who < 0 ? s->player_unit.y + pl_h * 0.55f : ey + eh * 0.55f;
        dx = ex - (float)s->actors[i].mech.origin[0];
        dz = ez - (float)s->actors[i].mech.origin[2];
        from[0] = (float)s->actors[i].mech.origin[0]; from[1] = s->units[i].y + s->height[i] * 0.7f; from[2] = (float)s->actors[i].mech.origin[2];
        if (m) {   /* AI missile lock (0x10044560, every live mech, for the AI's selected weapon +0xac = combat_unit.ai_sel):
                    * guided, origin distance (3D) in the weapon's window (+0x3c < d <= +0x40), within 16 degrees of the aim
                    * (bearing off heading + lagged twist; pitch + elevation): 0x8000 on target; 0x40 + a 0x16a-tick timer
                    * when first on, counted down by dt, locked (0x80) at <= 0. Off target: 0x8000 / 0x80 cleared and the
                    * timer counts back UP by dt, 0x40 cleared at 0x16a. No guided weapon selected: all cleared. */
            int sw = s->units[i].ai_sel, wid = sw >= 0 && sw < s->units[i].weapon_count ? s->units[i].weapons[sw].weapon : -1;
            m->lock &= ~0x80;
            if (wid >= 0 && msim_weapon_guided(wid)) {
                float dy = ey - s->units[i].y, d3 = sqrtf(dx * dx + dz * dz + dy * dy), on = 0;
                if (d3 > (float)LOCK_MIN[wid] && d3 <= combat_weapon_range(wid)) {
                    float yaw = wrap180(atan2f(dx, dz) * 180.0f / 3.14159265f - s->actors[i].mech.heading - m->twist);
                    float el = atan2f(dy, sqrtf(dx * dx + dz * dz)) * 180.0f / 3.14159265f;
                    on = fabsf(yaw) < 16.0f && fabsf(m->pitch - el) < 16.0f;
                }
                if (on) {
                    m->lock |= 0x8000;
                    if (!(m->lock & 0x40)) { m->lock |= 0x40; m->lock_ticks = 362.0f; }
                    else if (m->lock_ticks < 1.0f) { m->lock_ticks = 0; m->lock |= 0x80; }
                    else m->lock_ticks -= ticks;
                } else {
                    m->lock &= ~(0x8000 | 0x80);
                    if (m->lock & 0x40) { m->lock_ticks += ticks; if (m->lock_ticks >= 362.0f) m->lock &= ~0x40; }
                }
            } else m->lock = 0;
        }
        if ((m ? m->wants_fire : (who < 0 && order_attacks_player(s, i))) &&
            !los_blocked(s, (float)s->actors[i].mech.origin[0], s->units[i].y + s->height[i] * 0.7f, (float)s->actors[i].mech.origin[2],
                         ex, aim_h, ez)) {
            int fw = combat_ai_fire(&s->units[i], sqrtf(dx * dx + dz * dz), ms_roll, &s->rng);   /* one weapon per decision (0x100232b0) */
            if (fw >= 0 && m) m->fire_locked = (m->lock & 0x80) != 0;   /* homing only if locked when triggered (0x100437a0) */
            /* 0x10023428: a guided weapon (weapon table +0x18) fired at the player (+0x14e == player | 0x200): ENEMYFIR
             * 0x6f, volume 0x32, not positional (pos NULL) */
            if (fw >= 0 && who < 0 && msim_weapon_guided(s->units[i].weapons[fw].weapon) && s->on_sound) s->on_sound(s->shot_user, 0x6f, NULL);
        }
        {   /* engine 0x100448e0: the shot follows the shooter's aim line (no random scatter): the legs' heading + the
             * LAGGED torso twist (clamped to MGEO) and the lagged pitch, from the aim origin; aimed at the ray's point at
             * the target's distance */
            float lim = s->twist_limit ? s->twist_limit[i] : 90.0f, rel, yaw, pit, d3;
            float tx = ex - from[0], ty = aim_h - from[1], tz = ez - from[2];
            if (m) rel = m->twist;
            else {   /* no AI programs: straight at the target within the twist limit */
                rel = wrap180(atan2f(dx, dz) * 180.0f / 3.14159265f - s->actors[i].mech.heading);
                if (lim < 360.0f) { if (rel > lim) rel = lim; if (rel < -lim) rel = -lim; }
            }
            yaw = (s->actors[i].mech.heading + rel) * 3.14159265f / 180.0f;
            pit = (m ? m->pitch : atan2f(ty, sqrtf(tx * tx + tz * tz)) * 180.0f / 3.14159265f) * 3.14159265f / 180.0f;
            d3 = sqrtf(tx * tx + ty * ty + tz * tz);
            to[0] = from[0] + sinf(yaw) * cosf(pit) * d3; to[1] = from[1] + sinf(pit) * d3; to[2] = from[2] + cosf(yaw) * cosf(pit) * d3;
        }
        {
            int first = 0;
            int loc = -1;
            while ((w = combat_next_projectile_at(&s->units[i], &first, &loc)) >= 0) {
                /* from the weapon's mount (POFO slot by its location), else the body point */
                float o[3] = {(float)s->actors[i].mech.origin[0], s->units[i].y, (float)s->actors[i].mech.origin[2]}, mf[3];
                const float *src = mech3d_mount_world(&s->actors[i].mech, mech3d_mount_slot(loc), o, s->actors[i].mech.heading, mf) == 0 ? mf : from;
                spawn_shot_h(s, w, i, src, to, 0.0f, m && m->fire_locked ? who : -2);
                if (who >= 0 && (msim_weapon_guided(w) || w == 0x15)) {   /* the target dodges (0x10020ac0 from 0x1004397b) */
                    int lv = s->actors[i].ai_level >= 1 && s->actors[i].ai_level <= 4 ? s->actors[i].ai_level : 1;
                    missile_warning(s, who, lv);
                }
                if (s->on_shot) s->on_shot(s->shot_user, w, i, first, src[0], src[2]);
                if (first) muzzle(s, w, src);
            }
        }
    }
    /* missile lock (engine 0x10044560, per tick for the selected weapon): a guided weapon, a selected target within
     * the weapon's range window (+0x3c .. +0x40) and within 16 degrees of the torso's aim both ways: on target (0x8000);
     * held for 0x16a ticks (2 s) -> locked (0x80); any break restarts it */
    {
        int e = s->player_lock_target, sw = s->player_sel_w, wid = -1, in = 0;
        s->player_lock &= ~0x80;
        if (sw >= 0 && sw < s->player_unit.weapon_count) wid = s->player_unit.weapons[sw].weapon;
        if (wid >= 0 && msim_weapon_guided(wid) && e >= 0 && e < s->actor_count && s->armed[e] && !s->units[e].destroyed && !s->player_unit.destroyed) {
            float dx = (float)s->actors[e].mech.origin[0] - s->player[0], dz = (float)s->actors[e].mech.origin[2] - s->player[2];
            float dy = (s->units[e].y + s->height[e] * 0.5f) - (s->player_unit.y + pl_h * 0.7f);
            float d = sqrtf(dx * dx + dz * dz + dy * dy), yaw, pit;
            if (d > (float)LOCK_MIN[wid] && d < combat_weapon_range(wid)) {
                yaw = atan2f(dx, dz) * 180.0f / 3.14159265f - s->player_aim_yaw;
                while (yaw > 180.0f) yaw -= 360.0f;
                while (yaw < -180.0f) yaw += 360.0f;
                pit = atan2f(dy, sqrtf(dx * dx + dz * dz)) * 180.0f / 3.14159265f - s->player_aim_pitch;
                in = fabsf(yaw) < 16.0f && fabsf(pit) < 16.0f;
            }
        }
        if (in) {
            s->player_lock |= 0x8000;
            if (!(s->player_lock & 0x40)) { s->player_lock |= 0x40; s->player_lock_ticks = 362.0f; }
            else if (s->player_lock_ticks < 1.0f) { s->player_lock_ticks = 0; s->player_lock |= 0x80; }
            else s->player_lock_ticks -= ticks;
        } else if (wid >= 0 && msim_weapon_guided(wid) && e >= 0) {
            /* off target / out of the window (0x10044560): 0x8000 and 0x80 cleared, the timer counts back up by dt and
             * 0x40 goes at 0x16a - so a brief break resumes where it was */
            s->player_lock &= ~(0x8000 | 0x80);
            if (s->player_lock & 0x40) { s->player_lock_ticks += ticks; if (s->player_lock_ticks >= 362.0f) s->player_lock &= ~0x40; }
        } else s->player_lock = 0;   /* no guided weapon or no target: everything cleared */
    }
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
                int q, hit = 0;
                if (s->player_eye_ok && !getenv("MW2_AIM_BODY")) memcpy(from, s->player_eye, sizeof from);   /* TEST ONLY: MW2_AIM_BODY the old ray */
                for (q = 0; q < s->actor_count; q++) {
                    float c[3], t, px, py, pz, rr = s->radius ? s->radius[q] : 450.0f, t0, t1, h;
                    if (!s->armed[q] || s->units[q].destroyed) continue;
                    c[0] = (float)s->actors[q].mech.origin[0] - from[0];
                    c[1] = s->units[q].y + (s->centre_h ? s->centre_h[q] : s->height[q] * 0.5f) - from[1];
                    c[2] = (float)s->actors[q].mech.origin[2] - from[2];
                    t = c[0] * dir[0] + c[1] * dir[1] + c[2] * dir[2];
                    rr = rr > s->height[q] * 0.75f ? rr : s->height[q] * 0.75f;   /* the whole mech inside */
                    px = c[0] - dir[0] * t; py = c[1] - dir[1] * t; pz = c[2] - dir[2] * t;
                    if (t <= 0 || px * px + py * py + pz * pz > rr * rr) continue;
                    h = sqrtf(rr * rr - (px * px + py * py + pz * pz));
                    t0 = t - h > 0 ? t - h : 0; t1 = t + h;
                    for (; t0 < t1 && t0 < best; t0 += 100.0f) {   /* 25 cm samples (mech3d_hull_hit: four a segment) */
                        float o2[3] = {from[0] + dir[0] * t0, from[1] + dir[1] * t0, from[2] + dir[2] * t0}, d2[3] = {dir[0] * 100.0f, dir[1] * 100.0f, dir[2] * 100.0f}, ft;
                        if (mech3d_hull_hit(&s->actors[q].mech, o2, d2, &ft)) { float th = t0 + ft * 100.0f; best = th < 2000.0f ? 2000.0f : th; hit = 1; break; }
                    }
                }
                /* nothing of class 0x300 struck: +0xb0 keeps its last value (0x10044847 / 0x10044858 skip the store) -
                 * the range stays at the last mech's (the engine's ramp toward it, 0x1003b740 on +0x98, left out) */
                if (hit && los_blocked(s, from[0], from[1], from[2], from[0] + dir[0] * best, from[1] + dir[1] * best, from[2] + dir[2] * best)) hit = 0;   /* the ground nearer */
                if (hit) s->player_conv = best;
                else best = s->player_conv > 0 ? s->player_conv : 150000.0f;
                to[0] = from[0] + dir[0] * best; to[1] = from[1] + dir[1] * best; to[2] = from[2] + dir[2] * best;
            }
            else if (tgt_ok) { to[0] = (float)s->actors[e].mech.origin[0]; to[1] = s->units[e].y + s->height[e] * 0.55f; to[2] = (float)s->actors[e].mech.origin[2]; }
            else { to[0] = from[0] + dx; to[1] = from[1] + sinf(ap) * 100000.0f; to[2] = from[2] + dz; }
            if (s->player_attacks) combat_trigger_mask(&s->player_unit, tgt_ok ? sqrtf(dx * dx + dz * dz) : 0.0f, s->player_fire_mask ? s->player_fire_mask : 0xFFFFFFFFu);
            {
                int first = 0;
                int loc = -1;
                while ((w = combat_next_projectile_at(&s->player_unit, &first, &loc)) >= 0) {
                    float o[3] = {s->player[0], s->player_unit.y, s->player[2]}, mf[3];
                    const float *src = (s->pl_shown_ok && mech3d_mount_world(&s->pl_shown, mech3d_mount_slot(loc), o, s->player_facing, mf) == 0) ||
                                       (s->pl_mech_ok && mech3d_mount_world(&s->pl_mech, mech3d_mount_slot(loc), o, s->player_facing, mf) == 0) ? mf : from;
                    if (getenv("MW2_MUZZLE_TRACE")) {   /* tests: each player shot's start in the mech's frame (right, up, forward) */
                        float hh = s->player_facing * 3.14159265f / 180.0f, dx2 = src[0] - s->player[0], dz2 = src[2] - s->player[2];
                        fprintf(stderr, "muzzle w %d loc %d slot %d: right %.0f up %.0f fwd %.0f (%s) eye %.0f %.0f %.0f to %.0f %.0f %.0f\n", w, loc, mech3d_mount_slot(loc), dx2 * cosf(hh) - dz2 * sinf(hh), src[1] - s->player_unit.y, dx2 * sinf(hh) + dz2 * cosf(hh), src == from ? "body" : "mount", from[0], from[1], from[2], to[0], to[1], to[2]);
                    }
                    spawn_shot_h(s, w, -1, src, to, 0.0f, (s->player_lock & 0x80) ? s->player_lock_target : -2);
                    if (s->shot_count > 0 && s->shots[s->shot_count - 1].owner == -1) {   /* TEST ONLY: the aim line */
                        msim_shot *ns = &s->shots[s->shot_count - 1];
                        float ay2 = s->player_aim_yaw * 3.14159265f / 180.0f, ap2 = s->player_aim_pitch * 3.14159265f / 180.0f;
                        memcpy(ns->aim_o, s->player_eye_ok ? s->player_eye : from, sizeof ns->aim_o);
                        ns->aim_d[0] = sinf(ay2) * cosf(ap2); ns->aim_d[1] = sinf(ap2); ns->aim_d[2] = cosf(ay2) * cosf(ap2);
                    }
                    if (tgt_ok && (msim_weapon_guided(w) || w == 0x15)) {
                        /* 0x10020ac0 (from 0x1004397b): the player (level 1) launching at its target - in its view: within
                         * 15 degrees of the aim - warns it */
                        float off = wrap180(atan2f(dx, dz) * 180.0f / 3.14159265f - s->player_aim_yaw);
                        if (fabsf(off) < 15.0f) missile_warning(s, e, 1);
                    }
                    if (s->on_shot) s->on_shot(s->shot_user, w, -1, first, src[0], src[2]);
                    if (first && !s->no_player_muzzle) muzzle(s, w, src);
                }
            }
        } else {
            while (combat_next_projectile(&s->player_unit) >= 0) {}
        }
    }

    /* fly and collide */
    for (k = 0; k < s->shot_count; k++) {
        msim_shot *sh = &s->shots[k];
        float total = ticks < sh->life ? ticks : sh->life, step, o[3], dv[3];
        float tbest = 2, t;
        int hit_unit = -2, hit_group = 0, g, gone = 0, sub, nsub = (int)ceilf(total);
        if (nsub < 1) nsub = 1;
        step = total / (float)nsub;
        /* the engine moves and tests a projectile every tick (0x10044f60, 4 hull samples per tick);
         * testing a whole multi-tick step with 4 samples steps over mechs at close range */
        for (sub = 0; sub < nsub && hit_unit == -2; sub++) {
            o[0] = sh->x; o[1] = sh->y; o[2] = sh->z;
            if (sh->prox && sh->homing != -2) {
                /* 0x10044f60: a projectile flagged by the guidance (+0x38 & 0x8000) skips the collision test and hits its
                 * target where it is (the unit's own object, 0x1000f760: its root part's location) */
                int tg = sh->homing;
                if (!(tg == -1 ? s->player_unit.destroyed || !s->pl_mech_ok : (tg < 0 || tg >= s->actor_count || !s->armed[tg] || s->units[tg].destroyed))) {
                    const mech3d *tm = tg == -1 ? &s->pl_mech : &s->actors[tg].mech;
                    hit_unit = tg; hit_group = tm->part_count > 0 && tm->parts[0].group > 0 ? tm->parts[0].group : 3; tbest = 0;
                    dv[0] = dv[1] = dv[2] = 0;
                    break;
                }
                sh->prox = 0;
            }
            if (sh->homing != -2 && sh->age > 0) {   /* guidance (0x10045910): acceleration = unit(target - p) - unit(v), per tick^2 */
                float tx, ty, tz, tl, vl, ax, ay2, az;
                int tg = sh->homing;
                if (tg == -1 ? s->player_unit.destroyed : (tg < 0 || tg >= s->actor_count || !s->armed[tg] || s->units[tg].destroyed)) sh->homing = -2;
                else {
                    /* at the target OBJECT's position (+0x50..+0x58: the feet); within 100 cm (0x102485c4) the projectile is
                     * flagged to hit it next tick. Its speed is not held: the turn costs speed, as in the engine */
                    if (tg == -1) { tx = s->player[0]; ty = s->player_unit.y; tz = s->player[2]; }
                    else { tx = (float)s->actors[tg].mech.origin[0]; ty = s->units[tg].y; tz = (float)s->actors[tg].mech.origin[2]; }
                    tx -= sh->x; ty -= sh->y; tz -= sh->z;
                    tl = sqrtf(tx * tx + ty * ty + tz * tz); vl = sqrtf(sh->vx * sh->vx + sh->vy * sh->vy + sh->vz * sh->vz);
                    if (tl <= 100.0f) sh->prox = 1;
                    if (tl > 1 && vl > 0) {
                        ax = tx / tl - sh->vx / vl; ay2 = ty / tl - sh->vy / vl; az = tz / tl - sh->vz / vl;
                        sh->x += 0.5f * ax * step * step; sh->y += 0.5f * ay2 * step * step; sh->z += 0.5f * az * step * step;
                        sh->vx += ax * step; sh->vy += ay2 * step; sh->vz += az * step;
                        o[0] = sh->x; o[1] = sh->y; o[2] = sh->z;
                    }
                }
            }
            sh->age += step;
            /* 0x10044f60: p += v dt + a dt^2 / 2; v += a dt */
            dv[0] = sh->vx * step; dv[1] = sh->vy * step + 0.5f * sh->ay * step * step; dv[2] = sh->vz * step;
            /* candidates: the player and actors near the segment */
            for (i = -1; i < s->actor_count; i++) {
                const mech3d *m;
                float cx, cz, ex, ez, lx, lz, len2, u, px, pz;
                if (i == sh->owner) continue;
                if (i == -1) { if (!s->pl_mech_ok || s->player_unit.destroyed) continue; m = &s->pl_mech; }
                else { if (!s->armed[i] || s->units[i].destroyed) continue; m = &s->actors[i].mech; }
                cx = (float)m->origin[0]; cz = (float)m->origin[2];
                /* coarse: horizontal distance from the segment to the unit < 20 m */
                ex = dv[0]; ez = dv[2]; lx = cx - o[0]; lz = cz - o[2];
                len2 = ex * ex + ez * ez;
                u = len2 > 0 ? (lx * ex + lz * ez) / len2 : 0;
                if (u < 0) u = 0;
                if (u > 1) u = 1;
                px = o[0] + ex * u - cx; pz = o[2] + ez * u - cz;
                if (px * px + pz * pz > 2000.0f * 2000.0f) continue;
                g = mech3d_hull_hit((mech3d *)m, o, dv, &t);   /* engine: 4 samples vs part hulls */
                if (g && t < tbest) { tbest = t; hit_unit = i; hit_group = g; }
            }
            if (hit_unit == -2 && s->world && s->terrain) {
                /* world objects (engine 0x10044f60 -> 0x100103e0): the NEAREST object of the collision list the step
                 * crosses - the bounding sphere first (0x10029db0), then the type's segment test (0x10010650, table
                 * 0x1024b3a8): 0 / 1 the world box (0x1000d310, a slab test - a pipe or a tank is hit anywhere in its box),
                 * 2 / 7 the faces (0x1000d7b0), 3 / 6 / 8 / 9 nothing more (no entry: 0x100107b0 - the sphere's crossing
                 * is the hit; the GOLD shield dome, the TNJ3 target spheres). A thing's object (+2 flags 0x200) takes the
                 * damage (0x10046c20); any other just stops the shot (0x400: an impact). Hidden objects (destroyed, or
                 * a destroyed object's children) are out of the list (0x10025730) */
                int part = -1;
                float tb = 2.0f;
                if (shot_objects(s, o, dv, &tb, &part)) {
                    float hp3[3] = {o[0] + dv[0] * tb, o[1] + dv[1] * tb, o[2] + dv[2] * tb};
                    int ft = fx_impact_type(combat_weapon_visual(sh->weapon), FX_HIT_GROUND), b2 = shot_thing(s, part);
                    if (ft >= 0) effect(s, ft, hp3, missile_fx(sh->weapon));
                    if (b2 >= 0) building_damage(s, b2, combat_weapon_damage(sh->weapon), sh->owner, hp3);
                    gone = 1;
                }
                if (gone) { sh->life = 0; sh->y = -1.0f; break; }
            }
            if (hit_unit == -2) {
                sh->x += dv[0]; sh->y += dv[1]; sh->z += dv[2];
                sh->vy += sh->ay * step;
                sh->life -= step;
                if (sh->life <= 0) break;
                {
                    float gy = msim_ground(s, sh->x, sh->z, sh->y);
                    if (sh->y < gy) {   /* hit the terrain: a ground impact effect */
                        if (s->on_effect) {
                            float gp[3] = {sh->x, gy, sh->z};
                            int ft = fx_impact_type(combat_weapon_visual(sh->weapon), FX_HIT_GROUND);
                            if (ft >= 0) effect(s, ft, gp, missile_fx(sh->weapon));
                            if (sh->owner == -1) aim_trace(s, sh, "ground", gp);   /* TEST ONLY */
                        }
                        sh->y = -1.0f;
                        break;
                    }
                }
            }
        }
        if (hit_unit != -2) {
            combat_unit *tu = hit_unit == -1 ? &s->player_unit : &s->units[hit_unit];
            float th = hit_unit == -1 ? s->player_facing : s->actors[hit_unit].mech.heading;
            float dir = atan2f(sh->vx, sh->vz) * 180.0f / 3.14159265f;
            int gone_before[8], li;
            int was = tu->destroyed, dealt;
            memcpy(gone_before, tu->loc_gone, sizeof gone_before);
            dealt = combat_hit(tu, sh->weapon, hit_group, th - dir, &s->rng);
            (void)gone_before; (void)li;   /* the blown-off parts: blow_offs, each step */
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
                    for (j = 0; j < s->actor_count && getenv("MW2_PROVOKE"); j++) {
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
                float hp[3] = {o[0] + dv[0] * tbest, o[1] + dv[1] * tbest, o[2] + dv[2] * tbest};
                int ft = fx_impact_type(combat_weapon_visual(sh->weapon), FX_HIT_UNIT | (hit_unit == -1 ? FX_HIT_PLAYER : 0));
                if (ft >= 0) effect(s, ft, hp, missile_fx(sh->weapon));
                if (sh->owner == -1) aim_trace(s, sh, "unit", hp);   /* TEST ONLY */
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
        } else if (sh->life <= 0 || sh->y < 0) { gone = 1; s->misses++; if (getenv("MW2_BLD_TRACE") && sh->owner == -1) fprintf(stderr, "miss w %d at %.0f,%.0f,%.0f life %.1f\n", sh->weapon, sh->x, sh->y, sh->z, sh->life); }
        if (gone) { s->shots[k] = s->shots[--s->shot_count]; k--; }
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
typedef struct { float minx, maxx, minz, maxz, x[3], z[3], n[3], d; } tri_t;
typedef struct { float minx, maxx, minz, maxz, top; } box_t;
typedef struct { int *idx; int n, cap; } cell_t;
typedef struct {
    tri_t *tri; int ntri;
    box_t *box; int nbox;
    box_t *wall; int nwall;    /* type 1: XZ boxes, no height limit (engine 0x1000d520) */
    float x0, z0, cs; int gw, gh;
    cell_t *cell;              /* triangles (index >= 0) and boxes (-1 - index) per 50 m cell */
    int *solid; float (*solid_box)[4]; int nsolid, capsolid;   /* objects with a segment test (types 0, 2, 7): part, XZ box */
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
    for (i = 0; t->cell && i < t->gw * t->gh; i++) free(t->cell[i].idx);
    for (i = 0; t->ocell && i < t->gw * t->gh; i++) free(t->ocell[i].idx);
    free(t->cell); free(t->tri); free(t->box); free(t->wall); free(t->solid); free(t->solid_box);
    free(t->obj); free(t->obj_box); free(t->obj_sph); free(t->ocell); free(t->thing_of); free(t->stamp);
    free(t->mov); free(t->mov_box); free(t);
}

static void terrain_release(void *t) { terrain_free((terrain_t *)t); }

static terrain_t *terrain_build(const mech3d *w)
{
    terrain_t *t = calloc(1, sizeof *t);
    int i, k, j, capt = 0, capb = 0, capw = 0;
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
        if (p->coll == 0 || p->coll == 2 || p->coll == 7) {   /* the move segment's objects (msim_can_step_hr) */
            if (t->nsolid == t->capsolid) {
                t->capsolid = t->capsolid ? t->capsolid * 2 : 64;
                t->solid = realloc(t->solid, (size_t)t->capsolid * sizeof *t->solid);
                t->solid_box = realloc(t->solid_box, (size_t)t->capsolid * sizeof *t->solid_box);
            }
            t->solid[t->nsolid] = i;
            t->solid_box[t->nsolid][0] = bmn[0]; t->solid_box[t->nsolid][1] = bmx[0];
            t->solid_box[t->nsolid][2] = bmn[2]; t->solid_box[t->nsolid][3] = bmx[2];
            t->nsolid++;
            if (p->coll != 0) continue;   /* types 2 and 7: no ground (no entry at 0x1024b3b0) */
        }
        if (p->coll == 1) {   /* engine type 1 (0x1000d520): an XZ box, blocking at any height */
            if (t->nwall == capw) { capw = capw ? capw * 2 : 64; t->wall = realloc(t->wall, (size_t)capw * sizeof *t->wall); }
            t->wall[t->nwall].minx = bmn[0]; t->wall[t->nwall].maxx = bmx[0]; t->wall[t->nwall].minz = bmn[2]; t->wall[t->nwall].maxz = bmx[2];
            t->wall[t->nwall].top = bmx[1];
            t->nwall++;
        }
        if (p->coll == 1) { /* bounds below */ }
        else if (p->coll == 0) {   /* engine type 0 (0x1000d250): the object's box; its top is the ground */
            if (t->nbox == capb) { capb = capb ? capb * 2 : 64; t->box = realloc(t->box, (size_t)capb * sizeof *t->box); }
            t->box[t->nbox].minx = bmn[0]; t->box[t->nbox].maxx = bmx[0]; t->box[t->nbox].minz = bmn[2]; t->box[t->nbox].maxz = bmx[2];
            t->box[t->nbox].top = bmx[1];
            t->nbox++;
        } else {              /* engine type 5 (0x1000cf80): upward-facing polygons, height on their plane */
            for (k = 0; k < o->poly_count; k++) {
                const wtb_poly *pg = &o->polys[k];
                float a[3], b[3], c[3], u[3], v[3], n[3], len;
                int f;
                if (pg->n < 3) continue;
                part_vertex(p, &o->verts[pg->idx[0]], a);
                for (f = 1; f + 1 < pg->n; f++) {   /* fan into triangles */
                    tri_t *tr;
                    part_vertex(p, &o->verts[pg->idx[f]], b);
                    part_vertex(p, &o->verts[pg->idx[f + 1]], c);
                    for (j = 0; j < 3; j++) { u[j] = b[j] - a[j]; v[j] = c[j] - a[j]; }
                    n[0] = u[1] * v[2] - u[2] * v[1]; n[1] = u[2] * v[0] - u[0] * v[2]; n[2] = u[0] * v[1] - u[1] * v[0];
                    len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    if (len < 1e-3f) continue;
                    for (j = 0; j < 3; j++) n[j] /= len;
                    if (n[1] < 0) for (j = 0; j < 3; j++) n[j] = -n[j];   /* winding varies; the surface faces up */
                    if (n[1] < 0.05f) continue;                         /* walls: not ground */
                    if (t->ntri == capt) { capt = capt ? capt * 2 : 256; t->tri = realloc(t->tri, (size_t)capt * sizeof *t->tri); }
                    tr = &t->tri[t->ntri++];
                    tr->x[0] = a[0]; tr->z[0] = a[2]; tr->x[1] = b[0]; tr->z[1] = b[2]; tr->x[2] = c[0]; tr->z[2] = c[2];
                    tr->minx = fminf(a[0], fminf(b[0], c[0])); tr->maxx = fmaxf(a[0], fmaxf(b[0], c[0]));
                    tr->minz = fminf(a[2], fminf(b[2], c[2])); tr->maxz = fmaxf(a[2], fmaxf(b[2], c[2]));
                    memcpy(tr->n, n, sizeof n);
                    tr->d = n[0] * a[0] + n[1] * a[1] + n[2] * a[2];
                }
            }
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
    if (!t->ntri && !t->nbox && !t->nwall && !t->nsolid && !t->nobj && !t->nmov) { terrain_free(t); return NULL; }
    t->cs = 5000.0f;
    t->x0 = minx; t->z0 = minz;
    if (!(maxx >= minx) || !(maxz >= minz) || maxx - minx > 4.0e8f || maxz - minz > 4.0e8f) { terrain_free(t); return NULL; }
    t->gw = (int)((maxx - minx) / t->cs) + 1; t->gh = (int)((maxz - minz) / t->cs) + 1;
    if (t->gw > 4096) t->gw = 4096;
    if (t->gh > 4096) t->gh = 4096;
    t->cell = calloc((size_t)t->gw * (size_t)t->gh, sizeof *t->cell);
    t->ocell = calloc((size_t)t->gw * (size_t)t->gh, sizeof *t->ocell);
    t->stamp = calloc((size_t)(w->part_count > 0 ? w->part_count : 1), sizeof *t->stamp);
    if (!t->cell || !t->ocell || !t->stamp) { terrain_free(t); return NULL; }
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
    for (i = 0; i < t->ntri + t->nbox; i++) {
        float a0 = i < t->ntri ? t->tri[i].minx : t->box[i - t->ntri].minx, a1 = i < t->ntri ? t->tri[i].maxx : t->box[i - t->ntri].maxx;
        float b0 = i < t->ntri ? t->tri[i].minz : t->box[i - t->ntri].minz, b1 = i < t->ntri ? t->tri[i].maxz : t->box[i - t->ntri].maxz;
        int cx0 = (int)((a0 - t->x0) / t->cs), cx1 = (int)((a1 - t->x0) / t->cs), cz0 = (int)((b0 - t->z0) / t->cs), cz1 = (int)((b1 - t->z0) / t->cs), cx, cz;
        if (cx0 < 0) cx0 = 0;
        if (cz0 < 0) cz0 = 0;
        if (cx1 >= t->gw) cx1 = t->gw - 1;
        if (cz1 >= t->gh) cz1 = t->gh - 1;
        for (cz = cz0; cz <= cz1; cz++)
            for (cx = cx0; cx <= cx1; cx++) cell_add(&t->cell[cz * t->gw + cx], i < t->ntri ? i : -1 - (i - t->ntri));
    }
    return t;
}

static int in_tri(const tri_t *t, float x, float z)
{
    float d1 = (x - t->x[1]) * (t->z[0] - t->z[1]) - (t->x[0] - t->x[1]) * (z - t->z[1]);
    float d2 = (x - t->x[2]) * (t->z[1] - t->z[2]) - (t->x[1] - t->x[2]) * (z - t->z[2]);
    float d3 = (x - t->x[0]) * (t->z[2] - t->z[0]) - (t->x[2] - t->x[0]) * (z - t->z[0]);
    int neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

/* the servo 0x1003b7e0: value += elapsed ticks x (target - value) / T, never past the target (snaps within 1e-7 of it) */
static float drive_servo(float cur, float tgt, float T, float ticks)
{
    float d = tgt - cur, inc = ticks * d / T;
    if (fabsf(d) < 1e-7f || fabsf(inc) >= fabsf(d)) return tgt;
    return cur + inc;
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
void msim_drive_step(float *thr, float *spd, float *vel, float thr_target, float rev, float top, int drive, int slow, float ticks)
{
    float c10;
    if (!drive) thr_target = 0;
    *thr = drive_servo(*thr, thr_target, 36.2f, ticks);
    c10 = (!drive || fabsf(*thr) < 1.0f / 32.0f) ? 0.0f : *thr * top * (*thr < 0 ? 0.5f : 1.0f) * rev;
    *spd = drive_servo(*spd, c10, slow ? 90.5f : 36.2f, ticks);
    *vel += (*spd - *vel) * (ticks < 45.0f ? ticks / 45.0f : 1.0f);
}
float msim_ground(const msim *s, float x, float z, float y) { return msim_ground_n(s, x, z, y, NULL); }

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
int msim_unit_contact(const msim *s, int self, const float p0[3], const float p1[3], float n[3], float c[3], float *rr, float ov[3])
{
    float r1 = self >= 0 ? (s->radius ? s->radius[self] : 450.0f) : s->player_radius, best = 1e30f;
    int k, hit = -2;
    for (k = -1; k < s->actor_count; k++) {
        float q[3], r2, d1, d0, rs;
        if (k == self) continue;
        if (k < 0) {
            if (s->player_unit.destroyed) continue;
            q[0] = s->player[0]; q[1] = s->player_unit.y + s->player_centre_h; q[2] = s->player[2]; r2 = s->player_radius;
        } else {
            if (s->units[k].destroyed) continue;
            q[0] = (float)s->actors[k].mech.origin[0]; q[1] = s->units[k].y + (s->centre_h ? s->centre_h[k] : 550.0f);
            q[2] = (float)s->actors[k].mech.origin[2]; r2 = s->radius ? s->radius[k] : 450.0f;
        }
        rs = r1 + r2;
        d1 = (p1[0] - q[0]) * (p1[0] - q[0]) + (p1[1] - q[1]) * (p1[1] - q[1]) + (p1[2] - q[2]) * (p1[2] - q[2]);
        d0 = (p0[0] - q[0]) * (p0[0] - q[0]) + (p0[1] - q[1]) * (p0[1] - q[1]) + (p0[2] - q[2]) * (p0[2] - q[2]);
        if (d1 < rs * rs && d1 < d0 && d1 < best) {
            float d = sqrtf(d1);
            best = d1; hit = k; *rr = rs;
            c[0] = q[0]; c[1] = q[1]; c[2] = q[2];
            if (d > 1e-3f) { n[0] = (p1[0] - q[0]) / d; n[1] = (p1[1] - q[1]) / d; n[2] = (p1[2] - q[2]) / d; }
            else { n[0] = 0; n[1] = 0; n[2] = 1.0f; }   /* coincident centres: the engine's (0, 0, 1) */
            if (k < 0) { ov[0] = s->player_vel[0]; ov[1] = s->player_vel[1]; ov[2] = s->player_vel[2]; }
            else {
                int air = s->units[k].y > s->units[k].ground + 1.0f;
                float sp = (air && s->air_speed ? s->air_speed[k] : s->minds ? s->minds[k].speed : 0.0f) / 182.0f;
                float hh = s->actors[k].mech.heading * 3.14159265f / 180.0f;
                ov[0] = sinf(hh) * sp; ov[1] = air ? s->units[k].vy : 0.0f; ov[2] = cosf(hh) * sp;
            }
        }
    }
    return hit;
}

int msim_can_step_hr(const msim *s, float x0, float z0, float x, float z, float y, float H, float R)
{
    g_block_what = NULL;
    float n[3], g = msim_ground_n(s, x, z, y, n), dh = y - g, dx = x - x0, dz = z - z0, dl = sqrtf(dx * dx + dz * dz);
    /* the engine's move segment runs from the old centre to the new one (H above the feet), extended R ahead along
     * the move (0x1000b5e0); it stops only on FRONT faces it crosses (0x10010830: direction . normal < 0), and a
     * crossed face steeper than 45 degrees (normal y < 0x3f350482) blocks - so the climbable rise is H (Timber Wolf
     * 5.5 m), seen R ahead. Walking off an edge isn't blocked (the face below is a back face). */
    if (n[1] < 0.70710677f && g - y > H && n[0] * dx + n[2] * dz < 0) return refuse(n);
    if (dl > 1e-3f) {
        float n2[3], ax = x + dx / dl * R, az = z + dz / dl * R, g2 = msim_ground_n(s, ax, az, y, n2);
        if (n2[1] < 0.70710677f && g2 - y > H && n2[0] * dx + n2[2] * dz < 0) return refuse(n2);
    }
    {
        float wn[3];
        if (msim_wall_at(s, x, z, wn)) { g_block_what = "wall (type 1)"; return refuse(wn); }   /* type 1 obstacle */
    }
    if (dh < -9999.99f) return refuse(n);   /* 0xc61c3fff: feet 100 m under the ground (the engine's other limit, feet > 999.99 above
                                     * after crossing a face, has no counterpart without the face test) */
    if (s->world && s->terrain) {
        const terrain_t *tt = (const terrain_t *)s->terrain;
        /* objects (engine 0x1000b5e0 -> 0x10010530 -> 0x10010650, the segment test of the object's collision type from the
         * table at 0x1024b3a8): the move segment runs from the old centre (H above the feet) to the new one extended R along
         * the move. Type 0 (0x1000d310): the object's world box, a 3D slab test - so a box blocks only where it spans the
         * centre's height: a pipe overhead (YELLARE6's Coolant-Piping, 7.4 - 9.2 m up) is walked under, and a box lower
         * than the centre is climbed by the ground (0x1000d1f0: the box top). Types 2 and 7 (0x1000d7b0 -> 0x10010830):
         * the polygons, FRONT faces crossed only (the stored normal, negated on load, 0x10042140); type 7 = the chemical
         * plant's processor and the destroyed variants. A hit whose normal y is under cos 45 (0x3f350482) is refused
         * (0x10019310); a flatter face is climbed. */
        float cy = y + H, ex = x, ez = z, sx, sz;
        int k;
        if (dl > 1e-3f) { ex = x + dx / dl * R; ez = z + dz / dl * R; }
        sx = ex - x0; sz = ez - z0;
        for (k = 0; k < tt->nsolid + tt->nmov; k++) {   /* the fixed objects, then the moving ones where they are now */
            const float *mb = k < tt->nsolid ? NULL : tt->mov_box[k - tt->nsolid];
            float mxz[4];
            const float *b = mb ? mxz : tt->solid_box[k];
            int pi = mb ? tt->mov[k - tt->nsolid] : tt->solid[k];
            const mech3d_part *wp;
            if (mb) { mxz[0] = mb[0]; mxz[1] = mb[3]; mxz[2] = mb[2]; mxz[3] = mb[5]; }
            const wtb_object *o;
            float t0 = 0, t1 = 1, lo[2] = {b[0], b[2]}, hi[2] = {b[1], b[3]}, org[2] = {x0, z0}, dv[2] = {sx, sz};
            int a, ok = 1, ent = 0;
            if (pi < 0 || pi >= s->world->part_count) continue;
            wp = &s->world->parts[pi];
            if (wp->hidden || wp->model.object_count < 1 || (wp->coll != 0 && wp->coll != 2 && wp->coll != 7)) continue;
            for (a = 0; a < 2 && ok; a++) {   /* the segment's XZ against the footprint */
                if (fabsf(dv[a]) < 1e-6f) { if (org[a] < lo[a] || org[a] > hi[a]) ok = 0; continue; }
                {
                    float ta = (lo[a] - org[a]) / dv[a], tb = (hi[a] - org[a]) / dv[a];
                    if (ta > tb) { float tq = ta; ta = tb; tb = tq; }
                    if (ta > t0) { t0 = ta; ent = a; }
                    if (tb < t1) t1 = tb;
                    if (t0 > t1) ok = 0;
                }
            }
            if (!ok) continue;
            o = &wp->model.objects[0];
            if (wp->coll == 0) {
                float bot = 3.4e38f, top = -3.4e38f;
                int q;
                /* already inside the footprint: the port lets the mech walk out (the engine's slab test would hold it) */
                if (x0 >= b[0] && x0 <= b[1] && z0 >= b[2] && z0 <= b[3]) continue;
                for (q = 0; q < o->vert_count; q++) {
                    float v = (float)o->verts[q].y;
                    if (v > top) top = v;
                    if (v < bot) bot = v;
                }
                top += (float)wp->pos[1]; bot += (float)wp->pos[1];
                if (mb) { bot = mb[1]; top = mb[4]; }   /* moving (turned): its world box now */
                if (bot <= cy && cy <= top) {   /* the box face entered (0x1000d310's entry axis) */
                    float bn[3] = {0, 0, 0};
                    g_block_what = wp->model_name;
                    bn[ent ? 2 : 0] = dv[ent] > 0 ? -1.0f : 1.0f;
                    return refuse(bn);
                }
            } else {
                float org3[3] = {x0, cy, z0}, d3[3] = {sx, 0.0f, sz};
                int q, j;
                for (q = 0; q < o->poly_count; q++) {
                    const wtb_poly *pg = &o->polys[q];
                    float A[3], B[3], C[3], nw[3], zero[3], one[3];
                    wtb_vertex nv;
                    if (pg->n < 3) continue;
                    part_vertex(wp, &o->verts[pg->idx[0]], A);
                    part_vertex(wp, &o->verts[pg->idx[1]], B);
                    part_vertex(wp, &o->verts[pg->idx[2]], C);
                    if (pg->has_normal) {   /* the engine's normal: the stored one negated, turned with the object */
                        memset(&nv, 0, sizeof nv);
                        part_vertex(wp, &nv, zero);
                        nv.x = (int32_t)lrintf(-pg->normal[0] * 4096.0f); nv.y = (int32_t)lrintf(-pg->normal[1] * 4096.0f); nv.z = (int32_t)lrintf(-pg->normal[2] * 4096.0f);
                        part_vertex(wp, &nv, one);
                        for (j = 0; j < 3; j++) nw[j] = (one[j] - zero[j]) / 4096.0f;
                    } else {                /* the winding normal points like the stored one: negated */
                        float u[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, v[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]}, l;
                        nw[0] = -(u[1] * v[2] - u[2] * v[1]); nw[1] = -(u[2] * v[0] - u[0] * v[2]); nw[2] = -(u[0] * v[1] - u[1] * v[0]);
                        l = sqrtf(nw[0] * nw[0] + nw[1] * nw[1] + nw[2] * nw[2]);
                        if (l < 1e-6f) continue;
                        for (j = 0; j < 3; j++) nw[j] /= l;
                    }
                    if (nw[0] * d3[0] + nw[2] * d3[2] >= 0 || nw[1] >= 0.70710677f) continue;   /* back face, or climbable */
                    for (j = 1; j + 1 < pg->n; j++) {
                        if (j > 1) { part_vertex(wp, &o->verts[pg->idx[j]], B); part_vertex(wp, &o->verts[pg->idx[j + 1]], C); }
                        if (seg_tri(org3, d3, A, B, C) >= 0) { g_block_what = wp->model_name; return refuse(nw); }
                    }
                }
            }
        }
    }
    return 1;
}

float msim_ground_n(const msim *s, float x, float z, float y, float *nout)
{
    const terrain_t *t = s ? (const terrain_t *)s->terrain : NULL;
    const cell_t *c;
    static const float UP[3] = {0, 1, 0};
    float win = -3.4e38f, below = -3.4e38f;
    const float *nwin = UP, *nbelow = UP;
    int cx, cz, k;
    if (nout) memcpy(nout, UP, sizeof UP);
    if (!t) return 0.0f;
    cx = (int)((x - t->x0) / t->cs); cz = (int)((z - t->z0) / t->cs);
    c = cx < 0 || cz < 0 || cx >= t->gw || cz >= t->gh || !t->cell ? NULL : &t->cell[cz * t->gw + cx];
    for (k = 0; k < (c ? c->n : 0) + t->nmov; k++) {
        float h;
        const float *ny = UP;
        int v = c && k < c->n ? c->idx[k] : -1;
        if (!c || k >= c->n) {   /* a moving type 0 object: its box top where it is now */
            const float *mb = t->mov_box[k - (c ? c->n : 0)];
            const mech3d_part *mp = s->world && t->mov[k - (c ? c->n : 0)] < s->world->part_count ? &s->world->parts[t->mov[k - (c ? c->n : 0)]] : NULL;
            if (!mp || mp->coll != 0 || x < mb[0] || x > mb[3] || z < mb[2] || z > mb[5]) continue;
            h = mb[4];
        } else if (v >= 0) {
            const tri_t *tr = &t->tri[v];
            if (x < tr->minx || x > tr->maxx || z < tr->minz || z > tr->maxz || !in_tri(tr, x, z)) continue;
            h = (tr->d - tr->n[0] * x - tr->n[2] * z) / tr->n[1];
            ny = tr->n;
        } else {
            const box_t *b = &t->box[-1 - v];
            if (x < b->minx || x > b->maxx || z < b->minz || z > b->maxz) continue;
            h = b->top;
        }
        /* engine 0x10010080: within 10 m of y wins (highest); else the highest below y */
        if (win == -3.4e38f && h < y && h > below) { below = h; nbelow = ny; }
        if (y < h + 1000.0f && h - 1000.0f < y && h > win) { win = h; nwin = ny; }
    }
    if (win > -3.4e38f) { if (nout) memcpy(nout, nwin, sizeof UP); return win; }
    if (below > -3.4e38f) { if (nout) memcpy(nout, nbelow, sizeof UP); return below; }
    return 0.0f;
}

static void part_vertex(const mech3d_part *p, const wtb_vertex *v, float out[3]);
static void buildings_init(msim *s, const mech3d *w)
{
    int i, k, q;
    free(s->bld); s->bld = NULL; s->bld_count = 0;
    if (!s->arch) return;
    for (i = 0; i < w->part_count; i++) {
        prj_record r;
        bwd_chunk *c;
        int n, seen = 0;
        if (!w->parts[i].rec[0]) continue;
        for (k = 0; k < i && !seen; k++) if (strcmp(w->parts[k].rec, w->parts[i].rec) == 0) seen = 1;
        if (seen || prj_read_named(s->arch, "BWD", w->parts[i].rec, &r) != PRJ_OK) continue;
        n = bwd_chunks(r.data, r.size, NULL, 0);
        if (n > 0 && (c = calloc((size_t)n, sizeof *c))) {
            bwd_chunks(r.data, r.size, c, n);
            for (k = 0; k < n; k++) {
                const uint8_t *d = c[k].data;
                int a, b, hp, ia = -1, ib = -1;
                if (strcmp(c[k].tag, "GT") != 0 || c[k].size < 68) continue;
                a = (int16_t)(d[0] | d[1] << 8); b = (int16_t)(d[2] | d[3] << 8); hp = (int16_t)(d[16] | d[17] << 8);
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
                    {   /* broad phase: the intact model's world box */
                        const mech3d_part *pp = &w->parts[ia];
                        int v, j;
                        for (j = 0; j < 3; j++) { g->mn[j] = 1e30f; g->mx[j] = -1e30f; }
                        if (pp->model.object_count > 0)
                            for (v = 0; v < pp->model.objects[0].vert_count; v++) {
                                float q3[3];
                                part_vertex(pp, &pp->model.objects[0].verts[v], q3);
                                for (j = 0; j < 3; j++) { if (q3[j] < g->mn[j]) g->mn[j] = q3[j]; if (q3[j] > g->mx[j]) g->mx[j] = q3[j]; }
                            }
                    }
                    snprintf(g->name, sizeof g->name, "%.22s", (const char *)d + 24);
                    snprintf(g->sub, sizeof g->sub, "%.22s", (const char *)d + 46);
                }
            }
            free(c);
        }
        prj_record_free(&r);
    }
}

void msim_set_world(msim *s, const mech3d *world)
{
    if (s->world != world) {
        /* path tasks first: the carried parts are flagged `moving` and kept out of the static lists below */
        world3d_paths_free(s->paths);
        s->paths = s->arch && s->scene[0] ? world3d_paths_load(s->arch, s->scene, (mech3d *)world) : NULL;
        buildings_init(s, world);
    }
    terrain_free((terrain_t *)s->terrain);
    s->terrain = terrain_build(world);
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
        /* candidates: anything reaching probe height (2 m), excluding terrain-sized objects (> 300 m) */
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

/* segment (2D) against the building footprints: returns the index hit or -1 */
static int probe_hits(const msim *s, float x0, float z0, float x1, float z1)
{
    int i;
    for (i = 0; i < s->obstacle_count; i++) {
        const float *b = s->obstacles[i];
        float t0 = 0, t1 = 1, d[2] = {x1 - x0, z1 - z0}, o[2] = {x0, z0}, lo[2] = {b[0], b[2]}, hi[2] = {b[1], b[3]};
        int a, ok = 1;
        /* the engine tests rays against faces: something the mech already stands in doesn't block it */
        if (x0 >= b[0] && x0 <= b[1] && z0 >= b[2] && z0 <= b[3]) continue;
        for (a = 0; a < 2 && ok; a++) {
            if (fabsf(d[a]) < 1e-6f) { if (o[a] < lo[a] || o[a] > hi[a]) ok = 0; continue; }
            {
                float ta = (lo[a] - o[a]) / d[a], tb = (hi[a] - o[a]) / d[a];
                if (ta > tb) { float t = ta; ta = tb; tb = t; }
                if (ta > t0) t0 = ta;
                if (tb < t1) t1 = tb;
                if (t0 > t1) ok = 0;
            }
        }
        if (ok) {
            /* narrow phase: the probe at 2 m height against this object's faces */
            if (s->world && s->obstacle_part) {
                float o3[3] = {x0, 200.0f, z0}, d3[3] = {x1 - x0, 0.0f, z1 - z0}, t;
                const mech3d_part *p = &s->world->parts[s->obstacle_part[i]];
                mech3d one;
                memset(&one, 0, sizeof one);
                one.parts = (mech3d_part *)p;
                one.part_count = 1;
                if (seg_mech(&one, o3, d3, &t)) return i;
                continue;
            }
            return i;
        }
    }
    return -1;
}

/* A probe as the engine's 0x100103e0 sees it: the NEAREST object face the ray crosses (*ny = that face's normal y, the
 * engine's DAT_1024b424 from 0x10010650; the normal as in msim_can_step_hr: the stored one negated), -1 none. */
static int probe_nearest(const msim *s, float x0, float z0, float x1, float z1, float *ny)
{
    int i, best = -1;
    float tbest = 2.0f;
    *ny = 0;
    if (!s->world || !s->obstacle_part) { best = probe_hits(s, x0, z0, x1, z1); return best; }
    for (i = 0; i < s->obstacle_count; i++) {
        const float *b = s->obstacles[i];
        const mech3d_part *wp = &s->world->parts[s->obstacle_part[i]];
        const wtb_object *o;
        float o3[3] = {x0, 200.0f, z0}, d3[3] = {x1 - x0, 0.0f, z1 - z0};
        int k, j;
        if (x0 >= b[0] && x0 <= b[1] && z0 >= b[2] && z0 <= b[3]) continue;   /* standing in it: no front face (as probe_hits) */
        if ((x0 < b[0] && x1 < b[0]) || (x0 > b[1] && x1 > b[1]) || (z0 < b[2] && z1 < b[2]) || (z0 > b[3] && z1 > b[3])) continue;
        if (wp->model.object_count < 1) continue;
        o = &wp->model.objects[0];
        for (k = 0; k < o->poly_count; k++) {
            const wtb_poly *pg = &o->polys[k];
            float A[3], B[3], C[3], t;
            if (pg->n < 3) continue;
            part_vertex(wp, &o->verts[pg->idx[0]], A);
            for (j = 1; j + 1 < pg->n; j++) {
                part_vertex(wp, &o->verts[pg->idx[j]], B);
                part_vertex(wp, &o->verts[pg->idx[j + 1]], C);
                t = seg_tri(o3, d3, A, B, C);
                if (t >= 0 && t <= 1 && t < tbest) {
                    float nw[3], zero[3], one[3];
                    tbest = t; best = i;
                    if (pg->has_normal) {
                        wtb_vertex nv;
                        memset(&nv, 0, sizeof nv);
                        part_vertex(wp, &nv, zero);
                        nv.x = (int32_t)lrintf(-pg->normal[0] * 4096.0f); nv.y = (int32_t)lrintf(-pg->normal[1] * 4096.0f); nv.z = (int32_t)lrintf(-pg->normal[2] * 4096.0f);
                        part_vertex(wp, &nv, one);
                        nw[1] = (one[1] - zero[1]) / 4096.0f;
                    } else {
                        float P[3], u[3], v[3], l;
                        part_vertex(wp, &o->verts[pg->idx[1]], P);
                        u[0] = P[0] - A[0]; u[1] = P[1] - A[1]; u[2] = P[2] - A[2];
                        part_vertex(wp, &o->verts[pg->idx[2]], P);
                        v[0] = P[0] - A[0]; v[1] = P[1] - A[1]; v[2] = P[2] - A[2];
                        nw[0] = u[1] * v[2] - u[2] * v[1]; nw[1] = u[2] * v[0] - u[0] * v[2]; nw[2] = u[0] * v[1] - u[1] * v[0];
                        l = sqrtf(nw[0] * nw[0] + nw[1] * nw[1] + nw[2] * nw[2]);
                        nw[1] = l > 1e-6f ? -nw[1] / l : 0.0f;
                    }
                    *ny = nw[1];
                }
            }
        }
    }
    return best;
}

/* engine 0x100204b0: a probe hit that does not count - an object whose collision type has a ground handler (column 0 of
 * the type table 0x1024b3b0: types 0 (0x1000d1f0) and 5 (0x1000d690); 0x100102c0) where the face hit slopes at most 40
 * degrees (normal y >= 0x3f441893 = 0.766: it would be climbed), or any object of class 0x50 (object +2 = the OBJ type
 * word, & 0xf0) */
static int probe_climbable(const msim *s, int hit, float ny)
{
    const mech3d_part *p;
    if (hit < 0 || !s->world || !s->obstacle_part) return 0;
    p = &s->world->parts[s->obstacle_part[hit]];
    if ((p->objtype & 0xf0) == 0x50) return 1;
    return (p->coll == 0 || p->coll == 5) && ny >= 0.76604f;
}

/* engine 0x100201c0: probe ahead at 0, 26.6, 45, 63.4 degrees toward the avoid side; each blocked
 * probe adds 204.8 turn; deflecting drops throttle to 1/4; re-checked every 90 / 181 ticks */
/* Terrain on a probe ray (engine 0x100201c0 probes the collision list, terrain included): walk it in
 * 50 cm steps (thin rock fins are under a metre) from height y, following the ground like a mech; a
 * refused step blocks. */
static int terrain_probe(const msim *s, float x0, float z0, float y, float x1, float z1)
{
    float dx = x1 - x0, dz = z1 - z0, len = sqrtf(dx * dx + dz * dz);
    int k, n = (int)(len / 50.0f) + 1;
    if (!s->terrain) return 0;
    for (k = 1; k <= n; k++) {
        float px = x0 + dx * (float)k / (float)n, pz = z0 + dz * (float)k / (float)n;
        if (!msim_can_step(s, x0 + dx * (float)(k - 1) / (float)n, z0 + dz * (float)(k - 1) / (float)n, px, pz, y)) return 1;
        y = msim_ground(s, px, pz, y);
    }
    return 0;
}

/* Mechs on a probe ray: the engine's probes (0x100201c0 -> 0x100103e0) test the whole collision list, the player and
 * the other mechs included, skipping only the unit's own object. A mech is touched within the two contact radii
 * (MGEO int[6] each, 0x1000ba20). Returns 1 and the mech's position when one is hit. */
static int mech_probe(const msim *s, int self, float x0, float z0, float x1, float z1, float *hx, float *hz)
{
    float dx = x1 - x0, dz = z1 - z0, l2 = dx * dx + dz * dz;
    int k;
    for (k = -1; k < s->actor_count; k++) {
        float cx, cz, t, px, pz;
        if (k == self) continue;
        if (k < 0) { if (s->player_unit.destroyed) continue; cx = s->player[0]; cz = s->player[2]; }
        else { if (!s->armed[k] || s->units[k].destroyed) continue; cx = (float)s->actors[k].mech.origin[0]; cz = (float)s->actors[k].mech.origin[2]; }
        float rr = (self >= 0 ? s->radius[self] : s->player_radius) + (k < 0 ? s->player_radius : s->radius[k]);
        if ((cx - x0) * (cx - x0) + (cz - z0) * (cz - z0) < rr * rr) continue;   /* already touching: a face test misses */
        t = l2 > 0 ? ((cx - x0) * dx + (cz - z0) * dz) / l2 : 0;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        px = x0 + dx * t - cx; pz = z0 + dz * t - cz;
        if (px * px + pz * pz < rr * rr) { *hx = cx; *hz = cz; return 1; }
    }
    return 0;
}

static void avoid_obstacles(msim *s, int i, ai_mind *m, float goal_bearing, float dt)
{
    static const float ANG[4] = {0.0f, 26.565f, 45.0f, 63.435f}, LEN[4] = {1.0f, 1.118f, 1.414f, 1.118f};
    world_actor *ac = &s->actors[i];
    float x = (float)ac->mech.origin[0], z = (float)ac->mech.origin[2];
    float vel = ac->speed * m->throttle / 182.0f, len = vel * 0.12987f, accum = 0;
    int step, side = s->avoid_side[i];
    if (m->throttle <= 0) { s->avoid_turn[i] = 0; return; }
    s->avoid_next[i] -= dt * 182.0f;
    if (s->avoid_next[i] > 0) {
        if (s->avoid_turn[i] != 0) { m->turn = s->avoid_turn[i]; m->throttle = s->avoid_thr[i]; }
        return;
    }
    if (ac->unit_class != 1) len *= 0.5f;   /* 0x100201c0: the unit record's class (+0) other than 1 - vehicles - probes half as far */
    if (len < 0.3f) len = 0.3f;
    len *= 5000.0f;
    for (step = 0; step < 4; step++) {
        float a = (ac->mech.heading + (float)side * ANG[step]) * 3.14159265f / 180.0f, L = len * LEN[step];
        float ny = 0;
        int hit = probe_nearest(s, x, z, x + sinf(a) * L, z + cosf(a) * L, &ny);
        float mx = 0, mz = 0;
        if (probe_climbable(s, hit, ny)) break;   /* 0x100204b0: a climbable / class 0x50 hit ends the scan - no turn, no wall follow */
        if (hit < 0 && mech_probe(s, i, x, z, x + sinf(a) * L, z + cosf(a) * L, &mx, &mz)) hit = -3;   /* a mech */
        if (hit < 0 && hit != -3 && terrain_probe(s, x, z, s->units[i].y, x + sinf(a) * L, z + cosf(a) * L)) hit = -2;   /* terrain */
        if (hit == -1) {
            if (step == 0 && fabsf(wrap180(goal_bearing - ac->mech.heading)) <= 1.0f) side = 0;
            if (step == 0 && side != 0) {
                /* ahead is clear but the goal is still off to the other side: the engine probes 26.6 degrees back toward
                 * it (0x100201c0: 0x100204e0(-side, 1, ..)) and, while that ray is blocked, keeps turning away (+side x
                 * 409.6, 0x10247594) at full throttle - it follows the wall instead of steering straight back into it */
                float b = (ac->mech.heading - (float)side * ANG[1]) * 3.14159265f / 180.0f, L = len * LEN[1], bx = 0, bz = 0;
                float ex = x + sinf(b) * L, ez = z + cosf(b) * L, bny = 0;
                int bh = probe_nearest(s, x, z, ex, ez, &bny);
                if ((bh >= 0 && !probe_climbable(s, bh, bny)) || mech_probe(s, i, x, z, ex, ez, &bx, &bz) || terrain_probe(s, x, z, s->units[i].y, ex, ez))
                    accum += (float)side * 409.6f;
            }
            break;
        }
        if (side == 0 && (hit >= 0 || hit == -3)) {   /* 0x10020670: turn away from the obstacle */
            float ob = hit == -3 ? atan2f(mx - x, mz - z) * 180.0f / 3.14159265f
                                 : atan2f((s->obstacles[hit][0] + s->obstacles[hit][1]) * 0.5f - x, (s->obstacles[hit][2] + s->obstacles[hit][3]) * 0.5f - z) * 180.0f / 3.14159265f;
            side = wrap180(ob - ac->mech.heading) < 0 ? 1 : -1;
        } else if (side == 0) {   /* terrain: the side whose 45-degree ray is walkable (port) */
            float ar = (ac->mech.heading + 45.0f) * 3.14159265f / 180.0f;
            side = terrain_probe(s, x, z, s->units[i].y, x + sinf(ar) * L, z + cosf(ar) * L) ? -1 : 1;
        }
        accum += (float)side * 204.8f;
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
    s->avoid_next[i] = side == 0 ? 181.0f : 90.0f;
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
    return 0;
}

int msim_player_restart(msim *s)
{
    combat_unit *u = &s->player_unit;
    if (!u->shutdown || u->destroyed || (u->shutdown_pending && !u->override)) return 0;
    u->shutdown = 0; u->manual_down = 0;
    s->player_online_at = (float)s->now + (float)(0x43e + rnd_ms(&s->rng) % 0x16a) * 1000.0f / 182.0f;
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
        if (mech3d_mount_world(m, q, o, heading, p) == 0) { s->on_effect(s->shot_user, FX_JET, p); n++; }
    if (!n) msim_jet_flames(s, x, y, z, heading);
}

void msim_jet_flames(msim *s, float x, float y, float z, float heading)
{
    /* engine 0x1001bf90: a flame at each of the two jet mounts every tick while jetting; the mounts
     * are taken as 1.2 m either side of the feet (port) */
    float h = heading * 3.14159265f / 180.0f, side;
    if (!s->on_effect) return;
    for (side = -1; side <= 1; side += 2) {
        float p[3] = {x + cosf(h) * 120.0f * side, y + 50.0f, z - sinf(h) * 120.0f * side};
        s->on_effect(s->shot_user, FX_JET, p);
    }
}
