/* shelltool - inspect MW2 shell data.
 *   shelltool list  DATABASE.MW2            entries, sizes, kinds
 *   shelltool image DATABASE.MW2 N out.ppm  export a PCX screen
 *   shelltool smk   FILE.SMK frame out.ppm  export one Smacker frame */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shelldb.h"
#include "smk.h"

static void ppm(const char *path, int w, int h, const uint8_t *pix, const uint8_t (*pal)[3])
{
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) fwrite(pal[pix[i]], 1, 3, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "list")) {
        shelldb db;
        int i;
        if (shelldb_open(argv[2], &db)) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
        for (i = 0; i < db.count; i++) {
            uint8_t *e;
            long n = shelldb_entry(&db, i, &e);
            const char *k = n < 4 ? "?" : e[0] == 0x0a ? "PCX image" : !memcmp(e, "RIFF", 4) ? "WAV sound" :
                            !memcmp(e, "FORM", 4) ? "XMIDI music" : !memcmp(e, "1.10", 4) ? "SHP sprites" :
                            !memcmp(e, "1.", 2) ? "font" : !memcmp(e, "MZ", 2) ? "DLL" : "data";
            printf("%3d %8ld %s%s\n", i, n, k, shelldb_is_compressed(&db, i) ? " (LZSS)" : "");
            free(e);
        }
        shelldb_close(&db);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "image")) {
        shelldb db;
        uint8_t *e;
        long n;
        shell_image im;
        int exact;
        if (shelldb_open(argv[2], &db)) return 1;
        n = shelldb_entry(&db, atoi(argv[3]), &e);
        if (n <= 0 || shell_pcx(e, (size_t)n, &im, &exact)) { fprintf(stderr, "entry is not a PCX image\n"); return 1; }
        ppm(argv[4], im.w, im.h, im.pix, (const uint8_t (*)[3])im.pal);
        printf("%dx%d%s\n", im.w, im.h, exact ? "" : " (run-length data did not end at the palette)");
        shell_image_free(&im); free(e); shelldb_close(&db);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "smk")) {
        smk *s = smk_open_file(argv[2]);
        int want = atoi(argv[3]), f = -1;
        if (!s) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
        while (f < want && f < smk_frames(s) - 1) f = smk_next(s);
        ppm(argv[4], smk_width(s), smk_height(s), smk_pixels(s), smk_palette(s));
        printf("%dx%d, %d frames, %.1f ms/frame; wrote frame %d\n", smk_width(s), smk_height(s), smk_frames(s), smk_frame_ms(s), f);
        smk_close(s);
        return 0;
    }
    fprintf(stderr, "usage: shelltool list DB | image DB N out.ppm | smk FILE frame out.ppm\n");
    return 2;
}
