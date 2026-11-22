/*
 * sweep_ai - tests only (tools/sweep_walk.py's AI pass): run a mission headless as missionsim does and report
 * AI mechs that stop making progress while their AI still drives them (throttle up), with the object in the way.
 *   sweep_ai MODELS.PRJ SCENE SECONDS [attack]
 * Prints "aistuck SCENE actor k NAME state S from T1 to T2 (to the end) at X,Z y Y: <cause>" for every interval
 * of at least 10 s with under 1 m moved and the throttle over 0.1 (the end flag: still stuck when the run ended).
 * Cause: the unit or world part hit by a step probe in the direction of travel, or a clear probe.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "datapath.h"
#include "msim.h"

static void quiet(void *u, const char *l) { (void)u; (void)l; }

static void cause(const msim *s, int i, char *out, size_t n)
{
    const world_actor *actor = &s->actors[i];
    float heading = (actor->mech.heading + (s->minds[i].speed < 0 ? 180.0f : 0.0f)) * 3.14159265f / 180.0f;
    float y = s->units[i].y, height = s->centre_h[i];
    float from[3] = {(float)actor->mech.origin[0], y + height, (float)actor->mech.origin[2]};
    float to[3] = {from[0] + sinf(heading) * 60.0f, from[1], from[2] + cosf(heading) * 60.0f};
    float normal[3], centre[3], radius, velocity[3];
    int hit = msim_unit_contact(s, i, from, to, normal, centre, &radius, velocity);

    if (hit == -1) {
        snprintf(out, n, " step probe hit player");
    } else if (hit >= 0) {
        snprintf(out, n, " step probe hit actor %d (%s)", hit, s->actors[hit].name);
    } else if (!msim_can_step_hr(s, from[0], from[2], to[0], to[2], y, height, s->radius[i])) {
        const char *model = msim_block_what();
        msim_block_normal(normal);
        snprintf(out, n, " step probe hit %s (normal %.2f, %.2f, %.2f)",
                 model && model[0] ? model : "unnamed world part", normal[0], normal[1], normal[2]);
    } else {
        snprintf(out, n, " step probe clear (check AI steering and avoidance)");
    }
}

int main(int argc, char **argv)
{
    char err[256];
    prj_archive *a;
    world_actor *ac;
    int n, i, k, steps;
    msim s;
    static mech3d world;
    float *lx, *lz, *since, *bx, *bz, *hmax;
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
    hmax = calloc((size_t)n, sizeof *hmax);   /* SWEEP_AI_HEIGHT: each unit's greatest height over its ground, cm */
    for (k = 0; k < n; k++) { lx[k] = (float)ac[k].mech.origin[0]; lz[k] = (float)ac[k].mech.origin[2]; since[k] = -1; }
    steps = (int)(atof(argv[3]) * 20);
    for (i = 0; i < steps && !s.over; i++) {
        s.debug_invulnerable = 1;
        msim_step(&s, 50);
        for (k = 0; k < n; k++) if (s.armed[k] && !s.units[k].destroyed && s.units[k].y - s.units[k].ground > hmax[k]) hmax[k] = s.units[k].y - s.units[k].ground;
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
            if (t - since[k] >= 5.0f && !why[k][0]) cause(&s, k, why[k], sizeof why[k]);
            if (since[k] == t) why[k][0] = 0;
        }
    }
    for (k = 0; k < n; k++)
        if (since[k] >= 0 && s.now / 1000.0f - since[k] >= 10.0f)
            printf("aistuck %s actor %d %s state %s%s from %.0f to %.0f (to the end) at %.0f,%.0f y %.0f:%s\n", argv[2], k, ac[k].name, s.minds ? ai_state_name(s.minds[k].state) : "-", s.minds && s.minds[k].speed < 0 ? " REV" : "",
                   since[k], s.now / 1000.0f, bx[k], bz[k], s.units[k].y, why[k]);
    if (getenv("SWEEP_AI_HEIGHT"))
        for (k = 0; k < n; k++) if (hmax[k] > 100.0f * atof(getenv("SWEEP_AI_HEIGHT"))) printf("aiheight %s actor %d %s max %.0f m\n", argv[2], k, ac[k].name, hmax[k] / 100.0f);
    printf("aiinfo %s %d actors, %.0f s, outcome %s\n", argv[2], n, s.now / 1000.0f, msim_outcome_text(&s) ? msim_outcome_text(&s) : "none");
    msim_free(&s);
    mech3d_free(&world);
    world3d_free_actors(ac, n);
    prj_close(a);
    return 0;
}
