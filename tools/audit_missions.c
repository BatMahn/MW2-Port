/*
 * audit_missions - a static check of every mission's set-up, for the kind of data problems players report: units
 * placed far from their group or outside the mission boundary, objects drawn where nothing should be, destroyed
 * variants shown before their thing is destroyed, nav points beyond the boundary.
 *
 *   audit_missions MW2.PRJ [SCENE ...]      (no scene: every *SCN1 record)
 *   env: MW2_INSTALL_DIR for the install's star files (USERSTAR / EN0nSTAR)
 *
 * Prints one line per finding: SCENE CHECK details. Nothing printed for a scene = nothing found. Findings are
 * leads, not proof: some are as the original has them (a unit parked outside the boundary on purpose).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "msim.h"
#include "bwd.h"
#include "datapath.h"

static int is_scene(const char *nm) { size_t l = strlen(nm); return l == 8 && strcasecmp(nm + 4, "SCN1") == 0; }

static float dist2(float ax, float az, float bx, float bz) { return sqrtf((ax - bx) * (ax - bx) + (az - bz) * (az - bz)); }

static int audit(prj_archive *a, const char *scene)
{
    world_actor *ac = NULL;
    int n = 0, i, k, nb = 0, findings = 0;
    msim s;
    mech3d world;
    bwd_mission m;
    struct { float x, z, r; } bnd[16];
#define F(...) do { printf("%-9s ", scene); printf(__VA_ARGS__); printf("\n"); findings++; } while (0)

    if (bwd_mission_load(a, scene, &m) != 0) { F("ERROR mission load"); return 1; }
    for (i = 0; i < m.nav_count && nb < 16; i++)
        if ((m.navs[i].type == 0x20 || m.navs[i].type == 0x22) && m.navs[i].radius > 0) { bnd[nb].x = (float)m.navs[i].x; bnd[nb].z = (float)m.navs[i].z; bnd[nb].r = (float)m.navs[i].radius * 100.0f; nb++; }
    if (world3d_actors(a, scene, &ac, &n) != 0) { F("ERROR world3d_actors"); bwd_mission_free(&m); return 1; }
    if (msim_init(&s, a, scene, ac, n) != 0) { F("ERROR msim_init"); world3d_free_actors(ac, n); bwd_mission_free(&m); return 1; }
    memset(&world, 0, sizeof world);
    if (world3d_mission(a, scene, 1, &world) > 0) msim_set_world(&s, &world);

    /* the player */
    {
        int in = nb == 0;
        for (k = 0; k < nb; k++) if (dist2(s.player[0], s.player[2], bnd[k].x, bnd[k].z) <= bnd[k].r) in = 1;
        if (!in) F("PLAYER outside the boundary at %.0f,%.0f", (double)s.player[0] / 100, (double)s.player[2] / 100);
        if (strcasecmp(s.player_rec, "USERSTAR") != 0) printf("%-9s info player record %s\n", scene, s.player_rec);
    }
    /* nav points */
    for (i = 0; i < m.nav_count; i++) {
        int in = nb == 0;
        if (m.navs[i].type == 0x20 || m.navs[i].type == 0x22) continue;
        for (k = 0; k < nb; k++) if (dist2((float)m.navs[i].x, (float)m.navs[i].z, bnd[k].x, bnd[k].z) <= bnd[k].r) in = 1;
        if (!in && m.navs[i].shown) F("NAV %s (shown) outside the boundary at %.0f,%.0f m", m.navs[i].name, m.navs[i].x / 100.0, m.navs[i].z / 100.0);
    }
    /* units: let the sim settle them 3 s (the player stands still, nobody is near enough to fight yet in most) */
    float *settled_y = calloc((size_t)(n > 0 ? n : 1), sizeof *settled_y);
    {
        int t;
        s.debug_invulnerable = 1; s.player_unit.invulnerable = 1;
        for (t = 0; t < (getenv("AUDIT_SECS") ? atoi(getenv("AUDIT_SECS")) * 20 : 60); t++) msim_step(&s, 50);
        for (i = 0; i < n; i++) settled_y[i] = s.units[i].y;
    }
    /* units */
    for (i = 0; i < n; i++) {
        const world_actor *u = &s.actors[i];
        const float x = (float)u->mech.origin[0], z = (float)u->mech.origin[2], y = (float)u->mech.origin[1];
        int in = nb == 0, lead = -1;
        for (k = 0; k < nb; k++) if (dist2(x, z, bnd[k].x, bnd[k].z) <= bnd[k].r) in = 1;
        if (!in) F("UNIT %d %s %s (table %d) outside the boundary at %.0f,%.0f m", i, u->group, u->skel, u->table, x / 100.0, z / 100.0);
        for (k = 0; k < n; k++) if (s.actors[k].table == u->table && s.actors[k].leader && strcmp(s.actors[k].group, u->group) == 0) { lead = k; break; }
        if (lead >= 0 && lead != i) {
            const float d = dist2(x, z, (float)s.actors[lead].mech.origin[0], (float)s.actors[lead].mech.origin[2]);
            if (d > 100000.0f) F("UNIT %d %s %s %.0f m from its leader %d", i, u->group, u->skel, d / 100.0, lead);
        }
        {   /* after settling (below): where the sim holds it against the ground under it */
            const float uy = settled_y[i], g = msim_ground(&s, (float)s.actors[i].mech.origin[0], (float)s.actors[i].mech.origin[2], uy + 3000.0f);
            if (fabsf(g - uy) > 1500.0f && fabsf(g - uy) < 1e8f && u->unit_class != 8 && strcasecmp(u->skel, "DROPA"))
                F("UNIT %d %s %s (class %d) %.0f m %s the ground after 3 s (y %.0f, ground %.0f) at %.0f,%.0f", i, u->group, u->skel, u->unit_class, fabs(g - uy) / 100.0, uy > g ? "above" : "below", uy / 100.0, g / 100.0, s.actors[i].mech.origin[0] / 100.0, s.actors[i].mech.origin[2] / 100.0);
        }
        for (k = i + 1; k < n; k++)
            if (dist2(x, z, (float)s.actors[k].mech.origin[0], (float)s.actors[k].mech.origin[2]) < 300.0f && fabsf(y - (float)s.actors[k].mech.origin[1]) < 1000.0f)
                F("UNIT %d %s and %d %s within 3 m of each other at %.0f,%.0f m", i, u->skel, k, s.actors[k].skel, x / 100.0, z / 100.0);
    }
    /* world objects */
    if (s.world) {
        const mech3d *w = s.world;
        int b;
        if (getenv("AUDIT_GT"))
            for (b = 0; b < s.bld_count; b++)
                if (!strcmp(w->parts[s.bld[b].intact].rec, getenv("AUDIT_GT")))
                    printf("  GT intact obj %d %s hid %d -> destroyed obj %d %s hid %d hp %d type %#x flags %#x %s\n", w->parts[s.bld[b].intact].obj_index, w->parts[s.bld[b].intact].model_name, w->parts[s.bld[b].intact].hidden,
                           s.bld[b].destroyed >= 0 ? w->parts[s.bld[b].destroyed].obj_index : -1, s.bld[b].destroyed >= 0 ? w->parts[s.bld[b].destroyed].model_name : "-", s.bld[b].destroyed >= 0 ? w->parts[s.bld[b].destroyed].hidden : -1, s.bld[b].hp, s.bld[b].type, s.bld[b].flags, s.bld[b].name);
        for (b = 0; b < s.bld_count; b++) {
            int chained = 0, c2;   /* a wreck's own GT (its intact object is another thing's destroyed variant) waits too */
            for (c2 = 0; c2 < s.bld_count; c2++) if (s.bld[c2].destroyed >= 0 && w->parts[s.bld[c2].destroyed].obj_index == w->parts[s.bld[b].intact].obj_index && !strcmp(w->parts[s.bld[c2].destroyed].rec, w->parts[s.bld[b].intact].rec)) chained = 1;
            if (!chained && s.bld[b].intact >= 0 && w->parts[s.bld[b].intact].hidden && !(w->parts[s.bld[b].intact].sparse))
                F("THING %s %s (%s) intact object hidden at the start", w->parts[s.bld[b].intact].rec, w->parts[s.bld[b].intact].model_name, s.bld[b].name);
            if (s.bld[b].destroyed >= 0 && !w->parts[s.bld[b].destroyed].hidden)
                F("THING %s %s destroyed variant shown at the start", w->parts[s.bld[b].destroyed].rec, w->parts[s.bld[b].destroyed].model_name);
        }
        if (getenv("AUDIT_NEAR")) {
            float nx = 0, nz = 0, rr = 6000;
            sscanf(getenv("AUDIT_NEAR"), "%f,%f,%f", &nx, &nz, &rr);
            for (i = 0; i < w->part_count; i++) {
                const mech3d_part *p = &w->parts[i];
                if (dist2((float)p->pos[0], (float)p->pos[2], nx, nz) < rr)
                    printf("  near %s %s obj %d at %d,%d,%d hid %d coll %d type %#x moving %d\n", p->rec, p->model_name, p->obj_index, p->pos[0], p->pos[1], p->pos[2], p->hidden, p->coll, p->objtype, p->moving);
            }
        }
        for (i = 0; i < w->part_count; i++) {
            const mech3d_part *p = &w->parts[i];
            const float x = (float)p->pos[0], z = (float)p->pos[2];
            int in = nb == 0, far_sib = 0;
            if (p->hidden || p->moving) continue;
            if (!strncasecmp(p->rec, "MW2_SHT", 7)) continue;
            for (k = 0; k < nb; k++) if (dist2(x, z, bnd[k].x, bnd[k].z) <= bnd[k].r + 300000.0f) in = 1;   /* 3 km of scenery past the edge is normal */
            if (!in) F("OBJECT %s %s far outside the boundary at %.0f,%.0f m", p->rec, p->model_name, x / 100.0, z / 100.0);
            if (fabsf(x) < 3000.0f && fabsf(z) < 3000.0f) {   /* at the map origin: a stray (an object whose placement never happened)? */
                for (k = 0; k < w->part_count; k++)
                    if (k != i && !strcmp(w->parts[k].rec, p->rec) && !w->parts[k].hidden && dist2((float)w->parts[k].pos[0], (float)w->parts[k].pos[2], x, z) > 50000.0f) far_sib = 1;
                if (far_sib) F("OBJECT %s %s at the map origin (%.0f,%.0f m) while the rest of its record is elsewhere", p->rec, p->model_name, x / 100.0, z / 100.0);
            }
        }
    }
    free(settled_y);
    msim_free(&s);
    world3d_free_actors(ac, n);
    mech3d_free(&world);
    bwd_mission_free(&m);
    return findings;
}

int main(int argc, char **argv)
{
    char err[256];
    prj_archive *a;
    int i, t, total = 0;
    if (argc < 2) { fprintf(stderr, "usage: %s MW2.PRJ [SCENE ...]\n", argv[0]); return 2; }
    if (!(a = prj_open(argv[1], err, sizeof err))) { fprintf(stderr, "%s\n", err); return 1; }
    dp_add_roots_from_env();
    if (argc > 2) for (i = 2; i < argc; i++) total += audit(a, argv[i]);
    else {
        t = prj_find_type(a, "BWD");
        for (i = 0; i < prj_symbol_count(a, t); i++)
            if (is_scene(prj_symbol_name(a, t, i))) { printf("== %s\n", prj_symbol_name(a, t, i)); fflush(stdout); total += audit(a, prj_symbol_name(a, t, i)); }
    }
    printf("%d findings\n", total);
    prj_close(a);
    return 0;
}
