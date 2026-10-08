/* sfx.c - see sfx.h. */
#include "sfx.h"

#include <SDL.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define SFX_VOICES 24
#define SFX_CACHE  512

typedef struct { unsigned char *data; int len; Uint32 last; } clip;
typedef struct { const clip *c; double pos, step; float l, r; int loop, is_voice, index, prio; clip own; } voice;
/* the engine's sound table (0x1024e6a8, copied by 0x10030590): {sound, priority, playing cap, rate}; -1 = none
 * (priority default 0x32, no cap, the default rate). A later entry for the same sound replaces the earlier (64). */
static const short SND_TABLE[][4] = {
    {3, 80, -1, -1},
    {4, 80, -1, -1},
    {5, 80, -1, -1},
    {9, 80, -1, -1},
    {12, 80, -1, -1},
    {14, 80, 1, -1},
    {15, 80, 1, -1},
    {16, 80, -1, -1},
    {62, 80, -1, -1},
    {64, 80, -1, -1},
    {68, 80, -1, -1},
    {75, 80, -1, -1},
    {88, 80, -1, -1},
    {89, 80, -1, -1},
    {90, 80, -1, -1},
    {91, 80, -1, -1},
    {92, 80, -1, -1},
    {93, 80, -1, -1},
    {95, 80, -1, -1},
    {96, 80, -1, -1},
    {97, 80, -1, -1},
    {98, 80, -1, -1},
    {99, 80, -1, -1},
    {100, 80, -1, -1},
    {101, 80, -1, -1},
    {102, 80, -1, -1},
    {103, 80, -1, -1},
    {104, 80, -1, -1},
    {105, 80, -1, -1},
    {106, 80, -1, -1},
    {107, 80, -1, -1},
    {108, 80, -1, -1},
    {109, 80, -1, -1},
    {110, 100, -1, -1},
    {112, 80, -1, -1},
    {114, 80, -1, -1},
    {115, 80, -1, -1},
    {116, 80, -1, -1},
    {117, 80, -1, -1},
    {118, 80, -1, -1},
    {120, 80, -1, -1},
    {121, 80, -1, -1},
    {131, 80, -1, -1},
    {135, 80, -1, -1},
    {136, 80, -1, -1},
    {165, 80, -1, -1},
    {166, 80, -1, -1},
    {168, 80, -1, -1},
    {169, 80, -1, -1},
    {170, 80, -1, -1},
    {171, 80, -1, -1},
    {174, 80, -1, -1},
    {175, 80, -1, -1},
    {176, 80, -1, -1},
    {177, 80, -1, -1},
    {181, -1, 1, -1},
    {182, 80, -1, -1},
    {183, 80, -1, -1},
    {184, 80, -1, -1},
    {185, 80, -1, -1},
    {186, 80, -1, -1},
    {188, 80, 2, -1},
    {189, 80, -1, -1},
    {197, 100, 1, -1},
    {200, 80, -1, -1},
    {208, 100, -1, -1},
    {209, 80, 2, -1},
    {210, 80, 2, -1},
    {211, 80, 2, -1},
    {223, 0, -1, -1},
    {226, -1, -1, -1},
    {227, 100, 4, -1},
    {231, 80, -1, -1},
    {233, 80, -1, -1},
    {235, 80, 2, -1},
    {236, 80, -1, -1},
    {240, 80, -1, -1},
    {246, 80, 2, -1},
    {249, 80, -1, -1},
    {252, 80, -1, -1},
    {254, 80, -1, -1},
    {255, 80, -1, -1},
    {258, 80, -1, -1},
    {261, 80, -1, -1},
    {262, 80, -1, -1},
    {263, 80, -1, -1},
    {267, 80, -1, -1},
    {268, 80, -1, -1},
    {269, 80, -1, -1},
    {270, 80, -1, -1},
    {271, 80, -1, -1},
    {272, 80, -1, -1},
    {273, 80, -1, -1},
    {274, 80, -1, -1},
    {275, 80, -1, -1},
    {276, 80, -1, -1},
    {279, 80, -1, -1},
    {280, 80, -1, -1},
    {283, 80, -1, -1},
    {284, 80, -1, -1},
    {285, 80, -1, -1},
    {286, 80, -1, -1},
    {287, 80, -1, -1},
    {288, 80, -1, -1},
    {289, 80, -1, -1},
    {290, 80, -1, -1},
    {292, 80, -1, -1},
    {293, 80, -1, -1},
    {294, 80, -1, -1},
    {295, 80, -1, -1},
    {296, 80, -1, -1},
    {297, 80, -1, -1},
    {298, 80, 2, -1},
    {299, 80, 2, -1},
    {307, 80, -1, -1},
    {308, 80, -1, -1},
    {309, 80, -1, -1},
    {310, 80, -1, -1},
    {311, 80, -1, -1},
    {312, 80, -1, -1},
    {313, 80, -1, -1},
    {314, 80, -1, -1},
    {315, 80, -1, -1},
    {316, 0, 1, -1},
    {320, 80, -1, -1},
    {321, 80, -1, -1},
    {322, 80, -1, -1},
    {323, 80, -1, -1},
    {324, 80, -1, -1},
    {326, 80, -1, -1},
    {327, 10, 1, -1},
    {330, 80, -1, -1},
    {331, 80, -1, -1},
    {332, 80, -1, -1},
    {333, 80, -1, -1},
    {334, 80, -1, -1},
    {336, 80, -1, -1},
    {337, 80, -1, -1},
    {338, 80, -1, -1},
};
static void snd_rule(int index, int *prio, int *cap)
{
    size_t k;
    *prio = 0x32; *cap = -1;
    for (k = 0; k < sizeof SND_TABLE / sizeof SND_TABLE[0]; k++)
        if (SND_TABLE[k][0] == index) { if (SND_TABLE[k][1] != -1) *prio = SND_TABLE[k][1]; *cap = SND_TABLE[k][2]; return; }
}

struct sfx_s {
    float gain_fx, gain_voice;   /* sfx_set_gains; 0 = unset (full) */
    int   next_is_voice;         /* the next sfx_play_g is on the voice channel */
    long  plays, mixed_frames, callbacks;
    sfx_music_fn music; void *music_user;
    int   paused;                /* sfx_pause */
    prj_archive *a;
    SDL_AudioDeviceID dev;
    int    rate;
    clip   cache[SFX_CACHE];
    voice  v[SFX_VOICES];
};

/* SFLX (engine 0x10032238 / block decoder 0x1003356c): "SFLX", u32, u32 block count (+8), u16 samples per block (+12),
 * then blocks: a header byte - bits 6-7 the interpolation (0 none, 1 half the samples coded then doubled, 2 a quarter,
 * quadrupled, linearly), bits 0-3 the codec: 0 silence, 1 the previous block again, 2 / 3 / 4 delta codes of 1 / 2 / 4
 * bits (first 2 / 4 / 16 table bytes, each delta = byte x 2 - 128; codes LSB first) added to a running sample clamped to
 * +-127, 5 raw unsigned bytes. The running sample carries from block to block. Out: unsigned 8-bit at 11025 Hz. */
int sfx_decode_sflx(const uint8_t *rec, size_t len, unsigned char **out);
static int sflx_decode(const uint8_t *rec, size_t len, unsigned char **out);
int sfx_decode_sflx(const uint8_t *rec, size_t len, unsigned char **out) { return sflx_decode(rec, len, out); }
static int sflx_decode(const uint8_t *rec, size_t len, unsigned char **out)
{
    uint32_t nb, b;
    unsigned bs, n, i;
    size_t p = 14, o = 0;
    int prev = 0;
    unsigned char *dst, buf[2048], blk[2048];
    if (len < 14) return -1;
    nb = (uint32_t)rec[8] | (uint32_t)rec[9] << 8 | (uint32_t)rec[10] << 16 | (uint32_t)rec[11] << 24;
    bs = (unsigned)rec[12] | (unsigned)rec[13] << 8;
    if (bs == 0 || bs > 2048 || nb > 1000000) return -1;
    dst = malloc((size_t)nb * bs + 1);
    if (!dst) return -1;
    memset(buf, 0x80, sizeof buf);
    for (b = 0; b < nb && p < len; b++) {
        int h = rec[p++], m = h >> 6, c = h & 15;
        n = bs >> (m > 2 ? 2 : m);
        if (c == 0) { memset(buf, 0x80, n); prev = 0; }
        else if (c == 1) { /* the previous block's samples again */ }
        else if (c >= 2 && c <= 4) {
            int k = c == 2 ? 2 : c == 3 ? 4 : 16, bits = c == 2 ? 1 : c == 3 ? 2 : 4, t[16], j;
            if (p + (size_t)k > len) break;
            for (j = 0; j < k; j++) t[j] = rec[p + (size_t)j] * 2 - 128;
            p += (size_t)k;
            for (i = 0; i < n;) {
                unsigned byte;
                int q;
                if (p >= len) break;
                byte = rec[p++];
                for (q = 0; q < 8 / bits && i < n; q++) {
                    prev += t[byte & (unsigned)(k - 1)];
                    byte >>= bits;
                    prev = prev > 127 ? 127 : prev < -127 ? -127 : prev;
                    buf[i++] = (unsigned char)(prev + 128);
                }
            }
        } else if (c == 5) {
            if (p + n > len) break;
            memcpy(buf, rec + p, n); p += n; prev = buf[n - 1] - 128;
        } else break;
        if (m) {   /* linear interpolation up to the block size */
            unsigned f = 1u << m, q;
            for (i = 0; i < n; i++) {
                int a0 = buf[i], a1 = i + 1 < n ? buf[i + 1] : buf[i];
                for (q = 0; q < f && i * f + q < bs; q++) blk[i * f + q] = (unsigned char)(a0 + (a1 - a0) * (int)q / (int)f);
            }
            memcpy(dst + o, blk, bs);
        } else memcpy(dst + o, buf, bs);
        o += bs;
    }
    *out = dst;
    return (int)o;
}

int sfx_decode(prj_archive *a, int index, unsigned char **out)
{
    prj_record r;
    int n;
    *out = NULL;
    if (!a) return -1;
    {
        int t = prj_find_type(a, "SNDS");
        if (t < 0 || prj_read(a, t, index, &r) != PRJ_OK) return -1;
    }
    if (r.size < 14 || memcmp(r.data, "SFLX", 4) != 0) { prj_record_free(&r); return -1; }
    n = sflx_decode(r.data, r.size, out);
    prj_record_free(&r);
    return n;
}

static void mix(void *user, Uint8 *stream, int len)
{
    sfx *s = user;
    float *o = (float *)stream;
    int frames = len / (int)(2 * sizeof(float)), f, k;
    memset(stream, 0, (size_t)len);
    s->callbacks++;
    if (s->paused) return;   /* everything held where it was */
    if (s->music) s->music(s->music_user, o, frames);
    for (k = 0; k < SFX_VOICES; k++) {
        voice *vc = &s->v[k];
        if (!vc->c) continue;
        for (f = 0; f < frames; f++) {
            int i = (int)vc->pos, n = vc->c->len;
            float smp, fr, a, b, env = 1.0f;
            if (i >= n) {
                if (vc->loop) { vc->pos = 0; i = 0; }
                else { vc->c = NULL; break; }
            }
            /* linear interpolation between the 11 kHz samples (nearest-sample stepping aliased into a grit), and a
             * 2.5 ms ramp at both ends of a one-shot (a clip that starts or stops off zero clicks) */
            fr = (float)(vc->pos - (double)i);
            a = ((float)vc->c->data[i] - 128.0f) / 128.0f;
            b = i + 1 < n ? ((float)vc->c->data[i + 1] - 128.0f) / 128.0f : (vc->loop ? ((float)vc->c->data[0] - 128.0f) / 128.0f : 0.0f);
            smp = a + (b - a) * fr;
            if (!vc->loop) {
                float ramp = 28.0f;   /* source samples (2.5 ms at 11025 Hz) */
                if (vc->pos < ramp) env = (float)vc->pos / ramp;
                if ((double)n - vc->pos < ramp) env = (float)(((double)n - vc->pos) / ramp);
            }
            smp *= env;
            s->mixed_frames++;
            o[f * 2] += smp * vc->l;
            o[f * 2 + 1] += smp * vc->r;
            vc->pos += vc->step;
        }
    }
    for (f = 0; f < frames * 2; f++) o[f] = tanhf(o[f] * 0.6f);   /* headroom + soft limit: overlapping full-scale samples */
}

sfx *sfx_create(prj_archive *a)
{
    SDL_AudioSpec want, have;
    sfx *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->a = a;
    SDL_zero(want);
    want.freq = 44100; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 1024;
    want.callback = mix; want.userdata = s;
    s->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!s->dev) { free(s); return NULL; }
    s->rate = have.freq;
    SDL_PauseAudioDevice(s->dev, 0);
    return s;
}

void sfx_destroy(sfx *s)
{
    int i;
    if (!s) return;
    if (getenv("MW2_SFX_TRACE")) fprintf(stderr, "sfx: %ld plays, %ld callbacks, %ld frames mixed\n", s->plays, s->callbacks, s->mixed_frames);
    SDL_CloseAudioDevice(s->dev);
    for (i = 0; i < SFX_CACHE; i++) free(s->cache[i].data);
    free(s);
}

static void sfx_play_g(sfx *s, int index, float volume, float pan);
void sfx_set_gains(sfx *s, float effects, float voice)
{
    if (!s) return;
    s->gain_fx = effects <= 0 ? -1.0f : effects;   /* -1: silent */
    s->gain_voice = voice <= 0 ? -1.0f : voice;
}
static float gain_of(float g) { return g == 0 ? 1.0f : g < 0 ? 0.0f : g; }
/* a voice line would be heard: the VOICE slider above 0 and the clip present (engine 0x10031960 case 1: voices on
 * while the volume is non-zero; 0x10031440 posts the line's text only when it could not play) */
int sfx_voice_audible(sfx *s, int index)
{
    clip *c;
    if (!s || index <= 0 || index >= SFX_CACHE || gain_of(s->gain_voice) <= 0.01f) return 0;
    c = &s->cache[index];
    if (!c->data) c->len = sfx_decode(s->a, index, &c->data);
    return c->data && c->len > 0;
}
void sfx_play(sfx *s, int index, float volume, float pan) { if (s) sfx_play_g(s, index, volume * gain_of(s->gain_fx), pan); }
void sfx_play_voice(sfx *s, int index, float volume) { if (s) { s->next_is_voice = 1; sfx_play_g(s, index, volume * gain_of(s->gain_voice), 0.0f); s->next_is_voice = 0; } }
double sfx_log_time = -1;   /* TEST ONLY (MW2_SFX_LOG): the caller's clock (s) printed with each play */
static void sfx_log(const char *what, int index, float volume, int voice)
{
    if (getenv("MW2_SFX_LOG")) fprintf(stderr, "SFXLOG %.3f %s %d vol %.2f%s\n", sfx_log_time, what, index, (double)volume, voice ? " voice" : "");
}
static void sfx_play_g(sfx *s, int index, float volume, float pan)
{
    clip *c;
    int k, best = 0;
    if (s && index > 0) sfx_log("snd", index, volume, s->next_is_voice);   /* TEST ONLY */
    if (!s || index <= 0 || index >= SFX_CACHE || volume <= 0.01f) return;
    c = &s->cache[index];
    if (!c->data) { c->len = sfx_decode(s->a, index, &c->data); if (c->len <= 0) return; }
    s->plays++;
    SDL_LockAudioDevice(s->dev);
    {   /* the engine (0x10032ad0 -> 0x10032000): a sound at its playing cap (the table's) does not start; else a free
         * one of the 8 effect handles, else the last playing one of LOWER priority is stolen; none: it does not play.
         * Voice lines have their own handles. */
        int prio, cap, same = 0, busy = 0, steal = -1;
        snd_rule(index, &prio, &cap);
        for (k = 0; k < SFX_VOICES; k++) if (s->v[k].c && s->v[k].index == index) same++;
        if (cap >= 0 && same >= cap) { SDL_UnlockAudioDevice(s->dev); return; }
        for (k = 0; k < SFX_VOICES; k++) if (s->v[k].c && !s->v[k].is_voice) { busy++; if (s->v[k].prio < prio && !s->v[k].loop) steal = k; }
        best = -1;
        if (s->next_is_voice || busy < 8) { for (k = 0; k < SFX_VOICES; k++) if (!s->v[k].c) { best = k; break; } }
        else best = steal;
        if (best < 0) { SDL_UnlockAudioDevice(s->dev); return; }
        s->v[best].index = index; s->v[best].prio = prio;
    }
    if (pan < -1) pan = -1;
    if (pan > 1) pan = 1;
    s->v[best].c = c;
    s->v[best].loop = 0;
    s->v[best].is_voice = s->next_is_voice; s->next_is_voice = 0;
    s->v[best].pos = 0;
    s->v[best].step = 11025.0 / (double)s->rate;    /* engine rate: 11025 Hz */
    s->v[best].l = volume * (pan <= 0 ? 1.0f : 1.0f - pan);
    s->v[best].r = volume * (pan >= 0 ? 1.0f : 1.0f + pan);
    SDL_UnlockAudioDevice(s->dev);
}

void sfx_play_at(sfx *s, int index, float dx, float dz, float listener_yaw)
{
    float d = sqrtf(dx * dx + dz * dz) / 100.0f, a, rel, vol;
    if (!s) return;
    a = atan2f(dx, dz) * 180.0f / 3.14159265f;
    rel = a - listener_yaw;
    while (rel > 180.0f) rel -= 360.0f;
    while (rel < -180.0f) rel += 360.0f;
    {   /* engine 0x10032f60: volume % = 100 - 100 (d / 500 m)^2 in whole metres, nothing beyond 500 m (half at 354 m);
         * pan 0x10032c10: 64 + 64 sin(angle) clamped to 15..111 of 0..127 = +-0.766 */
        int m = (int)d;
        float pan = sinf(rel * 3.14159265f / 180.0f);
        if (m >= 500) return;
        vol = (float)(100 - m * m * 100 / 250000) / 100.0f;
        if (pan > 0.766f) pan = 0.766f;
        if (pan < -0.766f) pan = -0.766f;
        sfx_play(s, index, vol * 0.8f, pan);
    }
}

int sfx_play_pcm(sfx *s, const unsigned char *pcm, int len, int rate, float volume, int loop)
{
    if (s) volume *= gain_of(s->gain_fx);   /* the EFFECTS slider */
    int k, best = -1;
    if (s && pcm) sfx_log("pcm", len, volume, s->next_is_voice);   /* TEST ONLY */
    if (!s || !pcm || len <= 0 || rate <= 0) return -1;
    SDL_LockAudioDevice(s->dev);
    for (k = 0; k < SFX_VOICES; k++) if (!s->v[k].c) { best = k; break; }
    if (best < 0) best = 0;
    s->v[best].index = -1; s->v[best].prio = 0x32; s->v[best].is_voice = 0;
    s->v[best].own.data = (unsigned char *)pcm;   /* not owned: never freed here */
    s->v[best].own.len = len;
    s->v[best].c = &s->v[best].own;
    s->v[best].pos = 0;
    s->v[best].step = (double)rate / (double)s->rate;
    s->v[best].l = s->v[best].r = volume;
    s->v[best].loop = loop;
    s->v[best].is_voice = s->next_is_voice; s->next_is_voice = 0;
    SDL_UnlockAudioDevice(s->dev);
    return 0;
}

void sfx_play_pcm_voice(sfx *s, const unsigned char *pcm, int len, float volume)
{
    if (!s) return;
    volume = volume * gain_of(s->gain_voice) / gain_of(s->gain_fx);   /* the VOICE slider, not EFFECTS */
    s->next_is_voice = 1;
    sfx_play_pcm(s, pcm, len, 11025, volume, 0);
    s->next_is_voice = 0;
}

int sfx_voice_busy(sfx *s)
{
    int k, on = 0;
    if (!s) return 0;
    SDL_LockAudioDevice(s->dev);
    for (k = 0; k < SFX_VOICES; k++) if (s->v[k].c && s->v[k].is_voice) on = 1;
    SDL_UnlockAudioDevice(s->dev);
    return on;
}

int sfx_playing(sfx *s, int index)
{
    int k, on = 0;
    if (!s || index <= 0 || index >= SFX_CACHE) return 0;
    SDL_LockAudioDevice(s->dev);
    for (k = 0; k < SFX_VOICES; k++) if (s->v[k].c == &s->cache[index]) on = 1;
    SDL_UnlockAudioDevice(s->dev);
    return on;
}

void sfx_stop_pcm(sfx *s, const unsigned char *pcm)
{
    int k;
    if (!s || !pcm) return;
    SDL_LockAudioDevice(s->dev);
    for (k = 0; k < SFX_VOICES; k++) if (s->v[k].c == &s->v[k].own && s->v[k].own.data == pcm) s->v[k].c = NULL;
    SDL_UnlockAudioDevice(s->dev);
}

void sfx_pcm_level(sfx *s, const unsigned char *pcm, float volume, float pan)
{
    int k;
    if (!s || !pcm) return;
    volume *= gain_of(s->gain_fx);   /* the EFFECTS slider, as sfx_play_pcm */
    if (pan < -1) pan = -1;
    if (pan > 1) pan = 1;
    SDL_LockAudioDevice(s->dev);
    for (k = 0; k < SFX_VOICES; k++)
        if (s->v[k].c == &s->v[k].own && s->v[k].own.data == pcm) {
            s->v[k].l = volume * (pan <= 0 ? 1.0f : 1.0f - pan);
            s->v[k].r = volume * (pan >= 0 ? 1.0f : 1.0f + pan);
        }
    SDL_UnlockAudioDevice(s->dev);
}

void sfx_stop_loops(sfx *s)
{
    int k;
    if (!s) return;
    SDL_LockAudioDevice(s->dev);
    for (k = 0; k < SFX_VOICES; k++) if (s->v[k].loop) { s->v[k].c = NULL; s->v[k].loop = 0; }
    SDL_UnlockAudioDevice(s->dev);
}

void sfx_pause(sfx *s, int on)
{
    int k;
    if (!s) return;
    SDL_LockAudioDevice(s->dev);
    if (on && !s->paused)   /* engine 0x10031f30: the effect samples end (0x10032c70), the voice and the music pause */
        for (k = 0; k < SFX_VOICES; k++) if (s->v[k].c && !s->v[k].loop && !s->v[k].is_voice) s->v[k].c = NULL;
    s->paused = on != 0;
    SDL_UnlockAudioDevice(s->dev);
}

void sfx_set_music(sfx *s, sfx_music_fn fn, void *user)
{
    if (!s) return;
    SDL_LockAudioDevice(s->dev);
    s->music = fn; s->music_user = user;
    SDL_UnlockAudioDevice(s->dev);
}
int sfx_rate(const sfx *s) { return s ? s->rate : 44100; }
void sfx_lock(sfx *s) { if (s) SDL_LockAudioDevice(s->dev); }
void sfx_unlock(sfx *s) { if (s) SDL_UnlockAudioDevice(s->dev); }
