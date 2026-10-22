/* gusmid.c - see gusmid.h. */
#include "gusmid.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXV 32
/* the envelope (volume ramp) update rate = the GF1's frame rate, 617400 / active voices (GUS SDK: 14 voices 44100 Hz
 * ... 32 voices 19293 Hz). MW2's Miles driver ULTRA.MDI (cd SND/) sets 28 active voices: its init fills the card block
 * at 0x57f4 with 0x1c (code 0x8ca) and 0x1bb2 -> 0x1ee7 clamps the count to 14..28 and writes register 0x0e = 0xc0 |
 * (voices - 1) (0x2041) -> 22050 frames/s */
#define GF1_FRAME 22050.0

/* ---------------- patches ---------------- */
typedef struct {
    float   *data; int len;            /* samples */
    double   loop_start, loop_end;      /* in samples (with 1/16 fractions) */
    int      rate;
    double   low, high, root;           /* Hz */
    int      balance;                   /* 0..15 */
    uint8_t  env_rate[6], env_off[6];
    uint8_t  trem_rate, trem_depth, vib_sweep, vib_rate, vib_depth;   /* header 50 / 51 / 52 / 53 / 54 (ULTRA.MDI keeps these,
                                                                       * not the tremolo sweep, 0x2dc7) */
    int      modes;                     /* 0x01 16-bit, 0x02 unsigned, 0x04 loop, 0x08 bidir, 0x10 reverse, 0x20 sustain, 0x40 envelope */
    int      scale_freq, scale_factor;
} gus_sample;
typedef struct { gus_sample *s; int n; int tried; } gus_patch;

struct gus_bank_s {
    char      dir[600];
    char      name[256][16];    /* program 0-127, drums 128-255 (128 + note) */
    gus_patch patch[256];
};

static int rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

gus_bank *gus_bank_open(const char *dir, int memory_kb)
{
    gus_bank *b;
    char path[700], line[256];
    FILE *f;
    int col = memory_kb >= 1024 ? 4 : memory_kb >= 768 ? 3 : memory_kb >= 512 ? 2 : 1;
    int subst[256], i;
    char rowname[256][16];
    snprintf(path, sizeof path, "%s/MIDI/ULTRAMID.INI", dir);
    f = fopen(path, "r");
    if (!f) return NULL;
    b = calloc(1, sizeof *b);
    if (!b) { fclose(f); return NULL; }
    snprintf(b->dir, sizeof b->dir, "%s", dir);
    for (i = 0; i < 256; i++) { subst[i] = -1; rowname[i][0] = 0; }
    while (fgets(line, sizeof line, f)) {
        int n, c[4];
        char nm[16];
        if (line[0] == '#') continue;
        if (sscanf(line, "%d , %d , %d , %d , %d , %15s", &n, &c[0], &c[1], &c[2], &c[3], nm) == 6 && n >= 0 && n < 256) {
            subst[n] = c[col - 1];
            snprintf(rowname[n], sizeof rowname[n], "%s", nm);
        }
    }
    fclose(f);
    for (i = 0; i < 256; i++)   /* the memory-size column names the patch actually loaded */
        if (subst[i] >= 0 && subst[i] < 256 && rowname[subst[i]][0]) snprintf(b->name[i], sizeof b->name[i], "%s", rowname[subst[i]]);
    return b;
}

static void free_patch(gus_patch *p) { int i; for (i = 0; i < p->n; i++) free(p->s[i].data); free(p->s); p->s = NULL; p->n = 0; }
void gus_bank_close(gus_bank *b) { int i; if (!b) return; for (i = 0; i < 256; i++) free_patch(&b->patch[i]); free(b); }

static gus_patch *get_patch(gus_bank *b, int prog)
{
    gus_patch *p;
    char path[800];
    FILE *f;
    long n;
    uint8_t *d;
    size_t o;
    int ns, i;
    if (prog < 0 || prog > 255) return NULL;
    p = &b->patch[prog];
    if (p->tried) return p->n ? p : NULL;
    p->tried = 1;
    if (!b->name[prog][0]) return NULL;
    snprintf(path, sizeof path, "%s/MIDI/%s.pat", b->dir, b->name[prog]);
    f = fopen(path, "rb");
    if (!f) {   /* case-insensitive fallback */
        char up[32]; size_t k;
        snprintf(up, sizeof up, "%s", b->name[prog]);
        for (k = 0; up[k]; k++) if (up[k] >= 'a' && up[k] <= 'z') up[k] = (char)(up[k] - 32);
        snprintf(path, sizeof path, "%s/MIDI/%s.PAT", b->dir, up);
        f = fopen(path, "rb");
    }
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    d = n > 0 ? malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n || n < 239 || memcmp(d, "GF1PATCH1", 9)) { free(d); fclose(f); return NULL; }
    fclose(f);
    ns = d[129 + 63 + 6];               /* layer header: samples */
    o = 129 + 63 + 47;
    p->s = calloc((size_t)(ns > 0 ? ns : 1), sizeof *p->s);
    for (i = 0; i < ns && o + 96 <= (size_t)n; i++) {
        const uint8_t *h = d + o;
        gus_sample *s = &p->s[p->n];
        uint32_t bytes = rd32(h + 8);
        int w = (h[55] & 1) ? 2 : 1, k;
        o += 96;
        if (o + bytes > (size_t)n) break;
        s->modes = h[55];
        s->len = (int)(bytes / (uint32_t)w);
        s->loop_start = (double)rd32(h + 12) / w + (h[7] & 15) / 16.0;
        s->loop_end = (double)rd32(h + 16) / w + (h[7] >> 4) / 16.0;
        s->rate = rd16(h + 20);
        s->low = rd32(h + 22) / 1000.0; s->high = rd32(h + 26) / 1000.0; s->root = rd32(h + 30) / 1000.0;
        s->balance = h[36];
        memcpy(s->env_rate, h + 37, 6); memcpy(s->env_off, h + 43, 6);
        s->trem_rate = h[50]; s->trem_depth = h[51]; s->vib_sweep = h[52]; s->vib_rate = h[53]; s->vib_depth = h[54];
        s->scale_freq = (int16_t)rd16(h + 56); s->scale_factor = rd16(h + 58);
        s->data = malloc(((size_t)s->len + 2) * sizeof(float));
        for (k = 0; k < s->len; k++) {
            int v;
            if (w == 2) { v = (int16_t)rd16(d + o + (size_t)k * 2); if (s->modes & 2) v = (uint16_t)v - 32768; s->data[k] = (float)v / 32768.0f; }
            else { v = d[o + (size_t)k]; if (s->modes & 2) v -= 128; else v = (int8_t)v; s->data[k] = (float)v / 128.0f; }
        }
        s->data[s->len] = s->data[s->len + 1] = s->len ? s->data[s->len - 1] : 0;
        if (s->modes & 0x10) {   /* reverse: play backwards from the end */
            for (k = 0; k < s->len / 2; k++) { float t = s->data[k]; s->data[k] = s->data[s->len - 1 - k]; s->data[s->len - 1 - k] = t; }
        }
        if (s->loop_end > s->len) s->loop_end = s->len;
        if (s->loop_start >= s->loop_end) s->modes &= ~0x0c;
        o += bytes;
        p->n++;
    }
    free(d);
    return p->n ? p : NULL;
}

/* ---------------- song ---------------- */
typedef struct { uint32_t tick; uint8_t st, a, b; uint32_t dur; } ev;
typedef struct {
    int active, ch, note, released, stage;
    const gus_sample *s;
    double pos, step;            /* samples */
    int dir;                     /* +1 / -1 (bidirectional loops) */
    double env;                  /* GF1 volume register 0..4095 */
    int vel;                     /* the note's velocity 1..127 */
    int v11;                     /* ULTRA.MDI voice +0x11: the 8-bit volume the envelope offsets are scaled by */
    int trem;                    /* tremolo running (the sustain point held) */
    double trem_lo, trem_hi, trem_inc; int trem_dir;   /* 12-bit bounds, increment per GF1 frame, -1 / +1 */
    int vib;                     /* vibrato LFO: on, ticks in the quadrant (+0x17), sweep countdown (+0x18), quadrant (+0x19), */
    int vib_cnt, vib_sweep, vib_q, vib_quarter;   /* ticks per quadrant (+0x1a) */
    double vib_dev;              /* the peak deviation as a fraction of the frequency ((ratio - 1) / 2, +0xc / +0xa) */
    double vib_mul;              /* the frequency factor the LFO set */
    uint64_t off_at;             /* sample time of the scheduled note-off (0 = none) */
    uint64_t started;
} voice;

struct gus_song_s {
    gus_bank *bank;
    ev *ev; int nev, pos;
    int rate;
    double spt;                  /* samples per tick (120 Hz) */
    uint64_t t;                  /* samples rendered */
    double tick_base;            /* loop offset in ticks */
    int prog[16], vol[16], pan[16], expr[16], bend[16], pan_set[16], sustain[16];
    int mod_depth[16], mod_rate[16];   /* the modulation wheel's vibrato (channel record +9 / +0xa, 0x36de) */
    double lfo_next;             /* the next LFO timer tick (samples) */
    int loop_pos[4], loop_cnt[4]; double loop_tick[4]; int nloop;
    voice v[MAXV];
    int ended;
};

static uint32_t vlq(const uint8_t *d, size_t len, size_t *i)
{
    uint32_t v = 0;
    while (*i < len) { uint8_t c = d[(*i)++]; v = v << 7 | (c & 0x7f); if (!(c & 0x80)) break; }
    return v;
}

static int parse_evnt(gus_song *s, const uint8_t *d, size_t len)
{
    size_t i = 0;
    uint32_t tick = 0;
    int cap = 1024;
    s->ev = malloc((size_t)cap * sizeof *s->ev);
    while (i < len) {
        uint8_t c = d[i];
        ev e;
        if (c < 0x80) { while (i < len && d[i] < 0x80) tick += d[i++]; continue; }   /* interval count */
        i++;
        memset(&e, 0, sizeof e);
        e.tick = tick; e.st = c;
        if (c == 0xff) {
            uint8_t type = i < len ? d[i++] : 0;
            uint32_t l = vlq(d, len, &i);
            i += l;
            if (type == 0x2f) { e.a = 0x2f; }
            else continue;
        } else if (c == 0xf0 || c == 0xf7) { uint32_t l = vlq(d, len, &i); i += l; continue; }
        else {
            int hi = c & 0xf0;
            e.a = i < len ? d[i++] : 0;
            if (hi != 0xc0 && hi != 0xd0) e.b = i < len ? d[i++] : 0;
            if (hi == 0x90) e.dur = vlq(d, len, &i);
        }
        if (s->nev == cap) { cap *= 2; s->ev = realloc(s->ev, (size_t)cap * sizeof *s->ev); }
        s->ev[s->nev++] = e;
        if (c == 0xff) break;
    }
    return s->nev;
}

/* find the first EVNT chunk inside FORM XDIR / CAT XMID / FORM XMID */
static const uint8_t *find_chunk(const uint8_t *d, size_t len, const char *id, size_t *out_len)
{
    size_t i;
    for (i = 0; i + 8 <= len; i++)
        if (!memcmp(d + i, id, 4)) {
            uint32_t l = (uint32_t)d[i + 4] << 24 | (uint32_t)d[i + 5] << 16 | (uint32_t)d[i + 6] << 8 | d[i + 7];
            if (i + 8 + l <= len) { *out_len = l; return d + i + 8; }
        }
    return NULL;
}

gus_song *gus_song_open(gus_bank *b, const uint8_t *xmi, size_t len, int rate)
{
    gus_song *s;
    size_t el;
    const uint8_t *evnt = find_chunk(xmi, len, "EVNT", &el);
    int i;
    if (!b || !evnt) return NULL;
    s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->bank = b; s->rate = rate; s->spt = rate / 120.0;
    for (i = 0; i < 16; i++) { s->vol[i] = 100; s->pan[i] = 64; s->expr[i] = 127; s->bend[i] = 8192; }   /* 0x52c6 / 0x52e6 */
    if (parse_evnt(s, evnt, el) <= 0) { gus_song_close(s); return NULL; }
    /* preload the patches the song uses (the GUS driver loads them before playing) */
    {
        int prog[16] = {0};
        for (i = 0; i < s->nev; i++) {
            int hi = s->ev[i].st & 0xf0, ch = s->ev[i].st & 15;
            if (hi == 0xc0) prog[ch] = s->ev[i].a;
            if (hi == 0x90) get_patch(b, ch == 9 ? 128 + s->ev[i].a : prog[ch]);
        }
    }
    return s;
}

void gus_song_close(gus_song *s) { if (!s) return; free(s->ev); free(s); }
double gus_song_seconds(const gus_song *s) { return (double)s->t / s->rate; }

static double note_hz(double n) { return 440.0 * pow(2.0, (n - 69.0) / 12.0); }

/* ULTRA.MDI (MW2 CD SND\, Miles AIL 3.10 "Gravis UltraSound MIDI Synth"; offsets in the file, ndisasm -b16):
 * - volume: a voice's 8-bit volume +0x11 = (master 127 + 64) x T[velocity] x T[channel volume x expression / 127] / 48705
 *   (0x4105, again on CC7 / CC11 at 0x3276), T the 128-byte curve at 0x5196; the envelope's offsets are scaled by it in
 *   the GF1's logarithmic 8-bit scale: target = offset x v / 255, at least 5 (0x429f, 0x4967). Channel volume 100 and
 *   expression 127 to start and on reset all controllers (0x52c6 / 0x52e6, 0x4f0f); sustain pedal on at 24 (0x4ec7).
 * - envelope: note-on ramps from 5 to stage 0's target at its rate (0x4276..0x42c8); each ramp's end (0x4787) starts
 *   the next stage; with the sustain mode (0x20) and the note held the end of stage 2 holds (0x4886) - with tremolo
 *   there a looping volume ramp. Note-off (0x4488..): sustain mode -> stage 3 from the current volume (0x4593); an
 *   envelope without it runs on untouched; no envelope (0x40) or fast release (0x80) -> stage 5's rate down to 5.
 * - vibrato (0x3933, ticked by 0x3802 on GF1 timer 2: count 187 x 320 us = 45.29 Hz, 0x4bfd): depth / rate / sweep from
 *   the patch, or the modulation wheel's (0x36de: depth = value x 30 / 128 + 2, rate 200, no sweep); LFO frequency
 *   (rate x 2334 + 5000) / 100 mHz -> ticks a quarter = 45289 / (4 x that), rounded, at least 1; deviation (ratio - 1) / 2
 *   of the frequency, ratio = 2^(depth / 21 / 12) interpolated in the table 0x5216; a triangle 0 -> +dev -> 0 -> -dev,
 *   scaled by (sweep - countdown) / sweep while the countdown runs; restarted by note-on and pitch bend.
 * - tremolo (0x3acc, run once the sustain point holds, 0x48c2): depth d -> (d + 1) >> 3 = w; around the sustain level
 *   L = offset[2] x v / 255 (kept within 5 + w/2 .. 251 - w/2) from L - w/2 to L + w/2 (+1 if w is odd), a bidirectional
 *   looping ramp at the patch rate's LFO frequency (increment / divider from 0x3bc7) */
static const int VOL_T[128] = {   /* 0x5196 */
    0, 129, 145, 155, 161, 166, 171, 174, 177, 180, 182, 185, 187, 188, 190, 192, 193, 195, 196, 197, 198, 199, 201, 202,
    203, 203, 204, 205, 206, 207, 208, 208, 209, 210, 211, 211, 212, 213, 213, 214, 214, 215, 215, 216, 217, 217, 218, 218,
    219, 219, 219, 220, 220, 221, 221, 222, 222, 222, 223, 223, 224, 224, 224, 225, 225, 226, 227, 228, 229, 230, 231, 231,
    232, 233, 234, 234, 235, 236, 236, 237, 238, 238, 239, 239, 240, 241, 241, 242, 242, 243, 243, 244, 244, 244, 245, 245,
    246, 246, 247, 247, 247, 248, 248, 249, 249, 249, 250, 250, 250, 251, 251, 251, 252, 252, 252, 253, 253, 253, 254, 254,
    254, 254, 255, 255, 255, 255, 255, 255};
static const int SEMI_T[13] = {1024, 1085, 1149, 1218, 1290, 1367, 1448, 1534, 1625, 1722, 1825, 1933, 2048};   /* 0x5216 */
#define GF1_VOICES 28
#define LFO_HZ (1.0 / (69 * 320e-6))   /* GF1 timer 2 at 187: 45.29 Hz (0x4c21; 0xb0e9 = 45289 in 0x3933) */

static int voice_vol(const gus_song *s, int ch, int vel)   /* +0x11 */
{
    int cv = s->vol[ch] * s->expr[ch] / 127;
    if (cv < 0) cv = 0;
    if (cv > 127) cv = 127;
    return (127 + 64) * VOL_T[vel] * VOL_T[cv] / 48705;
}
static double env_target(const voice *v, int stage)   /* 12-bit: offset x v / 255, at least 5 */
{
    int t = v->s->env_off[stage] * v->v11 / 255;
    return (t < 5 ? 5 : t) * 16.0;
}
static void vib_setup(gus_song *s, voice *v, int start)   /* 0x3933 */
{
    const gus_sample *sm = v->s;
    int depth = sm->vib_depth, rate = sm->vib_rate, sweep = sm->vib_sweep, fm, q, a, b, rem;
    double ra, rb;
    if (s->mod_depth[v->ch]) { depth = s->mod_depth[v->ch]; rate = s->mod_rate[v->ch]; sweep = 0; }
    fm = (rate * 2334 + 5000) / 100;
    q = 45289 / (4 * fm);
    if ((uint32_t)(45289 % (4 * fm)) > (uint32_t)(2 * fm)) q++;
    v->vib_quarter = q < 1 ? 1 : q;
    a = depth / 21; b = a + 1; rem = depth % 21;
    ra = (double)SEMI_T[a % 12] * (1 << (a / 12)); rb = (double)SEMI_T[b % 12] * (1 << (b / 12));
    v->vib_dev = ((ra + (rb - ra) * rem / 21.0) / 1024.0 - 1.0) / 2.0;
    v->vib = depth != 0;
    if (start) { v->vib_sweep = sweep; v->vib_cnt = 0; v->vib_q = 0; }
}
static void vib_tick(gus_song *s, voice *v)   /* 0x3812: one timer tick */
{
    const gus_sample *sm = v->s;
    double c, x;
    if (!sm->vib_depth && !s->mod_depth[v->ch]) { v->vib_mul = 1.0; return; }
    c = (double)v->vib_cnt / v->vib_quarter;
    x = v->vib_q == 0 ? c : v->vib_q == 1 ? 1.0 - c : v->vib_q == 2 ? -c : -(1.0 - c);
    if (v->vib_sweep && sm->vib_sweep) { x *= (double)(sm->vib_sweep - v->vib_sweep) / sm->vib_sweep; v->vib_sweep--; }
    v->vib_mul = 1.0 + x * v->vib_dev;
    if (++v->vib_cnt >= v->vib_quarter) { v->vib_q = (v->vib_q + 1) % 4; v->vib_cnt = 0; }
}
static void trem_setup(voice *v, int outrate)   /* 0x3acc: the looping ramp over the sustain level */
{
    const gus_sample *sm = v->s;
    int L = sm->env_off[2] * v->v11 / 255, w = (sm->trem_depth + 1) >> 3, h = w >> 1, lo, hi, div = 0;
    double span, per;
    if (L - h < 5) L = h + 5;
    if (L + h + (w & 1) > 251) L = 251 - h - (w & 1);
    lo = L - h; hi = L + h + (w & 1);
    span = (double)(hi - lo) * 16.0 * GF1_VOICES * 16.0;
    per = (double)(5000000 / (sm->trem_rate * 2334 + 5000)) * 100000.0;
    while (div <= 3 && span <= per) { span *= 8; div++; }
    if (div > 3) div = 3;
    {
        int inc = (int)((span + per / 2) / per);
        if (inc > 63) inc = 63;
        v->trem_inc = inc / pow(8.0, div) * (GF1_FRAME / outrate);
    }
    v->trem_lo = lo * 16.0; v->trem_hi = hi * 16.0;
    v->trem_dir = v->trem_lo < v->env ? -1 : 1;
    v->trem = v->trem_inc > 0;
}

static void note_on(gus_song *s, int ch, int note, int vel, uint32_t dur)
{
    gus_patch *p = get_patch(s->bank, ch == 9 ? 128 + note : s->prog[ch]);
    const gus_sample *sm = NULL;
    double f, best = 1e30;
    int i, k, slot = -1;
    if (!p || vel == 0) return;
    f = note_hz(note + (s->bend[ch] - 8192) / 4096.0);
    for (i = 0; i < p->n; i++) {   /* the sample whose range holds the pitch, else the nearest root */
        if (f >= p->s[i].low && f <= p->s[i].high) { sm = &p->s[i]; break; }
        if (fabs(log(f / (p->s[i].root > 0 ? p->s[i].root : 1))) < best) { best = fabs(log(f / (p->s[i].root > 0 ? p->s[i].root : 1))); sm = &p->s[i]; }
    }
    if (!sm || sm->len < 2 || sm->root <= 0) return;
    if (sm->scale_factor != 1024) {   /* pitch scaling (drums: 0 = fixed pitch) */
        int sf = (sm->scale_freq >= 0 && sm->scale_freq < 128) ? sm->scale_freq : 60;
        f = note_hz(sf + (note - sf) * sm->scale_factor / 1024.0 + (s->bend[ch] - 8192) / 4096.0);
    }
    for (k = 0; k < MAXV; k++) if (!s->v[k].active) { slot = k; break; }
    if (slot < 0) {   /* steal: a released voice first, else the oldest */
        uint64_t old = (uint64_t)-1;
        for (k = 0; k < MAXV; k++) if (s->v[k].released && s->v[k].started < old) { old = s->v[k].started; slot = k; }
        if (slot < 0) for (k = 0; k < MAXV; k++) if (s->v[k].started < old) { old = s->v[k].started; slot = k; }
    }
    {
        voice *v = &s->v[slot];
        memset(v, 0, sizeof *v);
        v->active = 1; v->ch = ch; v->note = note; v->s = sm; v->dir = 1;
        v->step = (f / sm->root) * ((double)sm->rate / s->rate);
        v->vel = vel < 1 ? 1 : vel > 127 ? 127 : vel;
        v->v11 = voice_vol(s, ch, v->vel);
        v->env = 5 * 16.0;   /* the ramp starts at 5 (0x4291) */
        v->vib_mul = 1.0;
        vib_setup(s, v, 1);
        v->started = s->t;
        v->off_at = s->t + (uint64_t)((double)dur * s->spt) + 1;
    }
}

static void release(voice *v)   /* note-off (0x4488..0x46ab) */
{
    const gus_sample *sm = v->s;
    if (!v->active || v->released) return;
    v->released = 1;
    v->trem = 0;
    if (!(sm->modes & 0x40) || (sm->modes & 0x80)) v->stage = 5;   /* stage 5's rate down to 5 */
    else if (sm->modes & 0x20) { if (v->stage < 3) v->stage = 3; }   /* from the sustain point on */
}

static void set_volume(gus_song *s, int ch)   /* CC7 / CC11 (0x3586 -> 0x3266): each voice's volume again; the
                                                * envelope keeps its place (the current level rescaled) */
{
    int k;
    for (k = 0; k < MAXV; k++) {
        voice *v = &s->v[k];
        int nv;
        if (!v->active || v->ch != ch) continue;
        nv = voice_vol(s, ch, v->vel);
        if (v->v11 > 0 && nv != v->v11) v->env = v->env * nv / v->v11;
        v->v11 = nv;
        if (v->trem) trem_setup(v, s->rate);
    }
}

static void do_event(gus_song *s, const ev *e)
{
    int hi = e->st & 0xf0, ch = e->st & 15, k;
    switch (hi) {
    case 0x90: note_on(s, ch, e->a, e->b, e->dur); break;
    case 0x80: for (k = 0; k < MAXV; k++) if (s->v[k].active && s->v[k].ch == ch && s->v[k].note == e->a) release(&s->v[k]); break;
    case 0xc0: s->prog[ch] = e->a; break;
    case 0xe0:
        s->bend[ch] = e->a | e->b << 7;
        for (k = 0; k < MAXV; k++) if (s->v[k].active && s->v[k].ch == ch && s->v[k].vib) vib_setup(s, &s->v[k], 1);   /* 0x3644 */
        break;
    case 0xb0:
        switch (e->a) {
        case 1:   /* modulation (0x36de): the channel's vibrato */
            s->mod_depth[ch] = e->b ? e->b * 30 / 128 + 2 : 0; s->mod_rate[ch] = 200;
            for (k = 0; k < MAXV; k++) if (s->v[k].active && s->v[k].ch == ch && (s->v[k].s->vib_depth || s->mod_depth[ch])) vib_setup(s, &s->v[k], e->b == 0);
            break;
        case 7: s->vol[ch] = e->b; set_volume(s, ch); break;
        case 10: s->pan[ch] = e->b; s->pan_set[ch] = 1; break;
        case 11: s->expr[ch] = e->b; set_volume(s, ch); break;
        case 64: s->sustain[ch] = e->b >= 24; break;   /* 0x4ec7 */
        case 116:   /* AIL FOR loop: value = count (0 = forever) */
            if (s->nloop < 4) { s->loop_pos[s->nloop] = s->pos; s->loop_cnt[s->nloop] = e->b; s->loop_tick[s->nloop] = e->tick; s->nloop++; }
            break;
        case 117:   /* AIL NEXT */
            if (s->nloop > 0 && e->b >= 64) {
                int L = s->nloop - 1;
                if (s->loop_cnt[L] == 0 || --s->loop_cnt[L] > 0) {
                    s->tick_base += (double)e->tick - s->loop_tick[L];
                    s->pos = s->loop_pos[L];
                } else s->nloop--;
            }
            break;
        case 121:   /* reset all controllers (0x4eef): sustain off, no modulation, bend centred, volume 100, expression 127, pan the patch's */
            s->sustain[ch] = 0; s->mod_depth[ch] = 0; s->bend[ch] = 8192; s->vol[ch] = 100; s->expr[ch] = 127; s->pan_set[ch] = 0;
            set_volume(s, ch);
            break;
        case 123: for (k = 0; k < MAXV; k++) if (s->v[k].active && s->v[k].ch == ch) release(&s->v[k]); break;
        }
        break;
    case 0xf0: if (e->a == 0x2f) s->ended = 1; break;
    }
}

/* GF1 12-bit logarithmic volume -> amplitude */
static double gf1_amp(double v)
{
    int iv = (int)v;
    if (iv <= 0) return 0;
    if (iv > 4095) iv = 4095;
    return (256.0 + (iv & 255)) * ldexp(1.0, iv >> 8) / 16777216.0;
}

/* advance the envelope one output sample (the GF1's volume ramps); returns 0 when the voice has finished */
static int envelope(voice *v, int outrate)
{
    const gus_sample *sm = v->s;
    double target, inc;
    int r;
    if (v->stage > 5) return 0;
    if ((sm->modes & 0x20) && !v->released && v->stage == 3) {   /* the sustain point: held, or the tremolo's loop */
        if (v->trem) {
            v->env += v->trem_dir * v->trem_inc;
            if (v->env >= v->trem_hi) { v->env = v->trem_hi - (v->env - v->trem_hi); v->trem_dir = -1; }
            else if (v->env <= v->trem_lo) { v->env = v->trem_lo + (v->trem_lo - v->env); v->trem_dir = 1; }
        }
        return 1;
    }
    target = v->stage == 5 && (!(sm->modes & 0x40) || (sm->modes & 0x80)) && v->released ? 5 * 16.0 : env_target(v, v->stage);
    r = sm->env_rate[v->stage];
    inc = (r & 63) / pow(8.0, r >> 6) * (GF1_FRAME / outrate);
    if (inc <= 0) inc = 1e-6;
    if (v->env < target) { v->env += inc; if (v->env >= target) { v->env = target; v->stage++; } }
    else { v->env -= inc; if (v->env <= target) { v->env = target; v->stage++; } }
    if (v->stage > 5) return 0;   /* end of release */
    if (v->stage == 3 && (sm->modes & 0x20) && !v->released && sm->trem_depth) trem_setup(v, outrate);   /* 0x48c2 */
    return 1;
}

int gus_song_render(gus_song *s, float *out, int frames)
{
    int f, k;
    for (f = 0; f < frames; f++) {
        double now_tick = (double)s->t / s->spt;
        while (!s->ended && s->pos < s->nev && (double)s->ev[s->pos].tick + s->tick_base <= now_tick) {
            const ev *e = &s->ev[s->pos++];
            do_event(s, e);
        }
        if (s->pos >= s->nev) s->ended = 1;
        if ((double)s->t >= s->lfo_next) {   /* the LFO timer (0x3802) */
            s->lfo_next += s->rate / LFO_HZ;
            for (k = 0; k < MAXV; k++) if (s->v[k].active && s->v[k].vib) vib_tick(s, &s->v[k]);
        }
        for (k = 0; k < MAXV; k++) {
            voice *v = &s->v[k];
            const gus_sample *sm;
            double a, frac, smp, amp, pan;
            int i0;
            if (!v->active) continue;
            sm = v->s;
            if (!v->released && v->off_at && s->t >= v->off_at && !s->sustain[v->ch]) release(v);
            if (!envelope(v, s->rate)) { v->active = 0; continue; }
            i0 = (int)v->pos; frac = v->pos - i0;
            if (i0 < 0 || i0 >= sm->len) { v->active = 0; continue; }
            smp = sm->data[i0] + (sm->data[i0 + 1] - sm->data[i0]) * frac;   /* GF1 linear interpolation */
            amp = gf1_amp(v->env);   /* velocity and volume are in the envelope's levels (v11) */
            pan = s->pan_set[v->ch] ? s->pan[v->ch] / 127.0 : sm->balance / 15.0;
            a = smp * amp * 0.35;
            out[f * 2] += (float)(a * cos(pan * 1.5707963));
            out[f * 2 + 1] += (float)(a * sin(pan * 1.5707963));
            /* advance with loops */
            v->pos += v->step * v->vib_mul * v->dir;
            if (sm->modes & 0x04) {
                if (v->dir > 0 && v->pos >= sm->loop_end) {
                    if (sm->modes & 0x08) { v->dir = -1; v->pos = sm->loop_end - (v->pos - sm->loop_end); }
                    else v->pos -= sm->loop_end - sm->loop_start;
                } else if (v->dir < 0 && v->pos <= sm->loop_start) { v->dir = 1; v->pos = sm->loop_start + (sm->loop_start - v->pos); }
            } else if (v->pos >= sm->len - 1) v->active = 0;
        }
        s->t++;
    }
    if (s->ended) { for (k = 0; k < MAXV; k++) if (s->v[k].active) return 1; return 0; }
    return 1;
}
