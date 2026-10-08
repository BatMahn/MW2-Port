/* cdrip - turn a BIN/CUE image of the MechWarrior 2 disc into what the port uses:
 *
 *   cdrip disc.cue OUTDIR
 *     OUTDIR/mw2cd.iso           data track (2048-byte sectors), for MW2_CD_IMAGE
 *     OUTDIR/music/TrackNN.wav   one WAV per audio track, for the virtual CD drive
 *
 * Damage repair: some rippers store audio sectors in scrambled "data" form,
 * with a 12-byte sync pattern overwriting the first 3 stereo frames of every
 * sector. cdrip detects those sectors, descrambles them (ECMA-130), rebuilds
 * the lost frames by interpolation and conceals read-error clicks. It
 * reports how much of each track needed repair. Clean sectors pass through
 * bit-exact.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define RAW 2352
#define FRAMES_PER_SECTOR 588
#define MAX_TRACKS 99

typedef struct {
    int  number;
    int  audio;
    int  raw;            /* data track stored as 2352-byte sectors */
    char file[1024];
    long start;          /* sector index within file */
    long end;            /* exclusive */
} track;

static track tracks[MAX_TRACKS];
static int ntracks;
static unsigned char scrambler[RAW - 12];
static const unsigned char SYNC[12] = {0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0};

static void build_scrambler(void)
{
    unsigned reg = 1;
    int i, bit;
    for (i = 0; i < RAW - 12; i++) {
        unsigned char b = 0;
        for (bit = 0; bit < 8; bit++) {
            unsigned fb;
            b = (unsigned char)(b | ((reg & 1u) << bit));
            fb = (reg & 1u) ^ ((reg >> 1) & 1u);
            reg = (reg >> 1) | (fb << 14);
        }
        scrambler[i] = b;
    }
}

static long file_sectors(const char *path, int sector_size)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)(st.st_size / sector_size) : -1;
}

static int parse_cue(const char *cue)
{
    char line[1024], dir[1024], curfile[1024] = "";
    const char *slash = strrchr(cue, '/');
    FILE *f = fopen(cue, "r");
    int i;

    if (!f) { perror(cue); return -1; }
    if (slash) { memcpy(dir, cue, (size_t)(slash - cue + 1)); dir[slash - cue + 1] = '\0'; }
    else dir[0] = '\0';

    while (fgets(line, sizeof line, f)) {
        char *p = line, *q;
        while (*p == ' ' || *p == '\t') p++;
        if (strncasecmp(p, "FILE", 4) == 0) {
            p = strchr(p, '"');
            q = p ? strchr(p + 1, '"') : NULL;
            if (!p || !q) continue;
            *q = '\0';
            snprintf(curfile, sizeof curfile, "%s%s", dir, p + 1);
        } else if (strncasecmp(p, "TRACK", 5) == 0 && ntracks < MAX_TRACKS) {
            track *t = &tracks[ntracks++];
            memset(t, 0, sizeof *t);
            t->number = atoi(p + 6);
            t->audio = strstr(p, "AUDIO") != NULL;
            t->raw = strstr(p, "/2352") != NULL;
            t->start = -1;
            snprintf(t->file, sizeof t->file, "%s", curfile);
        } else if (strncasecmp(p, "INDEX 01", 8) == 0 && ntracks) {
            int m, s, fr;
            if (sscanf(p + 8, "%d:%d:%d", &m, &s, &fr) == 3) tracks[ntracks - 1].start = (long)m * 4500 + s * 75 + fr;
        }
    }
    fclose(f);
    for (i = 0; i < ntracks; i++) {
        int sz = (tracks[i].audio || tracks[i].raw) ? RAW : 2048;
        if (tracks[i].start < 0) { fprintf(stderr, "track %d has no INDEX 01\n", tracks[i].number); return -1; }
        if (i + 1 < ntracks && strcmp(tracks[i + 1].file, tracks[i].file) == 0) tracks[i].end = tracks[i + 1].start;
        else tracks[i].end = file_sectors(tracks[i].file, sz);
        if (tracks[i].end <= tracks[i].start) { fprintf(stderr, "cannot size track %d\n", tracks[i].number); return -1; }
    }
    return ntracks ? 0 : -1;
}

static int write_iso(const track *t, const char *out)
{
    unsigned char sec[RAW];
    int sz = t->raw ? RAW : 2048;
    long i;
    FILE *in = fopen(t->file, "rb"), *o = fopen(out, "wb");
    if (!in || !o) { perror("iso"); if (in) fclose(in); if (o) fclose(o); return -1; }
    fseek(in, t->start * sz, SEEK_SET);
    for (i = t->start; i < t->end; i++) {
        if (fread(sec, 1, (size_t)sz, in) != (size_t)sz) break;
        fwrite(t->raw ? sec + 16 : sec, 1, 2048, o);   /* MODE1: user data at 16..2063 */
    }
    fclose(in);
    fclose(o);
    printf("track %02d  data   %6ld sectors -> %s\n", t->number, t->end - t->start, out);
    printf("          for an exact TOC set MW2_CDA_DATA_SECTORS=%ld\n", t->end - t->start - 150);
    return 0;
}

static void put16(FILE *f, unsigned v) { fputc((int)(v & 255), f); fputc((int)((v >> 8) & 255), f); }
static void put32(FILE *f, unsigned long v) { put16(f, (unsigned)(v & 0xffff)); put16(f, (unsigned)(v >> 16)); }

static int clamp16(long v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int)v; }

static int cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return (x > y) - (x < y);
}

static long median_of(long *w, int n)
{
    qsort(w, (size_t)n, sizeof *w, cmp_long);
    return w[n / 2];
}

/* Conceal clicks in v[0..n) (one channel, stride 2): a sample is bad when it
 * sits far from the median of its 7 neighbours, relative to the local level of
 * such deviations. Bad samples (widened by one on each side) are replaced by
 * linear interpolation. Three passes. Returns samples changed. */
static long conceal(short *pcm, long n, int c)
{
    long *v = malloc((size_t)n * sizeof *v), *res = malloc((size_t)n * sizeof *res);
    unsigned char *bad = malloc((size_t)n);
    long w[33], changed = 0, i;
    int pass, k;

    if (!v || !res || !bad) { free(v); free(res); free(bad); return 0; }
    for (i = 0; i < n; i++) v[i] = pcm[i * 2 + c];
    for (pass = 0; pass < 3; pass++) {
        for (i = 0; i < n; i++) {
            for (k = -3; k <= 3; k++) {
                long j = i + k < 0 ? 0 : i + k >= n ? n - 1 : i + k;
                w[k + 3] = v[j];
            }
            res[i] = labs(v[i] - median_of(w, 7));
        }
        memset(bad, 0, (size_t)n);
        for (i = 0; i < n; i++) {
            long lvl, thr;
            for (k = -16; k <= 16; k++) {
                long j = i + k < 0 ? 0 : i + k >= n ? n - 1 : i + k;
                w[k + 16] = res[j];
            }
            lvl = median_of(w, 33);
            thr = 6 * lvl > 1200 ? 6 * lvl : 1200;
            if (res[i] > thr) {
                bad[i] = 1;
                if (i > 0) bad[i - 1] = 1;
                if (i + 1 < n) bad[i + 1] = 1;
            }
        }
        for (i = 0; i < n;) {
            long a, b;
            if (!bad[i]) { i++; continue; }
            a = i;
            while (i < n && bad[i]) i++;
            b = i; /* bad run [a, b) */
            for (k = 0; a + k < b; k++) {
                long left = a > 0 ? v[a - 1] : (b < n ? v[b] : 0);
                long right = b < n ? v[b] : left;
                long nv = left + (right - left) * (k + 1) / (b - a + 1);
                if (nv != v[a + k]) changed++;
                v[a + k] = nv;
            }
        }
    }
    for (i = 0; i < n; i++) pcm[i * 2 + c] = (short)clamp16(v[i]);
    free(v); free(res); free(bad);
    return changed;
}

static int write_wav(const track *t, const char *out)
{
    unsigned char sec[RAW];
    long n = t->end - t->start, i, damaged = 0, changed = 0;
    short *pcm = malloc((size_t)n * FRAMES_PER_SECTOR * 2 * sizeof(short));
    unsigned char *dmg = calloc((size_t)n, 1);
    FILE *in = fopen(t->file, "rb"), *o;
    int k, c;

    if (!pcm || !dmg || !in) { perror("wav"); free(pcm); free(dmg); if (in) fclose(in); return -1; }
    fseek(in, t->start * RAW, SEEK_SET);
    for (i = 0; i < n; i++) {
        short *p = pcm + i * FRAMES_PER_SECTOR * 2;
        if (fread(sec, 1, RAW, in) != RAW) memset(sec, 0, RAW);
        if (memcmp(sec, SYNC, 12) == 0) {
            dmg[i] = 1;
            damaged++;
            for (k = 12; k < RAW; k++) sec[k] ^= scrambler[k - 12];
        }
        for (k = 0; k < FRAMES_PER_SECTOR * 2; k++) p[k] = (short)(sec[k * 2] | (sec[k * 2 + 1] << 8));
        if (dmg[i]) {
            /* frames 0-2 were overwritten by the sync pattern: bridge from the previous sector */
            for (c = 0; c < 2; c++) {
                long a = i ? p[-2 + c] : 0, b = p[3 * 2 + c];
                for (k = 0; k < 3; k++) p[k * 2 + c] = (short)(a + (b - a) * (k + 1) / 4);
            }
        }
    }
    fclose(in);

    /* click concealment over each contiguous run of damaged sectors (+1 sector margin) */
    for (i = 0; i < n;) {
        long a, b;
        if (!dmg[i]) { i++; continue; }
        a = i;
        while (i < n && dmg[i]) i++;
        b = i;
        if (a > 0) a--;
        if (b < n) b++;
        for (c = 0; c < 2; c++)
            changed += conceal(pcm + a * FRAMES_PER_SECTOR * 2, (b - a) * FRAMES_PER_SECTOR, c);
    }

    o = fopen(out, "wb");
    if (!o) { perror(out); free(pcm); free(dmg); return -1; }
    fwrite("RIFF", 1, 4, o); put32(o, 36 + (unsigned long)n * RAW);
    fwrite("WAVEfmt ", 1, 8, o); put32(o, 16); put16(o, 1); put16(o, 2);
    put32(o, 44100); put32(o, 44100 * 4); put16(o, 4); put16(o, 16);
    fwrite("data", 1, 4, o); put32(o, (unsigned long)n * RAW);
    for (i = 0; i < n * FRAMES_PER_SECTOR * 2; i++) put16(o, (unsigned)(unsigned short)pcm[i]);
    fclose(o);
    free(pcm);
    free(dmg);

    printf("track %02d  audio  %6.1f s", t->number, (double)n / 75.0);
    if (damaged)
        printf("  REPAIRED %ld damaged sectors (first %.1f s); %ld samples reconstructed",
               damaged, (double)damaged / 75.0, changed);
    printf("\n");
    return 0;
}

int main(int argc, char **argv)
{
    char path[2048];
    int i, rc = 0;

    if (argc != 3) { fprintf(stderr, "usage: %s disc.cue OUTDIR\n", argv[0]); return 2; }
    build_scrambler();
    if (parse_cue(argv[1]) != 0) return 1;
    mkdir(argv[2], 0755);
    snprintf(path, sizeof path, "%s/music", argv[2]);
    mkdir(path, 0755);
    for (i = 0; i < ntracks && rc == 0; i++) {
        if (tracks[i].audio) {
            snprintf(path, sizeof path, "%s/music/Track%02d.wav", argv[2], tracks[i].number);
            rc = write_wav(&tracks[i], path);
        } else {
            snprintf(path, sizeof path, "%s/mw2cd.iso", argv[2]);
            rc = write_iso(&tracks[i], path);
        }
    }
    return rc != 0;
}
