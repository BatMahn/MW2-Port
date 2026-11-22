/* test_port - unit tests for cdaudio + datapath. Usage: test_port MUSICDIR INSTALLDIR [CD.iso] */
#include "cdaudio.h"
#include "datapath.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   %s\n", msg); else { printf("  FAIL %s\n", msg); fails++; } } while (0)

/* dominant frequency of the left channel via zero crossings */
static double freq(const float *b, size_t frames)
{
    size_t i, z = 0;
    for (i = 1; i < frames; i++) if ((b[(i - 1) * 2] < 0) != (b[i * 2] < 0)) z++;
    return (double)z * CDA_SAMPLE_RATE / (2.0 * (double)frames);
}
static double peak(const float *b, size_t frames)
{
    double m = 0; size_t i;
    for (i = 0; i < frames * 2; i++) if (fabs(b[i]) > m) m = fabs(b[i]);
    return m;
}

static void test_cue_sector_sizes(void)
{
    char dir[] = "/tmp/mw2-cue-XXXXXX", cue[128], data[128], audio[128];
    unsigned char data_sector[2048] = {0}, audio_sector[2352];
    float sample[2];
    if (!mkdtemp(dir)) { CHECK(0, "create CUE test directory"); return; }
    snprintf(cue, sizeof cue, "%s/disc.cue", dir);
    snprintf(data, sizeof data, "%s/data.bin", dir);
    snprintf(audio, sizeof audio, "%s/audio.bin", dir);
    for (size_t i = 0; i < sizeof audio_sector; i += 2) {
        audio_sector[i] = 0;
        audio_sector[i + 1] = 16;
    }
    for (int mixed = 0; mixed <= 1; mixed++) {
        cdaudio *disc = NULL;
        uint32_t start = 0, leadout = 0;
        FILE *f = fopen(data, "wb");
        if (!f) { CHECK(0, "create CUE data track"); break; }
        CHECK(fwrite(data_sector, sizeof data_sector, 1, f) == 1, "write a 2048-byte data sector");
        if (!mixed) { fclose(f); f = fopen(audio, "wb"); }
        if (!f) { CHECK(0, "create CUE audio track"); break; }
        CHECK(fwrite(audio_sector, sizeof audio_sector, 1, f) == 1, "write a 2352-byte audio sector");
        fclose(f);
        f = fopen(cue, "w");
        if (!f) { CHECK(0, "create CUE index"); break; }
        fprintf(f, "FILE \"data.bin\" BINARY\n TRACK 01 MODE1/2048\n INDEX 01 00:00:00\n%s"
                " TRACK 02 AUDIO\n PREGAP 00:02:00\n INDEX 01 00:00:%02d\n",
                mixed ? "" : "FILE \"audio.bin\" BINARY\n", mixed);
        fclose(f);
        if (cdaudio_open(&disc, cue, NULL) != 0) { CHECK(0, "open CUE with both sector sizes"); continue; }
        cdaudio_track_info(disc, 2, &start, NULL);
        cdaudio_disc_info(disc, NULL, NULL, &leadout);
        CHECK(cdaudio_track_length(disc, 1) == 1 && start == 151 && leadout == 152,
              "mixed sector widths and pregap produce the correct disc addresses");
        CHECK(cdaudio_play_track(disc, 2) == 0 && cdaudio_render(disc, sample, 1) == 1 &&
              fabsf(sample[0] - 0.125f) < 0.001f, "audio starts at the first PCM sample");
        cdaudio_close(disc);
    }
    remove(cue);
    remove(data);
    remove(audio);
    rmdir(dir);
}

int main(int argc, char **argv)
{
    static float buf[CDA_SAMPLE_RATE * 2];
    cdaudio_options o = {0, 0, 0};
    cdaudio *cd;
    cdaudio_status st;
    uint32_t s2, s3, s1;
    char path[2048];
    FILE *f;
    size_t got;

    if (argc < 3) return 2;
    printf("cdaudio\n");
    CHECK(cdaudio_open(&cd, argv[1], &o) == 0, "open rip folder");
    cdaudio_track_info(cd, 1, &s1, NULL);
    cdaudio_track_info(cd, 2, &s2, NULL);
    cdaudio_track_info(cd, 3, &s3, NULL);
    CHECK(cda_msf_to_hsg(cda_hsg_to_msf(s3)) == s3, "MSF<->HSG round trip");

    /* play the last second of track 2 straight into track 3, as a CD would */
    cdaudio_play(cd, s3 - 75, 150);
    got = cdaudio_render(cd, buf, CDA_SAMPLE_RATE / 2);
    CHECK(fabs(freq(buf, got) - 440) < 5, "range starts in track 2 (440 Hz)");
    cdaudio_render(cd, buf, CDA_SAMPLE_RATE / 2);
    got = cdaudio_render(cd, buf, CDA_SAMPLE_RATE / 2);
    CHECK(fabs(freq(buf, got) - 660) < 5, "playback continues across the boundary into track 3 (660 Hz)");
    cdaudio_get_status(cd, &st);
    CHECK(st.track == 3, "status reports track 3");

    /* stop = pause, resume continues from the same sector */
    cdaudio_play(cd, s2, 525);
    cdaudio_render(cd, buf, CDA_SAMPLE_RATE / 2);
    cdaudio_stop(cd);
    cdaudio_get_status(cd, &st);
    CHECK(!st.busy && st.paused && st.position == s2 + 37, "first stop pauses at the current sector");
    got = cdaudio_render(cd, buf, 1000);
    CHECK(got == 0 && peak(buf, 1000) == 0, "paused drive outputs silence");
    CHECK(cdaudio_resume(cd) == 0, "resume accepted");
    cdaudio_get_status(cd, &st);
    CHECK(st.busy && st.position == s2 + 37, "resumes from the paused sector");
    cdaudio_stop(cd); cdaudio_stop(cd);
    CHECK(cdaudio_resume(cd) != 0, "second stop resets; resume refused");

    /* busy clears exactly at the end of the requested range */
    cdaudio_play(cd, s2, 75);
    got = cdaudio_render(cd, buf, CDA_SAMPLE_RATE);
    cdaudio_get_status(cd, &st);
    CHECK(got == 75 * CDA_FRAMES_PER_SECTOR && !st.busy, "1-second request renders 1 s, then busy clears");

    /* play request starting on the data track skips to the first audio */
    cdaudio_play(cd, s1 + 100, (s3 - s1));
    got = cdaudio_render(cd, buf, CDA_SAMPLE_RATE / 4);
    CHECK(fabs(freq(buf, got) - 440) < 5, "request starting on data track plays from track 2");

    /* volume */
    cdaudio_play(cd, s2, 75);
    got = cdaudio_render(cd, buf, CDA_SAMPLE_RATE / 4);
    { double full = peak(buf, got);
      cdaudio_set_volume(cd, 128, 0);
      got = cdaudio_render(cd, buf, CDA_SAMPLE_RATE / 4);
      { double l = 0, r = 0; size_t i;
        for (i = 0; i < got; i++) { if (fabs(buf[i*2]) > l) l = fabs(buf[i*2]); if (fabs(buf[i*2+1]) > r) r = fabs(buf[i*2+1]); }
        CHECK(fabs(l / full - 128.0 / 255.0) < 0.02 && r == 0, "per-channel volume (L=128, R=0)"); } }
    cdaudio_close(cd);
    test_cue_sector_sizes();

    printf("datapath (against the real install)\n");
    CHECK(dp_add_root(argv[2]) == 0, "add install folder as root");
    CHECK(dp_resolve("MW2.PRJ", path, sizeof path) == 0, "resolve MW2.PRJ");
    CHECK(dp_resolve("d:\\smk\\aiagrid.smk", path, sizeof path) == 0 && strstr(path, "SMK/AIAGRID.SMK"),
          "lowercase DOS path with drive letter finds SMK/AIAGRID.SMK");
    CHECK(dp_resolve("..\\etc\\passwd", path, sizeof path) != 0, "refuses to escape the root");
    CHECK(dp_resolve("MEK\\NOPE.MEK", path, sizeof path) != 0, "missing file reports not found");
    f = dp_fopen("SAVES\\..", "rb");
    CHECK(f == NULL, "no directory traversal through fopen");
    f = dp_fopen("PORTTEST.TMP", "wb");
    CHECK(f != NULL, "write creates a new file in the first root");
    if (f) { fclose(f); dp_resolve("porttest.tmp", path, sizeof path); remove(path); }

    if (argc > 3) {
        unsigned char *a = NULL, *b = NULL;
        size_t la = 0, lb = 0;
        char inst[2048];
        printf("ISO 9660 (against the disc image)\n");
        dp_clear_roots();
        CHECK(dp_add_iso_root(argv[3], "MECH2") == 0, "open image, MECH2 folder as root");
        CHECK(dp_add_iso_root(argv[3], NULL) == 0, "open image, disc root");
        CHECK(dp_add_iso_root(argv[3], "NOPE") != 0, "missing subfolder rejected");
        CHECK(dp_read_file("mw2.prj", &a, &la) == 0 && la == 19960257, "MW2.PRJ read from image (lowercase name)");
        snprintf(inst, sizeof inst, "%s/MW2.PRJ", argv[2]);
        f = fopen(inst, "rb");
        if (f) { b = malloc(la); lb = fread(b, 1, la, f); fclose(f); }
        CHECK(b && lb == la && memcmp(a, b, la) == 0, "image copy byte-identical to install copy");
        free(a); free(b); a = b = NULL;
        CHECK(dp_read_file("d:\\smk\\aiagrid.smk", &a, &la) == 0 && la > 0, "SMK video from disc root, via drive-letter path");
        free(a); a = NULL;
        CHECK(dp_read_file("KEATING\\TRN1_01S.SFL", &a, &la) == 0 && la == 203651, "training audio KEATING\\TRN1_01S.SFL");
        free(a); a = NULL;
        CHECK(dp_read_file("MECH2\\..\\..\\etc", &a, &la) != 0, "no traversal out of the image");
        CHECK(dp_read_file("SMK", &a, &la) != 0, "directories are not read as files");
        dp_clear_roots();
        dp_add_root(argv[2]);
        dp_add_iso_root(argv[3], NULL);
        CHECK(dp_resolve("MW2.PRJ", path, sizeof path) == 0 && strstr(path, argv[2]) == path,
              "install folder takes priority over the image");
    }

    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails != 0;
}
