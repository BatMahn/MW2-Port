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
#include "wtb.h"
#include <strings.h>
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
/* The Titanium look: its archive's textures by name first (TEX_TITANIUM), the mission's texture archive for the names it
 * lacks - it has 51 of the 418 names the 3D editions' texture maps use (effects and 17 grounds) and 597 of the ATi
 * archive's 1404 CEL names (580 of them effect frames); counts for the tests */
static prj_archive *c3d_ti;
static int c3d_ti_hits, c3d_ti_miss;

static texture *c3d_texture(prj_archive *texs, int ati, const char *name)
{
    prj_record r;
    texture *t = calloc(1, sizeof *t);
    /* `ati` is the archive's texture kind: 0 3Dfx / S3, 1 ATi, 2 PowerVR, 3 DOS, 4 Matrox Mystique, 5 Titanium */
    int enc = ati == 1 ? TEX_ATI : ati == 2 ? TEX_PVR : ati == 3 ? TEX_DOS : ati == 4 ? TEX_MGA : ati == 5 ? TEX_TITANIUM
            : ((toupper((unsigned char)name[0]) == 'L' && isdigit((unsigned char)name[1])) ? TEX_3DFX_4444 : TEX_3DFX_555);
    if (!t) return NULL;
    if (c3d_ti && ati != 3 && ati != 5) {
        if (prj_read_named(c3d_ti, "CEL", name, &r) == PRJ_OK) {
            int ok = tex_decode(r.data, r.size, TEX_TITANIUM, t) == 0;
            prj_record_free(&r);
            if (ok) { c3d_ti_hits++; return t; }
        }
        c3d_ti_miss++;
    }
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
    v->ti_fog[0] = 0;
}

/* The Titanium look's sky and ground (mercsw.dll): its SKYGND.PAR (MW2_SKYGND) for the mission; the Titanium archive's
 * own texture map (its slot numbers differ from the 3D editions') for the ground (slot 0x4a, 0x100559eb) and the sky
 * dome (the SKYDOME object its planets include: clouds 0x8c, mountains 0x5d / 0x6c / 0x6d / 0x8d, moon 0x7c, sun 0x7d);
 * the clear colour sky=, the fog colour fog=, density's Z the full-fog depth (fog: whether the renderer draws fog).
 * glview updates the camera-height terms (c3d_ti_frame) every frame. */
static skygnd_ti c3d_ti_sg;
#if defined(__GNUC__)
__attribute__((unused))
#endif
static void c3d_ti_environment(glr *r, c3d_scene *s, const char *mission, glr_view *v, int fog)
{
    c3d_scene ts;
    prj_archive *keep = c3d_ti;
    texture *ground = s->slot[SKYGND_GROUND_SLOT];
    int k, dome = 0;
    skygnd_ti_lookup(getenv("MW2_SKYGND"), mission, &c3d_ti_sg);
    c3d_ti = NULL;   /* (the scene below is all Titanium: no overlay counting) */
    c3d_load_scene(keep, keep, 5, mission, &ts);
    c3d_ti = keep;
    if (ts.slot[0x4a]) ground = ts.slot[0x4a];
    /* the ground quad (0x1005545b): half size ground_max x 2 around the camera, UV = world / (half size / tile_ground):
     * in the port's terms (repeats over 32e6, see skygnd.h) 32e6 x tile / half size */
    glr_set_environment(r, c3d_ti_sg.tile_ground > 0 ? ground : NULL, NULL, 4.0f,
                        32.0e6f * c3d_ti_sg.tile_ground / (c3d_ti_sg.ground_max * 2.0f));
    {   /* the dome: the SKYDOME record's OBJ (its POLY id at +48) */
        char name[16];
        bwd_mission bm;
        snprintf(name, sizeof name, "%.4sSCN1", mission);
        if (bwd_mission_load(keep, name, &bm) == 0) {
            for (k = 0; k < bm.record_count && !dome; k++) {
                int c;
                if (strcasecmp(bm.records[k].name, "SKYDOME") != 0) continue;
                for (c = 0; c < bm.records[k].chunk_count; c++) {
                    const bwd_chunk *ch = &bm.records[k].chunks[c];
                    prj_record pr;
                    wtb_model wm;
                    if (strcmp(ch->tag, "OBJ") != 0 || ch->size < 52) continue;
                    if (prj_read(keep, prj_find_type(keep, "POLY"), ch->data[48] | ch->data[49] << 8, &pr) != PRJ_OK) continue;
                    if (wtb_parse(pr.data, pr.size, &wm) == 0) {
                        if (wm.object_count > 0) { glr_set_skydome(r, &wm.objects[0], ts.slot); dome = 1; }
                        wtb_free(&wm);
                    }
                    prj_record_free(&pr);
                    break;
                }
            }
            bwd_mission_free(&bm);
        }
    }
    if (!dome) glr_set_skydome(r, NULL, ts.slot);
    c3d_free_scene(&ts);
    for (k = 0; k < 3; k++) { v->fog_color[k] = (float)c3d_ti_sg.fog[k] / 255.0f; v->sky_color[k] = (float)c3d_ti_sg.sky[k] / 255.0f; }
    v->fog_density = 0;
    /* fog: with a renderer that fogs (glview look_fog: the PowerVR edition, the enhanced mix) the Titanium fog; else none,
     * and the clear colour is sky= as it is (the engine's Y = 0 case, 0x1006a989) - a port choice */
    v->ti_fog[0] = fog ? 1.0f : 0.0f; v->ti_fog[1] = c3d_ti_sg.dens_z; v->ti_fog[2] = 1;
    v->pvr_veil = 0;
}
/* the camera-height terms (0x1006a851): f = 1; with density Y != 0, above the ground t = Y / height (Y > 0, low cloud)
 * or -Y x height (Y < 0, ground fog), f = t < M ? M : min(t, 1); at or below it f = M when Y < 0. The fog factor
 * multiplies every vertex's (v->ti_fog[2]); the clear colour is sky x g + fog x (1 - g), g = f, x Y / 50000 when Y > 0,
 * clamped to M..1 */
#if defined(__GNUC__)
__attribute__((unused))
#endif
static void c3d_ti_frame(glr_view *v, float eye_y)
{
    const skygnd_ti *t = &c3d_ti_sg;
    float f = 1.0f, g;
    int k;
    if (!v->ti_fog[0]) return;
    if (t->dens_y != 0) {
        if (eye_y > 0) {
            float q = t->dens_y > 0 ? t->dens_y / eye_y : -t->dens_y * eye_y;
            f = q < t->dens_m ? t->dens_m : q < 1.0f ? q : 1.0f;
        } else if (t->dens_y < 0) f = t->dens_m;
    }
    v->ti_fog[2] = f;
    g = f;
    if (t->dens_y != 0) {
        if (t->dens_y > 0) g = t->dens_y / 50000.0f * f;
        if (g < t->dens_m) g = t->dens_m; else if (g > 1.0f) g = 1.0f;
    } else g = 1.0f;
    for (k = 0; k < 3; k++) v->sky_color[k] = ((float)t->sky[k] * g + (float)t->fog[k] * (1.0f - g)) / 255.0f;
}
#endif
