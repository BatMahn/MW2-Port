/* tex.c - see tex.h. */
#include "tex.h"

#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t pack(unsigned r, unsigned g, unsigned b, unsigned a)
{
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}
static unsigned x5(unsigned v) { return (v << 3) | (v >> 2); }
static unsigned x4(unsigned v) { return v * 17u; }

/* index of (x, y) in a Morton-twiddled square: y bits even, x bits odd */
static size_t twiddle(unsigned x, unsigned y)
{
    size_t r = 0;
    unsigned b;
    for (b = 0; b < 12; b++) r |= ((size_t)((x >> b) & 1u) << (2 * b + 1)) | ((size_t)((y >> b) & 1u) << (2 * b));
    return r;
}

int tex_decode(const uint8_t *d, size_t len, int enc, texture *t)
{
    size_t hdr, n, i;
    int x, y;

    memset(t, 0, sizeof *t);
    if (enc == TEX_DOS) {
        if (len < 4) return -1;
        t->w = rd16(d); t->h = rd16(d + 2);
        n = (size_t)t->w * (size_t)t->h;
        if (!n || n > len - 4) return -1;
        if (!(t->rgba = malloc(n * sizeof *t->rgba))) return -1;
        for (i = 0; i < n; i++) t->rgba[i] = pack(d[4 + i], d[4 + i], d[4 + i], d[4 + i] == 0xff ? 0 : 255);
        return 0;
    }
    if (enc == TEX_MGA) {
        uint16_t pal[256], cnt = 0;
        uint32_t acc[256][4];
        uint16_t *px;
        size_t k;
        int masked = 0;
        if (len < 4) return -1;
        t->w = rd16(d); t->h = rd16(d + 2);
        n = (size_t)t->w * (size_t)t->h;
        if (!n || n * 2 > len - 4 || !(px = malloc(n * 2))) return -1;
        for (i = 0; i < n; i++) px[i] = rd16(d + 4 + i * 2);
        for (;;) {   /* the distinct opaque colours */
            cnt = 0;
            for (i = 0; i < n && cnt <= 255; i++) {
                if (!(px[i] & 0x8000)) continue;
                for (k = 0; k < cnt && pal[k] != (px[i] & 0x7fff); k++) {}
                if (k == cnt) { if (cnt == 255) { cnt = 256; break; } pal[cnt++] = (uint16_t)(px[i] & 0x7fff); }
            }
            if (cnt <= 255 || masked) break;
            for (i = 0; i < n; i++) if (px[i] & 0x8000) px[i] = (uint16_t)(0x8000 | (px[i] & 0x7BDE));   /* drop each channel's low bit */
            masked = 1;
        }
        if (cnt > 255) {   /* nearest-colour clusters, each a running mean (stops early within distance 3) */
            cnt = 0;
            memset(acc, 0, sizeof acc);
            for (i = 0; i < n; i++) {
                int r, g, b, best = -1, bd = 1 << 30;
                if (!(px[i] & 0x8000)) continue;
                r = (px[i] >> 10) & 31; g = (px[i] >> 5) & 31; b = px[i] & 31;
                for (k = 0; k < cnt; k++) {
                    int dr = r - (int)(acc[k][0] / acc[k][3]), dg = g - (int)(acc[k][1] / acc[k][3]), db = b - (int)(acc[k][2] / acc[k][3]);
                    int dd = dr * dr + dg * dg + db * db;
                    if (dd < bd) { bd = dd; best = (int)k; if (dd <= 9) break; }
                }
                if (best < 0 || (bd > 9 && cnt < 255)) { best = cnt++; }
                acc[best][0] += (uint32_t)r; acc[best][1] += (uint32_t)g; acc[best][2] += (uint32_t)b; acc[best][3]++;
                px[i] = (uint16_t)(0x8000 | ((acc[best][0] / acc[best][3]) << 10) | ((acc[best][1] / acc[best][3]) << 5) | (acc[best][2] / acc[best][3]));
            }
        }
        if (!(t->rgba = malloc(n * sizeof *t->rgba))) { free(px); return -1; }
        for (i = 0; i < n; i++) t->rgba[i] = pack(x5((px[i] >> 10) & 31), x5((px[i] >> 5) & 31), x5(px[i] & 31), (px[i] & 0x8000) ? 255 : 0);
        free(px);
        return 0;
    }
    if (enc == TEX_PVR) {
        /* header: "PT", byte 2 the map type (0 opaque RGB555, 1 RGB555 with its mip chain, 3 translucent ARGB4444),
         * byte 3 a size code, +4 u32 log2 of the texel count (0x0c = 64 x 64); a mip chain is stored smallest level
         * first, so the full-size level is the last side x side x 2 bytes */
        size_t side = 1, cnt;
        if (len < 16 || d[0] != 'P' || d[1] != 'T') return -1;
        cnt = d[4] >= 4 && d[4] <= 22 ? (size_t)1 << d[4] : 0;
        while (side * side < cnt) side *= 2;
        if (!cnt || side * side != cnt || side * side * 2 > len - 16) {   /* unknown: the largest square that fits */
            side = 1;
            while ((side * 2) * (side * 2) * 2 <= len - 16) side *= 2;
        }
        hdr = len - side * side * 2;   /* 16, or past the smaller mip levels */
        t->w = t->h = (int)side;
    } else {
        if (len < 4) return -1;
        hdr = 4;
        t->w = rd16(d);
        t->h = rd16(d + 2);
    }
    n = (size_t)t->w * (size_t)t->h;
    if (!n || n * 2 > len - hdr) return -1;
    t->rgba = malloc(n * sizeof *t->rgba);
    if (!t->rgba) return -1;
    for (y = 0; y < t->h; y++) {
        for (x = 0; x < t->w; x++) {
            size_t src = enc == TEX_PVR ? twiddle((unsigned)x, (unsigned)y) : (size_t)y * (size_t)t->w + (size_t)x;
            unsigned p = rd16(d + hdr + src * 2);
            uint32_t c;
            switch (enc) {
            case TEX_ATI:
                c = pack(x5((p >> 10) & 31), x5((p >> 5) & 31), x5(p & 31), (p >> 15) ? 255 : 0);
                break;
            case TEX_3DFX_555:
                c = pack(x5((p >> 10) & 31), x5((p >> 5) & 31), x5(p & 31), 255);
                break;
            case TEX_3DFX_4444:
                c = pack(x4((p >> 8) & 15), x4((p >> 4) & 15), x4(p & 15), x4((p >> 12) & 15));
                break;
            default: /* TEX_PVR: header byte 2 = the map type - 0 opaque RGB555 (sgl_map_16bit), else ARGB4444 with the
                      * alpha inverted (sgl_map_trans16) */
                if (d[2] == 0 || d[2] == 1) c = pack(x5((p >> 10) & 31), x5((p >> 5) & 31), x5(p & 31), 255);
                else c = pack(x4((p >> 8) & 15), x4((p >> 4) & 15), x4(p & 15), x4(15u - ((p >> 12) & 15)));
                break;
            }
            t->rgba[(size_t)y * (size_t)t->w + (size_t)x] = c;
        }
    }
    (void)i;
    return 0;
}

void tex_free(texture *t)
{
    free(t->rgba);
    t->rgba = NULL;
}
