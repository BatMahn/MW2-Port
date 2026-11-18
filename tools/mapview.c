/*
 * mapview - the MW2 map viewer: fly around any mission's map as the 3D editions draw it, and click on what is there.
 *
 *   mapview [game folder] [SCENE]        (no folder: the port's own, mw2port.cfg; no scene: the first campaign mission)
 *
 * The world (every xxxxAREn record of the mission, buildings as they stand at the start - destroyed variants hidden),
 * the units each star places, the nav points and the mission boundary. A free camera (3D) or a map straight down (Tab).
 * Clicking an object, unit or nav point shows what the mission data says about it. Read-only: nothing is written.
 *
 * Keys: W A S D move, Q / E down / up, Shift faster, mouse drag (left or right) look - pan in the map view, wheel
 * speed / zoom, click select, Tab 3D / map, M missions, PgUp / PgDn previous / next mission, Home the player's start,
 * F go to the selection, N nav points, U units, B boundary, L labels, G fog, H help, Esc close / deselect.
 *
 * Needs the 3D editions' archives (the game folder's 3d/: models.prj and textures.prj), as the port's 3D looks do.
 * Tests: MW2_MAPVIEW_SHOT=out.ppm (one frame, then exit), MW2_MAPVIEW_CAM="x,y,z,yaw,pitch" (m, degrees),
 * MW2_MAPVIEW_TOP=width_m (start in the map view), MW2_MAPVIEW_CLICK="x,y" (a click at that window pixel first),
 * MW2_MAPVIEW_LIST=1 / MW2_MAPVIEW_HELP=1 (the mission list / the help shown), MW2_MAPVIEW_CYCLE=n (the next n missions
 * loaded and drawn in turn first).
 */
#include "sdlcompat.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "common3d.h"
#include "datapath.h"
#include "glr.h"
#include "hud.h"
#include "msim.h"
#include "portcfg.h"
#include "rb.h"
#include "skygnd.h"
#include "ttext.h"

#define MV_TITLE "MW2 Map Viewer"

/* ---- the missions: every <xxxx>SCN1 scene in the archive; the campaign's own names (MW2SHELL's table) */
static const char *const CAMP_SCEN[2][16] = {
    {"YELL", "ORAN", "TEAL", "TAUP", "JENN", "SABL", "GREY", "BROW", "AMY_", "SILV", "AQUA", "KIM_", "CYAN", "MARO", "GOLD", "IREN"},
    {"PINK", "GREE", "RED_", "FUCH", "CIND", "RUST", "UMBE", "TAN_", "HEID", "PLUM", "WHIT", "JILL", "PUCE", "BLON", "BRON", "MARY"}};
static const char *const CAMP_TITLE[2][16] = {
    {"Pyre Light", "Flame Tongue", "Blade Splint", "Temper Edge", "Trial 1", "Sable Flame", "Burning Chrome", "Scorching Sand",
     "Trial 2", "Silver Staff", "Aquiline Fire", "Trial 3", "Cold Crescent", "Velvet Hammer", "Golden Spade", "Trial 4"},
    {"Silent Thunder", "Arkham Bridge", "Mirror Cage", "Bone Machine", "Trial 1", "Bouk Obelisk", "Umber Wall", "Rogue Chariot",
     "Trial 2", "Plum Wine", "Rust Heart", "Trial 3", "Armor Veil", "Iron Piston", "Bronze Anvil", "Trial 4"}};
typedef struct { char scene[12]; char title[64]; int order; } mv_mission;
static mv_mission g_ms[256];
static int g_mn, g_cur = -1;

static int mission_cmp(const void *a, const void *b)
{
    const mv_mission *x = a, *y = b;
    if (x->order != y->order) return x->order - y->order;
    return strcmp(x->scene, y->scene);
}
static void missions_list(prj_archive *a)
{
    int t = prj_find_type(a, "BWD"), i, c, k;
    g_mn = 0;
    if (t < 0) return;
    for (i = 0; i < prj_symbol_count(a, t) && g_mn < 256; i++) {
        const char *nm = prj_symbol_name(a, t, i);
        mv_mission *m;
        if (!nm || strlen(nm) != 8 || strcasecmp(nm + 4, "SCN1") != 0) continue;
        char sc[12];
        m = &g_ms[g_mn++];
        snprintf(sc, sizeof sc, "%s", nm);
        for (k = 0; sc[k]; k++) if (sc[k] >= 'a' && sc[k] <= 'z') sc[k] = (char)(sc[k] - 32);
        memcpy(m->scene, sc, sizeof sc);
        m->order = 1000;
        snprintf(m->title, sizeof m->title, "%.4s", sc);
        for (c = 0; c < 2; c++)
            for (k = 0; k < 16; k++)
                if (strncmp(m->scene, CAMP_SCEN[c][k], 4) == 0) {
                    m->order = c * 16 + k;
                    snprintf(m->title, sizeof m->title, "%s %d: %s", c ? "Jade Falcon" : "Wolf", k + 1, CAMP_TITLE[c][k]);
                }
        if (m->order == 1000 && sc[0] == 'T' && sc[1] == 'N' && (sc[2] == 'W' || sc[2] == 'J') && sc[3] >= '1' && sc[3] <= '9') {
            m->order = 100 + (sc[2] == 'J') * 10 + (sc[3] - '0');
            snprintf(m->title, sizeof m->title, "%s training %c", sc[2] == 'W' ? "Wolf" : "Jade Falcon", sc[3]);
        } else if (m->order == 1000) snprintf(m->title, sizeof m->title, "%.4s (Instant Action / network)", sc);
    }
    qsort(g_ms, (size_t)g_mn, sizeof g_ms[0], mission_cmp);
}

/* ---- the loaded mission */
static prj_archive *g_ma, *g_ta;
static int g_kind;   /* the texture archive's kind (c3d_texture: 0 3Dfx / S3, 1 ATi, 2 PowerVR) */
static char g_sky[PORTCFG_PATH], g_fog[PORTCFG_PATH];
static glr *g_r;
static hud *g_hud;
static c3d_scene g_sc;
static int g_sc_ok;
static mech3d g_world, g_all;   /* the world layer; the actor layer (units + moving / facing world parts) */
static world_actor *g_ac;
static int g_na;
static int *g_act_first, *g_act_count;   /* each actor's parts in g_all */
static msim g_s;
static int g_s_ok;
static bwd_mission g_bm;
static int g_bm_ok;
static float (*g_pmin)[3], (*g_pmax)[3];   /* world parts' boxes (world coordinates) */
typedef struct { float x, y, z, r; char name[48]; int type, shown, boundary; } mv_nav;
static mv_nav g_nav[128];
static int g_nn;
static float g_bnd[16][64][3];   /* boundary circles, on the ground */
static int g_bn;
static glr_view g_base;          /* the mission's sky / ground / fog */
static float g_fog_density;

/* ---- the camera and the overlays */
static float g_eye[3], g_yaw, g_pitch = -10.0f, g_speed = 3000.0f;   /* cm, degrees, cm/s */
static int g_top;                 /* the map view (straight down, north up) */
static float g_top_c[2], g_top_w = 200000.0f;   /* its centre (x, z) and width, cm */
static int g_show_nav = 1, g_show_units = 1, g_show_bnd = 1, g_show_labels = 1, g_fog_on, g_help, g_list, g_list_top;
static int g_sel_kind, g_sel;     /* 0 none, 1 world part, 2 unit, 3 nav point */
static int g_w = 1600, g_h = 900; /* the picture, pixels */
static float g_dpi = 1.0f;
static char g_msg[256];
static int g_face_src[1024], g_face_dst[1024], g_nface;   /* the fire / smoke cards: world part -> its copy on the actor layer */
static int g_scrg_gx, g_scrg_gz, g_scrg_cx, g_scrg_cz;   /* the scrub patch's grid point (scrub_follow) */

static void part_vertex(const mech3d_part *p, const wtb_vertex *v, float out[3])
{
    if (p->has_rot) {
        const float *R = p->rot;
        out[0] = R[0] * v->x + R[1] * v->y + R[2] * v->z + (float)p->pos[0];
        out[1] = R[3] * v->x + R[4] * v->y + R[5] * v->z + (float)p->pos[1];
        out[2] = R[6] * v->x + R[7] * v->y + R[8] * v->z + (float)p->pos[2];
    } else {
        const float c = cosf(p->yaw * 3.14159265f / 180.0f), sn = sinf(p->yaw * 3.14159265f / 180.0f);
        out[0] = c * v->x + sn * v->z + (float)p->pos[0];
        out[1] = (float)v->y + (float)p->pos[1];
        out[2] = -sn * v->x + c * v->z + (float)p->pos[2];
    }
}
static int part_box(const mech3d_part *p, float mn[3], float mx[3])
{
    int o, v, k, any = 0;
    for (k = 0; k < 3; k++) { mn[k] = 1e30f; mx[k] = -1e30f; }
    for (o = 0; o < p->model.object_count && o < 1; o++)
        for (v = 0; v < p->model.objects[o].vert_count; v++) {
            float q[3];
            part_vertex(p, &p->model.objects[o].verts[v], q);
            for (k = 0; k < 3; k++) { if (q[k] < mn[k]) mn[k] = q[k]; if (q[k] > mx[k]) mx[k] = q[k]; }
            any = 1;
        }
    if (!any) for (k = 0; k < 3; k++) mn[k] = mx[k] = (float)p->pos[k];
    return any;
}

/* ---- projection: game coordinates (cm) -> window pixels (top-left origin); 0 if behind the eye */
static float g_vfov = 50.0f;
#define TOP_PITCH (-89.9f)
#define TOP_EYE_Y 300000.0f
static int project(const float p[3], float *sx, float *sy)
{
    if (g_top) {
        /* the renderer looks down at pitch -89.9 (straight down would leave look_at no up vector): its screen up is
         * (0, sin 0.1, cos 0.1), so a point's height shifts it a little - taken in, or a unit's marker sits 10 m off */
        const float sc = (float)g_w / g_top_w, lp = TOP_PITCH * 3.14159265f / 180.0f;
        *sx = (float)g_w * 0.5f + (p[0] - g_top_c[0]) * sc;
        *sy = (float)g_h * 0.5f - ((p[1] - TOP_EYE_Y) * cosf(lp) - (p[2] - g_top_c[1]) * sinf(lp)) * sc;
        return 1;
    } else {
        const float ly = g_yaw * 3.14159265f / 180.0f, lp = g_pitch * 3.14159265f / 180.0f;
        const float F[3] = {cosf(lp) * sinf(ly), sinf(lp), cosf(lp) * cosf(ly)};
        const float R[3] = {cosf(ly), 0.0f, -sinf(ly)};
        const float U[3] = {-sinf(lp) * sinf(ly), cosf(lp), -sinf(lp) * cosf(ly)};
        const float d[3] = {p[0] - g_eye[0], p[1] - g_eye[1], p[2] - g_eye[2]};
        const float z = d[0] * F[0] + d[1] * F[1] + d[2] * F[2];
        const float f = 1.0f / tanf(g_vfov * 3.14159265f / 360.0f) * (float)g_h * 0.5f;
        if (z < 60.0f) return 0;
        *sx = (float)g_w * 0.5f + (d[0] * R[0] + d[1] * R[1] + d[2] * R[2]) / z * f;
        *sy = (float)g_h * 0.5f - (d[0] * U[0] + d[1] * U[1] + d[2] * U[2]) / z * f;
        return 1;
    }
}
static float cam_dist(const float p[3])
{
    if (g_top) return 0;
    return sqrtf((p[0] - g_eye[0]) * (p[0] - g_eye[0]) + (p[1] - g_eye[1]) * (p[1] - g_eye[1]) + (p[2] - g_eye[2]) * (p[2] - g_eye[2]));
}

/* ---- 2D drawing in window pixels (the HUD's 640 x 480 layout stretched over the window) */
static float VX(float px) { return px * 640.0f / (float)g_w; }
static float VY(float py) { return py * 480.0f / (float)g_h; }
static void ui_rect(float x, float y, float w, float h, const float c[4]) { hud_rect(g_hud, VX(x), VY(y), VX(w), VY(h), c); }
static void ui_line(float x0, float y0, float x1, float y1, float wpx, const float c[4]) { hud_line(g_hud, VX(x0), VY(y0), VX(x1), VY(y1), wpx, c); }

typedef struct { char s[96]; int px; hud_sprite sp; int w, h, base; unsigned used; } mv_label;
static mv_label g_lab[384];
static unsigned g_lab_clock;
static const mv_label *label(const char *s, int px)
{
    int i, best = 0;
    uint8_t *cov, *rgba;
    mv_label *l;
    int w, h, base;
    size_t k, n;
    if (!ttext_available() || !s[0]) return NULL;
    for (i = 0; i < 384; i++) if (g_lab[i].sp.tex && g_lab[i].px == px && strcmp(g_lab[i].s, s) == 0) { g_lab[i].used = ++g_lab_clock; return &g_lab[i]; }
    for (i = 1; i < 384; i++) if (g_lab[i].used < g_lab[best].used) best = i;
    l = &g_lab[best];
    if (l->sp.tex) hud_sprite_free(&l->sp);
    memset(l, 0, sizeof *l);
    if (!(cov = ttext_render(s, (float)px, 1.0f, &w, &h, &base))) return NULL;
    n = (size_t)w * (size_t)h;
    if (!(rgba = malloc(n * 4))) { free(cov); return NULL; }
    for (k = 0; k < n; k++) { rgba[k * 4] = rgba[k * 4 + 1] = rgba[k * 4 + 2] = 255; rgba[k * 4 + 3] = cov[k]; }
    free(cov);
    if (hud_sprite_rgba(&l->sp, rgba, w, h) != 0) { free(rgba); return NULL; }
    free(rgba);
    snprintf(l->s, sizeof l->s, "%s", s);
    l->px = px; l->w = w; l->h = h; l->base = base; l->used = ++g_lab_clock;
    return l;
}
static int text_px(void) { return (int)lrintf(11.0f * g_dpi); }
/* text with its top-left at (x, y) pixels; returns its width */
static float ui_text(float x, float y, const char *s, const float c[4])
{
    const mv_label *l = label(s, text_px());
    if (!l) return 0;
    hud_draw_rect_tex(g_hud, &l->sp, VX(floorf(x)), VY(floorf(y)), VX((float)l->w), VY((float)l->h), c);
    return (float)l->w;
}
static float text_w(const char *s) { return ttext_available() ? ttext_width(s, (float)text_px(), 1.0f) : 0.0f; }
static float line_h(void) { return floorf(16.0f * g_dpi); }

/* ---- loading */
static void free_mission(void)
{
    int i;
    if (g_s_ok) { msim_free(&g_s); g_s_ok = 0; }
    if (g_ac) { world3d_free_actors(g_ac, g_na); g_ac = NULL; g_na = 0; }
    free(g_all.parts); memset(&g_all, 0, sizeof g_all);
    if (g_world.parts) mech3d_free(&g_world);
    memset(&g_world, 0, sizeof g_world);
    if (g_bm_ok) { bwd_mission_free(&g_bm); g_bm_ok = 0; }
    free(g_pmin); free(g_pmax); g_pmin = g_pmax = NULL;
    free(g_act_first); free(g_act_count); g_act_first = g_act_count = NULL;
    if (g_r) glr_flush_textures(g_r);
    if (g_sc_ok) { c3d_free_scene(&g_sc); g_sc_ok = 0; }
    for (i = 0; i < 384; i++) if (g_lab[i].sp.tex) { hud_sprite_free(&g_lab[i].sp); memset(&g_lab[i], 0, sizeof g_lab[i]); }
    g_nn = g_bn = 0;
    g_sel_kind = 0;
    g_scrg_gx = g_scrg_gz = g_scrg_cx = g_scrg_cz = 0;
    g_nface = 0;
}

static void environment(const char *code)
{
    skygnd tiles, fog;
    texture *g = g_sc.slot[SKYGND_GROUND_SLOT], *sky = g_sc.slot[SKYGND_SKY_SLOT];
    float ga[3] = {0.5f, 0.5f, 0.5f}, sa[3] = {0.5f, 0.5f, 0.5f};
    int k;
    skygnd_defaults(&tiles);
    if (g_sky[0]) skygnd_lookup(g_sky, code, &tiles);
    skygnd_defaults(&fog);
    if (g_fog[0]) skygnd_lookup(g_fog, code, &fog);
    glr_set_environment(g_r, g, sky, tiles.tile_sky, tiles.tile_ground);
    if (g) glr_texture_average(g, ga);
    if (sky) glr_texture_average(sky, sa);
    glr_default_view(&g_base);
    for (k = 0; k < 3; k++) g_base.fog_color[k] = g_base.sky_color[k] = (ga[k] + sa[k] + g_base.ambient) / 3.0f;
    g_fog_density = fog.has_fog ? fog.fog : 0.0f;
    {   /* the planet's LITE ambient level (<code>PLT1, payload +20), as the simulation lights the world (glview) */
        char pn[16];
        prj_record pr;
        snprintf(pn, sizeof pn, "%.4sPLT1", code);
        if (prj_read_named(g_ma, "BWD", pn, &pr) == PRJ_OK) {
            bwd_chunk lc[64];
            int nlc = bwd_chunks(pr.data, pr.size, lc, 64), q, lvl = 0;
            for (q = 0; q < nlc; q++) if (strcmp(lc[q].tag, "LITE") == 0 && lc[q].size >= 22) lvl = (int16_t)(lc[q].data[20] | lc[q].data[21] << 8);
            if (lvl > 0) g_base.ambient = (float)lvl * 3.0f / 256.0f;
            prj_record_free(&pr);
        }
    }
    /* the enhanced look's texturing (glview look_tex / edition_look): types 6-8 and 10 affine, and the alpha mask only for
     * types 8-11 - the ground fill and types 5-7 draw their alpha-0 texels' colour (without it the ground's clear texels
     * were discarded and it showed flat) */
    g_base.ati_tex = 1 | 4;
    glr_set_titanium(g_r, NULL, NULL);
}

static void goto_start(void)
{
    g_eye[0] = g_s_ok ? g_s.player[0] : 0.0f;
    g_eye[2] = g_s_ok ? g_s.player[2] : 0.0f;
    g_eye[1] = (g_s_ok ? msim_ground(&g_s, g_eye[0], g_eye[2], 1e7f) : 0.0f) + 2500.0f;
    g_yaw = g_s_ok ? g_s.player_heading : 0.0f;
    g_pitch = -10.0f;
    g_top_c[0] = g_eye[0]; g_top_c[1] = g_eye[2];
}

static int load_mission(int idx)
{
    char code[8];
    const char *scene;
    int i, k, n = 0;
    if (idx < 0 || idx >= g_mn) return -1;
    free_mission();
    g_cur = idx;
    scene = g_ms[idx].scene;
    snprintf(code, sizeof code, "%.4s", scene);
    c3d_load_scene(g_ma, g_ta, g_kind, code, &g_sc);
    g_sc_ok = 1;
    if (world3d_mission(g_ma, scene, 1, &g_world) <= 0) snprintf(g_msg, sizeof g_msg, "%s: no world records", scene);
    if (world3d_actors(g_ma, scene, &g_ac, &g_na) != 0) { g_ac = NULL; g_na = 0; }
    if (msim_init(&g_s, g_ma, scene, g_ac, g_na) == 0) {
        g_s_ok = 1;
        if (g_world.part_count) msim_set_world(&g_s, &g_world);   /* (buildings: the destroyed variants hidden) */
        for (i = 0; i < g_na; i++)
            if (g_s.units) {   /* standing on the ground the simulation finds under them */
                const float y = msim_ground(&g_s, (float)g_ac[i].mech.origin[0], (float)g_ac[i].mech.origin[2], (float)g_ac[i].mech.origin[1] + 3000.0f);
                if (fabsf(y) < 1e8f && g_ac[i].unit_class != 5 && g_ac[i].unit_class != 8) g_ac[i].mech.origin[1] = (int32_t)lrintf(y);
            }
    }
    /* the boxes */
    g_pmin = calloc((size_t)(g_world.part_count + 1), sizeof *g_pmin);
    g_pmax = calloc((size_t)(g_world.part_count + 1), sizeof *g_pmax);
    for (i = 0; i < g_world.part_count && g_pmin; i++) part_box(&g_world.parts[i], g_pmin[i], g_pmax[i]);
    /* the actor layer: the units, then the world's moving / facing parts (as the game draws them) */
    g_act_first = calloc((size_t)(g_na + 1), sizeof *g_act_first);
    g_act_count = calloc((size_t)(g_na + 1), sizeof *g_act_count);
    for (i = 0; i < g_na; i++) n += g_ac[i].mech.part_count;
    for (i = 0; i < g_world.part_count; i++) if (!g_world.parts[i].hidden && (g_world.parts[i].moving || g_world.parts[i].facing)) n++;
    g_all.parts = calloc((size_t)(n + 1), sizeof *g_all.parts);
    if (g_all.parts) {
        mech3d tmp;
        memset(&tmp, 0, sizeof tmp);
        if (g_na && world3d_compose(g_ac, g_na, &tmp) == 0) {
            memcpy(g_all.parts, tmp.parts, (size_t)tmp.part_count * sizeof *tmp.parts);
            g_all.part_count = tmp.part_count;
            free(tmp.parts);
        }
        for (i = 0, k = 0; i < g_na; i++) { g_act_first[i] = k; g_act_count[i] = g_ac[i].mech.part_count; k += g_ac[i].mech.part_count; }
        for (i = 0; i < g_world.part_count; i++)
            if (!g_world.parts[i].hidden && (g_world.parts[i].moving || g_world.parts[i].facing)) {
                mech3d_part *q = &g_all.parts[g_all.part_count++];
                *q = g_world.parts[i];
                q->moving = 0;   /* (as placed at the start; glr skips the flagged ones on both layers) */
                if (q->facing) { q->facing = 0; q->has_rot = 0; g_face_src[g_nface] = i; g_face_dst[g_nface] = g_all.part_count - 1; if (g_nface < 1023) g_nface++; }
            }
    }
    /* the nav points and the boundary */
    if (bwd_mission_load(g_ma, scene, &g_bm) == 0) {
        g_bm_ok = 1;
        for (i = 0; i < g_bm.nav_count && g_nn < 128; i++) {
            const bwd_navpoint *b = &g_bm.navs[i];
            mv_nav *v = &g_nav[g_nn++];
            v->x = (float)b->x; v->z = (float)b->z;
            v->y = g_s_ok ? msim_ground(&g_s, v->x, v->z, 1e7f) : 0.0f;
            if (fabsf(v->y) > 1e8f) v->y = 0;
            v->r = (float)b->radius * 100.0f;
            v->type = b->type; v->shown = b->shown;
            v->boundary = (b->type == 0x20 || b->type == 0x22) && b->radius > 0;
            snprintf(v->name, sizeof v->name, "%s", b->name);
            if (v->boundary && g_bn < 16) {
                for (k = 0; k < 64; k++) {
                    const float a = (float)k * 6.2831853f / 64.0f, x = v->x + sinf(a) * v->r, z = v->z + cosf(a) * v->r;
                    float y = g_s_ok ? msim_ground(&g_s, x, z, 1e7f) : 0.0f;
                    if (fabsf(y) > 1e8f) y = 0;
                    g_bnd[g_bn][k][0] = x; g_bnd[g_bn][k][1] = y + 200.0f; g_bnd[g_bn][k][2] = z;
                }
                g_bn++;
            }
        }
    }
    /* to the renderer */
    glr_set_bank0(g_r, g_sc.bank0);
    environment(code);
    glr_set_mech(g_r, &g_world, (const uint8_t (*)[3])g_sc.palette, g_sc.slot, 0, 0);
    glr_set_actors(g_r, &g_all, (const uint8_t (*)[3])g_sc.palette, g_sc.slot, 0, 0);
    goto_start();
    {
        char t[160];
        snprintf(t, sizeof t, "%s - %s (%s)", MV_TITLE, g_ms[idx].title, scene);
        if (SDL_GetKeyboardFocus()) SDL_SetWindowTitle(SDL_GetKeyboardFocus(), t);
    }
    printf("mapview: %s (%s): %d world objects, %d units, %d nav points, %d textures\n", scene, g_ms[idx].title, g_world.part_count, g_na, g_nn, g_sc.textures_loaded);
    return 0;
}

/* ---- what a world part is */
static int part_thing(int pi)
{
    int b;
    if (!g_s_ok) return -1;
    for (b = 0; b < g_s.bld_count; b++) if (g_s.bld[b].intact == pi || g_s.bld[b].destroyed == pi) return b;
    return -1;
}
static const char *side_name(int side) { return side == 0 ? "your side" : side == 1 ? "enemy" : "neutral"; }
static const char *class_name(int c)
{
    switch (c) {
    case 1: return "mech"; case 2: return "hovercraft"; case 3: return "turret"; case 4: return "tank";
    case 5: return "aircraft"; case 7: return "door"; case 8: return "dropship";
    default: return "unit";
    }
}
static int unit_side(int i) { return g_ac[i].friendly ? 0 : g_ac[i].alliance == 2 ? 2 : 1; }

/* the selection's lines for the info panel */
static int info_lines(char lines[][96], int maxl)
{
    int n = 0;
#define L(...) do { if (n < maxl) snprintf(lines[n++], 96, __VA_ARGS__); } while (0)
    if (g_sel_kind == 1 && g_sel >= 0 && g_sel < g_world.part_count) {
        const mech3d_part *p = &g_world.parts[g_sel];
        const int b = part_thing(g_sel);
        L("OBJECT  %s", p->model_name);
        L("Record %s, OBJ node %d", p->rec, p->obj_index);
        L("Position %.1f, %.1f, %.1f m", p->pos[0] / 100.0, p->pos[1] / 100.0, p->pos[2] / 100.0);
        L("Size %.1f x %.1f x %.1f m", (g_pmax[g_sel][0] - g_pmin[g_sel][0]) / 100.0, (g_pmax[g_sel][1] - g_pmin[g_sel][1]) / 100.0, (g_pmax[g_sel][2] - g_pmin[g_sel][2]) / 100.0);
        L("Type word 0x%x, collision %d%s%s", p->objtype, p->coll, p->sparse ? ", density HIGH only" : "", p->moving ? ", moves on a path" : "");
        if (b >= 0) {
            const struct msim_building *g = &g_s.bld[b];
            L("Destructible: %s", g->name[0] ? g->name : "(unnamed)");
            L("  %s, %d hit points, flags 0x%x", side_name(g->side), g->hp, g->flags);
            if (g->destroyed == g_sel) L("  (this is its destroyed variant)");
            else if (g->destroyed >= 0) L("  destroyed variant %s", g_world.parts[g->destroyed].model_name);
        }
    } else if (g_sel_kind == 2 && g_sel >= 0 && g_sel < g_na) {
        const world_actor *a = &g_ac[g_sel];
        L("UNIT  %s (%s)", a->skel, class_name(a->unit_class));
        if (a->name[0]) L("Name %s", a->name);
        if (a->pilot[0]) L("Pilot %s", a->pilot);
        L("Loadout %s", a->loadout[0] ? a->loadout : "-");
        L("Group %s, state table %d%s", a->group, a->table, a->leader ? ", leader" : "");
        L("Side: %s", side_name(unit_side(g_sel)));
        L("Position %.1f, %.1f, %.1f m, heading %.0f", a->mech.origin[0] / 100.0, a->mech.origin[1] / 100.0, a->mech.origin[2] / 100.0, (double)a->mech.heading);
        L("Pilot level %d, gunnery %d", a->ai_level, a->ai_skill);
        L("Ranges: leash %.0f m, rest alert %.0f m, alert %.0f m", a->ai_range[0] / 100.0, a->ai_range[1] / 100.0, a->ai_range[2] / 100.0);
        L("Walk speed %.0f km/h", a->speed * 0.036);
    } else if (g_sel_kind == 3 && g_sel >= 0 && g_sel < g_nn) {
        const mv_nav *v = &g_nav[g_sel];
        L("NAV POINT  %s", v->name[0] ? v->name : "(unnamed)");
        L("Position %.1f, %.1f m", v->x / 100.0, v->z / 100.0);
        L("Type 0x%x%s%s", v->type, v->boundary ? " (mission boundary)" : "", v->shown ? ", selectable" : ", hidden");
        if (v->r > 0) L("Radius %.0f m", v->r / 100.0);
    }
#undef L
    return n;
}

/* ---- picking: the nearest thing under the pointer (pixels) */
/* the pointer's ray (game coordinates, cm): from the eye through the pixel; the map view: straight down its tilt */
static void pointer_ray(float mx, float my, float o[3], float d[3])
{
    if (g_top) {
        const float sc = (float)g_w / g_top_w, lp = TOP_PITCH * 3.14159265f / 180.0f, yc = ((float)g_h * 0.5f - my) / sc;
        o[0] = g_top_c[0] + (mx - (float)g_w * 0.5f) / sc; o[1] = TOP_EYE_Y; o[2] = g_top_c[1] - yc / sinf(lp);
        d[0] = 0; d[1] = sinf(lp); d[2] = cosf(lp);
    } else {
        const float ly = g_yaw * 3.14159265f / 180.0f, lp = g_pitch * 3.14159265f / 180.0f;
        const float F[3] = {cosf(lp) * sinf(ly), sinf(lp), cosf(lp) * cosf(ly)}, R[3] = {cosf(ly), 0.0f, -sinf(ly)};
        const float U[3] = {-sinf(lp) * sinf(ly), cosf(lp), -sinf(lp) * cosf(ly)};
        const float f = 1.0f / tanf(g_vfov * 3.14159265f / 360.0f) * (float)g_h * 0.5f;
        const float a = (mx - (float)g_w * 0.5f) / f, b = -(my - (float)g_h * 0.5f) / f;
        int k;
        for (k = 0; k < 3; k++) { o[k] = g_eye[k]; d[k] = F[k] + R[k] * a + U[k] * b; }
    }
}
/* the ray's entry distance into a box (1e30 if it misses) */
static float ray_box(const float o[3], const float d[3], const float mn[3], const float mx[3])
{
    float t0 = 0.0f, t1 = 1e30f;
    int k;
    for (k = 0; k < 3; k++) {
        if (fabsf(d[k]) < 1e-9f) { if (o[k] < mn[k] - 1.0f || o[k] > mx[k] + 1.0f) return 1e30f; continue; }
        {
            float a = (mn[k] - 1.0f - o[k]) / d[k], b = (mx[k] + 1.0f - o[k]) / d[k];
            if (a > b) { const float t = a; a = b; b = t; }
            if (a > t0) t0 = a;
            if (b < t1) t1 = b;
            if (t0 > t1) return 1e30f;
        }
    }
    return t0;
}
/* the ray's nearest hit on a part's polygons (1e30 if none) */
static float ray_part(const float o[3], const float d[3], const mech3d_part *p)
{
    float best = 1e30f;
    int q, c, k;
    const wtb_object *ob = p->model.object_count > 0 ? &p->model.objects[0] : NULL;
    if (!ob) return best;
    for (q = 0; q < ob->poly_count; q++) {
        const wtb_poly *pl = &ob->polys[q];
        float v0[3];
        if (pl->n < 3) continue;
        part_vertex(p, &ob->verts[pl->idx[0]], v0);
        for (c = 1; c + 1 < pl->n; c++) {   /* fan (Moller-Trumbore, both sides) */
            float v1[3], v2[3], e1[3], e2[3], pv[3], tv[3], qv[3], det, u, v, t;
            part_vertex(p, &ob->verts[pl->idx[c]], v1);
            part_vertex(p, &ob->verts[pl->idx[c + 1]], v2);
            for (k = 0; k < 3; k++) { e1[k] = v1[k] - v0[k]; e2[k] = v2[k] - v0[k]; tv[k] = o[k] - v0[k]; }
            pv[0] = d[1] * e2[2] - d[2] * e2[1]; pv[1] = d[2] * e2[0] - d[0] * e2[2]; pv[2] = d[0] * e2[1] - d[1] * e2[0];
            det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
            if (fabsf(det) < 1e-12f) continue;
            u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) / det;
            if (u < 0 || u > 1) continue;
            qv[0] = tv[1] * e1[2] - tv[2] * e1[1]; qv[1] = tv[2] * e1[0] - tv[0] * e1[2]; qv[2] = tv[0] * e1[1] - tv[1] * e1[0];
            v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) / det;
            if (v < 0 || u + v > 1) continue;
            t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) / det;
            if (t > 0 && t < best) best = t;
        }
    }
    return best;
}

static void pick(float mx, float my)
{
    float best = 1e30f, sx, sy;
    int i, kind = 0, idx = -1;
    if (g_show_units)
        for (i = 0; i < g_na; i++) {
            const float p[3] = {(float)g_ac[i].mech.origin[0], (float)g_ac[i].mech.origin[1] + 400.0f, (float)g_ac[i].mech.origin[2]};
            if (project(p, &sx, &sy)) {
                const float d = hypotf(sx - mx, sy - my) / (22.0f * g_dpi);
                if (d < 1.0f && d * 0.5f < best) { best = d * 0.5f; kind = 2; idx = i; }
            }
        }
    if (g_show_nav)
        for (i = 0; i < g_nn; i++) {
            const float p[3] = {g_nav[i].x, g_nav[i].y + 1500.0f, g_nav[i].z};
            if (project(p, &sx, &sy)) {
                const float d = hypotf(sx - mx, sy - my) / (16.0f * g_dpi);
                if (d < 1.0f && d * 0.5f < best) { best = d * 0.5f; kind = 3; idx = i; }
            }
        }
    {   /* the world: the nearest polygon the pointer's ray hits (a markers' hit within its radius wins) */
        float o[3], d[3], tbest = 1e30f;
        int hit = -1;
        pointer_ray(mx, my, o, d);
        for (i = 0; i < g_world.part_count; i++) {
            const mech3d_part *p = &g_world.parts[i];
            if (p->hidden || p->scrg) continue;
            if (ray_box(o, d, g_pmin[i], g_pmax[i]) >= tbest) continue;
            {
                const float t = ray_part(o, d, p);
                if (t < tbest) { tbest = t; hit = i; }
            }
        }
        if (hit >= 0 && kind == 0) { kind = 1; idx = hit; }
    }
    g_sel_kind = kind; g_sel = idx;
}

static void selection_point(float out[3])
{
    out[0] = out[1] = out[2] = 0;
    if (g_sel_kind == 1) { int k; for (k = 0; k < 3; k++) out[k] = (g_pmin[g_sel][k] + g_pmax[g_sel][k]) * 0.5f; }
    else if (g_sel_kind == 2) { out[0] = (float)g_ac[g_sel].mech.origin[0]; out[1] = (float)g_ac[g_sel].mech.origin[1] + 400.0f; out[2] = (float)g_ac[g_sel].mech.origin[2]; }
    else if (g_sel_kind == 3) { out[0] = g_nav[g_sel].x; out[1] = g_nav[g_sel].y; out[2] = g_nav[g_sel].z; }
}
static void goto_selection(void)
{
    float p[3], sz = 3000.0f;
    if (!g_sel_kind) return;
    selection_point(p);
    if (g_sel_kind == 1) sz = fmaxf(3000.0f, fmaxf(g_pmax[g_sel][0] - g_pmin[g_sel][0], g_pmax[g_sel][2] - g_pmin[g_sel][2]));
    if (g_top) { g_top_c[0] = p[0]; g_top_c[1] = p[2]; return; }
    {   /* back off along the current heading, a little above, looking at it */
        const float ly = g_yaw * 3.14159265f / 180.0f, d = sz * 2.2f;
        g_eye[0] = p[0] - sinf(ly) * d; g_eye[2] = p[2] - cosf(ly) * d; g_eye[1] = p[1] + d * 0.45f;
        g_pitch = -atan2f(d * 0.45f, d) * 180.0f / 3.14159265f;
    }
}

/* ---- the overlays */
static const float C_WHITE[4] = {1, 1, 1, 1}, C_PANEL[4] = {0, 0, 0, 0.72f}, C_DIM[4] = {0.75f, 0.78f, 0.8f, 1},
                   C_YOU[4] = {1, 1, 1, 1}, C_SEL[4] = {1.0f, 0.85f, 0.1f, 1}, C_NAV[4] = {0.35f, 0.85f, 1.0f, 1},
                   C_BND[4] = {1.0f, 0.55f, 0.1f, 0.9f}, C_SIDE[3][4] = {{0.3f, 1.0f, 0.35f, 1}, {1.0f, 0.3f, 0.25f, 1}, {0.85f, 0.85f, 0.85f, 1}};

static void draw_box3(const float mn[3], const float mx[3], const float c[4])
{
    static const int E[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    float s[8][2];
    int ok[8], k;
    for (k = 0; k < 8; k++) {
        const float p[3] = {k & 1 ? mx[0] : mn[0], k & 2 ? mx[1] : mn[1], k & 4 ? mx[2] : mn[2]};
        ok[k] = project(p, &s[k][0], &s[k][1]);
    }
    for (k = 0; k < 12; k++) if (ok[E[k][0]] && ok[E[k][1]]) ui_line(s[E[k][0]][0], s[E[k][0]][1], s[E[k][1]][0], s[E[k][1]][1], 2.0f * g_dpi, c);
}
static void marker(float x, float y, float r, const float c[4])
{
    ui_line(x - r, y, x, y - r, 2.0f * g_dpi, c); ui_line(x, y - r, x + r, y, 2.0f * g_dpi, c);
    ui_line(x + r, y, x, y + r, 2.0f * g_dpi, c); ui_line(x, y + r, x - r, y, 2.0f * g_dpi, c);
}
static void label_at(float x, float y, const char *s, const float c[4])
{
    const float w = text_w(s);
    const float bg[4] = {0, 0, 0, 0.55f};
    if (!g_show_labels || !ttext_available()) return;
    ui_rect(x - w * 0.5f - 3, y - 1, w + 6, line_h(), bg);
    ui_text(x - w * 0.5f, y + 1, s, c);
}

static void draw_overlays(void)
{
    float sx, sy;
    int i, k;
    const float r = 7.0f * g_dpi;
    if (g_show_bnd)
        for (i = 0; i < g_bn; i++)
            for (k = 0; k < 64; k++) {
                float ax, ay, bx, by;
                if (project(g_bnd[i][k], &ax, &ay) && project(g_bnd[i][(k + 1) & 63], &bx, &by)) ui_line(ax, ay, bx, by, 2.0f * g_dpi, C_BND);
            }
    if (g_show_nav)
        for (i = 0; i < g_nn; i++) {
            const mv_nav *v = &g_nav[i];
            const float p0[3] = {v->x, v->y, v->z}, p1[3] = {v->x, v->y + 1500.0f, v->z};
            float x0, y0;
            if (v->boundary) continue;
            if (project(p1, &sx, &sy)) {
                if (project(p0, &x0, &y0) && !g_top) ui_line(x0, y0, sx, sy, 1.5f * g_dpi, C_NAV);
                marker(sx, sy, r, C_NAV);
                label_at(sx, sy + r + 2, v->name, C_NAV);
            }
        }
    if (g_show_units) {
        for (i = 0; i < g_na; i++) {
            const float p[3] = {(float)g_ac[i].mech.origin[0], (float)g_ac[i].mech.origin[1] + 400.0f, (float)g_ac[i].mech.origin[2]};
            if (!project(p, &sx, &sy)) continue;
            {
                const float *c = C_SIDE[unit_side(i)];
                ui_line(sx - r, sy - r, sx + r, sy - r, 2.0f * g_dpi, c); ui_line(sx + r, sy - r, sx + r, sy + r, 2.0f * g_dpi, c);
                ui_line(sx + r, sy + r, sx - r, sy + r, 2.0f * g_dpi, c); ui_line(sx - r, sy + r, sx - r, sy - r, 2.0f * g_dpi, c);
                if (g_top) {   /* its heading */
                    const float h = g_ac[i].mech.heading * 3.14159265f / 180.0f;
                    ui_line(sx, sy, sx + sinf(h) * r * 2.2f, sy - cosf(h) * r * 2.2f, 2.0f * g_dpi, c);
                }
                if (cam_dist(p) < 60000.0f || g_top) label_at(sx, sy + r + 2, g_ac[i].skel, c);
            }
        }
        if (g_s_ok) {   /* the player's start */
            const float p[3] = {g_s.player[0], msim_ground(&g_s, g_s.player[0], g_s.player[2], 1e7f) + 400.0f, g_s.player[2]};
            if (project(p, &sx, &sy)) {
                ui_line(sx - r, sy, sx + r, sy, 2.0f * g_dpi, C_YOU); ui_line(sx, sy - r, sx, sy + r, 2.0f * g_dpi, C_YOU);
                label_at(sx, sy + r + 2, "YOUR START", C_YOU);
            }
        }
    }
    /* the selection */
    if (g_sel_kind == 1) draw_box3(g_pmin[g_sel], g_pmax[g_sel], C_SEL);
    else if (g_sel_kind == 2 && g_all.parts) {
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f}, a[3], b[3];
        for (k = g_act_first[g_sel]; k < g_act_first[g_sel] + g_act_count[g_sel]; k++) {
            int q;
            if (part_box(&g_all.parts[k], a, b)) for (q = 0; q < 3; q++) { if (a[q] < mn[q]) mn[q] = a[q]; if (b[q] > mx[q]) mx[q] = b[q]; }
        }
        if (mn[0] < 1e29f) draw_box3(mn, mx, C_SEL);
    } else if (g_sel_kind == 3) {
        const float p[3] = {g_nav[g_sel].x, g_nav[g_sel].y + 1500.0f, g_nav[g_sel].z};
        if (project(p, &sx, &sy)) marker(sx, sy, r * 1.6f, C_SEL);
    }
}

static void draw_panels(void)
{
    char buf[256], lines[24][96];
    const float pad = 8.0f * g_dpi, lh = line_h();
    int n, i;
    /* status */
    snprintf(buf, sizeof buf, "%s (%s)   %d objects, %d units   %s   %.0f, %.0f m   %s",
             g_cur >= 0 ? g_ms[g_cur].title : "-", g_cur >= 0 ? g_ms[g_cur].scene : "", g_world.part_count, g_na,
             g_top ? "MAP" : "3D", (g_top ? g_top_c[0] : g_eye[0]) / 100.0, (g_top ? g_top_c[1] : g_eye[2]) / 100.0,
             g_top ? "" : "");
    if (g_top) { char z[40]; snprintf(z, sizeof z, "width %.0f m", g_top_w / 100.0); strncat(buf, z, sizeof buf - strlen(buf) - 1); }
    else { char z[40]; snprintf(z, sizeof z, "speed %.0f m/s", g_speed / 100.0); strncat(buf, z, sizeof buf - strlen(buf) - 1); }
    ui_rect(0, 0, (float)g_w, lh + pad, C_PANEL);
    ui_text(pad, pad * 0.5f, buf, C_WHITE);
    /* the selection */
    if ((n = info_lines(lines, 24)) > 0) {
        float w = 0, x, y = lh + pad * 2;
        for (i = 0; i < n; i++) w = fmaxf(w, text_w(lines[i]));
        x = (float)g_w - w - pad * 3;
        ui_rect(x - pad, y - pad * 0.5f, w + pad * 2, (float)n * lh + pad, C_PANEL);
        for (i = 0; i < n; i++) ui_text(x, y + (float)i * lh, lines[i], i ? C_WHITE : C_SEL);
    }
    /* the help / keys line */
    {
        static const char *const HELP[] = {
            "W A S D  move        Q / E  down / up        Shift  faster        drag  look (map: pan)        wheel  speed (map: zoom)",
            "click  select        F  go to selection        Home  your start        Tab  3D / map view",
            "M  mission list        PgUp / PgDn  previous / next mission",
            "N  nav points        U  units        B  boundary        L  labels        G  fog        H  this help        Esc  close / deselect"};
        if (g_help) {
            const int nh = (int)(sizeof HELP / sizeof HELP[0]);
            float w = 0, y;
            for (i = 0; i < nh; i++) w = fmaxf(w, text_w(HELP[i]));
            y = (float)g_h - (float)nh * lh - pad * 2;
            ui_rect(0, y - pad * 0.5f, (float)g_w, (float)nh * lh + pad * 2.5f, C_PANEL);
            for (i = 0; i < nh; i++) ui_text(pad, y + (float)i * lh, HELP[i], C_DIM);
        } else {
            const char *hint = g_msg[0] ? g_msg : "H  help     M  missions     Tab  map view     click  select";
            ui_rect(0, (float)g_h - lh - pad, text_w(hint) + pad * 2, lh + pad, C_PANEL);
            ui_text(pad, (float)g_h - lh - pad * 0.5f, hint, C_DIM);
        }
    }
    /* the mission list */
    if (g_list) {
        const float x = pad, y0 = lh + pad * 2, w = 320.0f * g_dpi;
        const int rows = (int)(((float)g_h - y0 - lh * (g_help ? 6.5f : 3.0f)) / lh);   /* (above the help / keys line) */
        if (g_list_top > g_mn - rows) g_list_top = g_mn - rows;
        if (g_list_top < 0) g_list_top = 0;
        ui_rect(x - pad * 0.5f, y0 - pad * 0.5f, w, (float)(rows < g_mn ? rows : g_mn) * lh + pad, C_PANEL);
        for (i = 0; i < rows && g_list_top + i < g_mn; i++) {
            const int m = g_list_top + i;
            snprintf(buf, sizeof buf, "%-9s %s", g_ms[m].scene, g_ms[m].title);
            ui_text(x, y0 + (float)i * lh, buf, m == g_cur ? C_SEL : C_WHITE);
        }
    }
}
static int list_hit(float mx, float my)
{
    const float pad = 8.0f * g_dpi, lh = line_h(), y0 = lh + pad * 2, w = 320.0f * g_dpi;
    int row;
    if (!g_list || mx > w || my < y0) return -1;
    row = (int)((my - y0) / lh);
    if (row >= (int)(((float)g_h - y0 - lh * (g_help ? 6.5f : 3.0f)) / lh)) return -1;
    return g_list_top + row < g_mn ? g_list_top + row : -1;
}

/* ---- the scrub / ground-detail patch (a record's SCRG object): it follows the camera on a grid, as the simulation's
 * (glview scrub_follow, engine 0x10011570) - the near ground's texture detail in many missions */
static void scrub_follow(float camx, float camz)
{
    int k, any = 0, step = world3d_scrg_step, h, x = (int)camx, z = (int)camz, nx, nz;
    for (k = 0; k < g_world.part_count; k++) if (g_world.parts[k].scrg) { any = 1; break; }
    if (!any || step <= 0) return;
    h = ((step >> 3) + step) >> 1;
    nx = g_scrg_cx; nz = g_scrg_cz;
    if (abs(x - g_scrg_gx) > h || abs(z - g_scrg_gz) > h) {
        nx = (x + (x < 0 ? -step : step) / 2) / step;
        nz = (z + (z < 0 ? -step : step) / 2) / step;
    }
    if (nx != g_scrg_cx || nz != g_scrg_cz) {
        const int dx = nx * step - g_scrg_gx, dz = nz * step - g_scrg_gz;
        for (k = 0; k < g_world.part_count; k++) if (g_world.parts[k].scrg) { g_world.parts[k].pos[0] += dx; g_world.parts[k].pos[2] += dz; }
        g_scrg_gx += dx; g_scrg_gz += dz; g_scrg_cx = nx; g_scrg_cz = nz;
        glr_set_mech(g_r, &g_world, (const uint8_t (*)[3])g_sc.palette, g_sc.slot, 0, 0);
    }
}

/* the fire / smoke cards turned to face the eye (glview: yaw = the heading of eye - card, every frame), and the animated
 * texture slots (fires, smoke, flags) stepped by the clock as the engine does (glview scene_anim, 182 ticks a second) */
static void cards_and_frames(const float eye[3])
{
    int k;
    const long t = (long)(SDL_GetTicks() * 182 / 1000);
    for (k = 0; k < g_nface; k++) {
        const mech3d_part *fp = &g_world.parts[g_face_src[k]];
        g_all.parts[g_face_dst[k]].yaw = mech3d_heading(eye[0] - (float)fp->pos[0], eye[2] - (float)fp->pos[2]);
    }
    for (k = 0; k < GLR_SLOTS; k++)
        if (g_sc.nfr[k] >= 2 && g_sc.rate[k] > 0 && g_sc.mode[k] != 0) {
            const long st = t / g_sc.rate[k];
            const int f = g_sc.mode[k] == 2 && st >= g_sc.nfr[k] ? 0 : (int)(st % g_sc.nfr[k]);
            glr_slot_frame(g_r, k, g_sc.fr[k][f] ? g_sc.fr[k][f] : NULL);
        }
    if (g_all.parts) glr_set_actors(g_r, &g_all, (const uint8_t (*)[3])g_sc.palette, g_sc.slot, 0, 0);
}

/* ---- the frame */
static void draw_frame(void)
{
    glr_view v = g_base;
    scrub_follow(g_top ? g_top_c[0] : g_eye[0], g_top ? g_top_c[1] : g_eye[2]);
    rb_frame(g_w, g_h);
    rb_target(0);
    v.vfov = g_vfov;
    v.free_cam = 1;
    v.fog_density = g_fog_on ? g_fog_density : 0.0f;
    if (g_top) {
        v.eye[0] = g_top_c[0]; v.eye[2] = g_top_c[1];
        v.eye[1] = TOP_EYE_Y;
        v.look_yaw = 0; v.look_pitch = TOP_PITCH;
        v.ortho_w = g_top_w;
        v.ambient = fminf(1.0f, v.ambient + 0.3f);   /* (lit from straight above, the map reads better a little brighter) */
    } else {
        memcpy(v.eye, g_eye, sizeof v.eye);
        v.look_yaw = g_yaw; v.look_pitch = g_pitch;
    }
    cards_and_frames(v.eye);
    glr_draw(g_r, &v, g_w, g_h);
    hud_begin_stretched(g_hud, g_w, g_h);
    draw_overlays();
    draw_panels();
    hud_end(g_hud);
}

static void write_shot(const char *path)
{
    unsigned char *px = malloc((size_t)g_w * (size_t)g_h * 3);
    FILE *f;
    int y;
    if (!px) return;
    rb_read_rgb(0, 0, g_w, g_h, px);
    if ((f = fopen(path, "wb"))) {
        fprintf(f, "P6\n%d %d\n255\n", g_w, g_h);
        for (y = g_h - 1; y >= 0; y--) fwrite(px + (size_t)y * (size_t)g_w * 3, 1, (size_t)g_w * 3, f);
        fclose(f);
    }
    free(px);
}

/* ---- the game folder (mw2port.cfg, else ask) */
static volatile int g_pick_done;
static char g_pick[PORTCFG_PATH];
static void SDLCALL picked(void *ud, const char *const *list, int filter)
{
    (void)ud; (void)filter;
    if (list && list[0]) snprintf(g_pick, sizeof g_pick, "%s", list[0]);
    g_pick_done = 1;
}

int main(int argc, char **argv)
{
    char err[256], models[PORTCFG_PATH], textures[PORTCFG_PATH], kind[32];
    const char *base, *hint = NULL, *want = NULL, *shot = getenv("MW2_MAPVIEW_SHOT");
    portcfg cfg;
    SDL_Window *win;
    int i, running = 1, drag = 0, dragged = 0;
    float dx0 = 0, dy0 = 0;
    Uint64 last;

    for (i = 1; i < argc; i++) {
        size_t l = strlen(argv[i]);
        if (l == 8 && strcasecmp(argv[i] + 4, "SCN1") == 0) want = argv[i]; else hint = argv[i];
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    base = SDL_GetBasePath();
    portcfg_defaults(&cfg);
    portcfg_load(NULL, &cfg);
    if (portcfg_resolve(&cfg, base, hint) != 0 || portcfg_edition(&cfg, PORTCFG_ED_ENHANCED, models, textures, kind, g_sky, g_fog) != 0) {
        if (!shot) {   /* ask for the folder */
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, MV_TITLE,
                "Choose your MechWarrior 2 game folder (the one the port uses, with the 3d folder inside).", NULL);
            SDL_ShowOpenFolderDialog(picked, NULL, NULL, NULL, 0);
            while (!g_pick_done) { SDL_Event e; while (SDL_PollEvent(&e)) {} SDL_Delay(20); }
            portcfg_defaults(&cfg);
            portcfg_load(NULL, &cfg);
            cfg.game[0] = cfg.install[0] = cfg.cd[0] = cfg.models[0] = cfg.textures[0] = 0;
            if (g_pick[0]) portcfg_resolve(&cfg, base, g_pick);
        }
        if (!cfg.game[0] && !cfg.install[0]) { fprintf(stderr, "mapview: no game folder\n"); return 1; }
        if (portcfg_edition(&cfg, PORTCFG_ED_ENHANCED, models, textures, kind, g_sky, g_fog) != 0) {
            const char *m = "The map viewer needs the 3D editions' files: the game folder's 3d folder with models.prj and textures.prj.";
            fprintf(stderr, "mapview: %s\n", m);
            if (!shot) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, MV_TITLE, m, NULL);
            return 1;
        }
    }
    g_kind = !strcmp(kind, "ati") ? 1 : !strcmp(kind, "pvr") ? 2 : !strcmp(kind, "mga") ? 4 : 0;
    dp_clear_roots();
    if (cfg.install[0]) dp_add_root(cfg.install);
    if (cfg.cd[0]) {
        const size_t k = strlen(cfg.cd);
        if (k > 4 && strcasecmp(cfg.cd + k - 4, ".iso") == 0) dp_add_iso_root(cfg.cd, "MECH2");
        else { char m2[PORTCFG_PATH + 8]; snprintf(m2, sizeof m2, "%s/MECH2", cfg.cd); dp_add_root(m2); dp_add_root(cfg.cd); }
    }
    if (!(g_ma = prj_open(models, err, sizeof err)) || !(g_ta = prj_open(textures, err, sizeof err))) { fprintf(stderr, "mapview: %s\n", err); return 1; }
    printf("mapview: game folder %s\nmapview: models %s, textures %s (%s)\n", cfg.game[0] ? cfg.game : cfg.install, models, textures, kind);
    ttext_init(base);
    missions_list(g_ma);
    if (!g_mn) { fprintf(stderr, "mapview: no missions in %s\n", models); return 1; }

    {
        SDL_PropertiesID p = SDL_CreateProperties();
        SDL_SetStringProperty(p, SDL_PROP_WINDOW_CREATE_TITLE_STRING, MV_TITLE);
        SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, shot ? 1280 : 1600);
        SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, shot ? 720 : 900);
        SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, (Sint64)(SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | rb_window_flags(0)));
        win = SDL_CreateWindowWithProperties(p);
        SDL_DestroyProperties(p);
    }
    if (!win || rb_init(win, RB_GPU_AUTO, 4, err, sizeof err) != 0) {
        char m[400];
        snprintf(m, sizeof m, "No GPU device: %s", err[0] ? err : SDL_GetError());
        fprintf(stderr, "mapview: %s\n", m);
        if (!shot) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, MV_TITLE, m, win);
        return 1;
    }
    if (!(g_r = glr_create()) || !(g_hud = hud_create(g_ma))) { fprintf(stderr, "mapview: renderer\n"); return 1; }
    {
        int start = 0;
        if (want) for (i = 0; i < g_mn; i++) if (!strcasecmp(g_ms[i].scene, want)) start = i;
        load_mission(start);
    }
    if (getenv("MW2_MAPVIEW_CAM")) {
        float c[5] = {0, 0, 0, 0, 0};
        sscanf(getenv("MW2_MAPVIEW_CAM"), "%f,%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3], &c[4]);
        g_eye[0] = c[0] * 100.0f; g_eye[1] = c[1] * 100.0f; g_eye[2] = c[2] * 100.0f; g_yaw = c[3]; g_pitch = c[4];
        g_top_c[0] = g_eye[0]; g_top_c[1] = g_eye[2];
    }
    if (getenv("MW2_MAPVIEW_TOP")) { g_top = 1; g_top_w = (float)atof(getenv("MW2_MAPVIEW_TOP")) * 100.0f; }
    SDL_SetWindowTitle(win, g_cur >= 0 ? g_ms[g_cur].title : MV_TITLE);
    {
        int ww, wh;
        SDL_GetWindowSizeInPixels(win, &g_w, &g_h);
        SDL_GetWindowSize(win, &ww, &wh);
        g_dpi = ww > 0 ? (float)g_w / (float)ww : 1.0f;
        if (g_dpi < 1.0f) g_dpi = 1.0f;
    }
    if (getenv("MW2_MAPVIEW_CYCLE")) {   /* tests: every mission loaded in turn and drawn once (map and 3D), back to the first */
        int c, nc = atoi(getenv("MW2_MAPVIEW_CYCLE"));
        for (c = 0; c < nc; c++) { load_mission((g_cur + 1) % g_mn); g_top = c & 1; draw_frame(); rb_present(); }
        g_top = 0;
    }
    if (getenv("MW2_MAPVIEW_LIST")) g_list = 1;   /* tests */
    if (getenv("MW2_MAPVIEW_HELP")) g_help = 1;
    if (getenv("MW2_MAPVIEW_CLICK")) { float cx = 0, cy = 0; sscanf(getenv("MW2_MAPVIEW_CLICK"), "%f,%f", &cx, &cy); pick(cx, cy); }
    if (shot) {
        draw_frame();
        write_shot(shot);
        if (g_sel_kind) { char lines[24][96]; int n = info_lines(lines, 24); for (i = 0; i < n; i++) printf("info: %s\n", lines[i]); }
        printf("mapview: %dx%d -> %s\n", g_w, g_h, shot);
        rb_finish();
        running = 0;
    }

    last = SDL_GetTicksNS();
    while (running) {
        SDL_Event e;
        static int frame;
        if (getenv("MW2_MAPVIEW_FRAMES")) {   /* tests: n frames of the window's loop; MW2_MAPVIEW_EVENTS="frame:k<keycode>|c<x>,<y>|d<dx>,<dy>;..."
                                               * pushes a key press, a click or a drag at that frame; the last frame -> MW2_MAPVIEW_OUT */
            const char *ev = getenv("MW2_MAPVIEW_EVENTS");
            while (ev && *ev) {
                int f = atoi(ev);
                const char *c = strchr(ev, ':');
                if (!c) break;
                if (f == frame) {
                    SDL_Event q;
                    memset(&q, 0, sizeof q);
                    if (c[1] == 'k') { q.type = SDL_EVENT_KEY_DOWN; q.key.key = (SDL_Keycode)strtol(c + 2, NULL, 0); q.key.down = true; SDL_PushEvent(&q); }
                    else if (c[1] == 'c' || c[1] == 'd') {
                        float x = 0, y = 0;
                        sscanf(c + 2, "%f,%f", &x, &y);
                        q.type = SDL_EVENT_MOUSE_BUTTON_DOWN; q.button.button = SDL_BUTTON_LEFT; q.button.down = true;
                        q.button.x = c[1] == 'c' ? x : 400; q.button.y = c[1] == 'c' ? y : 300; SDL_PushEvent(&q);
                        if (c[1] == 'd') { memset(&q, 0, sizeof q); q.type = SDL_EVENT_MOUSE_MOTION; q.motion.x = 400 + x; q.motion.y = 300 + y; q.motion.xrel = x; q.motion.yrel = y; SDL_PushEvent(&q); }
                        memset(&q, 0, sizeof q); q.type = SDL_EVENT_MOUSE_BUTTON_UP; q.button.button = SDL_BUTTON_LEFT;
                        q.button.x = c[1] == 'c' ? x : 400 + x; q.button.y = c[1] == 'c' ? y : 300 + y; SDL_PushEvent(&q);
                    }
                }
                ev = strchr(c, ';');
                if (ev) ev++;
            }
            if (++frame > atoi(getenv("MW2_MAPVIEW_FRAMES"))) {
                if (getenv("MW2_MAPVIEW_OUT")) { draw_frame(); write_shot(getenv("MW2_MAPVIEW_OUT")); }
                printf("mapview: loop end - mission %s, %s view, yaw %.1f pitch %.1f, centre %.0f,%.0f, selection %d/%d, list %d\n", g_cur >= 0 ? g_ms[g_cur].scene : "-",
                       g_top ? "map" : "3D", (double)g_yaw, (double)g_pitch, (double)(g_top ? g_top_c[0] : g_eye[0]), (double)(g_top ? g_top_c[1] : g_eye[2]), g_sel_kind, g_sel, g_list);
                break;
            }
        }
        const Uint64 now = SDL_GetTicksNS();
        float dt = (float)(now - last) / 1e9f, mxs = 1.0f;
        int ww, wh;
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        SDL_GetWindowSize(win, &ww, &wh);
        SDL_GetWindowSizeInPixels(win, &g_w, &g_h);
        mxs = ww > 0 ? (float)g_w / (float)ww : 1.0f;   /* event coordinates are in window points */
        g_dpi = mxs < 1.0f ? 1.0f : mxs;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = 0;
            else if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode k = e.key.key;
                g_msg[0] = 0;
                if (k == SDLK_ESCAPE) { if (g_list) g_list = 0; else if (g_help) g_help = 0; else g_sel_kind = 0; }
                else if (k == SDLK_TAB) {
                    if (!g_top) { g_top_c[0] = g_eye[0]; g_top_c[1] = g_eye[2]; }
                    else { g_eye[0] = g_top_c[0]; g_eye[2] = g_top_c[1]; if (g_s_ok) g_eye[1] = msim_ground(&g_s, g_eye[0], g_eye[2], 1e7f) + 2500.0f; }
                    g_top = !g_top;
                }
                else if (k == SDLK_M) g_list = !g_list;
                else if (k == SDLK_H || k == SDLK_F1) g_help = !g_help;
                else if (k == SDLK_N) g_show_nav = !g_show_nav;
                else if (k == SDLK_U) g_show_units = !g_show_units;
                else if (k == SDLK_B) g_show_bnd = !g_show_bnd;
                else if (k == SDLK_L) g_show_labels = !g_show_labels;
                else if (k == SDLK_G) g_fog_on = !g_fog_on;
                else if (k == SDLK_F) goto_selection();
                else if (k == SDLK_HOME) goto_start();
                else if (k == SDLK_PAGEDOWN) load_mission((g_cur + 1) % g_mn);
                else if (k == SDLK_PAGEUP) load_mission((g_cur + g_mn - 1) % g_mn);
                if ((k == SDLK_PAGEDOWN || k == SDLK_PAGEUP) && g_cur >= 0) SDL_SetWindowTitle(win, g_ms[g_cur].title);
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT)) {
                const int li = e.button.button == SDL_BUTTON_LEFT ? list_hit(e.button.x * mxs, e.button.y * mxs) : -1;
                if (li >= 0) { load_mission(li); g_list = 0; SDL_SetWindowTitle(win, g_ms[g_cur].title); continue; }
                drag = e.button.button; dragged = 0; dx0 = e.button.x; dy0 = e.button.y;
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == (Uint8)drag) {
                if (!dragged && drag == SDL_BUTTON_LEFT) pick(e.button.x * mxs, e.button.y * mxs);
                drag = 0;
                SDL_SetWindowRelativeMouseMode(win, false);
            } else if (e.type == SDL_EVENT_MOUSE_MOTION && drag) {
                if (!dragged && hypotf(e.motion.x - dx0, e.motion.y - dy0) > 4.0f) { dragged = 1; if (!g_top) SDL_SetWindowRelativeMouseMode(win, true); }
                if (dragged) {
                    if (g_top) { const float sc = g_top_w / (float)g_w * mxs; g_top_c[0] -= e.motion.xrel * sc; g_top_c[1] += e.motion.yrel * sc; }
                    else {
                        g_yaw += e.motion.xrel * 0.25f;
                        g_pitch -= e.motion.yrel * 0.25f;
                        if (g_pitch > 89.0f) g_pitch = 89.0f;
                        if (g_pitch < -89.0f) g_pitch = -89.0f;
                        if (g_yaw > 180.0f) g_yaw -= 360.0f;
                        if (g_yaw < -180.0f) g_yaw += 360.0f;
                    }
                }
            } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                float mx, my;
                SDL_GetMouseState(&mx, &my);
                if (g_list && list_hit(mx * mxs, my * mxs) >= 0) g_list_top -= (int)(e.wheel.y * 3.0f);
                else if (g_top) {   /* zoom about the pointer */
                    const float k = e.wheel.y > 0 ? 0.8f : 1.25f, sc = g_top_w / (float)g_w;
                    const float wx = g_top_c[0] + (mx * mxs - (float)g_w * 0.5f) * sc, wz = g_top_c[1] - (my * mxs - (float)g_h * 0.5f) * sc;
                    g_top_w *= k;
                    if (g_top_w < 2000.0f) g_top_w = 2000.0f;
                    if (g_top_w > 4000000.0f) g_top_w = 4000000.0f;
                    g_top_c[0] = wx - (wx - g_top_c[0]) * k; g_top_c[1] = wz - (wz - g_top_c[1]) * k;
                } else {
                    g_speed *= e.wheel.y > 0 ? 1.25f : 0.8f;
                    if (g_speed < 200.0f) g_speed = 200.0f;
                    if (g_speed > 200000.0f) g_speed = 200000.0f;
                }
            }
        }
        {   /* movement */
            const bool *ks = SDL_GetKeyboardState(NULL);
            const float sp = g_speed * dt * (ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT] ? 5.0f : 1.0f);
            const float fw = (ks[SDL_SCANCODE_W] || ks[SDL_SCANCODE_UP] ? 1.0f : 0.0f) - (ks[SDL_SCANCODE_S] || ks[SDL_SCANCODE_DOWN] ? 1.0f : 0.0f);
            const float st = (ks[SDL_SCANCODE_D] || ks[SDL_SCANCODE_RIGHT] ? 1.0f : 0.0f) - (ks[SDL_SCANCODE_A] || ks[SDL_SCANCODE_LEFT] ? 1.0f : 0.0f);
            const float up = (ks[SDL_SCANCODE_E] || ks[SDL_SCANCODE_SPACE] ? 1.0f : 0.0f) - (ks[SDL_SCANCODE_Q] || ks[SDL_SCANCODE_C] ? 1.0f : 0.0f);
            if (g_top) {
                const float m = g_top_w * 0.6f * dt * (ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT] ? 3.0f : 1.0f);
                g_top_c[0] += st * m; g_top_c[1] += fw * m;
            } else {
                const float ly = g_yaw * 3.14159265f / 180.0f, lp = g_pitch * 3.14159265f / 180.0f;
                g_eye[0] += (cosf(lp) * sinf(ly) * fw + cosf(ly) * st) * sp;
                g_eye[1] += (sinf(lp) * fw + up) * sp;
                g_eye[2] += (cosf(lp) * cosf(ly) * fw - sinf(ly) * st) * sp;
            }
        }
        draw_frame();
        rb_present();
        if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) SDL_Delay(30);
    }
    free_mission();
    hud_destroy(g_hud);
    glr_destroy(g_r);
    rb_quit();
    SDL_DestroyWindow(win);
    SDL_Quit();
    prj_close(g_ma); prj_close(g_ta);
    return 0;
}
