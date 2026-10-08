/* xmiplay - render MW2 shell XMIDI music through Gravis UltraSound patches to a WAV.
 *   xmiplay DATABASE.MW2 entry ULTRASND-dir out.wav [max seconds] [memory KB] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gusmid.h"
#include "shelldb.h"

int main(int argc, char **argv)
{
    shelldb db;
    uint8_t *x;
    long n;
    gus_bank *b;
    gus_song *s;
    int rate = 44100, maxs = argc > 5 ? atoi(argv[5]) : 90, mem = argc > 6 ? atoi(argv[6]) : 1024, chunk = 1024;
    float buf[2048];
    short *pcm;
    long total = 0, cap = (long)rate * maxs;
    FILE *o;
    double peak = 0;
    if (argc < 5) { fprintf(stderr, "usage: xmiplay DATABASE.MW2 entry ULTRASND-dir out.wav [seconds] [memory KB]\n"); return 2; }
    if (shelldb_open(argv[1], &db)) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    n = shelldb_entry(&db, atoi(argv[2]), &x);
    b = gus_bank_open(argv[3], mem);
    if (!b) { fprintf(stderr, "cannot read %s/MIDI/ULTRAMID.INI\n", argv[3]); return 1; }
    s = n > 0 ? gus_song_open(b, x, (size_t)n, rate) : NULL;
    if (!s) { fprintf(stderr, "entry %s is not XMIDI\n", argv[2]); return 1; }
    pcm = malloc((size_t)cap * 2 * sizeof *pcm);
    while (total < cap) {
        int k, more;
        memset(buf, 0, sizeof buf);
        more = gus_song_render(s, buf, chunk);
        for (k = 0; k < chunk * 2 && total * 2 + k < cap * 2; k++) {
            double v = buf[k];
            if (v > peak) peak = v;
            if (-v > peak) peak = -v;
            if (v > 1) v = 1;
            if (v < -1) v = -1;
            pcm[total * 2 + k] = (short)(v * 32767);
        }
        total += chunk;
        if (!more) break;
    }
    if (total > cap) total = cap;
    o = fopen(argv[4], "wb");
    {
        unsigned int dl = (unsigned int)(total * 4), sz = 36 + dl, fl = 16, sr = (unsigned)rate, br = (unsigned)rate * 4;
        unsigned short pcm1 = 1, ch = 2, ba = 4, bits = 16;
        fwrite("RIFF", 1, 4, o); fwrite(&sz, 4, 1, o); fwrite("WAVEfmt ", 1, 8, o); fwrite(&fl, 4, 1, o);
        fwrite(&pcm1, 2, 1, o); fwrite(&ch, 2, 1, o); fwrite(&sr, 4, 1, o); fwrite(&br, 4, 1, o); fwrite(&ba, 2, 1, o); fwrite(&bits, 2, 1, o);
        fwrite("data", 1, 4, o); fwrite(&dl, 4, 1, o); fwrite(pcm, 4, (size_t)total, o);
    }
    fclose(o);
    printf("%.1f s rendered, peak %.2f\n", (double)total / rate, peak);
    gus_song_close(s); gus_bank_close(b); free(pcm); free(x); shelldb_close(&db);
    return 0;
}
