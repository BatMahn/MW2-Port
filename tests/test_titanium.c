/* test_titanium: the Titanium look's data paths against the user's Titanium files (make check; skipped without them).
 *   ./test_titanium [TITANIUM_DIR]   (the folder with the 16-bit build's MW2.PRJ and SKYGND.PAR)
 * Texture decoding (tex.c TEX_TITANIUM: the inverted alpha, the plain records refused), the SKYGND.PAR reader
 * (skygnd.c skygnd_ti_lookup), the folder convention (portcfg.c portcfg_titanium) and the RENDERER / TEXTURES settings
 * (portcfg_texset_ok, portcfg_look; renderer= / textures=, the old edition= migrated). */
#include "portcfg.h"
#include "prj.h"
#include "skygnd.h"
#include "tex.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int decode(prj_archive *a, const char *name, texture *t, uint8_t *raw, size_t *rawlen)
{
    prj_record r;
    int rc;
    memset(t, 0, sizeof *t);
    if (prj_read_named(a, "CEL", name, &r) != PRJ_OK) return -2;
    if (raw && r.size <= *rawlen) { memcpy(raw, r.data, r.size); *rawlen = r.size; }
    rc = tex_decode(r.data, r.size, TEX_TITANIUM, t);
    prj_record_free(&r);
    return rc;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../MW2-game/titanium";
    char prj[1100], par[1100], err[256], cfg[64];
    static uint8_t raw[1 << 16];
    prj_archive *a;
    texture t;
    skygnd_ti s;
    portcfg pc;
    size_t rl;
    FILE *f;
    snprintf(prj, sizeof prj, "%s/MW2.PRJ", dir);
    snprintf(par, sizeof par, "%s/SKYGND.PAR", dir);
    if (!(f = fopen(prj, "rb"))) { printf("test_titanium: no %s - skipped\n", prj); return 0; }
    fclose(f);
    if (!(a = prj_open(prj, err, sizeof err))) { printf("FAIL: %s\n", err); return 1; }

    /* A9EXP328: type 3, 64 x 64, 255 entries; index 253 = (0, 0, 0, 255) - a transparent texel (the plugin's
     * A = 15 - (a >> 4)); index 0 = B 0x0c G 0x1c R 0x67 A 0x3b -> ARGB4444 R 6 G 1 B 0, A 15 - 3 = 12 */
    rl = sizeof raw;
    CHECK(decode(a, "A9EXP328", &t, raw, &rl) == 0 && t.w == 64 && t.h == 64, "A9EXP328 decodes 64 x 64");
    if (t.rgba) {
        size_t i, n = (size_t)t.w * (size_t)t.h, clear = 0;
        int found0 = 0;
        for (i = 0; i < n; i++) {
            unsigned ix = raw[12 + 255 * 4 + i];
            if (ix == 253) { clear++; CHECK((t.rgba[i] >> 24) == 0, "index 253 transparent (%08x)", t.rgba[i]); if ((t.rgba[i] >> 24)) break; }
            if (ix == 0 && !found0) { found0 = 1; CHECK(t.rgba[i] == (0x66u | 0x11u << 8 | 0x00u << 16 | 0xccu << 24), "index 0 = %08x", t.rgba[i]); }
        }
        CHECK(clear == 2681, "2681 transparent texels (%zu)", clear);
        tex_free(&t);
    }
    /* CYANGROU: type 0 - opaque whatever its palette alpha says, RGB565 */
    CHECK(decode(a, "CYANGROU", &t, NULL, NULL) == 0 && t.w == 128, "CYANGROU decodes");
    if (t.rgba) {
        size_t i, n = (size_t)t.w * (size_t)t.h, opaque = 0;
        for (i = 0; i < n; i++) opaque += (t.rgba[i] >> 24) == 255;
        CHECK(opaque == n, "type 0 opaque (%zu of %zu)", opaque, n);
        tex_free(&t);
    }
    CHECK(decode(a, "L1NOVACT", &t, NULL, NULL) == -1, "a plain record (L1NOVACT.XEL) is not a texture (mercsw 0x10002867)");
    CHECK(decode(a, "A9EXP222", &t, NULL, NULL) == -1, "a one-colour placeholder (A9EXP222) counts as missing");
    prj_close(a);

    /* SKYGND.PAR: cyan - fog=94,70,49 density=-5,0.17,0 sky=45,45,64 skyAnim=.01,.05, tile_ground 15000 / 128 */
    CHECK(skygnd_ti_lookup(par, "CYAN", &s) == 0, "cyan in SKYGND.PAR");
    CHECK(s.fog[0] == 94 && s.fog[1] == 70 && s.fog[2] == 49 && s.sky[0] == 45 && s.sky[2] == 64, "cyan colours");
    CHECK(fabsf(s.dens_y + 5) < 1e-4f && fabsf(s.dens_m - 0.17f) < 1e-4f && s.dens_z == 0, "cyan density");
    CHECK(fabsf(s.tile_ground - 15000.0f / 128.0f) < 1e-3f && s.ground_max == 320000.0f, "cyan ground");
    CHECK(fabsf(s.anim[1] * 181.0f - 0.05f) < 1e-5f, "cyan skyAnim");
    /* the lights (0x10053dc5..0x10053ec0): amb, wa, wb with their type (D -> 1 directional, P -> 2 point) and vector */
    CHECK(s.amb[0] == 30 && s.amb[2] == 35 && s.wa[0] == 94 && s.wa[2] == 49 && s.wb[1] == 70, "cyan amb / wa / wb");
    CHECK(s.wa_type == 1 && s.wb_type == 1 && s.wa_vec[0] == -1 && s.wa_vec[1] == -1 && s.wa_vec[2] == 0 && s.wb_vec[0] == 1 && s.wb_vec[1] == 1,
          "cyan light types and vectors");
    CHECK(s.shadowtable == 7 && s.gclip[0] == 5000 && s.grounddelta == 60, "cyan shadowtable / gclip / grounddelta (read, unused)");
    CHECK(skygnd_ti_lookup(par, "YELL", &s) == 0 && s.dens_z == 150000.0f, "yell: density Z x 10 = 150000 (%g)", s.dens_z);
    CHECK(skygnd_ti_lookup(par, "WHIT", &s) == 0 && s.sky[0] == 255, "whit: the commented-out line is skipped");

    /* the folder convention: titanium= in the configuration, or the game folder's titanium/; the old edition=titanium
     * read as renderer=enhanced textures=titanium and saved as the two keys */
    snprintf(cfg, sizeof cfg, "/tmp/test_titanium_%d.cfg", (int)getpid());
    if ((f = fopen(cfg, "w"))) { fprintf(f, "game=/nonexistent\ntitanium=%s\nedition=titanium\n", dir); fclose(f); }
    setenv("MW2_CONFIG", cfg, 1);
    portcfg_load(NULL, &pc);
    CHECK(pc.renderer == PORTCFG_ED_ENHANCED && pc.texset == PORTCFG_TX_TITANIUM, "edition=titanium -> renderer=enhanced textures=titanium");
    CHECK(portcfg_titanium(&pc, prj, par) == 0 && strstr(prj, "MW2.PRJ") && par[0], "the Titanium files found");
    CHECK(portcfg_texset_ok(&pc, PORTCFG_ED_ENHANCED, PORTCFG_TX_TITANIUM) && portcfg_texset_ok(&pc, PORTCFG_ED_3DFX, PORTCFG_TX_TITANIUM),
          "TITANIUM with a 3D renderer");
    CHECK(!portcfg_texset_ok(&pc, PORTCFG_ED_DOS, PORTCFG_TX_TITANIUM) && portcfg_texset_ok(&pc, PORTCFG_ED_DOS, PORTCFG_TX_ORIGINAL),
          "the DOS renderer: ORIGINAL only");
    {
        static char m1[PORTCFG_PATH], m2[PORTCFG_PATH], m3[PORTCFG_PATH], m4[PORTCFG_PATH], m5[PORTCFG_PATH];
        CHECK(portcfg_look(&pc, PORTCFG_ED_ENHANCED, PORTCFG_TX_TITANIUM, m1, m2, m3, m4, m5) != 0, "no look without the models archive");
        snprintf(pc.models, sizeof pc.models, "/x/models.prj");
        CHECK(portcfg_look(&pc, PORTCFG_ED_ENHANCED, PORTCFG_TX_TITANIUM, m1, m2, m3, m4, m5) == 0 && strstr(m4, "SKYGND.PAR") && !m5[0],
              "TITANIUM: the Titanium sky file, no fog file (%s)", m4);
        pc.models[0] = 0;
    }
    portcfg_save(NULL, &pc);
    {
        char line[256];
        int ren = 0, tex = 0, old = 0;
        if ((f = fopen(cfg, "r"))) {
            while (fgets(line, sizeof line, f)) {
                ren += strcmp(line, "renderer=enhanced\n") == 0; tex += strcmp(line, "textures=titanium\n") == 0;
                old += strncmp(line, "edition=", 8) == 0;
            }
            fclose(f);
        }
        CHECK(ren == 1 && tex == 1 && old == 0, "saved as renderer= / textures= (%d %d %d)", ren, tex, old);
    }
    if ((f = fopen(cfg, "w"))) { fprintf(f, "game=/nonexistent\ntitanium=%s\nedition=pvr\ntextures=/x/textures.prj\n", dir); fclose(f); }
    portcfg_load(NULL, &pc);
    CHECK(pc.renderer == PORTCFG_ED_PVR && pc.texset == PORTCFG_TX_ORIGINAL && strcmp(pc.textures, "/x/textures.prj") == 0,
          "edition=pvr -> renderer=pvr; textures=<path> stays the archive path");
    pc.titanium[0] = 0;
    CHECK(portcfg_titanium(&pc, prj, par) != 0, "no folder: unavailable");
    CHECK(!portcfg_texset_ok(&pc, PORTCFG_ED_ENHANCED, PORTCFG_TX_TITANIUM), "no folder: TEXTURES locked to ORIGINAL");
    remove(cfg);

    printf("test_titanium: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
