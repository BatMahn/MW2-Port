/* ai.c - see ai.h. */
#include "ai.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *NAMES[9] = {"DEFLT", "FDEFEND", "FDESTROY", "FLEAVE", "FRECON", "LDEFEND", "LDESTROY", "LLEAVE", "LRECON"};

const char *ai_state_name(int s)
{
    static const char *n[13] = {"idle", "avoid", "target", "attack", "flee", "follow", "recon", "patrol", "godirect", "?", "rest", "shutdown", "dead"};
    return (s >= 0 && s < 13) ? n[s] : "?";
}

int ai_load(ai_library *lib, prj_archive *a)
{
    int p, i, r;
    memset(lib, 0, sizeof *lib);
    for (p = 0; p < 9; p++) {
        prj_record rec;
        ai_program *g = &lib->progs[p];
        size_t o;
        snprintf(g->name, sizeof g->name, "%s", NAMES[p]);
        if (prj_read_named(a, "AIT", NAMES[p], &rec) != PRJ_OK) continue;
        if (rec.size >= 2) {
            const uint8_t *d = rec.data;
            int n = d[0] | (d[1] << 8), first = 0;
            if (n > 16) n = 16;
            g->state_count = n;
            for (i = 0; i < n && 2 + (size_t)i * 4 + 4 <= rec.size; i++) {
                g->states[i] = (int16_t)(d[2 + i * 4] | (d[3 + i * 4] << 8));
                g->rule_count[i] = (int16_t)(d[4 + i * 4] | (d[5 + i * 4] << 8));
                g->first_rule[i] = (int16_t)first;
                first += g->rule_count[i] > 0 ? g->rule_count[i] : 0;
            }
            o = 2 + (size_t)n * 4;
            for (r = 0; r < first && r < 48 && o + 12 <= rec.size; r++, o += 12) {
                ai_rule *u = &g->rules[r];
                u->cond = (uint16_t)(d[o] | (d[o + 1] << 8));
                u->cond_ref = (uint16_t)(d[o + 2] | (d[o + 3] << 8));
                u->range = (int16_t)(d[o + 4] | (d[o + 5] << 8));
                u->action = d[o + 6];
                u->next_state = d[o + 7];
                u->b8 = d[o + 8];
                u->ref = (uint16_t)(d[o + 9] | (d[o + 10] << 8));
            }
            g->total_rules = r;
        }
        prj_record_free(&rec);
        lib->count++;
    }
    return lib->count == 9 ? 0 : -1;
}

static unsigned ai_rand(ai_mind *m, unsigned n) { m->rng = m->rng * 1103515245u + 12345u; return n ? ((m->rng >> 16) & 0x7FFF) % n : 0; }

void ai_post(ai_mind *m, int cond) { if (m->event_cond == 0) m->event_cond = cond; }

void ai_init(ai_mind *m, int order_prog)
{
    memset(m, 0, sizeof *m);
    m->rng = 0x1234567u ^ (unsigned)order_prog;
    m->weave = 1;
    m->state = AI_IDLE;
    m->progs[0] = AIP_DEFLT;
    m->progs[1] = order_prog;
    m->progs[2] = -1;
    m->sweep_dir = 1;
    m->man = m->prev_man = m->forced_man = -1;
    m->flank_slot = -1;
    m->tgt = m->event_who = m->post_who = -2;
}

static float wrap180(float a)
{
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

/* the rule's reference (engine 0x10011d20 resolves cond_ref with 0x10013400; the condition iterates it with
 * 0x10013a50): 0x40 the current target (+0x14e), 0x2a the assigned goal (+0x14c), 0x2101 the assigned nav (+0x16a),
 * 0x2201 my leader, 0x4201 the enemies. Gives the distance and the reference's radius (0x10013450: a nav point's
 * radius, objects 100 m). Returns 0 when the reference is none / excluded (0x10013c40). */
static int ref_get(const ai_mind *m, const ai_view *v, int ref, float *d, float *radius, int *is_enemy)
{
    *radius = 10000.0f;
    *is_enemy = 0;
    switch (ref) {
    case 0x2a:
        if (!v->has_goal) return 0;
        *d = v->goal_dist; *radius = v->goal_radius > 100.0f ? v->goal_radius : 10000.0f;
        return 1;
    case 0x2101:
        if (!v->navref_ok) return 0;
        *d = v->navref_dist; *radius = v->navref_radius > 100.0f ? v->navref_radius : 10000.0f;
        return 1;
    case 0x2201:
        if (!v->leader_ok) return 0;
        *d = v->leader_dist;
        return 1;
    case 0x40:
        if (m->to_navref) return ref_get(m, v, 0x2101, d, radius, is_enemy);
        if (!m->target && (m->state == AI_GODIRECT || m->state == AI_RECON))
            return ref_get(m, v, 0x2a, d, radius, is_enemy);   /* the order's nav is the "target" there */
        if (!v->has_enemy) return 0;                            /* else the enemy engaged */
        *d = v->enemy_dist; *is_enemy = 1;
        return 1;
    default:
        if (!v->has_enemy) return 0;
        *d = v->enemy_dist; *is_enemy = 1;
        return 1;
    }
}

/* conditions (engine table 0x1024bc70): non-zero = true / a target. Ranges (GPS ranges at +0x15e/+0x162/+0x166,
 * x 100 at load, 250 m when 0): */
static int condition(const ai_mind *m, const ai_rule *r, const ai_view *v)
{
    float d = 0, rad = 10000.0f, R;
    int en = 0, ok = 1;
    if (r->cond >= 1 && r->cond <= 3) ok = ref_get(m, v, r->cond_ref, &d, &rad, &en);
    switch (r->cond) {
    case 1:   /* nearest within R (0x10012b40): -3 unlimited, else metres (a negative code never passes) */
        R = r->range == -3 ? 1e12f : (float)r->range * 100.0f;
        return ok && ((en && v->provoked) || d < R);
    case 2:   /* farthest beyond R (0x10012bf0): -4 / -2 stored ranges, 0 the reference's radius, -3 = 0 */
        R = r->range == -4 ? v->range_c : r->range == -2 ? v->range_b : r->range == 0 ? rad : r->range == -3 ? 0.0f : (float)r->range * 100.0f;
        return ok && d > R;
    case 3:   /* within (0x10012a80): range 0 = the reference's radius, else as condition 1 */
        R = r->range == 0 ? rad : r->range == -3 ? 1e12f : (float)r->range * 100.0f;
        return ok && ((en && v->provoked) || d < R);
    case 4: return m->target;                                                              /* keep current target */
    case 5: return 0;                                                                      /* event only */
    case 6: return m->target && !v->has_enemy;                                             /* target destroyed (flag 4, 0x10013c40) */
    case 7:   /* 0x10012fd0: none while commanded (+0x152 & 3); -2 / -1 stored ranges, 0 the radius; the pick
               * (0x10012de0, msim.c) counts only if the nearest enemy is in range (0x10012b40) */
        if (v->commanded || m->fled) return 0;
        R = r->range == -2 ? v->range_b : r->range == -1 ? v->range_a : r->range == 0 ? 10000.0f : r->range == -3 ? 1e12f : (float)r->range * 100.0f;
        return v->c7_ok && (v->provoked || v->c7_nearest < R);
    default: return 0;
    }
}

/* actions (engine table 0x1024bc90): non-zero = commit */
static int action(ai_mind *m, const ai_rule *r)
{
    switch (r->action) {
    case 0: return 1;
    case 1: m->counter = 0; return 1;
    case 2: m->prev_state = m->state; m->counter = 1; return 1;   /* 0x100132c0: push (state, target); depth 1 */
    case 3:   /* 0x10013320: pop - with nothing pushed the rule commits; else back to the pushed state, no commit */
        if (m->counter == 0) return 1;
        m->counter = 0;
        m->state = m->prev_state;
        m->target = m->state == AI_TARGET || m->state == AI_ATTACK;
        m->rule_fired = 1;
        return 0;
    case 4: m->post_cond = r->cond; return 0;   /* 0x100141e0: post (cond, +0xdc) to the leader - itself if it leads; no state change */
    default: return 0;
    }
}

/* engine table 0x1024cdf0 (mechs): rows (manoeuvre, n, next options) */
static const short MAN_TABLE[12][9] = {
    {0, 6, 0, 1, 2, 3, 4, 4, 0}, {1, 5, 0, 1, 2, 3, 4, 0, 0}, {2, 3, 7, 8, 4, 0, 0, 0, 0}, {3, 2, 7, 8, 0, 0, 0, 0, 0},
    {4, 3, 5, 5, 8, 0, 0, 0, 0}, {5, 2, 7, 8, 0, 0, 0, 0, 0}, {6, 1, 6, 0, 0, 0, 0, 0, 0}, {7, 3, 0, 1, 8, 0, 0, 0, 0},
    {8, 5, 0, 1, 2, 3, 4, 0, 0}, {9, 3, 0, 8, 7, 0, 0, 0, 0}, {10, 3, 7, 7, 4, 0, 0, 0, 0}, {11, 2, 3, 1, 0, 0, 0, 0, 0}};

/* 0x10020a30(n): jets with fuel and heat below n */
static int jets_ok(const ai_view *v, float n) { return v->can_jump && v->heat < n; }

/* the manoeuvre chooser (engine 0x1001f030). Returns -2 for "flee" (forced -2). */
static int choose_manoeuvre(ai_mind *m, const ai_view *v)
{
    int tries, level_lt4 = v->pilot_level < 4;   /* +0x19e bit 2 (0x1001ee30) */
    if (m->forced_man != -1) { int f = m->forced_man; m->forced_man = -1; return f; }
    if (v->contact) return 10;                                                    /* 0x10020e00 */
    /* jump pre-check: heat < 20, level < 4, the target above (0x10020090: > 8 m), reachable (0x100207b0) */
    if (jets_ok(v, 20.0f) && level_lt4 && v->enemy_height > v->height + 800.0f) {
        if (m->prev_man == 0) return 9;
        return ai_rand(m, 2) != 0 ? 8 : 0;
    }
    for (tries = 0; tries < 64; tries++) {
        int pick;
        if (m->prev_man < 0) pick = MAN_TABLE[ai_rand(m, 6)][0];                  /* 0x1001f0dd: rand(6) - rows 0-5 */
        else {
            int row = -1, r;
            for (r = 0; r < 12; r++) if (MAN_TABLE[r][0] == m->prev_man) row = r;
            if (row < 0) row = 0;
            pick = MAN_TABLE[row][2 + ai_rand(m, (unsigned)MAN_TABLE[row][1])];
        }
        if (m->prev_man == 4 && m->queue5) { m->queue5 = 0; return 5; }          /* +0x184 == 1: 4 then 5 */
        if (pick == 4) {
            if (!jets_ok(v, 20.0f) || !level_lt4) continue;                       /* re-roll the same row */
            if (v->twist_limit < 360.0f && fabsf(wrap180(v->enemy_bearing - v->heading)) > v->twist_limit) pick = 0;
        }
        if (pick == 3 && v->twist_limit > 0 && v->twist_limit < 10.0f) pick = 2;  /* engine override */
        if ((pick == 5 || pick == 9) && !v->can_jump) pick = 0;                   /* port: no jets / fuel */
        return pick;
    }
    return 0;
}

static void start_manoeuvre(ai_mind *m, const ai_view *v, int man)
{
    m->man = man;
    m->man_ticks = 0;
    m->man_phase = 0;
    m->last_dist = 0;                                                           /* +0x19a = 0 */
    m->turn_jet = 0;   /* manoeuvre change resets the jets (0x1001f500 -> 0x100201b0) */
    m->reverse = 0;
    m->jet_side = 0;
    m->man_limit = 9050.0f;                                                     /* 0x235a ticks (0x1001f290) */
    switch (man) {
    case 0: m->man_limit = (float)(ai_rand(m, 5) * 181u + 1448u); break;      /* rand(5) x 181 + 1448 */
    case 1: m->man_limit = 3620.0f; break;
    case 3: m->weave = ai_rand(m, 2) ? 1.0f : -1.0f; m->weave_ticks = 0; break;
    case 4: m->man_limit = 905.0f; m->throttle = 0; m->turn = 0; break;      /* 0x389 */
    case 7: {   /* 0x1001f290 case 7 -> 0x1001f630: a point 100 m from the target in a 16-way slot round its facing:
                 * 3 or 15 (rand(2) == 0) when 40 m or more away, 4 or 12 (rand(2) == 0) when closer */
        int slot = v->enemy_dist >= 4000.0f ? (ai_rand(m, 2) == 0 ? 15 : 3) : (ai_rand(m, 2) == 0 ? 12 : 4);
        float a = (v->enemy_heading + (float)slot * 22.5f) * 3.14159265f / 180.0f;
        m->flank_x = v->enemy_x + sinf(a) * 10000.0f;
        m->flank_z = v->enemy_z + cosf(a) * 10000.0f;
        break;
    }
    case 8: m->reverse = 1; m->man_limit = (float)(ai_rand(m, 11) * 181u + 1810u); break;   /* rand(11) x 181 + 0x712 */
    case 10: m->reverse = 1; m->man_limit = (m->prev_man == 10 && v->contact) ? 724.0f : 1448.0f; break;   /* 0x2d4 / 0x5a8 */
    case 11: m->man_limit = 362.0f; m->turn = 0; m->jet_side = -1; break;        /* 0x16a; lateral jets input +0x1e */
    case 12: m->man_limit = 18100.0f; break;                                    /* 0x46b4 */
    default: break;
    }
}

/* closing speed (0x100208f0): (previous distance - distance) / ticks, cm/tick; +0x19a = 0 at a start */
static float closing(ai_mind *m, float d, float ticks)
{
    float c = ticks > 0 ? (m->last_dist - d) / ticks : 0;
    m->last_dist = d;
    return c;
}

/* one attack manoeuvre step (engine 0x1001ea40 / 0x1001f950...): sets the throttle range R
 * (R < 0 = full throttle) and switches manoeuvres when one finishes */
static void attack_manoeuvre(ai_mind *m, const ai_view *v, float ticks, float *R, float *dist)
{
    int done = 0, jumping;
    float d = v->enemy_dist;
    m->jet = 0;
    m->jet_forward = 0;
    if (m->man < 0) {
        int c = choose_manoeuvre(m, v);
        if (c == -2) {   /* forced -2 (0x1001f030): flee the target, +0x152 = 1 (no further targeting) */
            m->fled = 1; m->state = AI_FLEE; m->target = 0; m->man = -1;
            *R = 1e9f;
            return;
        }
        start_manoeuvre(m, v, c);
    }
    m->man_ticks += ticks;
    switch (m->man) {
    case 0: *R = 15000.0f; if (d < 4500.0f) m->forced_man = 10; break;   /* 0x1001f950: under 45 m -> 10 */
    case 1: {   /* flank (0x1001f9d0): move to the slot point, re-pick every 543 ticks */
        m->flank_retarget -= ticks;
        if (m->flank_slot < 0 || m->flank_retarget <= 0) {
            float rel = wrap180(atan2f(v->self_x - v->enemy_x, v->self_z - v->enemy_z) * 180.0f / 3.14159265f - v->enemy_heading);
            int sector = ((int)lrintf(rel / 45.0f) + 8) % 8, slot, k2;
            if (m->flank_slot >= 0 && v->flank_slots) v->flank_slots[m->flank_slot]--;
            if (sector == 0) {                         /* in front: a side at random, the other if taken */
                slot = ai_rand(m, 2) ? 2 : 6;
                if (v->flank_slots && v->flank_slots[slot]) slot = slot == 2 ? 6 : 2;
            } else if (sector >= 2 && sector <= 4) {
                slot = 4;
                for (k2 = 2; k2 <= 4; k2++) if (!v->flank_slots || !v->flank_slots[k2]) { slot = k2; break; }
            } else if (sector >= 5 && sector <= 6) {
                slot = 4;
                for (k2 = 6; k2 >= 4; k2--) if (!v->flank_slots || !v->flank_slots[k2]) { slot = k2; break; }
            } else {
                slot = sector == 1 ? 3 : 5;            /* 1 -> 3, 7 -> 5 */
            }
            if (v->flank_slots) v->flank_slots[slot]++;
            m->flank_slot = slot;
            m->flank_retarget = 543.0f;
            {
                float a = (v->enemy_heading + (float)slot * 45.0f) * 3.14159265f / 180.0f;
                m->flank_x = v->enemy_x + sinf(a) * 15000.0f;
                m->flank_z = v->enemy_z + cosf(a) * 15000.0f;
            }
        }
        *R = 4000.0f;
        *dist = -1.0f;   /* tell the caller to steer to the flank point */
        break;
    }
    case 2: case 3: {   /* 0x1001faf0: done within max(1, closing cm/tick x 0.12987) x 80 m (0x100208f0) */
        float k = closing(m, d, ticks) * 0.12987013f;
        *R = 5500.0f;
        if (k < 1.0f) k = 1.0f;
        if (d <= k * 8000.0f) done = 1;
        if (m->man == 3) {
            m->weave_ticks += ticks;
            if (m->weave_ticks >= 543.0f) { m->weave_ticks = 0; m->weave = -m->weave; }
        }
        break;
    }
    case 4:   /* stand and jet straight up 20 m (0x1001fdd0) */
        if (m->man_ticks <= ticks) m->jump_from = v->height;
        *R = 1e9f; m->jet = 1;
        if (v->height >= m->jump_from + 2000.0f) { m->jet = 0; done = 1; }
        break;
    case 9: {   /* 0x1001fcc0 */
        int above = v->enemy_height > v->height + 800.0f;
        *R = 10000.0f;
        if (m->man_phase == 0) {
            if (v->can_jump && v->heat < 40.0f && above) { m->jet = 1; break; }          /* 0x10020a30(40) */
            m->man_phase = 1; m->man_ticks = 0;
            if (v->can_jump && v->heat < 20.0f && ai_rand(m, 4) == 0) m->forced_man = 5;  /* 0x10020a30(20) */
        } else if (m->man_phase == 1) {
            if (m->man_ticks >= 181.0f) m->man_phase = 2;
        } else if (v->height <= 0) {
            done = 1;
        }
        break;
    }
    case 5: {   /* 0x1001fe30 */
        *R = -1.0f;
        if (m->man_phase >= 2) { if (v->height <= 0) done = 1; break; }
        if (d < 1000.0f) {
            if (m->man_phase == 1) m->man_phase = 2;       /* cut the jets inside 10 m */
        } else {
            float off = fabsf(wrap180(v->enemy_bearing - v->heading)), lim = v->twist_limit > 0 ? v->twist_limit : 90.0f;
            int within = off < lim && off < lim * 3.000000f;
            float cl = (m->last_dist > 0 && ticks > 0) ? (m->last_dist - d) / ticks : 0;
            float thresh = d <= 6000.0f ? 9.0f : 36.0f;
            if (!v->can_jump && v->height <= 0) { done = 1; break; }
            m->jet = 1;
            m->jet_forward = cl > thresh ? within : !within;   /* 0x1001fe30 */
            m->man_phase = 1;
        }
        if (m->man_phase == 1 && v->height <= 0 && m->man_ticks > 181.0f) done = 1;
        m->last_dist = d;
        break;
    }
    case 6: *R = -1.0f; if (d <= 2000.0f) done = 1; break;   /* 0x1001fb90: charge, done within 20 m (input +0x43 not modelled) */
    case 7: {   /* 0x1001fc40: to the slot point (R 30 m), done within 30 m of it */
        float fx = m->flank_x - v->self_x, fz = m->flank_z - v->self_z;
        *R = 3000.0f;
        *dist = -1.0f;
        if (sqrtf(fx * fx + fz * fz) <= 3000.0f) done = 1;
        break;
    }
    case 8:  *R = -1.0f; if (v->contact) done = 1; break;   /* 0x1001fc00: full reverse, ends on contact (+0xa4) */
    case 10: *R = -1.0f; break;                              /* 0x1001ff70: full reverse until the time limit */
    case 11: *R = 0; m->jet = 1; break;                      /* 0x1001fe10: sidestep on the jets, 362 ticks */
    case 12: *R = 3000.0f; break;
    default: done = 1;
    }
    if (*dist >= 0) *dist = d;
    /* out of weapons (0x1001ea40 -> 0x10020880; the controller +0x88 test not modelled): unless jumping or in 6 */
    jumping = m->man == 4 || m->man == 5 || m->man == 9 || m->man == 11;   /* +0x196 */
    if (v->out_of_weapons && !jumping && m->man != 6 && m->forced_man == -1) {
        if (ai_rand(m, 4) != 0 && m->prev_man != 5 && jets_ok(v, 40.0f)) { m->forced_man = 4; m->queue5 = 1; }   /* +0x184 = 1 */
        else {
            unsigned r2 = ai_rand(m, 2);
            m->forced_man = r2 == 0 ? -2 : 6;
            if (v->pilot_level >= 2) m->forced_man = -2;   /* +0x19e bit 0x20: level 1 mechs only charge */
        }
    }
    if (v->contact && !m->reverse && !jumping && m->man != 10) done = 1;   /* 0x10020e00 */
    if (done || m->man_ticks >= m->man_limit || m->forced_man != -1) {
        if (m->man == 1 && m->flank_slot >= 0 && v->flank_slots) { v->flank_slots[m->flank_slot]--; m->flank_slot = -1; }
        m->prev_man = m->man;
        m->man = -1;
        m->reverse = 0;   /* 0x1001f500: 8 / 10 clear +0x2f */
        m->jet_side = 0;
        m->man_phase = 0;
    }
}

static int program_allows(const ai_library *lib, const ai_mind *m, int state)
{
    int s, i;
    for (s = 0; s < 3; s++) {
        const ai_program *g;
        if (m->progs[s] < 0) continue;
        g = &lib->progs[m->progs[s]];
        for (i = 0; i < g->state_count; i++) if (g->states[i] == state || g->states[i] == -1) return 1;
    }
    return 0;
}

int ai_allows(const ai_library *lib, const ai_mind *m, int state) { return program_allows(lib, m, state); }

void ai_missile_warning(ai_mind *m, const ai_view *v, int shooter_level, int (*side_free)(void *ctx, int side), void *ctx)
{
    /* 0x10020ac0: shooter +0x19e bit 4 (level < 3) and rand(3) != 0; then the target: jets and heat < 65
     * (0x10020a30(0x41)), rand(3) != 0 -> try a 50 m sidestep, one side at random then the other (0x100204e0):
     * clear -> manoeuvre 11; otherwise (or on the rand) a level < 4 pilot jumps (4) */
    int side, k;
    if (shooter_level >= 3 || ai_rand(m, 3) == 0) return;
    if (m->forced_man == 4 || m->state != AI_ATTACK) return;   /* +0x174 already 4; manoeuvres run in attack */
    if (!jets_ok(v, 65.0f)) return;
    if (ai_rand(m, 3) != 0) {
        side = ai_rand(m, 2) == 0 ? 1 : -1;
        for (k = 0; k < 2; k++, side = -side)
            if (!side_free || side_free(ctx, side)) { m->forced_man = 11; return; }
    }
    if (m->forced_man == -1 && v->pilot_level < 4) m->forced_man = 4;
}

void ai_step(const ai_library *lib, ai_mind *m, const ai_view *v, float dt)
{
    int s, i, k, fired = 0;
    float ticks = dt * 182.0f;
    m->c7_fired = 0;
    /* rules for the current state, order program first (engine collects slot 2, 1, 0); the first rule whose action
     * commits ends the pass (0x10011d20 returns 1 -> no movement this tick, 0x10011c50) */
    for (s = 2; s >= 0 && !fired; s--) {
        const ai_program *g;
        if (m->progs[s] < 0) continue;
        g = &lib->progs[m->progs[s]];
        for (i = 0; i < g->state_count && !fired; i++) {
            if (g->states[i] != m->state) continue;
            for (k = 0; k < g->rule_count[i] && !fired; k++) {
                const ai_rule *r = &g->rules[g->first_rule[i] + k];
                if (r->cond == 0) continue;
                if (m->event_cond == r->cond) m->event_cond = 0;           /* posted event: bypass the condition */
                else if (!condition(m, r, v)) continue;
                m->rule_fired = 0;
                if (!action(m, r)) { if (m->rule_fired) fired = 1; continue; }   /* a pop changed the state */
                fired = 1;
                if (r->cond == 3 && m->to_navref) m->nav_ok = 0;            /* 0x10014f60: the nav reached is released */
                if (r->cond == 7) m->c7_fired = 1;                          /* ref2 0x25: the condition's pick */
                if (r->next_state != m->state && program_allows(lib, m, r->next_state)) {
                    int from = m->state;
                    m->state = r->next_state;
                    if (m->state == AI_TARGET || m->state == AI_ATTACK) m->target = 1;
                    else if (m->state != AI_GODIRECT) m->target = 0;
                    if (m->state == AI_GODIRECT && r->cond_ref == 0x2101 && r->ref == 0x25) { m->to_navref = 1; m->target = 0; }
                    if (from == AI_TARGET && m->state == AI_GODIRECT) m->target = m->to_navref ? 0 : m->target;
                    if (m->state == AI_TARGET) {   /* entry 0x10013790 case 2: next fire decision rand(10) x 22 ticks on; leaving
                                                    * patrol (7) with no nav assigned, a 30 m nav point here (0x1001cab0) */
                        m->next_fire = (float)(ai_rand(m, 10) * 22u);
                        if (from == AI_PATROL && v->unit_led && !m->nav_ok) { m->nav_ok = 1; m->nav_x = v->self_x; m->nav_z = v->self_z; }
                    }
                }
            }
        }
    }
    m->rule_fired = fired;
    if (m->state != AI_GODIRECT) m->to_navref = 0;
    if (!v->has_enemy && (m->state == AI_TARGET || m->state == AI_ATTACK) && !fired) { m->state = m->prev_state; m->target = 0; }
    if (m->state != AI_ATTACK && m->man >= 0) { m->prev_man = m->man; m->man = -1; m->reverse = 0; m->jet_side = 0; }
    if (m->state != AI_ATTACK) m->jet = 0;
    m->wants_fire = 0;
    m->next_fire -= ticks;
    if (fired) return;   /* inputs hold (0x10011c50: the state's movement runs only when no rule fired) */

    /* movement per state (engine 0x100130f0) */
    {
        float want = v->heading, dist = 0, R = 10000.0f, thr = 0, err;
        int moving = 1, full_attack = 0, engaged;
        switch (m->state) {
        case AI_TARGET:
            if (!v->has_enemy) { moving = 0; break; }
            want = v->enemy_bearing; dist = v->enemy_dist; R = 10000.0f;
            break;
        case AI_ATTACK:
            if (!v->has_enemy) { moving = 0; break; }
            want = v->enemy_bearing; dist = v->enemy_dist;
            attack_manoeuvre(m, v, ticks, &R, &dist);
            if (m->state != AI_ATTACK) { moving = 0; break; }   /* fled (forced -2) */
            /* 0x10020950: skilled pilots jump to turn when the target is outside the torso arc */
            if (v->pilot_level < 4 && m->man != 10 && m->man != 4 && m->man != 5 && m->man != 9 && m->man != 11) {
                /* every tick: the target outside the torso arc -> jets on if heat < 20 with fuel (0x10020a30(0x14));
                 * back inside -> off (the jump manoeuvres excepted, +0x196). Fuel is not re-checked while on. */
                float off = fabsf(wrap180(v->enemy_bearing - v->heading));
                float lim = v->twist_limit > 0 ? v->twist_limit : 90.0f;
                if (lim < 360.0f && off >= lim) { if (!m->turn_jet && jets_ok(v, 20.0f)) m->turn_jet = 1; }
                else m->turn_jet = 0;
                if (m->turn_jet) m->jet = 1;
            }
            if (dist < 0) {   /* manoeuvres 1 / 7: steer to the slot point */
                float fx = m->flank_x - v->self_x, fz = m->flank_z - v->self_z;
                dist = sqrtf(fx * fx + fz * fz);
                want = atan2f(fx, fz) * 180.0f / 3.14159265f;
            }
            if (R < 0) { R = 10000.0f; full_attack = 1; }
            break;
        case AI_FLEE:
            want = v->has_enemy ? v->enemy_bearing + 180.0f : v->heading; dist = 1e9f; break;
        case AI_GODIRECT:
            if (m->to_navref && v->navref_ok) { want = v->navref_bearing; dist = v->navref_dist; R = v->navref_radius > 100 ? v->navref_radius : 10000.0f; break; }
            if (m->target && v->has_enemy) { want = v->enemy_bearing; dist = v->enemy_dist; R = 10000.0f; break; }
            if (!v->has_goal) { moving = 0; break; }
            want = v->goal_bearing; dist = v->goal_dist; R = v->goal_radius > 100 ? v->goal_radius : 10000.0f; break;
        case AI_FOLLOW: case AI_RECON: case AI_PATROL:
            if (!v->has_goal) { moving = 0; break; }
            want = v->goal_bearing; dist = v->goal_dist; R = v->goal_radius > 100 ? v->goal_radius : 10000.0f; break;
        default:
            moving = 0;
        }
        /* the torso: aimed at the target while engaged (0x100231a0 writes input +4 = the bearing off the legs,
         * +0 = -elevation, 0x100135a0 / 0x100135d0: clamped to +-180 / +-60); patrol sweep otherwise (0x10014ff0) */
        engaged = (m->state == AI_TARGET || m->state == AI_ATTACK || (m->state == AI_GODIRECT && m->target)) && v->has_enemy;
        if (engaged) {
            float aim = wrap180(v->enemy_bearing - v->heading);
            m->twist_demand = aim;
            m->pitch_demand = v->enemy_elev > 60.0f ? 60.0f : v->enemy_elev < -60.0f ? -60.0f : v->enemy_elev;
            /* fire decision (0x100231a0): when +0x15a is due, r = rand(+0x158) sets the next decision r x 22 ticks on;
             * fires only with the LAGGED twist within 10 deg of the bearing, r == 0, a target other than the player
             * passing rand(3) == 0 too. (+0x158 = max(GPS skill, 1) + 1, 0x10012000.) No range test here: 0x100232b0
             * (combat_ai_fire) checks each weapon's window. */
            if (m->next_fire <= 0) {
                unsigned r = ai_rand(m, (unsigned)(v->skill > 0 ? v->skill : 1) + 1u);
                m->next_fire = (float)(r * 22u);
                if (fabsf(aim - m->twist) < 10.0f && (v->enemy_is_player || ai_rand(m, 3) == 0) && r == 0) m->wants_fire = 1;
            }
        } else {
            m->twist_demand = (m->state >= AI_FOLLOW && m->state <= AI_GODIRECT) ? m->torso : 0;
            m->pitch_demand = 0;
        }
        if (!moving) { m->throttle = 0; m->turn = 0; m->reverse = 0; return; }
        /* steering 0x10013600 */
        err = wrap180(want - v->heading);
        if (err > 45.0f) m->turn = 819.2f;
        else if (err < -45.0f) m->turn = -819.2f;
        else m->turn = err * 18.2044f;
        if (m->state == AI_ATTACK && m->man == 3) m->turn += 450.0f * m->weave;
        if (m->state == AI_ATTACK && m->man == 11) m->turn = 0;            /* 0x1001f290 case 11: +0xc = 0 */
        /* throttle 0x100134c0 (fractions of full = 1/64) */
        if (dist > 2.0f * R) thr = 1.0f;
        else if (dist >= R) thr = 0.8f;
        else thr = 0;
        if (full_attack) thr = 1.0f;
        if (m->state == AI_ATTACK && (m->man == 11 || m->man == 4)) thr = m->throttle;   /* hold the throttle */
        if (thr > 0 && fabsf(m->turn) > 45.0f) {
            thr = (0.015625f - fabsf(m->turn) * 1.01725e-05f) / 0.015625f;
            if (thr < 0.1f) thr = 0.1f;
        }
        if (v->heat > 65.0f) thr *= 0.5f;
        if (thr < 0.1f) thr = 0;
        /* 0x1001f910 (manoeuvres 0 / 1): no throttle but more than 5 deg off -> 1/640 = 10% of full to step round */
        if (m->state == AI_ATTACK && (m->man == 0 || m->man == 1) && thr <= 0 && fabsf(err) > 5.0f) thr = 0.1f;
        if (m->state == AI_FLEE) thr = 1.0f;
        m->throttle = thr;
        /* patrol / scouting torso sweep (0x10014ff0) */
        if (m->state >= AI_FOLLOW && m->state <= AI_GODIRECT) {
            m->sweep_ticks += ticks;
            while (m->sweep_ticks >= (thr > 0 ? 32.0f : 64.0f)) {
                m->sweep_ticks -= (thr > 0 ? 32.0f : 64.0f);
                m->torso += 5.0f * (float)m->sweep_dir;
                if (fabsf(m->torso) > 45.0f) m->sweep_dir = -m->sweep_dir;
            }
        } else {
            m->torso = 0;
        }
    }
}
