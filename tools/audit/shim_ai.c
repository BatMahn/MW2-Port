/* shim_ai.c - the port's ai.c with its statics reachable, for tools/audit/simdiff.py */
#include "../../src/ai.c"
void shim_set_dos(int on) { ai_dos_int = on; }
float shim_nav_turn(float err) { return ai_nav_turn(err); }

void shim_dos_close(int previous, int distance, int ticks, int out[3])
{
    ai_mind m = {.man = 2, .state = AI_ATTACK, .last_dist = (float)previous,
                 .forced_man = -1, .man_limit = 9100};
    const ai_view view = {.enemy_dist = (float)distance};
    float range = 0, dist = 0, weave = 0;
    int avoiding = 0;
    attack_manoeuvre(&m, &view, (float)ticks, &range, &dist, &weave, &avoiding);
    out[0] = m.man < 0;
    out[1] = (int)m.last_dist;
    m.last_dist = (float)previous;
    out[2] = (int)closing(&m, (float)distance, (float)ticks);
}

/* Compare the original attack controller's start and active-step paths. */
void shim_dos_attack_step(int man, int chosen, int ticks, int left, int distance, int bearing,
                          int throttle, int height, int target_height, unsigned seed, int out[7])
{
    ai_mind m;
    ai_init(&m, -1);
    m.progs[0] = -1;
    m.state = AI_ATTACK;
    m.man = man;
    m.forced_man = man < 0 ? chosen : -1;
    m.man_limit = (float)left;
    m.next_fire = 1000.0f;
    m.throttle = (float)throttle / 1024.0f;
    m.turn = 15.0f;
    m.reverse = man == 8 || man == 10;
    m.jet = man == 4 || man == 11;
    m.jet_side = man == 11 ? -1 : 0;
    m.jump_man = m.jet;
    m.rng = seed;
    const ai_view view = {.has_enemy = 1, .enemy_dist = (float)distance, .enemy_bearing = (float)bearing,
                          .world_height = (float)height, .target_world_height = (float)target_height,
                          .pilot_level = 4, .grounded = 1, .twist_limit = 90.0f};
    ai_step(NULL, &m, &view, (ticks + 0.25f) / 182.0f, ticks);
    out[0] = m.man;
    out[1] = (int)lrintf(m.throttle * 1024.0f);
    out[2] = (int)lrintf(m.turn * 65536.0f);
    out[3] = m.reverse;
    out[4] = m.jet;
    out[5] = m.jet_side < 0;
    out[6] = m.man < 0 ? 0 : (int)(m.man_limit - m.man_ticks);
}

void shim_dos_glide(int phase, int previous, int distance, int bearing, int twist_limit,
                   int fuel, int heat, int installed, int grounded, int contact, int ticks,
                   int held_jet, int held_forward, int held_back, int out[8])
{
    ai_mind m;
    ai_init(&m, -1);
    m.progs[0] = -1;
    m.state = AI_ATTACK;
    m.man = 5;
    m.jump_man = 1;
    m.man_phase = phase;
    m.last_dist = (float)previous;
    m.man_limit = 900;
    m.next_fire = 1000;
    m.throttle = 0.5f;
    m.turn = 15;
    m.jet = held_jet;
    m.jet_forward = held_forward;
    m.jet_back = held_back;
    const ai_view v = {.has_enemy = 1, .enemy_dist = (float)distance, .enemy_bearing = (float)bearing,
                       .pilot_level = 4, .twist_limit = (float)twist_limit, .grounded = grounded,
                       .contact = contact, .jet_fuel = (float)fuel, .heat = (float)heat,
                       .can_jump = installed && fuel >= 6};
    ai_step(NULL, &m, &v, (ticks + 0.25f) / 182.0f, ticks);
    out[0] = m.man;
    out[1] = m.man >= 0 ? m.man_phase : -1;
    out[2] = (int)m.last_dist;
    out[3] = m.jet;
    out[4] = m.jet_forward;
    out[5] = m.jet_back;
    out[6] = (int)lrintf(m.turn * 65536.0f);
    out[7] = (int)lrintf(m.throttle * 1024.0f);
}

void shim_dos_jump_climb(int phase, int elapsed, int left, int ticks, int bearing, int fuel,
                         int heat, int blocked, int grounded, int height, int vy, int jets, int held_jet, int out[6])
{
    ai_mind m;
    ai_init(&m, -1);
    m.progs[0] = -1;
    m.state = AI_ATTACK;
    m.man = 9;
    m.jump_man = 1;
    m.man_phase = phase;
    m.jet = held_jet;
    m.man_phase_ticks = (float)elapsed;
    m.man_limit = (float)left;
    m.next_fire = 1000;
    m.throttle = 0.5f;
    m.turn = 15;
    const ai_view v = {.has_enemy = 1, .enemy_dist = 20000, .enemy_bearing = (float)bearing,
                       .pilot_level = 4, .grounded = grounded, .height = grounded ? 1 : 1500,
                       .can_jump = jets && fuel >= 6, .jet_fuel = (float)fuel, .heat = (float)heat,
                       .world_height = (float)height, .vertical_speed = (float)vy / 65536, .jets_installed = jets,
                       .jump_low = blocked, .twist_limit = 90};
    ai_step(NULL, &m, &v, (ticks + 0.25f) / 182.0f, ticks);
    out[0] = m.man;
    out[1] = m.man < 0 ? -1 : m.man_phase;
    out[2] = m.man < 0 ? 0 : (int)(m.man_limit - m.man_ticks);
    out[3] = m.jet;
    out[4] = (int)lrintf(m.turn * 65536.0f);
    out[5] = (int)lrintf(m.throttle * 1024.0f);
}

static int blocked_approach(void *context, float bearing)
{
    ai_mind *m = context;
    (void)bearing;
    m->turn = -204.8f;
    m->throttle = 0.25f;
    return 1;
}

static int target_blocks_charge(void *context, float bearing)
{
    blocked_approach(context, bearing);
    return 0;
}

void shim_dos_flank_point(int32_t x, int32_t z, int32_t yaw, int slot, int dist, int32_t out[2])
{
    float fx, fz;
    slot_point((float)x, (float)z, yaw / 65536.0f, slot, (float)dist, &fx, &fz);
    out[0] = (int32_t)fx;
    out[1] = (int32_t)fz;
}

void shim_dos_flank(int man, int face, int x, int z, int target, int throttle, int blocked, int out[3])
{
    ai_mind m;
    ai_init(&m, -1);
    m.progs[0] = -1;
    m.state = AI_ATTACK;
    m.man = man;
    m.man_limit = 900;
    m.next_fire = 1000;
    m.throttle = (float)throttle / 1024.0f;
    m.turn = 15;
    m.flank_slot = 0;
    m.flank_retarget = 900;
    m.flank_face = face;
    m.flank_x = (float)x;
    m.flank_z = (float)z;
    const ai_view v = {.has_enemy = 1, .enemy_dist = 12000, .enemy_bearing = (float)target,
                       .pilot_level = 4, .dos_avoid = blocked ? blocked_approach : NULL,
                       .avoid_context = &m};
    ai_step(NULL, &m, &v, 6.0f / 182.0f, 6);
    out[0] = man == 7 ? m.man < 0 : m.flank_face;
    out[1] = (int)lrintf(m.turn * 65536.0f);
    out[2] = (int)lrintf(m.throttle * 1024.0f);
}

void shim_dos_circle(int x, int z, int until_retarget, int ticks, int blocked, int out[4])
{
    ai_mind m;
    ai_init(&m, -1);
    m.progs[0] = -1;
    m.state = AI_ATTACK;
    m.man = 12;
    m.man_limit = 900;
    m.next_fire = 1000;
    m.throttle = 0.5f;
    m.turn = 15;
    m.flank_retarget = (float)until_retarget;
    m.guard_n = 2;
    m.guard[0][0] = (float)x;
    m.guard[0][1] = (float)z;
    m.guard[1][0] = -9000;
    own_goal(&m, -1);
    const ai_view v = {.has_enemy = 1, .enemy_dist = 12000, .pilot_level = 4,
                       .dos_avoid = blocked ? blocked_approach : NULL, .avoid_context = &m};
    ai_step(NULL, &m, &v, (ticks + 0.25f) / 182.0f, ticks);
    out[0] = m.goal_pt;
    out[1] = (int)m.flank_retarget;
    out[2] = (int)lrintf(m.turn * 65536.0f);
    out[3] = (int)lrintf(m.throttle * 1024.0f);
}

void shim_dos_circle_replay(const int32_t *frames, int count, int32_t *out)
{
    ai_mind m;
    ai_init(&m, -1);
    ai_set_class(&m, 5, 80);
    m.progs[0] = -1;
    m.state = AI_ATTACK;
    m.next_fire = 30000;
    ai_view v = {.has_enemy = 1, .pilot_level = 4, .enemy_x = 50000, .enemy_z = 60000};
    start_manoeuvre(&m, &v, 12);
    for (int i = 0; i < count; i++) {
        const int32_t *frame = frames + 5 * i;
        v.self_x = (float)frame[0];
        v.self_z = (float)frame[1];
        v.enemy_x = (float)frame[2];
        v.enemy_z = (float)frame[3];
        v.enemy_dist = (float)dos_level_distance(frame[2] - frame[0], frame[3] - frame[1]);
        ai_step(NULL, &m, &v, frame[4] / 182.0f, frame[4]);
        int32_t *row = out + 35 * i;
        row[0] = m.goal_pt;
        row[1] = m.guard_n;
        row[2] = (int32_t)m.flank_retarget;
        for (int j = 0; j < m.guard_n; j++) {
            row[3 + 2*j] = (int32_t)m.guard[j][0];
            row[4 + 2*j] = (int32_t)m.guard[j][1];
        }
    }
}

void shim_dos_approach(int man, int previous, int distance, int bearing, int ticks,
                      int sign, int until_flip, int blocked, int out[6])
{
    ai_mind m;
    ai_init(&m, -1);
    m.progs[0] = -1;
    m.state = AI_ATTACK;
    m.man = man;
    m.man_limit = 900;
    m.last_dist = (float)previous;
    m.weave = (float)sign;
    m.weave_ticks = (float)until_flip;
    m.next_fire = 1000;
    m.throttle = 0.5f;
    m.turn = 15;
    const ai_view v = {.has_enemy = 1, .enemy_dist = (float)distance, .enemy_bearing = (float)bearing,
                       .pilot_level = 4, .twist_limit = 90,
                       .dos_avoid = blocked == 2 ? target_blocks_charge : blocked ? blocked_approach : NULL,
                       .avoid_context = &m};
    ai_step(NULL, &m, &v, (ticks + 0.25f) / 182.0f, ticks);
    out[0] = m.man;
    out[1] = (int)lrintf(m.turn * 65536.0f);
    out[2] = (int)lrintf(m.throttle * 1024.0f);
    out[3] = (int)m.weave;
    out[4] = (int)m.weave_ticks;
    out[5] = m.self_destruct;
}

/* the manoeuvre start's limit (start_manoeuvre) for manoeuvre man after prev (contact / contact_unit), rand fixed via seed */
float shim_man_limit(int man, int prev, int contact, int contact_unit, unsigned seed, int *reverse, int *jet_side, int *jet_fwd, int dodge_fwd)
{
    ai_mind m; ai_view v;
    ai_init(&m, 0); memset(&v, 0, sizeof v);
    m.rng = seed; m.prev_man = prev; m.dodge_fwd = dodge_fwd;
    v.contact = contact; v.contact_unit = contact_unit; v.enemy_dist = 10000.0f;
    start_manoeuvre(&m, &v, man);
    *reverse = m.reverse; *jet_side = m.jet_side; *jet_fwd = m.jet_forward;
    return m.man_limit;
}
/* the chooser's answer for a unit of class cls / tons with the given state (prev, contact, reverse, jump_man, man,
 * target_not_unit, can_jump, heat, level, enemy above, grounded); seed for its rands */
int shim_choose(int cls, int tons, int prev, int contact, int reverse, int jump_man, int man, int tnu, int can_jump, float heat,
                int level, int above, int grounded, float twist_limit, float off, unsigned seed)
{
    ai_mind m; ai_view v;
    ai_init(&m, 0); memset(&v, 0, sizeof v);
    ai_set_class(&m, cls, tons);
    m.rng = seed; m.prev_man = prev; m.reverse = reverse; m.jump_man = jump_man; m.man = man;
    v.contact = contact; v.target_not_unit = tnu; v.can_jump = can_jump; v.heat = heat; v.pilot_level = level;
    v.enemy_height = above ? 1000.0f : 0.0f; v.jump_view = above; v.height = 0; v.grounded = grounded; v.twist_limit = twist_limit;
    v.enemy_bearing = off; v.heading = 0;
    return choose_manoeuvre(&m, &v);
}
unsigned shim_rand(unsigned seed, unsigned n) { ai_mind m; ai_init(&m, 0); m.rng = seed; return ai_rand(&m, n); }
/* the DOS patrol sweep over a run of frames: per frame its ticks and the throttle input (of the last tick); the torso after each */
void shim_patrol_seq(int n, const float *ticks, const float *thr, float *torso_out, int dir0, float torso0, float rem0)
{
    ai_mind m; int k;
    ai_init(&m, 0);
    m.sweep_dir = dir0; m.torso = torso0; m.sweep_ticks = rem0;
    for (k = 0; k < n; k++) { patrol_sweep_dos(&m, thr[k], ticks[k]); torso_out[k] = m.torso; }
}
/* manoeuvre 1's slot pick for a bearing rel off the target's facing, rand fed by seed, the counters */
int shim_flank_pick(float rel, unsigned seed, const int *own, const int *tgt)
{ ai_mind m; ai_init(&m, 0); m.rng = seed; return flank_pick(&m, rel, own, tgt); }
/* the DOS chooser with the 4-then-5 flag and the unit's last manoeuvre none (prev -1) or set */
int shim_choose2(int cls, int tons, int prev, int contact, int reverse, int jump_man, int man, int tnu, int can_jump, float heat,
                 int level, int above, int grounded, float twist_limit, float off, int queue5, unsigned seed, unsigned *rng_out)
{
    ai_mind m; ai_view v; int r;
    ai_init(&m, 0); memset(&v, 0, sizeof v);
    ai_set_class(&m, cls, tons);
    m.rng = seed; m.prev_man = prev; m.reverse = reverse; m.jump_man = jump_man; m.man = man; m.queue5 = queue5;
    v.contact = contact; v.target_not_unit = tnu; v.can_jump = can_jump; v.heat = heat; v.pilot_level = level;
    v.jump_view = above; v.grounded = grounded; v.twist_limit = twist_limit; v.enemy_bearing = off; v.heading = 0;
    r = choose_manoeuvre(&m, &v);
    *rng_out = m.rng;
    return r;
}
/* the 4-then-5 flag after a manoeuvre start (DOS: +0x17e cleared by every start) */
int shim_start_q5(int man) { ai_mind m; ai_view v; ai_init(&m, 0); memset(&v, 0, sizeof v); m.queue5 = 1; v.enemy_dist = 10000.0f; start_manoeuvre(&m, &v, man); return m.queue5; }
/* the DOS missile warning on a target: the probes answer from free_side[0] (side +1) / free_side[1] (side -1), in the
 * order asked (order_out: up to two sides) */
static int g_free[2], g_order[2], g_on;
static int shim_side(void *ctx, int side) { (void)ctx; if (g_on < 2) g_order[g_on++] = side; return g_free[side > 0 ? 0 : 1]; }
int shim_warn(unsigned seed, int shooter_level, int in_view, float rel, int can_jump, float heat, int level, int forced_in, int free_plus,
              int free_minus, int *order_out, unsigned *rng_out)
{
    ai_mind m; ai_view v;
    ai_init(&m, 0); memset(&v, 0, sizeof v);
    m.rng = seed; m.forced_man = forced_in; m.state = AI_ATTACK;
    v.can_jump = can_jump; v.heat = heat; v.pilot_level = level;
    g_free[0] = free_plus; g_free[1] = free_minus; g_on = 0; g_order[0] = g_order[1] = 0;
    ai_missile_warning(&m, &v, shooter_level, in_view, rel, shim_side, NULL);
    order_out[0] = g_order[0]; order_out[1] = g_order[1];
    *rng_out = m.rng;
    return m.forced_man;
}
float shim_twist_q(float d) { return dos_twist_q(d); }
float shim_pitch_q(float d) { return dos_pitch_q(d); }
