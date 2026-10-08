/* shp.c - see shp.h. */
#include "shp.h"

#include <stdlib.h>
#include <string.h>

static int32_t rd32(const uint8_t *p) { return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

int shp_count(const uint8_t *d, size_t len)
{
    if (len < 8 || memcmp(d, "1.10", 4) != 0) return -1;
    return rd32(d + 4);
}

int shp_decode(const uint8_t *d, size_t len, int i, shp_frame *out)
{
    int n = shp_count(d, len), x, y;
    size_t off, p;
    memset(out, 0, sizeof *out);
    if (n <= 0 || i < 0 || i >= n || 8 + (size_t)i * 8 + 4 > len) return -1;
    off = (size_t)(uint32_t)rd32(d + 8 + (size_t)i * 8);
    if (off + 0x18 > len) return -1;
    out->left = rd32(d + off + 8); out->top = rd32(d + off + 12);
    out->right = rd32(d + off + 16); out->bottom = rd32(d + off + 20);
    out->w = out->right - out->left + 1; out->h = out->bottom - out->top + 1;
    if (out->w <= 0 || out->h <= 0 || out->w > 4096 || out->h > 4096) return -1;
    out->pix = calloc((size_t)out->w * (size_t)out->h, 1);
    out->mask = calloc((size_t)out->w * (size_t)out->h, 1);
    if (!out->pix || !out->mask) { shp_frame_free(out); return -1; }
    p = off + 0x18;
    for (y = 0; y < out->h; y++) {
        x = 0;
        while (p < len) {
            uint8_t b = d[p++];
            int cnt = b >> 1;
            if (b == 0) break;
            if (b == 1) { if (p >= len) break; x += d[p++]; continue; }
            if (!(b & 1)) {                       /* run */
                uint8_t c;
                if (p >= len) break;
                c = d[p++];
                while (cnt-- > 0) { if (x < out->w) { out->pix[y * out->w + x] = c; out->mask[y * out->w + x] = 1; } x++; }
            } else {                              /* literal */
                while (cnt-- > 0 && p < len) { uint8_t c = d[p++]; if (x < out->w) { out->pix[y * out->w + x] = c; out->mask[y * out->w + x] = 1; } x++; }
            }
        }
    }
    return 0;
}

void shp_frame_free(shp_frame *f)
{
    free(f->pix);
    free(f->mask);
    memset(f, 0, sizeof *f);
}
