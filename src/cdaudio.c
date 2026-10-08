/* cdaudio.c - see cdaudio.h. */
#include "cdaudio.h"

#include "../third_party/miniaudio.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DEFAULT_DATA_SECTORS 150000u /* ~290 MB data track; only needs to be plausible */
#define PREGAP_SECTORS 150u          /* 2 s gap between the data track and audio */

typedef struct {
    char    *path;       /* NULL for a data/placeholder track */
    uint32_t start;      /* HSG */
    uint32_t length;     /* sectors */
} cda_track;

struct cdaudio {
    cda_track  tracks[CDA_MAX_TRACKS + 1]; /* 1-based */
    int        first, last;
    uint32_t   leadout;

    ma_mutex   lock;
    int        playing, paused;
    uint64_t   pos_frame, end_frame;       /* absolute, in 44.1 kHz frames */
    float      vol_l, vol_r;

    ma_decoder decoder;
    int        decoder_track;              /* track the decoder has open, 0 = none */
    uint64_t   decoder_frame;              /* decoder read position within that track */

    ma_device  device;
    int        device_running;
};

/* ---- table of contents ------------------------------------------------ */

static int has_audio_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (strcasecmp(dot, ".flac") == 0 || strcasecmp(dot, ".mp3") == 0 ||
                   strcasecmp(dot, ".wav") == 0);
}

/* "Track02.flac" -> 2, "02 - Battle.mp3" -> 2, "mw2_cd_07.wav" -> 7. -1 if none. */
static int parse_track_number(const char *name)
{
    char stem[256];
    const char *p, *dot;
    size_t n;
    int last = -1;

    dot = strrchr(name, '.');
    n = dot ? (size_t)(dot - name) : strlen(name);
    if (n >= sizeof stem) n = sizeof stem - 1;
    memcpy(stem, name, n);
    stem[n] = '\0';

    for (p = stem; *p; p++) {
        if (strncasecmp(p, "track", 5) == 0) {
            const char *q = p + 5;
            while (*q == ' ' || *q == '_' || *q == '-') q++;
            if (isdigit((unsigned char)*q)) return atoi(q);
        }
    }
    if (isdigit((unsigned char)stem[0])) return atoi(stem);
    for (p = stem; *p; p++) {
        if (isdigit((unsigned char)*p) && (p == stem || !isdigit((unsigned char)p[-1]))) last = atoi(p);
    }
    return last;
}

static uint32_t measure_sectors(const char *path)
{
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, CDA_SAMPLE_RATE);
    ma_decoder dec;
    ma_uint64 frames = 0;
    if (ma_decoder_init_file(path, &cfg, &dec) != MA_SUCCESS) return 0;
    if (ma_decoder_get_length_in_pcm_frames(&dec, &frames) != MA_SUCCESS) frames = 0;
    ma_decoder_uninit(&dec);
    return (uint32_t)((frames + CDA_FRAMES_PER_SECTOR - 1) / CDA_FRAMES_PER_SECTOR);
}

int cdaudio_open(cdaudio **out, const char *dir, const cdaudio_options *opt)
{
    cdaudio_options o = {0, 0, 0};
    cdaudio *cd;
    DIR *d;
    struct dirent *e;
    const char *env;
    uint32_t cursor = 0;
    int n, first_audio = 0, prev_audio = 0;

    *out = NULL;
    if (opt) o = *opt;
    if (!o.data_sectors && (env = getenv("MW2_CDA_DATA_SECTORS")) != NULL) o.data_sectors = (uint32_t)atol(env);
    if (!o.data_sectors) o.data_sectors = DEFAULT_DATA_SECTORS;
    if (o.track_offset == 0 && (env = getenv("MW2_CDA_TRACK_OFFSET")) != NULL) o.track_offset = atoi(env);

    d = opendir(dir);
    if (!d) return -1;
    cd = (cdaudio *)calloc(1, sizeof *cd);
    if (!cd) { closedir(d); return -1; }

    while ((e = readdir(d)) != NULL) {
        char full[2048];
        int num;
        if (!has_audio_ext(e->d_name)) continue;
        num = parse_track_number(e->d_name);
        if (num < 0) continue;
        num += o.track_offset;
        if (num < 1 || num > CDA_MAX_TRACKS) continue;
        if (cd->tracks[num].path) {
            fprintf(stderr, "cdaudio: two files for track %d, keeping %s\n", num, cd->tracks[num].path);
            continue;
        }
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        cd->tracks[num].length = measure_sectors(full);
        if (cd->tracks[num].length == 0) {
            fprintf(stderr, "cdaudio: cannot decode %s, skipping\n", full);
            continue;
        }
        cd->tracks[num].path = strdup(full);
        if (num > cd->last) cd->last = num;
        if (!first_audio || num < first_audio) first_audio = num;
    }
    closedir(d);
    if (!first_audio) { free(cd); return -1; }

    /* Lay the tracks out like a real disc: data track(s) first, then audio. */
    cd->first = 1;
    for (n = 1; n <= cd->last; n++) {
        cda_track *t = &cd->tracks[n];
        if (!t->path) t->length = (n < first_audio) ? o.data_sectors : PREGAP_SECTORS;
        if (t->path && !prev_audio && n > 1) cursor += PREGAP_SECTORS;
        t->start = cursor;
        cursor += t->length;
        prev_audio = t->path != NULL;
    }
    cd->leadout = cursor;
    cd->vol_l = cd->vol_r = 1.0f;

    if (ma_mutex_init(&cd->lock) != MA_SUCCESS) { cdaudio_close(cd); return -1; }

    if (o.verbose) {
        for (n = cd->first; n <= cd->last; n++) {
            uint32_t msf = cda_hsg_to_msf(cd->tracks[n].start);
            fprintf(stderr, "track %2d  %02u:%02u:%02u  %7u sectors  %s\n", n, (msf >> 16) & 0xff,
                    (msf >> 8) & 0xff, msf & 0xff, cd->tracks[n].length,
                    cd->tracks[n].path ? cd->tracks[n].path : "(data)");
        }
    }
    *out = cd;
    return 0;
}

void cdaudio_close(cdaudio *cd)
{
    int n;
    if (!cd) return;
    cdaudio_stop_device(cd);
    if (cd->decoder_track) ma_decoder_uninit(&cd->decoder);
    ma_mutex_uninit(&cd->lock);
    for (n = 0; n <= CDA_MAX_TRACKS; n++) free(cd->tracks[n].path);
    free(cd);
}

void cdaudio_disc_info(const cdaudio *cd, int *first_track, int *last_track, uint32_t *leadout)
{
    if (first_track) *first_track = cd->first;
    if (last_track) *last_track = cd->last;
    if (leadout) *leadout = cd->leadout;
}

int cdaudio_track_info(const cdaudio *cd, int track, uint32_t *start, int *is_audio)
{
    if (track < cd->first || track > cd->last) return -1;
    if (start) *start = cd->tracks[track].start;
    if (is_audio) *is_audio = cd->tracks[track].path != NULL;
    return 0;
}

uint32_t cdaudio_track_length(const cdaudio *cd, int track)
{
    return (track < cd->first || track > cd->last) ? 0 : cd->tracks[track].length;
}

static int track_at(const cdaudio *cd, uint64_t frame)
{
    int n;
    for (n = cd->first; n <= cd->last; n++) {
        uint64_t s = (uint64_t)cd->tracks[n].start * CDA_FRAMES_PER_SECTOR;
        uint64_t e = s + (uint64_t)cd->tracks[n].length * CDA_FRAMES_PER_SECTOR;
        if (frame >= s && frame < e) return n;
    }
    return 0;
}

/* ---- transport ---------------------------------------------------------- */

int cdaudio_play(cdaudio *cd, uint32_t start, uint32_t sector_count)
{
    if (start >= cd->leadout || sector_count == 0) return -1;
    if (sector_count > cd->leadout - start) sector_count = cd->leadout - start;
    ma_mutex_lock(&cd->lock);
    cd->pos_frame = (uint64_t)start * CDA_FRAMES_PER_SECTOR;
    cd->end_frame = (uint64_t)(start + sector_count) * CDA_FRAMES_PER_SECTOR;
    cd->playing = 1;
    cd->paused = 0;
    ma_mutex_unlock(&cd->lock);
    return 0;
}

int cdaudio_play_track(cdaudio *cd, int track)
{
    if (track < cd->first || track > cd->last || !cd->tracks[track].path) return -1;
    return cdaudio_play(cd, cd->tracks[track].start, cd->tracks[track].length);
}

void cdaudio_stop(cdaudio *cd)
{
    ma_mutex_lock(&cd->lock);
    if (cd->playing) {
        cd->playing = 0;
        cd->paused = 1;  /* position kept for resume */
    } else {
        cd->paused = 0;  /* second stop: forget the range */
        cd->pos_frame = cd->end_frame = 0;
    }
    ma_mutex_unlock(&cd->lock);
}

int cdaudio_resume(cdaudio *cd)
{
    int ok;
    ma_mutex_lock(&cd->lock);
    ok = cd->paused && cd->pos_frame < cd->end_frame;
    if (ok) { cd->playing = 1; cd->paused = 0; }
    ma_mutex_unlock(&cd->lock);
    return ok ? 0 : -1;
}

void cdaudio_get_status(cdaudio *cd, cdaudio_status *st)
{
    ma_mutex_lock(&cd->lock);
    st->busy = cd->playing;
    st->paused = cd->paused;
    st->position = (uint32_t)(cd->pos_frame / CDA_FRAMES_PER_SECTOR);
    st->end = (uint32_t)(cd->end_frame / CDA_FRAMES_PER_SECTOR);
    st->track = track_at(cd, cd->pos_frame);
    ma_mutex_unlock(&cd->lock);
}

void cdaudio_set_volume(cdaudio *cd, uint8_t left, uint8_t right)
{
    ma_mutex_lock(&cd->lock);
    cd->vol_l = (float)left / 255.0f;
    cd->vol_r = (float)right / 255.0f;
    ma_mutex_unlock(&cd->lock);
}

/* ---- rendering ---------------------------------------------------------- */

/* Make the decoder sit on `track` at `frame_in_track`. Caller holds the lock. */
static int seek_decoder(cdaudio *cd, int track, uint64_t frame_in_track)
{
    if (cd->decoder_track != track) {
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, CDA_SAMPLE_RATE);
        if (cd->decoder_track) ma_decoder_uninit(&cd->decoder);
        cd->decoder_track = 0;
        if (ma_decoder_init_file(cd->tracks[track].path, &cfg, &cd->decoder) != MA_SUCCESS) return -1;
        cd->decoder_track = track;
        cd->decoder_frame = 0;
    }
    if (cd->decoder_frame != frame_in_track) {
        if (ma_decoder_seek_to_pcm_frame(&cd->decoder, frame_in_track) != MA_SUCCESS) return -1;
        cd->decoder_frame = frame_in_track;
    }
    return 0;
}

size_t cdaudio_render(cdaudio *cd, float *out, size_t frames)
{
    size_t done = 0;

    ma_mutex_lock(&cd->lock);
    while (done < frames && cd->playing) {
        int track = track_at(cd, cd->pos_frame);
        uint64_t want = frames - done;
        uint64_t left_in_range = cd->end_frame - cd->pos_frame;
        float *dst = out + done * 2;
        ma_uint64 got = 0;

        if (want > left_in_range) want = left_in_range;
        if (track && cd->tracks[track].path) {
            uint64_t tstart = (uint64_t)cd->tracks[track].start * CDA_FRAMES_PER_SECTOR;
            uint64_t tend = tstart + (uint64_t)cd->tracks[track].length * CDA_FRAMES_PER_SECTOR;
            if (want > tend - cd->pos_frame) want = tend - cd->pos_frame;
            if (seek_decoder(cd, track, cd->pos_frame - tstart) == 0) {
                ma_decoder_read_pcm_frames(&cd->decoder, dst, want, &got);
                cd->decoder_frame += got;
            }
        } else {
            /* data track or gap: a real drive plays nothing here; skip to the next track */
            int n;
            uint64_t next = cd->end_frame;
            for (n = cd->first; n <= cd->last; n++) {
                uint64_t s = (uint64_t)cd->tracks[n].start * CDA_FRAMES_PER_SECTOR;
                if (cd->tracks[n].path && s > cd->pos_frame) { next = s; break; }
            }
            if (next > cd->end_frame) next = cd->end_frame;
            cd->pos_frame = next; /* jump, don't play silence for the whole data track */
            if (cd->pos_frame >= cd->end_frame) cd->playing = 0;
            continue;
        }
        /* rounding the track up to whole sectors (or a short decode) leaves a tail: silence */
        if (got < want) memset(dst + got * 2, 0, (size_t)(want - got) * 2 * sizeof(float));
        {
            size_t i;
            for (i = 0; i < want; i++) { dst[i * 2] *= cd->vol_l; dst[i * 2 + 1] *= cd->vol_r; }
        }
        done += (size_t)want;
        cd->pos_frame += want;
        if (cd->pos_frame >= cd->end_frame) cd->playing = 0; /* range finished: busy clears */
    }
    ma_mutex_unlock(&cd->lock);

    if (done < frames) memset(out + done * 2, 0, (frames - done) * 2 * sizeof(float));
    return done;
}

static void device_callback(ma_device *dev, void *out, const void *in, ma_uint32 frames)
{
    (void)in;
    cdaudio_render((cdaudio *)dev->pUserData, (float *)out, frames);
}

int cdaudio_start_device(cdaudio *cd)
{
    ma_device_config cfg;
    if (cd->device_running) return 0;
    cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = CDA_SAMPLE_RATE;
    cfg.dataCallback = device_callback;
    cfg.pUserData = cd;
    if (ma_device_init(NULL, &cfg, &cd->device) != MA_SUCCESS) return -1;
    if (ma_device_start(&cd->device) != MA_SUCCESS) { ma_device_uninit(&cd->device); return -1; }
    cd->device_running = 1;
    return 0;
}

void cdaudio_stop_device(cdaudio *cd)
{
    if (!cd->device_running) return;
    ma_device_uninit(&cd->device);
    cd->device_running = 0;
}
