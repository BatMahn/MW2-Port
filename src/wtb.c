/* wtb.c - see wtb.h. */
#include "wtb.h"

#include <stdlib.h>
#include <string.h>

static float rdf64(const uint8_t *p)
{
    uint64_t v = 0;
    double d;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    memcpy(&d, &v, sizeof d);
    return (float)d;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Parse one object at d[*off]; advances *off. */
static int parse_object(const uint8_t *d, size_t len, size_t *off, wtb_object *o, int ext)
{
    size_t vsize = ext ? 40 : 16;
    size_t p = *off;
    int i, k;

    memset(o, 0, sizeof *o);
    if (len - p < 0x20 || memcmp(d + p, "WTBO", 4) != 0) return -1;
    for (i = 0; i < 16; i++) {
        uint8_t c = ext ? d[p + 8 + (size_t)i] : (uint8_t)(0u - d[p + 8 + (size_t)i]);
        if (!c) break;
        o->name[i] = (char)c;
    }
    o->vert_count = rd16(d + p + 0x18);
    o->poly_count = rd16(d + p + 0x1A);
    o->flags = rd16(d + p + 0x1C);
    p += 0x20;

    if ((size_t)o->vert_count * vsize > len - p) return -1;
    o->verts = calloc((size_t)(o->vert_count ? o->vert_count : 1), sizeof *o->verts);
    o->polys = calloc((size_t)(o->poly_count ? o->poly_count : 1), sizeof *o->polys);
    if (!o->verts || !o->polys) return -1;
    for (i = 0; i < o->vert_count; i++, p += vsize) {
        o->verts[i].x = (int32_t)rd32(d + p);
        o->verts[i].y = (int32_t)rd32(d + p + 4);
        o->verts[i].z = (int32_t)rd32(d + p + 8);
        if (ext) {
            for (k = 0; k < 3; k++) o->verts[i].normal[k] = rdf64(d + p + 12 + (size_t)k * 8);
            o->verts[i].a = rd16(d + p + 36);
            o->verts[i].b = rd16(d + p + 38);
        } else {
            o->verts[i].a = rd16(d + p + 12);
            o->verts[i].b = rd16(d + p + 14);
        }
    }
    for (i = 0; i < o->poly_count; i++) {
        wtb_poly *q = &o->polys[i];
        size_t size;
        size_t ioff = ext ? 32 : 4;
        if (len - p < ioff) return -1;
        q->color = rd16(d + p);
        if (ext) {
            q->material = rd16(d + p + 2);
            q->unknown = rd16(d + p + 4);
            for (k = 0; k < 3; k++) q->normal[k] = (float)rdf64(d + p + 6 + (size_t)k * 8);
            q->has_normal = 1;
            q->n = rd16(d + p + 30);
        } else {
            q->n = rd16(d + p + 2);
        }
        if (q->n < 1 || q->n > WTB_MAX_POLY_VERTS) return -1;
        size = ext ? (q->n <= 4 ? 40 : 46) : (q->n <= 4 ? 12 : 18);
        if (len - p < size) return -1;
        for (k = 0; k < q->n; k++) {
            q->idx[k] = rd16(d + p + ioff + (size_t)k * 2);
            if (q->idx[k] >= o->vert_count) return -1;
        }
        p += size;
    }
    *off = p;
    return 0;
}

static int parse_all(const uint8_t *data, size_t len, wtb_model *out, int ext)
{
    size_t off = 0;
    memset(out, 0, sizeof *out);
    out->extended = ext;
    while (off < len) {
        wtb_object *grown = realloc(out->objects, (size_t)(out->object_count + 1) * sizeof *grown);
        if (!grown) { wtb_free(out); return -1; }
        out->objects = grown;
        if (parse_object(data, len, &off, &out->objects[out->object_count], ext) != 0) {
            out->object_count++; /* so wtb_free releases the partial object */
            wtb_free(out);
            return -1;
        }
        out->object_count++;
    }
    return out->object_count > 0 ? 0 : -1;
}

/* DOS layout first; if it doesn't consume the record exactly, the 3D-edition one. */
int wtb_parse(const uint8_t *data, size_t len, wtb_model *out)
{
    if (parse_all(data, len, out, 0) == 0) return 0;
    return parse_all(data, len, out, 1);
}

void wtb_free(wtb_model *m)
{
    int i;
    for (i = 0; i < m->object_count; i++) {
        free(m->objects[i].verts);
        free(m->objects[i].polys);
    }
    free(m->objects);
    memset(m, 0, sizeof *m);
}
