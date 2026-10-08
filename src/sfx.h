/*
 * sfx.h - sound effects: the game's SNDS records mixed through SDL audio.
 *
 * SNDS record (engine sound code 0x100314e9..: 11025 Hz): "SFLX", u32 length of the rest,
 * u32 (51 in samples seen; unknown), then unsigned 8-bit mono samples. Records are addressed by
 * their PRJ index, as the weapon table (+0x24) and the message table (0x1024f144) do.
 */
#ifndef MW2_SFX_H
#define MW2_SFX_H

#include "prj.h"

typedef struct sfx_s sfx;

/* Opens an SDL audio device (SDL_INIT_AUDIO must be initialised). NULL if unavailable. */
sfx *sfx_create(prj_archive *a);
void sfx_destroy(sfx *s);
/* Play SNDS record `index` (1-based PRJ index). volume 0..1, pan -1 (left) .. 1 (right). */
void sfx_play(sfx *s, int index, float volume, float pan);
/* The EFFECTS and VOICE sliders (MW2SND.CFG +4 / +8, 16.16): every sound scaled by effects, voice lines (sfx_play_voice)
 * by voice instead. */
void sfx_set_gains(sfx *s, float effects, float voice);
void sfx_play_voice(sfx *s, int index, float volume);
int  sfx_voice_audible(sfx *s, int index);   /* the VOICE slider up and the clip present */
/* Play at a world position relative to the listener (cm; listener facing yaw degrees). */
void sfx_play_at(sfx *s, int index, float dx, float dz, float listener_yaw);
/* Play caller-owned unsigned 8-bit mono PCM at `rate` Hz (data must stay valid while playing).
 * loop = 1 repeats until sfx_stop_loops. Returns 0 on success. */
int  sfx_play_pcm(sfx *s, const unsigned char *pcm, int len, int rate, float volume, int loop);
void sfx_stop_loops(sfx *s);
/* the in-game menu's pause (engine 0x10031f30 / 0x10031f50): one-shot effects end, voice, loops and music hold */
void sfx_pause(sfx *s, int on);
/* 1 while a voice is playing sound `index` */
int  sfx_playing(sfx *s, int index);
/* 1 while a voice-channel sound (sfx_play_voice) plays */
int  sfx_voice_busy(sfx *s);
/* decode an SFLX sound (a SNDS record or a disc .SFL file) to unsigned 8-bit at 11025 Hz; free(*out). Bytes, or -1 */
int  sfx_decode_sflx(const unsigned char *rec, size_t len, unsigned char **out);
/* play decoded 8-bit PCM on the voice channel (counts for sfx_voice_busy); the buffer must stay valid */
void sfx_play_pcm_voice(sfx *s, const unsigned char *pcm, int len, float volume);
/* stop every voice playing this buffer (sfx_play_pcm) */
void sfx_stop_pcm(sfx *s, const unsigned char *pcm);
/* set the volume (EFFECTS-scaled) and pan of every voice playing this buffer (a positional loop) */
void sfx_pcm_level(sfx *s, const unsigned char *pcm, float volume, float pan);
/* Music: called from the audio thread to add `frames` stereo float frames into out. */
typedef void (*sfx_music_fn)(void *user, float *out, int frames);
void sfx_set_music(sfx *s, sfx_music_fn fn, void *user);
int  sfx_rate(const sfx *s);
void sfx_lock(sfx *s);
void sfx_unlock(sfx *s);
/* Decode a record to 8-bit unsigned samples (for tests). Returns sample count, or -1. */
int  sfx_decode(prj_archive *a, int index, unsigned char **out);

/* TEST ONLY: with MW2_SFX_LOG set every play is printed (stderr) with this clock (seconds, set by the caller) */
extern double sfx_log_time;

#endif
