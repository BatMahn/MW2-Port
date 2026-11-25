/* shim_msim.c - the port's msim.c with its statics reachable, for tools/audit/simdiff.py */
#include "../../src/msim.c"
void shim_set_dos(int on) { msim_dos_int = on; }
float shim_servo(float cur, float tgt, float T, float ticks) { return drive_servo(cur, tgt, T, ticks); }
int32_t shim_dos_drive_servo(int32_t value, int32_t target, int ticks, int tau) { return dos_drive_servo(value, target, ticks, tau); }
float shim_oct_len(float x, float y, float z) { return oct_len(x, y, z); }

int shim_ai_volley(float dt, int ticks, int online, int rounds, int elapsed, int *lock)
{
    msim *s = calloc(1, sizeof *s);
    world_actor actor = {0};
    combat_unit unit = {0};
    ai_mind mind = {0};
    float height = 1000.0f, online_at = online ? 0.0f : 1000.0f;
    int armed = 1, launched;
    if (!s) return -1;
    s->actor_count = 1;
    s->actors = &actor;
    s->units = &unit;
    s->minds = &mind;
    s->height = &height;
    s->armed = &armed;
    s->online_at = &online_at;
    s->ai_ok = 1;
    s->player_target = -2;
    s->player_unit.destroyed = 1;
    actor.alliance = 1;
    mind.tgt = -2;
    mind.lock = 0x80c0;
    unit.no_heat = 1;
    unit.weapon_count = 1;
    unit.weapons[0].weapon = 5;
    unit.weapons[0].state = CW_FIRING;
    unit.weapons[0].shots = rounds;
    unit.weapons[0].t = (float)elapsed;
    unit.weapons[0].homing = -2;
    unit.weapons[0].ammo = -1;
    msim_set_dos(1);
    combat_step(s, dt, ticks);
    launched = (int)s->shot_serial;
    *lock = mind.lock;
    free(s);
    return launched;
}

int shim_reticle_contact(int first, int second, int radius, int wreck, float *distance)
{
    msim *s = calloc(1, sizeof *s);
    world_actor actors[2] = {0};
    combat_unit units[2] = {0};
    mech3d_part parts[2] = {0};
    const int ranges[2] = {first, second};
    const float origin[3] = {0, 500, 0}, dir[3] = {0, 0, 1};
    float heights[2] = {1000, 1000};
    int armed[2] = {1, 1}, actor;
    if (!s) return -1;
    s->actor_count = 2;
    s->actors = actors;
    s->units = units;
    s->armed = armed;
    s->height = heights;
    s->reticle_actor = -1;
    for (int i = 0; i < 2; i++) {
        actors[i].mech.origin[2] = ranges[i] + radius;
        actors[i].mech.part_count = 1;
        actors[i].mech.parts = &parts[i];
        parts[i].pos[1] = 500;
        parts[i].pos[2] = actors[i].mech.origin[2];
        parts[i].group = 3;
        parts[i].objl = parts[i].model.object_count = parts[i].sphere_ok = 1;
        parts[i].hull_radius = (float)radius;
        units[i].destroyed = i == wreck;
    }
    aim_ray(s, -1, origin, dir, distance);
    actor = s->reticle_actor;
    free(s);
    return actor;
}

int shim_player_lock(int y, int z, int flags, int *timer, int ticks)
{
    msim *s = calloc(1, sizeof *s);
    world_actor actor = {0};
    combat_unit unit = {0};
    float height = 1000;
    int armed = 1, lock;
    if (!s) return -1;
    s->actor_count = 1;
    s->actors = &actor;
    s->units = &unit;
    s->armed = &armed;
    s->height = &height;
    s->player_lock = flags;
    s->player_lock_ticks = (float)*timer;
    s->player_unit.weapon_count = 1;
    actor.mech.origin[2] = z;
    unit.y = (float)y;
    player_lock_step(s, 950.0f, (float)ticks);
    lock = s->player_lock;
    *timer = (int)s->player_lock_ticks;
    free(s);
    return lock;
}

/* one face against one segment (eseg_poly): v = n world vertices, nrm the face normal; returns 1 and the cut end */
int shim_seg_poly(const float *v, int n, const float *nrm, const float *p0, const float *p1, float *out)
{
    cpoly_t c;
    eseg_t g;
    float nn[3];
    int j, a;
    memset(&c, 0, sizeof c);
    c.n = n;
    for (a = 0; a < 3; a++) { c.nrm[a] = nrm[a]; c.mn[a] = 3.4e38f; c.mx[a] = -3.4e38f; }
    for (j = 0; j < n; j++) for (a = 0; a < 3; a++) {
        c.v[j][a] = v[j * 3 + a];
        if (c.v[j][a] < c.mn[a]) c.mn[a] = c.v[j][a];
        if (c.v[j][a] > c.mx[a]) c.mx[a] = c.v[j][a];
    }
    eseg_set(&g, p0, p1);
    if (!eseg_poly(&c, &g, nn)) return 0;
    for (a = 0; a < 3; a++) out[a] = g.e[a];
    return 1;
}
