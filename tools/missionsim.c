/*
 * missionsim - run a mission's logic and group AI headless and print what happens.
 *   missionsim MODELS.PRJ SCENE SECONDS [player_x player_z [attack]]
 * The player stands still (at the mission start, or the given point); with
 * "attack" it fires at the nearest enemy in range.
 */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "msim.h"
#include "datapath.h"

static void logline(void *u, const char *l) { (void)u; printf("%s\n", l); }

static int g_fxcount[32];
static void count_effect(void *u, int type, const float pos[3]) { (void)u; (void)pos; if (type >= 0 && type < 32) g_fxcount[type]++; }

static float g_peak_heat;
int main(int argc, char **argv)
{
    char err[256];
    prj_archive *a;
    world_actor *ac;
    int n, i, steps;
    msim s;
    if (argc < 4) { fprintf(stderr, "usage: %s MODELS.PRJ SCENE SECONDS [player_x player_z]\n", argv[0]); return 2; }
    if (!(a = prj_open(argv[1], err, sizeof err))) { fprintf(stderr, "%s\n", err); return 1; }
    if (world3d_actors(a, argv[2], &ac, &n) != 0) { fprintf(stderr, "no actors\n"); return 1; }
    msim_init(&s, a, argv[2], ac, n);
    s.player_vertical = 1;
    s.on_effect = count_effect;   /* tally the effects the sim asks for */
    static mech3d world;   /* kept for the sim's face tests */
    memset(&world, 0, sizeof world);
    {   /* Combat Variables OBJECT DENSITY (MW2SND.CFG +0x20, 0 = LOW), as glview */
        unsigned char *snd = NULL;
        size_t sl = 0;
        if (dp_read_file("MW2SND.CFG", &snd, &sl) == 0) { if (sl >= 0x24) world3d_density_low = (snd[0x20] | snd[0x21] | snd[0x22] | snd[0x23]) == 0; free(snd); }
    }
    if (world3d_mission(a, argv[2], 1, &world) > 0) { msim_set_world(&s, &world); printf("obstacles: %d candidate objects, %d destructible\n", s.obstacle_count, s.bld_count); }
    s.log = logline;
    if (argc > 5 && strcmp(argv[4], "-") != 0) { s.player[0] = (float)atof(argv[4]); s.player[2] = (float)atof(argv[5]); }   /* "-" keeps the start */
    if (argc > 6 && strcmp(argv[6], "attack") == 0) s.player_attacks = 1;
    printf("player: %s (%s)\n", s.player_skel, s.player_loadout);
    printf("planet: gravity %.2f g, temperature %d, jet damping %.0f, %s atmosphere\n", (double)s.planet.gravity_g,
           s.planet.temperature, (double)s.planet.jet_damping, s.planet.hostile ? "hostile" : "breathable");
    printf("%d tables, %d nav points, %d actors; player at %.0f,%.0f\n", s.logic.table_count, s.nav_count, n, s.player[0], s.player[2]);
    for (i = 0; i < s.nav_count; i++)
        printf("  nav    %-9s %-14s%s at %7.0f,%7.0f radius %6.0f\n", s.navs[i].name, s.navs[i].label[0] ? s.navs[i].label : "-", s.navs[i].selectable ? "*" : " ", s.navs[i].x, s.navs[i].z, s.navs[i].radius);
    for (i = 0; i < n; i++)
        printf("  start  %-22s table %d %s at %7d,%7d heading %5.0f\n", ac[i].name, ac[i].table, ac[i].leader ? "leader" : "      ",
               ac[i].mech.origin[0], ac[i].mech.origin[2], ac[i].mech.heading);
    steps = (int)(atof(argv[3]) * 20);
    {
        float *peak = calloc((size_t)(n ? n : 1), sizeof *peak);
        int *jumps = calloc((size_t)(n ? n : 1), sizeof *jumps), *air = calloc((size_t)(n ? n : 1), sizeof *air), j;
        for (i = 0; i < steps; i++) {
            {
                static int last_h = -1, last_a = -1;
                int h0, a0, w3;
                if (getenv("MW2_TEST_LOCK")) {   /* tests: aim at the nearest enemy with the first guided weapon selected */
                    float dd;
                    int e = msim_nearest_enemy(&s, &dd), q;
                    static int placed;
                    if (e < 0 && !placed) {   /* bring the player to 600 m south of the first armed actor that isn't a friend */
                        int a2;
                        for (a2 = 0; a2 < s.actor_count; a2++) if (s.armed[a2] && !s.actors[a2].friendly) break;
                        if (a2 < s.actor_count) { s.player[0] = (float)s.actors[a2].mech.origin[0]; s.player[2] = (float)s.actors[a2].mech.origin[2] - 60000.0f; placed = 1; }
                        e = msim_nearest_enemy(&s, &dd);
                    }
                    static int prev = -1;
                    s.player_lock_target = e;
                    for (q = 0; q < s.player_unit.weapon_count; q++) if (msim_weapon_guided(s.player_unit.weapons[q].weapon)) { s.player_sel_w = q; break; }
                    if (e >= 0) {
                        float dx = (float)s.actors[e].mech.origin[0] - s.player[0], dz = (float)s.actors[e].mech.origin[2] - s.player[2];
                        s.player_aim_yaw = atan2f(dx, dz) * 180.0f / 3.14159265f; s.player_aim_pitch = 0;
                    }
                    if (s.player_lock != prev) { printf("lock %.2f s: %04x (target %d at %.0f m)\n", (double)i * 0.05, s.player_lock, e, (double)(dd / 100.0f)); prev = s.player_lock; }
                    if (s.player_lock & 0x80) { int k, hom = 0; for (k = 0; k < s.shot_count; k++) if (s.shots[k].homing >= 0) hom++; if (hom) { printf("homing shots in flight: %d at %.2f s\n", hom, (double)i * 0.05); setenv("MW2_TEST_LOCK_DONE", "1", 1); } }
                    s.player_attacks = (s.player_lock & 0x80) && !getenv("MW2_TEST_LOCK_DONE");
                }
                if (getenv("MW2_SHOOT_REC") && s.world) {   /* tests: shoot every structure of record MW2_SHOOT_REC with real
                     * projectiles, one at a time, from MW2_SHOOT_DIST m (default 150) at bearing MW2_SHOOT_BRG deg from it */
                    static int cur = -1, n_hit[256];
                    static float since;
                    float dist = getenv("MW2_SHOOT_DIST") ? (float)atof(getenv("MW2_SHOOT_DIST")) * 100.0f : 15000.0f;
                    float brg = getenv("MW2_SHOOT_BRG") ? (float)atof(getenv("MW2_SHOOT_BRG")) * 3.14159265f / 180.0f : 0.0f;
                    s.debug_invulnerable = 1;
                    if (cur >= 0 && (s.bld[cur].hp <= 0 || since > 30.0f)) {
                        printf("shoot: %s (%s) %s after %.1f s\n", s.bld[cur].name, s.bld[cur].sub, s.bld[cur].hp <= 0 ? "DESTROYED" : "still standing", (double)since);
                        cur = -1;
                    }
                    if (cur < 0) {
                        int b, pass;
                        for (pass = 0, b = s.bld_count; pass < 2 && b >= s.bld_count; pass++)   /* MW2_SHOOT_LAST: those sub-names last */
                            for (b = 0; b < s.bld_count; b++)
                                if (s.bld[b].hp > 0 && !n_hit[b] && strcasecmp(s.world->parts[s.bld[b].intact].rec, getenv("MW2_SHOOT_REC")) == 0 &&
                                    (pass || !getenv("MW2_SHOOT_LAST") || !strstr(getenv("MW2_SHOOT_LAST"), s.bld[b].sub))) break;
                        if (b < s.bld_count) { cur = b; n_hit[b] = 1; since = 0; }
                        else { s.player_attacks = 0; }
                    }
                    if (cur >= 0) {
                        const struct msim_building *g = &s.bld[cur];
                        float c[3] = {(g->mn[0] + g->mx[0]) * 0.5f, (g->mn[1] + g->mx[1]) * 0.5f, (g->mn[2] + g->mx[2]) * 0.5f}, dx, dz, dy;
                        if (c[1] > g->mn[1] + 500.0f) c[1] = g->mn[1] + 500.0f;   /* low on it: a box's middle can be empty */
                        s.player[0] = c[0] + sinf(brg) * dist; s.player[2] = c[2] + cosf(brg) * dist;
                        dx = c[0] - s.player[0]; dz = c[2] - s.player[2]; dy = c[1] - (s.player_unit.y + 700.0f);
                        s.player_facing = s.player_aim_yaw = atan2f(dx, dz) * 180.0f / 3.14159265f;
                        s.player_aim_pitch = atan2f(dy, sqrtf(dx * dx + dz * dz)) * 180.0f / 3.14159265f;
                        s.player_aim_free = 1; s.player_target = -1; s.player_attacks = 1;
                        s.player_unit.no_heat = 1;
                        since += 0.05f;
                    }
                }
                msim_step(&s, 50);
                if (getenv("MW2_HIT_BUILDINGS") && i == 20) {   /* test: 50 damage to every destructible object at 1 s */
                    int b;
                    for (b = 0; b < s.bld_count; b++) if (!getenv("MW2_HIT_ONLY") || strstr(getenv("MW2_HIT_ONLY"), s.bld[b].sub)) msim_damage_building(&s, b, (float)atof(getenv("MW2_HIT_BUILDINGS")) > 1 ? (float)atof(getenv("MW2_HIT_BUILDINGS")) : 50.0f);
                    printf("buildings destroyed %d of %d, obstacles now %d\n", s.buildings_destroyed, s.bld_count, s.obstacle_count);
                    for (b = 0; b < s.bld_count; b++) printf("  %s (%s) rec %s type %#x hp %d at %d,%d\n", s.bld[b].name, s.bld[b].sub, s.world->parts[s.bld[b].intact].rec, s.bld[b].type, s.bld[b].hp, s.world->parts[s.bld[b].intact].pos[0], s.world->parts[s.bld[b].intact].pos[2]);
                }
                if (getenv("MW2_INSPECT") && i == 20) {   /* test: inspect every structure, far (no effect) then from its centre */
                    int b, far = 0, ok = 0;
                    for (b = 0; b < s.bld_count; b++) if (msim_inspect(&s, -1, b) == 2) far++;
                    for (b = 0; b < s.bld_count; b++) {
                        float sv0 = s.player[0], sv2 = s.player[2];
                        s.player[0] = (s.bld[b].mn[0] + s.bld[b].mx[0]) * 0.5f; s.player[2] = (s.bld[b].mn[2] + s.bld[b].mx[2]) * 0.5f;
                        if (msim_inspect(&s, -1, b) == 1) ok++;
                        s.player[0] = sv0; s.player[2] = sv2;
                    }
                    printf("inspect: %d out of range, %d inspected of %d structures\n", far, ok, s.bld_count);
                }
                if (getenv("MW2_ORDERS")) {   /* test: "secs:point:order[:target]; ..." (order: msim.h MSIM_CMD_*, or -f for formation f) */
                    const char *o = getenv("MW2_ORDERS");
                    while (*o) {
                        float t = (float)atof(o);
                        int pt = 0, od = 0, tg = -1, nf;
                        nf = sscanf(o, "%*f:%d:%d:%d", &pt, &od, &tg);
                        if (nf >= 2 && (int)(t * 20) == i + 1) {
                            if (od < 0) msim_set_formation(&s, -od - 1);
                            else if (tg == -2) msim_command(&s, pt, od, msim_nearest_enemy(&s, NULL));
                            else msim_command(&s, pt, od, tg);
                        }
                        while (*o && *o != ';') o++;
                        if (*o) o++;
                    }
                }
                if (getenv("MW2_DEBUG_STAR") && i % 100 == 0) {   /* test: the player's starmates every 5 s */
                    int q;
                    for (q = 1; q <= 4; q++) {
                        int a = msim_point_actor(&s, q);
                        if (a < 0) continue;
                        printf("  %6.1fs point %d %-14s state %-13s cmd %2d at %7d,%7d (player %7.0f,%7.0f)\n", i / 20.0, q + 1, s.actors[a].name,
                               MSIM_STATUS_TEXT[msim_point_status(&s, q)], s.cmd[a], s.actors[a].mech.origin[0], s.actors[a].mech.origin[2], s.player[0], s.player[2]);
                    }
                }
                if (s.player_unit.heat > g_peak_heat) g_peak_heat = s.player_unit.heat;   /* the report's peak heat */
                if (getenv("MW2_DEBUG_PLAYER")) {   /* test: every change to the player's armour / ammo */
                    h0 = combat_health(&s.player_unit);
                    for (a0 = 0, w3 = 0; w3 < s.player_unit.weapon_count; w3++) if (s.player_unit.weapons[w3].ammo > 0) a0 += s.player_unit.weapons[w3].ammo;
                    if (last_h >= 0 && h0 != last_h) printf("  %6.2fs player armour+structure %d -> %d\n", i / 20.0, last_h, h0);
                    if (last_a >= 0 && a0 != last_a) printf("  %6.2fs player ammo %d -> %d\n", i / 20.0, last_a, a0);
                    last_h = h0; last_a = a0;
                }
            }
            s.debug_invulnerable = getenv("MW2_DEBUG_INVULN") != NULL;   /* test: the player survives */
            if (getenv("MW2_DEBUG_TRACK") && i % 600 == 0) {   /* test: a table's units every 30 s */
                int q, tb = atoi(getenv("MW2_DEBUG_TRACK"));
                for (q = 0; q < n; q++)
                    if (s.actors[q].table == tb && s.armed[q] && !s.units[q].destroyed)
                        printf("  track %5.0fs unit %d at %d,%d height %.1f m heading %.0f\n", i / 20.0, q, s.actors[q].mech.origin[0], s.actors[q].mech.origin[2],
                               s.units[q].y / 100.0, s.actors[q].mech.heading);
            }
            if (getenv("MW2_DEBUG_KILL_AT") && i == (int)(atof(getenv("MW2_DEBUG_KILL_AT")) * 20)) s.player_unit.destroyed = 1;   /* test: player destroyed */
            if (s.over) break;   /* the mission's end sequence has finished */
            for (j = 0; peak && j < n; j++) {
                if (s.units[j].y > peak[j]) peak[j] = s.units[j].y;
                if (s.units[j].y > 0 && !air[j]) jumps[j]++;
                air[j] = s.units[j].y > 0;
            }
        }
        for (j = 0; peak && j < n; j++)
            if (jumps[j]) printf("  jumps  %-22s %d jump(s), peak %.1f m\n", ac[j].name, jumps[j], (double)peak[j] / 100.0);
        free(peak); free(jumps); free(air);
    }
    for (i = 0; i < n; i++) {
        int cur = s.logic.current[ac[i].table];
        printf("  end    %-22s table %d %-8s man %2d (last %2d) at %7d,%7d heading %5.0f  order %s %s\n", ac[i].name, ac[i].table,
               s.minds ? ai_state_name(s.minds[i].state) : "-", s.minds ? s.minds[i].man : -1, s.minds ? s.minds[i].prev_man : -1,
               ac[i].mech.origin[0],
               ac[i].mech.origin[2], ac[i].mech.heading, cur >= 0 ? mtbl_kind_name(s.logic.tables[ac[i].table].nodes[cur].kind) : "-",
               cur >= 0 ? s.logic.tables[ac[i].table].nodes[cur].target : "");
    }
    printf("projectiles: %d hit, %d missed\n", s.hits, s.misses);
    printf("player %s, %d armour+structure left\n", s.player_unit.destroyed ? "DESTROYED" : "alive", combat_health(&s.player_unit));

    {   /* the Combat Variables checks: remaining ammunition, peak heat */
        int w2, ammo = 0;
        for (w2 = 0; w2 < s.player_unit.weapon_count; w2++) if (s.player_unit.weapons[w2].ammo > 0) ammo += s.player_unit.weapons[w2].ammo;
        printf("player ammo left %d, peak heat %.1f\n", ammo, g_peak_heat);
    }    {
        int t, any = 0;
        for (t = 0; t < 32; t++) if (g_fxcount[t]) { if (!any) printf("effects:"); printf(" %x:%d", t, g_fxcount[t]); any = 1; }
        if (any) printf("\n");
    }
    printf("outcome: %s%s at %.1f s\n", msim_outcome_text(&s) ? msim_outcome_text(&s) : "none", s.over ? " (mission over)" : " (still running)", s.now / 1000.0);
    if (getenv("MW2_INSTALL_DIR")) {   /* MW2MSN.CFG for the shell, as the engine writes on exit */
        char rp[1100];
        snprintf(rp, sizeof rp, "%s/MW2MSN.CFG", getenv("MW2_INSTALL_DIR"));
        if (msim_write_results(&s, rp) == 0) printf("results: %s\n", rp);
        snprintf(rp, sizeof rp, "%s/MW2CAR.CFG", getenv("MW2_INSTALL_DIR"));
        msim_write_career(&s, rp);
    }
    msim_free(&s);
    mech3d_free(&world);
    world3d_free_actors(ac, n);
    prj_close(a);
    return 0;
}
