/*
 * cdaudio.h - virtual CD-audio drive backed by a folder of FLAC/MP3/WAV files.
 *
 * MechWarrior 2 plays its soundtrack as Red Book CD audio through MSCDEX. It
 * reads the disc's table of contents, then asks the drive to play a range of
 * sectors. This module recreates that drive in software:
 *
 *   original (MW2.EXE)                     replacement
 *   ------------------------------------   ------------------------------
 *   cd_find_mscdex_drive  0x423f0          cdaudio_open (no drive check)
 *   IOCTL in  0Ah/0Bh  disc/track info     cdaudio_disc_info / _track_info
 *   IOCTL in  0Fh      audio status        cdaudio_get_status
 *   IOCTL out 03h      channel volume      cdaudio_set_volume
 *   cd_audio_play   (cmd 84h) 0x42780      cdaudio_play
 *   cd_audio_stop   (cmd 85h) 0x42710      cdaudio_stop
 *   cd_audio_resume (cmd 88h) 0x426a0      cdaudio_resume
 *
 * Track numbers come from the filenames: "Track02.flac", "02 - Title.mp3",
 * "mw2_02.wav" and similar all map to track 2. On the retail disc track 1 is
 * the data track and the music starts at track 2. If your rip renumbered the
 * music to start at 1, set track_offset = 1 (or MW2_CDA_TRACK_OFFSET=1).
 *
 * Addresses are HSG sector numbers (75 sectors per second; 588 stereo frames
 * per sector at 44.1 kHz), the same units MSCDEX uses.
 */
#ifndef MW2_CDAUDIO_H
#define MW2_CDAUDIO_H

#include <stddef.h>
#include <stdint.h>

#define CDA_MAX_TRACKS 99
#define CDA_SAMPLE_RATE 44100
#define CDA_FRAMES_PER_SECTOR 588 /* 44100 / 75 */

typedef struct cdaudio cdaudio;

typedef struct {
    int      track_offset;  /* added to the number parsed from each filename */
    uint32_t data_sectors;  /* length of the data track before the 2 s audio pregap;
                               0 = MW2_CDA_DATA_SECTORS, else a default */
    int      verbose;       /* print the TOC to stderr on open */
} cdaudio_options;

typedef struct {
    int      busy;          /* audio is playing (MSCDEX "busy" bit) */
    int      paused;        /* stopped mid-range; cdaudio_resume continues */
    int      track;         /* track at the current position, 0 if none */
    uint32_t position;      /* current HSG sector */
    uint32_t end;           /* HSG sector where the current play request ends */
} cdaudio_status;

/* Scan dir and build the table of contents. Returns 0 on success. */
int  cdaudio_open(cdaudio **out, const char *dir, const cdaudio_options *opt);
void cdaudio_close(cdaudio *cd);

/* Table of contents. */
void cdaudio_disc_info(const cdaudio *cd, int *first_track, int *last_track, uint32_t *leadout);
int  cdaudio_track_info(const cdaudio *cd, int track, uint32_t *start, int *is_audio);
uint32_t cdaudio_track_length(const cdaudio *cd, int track); /* in sectors */

/* Transport. */
int  cdaudio_play(cdaudio *cd, uint32_t start, uint32_t sector_count);
int  cdaudio_play_track(cdaudio *cd, int track); /* convenience: whole track */
void cdaudio_stop(cdaudio *cd);   /* 1st call pauses, 2nd call resets (as MSCDEX) */
int  cdaudio_resume(cdaudio *cd);
void cdaudio_get_status(cdaudio *cd, cdaudio_status *st);
void cdaudio_set_volume(cdaudio *cd, uint8_t left, uint8_t right); /* 0-255 */

/* Output. cdaudio_start_device plays through the system's default audio
 * device; cdaudio_render pulls interleaved stereo float samples directly
 * (for mixing into the game's own audio, or for testing). */
int    cdaudio_start_device(cdaudio *cd);
void   cdaudio_stop_device(cdaudio *cd);
size_t cdaudio_render(cdaudio *cd, float *out, size_t frames);

/* Red Book MSF <-> HSG helpers (MSF packed as min<<16 | sec<<8 | frame). */
static inline uint32_t cda_msf_to_hsg(uint32_t msf)
{
    return ((msf >> 16) & 0xff) * 4500u + ((msf >> 8) & 0xff) * 75u + (msf & 0xff) - 150u;
}
static inline uint32_t cda_hsg_to_msf(uint32_t hsg)
{
    uint32_t f = hsg + 150u;
    return ((f / 4500u) << 16) | (((f / 75u) % 60u) << 8) | (f % 75u);
}

#endif
