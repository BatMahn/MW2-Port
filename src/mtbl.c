/* mtbl.c - see mtbl.h. */
#include "mtbl.h"

#include <stdlib.h>
#include <string.h>

#include "bwd.h"

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static int rd16s(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }

static void cstr(char *dst, size_t n, const uint8_t *src, size_t max)
{
    size_t i;
    for (i = 0; i + 1 < n && i < max && src[i] >= 32 && src[i] < 127; i++) dst[i] = (char)src[i];
    dst[i] = '\0';
    while (i > 0 && dst[i - 1] == ' ') dst[--i] = '\0';
}

int mtbl_decode(const uint8_t *c, size_t len, mtbl_table *t)
{
    int k, i;
    memset(t, 0, sizeof *t);
    if (len < 0x26 + 0x97 || memcmp(c, "MTBL", 4) != 0) return -1;
    t->index = (int)rd32(c + 8);
    t->time_limit = rd32(c + 12);
    cstr(t->win_sound, sizeof t->win_sound, c + 0x12, 9);
    cstr(t->lose_sound, sizeof t->lose_sound, c + 0x1d, 9);
    t->node_count = (int)((len - 0x26) / 0x97);
    if (t->node_count > MTBL_MAX_NODES) t->node_count = MTBL_MAX_NODES;
    for (k = 0; k < t->node_count; k++) {
        const uint8_t *L = c + 0x5a + (size_t)k * 0x97;
        mtbl_node *n = &t->nodes[k];
        if ((size_t)(L - c) + 0x23 + 1 > len) { t->node_count = k; break; }
        n->kind = rd32(L - 0x34);
        n->visible = L[-0x30] == 'V';
        n->require_all = L[-0x2e] != 0;
        for (i = 0; i < MTBL_ITEMS; i++) {
            const uint8_t *it = L - 0x2a + i * 4;
            char ty = (char)it[0];
            if (ty != 'C' && ty != 'S' && ty != 'F') break;   /* 'I' / other ends the list */
            n->items[i].type = ty;
            n->items[i].node = it[1];
            n->items[i].table = it[2];
            n->item_count++;
        }
        n->delay = rd32(L - 0x0a);
        n->objective = (char)L[-5];
        n->priority = L[-6];
        n->need = L[-4];   /* members to put on each target (0 = all; engine runtime node +0xb3) */
        n->lock = L[-3];   /* command lock bits (+0xb7: 1 -> 2, 2 -> 1, 0x100146e0) */
        cstr(n->sound_start, sizeof n->sound_start, L, 9);
        cstr(n->sound_finish, sizeof n->sound_finish, L + 0x0b, 9);
        n->target_id = rd16s(L + 0x14);
        cstr(n->target, sizeof n->target, L + 0x16, 9);
        n->other_table = rd16s(L + 0x1f);
        n->other_node = rd16s(L + 0x21);
        if ((size_t)(L - c) + 0x23 + 60 <= len) cstr(n->text, sizeof n->text, L + 0x23, 60);
    }
    return 0;
}

const char *mtbl_kind_name(uint32_t k)
{
    switch (k) {
    case MTBL_K_TIMER: return "timer";
    case MTBL_K_DESTROY1: case MTBL_K_DESTROY2: return "destroy";
    case MTBL_K_PROTECT: return "protect";
    case MTBL_K_SCAN: return "visit";
    case MTBL_K_START: return "start";
    case MTBL_K_ALLMAIN: return "finish";
    case MTBL_K_REACH: return "reach";
    case MTBL_K_TIMER2: return "timer2";
    case MTBL_K_ORDER: return "order";
    case MTBL_K_HOLD: return "hold";
    case MTBL_K_LEAVE: return "leave";
    case MTBL_K_STAR_WIN: return "star-win";
    case MTBL_K_STAR_LOSE: return "star-lose";
    case MTBL_K_TOGGLE: return "toggle";
    case MTBL_K_FAIL: return "force-fail";
    case MTBL_K_SUCCEED: return "force-ok";
    case MTBL_K_RESET: return "reset";
    case MTBL_K_INSTANT: return "instant";
    case MTBL_K_REARM: return "re-arm";
    default: return "?";
    }
}

/* ---- interpreter ---------------------------------------------------------------- */

int mtbl_logic_init(mtbl_logic *ml, const uint8_t *scene, size_t len)
{
    int n = bwd_chunks(scene, len, NULL, 0), k, t, i;
    bwd_chunk *c;
    memset(ml, 0, sizeof *ml);
    if (n <= 0 || !(c = calloc((size_t)n, sizeof *c))) return -1;
    bwd_chunks(scene, len, c, n);
    for (k = 0; k < n; k++) {
        mtbl_table tmp;
        if (strcmp(c[k].tag, "MTBL") != 0) continue;
        if (mtbl_decode(c[k].data - 8, c[k].size + 8, &tmp) != 0) continue;
        if (tmp.index < 0 || tmp.index >= MTBL_MAX_TABLES) continue;
        ml->tables[tmp.index] = tmp;
        if (tmp.index + 1 > ml->table_count) ml->table_count = tmp.index + 1;
    }
    free(c);
    for (t = 0; t < ml->table_count; t++) {
        ml->current[t] = -1;
        for (i = 0; i < MTBL_MAX_NODES; i++) {
            ml->started[t][i] = -1;
            ml->ended[t][i] = -1;
            ml->shown[t][i] = ml->tables[t].nodes[i].visible;
        }
    }
    return 0;
}

/* engine 0x1000a110 / 0x1000a080 */
static int node_ready(const mtbl_logic *ml, int t, int i)
{
    const mtbl_node *n = &ml->tables[t].nodes[i];
    int k, all = 1;
    if (ml->state[t][i] == MTBL_OK || ml->state[t][i] == MTBL_FAILED) return 0;
    if (n->kind == MTBL_K_START) return 1;
    for (k = 0; k < n->item_count; k++) {
        const mtbl_item *it = &n->items[k];
        int st = (it->table < ml->table_count && it->node < MTBL_MAX_NODES) ? ml->state[it->table][it->node] : 0;
        int ok = (it->type == 'C' && (st == MTBL_OK || st == MTBL_FAILED)) || (it->type == 'S' && st == MTBL_OK) ||
                 (it->type == 'F' && st == MTBL_FAILED);
        if (!n->require_all && ok) return 1;
        all &= ok;
        if (n->require_all && !all) return 0;
    }
    return n->require_all;
}

static void finish_s(mtbl_logic *ml, int t, int i, int ok, const mtbl_world *w, int silent)
{
    ml->state[t][i] = ok ? MTBL_OK : MTBL_FAILED;
    ml->ended[t][i] = ml->now;
    if (ml->current[t] == i) ml->current[t] = -1;   /* re-picked at the end of the step */
    ml->silent = silent;
    if (w && w->finished) w->finished(w->user, t, i, ok);
    ml->silent = 0;
}
static void finish(mtbl_logic *ml, int t, int i, int ok, const mtbl_world *w) { finish_s(ml, t, i, ok, w, 0); }

static void pick_current(mtbl_logic *ml)
{
    /* engine 0x1000a9b0: the first node (table order) that is ready and is not a table-control
     * kind (kind & 0xffff0000 == 0) is the group's current order */
    int t, i;
    for (t = 0; t < ml->table_count; t++) {
        ml->current[t] = -1;
        for (i = 0; i < ml->tables[t].node_count; i++)
            if (node_ready(ml, t, i) && (ml->tables[t].nodes[i].kind & 0xFFFF0000u) == 0) { ml->current[t] = i; break; }
    }
}

void mtbl_logic_step(mtbl_logic *ml, int32_t now, const mtbl_world *w)
{
    int t, i, changed = 1, guard = 0;
    ml->now = now;
    /* repeat while something finishes, so chains resolve within one step */
    while (changed && guard++ < 16) {
        changed = 0;
        for (t = 0; t < ml->table_count; t++) {
            for (i = 0; i < ml->tables[t].node_count; i++) {
                const mtbl_node *n = &ml->tables[t].nodes[i];
                const char *tg = (n->target[0] && strcmp(n->target, "NULL") != 0) ? n->target : NULL;
                int elapsed;
                if (!node_ready(ml, t, i)) continue;
                if (ml->state[t][i] != MTBL_ACTIVE) {
                    ml->state[t][i] = MTBL_ACTIVE;
                    ml->started[t][i] = now;
                    if (w && w->started) w->started(w->user, t, i);

                }
                /* the timer (engine 0x1000a1d0): delay < floor(now) - floor(armed), whole mission seconds (clock / 181);
                 * a negative delay never passes */
                elapsed = (int32_t)n->delay >= 0 && (int64_t)(int32_t)n->delay < (int64_t)(now / 1000) - (int64_t)(ml->started[t][i] / 1000);
                switch (n->kind) {
                case MTBL_K_START: case MTBL_K_INSTANT:
                    finish(ml, t, i, 1, w); changed = 1; break;   /* no condition: LAB_1000a621 with the test true */
                case MTBL_K_TIMER: case MTBL_K_TIMER2: case MTBL_K_ORDER: case MTBL_K_HOLD:
                    if (elapsed) { finish(ml, t, i, 1, w); changed = 1; }
                    break;
                case MTBL_K_DESTROY1: case MTBL_K_DESTROY2: case MTBL_K_SCAN: case MTBL_K_ALLMAIN: case MTBL_K_REACH:
                case MTBL_K_LEAVE: {
                    /* 0x1000a1d0: every attached target must pass (destroyed / visited / reached); NO target attached
                     * passes at once (the test flag starts true) - a node whose target has nothing of a matching class.
                     * Leave: none of them within. Reach on a group's table (not the player's) only for its current node */
                    int nok = 0, cnt = w && w->targets ? w->targets(w->user, t, i, n->kind, tg, &nok) : 0, ok, j;
                    if (n->kind == MTBL_K_LEAVE) ok = nok == 0;
                    else ok = nok >= cnt;
                    if (n->kind == MTBL_K_REACH && t != 0 && ml->current[t] != i) ok = 0;
                    if (n->kind == MTBL_K_ALLMAIN)   /* and every OTHER main ('M') node of the table succeeded (node +0x75) */
                        for (j = 0; j < ml->tables[t].node_count && ok; j++)
                            if (j != i && ml->tables[t].nodes[j].objective == 'M' && ml->state[t][j] != MTBL_OK) ok = 0;
                    if (ok) { finish(ml, t, i, 1, w); changed = 1; }   /* LAB_1000a621 */
                    else if (elapsed) { finish(ml, t, i, 0, w); changed = 1; }   /* timed out */
                    break;
                }
                case MTBL_K_PROTECT: {
                    /* 0x1000a1d0 kind 4: before the timer, a target lost fails it on the player's star (state 6, with the
                     * failure message); on another star it goes to state 8 - not finished, re-tested - and fails (6) once
                     * ALL are lost (no message unless one was lost just now: none attached fails at once, silently). When
                     * the timer passes: any lost -> failed (no message), else succeeded */
                    int nok = 0, cnt = w && w->targets ? w->targets(w->user, t, i, n->kind, tg, &nok) : 0;
                    int any = nok > 0, all = nok >= cnt;
                    if (!elapsed) {
                        if ((any && t == 0) || all) { finish_s(ml, t, i, 0, w, !any); changed = 1; }
                    } else { finish_s(ml, t, i, !any, w, any); changed = 1; }
                    break;
                }
                case MTBL_K_STAR_WIN: case MTBL_K_STAR_LOSE:
                    /* sets the star's result outright (no "already set" test in the engine); out of range: stays pending */
                    if (n->other_table < 0 || n->other_table >= ml->table_count) break;
                    ml->result[n->other_table] = n->kind == MTBL_K_STAR_WIN ? 2 : 3;
                    ml->result_time[n->other_table] = now;
                    finish(ml, t, i, 1, w); changed = 1; break;
                case MTBL_K_TOGGLE:
                    /* flips the shown flag of node (other_table, other_node) - another table's node too */
                    if (n->other_table < 0 || n->other_table >= ml->table_count || n->other_node < 0 ||
                        n->other_node >= MTBL_MAX_NODES || n->other_node > ml->tables[n->other_table].node_count) break;
                    ml->shown[n->other_table][n->other_node] ^= 1;
                    finish(ml, t, i, 1, w); changed = 1; break;
                case MTBL_K_FAIL: case MTBL_K_SUCCEED: {
                    /* out of range, or the target already finished: this node stays pending (state 3) */
                    int ot = n->other_table, on = n->other_node;
                    if (ot < 0 || ot >= ml->table_count || on < 0 || on >= MTBL_MAX_NODES || on > ml->tables[ot].node_count ||
                        ml->state[ot][on] == MTBL_OK || ml->state[ot][on] == MTBL_FAILED) break;
                    finish(ml, ot, on, n->kind == MTBL_K_SUCCEED, w);
                    finish(ml, t, i, 1, w); changed = 1; break;
                }
                case MTBL_K_RESET:
                    /* every node of the table back to unfinished with no times, the announce flags cleared (start nodes
                     * excepted), the star's result and its time cleared; this node always succeeds */
                    if (n->other_table < 0 || n->other_table >= ml->table_count) break;
                    {
                        int j, ot = n->other_table;
                        for (j = 0; j < ml->tables[ot].node_count; j++) {
                            ml->state[ot][j] = MTBL_WAITING; ml->started[ot][j] = -1; ml->ended[ot][j] = -1;
                            if (ml->tables[ot].nodes[j].kind != MTBL_K_START) ml->announced[j] = 0;
                        }
                        ml->current[ot] = -1;
                        ml->result[ot] = 0; ml->result_time[ot] = -1;
                    }
                    finish(ml, t, i, 1, w); changed = 1; break;
                case MTBL_K_REARM:
                    /* node (other_table, other_node) back to unfinished with no times; the engine clears the announce
                     * flag of its OWN index (unless the target is a start node) - kept as the engine has it */
                    if (n->other_table < 0 || n->other_table >= ml->table_count || n->other_node < 0 ||
                        n->other_node >= MTBL_MAX_NODES || n->other_node > ml->tables[n->other_table].node_count) break;
                    ml->state[n->other_table][n->other_node] = MTBL_WAITING;
                    ml->started[n->other_table][n->other_node] = -1;
                    ml->ended[n->other_table][n->other_node] = -1;
                    if (ml->tables[n->other_table].nodes[n->other_node].kind != MTBL_K_START) ml->announced[i] = 0;
                    finish(ml, t, i, 1, w); changed = 1; break;
                default:
                    /* anything else goes the generic way (LAB_1000a621): no condition -> succeeds */
                    finish(ml, t, i, 1, w); changed = 1; break;
                }
            }
        }
    }
    /* per-star mission result (engine 0x1000a9b0): every main ('M') node done -> success; any failed
     * -> failure; else the table's time limit -> time exceeded */
    for (t = 0; t < ml->table_count; t++) {
        int all = 1, failed = 0;
        if (ml->result[t]) continue;
        for (i = 0; i < ml->tables[t].node_count; i++) {
            if (ml->tables[t].nodes[i].objective != 'M' || ml->state[t][i] == MTBL_OK) continue;
            all = 0;
            if (ml->state[t][i] == MTBL_FAILED) failed = 1;
        }
        if (all) ml->result[t] = 2;
        else if (failed) ml->result[t] = 3;
        else if ((int32_t)ml->tables[t].time_limit > 0 && (int64_t)now >= (int64_t)(int32_t)ml->tables[t].time_limit * 1000) ml->result[t] = 4;   /* signed: -1 = none */
        if (ml->result[t]) ml->result_time[t] = now;
    }
    pick_current(ml);
}
