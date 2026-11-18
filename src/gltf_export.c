/*
 * gltf_export.c - PNG and glTF 2.0 binary writers for the game's models and textures (see gltf_export.h).
 * Self-contained: the PNG's image data is zlib-wrapped in stored (uncompressed) deflate blocks - larger files, no
 * compression library.
 */
#include "gltf_export.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------------------------------- byte buffers */
typedef struct { uint8_t *p; size_t n, cap; } gbuf;
static int gb_put(gbuf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        uint8_t *q;
        while (c < b->n + n) c *= 2;
        if (!(q = realloc(b->p, c))) return -1;
        b->p = q; b->cap = c;
    }
    if (d) memcpy(b->p + b->n, d, n); else memset(b->p + b->n, 0, n);
    b->n += n;
    return 0;
}
static void gb_be32(gbuf *b, uint32_t v) { uint8_t q[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; gb_put(b, q, 4); }
static void gb_le32(gbuf *b, uint32_t v) { uint8_t q[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; gb_put(b, q, 4); }
static void gb_printf(gbuf *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { gb_put(b, tmp, (size_t)n); return; }
    {
        char *big = malloc((size_t)n + 1);
        if (!big) return;
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        gb_put(b, big, (size_t)n);
        free(big);
    }
}
static void gb_str(gbuf *b, const char *s) { gb_put(b, s, strlen(s)); }

void gx_json_str(char *out, size_t n, const char *s)
{
    size_t o = 0;
    if (!n) return;
    for (; s && *s && o + 7 < n; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20 || c >= 0x7f) o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

/* ---------------------------------------------------------------------------------------------------- PNG */
static uint32_t crc_tab[256];
static void crc_init(void)
{
    uint32_t n, c;
    int k;
    if (crc_tab[1]) return;
    for (n = 0; n < 256; n++) { c = n; for (k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; crc_tab[n] = c; }
}
static uint32_t crc32_of(const uint8_t *p, size_t n, uint32_t c)
{
    size_t i;
    c ^= 0xffffffffu;
    for (i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}
static void png_chunk(gbuf *b, const char *type, const uint8_t *d, size_t n)
{
    uint32_t c;
    gb_be32(b, (uint32_t)n);
    gb_put(b, type, 4);
    if (n) gb_put(b, d, n);
    c = crc32_of((const uint8_t *)type, 4, 0);
    c = crc32_of(d, n, c);
    gb_be32(b, c);
}
static int png_mem(gbuf *out, const uint8_t *rgba, int w, int h)
{
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    uint8_t ihdr[13];
    gbuf raw = {0}, z = {0};
    size_t off;
    uint32_t a1 = 1, a2 = 0;
    int y;
    if (w <= 0 || h <= 0) return -1;
    crc_init();
    for (y = 0; y < h; y++) { const uint8_t f = 0; gb_put(&raw, &f, 1); gb_put(&raw, rgba + (size_t)y * (size_t)w * 4, (size_t)w * 4); }
    if (!raw.p) return -1;
    { const uint8_t zh[2] = {0x78, 0x01}; gb_put(&z, zh, 2); }
    for (off = 0; off < raw.n || off == 0; ) {   /* stored deflate blocks of up to 65535 bytes */
        const size_t len = raw.n - off > 65535 ? 65535 : raw.n - off;
        uint8_t hd[5];
        hd[0] = off + len >= raw.n ? 1 : 0;
        hd[1] = (uint8_t)len; hd[2] = (uint8_t)(len >> 8); hd[3] = (uint8_t)~len; hd[4] = (uint8_t)(~len >> 8);
        gb_put(&z, hd, 5);
        gb_put(&z, raw.p + off, len);
        off += len;
        if (len == 0) break;
    }
    for (off = 0; off < raw.n; off++) { a1 = (a1 + raw.p[off]) % 65521u; a2 = (a2 + a1) % 65521u; }
    gb_be32(&z, a2 << 16 | a1);
    gb_put(out, sig, 8);
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16); ihdr[2] = (uint8_t)(w >> 8); ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16); ihdr[6] = (uint8_t)(h >> 8); ihdr[7] = (uint8_t)h;
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;   /* 8-bit RGBA, no interlace */
    png_chunk(out, "IHDR", ihdr, 13);
    png_chunk(out, "IDAT", z.p, z.n);
    png_chunk(out, "IEND", NULL, 0);
    free(raw.p); free(z.p);
    return 0;
}
static uint8_t *tex_rgba_bytes(const texture *t)
{
    size_t i, n = (size_t)t->w * (size_t)t->h;
    uint8_t *o = malloc(n * 4 + 1);
    if (!o) return NULL;
    for (i = 0; i < n; i++) {
        const uint32_t px = t->rgba[i];
        o[i * 4] = (uint8_t)px; o[i * 4 + 1] = (uint8_t)(px >> 8); o[i * 4 + 2] = (uint8_t)(px >> 16); o[i * 4 + 3] = (uint8_t)(px >> 24);
    }
    return o;
}
int gx_png_write(const char *path, const uint8_t *rgba, int w, int h)
{
    gbuf b = {0};
    FILE *f;
    int rc = -1;
    if (png_mem(&b, rgba, w, h) == 0 && (f = fopen(path, "wb"))) {
        rc = fwrite(b.p, 1, b.n, f) == b.n ? 0 : -1;
        if (fclose(f) != 0) rc = -1;
    }
    free(b.p);
    return rc;
}
int gx_png_texture(const char *path, const texture *t)
{
    uint8_t *px;
    int rc;
    if (!t || !t->rgba || t->w <= 0 || t->h <= 0 || !(px = tex_rgba_bytes(t))) return -1;
    rc = gx_png_write(path, px, t->w, t->h);
    free(px);
    return rc;
}

/* ---------------------------------------------------------------------------------------------------- glTF */
typedef struct { size_t off, len; int target; } gx_view;
typedef struct { int kind, img, pal, alpha, slot; } gx_mat;   /* kind 0 textured, 1 flat, 2 line (unlit) */
typedef struct { char name[64]; float m[16]; int mesh, parent, has_m; float t[3]; char *extras; } gx_node;
typedef struct { char key[96]; int mesh; } gx_mkey;

struct gx {
    uint8_t      pal[256][3];
    texture     *slots[256];
    char         names[256][16];
    gbuf         bin, accessors, meshes;
    int          naccessors, nmeshes;
    gx_view     *views; int nviews, cviews;
    const texture **imgs; int nimgs, cimgs;
    gx_mat      *mats; int nmats, cmats;
    gx_node     *nodes; int nnodes, cnodes;
    gx_mkey     *mkeys; int nmkeys, cmkeys;
    int          used_unlit;
};

#define GROW(arr, n, cap) do { if ((n) >= (cap)) { int nc_ = (cap) ? (cap) * 2 : 64; void *q_ = realloc((arr), (size_t)nc_ * sizeof *(arr)); if (!q_) return -1; (arr) = q_; (cap) = nc_; } } while (0)

gx *gx_new(const uint8_t palette[256][3], texture *const slots[256], const char (*names)[16])
{
    gx *g = calloc(1, sizeof *g);
    int i;
    if (!g) return NULL;
    if (palette) memcpy(g->pal, palette, sizeof g->pal);
    for (i = 0; i < 256; i++) {
        g->slots[i] = slots ? slots[i] : NULL;
        if (names) snprintf(g->names[i], sizeof g->names[i], "%s", names[i]);
    }
    return g;
}
void gx_free(gx *g)
{
    int i;
    if (!g) return;
    for (i = 0; i < g->nnodes; i++) free(g->nodes[i].extras);
    free(g->bin.p); free(g->accessors.p); free(g->meshes.p);
    free(g->views); free(g->imgs); free(g->mats); free(g->nodes); free(g->mkeys);
    free(g);
}

/* a buffer view over bytes appended to the binary chunk (4-byte aligned) */
static int add_view(gx *g, const void *d, size_t n, int target)
{
    while (g->bin.n & 3) gb_put(&g->bin, NULL, 1);
    GROW(g->views, g->nviews, g->cviews);
    g->views[g->nviews].off = g->bin.n; g->views[g->nviews].len = n; g->views[g->nviews].target = target;
    if (gb_put(&g->bin, d, n) != 0) return -1;
    return g->nviews++;
}
/* a float accessor of count elements of `comps` (VEC2 / VEC3), with min / max for positions */
static int add_accessor(gx *g, const float *d, int count, int comps, int minmax)
{
    const int v = add_view(g, d, (size_t)count * (size_t)comps * sizeof(float), 34962);
    if (v < 0) return -1;
    if (g->naccessors) gb_str(&g->accessors, ",");
    gb_printf(&g->accessors, "{\"bufferView\":%d,\"componentType\":5126,\"count\":%d,\"type\":\"%s\"", v, count, comps == 2 ? "VEC2" : "VEC3");
    if (minmax) {
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        int i, k;
        for (i = 0; i < count; i++) for (k = 0; k < comps; k++) { const float x = d[i * comps + k]; if (x < mn[k]) mn[k] = x; if (x > mx[k]) mx[k] = x; }
        gb_printf(&g->accessors, ",\"min\":[%.6g,%.6g,%.6g],\"max\":[%.6g,%.6g,%.6g]", mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
    }
    gb_str(&g->accessors, "}");
    return g->naccessors++;
}
static int image_of(gx *g, const texture *t)
{
    int i;
    for (i = 0; i < g->nimgs; i++) if (g->imgs[i] == t) return i;
    GROW(g->imgs, g->nimgs, g->cimgs);
    g->imgs[g->nimgs] = t;
    return g->nimgs++;
}
static int material_of(gx *g, int kind, const texture *t, int pal, int alpha, int slot)
{
    const int img = kind == 0 ? image_of(g, t) : -1;
    int i;
    if (kind == 0 && img < 0) return -1;
    for (i = 0; i < g->nmats; i++) {
        const gx_mat *m = &g->mats[i];
        if (m->kind == kind && m->img == img && (kind == 0 || m->pal == pal) && m->alpha == alpha && (kind != 0 || m->slot == slot)) return i;
    }
    GROW(g->mats, g->nmats, g->cmats);
    g->mats[g->nmats].kind = kind; g->mats[g->nmats].img = img; g->mats[g->nmats].pal = pal; g->mats[g->nmats].alpha = alpha; g->mats[g->nmats].slot = slot;
    if (kind == 2) g->used_unlit = 1;
    return g->nmats++;
}

/* one primitive's vertices while a mesh is built */
typedef struct { int mat, lines; float *pos, *nrm, *uv; int n, cap; } gx_prim;
static int prim_add(gx_prim *p, const float pos[3], const float nrm[3], const float uv[2])
{
    if (p->n >= p->cap) {
        const int nc = p->cap ? p->cap * 2 : 96;
        float *a = realloc(p->pos, (size_t)nc * 3 * sizeof *a), *b, *c;
        if (!a) return -1;
        p->pos = a;
        if (!(b = realloc(p->nrm, (size_t)nc * 3 * sizeof *b))) return -1;
        p->nrm = b;
        if (!(c = realloc(p->uv, (size_t)nc * 2 * sizeof *c))) return -1;
        p->uv = c;
        p->cap = nc;
    }
    memcpy(p->pos + p->n * 3, pos, 3 * sizeof *pos);
    memcpy(p->nrm + p->n * 3, nrm, 3 * sizeof *nrm);
    p->uv[p->n * 2] = uv ? uv[0] : 0; p->uv[p->n * 2 + 1] = uv ? uv[1] : 0;
    p->n++;
    return 0;
}
static void game_pos(const wtb_vertex *v, float o[3]) { o[0] = (float)v->x / 100.0f; o[1] = (float)v->y / 100.0f; o[2] = -(float)v->z / 100.0f; }
static void unit3(float v[3])
{
    const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-12f) { v[0] /= l; v[1] /= l; v[2] /= l; } else { v[0] = 0; v[1] = 1; v[2] = 0; }
}

/* the mesh of a part's model (polygons classified as in glr.c's 3D-edition path), or -1 if nothing drawable */
static int build_mesh(gx *g, const mech3d_part *part, const char *name)
{
    const wtb_model *model = &part->model;
    const wtb_object *o = model->object_count > 0 ? &model->objects[0] : NULL;
    gx_prim pr[64];
    int np = 0, q, i, k, mesh = -1;
    if (!o) return -1;
    memset(pr, 0, sizeof pr);
    for (q = 0; q < o->poly_count; q++) {
        const wtb_poly *pl = &o->polys[q];
        const int ty = (pl->color >> 12) & 7, ty15 = (pl->color >> 12) & 15;
        const int hi_card = model->extended && (pl->color & 0x8000) != 0;
        int slot = pl->color & 0xff, kind, mat, pidx, alpha = 0, lines = 0;
        const texture *t = NULL;
        gx_prim *p = NULL;
        if (pl->n < 3) continue;
        if (model->extended && ty15 == 2) lines = 1;                          /* outlines */
        else if (ty == 3 && !hi_card) continue;                               /* billboards: sprites in the game */
        if (slot == 0x00) slot += part->tex_offset;
        else if (slot == 0x14) slot += part->tex_offset + part->world_clan;
        if (!lines && model->extended && !part->flat && slot >= 0 && slot < 256 && g->slots[slot] && (pl->color & 0xff) != 0xf0 &&
            ty != 1 && !((pl->color >> 12) == 0 && (pl->color & 0xf) == 0)) {
            t = g->slots[slot];
            kind = 0;
            alpha = ty15 >= 8 && ty15 <= 11;
        } else kind = lines ? 2 : 1;
        pidx = lines ? (pl->color >> 4) & 0xff
             : model->extended && (pl->color >> 12) == 0 && (pl->color & 0xf) == 0 ? (pl->color >> 4) & 0xff
             : model->extended && (pl->color >> 12) == 1 ? (((pl->color >> 8) & 0xf) << 4) | 0xf : pl->color >> 8;
        mat = material_of(g, kind, t, pidx & 0xff, alpha, kind == 0 ? slot : -1);
        if (mat < 0) continue;
        for (i = 0; i < np; i++) if (pr[i].mat == mat && pr[i].lines == lines) { p = &pr[i]; break; }
        if (!p) { if (np >= 64) continue; p = &pr[np++]; p->mat = mat; p->lines = lines; }
        if (lines) {
            for (k = 0; k < pl->n; k++) {
                float a[3], b[3], nz[3] = {0, 1, 0};
                game_pos(&o->verts[pl->idx[k]], a);
                game_pos(&o->verts[pl->idx[(k + 1) % pl->n]], b);
                prim_add(p, a, nz, NULL); prim_add(p, b, nz, NULL);
            }
            continue;
        }
        {
            const wtb_vertex *v0 = &o->verts[pl->idx[0]], *v1 = &o->verts[pl->idx[1]], *v2 = &o->verts[pl->idx[2]];
            const float e1[3] = {(float)(v1->x - v0->x), (float)(v1->y - v0->y), (float)(v1->z - v0->z)};
            const float e2[3] = {(float)(v2->x - v0->x), (float)(v2->y - v0->y), (float)(v2->z - v0->z)};
            float face[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            int j;
            face[2] = -face[2];
            unit3(face);
            for (j = 1; j + 1 < pl->n; j++) {
                const int tri[3] = {0, j, j + 1};
                int a;
                for (a = 0; a < 3; a++) {
                    const wtb_vertex *v = &o->verts[pl->idx[tri[a]]];
                    float ps[3], nr[3], uv[2];
                    game_pos(v, ps);
                    if (model->extended && (v->normal[0] != 0 || v->normal[1] != 0 || v->normal[2] != 0)) { nr[0] = v->normal[0]; nr[1] = v->normal[1]; nr[2] = -v->normal[2]; unit3(nr); }
                    else memcpy(nr, face, sizeof nr);
                    if (t) { uv[0] = (float)v->a / (float)t->w; uv[1] = (float)v->b / (float)t->h; }   /* (texels; rows top first, as glTF) */
                    prim_add(p, ps, nr, t ? uv : NULL);
                }
            }
        }
    }
    if (np) {
        char nm[80];
        int first = 1;
        gx_json_str(nm, sizeof nm, name);
        if (g->nmeshes) gb_str(&g->meshes, ",");
        gb_printf(&g->meshes, "{\"name\":\"%s\",\"primitives\":[", nm);
        for (i = 0; i < np; i++) {
            const int pa = add_accessor(g, pr[i].pos, pr[i].n, 3, 1);
            const int na = pr[i].lines ? -1 : add_accessor(g, pr[i].nrm, pr[i].n, 3, 0);
            const int ua = !pr[i].lines && g->mats[pr[i].mat].kind == 0 ? add_accessor(g, pr[i].uv, pr[i].n, 2, 0) : -1;
            if (pa < 0) continue;
            gb_printf(&g->meshes, "%s{\"attributes\":{\"POSITION\":%d", first ? "" : ",", pa);
            if (na >= 0) gb_printf(&g->meshes, ",\"NORMAL\":%d", na);
            if (ua >= 0) gb_printf(&g->meshes, ",\"TEXCOORD_0\":%d", ua);
            gb_printf(&g->meshes, "},\"material\":%d,\"mode\":%d}", pr[i].mat, pr[i].lines ? 1 : 4);
            first = 0;
        }
        gb_str(&g->meshes, "]}");
        mesh = g->nmeshes++;
    }
    for (i = 0; i < np; i++) { free(pr[i].pos); free(pr[i].nrm); free(pr[i].uv); }
    return mesh;
}

static int add_node(gx *g, const char *name, int mesh, int parent, const char *extras)
{
    gx_node *n;
    GROW(g->nodes, g->nnodes, g->cnodes);
    n = &g->nodes[g->nnodes];
    memset(n, 0, sizeof *n);
    snprintf(n->name, sizeof n->name, "%s", name ? name : "");
    n->mesh = mesh; n->parent = parent;
    n->extras = extras && extras[0] ? strdup(extras) : NULL;
    return g->nnodes++;
}

int gx_group(gx *g, const char *name, const float pos[3], int parent, const char *extras)
{
    const int i = add_node(g, name, -1, parent, extras);
    if (i >= 0 && pos) { g->nodes[i].t[0] = pos[0] / 100.0f; g->nodes[i].t[1] = pos[1] / 100.0f; g->nodes[i].t[2] = -pos[2] / 100.0f; }
    return i;
}

int gx_part(gx *g, const mech3d_part *p, const float origin[3], int parent, const char *extras)
{
    char key[96];
    int mesh = -1, i, nd;
    const wtb_object *o = p->model.object_count > 0 ? &p->model.objects[0] : NULL;
    float R[9], M[16];
    if (!o) return -1;
    snprintf(key, sizeof key, "%s|%d|%d|%d|%d|%d", p->model_name, p->tex_offset, p->world_clan, o->vert_count, o->poly_count, p->flat);
    for (i = 0; i < g->nmkeys; i++) if (!strcmp(g->mkeys[i].key, key)) { mesh = g->mkeys[i].mesh; break; }
    if (i == g->nmkeys) {
        mesh = build_mesh(g, p, p->model_name);
        GROW(g->mkeys, g->nmkeys, g->cmkeys);
        snprintf(g->mkeys[g->nmkeys].key, sizeof g->mkeys[0].key, "%s", key);
        g->mkeys[g->nmkeys++].mesh = mesh;
    }
    if (mesh < 0) return -1;
    if (p->has_rot) memcpy(R, p->rot, sizeof R);
    else {
        const float c = cosf(p->yaw * 3.14159265f / 180.0f), s = sinf(p->yaw * 3.14159265f / 180.0f);
        R[0] = c; R[1] = 0; R[2] = s; R[3] = 0; R[4] = 1; R[5] = 0; R[6] = -s; R[7] = 0; R[8] = c;
    }
    {   /* C R C (C = diag(1, 1, -1)) and the position, column-major */
        static const float C[3] = {1, 1, -1};
        int r, c;
        memset(M, 0, sizeof M);
        for (r = 0; r < 3; r++) for (c = 0; c < 3; c++) M[c * 4 + r] = C[r] * R[r * 3 + c] * C[c];
        M[12] = ((float)p->pos[0] - (origin ? origin[0] : 0)) / 100.0f;
        M[13] = ((float)p->pos[1] - (origin ? origin[1] : 0)) / 100.0f;
        M[14] = -((float)p->pos[2] - (origin ? origin[2] : 0)) / 100.0f;
        M[15] = 1;
    }
    nd = add_node(g, p->model_name, mesh, parent, extras);
    if (nd >= 0) { memcpy(g->nodes[nd].m, M, sizeof M); g->nodes[nd].has_m = 1; }
    return nd;
}

int gx_ground(gx *g, const texture *t, float cx, float cz, float half, float per_cm, int parent)
{
    float pos[18], nrm[18], uv[12];
    static const int Q[6] = {0, 1, 2, 0, 2, 3};
    const float X[4] = {cx - half, cx + half, cx + half, cx - half}, Z[4] = {cz - half, cz - half, cz + half, cz + half};
    int k, mat, pa, na, ua;
    if (!t || !t->rgba) return -1;
    for (k = 0; k < 6; k++) {
        pos[k * 3] = X[Q[k]] / 100.0f; pos[k * 3 + 1] = 0; pos[k * 3 + 2] = -Z[Q[k]] / 100.0f;
        nrm[k * 3] = 0; nrm[k * 3 + 1] = 1; nrm[k * 3 + 2] = 0;
        uv[k * 2] = X[Q[k]] * per_cm; uv[k * 2 + 1] = Z[Q[k]] * per_cm;   /* (glr: world-anchored, u = x, v = game z) */
    }
    if ((mat = material_of(g, 0, t, 0, 0, 0x08)) < 0) return -1;
    pa = add_accessor(g, pos, 6, 3, 1); na = add_accessor(g, nrm, 6, 3, 0); ua = add_accessor(g, uv, 6, 2, 0);
    if (pa < 0 || na < 0 || ua < 0) return -1;
    if (g->nmeshes) gb_str(&g->meshes, ",");
    gb_printf(&g->meshes, "{\"name\":\"ground\",\"primitives\":[{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d,\"TEXCOORD_0\":%d},\"material\":%d,\"mode\":4}]}", pa, na, ua, mat);
    return add_node(g, "ground", g->nmeshes++, parent, "{\"mw2\":{\"ground\":true}}");
}

static float srgb_to_linear(int c) { const float x = (float)c / 255.0f; return x <= 0.04045f ? x / 12.92f : powf((x + 0.055f) / 1.055f, 2.4f); }

int gx_write_glb(gx *g, const char *path)
{
    gbuf js = {0}, out = {0};
    int i, k, rc = -1, *img_view = NULL, first;
    FILE *f;
    /* the images (PNG) into the binary chunk */
    if (g->nimgs && !(img_view = calloc((size_t)g->nimgs, sizeof *img_view))) return -1;
    for (i = 0; i < g->nimgs; i++) {
        gbuf png = {0};
        uint8_t *px = tex_rgba_bytes(g->imgs[i]);
        img_view[i] = -1;
        if (px && png_mem(&png, px, g->imgs[i]->w, g->imgs[i]->h) == 0) img_view[i] = add_view(g, png.p, png.n, 0);
        free(px); free(png.p);
    }
    gb_str(&js, "{\"asset\":{\"version\":\"2.0\",\"generator\":\"MW2 port map viewer (mapview)\"}");
    if (g->used_unlit) gb_str(&js, ",\"extensionsUsed\":[\"KHR_materials_unlit\"]");
    /* the scene: the root nodes */
    gb_str(&js, ",\"scene\":0,\"scenes\":[{\"nodes\":[");
    for (i = 0, first = 1; i < g->nnodes; i++) if (g->nodes[i].parent < 0) { gb_printf(&js, "%s%d", first ? "" : ",", i); first = 0; }
    gb_str(&js, "]}]");
    /* nodes */
    gb_str(&js, ",\"nodes\":[");
    for (i = 0; i < g->nnodes; i++) {
        const gx_node *n = &g->nodes[i];
        char nm[140];
        int c, fc = 1;
        gx_json_str(nm, sizeof nm, n->name);
        gb_printf(&js, "%s{\"name\":\"%s\"", i ? "," : "", nm);
        if (n->mesh >= 0) gb_printf(&js, ",\"mesh\":%d", n->mesh);
        if (n->has_m) {
            gb_str(&js, ",\"matrix\":[");
            for (k = 0; k < 16; k++) gb_printf(&js, "%s%.7g", k ? "," : "", n->m[k]);
            gb_str(&js, "]");
        } else if (n->t[0] != 0 || n->t[1] != 0 || n->t[2] != 0) gb_printf(&js, ",\"translation\":[%.7g,%.7g,%.7g]", n->t[0], n->t[1], n->t[2]);
        for (c = 0; c < g->nnodes; c++)
            if (g->nodes[c].parent == i) { gb_printf(&js, "%s%d", fc ? ",\"children\":[" : ",", c); fc = 0; }
        if (!fc) gb_str(&js, "]");
        if (n->extras) gb_printf(&js, ",\"extras\":%s", n->extras);
        gb_str(&js, "}");
    }
    gb_str(&js, "]");
    if (g->nmeshes) { gb_str(&js, ",\"meshes\":["); gb_put(&js, g->meshes.p, g->meshes.n); gb_str(&js, "]"); }
    /* materials */
    if (g->nmats) {
        gb_str(&js, ",\"materials\":[");
        for (i = 0; i < g->nmats; i++) {
            const gx_mat *m = &g->mats[i];
            if (i) gb_str(&js, ",");
            if (m->kind == 0) {
                char cel[40];
                gx_json_str(cel, sizeof cel, m->slot >= 0 && m->slot < 256 ? g->names[m->slot] : "");
                gb_printf(&js, "{\"name\":\"slot_%02x%s%s\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":%d},\"metallicFactor\":0,\"roughnessFactor\":1},\"doubleSided\":true",
                          m->slot & 0xff, cel[0] ? "_" : "", cel, m->img);
                if (m->alpha) gb_str(&js, ",\"alphaMode\":\"MASK\",\"alphaCutoff\":0.5");
                gb_printf(&js, ",\"extras\":{\"mw2\":{\"slot\":%d,\"cel\":\"%s\"}}}", m->slot, cel);
            } else {
                const uint8_t *c = g->pal[m->pal & 0xff];
                gb_printf(&js, "{\"name\":\"%s_%02x\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[%.5f,%.5f,%.5f,1],\"metallicFactor\":0,\"roughnessFactor\":1},\"doubleSided\":true",
                          m->kind == 2 ? "outline" : "colour", m->pal & 0xff, srgb_to_linear(c[0]), srgb_to_linear(c[1]), srgb_to_linear(c[2]));
                if (m->kind == 2) gb_str(&js, ",\"extensions\":{\"KHR_materials_unlit\":{}}");
                gb_printf(&js, ",\"extras\":{\"mw2\":{\"palette\":%d%s}}}", m->pal & 0xff, m->kind == 2 ? ",\"outline\":true" : "");
            }
        }
        gb_str(&js, "]");
    }
    /* textures, images, samplers */
    if (g->nimgs) {
        gb_str(&js, ",\"samplers\":[{\"magFilter\":9729,\"minFilter\":9987,\"wrapS\":10497,\"wrapT\":10497}],\"textures\":[");
        for (i = 0; i < g->nimgs; i++) gb_printf(&js, "%s{\"sampler\":0,\"source\":%d}", i ? "," : "", i);
        gb_str(&js, "],\"images\":[");
        for (i = 0; i < g->nimgs; i++) {
            char nm[40] = "";
            for (k = 0; k < 256; k++) if (g->slots[k] == g->imgs[i] && g->names[k][0]) { gx_json_str(nm, sizeof nm, g->names[k]); break; }
            if (!nm[0]) snprintf(nm, sizeof nm, "image%d", i);
            gb_printf(&js, "%s{\"name\":\"%s\",\"mimeType\":\"image/png\",\"bufferView\":%d}", i ? "," : "", nm, img_view[i] >= 0 ? img_view[i] : 0);
        }
        gb_str(&js, "]");
    }
    if (g->naccessors) { gb_str(&js, ",\"accessors\":["); gb_put(&js, g->accessors.p, g->accessors.n); gb_str(&js, "]"); }
    while (g->bin.n & 3) gb_put(&g->bin, NULL, 1);
    if (g->nviews) {
        gb_str(&js, ",\"bufferViews\":[");
        for (i = 0; i < g->nviews; i++) {
            gb_printf(&js, "%s{\"buffer\":0,\"byteOffset\":%lu,\"byteLength\":%lu", i ? "," : "", (unsigned long)g->views[i].off, (unsigned long)g->views[i].len);
            if (g->views[i].target) gb_printf(&js, ",\"target\":%d", g->views[i].target);
            gb_str(&js, "}");
        }
        gb_str(&js, "]");
        gb_printf(&js, ",\"buffers\":[{\"byteLength\":%lu}]", (unsigned long)g->bin.n);
    }
    gb_str(&js, "}");
    while (js.n & 3) gb_put(&js, " ", 1);
    /* GLB: header, JSON chunk, BIN chunk */
    gb_le32(&out, 0x46546c67u); gb_le32(&out, 2);
    gb_le32(&out, (uint32_t)(12 + 8 + js.n + (g->bin.n ? 8 + g->bin.n : 0)));
    gb_le32(&out, (uint32_t)js.n); gb_le32(&out, 0x4e4f534au); gb_put(&out, js.p, js.n);
    if (g->bin.n) { gb_le32(&out, (uint32_t)g->bin.n); gb_le32(&out, 0x004e4942u); gb_put(&out, g->bin.p, g->bin.n); }
    if (out.p && (f = fopen(path, "wb"))) {
        rc = fwrite(out.p, 1, out.n, f) == out.n ? 0 : -1;
        if (fclose(f) != 0) rc = -1;
    }
    free(js.p); free(out.p); free(img_view);
    return rc;
}
