/* cdtool - exercise the virtual CD drive
 *   cdtool toc    MUSICDIR
 *   cdtool render MUSICDIR TRACK OUT.wav [SECONDS]   offline, no audio device needed
 *   cdtool play   MUSICDIR TRACK                     through the default audio device
 */
#include "cdaudio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void put16(FILE *f, unsigned v) { fputc((int)(v & 0xff), f); fputc((int)((v >> 8) & 0xff), f); }
static void put32(FILE *f, unsigned long v) { put16(f, (unsigned)(v & 0xffff)); put16(f, (unsigned)(v >> 16)); }

static int render_wav(cdaudio *cd, int track, const char *path, double seconds)
{
    enum { CHUNK = 4096 };
    float buf[CHUNK * 2];
    uint32_t start;
    int audio;
    unsigned long frames_total = 0;
    FILE *f;
    cdaudio_status st;

    if (cdaudio_track_info(cd, track, &start, &audio) != 0 || !audio) {
        fprintf(stderr, "track %d is not an audio track\n", track);
        return 1;
    }
    /* Ask for the track the way the game does: a sector range from the TOC. */
    cdaudio_play(cd, start, seconds > 0 ? (uint32_t)(seconds * 75.0) : cdaudio_track_length(cd, track));

    f = fopen(path, "wb");
    if (!f) return 1;
    fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, f);
    put32(f, 16); put16(f, 1); put16(f, 2); put32(f, CDA_SAMPLE_RATE); put32(f, CDA_SAMPLE_RATE * 4);
    put16(f, 4); put16(f, 16);
    fwrite("data\0\0\0\0", 1, 8, f);

    for (;;) {
        size_t got = cdaudio_render(cd, buf, CHUNK), i;
        for (i = 0; i < got * 2; i++) {
            float s = buf[i] < -1.0f ? -1.0f : buf[i] > 1.0f ? 1.0f : buf[i];
            put16(f, (unsigned)(int16_t)(s * 32767.0f) & 0xffff);
        }
        frames_total += got;
        if (got < CHUNK) break;
    }
    cdaudio_get_status(cd, &st);
    fseek(f, 4, SEEK_SET); put32(f, 36 + frames_total * 4);
    fseek(f, 40, SEEK_SET); put32(f, frames_total * 4);
    fclose(f);
    printf("rendered %.2f s of track %d to %s (busy=%d at end)\n",
           (double)frames_total / CDA_SAMPLE_RATE, track, path, st.busy);
    return 0;
}

int main(int argc, char **argv)
{
    cdaudio_options opt = {0, 0, 1};
    cdaudio *cd;
    int rc = 0;

    if (argc < 3) { fprintf(stderr, "usage: %s toc|render|play MUSICDIR [args]\n", argv[0]); return 2; }
    if (getenv("MW2_CDA_TRACK_OFFSET")) opt.track_offset = atoi(getenv("MW2_CDA_TRACK_OFFSET"));
    if (cdaudio_open(&cd, argv[2], &opt) != 0) { fprintf(stderr, "no playable tracks in %s\n", argv[2]); return 1; }

    if (strcmp(argv[1], "toc") == 0) {
        int first, last;
        uint32_t leadout;
        cdaudio_disc_info(cd, &first, &last, &leadout);
        printf("tracks %d-%d, lead-out at sector %u\n", first, last, leadout);
    } else if (strcmp(argv[1], "render") == 0 && argc > 4) {
        rc = render_wav(cd, atoi(argv[3]), argv[4], argc > 5 ? atof(argv[5]) : 0);
    } else if (strcmp(argv[1], "play") == 0 && argc > 3) {
        cdaudio_status st;
        if (cdaudio_play_track(cd, atoi(argv[3])) != 0 || cdaudio_start_device(cd) != 0) {
            fprintf(stderr, "cannot play track %s\n", argv[3]);
            rc = 1;
        } else {
            do { sleep(1); cdaudio_get_status(cd, &st); } while (st.busy);
        }
    } else {
        fprintf(stderr, "bad command\n");
        rc = 2;
    }
    cdaudio_close(cd);
    return rc;
}
