/* shelldb.c - see shelldb.h. */
#include "shelldb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int shelldb_open(const char *path, shelldb *db)
{
    FILE *f = fopen(path, "rb");
    long n;
    int i;
    memset(db, 0, sizeof *db);
    if (!f) return -1;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 8) { fclose(f); return -1; }
    db->data = malloc((size_t)n);
    if (!db->data || fread(db->data, 1, (size_t)n, f) != (size_t)n) { fclose(f); shelldb_close(db); return -1; }
    fclose(f);
    db->size = (size_t)n;
    db->count = (int)rd32(db->data);
    if (db->count <= 0 || (size_t)db->count * 4 + 4 > db->size) { shelldb_close(db); return -1; }
    db->offs = malloc(((size_t)db->count + 1) * sizeof *db->offs);
    if (!db->offs) { shelldb_close(db); return -1; }
    for (i = 0; i < db->count; i++) {
        db->offs[i] = rd32(db->data + 4 + (size_t)i * 4);
        if (db->offs[i] > db->size || (i && db->offs[i] < db->offs[i - 1])) { shelldb_close(db); return -1; }
    }
    db->offs[db->count] = (uint32_t)db->size;
    return 0;
}

void shelldb_close(shelldb *db)
{
    free(db->data);
    free(db->offs);
    memset(db, 0, sizeof *db);
}

int shelldb_is_compressed(const shelldb *db, int i)
{
    const uint8_t *e;
    size_t len;
    if (i < 0 || i >= db->count) return 0;
    e = db->data + db->offs[i];
    len = db->offs[i + 1] - db->offs[i];
    if (len < 8) return 0;
    if (!memcmp(e, "RIFF", 4) || !memcmp(e, "1.", 2) || !memcmp(e, "MZ", 2)) return 0;
    /* stored size: LZSS output is at least 8/9 of the input (worst-case expansion 9/8) */
    return (uint64_t)rd32(e) * 9 >= (uint64_t)(len - 4) * 8 && rd32(e) < 64u * 1024u * 1024u;
}

size_t shell_lzss(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen)
{
    uint8_t win[4096];
    size_t i = 0, o = 0;
    unsigned r = 0, flags = 0, bits = 0;
    memset(win, 0, sizeof win);
    while (o < dstlen && i < srclen) {
        if (!bits) { flags = src[i++]; bits = 8; }
        if (flags & 1) {
            if (i >= srclen) break;
            win[r] = dst[o++] = src[i++];
            r = (r + 1) & 4095;
        } else {
            unsigned pos, len, k;
            if (i + 1 >= srclen) break;
            pos = src[i] | (unsigned)(src[i + 1] & 15) << 8;
            len = (unsigned)(src[i + 1] >> 4) + 3;
            i += 2;
            for (k = 0; k < len && o < dstlen; k++) {
                uint8_t c = win[(pos + k) & 4095];
                win[r] = dst[o++] = c;
                r = (r + 1) & 4095;
            }
        }
        flags >>= 1;
        bits--;
    }
    return o;
}

long shelldb_entry(const shelldb *db, int i, uint8_t **out)
{
    const uint8_t *e;
    size_t len;
    *out = NULL;
    if (i < 0 || i >= db->count) return -1;
    e = db->data + db->offs[i];
    len = db->offs[i + 1] - db->offs[i];
    if (shelldb_is_compressed(db, i)) {
        size_t want = rd32(e), got;
        *out = malloc(want ? want : 1);
        if (!*out) return -1;
        got = shell_lzss(e + 4, len - 4, *out, want);
        return (long)got;
    }
    *out = malloc(len ? len : 1);
    if (!*out) return -1;
    memcpy(*out, e, len);
    return (long)len;
}

int shell_pcx(const uint8_t *d, size_t len, shell_image *img, int *exact)
{
    int x0, y0, x1, y1, bpl, w, h, row;
    size_t i = 128, end, k;
    uint8_t *line;
    memset(img, 0, sizeof *img);
    if (exact) *exact = 0;
    if (len < 128 + 769 || d[0] != 0x0a || d[3] != 8 || d[65] != 1) return -1;
    x0 = d[4] | d[5] << 8; y0 = d[6] | d[7] << 8; x1 = d[8] | d[9] << 8; y1 = d[10] | d[11] << 8;
    bpl = d[66] | d[67] << 8;
    w = x1 - x0 + 1; h = y1 - y0 + 1;
    if (w <= 0 || h <= 0 || bpl < w || w > 4096 || h > 4096) return -1;
    end = len - 769;
    if (d[end] != 0x0c) return -1;
    img->w = w; img->h = h;
    img->pix = calloc((size_t)w * (size_t)h, 1);
    line = malloc((size_t)bpl);
    if (!img->pix || !line) { free(line); shell_image_free(img); return -1; }
    for (row = 0; row < h; row++) {
        size_t o = 0;
        while (o < (size_t)bpl && i < end) {
            uint8_t c = d[i++];
            if (c >= 0xc0) {
                unsigned cnt = c & 0x3f;
                uint8_t v = i < end ? d[i++] : 0;
                while (cnt-- && o < (size_t)bpl) line[o++] = v;
            } else line[o++] = c;
        }
        memcpy(img->pix + (size_t)row * (size_t)w, line, (size_t)w);
    }
    free(line);
    for (k = 0; k < 768; k++) img->pal[k / 3][k % 3] = d[end + 1 + k];
    if (exact) *exact = (i == end);
    return 0;
}

void shell_image_free(shell_image *img)
{
    free(img->pix);
    memset(img, 0, sizeof *img);
}
