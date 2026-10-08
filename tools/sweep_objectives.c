/*
 * sweep_objectives - the objective sweep: every mission scene run headless by a "perfect player" that works
 * through table 0's objectives, with a report of every objective node and of what looks wrong.
 *
 *   sweep_objectives MW2.PRJ [-v] [-t max_seconds] [-nofire] [-installstars] [SCENE ...]
 *     -v             every node change and the sim's log
 *     -nofire        the player never fires (only the fallbacks destroy things)
 *     -installstars  use the install's USERSTAR / EN0nSTAR files instead of a fresh fixed lance (temp dir)
 *   env: MW2_SWEEP_TRACE=1 the player's state every second, MW2_SWEEP_BRG=1 the firing-position scores,
 *        MW2_SWEEP_ACTOR=n actor n's position / AI state every 5 s, MW2_SWEEP_STAR_SIZE=1..3 the player's star size
 *        (default: the mission's BRF2 SDSC default, as the campaign shell starts it; capped at the SDSC maximum)
 *
 * With no scene, every *SCN1 record is run, each in its own child process (crashes and hangs are caught: a
 * signal, a nonzero exit or the alarm). Set MW2_INSTALL_DIR to the install so USERSTAR / EN0nSTAR are read
 * (the harness never writes MW2MSN.CFG / MW2CAR.CFG).
 *
 * The player (invulnerable, unlimited ammunition, no heat) is teleported:
 *   reach / finish (0x20) / scan of a nav point  -> to the target (nav centre, group leader)
 *   destroy -> 120 m from each living target in turn and fires real projectiles at it (mechs: aimed at the
 *              unit; structures: along the aim line at the box); after 25 s without a kill the target is
 *              finished directly (combat_hit / msim_damage_building) and the fallback is reported
 *   scan    -> next to each target and msim_inspect() it
 *   protect -> attacks hostile mechs within 800 m of the protected targets
 *   leave   -> outside the target's radius (only when the node is wanted)
 * A node is acted on when it is "wanted" (a main / optional objective, a star-win for table 0, or what one of
 * those needs to succeed - items 'S' / 'C') and not "bad" (its success leads to a star-lose of table 0 or to
 * failing a main node). Others are left alone (e.g. "you strayed" leave nodes).
 *
 * Anomalies reported (ANOM lines): unresolved targets, targets that cannot be destroyed (unarmed actors,
 * indestructible structures), nodes still active although their targets are destroyed / reached / scanned,
 * fallbacks, nodes active at the end, mission failed / not finished.
 */
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "msim.h"
#include "bwd.h"
#include "datapath.h"
#include "lance.h"

#define DT_MS 50
#define PI_F 3.14159265f

static int g_verbose, g_nofire;
static const char *g_stars_dir;   /* own star files: USERSTAR is rewritten per scene with the mission's star size */
static lance_slot g_fr[3];
static float g_max_s = 1800.0f;

static void logline(void *u, const char *l) { (void)u; if (g_verbose) printf("    log %s\n", l); }

/* ---- target resolution ---------------------------------------------------------- */

typedef struct {
    int user;            /* UserStar */
    int nav;             /* index in s.navs, -1 */
    int actors, armed;   /* actors in the group, armed ones */
    int bld_all;         /* structures placed by the record (any class) */
    int bld_cls;         /* those whose widened class shares a bit with mask */
    int bld_indestr;     /* of those: hit points 0x7fffffff */
    int record;          /* the name is a record of the mission tree */
} target_info;

static unsigned widen(unsigned c) { if (c & 0x730) c |= 0x730; if (c & 3) c |= 3; return c; }

static const char *bld_rec(const msim *s, int b) { return s->world->parts[s->bld[b].intact].rec; }

static int nav_index(const msim *s, const char *t)
{
    int i;
    for (i = 0; i < s->nav_count; i++) if (strcasecmp(s->navs[i].name, t) == 0) return i;
    return -1;
}

static void resolve(const msim *s, const bwd_mission *bm, const char *t, unsigned mask, target_info *ti)
{
    int i;
    memset(ti, 0, sizeof *ti);
    ti->nav = -1;
    if (!t || !t[0]) return;
    ti->user = strcasecmp(t, "UserStar") == 0;
    ti->nav = nav_index(s, t);
    for (i = 0; i < s->actor_count; i++)
        if (strcasecmp(s->actors[i].group, t) == 0) { ti->actors++; if (s->armed[i]) ti->armed++; }
    for (i = 0; s->world && i < s->bld_count; i++) {
        if (strcasecmp(bld_rec(s, i), t) != 0) continue;
        ti->bld_all++;
        if (widen((unsigned)s->bld[i].type) & mask) { ti->bld_cls++; if (s->bld[i].hp == 0x7fffffff) ti->bld_indestr++; }
    }
    for (i = 0; bm && i < bm->record_count; i++) if (strcasecmp(bm->records[i].name, t) == 0) ti->record = 1;
}

static int has_target(const mtbl_node *n) { return n->target[0] && strcasecmp(n->target, "NULL") != 0; }

/* the harness's own view of the conditions (independent of msim's callbacks) */
static int all_destroyed(const msim *s, const char *t, int *members)
{
    int i, m = 0, d = 0;
    for (i = 0; i < s->actor_count; i++)
        if (strcasecmp(s->actors[i].group, t) == 0) { m++; if (s->armed[i] && s->units[i].destroyed) d++; }
    for (i = 0; s->world && i < s->bld_count; i++)
        if ((widen((unsigned)s->bld[i].type) & 3) && strcasecmp(bld_rec(s, i), t) == 0) { m++; if (s->bld[i].hp <= 0) d++; }
    if (members) *members = m;
    return m > 0 && d == m;
}

/* ---- wanted / bad nodes ------------------------------------------------------------ */

static void classify(const mtbl_logic *ml, int *wanted, int *bad)
{
    const mtbl_table *t0 = &ml->tables[0];
    int i, j, k, changed = 1;
    for (i = 0; i < t0->node_count; i++) {
        const mtbl_node *n = &t0->nodes[i];
        wanted[i] = n->objective == 'M' || n->objective == 'O' || (n->kind == MTBL_K_STAR_WIN && n->other_table == 0);
        bad[i] = (n->kind == MTBL_K_STAR_LOSE && n->other_table == 0) ||
                 (n->kind == MTBL_K_FAIL && n->other_table == 0 && n->other_node >= 0 && n->other_node < t0->node_count &&
                  t0->nodes[n->other_node].objective == 'M');
    }
    while (changed) {
        changed = 0;
        for (j = 0; j < t0->node_count; j++)
            for (k = 0; k < t0->nodes[j].item_count; k++) {
                const mtbl_item *it = &t0->nodes[j].items[k];
                if (it->table != 0 || it->node >= t0->node_count || (it->type != 'S' && it->type != 'C')) continue;
                if (wanted[j] && !wanted[it->node]) { wanted[it->node] = 1; changed = 1; }
                if (bad[j] && !bad[it->node]) { bad[it->node] = 1; changed = 1; }
            }
    }
}

/* ---- the run ---------------------------------------------------------------------- */

typedef struct {
    float  brg, dist;        /* where the player shoots from (chosen per target) */
    int    actor, bld;       /* the current destroy target, -1 */
    float  since;            /* seconds spent on it */
    int    last_health, aim;
    float  stagnant;         /* seconds without damage taking effect */
    int    clamped, start_health;
    float  waited;           /* seconds held back at the mission area's edge */
    float  held;             /* seconds of fire held for a protected structure in the line of fire */
    int    fallbacks;
} attack_state;

/* 2D: does the segment (x0,z0)-(x1,z1) cross structure b's box (slightly grown)? */
static int seg_box(const struct msim_building *g, float x0, float z0, float x1, float z1)
{
    float t0 = 0, t1 = 1, o[2] = {x0, z0}, d[2] = {x1 - x0, z1 - z0}, mn[2] = {g->mn[0] - 300, g->mn[2] - 300}, mx[2] = {g->mx[0] + 300, g->mx[2] + 300};
    int j;
    for (j = 0; j < 2; j++) {
        if (fabsf(d[j]) < 1e-3f) { if (o[j] < mn[j] || o[j] > mx[j]) return 0; continue; }
        {
            float ta = (mn[j] - o[j]) / d[j], tb = (mx[j] - o[j]) / d[j];
            if (ta > tb) { float q = ta; ta = tb; tb = q; }
            if (ta > t0) t0 = ta;
            if (tb < t1) t1 = tb;
            if (t0 > t1) return 0;
        }
    }
    return 1;
}

/* the bearing (of 16) from which a shot at (x, y, z) from dist crosses the fewest standing structures (skip: the target),
 * with the line of fire clear of the ground (msim_ground: terrain and the tops of standable boxes) and the player not
 * standing in an obstacle's footprint */
static float clear_bearing(const msim *s, float x, float y, float z, float dist0, int skip, float *dist_out)
{
    static const float DSCALE[4] = {1.0f, 0.6f, 0.3f, 1.7f};
    int k, b, q, best_n = 1 << 30;
    float best = 180.0f;
    *dist_out = dist0;
    for (k = 0; k < 64; k++) {
        float dist = dist0 * DSCALE[k / 16];
        float brg = 180.0f + (float)(k % 16) * 22.5f, r = brg * PI_F / 180.0f, px = x + sinf(r) * dist, pz = z + cosf(r) * dist;
        float py = msim_ground(s, px, pz, y + 1000.0f) + 800.0f;
        int n = 0;
        for (b = 0; b < s->bld_count; b++)
            if (b != skip && s->bld[b].hp > 0 && !s->world->parts[s->bld[b].intact].hidden && seg_box(&s->bld[b], px, pz, x, z)) n++;
        for (q = 0; q < 40; q++) {   /* the ground along the line, from the muzzle (a box top within 10 m of it counts) */
            float f = (float)q / 40.0f, lx = px + (x - px) * f, lz = pz + (z - pz) * f, ly = py + (y - py) * f;
            if (skip >= 0 && lx >= s->bld[skip].mn[0] - 200 && lx <= s->bld[skip].mx[0] + 200 && lz >= s->bld[skip].mn[2] - 200 && lz <= s->bld[skip].mx[2] + 200) break;
            if (f > 0.95f) break;
            if (msim_ground(s, lx, lz, ly) > ly) { n += 100; break; }
        }
        for (q = 0; q < s->obstacle_count; q++)
            if (px >= s->obstacles[q][0] - 600 && px <= s->obstacles[q][1] + 600 && pz >= s->obstacles[q][2] - 600 && pz <= s->obstacles[q][3] + 600) { n += 50; break; }
        if (getenv("MW2_SWEEP_BRG")) printf("    bearing %5.1f dist %6.0f score %d\n", brg, dist, n);
        if (n < best_n) { best_n = n; best = brg; *dist_out = dist; }
    }
    return best;
}

static void place_near(msim *s, float x, float z, float dist, float brg_deg)
{
    float b = brg_deg * PI_F / 180.0f;
    s->player[0] = x + sinf(b) * dist;
    s->player[2] = z + cosf(b) * dist;
}

static void aim_at(msim *s, float x, float y, float z)
{
    float dx = x - s->player[0], dz = z - s->player[2], dy = y - (s->player_unit.y + 700.0f);
    s->player_facing = s->player_aim_yaw = atan2f(dx, dz) * 180.0f / PI_F;
    s->player_aim_pitch = atan2f(dy, sqrtf(dx * dx + dz * dz)) * 180.0f / PI_F;
}

static void anom(int *count, const char *scene, const char *fmt, ...)
{
    va_list ap;
    printf("  ANOM %s: ", scene);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    (*count)++;
}

static const char *state_name(int st)
{
    switch (st) { case MTBL_WAITING: return "waiting"; case MTBL_ACTIVE: return "ACTIVE"; case MTBL_OK: return "ok"; case MTBL_FAILED: return "FAILED"; }
    return "?";
}

/* find the first living armed actor of a group */
static int live_member(const msim *s, const char *t)
{
    int i;
    for (i = 0; i < s->actor_count; i++)   /* never the player's own side (a dropship that has to leave, a starmate) */
        if (strcasecmp(s->actors[i].group, t) == 0 && s->armed[i] && !s->units[i].destroyed && !s->actors[i].friendly && s->actors[i].alliance != 0) return i;
    return -1;
}
static int live_bld(const msim *s, const char *t, unsigned mask)
{
    int i;
    for (i = 0; s->world && i < s->bld_count; i++)
        if ((widen((unsigned)s->bld[i].type) & mask) && s->bld[i].hp > 0 && s->bld[i].hp != 0x7fffffff && strcasecmp(bld_rec(s, i), t) == 0) return i;   /* indestructible: reported, not shot at */
    return -1;
}

/* the target's position: nav centre, group leader (first living member), or a structure's box centre */
static int target_xz(const msim *s, const char *t, float *x, float *z, float *r)
{
    int i = nav_index(s, t);
    *r = 20000.0f;
    if (i >= 0) { *x = s->navs[i].x; *z = s->navs[i].z; *r = s->navs[i].radius; return 0; }
    for (i = 0; i < s->actor_count; i++)
        if (strcasecmp(s->actors[i].group, t) == 0 && !(s->armed[i] && s->units[i].destroyed)) {
            *x = (float)s->actors[i].mech.origin[0]; *z = (float)s->actors[i].mech.origin[2]; return 0;
        }
    for (i = 0; s->world && i < s->bld_count; i++)
        if (strcasecmp(bld_rec(s, i), t) == 0) {
            *x = (s->bld[i].mn[0] + s->bld[i].mx[0]) * 0.5f; *z = (s->bld[i].mn[2] + s->bld[i].mx[2]) * 0.5f; return 0;
        }
    return -1;
}

/* the nearest hostile armed mech within 800 m of a protected thing, -1 */
static int protect_threat(const msim *s, const char *t)
{
    float px, pz, pr, bd = 80000.0f * 80000.0f;
    int k, e = -1;
    if (target_xz(s, t, &px, &pz, &pr) != 0) return -1;
    for (k = 0; k < s->actor_count; k++) {
        float dx = (float)s->actors[k].mech.origin[0] - px, dz = (float)s->actors[k].mech.origin[2] - pz;
        if (!s->armed[k] || s->units[k].destroyed || s->actors[k].friendly || s->actors[k].alliance != 1 ||
            strcasecmp(s->actors[k].group, t) == 0) continue;
        if (dx * dx + dz * dz < bd) { bd = dx * dx + dz * dz; e = k; }
    }
    return e;
}

/* the posed parts' centre and height range (cm); returns 1 if the centre is more than 15 m from the unit's own point
 * (origin, unit height + half the model) - a model drawn away from where the sim keeps the unit */
static int hull_centre(const msim *s, int e, float c[3], float *y0, float *y1)
{
    const mech3d *m = &s->actors[e].mech;
    int q, n = 0;
    float ux = (float)m->origin[0], uy = s->units[e].y + s->height[e] * 0.5f, uz = (float)m->origin[2], d2;
    c[0] = c[1] = c[2] = 0; *y0 = 1e9f; *y1 = -1e9f;
    for (q = 0; q < m->part_count; q++) {
        if (m->parts[q].model.object_count < 1) continue;
        c[0] += (float)m->parts[q].pos[0]; c[1] += (float)m->parts[q].pos[1]; c[2] += (float)m->parts[q].pos[2]; n++;
        if ((float)m->parts[q].pos[1] < *y0) *y0 = (float)m->parts[q].pos[1];
        if ((float)m->parts[q].pos[1] > *y1) *y1 = (float)m->parts[q].pos[1];
    }
    if (!n) { c[0] = ux; c[1] = uy; c[2] = uz; *y0 = s->units[e].y; *y1 = s->units[e].y + s->height[e]; return 0; }
    c[0] /= (float)n; c[1] /= (float)n; c[2] /= (float)n;
    d2 = (c[0] - ux) * (c[0] - ux) + (c[1] - uy) * (c[1] - uy) + (c[2] - uz) * (c[2] - uz);
    if (d2 <= 1500.0f * 1500.0f) { *y0 = s->units[e].y; *y1 = s->units[e].y + s->height[e]; return 0; }
    *y1 += 200.0f;
    return 1;
}

static int run_scene(prj_archive *a, const char *scene)
{
    world_actor *ac = NULL;
    int n = 0, i, step, steps, nanom = 0, wanted[MTBL_MAX_NODES], bad[MTBL_MAX_NODES];
    static msim s;
    static mech3d world;
    bwd_mission bm;
    int have_bm;
    int32_t stuck_since[MTBL_MAX_NODES];   /* ms: the node's condition is met (by our test) but it is still active */
    int reported[MTBL_MAX_NODES], clamp_noted[MTBL_MAX_NODES], gave_up[MTBL_MAX_NODES];
    float worked[MTBL_MAX_NODES];
    static char displaced_noted[256];
    attack_state at;
    int idle_steps = 0, last_change_step = 0;
    float last_px = 0, last_pz = 0, place_y_ref = 0;
    uint8_t prev_state[MTBL_MAX_NODES];
    const char *why_stop = "time cap";

    memset(&at, 0, sizeof at);
    memset(displaced_noted, 0, sizeof displaced_noted);
    at.brg = 180.0f; at.actor = at.bld = -1;
    printf("=== %s\n", scene);
    if (g_stars_dir) {   /* the campaign shell writes the star at the mission's size (BRF2 SDSC +8 default, capped at 3,
                          * FUN_000291b0 -> 0x0003ab50): a mate beyond it has no formation slot (RUST's "heyyou" has one) */
        char brf[16], path[512];
        prj_record r;
        int size = 3, maxsz = 3;
        snprintf(brf, sizeof brf, "%.4sBRF2", scene);
        if (prj_read_named(a, "BWD", brf, &r) == PRJ_OK) {
            int nc = bwd_chunks(r.data, r.size, NULL, 0), q;
            bwd_chunk *cc = nc > 0 ? calloc((size_t)nc, sizeof *cc) : NULL;
            if (cc) {
                bwd_chunks(r.data, r.size, cc, nc);
                for (q = 0; q < nc; q++)
                    if (strcmp(cc[q].tag, "SDSC") == 0 && cc[q].size >= 16) { size = cc[q].data[8] | cc[q].data[9] << 8; maxsz = cc[q].data[12] | cc[q].data[13] << 8; break; }
                free(cc);
            }
            prj_record_free(&r);
        }
        if (getenv("MW2_SWEEP_STAR_SIZE")) {   /* override (e.g. 3), capped at the mission's maximum (SDSC +12) as the shell's
                                                * Star Configuration caps it - RUST takes 2 ("heyyou" has one follower slot) */
            size = atoi(getenv("MW2_SWEEP_STAR_SIZE"));
            if (maxsz >= 1 && size > maxsz) size = maxsz;
        }
        if (size < 1) size = 1;
        if (size > 3) size = 3;
        snprintf(path, sizeof path, "%s/USERSTAR.BWD", g_stars_dir);
        lance_write_star(a, path, g_fr, size, 0, 0);
        printf("  star size %d (BRF2 SDSC)\n", size);
    }
    if (world3d_actors(a, scene, &ac, &n) != 0) { printf("  ERROR world3d_actors failed\n"); return 1; }
    if (msim_init(&s, a, scene, ac, n) != 0) { printf("  ERROR msim_init failed\n"); world3d_free_actors(ac, n); return 1; }
    memset(&world, 0, sizeof world);
    if (world3d_mission(a, scene, 1, &world) > 0) msim_set_world(&s, &world);
    have_bm = bwd_mission_load(a, scene, &bm) == 0;
    s.player_vertical = 1;
    s.log = logline;
    s.debug_invulnerable = 1;
    s.player_unit.invulnerable = 1;
    s.player_unit.unlimited_ammo = 1;
    s.player_unit.no_heat = 1;
    {
        int fr = 0, en = 0, ua = 0;
        for (i = 0; i < n; i++) { if (ac[i].friendly) fr++; else en++; if (!s.armed[i]) ua++; }
        printf("  %d tables, %d nodes in table 0, %d navs, %d actors (%d friendly, %d other, %d unarmed), %d structures, time limit %d s, player %s\n",
               s.logic.table_count, s.logic.table_count ? s.logic.tables[0].node_count : 0, s.nav_count, n, fr, en, ua, s.bld_count,
               s.logic.table_count ? (int)(int32_t)s.logic.tables[0].time_limit : 0, s.player_loadout);
    }
    {   /* spawn overlap at t = 0 (training report: a second Timber Wolf stood inside the player): two units within 3 m of
         * each other (a note), or a unit within 10 m of the player (an anomaly); and the mission tree's player GPSs (+0x0e == 0, engine
         * 0x1003f36b) - exactly one, or none (then msim falls back to USERSTAR.BWD on disk) */
        int j, nplayer = 0;
        if (have_bm) {
            int r, c;
            for (r = 0; r < bm.record_count; r++)
                for (c = 0; c < bm.records[r].chunk_count; c++) {
                    bwd_gps g;
                    if (bwd_gps_decode(&bm.records[r].chunks[c], &g) == 0 && g.is_player) nplayer++;
                }
            if (nplayer > 1) anom(&nanom, scene, "spawn: %d player GPS records (+0x0e == 0) in the mission tree", nplayer);
        }
        for (i = 0; i < n; i++) {
            float dx = (float)ac[i].mech.origin[0] - s.player[0], dz = (float)ac[i].mech.origin[2] - s.player[2];
            if (dx * dx + dz * dz < 1000.0f * 1000.0f)
                anom(&nanom, scene, "spawn: actor %d %s (%s, %s) starts %.1f m from the player", i, ac[i].name, ac[i].group, ac[i].skel,
                     (double)sqrtf(dx * dx + dz * dz) / 100.0);
            for (j = i + 1; j < n; j++) {
                float ex = (float)(ac[i].mech.origin[0] - ac[j].mech.origin[0]), ez = (float)(ac[i].mech.origin[2] - ac[j].mech.origin[2]);
                float ey = (float)(ac[i].mech.origin[1] - ac[j].mech.origin[1]);
                /* AI units on one spot are the mission data's own placement, as the engine does it: every table's
                 * members take the formation slot of their order in the table (0x1003f480 -> 0x10018cd0), offsets
                 * relative to the leader's slot (0x10018d00), and FORMATNS "star" has two followers - a fourth member
                 * (TEALENS4) gets the zero slot 3; Instant Action arenas put two enemy stars' start navs on one point
                 * (COLB 1 / 5, SWIS 3 / 4). A note, not an anomaly; any unit on the player is one */
                if (ex * ex + ez * ez < 300.0f * 300.0f && fabsf(ey) < 1000.0f)
                    printf("  note: spawn: actors %d %s (%s) and %d %s (%s) start %.1f m apart (mission data)\n", i, ac[i].name, ac[i].group, j,
                           ac[j].name, ac[j].group, (double)sqrtf(ex * ex + ez * ez) / 100.0);
            }
        }
        printf("  spawn check: %d player GPS, %d actors\n", nplayer, n);
    }
    if (s.logic.table_count == 0) { printf("  ERROR no MTBL tables\n"); goto done; }
    classify(&s.logic, wanted, bad);

    /* static checks: every table 0 node's target */
    for (i = 0; i < s.logic.tables[0].node_count; i++) {
        const mtbl_node *nd = &s.logic.tables[0].nodes[i];
        target_info ti;
        unsigned mask = nd->kind == MTBL_K_PROTECT ? 4 : nd->kind == MTBL_K_SCAN ? 8 : (nd->kind & 3) ? 3 : nd->kind;
        stuck_since[i] = -1; reported[i] = 0; prev_state[i] = 0; clamp_noted[i] = 0; gave_up[i] = 0; worked[i] = 0;
        if (!has_target(nd)) continue;
        if (nd->kind & 0xFFFF0000u) continue;   /* table-control kinds: no target list */
        if (nd->kind == MTBL_K_START || nd->kind == MTBL_K_TIMER || nd->kind == MTBL_K_TIMER2 || nd->kind == MTBL_K_INSTANT ||
            nd->kind == MTBL_K_ORDER || nd->kind == MTBL_K_HOLD) continue;
        resolve(&s, have_bm ? &bm : NULL, nd->target, mask, &ti);
        if (!ti.user && ti.nav < 0 && !ti.actors && !ti.bld_all)
            anom(&nanom, scene, "node %d %s target %s resolves to nothing (%s)%s", i, mtbl_kind_name(nd->kind), nd->target,
                 ti.record ? "a record of the mission tree with no nav, spawn or structure" : "no such record in the mission",
                 (nd->kind == MTBL_K_DESTROY1 || nd->kind == MTBL_K_DESTROY2 || nd->kind == MTBL_K_SCAN || nd->kind == MTBL_K_ALLMAIN || nd->kind == MTBL_K_PROTECT || nd->kind == MTBL_K_REACH)
                     ? " - engine: an empty target list passes the test at once (0x1000a1d0 bVar4 stays true)" : "");
        if ((nd->kind == MTBL_K_DESTROY1 || nd->kind == MTBL_K_DESTROY2) && !ti.user) {
            if (ti.actors && ti.armed < ti.actors)
                anom(&nanom, scene, "node %d destroy %s: %d of %d group members are unarmed (no loadout) - msim counts them but can never destroy them",
                     i, nd->target, ti.actors - ti.armed, ti.actors);
            if (!ti.actors && ti.bld_all && !ti.bld_cls)
                anom(&nanom, scene, "node %d destroy %s: %d structures, none of a destroy class (widened class & 3)", i, nd->target, ti.bld_all);
            if (ti.bld_indestr)
                anom(&nanom, scene, "node %d destroy %s: %d of %d destroy-class structures are indestructible (hp 0x7fffffff)", i, nd->target, ti.bld_indestr, ti.bld_cls);
            if (!ti.actors && !ti.bld_all && ti.nav >= 0)
                anom(&nanom, scene, "node %d destroy %s names a nav point (engine 0x10009a50: a nav is never destroyed)", i, nd->target);
        }
        if (nd->kind == MTBL_K_SCAN && !ti.actors && !ti.bld_cls && ti.nav < 0 && ti.bld_all)
            anom(&nanom, scene, "node %d scan %s: %d structures, none of scan class 8", i, nd->target, ti.bld_all);
        if ((nd->kind == MTBL_K_SCAN || nd->kind == MTBL_K_ALLMAIN) && ti.nav >= 0 && !ti.actors && !ti.bld_all)
            printf("  note: node %d %s targets nav %s (engine: visited flag 0x20 set by 0x10009b80 / 0x1001d510 when the player is in its radius)\n",
                   i, mtbl_kind_name(nd->kind), nd->target);
        if ((nd->kind == MTBL_K_REACH || nd->kind == MTBL_K_LEAVE) && ti.nav < 0 && !ti.actors && !ti.user && ti.bld_all)
            anom(&nanom, scene, "node %d %s %s targets structures (engine 0x10009b80 type 4: within 200 m) - msim's within() only knows navs and groups",
                 i, mtbl_kind_name(nd->kind), nd->target);
    }

    steps = (int)(g_max_s * 1000.0f / DT_MS);
    for (step = 0; step < steps; step++) {
        const mtbl_table *t0 = &s.logic.tables[0];
        int best = -1, k;
        int32_t best_t = 0x7fffffff;
        float now_s = (float)s.now / 1000.0f;
        /* the next node to work on: the earliest activated, wanted, not bad, active one with something to do */
        for (k = 0; k < t0->node_count; k++) {
            const mtbl_node *nd = &t0->nodes[k];
            int doable = 0;
            if (s.logic.state[0][k] != MTBL_ACTIVE || !wanted[k] || (bad[k] && nd->objective != 'M') || !has_target(nd) || gave_up[k]) continue;
            switch (nd->kind) {
            case MTBL_K_REACH: doable = 1; break;
            case MTBL_K_ALLMAIN: {   /* only once every other main objective is done */
                int j;
                doable = 1;
                for (j = 0; j < t0->node_count; j++) if (j != k && t0->nodes[j].objective == 'M' && s.logic.state[0][j] != MTBL_OK) doable = 0;
                break;
            }
            case MTBL_K_LEAVE: doable = nd->objective == 'M' || (nd->objective == 'N' && !bad[k]); break;   /* 'O' leave nodes are "going too far" warnings */
            case MTBL_K_DESTROY1: case MTBL_K_DESTROY2: doable = live_member(&s, nd->target) >= 0 || live_bld(&s, nd->target, 3) >= 0; break;
            case MTBL_K_SCAN: {
                int q;
                for (q = 0; q < s.actor_count && !doable; q++) if (strcasecmp(s.actors[q].group, nd->target) == 0 && !s.inspected[q]) doable = 1;
                for (q = 0; q < s.bld_count && !doable; q++) if ((widen((unsigned)s.bld[q].type) & 8) && !s.bld[q].inspected && strcasecmp(bld_rec(&s, q), nd->target) == 0) doable = 1;
                if (!doable && nav_index(&s, nd->target) >= 0) doable = 1;
                break;
            }
            case MTBL_K_PROTECT: doable = protect_threat(&s, nd->target) >= 0; break;
            default: break;
            }
            {   /* protecting comes after everything else */
                int32_t key = s.logic.started[0][k] + (nd->kind == MTBL_K_PROTECT ? 100000000 : 0);
                if (doable && key < best_t) { best_t = key; best = k; }
            }
        }
        s.player_attacks = 0;
        s.player_target = -1;
        s.player_aim_free = 0;
        place_y_ref = s.player_unit.y;
        if (best >= 0) {
            const mtbl_node *nd = &t0->nodes[best];
            worked[best] += DT_MS / 1000.0f;
            if (worked[best] > 60.0f && (nd->kind == MTBL_K_REACH || nd->kind == MTBL_K_ALLMAIN || nd->kind == MTBL_K_LEAVE || nd->kind == MTBL_K_SCAN)) {
                gave_up[best] = 1;   /* done what it asks for a minute: move on (the stuck test reports it) */
                printf("  note: node %d %s %s: still active after 60 s at its target - moving on\n", best, mtbl_kind_name(nd->kind), nd->target);
            }
            float x, z, r;
            idle_steps = 0;
            switch (nd->kind) {
            case MTBL_K_REACH: case MTBL_K_ALLMAIN:
                if (target_xz(&s, nd->target, &x, &z, &r) == 0) { s.player[0] = x; s.player[2] = z; }
                { int ni = nav_index(&s, nd->target); if (ni >= 0) { s.player_nav = ni; place_y_ref = s.navs[ni].y; } }   /* the reach test is 3D (0x10009b80) */
                break;
            case MTBL_K_LEAVE:
                if (target_xz(&s, nd->target, &x, &z, &r) == 0) {
                    float dx = s.player[0] - x, dz = s.player[2] - z, d = sqrtf(dx * dx + dz * dz);
                    if (d < 1) { dx = 1; dz = 0; d = 1; }
                    if (d <= r) { s.player[0] = x + dx / d * (r * 1.05f + 2000.0f); s.player[2] = z + dz / d * (r * 1.05f + 2000.0f); }
                }
                break;
            case MTBL_K_SCAN: {
                int q, done = 0;
                for (q = 0; q < s.actor_count && !done; q++)
                    if (strcasecmp(s.actors[q].group, nd->target) == 0 && !s.inspected[q]) {
                        place_near(&s, (float)s.actors[q].mech.origin[0], (float)s.actors[q].mech.origin[2], 5000.0f, 180.0f);
                        if (msim_inspect(&s, q, -1) != 1) anom(&nanom, scene, "node %d scan: msim_inspect(actor %d %s) from 50 m failed", best, q, s.actors[q].name);
                        done = 1;
                    }
                for (q = 0; q < s.bld_count && !done; q++)
                    if ((widen((unsigned)s.bld[q].type) & 8) && !s.bld[q].inspected && strcasecmp(bld_rec(&s, q), nd->target) == 0) {
                        place_near(&s, (s.bld[q].mn[0] + s.bld[q].mx[0]) * 0.5f, (s.bld[q].mn[2] + s.bld[q].mx[2]) * 0.5f, 5000.0f, 180.0f);
                        if (msim_inspect(&s, -1, q) != 1) anom(&nanom, scene, "node %d scan: msim_inspect(structure %d %s) failed", best, q, s.bld[q].sub);
                        done = 1;
                    }
                if (!done && target_xz(&s, nd->target, &x, &z, &r) == 0) { s.player[0] = x; s.player[2] = z; s.player_nav = nav_index(&s, nd->target); }
                break;
            }
            case MTBL_K_DESTROY1: case MTBL_K_DESTROY2: case MTBL_K_PROTECT: {
                int e = -1, b = -1;
                if (nd->kind == MTBL_K_PROTECT) e = protect_threat(&s, nd->target);
                else {
                    e = live_member(&s, nd->target);
                    if (e < 0) b = live_bld(&s, nd->target, 3);
                }
                if ((e >= 0 && e != at.actor) || (b >= 0 && b != at.bld) || (e < 0 && b < 0)) {
                    at.actor = e; at.bld = b; at.since = 0; at.aim = 0; at.stagnant = 0; at.last_health = -1; at.waited = 0; at.held = 0;
                    at.start_health = e >= 0 ? combat_health(&s.units[e]) : b >= 0 ? s.bld[b].hp : 0;
                    if (g_verbose && b >= 0)
                        printf("  %7.1fs target structure %d %s (%s) rec %s type %#x hp %d box %.0f..%.0f, %.0f..%.0f, %.0f..%.0f coll %d hidden %d\n", now_s, b,
                               s.bld[b].name, s.bld[b].sub, bld_rec(&s, b), s.bld[b].type, s.bld[b].hp, s.bld[b].mn[0], s.bld[b].mx[0], s.bld[b].mn[1], s.bld[b].mx[1],
                               s.bld[b].mn[2], s.bld[b].mx[2], s.world->parts[s.bld[b].intact].coll, s.world->parts[s.bld[b].intact].hidden);
                    if (e >= 0) at.brg = clear_bearing(&s, (float)s.actors[e].mech.origin[0], s.units[e].y + s.height[e] * 0.55f, (float)s.actors[e].mech.origin[2], 12000.0f, -1, &at.dist);
                    else if (b >= 0) at.brg = clear_bearing(&s, (s.bld[b].mn[0] + s.bld[b].mx[0]) * 0.5f, fminf((s.bld[b].mn[1] + s.bld[b].mx[1]) * 0.5f, s.bld[b].mn[1] + 500.0f), (s.bld[b].mn[2] + s.bld[b].mx[2]) * 0.5f,
                                                            12000.0f + sqrtf((s.bld[b].mx[0] - s.bld[b].mn[0]) * (s.bld[b].mx[0] - s.bld[b].mn[0]) + (s.bld[b].mx[2] - s.bld[b].mn[2]) * (s.bld[b].mx[2] - s.bld[b].mn[2])) * 0.5f, b, &at.dist);
                }
                if (at.clamped) at.waited += DT_MS / 1000.0f;   /* outside the mission area: it has to come to us */
                else at.since += DT_MS / 1000.0f;
                if (at.waited > 60.0f) at.since = 1e9f;
                if (e >= 0) {
                    static const float AIM_H[4] = {0.55f, 0.8f, 0.4f, 0.95f};
                    int hl = combat_health(&s.units[e]);
                    if (hl != at.last_health) { at.last_health = hl; at.stagnant = 0; }
                    else if ((at.stagnant += DT_MS / 1000.0f) > 4.0f) {   /* hits that do nothing (a gone location): another side, another height */
                        at.stagnant = 0; at.aim++; at.brg += 90.0f;
                    }
                    float hc[3], hy0, hy1;
                    int displaced = hull_centre(&s, e, hc, &hy0, &hy1);
                    if (displaced && !displaced_noted[e < 256 ? e : 255]) {
                        displaced_noted[e < 256 ? e : 255] = 1;
                        anom(&nanom, scene, "actor %d %s (%s, skeleton %s): its parts are posed around %.0f,%.0f,%.0f, %.0f m from its origin %d,%.0f,%d - shots at the unit miss; aiming at the parts",
                             e, s.actors[e].name, s.actors[e].group, s.actors[e].skel, hc[0], hc[1], hc[2],
                             sqrtf((hc[0] - (float)s.actors[e].mech.origin[0]) * (hc[0] - (float)s.actors[e].mech.origin[0]) + (hc[1] - s.units[e].y) * (hc[1] - s.units[e].y) +
                                   (hc[2] - (float)s.actors[e].mech.origin[2]) * (hc[2] - (float)s.actors[e].mech.origin[2])) / 100.0f,
                             s.actors[e].mech.origin[0], s.units[e].y, s.actors[e].mech.origin[2]);
                    }
                    place_near(&s, hc[0], hc[2], at.dist, at.brg);
                    place_y_ref = displaced ? hy0 : s.units[e].y;
                    aim_at(&s, hc[0], hy0 + (hy1 - hy0) * AIM_H[at.aim & 3], hc[2]);
                    s.player_target = e; s.player_attacks = 1;
                    if (at.aim || displaced) { s.player_aim_free = 1; s.player_target = -1; }   /* along the aim line at the chosen height */
                    if (at.since > 25.0f + (float)at.start_health / 40.0f) {   /* fallback: hits through the combat API until it goes */
                        int guard = 0, w0 = s.player_unit.weapon_count > 0 ? s.player_unit.weapons[0].weapon : 0;
                        anom(&nanom, scene, "node %d %s: %s (%s, actor %d) not destroyed by projectiles (health %d of %d)%s - finished with combat_hit",
                             best, mtbl_kind_name(nd->kind), s.actors[e].name, s.actors[e].group, e, combat_health(&s.units[e]), at.start_health,
                             at.waited > 60.0f ? (s.actors[e].have_anim ? " - stayed outside the mission area 60 s" : " - stayed outside the mission area 60 s (a vehicle: msim_step never moves actors without walk animations)") : "");
                        while (!s.units[e].destroyed && guard++ < 5000) combat_hit(&s.units[e], w0, 3, 0.0f, &s.rng);
                        if (!s.units[e].destroyed) anom(&nanom, scene, "  actor %d survives 5000 combat_hit calls", e);
                        at.since = 0; at.fallbacks++;
                    }
                } else if (b >= 0) {
                    const struct msim_building *g = &s.bld[b];
                    float c[3] = {(g->mn[0] + g->mx[0]) * 0.5f, (g->mn[1] + g->mx[1]) * 0.5f, (g->mn[2] + g->mx[2]) * 0.5f};
                    if (c[1] > g->mn[1] + 500.0f) c[1] = g->mn[1] + 500.0f;
                    place_near(&s, c[0], c[2], at.dist, at.brg);
                    place_y_ref = g->mn[1];
                    {   /* the aim swings sideways every 2 s (centre, right, left): with nothing of class 0x300 on the ray the
                         * convergence stays far out (0x10044847), so arm mounts wide of the eye (a Firemoth's at +-2.8 m) pass
                         * either side of a small target such as a 5.7 m training sphere aimed at its centre */
                        static const float SIDE[3] = {0.0f, 1.0f, -1.0f};
                        float bb = at.brg * PI_F / 180.0f, off = SIDE[((int)(at.since / 2.0f)) % 3] * 250.0f;
                        aim_at(&s, c[0] - cosf(bb) * off, c[1], c[2] + sinf(bb) * off);
                    }
                    s.player_aim_free = 1; s.player_attacks = 1;
                    if (at.since > 25.0f + (float)at.start_health / 40.0f) {
                        anom(&nanom, scene, "node %d %s: structure %s (%s) of %s not destroyed by projectiles (hp %d of %d)%s - finished with msim_damage_building",
                             best, mtbl_kind_name(nd->kind), g->name, g->sub, nd->target, g->hp, at.start_health, at.waited > 60.0f ? " - outside the mission area" : "");
                        msim_damage_building(&s, b, (float)g->hp + 1.0f);
                        at.since = 0; at.fallbacks++;
                    }
                }
                break;
            }
            default: break;
            }
        } else idle_steps++;

        /* stay inside the mission area: every leave node of table 0 that is not itself the goal (the "going too far"
         * warnings and the star-lose ones) - enemies outside it come to the player */
        at.clamped = 0;
        if (!(best >= 0 && t0->nodes[best].kind == MTBL_K_LEAVE)) {
            for (k = 0; k < t0->node_count; k++) {
                float x, z, r, dx, dz, d;
                if (t0->nodes[k].kind != MTBL_K_LEAVE || !bad[k] || t0->nodes[k].objective == 'M' || !has_target(&t0->nodes[k]) ||
                    target_xz(&s, t0->nodes[k].target, &x, &z, &r) != 0) continue;
                dx = s.player[0] - x; dz = s.player[2] - z; d = sqrtf(dx * dx + dz * dz);
                if (d > r * 0.9f && d > 0) {
                    s.player[0] = x + dx / d * r * 0.9f; s.player[2] = z + dz / d * r * 0.9f;
                    at.clamped = 1;
                    if (best >= 0 && !clamp_noted[best]) {
                        clamp_noted[best] = 1;
                        printf("  note: node %d %s %s: target outside the mission area (leave node %d, %s): the player waits at its edge\n",
                               best, mtbl_kind_name(t0->nodes[best].kind), t0->nodes[best].target, k, t0->nodes[k].target);
                    }
                }
            }
        }
        if (s.player_attacks && best >= 0) {   /* hold fire when a protected structure or unit is on or near the line of fire */
            float tx, tz;
            int b2, q;
            if (s.player_target >= 0) { tx = (float)s.actors[s.player_target].mech.origin[0]; tz = (float)s.actors[s.player_target].mech.origin[2]; }
            else { tx = s.player[0] + sinf(s.player_aim_yaw * PI_F / 180.0f) * 20000.0f; tz = s.player[2] + cosf(s.player_aim_yaw * PI_F / 180.0f) * 20000.0f; }
            {   /* 40 m beyond the target too */
                float dx = tx - s.player[0], dz = tz - s.player[2], d = sqrtf(dx * dx + dz * dz);
                if (d > 1) { tx += dx / d * 4000.0f; tz += dz / d * 4000.0f; }
            }
            for (b2 = 0; b2 < n && s.player_attacks; b2++) {   /* check fire: a friendly unit within 15 m of the line of fire,
                                                                    * 600 m long (misses fly on past the target) */
                float ux = (float)s.actors[b2].mech.origin[0] - s.player[0], uz = (float)s.actors[b2].mech.origin[2] - s.player[2];
                float lx = tx - s.player[0], lz = tz - s.player[2], ll = lx * lx + lz * lz, u, ex, ez;
                if (ll >= 1) { float k = 60000.0f / sqrtf(ll); lx *= k; lz *= k; ll = 60000.0f * 60000.0f; }
                if (b2 == s.player_target || !s.armed[b2] || s.units[b2].destroyed || !(s.actors[b2].friendly || s.actors[b2].alliance == 0) || ll < 1) continue;
                u = (ux * lx + uz * lz) / ll; u = u < 0 ? 0 : u > 1 ? 1 : u;
                ex = ux - u * lx; ez = uz - u * lz;
                if (ex * ex + ez * ez < 1500.0f * 1500.0f) {
                    s.player_attacks = 0;
                    if (at.since > 2.0f) { at.brg += 67.5f; at.since -= 1.0f; }   /* try another side */
                    if ((at.held += DT_MS / 1000.0f) > 40.0f) { at.since = 1e9f; at.held = 0; if (g_verbose) printf("    fire held 40 s for a friendly unit\n"); }
                }
            }
            for (q = 0; q < t0->node_count && s.player_attacks; q++) {
                if (t0->nodes[q].kind != MTBL_K_PROTECT || s.logic.state[0][q] != MTBL_ACTIVE || !has_target(&t0->nodes[q]) || !wanted[q] ||
                    strcasecmp(t0->nodes[q].target, t0->nodes[best].target) == 0) continue;   /* (a record both to destroy and to protect: destroy wins) */
                for (b2 = 0; b2 < s.bld_count && s.player_attacks; b2++) {
                    struct msim_building g = s.bld[b2];
                    if (g.hp <= 0 || !(widen((unsigned)g.type) & 4) || strcasecmp(bld_rec(&s, b2), t0->nodes[q].target) != 0) continue;
                    g.mn[0] -= 2000; g.mn[2] -= 2000; g.mx[0] += 2000; g.mx[2] += 2000;
                    if (seg_box(&g, s.player[0], s.player[2], tx, tz)) {
                        s.player_attacks = 0;
                        if (at.since > 2.0f) { at.brg += 67.5f; at.since -= 1.0f; }   /* try another side */
                        /* every side passes the protected structure (GOLDENS4 fighting inside the palace complex): after
                         * 40 s of held fire the target is finished by the fallback (reported) */
                        if ((at.held += DT_MS / 1000.0f) > 40.0f) { at.since = 1e9f; at.held = 0; if (g_verbose) printf("    fire held 40 s for %s\n", t0->nodes[q].target); }
                        break;
                    }
                }
            }
        }
        if (g_nofire) s.player_attacks = 0;
        if (fabsf(s.player[0] - last_px) > 100.0f || fabsf(s.player[2] - last_pz) > 100.0f) {
            /* teleported: stand on the surface at about the target's level (a mesa top when the target is up there, not
             * inside the mesa; the canyon floor when it is down there) */
            s.player_unit.y = msim_ground(&s, s.player[0], s.player[2], place_y_ref + 1000.0f);
            s.player_unit.vy = 0;
        }
        last_px = s.player[0]; last_pz = s.player[2];
        if (getenv("MW2_SWEEP_ACTOR") && step % 100 == 0) {   /* tests: one actor's position every 5 s */
            int q = atoi(getenv("MW2_SWEEP_ACTOR"));
            if (q >= 0 && q < n) printf("  actor %d %s at %d,%d dead %d state %d target %d cmd %d cmdt %d\n", q, s.actors[q].group, s.actors[q].mech.origin[0], s.actors[q].mech.origin[2], s.units[q].destroyed,
                                        s.minds ? s.minds[q].state : -1, s.minds ? s.minds[q].target : -1, s.cmd ? s.cmd[q] : -1, s.cmd_target ? s.cmd_target[q] : -9);
        }
        if (getenv("MW2_SWEEP_TRACE") && step % 20 == 0)
            printf("  trace %6.1fs best %d player %.0f,%.0f y %.0f attacks %d target %d free %d aim %.1f/%.1f shots %d hits %d misses %d\n", now_s, best,
                   s.player[0], s.player[2], s.player_unit.y, s.player_attacks, s.player_target, s.player_aim_free, s.player_aim_yaw, s.player_aim_pitch,
                   s.shot_count, s.hits, s.misses);
        msim_step(&s, DT_MS);

        /* bookkeeping: state changes, met-but-active conditions */
        for (k = 0; k < t0->node_count; k++) {
            const mtbl_node *nd = &t0->nodes[k];
            int met = 0, mem = 0;
            if (s.logic.state[0][k] != prev_state[k]) {
                if (g_verbose) printf("  %7.1fs node %2d %-9s %-8s %s\n", now_s, k, mtbl_kind_name(nd->kind), nd->target, state_name(s.logic.state[0][k]));
                prev_state[k] = s.logic.state[0][k];
                worked[k] = 0; gave_up[k] = 0;
                last_change_step = step;
            }
            if (s.logic.state[0][k] != MTBL_ACTIVE || !has_target(nd)) { stuck_since[k] = -1; continue; }
            switch (nd->kind) {
            case MTBL_K_DESTROY1: case MTBL_K_DESTROY2: met = all_destroyed(&s, nd->target, &mem); break;
            case MTBL_K_REACH: {
                float x, z, r;
                if (target_xz(&s, nd->target, &x, &z, &r) == 0) {
                    float dx = s.player[0] - x, dz = s.player[2] - z;
                    met = dx * dx + dz * dz <= r * r * 0.9f;
                }
                break;
            }
            case MTBL_K_SCAN: {
                int q, m2 = 0, d2 = 0;
                for (q = 0; q < s.actor_count; q++) if (strcasecmp(s.actors[q].group, nd->target) == 0) { m2++; if (s.inspected[q]) d2++; }
                for (q = 0; q < s.bld_count; q++) if ((widen((unsigned)s.bld[q].type) & 8) && strcasecmp(bld_rec(&s, q), nd->target) == 0) { m2++; if (s.bld[q].inspected) d2++; }
                met = m2 > 0 && d2 == m2;
                if (!m2 && nav_index(&s, nd->target) >= 0) {
                    const msim_nav *nv = &s.navs[nav_index(&s, nd->target)];
                    float dx = s.player[0] - nv->x, dz = s.player[2] - nv->z;
                    met = dx * dx + dz * dz <= nv->radius * nv->radius * 0.9f;
                }
                break;
            }
            case MTBL_K_ALLMAIN: {
                int j, others = 1;
                float x, z, r;
                for (j = 0; j < t0->node_count; j++) if (j != k && t0->nodes[j].objective == 'M' && s.logic.state[0][j] != MTBL_OK) others = 0;
                met = others && target_xz(&s, nd->target, &x, &z, &r) == 0 &&
                      (s.player[0] - x) * (s.player[0] - x) + (s.player[2] - z) * (s.player[2] - z) <= r * r * 0.9f;
                break;
            }
            default: break;
            }
            if (!met) { stuck_since[k] = -1; continue; }
            if (stuck_since[k] < 0) stuck_since[k] = s.now;
            else if (s.now - stuck_since[k] > 5000 && !reported[k]) {
                reported[k] = 1;
                gave_up[k] = 1;
                anom(&nanom, scene, "node %d %s %s: condition met for 5 s (by the harness's test) but the node is still active", k,
                     mtbl_kind_name(nd->kind), nd->target);
            }
        }
        if (s.ending) { why_stop = "mission ended"; break; }
        /* nothing to do and nothing changed for 10 minutes with no table 0 timer due: stalled */
        if (idle_steps > 0 && step - last_change_step > 600 * 1000 / DT_MS) {
            int timer_due = 0;
            for (k = 0; k < t0->node_count; k++)
                if (s.logic.state[0][k] == MTBL_ACTIVE && (int32_t)t0->nodes[k].delay >= 0 &&
                    s.logic.started[0][k] / 1000 + (int32_t)t0->nodes[k].delay < (int32_t)(g_max_s)) timer_due = 1;
            if (!timer_due) { why_stop = "stalled (nothing to do, nothing changes)"; break; }
        }
    }

    /* the report */
    printf("  stop: %s at %.1f s; outcome %s%s\n", why_stop, (double)s.now / 1000.0,
           msim_outcome_text(&s) ? msim_outcome_text(&s) : "none", s.player_lost ? " (player lost)" : "");
    for (i = 0; i < s.logic.tables[0].node_count; i++) {
        const mtbl_node *nd = &s.logic.tables[0].nodes[i];
        int st = s.logic.state[0][i];
        char items[80] = "";
        int k2;
        for (k2 = 0; k2 < nd->item_count; k2++) {
            size_t l = strlen(items);
            snprintf(items + l, sizeof items - l, "%s%c%d.%d", k2 ? (nd->require_all ? "&" : "|") : "", nd->items[k2].type, nd->items[k2].table, nd->items[k2].node);
        }
        printf("  node %2d %c%s %-10s %-9s %-7s delay %5d %-8s [%s]%s%s %s%s%s",
               i, nd->objective ? nd->objective : '?', s.logic.shown[0][i] ? "V" : " ", mtbl_kind_name(nd->kind), nd->target[0] ? nd->target : "-",
               state_name(st), (int)(int32_t)nd->delay,
               s.logic.ended[0][i] >= 0 ? "" : "", items, wanted[i] ? " W" : "", bad[i] ? " B" : "",
               nd->text[0] ? "\"" : "", nd->text, nd->text[0] ? "\"" : "");
        if ((nd->kind & 0xFFFF0000u) && nd->other_table >= 0) printf(" -> %d.%d", nd->other_table, nd->other_node);
        if (s.logic.ended[0][i] >= 0) printf(" @%.1fs", (double)s.logic.ended[0][i] / 1000.0);
        printf("\n");
        if (st == MTBL_ACTIVE && (nd->objective == 'M' || (wanted[i] && !bad[i])) && !reported[i] && nd->kind != MTBL_K_PROTECT &&
            s.outcome != 2)   /* after a success the rest no longer matters (a star-win can end it early) */
            anom(&nanom, scene, "node %d %s %s (%c) still active at the end", i, mtbl_kind_name(nd->kind), nd->target[0] ? nd->target : "-", nd->objective);
        if (st == MTBL_FAILED && nd->objective == 'M')
            anom(&nanom, scene, "main objective node %d %s %s FAILED", i, mtbl_kind_name(nd->kind), nd->target);
    }
    if (s.outcome != 2) anom(&nanom, scene, "mission not successful: %s", msim_outcome_text(&s) ? msim_outcome_text(&s) : "no outcome");
    printf("RESULT %-9s %-22s %7.1f s  anomalies %d  fallbacks %d\n", scene,
           msim_outcome_text(&s) ? msim_outcome_text(&s) : (s.logic.tables[0].node_count <= 1 ? "none (no objectives)" : "none (running)"),
           (double)(s.ending ? s.end_at - (0x71c * 1000 / 182) : s.now) / 1000.0, nanom, at.fallbacks);
done:
    if (have_bm) bwd_mission_free(&bm);
    msim_free(&s);
    mech3d_free(&world);
    world3d_free_actors(ac, n);
    return 0;
}

static void cleanup_stars(const char *dir)
{
    static const char *const F[7] = {"USERSTAR.BWD", "EN01STAR.BWD", "EN02STAR.BWD", "EN03STAR.BWD", "EN04STAR.BWD", "EN05STAR.BWD", "INSTMAP1.BWD"};
    char p[512];
    int k;
    if (!dir) return;
    for (k = 0; k < 7; k++) { snprintf(p, sizeof p, "%s/%s", dir, F[k]); unlink(p); }
    rmdir(dir);
}

static int is_scene(const char *nm) { size_t l = strlen(nm); return l == 8 && strcasecmp(nm + 4, "SCN1") == 0; }

int main(int argc, char **argv)
{
    char err[256];
    prj_archive *a;
    int i, first = 2, nscenes = 0, fails = 0, own_stars = 1;
    const char *stars_dir = NULL;
    char **scenes = NULL;
    if (argc < 2) { fprintf(stderr, "usage: %s MW2.PRJ [-v] [-t max_seconds] [SCENE ...]\n", argv[0]); return 2; }
    for (; first < argc && argv[first][0] == '-'; first++) {
        if (strcmp(argv[first], "-v") == 0) g_verbose = 1;
        else if (strcmp(argv[first], "-nofire") == 0) g_nofire = 1;   /* debugging: the player never fires (fallbacks only) */
        else if (strcmp(argv[first], "-t") == 0 && first + 1 < argc) g_max_s = (float)atof(argv[++first]);
        else if (strcmp(argv[first], "-installstars") == 0) own_stars = 0;
    }
    if (!(a = prj_open(argv[1], err, sizeof err))) { fprintf(stderr, "%s\n", err); return 1; }
    if (own_stars) {
        /* the shell's hand-off files (USERSTAR, EN01..05STAR, INSTMAP1) written fresh, as the Instant Action launch does
         * (mw2shell 0x1d540): a fixed lance so runs are repeatable and the Instant Action scenes have enemies. They go
         * in a temporary directory searched before the install */
        static char dir[256];
        lance_slot fr[3], en[3];
        static const char *const FR[3] = {"timbrwlf", "maddog", "strmcrow"}, *const EN[3] = {"maddog", "strmcrow", "nova"};
        int k, m;
        snprintf(dir, sizeof dir, "%s/mw2sweepXXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
        if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
        memset(fr, 0, sizeof fr); memset(en, 0, sizeof en);
        for (k = 0; k < 3; k++) {
            fr[k].mech = en[k].mech = -1;
            for (m = 0; m < lance_mech_count(); m++) {
                if (strcmp(lance_mech_file(m), FR[k]) == 0) fr[k].mech = m;
                if (strcmp(lance_mech_file(m), EN[k]) == 0) en[k].mech = m;
            }
        }
        if (lance_write_all(a, dir, fr, en, 1) != 0 || lance_write_instmap(a, dir, 1, 0) != 0) { fprintf(stderr, "lance files: failed\n"); return 1; }
        dp_add_root(dir);
        stars_dir = g_stars_dir = dir;
        memcpy(g_fr, fr, sizeof g_fr);
        printf("star files: %s (Timber Wolf, Mad Dog, Stormcrow vs Mad Dog, Stormcrow, Nova)\n", dir);
    }
    dp_add_roots_from_env();
    if (first < argc) { scenes = argv + first; nscenes = argc - first; }
    else {
        int t = prj_find_type(a, "BWD");
        scenes = calloc((size_t)prj_symbol_count(a, t) + 1, sizeof *scenes);
        for (i = 0; scenes && i < prj_symbol_count(a, t); i++)
            if (is_scene(prj_symbol_name(a, t, i))) scenes[nscenes++] = strdup(prj_symbol_name(a, t, i));
    }
    if (nscenes == 1) { int r = run_scene(a, scenes[0]); prj_close(a); cleanup_stars(stars_dir); return r; }
    for (i = 0; i < nscenes; i++) {
        pid_t p;
        int st;
        fflush(stdout);
        p = fork();
        if (p == 0) { int r; setvbuf(stdout, NULL, _IOLBF, 0); alarm(600); r = run_scene(a, scenes[i]); fflush(stdout); _exit(r); }
        if (p < 0 || waitpid(p, &st, 0) < 0) { printf("RESULT %-9s HARNESS fork/wait failed\n", scenes[i]); fails++; continue; }
        if (WIFSIGNALED(st)) { printf("RESULT %-9s CRASH signal %d%s\n", scenes[i], WTERMSIG(st), WTERMSIG(st) == SIGALRM ? " (timeout)" : ""); fails++; }
        else if (WEXITSTATUS(st) != 0) { printf("RESULT %-9s ERROR exit %d\n", scenes[i], WEXITSTATUS(st)); fails++; }
    }
    prj_close(a);
    cleanup_stars(stars_dir);
    return fails ? 1 : 0;
}
