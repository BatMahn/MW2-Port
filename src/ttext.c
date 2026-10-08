#include "ttext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"

static unsigned char *g_ttf;
static stbtt_fontinfo g_font;
static int g_ok;

int ttext_available(void) { return g_ok; }

int ttext_init(const char *base_dir)
{
    static const char *REL[] = {"assets/fonts/LiberationSans-Bold.ttf", "../assets/fonts/LiberationSans-Bold.ttf"};
    const char *sys = "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf";
    char path[1024];
    int i;
    if (g_ok) return 0;
    for (i = 0; i < 5 && !g_ok; i++) {
        FILE *f;
        long n;
        if (i < 2) { if (!base_dir) continue; snprintf(path, sizeof path, "%s%s", base_dir, REL[i]); }
        else if (i < 4) snprintf(path, sizeof path, "%s", REL[i - 2]);
        else snprintf(path, sizeof path, "%s", sys);
        if (!(f = fopen(path, "rb"))) continue;
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        free(g_ttf);
        g_ttf = malloc((size_t)n);
        if (g_ttf && fread(g_ttf, 1, (size_t)n, f) == (size_t)n && stbtt_InitFont(&g_font, g_ttf, stbtt_GetFontOffsetForIndex(g_ttf, 0))) g_ok = 1;
        fclose(f);
    }
    return g_ok ? 0 : -1;
}

uint8_t *ttext_render(const char *s, float cap_px, float hscale, int *w, int *h, int *baseline_y)
{
    return ttext_render_track(s, cap_px, hscale, 0.0f, w, h, baseline_y);
}

float ttext_width(const char *s, float cap_px, float hscale)
{
    int x0, y0, x1, y1, i;
    float scale, pen = 0;
    if (!g_ok || !s) return 0;
    stbtt_GetCodepointBox(&g_font, 'H', &x0, &y0, &x1, &y1);
    scale = cap_px / (float)(y1 > 0 ? y1 : 1) * hscale;
    for (i = 0; s[i]; i++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, (unsigned char)s[i], &adv, &lsb);
        pen += (float)adv * scale;
        if (s[i + 1]) pen += (float)stbtt_GetCodepointKernAdvance(&g_font, (unsigned char)s[i], (unsigned char)s[i + 1]) * scale;
    }
    return pen;
}

uint8_t *ttext_render_track(const char *s, float cap_px, float hscale, float track, int *w, int *h, int *baseline_y)
{
    int x0, y0, x1, y1, asc, desc, gap, W, H, i;
    float scale, sx, pen = 0;
    uint8_t *img;
    if (!g_ok || !s) return NULL;
    /* the cap height: the 'H' glyph's box */
    stbtt_GetCodepointBox(&g_font, 'H', &x0, &y0, &x1, &y1);
    scale = cap_px / (float)(y1 > 0 ? y1 : 1);
    sx = scale * hscale;
    stbtt_GetFontVMetrics(&g_font, &asc, &desc, &gap);
    W = 4;
    for (i = 0; s[i]; i++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, (unsigned char)s[i], &adv, &lsb);
        W += (int)ceilf((float)adv * sx + track) + 1;
        if (s[i + 1]) W += (int)ceilf((float)stbtt_GetCodepointKernAdvance(&g_font, (unsigned char)s[i], (unsigned char)s[i + 1]) * sx);
    }
    H = (int)ceilf((float)(asc - desc) * scale) + 2;
    if (!(img = calloc((size_t)W * (size_t)H, 1))) return NULL;
    for (i = 0; s[i]; i++) {
        int adv, lsb, bx0, by0, bx1, by1, gw, gh;
        float shift = pen - floorf(pen);
        stbtt_GetCodepointHMetrics(&g_font, (unsigned char)s[i], &adv, &lsb);
        stbtt_GetCodepointBitmapBoxSubpixel(&g_font, (unsigned char)s[i], sx, scale, shift, 0, &bx0, &by0, &bx1, &by1);
        gw = bx1 - bx0; gh = by1 - by0;
        if (gw > 0 && gh > 0) {
            int ox = (int)floorf(pen) + bx0 + 1, oy = (int)((float)asc * scale) + by0 + 1;
            if (ox >= 0 && oy >= 0 && ox + gw <= W && oy + gh <= H) {
                uint8_t *tmp = calloc((size_t)gw * (size_t)gh, 1);
                if (tmp) {
                    int yy, xx;
                    stbtt_MakeCodepointBitmapSubpixel(&g_font, tmp, gw, gh, gw, sx, scale, shift, 0, (unsigned char)s[i]);
                    for (yy = 0; yy < gh; yy++) for (xx = 0; xx < gw; xx++) { uint8_t v = tmp[yy * gw + xx], *d = img + (size_t)(oy + yy) * (size_t)W + (size_t)(ox + xx); if (v > *d) *d = v; }
                    free(tmp);
                }
            }
        }
        pen += (float)adv * sx + track;
        if (s[i + 1]) pen += (float)stbtt_GetCodepointKernAdvance(&g_font, (unsigned char)s[i], (unsigned char)s[i + 1]) * sx;
    }
    {   /* trim to the ink on the right, so right-aligned text ends where its letters end */
        int x, y, last = 0;
        for (x = 0; x < W; x++) for (y = 0; y < H; y++) if (img[y * W + x]) { last = x; break; }
        if (last + 1 < W) {
            int nw = last + 1;
            for (y = 1; y < H; y++) memmove(img + (size_t)y * (size_t)nw, img + (size_t)y * (size_t)W, (size_t)nw);
            W = nw;
        }
    }
    *w = W; *h = H; *baseline_y = (int)((float)asc * scale) + 1;
    return img;
}
