/* shim_ai.c - the port's ai.c with its statics reachable, for tools/audit/simdiff.py */
#include "../../src/ai.c"
void shim_set_dos(int on) { ai_dos_int = on; }
float shim_nav_turn(float err) { return ai_nav_turn(err); }
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
