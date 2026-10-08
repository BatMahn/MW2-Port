/* render.c - see render.h. */
#include "render.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int r_init(r_target *t, int w, int h)
{
    int i;
    memset(t, 0, sizeof *t);
    t->w = w;
    t->h = h;
    t->pixels = malloc((size_t)w * (size_t)h);
    t->depth = malloc((size_t)w * (size_t)h * sizeof *t->depth);
    if (!t->pixels || !t->depth) { r_free(t); return -1; }
    for (i = 0; i < 16 * 256; i++) t->shade[i] = (uint8_t)(i & 255); /* identity until set */
    return 0;
}

void r_free(r_target *t)
{
    free(t->pixels);
    free(t->depth);
    t->pixels = NULL;
    t->depth = NULL;
}

void r_clear(r_target *t, uint8_t colour)
{
    int i, n = t->w * t->h;
    memset(t->pixels, colour, (size_t)n);
    for (i = 0; i < n; i++) t->depth[i] = FLT_MAX;
}

int r_set_palette(r_target *t, const uint8_t *vga6, size_t len)
{
    int i, k;
    if (len < 768) return -1;
    for (i = 0; i < 256; i++)
        for (k = 0; k < 3; k++) {
            unsigned v = vga6[i * 3 + k] & 63u;
            t->palette[i][k] = (uint8_t)((v << 2) | (v >> 4)); /* 6-bit -> 8-bit */
        }
    return 0;
}

int r_set_shade_table(r_target *t, const uint8_t *table, size_t len)
{
    if (len < 16 * 256) return -1;
    memcpy(t->shade, table, 16 * 256);
    return 0;
}

typedef struct { float x, y, z; } vec3;

static vec3 rotate(vec3 v, float cy, float sy, float cp, float sp)
{
    vec3 a, b;
    a.x = cy * v.x + sy * v.z;           /* yaw about y */
    a.y = v.y;
    a.z = -sy * v.x + cy * v.z;
    b.x = a.x;                            /* pitch about x */
    b.y = cp * a.y - sp * a.z;
    b.z = sp * a.y + cp * a.z;
    return b;
}

static float edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static void fill_triangle(r_target *t, const vec3 *a, const vec3 *b, const vec3 *c, uint8_t colour)
{
    float area = edge(a->x, a->y, b->x, b->y, c->x, c->y);
    int x0, x1, y0, y1, x, y;
    if (fabsf(area) < 1e-6f) return;
    x0 = (int)floorf(fminf(a->x, fminf(b->x, c->x)));
    x1 = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    y0 = (int)floorf(fminf(a->y, fminf(b->y, c->y)));
    y1 = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > t->w - 1) x1 = t->w - 1;
    if (y1 > t->h - 1) y1 = t->h - 1;
    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = edge(b->x, b->y, c->x, c->y, px, py) / area;
            float w1 = edge(c->x, c->y, a->x, a->y, px, py) / area;
            float w2 = 1.0f - w0 - w1;
            float z;
            int i;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;  /* works for either winding */
            z = w0 * a->z + w1 * b->z + w2 * c->z;
            i = y * t->w + x;
            if (z < t->depth[i]) {
                t->depth[i] = z;
                t->pixels[i] = colour;
            }
        }
    }
}

void r_draw_mech(r_target *t, const mech3d *m, float yaw, float pitch, const float light[3])
{
    float cy = cosf(yaw * 3.14159265f / 180.0f), sy = sinf(yaw * 3.14159265f / 180.0f);
    float cp = cosf(pitch * 3.14159265f / 180.0f), sp = sinf(pitch * 3.14159265f / 180.0f);
    float minx = FLT_MAX, miny = FLT_MAX, maxx = -FLT_MAX, maxy = -FLT_MAX, scale, ox, oy;
    float ll = sqrtf(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
    vec3 L;
    int pi, oi, k, j;

    if (ll <= 0) ll = 1;
    L.x = light[0] / ll; L.y = light[1] / ll; L.z = light[2] / ll;

    /* pass 1: bounds of the rotated mech (detail object 0 of each part) */
    for (pi = 0; pi < m->part_count; pi++) {
        const wtb_object *o = &m->parts[pi].model.objects[0];
        for (k = 0; k < o->vert_count; k++) {
            vec3 v = { (float)(o->verts[k].x + m->parts[pi].pos[0]), (float)(o->verts[k].y + m->parts[pi].pos[1]),
                       (float)(o->verts[k].z + m->parts[pi].pos[2]) };
            v = rotate(v, cy, sy, cp, sp);
            minx = fminf(minx, v.x); maxx = fmaxf(maxx, v.x);
            miny = fminf(miny, v.y); maxy = fmaxf(maxy, v.y);
        }
    }
    if (minx > maxx) return;
    scale = 0.9f * fminf((float)t->w / fmaxf(maxx - minx, 1.0f), (float)t->h / fmaxf(maxy - miny, 1.0f));
    ox = ((float)t->w - (maxx - minx) * scale) / 2.0f;
    oy = ((float)t->h - (maxy - miny) * scale) / 2.0f;

    /* pass 2: draw */
    for (pi = 0; pi < m->part_count; pi++) {
        const mech3d_part *part = &m->parts[pi];
        for (oi = 0; oi < 1 && oi < part->model.object_count; oi++) {
            const wtb_object *o = &part->model.objects[oi];
            for (k = 0; k < o->poly_count; k++) {
                const wtb_poly *q = &o->polys[k];
                vec3 w[WTB_MAX_POLY_VERTS], s[WTB_MAX_POLY_VERTS], e1, e2, nrm;
                float nl, lam;
                int level;
                if (q->n < 3) continue;
                for (j = 0; j < q->n; j++) {
                    const wtb_vertex *v = &o->verts[q->idx[j]];
                    vec3 p = { (float)(v->x + part->pos[0]), (float)(v->y + part->pos[1]), (float)(v->z + part->pos[2]) };
                    w[j] = p;
                    p = rotate(p, cy, sy, cp, sp);
                    s[j].x = ox + (p.x - minx) * scale;
                    s[j].y = (float)t->h - (oy + (p.y - miny) * scale); /* y up -> screen down */
                    s[j].z = -p.z;                                       /* nearer = smaller */
                }
                e1.x = w[1].x - w[0].x; e1.y = w[1].y - w[0].y; e1.z = w[1].z - w[0].z;
                e2.x = w[2].x - w[0].x; e2.y = w[2].y - w[0].y; e2.z = w[2].z - w[0].z;
                nrm.x = e1.y * e2.z - e1.z * e2.y;
                nrm.y = e1.z * e2.x - e1.x * e2.z;
                nrm.z = e1.x * e2.y - e1.y * e2.x;
                nl = sqrtf(nrm.x * nrm.x + nrm.y * nrm.y + nrm.z * nrm.z);
                lam = nl > 0 ? fabsf((nrm.x * L.x + nrm.y * L.y + nrm.z * L.z) / nl) : 0;
                level = 15 - (int)(lam * 15.0f + 0.5f);
                for (j = 1; j + 1 < q->n; j++)
                    fill_triangle(t, &s[0], &s[j], &s[j + 1], t->shade[level * 256 + (q->color >> 8)]);
            }
        }
    }
}

int r_write_ppm(const r_target *t, const char *path)
{
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", t->w, t->h);
    for (i = 0; i < t->w * t->h; i++) fwrite(t->palette[t->pixels[i]], 1, 3, f);
    return fclose(f) == 0 ? 0 : -1;
}

/* ---- true-colour textured path ---------------------------------------------- */

int rt_init(rt_target *t, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    int i;
    t->w = w;
    t->h = h;
    t->rgb = malloc((size_t)w * (size_t)h * 3);
    t->depth = malloc((size_t)w * (size_t)h * sizeof *t->depth);
    if (!t->rgb || !t->depth) { rt_free(t); return -1; }
    for (i = 0; i < w * h; i++) {
        t->rgb[i * 3] = r; t->rgb[i * 3 + 1] = g; t->rgb[i * 3 + 2] = b;
        t->depth[i] = FLT_MAX;
    }
    return 0;
}

void rt_free(rt_target *t)
{
    free(t->rgb);
    free(t->depth);
    t->rgb = NULL;
    t->depth = NULL;
}

typedef struct { float x, y, z, u, v; } tvert;

static void rt_triangle(rt_target *t, const tvert *a, const tvert *b, const tvert *c, const texture *tx,
                        const uint8_t flat[3], float shade)
{
    float area = edge(a->x, a->y, b->x, b->y, c->x, c->y);
    int x0, x1, y0, y1, x, y;
    if (fabsf(area) < 1e-6f) return;
    x0 = (int)floorf(fminf(a->x, fminf(b->x, c->x)));
    x1 = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    y0 = (int)floorf(fminf(a->y, fminf(b->y, c->y)));
    y1 = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > t->w - 1) x1 = t->w - 1;
    if (y1 > t->h - 1) y1 = t->h - 1;
    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = edge(b->x, b->y, c->x, c->y, px, py) / area;
            float w1 = edge(c->x, c->y, a->x, a->y, px, py) / area;
            float w2 = 1.0f - w0 - w1, z;
            int i, k;
            uint8_t col[3];
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            z = w0 * a->z + w1 * b->z + w2 * c->z;
            i = y * t->w + x;
            if (z >= t->depth[i]) continue;
            if (tx) {
                int u = (int)floorf(w0 * a->u + w1 * b->u + w2 * c->u);
                int v = (int)floorf(w0 * a->v + w1 * b->v + w2 * c->v);
                uint32_t p;
                u %= tx->w; if (u < 0) u += tx->w;
                v %= tx->h; if (v < 0) v += tx->h;
                p = tx->rgba[v * tx->w + u];
                if ((p >> 24) < 128) continue; /* alpha test, as the cut-out textures need */
                col[0] = (uint8_t)(p & 255); col[1] = (uint8_t)((p >> 8) & 255); col[2] = (uint8_t)((p >> 16) & 255);
            } else {
                memcpy(col, flat, 3);
            }
            t->depth[i] = z;
            for (k = 0; k < 3; k++) {
                float f = (float)col[k] * shade;
                t->rgb[i * 3 + k] = (uint8_t)(f > 255.0f ? 255.0f : f);
            }
        }
    }
}

void rt_draw_mech(rt_target *t, const mech3d *m, float yaw, float pitch, const float light[3],
                  const texture *camo, const texture *decal, const uint8_t palette[256][3])
{
    float cy = cosf(yaw * 3.14159265f / 180.0f), sy = sinf(yaw * 3.14159265f / 180.0f);
    float cp = cosf(pitch * 3.14159265f / 180.0f), sp = sinf(pitch * 3.14159265f / 180.0f);
    float minx = FLT_MAX, miny = FLT_MAX, maxx = -FLT_MAX, maxy = -FLT_MAX, scale, ox, oy;
    float ll = sqrtf(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
    vec3 L;
    int pi, k, j;

    if (ll <= 0) ll = 1;
    L.x = light[0] / ll; L.y = light[1] / ll; L.z = light[2] / ll;
    for (pi = 0; pi < m->part_count; pi++) {
        const wtb_object *o = &m->parts[pi].model.objects[0];
        for (k = 0; k < o->vert_count; k++) {
            vec3 v = { (float)(o->verts[k].x + m->parts[pi].pos[0]), (float)(o->verts[k].y + m->parts[pi].pos[1]),
                       (float)(o->verts[k].z + m->parts[pi].pos[2]) };
            v = rotate(v, cy, sy, cp, sp);
            minx = fminf(minx, v.x); maxx = fmaxf(maxx, v.x);
            miny = fminf(miny, v.y); maxy = fmaxf(maxy, v.y);
        }
    }
    if (minx > maxx) return;
    scale = 0.9f * fminf((float)t->w / fmaxf(maxx - minx, 1.0f), (float)t->h / fmaxf(maxy - miny, 1.0f));
    ox = ((float)t->w - (maxx - minx) * scale) / 2.0f;
    oy = ((float)t->h - (maxy - miny) * scale) / 2.0f;

    for (pi = 0; pi < m->part_count; pi++) {
        const mech3d_part *part = &m->parts[pi];
        const wtb_object *o = &part->model.objects[0];
        int is_decal = strstr(part->model_name, "DECL") != NULL;
        for (k = 0; k < o->poly_count; k++) {
            const wtb_poly *q = &o->polys[k];
            tvert s[WTB_MAX_POLY_VERTS];
            vec3 w[WTB_MAX_POLY_VERTS], e1, e2, nrm;
            const texture *tx = NULL;
            float nl, lam;
            if (q->n < 3) continue;
            if (is_decal) tx = decal;
            else if (part->model.extended && (q->material >> 8) == 0x0B) tx = camo;
            for (j = 0; j < q->n; j++) {
                const wtb_vertex *v = &o->verts[q->idx[j]];
                vec3 p = { (float)(v->x + part->pos[0]), (float)(v->y + part->pos[1]), (float)(v->z + part->pos[2]) };
                w[j] = p;
                p = rotate(p, cy, sy, cp, sp);
                s[j].x = ox + (p.x - minx) * scale;
                s[j].y = (float)t->h - (oy + (p.y - miny) * scale);
                s[j].z = -p.z;
                s[j].u = (float)v->a;
                s[j].v = (float)v->b;
            }
            e1.x = w[1].x - w[0].x; e1.y = w[1].y - w[0].y; e1.z = w[1].z - w[0].z;
            e2.x = w[2].x - w[0].x; e2.y = w[2].y - w[0].y; e2.z = w[2].z - w[0].z;
            nrm.x = e1.y * e2.z - e1.z * e2.y;
            nrm.y = e1.z * e2.x - e1.x * e2.z;
            nrm.z = e1.x * e2.y - e1.y * e2.x;
            nl = sqrtf(nrm.x * nrm.x + nrm.y * nrm.y + nrm.z * nrm.z);
            lam = nl > 0 ? fabsf((nrm.x * L.x + nrm.y * L.y + nrm.z * L.z) / nl) : 0.5f;
            for (j = 1; j + 1 < q->n; j++)
                rt_triangle(t, &s[0], &s[j], &s[j + 1], tx, palette[q->color >> 8], 0.35f + 0.65f * lam);
        }
    }
}

int rt_write_ppm(const rt_target *t, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", t->w, t->h);
    fwrite(t->rgb, 1, (size_t)t->w * (size_t)t->h * 3, f);
    return fclose(f) == 0 ? 0 : -1;
}
