/* common3d.h - shared setup for the GL tools: a mech plus its mission's textures. */
#ifndef MW2_COMMON3D_H
#define MW2_COMMON3D_H
#include "bwd.h"
#include "glr.h"
#include "mech3d.h"
#include "prj.h"
#include "render.h"
#include "tex.h"
#include "skygnd.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    texture *slot[GLR_SLOTS];
    texture *bank0[GLR_SLOTS];   /* the bank-0 bitmaps (scrub billboards, colour type 3) */
    uint8_t  palette[256][3];
    int      textures_loaded;
} c3d_scene;

/* Textures come from `texs`. An ATi archive has one format (ARGB1555). For 3dfx/S3
 * the format per texture isn't stored; insignia (L1/L2...) are ARGB4444, the rest
 * are taken as RGB555 - right for nearly all surfaces, ATi is the exact source. */
static texture *c3d_texture(prj_archive *texs, int ati, const char *name)
{
    prj_record r;
    texture *t = calloc(1, sizeof *t);
    /* `ati` is the archive's texture kind: 0 3Dfx / S3, 1 ATi, 2 PowerVR, 3 DOS, 4 Matrox Mystique */
    int enc = ati == 1 ? TEX_ATI : ati == 2 ? TEX_PVR : ati == 3 ? TEX_DOS : ati == 4 ? TEX_MGA
            : ((toupper((unsigned char)name[0]) == 'L' && isdigit((unsigned char)name[1])) ? TEX_3DFX_4444 : TEX_3DFX_555);
    if (!t) return NULL;
    if (prj_read_named(texs, "CEL", name, &r) != PRJ_OK) { free(t); return NULL; }
    if (tex_decode(r.data, r.size, enc, t) != 0) { free(t); t = NULL; }
    prj_record_free(&r);
    return t;
}

static void c3d_free_scene(c3d_scene *s)
{
    int i;
    for (i = 0; i < GLR_SLOTS; i++)
    {
        if (s->slot[i]) { tex_free(s->slot[i]); free(s->slot[i]); s->slot[i] = NULL; }
        if (s->bank0[i]) { tex_free(s->bank0[i]); free(s->bank0[i]); s->bank0[i] = NULL; }
    }
}

/* mission: 4-letter code, e.g. "CYAN" (uses CYANMAP1 and the CYAN_DA palette) */
static int c3d_load_scene(prj_archive *models, prj_archive *texs, int ati, const char *mission, c3d_scene *s)
{
    char name[16];
    prj_record rec;
    bwd_texmap map;
    r_target holder;
    int i;

    memset(s, 0, sizeof *s);
    /* every texture map (BMPJ/BMID records) in the mission's include tree, in include order: each BMID sets its slot's
     * group with no "already filled" test (engine 0x1003e463 -> 0x1002d060), so the LATER map wins - CYANMAP2 / CINDMAP2
     * re-point 0x114 / 0x115 (the player's star's Falcon insignia), and the shell's INSTMAP1 (Instant Action), included
     * last, sets the chosen clans' insignia and camo */
    {
        bwd_mission bm;
        int k, c, maps = 0;
        snprintf(name, sizeof name, "%.4sSCN1", mission);
        if (bwd_mission_load(models, name, &bm) == 0) {
            for (k = 0; k < bm.record_count; k++) {
                int has = 0;
                for (c = 0; c < bm.records[k].chunk_count; c++)
                    if (strcmp(bm.records[k].chunks[c].tag, "BMID") == 0) { has = 1; break; }
                if (!has || bwd_texmap_decode(bm.records[k].data, bm.records[k].size, &map) != 0) continue;
                maps++;
                for (i = 0; i < GLR_SLOTS; i++)
                    if (map.present[i]) {
                        texture *t = c3d_texture(texs, ati, map.name[i]);
                        if (!t) continue;
                        if (s->slot[i]) { tex_free(s->slot[i]); free(s->slot[i]); } else s->textures_loaded++;
                        s->slot[i] = t;
                    }
                for (i = 0; i < GLR_SLOTS; i++)   /* bank 0: only the scrub bitmaps matter here (effects load their own) */
                    if (map.b0_present[i]) {
                        texture *t = c3d_texture(texs, ati, map.b0_name[i]);
                        if (!t) continue;
                        if (s->bank0[i]) { tex_free(s->bank0[i]); free(s->bank0[i]); }
                        s->bank0[i] = t;
                    }
            }
            bwd_mission_free(&bm);
        }
        if (!maps) fprintf(stderr, "no texture map for mission %s\n", mission);
    }
    if (r_init(&holder, 1, 1) == 0) {
        snprintf(name, sizeof name, "%.4s_DA", mission);
        if (prj_read_named(models, "PAL", name, &rec) == PRJ_OK || prj_read_named(models, "PAL", "CYAN_DA", &rec) == PRJ_OK) {
            r_set_palette(&holder, rec.data, rec.size);
            prj_record_free(&rec);
        }
        memcpy(s->palette, holder.palette, sizeof s->palette);
        r_free(&holder);
    }
    return 0;
}
/* Sky, ground and fog for a mission, the "3dfx + PowerVR fog" way:
 * tiling from MW2_SKYGND (3dfx skygnd.par), fog from MW2_SKYGND_FOG (PowerVR one).
 * Fog colour = mean of ground texture, sky texture and ambient grey (PowerVR recipe).
 * fog_override >= 0 replaces the density (per game unit). */
static void c3d_environment(glr *r, c3d_scene *s, const char *mission, glr_view *v, float fog_override)
{
    skygnd tiles, fog;
    texture *g = s->slot[SKYGND_GROUND_SLOT], *sky = s->slot[SKYGND_SKY_SLOT];
    float ga[3] = {0.5f, 0.5f, 0.5f}, sa[3] = {0.5f, 0.5f, 0.5f};
    int k;
    skygnd_defaults(&tiles);
    skygnd_lookup(getenv("MW2_SKYGND"), mission, &tiles);
    /* the PowerVR edition's sky plane repeats every 64e6 / tile_sky (0x1005ED90: q = a2 / a3), twice as often as the 3Dfx
     * ceiling's 128e6 / tile_sky */
    if (getenv("MW2_EDITION") && strcmp(getenv("MW2_EDITION"), "pvr") == 0) tiles.tile_sky *= 2.0f;
    skygnd_defaults(&fog);
    skygnd_lookup(getenv("MW2_SKYGND_FOG"), mission, &fog);
    glr_set_environment(r, g, sky, tiles.tile_sky, tiles.tile_ground);
    if (g) glr_texture_average(g, ga);
    if (sky) glr_texture_average(sky, sa);
    for (k = 0; k < 3; k++) {
        v->fog_color[k] = (ga[k] + sa[k] + v->ambient) / 3.0f;
        v->sky_color[k] = v->fog_color[k];
    }
    v->fog_density = fog_override >= 0 ? fog_override : (fog.has_fog ? fog.fog : 0);   /* the raw sgl_set_fog density */
}
#endif
