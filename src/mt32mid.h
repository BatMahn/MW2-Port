/*
 * mt32mid.h - XMIDI music on an emulated Roland MT-32 (third_party/mt32emu, Munt, LGPL 2.1+), the MT-32 song set
 * of MW2's DATABASE (Miles MT32MPU.MDI: song number + 24). Needs the user's MT-32 ROMs.
 */
#ifndef MW2_MT32MID_H
#define MW2_MT32MID_H

#include <stddef.h>
#include <stdint.h>

typedef struct mt32_synth_s mt32_synth;
typedef struct mt32_song_s mt32_song;

/* rom_dir holds MT32_CONTROL.ROM and MT32_PCM.ROM (or the CM-32L pair). NULL if missing or unusable. */
mt32_synth *mt32_open(const char *rom_dir, int rate);
void        mt32_close(mt32_synth *s);
/* An XMIDI file (sequence 0). The data is copied. */
mt32_song  *mt32_song_open(mt32_synth *syn, const uint8_t *xmi, size_t len);
void        mt32_song_close(mt32_song *s);
/* Render frames of interleaved stereo float (added to out). Returns 0 once the song has ended. */
int         mt32_song_render(mt32_song *s, float *out, int frames);

#endif
