/*
 * mw2shell - the MechWarrior 2 shell (menus), rebuilt from MW2SHELL.EXE's data and logic.
 *   mw2shell <MW2 install dir>
 * Screens follow the original state machine (FUN_00036700): each screen is a DATABASE.MW2
 * background, Smacker overlays and hotspots whose rectangles, labels and transitions come from
 * the shell's tables (title: FUN_0002a410, hotspot table 0x7e7fc).
 * Headless capture: MW2_SHELL_SHOT=out.ppm MW2_SHELL_FRAMES=n MW2_SHELL_MOUSE=x,y MW2_SHELL_SCREEN=id
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "gusmid.h"
#include "lance.h"
#include "mek.h"
#include "shp.h"
#include "mt32mid.h"
#include "bwd.h"
#include "cockpitcfg.h"
#include "portcfg.h"
#include "datapath.h"
#include "ttext.h"
#include "prj.h"
#include "sfx.h"
#include "shelldb.h"
#include "smk.h"

#define W 640
#define H 480

typedef struct { int x0, y0, x1, y1, lx, ly; const char *label; int next, clan; const char *video; } hotspot;
typedef struct { const char *name; int x, y; } overlay_def;
typedef struct { int id; const char *name; int bg; const char *smk; int sx, sy; int ambient; const hotspot *hs; int nhs;
                 int clan; const overlay_def *ov; int nov; } screen_def;

/* title (screen 8): FUN_0002a410, hotspots 0x7e7fc; actions: 0 -> 7 (clan 2), 1 -> 15 (Wolf), 2 -> 15 (Falcon), 3 -> exit */
static const hotspot TITLE_HS[] = {
    {219, 294, 426, 419, 320, 395, "TRIALS OF GRIEVANCE", 7, 2, NULL},
    {427, 245, 634, 373, 525, 374, "WOLF CLAN HALL", 15, 0, NULL},
    {10, 197, 200, 370, 124, 374, "JADE FALCON CLAN HALL", 15, 1, NULL},
    {0, 450, 639, 479, 320, 455, "EXIT", -3, -1, NULL},
};
/* clan halls (screen 1): FUN_0001dbe0; per-clan records 0x7f08c (16 bytes: hotspots, 5, background slot,
 * music): Wolf 0x7d52c / slot 11, Jade Falcon 0x7de94 / slot 18. Actions: 0 CADET TRAINING -> 14,
 * 1 ARCHIVE HOLOPROJECTOR -> 5, 2 READY ROOM -> 11 (the ending once the pilot's rank reaches 16),
 * 3 REGISTER -> registration video -> 12, 4 EXIT -> 8 */
static const hotspot WOLF_HALL_HS[] = {
    {185, 280, 240, 400, 197, 327, "CADET TRAINING", 14, 0, NULL},
    {320, 300, 470, 400, 246, 371, "ARCHIVE HOLOPROJECTOR", 5, 0, "AWOHOLOP"},
    {20, 245, 90, 411, 25, 307, "READY ROOM", 11, 0, NULL},
    {95, 385, 180, 479, 80, 455, "REGISTER", 12, 0, "AWORGSTR"},
    {0, 0, 639, 40, 320, 15, "EXIT", 8, 2, NULL},
};
static const hotspot FALCON_HALL_HS[] = {
    {66, 167, 152, 303, 69, 187, "CADET TRAINING", 14, 1, NULL},
    {160, 290, 375, 322, 110, 340, "ARCHIVE HOLOPROJECTOR", 5, 1, "AJFHOLOP"},
    {523, 154, 636, 332, 450, 200, "READY ROOM", 11, 1, NULL},
    {397, 385, 492, 466, 397, 443, "REGISTER", 12, 1, "AJFRGSTR"},
    {0, 0, 639, 40, 320, 15, "EXIT", 8, 2, NULL},
};
/* the halls' looping clips (FUN_000399b0 slot, name, x, y) */
static const overlay_def WOLF_HALL_OV[] = {{"AWOBALL", 91, 380}, {"AWOARCHT", 328, 332}, {"AWOLITE1", 0, 280}, {"AWOLITE2", 158, 300}, {"AWOLITE3", 580, 275}};
static const overlay_def FALCON_HALL_OV[] = {{"AJFBALL", 400, 384}, {"AJFARCHT", 220, 280}};
static const screen_def SCREENS[] = {
    {8, "title", 0, "AMWLOGO1", 111, 33, 73, TITLE_HS, 4, -1, NULL, 0},   /* ambient: getter index 0x4a, 1-based -> entry 73 */
    {7, "Instant Action (Trials of Grievance)", 8, NULL, 0, 0, -1, NULL, 0, -1, NULL, 0},
    {1, "Wolf clan hall", 10, NULL, 0, 0, -1, WOLF_HALL_HS, 5, 0, WOLF_HALL_OV, 5},
    {1, "Jade Falcon clan hall", 17, NULL, 0, 0, -1, FALCON_HALL_HS, 5, 1, FALCON_HALL_OV, 2},
};

static uint8_t g_fb[W * H * 3];

/* ---- music (MW2SHELL main loop): per-screen numbers for contexts Wolf / Falcon / neutral
 * (tables 0x838c8 / 0x83910 / 0x83880); 0x20000000 keep, 0x10000000 stop first, 0 no change.
 * DATABASE index = number + driver offset (ULTRA.MDI: 32, table 0x83e14); entry = index - 1. */
static const uint32_t MUSIC[3][17] = {
    {0x25, 0x24, 0, 0, 0x20000000, 0x24, 0, 0x23, 0x20000000, 0x25, 0x20000000, 0x25, 0x24, 0x25, 0x26, 0x20000000, 0x20000000},
    {0x28, 0x27, 0, 0, 0x20000000, 0x27, 0, 0x23, 0x20000000, 0x28, 0x20000000, 0x28, 0x27, 0x28, 0x29, 0x20000000, 0x20000000},
    {0x23, 0, 0, 0, 0x20000000, 0, 0, 0x23, 0x20000000, 0x23, 0x20000000, 0, 0, 0x23, 0, 0x20000000, 0x20000000},
};
#define GUS_MUSIC_OFFSET 32
static gus_bank *g_bank;
static gus_song *g_song;
static uint8_t  *g_song_data;
static int       g_song_entry = -1;
static mt32_synth *g_mt32;   /* the MT-32 (the menu music's default); NULL: Gravis patches if present */
static mt32_song  *g_msong;
static long g_song_len;
static int  g_song_rate = 44100;
static void music_cb(void *u, float *out, int frames)
{
    (void)u;
    if (g_mt32) {   /* the MT-32 song set; loops */
        if (g_msong && !mt32_song_render(g_msong, out, frames) && g_song_data && g_song_len > 0) {
            mt32_song_close(g_msong);
            g_msong = mt32_song_open(g_mt32, g_song_data, (size_t)g_song_len);
        }
        return;
    }
    if (g_song && !gus_song_render(g_song, out, frames) && g_song_data && g_song_len > 0) {
        /* ended: the menu music loops (the shell's songs repeat while you stay on a screen) */
        gus_song_close(g_song);
        g_song = gus_song_open(g_bank, g_song_data, (size_t)g_song_len, g_song_rate);
    }
}
static shelldb g_db;
static sfx *g_sfx;
static char g_dir[512];
static portcfg g_cfg;          /* the per-user configuration (paths, display) */
static char    g_exe_dir[1024];
#ifdef MW2_ONE_BINARY
static char    g_exe_path[1100];   /* this executable: the shell runs it again with --sim */
#endif
/* a game file for reading: the install folder first, then the disc (datapath roots); else <install>/name */
static const char *gpath(const char *name)
{
    static char out[4][1100];
    static int k;
    char *o = out[k = (k + 1) & 3];
    if (dp_resolve(name, o, sizeof out[0]) != 0) snprintf(o, sizeof out[0], "%s/%s", g_dir, name);
    return o;
}

/* ---- fonts ("1." format: glyph count, height, transparent index, offsets; glyph = u32 width + w*h)
 * entry 27 (16 px, getter index 0x1c): hotspot labels; entry 25 (8 px, index 0x1a): lists.
 * remap1 >= 0 draws glyph colour 1 as that palette index (Instant Action uses 0x22). */
typedef struct { uint8_t *d; long len; } font;
static font g_flabel, g_flist, g_fthin, g_fhead, g_fbig, g_f26, g_f28;   /* + 29: white large capitals (roster) */   /* DATABASE fonts 27, 25, 31 (thin), 30 (grey capitals) */
static int text_width(const font *fo, const char *t)
{
    int w = 0;
    if (!fo->d) return 0;
    for (; *t; t++) {
        int c = (unsigned char)*t & 127;
        uint32_t o = fo->d[16 + c * 4] | fo->d[17 + c * 4] << 8 | fo->d[18 + c * 4] << 16;
        w += (o + 4 <= (uint32_t)fo->len) ? (fo->d[o] | fo->d[o + 1] << 8) : 4;
    }
    return w;
}
static int font_height(const font *fo) { return fo->d && fo->len > 12 ? (fo->d[8] | fo->d[9] << 8) : 16; }   /* header: "1." , glyph count, height, transparent index */
/* ---- text. The original shell fonts, but not rasterised into the 640x480 frame: with a renderer, each call is queued
 * and drawn after the frame as glyph textures at the display's resolution - the glyph upscaled 3x with Scale3x (edge-
 * aware, keeps the letterforms) and linearly filtered - so text stays crisp in native fullscreen. Headless captures
 * still rasterise into the frame. The queue is cleared when a frame starts (draw_bg / text_layer_clear). */
static int g_text_hires;   /* set once a renderer exists */
typedef struct { const font *fo; int x, y, remap1; char t[96]; const uint8_t (*pal)[3]; uint32_t palsum; } text_item;
static text_item g_text[256];
static int g_ntext;
static int g_ntt;
static void text_layer_clear(void) { g_ntext = 0; g_ntt = 0; }
static void text_layer_cull(int x0, int y0, int x1, int y1)   /* a dimmed / covered area hides the text under it */
{
    int i, n = 0;
    for (i = 0; i < g_ntext; i++) {
        const text_item *it = &g_text[i];
        int fh = it->fo->d ? it->fo->d[8] : 10;
        if (it->x < x1 && it->x + 400 > x0 && it->y < y1 && it->y + fh > y0 && it->x >= x0 && it->y >= y0 && it->y + fh <= y1) continue;
        g_text[n++] = g_text[i];
    }
    g_ntext = n;
}
static void raster_text(const font *fo, int x, int y, const char *t, const uint8_t (*pal)[3], int remap1)
{
    int fh, key;
    if (!fo->d) return;
    fh = fo->d[8]; key = fo->d[12];
    for (; *t; t++) {
        int c = (unsigned char)*t & 127, gx, gy, gw;
        uint32_t o = fo->d[16 + c * 4] | fo->d[17 + c * 4] << 8 | fo->d[18 + c * 4] << 16;
        if (o + 4 > (uint32_t)fo->len) { x += 4; continue; }
        gw = fo->d[o] | fo->d[o + 1] << 8;
        for (gy = 0; gy < fh; gy++)
            for (gx = 0; gx < gw; gx++) {
                size_t si = o + 4 + (size_t)gy * (size_t)gw + (size_t)gx;
                int px = x + gx, py = y + gy;
                uint8_t v;
                if (si >= (size_t)fo->len || px < 0 || py < 0 || px >= W || py >= H) continue;
                v = fo->d[si];
                if (v == key) continue;
                if (remap1 >= 0 && v == 1) v = (uint8_t)remap1;
                memcpy(g_fb + ((size_t)py * W + (size_t)px) * 3, pal[v], 3);
            }
        x += gw;
    }
}
static void draw_text(const font *fo, int x, int y, const char *t, const uint8_t (*pal)[3], int remap1)
{
    if (!g_text_hires) { raster_text(fo, x, y, t, pal, remap1); return; }
    if (g_ntext < 256 && fo->d) {
        text_item *it = &g_text[g_ntext++];
        int k;
        it->fo = fo; it->x = x; it->y = y; it->remap1 = remap1; it->pal = pal;
        snprintf(it->t, sizeof it->t, "%s", t);
        it->palsum = 0;
        for (k = 0; k < 256; k++) it->palsum = it->palsum * 31u + (uint32_t)(pal[k][0] | pal[k][1] << 8 | pal[k][2] << 16);
    }
}

/* ---- TrueType text (the port's own settings text, matched to the artwork's painted lettering: Liberation Sans Bold,
 * caps 12 px in 640x480 space, condensed to 85%, (221,221,221)). Queued like the shell fonts and rendered at the
 * display's physical scale; rasterised into the frame when headless. align: 0 left, 1 right (x = right edge). */
typedef struct { int x, y, align; float cap; uint8_t rgb[3]; char t[64]; } tt_item;
static tt_item g_tt[32];
static void tt_text(int x, int y, const char *t, int align)
{
    if (g_ntt < 32) {
        tt_item *it = &g_tt[g_ntt++];
        it->x = x; it->y = y; it->align = align; it->cap = 12.0f;
        it->rgb[0] = it->rgb[1] = it->rgb[2] = 221;
        snprintf(it->t, sizeof it->t, "%s", t);
    }
}
static void tt_raster(const tt_item *it)   /* headless: into the 640x480 frame */
{
    int w, h, by, x, y;
    uint8_t *img = ttext_render(it->t, it->cap, 0.85f, &w, &h, &by);
    int x0, y0;
    if (!img) return;
    x0 = it->align ? it->x - w : it->x;
    y0 = it->y - (by - (int)it->cap);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int px = x0 + x, py = y0 + y, a = img[y * w + x], k;
            uint8_t *q;
            if (!a || px < 0 || py < 0 || px >= W || py >= H) continue;
            q = g_fb + ((size_t)py * W + (size_t)px) * 3;
            for (k = 0; k < 3; k++) q[k] = (uint8_t)((q[k] * (255 - a) + it->rgb[k] * a) / 255);
        }
    free(img);
}
static void tt_draw(SDL_Renderer *rd, const tt_item *it)
{
    float sx, sy, ps;
    int w, h, by, i;
    uint8_t *img;
    uint32_t *rgba;
    SDL_Texture *tex;
    SDL_RenderGetScale(rd, &sx, &sy);
    ps = sy > 0 ? sy : 1.0f;
    {   /* the physical pixels per 640x480 unit: output size over the logical size */
        int ow, oh, lw, lh;
        SDL_GetRendererOutputSize(rd, &ow, &oh);
        SDL_RenderGetLogicalSize(rd, &lw, &lh);
        if (lw > 0 && lh > 0) { float a = (float)ow / (float)lw, b = (float)oh / (float)lh; ps = a < b ? a : b; }
    }
    if (!(img = ttext_render(it->t, it->cap * ps, 0.85f, &w, &h, &by))) return;
    if ((rgba = malloc((size_t)w * (size_t)h * 4))) {
        for (i = 0; i < w * h; i++) rgba[i] = (uint32_t)img[i] << 24 | (uint32_t)it->rgb[2] << 16 | (uint32_t)it->rgb[1] << 8 | it->rgb[0];
        if ((tex = SDL_CreateTexture(rd, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h))) {
            SDL_FRect dr;
            SDL_UpdateTexture(tex, NULL, rgba, w * 4);
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
            dr.w = (float)w / ps; dr.h = (float)h / ps;
            dr.x = it->align ? (float)it->x - dr.w : (float)it->x;
            dr.y = (float)it->y - ((float)by / ps - it->cap);
            SDL_RenderCopyF(rd, tex, NULL, &dr);
            SDL_DestroyTexture(tex);
        }
        free(rgba);
    }
    free(img);
}

/* Scale3x of an RGBA image (pixels compared exactly) */
static void scale3x(const uint32_t *in, int w, int h, uint32_t *out)
{
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
#define P(xx, yy) in[((yy) < 0 ? 0 : (yy) >= h ? h - 1 : (yy)) * w + ((xx) < 0 ? 0 : (xx) >= w ? w - 1 : (xx))]
            uint32_t A = P(x - 1, y - 1), B = P(x, y - 1), C = P(x + 1, y - 1), D = P(x - 1, y), E = P(x, y), F = P(x + 1, y),
                     G = P(x - 1, y + 1), H2 = P(x, y + 1), I = P(x + 1, y + 1), o[9];
#undef P
            if (B != H2 && D != F) {
                o[0] = D == B ? D : E;
                o[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
                o[2] = B == F ? F : E;
                o[3] = (D == B && E != G) || (D == H2 && E != A) ? D : E;
                o[4] = E;
                o[5] = (B == F && E != I) || (H2 == F && E != C) ? F : E;
                o[6] = D == H2 ? D : E;
                o[7] = (D == H2 && E != I) || (H2 == F && E != G) ? H2 : E;
                o[8] = H2 == F ? F : E;
            } else { int k; for (k = 0; k < 9; k++) o[k] = E; }
            {
                int k;
                for (k = 0; k < 9; k++) out[(y * 3 + k / 3) * (w * 3) + x * 3 + k % 3] = o[k];
            }
        }
}
typedef struct { const font *fo; int c, remap1; uint32_t palsum; SDL_Texture *tex; int w, h; } glyph_tex;
static glyph_tex g_gcache[1024];
static int g_ngcache;
static SDL_Texture *glyph_texture(SDL_Renderer *rd, const text_item *it, int c, int *gw_out)
{
    const font *fo = it->fo;
    int fh = fo->d[8], key = fo->d[12], gw, i, gx, gy;
    uint32_t o = fo->d[16 + c * 4] | fo->d[17 + c * 4] << 8 | fo->d[18 + c * 4] << 16;
    uint32_t *src, *dst;
    glyph_tex *g;
    if (o + 4 > (uint32_t)fo->len) { *gw_out = 4; return NULL; }
    gw = fo->d[o] | fo->d[o + 1] << 8;
    *gw_out = gw;
    for (i = 0; i < g_ngcache; i++) {
        g = &g_gcache[i];
        if (g->fo == fo && g->c == c && g->remap1 == it->remap1 && g->palsum == it->palsum) return g->tex;
    }
    if (gw <= 0) return NULL;
    if (g_ngcache == 1024) { for (i = 0; i < g_ngcache; i++) SDL_DestroyTexture(g_gcache[i].tex); g_ngcache = 0; }
    src = calloc((size_t)gw * (size_t)fh, 4); dst = calloc((size_t)gw * (size_t)fh * 9, 4);
    if (!src || !dst) { free(src); free(dst); return NULL; }
    for (gy = 0; gy < fh; gy++)
        for (gx = 0; gx < gw; gx++) {
            size_t si = o + 4 + (size_t)gy * (size_t)gw + (size_t)gx;
            uint8_t v = si < (size_t)fo->len ? fo->d[si] : (uint8_t)key;
            if (v == key) continue;
            if (it->remap1 >= 0 && v == 1) v = (uint8_t)it->remap1;
            src[gy * gw + gx] = 0xff000000u | (uint32_t)it->pal[v][2] << 16 | (uint32_t)it->pal[v][1] << 8 | it->pal[v][0];
        }
    /* the Win9x shell's look (default): the bitmap font at its own size, one texel per font pixel, scaled with the
     * 640 x 480 screen by whole pixels - every glyph pixel the same size, as DirectDraw drew it (a 3x texture squeezed
     * into a 2x screen made uneven strokes). MW2_TEXT_SMOOTH=1: Scale3x and linear filtering */
    int smooth = getenv("MW2_TEXT_SMOOTH") != NULL, k3 = smooth ? 3 : 1;
    if (smooth) scale3x(src, gw, fh, dst);
    else memcpy(dst, src, (size_t)gw * (size_t)fh * 4);
    g = &g_gcache[g_ngcache];
    g->tex = SDL_CreateTexture(rd, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, gw * k3, fh * k3);
    if (g->tex) {
        SDL_UpdateTexture(g->tex, NULL, dst, gw * k3 * 4);
        SDL_SetTextureBlendMode(g->tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g->tex, getenv("MW2_TEXT_SMOOTH") ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        g->fo = fo; g->c = c; g->remap1 = it->remap1; g->palsum = it->palsum; g->w = gw; g->h = fh;
        g_ngcache++;
    }
    free(src); free(dst);
    return g->tex;
}
/* the menu text as a vector font (user request: cleaner than the bitmap font scaled up by whole pixels): each string
 * in Liberation Sans Bold at the display's physical resolution, its cap height and top taken from the bitmap font's
 * 'H', fitted to the bitmap string's width (letter spacing added, or condensed when wider), in the bitmap font's main
 * ink colour. Strings with the fonts' special glyphs (arrows, < 32) keep the bitmap font. MW2_BITMAP_TEXT=1: the
 * original bitmap font throughout */
typedef struct { const font *fo; char t[96]; uint8_t rgb[3]; float ps; SDL_Texture *tex; int w, h; float dx, dy; } vec_entry;
static vec_entry g_vcache[256];
static int g_nvcache;
static void font_caps(const font *fo, int *top, int *cap)
{
    static const font *last; static int lt, lc;
    const char *probe = "HEI0";
    int k;
    if (fo == last) { *top = lt; *cap = lc; return; }
    *top = 0; *cap = fo->d[8];
    for (k = 0; probe[k]; k++) {
        int c = probe[k], fh = fo->d[8], key = fo->d[12], gw, x, y, y0 = -1, y1 = -1;
        uint32_t o = fo->d[16 + c * 4] | fo->d[17 + c * 4] << 8 | fo->d[18 + c * 4] << 16;
        if (o + 4 > (uint32_t)fo->len) continue;
        gw = fo->d[o] | fo->d[o + 1] << 8;
        for (y = 0; y < fh; y++)
            for (x = 0; x < gw; x++) {
                size_t si = o + 4 + (size_t)y * (size_t)gw + (size_t)x;
                if (si < (size_t)fo->len && fo->d[si] != key) { if (y0 < 0) y0 = y; y1 = y; }
            }
        if (y0 >= 0) { *top = y0; *cap = y1 - y0 + 1; break; }
    }
    last = fo; lt = *top; lc = *cap;
}
static int vec_text(SDL_Renderer *rd, const text_item *it)
{
    const font *fo = it->fo;
    float ps = 1.0f, tw, target, hs = 1.0f, track = 0.0f;
    int top, cap, i, n = (int)strlen(it->t), counts[256], best = -1, w, h, by;
    uint8_t rgb[3];
    vec_entry *e = NULL;
    if (!ttext_available() || getenv("MW2_BITMAP_TEXT") || n == 0) return 0;
    for (i = 0; i < n; i++) if ((unsigned char)it->t[i] < 32 || (unsigned char)it->t[i] > 126) return 0;
    {   /* the physical pixels per 640x480 unit */
        int ow, oh, lw, lh;
        SDL_GetRendererOutputSize(rd, &ow, &oh);
        SDL_RenderGetLogicalSize(rd, &lw, &lh);
        if (lw > 0 && lh > 0) { float a = (float)ow / (float)lw, b = (float)oh / (float)lh; ps = a < b ? a : b; }
        if (SDL_RenderGetIntegerScale(rd) && ps >= 1.0f) ps = floorf(ps);
    }
    /* the ink colour: the most used palette index in the string's glyphs */
    memset(counts, 0, sizeof counts);
    for (i = 0; i < n; i++) {
        int c = (unsigned char)it->t[i], fh = fo->d[8], key = fo->d[12], gw, j;
        uint32_t o = fo->d[16 + c * 4] | fo->d[17 + c * 4] << 8 | fo->d[18 + c * 4] << 16;
        if (o + 4 > (uint32_t)fo->len) continue;
        gw = fo->d[o] | fo->d[o + 1] << 8;
        for (j = 0; j < gw * fh; j++) {
            size_t si = o + 4 + (size_t)j;
            int v;
            if (si >= (size_t)fo->len || fo->d[si] == key) continue;
            v = fo->d[si];
            if (it->remap1 >= 0 && v == 1) v = it->remap1;
            counts[v]++;
        }
    }
    for (i = 0; i < 256; i++) if (counts[i] && (best < 0 || counts[i] > counts[best])) best = i;
    if (best < 0) return 1;   /* spaces only */
    rgb[0] = it->pal[best][0]; rgb[1] = it->pal[best][1]; rgb[2] = it->pal[best][2];
    for (i = 0; i < g_nvcache; i++) {
        vec_entry *c = &g_vcache[i];
        if (c->fo == fo && c->ps == ps && !memcmp(c->rgb, rgb, 3) && !strcmp(c->t, it->t)) { e = c; break; }
    }
    if (!e) {
        uint8_t *img;
        uint32_t *rgba;
        font_caps(fo, &top, &cap);
        tw = ttext_width(it->t, (float)cap * ps, 1.0f);
        target = (float)text_width(fo, it->t) * ps;
        if (tw > target) { hs = target / tw; if (hs < 0.72f) hs = 0.72f; }
        else if (n > 1) track = (target - tw) / (float)n;
        if (track > (float)cap * ps * 0.6f) track = (float)cap * ps * 0.6f;
        if (!(img = ttext_render_track(it->t, (float)cap * ps, hs, track, &w, &h, &by))) return 0;
        if (g_nvcache == 256) { for (i = 0; i < g_nvcache; i++) SDL_DestroyTexture(g_vcache[i].tex); g_nvcache = 0; }
        e = &g_vcache[g_nvcache];
        e->tex = NULL;
        if ((rgba = malloc((size_t)w * (size_t)h * 4))) {
            for (i = 0; i < w * h; i++) rgba[i] = (uint32_t)img[i] << 24 | (uint32_t)rgb[2] << 16 | (uint32_t)rgb[1] << 8 | rgb[0];
            if ((e->tex = SDL_CreateTexture(rd, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h))) {
                SDL_UpdateTexture(e->tex, NULL, rgba, w * 4);
                SDL_SetTextureBlendMode(e->tex, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(e->tex, SDL_ScaleModeLinear);
            }
            free(rgba);
        }
        free(img);
        if (!e->tex) return 0;
        e->fo = fo; snprintf(e->t, sizeof e->t, "%s", it->t); memcpy(e->rgb, rgb, 3); e->ps = ps; e->w = w; e->h = h;
        e->dx = -1.0f / ps;                                         /* the renderer's 1-pixel left margin */
        e->dy = (float)top - ((float)by - (float)cap * ps) / ps;    /* its cap top on the bitmap font's */
        g_nvcache++;
    }
    {
        SDL_FRect dr = {(float)it->x + e->dx, (float)it->y + e->dy, (float)e->w / ps, (float)e->h / ps};
        SDL_RenderCopyF(rd, e->tex, NULL, &dr);
    }
    return 1;
}
static void text_layer_draw(SDL_Renderer *rd)
{
    int i;
    static int grab_frames = -1;
    if (grab_frames < 0) grab_frames = getenv("MW2_SHELL_GRAB_FRAMES") ? atoi(getenv("MW2_SHELL_GRAB_FRAMES")) : 0;
    for (i = 0; i < g_ntext; i++) {
        const text_item *it = &g_text[i];
        const char *t;
        float x = (float)it->x;
        int fh = it->fo->d[8];
        if (vec_text(rd, it)) continue;
        for (t = it->t; *t; t++) {
            int gw = 4;
            SDL_Texture *tex = glyph_texture(rd, it, (unsigned char)*t & 127, &gw);
            if (tex) { SDL_FRect dr = {x, (float)it->y, (float)gw, (float)fh}; SDL_RenderCopyF(rd, tex, NULL, &dr); }
            x += (float)gw;
        }
    }
    for (i = 0; i < g_ntt; i++) tt_draw(rd, &g_tt[i]);
    if (grab_frames > 0 && --grab_frames == 0 && getenv("MW2_SHELL_GRAB")) {   /* tests: the rendered window, full resolution */
        int ow, oh;
        uint8_t *buf;
        SDL_GetRendererOutputSize(rd, &ow, &oh);
        if ((buf = malloc((size_t)ow * (size_t)oh * 3))) {
            if (SDL_RenderReadPixels(rd, NULL, SDL_PIXELFORMAT_RGB24, buf, ow * 3) == 0) {
                FILE *o = fopen(getenv("MW2_SHELL_GRAB"), "wb");
                if (o) { fprintf(o, "P6\n%d %d\n255\n", ow, oh); fwrite(buf, 1, (size_t)ow * (size_t)oh * 3, o); fclose(o); }
            }
            free(buf);
        }
        exit(0);
    }
}

/* ---- images ---- */
static shell_image g_bg; static int g_bg_entry = -999;
static void load_bg(int entry)
{
    uint8_t *e;
    long n;
    if (entry == g_bg_entry) return;
    shell_image_free(&g_bg);
    g_bg_entry = entry;
    if (entry < 0) return;
    n = shelldb_entry(&g_db, entry, &e);
    if (n > 0) shell_pcx(e, (size_t)n, &g_bg, NULL);
    free(e);
}
static void draw_bg(void)
{
    int i;
    text_layer_clear();
    if (!g_bg.pix) { memset(g_fb, 0, sizeof g_fb); return; }
    for (i = 0; i < W * H && i < g_bg.w * g_bg.h; i++) memcpy(g_fb + (size_t)i * 3, g_bg.pal[g_bg.pix[i]], 3);
}
static smk *open_smk(const char *name)
{
    char p[700];
    {
        char dn[64];
        snprintf(dn, sizeof dn, "SMK\\%s.SMK", name);
        snprintf(p, sizeof p, "%s", gpath(dn));
    }
    return smk_open_file(p);
}
static void blit_smk(smk *s, int x, int y)
{
    int w = smk_width(s), h = smk_height(s), i, j;
    const uint8_t *px = smk_pixels(s);
    const uint8_t (*pal)[3] = smk_palette(s);
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            int X = x + i, Y = y + j;
            if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
            memcpy(g_fb + ((size_t)Y * W + (size_t)X) * 3, pal[px[(size_t)j * (size_t)w + (size_t)i]], 3);
        }
}

/* ---- a screen "clip" (FUN_000399b0): a Smacker video, or a still SHP (the Instant Action context's buttons,
 * WIABKG1 / WIASTAR / WIACN ..., flags 4 / 0x24), drawn at its hotspot */
typedef struct { smk *s; uint8_t *shp; size_t len; shp_frame fr; int have_fr; } clip;
/* the palette stills are drawn in: the screen's 8-bit palette, i.e. the background's as overwritten by the playing
 * Smacker clip (the grid sets the entries the IA buttons' fill uses) - NULL = the background's */
static const uint8_t (*g_clip_pal)[3];
static clip *clip_open(const char *name)
{
    clip *c = calloc(1, sizeof *c);
    char dn[64];
    FILE *fp;
    if (!c || !name) { free(c); return NULL; }
    if ((c->s = open_smk(name))) return c;
    snprintf(dn, sizeof dn, "SMK\\%s.SHP", name);
    if ((fp = fopen(gpath(dn), "rb"))) {
        fseek(fp, 0, SEEK_END); c->len = (size_t)ftell(fp); fseek(fp, 0, SEEK_SET);
        if ((c->shp = malloc(c->len)) && fread(c->shp, 1, c->len, fp) == c->len && shp_decode(c->shp, c->len, 0, &c->fr) == 0) c->have_fr = 1;
        fclose(fp);
    }
    if (!c->have_fr) { free(c->shp); free(c); return NULL; }
    return c;
}
static void clip_next(clip *c) { if (c && c->s) smk_next(c->s); }
static void clip_blit(clip *c, int x, int y)
{
    int px, py, X, Y;
    if (!c) return;
    if (c->s) { blit_smk(c->s, x, y); return; }
    for (Y = 0; Y < c->fr.h; Y++)
        for (X = 0; X < c->fr.w; X++) {
            px = x + c->fr.left + X; py = y + c->fr.top + Y;
            if (!c->fr.mask[Y * c->fr.w + X] || px < 0 || py < 0 || px >= W || py >= H) continue;
            memcpy(g_fb + ((size_t)py * W + (size_t)px) * 3, (g_clip_pal ? g_clip_pal : (const uint8_t (*)[3])g_bg.pal)[c->fr.pix[Y * c->fr.w + X]], 3);
        }
}
static void clip_close(clip *c)
{
    if (!c) return;
    if (c->s) smk_close(c->s);
    if (c->have_fr) shp_frame_free(&c->fr);
    free(c->shp);
    free(c);
}
/* screen contexts: clan 0 Wolf, 1 Jade Falcon, 2 / 3 Instant Action friendly / enemy star (context 2 visuals) */
#define CTX(c) ((c) >= 2 ? 2 : (c))

/* ---- ambient WAV from DATABASE (RIFF, 8-bit mono) ---- */
static uint8_t *g_amb; static long g_amblen;
static void play_ambient(int entry)
{
    long n, i;
    sfx_stop_loops(g_sfx);
    free(g_amb); g_amb = NULL;
    if (entry < 0 || !g_sfx) return;
    n = shelldb_entry(&g_db, entry, &g_amb);
    g_amblen = n;
    for (i = 12; i + 8 <= n; ) {   /* find fmt / data chunks */
        uint32_t cl = g_amb[i + 4] | g_amb[i + 5] << 8 | g_amb[i + 6] << 16 | (uint32_t)g_amb[i + 7] << 24;
        if (!memcmp(g_amb + i, "data", 4)) {
            int rate = g_amb[24] | g_amb[25] << 8;
            sfx_play_pcm(g_sfx, g_amb + i + 8, (int)(cl < (uint32_t)(n - i - 8) ? cl : (uint32_t)(n - i - 8)), rate, 0.5f, 1);
            return;
        }
        i += 8 + (long)cl + (cl & 1);
    }
}

/* ---- the shell's sound objects (MW2SHELL FUN_0003a340 make, 0x3a6e0 volume 0-127, 0x3a560 play, 0x3a400 stop): DATABASE
 * WAVs by the getter's 1-based index (entry = index - 1). Each object is one sample handle: playing it again restarts it.
 * Buttons (made at start-up, 0x36c1e): 0x65 and 0x66 at volume 0x32; the Mech Lab's chassis voices (the chassis table
 * 0x8367c +0x10, e.g. 0x55 "Firemoth") at 0x28; CUSTOMIZE's location pick 0x50 at 0x28 (0x2fb20 / 0x2fb50). */
typedef struct { uint8_t *wav; const uint8_t *pcm; int len, rate; } shell_snd;
static shell_snd g_snd[160];
static void shell_sound(int index, int vol)
{
    shell_snd *z;
    long n, i;
    if (index <= 0 || index > 160 || !g_sfx) return;
    z = &g_snd[index - 1];
    if (!z->wav) {
        n = shelldb_entry(&g_db, index - 1, &z->wav);
        for (i = 12; z->wav && i + 8 <= n; ) {
            uint32_t cl = z->wav[i + 4] | z->wav[i + 5] << 8 | z->wav[i + 6] << 16 | (uint32_t)z->wav[i + 7] << 24;
            if (!memcmp(z->wav + i, "data", 4)) {
                z->rate = z->wav[24] | z->wav[25] << 8;
                z->pcm = z->wav + i + 8;
                z->len = (int)(cl < (uint32_t)(n - i - 8) ? cl : (uint32_t)(n - i - 8));
                break;
            }
            i += 8 + (long)cl + (cl & 1);
        }
    }
    if (!z->pcm || z->len <= 0) return;
    sfx_stop_pcm(g_sfx, z->pcm);
    sfx_play_pcm(g_sfx, z->pcm, z->len, z->rate, (float)vol / 127.0f, 0);
}
static void shell_sound_stop(int index) { if (index > 0 && index <= 160 && g_sfx && g_snd[index - 1].pcm) sfx_stop_pcm(g_sfx, g_snd[index - 1].pcm); }
/* the Mech Lab's chassis voices, in the lab's chassis order (0x8367c + 0x10; -1 none) */
static const int LAB_VOICE[18] = {0x55, 0x59, 0x58, 0x5c, 0x5e, 0x5a, 0x57, 0x5d, 0x5f, 0x60, 0x56, 0x61, 0x5b, 0x62, 0x53, 0x54, -1, 0x63};

/* ---- full-screen cinematic (320x200 scaled to 640x480) ---- */
static void play_cinematic(SDL_Renderer *rd, SDL_Texture *tx, const char *name, int headless)
{
    smk *s = open_smk(name);
    int f, n, last = -1;
    unsigned char *snd = NULL;
    Uint32 t0;
    if (!s) return;
    n = headless ? 3 : smk_frames(s);
    {   /* the clip's sound (audio track 0), mixed as one 8-bit voice; the frames then follow the clock */
        int16_t *a = NULL;
        size_t ns = 0;
        int rate = 0, ch = 1;
        if (!headless && smk_audio(s, &a, &ns, &rate, &ch) == 0 && ns > 0 && (snd = malloc(ns / (size_t)ch + 1))) {
            size_t i;
            for (i = 0; i < ns / (size_t)ch; i++) {
                int v = ch == 2 ? ((int)a[2 * i] + (int)a[2 * i + 1]) / 2 : a[i];
                snd[i] = (unsigned char)((v >> 8) + 128);
            }
            sfx_play_pcm(g_sfx, snd, (int)(ns / (size_t)ch), rate, 1.0f, 0);
        }
        free(a);
        if (getenv("MW2_SMK_TRACE")) fprintf(stderr, "cinematic %s: %d frames, audio %zu samples at %d Hz, %d channel(s)\n", name, smk_frames(s), ns, rate, ch);
    }
    t0 = SDL_GetTicks();
    for (f = 0; f < n; f++) {
        int x, y, sw = smk_width(s), sh = smk_height(s);
        SDL_Event e;
        int skip = 0;
        smk_next(s);
        if (!headless) {   /* keep pace with the sound: drop frames when behind, wait when ahead */
            double due = (double)(f + 1) * smk_frame_ms(s);
            if ((double)(SDL_GetTicks() - t0) > due + 2.0 * smk_frame_ms(s) && f + 1 < n) continue;
        }
        last = f;
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++) {
                int sx = x * sw / W, sy = y * sh / H;
                memcpy(g_fb + ((size_t)y * W + (size_t)x) * 3, smk_palette(s)[smk_pixels(s)[(size_t)sy * (size_t)sw + (size_t)sx]], 3);
            }
        if (!headless) {
            Uint32 now;
            SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
            SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
            while (SDL_PollEvent(&e)) if (e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_QUIT) skip = 1;
            if (skip) break;
            now = SDL_GetTicks() - t0;
            if ((double)now < (double)(f + 1) * smk_frame_ms(s)) SDL_Delay((Uint32)((double)(f + 1) * smk_frame_ms(s) - (double)now));
        }
    }
    (void)last;
    if (snd) { sfx_stop_pcm(g_sfx, snd); free(snd); }
    smk_close(s);
}

static void apply_music(int screen, int clan)
{
    uint32_t v;
    int entry;
    long n;
    if ((!g_bank && !g_mt32) || !g_sfx || screen < 0 || screen > 16 || clan < 0 || clan > 2) return;
    v = MUSIC[clan][screen];
    if (!v || (v & 0x20000000)) return;
    /* the song set by device (driver table 0x83e14): MT32MPU.MDI +24, ULTRA.MDI +32; DATABASE index = number + that */
    entry = (int)(v & 0xffffff) + (g_mt32 ? 24 : GUS_MUSIC_OFFSET) - 1;
    sfx_lock(g_sfx);
    if (v & 0x10000000) { gus_song_close(g_song); g_song = NULL; mt32_song_close(g_msong); g_msong = NULL; g_song_entry = -1; }
    if (entry != g_song_entry || (!g_song && !g_msong)) {
        gus_song_close(g_song); g_song = NULL;
        mt32_song_close(g_msong); g_msong = NULL;
        free(g_song_data); g_song_data = NULL;
        n = shelldb_entry(&g_db, entry, &g_song_data);
        if (n > 0) {
            if (g_mt32) g_msong = mt32_song_open(g_mt32, g_song_data, (size_t)n);
            else g_song = gus_song_open(g_bank, g_song_data, (size_t)n, sfx_rate(g_sfx));
        }
        g_song_entry = entry;
        g_song_len = n; g_song_rate = sfx_rate(g_sfx);
    }
    sfx_unlock(g_sfx);
}


/* ================= Instant Action: "Trials of Grievance" (screen 7, FUN_00029680) =================
 * Hotspots 0x7e8a4 (25), scenarios 0x7f2fc, planet videos 0x7f2a0, clan emblem videos 0x7f328,
 * mech list 0x8367c (code, file, name, tons), formations 0x7d470. Lists use font entry 25 with
 * colour 1 -> palette 0x22. */
typedef struct { int x0, y0, x1, y1, lx, ly; } rect7;
static const rect7 IA_HS[25] = {
    {209, 371, 420, 452, 0, 0}, {50, 445, 149, 469, 100, 450}, {238, 67, 400, 102, 239, 69},
    {13, 124, 103, 137, 29, 126}, {13, 138, 103, 151, 29, 140}, {13, 152, 103, 165, 29, 154}, {13, 181, 103, 194, 29, 183},
    {4, 124, 12, 137, 29, 126}, {4, 138, 12, 151, 29, 140}, {4, 152, 12, 165, 29, 154}, {4, 181, 12, 194, 29, 183},
    {13, 205, 160, 352, 0, 0}, {134, 122, 172, 166, 0, 0}, {134, 168, 172, 200, 0, 0},
    {481, 249, 572, 262, 498, 251}, {481, 263, 572, 276, 498, 265}, {481, 277, 572, 290, 498, 279}, {481, 306, 572, 319, 498, 308},
    {472, 249, 480, 262, 498, 251}, {472, 263, 480, 276, 498, 265}, {472, 277, 480, 290, 498, 279}, {472, 306, 480, 319, 498, 308},
    {482, 330, 629, 477, 0, 0}, {583, 247, 621, 291, 0, 0}, {583, 293, 621, 325, 0, 0},
};
static const char *const IA_SCEN[10] = {"jackscn1", "chedscn1", "edamscn1", "provscn1", "goudscn1", "colbscn1", "goatscn1", "whizscn1", "ricoscn1", "swisscn1"};
static const char *const IA_CLAN[6] = {"WIAWOLF", "WIAJF", "WIAGHOST", "WIASMOKE", "WIANOVA", "WIASTEEL"};
static const char *const IA_FORM[6] = {"Echelon Left", "Echelon Right", "Line Abreast", "Line Astern", "V-Form", "Wedge"};
typedef struct { const char *code, *file, *name; int tons; } mech_entry;
static const mech_entry MECHS[18] = {
    {"frm", "firemoth", "Firemoth", 20}, {"ktf", "kitfox", "Kit Fox", 30}, {"jnr", "jenner", "Jenner IIC", 35},
    {"nva", "nova", "Nova", 50}, {"stm", "strmcrow", "Stormcrow", 55}, {"mdg", "maddog", "Mad Dog", 60},
    {"hlb", "hellbrgr", "Hellbringer", 65}, {"rfl", "rifleman", "Rifleman IIC", 65}, {"smn", "summoner", "Summoner", 70},
    {"tbr", "timbrwlf", "Timber Wolf", 75}, {"grg", "gargoyle", "Gargoyle", 80}, {"whm", "warhammr", "Warhammer IIC", 80},
    {"mrd", "marauder", "Marauder IIC", 85}, {"whk", "warhawk", "Warhawk", 85}, {"drw", "direwolf", "Dire Wolf", 100},
    {"ele", "elementl", "Elemental", 100}, {"tar", "tarantul", "Tarantula", 100}, {"btm", "btllmstr", "Battle Master IIC", 100},
};
typedef struct { int scen, mech[2][3], form[2], clan[2], nmech; char loadout[2][3][9]; int tons[2], size[2], planet, loaded;
                 char text[3][64]; } ia_state;
static ia_state g_ia = {0, {{9, -1, -1}, {5, -1, -1}}, {0, 0}, {0, 1}, 15, {{""}}, {100, 100}, {3, 3}, 0, 0, {""}};

typedef struct { smk *s; int x, y; Uint32 last; int hold, once; char then[16]; } overlay;
static int g_from_screen = -1;   /* the screen before the current one */
static int g_ia_ctx = 2;   /* the star the IA screen's MECH LAB / STAR CONFIG buttons opened: 2 friendly, 3 enemy */
static int g_lab_slot;
static void ov_set(overlay *o, const char *name, int x, int y)
{
    if (o->s) smk_close(o->s);
    o->s = name ? open_smk(name) : NULL; o->x = x; o->y = y; o->last = 0; o->hold = 0; o->once = 0; o->then[0] = 0;
    if (o->s) smk_next(o->s);
}
static void ov_draw(overlay *o, int headless)
{
    Uint32 now;
    if (!o->s) return;
    now = SDL_GetTicks();
    if (!o->hold && (headless || now - o->last >= (Uint32)smk_frame_ms(o->s))) {
        int fr = smk_next(o->s);
        o->last = now;
        if (o->once && fr == 0 && o->then[0]) {   /* the clip ended (it looped back): its follow-on clip (FUN_00039590) */
            char nm[16];
            int x = o->x, y = o->y;
            snprintf(nm, sizeof nm, "%s", o->then);
            ov_set(o, nm, x, y);
            if (!o->s) return;
        }
    }
    blit_smk(o->s, o->x, o->y);
}

/* ---- the scenario's record <first 4 letters>BRF2 (FUN_000291b0): SDSC x 2 = the friendly then the enemy lance
 * ({+0 ?, +4 tonnage limit, +8 ?, +12 star size, loadouts at +16 x 16}; FUN_0003ab50 / FUN_0003a8a0 fill the star
 * records 0x840dc / 0x8415c), PDSC = {planet 1-12, three text lines} drawn at (239, 69) every 12 lines; the planet
 * clip aplanNN (0x7f29c) plays once, then aplanNNc loops (0x7f2a0, when slot 0 has finished). SUPS is not used here. */
#define IA_FONT g_fthin
#define IA_INK 0x22
enum { IA_TEXT_X = 239, IA_TEXT_Y = 69 };
static int mech_index_of(const char *loadout);
static int mission_record(const char *scen, const char *suffix, prj_record *r);
static void ia_load_scenario(void)
{
    char rec[8];
    prj_record r;
    size_t o = 12;
    int side = 0, k;
    snprintf(rec, sizeof rec, "%.4s", IA_SCEN[g_ia.scen]);
    memset(g_ia.text, 0, sizeof g_ia.text);
    g_ia.planet = g_ia.scen;
    if (mission_record(rec, "BRF2", &r) != 0) return;
    while (o + 8 <= r.size) {
        uint32_t sz = r.data[o + 4] | r.data[o + 5] << 8 | r.data[o + 6] << 16 | (uint32_t)r.data[o + 7] << 24;
        const uint8_t *p = r.data + o + 8;
        uint32_t len = sz >= 8 ? sz - 8 : 0;
        if (sz < 8) break;
        if (memcmp(r.data + o, "SDSC", 4) == 0 && len >= 16 && side < 2) {
            int n = p[12] | p[13] << 8;                      /* FUN_0003ab50: capped at 3 */
            g_ia.tons[side] = p[4] | p[5] << 8;
            g_ia.size[side] = n < 1 ? 1 : n > 3 ? 3 : n;
            for (k = 0; k < 3; k++) {
                g_ia.mech[side][k] = -1; g_ia.loadout[side][k][0] = 0;
                if (k < n && 16 + (k + 1) * 16 <= (int)len) {
                    snprintf(g_ia.loadout[side][k], 9, "%.8s", (const char *)p + 16 + k * 16);
                    g_ia.mech[side][k] = mech_index_of(g_ia.loadout[side][k]);
                }
            }
            side++;
        } else if (memcmp(r.data + o, "PDSC", 4) == 0 && len >= 4) {
            int pl = (p[0] | p[1] << 8) - 1, line = 0;
            size_t q = 4, c = 0;
            g_ia.planet = pl < 0 || pl > 11 ? 0 : pl;          /* 0..11 */
            for (; q < len && line < 3; q++) {
                if (p[q] == '\n' || p[q] == 0 || c >= 63) { g_ia.text[line][c] = 0; line++; c = 0; if (!p[q]) break; continue; }
                if (p[q] >= 32) g_ia.text[line][c++] = (char)p[q];
            }
        }
        o += sz;
    }
    prj_record_free(&r);
    g_ia.loaded = 1;
}
static void ia_planet(overlay *o)
{
    char a[16], c[16];
    snprintf(a, sizeof a, "APLAN%02d", (g_ia.planet + 1) % 13);
    snprintf(c, sizeof c, "APLAN%02dC", (g_ia.planet + 1) % 13);
    ov_set(o, a, 414, 10);
    o->once = 1; snprintf(o->then, sizeof o->then, "%s", c);
    if (!o->s) { ov_set(o, c, 414, 10); o->once = 0; }
}


/* ---- Launch (screen 10, FUN_000375a0): write the lance / camo files, run the simulation, come back.
 * MW2_SIM_PRJ: archive the simulation uses (record ids differ between editions); MW2_SAVE_DIR: where
 * the files go (default: the install dir, as the original); MW2_SIM: command with %s = scenario;
 * MW2_PILOT: the player's name. Formations go to the simulation as MW2_OF / MW2_OE (-of= / -oe=). */
static const char *const FORM_ARG[6] = {"echelonl", "echelonr", "lineabreast", "lineastern", "vform", "wedge"};
static SDL_Window *g_win;
static char g_result[64];   /* the last mission's result, from MW2MSN.CFG */
static void read_results(const char *dir)
{
    char path[1100];
    unsigned char b[20];
    FILE *f;
    int st, n;
    g_result[0] = 0;
    snprintf(path, sizeof path, "%s/MW2MSN.CFG", dir);
    f = fopen(path, "rb");
    if (!f) return;
    if (fread(b, 1, sizeof b, f) == sizeof b) {
        st = b[16] | b[17] << 8; n = b[4] | b[5] << 8;
        snprintf(g_result, sizeof g_result, "%s (%d OBJECTIVES)", st == 2 ? "MISSION SUCCESSFUL" : st == 3 ? "MISSION FAILED" :
                 st == 4 ? "MISSION TIME EXCEEDED" : "MISSION OVER", n);
    }
    fclose(f);
}

/* the simulation's command line: MW2_SIM if set (tests), else the glview beside this executable with the configured
 * archives; and the child's paths (the configuration, passed as its environment) */
/* the chosen edition's files (Combat Variables RENDERER; mw2port.cfg edition=), falling back to the enhanced mix when
 * that edition's files are missing */
static struct { char models[PORTCFG_PATH], textures[PORTCFG_PATH], kind[16], sky[PORTCFG_PATH], fog[PORTCFG_PATH]; int ed; } g_edf;
static void edition_files(void)
{
    char kind[PORTCFG_PATH];
    g_edf.ed = g_cfg.edition;
    if (portcfg_edition(&g_cfg, g_cfg.edition, g_edf.models, g_edf.textures, kind, g_edf.sky, g_edf.fog) != 0) {
        g_edf.ed = PORTCFG_ED_ENHANCED;
        portcfg_edition(&g_cfg, PORTCFG_ED_ENHANCED, g_edf.models, g_edf.textures, kind, g_edf.sky, g_edf.fog);
    }
    snprintf(g_edf.kind, sizeof g_edf.kind, "%.15s", kind);
}
/* the archive the simulation will use (record ids differ between editions): MW2_SIM_PRJ (tests), else the edition's */
static const char *sim_archive(void)
{
    const char *e = getenv("MW2_SIM_PRJ");
    if (e) return e;
    edition_files();
    return g_edf.models[0] ? g_edf.models : gpath("MW2.PRJ");
}

static void setenv_or_unset(const char *k, const char *v) { if (v && v[0]) setenv(k, v, 1); else unsetenv(k); }

/* the simulation's exit: 42 = the in-game MAIN MENU's Flee to Windows (MW2.DLL 0x100642e0) - the game ends there */
static void sim_returned(int rc)
{
#ifdef _WIN32
    int code = rc;
#else
    int code = rc != -1 && WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
    if (code == 42) { printf("launch: fled to Windows\n"); SDL_Quit(); exit(0); }
}

static void sim_command(char *line, size_t n, const char *scen, const char *save)
{
    const char *cmd = getenv("MW2_SIM");
    setenv("MW2_INSTALL_DIR", save, 1);   /* (the only place the child gets its paths) */
    if (g_cfg.cd[0]) {
        size_t k = strlen(g_cfg.cd);
        if (k > 4 && strcasecmp(g_cfg.cd + k - 4, ".iso") == 0) setenv("MW2_CD_IMAGE", g_cfg.cd, 1); else setenv("MW2_CD_DIR", g_cfg.cd, 1);
    }
    edition_files();
    /* the edition decides the sky file and the fog (only the PowerVR edition, and the enhanced mix, have fog) */
    setenv_or_unset("MW2_SKYGND", g_edf.sky);
    setenv_or_unset("MW2_SKYGND_FOG", g_edf.fog);
    setenv("MW2_EDITION", PORTCFG_ED_NAME[g_edf.ed], 1);
    if (g_cfg.ultrasnd[0] && !getenv("MW2_ULTRASND")) setenv("MW2_ULTRASND", g_cfg.ultrasnd, 1);
    if (g_cfg.music[0] && !getenv("MW2_MUSIC_DIR")) setenv("MW2_MUSIC_DIR", g_cfg.music, 1);
    if (cmd) snprintf(line, n, cmd, scen);
    else {
#ifdef MW2_ONE_BINARY   /* one executable: the shell runs itself again as the simulation ("mw2 --sim ...") */
        snprintf(line, n, "\"%s\" --sim \"%s\" \"%s\" %s @ %s", g_exe_path, g_edf.models, g_edf.textures, g_edf.kind, scen);
#else
        snprintf(line, n, "\"%sglview\" \"%s\" \"%s\" %s @ %s", g_exe_dir, g_edf.models, g_edf.textures, g_edf.kind, scen);
#endif
    }
}

static SDL_Renderer *g_rd;   /* the shell's window, for the loading screen */
static int g_load_clan;      /* the clan whose dropship the loading screen shows */
static SDL_Texture  *g_tx;
/* the drop screen (DOSBox capture, the simulation's loading screen): the mission's launch picture - its brief record's
 * SUPS chunk names it (BRF2: "lwocolm", "liagoat" ...), the disc's LAUNCH\<name>6.SHP, a 640 x 480 shape that carries its
 * own palette (header +12: offset of {u32 count, count x {index, r, g, b} 6-bit}) - with LAUNCH6.SHP ("DROP PROCEDURE
 * INITIATED") over it at the top right; held 2 s (user choice) (the port loads at once; period PCs took seconds) */
static char g_load_scene[16];   /* the mission about to launch, e.g. "YELLSCN1" */
static int load_shp_pal(const char *name, shp_frame *fr, uint8_t pal[256][3])
{
    char dn[64];
    const char *p;
    FILE *f;
    long n;
    uint8_t *d;
    snprintf(dn, sizeof dn, "LAUNCH\\%s.SHP", name);
    p = gpath(dn);
    if (!(f = fopen(p, "rb"))) return -1;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 32 || !(d = malloc((size_t)n))) { fclose(f); return -1; }
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return -1; }
    fclose(f);
    if (shp_decode(d, (size_t)n, 0, fr) != 0) { free(d); return -1; }
    if (pal) {
        uint32_t po = (uint32_t)d[12] | (uint32_t)d[13] << 8 | (uint32_t)d[14] << 16 | (uint32_t)d[15] << 24, cnt, i2;
        memset(pal, 0, 768);
        if (po + 4 <= (uint32_t)n) {
            cnt = (uint32_t)d[po] | (uint32_t)d[po + 1] << 8;
            for (i2 = 0; i2 < cnt && po + 8 + i2 * 4 <= (uint32_t)n; i2++) {
                const uint8_t *e = d + po + 4 + i2 * 4;
                pal[e[0]][0] = (uint8_t)(e[1] << 2); pal[e[0]][1] = (uint8_t)(e[2] << 2); pal[e[0]][2] = (uint8_t)(e[3] << 2);
            }
        }
    }
    free(d);
    return 0;
}
static void loading_screen(int clan)
{
    char brf[16], sups[16] = "";
    shp_frame pic, txt;
    uint8_t pal[256][3], tpal[256][3];
    int x, y, have_txt;
    Uint32 t0;
    (void)clan;
    const char *shot = getenv("MW2_LOADING_SHOT");   /* tests: compose, write a PPM, no wait */
    if (getenv("MW2_LOADING_SCENE")) snprintf(g_load_scene, sizeof g_load_scene, "%s", getenv("MW2_LOADING_SCENE"));
    if (getenv("MW2_NO_LOADING") || !g_load_scene[0]) return;
    if (!shot) return;   /* the simulation shows the drop screen in its own window (glview drop_screen_*); this is the test path */
    if (!shot && SDL_GetCurrentVideoDriver() && (strcmp(SDL_GetCurrentVideoDriver(), "offscreen") == 0 || strcmp(SDL_GetCurrentVideoDriver(), "dummy") == 0)) return;   /* tests */
    {   /* the brief record's SUPS: the launch picture's name */
        char err[256];
        prj_archive *a = prj_open(g_cfg.models[0] ? g_cfg.models : gpath("MW2.PRJ"), err, sizeof err);
        prj_record r;
        snprintf(brf, sizeof brf, "%.4sBRF2", g_load_scene);
        if (a && prj_read_named(a, "BWD", brf, &r) == PRJ_OK) {
            bwd_chunk ch[96];
            int nc = bwd_chunks(r.data, r.size, ch, 96), q;
            for (q = 0; q < nc; q++) if (strcmp(ch[q].tag, "SUPS") == 0 && ch[q].size > 0) { snprintf(sups, sizeof sups, "%.*s", (int)(ch[q].size < 15 ? ch[q].size : 15), (const char *)ch[q].data); break; }
            prj_record_free(&r);
        }
        if (a) prj_close(a);
    }
    if (!sups[0]) return;
    {
        char nm[24];
        snprintf(nm, sizeof nm, "%s6", sups);
        for (x = 0; nm[x]; x++) if (nm[x] >= 'a' && nm[x] <= 'z') nm[x] = (char)(nm[x] - 32);
        if (load_shp_pal(nm, &pic, pal) != 0) return;
    }
    have_txt = load_shp_pal("LAUNCH6", &txt, tpal) == 0;
    memset(g_fb, 0, sizeof g_fb);
    for (y = 0; y < pic.h && y < H; y++)
        for (x = 0; x < pic.w && x < W; x++)
            if (pic.mask[y * pic.w + x]) memcpy(g_fb + ((size_t)y * W + (size_t)x) * 3, pal[pic.pix[y * pic.w + x]], 3);
    if (have_txt) {   /* DOSBox: the text's top left at (368, 20) of the picture */
        for (y = 0; y < txt.h; y++)
            for (x = 0; x < txt.w; x++) {
                int px = 368 + x, py = 20 + y;
                if (!txt.mask[y * txt.w + x] || px >= W || py >= H) continue;
                {   /* DOSBox: light grey letters with a darker edge - the shape's own palette as a grey ramp */
                    const uint8_t *c3 = tpal[txt.pix[y * txt.w + x]];
                    int gv = (int)c3[0] > (int)c3[1] ? c3[0] : c3[1];
                    uint8_t *o3 = g_fb + ((size_t)py * W + (size_t)px) * 3;
                    gv = gv > (int)c3[2] ? gv : c3[2];
                    gv = gv * 230 / 160; if (gv > 235) gv = 235;
                    o3[0] = o3[1] = o3[2] = (uint8_t)gv;
                }
            }
        shp_frame_free(&txt);
    }
    shp_frame_free(&pic);
    if (shot) { FILE *o = fopen(shot, "wb"); if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H); fwrite(g_fb, 1, sizeof g_fb, o); fclose(o); } return; }
    text_layer_clear();
    t0 = SDL_GetTicks();
    while (SDL_GetTicks() - t0 < 2000) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) return;
        SDL_UpdateTexture(g_tx, NULL, g_fb, W * 3);
        SDL_RenderClear(g_rd); SDL_RenderCopy(g_rd, g_tx, NULL, NULL); SDL_RenderPresent(g_rd);
        SDL_Delay(30);
    }
}
static int launch_ia(void)
{
    const char *simprj = NULL, *save = getenv("MW2_SAVE_DIR");
    const char *pilot = getenv("MW2_PILOT") ? getenv("MW2_PILOT") : "PLAYER";
    char path[1024], err[256], line[4200], scen[16];
    prj_archive *a;
    lance_slot fr[3], en[3];
    int k, rc;
    if (!save) save = g_dir;
    if (!simprj) simprj = sim_archive();
    a = prj_open(simprj, err, sizeof err);
    if (!a) { fprintf(stderr, "launch: cannot open %s: %s\n", simprj, err); return -1; }
    memset(fr, 0, sizeof fr); memset(en, 0, sizeof en);
    for (k = 0; k < 3; k++) {
        fr[k].mech = g_ia.mech[0][k]; en[k].mech = g_ia.mech[1][k];
        /* the scenario's loadout while the slot still holds that chassis, else the standard one */
        if (fr[k].mech >= 0 && g_ia.loadout[0][k][0] && mech_index_of(g_ia.loadout[0][k]) == fr[k].mech) snprintf(fr[k].loadout, 9, "%s", g_ia.loadout[0][k]);
        if (en[k].mech >= 0 && g_ia.loadout[1][k][0] && mech_index_of(g_ia.loadout[1][k]) == en[k].mech) snprintf(en[k].loadout, 9, "%s", g_ia.loadout[1][k]);
    }
    if (fr[0].mech < 0) {   /* the player needs a mech: the first filled slot leads */
        for (k = 1; k < 3; k++) if (fr[k].mech >= 0) { fr[0].mech = fr[k].mech; fr[k].mech = -1; break; }
        if (fr[0].mech < 0) fr[0].mech = 9;
    }
    snprintf(fr[0].pilot, sizeof fr[0].pilot, "%s", pilot);
    rc = lance_write_all(a, save, fr, en, 1) | lance_write_instmap(a, save, g_ia.clan[0], g_ia.clan[1]);
    prj_close(a);
    if (rc) { fprintf(stderr, "launch: cannot write the lance files into %s\n", save); return -1; }
    snprintf(scen, sizeof scen, "%s", IA_SCEN[g_ia.scen]);
    for (k = 0; scen[k]; k++) if (scen[k] >= 'a' && scen[k] <= 'z') scen[k] = (char)(scen[k] - 32);
    snprintf(g_load_scene, sizeof g_load_scene, "%s", scen);
    printf("launch: %s -of=%s -oe=%s (files in %s)\n", scen, FORM_ARG[g_ia.form[0]], FORM_ARG[g_ia.form[1]], save);
    setenv("MW2_OF", FORM_ARG[g_ia.form[0]], 1);
    setenv("MW2_OE", FORM_ARG[g_ia.form[1]], 1);
    sim_command(line, sizeof line, scen, save);
    sfx_lock(g_sfx); gus_song_close(g_song); g_song = NULL; mt32_song_close(g_msong); g_msong = NULL; g_song_entry = -1; sfx_unlock(g_sfx);   /* the menu music stops for the mission (the MT-32 one too) */
    sfx_stop_loops(g_sfx);
    if (g_win) SDL_HideWindow(g_win);
    snprintf(path, sizeof path, "%s/MW2MSN.CFG", save);
    remove(path);   /* a stale result must not be mistaken for this mission's */
    loading_screen(g_load_clan);
    rc = system(line);
    read_results(save);
    sim_returned(rc);
    if (g_win) { SDL_ShowWindow(g_win); SDL_RaiseWindow(g_win); }
    printf("launch: mission returned %d\n", rc);
    return 0;
}

static int run_instant_action(SDL_Renderer *rd, SDL_Texture *tx, int headless, int frames, int *mx, int *my)
{
    overlay ov[4];
    int next = -1, f = 0, i;
    char note[64] = "";
    const uint8_t (*pal)[3];
    memset(ov, 0, sizeof ov);
    load_bg(8);
    pal = (const uint8_t (*)[3])g_bg.pal;
    if (g_from_screen == 8) { g_ia.scen = 0; ia_load_scenario(); }   /* FUN_00029680 from the main menu (EBX 8): the first
                                                                         * scenario, both stars reloaded from it (FUN_000291b0) */
    else if (!g_ia.loaded) ia_load_scenario();
    ov_set(&ov[0], IA_CLAN[g_ia.clan[0]], 13, 205);
    ov_set(&ov[1], IA_CLAN[g_ia.clan[1]], 483, 329);
    ov_set(&ov[2], "WIALANCH", 209, 371);
    ov[2].hold = 1;                                   /* flags 0x24: loaded but not shown until LAUNCH starts it (FUN_00039610) */
    ia_planet(&ov[3]);
    while (next == -1) {
        SDL_Event e;
        int click = -1;
        if (!headless) {
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) next = -3;
                if (e.type == SDL_MOUSEMOTION) { *mx = e.motion.x; *my = e.motion.y; }
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) next = 8;
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT)
                    for (i = 0; i < 25; i++)
                        if (e.button.x >= IA_HS[i].x0 && e.button.x <= IA_HS[i].x1 && e.button.y >= IA_HS[i].y0 && e.button.y <= IA_HS[i].y1) click = i;
            }
        } else if (getenv("MW2_SHELL_CLICKS") && f == 0) {   /* headless: "3,3,14,11" */
            const char *c = getenv("MW2_SHELL_CLICKS");
            while (*c) {
                int h = atoi(c), side;
                if (h >= 3 && h <= 5) { side = 0; g_ia.mech[side][h - 3] = g_ia.mech[side][h - 3] + 1 >= g_ia.nmech ? -1 : g_ia.mech[side][h - 3] + 1; }
                if (h >= 14 && h <= 16) { side = 1; g_ia.mech[side][h - 14] = g_ia.mech[side][h - 14] + 1 >= g_ia.nmech ? -1 : g_ia.mech[side][h - 14] + 1; }
                if (h == 6) g_ia.form[0] = (g_ia.form[0] + 1) % 6;
                if (h == 2) { g_ia.scen = (g_ia.scen + 1) % 10; ia_load_scenario(); }
                if (h == 0) { launch_ia(); snprintf(note, sizeof note, "%s", g_result); }
                while (*c && *c != ',') c++;
                if (*c) c++;
            }
            ia_planet(&ov[3]);
        }
        if (click >= 0) {
            int side = (click >= 14) ? 1 : 0, h = side ? click - 11 : click;   /* map enemy hotspots onto 3..13 */
            if (click == 0) {   /* Launch -> the launch clip plays (FUN_00039610 slot 0x10), the mission runs, back here */
                if (ov[2].s) {
                    int fr, last = -1;
                    smk_rewind(ov[2].s);
                    while ((fr = smk_next(ov[2].s)) > last) {
                        last = fr;
                        draw_bg();
                        for (i = 0; i < 4; i++) if (i != 2) blit_smk(ov[i].s ? ov[i].s : ov[2].s, ov[i].s ? ov[i].x : 0, ov[i].s ? ov[i].y : 0);
                        blit_smk(ov[2].s, ov[2].x, ov[2].y);
                        SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
                        SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); SDL_RenderPresent(rd);
                        SDL_Delay((Uint32)smk_frame_ms(ov[2].s));
                    }
                    smk_rewind(ov[2].s);
                }
                launch_ia();
                apply_music(7, 2);
                play_ambient(-1);
                snprintf(note, sizeof note, "%s", g_result);
            }
            else if (click == 1) next = 8;
            else if (click == 2) {   /* next scenario: its lances, text and planet (FUN_000291b0) */
                g_ia.scen = (g_ia.scen + 1) % 10;
                ia_load_scenario();
                ia_planet(&ov[3]);
            } else if ((h >= 3 && h <= 5) || (h >= 7 && h <= 9)) {   /* next / previous chassis; FUN_0003a8a0 refuses one over
                                                                    * the scenario's tonnage limit, so the arrows pass it */
                int k = h <= 5 ? h - 3 : h - 7, d = h <= 5 ? 1 : -1, q, m2 = g_ia.mech[side][k];
                for (q = 0; q <= g_ia.nmech; q++) {
                    m2 += d;
                    if (m2 >= g_ia.nmech) m2 = -1;
                    if (m2 < -1) m2 = g_ia.nmech - 1;
                    if (m2 < 0 || MECHS[m2].tons <= g_ia.tons[side]) break;
                }
                g_ia.mech[side][k] = m2;
            }
            else if (h == 6) g_ia.form[side] = (g_ia.form[side] + 1) % 6;
            else if (h == 10) g_ia.form[side] = (g_ia.form[side] + 5) % 6;
            else if (h == 11) {   /* clan emblem: next clan, never the other side's */
                int o = g_ia.clan[!side], c = (g_ia.clan[side] + 1) % 6;
                if (c == o) c = (c + 1) % 6;
                g_ia.clan[side] = c;
                ov_set(&ov[side], IA_CLAN[c], side ? 483 : 13, side ? 329 : 205);
            } else if (h == 12) { g_ia_ctx = 2 + side; g_lab_slot = 0; next = 9; }   /* MECH LAB (context 2) */
            else if (h == 13) { g_ia_ctx = 2 + side; next = 13; }                  /* STAR CONFIG */
        }
        draw_bg();
        for (i = 0; i < 4; i++) if (i != 2 || !ov[2].hold) ov_draw(&ov[i], headless);
        {
            int sd, k;
            char sc[32];
            for (sd = 0; sd < 2; sd++) {
                for (k = 0; k < 3; k++) {
                    const rect7 *r = &IA_HS[(sd ? 14 : 3) + k];
                    int m = g_ia.mech[sd][k];
                    draw_text(&IA_FONT, r->lx, r->ly, m >= 0 ? MECHS[m].name : "[none]", pal, IA_INK);
                }
                draw_text(&IA_FONT, IA_HS[sd ? 17 : 6].lx, IA_HS[sd ? 17 : 6].ly, IA_FORM[g_ia.form[sd]], pal, IA_INK);
            }
            (void)sc;
            for (k = 0; k < 3; k++) draw_text(&IA_FONT, IA_TEXT_X, IA_TEXT_Y + 12 * k, g_ia.text[k], pal, IA_INK);   /* PDSC (FUN_000291b0) */
        }
        {   /* EXIT: grey, white under the cursor (as the roster's labels) */
            int hv = *mx >= IA_HS[1].x0 && *mx <= IA_HS[1].x1 && *my >= IA_HS[1].y0 && *my <= IA_HS[1].y1;
            draw_text(&g_flabel, IA_HS[1].lx - text_width(&g_flabel, "EXIT") / 2, IA_HS[1].ly, "EXIT", pal, hv ? -1 : 6);
        }
        if (note[0]) draw_text(&g_flist, 320 - text_width(&g_flist, note) / 2, 466, note, pal, 0x22);
        f++;
        if (headless) {
            if (f >= frames) next = -2;
        } else {
            SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
            SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
            SDL_Delay(10);
        }
    }
    for (i = 0; i < 4; i++) ov_set(&ov[i], NULL, 0, 0);
    return next;
}


/* ---- pilot registry MW2REG.CFG: 20 records x 60 bytes (Wolf 0-9, Jade Falcon 10-19), FUN_00038310:
 * +0 in use, +4 selected, +8 clan, +12 missions done (campaign progress; 16 = won), +16 rank title (0x7d394),
 * +20 honor (FUN_00028250: Hall of Honor), +24.. kills / hits / shots, +40 name (NUL-terminated, up to 16 bytes) */
typedef struct { int32_t used, selected, clan, rank, title, honor, stats[4]; char name[16]; int32_t rt; } pilot_rec;   /* 60 bytes */
typedef char pilot_rec_is_60_bytes[sizeof(pilot_rec) == 60 ? 1 : -1];
static pilot_rec g_reg[20];
static void reg_load(void)
{
    char p[700];
    FILE *f;
    snprintf(p, sizeof p, "%s/MW2REG.CFG", g_dir);
    memset(g_reg, 0, sizeof g_reg);
    if ((f = fopen(p, "rb"))) { if (fread(g_reg, 1, sizeof g_reg, f) != sizeof g_reg) { /* short: keep what was read */ } fclose(f); }
}
static void reg_save(void)
{
    char p[700];
    FILE *f;
    int i;
    snprintf(p, sizeof p, "%s/MW2REG.CFG", g_dir);
    for (i = 0; i < 20; i++) g_reg[i].rt = 0;   /* the runtime field (the name's text object) is not kept */
    if ((f = fopen(p, "wb"))) { fwrite(g_reg, 1, sizeof g_reg, f); fclose(f); }
}
static const char *ia_pilot(void)   /* the registered pilot (Wolf, else Jade Falcon), as the original's callsign */
{
    int c, i;
    reg_load();
    for (c = 0; c < 2; c++) for (i = 0; i < 10; i++) if (g_reg[c * 10 + i].used && g_reg[c * 10 + i].selected) return g_reg[c * 10 + i].name;
    return getenv("MW2_PILOT") ? getenv("MW2_PILOT") : "MechWarrior";
}
static int reg_selected(int clan)
{
    int i;
    for (i = 0; i < 10; i++) if (g_reg[clan * 10 + i].used && g_reg[clan * 10 + i].selected) return clan * 10 + i;
    return -1;
}
/* roster hotspots (0x7f0bc + clan x 16 -> 0x7d5b8 / 0x7df20, identical): 0 NEW ALLEGIANCE, 1-10 pilot rows,
 * 11 ACCEPT, 12 DELETE MECHWARRIOR, 13 LAUNCH OLD MISSION, 14 PILOT INFO; background slot 17 / 24 */
static const int ROSTER_HS[15][4] = {
    {466, 450, 619, 474}, {32, 83, 297, 116}, {32, 117, 297, 151}, {32, 152, 297, 186}, {32, 187, 297, 221}, {32, 222, 297, 256},
    {32, 257, 297, 291}, {32, 292, 297, 326}, {32, 327, 297, 361}, {32, 362, 297, 397}, {32, 398, 297, 432},
    {294, 450, 393, 474}, {20, 450, 221, 474}, {373, 403, 562, 427}, {418, 403, 517, 427}};
static const char *ROSTER_LABEL[15] = {"NEW ALLEGIANCE", "", "", "", "", "", "", "", "", "", "", "ACCEPT", "DELETE MECHWARRIOR", "LAUNCH OLD MISSION", "PILOT INFO"};
static const int ROSTER_LXY[15][2] = {{543, 455}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {344, 455}, {121, 455}, {468, 408}, {468, 408}};

static const char *RANK_TITLE[9] = {"Mechwarrior", "Star Commander", "Nova Commander", "Star Captain", "Nova Captain",
                                    "Star Colonel", "Nova Colonel", "Galaxy Commander", "Khan"};   /* 0x7d394 */
static const char *CAMPAIGN_TITLE[2][16];
static int launch_campaign_m(int clan, int reg, int m);
static void star_reset_mission(int clan, int mission);
/* LAUNCH OLD MISSION's list (panel table 0x83ad0, items made by 0x38150): the name (font 0x91174) at (468, 92), the
 * label "~Select Mission" (font 0x91178 = the RANK / HONOR labels') at (468, 200), then 16 items {x 418, w 100, index k}:
 * item k exists only while k < the pilot's missions done (MW2REG +12) and shows the campaign title (0x7f04c + clan x 4,
 * 9-byte records, title at +5) in font 0x91170 (DATABASE font 31, white) centred on 418 + 100 / 2; y is relative (the
 * table's 0x8000xxyy encoding, 0x2ac90): the first item 3 below the label's height, each next one 2 below the previous
 * one's height -> 219, 231, 243 ... (DOS capture). A click inside an item's box (0x305a0: 418 <= x < 518, y .. y + h)
 * launches that mission (0x385a0) */
#define OLD_LIST_LABEL_Y 200
static int old_item_y(int k) { return OLD_LIST_LABEL_Y + g_flabel.d[8] + 3 + k * (g_fthin.d[8] + 2); }
/* the title as the shell's table spells it (centred with its width): Wolf mission 2 is "Flame Tongue " (0x76598, a
 * trailing space - it sits half a space left of centre in the DOS capture) */
static const char *roster_title(int clan, int k)
{
    static char t[24];
    snprintf(t, sizeof t, "%s%s", CAMPAIGN_TITLE[clan][k], clan == 0 && k == 1 ? " " : "");
    return t;
}
static int run_roster(SDL_Renderer *rd, SDL_Texture *tx, int headless, int frames, int clan, int *mx, int *my)
{
    int next = -1, f = 0, editing = -1, i, confirm_del = 0, old_list = 0;
    char edit[16] = "";
    reg_load();
    load_bg(clan == 1 ? 23 : 16);
    if (!headless) { SDL_StartTextInput(); shell_sound(0x51, 0x1e); }   /* 0x384f8: DATABASE sound 0x51 at volume 0x1e on entering */
    while (next == -1) {
        SDL_Event e;
        int sel = reg_selected(clan), hover = -1, nclick = 0, c;
        int enabled[15], click[16][2];
        if (f == 0 && getenv("MW2_SHELL_CLICKS")) {   /* tests: "x,y;x,y;..." clicks on the first frame */
            const char *q = getenv("MW2_SHELL_CLICKS");
            while (*q && nclick < 16) {
                if (sscanf(q, "%d,%d", &click[nclick][0], &click[nclick][1]) == 2) nclick++;
                while (*q && *q != ';') q++;
                if (*q) q++;
            }
        }
        if (!headless) {
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) { next = -3; break; }
                if (e.type == SDL_MOUSEMOTION) { *mx = e.motion.x; *my = e.motion.y; }
                if (editing >= 0 && e.type == SDL_TEXTINPUT && strlen(edit) + strlen(e.text.text) < 15) strcat(edit, e.text.text);
                if (e.type == SDL_KEYDOWN) {
                    SDL_Keycode k = e.key.keysym.sym;
                    if (editing >= 0) {
                        if (k == SDLK_BACKSPACE && edit[0]) edit[strlen(edit) - 1] = 0;
                        else if (k == SDLK_ESCAPE) editing = -1;
                        else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && edit[0]) {   /* a new pilot: rank 0, selected */
                            pilot_rec *r = &g_reg[editing];
                            int j;
                            memset(r, 0, sizeof *r);
                            r->used = 1; r->clan = clan;
                            { static int seeded; if (!seeded) { srand((unsigned)time(NULL)); seeded = 1; } }
                            /* FUN_00038310 (0x388e7): honor = round(rand() x 1/32767 x 1000 + 1000), the C library's
                             * 15-bit rand: a new pilot starts with 1000..2000 (the install's pilot drew 1850) */
                            r->honor = (int)lrint((double)(rand() & 0x7fff) * (1.0 / 32767.0) * 1000.0 + 1000.0);
                            snprintf(r->name, sizeof r->name, "%s", edit);
                            for (j = 0; j < 10; j++) g_reg[clan * 10 + j].selected = 0;
                            r->selected = 1;
                            reg_save();
                            editing = -1;
                            old_list = 0;
                        }
                    } else if (k == SDLK_ESCAPE) next = 1;
                }
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && editing < 0 && nclick < 16) {
                    click[nclick][0] = e.button.x; click[nclick][1] = e.button.y; nclick++;
                }
            }
        }
        for (c = 0; c < nclick && next == -1; c++) {
            int bx = click[c][0], by = click[c][1];
            sel = reg_selected(clan);
            for (i = 0; i < 15; i++) enabled[i] = 1;   /* FUN_00028bc0 hides / 0x28b60 shows: none without a pilot; */
            if (sel < 0) enabled[11] = enabled[12] = enabled[13] = enabled[14] = 0;   /* LAUNCH OLD MISSION with missions */
            else { enabled[13] = !old_list && g_reg[sel].rank > 0; enabled[14] = old_list; }   /* done, PILOT INFO in the list */
            if (confirm_del) {   /* "Terminate MechWarrior?#Yes|No" */
                if (by >= 250 && by < 290 && bx >= 230 && bx < 310 && sel >= 0) { memset(&g_reg[sel], 0, sizeof g_reg[sel]); reg_save(); old_list = 0; }
                if (by >= 250 && by < 290 && bx >= 230 && bx < 410) confirm_del = 0;
                continue;
            }
            if (old_list && sel >= 0) {   /* 0x385a0: an old mission's item, before the buttons (it can lie over PILOT INFO) */
                int k, done = g_reg[sel].rank;
                for (k = 0; k < 16 && k < done; k++)
                    if (bx >= 418 && bx < 418 + 100 && by >= old_item_y(k) && by < old_item_y(k) + g_fthin.d[8]) break;
                if (k < 16 && k < done) {
                    /* 0x385d1: the mission's own star (0x3ab50(0,0,3,1,100), then 0x291b0: BRF2 SDSC size / mechs,
                     * formation 0), the pilot in slot 0, state 10: launch (0x375a0 with the roster as the return
                     * state 12): no briefing, and the simulation returns to the roster - no debriefing, so neither
                     * the campaign position nor the honor changes */
                    star_reset_mission(clan, k);
                    printf("roster: launch old mission %d (%s)\n", k + 1, CAMPAIGN_TITLE[clan][k]);
                    launch_campaign_m(clan, sel, k);
                    reg_load();
                    load_bg(clan == 1 ? 23 : 16);
                    old_list = 0;   /* the roster again, as on entering (the record panel) */
                    if (!headless) shell_sound(0x51, 0x1e);
                    continue;
                }
            }
            for (i = 0; i < 15; i++) {
                if (!enabled[i] || bx < ROSTER_HS[i][0] || bx > ROSTER_HS[i][2] || by < ROSTER_HS[i][1] || by > ROSTER_HS[i][3]) continue;
                if (i >= 1 && i <= 10) {
                    int slot = clan * 10 + i - 1, j;
                    if (g_reg[slot].used) { for (j = 0; j < 10; j++) g_reg[clan * 10 + j].selected = 0; g_reg[slot].selected = 1; reg_save(); }
                    else { editing = slot; edit[0] = 0; }
                    old_list = 0;                                  /* either way the record panel (0x38690 / 0x38800) */
                } else if (i == 0) next = 8;                       /* NEW ALLEGIANCE: back to the title */
                else if (i == 11) next = 1;                        /* ACCEPT: the clan hall with this pilot */
                else if (i == 12 && sel >= 0) confirm_del = 1;     /* asks first */
                else if (i == 13) old_list = 1;                    /* LAUNCH OLD MISSION (0x38708): the list, PILOT INFO */
                else if (i == 14) old_list = 0;                    /* PILOT INFO (0x38727): the record again */
                break;
            }
        }
        sel = reg_selected(clan);
        for (i = 0; i < 15; i++) enabled[i] = 1;
        if (sel < 0) { enabled[11] = enabled[12] = enabled[13] = enabled[14] = 0; old_list = 0; }
        else { enabled[13] = !old_list && g_reg[sel].rank > 0; enabled[14] = old_list; }
        draw_bg();
        for (i = 0; i < 10; i++) {   /* names in font 0x91174 (29) at x 42, y 92 + 35 i (FUN_00026890) */
            const pilot_rec *r = &g_reg[clan * 10 + i];
            const char *nm = editing == clan * 10 + i ? edit : (r->used ? r->name : "");
            if (nm[0] || editing == clan * 10 + i) {
                char line[24];
                snprintf(line, sizeof line, "%s%s", nm, editing == clan * 10 + i && (f / 15) % 2 ? "_" : "");
                draw_text(&g_fbig, 42, 92 + 35 * i, line, (const uint8_t (*)[3])g_bg.pal, -1);
                /* (no marker: the DOS roster shows the active pilot in the ACTIVE PILOT panel only) */
            }
        }
        if (sel >= 0) {   /* ACTIVE PILOT and RECORD (panel table 0x83970): all centred at x 468 - the name (font 29) at
                           * y 92; labels (font 27) RANK y 209, HONOR 260, MISSION 311; values (font 29) 19 below: the
                           * rank title, the honor, the current campaign mission's title. In the old-mission list
                           * (0x83ad0) the name, "Select Mission" and the missions done instead */
            const pilot_rec *r = &g_reg[sel];
            const uint8_t (*pal)[3] = (const uint8_t (*)[3])g_bg.pal;
            char v[48];
            int ri = r->title >= 0 && r->title < 9 ? r->title : 0, k2;
            static const int LY[3] = {209, 260, 311};   /* labels; values 19 below (DOS: 228, 279, 330) */
            static const char *LBL[3] = {"RANK", "HONOR", "MISSION"};
            snprintf(v, sizeof v, "%s", r->name);
            for (k2 = 0; v[k2]; k2++) if (v[k2] >= 'a' && v[k2] <= 'z') v[k2] = (char)(v[k2] - 32);
            draw_text(&g_fbig, 468 - text_width(&g_fbig, v) / 2, 92, v, pal, -1);
            if (old_list) {
                draw_text(&g_flabel, 468 - text_width(&g_flabel, "Select Mission") / 2, OLD_LIST_LABEL_Y, "Select Mission", pal, -1);
                for (k2 = 0; k2 < 16 && k2 < r->rank && clan >= 0 && clan < 2; k2++)
                    draw_text(&g_fthin, 418 + 100 / 2 - text_width(&g_fthin, roster_title(clan, k2)) / 2, old_item_y(k2), roster_title(clan, k2), pal, -1);
            } else
                for (k2 = 0; k2 < 3; k2++) {
                    int q;
                    draw_text(&g_flabel, 468 - text_width(&g_flabel, LBL[k2]) / 2, LY[k2], LBL[k2], pal, -1);
                    if (k2 == 0) snprintf(v, sizeof v, "%s", RANK_TITLE[ri]);
                    else if (k2 == 1) snprintf(v, sizeof v, "%d", r->honor);
                    else snprintf(v, sizeof v, "%s", r->rank >= 0 && r->rank < 16 && clan >= 0 && clan < 2 ? roster_title(clan, r->rank) : "-");
                    for (q = 0; v[q]; q++) if (v[q] >= 'a' && v[q] <= 'z') v[q] = (char)(v[q] - 32);
                    draw_text(&g_fbig, 468 - text_width(&g_fbig, v) / 2, LY[k2] + 19, v, pal, -1);
                }
        }
        if (confirm_del) {
            int x, y;
            for (y = 200; y < 300; y++) for (x = 200; x < 440; x++) { uint8_t *q = g_fb + ((size_t)y * W + (size_t)x) * 3; q[0] /= 4; q[1] /= 4; q[2] /= 4; }
            text_layer_cull(200, 200, 440, 300);
            draw_text(&g_flabel, 320 - text_width(&g_flabel, "Terminate MechWarrior?") / 2, 220, "Terminate MechWarrior?", (const uint8_t (*)[3])g_bg.pal, -1);
            draw_text(&g_flabel, 270 - text_width(&g_flabel, "YES") / 2, 265, "YES", (const uint8_t (*)[3])g_bg.pal, -1);
            draw_text(&g_flabel, 370 - text_width(&g_flabel, "NO") / 2, 265, "NO", (const uint8_t (*)[3])g_bg.pal, -1);
        }
        for (i = 0; i < 15; i++)
            if (enabled[i] && *mx >= ROSTER_HS[i][0] && *mx <= ROSTER_HS[i][2] && *my >= ROSTER_HS[i][1] && *my <= ROSTER_HS[i][3]) hover = i;
        for (i = 0; i < 15; i++) {   /* buttons: "<~" labels, shown when enabled, grey (palette 6, 170,170,170), white
                                      * under the cursor (DOS captures) */
            int tw;
            if (!ROSTER_LABEL[i][0] || !enabled[i]) continue;
            tw = text_width(&g_flabel, ROSTER_LABEL[i]);
            draw_text(&g_flabel, ROSTER_LXY[i][0] - tw / 2, ROSTER_LXY[i][1], ROSTER_LABEL[i], (const uint8_t (*)[3])g_bg.pal, hover == i ? -1 : 6);
        }
        f++;
        if (headless) { if (f >= frames) { next = -2; } }
        else {
            SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
            SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
            SDL_Delay(10);
        }
    }
    if (!headless) { SDL_StopTextInput(); shell_sound_stop(0x51); }   /* 0x387a0: the sound object is freed on leaving */
    return next;
}


/* ---- per-mission records: <first 4 letters><suffix> (BRF1 briefing, BRF2 star setup SDSC / SUPS, DBFS / DBFF
 * aftermath after success / failure); ORDR text read into lines */
static int mission_record(const char *scen, const char *suffix, prj_record *r)
{
    char err[256], path[700], rec[16];
    prj_archive *a;
    int i, rc = -1;
    snprintf(path, sizeof path, "%s", gpath("MW2.PRJ"));
    snprintf(rec, sizeof rec, "%.4s%s", scen, suffix);
    for (i = 0; rec[i]; i++) if (rec[i] >= 'a' && rec[i] <= 'z') rec[i] = (char)(rec[i] - 32);
    if ((a = prj_open(path, err, sizeof err))) { rc = prj_read_named(a, "BWD", rec, r) == PRJ_OK ? 0 : -1; prj_close(a); }
    return rc;
}
static const uint8_t *chunk_find(const prj_record *r, const char *tag, uint32_t *len)
{
    size_t o = 12;
    while (o + 8 <= r->size) {
        uint32_t sz = r->data[o + 4] | r->data[o + 5] << 8 | r->data[o + 6] << 16 | (uint32_t)r->data[o + 7] << 24;
        if (memcmp(r->data + o, tag, 4) == 0) { *len = sz - 8; return r->data + o + 8; }
        if (sz < 8) break;
        o += sz;
    }
    return NULL;
}
/* SDSC (BRF2; shell 0x000291b0 -> 0x0003ab50): {?, tonnage limit, default star size, maximum star size} then 16-byte
 * loadout names (default / assigned mechs; as many as the maximum). The star record takes +0xc = the default (the
 * current size) and +8 = the maximum, both capped at 3; Star Configuration's +/- move the size between 1 and the
 * maximum */
typedef struct { int ok, tons, size, def; char loadout[3][9]; } mission_star;
static mission_star mission_sdsc(const char *scen)
{
    mission_star ms;
    prj_record r;
    uint32_t len;
    const uint8_t *p;
    memset(&ms, 0, sizeof ms);
    if (mission_record(scen, "BRF2", &r) != 0) return ms;
    if ((p = chunk_find(&r, "SDSC", &len)) && len >= 16) {
        int k;
        ms.ok = 1;
        ms.tons = p[4] | p[5] << 8;
        ms.size = p[12] | p[13] << 8;
        if (ms.size < 1) ms.size = 1;
        if (ms.size > 3) ms.size = 3;
        ms.def = p[8] | p[9] << 8;
        if (ms.def < 1) ms.def = 1;
        if (ms.def > ms.size) ms.def = ms.size;
        for (k = 0; k < 3 && 16 + (k + 1) * 16 <= (int)len; k++) snprintf(ms.loadout[k], 9, "%.8s", (const char *)p + 16 + k * 16);
    }
    prj_record_free(&r);
    return ms;
}
static int loadout_tons(const char *loadout)
{
    char err[256], path[700];
    prj_archive *a;
    prj_record r;
    int t = 0;
    snprintf(path, sizeof path, "%s", gpath("MW2.PRJ"));
    if ((a = prj_open(path, err, sizeof err))) {
        if (prj_read_named(a, "MEK", loadout, &r) == PRJ_OK) { mek_def d; if (mek_parse(r.data, r.size, &d) == 0) t = (int)d.tonnage; prj_record_free(&r); }
        prj_close(a);
    }
    return t;
}
static int mech_index_of(const char *loadout)
{
    int i;
    for (i = 0; i < lance_mech_count(); i++) if (strncasecmp(lance_mech_code(i), loadout, 3) == 0) return i;
    return -1;
}
static int g_last_tons = -1, g_last_limit = -1;   /* the star's tonnage and the mission's limit at launch */

/* ---- the campaigns (trials table 0x7f04c + clan x 4 -> 0x7ef14 / 0x7efb0): 9-byte records
 * {scenario ptr, trial flag, title ptr}; the pilot's "missions done" (MW2REG +12) indexes them */
static const char *CAMPAIGN_SCEN[2][16] = {
    {"yellSCN1", "oranSCN1", "tealSCN1", "taupSCN1", "jennSCN1", "sablSCN1", "greySCN1", "browSCN1",
     "amy_SCN1", "silvSCN1", "aquaSCN1", "kim_SCN1", "cyanSCN1", "maroSCN1", "goldSCN1", "irenSCN1"},
    {"pinkSCN1", "greeSCN1", "red_SCN1", "fuchSCN1", "cindSCN1", "rustSCN1", "umbeSCN1", "tan_SCN1",
     "heidSCN1", "plumSCN1", "whitSCN1", "jillSCN1", "puceSCN1", "blonSCN1", "bronSCN1", "marySCN1"}};
static const char *CAMPAIGN_TITLE[2][16] = {
    {"Pyre Light", "Flame Tongue", "Blade Splint", "Temper Edge", "Trial 1", "Sable Flame", "Burning Chrome", "Scorching Sand",
     "Trial 2", "Silver Staff", "Aquiline Fire", "Trial 3", "Cold Crescent", "Velvet Hammer", "Golden Spade", "Trial 4"},
    {"Silent Thunder", "Arkham Bridge", "Mirror Cage", "Bone Machine", "Trial 1", "Bouk Obelisk", "Umber Wall", "Rogue Chariot",
     "Trial 2", "Plum Wine", "Rust Heart", "Trial 3", "Armor Veil", "Iron Piston", "Bronze Anvil", "Trial 4"}};
static const int CAMPAIGN_TRIAL[2][16] = {{0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1}, {0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1}};
int campaign_is_trial(int clan, int m) { return m >= 0 && m < 16 && CAMPAIGN_TRIAL[clan][m]; }   /* the mech is assigned */

static void shell_message(SDL_Renderer *rd, SDL_Texture *tx, int headless, const char *msg);
static int shell_dialog(SDL_Renderer *rd, SDL_Texture *tx, int headless, const char *msg, int def);
/* ---- the star (Star Configuration, screen 13, FUN_0003b420; record 0x841dc per clan): formation 0-5, size 1..3,
 * slots {mech, loadout, pilot name}; saved by the port as STAR<clan>.CFG in the install dir (the original's
 * storage NOT CONFIRMED) */
typedef struct { int formation, size; lance_slot slot[3]; } star_cfg;
static star_cfg g_star[4];   /* 0 / 1 the clans' stars, 2 / 3 Instant Action friendly / enemy (= g_ia) */
static const char *ia_pilot(void);
static void ia_load_scenario(void);
static void star_load(int clan)
{
    char p[700];
    FILE *f;
    star_cfg *st = &g_star[clan];
    int k;
    if (clan >= 2) {   /* the Instant Action star records 0x840dc / 0x8415c */
        int sd = clan - 2;
        if (!g_ia.loaded) ia_load_scenario();
        memset(st, 0, sizeof *st);
        st->formation = g_ia.form[sd];
        st->size = 0;
        for (k = 0; k < 3; k++) {
            st->slot[k].mech = g_ia.mech[sd][k];
            snprintf(st->slot[k].loadout, sizeof st->slot[k].loadout, "%s", g_ia.loadout[sd][k]);
            if (sd == 0 && k == 0) snprintf(st->slot[k].pilot, sizeof st->slot[k].pilot, "%s", ia_pilot());
            else snprintf(st->slot[k].pilot, sizeof st->slot[k].pilot, sd ? "Enemy %d" : "Friend %d", sd ? k + 1 : k);
            if (g_ia.mech[sd][k] >= 0) st->size = k + 1;
        }
        if (st->size < 1) st->size = 1;
        return;
    }
    memset(st, 0, sizeof *st);
    st->size = 1;
    for (k = 0; k < 3; k++) { st->slot[k].mech = -1; snprintf(st->slot[k].pilot, sizeof st->slot[k].pilot, k ? "Friend %d" : "MechWarrior", k); }
    st->slot[0].mech = -1;   /* not chosen: each mission's own mech (BRF2 SDSC) */
    snprintf(p, sizeof p, "%s/STAR%d.CFG", g_dir, clan);
    if ((f = fopen(p, "rb"))) { star_cfg t; if (fread(&t, 1, sizeof t, f) == sizeof t) *st = t; fclose(f); }
    if (st->size < 1 || st->size > 3) st->size = 1;
    if (st->formation < 0 || st->formation > 5) st->formation = 0;
}
static void star_save(int clan)
{
    char p[700];
    FILE *f;
    if (clan >= 2) {
        int sd = clan - 2, k;
        g_ia.form[sd] = g_star[clan].formation;
        for (k = 0; k < 3; k++) {
            g_ia.mech[sd][k] = k < g_star[clan].size ? g_star[clan].slot[k].mech : -1;
            snprintf(g_ia.loadout[sd][k], 9, "%s", k < g_star[clan].size ? g_star[clan].slot[k].loadout : "");
        }
        return;
    }
    snprintf(p, sizeof p, "%s/STAR%d.CFG", g_dir, clan);
    if ((f = fopen(p, "wb"))) { fwrite(&g_star[clan], 1, sizeof g_star[clan], f); fclose(f); }
}
/* the campaign star back to the mission's own (main loop 0x36700 case 0xb: entering the Ready Room from the clan
 * hall (1) or the debriefing (3) runs FUN_0003ab50(0, 0, 3, 1, 100) then FUN_000291b0(<mission>, 1): formation 0,
 * maximum / size / tonnage from BRF2 SDSC and every slot's loadout from it (FUN_0003a8a0), so a 'Mech accepted or
 * customized in the Mech Lab holds only until the room is next entered that way - not across missions or replays
 * from the Ready Room; the FREEBIRTHTOAD mission pick (FUN_00037a50 default case) does the same. The pilots' names stay. */
static void star_reset_mission(int clan, int mission)
{
    star_cfg *st = &g_star[clan];
    mission_star ms;
    int k;
    if (clan < 0 || clan > 1 || mission < 0 || mission > 15) return;
    star_load(clan);
    ms = mission_sdsc(CAMPAIGN_SCEN[clan][mission]);
    st->formation = 0;
    st->size = ms.ok ? ms.def : 1;
    for (k = 0; k < 3; k++) {
        st->slot[k].mech = -1;
        st->slot[k].loadout[0] = 0;
        if (ms.ok && k < ms.size && ms.loadout[k][0] && mech_index_of(ms.loadout[k]) >= 0) {
            st->slot[k].mech = mech_index_of(ms.loadout[k]);
            snprintf(st->slot[k].loadout, sizeof st->slot[k].loadout, "%s", ms.loadout[k]);
        }
    }
    star_save(clan);
}

/* ---- the Mech Lab (screen 9, FUN_000339e0), chassis / variant choice only for now (weapon editing NOT YET):
 * the 18 shell mechs; variants = the archive's MEK records <code>NN... */
static int mek_variants(const char *code, char out[][9], int max)
{
    char path[1100], err[256];
    prj_archive *a;
    int t, i, n = 0;
    if ((a = prj_open(gpath("MW2.PRJ"), err, sizeof err))) {
        t = prj_find_type(a, "MEK");
        for (i = 0; t >= 0 && i < prj_symbol_count(a, t) && n < max; i++) {
            const char *nm = prj_symbol_name(a, t, i);
            if (strncasecmp(nm, code, 3) == 0 && strlen(nm) <= 8) {
                int k;
                snprintf(out[n], 9, "%s", nm);
                for (k = 0; out[n][k]; k++) if (out[n][k] >= 'A' && out[n][k] <= 'Z') out[n][k] = (char)(out[n][k] + 32);
                n++;
            }
        }
        prj_close(a);
    }
    {   /* the player's designs: <install>/MEK/<code>NNusr.mek (any case) */
        DIR *dd;
        struct dirent *de;
        snprintf(path, sizeof path, "%s/MEK", g_dir);
        if ((dd = opendir(path))) {
            while ((de = readdir(dd)) && n < max) {
                const char *f = de->d_name;
                if (strlen(f) == 12 && strncasecmp(f, code, 3) == 0 && strncasecmp(f + 5, "usr.mek", 7) == 0) {
                    int k;
                    snprintf(out[n], 9, "%.8s", f);
                    for (k = 0; out[n][k]; k++) if (out[n][k] >= 'A' && out[n][k] <= 'Z') out[n][k] = (char)(out[n][k] + 32);
                    n++;
                }
            }
            closedir(dd);
        }
    }
    return n;
}
/* the shell's mech table 0x83684: picture code (SMK\AWOMP<code>.SHP / AJFMP / AIAMP), in the lance list's order */
static const char *MECH_PIC[18] = {"ds", "kf", "jn", "bh", "sc", "md", "lo", "rf", "su", "mc", "mw", "wh", "mr", "ms", "da", "el", "ta", "bm"};
/* the component table (BattleTech construction, Clan technology): matches the original's panel for the retail designs
 * (Mad Dog: engine 300 XL 9.5, gyro 3, cockpit 3, heat sinks 12 (24) 2, internal Std 6, armour Ferro 8.5, weapons 26, ammo 2) */
static double engine_std_tons(int r)
{
    static const double T[] = {   /* standard fusion engine, ratings 10..400 step 5 */
        0.5,0.5,0.5,0.5,1,1,1,1,1.5,1.5,1.5,2,2,2,2.5,2.5,3,3,3,3.5,3.5,4,4,4,4.5,4.5,5,5,5.5,5.5,6,6,6,7,7,7.5,7.5,8,8.5,8.5,
        9,8.5,10,10,10.5,11,11.5,12,12.5,13,13.5,14,14.5,15.5,16,16.5,17.5,18,19,19.5,20.5,21.5,22.5,23.5,24,25.5,27,28.5,29.5,31.5,
        33,34.5,36.5,38.5,41,43.5,46,49,52.5};   /* MW2SHELL table 0x7f6cc {rating, tons x 100, name}: 215 -> 8.5, 330 -> 24 as stored */
    int k = r / 5 - 2;
    if (k < 0) k = 0;
    if (k >= (int)(sizeof T / sizeof T[0])) k = (int)(sizeof T / sizeof T[0]) - 1;
    return T[k];
}
/* armour tons as MW2SHELL (0x314a4): points x 100 / 16 (/ 19.2 Ferro) truncated, then to the NEAREST half ton */
static double armor_tons(int pts, int ferro)
{
    int x = (int)(pts * (ferro ? 1 / 19.2 : 0.0625) * 100.0);
    return ((2 * x + 50) / 100) * 0.5;
}
static int masc_tons(int tonnage) { return (tonnage * 4 + 50) / 100; }   /* 0x31661: tons / 25 rounded half up (tons and slots) */
typedef struct { double engine, gyro, cockpit, sinks, jets, internal, armor, weapons, ammo, equip, used; int rating, xl, nsinks, dissip, njets, endo, ferro, dbl; } mek_mass;
static void mek_masses(const mek_def *d, mek_mass *m)
{
    char nb[64];
    int k, sl, pts = 0;
    memset(m, 0, sizeof *m);
    m->rating = (int)(d->walk_mp * d->tonnage);
    for (k = 0; k < MEK_LOC_COUNT; k++) {
        pts += (int)(d->loc[k].armor + d->loc[k].rear_armor);
        for (sl = 0; sl < d->loc[k].slot_count && sl < MEK_SLOTS; sl++) {
            const char *n;
            if (!d->loc[k].slots[sl]) continue;
            n = mek_item_name(d->loc[k].slots[sl], nb, sizeof nb);
            if ((k == MEK_RT || k == MEK_LT) && strcmp(n, "Engine") == 0) m->xl = 1;
            if (strstr(n, "Ferro")) m->ferro = 1;
            if (strstr(n, "Endo")) m->endo = 1;
        }
    }
    m->engine = m->xl ? engine_std_tons(m->rating) / 2.0 : engine_std_tons(m->rating);   /* XL: exactly half (quarter tons) */
    m->gyro = ceil(m->rating / 100.0);
    m->cockpit = 3;
    /* heat sinks (MW2SHELL load, FUN_00030db0): double when any 6001 slot is present; else double only when the design
     * is over its mass with single sinks, the extra sinks weigh 10 t or more and the count is even; else single */
    m->dissip = (int)d->heat_sinking;
    m->dbl = 0;
    for (k = 0; k < MEK_LOC_COUNT && !m->dbl; k++)
        for (sl = 0; sl < d->loc[k].slot_count && sl < MEK_SLOTS; sl++) if (d->loc[k].slots[sl] == 6001) { m->dbl = 1; break; }
    m->nsinks = m->dbl ? m->dissip / 2 : m->dissip;
    m->sinks = m->nsinks > 10 ? m->nsinks - 10 : 0;
    m->njets = (int)d->jump_mp;
    m->jets = m->njets * (d->tonnage <= 55 ? 0.5 : d->tonnage <= 85 ? 1.0 : 2.0);
    m->internal = m->endo ? d->tonnage / 20.0 : d->tonnage / 10.0;   /* exact, as the shell */
    m->armor = armor_tons(pts, m->ferro);
    for (k = 0; k < d->item_count; k++) {
        int w = (int)(d->items[k].id / 100);
        if (d->items[k].id >= 5000 || w >= mek_weapon_count) continue;
        if (w == 29) m->equip += mek_weapons[w].weight_x100 / 100.0;   /* the anti-missile system: equipment (ASSUMED) */
        else m->weapons += mek_weapons[w].weight_x100 / 100.0;
    }
    m->ammo = d->link_count;
    m->used = m->engine + m->gyro + m->cockpit + m->sinks + m->jets + m->internal + m->armor + m->weapons + m->ammo + m->equip;
    if (!m->dbl && m->used > d->tonnage + 1e-9 && m->sinks >= 10 && m->dissip % 2 == 0) {
        m->dbl = 1; m->nsinks = m->dissip / 2; m->sinks = m->nsinks > 10 ? m->nsinks - 10 : 0;
        m->used = m->engine + m->gyro + m->cockpit + m->sinks + m->jets + m->internal + m->armor + m->weapons + m->ammo + m->equip;
    }
}
static int load_mek(const char *loadout, mek_def *d)
{
    char err[256], up[16];
    prj_archive *a;
    int k, rc;
    snprintf(up, sizeof up, "%s", loadout);
    for (k = 0; up[k]; k++) if (up[k] >= 'a' && up[k] <= 'z') up[k] = (char)(up[k] - 32);
    a = prj_open(gpath("MW2.PRJ"), err, sizeof err);
    rc = mek_load(a, up, d);   /* the archive, else MEK\<name>.MEK */
    if (a) prj_close(a);
    return rc;
}
static int mech_lab_kdmt(int clan)   /* the mission's tonnage limit (BRF2 SDSC); 100 outside the campaign */
{
    int reg;
    reg_load();
    reg = reg_selected(clan);
    if (reg >= 0 && g_reg[reg].rank >= 0 && g_reg[reg].rank < 16) {
        mission_star ms = mission_sdsc(CAMPAIGN_SCEN[clan][g_reg[reg].rank]);
        if (ms.ok && ms.tons > 0) return ms.tons;
    }
    return 100;
}

/* ==== the Mech Lab editor (CUSTOMIZE): laid out from DOS captures (docs/reference/dos_customize_*.png).
 * A working copy of the design plus a pool of unassigned critical slots (one entry per slot). Clan rules:
 * engine rating = walk MP x tonnage (FASTER / SLOWER), rating / 25 heat sinks inside the engine (no slots), the first 10
 * sinks free; double heat sinks 2 slots; jump jets 1 slot each (at most walk MP); XL engine 2 slots in each side torso;
 * Endo-steel 7 slots (half the internal mass); Ferro-fibrous 7 slots (19.2 points a ton, else 16); armour bought in half
 * tons, at most 9 in the head and twice the internal structure elsewhere; weapons their table's slots; ammo 1 slot a ton;
 * MASC tonnage / 25 tons and slots (ASSUMED Clan figures). Slot ids as in the MEK data (equipment base + instance / side). */
typedef struct {
    mek_def  d;
    int      halftons;            /* armour bought, half tons */
    uint16_t pool[128];
    int      npool;
    int      page;                /* 0 engine, 3 heat sinks, 4 jump jets, 5 internal, 6 armor, 7 weapons, 8 ammo, 9 equipment, 10 criticals */
    int      armor_loc, crit_loc, sel_fit, sel_table, sel_pool;
    int      masc;                /* tons */
    int      dbl;                 /* double heat sinks (else single: 1 slot, dissipation 1 each) */
} medit;
/* MW2SHELL's location names (0x7f57c), used by index as they stand: 1 Right Torso, 3 Left Torso, 4 Right Arm, 5 Left Arm,
 * 6 Right Leg, 7 Left Leg (the engine's "... Selected" list agrees). mek.h's MEK_RT / MEK_LT etc. name the other way round. */
static const int ARMOR_ORDER[8] = {0, 1, 2, 3, 4, 5, 6, 7};
static const char *ARMOR_NAME[8] = {"Head", "Right Torso", "Center Torso", "Left Torso", "Right Arm", "Left Arm", "Right Leg", "Left Leg"};
static const int INTERNAL_ORDER[8] = {0, 1, 2, 3, 4, 5, 6, 7};   /* as the Armor page (DOS) */
static const char *LOC_NAME[8] = {"Head", "Right Torso", "Center Torso", "Left Torso", "Right Arm", "Left Arm", "Right Leg", "Left Leg"};
#define TABLE_N 27
static const int TABLE_W[TABLE_N] = {22, 23, 24, 21, 25, 26, 27, 11, 12, 13, 14, 15, 10, 16, 17, 18, 19, 6, 5, 4, 9, 8, 7, 3, 2, 1, 0};   /* the WEAPONS TABLE */
static const int TABLE_Y[TABLE_N] = {86, 97, 108, 119, 130, 141, 152, 174, 185, 196, 207, 218, 229, 240, 251, 262, 273, 295, 306, 317, 328, 339, 350, 361, 372, 383, 394};

static int ed_rating(const medit *e) { return (int)(e->d.walk_mp * e->d.tonnage); }
static double ed_ppt(const medit *e, int ferro) { (void)e; return ferro ? 19.2 : 16.0; }
static int ed_count(const medit *e, uint16_t id)   /* slots holding id, placed or pooled */
{
    int n = 0, l, k;
    for (l = 0; l < MEK_LOC_COUNT; l++) for (k = 0; k < e->d.loc[l].slot_count; k++) if (e->d.loc[l].slots[k] == id) n++;
    for (k = 0; k < e->npool; k++) if (e->pool[k] == id) n++;
    return n;
}
static int ed_has_base(const medit *e, int base)   /* any slot with an id in base+1 .. base+49 */
{
    int k;
    for (k = 1; k < 50; k++) if (ed_count(e, (uint16_t)(base + k))) return 1;
    return 0;
}
static uint16_t ed_free_id(const medit *e, int base)
{
    int k;
    for (k = 1; k < 99; k++) if (!ed_count(e, (uint16_t)(base + k))) return (uint16_t)(base + k);
    return (uint16_t)(base + 99);
}
static void ed_pool_add(medit *e, uint16_t id, int n) { while (n-- > 0 && e->npool < 128) e->pool[e->npool++] = id; }
static int ed_remove(medit *e, uint16_t id, int n)   /* n slots of id: the pool first, then the 'Mech; returns how many */
{
    int k, l, done = 0;
    for (k = e->npool - 1; k >= 0 && done < n; k--) if (e->pool[k] == id) { memmove(e->pool + k, e->pool + k + 1, (size_t)(e->npool - k - 1) * 2); e->npool--; done++; }
    for (l = MEK_LOC_COUNT - 1; l >= 0 && done < n; l--) for (k = e->d.loc[l].slot_count - 1; k >= 0 && done < n; k--) if (e->d.loc[l].slots[k] == id) { e->d.loc[l].slots[k] = 0; done++; }
    return done;
}
static int ed_is_xl(const medit *e)
{
    int k;
    for (k = 0; k < e->d.loc[MEK_RT].slot_count; k++) if (e->d.loc[MEK_RT].slots[k] == 5850) return 1;
    return 0;
}
static void ed_sync_sinks(medit *e)   /* heat sinks beyond the engine's capacity (rating / 25) take 2 slots each if double, else 1 */
{
    int per = e->dbl ? 2 : 1, need = (int)e->d.heat_sinking / per - ed_rating(e) / 25, have = 0, k;
    if (need < 0) need = 0;
    for (k = 1; k < 99; k++) if (ed_count(e, (uint16_t)(6000 + k))) have++;
    while (have < need) { ed_pool_add(e, ed_free_id(e, 6000), per); have++; }
    while (have > need) { for (k = 98; k >= 1; k--) if (ed_count(e, (uint16_t)(6000 + k))) { ed_remove(e, (uint16_t)(6000 + k), 9); break; } have--; }
}
static int ed_armor_alloc(const medit *e)
{
    int l, n = 0;
    for (l = 0; l < MEK_LOC_COUNT; l++) n += (int)(e->d.loc[l].armor + e->d.loc[l].rear_armor);
    return n;
}
static int ed_ferro(const medit *e) { return ed_has_base(e, 9000); }
static int ed_endo(const medit *e) { return ed_has_base(e, 8000); }
static int ed_factor(const medit *e)   /* points the bought armour gives (0x2bbd0): (round(tons x 100 x 16 or 19.2) + 49) / 100 */
{
    return ((int)lround(e->halftons * 50.0 * ed_ppt(e, ed_ferro(e))) + 49) / 100;
}
static void ed_trim_armor(medit *e)   /* FUN_0002bbd0: take points off until the allocation fits, round-robin from a
                                        * remembered location, front then rear in each */
{
    static int at;
    int alloc = 0, l, f = ed_factor(e);
    for (l = 0; l < MEK_LOC_COUNT; l++) alloc += (int)(e->d.loc[l].armor + e->d.loc[l].rear_armor);
    while (alloc > f) {
        mek_loc *L = &e->d.loc[at];
        if (L->armor > 0) { L->armor--; alloc--; }
        if (alloc <= f) break;
        if (L->rear_armor > 0) { L->rear_armor--; alloc--; }
        if (++at > 7) at = 0;
    }
}
static int ed_loc_max(const medit *e, int l) { return l == MEK_HEAD ? 9 : 2 * (int)e->d.loc[l].internal; }
static void ed_init(medit *e, const mek_def *d)
{
    int k;
    memset(e, 0, sizeof *e);
    e->d = *d;
    e->halftons = (int)(armor_tons(ed_armor_alloc(e), ed_ferro(e)) * 2.0 + 0.5);   /* as the shell's load (nearest half ton) */
    e->sel_fit = e->sel_table = e->sel_pool = -1;
    e->crit_loc = MEK_HEAD;
    for (k = 0; k < MEK_LOC_COUNT; k++) {}
    if (ed_count(e, 5001)) e->masc = masc_tons((int)e->d.tonnage);
    { mek_mass mm; mek_masses(&e->d, &mm); e->dbl = mm.dbl; }
}
static medit g_lab_test;
static void ed_masses(const medit *e, mek_mass *m)
{
    mek_masses(&e->d, m);
    m->dbl = e->dbl; m->nsinks = e->dbl ? m->dissip / 2 : m->dissip; m->sinks = m->nsinks > 10 ? m->nsinks - 10 : 0;
    m->armor = e->halftons * 0.5;
    m->equip += e->masc;
    m->used = m->engine + m->gyro + m->cockpit + m->sinks + m->jets + m->internal + m->armor + m->weapons + m->ammo + m->equip;
}
/* the items: weapons first, then ammo links */
static void ed_add_ammo(medit *e, int item);
/* the shell's limits (0x2efdc..): 10 weapons, 10 ammo bins a weapon, 25 bins in all, 78 unassigned entries */
static int ed_ammo_bins(const medit *e, uint32_t wid)
{
    int k, n = 0;
    for (k = e->d.item_count; k < e->d.item_count + e->d.link_count; k++) if (e->d.items[k].linked_id == (int32_t)wid) n++;
    return n;
}
static void ed_add_weapon(medit *e, int w)   /* ADD WEAPON: the weapon, plus a ton of its ammo when it uses any */
{
    mek_def *d = &e->d;
    uint32_t id;
    int inst, k;
    if (d->item_count >= 10 || d->item_count + d->link_count >= MEK_MAX_ITEMS || e->npool + mek_weapons[w].crits > 78) return;
    for (inst = 1; inst < 99; inst++) {
        id = (uint32_t)(w * 100 + inst);
        for (k = 0; k < d->item_count; k++) if (d->items[k].id == id) break;
        if (k == d->item_count && !ed_count(e, (uint16_t)id)) break;
    }
    id = (uint32_t)(w * 100 + inst);
    memmove(d->items + d->item_count + 1, d->items + d->item_count, (size_t)d->link_count * sizeof d->items[0]);
    d->items[d->item_count].id = id; d->items[d->item_count].linked_id = -1;
    d->item_count++;
    ed_pool_add(e, (uint16_t)id, mek_weapons[w].crits);
    if (mek_weapons[w].ammo_per_ton > 0) ed_add_ammo(e, d->item_count - 1);
}
static void ed_del_ammo(medit *e, uint32_t wid)
{
    mek_def *d = &e->d;
    int k;
    for (k = d->item_count + d->link_count - 1; k >= d->item_count; k--)
        if (d->items[k].linked_id == (int32_t)wid) {
            memmove(d->items + k, d->items + k + 1, (size_t)(d->item_count + d->link_count - k - 1) * sizeof d->items[0]);
            d->link_count--;
            ed_remove(e, (uint16_t)(10000 + wid), 1);
            return;
        }
}
static void ed_del_weapon(medit *e, int item)
{
    mek_def *d = &e->d;
    uint32_t id = d->items[item].id;
    int k;
    for (k = 0; k < 40; k++) ed_del_ammo(e, id);
    ed_remove(e, (uint16_t)id, 99);
    memmove(d->items + item, d->items + item + 1, (size_t)(d->item_count + d->link_count - item - 1) * sizeof d->items[0]);
    d->item_count--;
}
static void ed_add_ammo(medit *e, int item)
{
    mek_def *d = &e->d;
    uint32_t id = d->items[item].id;
    if (d->item_count + d->link_count >= MEK_MAX_ITEMS || mek_weapons[id / 100].ammo_per_ton <= 0) return;
    if (d->link_count >= 25 || ed_ammo_bins(e, id) >= 10 || e->npool >= 78) return;
    d->items[d->item_count + d->link_count].id = 10000 + id;
    d->items[d->item_count + d->link_count].linked_id = (int32_t)id;
    d->link_count++;
    ed_pool_add(e, (uint16_t)(10000 + id), 1);
}
static int ed_removable(uint16_t id)   /* items the player may move: not the structure */
{
    return id && !(id >= 5300 && id < 6000 && !(id >= 5400 && id < 5500));
}
static int ed_toggle_actuator(medit *e, int arm, int hand)   /* arm: location 4 (right, 5401 / 5451) or 5 (left, 5402 / 5452) */
{   /* 0x300d0 / 0x30186: the lower arm always in slot 2, the hand in slot 3 - whatever is there goes to the unassigned
     * list; the two are independent (removing one leaves the other) */
    mek_loc *l = &e->d.loc[arm];
    uint16_t side = (uint16_t)(arm == 4 ? 1 : 2), want = (uint16_t)((hand ? 5450 : 5400) + side);
    int k, at = hand ? 3 : 2;
    for (k = 0; k < l->slot_count; k++) if (l->slots[k] == want) { l->slots[k] = 0; return 1; }
    if (at >= l->slot_count) return 0;
    if (l->slots[at]) {
        uint16_t was = l->slots[at];
        int n = 0;
        for (k = 0; k < l->slot_count; k++) if (l->slots[k] == was) { l->slots[k] = 0; n++; }
        ed_pool_add(e, was, n);
    }
    l->slots[at] = want;
    return 1;
}
static int ed_has_actuator(const medit *e, int arm, int hand)
{
    uint16_t id = (uint16_t)((hand ? 5450 : 5400) + (arm == 4 ? 1 : 2));
    int k;
    for (k = 0; k < e->d.loc[arm].slot_count; k++) if (e->d.loc[arm].slots[k] == id) return 1;
    return 0;
}
static int ed_single(uint16_t id);
/* assign the selected pool item into location loc (FUN_0002ae40): the whole item into the location's first free slots,
 * not necessarily together; ammo bins one at a time. Returns 1 placed, -1 not enough free slots, -2 jump jets outside
 * the torso / legs (0x2fbad) */
static int ed_assign(medit *e, int loc, int s)
{
    mek_loc *l = &e->d.loc[loc];
    uint16_t id;
    int n, k, need, nfree = 0;
    (void)s;
    if (e->sel_pool < 0 || e->sel_pool >= e->npool) return 0;
    id = e->pool[e->sel_pool];
    if (id >= 7000 && id < 8000 && (loc == 0 || loc == 4 || loc == 5)) return -2;
    for (n = 0, k = 0; k < e->npool; k++) if (e->pool[k] == id) n++;
    need = id >= 10000 ? 1 : n;
    for (k = 0; k < l->slot_count; k++) if (!l->slots[k]) nfree++;
    if (nfree < need) return -1;
    for (k = 0, n = 0; k < l->slot_count && n < need; k++) if (!l->slots[k]) { l->slots[k] = id; n++; }
    for (k = 0; k < need; k++) { int j; for (j = 0; j < e->npool; j++) if (e->pool[j] == id) { memmove(e->pool + j, e->pool + j + 1, (size_t)(e->npool - j - 1) * 2); e->npool--; break; } }
    if (e->sel_pool >= e->npool) e->sel_pool = e->npool - 1;
    return 1;
}
static int ed_single(uint16_t id) { return (id >= 7000 && id < 10000) || id >= 10000; }   /* jets, Endo, Ferro, ammo */
static void ed_unassign(medit *e, int loc, int s)
{
    uint16_t id = e->d.loc[loc].slots[s];
    int l, k;
    if (!ed_removable(id) || (id >= 5400 && id < 5500)) return;   /* structure; actuators belong to the EQUIPMENT page */
    if (ed_single(id)) { e->d.loc[loc].slots[s] = 0; ed_pool_add(e, id, 1); return; }
    for (l = 0; l < MEK_LOC_COUNT; l++) for (k = 0; k < e->d.loc[l].slot_count; k++)
        if (e->d.loc[l].slots[k] == id) { e->d.loc[l].slots[k] = 0; ed_pool_add(e, id, 1); }
}

/* ---- the CUSTOMIZE screen. Panels (clips held on their last frame): left wwomp1 (12,52); right short wwomp1es or tall
 * wwomp7cr (436,52) (the weapons table); middle tall wwomp4ar (armour, criticals) or wwomp5wa (weapons) at (216,52). The
 * armour diagram: SHP WWOMP4MP pieces head (291,221), right torso (262,258), centre torso (302,258), left torso (342,258),
 * right arm (226,256), left arm (378,256), right leg (255,362), left leg (325,362) - placed by template matching; the
 * selected location cross-hatched (a 10-pixel grid). Text: font 31, grey (ink -> palette 6) for information, white for
 * what can be clicked; right-panel values at x 544. */
static const int DIAG_LOC[8] = {0, 1, 2, 3, 4, 5, 6, 7};   /* piece p is location p (the shell's diagram) */
static const int DIAG_ORDER[8] = {4,5,6,7,1,3,2,0};
static int DIAG_FILL = 0;   /* the palette entry of (247,89,0): unselected locations (DOS capture, full armour; other levels ASSUMED the same) */
static const int DIAG_XY[8][2] = {{291, 223}, {262, 258}, {302, 258}, {342, 258}, {226, 256}, {378, 256}, {255, 362}, {325, 362}};
static smk *open_smk_held(const char *name)   /* a panel: its opening plays, then it stays on the last frame */
{
    smk *c = open_smk(name);
    if (c) { int n = smk_frames(c), i; for (i = 0; i < n; i++) smk_next(c); }
    return c;
}
static int g_cmx = -1, g_cmy = -1;   /* CUSTOMIZE: the pointer, for the hover highlights */
static int diag_hit(const uint8_t *shp, size_t len, int x, int y)
{
    int p;
    for (p = 0; p < 8; p++) {
        shp_frame fr;
        int hit = 0;
        if (shp_decode(shp, len, p, &fr) != 0) continue;
        if (x >= DIAG_XY[p][0] && y >= DIAG_XY[p][1] && x < DIAG_XY[p][0] + fr.w && y < DIAG_XY[p][1] + fr.h) hit = fr.mask[(y - DIAG_XY[p][1]) * fr.w + (x - DIAG_XY[p][0])];
        shp_frame_free(&fr);
        if (hit) return DIAG_LOC[p];
    }
    return -1;
}
static void diag_draw(const uint8_t *shp, size_t len, int selected)
{
    {
        int i2, best = 1 << 30;
        for (i2 = 0; i2 < 256; i2++) { int d2 = abs((int)g_bg.pal[i2][0] - 247) + abs((int)g_bg.pal[i2][1] - 89) + (int)g_bg.pal[i2][2]; if (d2 < best) { best = d2; DIAG_FILL = i2; } }
    }
    /* the pieces' art carries the cross-hatching: the selected location is drawn as it is, the others with the grid
     * lines filled in the piece's main colour (as the DOS captures show) */
    int p, x, y, o2;
    for (o2 = 0; o2 < 8; o2++) {
        shp_frame fr;
        int cnt[256], fill = 0, i;
        p = DIAG_ORDER[o2];
        if (shp_decode(shp, len, p, &fr) != 0) continue;
        memset(cnt, 0, sizeof cnt);
        for (i = 0; i < fr.w * fr.h; i++) if (fr.mask[i]) cnt[fr.pix[i]]++;
        for (i = 0; i < 256; i++) if (g_bg.pal[i][0] > g_bg.pal[i][1] + 40 && cnt[i] > cnt[fill]) fill = i;   /* the reddish majority */
        for (y = 0; y < fr.h; y++)
            for (x = 0; x < fr.w; x++) {
                int px = DIAG_XY[p][0] + x, py = DIAG_XY[p][1] + y;
                uint8_t ix;
                if (!fr.mask[y * fr.w + x] || px >= W || py >= H) continue;
                ix = fr.pix[y * fr.w + x];
                if (DIAG_LOC[p] != selected && (g_bg.pal[ix][1] >= g_bg.pal[ix][0] || ix == fill)) ix = (uint8_t)DIAG_FILL;   /* fill and grid -> (247,89,0) */
                memcpy(g_fb + ((size_t)py * W + (size_t)px) * 3, g_bg.pal[ix], 3);
            }
        shp_frame_free(&fr);
    }
}
static void dlg_g(int x, int y, const char *t) { draw_text(&g_fthin, x, y, t, (const uint8_t (*)[3])g_bg.pal, 6); }   /* information */
static void dlg_w(int x, int y, const char *t) { draw_text(&g_fthin, x, y, t, (const uint8_t (*)[3])g_bg.pal, -1); }  /* clickable */
static int shell_confirm(SDL_Renderer *rd, SDL_Texture *tx, int headless, const char *msg);
/* returns 1 if saved (out = the new loadout name), 0 on ABORT, -3 to quit */
static int run_customize(SDL_Renderer *rd, SDL_Texture *tx, int headless, int frames, int clan, int m, const char *loadout, char *out)
{
    static medit E;
    medit *e = &E;
    mek_def d0;
    smk *cl[5];
    clip *bt[6];
    int cx = CTX(clan);
    uint8_t *diag = NULL;
    size_t diaglen = 0;
    int result = -9, f = 0, i, nn = 1;
    char code[4], dn[64];
    FILE *fp;
    static const char *BCL[3][6] = {{"AWOGRID", "WWOBKG", "WWOSTAR", "WWOCN", "WWOCP", "WWOVN"}, {"AJFGRID", "WJFBKG", "WJFSTAR", "WJFCN", "WJFCP", "WJFVN"},
                                    {"AIAGRID", "WIABKG1", "WIASTAR", "WIACN", "WIACP", "WIAVN"}};
    static const int BXY[3][6][2] = {{{140, 310}, {219, 429}, {407, 410}, {290, 433}, {240, 435}, {300, 429}}, {{108, 306}, {216, 434}, {369, 416}, {286, 438}, {242, 438}, {296, 434}},
                                     {{108, 306}, {219, 414}, {413, 417}, {265, 430}, {238, 437}, {302, 431}}};
    if (load_mek(loadout, &d0) != 0) return 0;
    ed_init(e, &d0);
    snprintf(code, sizeof code, "%s", lance_mech_code(m));
    for (nn = 0; nn < 100; nn++) {   /* the next free user variant number: files 00..99, "User Variant #1".. (0x35051) */
        char nm[32];
        snprintf(nm, sizeof nm, "MEK\\%s%02dUSR.MEK", code, nn);
        if (dp_resolve(nm, dn, sizeof dn) != 0) break;
    }
    if (nn >= 100) return result;   /* all 100 used: CUSTOMIZE is refused */
    snprintf(dn, sizeof dn, "SMK\\WWOMP4MP.SHP");
    if ((fp = fopen(gpath(dn), "rb"))) {
        fseek(fp, 0, SEEK_END); diaglen = (size_t)ftell(fp); fseek(fp, 0, SEEK_SET);
        if ((diag = malloc(diaglen)) && fread(diag, 1, diaglen, fp) != diaglen) { free(diag); diag = NULL; }
        fclose(fp);
    }
    {
        const char *p = clan == 1 ? "WJF" : "WWO";
        static const char *SUF[5] = {"MP1", "MP1ES", "MP7CR", "MP4AR", "MP5WA"};
        for (i = 0; i < 5; i++) { char nm[16]; snprintf(nm, sizeof nm, "%s%s", p, SUF[i]); cl[i] = open_smk_held(nm); }
        for (i = 0; i < 6; i++) {   /* panels: the opening plays, then the last frame holds */
            bt[i] = (cx == 2 && i >= 2) ? NULL : clip_open(BCL[cx][i]);   /* context 2: the 0x24 stills stay hidden */
            if (bt[i] && bt[i]->s) { int n2 = smk_frames(bt[i]->s), q2; for (q2 = 0; q2 < n2; q2++) smk_next(bt[i]->s); }
        }
    }
    load_bg(clan == 1 ? 21 : clan >= 2 ? 9 : 14);
    if (getenv("MW2_LAB_PAGE")) e->page = atoi(getenv("MW2_LAB_PAGE"));   /* tests */
    {   /* tests: MW2_LAB_CLICKS="x,y;x,y;..." - left clicks fed through the screen's own input handling */
        const char *cs = getenv("MW2_LAB_CLICKS");
        while (cs && *cs) {
            int x2, y2;
            if (sscanf(cs, "%d,%d", &x2, &y2) == 2) {
                SDL_Event ev2;
                memset(&ev2, 0, sizeof ev2);
                ev2.type = SDL_MOUSEBUTTONDOWN; ev2.button.button = SDL_BUTTON_LEFT; ev2.button.x = x2; ev2.button.y = y2;
                SDL_PushEvent(&ev2);
            }
            cs = strchr(cs, ';');
            if (cs) cs++;
        }
    }
    while (result == -9) {
        SDL_Event ev;
        mek_mass mm;
        int pg = e->page;
        ed_masses(e, &mm);
        if (!headless || getenv("MW2_LAB_CLICKS")) {
            while (SDL_PollEvent(&ev)) {
                int x, y, row, k;
                pg = e->page;   /* every event: an earlier one may have changed the page */
                ed_masses(e, &mm);
                if (ev.type == SDL_QUIT) { result = -3; break; }
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) { result = 0; break; }
                if (ev.type == SDL_MOUSEMOTION) { g_cmx = ev.motion.x; g_cmy = ev.motion.y; }
                if (ev.type != SDL_MOUSEBUTTONDOWN || ev.button.button != SDL_BUTTON_LEFT) continue;
                x = ev.button.x; y = ev.button.y;
                if (x >= 50 && x <= 149 && y >= 445 && y <= 469) { result = 0; break; }                       /* ABORT */
                if (x >= 50 && x <= 149 && y >= 420 && y <= 444) {                                              /* SAVE */
                    if (e->npool > 0) shell_message(rd, tx, headless, "Invalid 'Mech specification:|Unassigned criticals detected.#Ok");
                    else if (mm.used > (double)e->d.tonnage + 1e-6) shell_message(rd, tx, headless, "Invalid 'Mech specification:|Chassis can not support|current mass.#Ok");
                    else if (nn >= 100) shell_message(rd, tx, headless, "Error: Too many mechs|of this variant to save.#Ok");
                    else {
                        static uint8_t buf[8192];
                        char path[1100];
                        size_t n;
                        memset(e->d.config_raw, 0, sizeof e->d.config_raw);
                        snprintf(e->d.config_name, sizeof e->d.config_name, "User Variant #%d", nn + 1);
                        n = mek_write(&e->d, buf, sizeof buf);
                        snprintf(path, sizeof path, "%s/MEK", g_dir);
                        mkdir(path, 0755);
                        snprintf(path, sizeof path, "%s/MEK/%s%02dusr.mek", g_dir, code, nn);
                        if (n && (fp = fopen(path, "wb")) && fwrite(buf, 1, n, fp) == n && fclose(fp) == 0) { snprintf(out, 16, "%s%02dusr", code, nn); result = 1; }
                        else shell_message(rd, tx, headless, "Error saving 'Mech.#Ok");
                    }
                    break;
                }
                if (x >= 14 && x <= 200 && y >= 108 && y < 218) {                                              /* the component rows */
                    static const int PAGE[10] = {0, -1, -1, 3, 4, 5, 6, 7, 8, 9};
                    row = (y - 108) / 11;
                    if (row >= 0 && row < 10 && PAGE[row] >= 0) e->page = PAGE[row];
                    continue;
                }
                if (x >= 14 && x <= 200 && y >= 260 && y < 273) { e->page = 10; continue; }                    /* Assign Criticals */
                row = (y - 86) / 11;                                                                            /* right-panel rows */
                if (x >= 440 && x <= 626) {
                    if (pg == 0) {
                        int r = ed_rating(e);
                        if (y >= 97 && y < 108) {   /* Type: XL <-> Std */
                            if (ed_is_xl(e)) { for (k = 0; k < 12; k++) { if (e->d.loc[MEK_RT].slots[k] == 5850) e->d.loc[MEK_RT].slots[k] = 0; if (e->d.loc[MEK_LT].slots[k] == 5850) e->d.loc[MEK_LT].slots[k] = 0; } }
                            else {   /* 0x2d84e: never refused - slots 0 and 1 of both side torsos become engine, whatever held them goes to the unassigned list */
                                int sides[2] = {MEK_RT, MEK_LT}, sd, q;
                                for (sd = 0; sd < 2; sd++) for (q = 0; q < 2 && q < e->d.loc[sides[sd]].slot_count; q++) {
                                    mek_loc *L = &e->d.loc[sides[sd]];
                                    uint16_t was = L->slots[q];
                                    if (was && was != 5850) { int kk, n = 0; for (kk = 0; kk < L->slot_count; kk++) if (L->slots[kk] == was) { L->slots[kk] = 0; n++; } ed_pool_add(e, was, n); }
                                    L->slots[q] = 5850;
                                }
                            }
                        }
                        if (y >= 163 && y < 174 && r + (int)e->d.tonnage <= 400) { e->d.walk_mp++; ed_sync_sinks(e); }          /* FASTER */
                        if (y >= 174 && y < 185 && e->d.walk_mp > 1) {   /* SLOWER (0x2d66e): the highest jump jets go with it */
                            e->d.walk_mp--;
                            while (e->d.jump_mp > e->d.walk_mp) { int kk; e->d.jump_mp--; for (kk = 98; kk >= 1; kk--) if (ed_count(e, (uint16_t)(7000 + kk))) { ed_remove(e, (uint16_t)(7000 + kk), 1); break; } }
                            ed_sync_sinks(e);
                        }
                    } else if (pg == 3) {
                        int per = e->dbl ? 2 : 1;
                        if (y >= 97 && y < 108) {   /* Type (0x2e450): single <-> double, the count kept, the external sinks re-queued */
                            int cnt = (int)e->d.heat_sinking / per;
                            for (k = 1; k < 99; k++) ed_remove(e, (uint16_t)(6000 + k), 9);
                            e->dbl = !e->dbl; e->d.heat_sinking = (uint32_t)(cnt * (e->dbl ? 2 : 1)); ed_sync_sinks(e);
                        }
                        if (y >= 130 && y < 141) { e->d.heat_sinking += (uint32_t)per; ed_sync_sinks(e); }                     /* ADD */
                        if (y >= 141 && y < 152 && (int)e->d.heat_sinking / per > 10) { e->d.heat_sinking -= (uint32_t)per; ed_sync_sinks(e); }   /* DELETE (10 at least) */
                    } else if (pg == 4) {
                        if (y >= 119 && y < 130 && e->d.jump_mp < e->d.walk_mp) { e->d.jump_mp++; ed_pool_add(e, ed_free_id(e, 7000), 1); }
                        if (y >= 130 && y < 141 && e->d.jump_mp > 0) { int kk; e->d.jump_mp--; for (kk = 98; kk >= 1; kk--) if (ed_count(e, (uint16_t)(7000 + kk))) { ed_remove(e, (uint16_t)(7000 + kk), 1); break; } }
                    } else if (pg == 5 && y >= 86 && y < 97) {                                                   /* Type: Std <-> Endo */
                        if (ed_endo(e)) { for (k = 1; k < 50; k++) ed_remove(e, (uint16_t)(8000 + k), 9); }
                        else for (k = 0; k < 7; k++) ed_pool_add(e, (uint16_t)(8001 + k), 1);
                    } else if (pg == 6) {
                        int loc = ARMOR_ORDER[e->armor_loc], alloc = ed_armor_alloc(e), torso = loc == MEK_RT || loc == MEK_LT || loc == MEK_CT;
                        mek_loc *L = &e->d.loc[loc];
                        if (y >= 119 && y < 130) {                                                               /* Type: Std <-> Ferro (0x2e900: mass kept, allocation trimmed) */
                            if (ed_ferro(e)) { for (k = 1; k < 50; k++) ed_remove(e, (uint16_t)(9000 + k), 9); }
                            else for (k = 0; k < 7; k++) ed_pool_add(e, (uint16_t)(9001 + k), 1);
                            ed_trim_armor(e);
                        }
                        if (y >= 141 && y < 152) e->halftons++;                                                  /* ADD (0x2e6b9): no cap */
                        if (y >= 152 && y < 163 && e->halftons > 0) { e->halftons--; ed_trim_armor(e); }          /* DELETE: half a ton, the allocation trimmed */
                        if (y >= 163 && y < 180) {   /* the up arrows (0x2fd90): a free point, else one moved over from the other face */
                            int full = (int)(L->armor + L->rear_armor) >= ed_loc_max(e, loc), nofree = alloc >= ed_factor(e);
                            if (!full && !nofree) { if (x < 566) L->armor++; else if (torso) L->rear_armor++; }
                            else if (torso) { if (x < 566 && L->rear_armor > 0) { L->rear_armor--; L->armor++; } else if (x >= 566 && L->armor > 0) { L->armor--; L->rear_armor++; } }
                        }
                        if (y >= 197 && y < 212) { if (x < 566 && L->armor > 0) L->armor--; else if (x >= 566 && torso && L->rear_armor > 0) L->rear_armor--; }
                    } else if ((pg == 7 || pg == 8)) {
                        for (k = 0; k < TABLE_N; k++) if (y >= TABLE_Y[k] && y < TABLE_Y[k] + 11) e->sel_table = k;
                    } else if (pg == 9) {
                        if (y >= 97 && y < 108) {                                                                /* MASC */
                            int t = masc_tons((int)e->d.tonnage);
                            if (e->masc) { ed_remove(e, 5001, 99); e->masc = 0; } else { ed_pool_add(e, 5001, t); e->masc = t; }
                        }
                        if (y >= 108 && y < 119) ed_toggle_actuator(e, 4, 0);
                        if (y >= 119 && y < 130) ed_toggle_actuator(e, 4, 1);
                        if (y >= 130 && y < 141) ed_toggle_actuator(e, 5, 0);
                        if (y >= 141 && y < 152) ed_toggle_actuator(e, 5, 1);
                    } else if (pg == 10) {
                        int g = (y - 86) / 11, seen = 0, j;
                        for (j = 0; j < e->npool; j++) {   /* the pool's groups, in order */
                            int first = 1, q;
                            for (q = 0; q < j; q++) if (e->pool[q] == e->pool[j]) first = 0;
                            if (!first) continue;
                            if (seen == g) { e->sel_pool = j; break; }
                            seen++;
                        }
                        /* MW2SHELL 0x2fb70: clicking an unassigned item puts it straight into the location picked on the
                         * diagram - its first free slots (0x2ae40; refused and undone when they run out) */
                        if (y >= 86 && e->sel_pool >= 0 && e->sel_pool < e->npool) {
                            int r2 = ed_assign(e, e->crit_loc, 0);
                            if (r2 == -1) shell_message(rd, tx, headless, "Insufficient criticals|for item placement.#Ok");
                            if (r2 == -2) shell_message(rd, tx, headless, "Jump Jets may only|be assigned to torso|or leg sections.#Ok");
                            e->sel_pool = -1;
                        }
                    }
                }
                if (x >= 218 && x <= 422) {   /* the middle panel */
                    if (pg == 6) {
                        if (y >= 86 && y < 174) e->armor_loc = (y - 86) / 11;
                        if (diag) { int l = diag_hit(diag, diaglen, x, y); if (l >= 0) { shell_sound(0x50, 0x28); for (k = 0; k < 8; k++) if (ARMOR_ORDER[k] == l) e->armor_loc = k; } }   /* 0x2fb50 */
                    } else if (pg == 7 || pg == 8) {
                        int nw = 0;
                        for (k = 0; k < e->d.item_count; k++) if (e->d.items[k].id < 5000) { if (y >= 86 + 11 * nw && y < 97 + 11 * nw) e->sel_fit = k; nw++; }
                        if (y >= 207 && y < 218 && x < 322 && e->sel_table >= 0) ed_add_weapon(e, TABLE_W[e->sel_table]);              /* ADD WEAPON */
                        if (y >= 207 && y < 218 && x >= 322 && e->sel_fit >= 0 && e->sel_fit < e->d.item_count) ed_add_ammo(e, e->sel_fit);   /* ADD AMMO */
                        if (y >= 218 && y < 229 && x < 322 && e->sel_fit >= 0 && e->sel_fit < e->d.item_count) { ed_del_weapon(e, e->sel_fit); e->sel_fit = -1; }   /* DELETE WEAPON */
                        if (y >= 218 && y < 229 && x >= 322 && e->sel_fit >= 0 && e->sel_fit < e->d.item_count) ed_del_ammo(e, e->d.items[e->sel_fit].id);          /* DELETE AMMO */
                    } else if (pg == 10) {
                        mek_loc *L = &e->d.loc[e->crit_loc];
                        int sl = (y - 79) / 11;
                        if (y >= 79 && sl < L->slot_count) {
                            uint16_t at = L->slots[sl];
                            if (at >= 5300 && at < 6000) shell_message(rd, tx, headless, "Selected critical|can not be removed.#Ok");
                            else if (at) ed_unassign(e, e->crit_loc, sl);   /* 0x2fc90: back to the unassigned list; an empty slot does nothing */
                        }
                        if (diag) { int l = diag_hit(diag, diaglen, x, y); if (l >= 0) { shell_sound(0x50, 0x28); e->crit_loc = l; } }   /* 0x2fb50 */
                    }
                }
            }
        }
        pg = e->page;
        ed_masses(e, &mm);
        draw_bg();
        g_clip_pal = !bt[0] || !bt[0]->s ? NULL : smk_palette(bt[0]->s);
        if (bt[0]) { clip_next(bt[0]); clip_blit(bt[0], BXY[cx][0][0], BXY[cx][0][1]); }   /* the grid animates */
        for (i = 1; i < 6; i++) if (bt[i]) clip_blit(bt[i], BXY[cx][i][0], BXY[cx][i][1]);
        g_clip_pal = NULL;
        if (cl[0]) blit_smk(cl[0], 12, 52);
        if (pg == 6 || pg == 10) { if (cl[3]) blit_smk(cl[3], 216, 52); if (diag) diag_draw(diag, diaglen, pg == 6 ? ARMOR_ORDER[e->armor_loc] : e->crit_loc); }
        if (pg == 7 || pg == 8) { if (cl[4]) blit_smk(cl[4], 216, 52); if (cl[2]) blit_smk(cl[2], 436, 52); }
        else if (pg == 10) { if (cl[2]) blit_smk(cl[2], 436, 52); }   /* UNASSIGNED CRITICALS: the tall panel */
        else if (cl[1]) blit_smk(cl[1], 436, 52);
        {   /* the header */
            char nm[48];
            int k;
            draw_text(&g_fhead, 320 - text_width(&g_fhead, "CUSTOMIZING") / 2, 4, "CUSTOMIZING", (const uint8_t (*)[3])g_bg.pal, -1);
            snprintf(nm, sizeof nm, "%s", lance_mech_name(m));
            for (k = 0; nm[k]; k++) if (nm[k] >= 'a' && nm[k] <= 'z') nm[k] = (char)(nm[k] - 32);
            draw_text(&g_fhead, 320 - text_width(&g_fhead, nm) / 2, 28, nm, (const uint8_t (*)[3])g_bg.pal, -1);
        }
        {   /* the left panel: the component table, the editable rows white */
            static const char *ROW[10] = {"Engine", "Gyro", "Cockpit", "Heat Sinks", "Jump Jets", "Internal", "Armor", "Weapons", "Ammo", "Equipment"};
            double v[10];
            char t[64], note[32];
            v[0] = mm.engine; v[1] = mm.gyro; v[2] = mm.cockpit; v[3] = mm.sinks; v[4] = mm.jets; v[5] = mm.internal; v[6] = mm.armor; v[7] = mm.weapons; v[8] = mm.ammo; v[9] = mm.equip;
            dlg_g(20, 64, "Variant:");
            snprintf(t, sizeof t, "User Variant #%d", nn + 1); dlg_w(65, 64, t);
            dlg_g(20, 86, "COMPONENT"); dlg_g(98, 86, "MASS"); dlg_g(147, 86, "NOTES");
            for (i = 0; i < 10; i++) {
                int y = 108 + 11 * i;
                if (i == 1 || i == 2) dlg_g(20, y, ROW[i]); else dlg_w(20, y, ROW[i]);
                snprintf(t, sizeof t, "%.2f T", v[i]); dlg_g(98, y, t);
                note[0] = 0;
                if (i == 0) snprintf(note, sizeof note, "%d%s", mm.rating, ed_is_xl(e) ? "XL" : "");
                if (i == 3) snprintf(note, sizeof note, "%d (%d)", mm.nsinks, mm.dissip);
                if (i == 4) snprintf(note, sizeof note, "%d", mm.njets);
                if (i == 5) snprintf(note, sizeof note, "%s", ed_endo(e) ? "Endo" : "Std");
                if (i == 6) snprintf(note, sizeof note, "%s", ed_ferro(e) ? "Ferro-F" : "Std");
                if (note[0]) dlg_g(147, y, note);
            }
            dlg_g(20, 229, "Used Mass"); snprintf(t, sizeof t, "%.2f T", mm.used); dlg_g(98, 229, t);
            dlg_g(20, 240, "Max Mass"); snprintf(t, sizeof t, "%.2f T", (double)e->d.tonnage); dlg_g(98, 240, t);
            dlg_w(20, 262, "Assign Criticals");
        }
        {   /* the pages */
            char t[96];
            int k;
            if (pg == 0) {
                double walk = e->d.walk_mp * 10.8, run = ceil(e->d.walk_mp * 1.5) * 10.8;
                dlg_g(444, 64, "ENGINE");
                dlg_g(444, 86, "Rating"); snprintf(t, sizeof t, "%d%s", ed_rating(e), ed_is_xl(e) ? "XL" : ""); dlg_g(544, 86, t);
                dlg_g(444, 97, "Type"); dlg_w(544, 97, ed_is_xl(e) ? "XL" : "Std");
                dlg_g(444, 108, "Manufactur"); dlg_g(544, 108, "Vlar");
                dlg_g(444, 119, "Mass"); snprintf(t, sizeof t, "%.2f T", mm.engine); dlg_g(544, 119, t);
                dlg_g(444, 130, "Walking Speed"); snprintf(t, sizeof t, "%.1f kph", walk); dlg_g(544, 130, t);
                dlg_g(444, 141, "Running Speed"); snprintf(t, sizeof t, "%.1f kph", run); dlg_g(544, 141, t);
                dlg_w(444, 163, "FASTER"); dlg_w(444, 174, "SLOWER");
            } else if (pg == 3) {
                dlg_g(444, 64, "HEAT SINKS");
                dlg_g(444, 86, "Count"); snprintf(t, sizeof t, "%d (%d)", mm.nsinks, mm.dissip); dlg_g(544, 86, t);
                dlg_g(444, 97, "Type"); dlg_w(544, 97, e->dbl ? "Double" : "Single");
                dlg_g(444, 108, "Mass"); snprintf(t, sizeof t, "%.2f T", mm.sinks); dlg_g(544, 108, t);
                dlg_w(444, 130, "ADD"); dlg_w(444, 141, "DELETE");
            } else if (pg == 4) {
                dlg_g(444, 64, "JUMP JETS");
                dlg_g(444, 86, "Count"); snprintf(t, sizeof t, "%d", mm.njets); dlg_g(544, 86, t);
                dlg_g(444, 97, "Mass"); snprintf(t, sizeof t, "%.2f T", mm.jets); dlg_g(544, 97, t);
                dlg_w(444, 119, "ADD"); dlg_w(444, 130, "DELETE");
            } else if (pg == 5) {
                dlg_g(444, 64, "INTERNAL STRUCTURE");
                dlg_g(444, 86, "Type"); dlg_w(544, 86, ed_endo(e) ? "Endo" : "Std");
                dlg_g(444, 97, "Mass"); snprintf(t, sizeof t, "%.2f T", mm.internal); dlg_g(544, 97, t);
                for (k = 0; k < 8; k++) { dlg_g(444, 119 + 11 * k, ARMOR_NAME[k]); snprintf(t, sizeof t, "%u", e->d.loc[INTERNAL_ORDER[k]].internal); dlg_g(544, 119 + 11 * k, t); }
            } else if (pg == 6) {
                int loc = ARMOR_ORDER[e->armor_loc], torso = loc == MEK_RT || loc == MEK_LT || loc == MEK_CT;
                dlg_g(224, 64, "ARMOR ALLOCATION");
                for (k = 0; k < 8; k++) {
                    const mek_loc *L = &e->d.loc[ARMOR_ORDER[k]];
                    int tor = ARMOR_ORDER[k] == MEK_RT || ARMOR_ORDER[k] == MEK_LT || ARMOR_ORDER[k] == MEK_CT;
                    snprintf(t, sizeof t, "%s (%d)", ARMOR_NAME[k], ed_loc_max(e, ARMOR_ORDER[k])); dlg_g(224, 86 + 11 * k, t);
                    if (tor) snprintf(t, sizeof t, "%u/%u", L->armor, L->rear_armor); else snprintf(t, sizeof t, "%u", L->armor);
                    dlg_g(344 - text_width(&g_fthin, t) / 2, 86 + 11 * k, t);   /* centred at x 344.5 (DOS) */
                }
                dlg_g(444, 64, "ARMOR");
                dlg_g(444, 86, "Factor"); snprintf(t, sizeof t, "%d", ed_factor(e)); dlg_g(544, 86, t);
                dlg_g(444, 97, "Allocated"); snprintf(t, sizeof t, "%d", ed_armor_alloc(e)); dlg_g(544, 97, t);
                dlg_g(444, 108, "Mass"); snprintf(t, sizeof t, "%.2f T", mm.armor); dlg_g(544, 108, t);
                dlg_g(444, 119, "Type"); dlg_w(544, 119, ed_ferro(e) ? "Ferro-F" : "Std");
                dlg_w(444, 141, "ADD"); dlg_w(444, 152, "DELETE");
                dlg_w(551, 168, "\x01"); dlg_w(581, 168, "\x01");   /* font 31: 1 = up arrow, 2 = down arrow */
                snprintf(t, sizeof t, "%s (%d)", ARMOR_NAME[e->armor_loc], ed_loc_max(e, loc)); dlg_w(444, 185, t);
                snprintf(t, sizeof t, "%u", e->d.loc[loc].armor); dlg_g(551, 185, t);
                if (torso) { snprintf(t, sizeof t, "%u", e->d.loc[loc].rear_armor); dlg_g(580, 185, t); } else dlg_g(580, 185, "--");
                dlg_w(551, 202, "\x02"); dlg_w(581, 202, "\x02");
            } else if (pg == 7 || pg == 8) {
                int nw = 0, inst, j, ammo;
                const mek_weapon *wi = NULL;
                dlg_g(224, 64, "WEAPONS AND AMMO");
                for (k = 0; k < e->d.item_count && nw < 10; k++) {
                    int w = (int)(e->d.items[k].id / 100);
                    if (e->d.items[k].id >= 5000 || w >= mek_weapon_count) continue;
                    for (inst = 0, j = 0; j <= k; j++) if (e->d.items[j].id < 5000 && (int)(e->d.items[j].id / 100) == w) inst++;
                    for (ammo = 0, j = e->d.item_count; j < e->d.item_count + e->d.link_count; j++) if (e->d.items[j].linked_id == (int32_t)e->d.items[k].id) ammo++;
                    if (ammo) snprintf(t, sizeof t, "%s #%d (ammo %dT/%d)", mek_weapons[w].name, inst, ammo, ammo * mek_weapons[w].ammo_per_ton);
                    else snprintf(t, sizeof t, "%s #%d", mek_weapons[w].name, inst);
                    if (k == e->sel_fit) dlg_w(224, 86 + 11 * nw, t); else dlg_g(224, 86 + 11 * nw, t);
                    nw++;
                }
                for (; nw < 10; nw++) dlg_g(224, 86 + 11 * nw, "-");
                dlg_w(224, 207, "ADD WEAPON"); dlg_w(324, 207, "ADD AMMO"); dlg_w(224, 218, "DELETE WEAPON"); dlg_w(324, 218, "DELETE AMMO");
                dlg_g(224, 240, "WEAPON INFO");
                if (e->sel_table >= 0) wi = &mek_weapons[TABLE_W[e->sel_table]];
                else if (e->sel_fit >= 0 && e->sel_fit < e->d.item_count) wi = &mek_weapons[e->d.items[e->sel_fit].id / 100];
                dlg_g(224, 262, "Type"); if (wi) dlg_g(274, 262, wi->name);
                dlg_g(224, 284, "Heat"); dlg_g(324, 284, "Mass"); dlg_g(224, 295, "Damage"); dlg_g(324, 295, "Crit"); dlg_g(224, 306, "Range"); dlg_g(324, 306, "Ammo");
                if (wi) {
                    snprintf(t, sizeof t, "%d", wi->heat); dlg_g(274, 284, t);
                    snprintf(t, sizeof t, "%.2f T", wi->weight_x100 / 100.0); dlg_g(374, 284, t);
                    if (wi->damage < 0) snprintf(t, sizeof t, "%d / msl", -wi->damage); else snprintf(t, sizeof t, "%d", wi->damage); dlg_g(274, 295, t);
                    snprintf(t, sizeof t, "%d", wi->crits); dlg_g(374, 295, t);
                    snprintf(t, sizeof t, "%d m", wi->max_range_m); dlg_g(274, 306, t);
                    if (wi->ammo_per_ton) snprintf(t, sizeof t, "%d / T", wi->ammo_per_ton); else snprintf(t, sizeof t, "-"); dlg_g(374, 306, t);
                }
                dlg_g(444, 64, "WEAPONS TABLE");
                for (k = 0; k < TABLE_N; k++) { if (k == e->sel_table) dlg_w(444, TABLE_Y[k], mek_weapons[TABLE_W[k]].name); else dlg_g(444, TABLE_Y[k], mek_weapons[TABLE_W[k]].name); }
            } else if (pg == 9) {
                static const char *EQ[5] = {"MASC", "Right Lower Arm Actuator", "Right Hand Actuator", "Left Lower Arm Actuator", "Left Hand Actuator"};
                int on[5];
                on[0] = e->masc > 0; on[1] = ed_has_actuator(e, 4, 0); on[2] = ed_has_actuator(e, 4, 1); on[3] = ed_has_actuator(e, 5, 0); on[4] = ed_has_actuator(e, 5, 1);   /* rows: Right (index 4: 5401 / 5451), Left (5) */
                dlg_g(444, 64, "EQUIPMENT");
                dlg_g(444, 86, "Yes"); dlg_g(474, 86, "CASE");
                for (k = 0; k < 5; k++) { dlg_w(444, 97 + 11 * k, on[k] ? "Yes" : "No"); dlg_g(474, 97 + 11 * k, EQ[k]); }
            } else if (pg == 10) {
                const mek_loc *L = &e->d.loc[e->crit_loc];
                char nb[64];
                int g = 0, j;
                dlg_w(224, 64, LOC_NAME[e->crit_loc]);
                for (k = 0; k < L->slot_count; k++) dlg_g(224, 79 + 11 * k, L->slots[k] ? mek_item_name(L->slots[k], nb, sizeof nb) : "-");
                dlg_g(444, 64, "UNASSIGNED CRITICALS");
                for (j = 0; j < e->npool; j++) {
                    int first = 1, q, cnt = 0;
                    for (q = 0; q < j; q++) if (e->pool[q] == e->pool[j]) first = 0;
                    if (!first) continue;
                    for (q = 0; q < e->npool; q++) if (e->pool[q] == e->pool[j]) cnt++;
                    snprintf(t, sizeof t, "%s (%d)", mek_item_name(e->pool[j], nb, sizeof nb), cnt);
                    if (g_cmx >= 440 && g_cmx <= 626 && g_cmy >= 86 + 11 * g && g_cmy < 97 + 11 * g) dlg_w(444, 86 + 11 * g, t); else dlg_g(444, 86 + 11 * g, t);   /* the item under the pointer: a click assigns it */
                    g++;
                }
            }
        }
        {   /* SAVE / ABORT light up under the pointer, as the lab's labels */
            int hs = g_cmx >= 50 && g_cmx <= 149 && g_cmy >= 420 && g_cmy <= 444, ha = g_cmx >= 50 && g_cmx <= 149 && g_cmy >= 445 && g_cmy <= 469;
            draw_text(&g_flabel, 100 - text_width(&g_flabel, "SAVE") / 2, 425, "SAVE", (const uint8_t (*)[3])g_bg.pal, hs ? -1 : 6);
            draw_text(&g_flabel, 100 - text_width(&g_flabel, "ABORT") / 2, 450, "ABORT", (const uint8_t (*)[3])g_bg.pal, ha ? -1 : 6);
        }
        f++;
        if (headless) { if (f >= frames) result = -2; }
        else {
            SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
            SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
            SDL_Delay(15);
        }
    }
    for (i = 0; i < 5; i++) smk_close(cl[i]);
    for (i = 0; i < 6; i++) clip_close(bt[i]);
    free(diag);
    return result;
}
/* Yes / No: 1 for Yes */
static int shell_confirm(SDL_Renderer *rd, SDL_Texture *tx, int headless, const char *msg)
{
    char m2[220];
    snprintf(m2, sizeof m2, "%s%s", msg, strchr(msg, '#') ? "" : "#Yes|No");
    return shell_dialog(rd, tx, headless, m2, 1) == 0;
}

/* ---- the Mech Lab (screen 9, FUN_000339e0), main page; laid out from a DOS capture: header (label font, centred)
 * "CALLSIGN: <pilot> (<KDMT>.00 T MAX)" at y 4 and the chassis at y 27; left panel wwomp1 (12,52): "Variant: <config>"
 * (21,63), COMPONENT / MASS / NOTES at y 86 (x 21 / 99 / 148), ten rows from y 108 every 11, Used / Max Mass at 230 / 241;
 * right panel wwomp1es (436,52): WEAPONS AND AMMO (445,64), ten lines from y 87 ("-" when empty), ammo inline
 * "(ammo <t>T/<shots>)"; the turntable SHP AWOMP<code> (30 frames, 13 / s, advancing) at origin (175,127) plus each
 * frame's offset; hotspots 0x7f26c (EXIT LAB, STAR CONFIG, NEXT / PREV CHASSIS, NEXT / PREV VARIANT, CUSTOMIZE, ACCEPT
 * MECH, SAVE / ABORT / DELETE in customise mode). The weapon editor (CUSTOMIZE) NOT YET. */
static int run_mech_lab(SDL_Renderer *rd, SDL_Texture *tx, int headless, int frames, int clan, int slot, int *mx, int *my)
{
    lance_slot *sl = &g_star[clan].slot[slot];
    int next = -1, f = 0, m = sl->mech >= 0 ? sl->mech : (clan == 1 ? 5 : 9), nv = 0, vi = 0, i, kdmt = clan >= 2 ? g_ia.tons[clan - 2] : mech_lab_kdmt(clan), reg;
    char var[24][9], pilot[24] = "MechWarrior";
    uint8_t *shp = NULL;
    size_t shplen = 0;
    int shpn = 0, shp_m = -1;
    Uint32 t0 = SDL_GetTicks();
    clip *cl[9];
    int cx = CTX(clan);
    static const char *CL[3][9] = {{"AWOGRID", "WWOMP1", "WWOMP1ES", "WWOBKG", "WWOSTAR", "WWOCN", "WWOCP", "WWOVN", "WWOVP"},
                                   {"AJFGRID", "WJFMP1", "WJFMP1ES", "WJFBKG", "WJFSTAR", "WJFCN", "WJFCP", "WJFVN", "WJFVP"},
                                   {"AIAGRID", "WIAMP1", "WIAMP1ES", "WIABKG1", "WIASTAR", "WIACN", "WIACP", "WIAVN", "WIAVP"}};
    static const int CXY[3][9][2] = {{{140, 310}, {12, 52}, {436, 52}, {219, 429}, {407, 410}, {290, 433}, {240, 435}, {300, 429}, {220, 432}},
                                     {{108, 306}, {12, 52}, {436, 52}, {216, 434}, {369, 416}, {286, 438}, {242, 438}, {296, 434}, {220, 437}},
                                     {{108, 306}, {12, 52}, {436, 52}, {219, 414}, {413, 417}, {265, 430}, {238, 437}, {302, 431}, {223, 437}}};   /* 0x339e0 context 2 */
    static const int HS[11][6] = {{50, 445, 149, 469, 100, 450}, {404, 414, 474, 474, 440, 460}, {303, 425, 330, 469, 300, 465}, {200, 425, 236, 469, 240, 465},
                                  {263, 425, 302, 469, 280, 465}, {237, 425, 262, 469, 260, 465}, {50, 420, 149, 444, 100, 425}, {50, 395, 149, 419, 100, 400},
                                  {50, 420, 149, 444, 100, 425}, {50, 445, 149, 469, 100, 450}, {490, 445, 589, 469, 540, 450}};
    static const char *HL[11] = {"EXIT LAB", "STAR CONFIG", "NEXT CHASSIS", "PREV CHASSIS", "NEXT VARIANT", "PREV VARIANT", "CUSTOMIZE", "ACCEPT MECH", "SAVE", "ABORT", "DELETE"};
    char want[16] = "";
    reg_load();
    reg = clan < 2 ? reg_selected(clan) : -1;
    if (reg >= 0) snprintf(pilot, sizeof pilot, "%s", g_reg[reg].name);
    if (clan >= 2) snprintf(pilot, sizeof pilot, "%s", ia_pilot());
    snprintf(want, sizeof want, "%s", sl->loadout);
    if (sl->mech < 0 && reg >= 0 && g_reg[reg].rank >= 0 && g_reg[reg].rank < 16) {   /* nothing chosen: the mission's own 'Mech */
        mission_star ms = mission_sdsc(CAMPAIGN_SCEN[clan][g_reg[reg].rank]);
        if (ms.ok && ms.loadout[0][0] && mech_index_of(ms.loadout[0]) >= 0) { m = mech_index_of(ms.loadout[0]); snprintf(want, sizeof want, "%s", ms.loadout[0]); }
    }
    nv = mek_variants(lance_mech_code(m), var, 24);
    for (i = 0; i < nv; i++) if (strcasecmp(var[i], want) == 0) vi = i;
    load_bg(clan == 1 ? 21 : clan >= 2 ? 9 : 14);
    for (i = 0; i < 9; i++) cl[i] = (cx == 2 && i >= 4) ? NULL : clip_open(CL[cx][i]);   /* context 2: WIASTAR .. WIAVP flags 0x24, hidden */
    if (!headless) shell_sound(LAB_VOICE[m], 0x28);   /* the chassis' name on entering (0x34441) */
    while (next == -1) {
        SDL_Event e;
        const uint8_t (*pal)[3] = (const uint8_t (*)[3])g_bg.pal;
        mek_def d;
        mek_mass mm;
        int have = nv > 0 && load_mek(var[vi], &d) == 0, hover = -1;
        if (have) mek_masses(&d, &mm);
        if (shp_m != m) {   /* the turntable for this chassis */
            char dn[64];
            FILE *fp;
            free(shp); shp = NULL; shpn = 0; shp_m = m;
            snprintf(dn, sizeof dn, "SMK\\%s%s.SHP", clan == 1 ? "AJFMP" : clan >= 2 ? "AIAMP" : "AWOMP", MECH_PIC[m]);
            if ((fp = fopen(gpath(dn), "rb"))) {
                fseek(fp, 0, SEEK_END); shplen = (size_t)ftell(fp); fseek(fp, 0, SEEK_SET);
                if ((shp = malloc(shplen)) && fread(shp, 1, shplen, fp) == shplen) shpn = shp_count(shp, shplen);
                fclose(fp);
            }
        }
        if (!headless) {
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) { next = -3; break; }
                if (e.type == SDL_MOUSEMOTION) { *mx = e.motion.x; *my = e.motion.y; }
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) next = clan >= 2 ? 7 : 11;   /* as EXIT LAB */
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    int x = e.button.x, y = e.button.y, h = -1;
                    for (i = 0; i < 8; i++) if (x >= HS[i][0] && x <= HS[i][2] && y >= HS[i][1] && y <= HS[i][3]) h = i;
                    if (x >= HS[10][0] && x <= HS[10][2] && y >= HS[10][1] && y <= HS[10][3] && nv && strstr(var[vi], "usr")) {   /* DELETE (user designs) */
                        if (shell_confirm(rd, tx, headless, "Delete this 'Mech?|Are you sure?#")) {
                            char path[1100];
                            snprintf(path, sizeof path, "MEK\\%s.MEK", var[vi]);
                            if (dp_resolve(path, path, sizeof path) == 0) remove(path);
                            nv = mek_variants(lance_mech_code(m), var, 24); vi = 0;
                        }
                        load_bg(clan == 1 ? 21 : clan >= 2 ? 9 : 14);
                        continue;
                    }
                    if (h == 0) next = clan >= 2 ? 7 : 11;                                                          /* EXIT LAB (case 0: 0xb) */
                    else if (h == 1) { shell_sound(0x66, 0x32); next = 13; }                                        /* STAR CONFIG (0xd) */
                    else if (h == 2 || h == 3) {                                                                     /* chassis: the 15 clan 'Mechs */
                        shell_sound_stop(LAB_VOICE[m]);                                                              /* the last name stops (0x3a400) */
                        m = (m + (h == 2 ? 1 : 14)) % 15;
                        nv = mek_variants(lance_mech_code(m), var, 24); vi = 0;
                        shell_sound(LAB_VOICE[m], 0x28);
                    } else if ((h == 4 || h == 5) && nv) { shell_sound(0x65, 0x32); vi = (vi + (h == 4 ? 1 : nv - 1)) % nv; }   /* variant */
                    else if (h == 6 && have) {                                                                         /* CUSTOMIZE */
                        char saved[16] = "";
                        int r = run_customize(rd, tx, headless, frames, clan, m, var[vi], saved);
                        if (r == -3) { next = -3; break; }
                        nv = mek_variants(lance_mech_code(m), var, 24);
                        if (r == 1) for (i = 0; i < nv; i++) if (strcasecmp(var[i], saved) == 0) vi = i;
                        load_bg(clan == 1 ? 21 : clan >= 2 ? 9 : 14);
                    }
                    else if (h == 7 && have) {                                                                       /* ACCEPT MECH */
                        if ((int)d.tonnage > kdmt) shell_message(rd, tx, headless, "'Mech exceeds|Keshik Defined Maximum Tonnage (KDMT)|for mission.#Ok");
                        else { sl->mech = m; snprintf(sl->loadout, sizeof sl->loadout, "%s", var[vi]); star_save(clan); next = g_from_screen == 13 || g_from_screen == 7 || g_from_screen == 11 ? g_from_screen : (clan >= 2 ? 7 : 11); }   /* back where it came from (case 7) */
                    }
                }
            }
        }
        draw_bg();
        g_clip_pal = !cl[0] || !cl[0]->s ? NULL : smk_palette(cl[0]->s);
        for (i = 0; i < 9; i++) if (cl[i]) { clip_next(cl[i]); clip_blit(cl[i], CXY[cx][i][0], CXY[cx][i][1]); }
        g_clip_pal = NULL;
        if (shp && shpn > 0) {   /* the turntable: 13 frames a second, advancing */
            shp_frame fr;
            int fi = (int)(((headless ? (Uint32)f * 77u : SDL_GetTicks() - t0) / 77u) % (Uint32)shpn);
            if (shp_decode(shp, shplen, fi, &fr) == 0) {
                int x, y;
                for (y = 0; y < fr.h; y++)
                    for (x = 0; x < fr.w; x++) {
                        int px = 175 + fr.left + x, py = 127 + fr.top + y;
                        if (!fr.mask[y * fr.w + x] || px < 0 || py < 0 || px >= W || py >= H) continue;
                        memcpy(g_fb + ((size_t)py * W + (size_t)px) * 3, g_bg.pal[fr.pix[y * fr.w + x]], 3);
                    }
                shp_frame_free(&fr);
            }
        }
        {   /* the header */
            char t[96], nm[48];
            int k;
            snprintf(t, sizeof t, "CALLSIGN: %s (%d.00 T MAX)", pilot, kdmt);
            for (k = 0; t[k]; k++) if (t[k] >= 'a' && t[k] <= 'z') t[k] = (char)(t[k] - 32);
            /* context 2: the header is grey 170 (palette entry 6) where the clans' screens show entry 7 (153): DOS
             * capture; the mechanism (a palette change by the context's clips) is not traced, ASSUMED */
            static uint8_t ipal[256][3];
            const uint8_t (*tpal)[3] = pal;
            if (cx == 2) { memcpy(ipal, pal, sizeof ipal); memcpy(ipal[7], pal[6], 3); tpal = (const uint8_t (*)[3])ipal; }
            draw_text(&g_fhead, 320 - text_width(&g_fhead, t) / 2, 4, t, tpal, -1);
            snprintf(nm, sizeof nm, "%s", lance_mech_name(m));
            for (k = 0; nm[k]; k++) if (nm[k] >= 'a' && nm[k] <= 'z') nm[k] = (char)(nm[k] - 32);
            draw_text(&g_fhead, 320 - text_width(&g_fhead, nm) / 2, 28, nm, tpal, -1);
        }
        if (have) {   /* the left panel: the component table */
            static const char *ROW[10] = {"Engine", "Gyro", "Cockpit", "Heat Sinks", "Jump Jets", "Internal", "Armor", "Weapons", "Ammo", "Equipment"};
            double v[10];
            char t[64], note[32];
            v[0] = mm.engine; v[1] = mm.gyro; v[2] = mm.cockpit; v[3] = mm.sinks; v[4] = mm.jets; v[5] = mm.internal; v[6] = mm.armor; v[7] = mm.weapons; v[8] = mm.ammo; v[9] = mm.equip;
            draw_text(&g_fthin, 20, 64, "Variant:", pal, 6);   /* label and name drawn separately: the name at x 65 */
            snprintf(t, sizeof t, "%s", d.config_name[0] ? d.config_name : var[vi]);
            draw_text(&g_fthin, 65, 64, t, pal, 6);
            draw_text(&g_fthin, 20, 86, "COMPONENT", pal, 6); draw_text(&g_fthin, 98, 86, "MASS", pal, 6); draw_text(&g_fthin, 147, 86, "NOTES", pal, 6);
            for (i = 0; i < 10; i++) {
                int y = 108 + 11 * i;
                draw_text(&g_fthin, 20, y, ROW[i], pal, 6);
                snprintf(t, sizeof t, "%.2f T", v[i]); draw_text(&g_fthin, 98, y, t, pal, 6);
                note[0] = 0;
                if (i == 0) snprintf(note, sizeof note, "%d%s", mm.rating, mm.xl ? "XL" : "");
                if (i == 3) snprintf(note, sizeof note, "%d (%d)", mm.nsinks, mm.dissip);
                if (i == 4) snprintf(note, sizeof note, "%d", mm.njets);
                if (i == 5) snprintf(note, sizeof note, "%s", mm.endo ? "Endo" : "Std");
                if (i == 6) snprintf(note, sizeof note, "%s", mm.ferro ? "Ferro-F" : "Std");
                if (note[0]) draw_text(&g_fthin, 147, y, note, pal, 6);
            }
            draw_text(&g_fthin, 20, 229, "Used Mass", pal, 6); snprintf(t, sizeof t, "%.2f T", mm.used); draw_text(&g_fthin, 98, 229, t, pal, 6);
            draw_text(&g_fthin, 20, 240, "Max Mass", pal, 6); snprintf(t, sizeof t, "%.2f T", (double)d.tonnage); draw_text(&g_fthin, 98, 240, t, pal, 6);
        }
        {   /* the right panel: weapons and ammo */
            int line = 0, k, n2;
            draw_text(&g_fthin, 444, 64, "WEAPONS AND AMMO", pal, 6);
            if (have)
                for (k = 0; k < d.item_count && line < 10; k++) {
                    char t[96], nb[64];
                    int w = (int)(d.items[k].id / 100), inst = 0, ammo = 0, j;
                    if (d.items[k].id >= 5000 || w >= mek_weapon_count) continue;
                    for (j = 0; j <= k; j++) if (d.items[j].id < 5000 && (int)(d.items[j].id / 100) == w) inst++;
                    for (j = d.item_count; j < d.item_count + d.link_count; j++) if (d.items[j].linked_id == (int32_t)d.items[k].id) ammo++;
                    (void)nb;
                    if (ammo) snprintf(t, sizeof t, "%s #%d (ammo %dT/%d)", mek_weapons[w].name, inst, ammo, ammo * mek_weapons[w].ammo_per_ton);
                    else snprintf(t, sizeof t, "%s #%d", mek_weapons[w].name, inst);
                    draw_text(&g_fthin, 444, 86 + 11 * line++, t, pal, 6);
                }
            for (n2 = line; n2 < 10; n2++) draw_text(&g_fthin, 444, 86 + 11 * n2, "-", pal, 6);
        }
        for (i = 0; i < 11; i++) if (i != 8 && i != 9 && *mx >= HS[i][0] && *mx <= HS[i][2] && *my >= HS[i][1] && *my <= HS[i][3]) hover = i;
        /* the always-shown labels light up under the pointer (the font's own colours, as the other screens' hotspots) */
        for (i = 6; i <= 7; i++) draw_text(&g_flabel, HS[i][4] - text_width(&g_flabel, HL[i]) / 2, HS[i][5], HL[i], pal, hover == i ? -1 : 6);   /* "<~" labels */
        if (nv && strstr(var[vi], "usr")) draw_text(&g_flabel, HS[10][4] - text_width(&g_flabel, HL[10]) / 2, HS[10][5], HL[10], pal, hover == 10 ? -1 : 6);   /* DELETE: user designs */
        draw_text(&g_flabel, HS[0][4] - text_width(&g_flabel, HL[0]) / 2, HS[0][5], HL[0], pal, hover == 0 ? -1 : 6);
        if (hover > 5) hover = -1;
        if (hover > 0) draw_text(&g_flabel, HS[hover][4] - text_width(&g_flabel, HL[hover]) / 2, HS[hover][5], HL[hover], pal, 6);
        f++;
        if (headless) { if (f >= frames) next = -2; }
        else {
            SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
            SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
            SDL_Delay(15);
        }
    }
    for (i = 0; i < 9; i++) clip_close(cl[i]);
    shell_sound_stop(LAB_VOICE[m]);   /* leaving: the lab's sounds stop (0x35ba7) */
    free(shp);
    return next;
}

/* Star Configuration: hotspots 0x7f11c + clan x 16 (9; background slot 15 / 22): 0 EXIT CONFIG -> 11, 1 MECH LAB -> 9,
 * 2 / 3 NEXT / PREV FORMATION (0-5, wraps), 4 ADD / 5 DELETE STARMATE (1..3), 6-8 CHANGE MECH (a bay) -> 9;
 * clips grid (140,310), bays awostr1-3 (73,158) (305,125) (500,181), buttons wwobkg (219,429) wwodsgn (407,410)
 * wwocn (290,433) wwocp (240,435) wwovn (300,429) wwovp (220,432); wingman names at (81,166) (313,133) (508,189) */
static const int SC_HS[3][9][6] = {
    {{50, 445, 149, 469, 100, 450}, {404, 414, 474, 474, 440, 460}, {263, 425, 302, 469, 280, 465}, {237, 425, 262, 469, 260, 465},
     {303, 425, 330, 469, 300, 465}, {200, 425, 236, 469, 240, 465}, {159, 195, 187, 229, 73, 234}, {391, 162, 419, 196, 305, 201}, {586, 218, 614, 252, 500, 257}},
    {{50, 445, 149, 469, 100, 450}, {404, 414, 474, 474, 440, 460}, {263, 425, 302, 469, 280, 465}, {237, 425, 262, 469, 260, 465},
     {303, 425, 330, 469, 300, 465}, {200, 425, 236, 469, 240, 465}, {125, 201, 153, 235, 39, 240}, {358, 156, 386, 190, 272, 195}, {573, 246, 601, 280, 487, 285}},
    {{50, 445, 149, 469, 100, 450}, {404, 414, 474, 474, 440, 460}, {263, 425, 302, 469, 280, 465}, {237, 425, 262, 469, 260, 465},
     {303, 425, 330, 469, 300, 465}, {200, 425, 236, 469, 240, 465}, {159, 195, 187, 229, 73, 234}, {391, 162, 419, 196, 305, 201}, {586, 218, 614, 252, 500, 257}}   /* context 2 = the Wolf table 0x7dc64 */
};
static const char *SC_LABEL[9] = {"EXIT CONFIG", "MECH LAB", "NEXT FORMATION", "PREV FORMATION", "ADD STARMATE", "DELETE STARMATE", "CHANGE MECH", "CHANGE MECH", "CHANGE MECH"};
#define SC_HEAD_FONT g_fbig
enum { SC_LINE = 12 };
static int run_star_config(SDL_Renderer *rd, SDL_Texture *tx, int headless, int frames, int clan, int *mx, int *my)
{
    static const char *CLIPS[3][10] = {{"AWOGRID", "AWOSTR1", "AWOSTR2", "AWOSTR3", "WWOBKG", "WWODSGN", "WWOCN", "WWOCP", "WWOVN", "WWOVP"},
                                       {"AJFGRID", "AJFSTR1", "AJFSTR2", "AJFSTR3", "WJFBKG", "WJFDSGN", "WJFCN", "WJFCP", "WJFVN", "WJFVP"},
                                       {"AIAGRID", "AIASTR1", "AIASTR1", "AIASTR1", "WIABKG2", "WIADSGN", "WIACN", "WIACP", "WIAVN", "WIAVP"}};
    static const int CXY[3][10][2] = {{{140, 310}, {73, 158}, {305, 125}, {500, 181}, {219, 429}, {407, 410}, {290, 433}, {240, 435}, {300, 429}, {220, 432}},
                                      {{108, 306}, {39, 164}, {272, 119}, {487, 209}, {216, 434}, {369, 416}, {286, 438}, {242, 438}, {296, 434}, {220, 437}},
                                      {{108, 306}, {73, 158}, {305, 125}, {500, 181}, {219, 414}, {415, 417}, {265, 430}, {238, 437}, {302, 431}, {223, 437}}};   /* FUN_0003b420 context 2 */
    clip *cl[10];
    int next = -1, f = 0, i, k, cx = CTX(clan), back = clan >= 2 ? 7 : 11;
    star_cfg *st = &g_star[clan];
    star_load(clan);
    load_bg(clan == 1 ? 21 : clan >= 2 ? 9 : 14);   /* background slots 15 / 22 / 10 (0x7f11c) */
    /* the clans' button clips 6-9 play on press (not yet); in context 2 slots 5-9 are stills loaded hidden (flags 0x24,
     * as WIALANCH): only the grid, the bays and WIABKG2 (flags 4) show */
    for (k = 0; k < 10; k++) cl[k] = (cx == 2 ? k <= 4 : k <= 5) ? clip_open(CLIPS[cx][k]) : NULL;
    while (next == -1) {
        SDL_Event e;
        int hover = -1;
        const uint8_t (*pal)[3] = (const uint8_t (*)[3])g_bg.pal;
        if (!headless) {
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) { next = -3; break; }
                if (e.type == SDL_MOUSEMOTION) { *mx = e.motion.x; *my = e.motion.y; }
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) next = back;
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    int x = e.button.x, y = e.button.y;
                    for (i = 0; i < 9; i++) {
                        const int *h = SC_HS[cx][i];
                        if (x < h[0] || x > h[2] || y < h[1] || y > h[3]) continue;
                        if (i >= 2) shell_sound(0x65, 0x32);   /* FUN_0003b420: formation, size and bay buttons click (0x91694) */
                        if (i == 0) next = back;
                        else if (i == 1) { shell_sound(0x66, 0x32); g_lab_slot = 0; next = 9; }   /* MECH LAB (0x91698) */
                        else if (i == 2) st->formation = (st->formation + 1) % 6;
                        else if (i == 3) st->formation = (st->formation + 5) % 6;
                        else if (i == 4 && st->size < 3) { if (st->slot[st->size].mech < 0) st->slot[st->size].mech = st->slot[0].mech; st->size++; }
                        else if (i == 5 && st->size > 1) st->size--;
                        else if (i >= 6 && i - 6 < st->size) { g_lab_slot = i - 6; next = 9; }
                        star_save(clan);
                    }
                }
            }
        }
        draw_bg();
        g_clip_pal = getenv("MW2_NO_CLIPPAL") || !cl[0] || !cl[0]->s ? NULL : smk_palette(cl[0]->s);
        for (k = 0; k < 10; k++) {
            if (!cl[k]) continue;
            if (k >= 1 && k <= 3 && k > st->size) continue;   /* empty bays */
            clip_next(cl[k]);
            clip_blit(cl[k], CXY[cx][k][0], CXY[cx][k][1]);
        }
        if (!getenv("MW2_SC_CLIPPAL")) g_clip_pal = NULL;
        {   /* FUN_0003b1f0: the formation (centred at 320, y 4) and four lines (y 35 / 50 / 65 / 80);
             * FUN_0003ad60 / 0x3aeb0: per formation (0x83e84: 3 x {member, showcase x, y, box}) the member's showcase
             * <ctx>sc<pic> on the grid and its box text (pilot cut at 92 px, 'Mech, tonnage) at 0x84210 / 0x8421c */
            static const int FORM_SLOT[6][3][4] = {
                {{2, 430, 346, 2}, {1, 333, 353, 1}, {0, 204, 359, 0}}, {{2, 295, 330, 0}, {1, 332, 352, 1}, {0, 385, 395, 2}},
                {{2, 245, 341, 0}, {0, 343, 355, 1}, {1, 418, 363, 2}}, {{2, 390, 332, 2}, {1, 350, 355, 1}, {0, 277, 374, 0}},
                {{0, 390, 332, 1}, {2, 204, 358, 0}, {1, 385, 395, 2}}, {{2, 295, 330, 0}, {1, 430, 346, 2}, {0, 288, 376, 1}}};
            static const int BOX_XY[2][3][2] = {{{81, 166}, {313, 133}, {508, 189}}, {{47, 172}, {280, 127}, {495, 217}}};
            static const char *SCP[3] = {"AWOSC", "AJFSC", "AIASC"};
            static const char *FORM_MIXED[6] = {"Echelon Left", "Echelon Right", "Line Abreast", "Line Astern", "V-Form", "Wedge"};
            char line[96];
            int total = 0, limit = clan >= 2 ? g_ia.tons[clan - 2] : mech_lab_kdmt(clan), bx = clan == 1 ? 1 : 0;
            for (k = 0; k < 3; k++) {
                const int *e = FORM_SLOT[st->formation][k];
                const lance_slot *sl;
                int m2, tons;
                char nm[24], dn[32];
                clip *sc;
                if (e[0] >= st->size) continue;
                sl = &st->slot[e[0]];
                m2 = sl->mech;
                {   /* not chosen yet (campaign): the mission's own 'Mech (BRF2 SDSC), as the Mech Lab shows */
                    static lance_slot dflt;
                    if (m2 < 0 && clan < 2) {
                        int rg = reg_selected(clan), mm = rg >= 0 ? g_reg[rg].rank : 0;
                        mission_star ms = mission_sdsc(CAMPAIGN_SCEN[clan][mm >= 0 && mm < 16 ? mm : 0]);
                        dflt = *sl;
                        if (ms.ok && ms.loadout[e[0]][0]) { dflt.mech = mech_index_of(ms.loadout[e[0]]); snprintf(dflt.loadout, sizeof dflt.loadout, "%s", ms.loadout[e[0]]); }
                        sl = &dflt; m2 = dflt.mech;
                    }
                }
                if (m2 < 0) continue;
                tons = sl->loadout[0] ? loadout_tons(sl->loadout) : 0;
                if (tons <= 0) tons = MECHS[m2].tons;
                total += tons;
                snprintf(dn, sizeof dn, "%s%s", SCP[cx], MECH_PIC[m2]);
                if ((sc = clip_open(dn))) {   /* the showcase canvas' origin sits at (90, 135) of its frame coordinates (fitted to
                                                * the DOS capture: 0 offset error for all three 'Mechs) */
                    clip_blit(sc, e[1] - 90, e[2] - 135); clip_close(sc);
                }
                {   /* the pilot's name, cut to 92 px */
                    size_t q;
                    int rg = clan < 2 && e[0] == 0 ? reg_selected(clan) : -1;   /* the player: the registered pilot's name */
                    snprintf(nm, sizeof nm, "%s", rg >= 0 ? g_reg[rg].name : sl->pilot);
                    for (q = strlen(nm); q > 0 && text_width(&g_fthin, nm) > 92; q--) nm[q - 1] = 0;
                }
                draw_text(&g_fthin, BOX_XY[bx][e[3]][0], BOX_XY[bx][e[3]][1], nm, pal, -1);
                draw_text(&g_fthin, BOX_XY[bx][e[3]][0], BOX_XY[bx][e[3]][1] + SC_LINE, MECHS[m2].name, pal, -1);
                snprintf(line, sizeof line, "%d.00 T", tons);
                draw_text(&g_fthin, BOX_XY[bx][e[3]][0], BOX_XY[bx][e[3]][1] + 2 * SC_LINE, line, pal, -1);
            }
            {
                char up[24];
                size_t q;
                snprintf(up, sizeof up, "%s", FORM_MIXED[st->formation]);
                for (q = 0; up[q]; q++) up[q] = (char)toupper((unsigned char)up[q]);
                draw_text(&SC_HEAD_FONT, 320 - text_width(&SC_HEAD_FONT, up) / 2, 4, up, pal, -1);
            }
            if (clan >= 2) snprintf(line, sizeof line, "Mission: Trial of Grievance");
            else {
                int rg = reg_selected(clan), mm = rg >= 0 ? g_reg[rg].rank : 0;
                snprintf(line, sizeof line, "Mission: %s", CAMPAIGN_TITLE[clan][mm >= 0 && mm < 16 ? mm : 0]);
            }
            draw_text(&g_fthin, 320 - text_width(&g_fthin, line) / 2, 35, line, pal, -1);
            {   /* the mission's star size (SDSC; Pyre Light 1 in the DOS capture) */
                int mx2 = 3;
                if (clan >= 2) mx2 = g_ia.size[clan - 2];
                else {
                    int rg = reg_selected(clan), mm = rg >= 0 ? g_reg[rg].rank : 0;
                    mission_star ms = mission_sdsc(CAMPAIGN_SCEN[clan][mm >= 0 && mm < 16 ? mm : 0]);
                    if (ms.ok) mx2 = ms.size;
                }
                snprintf(line, sizeof line, "Maximum 'Mechs in current Star: %d", mx2);
            }
            draw_text(&g_fthin, 320 - text_width(&g_fthin, line) / 2, 50, line, pal, -1);
            snprintf(line, sizeof line, "Keshik Defined Maximum Tonnage (KDMT) per 'Mech: %d.00 T", limit);
            draw_text(&g_fthin, 320 - text_width(&g_fthin, line) / 2, 65, line, pal, -1);
            snprintf(line, sizeof line, "Current Total Mass of the Star: %d.00 T", total);
            draw_text(&g_fthin, 320 - text_width(&g_fthin, line) / 2, 80, line, pal, -1);
        }
        g_clip_pal = NULL;
        draw_text(&g_flabel, 100 - text_width(&g_flabel, "EXIT CONFIG") / 2, 450, "EXIT CONFIG", pal, 6);   /* grey (DOS) */
        for (i = 0; i < 9; i++) { const int *h = SC_HS[cx][i]; if (*mx >= h[0] && *mx <= h[2] && *my >= h[1] && *my <= h[3]) hover = i; }
        if (hover > 0) draw_text(&g_flabel, SC_HS[cx][hover][4] - text_width(&g_flabel, SC_LABEL[hover]) / 2, SC_HS[cx][hover][5], SC_LABEL[hover], pal, -1);
        f++;
        if (headless) { if (f >= frames) next = -2; }
        else {
            SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
            SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
            SDL_Delay(66);   /* the clips' pace (ASSUMED ~15 fps) */
        }
    }
    for (k = 0; k < 10; k++) clip_close(cl[k]);
    return next;
}


/* a campaign mission: the player's lance file (default mech until Star Config: Wolf Timber Wolf, Falcon Mad Dog,
 * ASSUMED), the simulation, the result; success advances the pilot's campaign */
static int launch_campaign(int clan, int reg) { return launch_campaign_m(clan, reg, g_reg[reg].rank); }
/* mission m of the clan's campaign (the pilot's current one; the roster's LAUNCH OLD MISSION passes an earlier one) */
static int launch_campaign_m(int clan, int reg, int m)
{
    g_load_clan = clan;
    const char *simprj = NULL, *save = getenv("MW2_SAVE_DIR");
    char path[1024], err[256], line[4200], scen[16];
    prj_archive *a;
    lance_slot fr[3], en[3];
    int k, rc;
    unsigned char b[20];
    FILE *f;
    if (m < 0 || m > 15) return -1;
    if (!save) save = g_dir;
    if (!simprj) simprj = sim_archive();
    a = prj_open(simprj, err, sizeof err);
    if (!a) { fprintf(stderr, "launch: cannot open %s: %s\n", simprj, err); return -1; }
    memset(fr, 0, sizeof fr); memset(en, 0, sizeof en);
    star_load(clan);
    {   /* the mission's star (BRF2 SDSC): trials assign the mechs; the size is capped; the limit scores underweight */
        mission_star ms = mission_sdsc(CAMPAIGN_SCEN[clan][m]);
        int sz = g_star[clan].size;
        if (ms.ok && g_star[clan].slot[0].mech < 0) sz = ms.def;   /* not configured: the mission's default size */
        if (ms.ok && sz > ms.size) sz = ms.size;                    /* capped at the maximum */
        for (k = 0; k < 3; k++) { fr[k] = g_star[clan].slot[k]; if (k >= sz) fr[k].mech = -1; en[k].mech = -1; }
        if (ms.ok && (CAMPAIGN_TRIAL[clan][m] || fr[0].mech < 0))
            for (k = 0; k < ms.size && k < 3; k++)
                if (ms.loadout[k][0]) { fr[k].mech = mech_index_of(ms.loadout[k]); snprintf(fr[k].loadout, sizeof fr[k].loadout, "%s", ms.loadout[k]); }
        if (fr[0].mech < 0) fr[0].mech = clan == 1 ? 5 : 9;
        g_last_limit = ms.ok ? ms.tons : -1;
        g_last_tons = 0;
        for (k = 0; k < 3; k++)
            if (fr[k].mech >= 0) {
                char lo[16];
                snprintf(lo, sizeof lo, "%s", fr[k].loadout[0] ? fr[k].loadout : "");
                if (!lo[0]) snprintf(lo, sizeof lo, "%s00std", lance_mech_code(fr[k].mech));
                g_last_tons += loadout_tons(lo);
            }
    }
    snprintf(fr[0].pilot, sizeof fr[0].pilot, "%s", g_reg[reg].name);
    rc = lance_write_all(a, save, fr, en, 1);
    prj_close(a);
    if (rc) { fprintf(stderr, "launch: cannot write the lance files into %s\n", save); return -1; }
    snprintf(scen, sizeof scen, "%s", CAMPAIGN_SCEN[clan][m]);
    snprintf(g_load_scene, sizeof g_load_scene, "%s", scen);
    for (k = 0; scen[k]; k++) if (scen[k] >= 'a' && scen[k] <= 'z') scen[k] = (char)(scen[k] - 32);
    printf("launch: campaign mission %d %s (%s)\n", m + 1, scen, CAMPAIGN_TITLE[clan][m]);
    unsetenv("MW2_OE");
    setenv("MW2_OF", FORM_ARG[g_star[clan].formation], 1);
    setenv("MW2_PILOT", g_reg[reg].name, 1);
    sim_command(line, sizeof line, scen, save);
    sfx_lock(g_sfx); gus_song_close(g_song); g_song = NULL; mt32_song_close(g_msong); g_msong = NULL; g_song_entry = -1; sfx_unlock(g_sfx);   /* the menu music stops for the mission (the MT-32 one too) */
    sfx_stop_loops(g_sfx);
    if (g_win) SDL_HideWindow(g_win);
    snprintf(path, sizeof path, "%s/MW2MSN.CFG", save);
    remove(path);
    loading_screen(g_load_clan);
    rc = system(line);
    read_results(save);
    sim_returned(rc);
    if (g_win) { SDL_ShowWindow(g_win); SDL_RaiseWindow(g_win); }
    (void)b; (void)f;   /* the debriefing (screen 3) scores the mission and updates the pilot */
    return 0;
}



static void shell_message(SDL_Renderer *rd, SDL_Texture *tx, int headless, const char *msg);
/* ---- the Hall of Honor (FUN_00028250, the global menu's 4th entry): background slot 2 (DATABASE entry 1), logo
 * amwlogo1 at (120,4); headers at
 * y 150: Pilot 0, Clan 125, Rank 200, Honor 325, Kills 400, Hit % 460, Last Mission 520; the 20 registry pilots
 * sorted by honor (FUN_000281e0: honor, then missions), the top 8 from y 182 every 16; any click / key closes */
static const char *CLAN_NAME[3] = {"Wolf", "Jade Falcon", "Ghost Bear"};   /* 0x7d3b8 */
static int honor_cmp(const void *a, const void *b)
{
    const pilot_rec *x = *(const pilot_rec *const *)a, *y = *(const pilot_rec *const *)b;
    if (x->honor != y->honor) return x->honor > y->honor ? -1 : 1;
    if (x->rank != y->rank) return x->rank > y->rank ? -1 : 1;
    return 0;
}
static void hall_of_honor(SDL_Renderer *rd, SDL_Texture *tx, int headless)
{
    const pilot_rec *list[20];
    int n = 0, i, done = 0;
    smk *logo = open_smk("AMWLOGO1");
    Uint32 last = 0;
    reg_load();
    for (i = 0; i < 20; i++) if (g_reg[i].used) list[n++] = &g_reg[i];
    qsort(list, (size_t)n, sizeof list[0], honor_cmp);
    load_bg(1);
    while (!done) {
        SDL_Event e;
        const uint8_t (*pal)[3] = (const uint8_t (*)[3])g_bg.pal;
        static const int CX[7] = {0, 125, 200, 325, 400, 460, 520};
        static const char *HD[7] = {"Pilot", "Clan", "Rank", "Honor", "Kills", "Hit %", "Last Mission"};
        draw_bg();
        if (logo) {
            Uint32 now = SDL_GetTicks();
            if (headless || now - last >= (Uint32)smk_frame_ms(logo) || last == 0) { smk_next(logo); last = now; }
            if (logo) blit_smk(logo, 120, 4);
        }
        for (i = 0; i < 7; i++) draw_text(&g_flabel, CX[i] + 4, 150, HD[i], pal, -1);
        for (i = 0; i < n && i < 8; i++) {
            const pilot_rec *r = list[i];
            char v[48];
            int y = 182 + i * 16, c = r->clan >= 0 && r->clan < 3 ? r->clan : (int)((r - g_reg) / 10);
            draw_text(&g_flist, CX[0] + 4, y, r->name, pal, -1);
            draw_text(&g_flist, CX[1] + 4, y, CLAN_NAME[c], pal, -1);
            draw_text(&g_flist, CX[2] + 4, y, RANK_TITLE[r->title >= 0 && r->title < 9 ? r->title : 0], pal, -1);
            snprintf(v, sizeof v, "%d", r->honor); draw_text(&g_flist, CX[3] + 4, y, v, pal, -1);
            snprintf(v, sizeof v, "%d", r->stats[0]); draw_text(&g_flist, CX[4] + 4, y, v, pal, -1);
            if (r->stats[2] > 0) snprintf(v, sizeof v, "%d%%", r->stats[1] * 100 / r->stats[2]); else snprintf(v, sizeof v, "-");
            draw_text(&g_flist, CX[5] + 4, y, v, pal, -1);
            snprintf(v, sizeof v, "%s", r->rank > 0 && r->rank <= 16 && c < 2 ? CAMPAIGN_TITLE[c][r->rank - 1] : "-");
            draw_text(&g_flist, CX[6] + 4, y, v, pal, -1);
        }
        if (headless) break;
        while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT || e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN) done = 1;
        SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
        SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
        SDL_Delay(30);
    }
    smk_close(logo);
}


/* ---- Combat Variables (the global menu's 2nd entry, FUN_000258e0 case 1): background slot 3 (entry 2), logo amwlogo1
 * (120,4); widget table 0x7d09c: values at x 393 - DIFFICULTY y 219 (MW2DIF +5), HEAT TRACKING 239 (+4), OBJECT TEXTURES
 * 277, TERRAIN TEXTURES 297, DISPLAY DETAIL 317, OBJECT DENSITY 337, CHUNKY EXPLOSIONS 357 (MW2SND +0x14 .. +0x24),
 * RESOLUTION 377 (MW2SND +0x2c), INVULNERABILITY 416 (MW2DIF +1), UNLIMITED AMMO 436 (+0), NO COLLISION DAMAGE 456 (+3);
 * sliders MUSIC / EFFECTS / VOICE at (335, 128 / 152 / 176) 285 wide (MW2SND +0xc / +4 / +8). RESOLUTION keeps its
 * meaning - the sim drawn at 320x200 / 640x480 / 1024x768 (MW2PORT.CFG render=, the original byte written as VGA /
 * vesa480 / vesa768) - plus the port's NATIVE choices (the display's resolution, anti-aliasing off / 2X / 4X / 8X); the
 * port's DISPLAY row at y 397 picks desktop / windowed / the display's modes */
/* the k-th word of a row in the pristine background (the option words: the columns differing from the column's key
 * colour at (380,130), split at gaps of 6+ pixels); returns its x range */
static int32_t rd32(const uint8_t *b) { return (int32_t)(b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24); }
static void wr32(uint8_t *b, int32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }

/* ---- Combat Variables (the global menu's 2nd entry, FUN_000258e0 case 1): background slot 3 (entry 2) holds every
 * option word; the value column (375,124) 258x352 is blanked and the chosen word copied back (widget table 0x7d09c,
 * x 393): DIFFICULTY y 219 (MW2DIF +5: EASY MEDIUM HARD), HEAT TRACKING 239 (+4), OBJECT TEXTURES 277, TERRAIN
 * TEXTURES 297, DISPLAY DETAIL 317, OBJECT DENSITY 337 (HIGH / LOW), CHUNKY EXPLOSIONS 357 (MW2SND.CFG int32 +0x14 ..
 * +0x24, 1 = ON / HIGH), RESOLUTION 377 (MW2SND +0x2c: the VESA driver name, vesa480.dll / vesa768.dll), INVULNERABILITY
 * 416 (MW2DIF +1), UNLIMITED AMMO 436 (+0), NO COLLISION DAMAGE 456 (+3); sliders MUSIC / EFFECTS / VOICE at
 * (335, 128 / 152 / 176) 285 wide = MW2SND int32 16.16 at +0xc / +4 / +8. The port: RESOLUTION lists the display's
 * modes: see above. */
static void combat_variables(SDL_Renderer *rd, SDL_Texture *tx, int headless)
{
    uint8_t snd[60], dif[8];
    char p[700];
    FILE *f;
    portcfg pc;
    int done = 0, i, nmodes = 0, mi = -1;
    struct { int w, h; } modes[64];
    smk *logo = open_smk("AMWLOGO1");
    static const int ROW_Y[13] = {219, 239, 277, 297, 317, 337, 357, 377, 397, 416, 436, 456, 258};   /* + the port's RENDERER */
    static const char *const ED_TEXT[PORTCFG_ED_COUNT] = {"ENHANCED", "DOS", "3DFX", "ATI RAGE", "S3 VIRGE", "POWERVR", "MATROX MYSTIQUE"};
    memset(snd, 0, sizeof snd); memset(dif, 0, sizeof dif);
    wr32(snd + 0x0c, 65536); wr32(snd + 0x04, 65536); wr32(snd + 0x08, 65536);
    for (i = 0x14; i <= 0x24; i += 4) wr32(snd + i, 1);
    dif[4] = 1; dif[5] = 1;
    snprintf(p, sizeof p, "%s", gpath("MW2SND.CFG")); if ((f = fopen(p, "rb"))) { if (fread(snd, 1, sizeof snd, f)) {} fclose(f); }
    snprintf(p, sizeof p, "%s", gpath("MW2DIF.CFG")); if ((f = fopen(p, "rb"))) { if (fread(dif, 1, sizeof dif, f)) {} fclose(f); }
    portcfg_load(g_dir, &pc);
    {
        int n = SDL_GetNumDisplayModes(0), k, j;
        for (k = 0; k < n && nmodes < 64; k++) {
            SDL_DisplayMode dm;
            if (SDL_GetDisplayMode(0, k, &dm) != 0) continue;
            for (j = 0; j < nmodes; j++) if (modes[j].w == dm.w && modes[j].h == dm.h) break;
            if (j == nmodes && dm.w >= 640 && dm.h >= 480) { modes[nmodes].w = dm.w; modes[nmodes].h = dm.h; nmodes++; }
        }
        if (pc.mode == 2) for (j = 0; j < nmodes; j++) if (modes[j].w == pc.w && modes[j].h == pc.h) mi = j;
    }
    load_bg(2);
    while (!done) {
        SDL_Event e;
        int sel[13];
        char res[32], aa[16];
        sel[0] = dif[5] <= 2 ? dif[5] : 1; sel[1] = dif[4] ? 0 : 1;
        for (i = 0; i < 5; i++) sel[2 + i] = rd32(snd + 0x14 + 4 * i) ? 0 : 1;   /* ON / HIGH first */
        /* RESOLUTION: the original's three words when the sim draws at one of them (render=), else the port's NATIVE text */
        sel[7] = pc.render_w == 320 ? 0 : pc.render_w == 640 ? 1 : pc.render_w == 1024 ? 2 : -1;
        sel[8] = -1;                                                           /* the port's DISPLAY row */
        sel[9] = dif[1] ? 0 : 1; sel[10] = dif[0] ? 0 : 1; sel[11] = dif[3] ? 0 : 1;
        if (pc.mode == 0) snprintf(aa, sizeof aa, "DESKTOP");
        else if (pc.mode == 1) snprintf(aa, sizeof aa, "WINDOWED");
        else snprintf(aa, sizeof aa, "%dx%d", pc.w, pc.h);
        if (pc.aa) snprintf(res, sizeof res, "NATIVE %dX AA", pc.aa); else snprintf(res, sizeof res, "NATIVE");
        if (!headless) {
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) || (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT)) done = 1;
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    int x = e.button.x, y = e.button.y;
                    for (i = 0; i < 13; i++) {
                        if (x < 375 || x > 633 || y < ROW_Y[i] - 2 || y > ROW_Y[i] + 14) continue;
                        if (i == 0) dif[5] = (uint8_t)((dif[5] + 1) % 3);
                        else if (i == 1) dif[4] ^= 1;
                        else if (i >= 2 && i <= 6) wr32(snd + 0x14 + 4 * (i - 2), !rd32(snd + 0x14 + 4 * (i - 2)));
                        else if (i == 7) {   /* RESOLUTION: NATIVE (no AA, 2X, 4X, 8X) -> 320x200 -> 640x480 -> 1024x768 */
                            if (!pc.render_w) {
                                if (pc.aa < 8) pc.aa = pc.aa == 0 ? 2 : pc.aa * 2;
                                else { pc.render_w = 320; pc.render_h = 200; }
                            } else if (pc.render_w == 320) { pc.render_w = 640; pc.render_h = 480; }
                            else if (pc.render_w == 640) { pc.render_w = 1024; pc.render_h = 768; }
                            else { pc.render_w = pc.render_h = 0; pc.aa = 0; }
                            {   /* the original's field: VGA (none) / vesa480.dll / vesa768.dll */
                                int hh = pc.render_w ? pc.render_h : (pc.mode == 2 ? pc.h : 768);
                                memset(snd + 0x2c, 0, 16);
                                if (hh >= 700) memcpy(snd + 0x2c, "vesa768.dll", 11); else if (hh >= 400) memcpy(snd + 0x2c, "vesa480.dll", 11);
                            }
                        }
                        else if (i == 8) {   /* the port's DISPLAY: desktop -> windowed -> the display's modes -> desktop */
                            if (pc.mode == 0) pc.mode = 1;
                            else if (pc.mode == 1) { if (nmodes) { pc.mode = 2; mi = 0; pc.w = modes[0].w; pc.h = modes[0].h; } else pc.mode = 0; }
                            else if (++mi < nmodes) { pc.w = modes[mi].w; pc.h = modes[mi].h; }
                            else pc.mode = 0;
                        }
                        else if (i == 9) dif[1] ^= 1;
                        else if (i == 10) dif[0] ^= 1;
                        else if (i == 11) dif[3] ^= 1;
                        else if (i == 12) {   /* the port's RENDERER: the look of the simulation, editions whose files are here */
                            int e2 = pc.edition, tries;
                            for (tries = 0; tries < PORTCFG_ED_COUNT; tries++) {
                                char m1[PORTCFG_PATH], m2[PORTCFG_PATH], m3[PORTCFG_PATH], m4[PORTCFG_PATH], m5[PORTCFG_PATH];
                                e2 = (e2 + 1) % PORTCFG_ED_COUNT;
                                if (portcfg_edition(&g_cfg, e2, m1, m2, m3, m4, m5) == 0) break;
                            }
                            pc.edition = g_cfg.edition = e2;
                        }
                    }
                    for (i = 0; i < 3; i++) {   /* MUSIC / EFFECTS / VOICE: the knob's centre at x 350 + v */
                        static const int SY[3] = {128, 152, 176}, SO[3] = {0x0c, 0x04, 0x08};
                        if (x >= 335 && x <= 620 && y >= SY[i] && y <= SY[i] + 21) {
                            int xc = x - (335 + 15);   /* 0x254f0: v = mouse x - (x + 15), 0..0x100, << 8 */
                            xc = xc < 0 ? 0 : xc > 256 ? 256 : xc;
                            wr32(snd + SO[i], xc << 8);
                        }
                    }
                }
            }
        }
        draw_bg();
        {   /* blank the value column, then the chosen words */
            int x, y;
            for (y = 124; y < 476; y++) for (x = 375; x < 634; x++) { uint8_t *q = g_fb + ((size_t)y * W + (size_t)x) * 3; q[0] = q[1] = q[2] = 0; }
        }
        {   /* every value in one lettering (the port's rows had made the artwork's words look out of place): the chosen
             * value per row as text at x 393; the DISPLAY label right-aligned at 360 */
            static const char *const DIFF[3] = {"EASY", "MEDIUM", "HARD"};
            char vals[13][32];
            tt_item it;
            int k;
            const char *lines[15];
            int ly[15], lx[15];
            for (k = 0; k < 13; k++) {
                if (k == 0) snprintf(vals[k], sizeof vals[k], "%s", DIFF[sel[0] >= 0 && sel[0] < 3 ? sel[0] : 1]);
                else if (k == 4 || k == 5) snprintf(vals[k], sizeof vals[k], "%s", sel[k] == 0 ? "HIGH" : "LOW");   /* DISPLAY DETAIL, OBJECT DENSITY */
                else if (k == 7) snprintf(vals[k], sizeof vals[k], "%s", sel[7] == 0 ? "320x200" : sel[7] == 1 ? "640x480" : sel[7] == 2 ? "1024x768" : res);
                else if (k == 8) snprintf(vals[k], sizeof vals[k], "%s", aa);
                else if (k == 12) snprintf(vals[k], sizeof vals[k], "%s", ED_TEXT[pc.edition >= 0 && pc.edition < PORTCFG_ED_COUNT ? pc.edition : 0]);
                else snprintf(vals[k], sizeof vals[k], "%s", sel[k] == 0 ? "ON" : "OFF");
                lines[k] = vals[k]; ly[k] = ROW_Y[k] + 1; lx[k] = 393;
            }
            lines[13] = "DISPLAY"; ly[13] = 398; lx[13] = 360;
            lines[14] = "RENDERER"; ly[14] = ROW_Y[12] + 1; lx[14] = 360;
            for (k = 0; k < 15; k++) {
                const char *t = lines[k];
                if (!ttext_available()) {   /* no font file: the original menu font */
                    int tx2 = k >= 13 ? lx[k] - text_width(&g_flabel, t) : lx[k];
                    draw_text(&g_flabel, tx2, ly[k] - 1, t, (const uint8_t (*)[3])g_bg.pal, -1);
                } else if (g_text_hires) tt_text(lx[k], ly[k], t, k >= 13);
                else { memset(&it, 0, sizeof it); it.x = lx[k]; it.y = ly[k]; it.align = k >= 13; it.cap = 12.0f; it.rgb[0] = it.rgb[1] = it.rgb[2] = 221; snprintf(it.t, sizeof it.t, "%s", t); tt_raster(&it); }
            }
        }
        if (logo) { smk_next(logo); blit_smk(logo, 120, 4); }
        {   /* the sliders (MW2SHELL 0x25480): DATABASE entry 7 (SHP) - frame 1, the track, at (335, y), then frame 0, the
             * knob, at (335 + 15 + v - 7, y - 1), v = the 16.16 level >> 8 (0..256) */
            static uint8_t *sl_shp; static long sl_len = -1;
            static const int SY[3] = {128, 152, 176}, SO[3] = {0x0c, 0x04, 0x08};
            if (sl_len < 0) sl_len = shelldb_entry(&g_db, 7, &sl_shp);
            for (i = 0; i < 3 && sl_shp && sl_len > 0; i++) {
                int v = rd32(snd + SO[i]), fi;
                v = v < 0 ? 0 : v > 65536 ? 65536 : v;
                v >>= 8;
                for (fi = 1; fi >= 0; fi--) {
                    shp_frame fr;
                    int ox = fi ? 335 : 335 + 15 + v - 7, oy = fi ? SY[i] : SY[i] - 1, xx, yy;
                    if (shp_decode(sl_shp, (size_t)sl_len, fi, &fr) != 0) continue;
                    for (yy = 0; yy < fr.h; yy++)
                        for (xx = 0; xx < fr.w; xx++) {
                            int px = ox + fr.left + xx, py = oy + fr.top + yy;
                            if (!fr.mask[yy * fr.w + xx] || px < 0 || py < 0 || px >= W || py >= H) continue;
                            memcpy(g_fb + ((size_t)py * W + (size_t)px) * 3, g_bg.pal[fr.pix[yy * fr.w + xx]], 3);
                        }
                    shp_frame_free(&fr);
                }
            }
        }
        if (headless) break;
        SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
        SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
        SDL_Delay(30);
    }
    smk_close(logo);
    if (!headless) {
        snprintf(p, sizeof p, "%s/MW2SND.CFG", g_dir); if ((f = fopen(p, "wb"))) { fwrite(snd, 1, sizeof snd, f); fclose(f); }
        snprintf(p, sizeof p, "%s/MW2DIF.CFG", g_dir); if ((f = fopen(p, "wb"))) { fwrite(dif, 1, sizeof dif, f); fclose(f); }
        portcfg_save(g_dir, &pc);
    }
}

/* ---- the global menu (FUN_000258e0: Esc / right button): panel DATABASE image 7 at (198,116) 244x248, six items
 * (0x7eefc) centred at x 320 from y 157 every 33, highlight under the mouse / arrows. Returns the next screen,
 * -1 to stay, -3 to quit */
/* a DATABASE SHP entry's first frame at (x, y) in the screen's palette (the global menu panel is shell image 7 = entry 6,
 * drawn at its top-left: FUN_0003eac0(.., 0xc6, 0x74, 0xf4, 0xf8)) */
static void db_shp(int entry, int x, int y)
{
    uint8_t *e = NULL;
    long n = shelldb_entry(&g_db, entry, &e);
    shp_frame fr;
    if (n > 0 && shp_decode(e, (size_t)n, 0, &fr) == 0) {
        int X, Y;
        for (Y = 0; Y < fr.h; Y++)
            for (X = 0; X < fr.w; X++) {
                int px = x + X, py = y + Y;
                if (!fr.mask[Y * fr.w + X] || px < 0 || py < 0 || px >= W || py >= H) continue;
                memcpy(g_fb + ((size_t)py * W + (size_t)px) * 3, g_bg.pal[fr.pix[Y * fr.w + X]], 3);
            }
        shp_frame_free(&fr);
    }
    free(e);
}

/* ---- The Keshik (FUN_00021400): the credits, read from the user's MW2SHELL.EXE (the 388-entry pointer table 0x7ca80; found
 * in the file by its first entries, "<ACTIVISION presents", 0, "<MECHWARRIOR 2"). A line's first character picks the font:
 * '<' shell font 0x1b, '>' 0x1c, '~' 0x1b with colour 1 drawn as 0x10, else 0x1a / 0x20 (thin). Lines are 20 px apart and
 * scroll up 1 px a frame from y 460; only 105 < y <= 459 shows; the amwlogo1 clip plays at (120,4); any key or click ends.
 * After 10 Oct 1995 the ">SHELL P..." credit becomes "~SHELL PROGRAMMING by REDLINE GAMES" (the date test). */
static char **g_credits;
static int g_ncredits;
static void credits_load(void)
{
    FILE *f = fopen(gpath("MW2SHELL.EXE"), "rb");
    uint8_t *b = NULL;
    long n = 0, f1 = -1, f2 = -1, i;
    if (g_credits || !f) { if (f) fclose(f); return; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0 && (b = malloc((size_t)n)) && fread(b, 1, (size_t)n, f) == (size_t)n) {
        for (i = 0; i + 20 < n && (f1 < 0 || f2 < 0); i++) {
            if (f1 < 0 && memcmp(b + i, "<ACTIVISION presents", 21) == 0) f1 = i;
            if (f2 < 0 && memcmp(b + i, "<MECHWARRIOR 2", 15) == 0) f2 = i;
        }
        for (i = 0; f1 >= 0 && f2 >= 0 && i + 388 * 4 <= n; i += 4) {
            uint32_t a0 = b[i] | b[i + 1] << 8 | b[i + 2] << 16 | (uint32_t)b[i + 3] << 24;
            uint32_t a1 = b[i + 4] | b[i + 5] << 8 | b[i + 6] << 16 | (uint32_t)b[i + 7] << 24;
            uint32_t a2 = b[i + 8] | b[i + 9] << 8 | b[i + 10] << 16 | (uint32_t)b[i + 11] << 24;
            long delta, k;
            if (a0 == 0 || a1 != 0 || (long)a2 - (long)a0 != f2 - f1) continue;
            delta = f1 - (long)a0;
            g_credits = calloc(388, sizeof *g_credits);
            if (!g_credits) break;
            for (k = 0; k < 388; k++) {
                const uint8_t *q = b + i + k * 4;
                uint32_t a = q[0] | q[1] << 8 | q[2] << 16 | (uint32_t)q[3] << 24;
                long o = (long)a + delta;
                if (a && o >= 0 && o < n) {
                    g_credits[k] = strdup((const char *)b + o);
                    if (strncmp(g_credits[k], ">SHELL P", 8) == 0) { free(g_credits[k]); g_credits[k] = strdup("~SHELL PROGRAMMING by REDLINE GAMES"); }
                }
            }
            g_ncredits = 388;
            break;
        }
    }
    free(b);
    fclose(f);
}
static void the_keshik(SDL_Renderer *rd, SDL_Texture *tx, int headless)
{
    smk *logo = open_smk("AMWLOGO1");
    int top = 0x1cc, done = 0, k, frames = 0, prev_bg = g_bg_entry;
    const uint8_t (*pal)[3];
    uint8_t *bgfb = malloc(sizeof g_fb);
    credits_load();
    load_bg(4);                                   /* shell image 5: "THE KESHIK" under the logo */
    pal = (const uint8_t (*)[3])g_bg.pal;
    draw_bg();
    if (bgfb) memcpy(bgfb, g_fb, sizeof g_fb);
    if (logo) smk_next(logo);
    while (!done) {
        SDL_Event e;
        if (bgfb) memcpy(g_fb, bgfb, sizeof g_fb); else memset(g_fb, 0, sizeof g_fb);   /* FUN_0003e9c0: the window from the background */
        text_layer_clear();
        for (k = 0; k < g_ncredits; k++) {
            int y = top + 20 * k;
            const char *t = g_credits[k];
            if (y > 0x1cb) break;
            if (y <= 0x68 || !t) continue;
            if (t[0] == '<') draw_text(&g_f26, 320 - text_width(&g_f26, t + 1) / 2, y, t + 1, pal, -1);
            else if (t[0] == '>') draw_text(&g_f28, 320 - text_width(&g_f28, t + 1) / 2, y, t + 1, pal, -1);
            else if (t[0] == '~') draw_text(&g_f26, 320 - text_width(&g_f26, t + 1) / 2, y, t + 1, pal, 0x10);
            else draw_text(&g_fthin, 320 - text_width(&g_fthin, t) / 2, y, t, pal, -1);
        }
        {   /* the bands above and below the window (0x69..0x7c, 0x1cc..0x1df) */
            int y, x;
            for (y = 0x69; y < 0x7d; y++) for (x = 0; x < W; x++) memcpy(g_fb + ((size_t)y * W + (size_t)x) * 3, bgfb ? bgfb + ((size_t)y * W + (size_t)x) * 3 : (const uint8_t *)"\0\0\0", 3);
            for (y = 0x1cc; y < H; y++) for (x = 0; x < W; x++) memcpy(g_fb + ((size_t)y * W + (size_t)x) * 3, bgfb ? bgfb + ((size_t)y * W + (size_t)x) * 3 : (const uint8_t *)"\0\0\0", 3);
            text_layer_cull(0, 0x69, W, 0x7d);
            text_layer_cull(0, 0x1cc, W, H);
        }
        if (logo) blit_smk(logo, 120, 4);
        top--;
        frames++;
        if (top + 20 * g_ncredits < 0x69) top = 0x1cc;   /* ASSUMED: starts over at the end */
        if (headless) { if (frames >= (getenv("MW2_KESHIK_FRAMES") ? atoi(getenv("MW2_KESHIK_FRAMES")) : 300)) done = 1; continue; }
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT || e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN) done = 1;
        if (logo && (frames & 1)) smk_next(logo);   /* the clip steps twice a scroll step in the original loop (ASSUMED rate) */
        SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
        SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
        SDL_Delay(20);
    }
    if (logo) smk_close(logo);
    free(bgfb);
    load_bg(prev_bg);
}


/* ---- Cockpit Controls (FUN_00021020; data and the INPUT.MAP writer in src/cockpitcfg.c). Background shell image 4
 * (entry 3) above y 95 (the logo and "COCKPIT CONTROLS"), two red column rules at x 172 and 477, the thin font (31):
 * white, grey (154) and red (255,16,24), rows 11 px apart.
 * Page 1 (table 0x7c6c0): "Current Config:" (0,110), SELECT INPUT DEVICES (0,132), the drivers from (0,154) - red =
 *   selected, a click toggles; ACCEPT (0,297): the selected devices' default configs merged, INPUT.MAP written;
 *   CUSTOM CONFIGURATION (0,319): the editor; ABORT (0,440).
 * Page 2 (table 0x7a9b4): INPUT DEVICES (0,110), drivers from (0,132) (red = the one listed on the right); Current
 *   Config (0,275) and its name (8,286); RESET DEFAULTS (0,308); Load Custom 1-4 (0,330..); Save Custom 1-4 (0,385..);
 *   ACCEPT CONFIG AND EXIT (0,440); ABORT (0,462). GAME CONTROLS (176,110) and the level ("Primary Controls" ..
 *   "Quaternary Controls", centred on 370) - a click on it changes the level; from (176,132) each control: label,
 *   direction ("+/-", "L/R", ...: a click reverses), modifier at 261 ("--", "Ctrl", "Alt", "Shft": a click cycles),
 *   the binding at 285. A click on a control then on an input at the right assigns it (two-button controls take two
 *   inputs: the first then the second); right button on a control clears it. Right: the device (480,110),
 *   "Directional" (480,132) and its axes, then "Buttons" and its buttons (the keyboard's list scrolls with the wheel).
 * Layout measured on DOS captures (docs/reference/dos_cockpit_*.png); the colours of selected / current items and the
 * click behaviour inferred from the captures. */
static cc_device g_ccdev[CC_DEVICES];
static int g_ccsel[CC_DEVICES], g_ccsel_init;
static char g_ccname[64] = "Default Config";
static int cc_colour(int r, int g, int b)
{
    int i, best = 0, bd = 1 << 30;
    for (i = 0; i < 256; i++) { int d = abs(g_bg.pal[i][0] - r) + abs(g_bg.pal[i][1] - g) + abs(g_bg.pal[i][2] - b); if (d < bd) { bd = d; best = i; } }
    return best;
}
static const char *giddi_dir(void) { static char p[1100]; snprintf(p, sizeof p, "%s", gpath("GIDDI")); return p; }
static void cc_init(void)
{
    FILE *f;
    char line[256];
    int k;
    cc_devices(g_ccdev, giddi_dir());
    if (g_ccsel_init) return;
    g_ccsel_init = 1;
    if ((f = fopen(gpath("INPUT.MAP"), "rb"))) {   /* the devices the current INPUT.MAP uses */
        while (fgets(line, sizeof line, f)) {
            char sg[8], dv[32];
            if (sscanf(line, " %7s %31s", sg, dv) == 2 && (sg[0] == '+' || sg[0] == '-'))
                for (k = 0; k < CC_DEVICES; k++) if (strcmp(dv, CC_DEVNAME[k]) == 0) g_ccsel[k] = 1;
        }
        fclose(f);
    } else g_ccsel[2] = 1;
}
static int cc_write_inputmap(SDL_Renderer *rd, SDL_Texture *tx, int headless, const cc_config *c)
{
    static char out[65536];
    size_t n = cc_generate(c, g_ccdev, g_ccsel, out, sizeof out);
    char path[1100], bak[1100];
    FILE *f;
    if (!n) { shell_message(rd, tx, headless, "Error: Sim can not|handle that many controls.#Ok"); return -1; }
    snprintf(path, sizeof path, "%s/INPUT.MAP", g_dir);
    snprintf(bak, sizeof bak, "%s/INPUT.BAK", g_dir);
    remove(bak);
    rename(path, bak);                               /* input.map -> input.bak, as the original */
    if (!(f = fopen(path, "wb")) || fwrite(out, 1, n, f) != n) { if (f) fclose(f); rename(bak, path); return -1; }
    fclose(f);
    shell_message(rd, tx, headless, "Cockpit Control Configured.#Ok");
    return 0;
}
static smk *g_cclogo;          /* the amwlogo1 clip at (120,4), as FUN_00021020 plays it on these pages */
static Uint32 g_cclogo_t;
static void cc_frame(int lines)
{
    int x, y;
    draw_bg();
    if (g_cclogo) {
        Uint32 now = SDL_GetTicks();
        if (!g_cclogo_t || now - g_cclogo_t >= (Uint32)smk_frame_ms(g_cclogo)) { smk_next(g_cclogo); g_cclogo_t = now; }
        blit_smk(g_cclogo, 120, 4);
    }
    for (y = 105; y < H; y++) memset(g_fb + (size_t)y * W * 3, 0, (size_t)W * 3);
    text_layer_cull(0, 105, W, H);
    if (lines) {
        int red = cc_colour(255, 16, 24);
        for (y = 120; y <= 470; y++) for (x = 0; x < 2; x++) memcpy(g_fb + ((size_t)y * W + (size_t)(x ? 477 : 172)) * 3, g_bg.pal[red], 3);
    }
}
static void cc_present(SDL_Renderer *rd, SDL_Texture *tx)
{
    SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
    SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
    SDL_Delay(15);
}
static void cc_text(int x, int y, const char *t, int col) { draw_text(&g_fthin, x, y, t, (const uint8_t (*)[3])g_bg.pal, col); }

static void cc_editor(SDL_Renderer *rd, SDL_Texture *tx, int headless, cc_config *cfg)
{
    static const char *LEVEL[4] = {"Primary Controls", "Secondary Controls", "Tertiary Controls", "Quaternary Controls"};
    static const char *DIRL[7][2] = {{"-/+", "+/-"}, {"L/R", "R/L"}, {"L/R", "R/L"}, {"D/U", "U/D"}, {"L/R", "R/L"}, {"D/U", "U/D"}, {"I/O", "O/I"}};
    static const char *MODL[5] = {"--", "Ctrl", "Alt", "--", "Shft"};
    int done = 0, level = 0, shown = 2, sel = -1, half = 0, scroll = 0, k, f = 0;
    int white = -1, grey = cc_colour(154, 154, 154), red = cc_colour(255, 16, 24);
    char msg[64] = "";
    while (!done) {
        SDL_Event e;
        const cc_device *d = &g_ccdev[shown];
        int mx = -1, my = -1, button = 0, wheel = 0;
        cc_frame(1);
        cc_text(0, 110, "INPUT DEVICES", grey);
        for (k = 0; k < CC_DEVICES; k++) cc_text(0, 132 + 11 * k, g_ccdev[k].title, k == shown ? red : g_ccsel[k] ? white : grey);   /* DOS: the selected white, others grey */
        cc_text(0, 275, "Current Config:", grey);
        cc_text(8, 286, cfg->name, white);
        cc_text(0, 308, "RESET DEFAULTS", white);
        for (k = 0; k < 4; k++) { char t[24]; snprintf(t, sizeof t, "Load Custom %d", k + 1); cc_text(0, 330 + 11 * k, t, white);
                                  snprintf(t, sizeof t, "Save Custom %d", k + 1); cc_text(0, 385 + 11 * k, t, white); }
        cc_text(0, 440, "ACCEPT CONFIG AND EXIT", white);
        cc_text(0, 462, "ABORT", white);
        cc_text(175, 110, "GAME CONTROLS", grey);
        cc_text(367 - text_width(&g_fthin, LEVEL[level]) / 2, 110, LEVEL[level], white);
        for (k = 0; k < CC_CONTROLS; k++) {
            const cc_binding *b = &cfg->b[level][k];
            char t[96];
            int y = 132 + 11 * k, x = 175 + text_width(&g_fthin, CC_LABEL[k]);
            cc_text(175, y, CC_LABEL[k], k == sel ? red : grey);
            if (k <= 6) cc_text(x + 1, y, DIRL[k][(b->flags & 0x80000000u) ? 1 : 0], k == sel ? red : white);   /* 1 px after the label */
            { const char *mt = b->device >= 0 ? MODL[b->flags & 7] : "--"; cc_text(270 - text_width(&g_fthin, mt) / 2, y, mt, white); }   /* centred on 270 */
            cc_describe(b, g_ccdev, t, sizeof t);
            if (k == sel && half) cc_text(285, y, "?", white);
            else {   /* a pair: "dev a" then "/b" one pixel further (DOS capture) */
                char *sl = b->type == 2 ? strchr(t, '/') : NULL;
                if (sl) { *sl = 0; cc_text(285, y, t, white); cc_text(285 + text_width(&g_fthin, t) + 1, y, "/", white); cc_text(285 + text_width(&g_fthin, t) + 1 + text_width(&g_fthin, "/"), y, sl + 1, white); }
                else cc_text(285, y, t, white);
            }
        }
        cc_text(480, 110, d->title, grey);
        {
            int y = 132, i;
            cc_text(480, y, "Directional", grey); y += 22;
            for (i = 0; i < d->naxis; i++, y += 11) cc_text(480, y, d->axis[i].label, grey);
            y = 242;
            cc_text(480, y, "Buttons", grey); y += 22;
            for (i = scroll; i < d->nbutton && y < 470; i++, y += 11) cc_text(480, y, d->button[i].label, grey);
        }
        if (msg[0]) cc_text(176, 470, msg, red);
        f++;
        if (headless) { if (f >= 2) done = 1; continue; }
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { done = 1; break; }
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) done = 1;
            if (e.type == SDL_MOUSEWHEEL) wheel = e.wheel.y;
            if (e.type == SDL_MOUSEBUTTONDOWN) { mx = e.button.x; my = e.button.y; button = e.button.button; }
        }
        if (wheel && mx < 0) { scroll -= wheel * 3; if (scroll < 0) scroll = 0; if (scroll > d->nbutton - 10) scroll = d->nbutton > 10 ? d->nbutton - 10 : 0; }
        if (mx >= 0) {
            int row = (my - 132) / 11;
            msg[0] = 0;
            if (mx < 172) {
                if (my >= 132 && my < 132 + 11 * CC_DEVICES) { shown = row; scroll = 0; }
                else if (my >= 308 && my < 319) { cc_merge_defaults(cfg, giddi_dir(), g_ccsel); }
                else if (my >= 330 && my < 374) {
                    char p[1100];
                    snprintf(p, sizeof p, "%.1000s/CONFIG%02d.CPC", giddi_dir(), (my - 330) / 11 + 1);
                    if (cc_load(cfg, p) == 0) snprintf(g_ccname, sizeof g_ccname, "%s", cfg->name); else snprintf(msg, sizeof msg, "No configuration saved there.");
                } else if (my >= 385 && my < 429) {
                    char p[1100], m[64];
                    int n = (my - 385) / 11 + 1;
                    snprintf(cfg->name, sizeof cfg->name, "Custom Config #%d", n);
                    snprintf(p, sizeof p, "%.1000s/CONFIG%02d.CPC", giddi_dir(), n);
                    if (cc_save(cfg, p) == 0) { snprintf(m, sizeof m, "Configuration %d Saved.#Ok", n); shell_message(rd, tx, headless, m); }
                } else if (my >= 440 && my < 451) { if (cc_write_inputmap(rd, tx, headless, cfg) == 0) { snprintf(g_ccname, sizeof g_ccname, "%s", cfg->name); done = 1; } }
                else if (my >= 462 && my < 473) done = 1;
            } else if (mx < 477) {
                if (my >= 110 && my < 121) level = (level + 1) % CC_LEVELS;
                else if (row >= 0 && row < CC_CONTROLS && my >= 132) {
                    cc_binding *b = &cfg->b[level][row];
                    int lw = 175 + text_width(&g_fthin, CC_LABEL[row]);
                    if (button == SDL_BUTTON_RIGHT) { b->device = -1; b->flags = 0; sel = -1; }
                    else if (mx >= 250 && mx < 285) { int m = (int)(b->flags & 7); m = m == 0 ? 1 : m == 1 ? 2 : m == 2 ? 4 : 0; b->flags = (b->flags & ~7u) | (uint32_t)m; }
                    else if (row <= 6 && mx >= lw && mx < lw + 24) b->flags ^= 0x80000000u;
                    else { sel = row; half = 0; }
                }
            } else if (sel >= 0) {   /* an input of the shown device */
                cc_binding *b = &cfg->b[level][sel];
                int y = 154, i, hit = -1, axis = 0;
                for (i = 0; i < d->naxis; i++, y += 11) if (my >= y && my < y + 11) { hit = i; axis = 1; }
                y = 264;
                for (i = scroll; i < d->nbutton && y < 470; i++, y += 11) if (my >= y && my < y + 11) { hit = i; axis = 0; }
                if (hit >= 0) {
                    if (axis && sel <= 6) { b->type = 0; b->device = shown; b->a = hit; b->b = 0; sel = -1; }
                    else if (!axis && sel <= 6) {
                        if (!half) { b->type = 2; b->device = shown; b->a = hit; b->b = hit; half = 1; }
                        else { b->b = hit; half = 0; sel = -1; }
                    } else if (!axis) { b->type = 1; b->device = shown; b->a = hit; b->b = shown == 2 ? -1 : 0; sel = -1; }
                    else snprintf(msg, sizeof msg, "That control takes a button.");
                }
            }
        }
        cc_present(rd, tx);
    }
}

static void cockpit_controls(SDL_Renderer *rd, SDL_Texture *tx, int headless)
{
    int done = 0, k, f = 0, prev_bg = g_bg_entry;
    int white = -1, grey, red;
    cc_config cfg;
    load_bg(3);
    g_cclogo = open_smk("AMWLOGO1");
    g_cclogo_t = 0;
    grey = cc_colour(154, 154, 154); red = cc_colour(255, 16, 24);
    cc_init();
    if (getenv("MW2_CC_EDITOR")) { cc_merge_defaults(&cfg, giddi_dir(), g_ccsel); cc_editor(rd, tx, headless, &cfg); if (g_cclogo) smk_close(g_cclogo); g_cclogo = NULL; load_bg(prev_bg); return; }   /* tests */
    while (!done) {
        SDL_Event e;
        int mx = -1, my = -1;
        char t[96];
        cc_frame(1);
        snprintf(t, sizeof t, "Current Config: %s", g_ccname);
        cc_text(0, 110, t, grey);
        cc_text(0, 132, "SELECT INPUT DEVICES", grey);
        for (k = 0; k < CC_DEVICES; k++) cc_text(0, 154 + 11 * k, g_ccdev[k].title, g_ccsel[k] ? red : white);
        cc_text(0, 297, "ACCEPT", white);
        cc_text(0, 319, "CUSTOM CONFIGURATION", white);
        cc_text(0, 440, "ABORT", white);
        f++;
        if (headless) { if (f >= 2) done = 1; continue; }
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) done = 1;
            if (e.type == SDL_MOUSEBUTTONDOWN) { mx = e.button.x; my = e.button.y; }
        }
        if (mx >= 0 && mx < 172) {
            if (my >= 154 && my < 154 + 11 * CC_DEVICES) { k = (my - 154) / 11; if (g_ccdev[k].present || k == 2) g_ccsel[k] = !g_ccsel[k]; }
            else if (my >= 297 && my < 308) {
                if (cc_merge_defaults(&cfg, giddi_dir(), g_ccsel) == 0 && cc_write_inputmap(rd, tx, headless, &cfg) == 0) { snprintf(g_ccname, sizeof g_ccname, "Default Config"); done = 1; }
            } else if (my >= 319 && my < 330) {
                cc_merge_defaults(&cfg, giddi_dir(), g_ccsel);
                snprintf(cfg.name, sizeof cfg.name, "%s", "Default Config");
                cc_editor(rd, tx, headless, &cfg);
                done = 1;
            } else if (my >= 440 && my < 451) done = 1;
        }
        cc_present(rd, tx);
    }
    if (g_cclogo) smk_close(g_cclogo);
    g_cclogo = NULL;
    load_bg(prev_bg);
}

static int global_menu(SDL_Renderer *rd, SDL_Texture *tx, int headless)
{
    static const char *ITEM[6] = {"NEW ALLEGIANCE", "COMBAT VARIABLES", "COCKPIT CONTROLS", "HALL OF HONOR", "THE KESHIK", "FLEE TO DOS"};
    uint8_t saved[W * H * 3];
    int sel = 0, done = 0, result = -1, mx2 = -1, my2 = -1, i, n0 = g_ntext, bg0 = g_bg_entry;
    memcpy(saved, g_fb, sizeof saved);
    while (!done) {
        SDL_Event e;
        int x, y, choose = -1;
        memcpy(g_fb, saved, sizeof saved);
        g_ntext = n0;   /* the screen's own text, then the panel over it */
        db_shp(6, 198, 116);   /* the panel: shell image 7 */
        text_layer_cull(198, 116, 442, 364);
        for (i = 0; i < 6; i++) {   /* items 0x7eefc: centred on x 320, y 157 + 33 k; grey (shell font 0x1f), the selected white (0x1e) */
            const font *fo = i == sel ? &g_fbig : &g_fhead;
            int tw = text_width(fo, ITEM[i]);
            (void)x; (void)y;
            draw_text(fo, 320 - tw / 2, 157 + 33 * i - font_height(fo) / 2, ITEM[i], (const uint8_t (*)[3])g_bg.pal, -1);
        }
        if (headless) {   /* tests: MW2_SHELL_MENU=item selected (and MW2_MENU_KESHIK=1 runs The Keshik) -> MW2_SHELL_SHOT */
            if (getenv("MW2_SHELL_MENU") && getenv("MW2_SHELL_SHOT")) {
                FILE *o;
                if (getenv("MW2_MENU_KESHIK")) the_keshik(rd, tx, headless);
                if (getenv("MW2_MENU_COCKPIT")) cockpit_controls(rd, tx, headless);
                if (getenv("MW2_MENU_FLEE")) shell_dialog(rd, tx, headless, "Embrace cowardice?#Yes|No", 1);
                if (getenv("MW2_MENU_CV")) combat_variables(rd, tx, headless);
                else if (sel != atoi(getenv("MW2_SHELL_MENU"))) { sel = atoi(getenv("MW2_SHELL_MENU")); continue; }
                if ((o = fopen(getenv("MW2_SHELL_SHOT"), "wb"))) { fprintf(o, "P6\n%d %d\n255\n", W, H); fwrite(g_fb, 1, sizeof g_fb, o); fclose(o); }
                exit(0);
            }
            break;
        }
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { result = -3; done = 1; }
            if (e.type == SDL_MOUSEMOTION) { mx2 = e.motion.x; my2 = e.motion.y; for (i = 0; i < 6; i++) if (mx2 >= 198 && mx2 < 442 && my2 >= 157 - 16 + 33 * i && my2 < 157 + 16 + 33 * i) sel = i; }
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE) done = 1;
                if (e.key.keysym.sym == SDLK_UP) sel = (sel + 5) % 6;
                if (e.key.keysym.sym == SDLK_DOWN) sel = (sel + 1) % 6;
                if (e.key.keysym.sym == SDLK_RETURN) choose = sel;
            }
            if (e.type == SDL_MOUSEBUTTONDOWN) {
                if (e.button.x >= 198 && e.button.x < 442 && e.button.y >= 116 && e.button.y < 364) choose = sel; else done = 1;
            }
        }
        if (choose == 0) { result = 8; done = 1; }
        else if (choose == 1) { combat_variables(rd, tx, headless); memcpy(g_fb, saved, sizeof saved); done = 1; result = -2; }
        else if (choose == 2) { cockpit_controls(rd, tx, headless); memcpy(g_fb, saved, sizeof saved); }
        else if (choose == 3) { hall_of_honor(rd, tx, headless); memcpy(g_fb, saved, sizeof saved); }
        else if (choose == 4) { the_keshik(rd, tx, headless); memcpy(g_fb, saved, sizeof saved); }
        else if (choose == 5) {   /* FLEE TO DOS: "Embrace cowardice?#Yes|No", No by default */
            if (shell_dialog(rd, tx, headless, "Embrace cowardice?#Yes|No", 1) == 0) { result = -3; done = 1; }
            else memcpy(g_fb, saved, sizeof saved);
        }
        if (!done) {
            SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
            SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
            SDL_Delay(10);
        }
    }
    memcpy(g_fb, saved, sizeof saved);
    /* the screen underneath gets its own background back (Combat Variables / Cockpit Controls loaded theirs, and the
     * screen redrawing its changed parts over the wrong one left patches of it) */
    if (g_bg_entry != bg0 && bg0 >= 0) load_bg(bg0);
    memcpy(g_fb, saved, sizeof saved);
    g_ntext = n0;
    return result == -2 ? -1 : result;
}


/* ---- Cadet Training (screen 14, FUN_0003d980): hotspots 0x7f23c + clan x 16 (7; background slot 13 / 20):
 * 0 CLAN HALL (5,149)-(57,440) -> 1; 1-6 (450, 13 + 25 k)-(629, ...) "<~" labels NAV COMPUTER, MECH HANDLING,
 * WEAPONS USAGE, HUNTING, INSPECTION, TRIAL -> the training scenarios tnw1-6 / tnj1-6; clips awotrnwn (453,0)
 * (the menu panel) then the instructor awotrnwa / awotrnwb alternating at (72,224) (Falcon ajftrn*) */
static const char *TRN_LABEL[6] = {"NAV COMPUTER", "MECH HANDLING", "WEAPONS USAGE", "HUNTING", "INSPECTION", "TRIAL"};
static int launch_scenario(int clan, const char *scen, const char *pilot)
{
    g_load_clan = clan;
    snprintf(g_load_scene, sizeof g_load_scene, "%s", scen);
    const char *save = getenv("MW2_SAVE_DIR");
    char line[4200], up[16];
    int k, rc;
    if (!save) save = g_dir;
    /* no star files: the original's launch (FUN_000375a0) skips FUN_0003abb0 -> FUN_0001d540 (USERSTAR / EN01..05STAR)
     * for screen 14, and the training scenarios carry the cadet's own mech in their tree (TNx#USS1, the GPS with
     * +0x0e == 0) - the sim takes the player from there, so whatever USERSTAR.BWD is on disk stays untouched */
    snprintf(up, sizeof up, "%s", scen);
    for (k = 0; up[k]; k++) if (up[k] >= 'a' && up[k] <= 'z') up[k] = (char)(up[k] - 32);
    printf("launch: training %s\n", up);
    unsetenv("MW2_OF"); unsetenv("MW2_OE");
    setenv("MW2_PILOT", pilot, 1);
    sim_command(line, sizeof line, up, save);
    sfx_lock(g_sfx); gus_song_close(g_song); g_song = NULL; mt32_song_close(g_msong); g_msong = NULL; g_song_entry = -1; sfx_unlock(g_sfx);   /* the menu music stops for the mission (the MT-32 one too) */
    sfx_stop_loops(g_sfx);
    if (g_win) SDL_HideWindow(g_win);
    loading_screen(g_load_clan);
    rc = system(line);
    read_results(save);
    sim_returned(rc);
    if (g_win) { SDL_ShowWindow(g_win); SDL_RaiseWindow(g_win); }
    return 0;
}
/* an overlay clip's own sound (e.g. the training hall's panel switching on, AWOTRNWN: 3 s at 22 kHz): decoded once
 * and played as the clip starts; the buffer lives until the next call */
static void clip_sound(const smk *s, int headless)
{
    static unsigned char *snd;
    int16_t *a = NULL;
    size_t ns = 0, i;
    int rate = 0, ch = 1;
    if (!s || headless || !g_sfx) return;
    if (snd) { sfx_stop_pcm(g_sfx, snd); free(snd); snd = NULL; }
    if (smk_audio(s, &a, &ns, &rate, &ch) == 0 && ns > 0 && (snd = malloc(ns / (size_t)ch + 1))) {
        for (i = 0; i < ns / (size_t)ch; i++) {
            int v = ch == 2 ? ((int)a[2 * i] + (int)a[2 * i + 1]) / 2 : a[i];
            snd[i] = (unsigned char)((v >> 8) + 128);
        }
        sfx_play_pcm(g_sfx, snd, (int)(ns / (size_t)ch), rate, 1.0f, 0);
    }
    free(a);
}
static int run_cadet_training(SDL_Renderer *rd, SDL_Texture *tx, int headless, int frames, int clan, int *mx, int *my)
{
    int next = -1, f = 0, i, reg, alt = 0, menu_done = 0;
    smk *panel, *inst;
    reg_load();
    reg = reg_selected(clan);
    load_bg(clan == 1 ? 19 : 12);
    panel = open_smk(clan == 1 ? "AJFTRNWN" : "AWOTRNWN");
    clip_sound(panel, headless);   /* the panel switching on */
    inst = open_smk(clan == 1 ? "AJFTRNWA" : "AWOTRNWA");
    while (next == -1) {
        SDL_Event e;
        const uint8_t (*pal)[3] = (const uint8_t (*)[3])g_bg.pal;
        int hover = -1;
        if (!headless) {
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) { next = -3; break; }
                if (e.type == SDL_MOUSEMOTION) { *mx = e.motion.x; *my = e.motion.y; }
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) next = 1;
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    int x = e.button.x, y = e.button.y;
                    if (x >= 5 && x <= 57 && y >= 149 && y <= 440) next = 1;
                    for (i = 0; i < 6; i++)
                        if (menu_done && x >= 450 && x <= 629 && y >= 13 + 25 * i && y <= 37 + 25 * i) {
                            char scen[16];
                            snprintf(scen, sizeof scen, "%s%dSCN1", clan == 1 ? "tnj" : "tnw", i + 1);
                            launch_scenario(clan, scen, reg >= 0 ? g_reg[reg].name : "Cadet");
                            load_bg(clan == 1 ? 19 : 12);
                        }
                }
            }
        }
        draw_bg();
        if (panel) {   /* the menu panel slides in once; its last frame stays */
            if (!menu_done) { int fi = smk_next(panel); if (fi < 0 || fi >= smk_frames(panel) - 1) menu_done = 1; }
            blit_smk(panel, clan == 1 ? 416 : 453, 0);
        } else menu_done = 1;
        if (inst) {   /* the instructor: a and b alternate */
            int fi = smk_next(inst);
            if (fi >= smk_frames(inst) - 1) { smk_close(inst); alt = 1 - alt; inst = open_smk(clan == 1 ? (alt ? "AJFTRNWB" : "AJFTRNWA") : (alt ? "AWOTRNWB" : "AWOTRNWA")); if (inst) smk_next(inst); }
            if (inst) blit_smk(inst, 72, 224);
        }
        if (*mx >= 5 && *mx <= 57 && *my >= 149 && *my <= 440) hover = 0;
        if (menu_done)
            for (i = 0; i < 6; i++) draw_text(&g_flabel, 540 - text_width(&g_flabel, TRN_LABEL[i]) / 2, 18 + 25 * i, TRN_LABEL[i], pal, -1);
        if (hover == 0) draw_text(&g_flabel, 8, 203, "CLAN HALL", pal, -1);
        f++;
        if (headless && getenv("MW2_SHELL_TRAIN") && f == 1) {   /* TEST ONLY: launch training item 1-6 as its button does */
            int it = atoi(getenv("MW2_SHELL_TRAIN"));
            if (it >= 1 && it <= 6) {
                char scen[16];
                snprintf(scen, sizeof scen, "%s%dSCN1", clan == 1 ? "tnj" : "tnw", it);
                launch_scenario(clan, scen, reg >= 0 ? g_reg[reg].name : "Cadet");
            }
        }
        if (headless) { if (f >= frames) next = -2; }
        else {
            SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
            SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
            SDL_Delay(66);
        }
    }
    smk_close(panel); smk_close(inst);
    return next;
}


/* ---- the Archive Holoprojector (screen 5, FUN_00026b50): ARCHWO.MW2 / ARCHJF.MW2 = u32 count, u32 offsets[count + 1],
 * pages of tagged fields: 00 01 title; 00 04 <target page> 00 <name> per link; 00 02 text (\n \t markup, \aNN = the
 * next word links to this page's link NN); 00 ff end. Background: the holo-room (entry 11 / 18); the text box and
 * the link highlighting are approximations of the footage */
typedef struct { char word[40]; int link, nl, x, y, w; } arch_tok;
static int run_archive(SDL_Renderer *rd, SDL_Texture *tx, int headless, int frames, int clan, int *mx, int *my)
{
    char path[700];
    uint8_t *d = NULL;
    long len = 0;
    FILE *f;
    int page = 0, hist[64], nh = 0, next = -1, fr = 0, top = 0;
    snprintf(path, sizeof path, "%s", gpath(clan == 1 ? "ARCHJF.MW2" : "ARCHWO.MW2"));
    if ((f = fopen(path, "rb"))) { fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET); d = malloc((size_t)len); if (d && fread(d, 1, (size_t)len, f) != (size_t)len) len = 0; fclose(f); }
    if (!d || len < 8) { free(d); return 1; }
    load_bg(clan == 1 ? 18 : 11);
    while (next == -1) {
        static arch_tok tok[3000];
        int nt = 0, links[64], nl = 0, i, n = (int)(d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24);
        char title[64] = "";
        const int bx = 120, by = 70, bw = 400, bh = 320, lh = 12;
        {   /* parse the page */
            uint32_t a = 0, b = 0;
            if (page >= 0 && page < n && 8 + page * 4 + 4 <= len) {
                a = d[4 + page * 4] | d[5 + page * 4] << 8 | d[6 + page * 4] << 16 | (uint32_t)d[7 + page * 4] << 24;
                b = d[8 + page * 4] | d[9 + page * 4] << 8 | d[10 + page * 4] << 16 | (uint32_t)d[11 + page * 4] << 24;
            }
            if (b > (uint32_t)len) b = (uint32_t)len;
            while (a + 2 <= b) {
                int tag = d[a + 1];
                a += 2;
                if (tag == 0xff) break;
                if (tag == 1) { snprintf(title, sizeof title, "%s", (const char *)d + a); a += (uint32_t)strlen((const char *)d + a) + 1; }
                else if (tag == 4) { if (nl < 64) links[nl++] = d[a]; a += 2; a += (uint32_t)strlen((const char *)d + a) + 1; }
                else if (tag == 2) {   /* the text, into words */
                    const char *t = (const char *)d + a;
                    size_t L = strlen(t), q = 0;
                    int pending = -1, x = 0, y = 0;
                    while (q < L && nt < 3000) {
                        if (t[q] == '\\' && q + 1 < L) {
                            char c = t[q + 1];
                            if (c == 'n') { x = 0; y++; q += 2; continue; }
                            if (c == 't') { x = (x / 64 + 1) * 64; q += 2; continue; }
                            if (c == 'a' && q + 3 < L) { pending = (t[q + 2] - '0') * 10 + (t[q + 3] - '0'); q += 4; continue; }
                            q += 2; continue;
                        }
                        if (t[q] == '\n' || t[q] == '\r' || t[q] == ' ') { if (t[q] == ' ') x += 5; q++; continue; }
                        {
                            arch_tok *k = &tok[nt];
                            int wl = 0;
                            while (q < L && t[q] != ' ' && t[q] != '\n' && t[q] != '\\' && wl < 39) k->word[wl++] = t[q++];
                            k->word[wl] = 0;
                            k->w = text_width(&g_flist, k->word);
                            if (x + k->w > bw - 8) { x = 0; y++; }
                            k->x = x; k->y = y; k->link = pending; pending = -1;
                            x += k->w;
                            nt++;
                        }
                    }
                    a += (uint32_t)L + 1;
                } else a += (uint32_t)strlen((const char *)d + a) + 1;   /* other fields: skipped */
            }
        }
        if (!headless) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) { next = -3; break; }
                if (e.type == SDL_MOUSEMOTION) { *mx = e.motion.x; *my = e.motion.y; }
                if (e.type == SDL_MOUSEWHEEL) top -= e.wheel.y * 3;
                if (e.type == SDL_KEYDOWN) {
                    if (e.key.keysym.sym == SDLK_ESCAPE) next = 1;
                    if (e.key.keysym.sym == SDLK_DOWN || e.key.keysym.sym == SDLK_PAGEDOWN) top += e.key.keysym.sym == SDLK_DOWN ? 1 : bh / lh - 2;
                    if (e.key.keysym.sym == SDLK_UP || e.key.keysym.sym == SDLK_PAGEUP) top -= e.key.keysym.sym == SDLK_UP ? 1 : bh / lh - 2;
                    if (e.key.keysym.sym == SDLK_BACKSPACE && nh) { page = hist[--nh]; top = 0; }
                }
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    int x = e.button.x, y = e.button.y;
                    if (y >= 440 && x >= 60 && x < 200) { if (nh) { page = hist[--nh]; top = 0; } else next = 1; }   /* BACK */
                    else if (y >= 440 && x >= 440 && x < 600) top += bh / lh - 2;                                     /* NEXT PAGE */
                    else for (i = 0; i < nt; i++) {
                        const arch_tok *k = &tok[i];
                        int ky = by + (k->y - top) * lh;
                        if (k->link >= 0 && k->link < nl && x >= bx + k->x && x < bx + k->x + k->w && y >= ky && y < ky + lh) {
                            if (nh < 64) hist[nh++] = page;
                            page = links[k->link]; top = 0;
                            break;
                        }
                    }
                }
            }
        }
        if (top < 0) top = 0;
        draw_bg();
        {
            const uint8_t (*pal)[3] = (const uint8_t (*)[3])g_bg.pal;
            int x, y;
            for (y = by - 30; y < by + bh + 6; y++) for (x = bx - 10; x < bx + bw + 10; x++) { uint8_t *q = g_fb + ((size_t)y * W + (size_t)x) * 3; q[0] = (uint8_t)(q[0] / 3 + 60); q[1] /= 3; q[2] = (uint8_t)(q[2] / 3 + 60); }
            draw_text(&g_flabel, 320 - text_width(&g_flabel, title) / 2, by - 24, title, pal, -1);
            for (i = 0; i < nt; i++) {
                const arch_tok *k = &tok[i];
                int ky = by + (k->y - top) * lh;
                if (ky < by || ky > by + bh - lh) continue;
                draw_text(&g_flist, bx + k->x, ky, k->word, pal, -1);
                if (k->link >= 0) for (x = bx + k->x; x < bx + k->x + k->w; x++) { uint8_t *q = g_fb + ((size_t)(ky + lh - 1) * W + (size_t)x) * 3; q[0] = 120; q[1] = 255; q[2] = 120; }   /* a link */
            }
            draw_text(&g_flabel, 130 - text_width(&g_flabel, "BACK") / 2, 450, "BACK", pal, -1);
            draw_text(&g_flabel, 520 - text_width(&g_flabel, "NEXT PAGE") / 2, 450, "NEXT PAGE", pal, -1);
        }
        fr++;
        if (headless) { if (fr >= frames) next = -2; }
        else {
            SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
            SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
            SDL_Delay(15);
        }
    }
    free(d);
    return next;
}

/* ---- the briefing (screen 0, FUN_0001c080): <mission 4 letters>BRF1 (BWD, ORDR text: \c centre, \n, \t);
 * text box Wolf (88,30) 454x400, Falcon (98,51) 403x372; hotspots 0x7f1dc + clan x 16 -> 0x7dac0 / 0x7e428:
 * 0 ABORT (430,450)-(529,474) -> 11, 1 SITUATION (110,450)-(209,474), 2 LAUNCH (270,450)-(369,474),
 * 3 SKIP (540,450)-(639,474); background slot 16 / 23 */
static const int BRF_HS[4][4] = {{430, 450, 529, 474}, {110, 450, 209, 474}, {270, 450, 369, 474}, {540, 450, 639, 474}};
static const char *BRF_LABEL[4] = {"ABORT", "SITUATION", "LAUNCH", "SKIP"};
/* SKIP (FUN_0001c080): the hotspot count is set to 3 unless the selected pilot's name is exactly "FERRARI" (a
 * developers' switch); SKIP returns screen 3, the debriefing, without flying the mission */
static int brf_skip(int reg) { return reg >= 0 && strcmp(g_reg[reg].name, "FERRARI") == 0; }
static const int BRF_LX[4] = {480, 160, 320, 590};
static int brf_lines(const char *txt, size_t len, char lines[][96], int *centre, int maxl, int width)
{
    int n = 0;
    size_t i = 0;
    char cur[96];
    int cl = 0, cen = 0;
    while (i < len && n < maxl) {
        if (txt[i] == '\\' && i + 1 < len) {
            char c = txt[i + 1];
            i += 2;
            if (c == 'n') { cur[cl] = 0; snprintf(lines[n], 96, "%s", cur); centre[n++] = cen; cl = 0; cen = 0; if (n < maxl) { lines[n][0] = 0; centre[n++] = 0; } }
            else if (c == 'c') cen = 1;
            else if (c == 't' && cl < 90) { do cur[cl++] = ' '; while (cl % 4 && cl < 90); }
            continue;
        }
        if (txt[i] == '\r' || txt[i] == '\n') { if (cl && cur[cl - 1] != ' ' && cl < 94) cur[cl++] = ' '; i++; continue; }
        if (cl < 94) cur[cl++] = txt[i];
        i++;
        cur[cl] = 0;
        if (text_width(&g_flist, cur) > width) {   /* wrap at the last space */
            int sp = cl - 1;
            while (sp > 0 && cur[sp] != ' ') sp--;
            if (sp <= 0) sp = cl - 1;
            {
                char rest[96];
                snprintf(rest, sizeof rest, "%s", cur + sp + 1);
                cur[sp] = 0;
                snprintf(lines[n], 96, "%s", cur); centre[n++] = cen;
                snprintf(cur, sizeof cur, "%s", rest); cl = (int)strlen(cur);
            }
        }
    }
    if (cl && n < maxl) { cur[cl] = 0; snprintf(lines[n], 96, "%s", cur); centre[n++] = cen; }
    return n;
}
static int run_briefing(SDL_Renderer *rd, SDL_Texture *tx, int headless, int frames, int clan, int *mx, int *my)
{
    static char lines[600][96];
    static int centre[600];
    int next = -1, f = 0, i, n = 0, top = 0, reg, m;
    int bx = clan == 1 ? 98 : 88, by = clan == 1 ? 51 : 30, bw = clan == 1 ? 403 : 454, bh = clan == 1 ? 372 : 400, lh = 12;
    char msg[96] = "";
    reg_load();
    reg = reg_selected(clan);
    if (reg < 0) return 12;
    m = g_reg[reg].rank;
    if (m > 15) return 1;
    {   /* the briefing text */
        char err[256], path[700], rec[16];
        prj_archive *a;
        prj_record r;
        snprintf(path, sizeof path, "%s", gpath("MW2.PRJ"));
        snprintf(rec, sizeof rec, "%.4sBRF1", CAMPAIGN_SCEN[clan][m]);
        for (i = 0; rec[i]; i++) if (rec[i] >= 'a' && rec[i] <= 'z') rec[i] = (char)(rec[i] - 32);
        if ((a = prj_open(path, err, sizeof err))) {
            if (prj_read_named(a, "BWD", rec, &r) == PRJ_OK) {
                size_t o = 12;
                while (o + 8 <= r.size) {
                    uint32_t sz = r.data[o + 4] | r.data[o + 5] << 8 | r.data[o + 6] << 16 | (uint32_t)r.data[o + 7] << 24;
                    if (memcmp(r.data + o, "ORDR", 4) == 0) n = brf_lines((const char *)r.data + o + 8, sz - 8, lines, centre, 600, bw - 8);
                    if (sz < 8) break;
                    o += sz;
                }
                prj_record_free(&r);
            }
            prj_close(a);
        }
    }
    load_bg(clan == 1 ? 22 : 15);
    if (getenv("MW2_SHELL_AUTOLAUNCH")) { launch_campaign(clan, reg); return 3; }   /* tests: press LAUNCH once */
    while (next == -1) {
        SDL_Event e;
        int hover = -1;
        if (!headless) {
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) { next = -3; break; }
                if (e.type == SDL_MOUSEMOTION) { *mx = e.motion.x; *my = e.motion.y; }
                if (e.type == SDL_MOUSEWHEEL) top -= e.wheel.y * 3;
                if (e.type == SDL_KEYDOWN) {
                    SDL_Keycode k = e.key.keysym.sym;
                    if (k == SDLK_ESCAPE) next = 11;   /* the Ready Room */
                    if (k == SDLK_DOWN) top++;
                    if (k == SDLK_UP) top--;
                    if (k == SDLK_PAGEDOWN) top += bh / lh - 2;
                    if (k == SDLK_PAGEUP) top -= bh / lh - 2;
                }
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    int x = e.button.x, y = e.button.y;
                    for (i = 0; i < 4; i++) {
                        if (x < BRF_HS[i][0] || x > BRF_HS[i][2] || y < BRF_HS[i][1] || y > BRF_HS[i][3]) continue;
                        if (i == 0) next = 11;                             /* ABORT: the Ready Room */
                        else if (i == 1) {                                  /* SITUATION: to that part of the text */
                            int j;
                            for (j = 0; j < n; j++) if (strncmp(lines[j], "SITUATION", 9) == 0) { top = j; break; }
                        } else if (i == 2) {                                /* LAUNCH, then the debriefing */
                            launch_campaign(clan, reg);
                            next = 3;
                        } else if (i == 3 && brf_skip(reg)) next = 3;       /* SKIP: straight to the debriefing */
                    }
                }
            }
        }
        if (top > n - bh / lh) top = n - bh / lh;
        if (top < 0) top = 0;
        draw_bg();
        {
            const uint8_t (*pal)[3] = (const uint8_t (*)[3])g_bg.pal;
            char head[96];
            snprintf(head, sizeof head, "MISSION %d: %s", m + 1, CAMPAIGN_TITLE[clan][m]);
            for (i = 0; i < bh / lh && top + i < n; i++) {
                const char *ln = lines[top + i];
                int x = centre[top + i] ? bx + (bw - text_width(&g_flist, ln)) / 2 : bx + 4;
                draw_text(&g_flist, x, by + i * lh, ln, pal, -1);
            }
            for (i = 0; i < 4; i++) {
                if (i == 3 && !brf_skip(reg)) continue;   /* SKIP: only for the pilot FERRARI */
                draw_text(&g_flabel, BRF_LX[i] - text_width(&g_flabel, BRF_LABEL[i]) / 2, 455, BRF_LABEL[i], pal, -1);
            }
            if (msg[0]) draw_text(&g_flabel, 320 - text_width(&g_flabel, msg) / 2, 432, msg, pal, -1);
            (void)head; (void)hover;
        }
        f++;
        if (headless) { if (f >= frames) next = -2; }
        else {
            SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
            SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
            SDL_Delay(10);
        }
    }
    return next;
}


/* ---- the debriefing (screen 3, FUN_00023890 + scoring 0x00021ee0): MW2MSN.CFG objectives, MW2CAR.CFG combat
 * counters, MW2DIF.CFG (+4 heat tracking, +5 difficulty). Honor: completion 5000 (0 if a primary failed),
 * secondary 1500 each, tertiary 500, wingman deaths -4000, enemy mechs 250 (total), vehicles 125 (total),
 * underweight 25 per ton (not yet: Star Config), hits / shots >= 0.7: 750; x 0.8 / 1.0 / 1.3 by difficulty;
 * heat tracking off: none ("The Keshik deems it Dishonorable ..."); failure: none. Kills += mechs total +
 * vehicles direct, hits / shots accumulate always; honor and progress on success. Hotspots 0x7f17c + clan x 16:
 * EXIT (270,450)-(369,474), AFTERMATH (110,450)-(209,474), REPLAY (430,450)-(529,474); background 16 / 23. */
static const int DEB_HS[3][4] = {{270, 450, 369, 474}, {110, 450, 209, 474}, {430, 450, 529, 474}};
static const char *DEB_LABEL[3] = {"EXIT", "AFTERMATH", "REPLAY"};
static const int DEB_LX[3] = {320, 160, 480};
typedef struct { char label[64]; char value[48]; int centre; } deb_line;
static int u16at(const unsigned char *b, int o) { return b[o] | b[o + 1] << 8; }
static int brf_lines(const char *txt, size_t len, char lines[][96], int *centre, int maxl, int width);
/* a text page (AFTERMATH): ORDR of <mission><suffix>, scrollable, any button / Esc returns */
static void text_page(SDL_Renderer *rd, SDL_Texture *tx, int headless, int clan, const char *scen, const char *suffix)
{
    static char lines[600][96];
    static int centre[600];
    int n = 0, top = 0, done = 0, i;
    int bx = clan == 1 ? 98 : 88, by = clan == 1 ? 51 : 30, bw = clan == 1 ? 403 : 454, bh = clan == 1 ? 372 : 400, lh = 12;
    prj_record r;
    uint32_t len;
    const uint8_t *p;
    if (mission_record(scen, suffix, &r) == 0) {
        if ((p = chunk_find(&r, "ORDR", &len))) n = brf_lines((const char *)p, len, lines, centre, 600, bw - 8);
        prj_record_free(&r);
    }
    while (!done) {
        SDL_Event e;
        if (!headless) {
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) || e.type == SDL_MOUSEBUTTONDOWN) done = 1;
                if (e.type == SDL_MOUSEWHEEL) top -= e.wheel.y * 3;
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_DOWN) top++;
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_UP) top--;
            }
        } else done = 1;
        if (top > n - bh / lh) top = n - bh / lh;
        if (top < 0) top = 0;
        draw_bg();
        for (i = 0; i < bh / lh && top + i < n; i++) {
            const char *ln = lines[top + i];
            draw_text(&g_flist, centre[top + i] ? bx + (bw - text_width(&g_flist, ln)) / 2 : bx + 4, by + i * lh, ln, (const uint8_t (*)[3])g_bg.pal, -1);
        }
        draw_text(&g_flabel, 320 - text_width(&g_flabel, "EXIT") / 2, 455, "EXIT", (const uint8_t (*)[3])g_bg.pal, -1);
        if (!headless) {
            SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
            SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
            SDL_Delay(10);
        }
    }
}
static int run_debrief(SDL_Renderer *rd, SDL_Texture *tx, int headless, int frames, int clan, int *mx, int *my)
{
    int mission_done;
    static deb_line L[64];
    int n = 0, i, next = -1, f = 0, reg, success, total = 0, final = 0, top = 0;
    unsigned char msn[0x9d4], car[80], dif[8];
    char path[800];
    FILE *fp;
    pilot_rec before;
    int bx = clan == 1 ? 98 : 88, by = clan == 1 ? 51 : 30, bw = clan == 1 ? 403 : 454, lh = 14;
    memset(msn, 0, sizeof msn); memset(car, 0, sizeof car); memset(dif, 0, sizeof dif);
    dif[4] = 1; dif[5] = 1;
    {
        const char *dir = getenv("MW2_SAVE_DIR") ? getenv("MW2_SAVE_DIR") : g_dir;
        snprintf(path, sizeof path, "%s/MW2MSN.CFG", dir); if ((fp = fopen(path, "rb"))) { if (fread(msn, 1, sizeof msn, fp)) {} fclose(fp); }
        snprintf(path, sizeof path, "%s/MW2CAR.CFG", dir); if ((fp = fopen(path, "rb"))) { if (fread(car, 1, sizeof car, fp)) {} fclose(fp); }
        snprintf(path, sizeof path, "%s", gpath("MW2DIF.CFG")); if ((fp = fopen(path, "rb"))) { if (fread(dif, 1, sizeof dif, fp)) {} fclose(fp); }
    }
    reg_load();
    reg = reg_selected(clan);
    if (reg < 0) return 12;
    before = g_reg[reg];
    mission_done = before.rank;   /* the mission just flown (before any advance) */
    success = (msn[16] | msn[17] << 8) == 2;
#define LINE(lbl, ...) do { if (n < 64) { snprintf(L[n].label, sizeof L[n].label, "%s", lbl); snprintf(L[n].value, sizeof L[n].value, __VA_ARGS__); L[n].centre = 0; n++; } } while (0)
#define TEXT(lbl) do { if (n < 64) { snprintf(L[n].label, sizeof L[n].label, "%s", lbl); L[n].value[0] = 0; L[n].centre = 1; n++; } } while (0)
    {   /* the objectives */
        int no = msn[4] | msn[5] << 8, k, primary_failed = 0, sec = 0, ter = 0;
        static const char *TY[9] = {"Default", "Primary", "Secondary", "", "Tertiary", "", "", "", "Return"};
        TEXT("OBJECTIVES");
        for (k = 0; k < no && k < 40; k++) {
            const unsigned char *e = msn + 20 + k * 52;
            int st = e[0], ty = e[4];
            char txt[40];
            snprintf(txt, sizeof txt, "%.31s", (const char *)e + 20);
            { int q = (int)strlen(txt); while (q > 0 && txt[q - 1] == ' ') txt[--q] = 0; }
            LINE(txt, "%s %s", ty <= 8 && TY[ty][0] ? TY[ty] : "Unknown", st == 1 ? "Successful" : "Failed");
            if (ty == 1 && st == 0) primary_failed = 1;
            if (ty == 2 && st == 1) sec++;
            if (ty == 4 && st == 1) ter++;
        }
        TEXT("");
        total = primary_failed ? 0 : 5000;
        LINE("Mission Completion:", "%d", total);
        if (sec) { total += sec * 1500; LINE("Secondary Objectives Completed:", "%d x1500 = %d", sec, sec * 1500); }
        if (ter) { total += ter * 500; LINE("Tertiary Objectives Completed:", "%d x500 = %d", ter, ter * 500); }
    }
    {
        int wing = u16at(car, 0x34), md = u16at(car, 0x07), mt = u16at(car, 0x1e), vd = u16at(car, 0x44), vt = u16at(car, 0x4a);
        int shots = u16at(car, 0x13), hits = u16at(car, 0x15);
        if (wing) { total -= wing * 4000; LINE("Wingman Deaths:", "%d x-4000 = %d", wing, -wing * 4000); }
        if (mt) { total += mt * 250; LINE("Enemy Mechs Destroyed (direct / total):", "%d / %d = %d", md, mt, mt * 250); }
        if (vt) { total += vt * 125; LINE("Enemy Vehicles Destroyed (direct / total):", "%d / %d = %d", vd, vt, vt * 125); }
        if (g_last_limit > 0 && g_last_tons >= 0 && g_last_tons < g_last_limit) {   /* 25 per ton under the limit */
            int under = g_last_limit - g_last_tons;
            total += under * 25;
            LINE("Star Underweight Bonus:", "%d (x%d tons) = %d", 25, under, under * 25);
        }
        if (shots > 0) {
            double pct = (double)hits / shots;
            int bonus = pct >= 0.7 ? 750 : 0;
            total += bonus;
            LINE("Hit Percentage:", "%.1f = %d", pct * 100.0, bonus);
        }
        /* the record: kills / hits / shots always (0x00021ee0) */
        g_reg[reg].stats[0] += mt + vd;
        g_reg[reg].stats[1] += hits;
        g_reg[reg].stats[2] += shots;
    }
    TEXT("");
    if (!dif[4]) { TEXT("The Keshik deems it Dishonorable to Alter Heat Tracking"); final = 0; }
    else if (!success) { TEXT("Mission Failed:  NO HONOR ACQUIRED"); final = 0; }
    else {
        static const char *DN[3] = {"EASY", "MEDIUM", "HARD"};
        static const double DM[3] = {0.8, 1.0, 1.3};
        int d = dif[5] <= 2 ? dif[5] : 1;
        final = (int)lround(total * DM[d]);
        LINE("Mission Honor:", "%d", total);
        LINE("Difficulty Multiplier:", "(%s = %.1f) %d", DN[d], DM[d], final);
        g_reg[reg].honor += final;
        g_reg[reg].rank++;   /* the campaign advances */
        LINE("Career Honor:", "%d", g_reg[reg].honor);
    }
#undef LINE
#undef TEXT
    reg_save();
    load_bg(clan == 1 ? 22 : 15);
    while (next == -1) {
        SDL_Event e;
        if (!headless) {
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) { next = -3; break; }
                if (e.type == SDL_MOUSEMOTION) { *mx = e.motion.x; *my = e.motion.y; }
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) next = 11;
                if (e.type == SDL_MOUSEWHEEL) top -= e.wheel.y;
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    int x = e.button.x, y = e.button.y;
                    for (i = 0; i < 3; i++) {
                        if (x < DEB_HS[i][0] || x > DEB_HS[i][2] || y < DEB_HS[i][1] || y > DEB_HS[i][3]) continue;
                        if (i == 0) next = 11;                                  /* EXIT: the Ready Room */
                        else if (i == 1 && mission_done >= 0 && mission_done < 16) {   /* AFTERMATH: DBFS / DBFF */
                            text_page(rd, tx, headless, clan, CAMPAIGN_SCEN[clan][mission_done], success ? "DBFS" : "DBFF");
                            load_bg(clan == 1 ? 22 : 15);
                        }
                        else if (i == 2) { g_reg[reg] = before; reg_save(); next = 0; launch_campaign(clan, reg); next = 3; }   /* REPLAY */
                    }
                }
            }
        }
        if (top > n - 25) top = n - 25;
        if (top < 0) top = 0;
        draw_bg();
        {
            const uint8_t (*pal)[3] = (const uint8_t (*)[3])g_bg.pal;
            for (i = 0; i < 28 && top + i < n; i++) {
                const deb_line *l = &L[top + i];
                if (l->centre) draw_text(&g_flist, bx + (bw - text_width(&g_flist, l->label)) / 2, by + i * lh, l->label, pal, -1);
                else {
                    draw_text(&g_flist, bx + 4, by + i * lh, l->label, pal, -1);
                    draw_text(&g_flist, bx + bw - 6 - text_width(&g_flist, l->value), by + i * lh, l->value, pal, -1);   /* \g350 \b: right-aligned */
                }
            }
            for (i = 0; i < 3; i++) draw_text(&g_flabel, DEB_LX[i] - text_width(&g_flabel, DEB_LABEL[i]) / 2, 455, DEB_LABEL[i], pal, -1);
        }
        f++;
        if (headless) { if (f >= frames) next = -2; }
        else {
            SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
            SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
            SDL_Delay(10);
        }
    }
    return next;
}


/* a modal message ("line|line#Button"): dimmed box, centred lines, any click / key closes */
/* The shell's dialog (FUN_000263d0; the global menu's FLEE TO DOS the same): "line|line#Button|Button". Panel = shell
 * image 6 (DATABASE entry 5) frame 2, 296 x 150 at (172,165); lines centred on x 320 from y 197, 16 apart, in shell font
 * 0x20 (entry 31); n buttons centred on y 276 at x 320 - 30 (n - 1) + 61 k, each frame 0 (frame 1 while pressed) at
 * (x - 26, 263) with its label centred. Enter / Esc take the default button (the last for Yes / No, as FLEE TO DOS's
 * local default 1), a key equal to a label's first letter that button, a click a button. Returns the button. */
static void db_shp_frame(int entry, int frame, int x, int y)
{
    uint8_t *e = NULL;
    long n = shelldb_entry(&g_db, entry, &e);
    shp_frame fr;
    if (n > 0 && shp_decode(e, (size_t)n, frame, &fr) == 0) {
        int X, Y;
        for (Y = 0; Y < fr.h; Y++)
            for (X = 0; X < fr.w; X++) {
                int px = x + X, py = y + Y;
                if (!fr.mask[Y * fr.w + X] || px < 0 || py < 0 || px >= W || py >= H) continue;
                memcpy(g_fb + ((size_t)py * W + (size_t)px) * 3, g_bg.pal[fr.pix[Y * fr.w + X]], 3);
            }
        shp_frame_free(&fr);
    }
    free(e);
}
static int shell_dialog(SDL_Renderer *rd, SDL_Texture *tx, int headless, const char *msg, int def)
{
    char buf[256], *lines[8], *btn[4];
    int n = 0, nb = 0, i, chosen = -1, pressed = -1;
    char *p2;
    uint8_t *saved = malloc(sizeof g_fb);
    const uint8_t (*pal)[3] = (const uint8_t (*)[3])g_bg.pal;
    int n0 = g_ntext;
    snprintf(buf, sizeof buf, "%s", msg);
    p2 = strchr(buf, '#');
    if (p2) {
        char *b = p2 + 1;
        *p2 = 0;
        while (b && nb < 4) { btn[nb++] = b; b = strchr(b, '|'); if (b) *b++ = 0; }
    }
    p2 = buf;
    while (p2 && n < 8) { lines[n++] = p2; p2 = strchr(p2, '|'); if (p2) *p2++ = 0; }
    if (def < 0 || def >= nb) def = nb > 0 ? nb - 1 : 0;
    if (saved) memcpy(saved, g_fb, sizeof g_fb);
    while (chosen < 0) {
        SDL_Event e;
        if (saved) memcpy(g_fb, saved, sizeof g_fb);
        g_ntext = n0;
        db_shp_frame(5, 2, 172, 165);
        text_layer_cull(172, 165, 172 + 296, 165 + 150);
        for (i = 0; i < n; i++) draw_text(&g_fthin, 320 - text_width(&g_fthin, lines[i]) / 2, 197 + 16 * i, lines[i], pal, -1);
        for (i = 0; i < nb; i++) {
            int cx = 320 - 30 * (nb - 1) + 61 * i;
            db_shp_frame(5, i == pressed ? 1 : 0, cx - 26, 263);
            draw_text(&g_fthin, cx - text_width(&g_fthin, btn[i]) / 2, 276 - font_height(&g_fthin) / 2, btn[i], pal, -1);
        }
        if (headless) { chosen = def; break; }
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) chosen = def;
            if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_ESCAPE) chosen = def;
                else for (i = 0; i < nb; i++) if (toupper((unsigned char)btn[i][0]) == toupper((int)k & 0x7f)) chosen = i;
                if (nb == 0) chosen = 0;
            }
            if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
                int hit = -1;
                for (i = 0; i < nb; i++) { int cx = 320 - 30 * (nb - 1) + 61 * i; if (e.button.x >= cx - 26 && e.button.x <= cx + 26 && e.button.y >= 263 && e.button.y <= 289) hit = i; }
                if (e.type == SDL_MOUSEBUTTONDOWN) pressed = hit;
                else { if (hit >= 0 && hit == pressed) chosen = hit; pressed = -1; }
            }
        }
        SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
        SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
        SDL_Delay(10);
    }
    if (saved && !headless) { memcpy(g_fb, saved, sizeof g_fb); g_ntext = n0; }
    free(saved);
    return chosen;
}
static void shell_message(SDL_Renderer *rd, SDL_Texture *tx, int headless, const char *msg) { shell_dialog(rd, tx, headless, msg, 0); }

/* ---- the Ready Room (screen 11, FUN_00037a50): record 0x7f14c + clan x 16 (background slot 14 / 21); four hotspots,
 * twenty (the missions by codename) for the pilot named FREEBIRTHTOAD; training grid awogrid1 (303,329) /
 * ajfgrid1 (277,341); 0 CLAN HALL -> 1, 1 MECH LAB -> 9 (trial: "Trial Protocol ..."), 2 STAR CONFIG -> 13 (trial:
 * "Your 'Mech has been selected ..."), 3 MISSION BRIEFING: clip awobrief (105,100) / ajfbrief (107,105) -> 0;
 * cheat list: the pilot's campaign position = the mission picked */
typedef struct { int x0, y0, x1, y1, lx, ly; const char *label; } rr_hs;
static const rr_hs RR_HS[2][4] = {
    {{0, 0, 130, 479, 56, 223, "CLAN HALL"}, {300, 320, 559, 419, 450, 364, "MECH LAB"}, {510, 420, 559, 469, 542, 455, "STAR CONFIG"}, {140, 90, 399, 313, 265, 226, "MISSION BRIEFING"}},
    {{0, 0, 130, 479, 56, 223, "CLAN HALL"}, {280, 340, 534, 439, 415, 370, "MECH LAB"}, {432, 440, 482, 479, 464, 460, "STAR CONFIG"}, {140, 90, 399, 313, 277, 226, "MISSION BRIEFING"}}};
static const char *RR_CODENAME[2][16] = {
    {"YELLOW", "ORANGE", "TEAL", "TAUPE", "JENNY", "SABLE", "GREY", "BROWN", "AMY", "SILVER", "AQUA", "KIM", "CYAN", "MAROON", "GOLD", "IRENE"},
    {"PINK", "GREEN", "RED", "FUCHSIA", "CINDY", "RUST", "UMBER", "TAN", "HEIDI", "PLUM", "WHITE", "JILL", "PUCE", "BLONDE", "BRONZE", "MARY"}};
int campaign_is_trial(int clan, int m);
static int run_ready_room(SDL_Renderer *rd, SDL_Texture *tx, int headless, int frames, int clan, int *mx, int *my)
{
    int next = -1, f = 0, i, reg, cheat;
    smk *grid, *tbl = NULL;   /* MECH LAB: the holotable clip, then the lab */
    Uint32 tlast = 0;
    int tdone = 0;
    Uint32 last = 0;
    reg_load();
    reg = reg_selected(clan);
    if (reg < 0) return 12;
    cheat = strcmp(g_reg[reg].name, "FREEBIRTHTOAD") == 0;
    load_bg(clan == 1 ? 20 : 13);
    grid = open_smk(clan == 1 ? "AJFGRID1" : "AWOGRID1");
    while (next == -1) {
        SDL_Event e;
        int hover = -1, trial = campaign_is_trial(clan, g_reg[reg].rank);
        if (!headless) {
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) { next = -3; break; }
                if (e.type == SDL_MOUSEMOTION) { *mx = e.motion.x; *my = e.motion.y; }
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) next = 1;
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    int x = e.button.x, y = e.button.y;
                    for (i = 0; i < 4; i++) {
                        const rr_hs *h = &RR_HS[clan][i];
                        if (x < h->x0 || x > h->x1 || y < h->y0 || y > h->y1) continue;
                        if (i == 0) next = 1;
                        else if (i == 1) { if (trial) shell_message(rd, tx, headless, "Trial Protocol: X0769-Q|Keshik to determine appropriate|'Mech for trial.#Ok"); else if (!tbl) {
                            /* FUN_00037a50 case 1: the holotable clip "awo<pic>tbl" / "ajf<pic>tbl" for the star's 'Mech
                             * (0x3aad0; none: 7) at (305, 185) / (276, 164), flags 6, with the whoosh (DATABASE 0x64,
                             * volume 0x32); the room waits for the clip, stops the sound, then opens the lab */
                            char nm[16];
                            int pm;
                            star_load(clan);
                            pm = g_star[clan].slot[0].mech;
                            if (pm < 0 || pm >= 18) pm = 7;
                            snprintf(nm, sizeof nm, "%s%sTBL", clan == 1 ? "AJF" : "AWO", MECH_PIC[pm]);
                            for (pm = 3; nm[pm]; pm++) if (nm[pm] >= 'a' && nm[pm] <= 'z') nm[pm] = (char)(nm[pm] - 32);
                            g_lab_slot = 0;
                            shell_sound(0x64, 0x32);
                            if (!(tbl = open_smk(nm))) { shell_sound_stop(0x64); next = 9; }
                            else { smk_next(tbl); tlast = SDL_GetTicks(); }
                        } }
                        else if (i == 2) { if (trial) shell_message(rd, tx, headless, "Your 'Mech has been|selected for you.|Prepare for Trial!#Ok"); else next = 13; }
                        else { play_cinematic(rd, tx, clan == 1 ? "AJFBRIEF" : "AWOBRIEF", headless); next = 0; }
                    }
                    if (cheat && next == -1)
                        for (i = 0; i < 16; i++) {
                            int cx = i < 8 ? 400 : 520, cy = 25 + 30 * (i % 8);
                            if (x >= cx && x <= cx + 119 && y >= cy && y <= cy + 24) { g_reg[reg].rank = i; reg_save(); star_reset_mission(clan, i); play_cinematic(rd, tx, clan == 1 ? "AJFBRIEF" : "AWOBRIEF", headless); next = 0; }
                        }
                }
            }
        }
        draw_bg();
        if (grid) {
            Uint32 now = SDL_GetTicks();
            if (headless || now - last >= (Uint32)smk_frame_ms(grid) || f == 0) {
                smk_next(grid);
                last = now;
            }
            if (grid) blit_smk(grid, clan == 1 ? 277 : 303, clan == 1 ? 341 : 329);
        }
        if (tbl) {   /* the holotable clip plays once; the lab opens when it ends */
            Uint32 now = SDL_GetTicks();
            if (now - tlast >= (Uint32)smk_frame_ms(tbl)) {
                tlast = now;
                if (tdone) { smk_close(tbl); tbl = NULL; shell_sound_stop(0x64); next = 9; }
                else { int fi = smk_next(tbl); if (fi < 0 || fi >= smk_frames(tbl) - 1) tdone = 1; }   /* the last frame shown once */
            }
            if (tbl) blit_smk(tbl, clan == 1 ? 276 : 305, clan == 1 ? 164 : 185);
        }
        for (i = 0; i < 4; i++) {
            const rr_hs *h = &RR_HS[clan][i];
            if (*mx >= h->x0 && *mx <= h->x1 && *my >= h->y0 && *my <= h->y1) hover = i;
        }
        if (hover >= 0) {
            const rr_hs *h = &RR_HS[clan][hover];
            int tw2 = text_width(&g_flabel, h->label), lx2 = h->lx - tw2 / 2;
            if (lx2 < 4) lx2 = 4;
            if (lx2 + tw2 > W - 4) lx2 = W - 4 - tw2;
            draw_text(&g_flabel, lx2, h->ly, h->label, (const uint8_t (*)[3])g_bg.pal, -1);
        }
        if (cheat)   /* "<~" labels: always shown */
            for (i = 0; i < 16; i++)
                draw_text(&g_flabel, (i < 8 ? 460 : 580) - text_width(&g_flabel, RR_CODENAME[clan][i]) / 2, 30 + 30 * (i % 8), RR_CODENAME[clan][i], (const uint8_t (*)[3])g_bg.pal, -1);
        f++;
        if (headless) { if (f >= frames) next = -2; }
        else {
            SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
            SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
            SDL_Delay(10);
        }
    }
    smk_close(grid);
    if (tbl) smk_close(tbl);
    return next;
}

static const screen_def *find_screen(int id, int clan)
{
    size_t i;
    for (i = 0; i < sizeof SCREENS / sizeof SCREENS[0]; i++)
        if (SCREENS[i].id == id && (SCREENS[i].clan < 0 || SCREENS[i].clan == clan)) return &SCREENS[i];
    return NULL;
}

#ifdef MW2_ONE_BINARY
int glview_main(int argc, char **argv);
#endif
int main(int argc, char **argv)
{
    SDL_Window *win = NULL;
    SDL_Renderer *rd = NULL;
    SDL_Texture *tx = NULL;
    const char *shot = getenv("MW2_SHELL_SHOT");
    int headless = shot != NULL, frames = getenv("MW2_SHELL_FRAMES") ? atoi(getenv("MW2_SHELL_FRAMES")) : 30;
    int screen = getenv("MW2_SHELL_SCREEN") ? atoi(getenv("MW2_SHELL_SCREEN")) : 8, clan = getenv("MW2_SHELL_CLAN") ? atoi(getenv("MW2_SHELL_CLAN")) : 2, running = 1, mx = -1, my = -1, confirm = 0;
#ifdef MW2_ONE_BINARY
    if (argc >= 2 && strcmp(argv[1], "--sim") == 0) return glview_main(argc - 1, argv + 1);   /* the simulation */
    snprintf(g_exe_path, sizeof g_exe_path, "%s", argv[0]);
    if (!strchr(argv[0], '/')) { char *bp = SDL_GetBasePath(); snprintf(g_exe_path, sizeof g_exe_path, "%s%s", bp ? bp : "./", argv[0]); SDL_free(bp); }
#endif
    char p[700];
    {   /* the configuration: the per-user file; the game folder from the command line or found and remembered */
        char *bp = SDL_GetBasePath();
        int had_game;
        snprintf(g_exe_dir, sizeof g_exe_dir, "%s", bp ? bp : "./");
        SDL_free(bp);
        portcfg_load(NULL, &g_cfg);
        had_game = g_cfg.game[0] || g_cfg.install[0];
        if (argc >= 2) { g_cfg.game[0] = 0; g_cfg.install[0] = 0; g_cfg.cd[0] = 0; g_cfg.models[0] = 0; g_cfg.textures[0] = 0; }   /* this run: as given */
        if (portcfg_resolve(&g_cfg, g_exe_dir, argc >= 2 ? argv[1] : NULL) != 0) {
            fprintf(stderr,
                "MechWarrior 2: no game folder found.\n"
                "Run:  mw2 <folder>\n"
                "where <folder> is either the game folder (with install/, cd/, music/, ultrasnd/, 3d/ inside)\n"
                "or a complete MechWarrior 2 DOS install. It is remembered in %s.\n", portcfg_file());
            return 2;
        }
        if (!had_game && argc < 2) portcfg_save(NULL, &g_cfg);   /* found on its own: remember it */
        if (argc >= 2 && !getenv("MW2_CONFIG") && portcfg_is_game_dir(argv[1])) {   /* given: remember it too */
            portcfg keep = g_cfg;
            portcfg_load(NULL, &g_cfg);
            snprintf(g_cfg.game, sizeof g_cfg.game, "%s", keep.game[0] ? keep.game : keep.install);
            g_cfg.install[0] = 0;
            portcfg_save(NULL, &g_cfg);
            g_cfg = keep;
        }
        snprintf(g_dir, sizeof g_dir, "%s", g_cfg.install);
        dp_clear_roots();
        dp_add_root(g_cfg.install);   /* settings, pilots, saves: first (and where writes go) */
        if (g_cfg.cd[0]) {
            size_t n = strlen(g_cfg.cd);
            if (n > 4 && (strcasecmp(g_cfg.cd + n - 4, ".iso") == 0)) dp_add_iso_root(g_cfg.cd, "MECH2");
            else { char m2[1100]; snprintf(m2, sizeof m2, "%s/MECH2", g_cfg.cd); dp_add_root(m2); dp_add_root(g_cfg.cd); }
        }
    }
    snprintf(p, sizeof p, "%s", gpath("DATABASE.MW2"));
    if (shelldb_open(p, &g_db)) { fprintf(stderr, "cannot open %s\n", p); return 1; }
    g_flabel.len = shelldb_entry(&g_db, 27, &g_flabel.d);
    g_flist.len = shelldb_entry(&g_db, 25, &g_flist.d);
    g_fthin.len = shelldb_entry(&g_db, 31, &g_fthin.d);   /* the Mech Lab panels */
    g_fhead.len = shelldb_entry(&g_db, 30, &g_fhead.d);   /* the Mech Lab header */
    g_fbig.len = shelldb_entry(&g_db, 29, &g_fbig.d);     /* the roster's values */
    g_f26.len = shelldb_entry(&g_db, 26, &g_f26.d);       /* shell font 0x1b (DAT_00091174): The Keshik's headings */
    g_f28.len = shelldb_entry(&g_db, 27, &g_f28.d);       /* shell font 0x1c (DAT_00091178) */
    if (getenv("MW2_SHELL_MOUSE")) sscanf(getenv("MW2_SHELL_MOUSE"), "%d,%d", &mx, &my);
    if (getenv("MW2_LAB_MASSES")) {   /* tests: every stock design's mass as the CUSTOMIZE panel computes it */
        char err[256];
        prj_archive *a = prj_open(gpath("MW2.PRJ"), err, sizeof err);
        int t = a ? prj_find_type(a, "MEK") : -1, n = t >= 0 ? prj_symbol_count(a, t) : 0, i2, over = 0, cnt = 0;
        for (i2 = 0; i2 < n; i2++) {
            const char *nm = prj_symbol_name(a, t, i2);
            mek_def d;
            mek_mass mm;
            if (!strstr(nm, "STD") || mek_load(a, nm, &d) != 0) continue;
            if (!strncmp(nm, "DOR", 3) || !strncmp(nm, "ELE", 3) || !strncmp(nm, "TAR", 3) || !strncmp(nm, "TCK", 3) || !strncmp(nm, "TUR", 3)) continue;   /* dropship, Elemental, vehicles */
            ed_init(&g_lab_test, &d); ed_masses(&g_lab_test, &mm); cnt++;
            if (mm.used > d.tonnage + 1e-9) over++;
            printf("%-9s %3u t used %6.2f %s%s\n", nm, d.tonnage, mm.used, mm.dbl ? "DHS" : "SHS", mm.used > d.tonnage + 1e-9 ? "  OVER" : "");
        }
        printf("%d designs, %d over their tonnage\n", cnt, over);
        return over != 0;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0 && SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    g_sfx = sfx_create(NULL);
    {   /* menu music: the MT-32 (its ROMs in the game folder's mt32/, mt32= or MW2_MT32), else Gravis UltraSound
         * patches if they are there (ultrasnd= / ULTRADIR / MW2_ULTRASND) - no longer asked for */
        const char *m = getenv("MW2_MT32");
        const char *u = getenv("MW2_ULTRASND");
        if (!m && g_cfg.mt32[0]) m = g_cfg.mt32;
        if (m && g_sfx) g_mt32 = mt32_open(m, sfx_rate(g_sfx));
        if (m && !g_mt32) fprintf(stderr, "mw2: no usable MT-32 ROMs in %s (MT32_CONTROL.ROM + MT32_PCM.ROM)\n", m);
        if (!g_mt32) {
            if (!u && g_cfg.ultrasnd[0]) u = g_cfg.ultrasnd;
            if (!u) u = getenv("ULTRADIR");
            if (!u && getenv("ULTRASND") && strchr(getenv("ULTRASND"), '/')) u = getenv("ULTRASND");
            if (u) g_bank = gus_bank_open(u, 1024);
        }
        if (g_mt32 || g_bank) sfx_set_music(g_sfx, music_cb, NULL);
        fprintf(stderr, "mw2: audio %s (%s), menu music %s%s%s\n", g_sfx ? "on" : "OFF - no audio device",
                SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "none",
                g_mt32 ? "Roland MT-32 (ROMs from " : g_bank ? "Gravis UltraSound patches (" : "OFF - put the MT-32 ROMs (MT32_CONTROL.ROM, MT32_PCM.ROM) in the game folder's mt32/",
                g_mt32 ? m : g_bank ? u : "", g_mt32 || g_bank ? ")" : "");
    }
    if (!headless) {
        portcfg pc;
        Uint32 wf = SDL_WINDOW_RESIZABLE;
        portcfg_load(g_dir, &pc);
        if (pc.mode == 0) wf |= SDL_WINDOW_FULLSCREEN_DESKTOP;        /* native fullscreen (default); the 640x480 art letterboxed */
        else if (pc.mode == 2) wf |= SDL_WINDOW_FULLSCREEN_DESKTOP;   /* the shell's art is 640x480: the desktop mode serves it */
        win = g_win = SDL_CreateWindow("MechWarrior 2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W * 2, H * 2, wf);
        rd = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
        SDL_RenderSetLogicalSize(rd, W, H);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");   /* the artwork smoothly scaled; the text sharp, at full resolution */
        tx = SDL_CreateTexture(rd, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, W, H);
        g_rd = rd; g_tx = tx;
        if (!getenv("MW2_TEXT_SMOOTH")) {
            /* the Win9x look: the 640 x 480 screen scaled by whole pixels with no filtering (the art and the text in the
             * frame stay sharp), centred */
            SDL_SetTextureScaleMode(tx, SDL_ScaleModeNearest);
            SDL_RenderSetIntegerScale(rd, SDL_TRUE);
        }
        g_text_hires = rd != NULL;
        { char *bp = SDL_GetBasePath(); ttext_init(bp); SDL_free(bp); }
    }
    if (getenv("MW2_SHELL_LAB_EDIT")) {   /* tests: the editor on the slot's (or the Mad Dog's) design */
        char saved[16] = "";
        int r = run_customize(rd, tx, 1, getenv("MW2_SHELL_FRAMES") ? atoi(getenv("MW2_SHELL_FRAMES")) : 3, 0, 5, getenv("MW2_SHELL_LAB_EDIT"), saved);
        (void)r;
        { FILE *o = fopen(shot, "wb"); if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H); fwrite(g_fb, 1, sizeof g_fb, o); fclose(o); } }
        running = 0;
    }
    if (getenv("MW2_SHELL_TW")) {   /* tests: text widths in fonts 26, 27, 29, 30 */
        int e2;
        for (e2 = 24; e2 < 32; e2++) {
            font fo;
            fo.len = shelldb_entry(&g_db, e2, &fo.d);
            if (fo.len > 32 && fo.d[0] == '1' && fo.d[1] == '.') printf("font %d: '%s' %d px, height %d\n", e2, getenv("MW2_SHELL_TW"), text_width(&fo, getenv("MW2_SHELL_TW")), fo.d[8]);
            free(fo.d);
        }
        running = 0;
    }
    if (getenv("MW2_SHELL_GLYPHS")) {   /* tests: font 31's codes 1-31 */
        int c2;
        load_bg(14); draw_bg();
        for (c2 = 1; c2 < 32; c2++) { char t[8]; snprintf(t, sizeof t, "%c", c2); raster_text(&g_fthin, 20 + (c2 % 16) * 30, 100 + (c2 / 16) * 40, t, (const uint8_t (*)[3])g_bg.pal, -1); snprintf(t, sizeof t, "%d", c2); raster_text(&g_fthin, 20 + (c2 % 16) * 30, 115 + (c2 / 16) * 40, t, (const uint8_t (*)[3])g_bg.pal, 6); }
        { FILE *o = fopen(shot, "wb"); if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H); fwrite(g_fb, 1, sizeof g_fb, o); fclose(o); } }
        running = 0;
    }
    if (getenv("MW2_SHELL_FONTTEST")) {   /* tests: every font entry, on the Mech Lab background */
        int e2, y = 10;
        load_bg(14); draw_bg();
        for (e2 = 0; e2 < 40 && y < 470; e2++) {
            font fo;
            char lbl[80];
            fo.len = shelldb_entry(&g_db, e2, &fo.d);
            if (fo.len < 32 || fo.d[0] != '1' || fo.d[1] != '.') { free(fo.d); continue; }
            snprintf(lbl, sizeof lbl, "%d: Pulse Laser (Large) #1  Engine 9.50 T", e2);
            raster_text(&fo, 10, y, lbl, (const uint8_t (*)[3])g_bg.pal, -1);
            y += fo.d[8] + 6;
            free(fo.d);
        }
        { FILE *o = fopen(shot, "wb"); if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H); fwrite(g_fb, 1, sizeof g_fb, o); fclose(o); } }
        running = 0;
    }
    if (getenv("MW2_SHELL_CV_LIVE") && rd) combat_variables(rd, tx, 0);
    if (getenv("MW2_SHELL_MENU_LIVE") && rd) { load_bg(g_bg_entry < 0 ? 0 : g_bg_entry); global_menu(rd, tx, 0); }   /* tests: the menu through the renderer (with MW2_SHELL_GRAB) */   /* tests: through the renderer (with MW2_SHELL_GRAB) */
    if (!g_text_hires) { char *bp = SDL_GetBasePath(); ttext_init(bp); SDL_free(bp); }
    if (getenv("MW2_SHELL_CV")) { combat_variables(rd, tx, 1); { FILE *o = fopen(shot, "wb"); if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H); fwrite(g_fb, 1, sizeof g_fb, o); fclose(o); } } running = 0; }   /* tests */
    if (getenv("MW2_SHELL_HOH")) { reg_load(); hall_of_honor(rd, tx, 1); { FILE *o = fopen(shot, "wb"); if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H); fwrite(g_fb, 1, sizeof g_fb, o); fclose(o); } } running = 0; }   /* tests */
    if (getenv("MW2_LOADING_SHOT")) { loading_screen(0); running = 0; }   /* tests */
    /* the opening movie (main 0x36700: "mintro" unless started as "mw2 sim" - the return from a mission) */
    if (running && rd && !shot && !getenv("MW2_NO_INTRO") && !(argc > 1 && strcasecmp(argv[argc - 1], "sim") == 0))
        play_cinematic(rd, tx, "MINTRO", 0);
    while (running && screen != -3) {
        const screen_def *sd = find_screen(screen, clan);
        {   /* the screen we came from (the Mech Lab's ACCEPT MECH returns there - FUN_000339e0 case 7: EBX) */
            static int cur = -99;
            if (screen != cur) { g_from_screen = cur == -99 && getenv("MW2_SHELL_FROM") ? atoi(getenv("MW2_SHELL_FROM")) : cur; cur = screen; }   /* tests: MW2_SHELL_FROM = the screen before the first */
        }
        smk *ov = NULL, *xov[8] = {0};
        Uint32 xlast[8] = {0};
        const char *pending_video = NULL;
        int next = -1, nclan = clan, f = 0;
        Uint32 last = SDL_GetTicks();
        if (screen == 16) {   /* the ending (mend / mend2), then the clan hall */
            play_cinematic(rd, tx, clan == 1 ? "MEND2" : "MEND", headless);
            screen = 1;
            continue;
        }
        if (screen == 15) {   /* clan landing cinematic, then the clan hall (screen 1) */
            play_cinematic(rd, tx, clan == 1 ? "MJFLAND" : "MWOLAND", headless);
            screen = 1;
            continue;
        }
        apply_music(screen, clan);
        if (screen == 11 && clan >= 0 && clan <= 1) {   /* the hall's READY ROOM once all 16 missions are won */
            int rg;
            reg_load();
            rg = reg_selected(clan);
            if (rg >= 0 && g_reg[rg].rank >= 16) {
                load_bg(clan == 1 ? 17 : 10); draw_bg();
                shell_message(rd, tx, headless, "This pilot has|already won the game.#Finale");
                screen = 16;
                continue;
            }
        }
        if (screen == 11 && clan >= 0 && clan <= 1 && (g_from_screen == 1 || g_from_screen == 3)) {   /* 0x36700 case 0xb: from
                                                                                                   * the hall / debriefing the star is the mission's own */
            int rg;
            reg_load();
            rg = reg_selected(clan);
            if (rg >= 0) star_reset_mission(clan, g_reg[rg].rank);
        }
        if (screen == 1 && clan >= 0 && clan <= 1) {   /* first visit with no pilot: registration clip, then the roster */
            reg_load();
            if (reg_selected(clan) < 0 && !getenv("MW2_SHELL_NOREG")) {
                play_cinematic(rd, tx, clan == 1 ? "AJFRGSTR" : "AWORGSTR", headless);
                screen = 12;
            }
        }
        if (screen == 5 && clan >= 0 && clan <= 1) {   /* the Archive Holoprojector */
            int r = run_archive(rd, tx, headless, frames, clan, &mx, &my);
            if (r == -2) {
                FILE *o = fopen(shot, "wb");
                if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H); fwrite(g_fb, 1, sizeof g_fb, o); fclose(o); }
                running = 0;
            } else if (r == -3) running = 0;
            else screen = r;
            continue;
        }
        if (screen == 14 && clan >= 0 && clan <= 1) {   /* Cadet Training */
            int r = run_cadet_training(rd, tx, headless, frames, clan, &mx, &my);
            if (r == -2) {
                FILE *o = fopen(shot, "wb");
                if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H); fwrite(g_fb, 1, sizeof g_fb, o); fclose(o); }
                running = 0;
            } else if (r == -3) running = 0;
            else screen = r;
            continue;
        }
        if ((screen == 13 || screen == 9) && clan >= 0 && clan <= 3) {   /* Star Configuration; the Mech Lab (2 / 3: Instant Action) */
            int r = screen == 13 ? run_star_config(rd, tx, headless, frames, clan, &mx, &my)
                                 : (star_load(clan), run_mech_lab(rd, tx, headless, frames, clan, g_lab_slot, &mx, &my));
            if (r == -2) {
                FILE *o = fopen(shot, "wb");
                if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H); fwrite(g_fb, 1, sizeof g_fb, o); fclose(o); }
                running = 0;
            } else if (r == -3) running = 0;
            else { screen = r; if (screen == 7) clan = 2; }
            continue;
        }
        if (screen == 3 && clan >= 0 && clan <= 1) {   /* the debriefing */
            int r = run_debrief(rd, tx, headless, frames, clan, &mx, &my);
            if (r == -2) {
                FILE *o = fopen(shot, "wb");
                if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H); fwrite(g_fb, 1, sizeof g_fb, o); fclose(o); }
                running = 0;
            } else if (r == -3) running = 0;
            else screen = r;
            continue;
        }
        if ((screen == 11 || screen == 0) && clan >= 0 && clan <= 1) {   /* the Ready Room; the briefing */
            int r = screen == 11 ? run_ready_room(rd, tx, headless, frames, clan, &mx, &my) : run_briefing(rd, tx, headless, frames, clan, &mx, &my);
            if (r == -2) {
                FILE *o = fopen(shot, "wb");
                if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H); fwrite(g_fb, 1, sizeof g_fb, o); fclose(o); }
                running = 0;
            } else if (r == -3) running = 0;
            else screen = r;
            continue;
        }
        if (screen == 12 && clan >= 0 && clan <= 1) {
            int r = run_roster(rd, tx, headless, frames, clan, &mx, &my);
            if (r == -2) {
                FILE *o = fopen(shot, "wb");
                if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H); fwrite(g_fb, 1, sizeof g_fb, o); fclose(o); }
                running = 0;
            } else if (r == -3) running = 0;
            else { screen = r; if (r == 8) clan = 2; }
            continue;
        }
        if (screen == 7) {
            int r = run_instant_action(rd, tx, headless, frames, &mx, &my);
            if (r == -2) {   /* headless capture finished */
                FILE *o = fopen(shot, "wb");
                if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H); fwrite(g_fb, 1, sizeof g_fb, o); fclose(o); }
                running = 0;
            } else { screen = r; clan = (r == 9 || r == 13) ? g_ia_ctx : 2; }
            continue;
        }
        load_bg(sd ? (sd->bg >= 0 ? sd->bg : (clan == 1 ? 17 : 10)) : (clan == 1 ? 17 : 10));   /* rooms not built: the hall's */
        if (sd && sd->smk) ov = open_smk(sd->smk);
        if (sd) { int q; for (q = 0; q < sd->nov && q < 8; q++) xov[q] = open_smk(sd->ov[q].name); }
        play_ambient(sd ? sd->ambient : -1);
        while (next == -1 && running) {
            SDL_Event e;
            int i, hover = -1;
            if (!headless) {
                while (SDL_PollEvent(&e)) {
                    if (e.type == SDL_QUIT) running = 0;
                    if (e.type == SDL_MOUSEMOTION) { mx = e.motion.x; my = e.motion.y; }
                    if ((e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE && (screen == 1 || screen == 8) && !confirm) ||   /* title: Esc (DOS capture) */
                        (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT && screen != 8)) {   /* the global menu */
                        int r = global_menu(rd, tx, headless);
                        if (r == -3) confirm = 1;
                        else if (r >= 0) { next = r; nclan = 2; }
                        continue;
                    }
                    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                        if (confirm) confirm = 0;
                        else if (screen == 5 || screen == 11 || screen == 12 || screen == 14) next = 1;   /* rooms: back to the hall */
                        else if (screen != 8) next = 8; else confirm = 1;
                    }
                    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                        int bx = e.button.x, by = e.button.y;
                        if (confirm) {   /* "Embrace cowardice? Yes / No" */
                            if (by >= 250 && by < 290 && bx >= 230 && bx < 310) { next = -3; }
                            else if (by >= 250 && by < 290 && bx >= 330 && bx < 410) confirm = 0;
                        } else if (sd) {
                            for (i = 0; i < sd->nhs; i++)
                                if (bx >= sd->hs[i].x0 && bx <= sd->hs[i].x1 && by >= sd->hs[i].y0 && by <= sd->hs[i].y1) {
                                    if (sd->hs[i].next == -3) confirm = 1;
                                    else {
                                        if (sd->id == 1 && sd->hs[i].next == 5) shell_sound(0x67, 0x32);   /* the hall's holoprojector (FUN_0001dbe0 case 1, 0x9169c) */
                                        next = sd->hs[i].next; nclan = sd->hs[i].clan; pending_video = sd->hs[i].video;
                                    }
                                }
                        }
                    }
                }
            }
            draw_bg();
            if (ov) {   /* overlay at its own frame rate */
                Uint32 now = SDL_GetTicks();
                if (headless || now - last >= (Uint32)smk_frame_ms(ov) || f == 0) { smk_next(ov); last = now; }
                blit_smk(ov, sd->sx, sd->sy);
            }
            if (sd) {   /* the screen's looping clips (clan halls) */
                int q;
                Uint32 now = SDL_GetTicks();
                for (q = 0; q < sd->nov && q < 8; q++) {
                    if (!xov[q]) continue;
                    if (headless || now - xlast[q] >= (Uint32)smk_frame_ms(xov[q]) || f == 0) {
                        smk_next(xov[q]);   /* loops by itself */
                        xlast[q] = now;
                    }
                    if (xov[q]) blit_smk(xov[q], sd->ov[q].x, sd->ov[q].y);
                }
            }
            if (sd)
                for (i = 0; i < sd->nhs; i++)
                    if (mx >= sd->hs[i].x0 && mx <= sd->hs[i].x1 && my >= sd->hs[i].y0 && my <= sd->hs[i].y1) hover = i;
            if (hover >= 0) {   /* label on hover, centred at its position (shell font, background palette) */
                const hotspot *h = &sd->hs[hover];
                int tw2 = text_width(&g_flabel, h->label), lx2 = h->lx - tw2 / 2;
                if (lx2 < 4) lx2 = 4;                       /* centred on the table position, kept on screen */
                if (lx2 + tw2 > W - 4) lx2 = W - 4 - tw2;
                draw_text(&g_flabel, lx2, h->ly, h->label, (const uint8_t (*)[3])g_bg.pal, -1);
            }
            if (!sd) {
                char msg[96];
                snprintf(msg, sizeof msg, "SCREEN %d NOT BUILT YET - ESC", screen);
                draw_text(&g_flabel, 320 - text_width(&g_flabel, msg) / 2, 460, msg, (const uint8_t (*)[3])g_bg.pal, -1);
            } else if (sd->nhs == 0) {
                char msg[128];
                snprintf(msg, sizeof msg, "%s - NOT BUILT YET - ESC", sd->name);
                draw_text(&g_flabel, 320 - text_width(&g_flabel, msg) / 2, 460, msg, (const uint8_t (*)[3])g_bg.pal, -1);
            }
            if (confirm) {
                int x, y;
                for (y = 200; y < 300; y++) for (x = 200; x < 440; x++) { uint8_t *q = g_fb + ((size_t)y * W + (size_t)x) * 3; q[0] /= 4; q[1] /= 4; q[2] /= 4; }
            text_layer_cull(200, 200, 440, 300);
                text_layer_cull(200, 200, 440, 300);
                draw_text(&g_flabel, 320 - text_width(&g_flabel, "Embrace cowardice?") / 2, 220, "Embrace cowardice?", (const uint8_t (*)[3])g_bg.pal, -1);
                draw_text(&g_flabel, 270 - text_width(&g_flabel, "YES") / 2, 265, "YES", (const uint8_t (*)[3])g_bg.pal, -1);
                draw_text(&g_flabel, 370 - text_width(&g_flabel, "NO") / 2, 265, "NO", (const uint8_t (*)[3])g_bg.pal, -1);
            }
            f++;
            if (headless && getenv("MW2_SHELL_MUSICLOG") && g_song) fprintf(stderr, "music entry %d playing\n", g_song_entry);
            if (headless) {
                if (f >= frames && getenv("MW2_SHELL_MENU")) global_menu(rd, tx, headless);   /* tests: the menu over this screen */
                if (f >= frames) {
                    FILE *o = fopen(shot, "wb");
                    if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H); fwrite(g_fb, 1, sizeof g_fb, o); fclose(o); }
                    running = 0;
                }
            } else {
                SDL_UpdateTexture(tx, NULL, g_fb, W * 3);
                SDL_RenderClear(rd); SDL_RenderCopy(rd, tx, NULL, NULL); text_layer_draw(rd); SDL_RenderPresent(rd);
                SDL_Delay(10);
            }
        }
        smk_close(ov);
        { int q; for (q = 0; q < 8; q++) smk_close(xov[q]); }
        if (pending_video) play_cinematic(rd, tx, pending_video, headless);   /* e.g. the registration clip */
        if (next != -1) { screen = next; clan = nclan; confirm = 0; }
    }
    sfx_stop_loops(g_sfx);
    sfx_set_music(g_sfx, NULL, NULL);
    sfx_destroy(g_sfx);
    gus_song_close(g_song); free(g_song_data); gus_bank_close(g_bank);
    free(g_amb); free(g_flabel.d); free(g_flist.d);
    shell_image_free(&g_bg);
    shelldb_close(&g_db);
    if (tx) SDL_DestroyTexture(tx);
    if (rd) SDL_DestroyRenderer(rd);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
