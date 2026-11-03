/*
 * rb_gl12_world.c - the world and HUD programs (shaders/world.vert, world.frag, hud.frag) carried out on OpenGL 1.2's
 * fixed-function pipeline (the RB=gl12 backend, src/rb_gl12.c; docs/RENDERER_GL12.md).
 *
 * The vertex stage is world.vert in C, per vertex; its clip-space positions go to GL with identity matrices, so GL clips
 * and rasterizes the triangles as the GPU did. The fragment stage becomes per-triangle fixed-function classes:
 *  - texture x vertex colour (the lighting, tint and fades per vertex), the alpha test for the shader's discards,
 *    texture variants for its per-texel steps (alpha squared, opaque, the DOS shade levels);
 *  - the PowerVR table fog per pixel: a 1D texture on unit 1 (GL_DECAL; RGB the fog colour, A the factor) indexed by
 *    density / z - the coordinate (K, 0, 0, z) at each vertex, which GL's perspective interpolation turns into exactly
 *    K / z per pixel; the Titanium fog the same way from a ramp; one texture unit: a second pass (rb_gl12.c);
 *  - the ATi look's screen-linear (affine) texels: coordinates (s w, t w, 0, w) - GL divides by the interpolated w;
 *  - the Matrox look's ground: triangles split at view depths 2500 / 5000 / 15000, each piece at its mip level;
 *  - the DOS look: shade levels per polygon; the Gouraud dither and the horizon band's by cutting along the shade's
 *    iso-lines (screen space) and drawing the checkerboards' two indices through complementary polygon stipples.
 * Approximations (docs/RENDERER_GL12.md): a point light's direction per vertex (per triangle in the DOS look); the
 * DOS depth fog without a fog key per triangle; the DOS span coverage (half a pixel more right and below) and the
 * Gouraud checker's phase per span not reproduced (one phase per polygon); wireframe edges as GL_LINES.
 */
#include "rb_gl12.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FPV 13   /* floats a world vertex (pos 3, normal 3, uv 2, colour 3, texsel, edge) */

/* world.vert's outputs */
typedef struct {
    float clip[4];
    float normal[3], uv[2], color[3], viewpos[3], world[3], tic[3], sph[4];
    float dkey, tivf;
    int texsel, emask, bb, tif, dnib, ticm;
} wv;

static const float *g_v0, *g_v1, *g_v2, *g_sb;   /* the bound buffers (vertex 0 at index 0), the storage buffer */
static size_t g_n0, g_n1, g_n2, g_nsb;           /* their sizes in vertices (floats for the storage buffer) */

static void mat4v(const float *m, const float v[4], float o[4])
{
    int r;
    for (r = 0; r < 4; r++) o[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
}
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float len3(const float *a) { return sqrtf(dot3(a, a)); }

/* ---- the DOS look's integer projection (world_common.glsl) */
static int dos_idiv(int n, int d) { int q = abs(n) / abs(d); return (n < 0) != (d < 0) ? -q : q; }
static float dos_rnd(float v) { return floorf(v + 0.5f); }
static int dos_pq(int c, int s, int z)
{
    int q;
    if (abs(c) < (1 << (30 - s))) q = dos_idiv(c * (1 << s), z); else q = (int)truncf((float)c * exp2f((float)s) / (float)z);
    return (q + 2) >> 2;
}
static float dos_vf(int k, int f) { size_t i = (size_t)k * FPV + (size_t)f; return g_sb && k >= 0 && i < g_nsb ? g_sb[i] : 0.0f; }
static void dos_vpos(int k, float o[3]) { o[0] = dos_vf(k, 0); o[1] = dos_vf(k, 1); o[2] = dos_vf(k, 2); }
static void dos_d(const float w[3], float d[3])
{
    const float *e = G.F.u_dos_eye;
    d[0] = dos_rnd(w[0]) - dos_rnd(e[0]); d[1] = dos_rnd(w[1]) - dos_rnd(e[1]); d[2] = dos_rnd(-w[2]) - dos_rnd(-e[2]);
}
static float dos_camz(const float w[3]) { float d[3]; dos_d(w, d); return dos_rnd(dot3(G.F.u_dos_rz, d)); }
static void dos_scr(const float w[3], int o[2])
{
    float d[3];
    int xc, yc, zc, cx = (int)G.F.u_vp[0] / 2, cy = (int)G.F.u_vp[1] / 2;
    dos_d(w, d);
    xc = (int)dos_rnd(dot3(G.F.u_dos_rx, d)); yc = (int)dos_rnd(dot3(G.F.u_dos_ry, d)); zc = (int)dos_rnd(dot3(G.F.u_dos_rz, d));
    if (zc < 1) zc = 1;
    o[0] = cx + dos_pq(xc, G.F.u_dos_ps[0], zc); o[1] = cy - dos_pq(yc, G.F.u_dos_ps[1], zc);
}
static int dos_ebits(int b) { return ((int)(dos_vf(b, 12) + 0.5f) % 32) / 3; }

/* ---- the vertex stage (world.vert) */
static void vertex_stage(int vid, int i, wv *o)
{
    const float *a = g_v0 && (size_t)i < g_n0 ? g_v0 + (size_t)i * FPV : NULL;
    static const float Z[FPV];
    const float *sph = g_v1 && (size_t)i < g_n1 ? g_v1 + (size_t)i * 4 : Z, *tc = g_v2 && (size_t)i < g_n2 ? g_v2 + (size_t)i * 4 : Z;
    const rb_world_frame *F = &G.F;
    float p[4], nrm[3], q[4];
    int e, pit, hi, corner;
    if (!a) a = Z;
    e = (int)(a[12] + 0.5f); pit = e / 65536;
    e -= 65536 * pit;
    o->dnib = e / 2048; e -= o->dnib * 2048;
    o->bb = e >= 1024 ? 1 : 0;
    o->tif = 0;
    if (e < 1024 && e >= 512) { o->tif |= 2; e -= 512; }
    if (e < 1024 && e >= 256) { o->tif |= 1; e -= 256; }
    p[0] = a[0]; p[1] = a[1]; p[2] = a[2]; p[3] = 1.0f;
    memcpy(nrm, a + 3, sizeof nrm);
    mat4v(F->u_mvp, p, o->clip);
    if (e >= 1024) {   /* a colour type 3 billboard (world.vert) */
        float cb[4], ca[4], pa[4] = {a[3], a[4], a[5], 1.0f};
        int k = (int)(a[11] + 0.5f), tib = k >= 4 ? 1 : 0;
        mat4v(F->u_mvp, p, cb); mat4v(F->u_mvp, pa, ca);
        e -= 1024; nrm[0] = nrm[1] = nrm[2] = 0.0f; k -= 4 * tib;
        if (ca[3] <= 1.0f || cb[3] <= 1.0f) { o->clip[0] = o->clip[1] = o->clip[2] = 2.0f; o->clip[3] = 1.0f; }
        else {
            float hx = 0.5f * F->u_vp[0], hy = 0.5f * F->u_vp[1], cx, cy;
            float Tx = ca[0] / ca[3] * hx, Ty = -(ca[1] / ca[3] * hy), Fx = cb[0] / cb[3] * hx, Fy = -(cb[1] / cb[3] * hy);
            if (tib) {
                float dx = 0.5f * (Fy - Ty), dy = 0.5f * (Fx - Tx);
                if (k == 0) { cx = Tx - dx; cy = Ty - dy; } else if (k == 1) { cx = Fx - dx; cy = Fy + dy; }
                else if (k == 2) { cx = Tx + dx; cy = Fy - dy; } else { cx = Fx + dx; cy = Ty + dy; }
                o->clip[2] = k == 0 || k == 3 ? ca[2] / ca[3] : cb[2] / cb[3];
                if (k == 0 || k == 3) { p[0] = a[3]; p[1] = a[4]; p[2] = a[5]; }
            } else {
                float dx = 0.5f * (Fx - Tx), dy = 0.5f * (Fy - Ty);
                if (k == 0) { cx = Tx - dy; cy = Ty + dx; } else if (k == 1) { cx = Tx + dy; cy = Ty - dx; }
                else if (k == 2) { cx = Fx + dy; cy = Fy - dx; } else { cx = Fx - dy; cy = Fy + dx; }
                o->clip[2] = k < 2 ? ca[2] / ca[3] : cb[2] / cb[3];
                if (k < 2) { p[0] = a[3]; p[1] = a[4]; p[2] = a[5]; }
            }
            o->clip[0] = cx / hx; o->clip[1] = -cy / hy; o->clip[3] = 1.0f;
        }
    }
    /* (the DOS Gouraud fill's coverage push-out is not done: GL's coverage rule - docs/RENDERER_GL12.md) */
    if (pit > 0 && F->u_dos != 0) {   /* the DOS cockpit set's draw order */
        int ix = pit - 1 < 127 ? pit - 1 : 127, rk = F->u_pit_rank4[ix >> 2][ix & 3];
        if (rk < 0) { o->clip[0] = o->clip[1] = o->clip[2] = 2.0f; o->clip[3] = 1.0f; }
        else o->clip[2] = (-1.0f + 0.01f * (1.0f - (float)(rk + 1) / 256.0f)) * o->clip[3];
    }
    if (F->u_far > 0.0f && sph[3] > 0.0f) {   /* the view distance */
        float sc4[4] = {sph[0], sph[1], sph[2], 1.0f}, sc[4], rr = F->u_far + sph[3];
        mat4v(F->u_view, sc4, sc);
        if (sc[0] * sc[0] + sc[1] * sc[1] + sc[2] * sc[2] > rr * rr || -sc[2] - sph[3] > F->u_far) { o->clip[0] = o->clip[1] = o->clip[2] = 2.0f; o->clip[3] = 1.0f; }
    }
    o->dkey = -1.0f;
    if (F->u_dos != 0 && G.prog == RB_PROG_WORLD_DOS && g_sb) {   /* the DOS fog key: the polygon's farthest corner */
        int t = vid / 3, k, c;
        if (fabsf(dos_vf(vid, 0) - a[0]) + fabsf(dos_vf(vid, 2) - a[2]) < 0.01f) {
            float km = -1e9f, w3[3];
            for (k = 0; k < 16 && t > 0; k++) { if (dos_ebits(3 * t) & 4) break; t--; }
            for (k = 0; k < 16; k++) {
                int b = 3 * (t + k);
                for (c = 0; c < 3; c++) { float z; dos_vpos(b + c, w3); z = dos_camz(w3); if (z > km) km = z; }
                if (dos_ebits(b) & 2) break;
            }
            o->dkey = km;
        }
    }
    mat4v(F->u_view, p, q);
    memcpy(o->viewpos, q, sizeof o->viewpos);
    memcpy(o->world, p, sizeof o->world);
    {
        float vz = -o->viewpos[2] > 1.0f ? -o->viewpos[2] : 1.0f, d = F->u_tifog[1] > 0.0f ? 1.0f - (vz / F->u_tifog[1] < 1.0f ? vz / F->u_tifog[1] : 1.0f) : 1.0f;
        o->tivf = 1.0f - d * F->u_tifog[3];
    }
    memcpy(o->normal, nrm, sizeof nrm);
    o->uv[0] = a[6]; o->uv[1] = a[7];
    memcpy(o->color, a + 8, sizeof o->color);
    o->texsel = (int)floorf(a[11] + 0.5f);
    memcpy(o->sph, sph, sizeof o->sph);
    memcpy(o->tic, tc, sizeof o->tic);
    o->ticm = (int)(tc[3] + 0.5f);
    hi = e / 32; e -= hi * 32; corner = e - 3 * (e / 3);
    (void)corner;
    o->emask = (e / 3) | (hi << 3);
}

/* ---- colours */
static void dos_colour(float idx, float out[4])
{
    int w = 0, h = 0, i = (int)idx;
    const uint8_t *tab = gl12_tex_texels(G.tex[1], &w, &h);
    if (i < 0) i = 0;
    if (i > 255) i = 255;
    out[3] = 1.0f;
    if (!tab || w != 256 || h < 17) { out[0] = out[1] = out[2] = 1.0f; return; }
    tab += ((size_t)16 * 256 + (size_t)i) * 4;
    out[0] = tab[0] / 255.0f; out[1] = tab[1] / 255.0f; out[2] = tab[2] / 255.0f;
}

/* the engine's vector length (0x100462fe) and the Titanium lights (world.frag ti_light), at a vertex */
static float ti_len(const float *v)
{
    float a = fabsf(v[0]), b = fabsf(v[1]), c = fabsf(v[2]), m = a > b ? (a > c ? a : c) : (b > c ? b : c);
    return m + (a + b + c - m) * 0.25f;
}
static void ti_light(const wv *v, const wv *flat, const float nn[3], float out[3])
{
    const rb_world_frame *F = &G.F;
    float c[3], acc[3];
    int i, j, k, hit = 0;
    memcpy(c, F->u_tiamb, sizeof c);
    for (i = 0; i < 2; i++) {
        if (F->u_tilt[i] == 1) { float d = -dot3(F->u_tilv[i], nn); if (d > 0) for (k = 0; k < 3; k++) c[k] += F->u_tilc[i][k] * d; }
        else if (F->u_tilt[i] == 2) {
            float l[3] = {F->u_tilv[i][0] - v->world[0], F->u_tilv[i][1] - v->world[1], F->u_tilv[i][2] - v->world[2]}, d = dot3(l, nn), ll = ti_len(l);
            if (d > 0) for (k = 0; k < 3; k++) c[k] += F->u_tilc[i][k] * d / (ll > 1e-3f ? ll : 1e-3f);
        } else for (k = 0; k < 3; k++) c[k] += F->u_tilc[i][k];
    }
    for (k = 0; k < 3; k++) if (c[k] > 1.0f) c[k] = 1.0f;
    memcpy(acc, c, sizeof acc);
    for (j = 0; j < 8 && j < F->u_tidn; j++) {
        float dd[3], l[3], d, ll;
        for (k = 0; k < 3; k++) dd[k] = fabsf(flat->sph[k] - F->u_tidp[j][k]);
        if (flat->sph[3] > 0 && (dd[0] > F->u_tidp[j][3] + flat->sph[3] || dd[1] > F->u_tidp[j][3] + flat->sph[3] || dd[2] > F->u_tidp[j][3] + flat->sph[3])) continue;
        for (k = 0; k < 3; k++) l[k] = F->u_tidp[j][k] - v->world[k];
        d = dot3(l, nn); ll = ti_len(l);
        hit = 1;
        if (d > 0) for (k = 0; k < 3; k++) acc[k] += F->u_tidc[j][k] * d / (ll > 1e-3f ? ll : 1e-3f);
    }
    for (k = 0; k < 3; k++) { float s = hit ? c[k] + acc[k] : c[k]; out[k] = s < 1.0f ? s : 1.0f; }
}

/* ---- output */
static gl12_vtx *g_lines;   /* the wireframe's edges, drawn after the draw's fills */
static int g_nlines, g_caplines;
static gl12_key g_lkey;

static void put_line(const gl12_vtx *a, const gl12_vtx *b, const float col[4])
{
    if (g_nlines + 2 > g_caplines) {
        int nc = g_caplines ? g_caplines * 2 : 256;
        gl12_vtx *q = realloc(g_lines, (size_t)nc * sizeof *q);
        if (!q) return;
        g_lines = q; g_caplines = nc;
    }
    g_lines[g_nlines] = *a; g_lines[g_nlines + 1] = *b;
    memcpy(g_lines[g_nlines].c, col, 4 * sizeof(float));
    memcpy(g_lines[g_nlines + 1].c, col, 4 * sizeof(float));
    g_nlines += 2;
}

static void key_init(gl12_key *k)
{
    memset(k, 0, sizeof *k);
    k->lod = -1;
    k->blend = -1;
}

/* a vertex for GL from the stage's output */
static void out_vtx(gl12_vtx *d, const wv *s, const float col[4])
{
    memset(d, 0, sizeof *d);
    memcpy(d->p, s->clip, sizeof d->p);
    memcpy(d->c, col, sizeof d->c);
    d->t0[3] = 1.0f; d->t1[3] = 1.0f;
}

/* the part of a convex polygon (n corners; z each corner's view depth, linear in clip space as every attribute sent)
 * with lo <= z < hi, into out / oz; its corner count */
static int depth_clip(const gl12_vtx *v, const float *z, int n, float lo, float hi, gl12_vtx *out, float *oz)
{
    gl12_vtx poly[12], tmp[12];
    float pz[12], tz[12];
    int s, i;
    memcpy(poly, v, (size_t)n * sizeof *v);
    memcpy(pz, z, (size_t)n * sizeof *z);
    for (s = 0; s < 2 && n >= 3; s++) {   /* keep z >= lo (s 0), z < hi (s 1) */
        const float lim = s == 0 ? lo : hi;
        int m = 0;
        if ((s == 0 && lo <= -1e29f) || (s == 1 && hi >= 1e29f)) continue;
        for (i = 0; i < n && m < 10; i++) {
            int j = (i + 1) % n, ia = s == 0 ? pz[i] >= lim : pz[i] < lim, ib = s == 0 ? pz[j] >= lim : pz[j] < lim;
            if (ia) { tmp[m] = poly[i]; tz[m++] = pz[i]; }
            if (ia != ib) {
                const int l = pz[i] < pz[j] ? i : j, h = l == i ? j : i;   /* (a fixed order: shared edges cut alike) */
                float t = (lim - pz[l]) / (pz[h] - pz[l]);
                gl12_vtx *o = &tmp[m];
                int c;
                for (c = 0; c < 4; c++) {
                    o->p[c] = poly[l].p[c] + (poly[h].p[c] - poly[l].p[c]) * t;
                    o->c[c] = poly[l].c[c] + (poly[h].c[c] - poly[l].c[c]) * t;
                    o->t0[c] = poly[l].t0[c] + (poly[h].t0[c] - poly[l].t0[c]) * t;
                    o->t1[c] = poly[l].t1[c] + (poly[h].t1[c] - poly[l].t1[c]) * t;
                }
                tz[m++] = lim;
            }
        }
        n = m;
        memcpy(poly, tmp, (size_t)n * sizeof *poly);
        memcpy(pz, tz, (size_t)n * sizeof *pz);
    }
    if (n >= 3) { memcpy(out, poly, (size_t)n * sizeof *out); memcpy(oz, pz, (size_t)n * sizeof *oz); }
    return n >= 3 ? n : 0;
}
static void put_fan(const gl12_key *k, const gl12_vtx *p, int n)
{
    int i;
    for (i = 1; i + 1 < n; i++) {
        gl12_vtx t3[3];
        t3[0] = p[0]; t3[1] = p[i]; t3[2] = p[i + 1];
        gl12_batch(k, t3, 3);
    }
}

/* the Matrox look's mip bands: the triangle split at view depths 2500 / 5000 / 15000 (world.frag u_mga_lod), each piece
 * at its level */
static void band_split(const gl12_key *k0, const gl12_vtx *v, const float *z)
{
    static const float D[5] = {-1e30f, 2500.0f, 5000.0f, 15000.0f, 1e30f};
    int b;
    for (b = 0; b < 4; b++) {
        gl12_vtx p[12];
        float pz[12];
        int n = depth_clip(v, z, 3, D[b], D[b + 1], p, pz);
        gl12_key k = *k0;
        k.lod = b;
        if (n) put_fan(&k, p, n);
    }
}

/* the Titanium per-pixel fog, 1 - (1 - min(1, z / Z)) w: linear in the view depth up to Z (the ramp's coordinate from
 * each corner's depth - GL's perspective interpolation keeps it exact), 1 beyond (the triangle split at Z) */
static float ramp_s(float ff) { return (0.5f + 255.0f * ff) / 256.0f; }
static void tifog_split(const gl12_key *k, const gl12_vtx *v, const float *z)
{
    const float Z = G.F.u_tifog[1], W = G.F.u_tifog[3];
    gl12_vtx p[12];
    float pz[12];
    int n, i;
    if (Z <= 0.0f) {
        gl12_vtx q[3];
        memcpy(q, v, sizeof q);
        for (i = 0; i < 3; i++) { q[i].t1[0] = ramp_s(1.0f - W); q[i].t1[3] = 1.0f; }
        gl12_batch(k, q, 3);
        return;
    }
    if ((n = depth_clip(v, z, 3, -1e30f, Z, p, pz)) != 0) {
        for (i = 0; i < n; i++) { p[i].t1[0] = ramp_s(1.0f - W + W * pz[i] / Z); p[i].t1[3] = 1.0f; }
        put_fan(k, p, n);
    }
    if ((n = depth_clip(v, z, 3, Z, 1e30f, p, pz)) != 0) {
        for (i = 0; i < n; i++) { p[i].t1[0] = ramp_s(1.0f); p[i].t1[3] = 1.0f; }
        put_fan(k, p, n);
    }
}

/* ---- screen-space polygons (the DOS look): NDC x, y, z and a scalar, cut along the scalar's iso-lines */
typedef struct { float x, y, z, s; } sv;

static int clip_s(const sv *in, int n, sv *out, float lim, int keep_greater)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const sv *a = &in[i], *b = &in[(i + 1) % n];
        int ia = keep_greater ? a->s >= lim : a->s < lim, ib = keep_greater ? b->s >= lim : b->s < lim;
        if (ia) out[m++] = *a;
        if (ia != ib) {
            const sv *p = a->s < b->s ? a : b, *q = p == a ? b : a;   /* (a fixed order: shared edges cut alike) */
            float t = (lim - p->s) / (q->s - p->s);
            sv *o = &out[m++];
            o->x = p->x + (q->x - p->x) * t; o->y = p->y + (q->y - p->y) * t; o->z = p->z + (q->z - p->z) * t; o->s = lim;
        }
    }
    return m;
}

static gl12_key g_ekey;   /* the wireframe's edge bands (deferred: drawn after the draw's fills) */
static gl12_vtx *g_edges;
static int g_nedges, g_capedges;

/* a convex polygon (NDC, w 1) in one colour (stipple: 0, or 1 / 2 the pixels with x + y even / odd) */
static void emit_poly(const gl12_key *k0, const sv *p, int n, const float col[4], int stipple)
{
    gl12_key k = *k0;
    int i;
    if (k0 == &g_ekey) {   /* deferred */
        for (i = 1; i + 1 < n; i++) {
            const sv *e[3] = {&p[0], &p[i], &p[i + 1]};
            int c;
            if (g_nedges + 3 > g_capedges) {
                int nc = g_capedges ? g_capedges * 2 : 768;
                gl12_vtx *q = realloc(g_edges, (size_t)nc * sizeof *q);
                if (!q) return;
                g_edges = q; g_capedges = nc;
            }
            for (c = 0; c < 3; c++) {
                gl12_vtx *t = &g_edges[g_nedges++];
                memset(t, 0, sizeof *t);
                t->p[0] = e[c]->x; t->p[1] = e[c]->y; t->p[2] = e[c]->z; t->p[3] = 1.0f;
                memcpy(t->c, col, sizeof t->c);
                t->t0[3] = t->t1[3] = 1.0f;
            }
        }
        return;
    }
    k.stipple = stipple;
    for (i = 1; i + 1 < n; i++) {
        gl12_vtx t[3];
        const sv *e[3] = {&p[0], &p[i], &p[i + 1]};
        int c;
        for (c = 0; c < 3; c++) {
            memset(&t[c], 0, sizeof t[c]);
            t[c].p[0] = e[c]->x; t[c].p[1] = e[c]->y; t[c].p[2] = e[c]->z; t[c].p[3] = 1.0f;
            memcpy(t[c].c, col, sizeof t[c].c);
            t[c].t0[3] = t[c].t1[3] = 1.0f;
        }
        gl12_batch(&k, t, 3);
    }
}
/* the same with each corner's colour from its scalar: the palette blended between floor(s) and floor(s) + 1 (m: the
 * piece's integer part, its corners all within m .. m + 1) */
static void emit_poly_blend(const gl12_key *k0, const sv *p, int n, int m)
{
    float a[4], b[4];
    int i, c;
    dos_colour((float)m, a); dos_colour((float)m + 1.0f, b);
    for (i = 1; i + 1 < n; i++) {
        gl12_vtx t[3];
        const sv *e[3] = {&p[0], &p[i], &p[i + 1]};
        for (c = 0; c < 3; c++) {
            float f = e[c]->s - (float)m;
            int q;
            memset(&t[c], 0, sizeof t[c]);
            t[c].p[0] = e[c]->x; t[c].p[1] = e[c]->y; t[c].p[2] = e[c]->z; t[c].p[3] = 1.0f;
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            for (q = 0; q < 3; q++) t[c].c[q] = a[q] + (b[q] - a[q]) * f;
            t[c].c[3] = 1.0f;
            t[c].t0[3] = t[c].t1[3] = 1.0f;
        }
        gl12_batch(k0, t, 3);
    }
}

/* the dithered fill of a polygon whose palette index runs screen-linearly with s: the pieces between the iso-lines
 * s = j / 2; mode 0 (DOS Gouraud, s = c + 0.5): [n, n + 0.5) flat n, [n + 0.5, n + 1) the checkerboard of n + 1 on the
 * pixels of parity `even` (1: x + y even) and n on the others; mode 1 (the horizon band): [n, n + 0.5) the
 * checkerboard (n + 1 on `even`), [n + 0.5, n + 1) flat n + 1; cap: the largest index */
/* the checkerboard's phase by window row band (NDC y0 .. y1, bottom up; the first and last open-ended): even 1 - n + 1 on
 * the pixels with x + y even. None: one phase (dither_fill's `even`) */
typedef struct { float y0, y1; int even; } dstrip;
static dstrip g_strip[1100];
static int g_nstrip;

static void dither_fill(const gl12_key *k, const sv *p, int n, int mode, int even, int cap)
{
    float lo = 1e30f, hi = -1e30f;
    int i, j0, j1, j;
    for (i = 0; i < n; i++) { if (p[i].s < lo) lo = p[i].s; if (p[i].s > hi) hi = p[i].s; }
    j0 = (int)floorf(lo * 2.0f); j1 = (int)floorf(hi * 2.0f);
    if (j1 - j0 > 600) j0 = j1 - 600;
    for (j = j0; j <= j1; j++) {
        sv a[24], b[24];
        int m = n, nn = j >> 1, odd = j & 1, chk, i0, i1;
        float c0[4], c1[4];
        memcpy(a, p, (size_t)n * sizeof *p);
        if (j > j0) { m = clip_s(a, m, b, (float)j * 0.5f, 1); memcpy(a, b, (size_t)m * sizeof *a); }
        if (j < j1 && m >= 3) { m = clip_s(a, m, b, (float)(j + 1) * 0.5f, 0); memcpy(a, b, (size_t)m * sizeof *a); }
        if (m < 3) continue;
        chk = mode == 0 ? odd : !odd;
        i0 = nn; i1 = nn + 1;
        if (mode == 1 && odd) i0 = nn + 1;
        if (i0 > cap) i0 = cap;
        if (i1 > cap) i1 = cap;
        dos_colour((float)(i0 & 255), c0);
        if (!chk || i0 == i1) { emit_poly(k, a, m, c0, 0); continue; }
        dos_colour((float)(i1 & 255), c1);
        if (g_nstrip > 0) {   /* the phase per row band (DOS Gouraud: the span's left end) */
            float ylo = 1e30f, yhi = -1e30f;
            int q, t;
            for (q = 0; q < m; q++) { if (a[q].y < ylo) ylo = a[q].y; if (a[q].y > yhi) yhi = a[q].y; }
            for (t = 0; t < g_nstrip; t++) {
                sv e[24], f[24];
                int me = m;
                if (g_strip[t].y1 <= ylo || g_strip[t].y0 >= yhi) continue;
                memcpy(e, a, (size_t)m * sizeof *a);
                for (q = 0; q < me; q++) e[q].s = e[q].y;
                if (t > 0) { me = clip_s(e, me, f, g_strip[t].y0, 1); memcpy(e, f, (size_t)me * sizeof *e); }
                if (t + 1 < g_nstrip && me >= 3) { me = clip_s(e, me, f, g_strip[t].y1, 0); memcpy(e, f, (size_t)me * sizeof *e); }
                if (me < 3) continue;
                emit_poly(k, e, me, c1, g_strip[t].even ? 1 : 2);
                emit_poly(k, e, me, c0, g_strip[t].even ? 2 : 1);
            }
            continue;
        }
        emit_poly(k, a, m, c1, even ? 1 : 2);   /* n + 1 on the pixels of that parity, */
        emit_poly(k, a, m, c0, even ? 2 : 1);   /* n on the others (each pixel drawn once) */
    }
}
/* the smooth fill (dos_shading / dos_sky = smooth): the palette blended, cut at whole indices */
static void blend_fill(const gl12_key *k, const sv *p, int n)
{
    float lo = 1e30f, hi = -1e30f;
    int i, j0, j1, j;
    for (i = 0; i < n; i++) { if (p[i].s < lo) lo = p[i].s; if (p[i].s > hi) hi = p[i].s; }
    j0 = (int)floorf(lo); j1 = (int)floorf(hi);
    if (j1 - j0 > 300) j0 = j1 - 300;
    for (j = j0; j <= j1; j++) {
        sv a[24], b[24];
        int m = n;
        memcpy(a, p, (size_t)n * sizeof *p);
        if (j > j0) { m = clip_s(a, m, b, (float)j, 1); memcpy(a, b, (size_t)m * sizeof *a); }
        if (j < j1 && m >= 3) { m = clip_s(a, m, b, (float)(j + 1), 0); memcpy(a, b, (size_t)m * sizeof *a); }
        if (m >= 3) emit_poly_blend(k, a, m, j);
    }
}

/* ---- the DOS horizon fill (world.frag u_horizon; MW2.EXE 0x3c000): the viewport cut at the horizon, the band's
 * iso-lines; window coordinates in, NDC out */
static void horizon(void)
{
    const rb_world_frame *F = &G.F;
    const float vx = (float)G.vp[0], vy = (float)G.vp[1], vw = (float)G.vp[2], vh = (float)G.vp[3];
    /* the corners in window coordinates; s: ray.y */
    float X[4] = {vx, vx + vw, vx + vw, vx}, Y[4] = {vy, vy, vy + vh, vy + vh};
    sv q[4], g[24], s[24];
    int i, ng, ns;
    float col[4];
    gl12_key k;
    key_init(&k);
    k.no_depth_write = 1;
    k.depth_func = GL_ALWAYS;
    for (i = 0; i < 4; i++) {
        q[i].x = (X[i] - vx) / vw * 2.0f - 1.0f; q[i].y = (Y[i] - vy) / vh * 2.0f - 1.0f; q[i].z = 0.999f;
        q[i].s = F->u_hz_r[1] * (X[i] - F->u_hz_c[0]) / F->u_hz_c[2] + F->u_hz_u[1] * (Y[i] - F->u_hz_c[1]) / F->u_hz_c[3] + F->u_hz_f[1];
    }
    ng = clip_s(q, 4, g, 0.0f, 0);   /* ray.y < 0: the ground (0xEF) */
    if (ng >= 3) { dos_colour(239.0f, col); emit_poly(&k, g, ng, col, 0); }
    ns = clip_s(q, 4, s, 0.0f, 1);   /* the sky (0xE0) and its band */
    if (ns < 3) return;
    if (F->u_hz_band > 0.0f && fabsf(F->u_hz_u[1]) > 1e-6f) {
        /* d = sy - sh(sx): rows above the line; the band 0 <= d < band, c = 238 - 14 d / band */
        float shl = F->u_hz_c[1] - F->u_hz_c[3] * ((F->u_hz_x[0] - F->u_hz_c[0]) * F->u_hz_r[1] / F->u_hz_c[2] + F->u_hz_f[1]) / F->u_hz_u[1];
        float shr = F->u_hz_c[1] - F->u_hz_c[3] * ((F->u_hz_x[1] - F->u_hz_c[0]) * F->u_hz_r[1] / F->u_hz_c[2] + F->u_hz_f[1]) / F->u_hz_u[1];
        float K = (shl > shr ? shl : shr) + F->u_hz_band;
        sv d[24], b[24], o[24];
        int nd, nb, no, A, even;
        for (i = 0; i < ns; i++) {
            float sx = (s[i].x + 1.0f) * 0.5f * vw + vx, sy = (s[i].y + 1.0f) * 0.5f * vh + vy;
            float sh = F->u_hz_c[1] - F->u_hz_c[3] * ((sx - F->u_hz_c[0]) * F->u_hz_r[1] / F->u_hz_c[2] + F->u_hz_f[1]) / F->u_hz_u[1];
            d[i] = s[i]; d[i].s = sy - sh;
        }
        nd = clip_s(d, ns, o, 0.0f, 0);   /* d < 0 (in the sky part: the line itself) */
        if (nd >= 3) { dos_colour(224.0f, col); emit_poly(&k, o, nd, col, 0); }
        nd = clip_s(d, ns, o, 0.0f, 1);
        no = clip_s(o, nd, b, F->u_hz_band, 1);   /* d >= band: the sky */
        if (no >= 3) { dos_colour(224.0f, col); emit_poly(&k, b, no, col, 0); }
        nb = clip_s(o, nd, b, F->u_hz_band, 0);
        if (nb >= 3) {
            for (i = 0; i < nb; i++) b[i].s = 238.0f - 14.0f * b[i].s / F->u_hz_band;
            if (F->u_hz_smooth) {   /* e = clamp(c + 0.25, 224, 238) */
                sv e1[24], e2[24];
                int n1, n2;
                for (i = 0; i < nb; i++) b[i].s += 0.25f;
                n1 = clip_s(b, nb, e1, 238.0f, 0);
                if (n1 >= 3) blend_fill(&k, e1, n1);
                n2 = clip_s(b, nb, e2, 238.0f, 1);
                if (n2 >= 3) { dos_colour(238.0f, col); emit_poly(&k, e2, n2, col, 0); }
            } else {
                /* the checker: (row + col) odd -> n + 1, row = floor(K - sy), col = sx - x0 at pixel centres: odd when
                 * (A + px + py) is, A = floor(K - 0.5) - x0 */
                A = (int)floorf(K - 0.5f) - (int)F->u_hz_x[0];
                even = (A & 1) != 0;   /* n + 1 on the pixels with x + y even when A is odd */
                dither_fill(&k, b, nb, 1, even, 238);
            }
        }
    } else { dos_colour(224.0f, col); emit_poly(&k, s, ns, col, 0); }
}

/* ---- one triangle of the world program */
static gl12_key g_k;            /* the draw's base key */
static int g_tex_ok;             /* unit 0's texture has texels */

/* the draw's vertex-stage outputs (the DOS spans' depth) and its first vertex in the layer; the last DOS polygon drawn
 * as spans (its fan's other triangles skip) */
static const wv *g_vs;
static int g_vfirst, g_vcount, g_lastpoly = -1;

/* the DOS palette colours through the palette-pair texture (GV_DOSPAIR of the DOS table, unit 0, nearest, s repeating):
 * a convex polygon (NDC, w 1) in colour lo, or - phase 0 / 1 - the checkerboard of lo and hi = lo + 1 with hi on the
 * pixels where x + y + phase is odd (column 1); s = window x + y + phase - 0.5 counts columns, t the pair's row. One
 * batch for the flat colours, the Gouraud runs and their checkerboards (no stipple, no texture switch) */
static gl12_key g_pkey;
static int g_pal_ok;
static void pal_poly(const sv *p, int n, int lo, int checker, int phase)
{
    int i, c;
    const float tt = ((float)(lo & 255) + 0.5f) / 256.0f;
    for (i = 1; i + 1 < n; i++) {
        gl12_vtx t[3];
        const sv *e[3] = {&p[0], &p[i], &p[i + 1]};
        for (c = 0; c < 3; c++) {
            memset(&t[c], 0, sizeof t[c]);
            t[c].p[0] = e[c]->x; t[c].p[1] = e[c]->y; t[c].p[2] = e[c]->z; t[c].p[3] = 1.0f;
            t[c].c[0] = t[c].c[1] = t[c].c[2] = t[c].c[3] = 1.0f;
            if (checker) {
                const float X = (e[c]->x + 1.0f) * 0.5f * (float)G.vp[2] + (float)G.vp[0], Y = (e[c]->y + 1.0f) * 0.5f * (float)G.vp[3] + (float)G.vp[1];
                t[c].t0[0] = (X + Y + (float)phase - 0.5f) * 0.5f;
            } else t[c].t0[0] = 0.25f;   /* (column 0's centre) */
            t[c].t0[1] = tt; t[c].t0[3] = 1.0f; t[c].t1[3] = 1.0f;
        }
        gl12_batch(&g_pkey, t, 3);
    }
}

/* the DOS Gouraud fill (MW2.EXE 0x59e77) exactly, as world.frag's dos_gouraud computes it per pixel: the polygon's
 * vertices projected as 0x3dfb7, its two chains walked row by row (dos_gchain), each row's span from the left chain's
 * x >> 16 to the right's inclusive, the shade stepped per pixel pair, the dither's 0x7fff on the pixels with k + r even.
 * Each row's span goes out as runs of one index - the even pixels' and the odd pixels' through the two checkerboard
 * stipples - at the polygon's depth plane. 0: not done (near-clipped, or no buffer: the caller's iso-line fill) */
static int dos_gouraud_spans(const wv *v0, int lev)
{
    const float g = ((float)lev + 1.0f) / 16.0f;
    const int s0 = v0->texsel - 1, vpo_x = (int)G.F.u_vpo[0], vpo_y = (int)G.F.u_vpo[1], vph = (int)G.F.u_vp[1];
    int V[20][2], CF[20], I[20], m = 0, t, top = 0, ybot, y, i;
    float pa = 0, pb = 0, pc = 0;   /* NDC z = pa x + pb y + pc */
    if (s0 < 0 || !g_sb || !g_pal_ok) return 0;
    if (s0 == g_lastpoly) return 1;   /* (drawn with its first triangle) */
    for (t = 0; t < 16; t++) {
        int b = s0 + 3 * t, c, c0 = t == 0 ? 0 : 1, last = (dos_ebits(b) & 2) != 0;
        for (c = c0; c < (last ? 3 : 2) && m < 18; c++) {
            float w3[3], q4[4], cl[4];
            dos_vpos(b + c, w3);
            q4[0] = w3[0]; q4[1] = w3[1]; q4[2] = w3[2]; q4[3] = 1.0f;
            mat4v(G.F.u_mvp, q4, cl);
            if (cl[2] + cl[3] < 0.0f || cl[3] <= 0.0f) return 0;   /* near-clipped */
            dos_scr(w3, V[m]);
            CF[m] = (int)floorf((dos_vf(b + c, 7) + dos_vf(b + c, 6) * g) * 65536.0f + 0.5f);
            I[m] = b + c;
            m++;
        }
        if (last) break;
    }
    if (m < 3) return 0;
    {   /* the depth plane from the vertex stage's corners (the cockpit's ranks included) */
        float X[20], Y[20], Z[20], best = 0;
        int n = 0, a, b, c;
        for (i = 0; i < m; i++) {
            int k = I[i] - g_vfirst;
            if (k < 0 || k >= g_vcount || g_vs[k].clip[3] <= 0.0f) continue;
            X[n] = g_vs[k].clip[0] / g_vs[k].clip[3]; Y[n] = g_vs[k].clip[1] / g_vs[k].clip[3]; Z[n] = g_vs[k].clip[2] / g_vs[k].clip[3]; n++;
        }
        if (n == 0) return 0;
        pc = Z[0];
        for (a = 0; a < n; a++)
            for (b = a + 1; b < n; b++)
                for (c = b + 1; c < n; c++) {
                    float d = (X[b] - X[a]) * (Y[c] - Y[a]) - (X[c] - X[a]) * (Y[b] - Y[a]);
                    if (fabsf(d) > best) {
                        best = fabsf(d);
                        pa = ((Z[b] - Z[a]) * (Y[c] - Y[a]) - (Z[c] - Z[a]) * (Y[b] - Y[a])) / d;
                        pb = ((X[b] - X[a]) * (Z[c] - Z[a]) - (X[c] - X[a]) * (Z[b] - Z[a])) / d;
                        pc = Z[a] - pa * X[a] - pb * Y[a];
                    }
                }
    }
    g_lastpoly = s0;
    ybot = V[0][1];
    for (i = 0; i < m; i++) { if (V[i][1] <= V[top][1]) top = i; if (V[i][1] > ybot) ybot = V[i][1]; }
    if (ybot - V[top][1] > 4096) return 1;
    for (y = V[top][1]; y <= ybot; y++) {
        int e[2][2], d, x0, w, st, r = y - V[top][1];
        const int py = vph - 1 - y + vpo_y;
        const float y0n = ((float)py - (float)G.vp[1]) / (float)G.vp[3] * 2.0f - 1.0f, y1n = ((float)(py + 1) - (float)G.vp[1]) / (float)G.vp[3] * 2.0f - 1.0f;
        for (d = 0; d < 2; d++) {   /* dos_gchain: the backward (-1) and forward chains at row y, (x, c) in 16.16 */
            const int dir = d ? 1 : -1;
            int a = top, b2 = (a + dir + m) % m, r0 = V[top][1], cnt, yq, gg;
            unsigned tt;
            for (gg = 0; gg < 20 && V[b2][1] == V[a][1] && b2 != top; gg++) { a = b2; b2 = (a + dir + m) % m; }
            cnt = V[b2][1] - V[a][1]; yq = y == ybot && y > r0 ? y - 1 : y;
            for (gg = 0; gg < 20 && yq >= r0 + cnt && b2 != top; gg++) { r0 += cnt; a = b2; b2 = (a + dir + m) % m; cnt = V[b2][1] - V[a][1]; if (cnt == 0) cnt = 1; }
            if (cnt == 0) cnt = 1;
            tt = (unsigned)(y - r0);
            e[d][0] = (int)((unsigned)(V[a][0] * 65536 + 0x8000) + tt * (unsigned)dos_idiv((V[b2][0] - V[a][0]) * 65536, cnt));
            e[d][1] = (int)((unsigned)(CF[a] + 0x8000) + tt * (unsigned)dos_idiv(CF[b2] - CF[a], cnt));
        }
        if (e[1][0] <= e[0][0]) { int q0 = e[0][0], q1 = e[0][1]; e[0][0] = e[1][0]; e[0][1] = e[1][1]; e[1][0] = q0; e[1][1] = q1; }   /* 0x5a1d5 */
        x0 = e[0][0] >> 16; w = (e[1][0] >> 16) - x0;
        st = w != 0 ? dos_idiv(e[1][1] - e[0][1], (w >> 1) > 1 ? (w >> 1) : 1) : 0;
        if (w < 0 || w > 8192) continue;
        {   /* pixel pair j (pixels 2j, 2j + 1) shares val = cL + j s: lo = val >> 16 and hi = (val + 0x7fff) >> 16 (lo or lo +
             * 1), hi on the pixels with k + r even = (px + py + B) even: column 1 where x + y + phase is odd, phase = B + 1 */
            const int B = -vpo_x - x0 + vph - 1 + vpo_y - V[top][1], phase = (B + 1) & 1;
            int j = 0, j1, n2 = w / 2 + 1;
            (void)r;
            while (j < n2) {
                const unsigned val = (unsigned)e[0][1] + (unsigned)j * (unsigned)st;
                const int lo = (int)((val >> 16) & 0xffu), hi = (int)(((val + 0x7fffu) >> 16) & 0xffu);
                int kend;
                for (j1 = j + 1; j1 < n2; j1++) {
                    const unsigned v2 = (unsigned)e[0][1] + (unsigned)j1 * (unsigned)st;
                    if ((int)((v2 >> 16) & 0xffu) != lo || (int)(((v2 + 0x7fffu) >> 16) & 0xffu) != hi) break;
                }
                kend = 2 * j1 - 1 < w ? 2 * j1 - 1 : w;   /* pixels 2j .. kend */
                {
                    const float xa = ((float)(vpo_x + x0 + 2 * j) - (float)G.vp[0]) / (float)G.vp[2] * 2.0f - 1.0f;
                    const float xb = ((float)(vpo_x + x0 + kend + 1) - (float)G.vp[0]) / (float)G.vp[2] * 2.0f - 1.0f;
                    sv q[4];
                    q[0].x = xa; q[0].y = y0n; q[1].x = xb; q[1].y = y0n; q[2].x = xb; q[2].y = y1n; q[3].x = xa; q[3].y = y1n;
                    for (i = 0; i < 4; i++) { q[i].z = pa * q[i].x + pb * q[i].y + pc; q[i].s = 0; }
                    pal_poly(q, 4, lo, hi != lo, phase);
                }
                j = j1;
            }
        }
    }
    return 1;
}

/* the DOS Gouraud fill of a triangle (world.frag dos_gouraud): the shade c = v + u (L + 1) / 16 at each corner,
 * screen-linear; near-clipped in clip space, then cut along its iso-lines in screen space */
static void dos_gouraud_tri(const wv *v[3], int lev)
{
    const float g = ((float)lev + 1.0f) / 16.0f;
    float P[3][5], Q[6][5];
    int n = 0, i, k;
    sv s[8];
    gl12_key key = g_k;
    for (i = 0; i < 3; i++) { memcpy(P[i], v[i]->clip, 4 * sizeof(float)); P[i][4] = v[i]->uv[1] + v[i]->uv[0] * g; }
    for (i = 0; i < 3; i++) {   /* z + w >= 0 */
        const float *a = P[i], *b = P[(i + 1) % 3];
        float da = a[2] + a[3], db = b[2] + b[3];
        if (da >= 0) memcpy(Q[n++], a, sizeof Q[0]);
        if ((da >= 0) != (db >= 0)) { float t = da / (da - db); for (k = 0; k < 5; k++) Q[n][k] = a[k] + (b[k] - a[k]) * t; n++; }
    }
    if (n < 3) return;
    for (i = 0; i < n; i++) {
        float w = Q[i][3];
        if (w <= 1e-9f) return;   /* (behind the eye after all) */
        s[i].x = Q[i][0] / w; s[i].y = Q[i][1] / w; s[i].z = Q[i][2] / w; s[i].s = Q[i][4];
    }
    key.tex = 0;
    if (G.F.u_dos_smooth) {   /* e = clamp(c + 0.25, the polygon's shades) */
        float cmax = -1e30f;
        sv a[24], b[24];
        int na, nb;
        for (i = 0; i < 3; i++) { float c = v[i]->uv[1] + v[i]->uv[0] * g; if (c > cmax) cmax = c; }
        for (i = 0; i < n; i++) s[i].s += 0.25f;
        na = clip_s(s, n, a, cmax, 0);
        if (na >= 3) blend_fill(&key, a, na);
        nb = clip_s(s, n, b, cmax, 1);
        if (nb >= 3) { float col[4]; dos_colour(floorf(cmax), col); emit_poly(&key, b, nb, col, 0); }
        return;
    }
    {   /* the checker's phase: pixel k of the DOS span (from its left end x0) on DOS row r of the polygon (from its top
         * vertex) takes n + 1 when k + r is even (world.frag dos_gouraud). In window terms k + r = px + py + B(row), B =
         * -vpo.x - x0(row) + vp_h - 1 + vpo.y - ytop (mod 2): the polygon's DOS vertices (0x3dfb7) and its left chain
         * walked as 0x59e77 does (dos_gchain) give x0 per row - row bands of one phase, each its own stipple */
        int s0 = v[0]->texsel - 1, top = 0, ybot, m = 0, t, B, y, ok = s0 >= 0 && g_sb != NULL;
        const int vpo_x = (int)G.F.u_vpo[0], vpo_y = (int)G.F.u_vpo[1], vph = (int)G.F.u_vp[1];
        int V[20][2], CF[20];
        for (t = 0; t < 16 && ok; t++) {
            int b = s0 + 3 * t, c, c0 = t == 0 ? 0 : 1, last = (dos_ebits(b) & 2) != 0;
            for (c = c0; c < (last ? 3 : 2) && m < 18; c++) {
                float w3[3], q4[4], cl[4];
                dos_vpos(b + c, w3);
                q4[0] = w3[0]; q4[1] = w3[1]; q4[2] = w3[2]; q4[3] = 1.0f;
                mat4v(G.F.u_mvp, q4, cl);
                if (cl[2] + cl[3] < 0.0f) ok = 0;   /* (near-clipped: one phase, from the top vertex) */
                dos_scr(w3, V[m]);
                CF[m] = 0;
                m++;
            }
            if (last) break;
        }
        (void)CF;
        g_nstrip = 0;
        for (i = 0; i < n; i++) s[i].s += 0.5f;   /* the chains start at c + 0x8000 */
        if (m < 3) { dither_fill(&key, s, n, 0, 1, 1 << 20); return; }
        ybot = V[0][1];
        for (i = 0; i < m; i++) { if (V[i][1] <= V[top][1]) top = i; if (V[i][1] > ybot) ybot = V[i][1]; }   /* 0x59f58: the last of the least y */
        if (!ok || ybot - V[top][1] > 1000) {
            B = -vpo_x - V[top][0] + vph - 1 + vpo_y - V[top][1];
            dither_fill(&key, s, n, 0, (B & 1) == 0, 1 << 20);
            return;
        }
        for (y = V[top][1]; y <= ybot; y++) {
            int e[2], d, x0;
            for (d = 0; d < 2; d++) {   /* the backward (dir -1) and forward chains at row y: x in 16.16 */
                const int dir = d ? 1 : -1;
                int a = top, b2 = (a + dir + m) % m, r0 = V[top][1], cnt, yq, g;
                for (g = 0; g < 20 && V[b2][1] == V[a][1] && b2 != top; g++) { a = b2; b2 = (a + dir + m) % m; }
                cnt = V[b2][1] - V[a][1]; yq = y == ybot && y > r0 ? y - 1 : y;
                for (g = 0; g < 20 && yq >= r0 + cnt && b2 != top; g++) { r0 += cnt; a = b2; b2 = (a + dir + m) % m; cnt = V[b2][1] - V[a][1]; if (cnt == 0) cnt = 1; }
                if (cnt == 0) cnt = 1;
                e[d] = (int)((unsigned)(V[a][0] * 65536 + 0x8000) + (unsigned)(y - r0) * (unsigned)dos_idiv((V[b2][0] - V[a][0]) * 65536, cnt));
            }
            x0 = (e[0] < e[1] ? e[0] : e[1]) >> 16;   /* 0x5a1d5: the smaller */
            B = -vpo_x - x0 + vph - 1 + vpo_y - V[top][1];
            {   /* DOS row y = window row vph - 1 - y + vpo.y: its band in NDC (G.vp: the draw's viewport) */
                const int py = vph - 1 - y + vpo_y;
                const float lo = ((float)py - (float)G.vp[1]) / (float)G.vp[3] * 2.0f - 1.0f, hi = ((float)(py + 1) - (float)G.vp[1]) / (float)G.vp[3] * 2.0f - 1.0f;
                const int ev = (B & 1) == 0;
                if (g_nstrip > 0 && g_strip[g_nstrip - 1].even == ev && g_strip[g_nstrip - 1].y0 == hi) g_strip[g_nstrip - 1].y0 = lo;   /* (rows go down) */
                else if (g_nstrip < (int)(sizeof g_strip / sizeof *g_strip)) { g_strip[g_nstrip].y0 = lo; g_strip[g_nstrip].y1 = hi; g_strip[g_nstrip].even = ev; g_nstrip++; }
            }
        }
        {   /* bottom up, as dither_fill clips them */
            int q;
            for (q = 0; q < g_nstrip / 2; q++) { dstrip tq = g_strip[q]; g_strip[q] = g_strip[g_nstrip - 1 - q]; g_strip[g_nstrip - 1 - q] = tq; }
        }
        dither_fill(&key, s, n, 0, 1, 1 << 20);
        g_nstrip = 0;
    }
}

/* a DOS flat-coloured triangle (kinds 0 and 1): palette index idx, through the palette-pair texture (one batch with the
 * Gouraud runs), else as a vertex colour */
static void dos_flat(const wv *v[3], int idx)
{
    gl12_vtx o[3];
    float cc[4];
    int i;
    gl12_key k = g_k;
    if (g_pal_ok) {
        const float tt = ((float)(idx < 0 ? 0 : idx > 255 ? 255 : idx) + 0.5f) / 256.0f;
        static const float wh[4] = {1, 1, 1, 1};
        for (i = 0; i < 3; i++) { out_vtx(&o[i], v[i], wh); o[i].t0[0] = 0.25f; o[i].t0[1] = tt; }
        gl12_batch(&g_pkey, o, 3);
        return;
    }
    dos_colour((float)idx, cc);
    k.tex = 0;
    for (i = 0; i < 3; i++) out_vtx(&o[i], v[i], cc);
    gl12_batch(&k, o, 3);
}

static void world_tri(const wv *v0, const wv *v1, const wv *v2)
{
    const rb_world_frame *F = &G.F;
    const rb_world_draw *D = &G.D;
    const wv *v[3] = {v0, v1, v2}, *fl = v0;   /* fl: the provoking vertex (the flat outputs) */
    gl12_vtx o[3];
    float col[3][4];
    int i, c;
    gl12_key k = g_k;
    if (D->u_wire[3] > 0.0f) {   /* the wireframe: the inside black, the polygon's own edges in the layer's colour */
        static const float black[4] = {0, 0, 0, 1};
        int wc = (fl->emask >> 3) & 7;
        float ec[4] = {D->u_wire[0], D->u_wire[1], D->u_wire[2], 1.0f};
        if (wc == 1) { ec[0] = 0.0f; ec[1] = 48.0f / 255.0f; ec[2] = 212.0f / 255.0f; }
        else if (wc == 2) { ec[0] = 252.0f / 255.0f; ec[1] = 24.0f / 255.0f; ec[2] = 24.0f / 255.0f; }
        else if (wc == 3) { ec[0] = 252.0f / 255.0f; ec[1] = 204.0f / 255.0f; ec[2] = 0.0f; }
        else if (wc == 4) { ec[0] = 60.0f / 255.0f; ec[1] = 0.0f; ec[2] = 0.0f; }
        k.tex = 0; k.offset_fill = 1;
        for (i = 0; i < 3; i++) out_vtx(&o[i], v[i], black);
        gl12_batch(&k, o, 3);
        if (v0->clip[3] > 1e-6f && v1->clip[3] > 1e-6f && v2->clip[3] > 1e-6f && v0->clip[2] >= -v0->clip[3] && v1->clip[2] >= -v1->clip[3] && v2->clip[2] >= -v2->clip[3]) {
            /* the shader's edge pixels: inside the triangle, bary / fwidth(bary) <= 1 - within |n.x| + |n.y| pixels of the
             * edge (n its unit normal on screen): that band of the triangle, cut in screen space (its depth screen-linear) */
            const float hx = 0.5f * (float)G.vp[2], hy = 0.5f * (float)G.vp[3];
            sv T[3], A[8];
            int e;
            for (i = 0; i < 3; i++) { T[i].x = v[i]->clip[0] / v[i]->clip[3] * hx; T[i].y = v[i]->clip[1] / v[i]->clip[3] * hy; T[i].z = v[i]->clip[2] / v[i]->clip[3]; }
            for (e = 0; e < 3; e++) {
                const int a = e == 0 ? 1 : 0, b = e == 2 ? 1 : 2;   /* bit e: the edge opposite corner e */
                float nx = T[b].y - T[a].y, ny = T[a].x - T[b].x, nl = sqrtf(nx * nx + ny * ny), side, lim;
                int m, q;
                if (!(fl->emask & (1 << e)) || nl < 1e-6f) continue;
                nx /= nl; ny /= nl;
                side = (T[e].x - T[a].x) * nx + (T[e].y - T[a].y) * ny;
                if (side < 0) { nx = -nx; ny = -ny; side = -side; }
                lim = fabsf(nx) + fabsf(ny);
                for (q = 0; q < 3; q++) T[q].s = (T[q].x - T[a].x) * nx + (T[q].y - T[a].y) * ny;
                m = clip_s(T, 3, A, lim, 0);
                if (m < 3) continue;
                for (q = 0; q < m; q++) { A[q].x /= hx; A[q].y /= hy; }
                emit_poly(&g_ekey, A, m, ec, 0);
            }
            return;
        }
        if (fl->emask & 1) put_line(&o[1], &o[2], ec);
        if (fl->emask & 2) put_line(&o[0], &o[2], ec);
        if (fl->emask & 4) put_line(&o[0], &o[1], ec);
        return;
    }
    if (D->u_shadow) {
        static const float sh[4] = {0, 0, 0, 0.45f};
        k.tex = 0;
        for (i = 0; i < 3; i++) out_vtx(&o[i], v[i], sh);
        gl12_batch(&k, o, 3);
        return;
    }
    if (F->u_dos != 0 && G.prog == RB_PROG_WORLD_DOS) {   /* the DOS edition (world.frag) */
        int kind = (int)(fl->color[0] + 0.5f);
        float base = floorf(fl->color[1] + 0.5f), Cc = floorf(fl->color[2] + 0.5f);
        float n[3], ctr[3], wc[3], nl, Ld[3], nf[3], f, lev;
        if (kind == 0) { dos_flat(v, (int)base); return; }
        if (kind == 4) {   /* a billboard's texel indices as they are */
            static const float wh[4] = {1, 1, 1, 1};
            if (!g_tex_ok) return;
            k.var = GV_DOS; k.param = 15; k.alpha_func = GL_GEQUAL; k.alpha_ref = 0.5f;
            for (i = 0; i < 3; i++) { out_vtx(&o[i], v[i], wh); o[i].t0[0] = v[i]->uv[0] / D->u_texsize[0]; o[i].t0[1] = v[i]->uv[1] / D->u_texsize[1]; }
            gl12_batch(&k, o, 3);
            return;
        }
        for (c = 0; c < 3; c++) {
            n[c] = (v0->normal[c] + v1->normal[c] + v2->normal[c]) / 3.0f;
            ctr[c] = (v0->viewpos[c] + v1->viewpos[c] + v2->viewpos[c]) / 3.0f;
            wc[c] = (v0->world[c] + v1->world[c] + v2->world[c]) / 3.0f;
        }
        nl = len3(n);
        if (F->u_dos_dir) memcpy(Ld, F->u_dos_lpos, sizeof Ld);
        else for (c = 0; c < 3; c++) Ld[c] = F->u_dos_lpos[c] - wc[c];
        { float l = len3(Ld); if (l > 0) for (c = 0; c < 3; c++) Ld[c] /= l; }
        for (c = 0; c < 3; c++) nf[c] = nl > 0 ? -n[c] / nl : 0.0f;
        if (nl > 0) {   /* one-sided: (viewpos) . (view's 3 x 3 x nf) >= 0 not drawn */
            const float *m = F->u_view;
            float vn[3];
            for (c = 0; c < 3; c++) vn[c] = m[c] * nf[0] + m[4 + c] * nf[1] + m[8 + c] * nf[2];
            if (dot3(ctr, vn) >= 0.0f) return;
        }
        f = nl > 0 ? truncf(127.0f * dot3(nf, Ld)) : 127.0f;
        lev = floorf(floorf(Cc / 2.0f) * (floorf(f * (128.0f - F->u_dos_amb) / 128.0f) + F->u_dos_amb) / 1088.0f);
        if (F->u_dos_fogdist > 0.0f)
            lev -= fl->dkey >= 0.0f ? (float)(((int)fl->dkey * 16 / (int)F->u_dos_fogdist) >> 4) : floorf((-ctr[2] > 0 ? -ctr[2] : 0.0f) / F->u_dos_fogdist);
        lev = lev < 0 ? 0 : lev > 15 ? 15 : lev;
        if (kind == 1) { dos_flat(v, (int)(base + lev)); return; }
        if (kind == 3) { if (G.F.u_dos_smooth || !dos_gouraud_spans(v0, (int)lev)) dos_gouraud_tri(v, (int)lev); return; }
        {
            static const float wh[4] = {1, 1, 1, 1};
            if (!g_tex_ok) return;
            k.var = GV_DOS; k.param = (int)lev; k.alpha_func = GL_GEQUAL; k.alpha_ref = 0.5f;
            for (i = 0; i < 3; i++) { out_vtx(&o[i], v[i], wh); o[i].t0[0] = v[i]->uv[0] / D->u_texsize[0]; o[i].t0[1] = v[i]->uv[1] / D->u_texsize[1]; }
            gl12_batch(&k, o, 3);
        }
        return;
    }
    /* the 3D looks */
    if (fl->ticm == 3 && ((-fl->texsel) & F->u_notex) == 0) return;
    {
        int textured = D->u_textured != 0 && D->u_mono[3] <= 0.0f && (fl->texsel >= 0 || ((-fl->texsel) & F->u_notex) == 0) && g_tex_ok;
        int af = F->u_ati != 0 && fl->texsel < 0 ? -fl->texsel : 0, affine = 0, tinted = 0;
        float ds = fl->dnib > 0 ? (F->u_dsign ? 1.0f + (float)fl->dnib / 15.0f : 1.0f - (float)fl->dnib / 15.0f) : 1.0f;
        float pf[3] = {0, 0, 0}, fz[3];
        int pfade = 0;
        if (!textured && F->u_pfade > 0.0f && fl->texsel > 0 && fl->bb == 0) {   /* toward the fade's palette */
            int w = 0, h = 0, ix = fl->texsel - 1;
            const uint8_t *pt = gl12_tex_texels(G.tex[2], &w, &h);
            if (pt && ix < w) { for (c = 0; c < 3; c++) pf[c] = pt[(size_t)ix * 4 + (size_t)c] / 255.0f; pfade = 1; }
        }
        if (textured) {
            affine = ((F->u_ati & 1) && (af & 4)) || ((F->u_ati & 2) && (af & 16));
            k.var = D->u_blend == 2 ? GV_ASQ : D->u_blend == 3 ? GV_OPAQUE : GV_PLAIN;
            if (D->u_blend == 3) k.alpha_func = 0;
            else if (D->u_blend != 0) { k.alpha_func = D->u_blend == 2 ? GL_GREATER : GL_GREATER; k.alpha_ref = D->u_blend == 2 ? 0.0f : 1.5f / 255.0f * D->u_alpha; }
            else if ((af & 8) && (F->u_ati & 4)) k.alpha_func = 0;
            else { k.alpha_func = GL_GEQUAL; k.alpha_ref = 0.5f * D->u_alpha; }
            tinted = D->u_tinton != 0;
        } else k.tex = 0;
        for (i = 0; i < 3; i++) {
            const wv *s = v[i];
            float base[3], lit[3], n = len3(s->normal), L[3], diff;
            if (D->u_mono[3] > 1.5f) memcpy(base, s->color, sizeof base);
            else if (D->u_mono[3] > 0.0f) memcpy(base, D->u_mono, sizeof base);
            else if (fl->ticm >= 2) memcpy(base, s->tic, sizeof base);
            else memcpy(base, s->color, sizeof base);
            if (textured) memcpy(base, D->u_tint, sizeof base);   /* (x the texel: GL_MODULATE) */
            else if (pfade) for (c = 0; c < 3; c++) base[c] += (pf[c] - base[c]) * F->u_pfade;
            if (F->u_point) { for (c = 0; c < 3; c++) L[c] = F->u_lpos[c] - s->world[c]; { float l = len3(L); if (l > 0) for (c = 0; c < 3; c++) L[c] /= l; } }
            else memcpy(L, F->u_light, sizeof L);
            diff = n > 0 ? fabsf(dot3(s->normal, L) / n) : 0.5f;
            if (D->u_unlit || fl->bb || tinted || (fl->tif & 2) || fl->ticm == 3) memcpy(lit, base, sizeof lit);
            else if (D->u_tilit == 1) {
                float tl[3];
                if (fl->ticm == 1) memcpy(tl, s->tic, sizeof tl);
                else {
                    float nn[3];
                    for (c = 0; c < 3; c++) nn[c] = (fl->tif & 1) ? s->normal[c] : n > 0 ? s->normal[c] / n : 0.0f;
                    ti_light(s, fl, nn, tl);
                }
                for (c = 0; c < 3; c++) lit[c] = base[c] * tl[c];
            } else if (D->u_tilit == 2) for (c = 0; c < 3; c++) lit[c] = base[c] * F->u_tignd[c];
            else { float m = (F->u_ambient + diff) * ds; if (m > 1.0f) m = 1.0f; for (c = 0; c < 3; c++) lit[c] = base[c] * m; }
            for (c = 0; c < 3; c++) col[i][c] = lit[c];
            col[i][3] = D->u_alpha;
            out_vtx(&o[i], s, col[i]);
            fz[i] = -s->viewpos[2];   /* (the view depth, linear in clip space: GL's clipping keeps it so - not clamped; after
                                       * clipping every pixel lies beyond the near plane, so max(z, 1) has no effect) */
            if (textured) {
                float su = s->uv[0] / D->u_texsize[0], tv = s->uv[1] / D->u_texsize[1], w = s->clip[3];
                if (affine) { o[i].t0[0] = su * w; o[i].t0[1] = tv * w; o[i].t0[3] = w; }
                else { o[i].t0[0] = su; o[i].t0[1] = tv; }
            }
        }
        /* the fog */
        if (F->u_tifog[0] > 0.5f) {   /* Titanium: per vertex, screen-linear (RENDITION), or per pixel from the view depth */
            k.fog = GF_RAMP;
            gl12_fog_table(F->u_fog_color, GF_RAMP);
            if (F->u_tifog[2] <= 0.5f) { tifog_split(&k, o, fz); return; }
            for (i = 0; i < 3; i++) { float w = v[i]->clip[3]; o[i].t1[0] = ramp_s(v[i]->tivf) * w; o[i].t1[3] = w; }
        } else if (F->u_fog_density > 0.0f) {   /* PowerVR: the table at density / z per pixel */
            const float K = F->u_fog_density / 2048.0f;
            k.fog = GF_TABLE;
            gl12_fog_table(F->u_fog_color, GF_TABLE);
            for (i = 0; i < 3; i++) { o[i].t1[0] = K; o[i].t1[3] = fz[i]; }
        }
        if (textured && D->u_mga_lod && g12_lodok) { band_split(&k, o, fz); return; }
        gl12_batch(&k, o, 3);
    }
}

void gl12_world_draw(int first, int count)
{
    static wv *vs;
    static int cap;
    size_t bytes;
    const uint8_t *b;
    int i;
    gl12_tex *t0;
    if (count > cap) {
        wv *q = realloc(vs, (size_t)count * sizeof *q);
        if (!q) return;
        vs = q; cap = count;
    }
    /* the bound buffers, vertex `first` of each at its offset */
    b = gl12_buf_ptr(G.vb[0], &bytes);
    g_v0 = b && G.voff[0] + (size_t)first * FPV * 4 <= bytes ? (const float *)(b + G.voff[0]) + (size_t)first * FPV : NULL;
    g_n0 = g_v0 ? (bytes - G.voff[0]) / (FPV * 4) - (size_t)first : 0;
    b = gl12_buf_ptr(G.vb[1], &bytes);
    g_v1 = b && G.voff[1] + (size_t)first * 16 <= bytes ? (const float *)(b + G.voff[1]) + (size_t)first * 4 : NULL;
    g_n1 = g_v1 ? (bytes - G.voff[1]) / 16 - (size_t)first : 0;
    b = gl12_buf_ptr(G.vb[2], &bytes);
    g_v2 = b && G.voff[2] + (size_t)first * 16 <= bytes ? (const float *)(b + G.voff[2]) + (size_t)first * 4 : NULL;
    g_n2 = g_v2 ? (bytes - G.voff[2]) / 16 - (size_t)first : 0;
    b = G.sb && G.sb != RB_STREAM ? gl12_buf_ptr(G.sb, &bytes) : NULL;
    g_sb = b ? (const float *)b : NULL;
    g_nsb = b ? bytes / 4 : 0;
    if (G.D.u_horizon) { horizon(); return; }
    key_init(&g_k);
    t0 = gl12_T(G.tex[0]);
    g_tex_ok = t0 && t0->rgba != NULL;
    g_k.tex = G.tex[0];
    if (!g_tex_ok) {   /* (nothing bound: SDL_GPU's backend samples a white texel) */
        static rb_tex white;
        static const uint32_t W = 0xffffffffu;
        if (!white || !gl12_T(white)) { white = rb_tex_new(); rb_tex_image(white, 1, 1, &W, 0); rb_tex_filter(white, RB_NEAREST, RB_NEAREST); }
        g_k.tex = white; g_tex_ok = 1;
    }
    {   /* the DOS palette-pair texture (the DOS table on unit 1) */
        int tw = 0, th = 0;
        key_init(&g_pkey);
        g_pkey.tex = G.tex[1]; g_pkey.var = GV_DOSPAIR;
        g_pal_ok = G.prog == RB_PROG_WORLD_DOS && G.F.u_dos != 0 && gl12_tex_texels(G.tex[1], &tw, &th) && tw == 256 && th >= 17;
    }
    for (i = 0; i < count; i++) vertex_stage(G.D.u_first + i, i, &vs[i]);
    g_vs = vs; g_vfirst = G.D.u_first; g_vcount = count; g_lastpoly = -1;
    g_nlines = g_nedges = 0;
    key_init(&g_ekey);
    g_ekey.depth_func = GL_LEQUAL;
    for (i = 0; i + 2 < count; i += 3) world_tri(&vs[i], &vs[i + 1], &vs[i + 2]);
    if (g_nedges) gl12_batch(&g_ekey, g_edges, g_nedges);   /* the wireframe's edges, over the fills (pushed back) */
    if (g_nlines) {   /* the wireframe's edges, over the fills (which are pushed back) */
        key_init(&g_lkey);
        g_lkey.lines = 1;
        g_lkey.depth_func = GL_LEQUAL;
        gl12_batch(&g_lkey, g_lines, g_nlines);
    }
}

/* ---- the HUD program (hud.frag): the texel (or white) x the tint, the alpha test at 0.01, the flash blended in */
void gl12_hud_draw(int first, int count)
{
    const rb_hud_draw *H = &G.H;
    size_t bytes;
    const uint8_t *b = gl12_buf_ptr(G.vb[0], &bytes);
    const float *v;
    gl12_vtx *o;
    gl12_key k;
    gl12_tex *t = gl12_T(G.tex[0]);
    int i, n = count - count % 3, textured, flash = H->u_flash[3] > 0.0f;
    float fa = H->u_flash[3] > 1.0f ? 1.0f : H->u_flash[3];
    if (!b || G.voff[0] + (size_t)(first + n) * 16 > bytes || n <= 0) return;
    v = (const float *)(b + G.voff[0]) + (size_t)first * 4;
    o = malloc((size_t)n * sizeof *o);
    if (!o) return;
    key_init(&k);
    textured = H->u_textured != 0 && t && t->rgba;
    if (textured) {
        int sharp = H->u_sharp > 0.0f ? (int)(H->u_sharp + 0.5f) : 0;
        k.tex = G.tex[0];
        k.var = sharp > 0 ? (H->u_smooth ? GV_SHARP_SQRTA : GV_SHARP) : H->u_smooth ? GV_SQRTA : GV_PLAIN;
        k.param = sharp;
    }
    k.alpha_func = GL_GEQUAL; k.alpha_ref = 0.01f;
    if (flash && g12_units > 1) { k.env_blend = 1; memcpy(k.flash, H->u_flash, sizeof k.flash); }
    for (i = 0; i < n; i++) {
        memset(&o[i], 0, sizeof o[i]);
        o[i].p[0] = v[i * 4]; o[i].p[1] = v[i * 4 + 1]; o[i].p[2] = 0.5f; o[i].p[3] = 1.0f;
        memcpy(o[i].c, H->u_tint, sizeof o[i].c);
        if (flash && g12_units == 1) { o[i].c[0] *= 1.0f - fa; o[i].c[1] *= 1.0f - fa; o[i].c[2] *= 1.0f - fa; }
        o[i].t0[0] = v[i * 4 + 2]; o[i].t0[1] = v[i * 4 + 3]; o[i].t0[3] = 1.0f;
        o[i].t1[0] = (floorf(fa * 255.0f + 0.5f) + 0.5f) / 256.0f; o[i].t1[3] = 1.0f;
    }
    if (textured) gl12_tiled_tris(&k, G.tex[0], o, n);
    else gl12_batch(&k, o, n);
    if (flash && g12_units == 1 && G.blend == RB_BLEND_ALPHA) {   /* one unit: + flash x a, added (the texel's alpha, white) */
        for (i = 0; i < n; i++) { o[i].c[0] = H->u_flash[0] * fa; o[i].c[1] = H->u_flash[1] * fa; o[i].c[2] = H->u_flash[2] * fa; o[i].c[3] = H->u_tint[3]; }
        k.blend = 4;
        if (textured) { k.var = k.var == GV_SQRTA || k.var == GV_SHARP_SQRTA ? GV_WHITE : GV_WHITE; k.param = 0; gl12_tiled_tris(&k, G.tex[0], o, n); }
        else gl12_batch(&k, o, n);
    }
    free(o);
}
