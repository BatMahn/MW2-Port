/* gusmid.c - see gusmid.h. */
#include "gusmid.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXV 32
#define GF1_FRAME 44100.0   /* envelope update rate (GF1 frames/s at <= 14 voices; ASSUMED) */

/* ---------------- patches ---------------- */
typedef struct {
    float   *data; int len;            /* samples */
    double   loop_start, loop_end;      /* in samples (with 1/16 fractions) */
    int      rate;
    double   low, high, root;           /* Hz */
    int      balance;                   /* 0..15 */
    uint8_t  env_rate[6], env_off[6];
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
    float vel;
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
    for (i = 0; i < 16; i++) { s->vol[i] = 127; s->pan[i] = 64; s->expr[i] = 127; s->bend[i] = 8192; }
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
        v->env = (sm->modes & 0x40) ? 0 : 4095;
        v->vel = (float)((vel / 127.0) * (vel / 127.0));
        v->started = s->t;
        v->off_at = s->t + (uint64_t)((double)dur * s->spt) + 1;
    }
}

static void release(voice *v) { if (v->active && !v->released) { v->released = 1; if (v->stage < 3) v->stage = 3; } }

static void do_event(gus_song *s, const ev *e)
{
    int hi = e->st & 0xf0, ch = e->st & 15, k;
    switch (hi) {
    case 0x90: note_on(s, ch, e->a, e->b, e->dur); break;
    case 0x80: for (k = 0; k < MAXV; k++) if (s->v[k].active && s->v[k].ch == ch && s->v[k].note == e->a) release(&s->v[k]); break;
    case 0xc0: s->prog[ch] = e->a; break;
    case 0xe0: s->bend[ch] = e->a | e->b << 7; break;
    case 0xb0:
        switch (e->a) {
        case 7: s->vol[ch] = e->b; break;
        case 10: s->pan[ch] = e->b; s->pan_set[ch] = 1; break;
        case 11: s->expr[ch] = e->b; break;
        case 64: s->sustain[ch] = e->b >= 64; break;
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
        case 121: s->vol[ch] = 127; s->expr[ch] = 127; s->bend[ch] = 8192; s->sustain[ch] = 0; break;
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

/* advance the envelope one output sample; returns 0 when the voice has finished */
static int envelope(voice *v, int outrate)
{
    const gus_sample *sm = v->s;
    double target, inc;
    int r;
    if (!(sm->modes & 0x40)) {   /* no envelope: full volume, quick fade on release */
        if (v->released) { v->env -= 4095.0 * 100.0 / outrate; if (v->env <= 0) return 0; }
        return 1;
    }
    if (v->stage > 5) return 0;
    if ((sm->modes & 0x20) && !v->released && v->stage == 3) return 1;   /* hold at the sustain point */
    target = sm->env_off[v->stage] * 16.0;
    r = sm->env_rate[v->stage];
    inc = (r & 63) / pow(8.0, r >> 6) * (GF1_FRAME / outrate);
    if (inc <= 0) inc = 1e-6;
    if (v->env < target) { v->env += inc; if (v->env >= target) { v->env = target; v->stage++; } }
    else { v->env -= inc; if (v->env <= target) { v->env = target; v->stage++; } }
    if (v->stage > 5) return 0;   /* end of release */

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
            amp = gf1_amp(v->env) * v->vel * (s->vol[v->ch] / 127.0) * (s->vol[v->ch] / 127.0) * (s->expr[v->ch] / 127.0) * (s->expr[v->ch] / 127.0);
            pan = s->pan_set[v->ch] ? s->pan[v->ch] / 127.0 : sm->balance / 15.0;
            a = smp * amp * 0.35;
            out[f * 2] += (float)(a * cos(pan * 1.5707963));
            out[f * 2 + 1] += (float)(a * sin(pan * 1.5707963));
            /* advance with loops */
            v->pos += v->step * v->dir;
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
