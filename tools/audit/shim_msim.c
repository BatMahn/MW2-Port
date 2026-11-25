/* shim_msim.c - the port's msim.c with its statics reachable, for tools/audit/simdiff.py */
#include "../../src/msim.c"
void shim_set_dos(int on) { msim_dos_int = on; }
float shim_servo(float cur, float tgt, float T, float ticks) { return drive_servo(cur, tgt, T, ticks); }
int32_t shim_dos_drive_servo(int32_t value, int32_t target, int ticks, int tau) { return dos_drive_servo(value, target, ticks, tau); }
void shim_dos_drive_frame(int32_t current, int32_t target, int32_t speed, int32_t factor,
                          int ticks, int reverse, int slow, int32_t out[2])
{
    const double scale = 65536.0 / 182.0;
    float throttle = current / 1024.0f, drive = (float)(speed / scale), velocity = 0;
    msim_drive_step(&throttle, &drive, &velocity, target / 1024.0f, reverse ? -0.5f : 1.0f,
                    (float)(factor * 1024.0 / scale), 1, slow, (float)ticks);
    out[0] = (int32_t)lroundf(throttle * 1024.0f);
    out[1] = (int32_t)lround(drive * scale);
}
float shim_oct_len(float x, float y, float z) { return oct_len(x, y, z); }

int shim_dos_scenery(const int32_t point[3], int radius, int near, int far,
                     const int32_t focal[2], const int32_t normal[2],
                     int scale, const int32_t distances[3], int previous)
{
    int64_t limit = (int64_t)far + radius;
    int64_t distance2 = 0;
    for (int axis = 0; axis < 3; axis++) distance2 += (int64_t)point[axis] * point[axis];
    if (distance2 > limit * limit || !dos_scenery_visible(point, radius, near, far, focal, normal))
        return previous;
    scenery_faces models[3] = {{0}};
    for (int i = 0; i < 3; i++) models[i].distance = distances[i];
    return dos_scenery_level(models, 3, point[2], scale);
}

unsigned shim_dos_startup(int distance, unsigned *index)
{
    msim s = {0};
    const float pos[3] = {0, 0, (float)distance};
    dos_random_init(&s);
    s.dos_random_index = *index;
    unsigned delay = startup_ticks(&s);
    startup_sound(&s, 0xf7, pos);
    *index = s.dos_random_index;
    return delay;
}

void shim_dos_random(const unsigned *limits, unsigned *out, int count)
{
    msim s = {0};
    dos_random_init(&s);
    for (int i = 0; i < count; i++) out[i] = ms_roll(&s, limits[i]);
}

int shim_dos_warning_roll(unsigned *index, int weapon, int level)
{
    msim s = {0};
    dos_random_init(&s);
    s.dos_random_index = *index;
    int react = dos_warning_roll(&s, weapon, level);
    *index = s.dos_random_index;
    return react;
}

static void record_selection_sound(void *user, int sound, const float pos[3])
{
    *(int *)user = pos ? -sound : sound;
}

int shim_dos_selection_effects(unsigned *index, int weapon, int skill, int target, int *sound)
{
    msim s = {.shot_user = sound, .on_sound = record_selection_sound};
    dos_random_init(&s);
    s.dos_random_index = *index;
    *sound = 0;
    int locked = dos_weapon_selection_effects(&s, weapon, skill, target);
    *index = s.dos_random_index;
    return locked;
}

int shim_dos_lock_window(int weapon, int32_t x, int32_t y, int32_t z, int32_t yaw, int32_t pitch)
{
    return dos_lock_in_window(weapon, x, y, z, yaw, pitch);
}

void shim_dos_sound(const int32_t input[5], unsigned *index, int out[3])
{
    msim s = {0};
    dos_random_init(&s);
    s.dos_random_index = *index;
    out[0] = msim_dos_sound_at(&s, input[0], input[1], input[2], input[3], input[4],
                               1, &out[1], &out[2]);
    *index = s.dos_random_index;
}

int shim_dos_sound_pitch(unsigned *index)
{
    msim s = {0};
    dos_random_init(&s);
    s.dos_random_index = *index;
    int rate = msim_dos_sound_pitch(&s);
    *index = s.dos_random_index;
    return rate;
}

int shim_dos_contact_state(int state, int idle_target)
{
    msim s = {.player_radius = 500};
    world_actor actor = {0};
    combat_unit target = {.offline = idle_target};
    s.actor_count = 1;
    s.actors = &actor;
    s.units = &target;
    s.player_unit.offline = state == 0 || state == 1;
    s.player_unit.shutdown = state == 3;
    s.player_unit.destroyed = state == 4 || state == 5;
    s.player_unit.intact_out = state == 5;
    float p[3] = {0, 550, 100}, normal[3], centre[3], velocity[3], radius;
    return msim_unit_contact(&s, -1, p, p, normal, centre, &radius, velocity) == 0;
}

void shim_dos_contact_order(int mover, const int32_t *bodies, const int32_t *position, int32_t *out)
{
    msim s = {.actor_count = 3};
    world_actor actors[3] = {0};
    combat_unit units[3] = {0};
    float heights[3] = {0}, radii[3];
    s.actors = actors;
    s.units = units;
    s.centre_h = heights;
    s.radius = radii;
    for (int i = 0; i < 3; i++) {
        const int32_t *body = bodies + i * 9;
        memcpy(actors[i].mech.origin, body, 3 * sizeof *body);
        units[i].y = body[1];
        units[i].destroyed = body[3];
        units[i].dos_contact_unit = body[4];
        radii[i] = body[5];
        for (int k = 0; k < 3; k++) units[i].dos_attempted[k] = body[6 + k] / 65536.0f;
    }
    s.player_unit.destroyed = 1;
    const float p[3] = {position[0], position[1], position[2]};
    float n[3], centre[3], v[3], radius;
    int hit = msim_unit_contact(&s, mover, p, p, n, centre, &radius, v);
    out[0] = hit >= 0 ? hit + 2 : 0;
    if (hit < 0) return;
    float end[3], bounced[3];
    dos_unit_bounce(centre, n, radius, units[mover].dos_attempted, v, end, bounced);
    for (int k = 0; k < 3; k++) {
        out[1 + k] = (int32_t)end[k];
        out[4 + k] = (int32_t)lrintf(bounced[k] * 65536.0f);
    }
}

void shim_dos_anim_frames(const int32_t *keys, int count, const int32_t *frames,
                          int frame_count, int rate, int kind, unsigned phase, int32_t *out)
{
    anim_set *anim = calloc(1, sizeof *anim);
    if (!anim) return;
    anim->key_count = count;
    anim->track_count = 1;
    anim->tracks[0].type = kind;
    for (int k = 0; k < count; k++) {
        uint32_t bytes = (uint32_t)keys[k * 3 + 2];
        anim->tracks[0].delta[k] = keys[k * 3];
        anim->key_flags[k] = (uint32_t)keys[k * 3 + 1];
        for (int j = 0; j < 4; j++) anim->key_bytes[k][j] = (uint8_t)(bytes >> (8 * j));
    }
    anim_player p;
    anim_player_init(&p, rate);
    mech3d_part part = {.node = 0};
    mech3d model = {.parts = &part, .part_count = 1, .skel_count = 1, .skel_parent = {-1},
                    .bind_count = 1, .twist_node = -1, .pitch_node = -1, .pose_dos = &p};
    int previous_mode = mech3d_dos_hips;
    mech3d_dos_hips = 1;
    mech3d_dos_anim_init(&model, anim, &p, &phase);
    for (int k = 0; k < frame_count; k++) {
        const int32_t *frame = frames + 4 * k;
        if (frame[3]) anim_player_hold(&p);
        int ev = anim_player_step(anim, &p, frame[0], frame[1], rate, (float)frame[2], 1);
        mech3d_dos_anim_step(&model, anim, &p, &phase);
        model.heading = (float)(k % 37 - 18) * 5.25f;
        model.origin[0] = -10000 + 82 * k;
        model.origin[1] = 400;
        model.origin[2] = 28000 - 49 * k;
        mech3d_pose(&model, anim, p.t);
        int32_t *row = out + 22 * k;
        row[0] = p.on;
        row[1] = p.key;
        row[2] = p.cur;
        row[3] = (int32_t)p.left;
        row[4] = p.dur;
        row[5] = p.dos_value[0];
        row[6] = p.dos_pending[0];
        row[7] = ev;
        memcpy(row + 8, part.dos_rot, sizeof part.dos_rot);
        memcpy(row + 17, part.pos, sizeof part.pos);
        row[20] = (int32_t)p.dos_rot_left[0];
        row[21] = (int32_t)phase;
    }
    mech3d_dos_hips = previous_mode;
    free(anim);
}

void shim_dos_motion_plane(int32_t velocity[2], int32_t drive, int32_t angle, int32_t thrust, int32_t top,
                           int freev, int jet_dir, int ticks, int32_t move[2])
{
    dos_motion_plane(velocity, drive, dos_sin(angle) >> 13, dos_cos(angle) >> 13,
                     thrust, top, freev, jet_dir, ticks, move);
}

void shim_dos_face_response(const int32_t *direction, const int32_t *normal, int32_t *velocity,
                            int travel, int32_t *rebound)
{
    dos_face_response(direction, normal, velocity, travel, rebound);
}

int32_t shim_dos_motion_lift(int32_t *velocity, int32_t gravity, int32_t thrust,
                            int32_t damping, int grounded, int mode, int ticks)
{
    return dos_motion_lift(velocity, gravity, thrust, damping, grounded, mode, ticks);
}

void shim_dos_unit_bounce(const int32_t *centre, const int32_t *normal, int32_t radius,
                          const int32_t *vel, const int32_t *other, int32_t *out)
{
    float c[3], n[3], v[3], ov[3], end[3], bounce[3];
    for (int k = 0; k < 3; k++) {
        c[k] = (float)centre[k];
        n[k] = normal[k] / 65536.0f;
        v[k] = vel[k] / 65536.0f;
        ov[k] = other[k] / 65536.0f;
    }
    dos_unit_bounce(c, n, (float)radius, v, ov, end, bounce);
    for (int k = 0; k < 3; k++) {
        out[k] = (int32_t)end[k];
        out[k + 3] = (int32_t)lrintf(bounce[k] * 65536.0f);
    }
}

void shim_dos_skeleton(int32_t heading, const int32_t angles[4], const int32_t origin[3],
                       const int32_t offsets[9], const int32_t point[3], int translation,
                       int quarter, int live_sample, int32_t out[39])
{
    mech3d_part parts[3] = {{.node = 0}, {.node = 1}, {.node = 2}};
    mech3d model = {.parts = parts, .part_count = 3, .skel_count = 3, .twist_node = 1, .pitch_node = 2,
                    .skel_index = {2, 0, 1}, .skel_parent = {1, -1, 0},
                    .bind_count = 2, .bind_node = {0, 2}, .bind_track = {0, 1}};
    anim_set *anim = calloc(1, sizeof *anim);
    if (!anim) return;
    anim->key_count = 1;
    anim->track_count = 2;
    anim->tracks[0].type = 5;
    anim->tracks[0].pose[1] = angles[3] / 65536.0f;
    anim->tracks[1].type = 1;
    anim->tracks[1].pose[1] = (float)translation;
    memcpy(model.origin, origin, sizeof model.origin);
    memcpy(model.skel_offset[0], offsets + 6, sizeof model.skel_offset[0]);
    memcpy(model.skel_offset[1], offsets, sizeof model.skel_offset[1]);
    memcpy(model.skel_offset[2], offsets + 3, sizeof model.skel_offset[2]);
    anim_player sample = {.dos_value = {angles[3] * quarter / 4, translation * quarter / 4}};
    if (live_sample) model.pose_dos = &sample;
    model.heading = heading / 65536.0f;
    model.tilt = angles[0] / 65536.0f;
    model.twist = angles[1] / 65536.0f;
    model.pitch = angles[2] / 65536.0f;
    int previous_mode = mech3d_dos_hips;
    mech3d_dos_hips = 1;
    mech3d_pose(&model, anim, quarter * 0.25f);
    mech3d_dos_hips = previous_mode;
    for (int j = 0; j < 3; j++) {
        memcpy(out + 12 * j, parts[j].dos_rot, sizeof parts[j].dos_rot);
        memcpy(out + 12 * j + 9, parts[j].pos, sizeof parts[j].pos);
    }
    dos_matrix last;
    memcpy(last.r, parts[2].dos_rot, sizeof last.r);
    memcpy(last.p, parts[2].pos, sizeof last.p);
    dos_matrix_point(&last, point, out + 36);
    free(anim);
}

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
    out[4] = dos_shot_sphere(&ray, sph, 1);
    out[5] = dos_shot_box(&ray, box);
    float effect_at[3];
    dos_impact_effect(d, o, effect_at);
    for (int i = 0; i < 3; i++) out[i + 6] = (int32_t)effect_at[i];
}

void shim_player_scenery_shots(int out[4])
{
    wtb_vertex vertices[4] = {
        {.x = -1000, .y = -1000}, {.x = 1000, .y = -1000},
        {.x = 1000, .y = 1000}, {.x = -1000, .y = 1000}
    };
    wtb_poly polygon = {.n = 4, .idx = {0, 1, 2, 3}};
    wtb_object models[2] = {
        {.name = "WALL_1", .flags = 0x1000, .verts = vertices, .vert_count = 4, .polys = &polygon, .poly_count = 1},
        {.name = "WALL_8500", .flags = 0x1000, .verts = vertices, .vert_count = 4, .polys = &polygon, .poly_count = 1}
    };
    mech3d_part wall = {.pos = {0, 0, 4000}, .coll = 7, .model = {models, 2, 0}};
    mech3d world = {.parts = &wall, .part_count = 1};
    msim *s = calloc(1, sizeof *s);
    if (!s) return;
    msim_dos_int = 1;
    s->world = &world;
    s->terrain = terrain_build(&world, NULL, 0);
    const float origin[3] = {0, 0, 2000}, delta[3] = {0, 0, 4000}, direction[3] = {0, 0, 1};
    msim_camera view = {.eye = {0, 0, 2000}, .yaw = 180, .lod_scale = 1,
                        .near_clip = 160, .far_clip = 40000, .focal_x = 65536, .focal_y = 87380};
    for (int i = 0; i < 2; i++) {
        msim_camera_lods(s, &view);
        int unit = -2, group = 0, part = -1;
        float contact[3], distance;
        out[i * 2] = dos_shot_contact(s, -1, origin, delta, &unit, &group, &part, contact) && part == 0
                     ? (int)contact[2] : -1;
        out[i * 2 + 1] = dos_aim_contact(s, -1, origin, direction, &unit, &part, &distance) && part == 0
                         ? (int)distance : -1;
        view.yaw = 0;
    }
    terrain_free(s->terrain);
    free(s);
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
    projectile_step(s, 6.0f, 6);
    int alive = s->shot_count;
    free(s);
    return alive;
}

unsigned shim_dos_projectile_launch(int weapon, unsigned roll, int32_t gravity, const int32_t from[3],
                                    const int32_t to[3], int32_t out[7])
{
    msim *s = calloc(1, sizeof *s);
    if (!s) return 0;
    s->dos_random[0] = (uint16_t)roll;
    s->planet.gravity_g = (float)gravity / 65536.0f;
    const float muzzle[3] = {(float)from[0], (float)from[1], (float)from[2]};
    const float aim[3] = {(float)to[0], (float)to[1], (float)to[2]};
    spawn_shot_h(s, weapon, 0, muzzle, aim, 0.0f, -2);
    const msim_shot *sh = &s->shots[0];
    out[0] = (int32_t)sh->x; out[1] = (int32_t)sh->y; out[2] = (int32_t)sh->z;
    memcpy(out + 3, sh->dos_vel, sizeof sh->dos_vel);
    out[6] = (int32_t)lrintf(sh->ay * 65536.0f);
    unsigned draws = s->dos_random_index;
    free(s);
    return draws;
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
    projectile_step(s, ticks + 0.25f, ticks);
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
    dos_ai_weapon_step(s, 0, dt, ticks, !online);
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

void shim_dos_gaussian(int32_t *out, int count)
{
    msim s = {0};
    dos_random_init(&s);
    for (int i = 0; i < count; i++) out[i] = dos_gauss(&s);
}

void shim_dos_debris_launch(unsigned index, int32_t gravity, int32_t *out)
{
    msim s = {0};
    msim_debris d = {0};
    dos_random_init(&s);
    s.dos_gaussian_index = index;
    s.planet.gravity_g = (float)gravity / 65536.0f;
    debris_start_motion(&s, &d, 0, NULL);
    memcpy(out, d.dos_velocity, sizeof d.dos_velocity);
    memcpy(out + 3, d.dos_spin, sizeof d.dos_spin);
    out[6] = (int32_t)s.dos_gaussian_index;
}

int shim_dos_detached_pose(const int32_t origin[3], int32_t out[5])
{
    msim s = {0};
    world_actor actor = {0};
    mech3d_part limb = {.node = 1, .group = 5, .model = {.object_count = 1}};
    mech3d *mech = &actor.mech;
    s.actors = &actor;
    s.planet.gravity_g = 1.0f;
    mech->parts = &limb;
    mech->part_count = mech->skel_count = 1;
    mech->skel_index[0] = 1;
    mech->skel_parent[0] = -1;
    mech->skel_offset[0][2] = 200;
    int saved = mech3d_dos_hips;
    mech3d_dos_hips = 1;
    mech3d_pose(mech, NULL, 0);
    memcpy(mech->origin, origin, sizeof mech->origin);
    mech->heading = 90.0f;
    blow_off(&s, 0, 1u << 5, 0);
    mech3d_dos_hips = saved;
    for (int k = 0; k < 3; k++) out[k] = (int32_t)s.debris[0].p[k];
    out[3] = s.debris[0].dos_rot[2];
    out[4] = s.debris[0].dos_rot[8];
    return s.debris_count;
}

void shim_dos_debris(const int32_t *in, int32_t *out)
{
    const float slope = in[13] * 0.5f, height = in[13] ? 120.0f : 0.0f;
    cpoly_t face = {.n = 4, .nrm = {-slope, 1, 0},
                    .v = {{0, height - 1000*slope, 0}, {0, height - 1000*slope, 3000},
                          {2000, height + 1000*slope, 3000}, {2000, height + 1000*slope, 0}}};
    qnode_t leaf = {.r = {0, 2000, -500, 1000, 0, 3000}, .np = 1};
    cobj_t tile = {.type = 5};
    int index = 0, stamp = 0;
    cell_t cell = {.idx = &index, .n = 1};
    terrain_t terrain = {.co = &tile, .nco = 1, .ccell = &cell, .stamp = &stamp, .qn = &leaf,
                         .cp = &face, .qidx = &index, .gw = 1, .gh = 1, .cs = 5000.0f};
    msim s = {.terrain = &terrain};
    dos_random_init(&s);
    s.dos_random_index = (unsigned)in[10];
    s.planet.gravity_g = (float)in[11] / 65536.0f;
    msim_debris d = {.dos_bottom = in[9]};
    for (int i = 0; i < 3; i++) {
        d.p[i] = (float)in[i];
        d.dos_velocity[i] = in[3 + i];
        d.dos_spin[i] = in[6 + i];
    }
    out[6] = dos_debris_step(&s, &d, in[12], out + 7);
    out[10] = (int32_t)s.dos_random_index;
    for (int i = 0; i < 3; i++) {
        out[i] = (int32_t)d.p[i];
        out[3 + i] = d.dos_velocity[i];
    }
}

unsigned shim_dos_damage_rolls(unsigned index, int location, int damage, int armor)
{
    msim s = {0};
    dos_random_init(&s);
    s.dos_random_index = index;
    combat_unit u = {.dos_roll = ms_roll, .dos_roll_context = &s};
    combat_damage_live = 1;
    for (int i = 0; i < 8; i++) {
        u.armor[i] = (float)armor;
        u.internal[i] = 100;
    }
    combat_damage_at(&u, (float)damage, location, &s.rng);
    return s.dos_random_index;
}
