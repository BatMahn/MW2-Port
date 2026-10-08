/*
 * mt32mid.c - XMIDI music played on an emulated Roland MT-32 (Munt's mt32emu, LGPL, third_party/mt32emu),
 * as MW2's Miles driver MT32MPU.MDI did with the MT-32 song set (DATABASE entries 57-64). The songs use the
 * MT-32's built-in timbres (TIMB: bank 0) and rhythm keys (bank 127), so no timbre library is uploaded.
 * The user supplies the MT-32 ROMs (MT32_CONTROL.ROM, MT32_PCM.ROM).
 */
#include "mt32mid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "c_interface/c_interface.h"

struct mt32_synth_s {
    mt32emu_context ctx;
    int rate;
};

typedef struct { uint32_t tick; uint8_t st, a, b; uint32_t dur; } mev;
typedef struct { double at; uint8_t ch, note; } noteoff;

struct mt32_song_s {
    mt32_synth *syn;
    mev *ev; int nev, pos;
    double spt;               /* samples per tick (120 Hz) */
    uint64_t t;               /* samples rendered */
    double tick_base;
    noteoff off[256]; int noff;
    int loop_pos[4], loop_cnt[4]; double loop_tick[4]; int nloop;
    int ended;
};

/* a quiet report handler: the library would print "Rhythm: Attempted to play unmapped key ..." and the like to
 * stdout; only the version query is answered */
static mt32emu_report_handler_version MT32EMU_C_CALL rh_version(mt32emu_report_handler_i i) { (void)i; return MT32EMU_REPORT_HANDLER_VERSION_0; }
static void MT32EMU_C_CALL rh_debug(void *d, const char *fmt, va_list l) { (void)d; (void)fmt; (void)l; }
static mt32emu_report_handler_i_v0 rh_v0;
static int exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) { fclose(f); return 1; } return 0; }

mt32_synth *mt32_open(const char *rom_dir, int rate)
{
    static const char *const CTL[] = {"MT32_CONTROL.ROM", "mt32_control.rom", "CM32L_CONTROL.ROM", "cm32l_control.rom", NULL};
    static const char *const PCM[] = {"MT32_PCM.ROM", "mt32_pcm.rom", "CM32L_PCM.ROM", "cm32l_pcm.rom", NULL};
    char a[1200], b[1200];
    int i, ok_c = 0, ok_p = 0;
    mt32_synth *s;
    mt32emu_report_handler_i none;
    if (!rom_dir || !rom_dir[0]) return NULL;
    for (i = 0; CTL[i] && !ok_c; i++) { snprintf(a, sizeof a, "%s/%s", rom_dir, CTL[i]); ok_c = exists(a); }
    for (i = 0; PCM[i] && !ok_p; i++) { snprintf(b, sizeof b, "%s/%s", rom_dir, PCM[i]); ok_p = exists(b); }
    if (!ok_c || !ok_p) return NULL;
    s = calloc(1, sizeof *s);
    if (!s) return NULL;
    memset(&rh_v0, 0, sizeof rh_v0);
    rh_v0.getVersionID = rh_version;
    rh_v0.printDebug = rh_debug;
    none.v0 = &rh_v0;
    s->ctx = mt32emu_create_context(none, NULL);
    if (!s->ctx) { free(s); return NULL; }
    if (mt32emu_add_rom_file(s->ctx, a) < 0 || mt32emu_add_rom_file(s->ctx, b) < 0) { mt32emu_free_context(s->ctx); free(s); return NULL; }
    mt32emu_set_stereo_output_samplerate(s->ctx, (double)rate);
    if (mt32emu_open_synth(s->ctx) != MT32EMU_RC_OK) { mt32emu_free_context(s->ctx); free(s); return NULL; }
    s->rate = rate;
    return s;
}

void mt32_close(mt32_synth *s)
{
    if (!s) return;
    mt32emu_close_synth(s->ctx);
    mt32emu_free_context(s->ctx);
    free(s);
}

static uint32_t vlq(const uint8_t *d, size_t len, size_t *i)
{
    uint32_t v = 0;
    while (*i < len) { uint8_t c = d[(*i)++]; v = v << 7 | (c & 0x7f); if (!(c & 0x80)) break; }
    return v;
}

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

mt32_song *mt32_song_open(mt32_synth *syn, const uint8_t *xmi, size_t len)
{
    size_t el, i = 0;
    const uint8_t *d = find_chunk(xmi, len, "EVNT", &el);
    uint32_t tick = 0;
    int cap = 1024;
    mt32_song *s;
    if (!syn || !d) return NULL;
    s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->syn = syn; s->spt = syn->rate / 120.0;
    s->ev = malloc((size_t)cap * sizeof *s->ev);
    if (!s->ev) { free(s); return NULL; }
    while (i < el) {
        uint8_t c = d[i];
        mev e;
        if (c < 0x80) { while (i < el && d[i] < 0x80) tick += d[i++]; continue; }   /* XMI interval counts */
        i++;
        memset(&e, 0, sizeof e);
        e.tick = tick; e.st = c;
        if (c == 0xff) {
            uint8_t type = i < el ? d[i++] : 0;
            uint32_t l = vlq(d, el, &i);
            i += l;
            if (type != 0x2f) continue;
            e.a = 0x2f;
        } else if (c == 0xf0 || c == 0xf7) { uint32_t l = vlq(d, el, &i); i += l; continue; }
        else {
            int hi = c & 0xf0;
            e.a = i < el ? d[i++] : 0;
            if (hi != 0xc0 && hi != 0xd0) e.b = i < el ? d[i++] : 0;
            if (hi == 0x90) e.dur = vlq(d, el, &i);
        }
        if (s->nev == cap) { mev *g = realloc(s->ev, (size_t)cap * 2 * sizeof *s->ev); if (!g) break; s->ev = g; cap *= 2; }
        s->ev[s->nev++] = e;
        if (c == 0xff) break;
    }
    if (s->nev <= 0) { mt32_song_close(s); return NULL; }
    /* a clean start: all notes off, controllers reset, on every part */
    for (i = 0; i < 16; i++) {
        mt32emu_play_msg_now(syn->ctx, (uint32_t)(0xb0 | i) | 121u << 8);
        mt32emu_play_msg_now(syn->ctx, (uint32_t)(0xb0 | i) | 123u << 8);
    }
    return s;
}

void mt32_song_close(mt32_song *s)
{
    int k;
    if (!s) return;
    for (k = 0; k < 16; k++) mt32emu_play_msg_now(s->syn->ctx, (uint32_t)(0xb0 | k) | 123u << 8);   /* all notes off */
    free(s->ev);
    free(s);
}

static void send(mt32_song *s, uint8_t st, uint8_t a, uint8_t b)
{
    mt32emu_play_msg_now(s->syn->ctx, (uint32_t)st | (uint32_t)a << 8 | (uint32_t)b << 16);
}

static void do_event(mt32_song *s, const mev *e)
{
    int hi = e->st & 0xf0;
    if (e->st == 0xff) { if (e->a == 0x2f) s->ended = 1; return; }
    if (hi == 0xb0 && e->a == 116) {   /* AIL FOR loop: value = count (0 = forever) */
        if (s->nloop < 4) { s->loop_pos[s->nloop] = s->pos; s->loop_cnt[s->nloop] = e->b; s->loop_tick[s->nloop] = e->tick; s->nloop++; }
        return;
    }
    if (hi == 0xb0 && e->a == 117) {   /* AIL NEXT */
        if (s->nloop > 0 && e->b >= 64) {
            int L = s->nloop - 1;
            if (s->loop_cnt[L] == 0 || --s->loop_cnt[L] > 0) { s->tick_base += (double)e->tick - s->loop_tick[L]; s->pos = s->loop_pos[L]; }
            else s->nloop--;
        }
        return;
    }
    send(s, e->st, e->a, e->b);
    if (hi == 0x90 && e->b > 0 && s->noff < 256) {   /* XMI: the note's duration replaces its note-off */
        s->off[s->noff].at = (double)s->t + (double)e->dur * s->spt;
        s->off[s->noff].ch = e->st & 15; s->off[s->noff].note = e->a;
        s->noff++;
    }
}

int mt32_song_render(mt32_song *s, float *out, int frames)
{
    float buf[2048];
    int done = 0, k;
    while (done < frames) {
        int n = frames - done > 1024 ? 1024 : frames - done;
        int f;
        /* events up to this block's start (120 Hz ticks, finer than a block at 44.1 kHz) */
        double now_tick = (double)s->t / s->spt;
        while (!s->ended && s->pos < s->nev && (double)s->ev[s->pos].tick + s->tick_base <= now_tick) {
            const mev *e = &s->ev[s->pos++];
            do_event(s, e);
        }
        if (s->pos >= s->nev) s->ended = 1;
        for (k = 0; k < s->noff;) {
            if (s->off[k].at <= (double)s->t) { send(s, (uint8_t)(0x80 | s->off[k].ch), s->off[k].note, 0); s->off[k] = s->off[--s->noff]; }
            else k++;
        }
        if (n > (int)(s->spt) && s->spt >= 1) n = (int)s->spt;   /* one tick at a time */
        if (n < 1) n = 1;
        mt32emu_render_float(s->syn->ctx, buf, (mt32emu_bit32u)n);
        for (f = 0; f < n * 2; f++) out[done * 2 + f] += buf[f];
        done += n;
        s->t += (uint64_t)n;
    }
    return !(s->ended && s->noff == 0);
}
