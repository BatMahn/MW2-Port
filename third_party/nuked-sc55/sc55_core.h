/*
 * sc55_core.h - the MechWarrior 2 port's interface to the Nuked SC-55 core (mcu.cpp's end). Not part of upstream.
 * Copyright (C) 2026 the MechWarrior 2 port's author; GNU General Public License v2.0 or later (as Nuked SC-55).
 * One emulated unit per process (the core keeps its state in globals). Not thread-safe: one thread at a time.
 */
#ifndef NSC55_CORE_H
#define NSC55_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NSC55_FILES 6   /* per model: ROM1 (H8/532), ROM2 (extra code), wave ROMs, sub-MCU ROM (upstream's roms[]) */

int         nsc55_model_count(void);
const char *nsc55_model_name(int model);
const char *nsc55_rom_name(int model, int i);   /* upstream's file name, "" if the model has no such file */
int         nsc55_model_is_gm(int model);       /* 0 for the JV-880 (no General MIDI module) */
/* Load the ROMs (paths[i] for nsc55_rom_name(model, i); NULL / "" where there is none) and reset the unit.
 * 0 on success; -1 with a message on stderr. */
int         nsc55_open(int model, const char *const paths[NSC55_FILES]);
void        nsc55_close(void);
int         nsc55_rate(void);                    /* the output rate: 66207 Hz (SC-55mk2 etc.), 64000 Hz (SC-55mk1, CM-300) */
void        nsc55_midi(const uint8_t *data, int len);   /* bytes into the MIDI IN port (UART) */
/* Run the emulation until `frames` stereo 16-bit frames are out; returns that count (fewer only if the unit stalls). */
int         nsc55_render(int16_t *out, int frames);
void        nsc55_test_all_voices(void);   /* TEST ONLY (CPU measurement without firmware): all 28 voice slots on */

#ifdef __cplusplus
}
#endif

#endif
