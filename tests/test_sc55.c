/* test_sc55: the Roland SC-55 menu music without the firmware (none can be shipped or tested here):
 *   - the device choice (portcfg_menu_music): menu_music= preferred when its files are there, else MT-32, SC-55, GUS;
 *   - menu_music= and sc55= round-trip through the settings file, and the game folder's sc55/ is found;
 *   - the ROM set detection (sc55_detect): no files / an incomplete set -> not offered; upstream's names, any case;
 *     the JV-880's set is not taken (no General MIDI module);
 *   - the whole path on stub ROM sets (zero-filled, the right sizes): load, boot on the thread, a song from DATABASE.MW2
 *     (the General MIDI set, entry 66) played and rendered, stopped, closed, opened again - no crash; wrong sizes -> NULL;
 *   - the core's cost per emulated second (all 28 voice slots on; the MCUs run the stub ROMs, not the firmware).
 *   ./test_sc55 [MW2 install folder]   (the folder for DATABASE.MW2; without it a built-in XMIDI song) */
#include "portcfg.h"
#include "sc55mid.h"
#include "shelldb.h"
#include "nuked-sc55/sc55_core.h"
#include "sdlcompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static char g_base[512];

/* the core prints its diagnostics ("Unknown read ...": the stub ROMs are no firmware) to stdout: hidden in the stub runs */
static int g_stdout = -1;
static void quiet(int on)
{
    fflush(stdout);
    if (on) { int nul = open("/dev/null", O_WRONLY); g_stdout = dup(1); if (nul >= 0) { dup2(nul, 1); close(nul); } }
    else if (g_stdout >= 0) { dup2(g_stdout, 1); close(g_stdout); g_stdout = -1; }
}

static void mkfile(const char *dir, const char *name, long size)
{
    char p[1100];
    FILE *f;
    static char zero[65536];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    if (!(f = fopen(p, "wb"))) { fprintf(stderr, "FAIL: cannot write %s\n", p); fails++; return; }
    while (size > 0) { long n = size > (long)sizeof zero ? (long)sizeof zero : size; fwrite(zero, 1, (size_t)n, f); size -= n; }
    fclose(f);
}

static void rmfile(const char *dir, const char *name)
{
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    remove(p);
}

static void mkd(const char *rel, char *out)
{
    snprintf(out, 600, "%s/%s", g_base, rel);
    mkdir(out, 0755);
}

/* a minimal XMIDI song: FORM XMID, one note (C4, 60 ticks), a program change, end */
static const uint8_t TINY_XMI[] = {
    'F', 'O', 'R', 'M', 0, 0, 0, 36, 'X', 'M', 'I', 'D',
    'E', 'V', 'N', 'T', 0, 0, 0, 12,
    0xc0, 0x00,                 /* program 0 */
    0x90, 60, 100, 60,          /* note on, duration 60 ticks */
    60,                         /* interval 60 */
    0xff, 0x2f, 0x00,           /* end */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static void test_choice(void)
{
    int have[PORTCFG_MM_COUNT];
    int i;
    struct { int mt, sc, gus, pref, want; } T[] = {
        {0, 0, 0, PORTCFG_MM_AUTO, PORTCFG_MM_AUTO},   /* nothing: no menu music */
        {1, 1, 1, PORTCFG_MM_AUTO, PORTCFG_MM_MT32},   /* auto: the MT-32 first (the default as before) */
        {0, 1, 1, PORTCFG_MM_AUTO, PORTCFG_MM_SC55},   /* then the SC-55 */
        {0, 0, 1, PORTCFG_MM_AUTO, PORTCFG_MM_GUS},    /* then the GUS */
        {1, 1, 1, PORTCFG_MM_SC55, PORTCFG_MM_SC55},   /* chosen and present */
        {1, 1, 1, PORTCFG_MM_GUS, PORTCFG_MM_GUS},
        {1, 0, 1, PORTCFG_MM_SC55, PORTCFG_MM_MT32},   /* chosen, no ROMs: not offered - auto's choice */
        {0, 0, 1, PORTCFG_MM_SC55, PORTCFG_MM_GUS},
        {0, 0, 0, PORTCFG_MM_SC55, PORTCFG_MM_AUTO},
        {0, 1, 0, PORTCFG_MM_MT32, PORTCFG_MM_SC55},
    };
    for (i = 0; i < (int)(sizeof T / sizeof T[0]); i++) {
        int got;
        memset(have, 0, sizeof have);
        have[PORTCFG_MM_MT32] = T[i].mt; have[PORTCFG_MM_SC55] = T[i].sc; have[PORTCFG_MM_GUS] = T[i].gus;
        got = portcfg_menu_music(T[i].pref, have);
        CHECK(got == T[i].want, "choice %d: mt32 %d sc55 %d gus %d pref %s -> %s, wanted %s", i, T[i].mt, T[i].sc, T[i].gus,
              PORTCFG_MM_NAME[T[i].pref], PORTCFG_MM_NAME[got], PORTCFG_MM_NAME[T[i].want]);
    }
}

static void test_settings(void)
{
    char cfg[700], game[600], d[700];
    portcfg c, r;
    int k;
    snprintf(cfg, sizeof cfg, "%s/mw2port.cfg", g_base);
    setenv("MW2_CONFIG", cfg, 1);
    for (k = 0; k < PORTCFG_MM_COUNT; k++) {   /* menu_music= round trip, sc55= kept without a game folder */
        portcfg_defaults(&c);
        CHECK(c.menu_music == PORTCFG_MM_AUTO, "default menu_music not auto");
        c.menu_music = k;
        snprintf(c.sc55, sizeof c.sc55, "/roms/sc-55 mk2");
        CHECK(portcfg_save(NULL, &c) == 0, "save");
        CHECK(portcfg_load(NULL, &r) == 0, "load");
        CHECK(r.menu_music == k, "menu_music=%s read back as %d", PORTCFG_MM_NAME[k], r.menu_music);
        CHECK(strcmp(r.sc55, "/roms/sc-55 mk2") == 0, "sc55= read back as '%s'", r.sc55);
    }
    {   /* an unknown value: auto */
        FILE *f = fopen(cfg, "w");
        if (f) { fprintf(f, "menu_music=opl3\n"); fclose(f); }
        portcfg_load(NULL, &r);
        CHECK(r.menu_music == PORTCFG_MM_AUTO, "menu_music=opl3 not auto");
    }
    /* the game folder's SC55/ (any case) */
    mkd("game", game); mkd("game/install", d); mkd("game/SC55", d);
    portcfg_defaults(&c);
    CHECK(portcfg_resolve(&c, NULL, game) == 0, "resolve %s", game);
    CHECK(strstr(c.sc55, "/game/SC55") != NULL, "sc55/ not found: '%s'", c.sc55);
    rmdir(d);
    snprintf(d, sizeof d, "%s/install", game); rmdir(d);
    rmdir(game);
    remove(cfg);
}

static const char *MK2[] = {"ROM1.BIN", "rom2.bin", "WaveRom1.bin", "waverom2.bin", "Rom_SM.bin"};
static const long MK2_SZ[] = {0x8000, 0x80000, 0x200000, 0x100000, 0x1000};
static const char *MK1[] = {"sc55_rom1.bin", "SC55_ROM2.BIN", "sc55_waverom1.bin", "sc55_waverom2.bin", "sc55_waverom3.bin"};
static const long MK1_SZ[] = {0x8000, 0x80000, 0x100000, 0x100000, 0x100000};

static void test_detect(void)
{
    char d[600], model[32];
    int i, m;
    mkd("roms", d);
    CHECK(sc55_detect(d, model, sizeof model) < 0 && !model[0], "empty folder detected as %s", model);
    CHECK(sc55_detect(NULL, NULL, 0) < 0, "NULL folder");
    CHECK(sc55_open(d, 44100) == NULL, "opened an empty folder");
    for (i = 0; i < 4; i++) mkfile(d, MK2[i], 16);
    CHECK(sc55_detect(d, model, sizeof model) < 0, "incomplete SC-55mk2 set detected as %s", model);
    mkfile(d, MK2[4], 16);
    m = sc55_detect(d, model, sizeof model);
    CHECK(m >= 0 && strcmp(model, "SC-55mk2") == 0, "SC-55mk2 set (mixed case) -> %d '%s'", m, model);
    CHECK(sc55_open(d, 44100) == NULL, "opened a set of the wrong sizes");
    for (i = 0; i < 5; i++) rmfile(d, MK2[i]);
    for (i = 0; i < 5; i++) mkfile(d, MK1[i], 16);
    m = sc55_detect(d, model, sizeof model);
    CHECK(m >= 0 && strcmp(model, "SC-55mk1") == 0, "SC-55 (mk1) set -> %d '%s'", m, model);
    for (i = 0; i < 5; i++) rmfile(d, MK1[i]);
    mkfile(d, "jv880_rom1.bin", 16); mkfile(d, "jv880_rom2.bin", 16); mkfile(d, "jv880_waverom1.bin", 16); mkfile(d, "jv880_waverom2.bin", 16);
    mkfile(d, "jv880_waverom_expansion.bin", 16);
    CHECK(sc55_detect(d, model, sizeof model) < 0, "JV-880 set taken as %s", model);
    rmfile(d, "jv880_rom1.bin"); rmfile(d, "jv880_rom2.bin"); rmfile(d, "jv880_waverom1.bin"); rmfile(d, "jv880_waverom2.bin");
    rmfile(d, "jv880_waverom_expansion.bin");
    rmdir(d);
}

/* the mixer's side: 1024 frames per call at 44.1 kHz, in real time */
static void pull(sc55_synth *s, double secs, double *peak)
{
    static float buf[2048];
    int n, i;
    for (n = 0; n < (int)(secs * 44100 / 1024); n++) {
        memset(buf, 0, sizeof buf);
        sc55_render(s, buf, 1024);
        for (i = 0; i < 2048; i++) if (buf[i] > *peak || -buf[i] > *peak) *peak = buf[i] > 0 ? buf[i] : -buf[i];
        SDL_Delay(23);
    }
}

static void test_stub_run(const char *mw2, const char *set_name, const char **names, const long *sizes)
{
    char d[600];
    uint8_t *song = NULL;
    long n = 0;
    shelldb db;
    sc55_synth *s;
    int i, round;
    memset(&db, 0, sizeof db);
    if (mw2) {
        char p[1100];
        snprintf(p, sizeof p, "%s/DATABASE.MW2", mw2);
        if (shelldb_open(p, &db) == 0) n = shelldb_entry(&db, 66, &song);   /* the GM set's first song (0x23 + 32 - 1) */
        CHECK(n > 0, "DATABASE.MW2 entry 66");
    }
    mkd("stub", d);
    for (i = 0; i < 5; i++) mkfile(d, names[i], sizes[i]);
    for (round = 0; round < 2; round++) {   /* twice: the core is reloaded after a close */
        double peak = 0, emu = 0, cpu = 0, boot;
        long under = 0;
        Uint64 t0 = SDL_GetTicks();
        s = sc55_open(d, 44100);
        CHECK(s != NULL, "%s stub set did not open", set_name);
        if (!s) break;
        CHECK(strcmp(sc55_model(s), set_name) == 0, "model %s", sc55_model(s));
        CHECK(sc55_open(d, 44100) == NULL, "a second unit opened");
        while (!sc55_booted(s) && SDL_GetTicks() - t0 < 20000) SDL_Delay(10);
        boot = (double)(SDL_GetTicks() - t0) / 1000.0;
        CHECK(sc55_booted(s), "%s: not booted after 20 s", set_name);
        CHECK(sc55_play(s, n > 0 ? song : TINY_XMI, n > 0 ? (size_t)n : sizeof TINY_XMI) == 0, "play");
        CHECK(sc55_play(s, (const uint8_t *)"not xmidi", 9) != 0, "played a non-XMIDI file");
        pull(s, 1.5, &peak);
        sc55_play(s, TINY_XMI, sizeof TINY_XMI);   /* a song change */
        pull(s, 0.5, &peak);
        sc55_stop(s);
        pull(s, 0.3, &peak);
        sc55_stats(s, &under, &emu, &cpu);
        fprintf(stderr, "test_sc55: %s stub run %d: boot %.2f s, %.2f s emulated in %.2f s on the thread, %ld frames short, peak %.3f\n",
               set_name, round, boot, emu, cpu, under, peak);
        CHECK(emu > 1.0, "%s: the thread emulated only %.2f s", set_name, emu);
        sc55_close(s);
    }
    for (i = 0; i < 5; i++) rmfile(d, names[i]);
    rmdir(d);
    free(song);
    shelldb_close(&db);
}

/* the core's cost: emulated seconds per CPU second, on this thread */
static void bench(const char **names, const long *sizes, const char *set_name)
{
    char d[600], p[NSC55_FILES][1100];
    const char *pp[NSC55_FILES];
    static int16_t buf[2 * 4096];
    int i, model = -1, n, total;
    Uint64 t0;
    double secs;
    mkd("bench", d);
    for (i = 0; i < 5; i++) mkfile(d, names[i], sizes[i]);
    for (i = 0; i < nsc55_model_count(); i++) if (strcmp(nsc55_model_name(i), set_name) == 0) model = i;
    for (i = 0; i < NSC55_FILES; i++) {
        const char *f = nsc55_rom_name(model, i);
        int k;
        p[i][0] = 0;
        for (k = 0; f[0] && k < 5; k++) if (strcasecmp(names[k], f) == 0) snprintf(p[i], sizeof p[i], "%s/%s", d, names[k]);
        pp[i] = p[i][0] ? p[i] : NULL;
    }
    CHECK(nsc55_open(model, pp) == 0, "bench open");
    nsc55_test_all_voices();
    total = nsc55_rate() * 3;
    t0 = SDL_GetPerformanceCounter();
    for (n = 0; n < total; n += 4096) nsc55_render(buf, 4096);
    secs = (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency();
    fprintf(stderr, "test_sc55: core cost (%s, stub ROMs, 28 voice slots): %.2f CPU s per emulated s (%.0f%% of one core in real time)\n",
           set_name, secs / 3.0, secs / 3.0 * 100.0);
    nsc55_close();
    for (i = 0; i < 5; i++) rmfile(d, names[i]);
    rmdir(d);
}

int main(int argc, char **argv)
{
    const char *mw2 = argc > 1 ? argv[1] : NULL;
    snprintf(g_base, sizeof g_base, "/tmp/test_sc55_%d", (int)getpid());
    mkdir(g_base, 0755);
    test_choice();
    test_settings();
    test_detect();
    quiet(1);
    test_stub_run(mw2, "SC-55mk2", MK2, MK2_SZ);
    test_stub_run(NULL, "SC-55mk1", MK1, MK1_SZ);
    if (getenv("MW2_SC55_BENCH")) { bench(MK2, MK2_SZ, "SC-55mk2"); bench(MK1, MK1_SZ, "SC-55mk1"); }
    quiet(0);
    rmdir(g_base);
    printf("test_sc55: %s (no SC-55 firmware here: the emulated unit's sound itself is not tested)\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
