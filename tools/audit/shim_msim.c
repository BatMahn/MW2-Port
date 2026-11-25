/* shim_msim.c - the port's msim.c with its statics reachable, for tools/audit/simdiff.py */
#include "../../src/msim.c"
void shim_set_dos(int on) { msim_dos_int = on; }
float shim_servo(float cur, float tgt, float T, float ticks) { return drive_servo(cur, tgt, T, ticks); }
int32_t shim_dos_drive_servo(int32_t value, int32_t target, int ticks, int tau) { return dos_drive_servo(value, target, ticks, tau); }
float shim_oct_len(float x, float y, float z) { return oct_len(x, y, z); }

void shim_dos_hull_bounds(const int32_t *xyz, int count, int32_t sphere[4])
{
    wtb_vertex vertices[64];
    if (count < 0 || count > 64) return;
    for (int i = 0; i < count; i++)
        vertices[i] = (wtb_vertex){.x = xyz[i * 3], .y = xyz[i * 3 + 1], .z = xyz[i * 3 + 2]};
    wtb_object model = {.vert_count = count, .verts = vertices};
    mech3d_dos_model_sphere(&model, sphere);
}

void shim_dos_lods(const int32_t *actors_in, int count, const int32_t camera[3], int scale, int mode, int32_t *out)
{
    msim *s = calloc(1, sizeof *s);
    if (!s) return;
    world_actor actors[64] = {0};
    combat_unit units[64] = {0};
    int armed[64];
    unsigned char repr[64], states[65][3];
    struct msim_dos_hull ordered[65] = {0};
    const int has_player = mode != 0;
    const int actor_count = count - has_player;
    mech3d (*models)[3] = calloc((size_t)actor_count + 1, sizeof *models);
    if (!models) { free(s); return; }
    memset(states, 1, sizeof states);
    for (int i = 0; i < actor_count; i++) armed[i] = 1;
    s->actor_count = actor_count;
    s->actors = actors;
    s->units = units;
    s->col_repr = repr;
    s->armed = armed;
    s->pl_mech_ok = s->pl_pit_ok = has_player;
    s->player_cockpit = mode == 1;
    s->player_external = mode == 2;
    s->cam_ok = 1;
    s->cam_a0 = (float)scale / 65536.0f;
    for (int axis = 0; axis < 3; axis++) s->cam_eye[axis] = (float)camera[axis];
    for (int i = 0; i < count; i++) {
        const int32_t *p = &actors_in[i * 5];
        const int unit = i - has_player, r = p[4];
        const mech3d *previous;
        if (unit < 0) {
            s->player[0] = (float)p[0];
            s->player[2] = (float)p[2];
            s->player_unit.y = (float)p[1];
            s->player_unit.destroyed = p[3];
            previous = r == 4 ? &s->pl_pit : r ? &models[actor_count][r - 1] : &s->pl_mech;
        } else {
            memcpy(actors[unit].mech.origin, p, 3 * sizeof *p);
            units[unit].y = (float)p[1];
            units[unit].destroyed = p[3];
            previous = r ? &models[unit][r - 1] : &actors[unit].mech;
        }
        int32_t h = dos_level_distance(p[0] - camera[0], p[2] - camera[2]);
        out[i * 4] = h;
        out[i * 4 + 1] = dos_slant_distance(h, p[1] - camera[1]);
        ordered[count - 1 - i] = (struct msim_dos_hull){.repr = previous, .unit = unit, .sequence = i + 1};
    }
    collision_lods(s);
    s->col_lod = models;
    s->col_lod_ok = states;
    s->dos_hulls = ordered;
    s->clist_next = count;
    if (!has_player) ordered[count].unit = -1;
    dos_sync_hull_order(s);
    for (int i = 0; i < count; i++) {
        out[i * 4 + 2] = i < has_player ? (s->player_cockpit ? 4 : s->player_col_repr) : repr[i - has_player];
        out[i * 4 + 3] = ordered[i].unit + has_player;
    }
    free(models);
    free(s);
}

void shim_dos_probe_points(const int32_t origin[3], float heading, int step, int side,
                           int length, int lateral, int32_t out[6])
{
    const float centre[3] = {(float)origin[0], (float)origin[1], (float)origin[2]};
    float p[3], q[3];
    dos_probe_points(centre, heading, step, side, length, lateral, p, q);
    for (int i = 0; i < 3; i++) {
        out[i] = (int32_t)p[i];
        out[i + 3] = (int32_t)q[i];
    }
}

void shim_dos_contact_math(const int32_t start[3], const int32_t delta[3], const int32_t sphere[4],
                          const int32_t bounds[6], int32_t out[9])
{
    float o[3], d[3], sph[4], box[6];
    for (int i = 0; i < 3; i++) {
        o[i] = (float)start[i];
        d[i] = (float)delta[i];
    }
    for (int i = 0; i < 4; i++) sph[i] = (float)sphere[i];
    for (int i = 0; i < 6; i++) box[i] = (float)bounds[i];
    const dos_shot_segment ray = dos_shot_segment_make(o, d, 1);
    out[0] = ray.length;
    for (int i = 0; i < 3; i++) out[i + 1] = ray.dir[i];
    out[4] = dos_shot_sphere(&ray, sph);
    out[5] = dos_shot_box(&ray, box);
    float effect_at[3];
    dos_impact_effect(d, o, effect_at);
    for (int i = 0; i < 3; i++) out[i + 6] = (int32_t)effect_at[i];
}

void *shim_projectile_pool_new(const uint8_t *kinds, int count)
{
    msim *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    memcpy(s->dos_shot_kinds, kinds, (size_t)count);
    s->dos_shot_slots = count;
    return s;
}

int shim_projectile_pool_insert(void *pool, int kind)
{
    msim_shot *sh = alloc_shot(pool, kind);
    return sh ? sh->dos_slot : -1;
}

void shim_projectile_pool_release(void *pool, int slot)
{
    msim *s = pool;
    for (int i = 0; i < s->shot_count; i++)
        if (s->shots[i].dos_slot == slot) {
            remove_shot(s, i);
            return;
        }
}

int shim_projectile_pool_at(void *pool, int index)
{
    const msim *s = pool;
    return index < s->shot_count ? s->shots[index].dos_slot : -1;
}

void shim_projectile_pool_free(void *pool) { free(pool); }

int shim_dos_projectile_obstruction(int first, int second, int newest)
{
    msim *s = calloc(1, sizeof *s);
    if (!s) return -2;
    int idx[2] = {0, 1}, stamp[2] = {0};
    cell_t cell = {.idx = idx, .n = 2};
    cobj_t objects[2] = {0};
    const int z[2] = {first, second};
    terrain_t terrain = {.co = objects, .nco = 2, .ccell = &cell, .stamp = stamp,
                         .gw = 1, .gh = 1, .cs = 10000.0f, .x0 = -1000.0f, .z0 = -1000.0f};
    s->terrain = &terrain;
    for (int i = 0; i < 2; i++) {
        int id = i ? 1 - newest : newest;
        objects[i] = (cobj_t){.part = id, .type = 0, .box = {-100, 100, 100, 300, (float)z[id], (float)z[id] + 200},
                              .sph = {0, 200, (float)z[id] + 100, 174}};
    }
    const float from[3] = {0, 200, 0}, dv[3] = {0, 0, 2400};
    float contact[3];
    int part = -1, unit = -2, group = 0;
    dos_shot_contact(s, -1, from, dv, &unit, &group, &part, contact);
    free(s);
    return part;
}

void shim_dos_mixed_contact(int terrain_z, int unit_z, int world_first, int skip_unit, int32_t out[4])
{
    msim *s = calloc(1, sizeof *s);
    if (!s) return;
    int idx = 0, stamp = 0, world_sequence = world_first ? 2 : 1;
    const float z = (float)terrain_z;
    cell_t cell = {.idx = &idx, .n = 1};
    cobj_t object = {.type = 0, .box = {-100, 100, 100, 300, z, z + 200},
                     .sph = {0, 200, z + 100, 174}};
    terrain_t terrain = {.co = &object, .nco = 1, .ccell = &cell, .stamp = &stamp,
                         .gw = 1, .gh = 1, .cs = 10000.0f, .x0 = -1000.0f, .z0 = -1000.0f};
    wtb_vertex vertices[2] = {{.x = -100}, {.x = 100}};
    wtb_object model = {.vert_count = 2, .verts = vertices};
    mech3d_part pieces[2] = {
        {.objl = 1, .group = 4, .pos = {0, 200, unit_z}, .model = {.object_count = 1, .objects = &model}},
        {.objl = 1, .group = 6, .pos = {0, 200, unit_z}, .model = {.object_count = 1, .objects = &model}},
    };
    for (int i = 0; i < 2; i++) mech3d_dos_model_sphere(&model, pieces[i].dos_sphere);
    mech3d hull = {.part_count = 2, .parts = pieces};
    struct msim_dos_hull entry = {.repr = &hull, .unit = -1, .sequence = 3 - world_sequence};
    s->dos_hulls = &entry;
    s->terrain = &terrain;
    s->clist_seq = &world_sequence;
    s->clist_seq_n = 1;
    const float from[3] = {0, 200, 0}, dv[3] = {0, 0, 2400};
    float contact[3] = {0};
    int unit = -2, group = 0, part = -1;
    int hit = dos_shot_contact(s, skip_unit ? -1 : 3, from, dv, &unit, &group, &part, contact);
    out[0] = hit && part >= 0 ? 1 : hit ? 2 : 0;
    out[1] = unit == -1 ? group : 0;
    out[2] = (int32_t)contact[1];
    out[3] = (int32_t)contact[2];
    free(s);
}

int shim_dos_projectile_terrain(int barrier)
{
    msim *s = calloc(1, sizeof *s);
    if (!s) return -1;
    int idx = 0, stamp = 0;
    cell_t cell = {.idx = &idx, .n = 1};
    cobj_t object = {.type = 5, .box = {-300, 300, 0, 800, 300, 600},
                     .sph = {0, 400, 450, 530}, .root = 0};
    qnode_t node = {.r = {-300, 300, 0, 800, 300, 600}, .np = 1};
    cpoly_t face = {.n = 4, .nrm = {0, 0, -1},
                    .v = {{-300, 0, 300}, {-300, 800, 300}, {300, 800, 300}, {300, 0, 300}}};
    terrain_t terrain = {.co = &object, .nco = 1, .ccell = &cell, .stamp = &stamp,
                         .qn = &node, .cp = &face, .qidx = &idx,
                         .gw = 1, .gh = 1, .cs = 2000.0f, .x0 = -400.0f, .z0 = -400.0f};
    mech3d_part part = {0};
    mech3d world = {.parts = &part, .part_count = 1};
    s->world = &world;
    s->terrain = barrier ? &terrain : NULL;
    s->player_unit.destroyed = 1;
    s->shots[0] = (msim_shot){.y = 400, .vz = 200, .dos_vel = {0, 0, 200 * 65536},
                               .life = 90, .owner = -1, .weapon = 22, .homing = -2};
    s->shot_count = 1;
    combat_step(s, 6.0f / 182.0f, 6);
    int alive = s->shot_count;
    free(s);
    return alive;
}

unsigned shim_dos_projectile_launch(int weapon, unsigned seed, int32_t gravity, const int32_t from[3],
                                    const int32_t to[3], int32_t out[7])
{
    msim *s = calloc(1, sizeof *s);
    if (!s) return seed;
    s->rng = seed;
    s->planet.gravity_g = (float)gravity / 65536.0f;
    const float muzzle[3] = {(float)from[0], (float)from[1], (float)from[2]};
    const float aim[3] = {(float)to[0], (float)to[1], (float)to[2]};
    spawn_shot_h(s, weapon, 0, muzzle, aim, 0.0f, -2);
    const msim_shot *sh = &s->shots[0];
    out[0] = (int32_t)sh->x; out[1] = (int32_t)sh->y; out[2] = (int32_t)sh->z;
    memcpy(out + 3, sh->dos_vel, sizeof sh->dos_vel);
    out[6] = (int32_t)lrintf(sh->ay * 65536.0f);
    seed = s->rng;
    free(s);
    return seed;
}

int shim_dos_projectile_guide(const int32_t vel[3], int32_t delta[3], int32_t acc[3])
{
    msim_shot sh = {.dos_vel = {vel[0], vel[1], vel[2]}};
    dos_projectile_guide(&sh, delta);
    acc[0] = (int32_t)lrintf(sh.ax * 65536.0f);
    acc[1] = (int32_t)lrintf(sh.ay * 65536.0f);
    acc[2] = (int32_t)lrintf(sh.az * 65536.0f);
    return sh.prox;
}

int shim_dos_projectile_frame(int ticks, int life, const int32_t pos[3],
                              const int32_t vel[3], const int32_t acc[3], int32_t out[6])
{
    msim *s = calloc(1, sizeof *s);
    if (!s) return -1;
    s->player_unit.destroyed = 1;
    s->shot_count = 1;
    s->shots[0] = (msim_shot){
        .x = (float)pos[0], .y = (float)pos[1], .z = (float)pos[2],
        .vx = (float)vel[0] / 65536.0f, .vy = (float)vel[1] / 65536.0f, .vz = (float)vel[2] / 65536.0f,
        .dos_vel = {vel[0], vel[1], vel[2]},
        .ax = (float)acc[0] / 65536.0f, .ay = (float)acc[1] / 65536.0f, .az = (float)acc[2] / 65536.0f,
        .life = (float)life, .homing = -2
    };
    /* Use a fractional dt as in a native frame. Projectile flight must use only the whole ticks. */
    combat_step(s, (ticks + 0.25f) / 182.0f, ticks);
    const msim_shot *sh = &s->shots[0];
    out[0] = (int32_t)sh->x; out[1] = (int32_t)sh->y; out[2] = (int32_t)sh->z;
    memcpy(out + 3, sh->dos_vel, sizeof sh->dos_vel);
    int alive = s->shot_count;
    free(s);
    return alive;
}

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
    wtb_object model = {0};
    struct msim_dos_hull hulls[3] = {{.unit = 0}, {.unit = 1}, {.unit = -1}};
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
    s->dos_hulls = hulls;
    s->reticle_actor = -1;
    for (int i = 0; i < 2; i++) {
        actors[i].mech.origin[2] = ranges[i] + radius;
        actors[i].mech.part_count = 1;
        actors[i].mech.parts = &parts[i];
        parts[i].pos[1] = 500;
        parts[i].pos[2] = actors[i].mech.origin[2];
        parts[i].group = 3;
        parts[i].objl = parts[i].model.object_count = parts[i].sphere_ok = 1;
        parts[i].model.objects = &model;
        parts[i].dos_sphere[3] = radius;
        parts[i].hull_radius = (float)radius;
        hulls[i].repr = &actors[i].mech;
        hulls[i].sequence = 2 - i;
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

int32_t shim_dos_shot_face(const float *v, int n, const float *nrm, const float *p0, const float *p1, int long_step, float *out)
{
    cpoly_t face = {.n = n};
    memcpy(face.nrm, nrm, sizeof face.nrm);
    memcpy(face.v, v, (size_t)n * sizeof face.v[0]);
    float dv[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    dos_shot_segment segment = dos_shot_segment_make(p0, dv, long_step);
    int32_t normal_y;
    return dos_shot_face(&segment, &face, out, &normal_y);
}
