/* smk.c - see smk.h. Format: Smacker 2/4 (RAD Game Tools), as documented by the multimedia wiki. */
#include "smk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- bit reader: LSB first ---- */
typedef struct { const uint8_t *p; size_t len, pos; } bits;
static int bit1(bits *b)
{
    int v;
    if (b->pos >= b->len * 8) return 0;
    v = (b->p[b->pos >> 3] >> (b->pos & 7)) & 1;
    b->pos++;
    return v;
}
static unsigned bitn(bits *b, int n) { unsigned v = 0; int i; for (i = 0; i < n; i++) v |= (unsigned)bit1(b) << i; return v; }

/* ---- 8-bit Huffman tree ---- */
typedef struct { int16_t *node; int n, cap; } tree8;   /* node: >= 0 child index pair base, leaf = -(value+1) */
static int t8_add(tree8 *t) { if (t->n + 2 > t->cap) { t->cap = t->cap ? t->cap * 2 : 64; t->node = realloc(t->node, (size_t)t->cap * sizeof *t->node); } t->n += 2; return t->n - 2; }
/* node layout: pairs [left, right]; root is a single slot at index 0 stored as pair start of a virtual parent */
static int16_t t8_build(tree8 *t, bits *b, int depth)
{
    if (depth > 32) return -1;
    if (!bit1(b)) return (int16_t)(-(int)bitn(b, 8) - 1);
    {
        int base = t8_add(t);
        int16_t l = t8_build(t, b, depth + 1), r;
        t->node[base] = l;
        r = t8_build(t, b, depth + 1);
        t->node[base + 1] = r;
        return (int16_t)base;
    }
}
static int t8_get(const tree8 *t, int16_t root, bits *b)
{
    int16_t v = root;
    while (v >= 0) v = t->node[v + bit1(b)];
    return -v - 1;
}

/* ---- 16-bit "big" tree with 3-entry recent-value cache ---- */
typedef struct {
    int32_t *node; int n, cap;      /* pairs of children; leaf = -(leafindex+1) */
    int32_t root;
    int     *val; int nval, capval;  /* leaf values */
    int      last[3];                /* leaf indices of the escape leaves, or -1 */
    int      empty;
} bigtree;

static int bt_addnode(bigtree *t) { if (t->n + 2 > t->cap) { t->cap = t->cap ? t->cap * 2 : 256; t->node = realloc(t->node, (size_t)t->cap * sizeof *t->node); } t->n += 2; return t->n - 2; }
static int bt_addleaf(bigtree *t, int v) { if (t->nval + 1 > t->capval) { t->capval = t->capval ? t->capval * 2 : 256; t->val = realloc(t->val, (size_t)t->capval * sizeof *t->val); } t->val[t->nval] = v; return t->nval++; }

static int32_t bt_build(bigtree *t, bits *b, const tree8 *lo, int16_t lor, int haslo, const tree8 *hi, int16_t hir, int hashi, const int esc[3], int depth)
{
    if (depth > 64) return -1;
    if (!bit1(b)) {
        int l = haslo ? t8_get(lo, lor, b) : 0, h = hashi ? t8_get(hi, hir, b) : 0, v = l | h << 8, idx;
        idx = bt_addleaf(t, v);
        if (v == esc[0]) { t->last[0] = idx; t->val[idx] = 0; }
        else if (v == esc[1]) { t->last[1] = idx; t->val[idx] = 0; }
        else if (v == esc[2]) { t->last[2] = idx; t->val[idx] = 0; }
        return -idx - 1;
    } else {
        int base = bt_addnode(t);
        int32_t l = bt_build(t, b, lo, lor, haslo, hi, hir, hashi, esc, depth + 1), r;
        t->node[base] = l;
        r = bt_build(t, b, lo, lor, haslo, hi, hir, hashi, esc, depth + 1);
        t->node[base + 1] = r;
        return base;
    }
}

static void bt_read(bigtree *t, bits *b)
{
    tree8 lo = {0}, hi = {0};
    int16_t lor = 0, hir = 0;
    int haslo, hashi, esc[3], i;
    memset(t, 0, sizeof *t);
    t->last[0] = t->last[1] = t->last[2] = -1;
    if (!bit1(b)) { t->empty = 1; return; }
    haslo = bit1(b);
    if (haslo) { lor = t8_build(&lo, b, 0); bit1(b); }
    hashi = bit1(b);
    if (hashi) { hir = t8_build(&hi, b, 0); bit1(b); }
    for (i = 0; i < 3; i++) esc[i] = (int)bitn(b, 16);
    t->root = bt_build(t, b, &lo, lor, haslo, &hi, hir, hashi, esc, 0);
    bit1(b);
    for (i = 0; i < 3; i++) if (t->last[i] < 0) t->last[i] = bt_addleaf(t, 0);
    free(lo.node); free(hi.node);
}

static void bt_reset(bigtree *t) { int i; if (!t->empty) for (i = 0; i < 3; i++) t->val[t->last[i]] = 0; }

static int bt_get(bigtree *t, bits *b)
{
    int32_t v = t->root;
    int idx, val;
    if (t->empty) return 0;
    while (v >= 0) v = t->node[v + bit1(b)];
    idx = -v - 1;
    val = t->val[idx];
    if (val != t->val[t->last[0]]) {
        t->val[t->last[2]] = t->val[t->last[1]];
        t->val[t->last[1]] = t->val[t->last[0]];
        t->val[t->last[0]] = val;
    }
    return val;
}

static void bt_free(bigtree *t) { free(t->node); free(t->val); memset(t, 0, sizeof *t); }

/* ---- decoder ---- */
struct smk_s {
    uint8_t *own;
    const uint8_t *d; size_t len;
    int w, h, frames, ver, ydouble;
    double ms;
    uint32_t *fsize; uint8_t *ftype; size_t *foff;
    bigtree mmap, mclr, full, type;
    uint8_t *pix, pal[256][3];
    int cur;
    uint32_t arate[7];   /* header AudioRate: bit 31 compressed, 30 present, 29 16-bit, 28 stereo, low 24 the rate */
};

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

smk *smk_open_mem(const uint8_t *d, size_t len)
{
    smk *s;
    uint32_t flags, trees;
    int32_t rate;
    size_t p;
    int i;
    bits b;
    if (len < 104 || memcmp(d, "SMK", 3) || (d[3] != '2' && d[3] != '4')) return NULL;
    s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->d = d; s->len = len; s->ver = d[3] - '0';
    s->w = (int)rd32(d + 4); s->h = (int)rd32(d + 8); s->frames = (int)rd32(d + 12);
    rate = (int32_t)rd32(d + 16); flags = rd32(d + 20);
    s->ms = rate > 0 ? rate : rate < 0 ? -rate / 100.0 : 100.0;
    if (flags & 1) s->frames++;                      /* ring frame */
    s->ydouble = (flags & 6) ? 1 : 0;
    trees = rd32(d + 52);
    for (i = 0; i < 7; i++) s->arate[i] = rd32(d + 72 + (size_t)i * 4);
    if (s->w <= 0 || s->h <= 0 || s->w > 4096 || s->h > 4096 || s->frames <= 0 || s->frames > 100000) { free(s); return NULL; }
    p = 104;
    if (p + (size_t)s->frames * 5 + trees > len) { free(s); return NULL; }
    s->fsize = malloc((size_t)s->frames * sizeof *s->fsize);
    s->ftype = malloc((size_t)s->frames);
    s->foff = malloc((size_t)s->frames * sizeof *s->foff);
    s->pix = calloc((size_t)s->w * (size_t)s->h, 1);
    if (!s->fsize || !s->ftype || !s->foff || !s->pix) { smk_close(s); return NULL; }
    for (i = 0; i < s->frames; i++) s->fsize[i] = rd32(d + p + (size_t)i * 4) & ~3u;
    p += (size_t)s->frames * 4;
    memcpy(s->ftype, d + p, (size_t)s->frames);
    p += (size_t)s->frames;
    b.p = d + p; b.len = trees; b.pos = 0;
    bt_read(&s->mmap, &b); bt_read(&s->mclr, &b); bt_read(&s->full, &b); bt_read(&s->type, &b);
    p += trees;
    for (i = 0; i < s->frames; i++) { s->foff[i] = p; p += s->fsize[i]; }
    if (p > len) { smk_close(s); return NULL; }
    return s;
}

smk *smk_open_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *buf;
    smk *s;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = n > 0 ? malloc((size_t)n) : NULL;
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    s = smk_open_mem(buf, (size_t)n);
    if (!s) { free(buf); return NULL; }
    s->own = buf;
    return s;
}

void smk_close(smk *s)
{
    if (!s) return;
    bt_free(&s->mmap); bt_free(&s->mclr); bt_free(&s->full); bt_free(&s->type);
    free(s->fsize); free(s->ftype); free(s->foff); free(s->pix); free(s->own);
    free(s);
}

int smk_width(const smk *s) { return s->w; }
int smk_height(const smk *s) { return s->ydouble ? s->h * 2 : s->h; }
int smk_frames(const smk *s) { return s->frames; }
double smk_frame_ms(const smk *s) { return s->ms; }
const uint8_t *smk_pixels(const smk *s) { return s->pix; }
const uint8_t (*smk_palette(const smk *s))[3] { return (const uint8_t (*)[3])s->pal; }
void smk_rewind(smk *s) { s->cur = 0; memset(s->pix, 0, (size_t)s->w * (size_t)s->h); memset(s->pal, 0, sizeof s->pal); }

static uint8_t pal6(int v) { v &= 63; return (uint8_t)(v * 4 + (v >> 4)); }   /* Smacker 6-bit -> 8-bit table */

static void do_palette(smk *s, const uint8_t *p, size_t n)
{
    uint8_t old[256][3];
    size_t i = 0;
    int sz = 0;
    memcpy(old, s->pal, sizeof old);
    while (sz < 256 && i < n) {
        uint8_t t = p[i++];
        if (t & 0x80) {                          /* keep (t & 0x7f) + 1 entries */
            int k = (t & 0x7f) + 1;
            while (k-- && sz < 256) { memcpy(s->pal[sz], old[sz], 3); sz++; }
        } else if (t & 0x40) {                   /* copy (t & 0x3f) + 1 entries from old[off] */
            int k = (t & 0x3f) + 1, off = i < n ? p[i++] : 0;
            while (k-- && sz < 256 && off < 256) { memcpy(s->pal[sz], old[off], 3); sz++; off++; }
        } else {                                  /* explicit 6-bit RGB */
            if (i + 2 > n) break;
            s->pal[sz][0] = pal6(t); s->pal[sz][1] = pal6(p[i]); s->pal[sz][2] = pal6(p[i + 1]);
            i += 2; sz++;
        }
    }
}

static const int block_runs[64] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
    31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
    128, 256, 512, 1024, 2048};

static void do_video(smk *s, const uint8_t *p, size_t n)
{
    bits b;
    int bw = s->w / 4, bh = s->h / 4, blocks = bw * bh, blk = 0;
    b.p = p; b.len = n; b.pos = 0;
    bt_reset(&s->mmap); bt_reset(&s->mclr); bt_reset(&s->full); bt_reset(&s->type);
    while (blk < blocks) {
        int t = bt_get(&s->type, &b), kind = t & 3, run = block_runs[(t >> 2) & 63];
        while (run-- && blk < blocks) {
            int bx = (blk % bw) * 4, by = (blk / bw) * 4, r;
            uint8_t *o = s->pix + (size_t)by * (size_t)s->w + (size_t)bx;
            switch (kind) {
            case 0: {                                        /* mono: two colours + 16-bit map */
                int clr = bt_get(&s->mclr, &b), map = bt_get(&s->mmap, &b), hi = clr >> 8, lo = clr & 0xff, x;
                for (r = 0; r < 4; r++, o += s->w)
                    for (x = 0; x < 4; x++) { o[x] = (uint8_t)((map & 1) ? hi : lo); map >>= 1; }
                break;
            }
            case 1: {                                        /* full colour (SMK2: 4 rows of 2 x 2 pixels) */
                for (r = 0; r < 4; r++, o += s->w) {
                    int c = bt_get(&s->full, &b);
                    o[2] = (uint8_t)(c & 0xff); o[3] = (uint8_t)(c >> 8);
                    c = bt_get(&s->full, &b);
                    o[0] = (uint8_t)(c & 0xff); o[1] = (uint8_t)(c >> 8);
                }
                break;
            }
            case 2: break;                                   /* skip: unchanged */
            default: {                                       /* solid */
                int x;
                uint8_t c = (uint8_t)(t >> 8);
                for (r = 0; r < 4; r++, o += s->w) for (x = 0; x < 4; x++) o[x] = c;
            }
            }
            blk++;
        }
    }
}

int smk_next(smk *s)
{
    const uint8_t *p;
    size_t n, i = 0;
    int f, k;
    if (s->cur >= s->frames) smk_rewind(s);
    f = s->cur++;
    p = s->d + s->foff[f];
    n = s->fsize[f];
    if (s->ftype[f] & 1) {                           /* palette chunk: size byte x 4 */
        size_t pl = (size_t)p[0] * 4;
        if (pl == 0 || pl > n) return -1;
        do_palette(s, p + 1, pl - 1);
        i += pl;
    }
    for (k = 1; k < 8; k++)                           /* audio tracks: u32 length incl. itself */
        if (s->ftype[f] & (1 << k)) {
            uint32_t al;
            if (i + 4 > n) return -1;
            al = rd32(p + i);
            if (al < 4 || i + al > n) return -1;
            i += al;
        }
    if (i < n) do_video(s, p + i, n - i);
    return f;
}

/* ---- audio (track 0), as the multimedia wiki and ffmpeg's smacker audio decoder describe it: per frame a packet
 * {u32 length, u32 unpacked size, bitstream}: 1 bit data present, 1 bit stereo, 1 bit 16-bit, then (1 << (16bit +
 * stereo)) 8-bit Huffman trees (each: 1 bit present, the tree, 1 bit skipped), the first sample(s) - 16-bit stored
 * byte-swapped, last channel first - and then deltas, the low and high bytes from separate trees. ---- */
static size_t audio_packet(const uint8_t *p, size_t n, int16_t *out, size_t cap, int *channels)
{
    bits b;
    tree8 t[4];
    int16_t root[4];
    int stereo, b16, nt, i;
    uint32_t unp;
    size_t k = 0, nout;
    if (n < 4) return 0;
    unp = rd32(p);
    b.p = p + 4; b.len = n - 4; b.pos = 0;
    if (!bit1(&b)) return 0;
    stereo = bit1(&b); b16 = bit1(&b);
    *channels = stereo ? 2 : 1;
    nt = 1 << (b16 + stereo);
    memset(t, 0, sizeof t);
    for (i = 0; i < nt; i++) {
        root[i] = -1;   /* absent: the value 0 */
        if (bit1(&b)) root[i] = t8_build(&t[i], &b, 0);
        bit1(&b);
    }
    nout = b16 ? unp / 2 : unp;
    if (nout > cap) nout = cap;
    if (b16) {
        int pred[2];
        for (i = stereo; i >= 0; i--) { unsigned v = bitn(&b, 16); pred[i] = (int16_t)(uint16_t)(((v & 0xff) << 8) | (v >> 8)); }
        for (i = 0; i <= stereo && k < nout; i++) out[k++] = (int16_t)pred[i];
        for (; k < nout; k++) {
            int c = (int)(k & (size_t)stereo), lo, hi;
            lo = root[2 * c] < 0 && t[2 * c].n == 0 ? 0 : t8_get(&t[2 * c], root[2 * c], &b);
            hi = root[2 * c + 1] < 0 && t[2 * c + 1].n == 0 ? 0 : t8_get(&t[2 * c + 1], root[2 * c + 1], &b);
            pred[c] = (int16_t)(pred[c] + (int16_t)(uint16_t)(lo | hi << 8));
            out[k] = (int16_t)pred[c];
        }
    } else {
        int pred[2];
        for (i = stereo; i >= 0; i--) pred[i] = (int)bitn(&b, 8);
        for (i = 0; i <= stereo && k < nout; i++) out[k++] = (int16_t)((pred[i] - 128) << 8);
        for (; k < nout; k++) {
            int c = (int)(k & (size_t)stereo), v;
            v = root[c] < 0 && t[c].n == 0 ? 0 : t8_get(&t[c], root[c], &b);
            pred[c] = (pred[c] + v) & 0xff;
            out[k] = (int16_t)((pred[c] - 128) << 8);
        }
    }
    for (i = 0; i < nt; i++) free(t[i].node);
    return k;
}

int smk_audio(const smk *s, int16_t **out, size_t *samples, int *rate, int *channels)
{
    size_t cap = 0, n = 0;
    int f;
    int16_t *buf = NULL;
    *out = NULL; *samples = 0; *channels = 1;
    *rate = (int)(s->arate[0] & 0xffffff);
    if (!(s->arate[0] & 0x40000000u) || *rate <= 0) return -1;
    for (f = 0; f < s->frames; f++) {
        const uint8_t *p = s->d + s->foff[f];
        size_t fn = s->fsize[f], i = 0;
        if (s->ftype[f] & 1) { size_t pl = (size_t)p[0] * 4; if (pl == 0 || pl > fn) break; i += pl; }
        if (s->ftype[f] & 2) {
            uint32_t al;
            if (i + 4 > fn) break;
            al = rd32(p + i);
            if (al < 8 || i + al > fn) break;
            {
                uint32_t unp = rd32(p + i + 4);
                size_t need = n + unp + 16;
                if (need > cap) { cap = need * 2; buf = realloc(buf, cap * sizeof *buf); if (!buf) return -1; }
                if (s->arate[0] & 0x80000000u) n += audio_packet(p + i + 4, al - 4, buf + n, cap - n, channels);
                else {   /* uncompressed: raw 8-bit unsigned or 16-bit samples */
                    size_t q, cnt = (s->arate[0] & 0x20000000u) ? (al - 4) / 2 : al - 4;
                    *channels = (s->arate[0] & 0x10000000u) ? 2 : 1;
                    for (q = 0; q < cnt && n < cap; q++)
                        buf[n++] = (s->arate[0] & 0x20000000u) ? (int16_t)(p[i + 4 + 2 * q] | p[i + 5 + 2 * q] << 8) : (int16_t)((p[i + 4 + q] - 128) << 8);
                }
            }
        }
    }
    *out = buf; *samples = n;
    return n ? 0 : -1;
}
