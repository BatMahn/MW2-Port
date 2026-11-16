/* cdaudio.c - see cdaudio.h. */
#include "cdaudio.h"

#include "../third_party/miniaudio.h"
#ifdef MW2_CLASSIC_MAC
/* classic Mac OS / Carbon: miniaudio decodes only (MA_NO_DEVICE_IO, MA_NO_THREADING); the music plays through the
 * SDL subset's stream, whose callback runs on the main thread with the rest of the game - no lock needed */
#include <SDL3/SDL.h>
typedef int cd_lock;
#define ma_mutex cd_lock
#define ma_mutex_init(m) (*(m) = 0, MA_SUCCESS)
#define ma_mutex_uninit(m) ((void)(m))
#define ma_mutex_lock(m) ((void)(m))
#define ma_mutex_unlock(m) ((void)(m))
#endif

#include "cddrive.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#if !defined(MW2_CLASSIC_MAC)
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif
#define CDA_HAVE_DRIVE 1
#endif

#define DEFAULT_DATA_SECTORS 150000u /* ~290 MB data track; only needs to be plausible */
#define PREGAP_SECTORS 150u          /* 2 s gap between the data track and audio */

typedef struct {
    char    *path;       /* NULL for a data/placeholder track */
    uint32_t start;      /* HSG */
    uint32_t length;     /* sectors */
    /* raw 16-bit stereo PCM read straight from a file instead of decoded: a disc image's audio track (a .cue's BINARY
     * file, little-endian, 2352 bytes a sector) or a mounted audio CD's AIFF track (macOS shows "N Audio Track.aiff",
     * big-endian) - path is then that file and raw_off the byte of the track's first frame */
    int      raw, raw_be;   /* raw 2: read from the CD drive (cdaudio.drive) */
    uint64_t raw_off;
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
    FILE      *rawf;                       /* the raw track's file (decoder_track's), or NULL */
    int        decoder_track;              /* track the decoder has open, 0 = none */
    uint64_t   decoder_frame;              /* decoder read position within that track */

#ifdef MW2_CLASSIC_MAC
    SDL_AudioStream *stream;
#else
    ma_device  device;
#endif
    int        device_running;

#ifdef CDA_HAVE_DRIVE
    /* a real drive: a reader thread keeps a ring of the frames from ring_pos on (about 3 s), read ahead of the play
     * position; when play jumps elsewhere it starts over there, and the music waits for it (as a drive seeks) */
    cddrive   *drive;
    unsigned char *ring;                   /* 4 bytes a frame */
    uint32_t   ring_cap, ring_head, ring_count;
    uint64_t   ring_pos, want_pos;
    unsigned   ring_gen;
    volatile int drive_quit;
#ifdef _WIN32
    HANDLE     drive_thread;
#else
    pthread_t  drive_thread;
#endif
    int        drive_thread_ok;
#endif
};

/* ---- table of contents ------------------------------------------------ */

static int has_audio_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (strcasecmp(dot, ".flac") == 0 || strcasecmp(dot, ".mp3") == 0 ||
                   strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".aif") == 0 || strcasecmp(dot, ".aiff") == 0);
}
static int is_aiff(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (strcasecmp(dot, ".aif") == 0 || strcasecmp(dot, ".aiff") == 0);
}

/* An AIFF file's 16-bit stereo 44.1 kHz sound data: its byte offset and its frames (0 if it is not such a file) - the
 * form a mounted audio CD's tracks take on macOS */
static uint64_t aiff_probe(const char *path, uint64_t *off)
{
    unsigned char h[12], c[8], comm[18];
    uint64_t frames = 0, pos = 12;
    int ok = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "FORM", 4) != 0 || (memcmp(h + 8, "AIFF", 4) != 0 && memcmp(h + 8, "AIFC", 4) != 0)) { fclose(f); return 0; }
    *off = 0;
    while (fseek(f, (long)pos, SEEK_SET) == 0 && fread(c, 1, 8, f) == 8) {
        uint32_t sz = (uint32_t)c[4] << 24 | (uint32_t)c[5] << 16 | (uint32_t)c[6] << 8 | c[7];
        if (memcmp(c, "COMM", 4) == 0 && sz >= 18 && fread(comm, 1, 18, f) == 18) {
            const int ch = comm[0] << 8 | comm[1], bits = comm[6] << 8 | comm[7], ex = ((comm[8] & 0x7f) << 8 | comm[9]) - 16383;
            frames = (uint64_t)comm[2] << 24 | (uint64_t)comm[3] << 16 | (uint64_t)comm[4] << 8 | comm[5];
            ok = ch == 2 && bits == 16 && ex == 15 && (comm[10] << 8 | comm[11]) == 0xac44;   /* 44100 as an 80-bit float */
        } else if (memcmp(c, "SSND", 4) == 0) {
            unsigned char so[4];
            if (fread(so, 1, 4, f) == 4) *off = pos + 16 + ((uint32_t)so[0] << 24 | (uint32_t)so[1] << 16 | (uint32_t)so[2] << 8 | so[3]);
        }
        pos += 8 + sz + (sz & 1);
    }
    fclose(f);
    return ok && *off ? frames : 0;
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

/* A disc image's table of contents from its .cue sheet: FILE "x" BINARY, TRACK nn AUDIO | MODEx/2352 | MODEx/2048,
 * INDEX 00 / 01 mm:ss:ff (relative to their FILE). The tracks keep the disc's own places (each file's sectors after the
 * last's), an audio track running from its INDEX 01 to the next track's INDEX 00 (or 01) - read raw from the BIN */
static int cue_load(cdaudio *cd, const char *cue, int track_offset)
{
    char line[1024], dir[1024], file[1024] = "";
    const char *sl = strrchr(cue, '/');
    FILE *f = fopen(cue, "r");
    struct { int num, audio, sector_size; char file[1024]; long i0, i1; uint64_t base, shift; } t[CDA_MAX_TRACKS + 1];
    int nt = 0, k;
    uint64_t base = 0, filesecs = 0, shift = 0;   /* shift: PREGAP sectors so far (on the disc, not in the files) */
    if (!f) return -1;
#ifdef _WIN32
    { const char *bs = strrchr(cue, '\\'); if (bs && (!sl || bs > sl)) sl = bs; }
#endif
    snprintf(dir, sizeof dir, "%.*s", sl ? (int)(sl - cue) : 1, sl ? cue : ".");
    memset(t, 0, sizeof t);
    while (fgets(line, sizeof line, f)) {
        char *p = line, kw[16] = "";
        while (*p == ' ' || *p == '\t') p++;
        if (sscanf(p, "%15s", kw) != 1) continue;
        if (strcasecmp(kw, "FILE") == 0) {
            char *q = strchr(p, '"'), *q2 = q ? strchr(q + 1, '"') : NULL, name[1024];
            FILE *bf;
            if (!q || !q2) continue;
            snprintf(name, sizeof name, "%.*s", (int)(q2 - q - 1), q + 1);
            if (name[0] == '/' || (name[0] && name[1] == ':')) snprintf(file, sizeof file, "%s", name);
            else if ((size_t)snprintf(file, sizeof file, "%s/%s", dir, name) >= sizeof file) continue;
            base += filesecs;   /* this file's sectors come after the last one's */
            filesecs = 0;
            if ((bf = fopen(file, "rb"))) { fseek(bf, 0, SEEK_END); filesecs = (uint64_t)ftell(bf) / 2352u; fclose(bf); }
        } else if (strcasecmp(kw, "TRACK") == 0 && nt < CDA_MAX_TRACKS) {
            int num = 0;
            char mode[32] = "";
            if (sscanf(p, "%*s %d %31s", &num, mode) != 2) continue;
            t[nt].num = num; t[nt].audio = strcasecmp(mode, "AUDIO") == 0;
            t[nt].sector_size = strstr(mode, "2048") ? 2048 : 2352;
            snprintf(t[nt].file, sizeof t[nt].file, "%s", file);
            t[nt].i0 = t[nt].i1 = -1; t[nt].base = base; t[nt].shift = shift;
            nt++;
        } else if (strcasecmp(kw, "PREGAP") == 0 && nt > 0) {
            int m = 0, sec = 0, fr = 0;
            if (sscanf(p, "%*s %d:%d:%d", &m, &sec, &fr) == 3) { shift += (uint64_t)(m * 4500 + sec * 75 + fr); t[nt - 1].shift = shift; }
        } else if (strcasecmp(kw, "INDEX") == 0 && nt > 0) {
            int ix = 0, m = 0, sec = 0, fr = 0;
            if (sscanf(p, "%*s %d %d:%d:%d", &ix, &m, &sec, &fr) != 4) continue;
            if (ix == 0) t[nt - 1].i0 = (long)m * 4500 + sec * 75 + fr;
            if (ix == 1) t[nt - 1].i1 = (long)m * 4500 + sec * 75 + fr;
        }
    }
    fclose(f);
    base += filesecs;
    if (nt == 0) return -1;
    for (k = 0; k < nt; k++) {
        int num = t[k].num + track_offset;
        uint64_t st, en;
        cda_track *ct;
        if (num < 1 || num > CDA_MAX_TRACKS || t[k].i1 < 0) continue;
        st = t[k].base + (uint64_t)t[k].i1;
        if (k + 1 < nt && t[k + 1].base == t[k].base) en = t[k].base + (uint64_t)(t[k + 1].i0 >= 0 ? t[k + 1].i0 : t[k + 1].i1);
        else if (k + 1 < nt) en = t[k + 1].base;   /* the file ends with the track */
        else en = base;
        st += t[k].shift; en += t[k].shift;
        if (en <= st) continue;
        ct = &cd->tracks[num];
        ct->start = (uint32_t)st;
        ct->length = (uint32_t)(en - st);
        if (t[k].audio && t[k].sector_size == 2352) {
            FILE *bf = fopen(t[k].file, "rb");
            if (!bf) { fprintf(stderr, "cdaudio: %s (track %d) is missing\n", t[k].file, num); continue; }
            fclose(bf);
            ct->path = strdup(t[k].file);
            ct->raw = 1; ct->raw_be = 0;
            ct->raw_off = (uint64_t)t[k].i1 * 2352u;
        }
        if (num > cd->last) cd->last = num;
    }
    cd->first = 1;
    cd->leadout = (uint32_t)(base + shift);
    for (k = 1; k <= cd->last; k++) if (cd->tracks[k].path) return 0;
    return -1;
}

/* a .cue in dir (the first one found), or NULL */
static char *find_cue(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char *got = NULL;
    if (!d) return NULL;
    while (!got && (e = readdir(d)) != NULL) {
        const char *dot = strrchr(e->d_name, '.');
        if (dot && strcasecmp(dot, ".cue") == 0) {
            size_t n = strlen(dir) + strlen(e->d_name) + 2;
            if ((got = malloc(n))) snprintf(got, n, "%s/%s", dir, e->d_name);
        }
    }
    closedir(d);
    return got;
}

#ifdef CDA_HAVE_DRIVE
#define DRIVE_RING_FRAMES (CDA_FRAMES_PER_SECTOR * 75u * 3u)
#define DRIVE_CHUNK 25   /* sectors a read (1/3 s) */

static void drive_sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000u);
#endif
}

static void drive_loop(cdaudio *cd)
{
    unsigned char *buf = (unsigned char *)malloc((size_t)DRIVE_CHUNK * CDD_SECTOR_BYTES);
    int fails = 0;
    if (!buf) return;
    while (!cd->drive_quit) {
        uint64_t fill;
        uint32_t lba, skip, frames, i;
        unsigned gen;
        int n, got;
        ma_mutex_lock(&cd->lock);
        if (!cd->playing) { ma_mutex_unlock(&cd->lock); drive_sleep_ms(20); continue; }
        if (cd->want_pos != cd->ring_pos) {   /* play moved: start over there */
            cd->ring_pos = cd->want_pos;
            cd->ring_head = cd->ring_count = 0;
            cd->ring_gen++;
            fails = 0;
        }
        if (cd->ring_count + DRIVE_CHUNK * CDA_FRAMES_PER_SECTOR > cd->ring_cap || cd->ring_pos + cd->ring_count >= cd->end_frame) {
            ma_mutex_unlock(&cd->lock);
            drive_sleep_ms(10);
            continue;
        }
        fill = cd->ring_pos + cd->ring_count;
        gen = cd->ring_gen;
        ma_mutex_unlock(&cd->lock);

        lba = (uint32_t)(fill / CDA_FRAMES_PER_SECTOR);
        skip = (uint32_t)(fill % CDA_FRAMES_PER_SECTOR);
        n = DRIVE_CHUNK;
        if (lba + (uint32_t)n > cd->leadout) n = (int)(cd->leadout - lba);
        if (n <= 0) { drive_sleep_ms(10); continue; }
        got = cddrive_read(cd->drive, lba, n, buf);
        if (got <= 0) {   /* a scratch or a busy drive: retry, then play those sectors as silence */
            if (++fails < 4) { drive_sleep_ms(20); continue; }
            memset(buf, 0, (size_t)n * CDD_SECTOR_BYTES);
            got = n;
        }
        fails = 0;
        frames = (uint32_t)got * CDA_FRAMES_PER_SECTOR - skip;
        ma_mutex_lock(&cd->lock);
        if (gen == cd->ring_gen && cd->want_pos == cd->ring_pos) {
            if (frames > cd->ring_cap - cd->ring_count) frames = cd->ring_cap - cd->ring_count;
            for (i = 0; i < frames; i++) {
                const uint32_t at = (cd->ring_head + cd->ring_count + i) % cd->ring_cap;
                memcpy(cd->ring + (size_t)at * 4, buf + (size_t)(skip + i) * 4, 4);
            }
            cd->ring_count += frames;
        }
        ma_mutex_unlock(&cd->lock);
    }
    free(buf);
}
#ifdef _WIN32
static DWORD WINAPI drive_thread_fn(LPVOID u) { drive_loop((cdaudio *)u); return 0; }
#else
static void *drive_thread_fn(void *u) { drive_loop((cdaudio *)u); return NULL; }
#endif

/* "cd:" / "cd:auto" / "cd:<device or drive letter>": the disc in a drive, its own table of contents */
static int cdaudio_open_drive(cdaudio **out, const char *dev, const cdaudio_options *opt)
{
    cddrive_toc toc;
    cddrive *drv = NULL;
    cdaudio *cd;
    int n;
    if (cddrive_open(&drv, dev, &toc) != 0) return -1;
    if (!(cd = (cdaudio *)calloc(1, sizeof *cd))) { cddrive_close(drv); return -1; }
    cd->drive = drv;
    cd->ring_cap = DRIVE_RING_FRAMES;
    if (!(cd->ring = (unsigned char *)malloc((size_t)cd->ring_cap * 4))) { cddrive_close(drv); free(cd); return -1; }
    cd->first = 1;
    for (n = toc.first; n <= toc.last; n++) {
        cda_track *t = &cd->tracks[n];
        t->start = toc.start[n];
        t->length = toc.start[n + 1] > toc.start[n] ? toc.start[n + 1] - toc.start[n] : 0;
        if (!toc.data[n] && t->length) { t->path = strdup(cddrive_name(drv)); t->raw = 2; }
    }
    cd->last = toc.last;
    cd->leadout = toc.start[toc.last + 1];
    cd->vol_l = cd->vol_r = 1.0f;
    cd->want_pos = cd->ring_pos = (uint64_t)-1;
    if (ma_mutex_init(&cd->lock) != MA_SUCCESS) { for (n = 0; n <= CDA_MAX_TRACKS; n++) free(cd->tracks[n].path); free(cd->ring); cddrive_close(drv); free(cd); return -1; }
#ifdef _WIN32
    cd->drive_thread = CreateThread(NULL, 0, drive_thread_fn, cd, 0, NULL);
    cd->drive_thread_ok = cd->drive_thread != NULL;
#else
    cd->drive_thread_ok = pthread_create(&cd->drive_thread, NULL, drive_thread_fn, cd) == 0;
#endif
    if (!cd->drive_thread_ok) { cdaudio_close(cd); return -1; }
    fprintf(stderr, "cdaudio: playing the CD in drive %s (tracks %d-%d)\n", cddrive_name(drv), toc.first, toc.last);
    if (opt && opt->verbose)
        for (n = cd->first; n <= cd->last; n++) {
            uint32_t msf = cda_hsg_to_msf(cd->tracks[n].start);
            fprintf(stderr, "track %2d  %02u:%02u:%02u  %7u sectors  %s\n", n, (msf >> 16) & 0xff, (msf >> 8) & 0xff, msf & 0xff,
                    cd->tracks[n].length, cd->tracks[n].path ? "audio" : "(data)");
        }
    *out = cd;
    return 0;
}
#endif

static int cdaudio_open_files(cdaudio **out, const char *dir, const cdaudio_options *opt);
#if defined(__APPLE__) && !defined(MW2_CLASSIC_MAC)
/* macOS mounts an audio CD's tracks as "N Audio Track.aiff" in a volume of their own: open the first such volume */
static int cdaudio_open_mac_volume(cdaudio **out, const cdaudio_options *opt)
{
    DIR *v = opendir("/Volumes");
    struct dirent *e;
    int rc = -1;
    if (!v) return -1;
    while (rc != 0 && (e = readdir(v)) != NULL) {
        char p[1024];
        DIR *d;
        struct dirent *f;
        int aiff = 0;
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/Volumes/%s", e->d_name);
        if (!(d = opendir(p))) continue;
        while ((f = readdir(d)) != NULL) if (strstr(f->d_name, "Audio Track") && is_aiff(f->d_name)) { aiff = 1; break; }
        closedir(d);
        if (aiff) rc = cdaudio_open_files(out, p, opt);
    }
    closedir(v);
    return rc;
}
#endif
int cdaudio_open(cdaudio **out, const char *dir, const cdaudio_options *opt)
{
    /* a disc image: dir names a .cue, or a folder of music files has none but holds a .cue (the BIN / CUE of the CD) */
    const char *dot = dir ? strrchr(dir, '.') : NULL;
    char *cue = NULL;
    int rc;
    *out = NULL;
    if (!dir) return -1;
    if (strncasecmp(dir, "cd:", 3) == 0) {   /* the disc in a drive */
#if defined(__APPLE__) && !defined(MW2_CLASSIC_MAC)
        if ((!dir[3] || strcmp(dir + 3, "auto") == 0) && cdaudio_open_mac_volume(out, opt) == 0) return 0;
#endif
#ifdef CDA_HAVE_DRIVE
        return cdaudio_open_drive(out, dir + 3, opt);
#else
        return -1;
#endif
    }
    if (dot && strcasecmp(dot, ".cue") == 0) cue = strdup(dir);
    else {
        if (cdaudio_open_files(out, dir, opt) == 0) return 0;
        cue = find_cue(dir);
    }
    if (!cue) return -1;
    {
        cdaudio *cd = (cdaudio *)calloc(1, sizeof *cd);
        int n;
        const char *env = getenv("MW2_CDA_TRACK_OFFSET");
        if (!cd) { free(cue); return -1; }
        rc = cue_load(cd, cue, opt && opt->track_offset ? opt->track_offset : env ? atoi(env) : 0);
        free(cue);
        if (rc != 0) { for (n = 0; n <= CDA_MAX_TRACKS; n++) free(cd->tracks[n].path); free(cd); return -1; }
        cd->vol_l = cd->vol_r = 1.0f;
        if (ma_mutex_init(&cd->lock) != MA_SUCCESS) { cdaudio_close(cd); return -1; }
        if (opt && opt->verbose)
            for (n = cd->first; n <= cd->last; n++) {
                uint32_t msf = cda_hsg_to_msf(cd->tracks[n].start);
                fprintf(stderr, "track %2d  %02u:%02u:%02u  %7u sectors  %s\n", n, (msf >> 16) & 0xff, (msf >> 8) & 0xff, msf & 0xff,
                        cd->tracks[n].length, cd->tracks[n].path ? cd->tracks[n].path : "(data)");
            }
        *out = cd;
        return 0;
    }
}

static int cdaudio_open_files(cdaudio **out, const char *dir, const cdaudio_options *opt)
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
        if (is_aiff(e->d_name)) {   /* (miniaudio has no AIFF decoder: read its PCM as it is) */
            uint64_t off = 0, fr = aiff_probe(full, &off);
            if (!fr) { fprintf(stderr, "cdaudio: %s is not 16-bit stereo 44.1 kHz AIFF, skipping\n", full); continue; }
            cd->tracks[num].length = (uint32_t)((fr + CDA_FRAMES_PER_SECTOR - 1) / CDA_FRAMES_PER_SECTOR);
            cd->tracks[num].raw = 1; cd->tracks[num].raw_be = 1; cd->tracks[num].raw_off = off;
            cd->tracks[num].path = strdup(full);
            if (num > cd->last) cd->last = num;
            if (!first_audio || num < first_audio) first_audio = num;
            continue;
        }
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
#ifdef CDA_HAVE_DRIVE
    if (cd->drive_thread_ok) {
        cd->drive_quit = 1;
#ifdef _WIN32
        WaitForSingleObject(cd->drive_thread, INFINITE);
        CloseHandle(cd->drive_thread);
#else
        pthread_join(cd->drive_thread, NULL);
#endif
    }
    if (cd->drive) cddrive_close(cd->drive);
    free(cd->ring);
#endif
    if (cd->decoder_track && !cd->rawf) ma_decoder_uninit(&cd->decoder);
    if (cd->rawf) fclose(cd->rawf);
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
    if (cd->decoder_track != track && cd->tracks[track].raw) {   /* a raw track: its file, positioned per read */
        if (cd->decoder_track && !cd->rawf) ma_decoder_uninit(&cd->decoder);
        if (cd->rawf) { fclose(cd->rawf); cd->rawf = NULL; }
        cd->decoder_track = 0;
        if (!(cd->rawf = fopen(cd->tracks[track].path, "rb"))) return -1;
        cd->decoder_track = track;
        cd->decoder_frame = (uint64_t)-1;
    }
    if (cd->tracks[track].raw) {
        if (cd->decoder_frame != frame_in_track) {
#ifdef _WIN32
            if (_fseeki64(cd->rawf, (long long)(cd->tracks[track].raw_off + frame_in_track * 4u), SEEK_SET) != 0) return -1;
#else
            if (fseeko(cd->rawf, (off_t)(cd->tracks[track].raw_off + frame_in_track * 4u), SEEK_SET) != 0) return -1;
#endif
            cd->decoder_frame = frame_in_track;
        }
        return 0;
    }
    if (cd->decoder_track != track) {
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, CDA_SAMPLE_RATE);
        if (cd->decoder_track && !cd->rawf) ma_decoder_uninit(&cd->decoder);
        if (cd->rawf) { fclose(cd->rawf); cd->rawf = NULL; }
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
#ifdef CDA_HAVE_DRIVE
            if (cd->tracks[track].raw == 2) {   /* the drive: what the reader has ready; nothing yet = still seeking */
                uint64_t i;
                if (cd->ring_pos != cd->pos_frame) cd->want_pos = cd->pos_frame;
                if (cd->ring_pos != cd->pos_frame || cd->ring_count == 0) break;   /* wait (silence; position held) */
                if (want > cd->ring_count) want = cd->ring_count;
                for (i = 0; i < want; i++) {
                    const unsigned char *b = cd->ring + (size_t)((cd->ring_head + i) % cd->ring_cap) * 4;
                    dst[i * 2] = (float)(int16_t)(b[0] | b[1] << 8) / 32768.0f;
                    dst[i * 2 + 1] = (float)(int16_t)(b[2] | b[3] << 8) / 32768.0f;
                }
                cd->ring_head = (uint32_t)((cd->ring_head + want) % cd->ring_cap);
                cd->ring_count -= (uint32_t)want;
                cd->ring_pos += want;
                cd->want_pos = cd->ring_pos;
                got = want;
            } else
#endif
            if (seek_decoder(cd, track, cd->pos_frame - tstart) == 0) {
                if (cd->tracks[track].raw) {   /* 16-bit stereo PCM straight from the file */
                    unsigned char buf[4096 * 4];
                    while (got < want) {
                        size_t chunk = (size_t)(want - got > 4096 ? 4096 : want - got), rd = fread(buf, 4, chunk, cd->rawf), i;
                        for (i = 0; i < rd * 2; i++) {
                            const int16_t v = cd->tracks[track].raw_be ? (int16_t)(buf[i * 2] << 8 | buf[i * 2 + 1]) : (int16_t)(buf[i * 2] | buf[i * 2 + 1] << 8);
                            dst[got * 2 + i] = (float)v / 32768.0f;
                        }
                        got += rd;
                        if (rd < chunk) break;
                    }
                } else ma_decoder_read_pcm_frames(&cd->decoder, dst, want, &got);
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

#ifdef MW2_CLASSIC_MAC
static void SDLCALL stream_callback(void *user, SDL_AudioStream *stream, int additional, int total)
{
    float buf[1024 * 2];
    (void)total;
    while (additional > 0) {
        int frames = additional / 8 > 1024 ? 1024 : (additional + 7) / 8;
        cdaudio_render((cdaudio *)user, buf, (ma_uint32)frames);
        SDL_PutAudioStreamData(stream, buf, frames * 8);
        additional -= frames * 8;
    }
}

int cdaudio_start_device(cdaudio *cd)
{
    SDL_AudioSpec spec;
    if (cd->device_running) return 0;
    spec.format = SDL_AUDIO_F32; spec.channels = 2; spec.freq = CDA_SAMPLE_RATE;
    cd->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, stream_callback, cd);
    if (!cd->stream) return -1;
    SDL_ResumeAudioStreamDevice(cd->stream);
    cd->device_running = 1;
    return 0;
}

void cdaudio_stop_device(cdaudio *cd)
{
    if (!cd->device_running) return;
    SDL_DestroyAudioStream(cd->stream);
    cd->stream = NULL;
    cd->device_running = 0;
}
#else
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
#endif
