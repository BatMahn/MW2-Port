/*
 * sc55mid.c - XMIDI music played on an emulated Roland SC-55 (Nuked SC-55, GPL 2.0+, third_party/nuked-sc55), as MW2's
 * Miles driver MPU401.MDI sent it to a Sound Canvas: the General MIDI song set (DATABASE entry = song number + 32, the
 * same set as ULTRA.MDI's - shell table 0x83e14). MPU401.MDI itself only resets the MPU-401 (command 0xFF) and puts it
 * in UART mode (0x3F); the songs carry no SysEx. The port gives the unit a GS reset once it has booted (upstream's
 * README: the SC-55mk2 firmware leaves some parameters unset at power-on until a GM / GS reset).
 *
 * The emulator runs the unit's two microcontrollers and PCM chip cycle by cycle (~24 MHz) and needs most of a modern CPU
 * core, so it runs on its own thread: the thread keeps a short queue of resampled audio (SDL_AudioStream: the unit's
 * 66207 / 64000 Hz to the mixer's rate) ahead of the mixer, plays the song's events on the unit's own clock, and goes
 * idle once nothing plays and the sound has died away. The mixer only takes from the queue (never waits).
 */
#include "sc55mid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

#include "sdlcompat.h"
#include "nuked-sc55/sc55_core.h"

#define SC55_BOOT_MS   1000   /* emulated time before the GS reset (ASSUMED: the firmware's start-up; no ROMs to measure) */
#define SC55_RESET_MS  250    /* and after it, before the first song (a GS reset takes the unit ~50 ms; margin) */
#define SC55_IDLE_MS   3000   /* after a stop: the reverb / release tails, then the thread sleeps */
#define SC55_QUEUE     6144   /* mixer-rate frames kept queued (~140 ms at 44.1 kHz) */

typedef struct { uint32_t tick; uint8_t st, a, b; uint32_t dur; } mev;
typedef struct { uint64_t at; uint8_t ch, note; } noteoff;

struct sc55_synth_s {
    SDL_Thread *th;
    SDL_Mutex *mx;
    SDL_Condition *cv;
    SDL_AudioStream *st;
    int rate, nrate, model;
    char name[32];
    SDL_AtomicInt quit, booted, playing;
    SDL_AtomicInt underrun;
    /* the command for the thread (under mx): 1 play pend, 2 stop */
    int cmd;
    uint8_t *pend; size_t pend_len;
    /* the thread's own: the song */
    mev *ev; int nev, pos;
    double spt;               /* unit samples per tick (120 Hz) */
    uint64_t t;               /* unit samples since the song started */
    double tick_base;
    noteoff off[256]; int noff;
    int loop_pos[4], loop_cnt[4]; double loop_tick[4]; int nloop;
    int ended, active;
    uint64_t idle;            /* unit samples rendered since the last stop */
    uint64_t emu_frames;      /* statistics */
    double busy;
};

/* ---- the ROM set ---- */

static int find_ci(const char *dir, const char *name, char *out, size_t n)
{
    DIR *d;
    struct dirent *e;
    FILE *f;
    snprintf(out, n, "%s/%s", dir, name);
    if ((f = fopen(out, "rb"))) { fclose(f); return 1; }
    if (!(d = opendir(dir))) return 0;
    while ((e = readdir(d)))
        if (strcasecmp(e->d_name, name) == 0) { snprintf(out, n, "%s/%s", dir, e->d_name); closedir(d); return 1; }
    closedir(d);
    return 0;
}

/* upstream's autodetect: the first model whose files (the first five names) are all there; General MIDI models only */
static int detect(const char *dir, char paths[NSC55_FILES][1100])
{
    int m, i;
    if (!dir || !dir[0]) return -1;
    for (m = 0; m < nsc55_model_count(); m++) {
        int ok = nsc55_model_is_gm(m);
        for (i = 0; i < NSC55_FILES; i++) {
            const char *f = nsc55_rom_name(m, i);
            paths[i][0] = 0;
            if (!f[0]) continue;
            if (!find_ci(dir, f, paths[i], sizeof paths[i])) { paths[i][0] = 0; if (i < 5) ok = 0; }
        }
        if (ok) return m;
    }
    return -1;
}

int sc55_detect(const char *rom_dir, char *model, size_t n)
{
    static char paths[NSC55_FILES][1100];
    int m = detect(rom_dir, paths);
    if (model && n) snprintf(model, n, "%s", m >= 0 ? nsc55_model_name(m) : "");
    return m;
}

/* ---- the song (on the thread) ---- */

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

/* XMIDI sequence 0's events (as mt32mid.c: interval counts, notes with their durations, AIL loop controllers 116/117) */
static int parse(sc55_synth *s, const uint8_t *xmi, size_t len)
{
    size_t el, i = 0;
    const uint8_t *d = find_chunk(xmi, len, "EVNT", &el);
    uint32_t tick = 0;
    int cap = 1024;
    free(s->ev); s->ev = NULL; s->nev = 0;
    if (!d) return -1;
    s->ev = malloc((size_t)cap * sizeof *s->ev);
    if (!s->ev) return -1;
    while (i < el) {
        uint8_t c = d[i];
        mev e;
        if (c < 0x80) { while (i < el && d[i] < 0x80) tick += d[i++]; continue; }
        i++;
        memset(&e, 0, sizeof e);
        e.tick = tick; e.st = c;
        if (c == 0xff) {
            uint8_t type = i < el ? d[i++] : 0;
            uint32_t l = vlq(d, el, &i);
            i += l;
            if (type != 0x2f) continue;
            e.a = 0x2f;
        } else if (c == 0xf0 || c == 0xf7) { uint32_t l = vlq(d, el, &i); i += l; continue; }   /* (none in the songs) */
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
    return s->nev > 0 ? 0 : -1;
}

static void send(uint8_t st, uint8_t a, uint8_t b)
{
    uint8_t m[3];
    int hi = st & 0xf0;
    m[0] = st; m[1] = a; m[2] = b;
    nsc55_midi(m, hi == 0xc0 || hi == 0xd0 ? 2 : 3);
}

static void notes_off(sc55_synth *s)
{
    int k;
    for (k = 0; k < 16; k++) { send((uint8_t)(0xb0 | k), 121, 0); send((uint8_t)(0xb0 | k), 123, 0); }   /* reset controllers, all notes off */
    s->noff = 0;
}

static void song_start(sc55_synth *s)
{
    s->pos = 0; s->t = 0; s->tick_base = 0; s->nloop = 0; s->ended = 0;
    notes_off(s);
}

static void do_event(sc55_synth *s, const mev *e)
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
    send(e->st, e->a, e->b);
    if (hi == 0x90 && e->b > 0 && s->noff < 256) {   /* XMI: the note's duration replaces its note-off */
        s->off[s->noff].at = s->t + (uint64_t)((double)e->dur * s->spt);
        s->off[s->noff].ch = e->st & 15; s->off[s->noff].note = e->a;
        s->noff++;
    }
}

/* the song's events up to now (one tick's block at a time) */
static void song_events(sc55_synth *s)
{
    double now_tick = (double)s->t / s->spt;
    int k;
    while (!s->ended && s->pos < s->nev && (double)s->ev[s->pos].tick + s->tick_base <= now_tick) {
        const mev *e = &s->ev[s->pos++];
        do_event(s, e);
    }
    if (s->pos >= s->nev) s->ended = 1;
    for (k = 0; k < s->noff;) {
        if (s->off[k].at <= s->t) { send((uint8_t)(0x80 | s->off[k].ch), s->off[k].note, 0); s->off[k] = s->off[--s->noff]; }
        else k++;
    }
    if (s->ended && s->noff == 0) song_start(s);   /* the menu music loops (as the MT-32 / GUS paths) */
}

/* run the unit for n samples; to the queue unless discarded (boot) */
static void emulate(sc55_synth *s, int16_t *buf, int n, int keep)
{
    uint64_t t0 = SDL_GetPerformanceCounter();
    nsc55_render(buf, n);
    s->busy += (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency();
    s->emu_frames += (uint64_t)n;
    if (keep) SDL_PutAudioStreamData(s->st, buf, n * 4);
}

static int SDLCALL worker(void *u)
{
    sc55_synth *s = u;
    static int16_t buf[2 * 4096];
    int chunk = s->nrate / 120, boot;
    if (chunk > 4096) chunk = 4096;
    for (boot = s->nrate * SC55_BOOT_MS / 1000; boot > 0 && !SDL_GetAtomicInt(&s->quit); boot -= chunk) emulate(s, buf, chunk, 0);
    {   /* GS reset (Roland: F0 41 10 42 12 40 00 7F 00 41 F7) */
        static const uint8_t gs[] = {0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7};
        nsc55_midi(gs, (int)sizeof gs);
    }
    for (boot = s->nrate * SC55_RESET_MS / 1000; boot > 0 && !SDL_GetAtomicInt(&s->quit); boot -= chunk) emulate(s, buf, chunk, 0);
    SDL_SetAtomicInt(&s->booted, 1);
    SDL_LockMutex(s->mx);
    while (!SDL_GetAtomicInt(&s->quit)) {
        int cmd = s->cmd, queued;
        if (cmd) {
            uint8_t *p = s->pend;
            size_t pl = s->pend_len;
            s->cmd = 0; s->pend = NULL; s->pend_len = 0;
            SDL_UnlockMutex(s->mx);
            notes_off(s);
            s->active = 0; s->idle = 0;
            if (cmd == 1 && p && parse(s, p, pl) == 0) { song_start(s); s->active = 1; }
            SDL_SetAtomicInt(&s->playing, s->active);
            free(p);
            SDL_LockMutex(s->mx);
            continue;
        }
        queued = SDL_GetAudioStreamAvailable(s->st) / 8;
        if (queued >= SC55_QUEUE || (!s->active && s->idle >= (uint64_t)s->nrate * SC55_IDLE_MS / 1000)) {
            SDL_WaitConditionTimeout(s->cv, s->mx, 10);
            continue;
        }
        SDL_UnlockMutex(s->mx);
        if (s->active) song_events(s);
        emulate(s, buf, chunk, 1);
        if (s->active) s->t += (uint64_t)chunk; else s->idle += (uint64_t)chunk;
        SDL_LockMutex(s->mx);
    }
    SDL_UnlockMutex(s->mx);
    return 0;
}

/* ---- the interface ---- */

static int in_use;   /* the core is one unit per process */

sc55_synth *sc55_open(const char *rom_dir, int rate)
{
    char paths[NSC55_FILES][1100];
    const char *pp[NSC55_FILES];
    SDL_AudioSpec src, dst;
    sc55_synth *s;
    int m = detect(rom_dir, paths), i;
    if (m < 0 || rate <= 0 || in_use) return NULL;
    for (i = 0; i < NSC55_FILES; i++) pp[i] = paths[i][0] ? paths[i] : NULL;
    if (nsc55_open(m, pp) != 0) return NULL;
    s = calloc(1, sizeof *s);
    if (!s) { nsc55_close(); return NULL; }
    s->model = m; s->rate = rate; s->nrate = nsc55_rate();
    snprintf(s->name, sizeof s->name, "%s", nsc55_model_name(m));
    s->spt = s->nrate / 120.0;
    SDL_zero(src); SDL_zero(dst);
    src.format = SDL_AUDIO_S16; src.channels = 2; src.freq = s->nrate;
    dst.format = SDL_AUDIO_F32; dst.channels = 2; dst.freq = rate;
    s->st = SDL_CreateAudioStream(&src, &dst);
    s->mx = SDL_CreateMutex();
    s->cv = SDL_CreateCondition();
    if (s->st && s->mx && s->cv) s->th = SDL_CreateThread(worker, "sc55", s);
    if (!s->th) {
        if (s->st) SDL_DestroyAudioStream(s->st);
        if (s->mx) SDL_DestroyMutex(s->mx);
        if (s->cv) SDL_DestroyCondition(s->cv);
        free(s);
        nsc55_close();
        return NULL;
    }
    in_use = 1;
    return s;
}

void sc55_close(sc55_synth *s)
{
    if (!s) return;
    SDL_SetAtomicInt(&s->quit, 1);
    SDL_SignalCondition(s->cv);
    SDL_WaitThread(s->th, NULL);
    SDL_DestroyAudioStream(s->st);
    SDL_DestroyMutex(s->mx);
    SDL_DestroyCondition(s->cv);
    free(s->pend);
    free(s->ev);
    free(s);
    nsc55_close();
    in_use = 0;
}

const char *sc55_model(const sc55_synth *s) { return s ? s->name : ""; }
int sc55_booted(const sc55_synth *s) { return s && SDL_GetAtomicInt((SDL_AtomicInt *)&s->booted); }

int sc55_play(sc55_synth *s, const uint8_t *xmi, size_t len)
{
    size_t el;
    uint8_t *c;
    if (!s || !xmi || !find_chunk(xmi, len, "EVNT", &el) || !(c = malloc(len))) return -1;
    memcpy(c, xmi, len);
    SDL_LockMutex(s->mx);
    free(s->pend);
    s->pend = c; s->pend_len = len; s->cmd = 1;
    SDL_SetAtomicInt(&s->playing, 1);
    SDL_UnlockMutex(s->mx);
    SDL_SignalCondition(s->cv);
    return 0;
}

void sc55_stop(sc55_synth *s)
{
    if (!s) return;
    SDL_LockMutex(s->mx);
    free(s->pend);
    s->pend = NULL; s->pend_len = 0; s->cmd = 2;
    SDL_SetAtomicInt(&s->playing, 0);
    SDL_UnlockMutex(s->mx);
    SDL_SignalCondition(s->cv);
}

void sc55_render(sc55_synth *s, float *out, int frames)
{
    float tmp[2048];
    int done = 0, f;
    if (!s) return;
    while (done < frames) {
        int n = frames - done > 1024 ? 1024 : frames - done;
        int got = SDL_GetAudioStreamData(s->st, tmp, n * 8);
        if (got <= 0) break;
        got /= 8;
        for (f = 0; f < got * 2; f++) out[done * 2 + f] += tmp[f];
        done += got;
    }
    if (done < frames && SDL_GetAtomicInt(&s->playing) && SDL_GetAtomicInt(&s->booted)) SDL_AddAtomicInt(&s->underrun, frames - done);
    SDL_SignalCondition(s->cv);
}

void sc55_stats(const sc55_synth *s, long *underrun_frames, double *emu_s, double *cpu_s)
{
    if (!s) return;
    if (underrun_frames) *underrun_frames = SDL_GetAtomicInt((SDL_AtomicInt *)&s->underrun);
    if (emu_s) *emu_s = (double)s->emu_frames / (double)s->nrate;   /* (read while the thread runs: approximate) */
    if (cpu_s) *cpu_s = s->busy;
}
