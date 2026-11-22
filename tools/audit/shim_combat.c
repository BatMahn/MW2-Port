/* shim_combat.c - the port's combat.c with its statics reachable, for tools/audit/simdiff.py (ctypes) */
#include "../../src/combat.c"

combat_unit *shim_unit_new(void) { combat_unit *u = calloc(1, sizeof *u); return u; }
void shim_unit_free(combat_unit *u) { free(u); }
void shim_set_dos(int on) { combat_dos = on; }

static unsigned record_weapon_roll(void *ctx, unsigned bound)
{
    *(unsigned *)ctx = bound;
    return 0;
}

void shim_weapon_parameters(int w, int out[7])
{
    const sim_weapon sw = weapon_spec(w);
    combat_unit u = {0};
    unsigned bound = 0;
    u.weapon_count = 1;
    u.weapons[0].weapon = w;
    u.weapons[0].ammo = -1;
    u.weapons[0].state = CW_READY;
    u.weapons[0].location = MEK_CT;
    u.internal[MEK_CT] = 1.0f;
    combat_ai_select(&u, 10000.0f, record_weapon_roll, &bound);
    out[0] = sw.refire;
    out[1] = sw.interval;
    out[2] = combat_weapon_life(w);
    out[3] = (int)combat_weapon_damage(w);
    out[4] = (int)lroundf(sw.heat * 65536.0f);
    out[5] = sw.ammo_per_ton;
    out[6] = (int)bound;
}

struct weapon_rolls { const unsigned *values; unsigned *bounds; int used; };
static unsigned scripted_weapon_roll(void *ctx, unsigned bound)
{
    struct weapon_rolls *r = ctx;
    int i = r->used++;
    r->bounds[i] = bound;
    return r->values[i] % bound;
}

int shim_ai_weapon_pick(int n, int sel, int distance, float heat, const int (*slots)[3],
                        const unsigned *rolls, unsigned *bounds, int *end_sel)
{
    combat_unit u = {0};
    struct weapon_rolls r = {rolls, bounds, 0};
    u.weapon_count = n;
    u.ai_sel = sel;
    u.heat = heat;
    u.internal[MEK_CT] = 1.0f;
    for (int i = 0; i < n; i++) {
        u.weapons[i].weapon = slots[i][0];
        u.weapons[i].state = slots[i][1];
        u.weapons[i].ammo = slots[i][2];
        u.weapons[i].location = MEK_CT;
    }
    int picked = combat_ai_select(&u, (float)distance, scripted_weapon_roll, &r);
    *end_sel = u.ai_sel;
    return picked;
}

void shim_jet_step(int fuel, int held, int power, int infinite, int out[5])
{
    combat_unit u = {0};
    const float dt = 6.0f / COMBAT_TICKS_PER_S;
    unsigned rng = 1;
    combat_jet_fuel_max = 1820;
    combat_jet_refill_dos = 1;
    u.jets = fuel < 0 ? 0 : 3;
    u.jet_fuel = (float)fuel;
    u.jet_refill_acc = 0.5f;  /* the six-tick step crosses a 30 Hz recharge boundary */
    u.jet_ddy = 0.12f;
    u.offline = power < 2;
    u.shutdown = power == 3;
    u.infinite_jets = infinite;
    out[3] = combat_jets_firing(&u, held, dt);
    out[4] = combat_jets_dy(&u, held, dt) > 0.0f;
    combat_jets(&u, held, dt, &rng);
    out[0] = (int)lroundf(u.jet_fuel);
    out[1] = u.jet_on;
    out[2] = (int)(u.heat * 65536.0f);
}

/* heat: one frame - `add` heat (16.16) arrives, `ticks` whole ticks pass (the DOS frame) */
typedef struct {
    int heat16, sink16, ticks, add16;
    int shutdown, pending, override, latch, warn_latch, destroyed;
    int msg_warn, msg_seq, msg_down, restarted;
    float seq_ticks, heat;
} shim_heat;
void shim_heat_setup(combat_unit *u, int sink16, int human, int seed)
{
    memset(u, 0, sizeof *u);
    u->sink_per_tick = (float)sink16 / 65536.0f;
    u->human = human;
    u->rng = (unsigned)seed;
    u->hold_fire_above = human ? 0 : 65;
}
void shim_heat_frame(combat_unit *u, shim_heat *h)
{
    u->heat += (float)h->add16 / 65536.0f;
    u->override = h->override;
    u->heat_warning = u->seq_alarm = u->auto_shutdown = u->restarted = 0;
    combat_tick(u, (float)h->ticks / COMBAT_TICKS_PER_S, h->ticks);
    h->heat = u->heat;
    h->shutdown = u->shutdown; h->pending = u->shutdown_pending; h->override = u->override;
    h->latch = u->alarm_latch; h->warn_latch = u->warn_latch; h->destroyed = u->destroyed;
    h->msg_warn = u->heat_warning; h->msg_seq = u->seq_alarm; h->msg_down = u->auto_shutdown; h->restarted = u->restarted;
    h->seq_ticks = u->seq_ticks;
}
void shim_set_seq(combat_unit *u, float t) { u->seq_ticks = t; }
/* the unit's dissipation per tick (16.16 units x heat sinking) as combat_init and combat_set_player_difficulty set it */
float shim_unit_sink(int heat_sinking, int player, int difficulty, int temperature)
{
    combat_unit u; combat_planet p = {1.0f, temperature, 100000.0f, 0};
    combat_set_planet(&p);
    memset(&u, 0, sizeof u);
    u.sink_per_tick = (float)heat_sinking * (g_planet.temperature < -30 ? 0.00152587890625f : g_planet.temperature > 50 ? 0.0006866455078125f : 0.000762939453125f);
    if (player) combat_set_player_difficulty(&u, difficulty);
    return u.sink_per_tick;
}
/* Try one heat override roll, returning bit 0 for a cook-off and bit 1 for a random draw. */
int shim_override_roll(int ticks, unsigned seed, int invulnerable)
{
    combat_unit u;
    memset(&u, 0, sizeof u);
    u.human = 1;
    u.rng = seed;
    u.override = 1;
    u.shutdown_pending = 1;
    u.invulnerable = invulnerable;
    u.heat = u.heat_mark = 105.0f;
    combat_tick(&u, (float)ticks / COMBAT_TICKS_PER_S, ticks);
    return u.destroyed | ((u.rng != seed) << 1);
}
/* collision damage: armour taken from each location (out[8]) for a collide() at the given speed / normal / bearing */
void shim_collide(float speed, float ny, float rel, int other_tons, int own_tons, float out[8])
{
    combat_unit u; int l; unsigned rng = 1;
    memset(&u, 0, sizeof u);
    for (l = 0; l < 8; l++) { u.armor[l] = 1000.0f; u.rear[l] = 1000.0f; u.internal[l] = 1000.0f; u.armor_max[l] = 1000; }
    u.tons = own_tons;
    collide(&u, speed, ny, rel, other_tons, 0, &rng);
    for (l = 0; l < 8; l++) out[l] = 1000.0f - u.armor[l];
}
