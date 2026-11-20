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
    v.enemy_height = above ? 1000.0f : 0.0f; v.height = 0; v.grounded = grounded; v.twist_limit = twist_limit;
    v.enemy_bearing = off; v.heading = 0;
    return choose_manoeuvre(&m, &v);
}
unsigned shim_rand(unsigned seed, unsigned n) { ai_mind m; ai_init(&m, 0); m.rng = seed; return ai_rand(&m, n); }
