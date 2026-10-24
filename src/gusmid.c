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
#define GF1_FRAME 22050   /* table 0x5170 [voices - 14] */

/* ---------------- patches ---------------- */
typedef struct {
    float   *data; int len;            /* samples */
    double   loop_start, loop_end;      /* in samples (with 1/16 fractions) */
    int      rate;
    int32_t  low, high, root;           /* mHz (the driver's sample record +0 / +4 / +8) */
    uint32_t r24;                       /* ULTRA.MDI record +0x24 = (GF1 rate << 9 + rate / 2) / rate (0x2b47) */
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
        s->low = (int32_t)rd32(h + 22); s->high = (int32_t)rd32(h + 26); s->root = (int32_t)rd32(h + 30);
        s->r24 = s->rate ? (uint16_t)((((uint32_t)GF1_FRAME << 9) + (uint32_t)(s->rate >> 1)) / (uint32_t)s->rate) : 0;
        s->balance = h[36];
        memcpy(s->env_rate, h + 37, 6); memcpy(s->env_off, h + 43, 6);
        s->trem_rate = h[50]; s->trem_depth = h[51]; s->vib_sweep = h[52]; s->vib_rate = h[53]; s->vib_depth = h[54];
        s->scale_freq = rd16(h + 56); s->scale_factor = rd16(h + 58);   /* words, unsigned in the driver (+0x10 / +0x12) */
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
    int active, ch, note;
    int released;                /* ULTRA.MDI voice +0x13 bit 2: note-off seen (or the envelope ended) */
    int held;                    /* +0x13 bit 1: holding at the sustain point */
    int playout;                 /* the sample's loop switched off (+0x14 bit 3 cleared): it plays on to its end */
    int stage;                   /* the envelope stage +0x1b (6 = past the end) */
    const gus_sample *s;
    double pos;                  /* samples */
    int dir;                     /* +1 / -1 (bidirectional loops) */
    double env;                  /* GF1 current volume (register 9), 12-bit 0..4095 */
    double r_lo, r_hi;           /* the volume ramp's start / end (registers 7 / 8, 8-bit x 16) */
    int r_rate, r_dir, r_run, r_loop;   /* ramp rate (register 6), direction, running, bidirectional loop (tremolo) */
    int tgt8, start8, rel8;      /* +0x1f the ramp's target, +0x20 its start, +0x1e the release level (unscaled offset) */
    int vc;                      /* +0x16: a volume-change ramp runs, then stage vc - 1 goes on */
    int prog;                    /* +0x4: how far the stage's ramp had got (1024 = all the way) */
    int vel;                     /* +0x1c the note's velocity 1..127 */
    int v11;                     /* +0x11: the 8-bit volume the envelope offsets are scaled by */
    int trem_lo8, trem_hi8, trem_rate;   /* +0xe / +0xf / +0x10: the tremolo's looping ramp */
    int fc_base, fc, fc_out;     /* GF1 frequency control: +0x8 the note's, +0xa bent, the value last written (register 1) */
    int vib_dev;                 /* +0xc the vibrato's peak FC deviation */
    int vib_cnt, vib_sweep, vib_q, vib_quarter;   /* +0x17 ticks in the quadrant, +0x18 sweep countdown, +0x19 quadrant,
                                                    * +0x1a ticks per quadrant */
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
    int prog[16], vol[16], pan[16], expr[16], pan_set[16], sustain[16];
    int bendf[16];               /* channel record +2: the bend as a frequency factor (1024 = none, 0x4c87) */
    int brange[16], rpn_msb[16], rpn_lsb[16];   /* bend range 0x5246 (2), RPN 101 / 100 0x5286 / 0x52a6 (0) */
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
    for (i = 0; i < 16; i++) { s->vol[i] = 100; s->pan[i] = 64; s->expr[i] = 127; s->bendf[i] = 0x400; s->brange[i] = 2; }   /* 0x52c6 / 0x52e6, 0x3203, 0x5246 */
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

/* ULTRA.MDI (MW2 CD SND\, Miles AIL 3.10 "Gravis UltraSound MIDI Synth"; offsets in the file, ndisasm -b16). The voices
 * are emulated as the driver programs the GF1 - frequency control (FC) and volume ramp registers, ramp-end interrupts:
 * - pitch (note-on 0x3ef4): the note's frequency in mHz from the table 0x5326 (or, with the patch's scale factor not 1024,
 *   0x3c97: scale note + (note - scale note) x factor / 1024 interpolated in it); the sample is the one whose low..high
 *   holds it (the last of a run of such; none -> no note, 0x4034); FC = ((f << 9 + root / 2) / root << 9 + r / 2) / r,
 *   r = (22050 << 9 + rate / 2) / rate (0x3c49, 0x2b47), all 32-bit integers; the voice plays FC / 512 samples a GF1
 *   frame. Pitch bend scales it: FC x factor >> 10 (0x35e7, re-written to the sounding voices), factor = the bend x range
 *   in 1/8192 semitones interpolated in the table 0x5216, 2^20 / factor downwards (0x4c87); range 2 (0x5246), set by data
 *   entry (CC6, capped 24) while RPN 101 / 100 are 0 / 0 - their start values, never reset (0x4dc0); CC38 / 96 / 97
 *   only keep a data LSB nothing reads.
 * - volume: a voice's 8-bit volume +0x11 = (master 127 + 64) x T[velocity] x T[channel volume x expression / 127, 1..127]
 *   / 48705 (0x4105, 0x3260, 0x3586), T the curve 0x5196; envelope levels = offset x v / 255, at least 5. Channel
 *   volume 100 and expression 127 to start and on reset all controllers (0x52c6 / 0x52e6, 0x4f0f); sustain pedal >= 24.
 * - envelope: note-on ramps from 5 to stage 0's level at its rate (0x4276); each ramp's end interrupt (0x4787) starts the
 *   next stage from the current level; with the sustain mode (0x20) and the note held the end of stage 2 holds (0x4886) -
 *   with tremolo there a looping ramp. After stage 5 (0x4804) the level stays and the sample's loop is switched off: it
 *   plays on to its end (then the voice is freed, 0x4729). Note-off (0x4488): no envelope (0x40) -> loop off at once;
 *   no envelope or fast release (0x80) -> stage 5's rate down to 5; sustain mode -> stage 3 from the current level (its
 *   direction from the unscaled offset 3 against the level, 0x4601); an envelope without it runs on untouched.
 * - a volume change (CC7 / CC11, 0x3260) stops the running ramp, takes how far it had got (level - start) / (target -
 *   start) in 1/1024 (1024 when held, when start = target or when 0), puts that on the stage's new start..target and
 *   ramps there from the current level at rate 0x43 - downwards always, upwards only up to stage 3; at that ramp's end
 *   the stage goes on from there at its own rate to its new target (+0x16). The tremolo loop is set up again.
 * - vibrato (0x3933, ticked by 0x3802 on GF1 timer 2: count 187 x 320 us = 45.29 Hz, 0x4bfd): depth / rate / sweep from
 *   the patch, or the modulation wheel's (0x36de: depth = value x 30 / 128 + 2, rate 200, no sweep); LFO frequency
 *   (rate x 2334 + 5000) / 100 mHz -> ticks a quarter = 45289 / (4 x that), rounded, at least 1; the peak deviation in FC
 *   units dev = ((ratio x FC >> 10) - FC) >> 1, ratio = 2^(depth / 21 / 12) x 1024 interpolated in 0x5216; each tick
 *   writes FC + dev x n / quarter on a triangle (0, +, 0, -), x (sweep - countdown) / sweep while the countdown runs;
 *   restarted by note-on and pitch bend; with no depth the ticks stop writing (the last FC stays).
 * - tremolo (0x3acc, set up at note-on and on a volume change, run once the sustain point holds): depth d ->
 *   (d + 1) >> 3 = w; around the sustain level L = offset[2] x v / 255 (kept within 5 + w/2 .. 251 - w/2) from L - w/2 to
 *   L + w/2 (+1 if w is odd), a bidirectional looping ramp at the patch rate's LFO frequency (increment / divider).
 * GF1 hardware (INFERRED, as DOSBox's GF1 emulation): a ramp rate byte = increment (bits 0-5) every 1 / 8 / 64 / 512
 * frames (bits 6-7) of the 12-bit volume; a ramp stops at its bound (snapped to it, even when it started past it). */
static const int VOL_T[128] = {   /* 0x5196 */
    0, 129, 145, 155, 161, 166, 171, 174, 177, 180, 182, 185, 187, 188, 190, 192, 193, 195, 196, 197, 198, 199, 201, 202,
    203, 203, 204, 205, 206, 207, 208, 208, 209, 210, 211, 211, 212, 213, 213, 214, 214, 215, 215, 216, 217, 217, 218, 218,
    219, 219, 219, 220, 220, 221, 221, 222, 222, 222, 223, 223, 224, 224, 224, 225, 225, 226, 227, 228, 229, 230, 231, 231,
    232, 233, 234, 234, 235, 236, 236, 237, 238, 238, 239, 239, 240, 241, 241, 242, 242, 243, 243, 244, 244, 244, 245, 245,
    246, 246, 247, 247, 247, 248, 248, 249, 249, 249, 250, 250, 250, 251, 251, 251, 252, 252, 252, 253, 253, 253, 254, 254,
    254, 254, 255, 255, 255, 255, 255, 255};
static const uint32_t SEMI_T[12] = {1024, 1085, 1149, 1218, 1290, 1367, 1448, 1534, 1625, 1722, 1825, 1933};   /* 0x5216 */
static const int32_t NOTE_T[129] = {   /* 0x5326: note -> mHz; note 127 reads 0 and the next dword (8) is past the table */
    8175, 8661, 9177, 9722, 10300, 10913, 11562, 12249, 12978, 13750, 14567, 15433, 16351, 17323, 18354, 19445, 20601,
    21826, 23124, 24499, 25956, 27500, 29135, 30867, 32703, 34647, 36708, 38890, 41203, 43653, 46249, 48999, 51913,
    55000, 58270, 61735, 65406, 69295, 73416, 77781, 82406, 87307, 92498, 97998, 103826, 110000, 116540, 123470,
    130812, 138591, 146832, 155563, 164813, 174614, 184997, 195997, 207652, 220000, 233081, 246941, 261625, 277182,
    293664, 311126, 329627, 349228, 369994, 391995, 415304, 440000, 466163, 493883, 523251, 554365, 587329, 622253,
    659255, 698456, 739988, 783990, 830609, 880000, 932327, 987766, 1046502, 1108730, 1174659, 1244507, 1318510,
    1396912, 1479977, 1567981, 1661218, 1760000, 1864655, 1975533, 2093004, 2217461, 2349318, 2489015, 2637020,
    2793825, 2959955, 3135963, 3322437, 3520000, 3729310, 3951066, 4186009, 4434922, 4698636, 4978031, 5274040,
    5587651, 5919910, 6271926, 6644875, 7040000, 7458620, 7902132, 8372018, 8869844, 9397272, 9956063, 10548081,
    11175303, 11839821, 0, 8};
#define GF1_VOICES 28
#define LFO_HZ (1.0 / (69 * 320e-6))   /* GF1 timer 2 at 187: 45.29 Hz (0x4c21; 0xb0e9 = 45289 in 0x3933) */
#define MUL32(a, b) ((int32_t)((uint32_t)(a) * (uint32_t)(b)))   /* the driver's 32-bit imul (wraps) */

static int voice_vol(const gus_song *s, int ch, int vel)   /* +0x11 */
{
    int cv = s->vol[ch] * s->expr[ch] / 127;
    if (cv < 1) cv = 1;   /* 0x3586 keeps the channel's level within 1..127 */
    if (cv > 127) cv = 127;
    return (127 + 64) * VOL_T[vel] * VOL_T[cv] / 48705;
}
static int lvl8(const voice *v, int off)   /* an envelope offset scaled by the voice's volume, at least 5 */
{
    int t = off * v->v11 / 255;
    return t < 5 ? 5 : t;
}

/* ---- pitch ---- */
static int32_t note_mhz(const gus_sample *sm, int note)   /* 0x3f6c / 0x3c97 */
{
    int32_t n, frac, a, b;
    if (sm->scale_factor == 0x400) return NOTE_T[note];
    n = MUL32(note - sm->scale_freq, sm->scale_factor);
    frac = n & 0x3ff;
    n = (n >> 10) + sm->scale_freq;
    if (n < 0) n = 0;
    if (n >= 128) return NOTE_T[127];
    a = NOTE_T[n]; b = NOTE_T[n + 1];
    if (n >= 0x5f) {   /* (the driver shifts only the lower entry down by 5 here) */
        a >>= 5;
        return (int32_t)((uint32_t)((MUL32(b - a, frac) >> 10) + a) << 5);
    }
    return (MUL32(b - a, frac) >> 10) + a;
}
static int fc_of(const gus_sample *sm, int32_t f)   /* 0x3c49: the GF1 frequency control (FC / 512 samples a frame) */
{
    uint32_t t = (((uint32_t)f << 9) + (uint32_t)(sm->root >> 1)) / (uint32_t)sm->root;
    return (uint16_t)(((t << 9) + (sm->r24 >> 1)) / sm->r24);
}
static int bend_factor(int range, int lsb, int msb)   /* 0x4c87 */
{
    int32_t p = range * ((msb << 7) + lsb - 0x2000);
    uint32_t a, b, f;
    int neg = p < 0, c, rem;
    if (!range) return 0x400;
    if (neg) p = -p;
    c = p >> 13; rem = p % 0x2000;
    a = SEMI_T[c % 12] << (c / 12); b = SEMI_T[(c + 1) % 12] << ((c + 1) / 12);
    f = (uint16_t)((((b - a) * (uint32_t)rem) >> 13) + a);
    if (neg) f = (uint16_t)(0x100000 / f);
    return (int)f;
}
static void vib_setup(gus_song *s, voice *v, int start)   /* 0x3933 */
{
    const gus_sample *sm = v->s;
    int depth = sm->vib_depth, rate = sm->vib_rate, sweep = sm->vib_sweep, fm, q, a;
    uint32_t ra, rb, ratio;
    if (s->mod_depth[v->ch]) { depth = s->mod_depth[v->ch]; rate = s->mod_rate[v->ch]; sweep = 0; }
    fm = (rate * 2334 + 5000) / 100;
    q = (uint8_t)(45289 / (4 * fm));
    if ((uint32_t)(45289 % (4 * fm)) > (uint32_t)(2 * fm)) q++;
    v->vib_quarter = q < 1 ? 1 : q;
    a = depth / 21;
    ra = SEMI_T[a % 12] << (a / 12); rb = SEMI_T[(a + 1) % 12] << ((a + 1) / 12);
    ratio = (rb - ra) * (uint32_t)(depth % 21) / 21 + ra;
    v->vib_dev = (uint16_t)((((ratio * (uint32_t)v->fc) >> 10) - (uint32_t)v->fc) >> 1);
    if (start) { v->vib_sweep = sweep; v->vib_cnt = 0; v->vib_q = 0; }
}
static void vib_tick(gus_song *s, voice *v)   /* 0x3812: one timer tick - FC + the triangle's offset */
{
    const gus_sample *sm = v->s;
    int cx;
    if (!sm->vib_depth && !s->mod_depth[v->ch]) return;   /* no vibrato: the FC last written stays */
    cx = (int16_t)(uint16_t)((uint32_t)v->vib_dev * (uint32_t)v->vib_cnt / (uint32_t)v->vib_quarter);
    if (v->vib_q == 1) cx = (int16_t)(v->vib_dev - cx);
    else if (v->vib_q == 2) cx = (int16_t)-cx;
    else if (v->vib_q == 3) cx = (int16_t)-(v->vib_dev - cx);
    if (v->vib_sweep && sm->vib_sweep) { cx = cx * (sm->vib_sweep - v->vib_sweep) / sm->vib_sweep; v->vib_sweep--; }
    v->fc_out = (uint16_t)(v->fc + cx);
    if (++v->vib_cnt >= v->vib_quarter) { v->vib_q = (v->vib_q + 1) % 4; v->vib_cnt = 0; }
}
static void apply_bend(gus_song *s, int ch, int bf)   /* 0x35e7 */
{
    int k;
    for (k = 0; k < MAXV; k++) {
        voice *v = &s->v[k];
        if (!v->active || v->ch != ch) continue;
        v->fc = (uint16_t)(((uint32_t)v->fc_base * (uint32_t)bf) >> 10);
        if (v->s->vib_depth || s->mod_depth[ch]) vib_setup(s, v, 1);   /* 0x3644 */
        v->fc_out = v->fc;
    }
    s->bendf[ch] = bf;
}

/* ---- volume ---- */
static void trem_setup(voice *v)   /* 0x3acc: the looping ramp over the sustain level */
{
    const gus_sample *sm = v->s;
    int w = (sm->trem_depth + 1) >> 3, h = w >> 1, div;
    uint32_t L = (uint32_t)(sm->env_off[2] * v->v11 / 255), span, per, inc;
    if (L - (uint32_t)h < 5) L = (uint32_t)h + 5;   /* (unsigned in the driver) */
    if (L + (uint32_t)(h + (w & 1)) > 251) L = (uint32_t)(251 - h - (w & 1));
    v->trem_lo8 = (uint8_t)(L - (uint32_t)h); v->trem_hi8 = (uint8_t)(L + (uint32_t)(h + (w & 1)));
    span = (uint32_t)((int16_t)((v->trem_hi8 - v->trem_lo8) << 4)) * (GF1_VOICES << 4);
    per = (uint32_t)(5000000 / (sm->trem_rate * 2334 + 5000)) * 100000u;
    for (div = 0; div <= 3 && span <= per; div++) span *= 8;
    if (div > 3) div = 3;
    inc = (uint8_t)((span + per / 2) / per);
    v->trem_rate = (int)(inc > 63 ? 63 : inc) | div << 6;
}
static void ramp_end(voice *v)   /* 0x4787: the volume ramp's end interrupt */
{
    const gus_sample *sm = v->s;
    int cur8 = (int)v->env >> 4, resumed = v->vc != 0, cx;
    v->r_run = 0;
    if (v->vc) { v->stage = v->vc - 1; v->vc = 0; } else v->stage++;
    if (v->stage >= 6) {   /* 0x4804: the envelope's end - the level stays, the loop off, the sample plays to its end */
        if (!resumed) v->start8 = v->tgt8;
        v->held = 0; v->released = 1; v->playout = 1;
        return;
    }
    if (v->stage == 3 && (sm->modes & 0x20) && !v->released) {   /* 0x4886: the sustain point holds */
        v->held = 1;
        if (!resumed) v->start8 = v->tgt8;
        v->rel8 = sm->env_off[2];
        if (sm->trem_depth) {   /* 0x48c2: the tremolo's loop (its lower bound, 8-bit, against the 12-bit level) */
            v->r_dir = v->trem_lo8 < (int)v->env ? -1 : 1;
            v->r_rate = v->trem_rate; v->r_lo = v->trem_lo8 * 16.0; v->r_hi = v->trem_hi8 * 16.0;
            v->r_loop = 1; v->r_run = 1;
        }
        return;
    }
    v->r_rate = sm->env_rate[v->stage];   /* 0x4936 */
    if (resumed) cx = v->tgt8;   /* after a volume change: on to the stage's new target */
    else { v->start8 = v->tgt8; cx = lvl8(v, sm->env_off[v->stage]); v->tgt8 = cx; }
    if (cur8 >= cx) { v->r_dir = -1; v->r_lo = cx * 16.0; }
    else { v->r_dir = 1; v->r_hi = cx * 16.0; }
    v->r_run = 1;
}
/* advance the GF1 volume ramp one output sample */
static void envelope(voice *v, int outrate)
{
    double inc;
    if (!v->r_run) return;
    inc = (v->r_rate & 63) / pow(8.0, v->r_rate >> 6) * ((double)GF1_FRAME / outrate);
    if (inc <= 0) return;   /* increment 0: the ramp does not move */
    if (v->r_dir > 0) {
        v->env += inc;
        if (v->env >= v->r_hi) {
            if (v->r_loop) { v->env = v->r_hi - (v->env - v->r_hi); v->r_dir = -1; if (v->env < v->r_lo) v->env = v->r_lo; }
            else { v->env = v->r_hi; ramp_end(v); }
        }
    } else {
        v->env -= inc;
        if (v->env <= v->r_lo) {
            if (v->r_loop) { v->env = v->r_lo + (v->r_lo - v->env); v->r_dir = 1; if (v->env > v->r_hi) v->env = v->r_hi; }
            else { v->env = v->r_lo; ramp_end(v); }
        }
    }
}

static void note_on(gus_song *s, int ch, int note, int vel, uint32_t dur)
{
    gus_patch *p = get_patch(s->bank, ch == 9 ? 128 + note : s->prog[ch]);
    const gus_sample *sm = NULL;
    int32_t f;
    int i, k, slot = -1;
    if (!p || vel == 0 || note < 0 || note > 127) return;
    f = note_mhz(&p->s[0], note);   /* the scale from the layer's first sample (0x3f69) */
    for (i = 0; i < p->n; i++) {   /* 0x4034: the sample whose low..high holds the frequency (the last of a run) */
        const gus_sample *c = &p->s[i];
        if (c->high < f || c->low > f) continue;
        sm = c;
        if (i + 1 < p->n && !(p->s[i + 1].high < f || p->s[i + 1].low > f)) continue;
        break;
    }
    if (!sm || sm->len < 2 || sm->root <= 0 || !sm->r24) return;   /* none: no note */
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
        v->fc_base = fc_of(sm, f);   /* 0x4152 */
        v->fc = (uint16_t)(((uint32_t)v->fc_base * (uint32_t)s->bendf[ch]) >> 10);   /* 0x415b */
        v->fc_out = v->fc;
        v->vel = vel < 1 ? 1 : vel > 127 ? 127 : vel;
        v->v11 = voice_vol(s, ch, v->vel);
        v->env = 5 * 16.0;   /* stage 0: from 5 up to its level (0x4276..0x42c8) */
        v->tgt8 = lvl8(v, sm->env_off[0]);
        v->r_rate = sm->env_rate[0]; v->r_lo = 5 * 16.0; v->r_hi = v->tgt8 * 16.0; v->r_dir = 1; v->r_run = 1;
        if (sm->vib_depth || s->mod_depth[ch]) vib_setup(s, v, 1);   /* 0x42ec */
        if (sm->trem_depth) trem_setup(v);   /* 0x4316 */
        v->started = s->t;
        v->off_at = s->t + (uint64_t)((double)dur * s->spt) + 1;
    }
}

static void release(voice *v)   /* note-off (0x4488..0x46ab) */
{
    const gus_sample *sm = v->s;
    int cur8;
    if (!v->active || v->released) return;
    v->held = 0; v->released = 1;
    if (!(sm->modes & 0x40)) v->playout = 1;   /* 0x449b: no envelope - the loop off, the sample plays to its end */
    cur8 = (int)v->env >> 4;
    if (!(sm->modes & 0x40) || (sm->modes & 0x80)) {   /* 0x4517: stage 5's rate down to 5 */
        v->stage = 5;
        if (!v->rel8) v->rel8 = (uint8_t)(cur8 * 255 / v->v11);
        v->start8 = cur8; v->tgt8 = 5;
        v->r_rate = sm->env_rate[5]; v->r_dir = -1; v->r_lo = 5 * 16.0; v->r_loop = 0; v->r_run = 1;
    } else if (sm->modes & 0x20) {   /* 0x4593: stage 3 from the current level */
        int t = lvl8(v, sm->env_off[3]);
        if (v->stage < 3) v->stage = 3;
        if (!v->rel8) v->rel8 = (uint8_t)(cur8 * 255 / v->v11);
        v->start8 = cur8; v->tgt8 = t;
        v->r_rate = sm->env_rate[3]; v->r_loop = 0;
        if (sm->env_off[3] > cur8) { v->r_dir = 1; v->r_lo = cur8 * 16.0; v->r_hi = t * 16.0; }   /* (unscaled offset) */
        else { v->r_dir = -1; v->r_lo = t * 16.0; v->r_hi = cur8 * 16.0; }
        v->r_run = 1;
    }
}

static void vol_change(gus_song *s, voice *v)   /* 0x3260: CC7 / CC11 on a sounding voice */
{
    const gus_sample *sm = v->s;
    int st = v->stage, cur12 = (int)v->env, cur8, S, T, cx;
    v->v11 = voice_vol(s, v->ch, v->vel);
    if (sm->trem_depth) trem_setup(v);   /* 0x32cc */
    v->r_run = 0; v->r_loop = 0;   /* the ramp stopped, its loop bits cleared (0x32da) */
    if (!v->vc) {   /* how far the stage's ramp had got */
        if (v->tgt8 == v->start8 || (st == 3 && (sm->modes & 0x20) && v->held)) v->prog = 0x400;
        else {
            int32_t p = (cur12 - v->start8 * 16) * 1024 / ((v->tgt8 - v->start8) * 16);
            if (p < 0) p = -p;
            else if (p == 0) p = 0x400;
            v->prog = p > 0x400 ? 0x400 : p;
        }
    }
    if (st == 0) { S = 0; T = sm->env_off[0] * v->v11 / 255; }   /* the stage's start / target with the new volume */
    else if (st >= 6) S = T = sm->env_off[5] * v->v11 / 255;
    else if (st == 3 && (sm->modes & 0x20)) {
        S = v->rel8 * v->v11 / 255;
        T = v->held ? S : sm->env_off[3] * v->v11 / 255;
    } else { T = sm->env_off[st] * v->v11 / 255; S = sm->env_off[st - 1] * v->v11 / 255; }
    if (S < 5) S = 5;
    if (T < 5) T = 5;
    cx = ((T - S) * v->prog + 0x200) / 0x400 + S;
    if (cx > 251) cx = 251;
    else if (cx < 5) cx = 5;
    cur8 = cur12 >> 4;
    if (cx < cur8) { v->r_dir = -1; v->r_lo = cx * 16.0; v->r_hi = cur8 * 16.0; }
    else if (cx > cur8) {
        if (st > 3) cx = cur8;   /* no ramp up in the release stages */
        else { v->r_dir = 1; v->r_lo = cur8 * 16.0; v->r_hi = cx * 16.0; }
    }
    if (cx != cur8) { v->tgt8 = T; v->start8 = S; v->r_rate = 0x43; }   /* else the ramp goes on as it was */
    v->vc = st + 1;
    v->r_run = 1;
}

static void set_volume(gus_song *s, int ch)   /* CC7 / CC11 (0x4e64 / 0x4e98 -> 0x3586) */
{
    int k;
    for (k = 0; k < MAXV; k++) if (s->v[k].active && s->v[k].ch == ch) vol_change(s, &s->v[k]);
}

static void do_event(gus_song *s, const ev *e)
{
    int hi = e->st & 0xf0, ch = e->st & 15, k;
    switch (hi) {
    case 0x90: note_on(s, ch, e->a, e->b, e->dur); break;
    case 0x80: for (k = 0; k < MAXV; k++) if (s->v[k].active && s->v[k].ch == ch && s->v[k].note == e->a) release(&s->v[k]); break;
    case 0xc0: s->prog[ch] = e->a; break;
    case 0xe0: apply_bend(s, ch, bend_factor(s->brange[ch], e->a, e->b)); break;   /* 0x4c87 */
    case 0xb0:
        switch (e->a) {   /* the controllers 0x4f39 */
        case 1:   /* modulation (0x36de): the channel's vibrato */
            s->mod_depth[ch] = e->b ? e->b * 30 / 128 + 2 : 0; s->mod_rate[ch] = 200;
            for (k = 0; k < MAXV; k++) if (s->v[k].active && s->v[k].ch == ch && (s->v[k].s->vib_depth || s->mod_depth[ch])) vib_setup(s, &s->v[k], e->b == 0);
            break;
        case 6:   /* data entry (0x4dc0): RPN 0 / 0 = the bend range, at most 24 */
            if (!s->rpn_msb[ch] && !s->rpn_lsb[ch]) s->brange[ch] = e->b > 24 ? 24 : e->b;
            break;
        case 7: s->vol[ch] = e->b; set_volume(s, ch); break;
        case 10: s->pan[ch] = e->b; s->pan_set[ch] = 1; break;
        case 11: s->expr[ch] = e->b; set_volume(s, ch); break;
        case 64: s->sustain[ch] = e->b >= 24; break;   /* 0x4ec7 */
        case 100: s->rpn_lsb[ch] = e->b; break;   /* 0x4edb */
        case 101: s->rpn_msb[ch] = e->b; break;   /* 0x4ee5 */
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
        case 121:   /* reset all controllers (0x4eef): sustain off, no modulation, bend centred, volume 100, expression 127,
                     * pan the patch's (the RPN and the bend range stay) */
            s->sustain[ch] = 0; s->mod_depth[ch] = 0; s->vol[ch] = 100; s->expr[ch] = 127; s->pan_set[ch] = 0;
            apply_bend(s, ch, 0x400);
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
            for (k = 0; k < MAXV; k++) if (s->v[k].active) vib_tick(s, &s->v[k]);
        }
        for (k = 0; k < MAXV; k++) {
            voice *v = &s->v[k];
            const gus_sample *sm;
            double a, frac, smp, amp, pan;
            int i0;
            if (!v->active) continue;
            sm = v->s;
            if (!v->released && v->off_at && s->t >= v->off_at && !s->sustain[v->ch]) release(v);
            envelope(v, s->rate);
            i0 = (int)v->pos; frac = v->pos - i0;
            if (i0 < 0 || i0 >= sm->len) { v->active = 0; continue; }
            smp = sm->data[i0] + (sm->data[i0 + 1] - sm->data[i0]) * frac;   /* GF1 linear interpolation */
            amp = gf1_amp(v->env);   /* velocity and volume are in the envelope's levels (v11) */
            pan = s->pan_set[v->ch] ? s->pan[v->ch] / 127.0 : sm->balance / 15.0;
            a = smp * amp * 0.35;
            out[f * 2] += (float)(a * cos(pan * 1.5707963));
            out[f * 2 + 1] += (float)(a * sin(pan * 1.5707963));
            /* advance: FC / 512 samples a GF1 frame (register 1 holds FC << 1), with loops */
            v->pos += (uint16_t)(v->fc_out << 1) / 1024.0 * ((double)GF1_FRAME / s->rate) * v->dir;
            if ((sm->modes & 0x04) && !v->playout) {
                if (v->dir > 0 && v->pos >= sm->loop_end) {
                    if (sm->modes & 0x08) { v->dir = -1; v->pos = sm->loop_end - (v->pos - sm->loop_end); }
                    else v->pos -= sm->loop_end - sm->loop_start;
                } else if (v->dir < 0 && v->pos <= sm->loop_start) { v->dir = 1; v->pos = sm->loop_start + (sm->loop_start - v->pos); }
            } else if (v->dir > 0 ? v->pos >= sm->len - 1 : v->pos <= sm->loop_start)
                v->active = 0;   /* the sample's end (0x4729 frees the voice; a bidirectional loop's backward pass stops
                                  * at its start, the boundary then) */
        }
        s->t++;
    }
    if (s->ended) { for (k = 0; k < MAXV; k++) if (s->v[k].active) return 1; return 0; }
    return 1;
}
