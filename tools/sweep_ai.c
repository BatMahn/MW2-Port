/*
 * sweep_ai - tests only (tools/sweep_walk.py's AI pass): run a mission headless as missionsim does and report
 * AI mechs that stop making progress while their AI still drives them (throttle up), with the object in the way.
 *   sweep_ai MODELS.PRJ SCENE SECONDS [attack]
 * Prints "aistuck SCENE actor k NAME state S from T1 to T2 (to the end) at X,Z y Y: <cause>" for every interval
 * of at least 10 s with under 1 m moved and the throttle over 0.1 (the end flag: still stuck when the run ended).
 * Cause: the refused step ahead (steep / wall1 / solid part + record + collision type), a mech in contact, or none.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "datapath.h"
#include "msim.h"

static void quiet(void *u, const char *l) { (void)u; (void)l; }

static void cause(msim *s, mech3d *w, int i, char *out, size_t n)
{
    world_actor *ac = &s->actors[i];
    float h = (ac->mech.heading + (s->minds && s->minds[i].speed < 0 ? 180.0f : 0.0f)) * 3.14159265f / 180.0f, x0 = (float)ac->mech.origin[0], z0 = (float)ac->mech.origin[2];
    float nx = x0 + sinf(h) * 60.0f, nz = z0 + cosf(h) * 60.0f, y = s->units[i].y, H = s->centre_h[i], R = s->radius[i], g, nn[3];
    int k, len = 0;
    out[0] = 0;
    for (k = 0; k < s->actor_count; k++) {
        float dx, dz;
        if (k == i || s->units[k].destroyed) continue;
        dx = (float)s->actors[k].mech.origin[0] - x0; dz = (float)s->actors[k].mech.origin[2] - z0;
        if (dx * dx + dz * dz < (R + s->radius[k] + 100.0f) * (R + s->radius[k] + 100.0f)) len += snprintf(out + len, n - (size_t)len, " mech %d", k);
    }
    {
        float dx = s->player[0] - x0, dz = s->player[2] - z0;
        if (!s->player_unit.destroyed && dx * dx + dz * dz < (R + s->player_radius + 100.0f) * (R + s->player_radius + 100.0f)) len += snprintf(out + len, n - (size_t)len, " player contact");
    }
    if (msim_can_step_hr(s, x0, z0, nx, nz, y, H, R)) { if (!len) snprintf(out, n, " step along the move free (AI steering / avoidance)"); return; }
    g = msim_ground_n(s, nx, nz, y, nn);
    if (nn[1] < 0.70710677f && g - y > H) len += snprintf(out + len, n - (size_t)len, " steep rise %.0f", g - y);
    {
        float ax = nx + sinf(h) * R, az = nz + cosf(h) * R, g2 = msim_ground_n(s, ax, az, y, nn);
        if (nn[1] < 0.70710677f && g2 - y > H) len += snprintf(out + len, n - (size_t)len, " steep rise ahead %.0f (n.y %.2f)", g2 - y, nn[1]);
    }
    if (y - g < -9999.99f) len += snprintf(out + len, n - (size_t)len, " ground %.0f above (window)", g - y);
    if (msim_wall_at(s, nx, nz, NULL)) len += snprintf(out + len, n - (size_t)len, " wall1");
    for (k = 0; k < w->part_count && len < (int)n - 80; k++) {
        mech3d_part *p = &w->parts[k];
        if (p->hidden || (p->coll != 0 && p->coll != 2 && p->coll != 7)) continue;
        p->hidden = 1;
        if (msim_can_step_hr(s, x0, z0, nx, nz, y, H, R)) len += snprintf(out + len, n - (size_t)len, " solid part %d '%s' rec %s coll %d at %d,%d,%d", k, p->model_name, p->rec, p->coll, p->pos[0], p->pos[1], p->pos[2]);
        p->hidden = 0;
    }
    if (!len) snprintf(out, n, " blocked (cause not found)");
}

int main(int argc, char **argv)
{
    char err[256];
    prj_archive *a;
    world_actor *ac;
    int n, i, k, steps;
    msim s;
    static mech3d world;
    float *lx, *lz, *since, *bx, *bz;
    char (*why)[512];
    if (argc < 4) { fprintf(stderr, "usage: %s MODELS.PRJ SCENE SECONDS [attack]\n", argv[0]); return 2; }
    if (!(a = prj_open(argv[1], err, sizeof err))) { fprintf(stderr, "%s\n", err); return 1; }
    if (world3d_actors(a, argv[2], &ac, &n) != 0 || n == 0) { printf("aiinfo %s no actors\n", argv[2]); return 0; }
    msim_init(&s, a, argv[2], ac, n);
    s.player_vertical = 1;
    s.log = quiet;
    {
        unsigned char *snd = NULL;
        size_t sl = 0;
        if (dp_read_file("MW2SND.CFG", &snd, &sl) == 0) { if (sl >= 0x24) world3d_density_low = (snd[0x20] | snd[0x21] | snd[0x22] | snd[0x23]) == 0; free(snd); }
    }
    if (world3d_mission(a, argv[2], 1, &world) > 0) msim_set_world(&s, &world);
    if (argc > 4 && strcmp(argv[4], "attack") == 0) s.player_attacks = 1;
    lx = calloc((size_t)n, sizeof *lx); lz = calloc((size_t)n, sizeof *lz); since = calloc((size_t)n, sizeof *since);
    bx = calloc((size_t)n, sizeof *bx); bz = calloc((size_t)n, sizeof *bz); why = calloc((size_t)n, sizeof *why);
    for (k = 0; k < n; k++) { lx[k] = (float)ac[k].mech.origin[0]; lz[k] = (float)ac[k].mech.origin[2]; since[k] = -1; }
    steps = (int)(atof(argv[3]) * 20);
    for (i = 0; i < steps && !s.over; i++) {
        s.debug_invulnerable = 1;
        msim_step(&s, 50);
        if (i % 20) continue;
        for (k = 0; k < n; k++) {   /* every second */
            float t = (float)i / 20.0f, dx = (float)ac[k].mech.origin[0] - lx[k], dz = (float)ac[k].mech.origin[2] - lz[k];
            int driven = s.armed[k] && !s.units[k].destroyed && !s.units[k].immobile && s.minds && s.minds[k].throttle > 0.1f && fabsf(s.minds[k].speed) > 50.0f &&
                         !(s.powered_down && s.powered_down[k]) && !s.units[k].shutdown;
            if (since[k] >= 0 && (!driven || dx * dx + dz * dz > 100.0f * 100.0f)) {   /* moved on: report the interval */
                if (t - since[k] >= 10.0f)
                    printf("aistuck %s actor %d %s state %s thr %.2f from %.0f to %.0f at %.0f,%.0f y %.0f:%s\n", argv[2], k, ac[k].name, ai_state_name(s.minds[k].state), s.minds[k].throttle,
                           since[k], t, bx[k], bz[k], s.units[k].y, why[k]);
                since[k] = -1;
            }
            if (!driven || dx * dx + dz * dz > 100.0f * 100.0f) { lx[k] = (float)ac[k].mech.origin[0]; lz[k] = (float)ac[k].mech.origin[2]; continue; }
            if (since[k] < 0) { since[k] = t; bx[k] = lx[k]; bz[k] = lz[k]; }
            if (t - since[k] >= 5.0f && !why[k][0]) cause(&s, &world, k, why[k], sizeof why[k]);
            if (since[k] == t) why[k][0] = 0;
        }
    }
    for (k = 0; k < n; k++)
        if (since[k] >= 0 && s.now / 1000.0f - since[k] >= 10.0f)
            printf("aistuck %s actor %d %s state %s%s from %.0f to %.0f (to the end) at %.0f,%.0f y %.0f:%s\n", argv[2], k, ac[k].name, s.minds ? ai_state_name(s.minds[k].state) : "-", s.minds && s.minds[k].speed < 0 ? " REV" : "",
                   since[k], s.now / 1000.0f, bx[k], bz[k], s.units[k].y, why[k]);
    printf("aiinfo %s %d actors, %.0f s, outcome %s\n", argv[2], n, s.now / 1000.0f, msim_outcome_text(&s) ? msim_outcome_text(&s) : "none");
    msim_free(&s);
    mech3d_free(&world);
    world3d_free_actors(ac, n);
    prj_close(a);
    return 0;
}
