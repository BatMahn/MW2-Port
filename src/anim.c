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
