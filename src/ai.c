/* ai.c - see ai.h. */
#include "ai.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

int ai_dos_int = 0;

/* 3Dfx 0x10013600: fmod(err, 360); > 45 -> 819.2, < -45 -> -819.2, else err x 18.2044 (0x102472a4). DOS 0x229e0 works on
 * the 16.16 error in WHOLE degrees: d = (err >> 16) % 360 (sar: floored), > 45 / < -45 the same caps, else
 * ((d << 16) / 45) x 0x3333333 >> 16 rounded (shrd + adc) - an error in [0, 1) degree steers nothing and one in [-1, 0)
 * a full degree's worth, so the DOS autopilot settles 0-1 degree right of the nav's bearing and walks a line beside the
 * 3Dfx one (DOSBox TNJ4 Alpha -> Beta: ~7 m west of the straight line from 470 m in; the 3D look's float steering ~1 m) */
float ai_nav_turn(float err)
{
    if (ai_dos_int) {
        int d = (int)floorf(err), q;
        long long p;
        if (d > 0x2d) return 819.2f;
        if (d < -0x2d) return -819.2f;
        q = (d * 65536) / 0x2d;
        p = (long long)q * 0x3333333;
        return (float)((int)(p >> 16) + (int)((p >> 15) & 1)) / 65536.0f;
    }
    if (err > 45.0f) return 819.2f;
    if (err < -45.0f) return -819.2f;
    return err * 18.2044f;
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
        m->entered = 1; m->entered_from = m->state;   /* 0x10013320 -> 0x10013e00(pushed state, pushed target) */
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

/* DOS MW2.EXE 0x2c778..0x2c861 (the records 0xfdf10 per class, built once): class 1 (mechs) the mech table 0x963f4 (13 rows,
 * the first pick rand(13 - 7)); class 5 (aircraft) one row {12} (0x964f0: manoeuvre 12 only, circling the target); class 8
 * (dropships) {1} (0x96502: the flank slots); every other class (hovercraft, tanks...) {0} (0x964de: the straight attack);
 * and a unit whose controller +0xe4 (the MEK tonnage) is 1 - the Elementals - the jumper table 0x96514 (0x2c8ab). The 3D
 * editions' port gave every unit the mech table */
static const short JUMPER_TABLE[12][9] = {
    {0, 7, 0, 1, 2, 3, 4, 4, 4}, {1, 6, 0, 1, 2, 3, 4, 4, 0}, {2, 4, 7, 8, 4, 4, 0, 0, 0}, {3, 4, 7, 8, 4, 4, 0, 0, 0},
    {4, 3, 5, 5, 8, 0, 0, 0, 0}, {5, 2, 7, 8, 0, 0, 0, 0, 0}, {6, 1, 6, 0, 0, 0, 0, 0, 0}, {7, 5, 0, 1, 2, 8, 4, 0, 0},
    {8, 6, 0, 1, 2, 3, 4, 4, 0}, {9, 3, 0, 8, 7, 0, 0, 0, 0}, {10, 4, 7, 7, 4, 4, 0, 0, 0}, {11, 1, 3, 0, 0, 0, 0, 0, 0}};
void ai_set_class(ai_mind *m, int unit_class, int tons) { m->cls = unit_class; m->tons1 = tons == 1; }

/* the unit's own nav points in manoeuvre 12 (DOS 0x23420: four round the target; 0x233b0 drops every other one as its
 * deletions shift the table, as msim's guard_drop) - kept in ai_mind.guard */
static void own_drop(ai_mind *m)
{
    int r, w = 0;
    for (r = 1; r < m->guard_n; r += 2, w++) { m->guard[w][0] = m->guard[r][0]; m->guard[w][1] = m->guard[r][1]; }
    m->guard_n = w;
    m->goal_pt /= 2;
}
static void own_goal(ai_mind *m, int i)
{
    if (m->guard_n <= 0) return;
    m->goal_pt = (i + 1) % m->guard_n;
    if (m->goal_pt < 0) m->goal_pt = 0;
    m->flank_x = m->guard[m->goal_pt][0];
    m->flank_z = m->guard[m->goal_pt][1];
}
static void own_add(ai_mind *m, float x, float z)   /* DOS 0x29d40-class nav add (refused past the table's room) */
{
    if (m->guard_n >= 16) return;
    m->guard[m->guard_n][0] = x; m->guard[m->guard_n][1] = z;
    m->guard_n++;
}
/* DOS MW2.EXE 0x2d070 (from 0x2d010): the point d cm out in slot k (16-way) of a unit: (d / 0x96374[k], d / 0x96378[k])
 * (0 for a 0 divisor; d a 16-bit word, idiv) in the unit's own frame (heading h) - on a square, not a circle */
static void slot_point(float cx, float cz, float h, int k, float d, float *x, float *z)
{
    static const int DX[16] = {0, 2, 1, 1, 1, 1, 1, 2, 0, -2, -1, -1, -1, -1, -1, -2};
    static const int DZ[16] = {1, 1, 1, 2, 0, -2, -1, -1, -1, -1, -1, -2, 0, 2, 1, 1};
    float a = h * 3.14159265f / 180.0f, lx, lz;
    int di = (int)d;
    lx = DX[k & 15] ? (float)(di / DX[k & 15]) : 0.0f;
    lz = DZ[k & 15] ? (float)(di / DZ[k & 15]) : 0.0f;
    *x = cx + cosf(a) * lx + sinf(a) * lz;
    *z = cz - sinf(a) * lx + cosf(a) * lz;
}
/* DOS MW2.EXE 0x2e6f0: manoeuvre 1's slot (8-way; x 2 the 16-way +0x17a) from the unit's bearing off the target's facing:
 * the sector rel / 45 in 16.16 + 0x8000 >> 16 (half up; 8 -> 0); in front (0) rand(2) != 0 -> 2, else 6, the other when
 * taken in the ATTACKER's own counters (+0x19c); 1 -> 3, 7 -> 5; 2-4 the first free of 2, 3, 4 (else 4), 5-6 of 6, 5,
 * 4 (else 4) in the target's counters. The port had read the front case from the target's counters and rounded half
 * to even (the GBL port's emulated comparison found it; MW2.EXE the same code) */
static int flank_pick(ai_mind *m, float rel, const int *own, const int *tgt)
{
    int sector = (int)floorf(fmodf(rel + 360.0f, 360.0f) / 45.0f + 0.5f), slot, k2;
    if (sector > 7) sector = 0;
    if (sector == 0) {
        slot = ai_rand(m, 2) != 0 ? 2 : 6;
        if (own && own[slot]) slot = slot == 2 ? 6 : 2;
    } else if (sector == 1 || sector == 7) slot = sector == 1 ? 3 : 5;
    else if (sector <= 4) {
        slot = 4;
        for (k2 = 2; k2 <= 4; k2++) if (!tgt || !tgt[k2]) { slot = k2; break; }
    } else {
        slot = 4;
        for (k2 = 6; k2 >= 4; k2--) if (!tgt || !tgt[k2]) { slot = k2; break; }
    }
    return slot;
}
/* DOS MW2.EXE 0x24710 (states 5-8, a class 1 unit only - moving or not, the throttle input of the last tick): every 0x40
 * ticks (0x20 with the throttle on) the torso +-5 degrees the way +0x17a says, turning round past 45; the due time is
 * first pulled in to now + the interval, and after a step it is now + the interval (no catching up). The port had
 * accumulated the ticks (several steps in a long frame, none pulled in) and swept vehicles too */
static void patrol_sweep_dos(ai_mind *m, float thr, float ticks)
{
    float iv = thr != 0.0f ? 32.0f : 64.0f;
    m->sweep_ticks -= ticks;
    if (m->sweep_ticks > iv) m->sweep_ticks = iv;
    if (m->sweep_ticks <= 0.0f) {
        m->torso += 5.0f * (float)m->sweep_dir;
        if (fabsf(m->torso) > 45.0f) m->sweep_dir = -m->sweep_dir;
        m->sweep_ticks = iv;
    }
}
static void own_square(ai_mind *m, const ai_view *v)   /* 0x23420: x - r, z - r, x + r, z + r (r: a unit 100 m) */
{
    static const float D[4][2] = {{-10000.0f, 0}, {0, -10000.0f}, {10000.0f, 0}, {0, 10000.0f}};
    int k;
    for (k = 0; k < 4 && m->guard_n < 16; k++) { m->guard[m->guard_n][0] = v->enemy_x + D[k][0]; m->guard[m->guard_n][1] = v->enemy_z + D[k][1]; m->guard_n++; }
    own_goal(m, -1);
}

/* 0x10020a30(n): jets with fuel and heat below n */
/* DOS MW2.EXE 0x2e930..0x2e971: the torso inputs the fire decision writes - the bearing off the legs in 1024ths of
 * 180 degrees (0x22970: / 0x2d00, idiv's chop) and the elevation in 1024ths of 60 (0x229a0: / 0xf00), each through
 * 0x23a40 (+-0x400; its lead term 2 x the decision's rand is always 0 there: the decision fires only on rand 0), back to
 * 16.16 (x 0x2d00 / x 0xf00) */
static float dos_twist_q(float deg)
{
    int q = (int)(deg * 65536.0f) / 0x2d00;
    if (q > 0x400) q = 0x400; else if (q < -0x400) q = -0x400;
    return (float)(q * 0x2d00) / 65536.0f;
}
static float dos_pitch_q(float deg)
{
    int q = (int)(deg * 65536.0f) / 0xf00;
    if (q > 0x400) q = 0x400; else if (q < -0x400) q = -0x400;
    return (float)(q * 0xf00) / 65536.0f;
}

static int jets_ok(const ai_view *v, float n) { return v->can_jump && v->heat < n; }

/* the manoeuvre chooser (engine 0x1001f030). Returns -2 for "flee" (forced -2). */
static int choose_manoeuvre(ai_mind *m, const ai_view *v)
{
    int tries, level_lt4 = v->pilot_level < 4;   /* +0x19e bit 2 (0x1001ee30) */
    if (m->forced_man != -1) { int f = m->forced_man; m->forced_man = -1; return f; }
    if (ai_dos_int) {
        /* DOS MW2.EXE 0x2c880: class 1 only - in contact (+0xa4), unless the reverse input +0x2f is on, +0x190 is set (the last
         * manoeuvre started was a jump: 4, 5, 9 or 11) or the manoeuvre is 10 -> 10; a target that is not a unit -> 0; the
         * jump pre-check. Then the class's record: the first pick rand(rows - first) (rows 0-5 of a mech's), later a row
         * matching the last manoeuvre (the first match) picks among its options; 4 after 4 with +0x17e -> 5; 4 needs jets
         * (heat < 20) and pilot level <= 3, else a re-roll, and 0 when the target is beyond the twist limit to the plus side; 3 -> 2 with a
         * twist limit under 10; 5 on the ground -> 0 */
        const short (*T)[9] = MAN_TABLE;
        int rows = 12, first = 6, solo = -1;
        if (m->tons1) { T = JUMPER_TABLE; first = 6; }
        else if (m->cls == 5) solo = 12;
        else if (m->cls == 8) solo = 1;
        else if (m->cls != 1) solo = 0;
        if (m->cls == 1) {
            if (v->contact && !m->reverse && !m->jump_man && m->man != 10) return 10;
            if (v->target_not_unit) return 0;
            if (jets_ok(v, 20.0f) && level_lt4 && v->jump_view) {   /* 0x2da50 + 0x2e150 (the view test, msim) */
                if (m->prev_man == 0) return 9;
                return ai_rand(m, 2) != 0 ? 8 : 0;
            }
        }
        if (solo >= 0) { (void)ai_rand(m, 1); return m->prev_man == 4 && m->queue5 ? 5 : solo; }   /* the one-row record: still rand(1) (0x2ca24 / 0x2caa8) */
        for (tries = 0; tries < 64; tries++) {
            int pick;
            if (m->prev_man < 0) pick = T[ai_rand(m, (unsigned)first)][0];
            else {
                int row = -1, r;
                for (r = 0; r < rows; r++) if (T[r][0] == m->prev_man) { row = r; break; }
                if (row < 0) row = 0;
                pick = T[row][2 + ai_rand(m, (unsigned)T[row][1])];
            }
            if (m->prev_man == 4 && m->queue5) return 5;   /* +0x17e - which every start clears (see start_manoeuvre) */
            if (pick == 4) {
                if (!jets_ok(v, 20.0f) || !level_lt4) continue;
                /* 0x2cb25: 0x230d0 is the SIGNED bearing off the heading (+0xd4 - +0x60, within +-180) against the twist
                 * limit - a target as far off the other side keeps the jump */
                if (wrap180(v->enemy_bearing - v->heading) > v->twist_limit) pick = 0;
            }
            if (pick == 3 && v->twist_limit < 10.0f) pick = 2;
            if (pick == 5 && v->grounded) pick = 0;   /* +0x78 */
            return pick;
        }
        return 0;
    }
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
    if (getenv("MW2_AI_MAN_TRACE")) fprintf(stderr, "aiman %p %d\n", (void *)m, man);   /* TEST ONLY: each manoeuvre start */
    m->man = man;
    m->man_ticks = 0;
    m->man_phase = 0;
    m->last_dist = 0;                                                           /* +0x19a = 0 */
    m->turn_jet = 0;   /* manoeuvre change resets the jets (0x1001f500 -> 0x100201b0) */
    m->reverse = 0;
    m->jet_side = 0;
    m->jet_back = 0;
    m->man_limit = 9050.0f;                                                     /* 0x235a ticks (0x1001f290) */
    m->jump_man = man == 4 || man == 5 || man == 9 || man == 11;   /* DOS +0x190 (0x2cc43 cleared, the jump cases set it) */
    if (ai_dos_int) {
        /* DOS MW2.EXE 0x2cc28..0x2ce87 (the jump table 0x2cbb8): the same cases at its 182 Hz clock - the 3Dfx DLL's tick
         * constants were scaled to its 181 Hz: now + 0x238c; 0 (rand(5) + 8) x 0xb6; 1 0xe38; 4 0x38e; 8 (rand(11) + 10) x
         * 0xb6 reverse; 10 in contact after a 10 with a non-unit (+0x7c == -1) 0x2d8 and NOT reversing, else reverse 0x5b0;
         * 11 0x16c, the left jet (+0x1e) when +0x17a is 0 or 2, else the forward jet (+0x20); 12 0x4718. (The GBL port's
         * emulated comparison, GBL 0x2ced0; MW2.EXE the same) */
        m->man_limit = 9100.0f;
        /* 0x2cc00: +0x17e = 0, then +0x17a = +0x17e - the 4-then-5 flag the out-of-weapons step set with its forced 4 is
         * gone by the time the 4 ends, and the dodge direction the missile warning left in +0x17a is lost before case 11
         * reads it (so its 5 never follows, and 11 always fires the left jet) */
        m->queue5 = 0;
        switch (man) {
        case 0: m->man_limit = (float)((ai_rand(m, 5) + 8u) * 182u); return;
        case 1: m->man_limit = 3640.0f; m->flank_face = 0; return;
        case 4: m->man_limit = 910.0f; m->throttle = 0; m->turn = 0; return;
        case 8: m->reverse = 1; m->man_limit = (float)((ai_rand(m, 11) + 10u) * 182u); return;
        case 10:
            if (m->prev_man == 10 && v->contact && !v->contact_unit) m->man_limit = 728.0f;
            else { m->reverse = 1; m->man_limit = 1456.0f; }
            return;
        case 11:
            m->man_limit = 364.0f; m->turn = 0;
            m->jet_side = -1;   /* +0x17a is 0 here (above): the left jet +0x1e, whatever the warning's quadrant */
            return;
        case 7: {   /* 0x2cdb3: slot 3 or 15 (rand(2) == 0) 40 m or more from the target (+0xc0 >= 0xfa0), 4 or 12 closer,
                     * through 0x2d010 with the unit's OWN code (+4 | 0x200) and 0x2710: a point 100 m out round the unit
                     * itself in its own frame, made an own point; the goal the first own point. The 3Dfx DLL's (kept for
                     * the 3D looks) is a point on a 100 m circle round the TARGET */
            int slot = v->enemy_dist >= 4000.0f ? (ai_rand(m, 2) == 0 ? 15 : 3) : (ai_rand(m, 2) == 0 ? 12 : 4);
            float px, pz;
            slot_point(v->self_x, v->self_z, v->heading, slot, 10000.0f, &px, &pz);
            own_add(m, px, pz);
            own_goal(m, -1);
            return;
        }
        case 12: m->man_limit = 18200.0f; m->flank_retarget = 0; own_square(m, v); return;   /* 0x2ce7d -> 0x23420 (the points made before stay) */
        default: break;   /* 3, 5, 7, 9 as below */
        }
    }
    switch (man) {
    case 0: m->man_limit = (float)(ai_rand(m, 5) * 181u + 1448u); break;      /* rand(5) x 181 + 1448 */
    case 1: m->man_limit = 3620.0f; m->flank_face = 0; break;
    case 3: m->weave = ai_rand(m, 2) ? 1.0f : -1.0f; m->weave_ticks = 0; break;
    case 4: m->man_limit = 905.0f; m->throttle = 0; m->turn = 0; break;      /* 0x389 */
    case 5: m->last_dist = v->enemy_dist; break;                                /* 0x1001f290 case 5 -> 0x100208f0: +0x19a = distance */
    case 9: m->turn = 0; break;                                                 /* input +0xc = 0 */
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
    case 11: m->man_limit = 362.0f; m->turn = 0; m->jet_side = -1; break;        /* 0x16a; input +0x1e = 1 always: the LEFT
                                                                                     * jet (0x10019310 tests +0x1e first;
                                                                                     * the AI display 0x10012150 shows '<') */
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

/* a manoeuvre ends (0x1001f500; DOS 0x2ced0, also from the attack state's exit 0x22b05): 1 drops its own points
 * (DOS 0x233b0) and frees its slot; 7 and 12 drop their own points (DOS); the reverse / jets inputs off */
static void end_manoeuvre(ai_mind *m, const ai_view *v)
{
    if (ai_dos_int && (m->man == 1 || m->man == 7 || m->man == 12)) own_drop(m);
    if (m->man == 1 && m->flank_slot >= 0 && v->flank_slots) v->flank_slots[m->flank_slot]--;
    m->flank_slot = -1;
    m->prev_man = m->man;
    m->man = -1;
    m->reverse = 0;   /* 0x1001f500: 8 / 10 clear +0x2f */
    m->jet_side = 0;
    m->jet = 0; m->jet_forward = 0; m->jet_back = 0;   /* 0x1001f500: the jets off (0x100201b0(0)), 5 / 11 +0x20 / +0x21 = 0 */
    m->man_phase = 0;
}

/* one attack manoeuvre step (engine 0x1001ea40 / 0x1001f950...): sets the throttle range R
 * (R < 0 = full throttle) and switches manoeuvres when one finishes */
static void attack_manoeuvre(ai_mind *m, const ai_view *v, float ticks, float *R, float *dist)
{
    int done = 0, jumping, was_jet = m->jet;
    float d = v->enemy_dist;
    m->jet = 0;
    m->jet_forward = 0;
    m->jet_back = 0;
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
            /* 0x1001ea8e: every 0x21f ticks the slot's point is placed again round the target as it stands now
             * (0x1001f630 -> 0x1001f6b0); the slot itself is chosen once, when the manoeuvre starts (+0x184 = -1 ->
             * 0x10020d00), and kept */
            if (ai_dos_int) {
                /* DOS MW2.EXE 0x2c265 (case 1, every 0x222 ticks: +0x176): the unit's own points dropped (0x233b0), the old
                 * slot's counter on the target down, the slot picked once (+0x17e == -1: 0x2e6f0), its point 150 m out round
                 * the target (0x2d010, 0x3a98) made an own point and the goal the FIRST own point - one left over from
                 * patrol or an earlier manoeuvre before the new one; the slot's counter up again */
                float rel = wrap180(atan2f(v->self_x - v->enemy_x, v->self_z - v->enemy_z) * 180.0f / 3.14159265f - v->enemy_heading);
                own_drop(m);
                if (m->flank_slot >= 0 && v->flank_slots) v->flank_slots[m->flank_slot]--;
                if (m->flank_slot < 0) m->flank_slot = flank_pick(m, rel, v->own_flank, v->flank_slots);
                if (v->flank_slots) v->flank_slots[m->flank_slot]++;
                m->flank_retarget = 546.0f;
                slot_point(v->enemy_x, v->enemy_z, v->enemy_heading, m->flank_slot * 2, 15000.0f, &m->flank_x, &m->flank_z);
                own_add(m, m->flank_x, m->flank_z);
                own_goal(m, -1);
            } else if (m->flank_slot < 0) {
                float rel = wrap180(atan2f(v->self_x - v->enemy_x, v->self_z - v->enemy_z) * 180.0f / 3.14159265f - v->enemy_heading);
                int sector = ((int)lrintf(rel / 45.0f) + 8) % 8, slot, k2;
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
            }
            if (!ai_dos_int) {
            m->flank_retarget = 543.0f;
            {   /* the 16-way table 0x1024cd70 at slot x 2: (x, z) = 15000 / (f_x, f_z) in the target's frame (0 -> 0):
                 * the eight directions on a square - the diagonals 212 m out, the sides 150 m */
                static const float FX[8] = {0, 1, 1, 1, 0, -1, -1, -1}, FZ[8] = {1, 1, 0, -1, -1, -1, 0, 1};
                float a = v->enemy_heading * 3.14159265f / 180.0f, lx = 15000.0f * FX[m->flank_slot], lz = 15000.0f * FZ[m->flank_slot];
                m->flank_x = v->enemy_x + cosf(a) * lx + sinf(a) * lz;
                m->flank_z = v->enemy_z - sinf(a) * lx + cosf(a) * lz;
            }
            }
        }
        {   /* 0x1001f9d0: +0x184 = 0 - steer for the slot (throttle 0x100134c0 with R 4000: none within 40 m); once the
             * throttle is none and the target is more than 5 deg (0x10247528, double) off the legs, +0x184 = 1: steer for
             * the TARGET, stepping round at 1/640 (0x1001f910) while more than 5 deg off, else standing - until the slot
             * is more than 45 m away (0x458ca000; a re-picked slot every 0x21f ticks) */
            float fx = m->flank_x - v->self_x, fz = m->flank_z - v->self_z, fd = sqrtf(fx * fx + fz * fz);
            if (m->flank_face && fd > 4500.0f) m->flank_face = 0;
            if (!m->flank_face && fd < 4000.0f && fabsf(wrap180(v->enemy_bearing - v->heading)) > 5.0f) m->flank_face = 1;
            if (m->flank_face) { *R = 1e9f; break; }   /* the caller steers for the target: no throttle, 10 % past 5 deg */
        }
        *R = 4000.0f;
        *dist = -1.0f;   /* tell the caller to steer to the flank point */
        break;
    }
    case 2: case 3: {   /* 0x1001faf0: done within max(1, closing cm/tick x 0.12987) x 80 m (0x100208f0) */
        /* DOS 0x2d42f: (closing << 16) / 500000 (the 3Dfx DLL's x 0.12987) */
        float k = closing(m, d, ticks) * (ai_dos_int ? 65536.0f / 500000.0f : 0.12987013f);
        *R = 5500.0f;
        if (k < 1.0f) k = 1.0f;
        if (d <= k * 8000.0f) done = 1;
        if (m->man == 3) {
            m->weave_ticks += ticks;
            if (m->weave_ticks >= (ai_dos_int ? 546.0f : 543.0f)) { m->weave_ticks = 0; m->weave = -m->weave; }   /* DOS 0x2c3a7: + 0x222 */
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
        if (ai_dos_int) {
            /* DOS MW2.EXE 0x2d5c0: phase 0 jets while usable (heat < 40) and 0x2da50 holds - the target 8 m above AND hidden
             * along the line from the unit's feet; then the jets off, phase 1 for 0xb6 ticks (rand(4) == 0 with heat
             * under 20: 5 next) with the jets ON again and steering at the target, phase 2 ends on the ground */
            if (m->man_phase == 0) {
                if (v->can_jump && v->heat < 40.0f && v->jump_low) { m->jet = 1; break; }
                m->man_phase = 1; m->man_ticks = 0;
                if (v->can_jump && v->heat < 20.0f && ai_rand(m, 4) == 0) m->forced_man = 5;
            } else if (m->man_phase == 1) {
                if (m->man_ticks > 182.0f) m->man_phase = 2; else m->jet = 1;
            } else if (v->height <= 0) done = 1;
            break;
        }
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
    case 5: {   /* 0x1001fe30, after 4 or 9 (in the air): the legs not steered, the throttle held (no 0x10013600 /
                 * 0x100134c0). Done (0x1001ea40 case 5) on the ground (object +0x78) or in contact (+0xa4), with the tank
                 * under 1 tick (+0xc0 < 1), or when 0x10020a30(40) fails. Beyond 10 m the jets fire (0x100201b0(1)) with
                 * the FORWARD (+0x20) or the BACKWARD (+0x21) jet, never neither (0x100208e0: +0x21 = !+0x20) - a
                 * directional jet gives no lift (0x10019310), so the unit glides. b = 1 when the target's bearing off
                 * the legs is beyond the twist limit (+0xe0) but under 3 x it (0x10247548); forward = b while closing
                 * faster than 36 cm/tick (9 within 60 m: 0x45bb8000), else !b - i.e. ahead and closing fast: brake;
                 * ahead and not closing: thrust at it. Inside 10 m: jets off, +0x20 / +0x21 = 0, +0x180 + 1 if they
                 * were firing. The port had read the flag as "ahead", leaving neither jet while not closing: the jets
                 * then lifted it straight up for the whole manoeuvre (250 m in PLUM / FUCH) */
        int done0 = v->height <= 1.0f || v->contact;
        *R = 1e9f;
        if (m->man_phase == 2) { done = done0; break; }
        if (d > 1000.0f) {   /* +0xc0 > 0x447a0000 */
            float off = fabsf(wrap180(v->enemy_bearing - v->heading)), lim = v->twist_limit;
            int b = off > lim && off < lim * 3.0f;
            float cl = closing(m, d, ticks);
            int fwd = cl > (d > 6000.0f ? 36.0f : 9.0f) ? b : !b;
            m->jet = 1; m->jet_forward = fwd; m->jet_back = !fwd;
            if (!jets_ok(v, 40.0f)) done0 = 1;
        } else if (was_jet) m->man_phase++;
        if (v->jet_fuel < 1.0f) done0 = 1;
        done = done0;
        break;
    }
    case 6:   /* 0x1001fb90: charge at full throttle; within 20 m (+0xc4 <= 2000.0) input +0x43 = 1 - the self-destruct */
        *R = -1.0f;
        if ((ai_dos_int && v->enemy_dist3 > 0.0f ? v->enemy_dist3 : d) <= 2000.0f) { done = 1; m->self_destruct = 1; }   /* DOS 0x2d4e4: +0xc4, the 3D distance */
        break;
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
    case 12:
        if (ai_dos_int) {   /* DOS MW2.EXE 0x2d990: every 0x5b0 ticks (+0x176) with the target beyond 150 m (+0xc0 > 0x3a98) the own
                             * points are dropped (0x233b0) and four made round it where it is now (0x23420); within 30 m of the goal
                             * the next own point - an aircraft circles its target 100 m out. Ends on its time limit 0x4718 */
            float fx, fz;
            m->flank_retarget -= ticks;
            if (m->flank_retarget < 0) {
                if (d > 15000.0f) { own_drop(m); own_square(m, v); }
                m->flank_retarget = 1456.0f;
            }
            *R = 3000.0f;
            *dist = -1.0f;
            fx = m->flank_x - v->self_x; fz = m->flank_z - v->self_z;
            if (sqrtf(fx * fx + fz * fz) < 3001.0f) own_goal(m, m->goal_pt);
            break;
        }
        *R = 3000.0f; break;
    default: done = 1;
    }
    if (*dist >= 0) *dist = d;
    /* out of weapons (0x1001ea40 -> 0x10020880, 0x1001ecc8..0x1001ed7d): a unit with a top speed (controller +0x88 != 0),
     * unless jumping (+0x196), in 6 (+0x170) or fleeing (state +0x14a == 4); a manoeuvre the step above has just forced
     * (0: 10 under 45 m) is replaced - +0x174 is not tested */
    jumping = m->man == 4 || m->man == 5 || m->man == 9 || m->man == 11;   /* +0x196 */
    if (v->moving && v->out_of_weapons && !jumping && m->man != 6 && m->state != AI_FLEE) {
        if (ai_rand(m, 4) != 0 && m->prev_man != 5 && jets_ok(v, 40.0f)) { m->forced_man = 4; m->queue5 = 1; }   /* +0x184 = 1 */
        else {
            unsigned r2 = ai_rand(m, 2);
            m->forced_man = r2 == 0 ? -2 : 6;
            if (v->pilot_level >= 2) m->forced_man = -2;   /* +0x19e bit 0x20: level 1 mechs only charge */
        }
    }
    if (ai_dos_int) { if (m->cls == 1 && v->contact && !m->reverse && !m->jump_man && m->man != 10) done = 1; }   /* DOS 0x2c587 -> 0x2e820: class 1 only */
    else if (v->contact && !m->reverse && !jumping && m->man != 10) done = 1;   /* 0x10020e00 */
    if (done || m->man_ticks >= m->man_limit || m->forced_man != -1) end_manoeuvre(m, v);
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

void ai_missile_warning(ai_mind *m, const ai_view *v, int shooter_level, int in_view, float shooter_rel,
                        int (*side_free)(void *ctx, int side), void *ctx)
{
    if (ai_dos_int) {
        /* DOS MW2.EXE 0x2e450: the shooter's +0x198 bit 4 (pilot level <= 2) and rand(3) != 0; a guided weapon or the PPC; the
         * target not already set to jump (+0x16e != 4); jets with heat < 65 (0x2e3d0(0x41)) and rand(3) != 0 -> the dodge
         * direction from the shooter's quadrant off the target's heading (q = (bearing - heading + 360) % 360 / 90): +0x17a =
         * ((q + 1) % 4) x 4, then a 50 m probe one side at random, then the other: clear -> manoeuvre 11. Otherwise a level
         * <= 3 pilot with no manoeuvre forced jumps (4) */
        int side, k, q;
        float r;
        if (shooter_level > 2 || ai_rand(m, 3) == 0) return;
        if (!in_view || m->forced_man == 4) return;   /* the forced manoeuvre waits for the next chooser call, +0x16e */
        if (!jets_ok(v, 65.0f)) return;
        if (ai_rand(m, 3) != 0) {
            r = fmodf(shooter_rel + 360.0f, 360.0f);
            q = (int)(r / 90.0f) & 3;
            side = ai_rand(m, 2) == 0 ? 1 : -1;   /* 0x2e62d: rand(2) != 0 -> -1 */
            for (k = 0; k < 2; k++, side = -side)
                if (!side_free || side_free(ctx, side)) { m->forced_man = 11; m->dodge_fwd = ((q + 1) % 4) * 4 > 2; return; }
        }
        if (m->forced_man == -1 && v->pilot_level <= 3) m->forced_man = 4;
        return;
    }
    /* 0x10020ac0: shooter +0x19e bit 4 (level < 3) and rand(3) != 0; then the target: jets and heat < 65
     * (0x10020a30(0x41)), rand(3) != 0 -> probe a 50 m sidestep, one side at random then the other (0x100204e0 /
     * 0x100103e0): clear -> manoeuvre 11, which always jets to the LEFT (0x1001f290 sets input +0x1e; the probed side
     * only decides whether it dodges); otherwise (or on the rand) a level < 4 pilot jumps (4) */
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
    m->entered = 0;
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
                m->entered = 1; m->entered_from = m->state;                 /* 0x10013e00: exit + entry, even into the same state */
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
                        m->next_fire = (float)(ai_rand(m, 10) * (ai_dos_int ? 18u : 22u));   /* DOS 0x22c56: x 0x12 */
                        if (from == AI_PATROL && v->unit_led && !m->nav_ok) { m->nav_ok = 1; m->nav_x = v->self_x; m->nav_z = v->self_z; }
                    }
                }
            }
        }
    }
    m->rule_fired = fired;
    if (m->state != AI_GODIRECT) m->to_navref = 0;
    if (!v->has_enemy && (m->state == AI_TARGET || m->state == AI_ATTACK) && !fired) { m->entered = 1; m->entered_from = m->state; m->state = m->prev_state; m->target = 0; }
    if (m->state != AI_ATTACK && m->man >= 0) {
        if (ai_dos_int) end_manoeuvre(m, v);   /* the attack state's exit (DOS 0x22b05 -> 0x2ced0) */
        else { m->prev_man = m->man; m->man = -1; m->reverse = 0; m->jet_side = 0; }
    }
    if (m->state != AI_ATTACK) m->jet = 0;
    m->wants_fire = 0;
    m->next_fire -= ticks;
    if (fired) return;   /* inputs hold (0x10011c50: the state's movement runs only when no rule fired) */

    /* movement per state (engine 0x100130f0) */
    {
        float want = v->heading, dist = 0, R = 10000.0f, thr = 0, err, turn0;
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
                if (ai_dos_int) {
                    /* DOS MW2.EXE 0x2e300: switched only while standing (+0x78) - a turn jump lasts until the landing - and
                     * not switched off while the manoeuvre is a jump (+0x190) */
                    if (lim < 360.0f && off >= lim) { if (!m->turn_jet && jets_ok(v, 20.0f) && v->grounded) m->turn_jet = 1; }
                    else if (v->grounded && !m->jump_man) m->turn_jet = 0;
                } else
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
        if (ai_dos_int) {   /* DOS 0x225be / 0x225f4 -> 0x24710: moving or not, with the throttle input of the last tick */
            if (m->state >= AI_FOLLOW && m->state <= AI_GODIRECT && m->cls == 1) patrol_sweep_dos(m, m->throttle, ticks);
            else m->torso = 0;
        }
        if (engaged) {
            float aim = wrap180(v->enemy_bearing - v->heading);
            m->twist_demand = aim;
            m->pitch_demand = v->enemy_elev > 60.0f ? 60.0f : v->enemy_elev < -60.0f ? -60.0f : v->enemy_elev;
            if (ai_dos_int) { m->twist_demand = dos_twist_q(aim); m->pitch_demand = dos_pitch_q(v->enemy_elev); }
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
        turn0 = m->turn;   /* as the manoeuvre left it (a start may have zeroed it) */
        err = wrap180(want - v->heading);
        m->turn = ai_nav_turn(err);   /* the DOS edition: whole degrees (DOS 0x229e0, from 0x2c472 / 0x2d230..0x2d3d8) */
        if (m->state == AI_ATTACK && m->man == 3) m->turn += 450.0f * m->weave;
        if (m->state == AI_ATTACK && m->man == 11) m->turn = 0;            /* 0x1001f290 case 11: +0xc = 0 */
        /* in attack only the manoeuvre handlers steer (0x100130f0 has no state-3 case): 4 (0x1001fdd0), 5 (0x1001fe30), 11
         * (0x1001fe10) and 9 outside phase 1 (0x1001fcc0) do not, nor a tick without a manoeuvre (one just ended,
         * 0x1001f500): the leg input +0xc stays as it was (4 / 9 / 11 zero it at the start, 0x1001f290; 5 keeps the last
         * manoeuvre's) */
        if (m->state == AI_ATTACK && (m->man < 0 || m->man == 4 || m->man == 5 || (m->man == 9 && m->man_phase != 1))) m->turn = turn0;
        /* throttle 0x100134c0 (3Dfx floats, full = 1/64) in the DOS edition's integers (DOS 0x228d0, input +8 with full =
         * 0x400): 0x400 beyond 2 R, 0x333 (819 / 1024) from R, else 0; turning harder than 45 (|turn| >> 16 > 0x2d):
         * (0x4000000 - chop(|turn| x 2/3)) >> 16, at least 0x66 (102 / 1024); halved (>> 1) above 65 heat (heat >> 16 >
         * 0x41); under 0x66 -> 0. The 3Dfx floats (0.8 / 0.1 of full, heat > 65.0) differ by under 0.4 % */
        {
            int ti;
            if (dist > 2.0f * R) ti = 0x400;
            else if (dist >= R) ti = 0x333;
            else ti = 0;
            if (full_attack) ti = 0x400;
            float tt = m->turn;
            if (ai_dos_int) {
                /* DOS: the turn rule reads the leg input as it stands when 0x228d0 runs - the state handler and manoeuvres
                 * 0 (0x2d221), 7 and 12 call it BEFORE the steering 0x229e0 writes this tick's turn; 1, 2 / 3 and 6 after
                 * it, 3 before its weave is added (the GBL port's audit; MW2.EXE the same code) */
                if (m->state != AI_ATTACK || m->man == 0 || m->man == 7 || m->man == 12) tt = turn0;
                else if (m->man == 3) tt = m->turn - 450.0f * m->weave;
            }
            if (ti != 0 && (int)(fabsf(tt) * 65536.0f) >> 16 > 0x2d) {
                ti = (0x4000000 - (int)((double)(int)(fabsf(tt) * 65536.0f) * (2.0 / 3.0))) >> 16;
                if (ti < 0x66) ti = 0x66;
            }
            if ((int)v->heat > 0x41) ti >>= 1;
            if (ti < 0x66) ti = 0;
            if (ai_dos_int && m->state == AI_ATTACK && (m->man == 8 || m->man == 10)) ti = 0x400;   /* DOS 0x2c48c / 0x2d97e: 0x400
                                                                                                      * written directly, no turn or heat rule */
            thr = (float)ti / 1024.0f;
            /* 4, 5, 9 and 11 do not call 0x100134c0: input +8 holds as it was (4 zeroes it at the start, 0x1001f290) - not
             * re-scaled by the turn or heat rules each tick (the port had halved a held throttle every tick above 65 heat) */
            if (m->state == AI_ATTACK && (m->man == 11 || m->man == 4 || m->man == 5 || m->man == 9)) thr = m->throttle;
        }
        /* 0x1001f910 (manoeuvres 0 / 1): no throttle but more than 5 deg off -> 1/640 = 10% of full to step round (DOS
         * 0x2d1c0 / 0x2d200: 0x66 = 102 / 1024) */
        if (m->state == AI_ATTACK && (m->man == 0 || m->man == 1) && thr <= 0 && fabsf(err) > 5.0f) thr = 102.0f / 1024.0f;
        if (m->state == AI_FLEE) thr = 1.0f;
        m->throttle = thr;
        /* patrol / scouting torso sweep (0x10014ff0) */
        if (ai_dos_int) {
            /* (swept before the steering: patrol_sweep_dos) */
        } else if (m->state >= AI_FOLLOW && m->state <= AI_GODIRECT) {
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
