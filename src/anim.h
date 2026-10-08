/*
 * anim.h - mech animation (ANIM resources, .3DI), decoded from the 3dfx engine
 * (loader MW2.DLL 0x100401c0, playback 0x1000dec0).
 *
 *   i32 track_count, i32 key_count (32)
 *   track_count x { i32 target, i32 type, i32 delta[key_count] }
 *       type 0-2: translate x/y/z (game units); 3-5: rotate about x/y/z
 *       (16.16 degrees). Deltas: each track sums to zero over its keys, so
 *       the pose at key k is the running total. The engine spreads each key's
 *       delta over a duration set by the mech's speed.
 *   key_count x 8-byte key entries: u32 flags, bytes. Flag 0x01 starts a
 *       sequence (byte 4 = its number; keys 0, 12, 20 for the Timber Wolf),
 *       0x400 marks a return to rest; 0x40/0x80 are gait-change points and
 *       0x800 likely footfalls (not confirmed).
 *
 * Which skeleton node a track drives comes from the mech record's TSK chunks:
 * text "node;rate,flags,track" (e.g. "4;70,1,2" = track 2 drives node 4); rate
 * differs per mech (70 Timber Wolf, 45 Kit Fox, 37 Firemoth).
 */
#ifndef MW2_ANIM_H
#define MW2_ANIM_H

#include <stdint.h>

#include "prj.h"

#define ANIM_MAX_TRACKS 32
#define ANIM_MAX_KEYS 64

typedef struct {
    int     target, type;
    float   pose[ANIM_MAX_KEYS + 1];   /* running total at the start of each key */
} anim_track;

typedef struct {
    int        track_count, key_count;
    anim_track tracks[ANIM_MAX_TRACKS];
    uint32_t   key_flags[ANIM_MAX_KEYS];
    uint8_t    key_bytes[ANIM_MAX_KEYS][4];
} anim_set;

int   anim_parse(const uint8_t *data, size_t len, anim_set *out);
int   anim_load_id(prj_archive *a, int anim_id, anim_set *out);

/* Value of a track at key time t (linear within a key), in units / degrees. */
float anim_value(const anim_set *s, int track, float t);

/* Sequence n: first key and length (from the 0x01 flags). Returns 0 if found. */
int   anim_sequence(const anim_set *s, int n, int *first, int *count);
/* the key (relative to the sequence) a walk cycle loops back to: the one before its first footfall (flag 0x800) - the
 * keys before it lead in from standing and play once; 0 when it has no footfall */
int   anim_loop_start(const anim_set *s, int first, int count);
/* a walk cycle's stride: from its first footfall (flag 0x800) through two footfalls - left, right - and back
 * (start, end relative to the sequence; returns the keys between footfalls, 0 without two) */
int   anim_loop_range(const anim_set *s, int first, int count, int *ls, int *le);

#endif
