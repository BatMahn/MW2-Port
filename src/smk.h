/*
 * smk.h - Smacker (SMK2/SMK4) video decoder for the MW2 shell's screens and cinematics.
 * Video: 8-bit frames + 256-colour palette; audio: track 0, decoded whole (smk_audio).
 */
#ifndef MW2_SMK_H
#define MW2_SMK_H

#include <stddef.h>
#include <stdint.h>

typedef struct smk_s smk;

smk *smk_open_mem(const uint8_t *data, size_t len);   /* data must outlive the decoder */
smk *smk_open_file(const char *path);
void smk_close(smk *s);
int  smk_width(const smk *s);
int  smk_height(const smk *s);
int  smk_frames(const smk *s);
double smk_frame_ms(const smk *s);
/* Decode the next frame (loops back to 0 after the last). Returns the frame index decoded, or -1. */
int  smk_next(smk *s);
void smk_rewind(smk *s);
const uint8_t *smk_pixels(const smk *s);              /* w*h indices */
const uint8_t (*smk_palette(const smk *s))[3];        /* 256 x RGB */
/* the whole of audio track 0 as signed 16-bit samples (interleaved when stereo); free(*out). 0, or -1 without audio */
int  smk_audio(const smk *s, int16_t **out, size_t *samples, int *rate, int *channels);

#endif
