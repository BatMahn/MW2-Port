/* anim.c - see anim.h. */
#include "anim.h"

#include <math.h>
#include <string.h>

static int32_t rd32s(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

int anim_parse(const uint8_t *d, size_t len, anim_set *s)
{
    size_t o = 8;
    int t, k;
    memset(s, 0, sizeof *s);
    if (len < 8) return -1;
    s->track_count = rd32s(d);
    s->key_count = rd32s(d + 4);
    if (s->track_count < 0 || s->track_count > ANIM_MAX_TRACKS || s->key_count < 1 || s->key_count > ANIM_MAX_KEYS)
        return -1;
    if ((size_t)s->track_count * (8 + 4 * (size_t)s->key_count) > len - o) return -1;
    for (t = 0; t < s->track_count; t++) {
        anim_track *tr = &s->tracks[t];
        float acc = 0;
        tr->target = rd32s(d + o);
        tr->type = rd32s(d + o + 4);
        tr->pose[0] = 0;
        for (k = 0; k < s->key_count; k++) {
            float v = (float)rd32s(d + o + 8 + (size_t)k * 4);
            if (tr->type > 2) v /= 65536.0f;
            acc += v;
            tr->pose[k + 1] = acc;
        }
        o += 8 + 4 * (size_t)s->key_count;
    }
    for (k = 0; k < s->key_count && o + 8 <= len; k++, o += 8) {
        s->key_flags[k] = (uint32_t)rd32s(d + o);
        memcpy(s->key_bytes[k], d + o + 4, 4);
    }
    return 0;
}

int anim_load_id(prj_archive *a, int anim_id, anim_set *out)
{
    prj_record r;
    int rc, type = prj_find_type(a, "ANIM");
    if (type < 0 || prj_read(a, type, anim_id, &r) != PRJ_OK) return -1;
    rc = anim_parse(r.data, r.size, out);
    prj_record_free(&r);
    return rc;
}

float anim_value(const anim_set *s, int track, float t)
{
    const anim_track *tr;
    int k;
    float f;
    if (track < 0 || track >= s->track_count) return 0;
    tr = &s->tracks[track];
    if (t < 0) t = 0;
    if (t > (float)s->key_count) t = (float)s->key_count;
    k = (int)floorf(t);
    if (k >= s->key_count) return tr->pose[s->key_count];
    f = t - (float)k;
    /* key k moves the pose from pose[k] to pose[k+1] */
    return tr->pose[k] + (tr->pose[k + 1] - tr->pose[k]) * f;
}

int anim_sequence(const anim_set *s, int n, int *first, int *count)
{
    int k, start = -1;
    for (k = 0; k < s->key_count; k++) {
        if (!(s->key_flags[k] & 0x01)) continue;
        if (start >= 0) { *first = start; *count = k - start; return 0; }
        if (s->key_bytes[k][0] == n) start = k;
    }
    if (start >= 0) { *first = start; *count = s->key_count - start; return 0; }
    return -1;
}

int anim_loop_start(const anim_set *s, int first, int count)
{
    int k;
    for (k = 0; k < count; k++)
        if (first + k < s->key_count && (s->key_flags[first + k] & 0x800)) return k > 0 ? k - 1 : 0;
    return 0;
}

/* the loop (engine key player 0x1000dec0): finishing a key flagged 0x8 (or 0x4, when the requested sequence is in its
 * list) jumps back to the nearest earlier key flagged 0x2 - TIMBANIM walk 3-8, run 13-18, reverse 23-28, whose next
 * keys (9, 19, 29) repeat the loop's first pose, so the wrap is seamless. Returns the footfall spacing (keys). */
int anim_loop_range(const anim_set *s, int first, int count, int *ls, int *le)
{
    int k, j, f1 = -1, f2 = -1, d, end = -1;
    for (k = 0; k < count; k++)
        if (first + k < s->key_count && (s->key_flags[first + k] & 0x800)) { if (f1 < 0) f1 = k; else { f2 = k; break; } }
    d = f1 >= 0 && f2 >= 0 ? f2 - f1 : 0;
    for (k = 0; k < count && first + k < s->key_count; k++) if (s->key_flags[first + k] & 0x8) { end = k; break; }
    if (end < 0) for (k = 0; k < count && first + k < s->key_count; k++) if (s->key_flags[first + k] & 0x4) { end = k; break; }
    if (end >= 0) {
        for (j = end - 1; j >= 0; j--) if (s->key_flags[first + j] & 0x2) break;
        if (j >= 0) { *ls = j; *le = end + 1; return d; }
    }
    /* no loop flags: from the first footfall over two footfalls' worth of keys */
    if (f1 < 0 || f2 < 0) { *ls = 0; *le = count; return 0; }
    *ls = f1; *le = f1 + 2 * d;
    if (*le > count) { *le = count; *ls = count - 2 * d; if (*ls < 0) *ls = 0; }
    return d;
}

/* engine 0x1000e600: a key's time by the gait (object +0x8c): 1 walk r + r/2, 2 trot r, 3 run r - r/4, 0 (stopping) r */
int anim_key_time(int gait, int rate)
{
    return gait == 1 ? rate + (rate >> 1) : gait == 3 ? rate - (rate >> 2) : rate;
}

void anim_player_init(anim_player *p, int rate)
{
    memset(p, 0, sizeof *p);
    p->key = -1; p->cur = -1; p->dur = rate;   /* 0x1000dec0 load: +0x94 = the TSK rate, +0x84 = -1, key -1 */
}

/* the key player (engine 0x1000dec0 case 1, one call a frame per animated node; the keys are the same for every node of
 * a mech, so one player drives them all). req = the sequence asked for (object +0x88, set by the gait code 0x1000b3a0:
 * -1 = stop), gait = +0x8c. A key's delta plays linearly over its time; when its time is up (one key a call, as the
 * engine) the key just finished decides the next:
 *   0x40  byte 5 = a sequence: go to it if it is the one asked for;
 *   0x80  byte 5: go to it if a sequence other than the one playing is asked for;
 *   0x100 byte 6: go to it if a stop is asked for (req -1) - the run's stop goes through the walk;
 *         going: search from the next key (up or down by the sequence number) for that sequence's start (0x1, byte 4),
 *         then on from there for a key flagged 0x200 whose byte 7 is the sequence playing, and continue AFTER it;
 *   else 0x8 (always) / 0x4 (when the sequence asked for is in bytes 4-7): back to the nearest earlier key flagged 0x2;
 *   0x400 (not jumping) or past the last key: the sequence ends - the keys sum back to the standing pose there (walk ends
 *         at key 11, reverse at 31). Asked to stop, the mech is then idle at rest; asked to walk, it starts again at key 0.
 * So a stopping mech finishes its stride and walks its feet together at the plain key time (gait 0), as the original.
 * Returns ANIM_EV_* for the key entered. The engine never sets a node's pose: each key adds its delta to the node
 * (0x1000dec0: +0x18 = the key's delta + what is left, applied over the key's time through 0x1000f850 / 0x1000f9d0, and
 * the loader 0x100401c0 keeps the ANIM values as they are), so when the key entered is not the next one (a jump, a loop,
 * a restart from key 0) and the table's pose there differs from the pose reached, the difference stays on the node:
 * p->off[track] += pose[end of the key finished] - pose[start of the key entered] (Timber Wolf / Mad Dog walk 5 -> run
 * 16: one hip 17 degrees, carried from then on - every later mismatch adds its own). The loops meet (keys 9 / 19 / 29
 * repeat the loop's first pose) and the sequences sum to the standing pose, so most transitions add nothing. DOS MW2.EXE
 * plays the keys the same way (0x1aad2: integer deltas through 0x1dfc0 / 0x1e140, no re-sync at a jump or a sequence
 * start); DOSBox with patched MADDANIM data shows the carried pose, not the table's
 * (docs/reference/walk_run_carried_offset_dos_vs_port.png). Records in use: MADDANIM (Timber Wolf, Mad Dog) 17 deg,
 * MARAANIM (Marauder) 15 cm, the rest about 1 or less (STRMANIM / DAISANIM are named by no mech). */
static void carry(const anim_set *s, anim_player *p, int from_end, int to_start)
{
    int k;
    if (from_end == to_start || from_end < 0 || to_start < 0 || from_end > s->key_count || to_start > s->key_count) return;
    for (k = 0; k < s->track_count && k < ANIM_MAX_TRACKS; k++) p->off[k] += s->tracks[k].pose[from_end] - s->tracks[k].pose[to_start];
}
void anim_player_hold(anim_player *p)
{
    p->on = 0;   /* +0x80 bit 0 (DOS 0x26990 / 0x27785 the same, & 0xfe) */
}
int anim_player_step(const anim_set *s, anim_player *p, int req, int gait, int rate, float ticks)
{
    int f, next, trans = 0, target = -1, jumped = 0, ev = 0;
    uint32_t fl;
    if (!p->on) {   /* idle: start when a sequence is asked for (+0x80 bit 0, the key time set; no time used this call) */
        if (req == -1 && p->cur == -1) return 0;
        p->on = 1;
        p->dur = anim_key_time(gait, rate);
        return 0;
    }
    if (p->key == -1) { carry(s, p, (int)p->t, 0); p->key = 0; p->left = 0; p->len = 0; p->cur = 0; }   /* from the first key (sequence 0) */
    else p->left -= ticks;
    if (p->left > 0) {
        p->t = (float)p->key + (p->len > 0 ? 1.0f - p->left / p->len : 1.0f);
        return 0;
    }
    f = p->key;
    next = f + 1;
    p->left += (float)p->dur;                          /* the key time set when this key began */
    if (p->left < -(float)p->dur) p->left = -(float)p->dur;
    if (f < 0 || f >= s->key_count) { p->key = -1; p->on = 0; p->cur = -1; p->t = 0; return 0; }
    fl = s->key_flags[f];
    if (fl & 0x40) { target = (int8_t)s->key_bytes[f][1]; trans = req == target; }
    if (fl & 0x80) { target = (int8_t)s->key_bytes[f][1]; if (req != -1 && p->cur != req) trans = 1; }
    if (fl & 0x100) { target = (int8_t)s->key_bytes[f][2]; trans |= req == -1; }
    if (trans) {
        int dir = target > p->cur ? 1 : -1, j = f, found = 0;
        for (;;) {
            j += dir;
            if (j >= s->key_count || j < 0) break;
            if ((s->key_flags[j] & 0x01) && (int8_t)s->key_bytes[j][0] == target) { found = 1; break; }
        }
        if (found) {
            for (found = 0; j < s->key_count && !found; j++)   /* continues past the key found (0x1000e460) */
                if ((s->key_flags[j] & 0x200) && (int8_t)s->key_bytes[j][3] == p->cur) found = 1;
            if (found) { next = j; jumped = 1; }
        }
    }
    if (!jumped && (fl & 0xc)) {
        int loop = 1, b;
        if (fl & 0x4)
            for (loop = 0, b = 0; b < 4 && (int8_t)s->key_bytes[f][b] != -1; b++) if ((int8_t)s->key_bytes[f][b] == req) { loop = 1; break; }
        if (loop) { int j = f - 1; while (j > 0 && !(s->key_flags[j] & 0x2)) j--; next = j; }
    }
    if ((!jumped && (fl & 0x400)) || next >= s->key_count) { p->left = 0; next = -1; }
    if (jumped) p->cur = target;
    if (next != -1) {
        carry(s, p, f + 1, next);
        p->key = next;
        p->len = p->left;
        p->t = (float)next;
        p->dur = anim_key_time(gait, rate);
        if (s->key_flags[next] & 0x10) ev |= ANIM_EV_SLOW;
        if (s->key_flags[next] & 0x800) ev |= ANIM_EV_STEP;
    } else {
        p->key = -1;
        p->t = (float)(f + 1);   /* the sequence's last key reached: the standing pose */
        if (req == -1) { p->cur = -1; p->on = 0; }
    }
    return ev;
}
