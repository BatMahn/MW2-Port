/*
 * sc55mid.h - XMIDI music on an emulated Roland SC-55 (third_party/nuked-sc55, Nuked SC-55, GPL 2.0+), the General MIDI
 * song set of MW2's DATABASE - the one Miles MPU401.MDI ("the Roland Sound Canvas, RAP-10, and compatibles") plays:
 * song number + 32 (the shell's driver table 0x83e14). Needs the user's SC-55 ROM dumps (sc55/ in the game folder).
 * The emulator is cycle-accurate and heavy: it runs on its own thread, a short queue ahead of the mixer.
 * Not in the legacy build (MW2_NO_SC55).
 */
#ifndef MW2_SC55MID_H
#define MW2_SC55MID_H

#include <stddef.h>
#include <stdint.h>

typedef struct sc55_synth_s sc55_synth;

/* The ROM set in rom_dir (file names as upstream's: rom1.bin ... for the SC-55mk2, sc55_rom1.bin ... for the SC-55,
 * case-insensitive): its model index (>= 0) and name, or -1 if there is no complete set. Looks at the files only. */
int         sc55_detect(const char *rom_dir, char *model, size_t n);
/* Load the set, start the unit (boot, then a GS reset) on its thread, its output resampled to `rate`. NULL on failure. */
sc55_synth *sc55_open(const char *rom_dir, int rate);
void        sc55_close(sc55_synth *s);
const char *sc55_model(const sc55_synth *s);
/* Play an XMIDI file (sequence 0), looping; the data is copied. Replaces the song playing. 0 on success. */
int         sc55_play(sc55_synth *s, const uint8_t *xmi, size_t len);
void        sc55_stop(sc55_synth *s);   /* notes off; the unit goes idle once its sound has died away */
/* The mixer: add frames of interleaved stereo float. Never blocks on the emulation; short of data it adds what there is. */
void        sc55_render(sc55_synth *s, float *out, int frames);
/* statistics: frames short (underruns) since open; seconds emulated and the CPU seconds that took on the thread */
void        sc55_stats(const sc55_synth *s, long *underrun_frames, double *emu_s, double *cpu_s);
int         sc55_booted(const sc55_synth *s);

#endif
