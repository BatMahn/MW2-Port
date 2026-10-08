/*
 * gusmid.h - XMIDI music played through Gravis UltraSound patches, as MW2's Miles driver
 * ULTRA.MDI does: ULTRAMID.INI maps each GM program (drums: 128 + note) to a patch for the
 * card's memory size (1024K column by default); patches load from <dir>/MIDI/<name>.pat.
 * Voices emulate the GF1: six-point envelope (rate = 6-bit increment, 2-bit range: every
 * 1/8/64/512 frames), 12-bit logarithmic volume, sample loops, per-sample pitch ranges.
 */
#ifndef MW2_GUSMID_H
#define MW2_GUSMID_H

#include <stddef.h>
#include <stdint.h>

typedef struct gus_bank_s gus_bank;
typedef struct gus_song_s gus_song;

/* dir = the ULTRASND folder (containing MIDI/); memory_kb 256/512/768/1024. NULL on failure. */
gus_bank *gus_bank_open(const char *dir, int memory_kb);
void      gus_bank_close(gus_bank *b);

/* An XMIDI file (FORM XDIR / CAT XMID or FORM XMID); sequence 0. Data is copied. */
gus_song *gus_song_open(gus_bank *b, const uint8_t *xmi, size_t len, int rate);
void      gus_song_close(gus_song *s);
/* Render frames of interleaved stereo float (added to out). Returns 0 once the song has ended. */
int       gus_song_render(gus_song *s, float *out, int frames);
double    gus_song_seconds(const gus_song *s);   /* elapsed */

#endif
