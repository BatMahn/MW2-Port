/*
 * fx.c - weapon effects (see fx.h).
 */
#include "fx.h"
#include "bwd.h"
#include "wtb.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* engine 0x1025b168: { duration ticks, ?, sound, ?, light, needs object, ? } per type */
static const int TABLE[FX_TYPES][3] = {   /* duration, sound, light */
    {271, 251, 0}, {271, 251, 0}, {271, 251, 0}, {253, 210, 1}, {253, 210, 1}, {90, 250, 0},
    {579, 235, 1}, {398, 188, 0}, {90, 248, 0}, {162, 239, 0}, {0, -1, 0}, {1810, -1, 0},
    {398, 210, 1}, {579, 246, 1}, {72, 249, 0}, {72, 249, 0}, {72, 249, 0}, {108, 255, 1},
    {144, 252, 0}, {362, 210, 1}, {362, 210, 1}, {579, 235, 1}, {5430, 209, 1}, {36, -1, 0},
    {36, -1, 0}, {72, -1, 0}, {36, -1, 0}, {36, -1, 0}, {36, -1, 0}, {36, -1, 0},
    {36, -1, 0}, {36, -1, 0},
};

/* engine weapon table 0x1025a6c0 (0x58 per weapon): +0x08, +0x0c */
static const signed char MUZZLE[31][2] = {
    {26, 27}, {26, 27}, {26, 27}, {26, 27}, {26, 27}, {26, 27}, {26, 27}, {26, 27}, {26, 27}, {26, 27},
    {23, 24}, {23, 24}, {23, 24}, {23, 24}, {23, 24}, {23, 24}, {23, 24}, {23, 24}, {23, 24}, {23, 24},
    {-1, -1}, {30, 31}, {28, 29}, {28, 29}, {28, 29}, {28, 29}, {28, 29}, {28, 29}, {23, 24}, {23, 24},
    {-1, -1},
};
int fx_muzzle(int weapon, int k) { return weapon >= 0 && weapon < 31 && (k == 0 || k == 1) ? MUZZLE[weapon][k] : -1; }

int fx_duration(int type) { return type >= 0 && type < FX_TYPES ? TABLE[type][0] : 0; }
int fx_sound(int type) { return type >= 0 && type < FX_TYPES ? TABLE[type][1] : -1; }
int fx_light(int type) { return type >= 0 && type < FX_TYPES ? TABLE[type][2] : 0; }

static int rd16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

int fx_impact_type(int visual, int flags)
{
    int t = visual & 0xFF;
    if (visual < 0 || t >= 0x20) return -1;
    switch (t) {
    case 0: case 1: case 2:   /* lasers */
        if (flags & FX_HIT_PLAYER) t = 0x0e + t;
        else if (flags & FX_HIT_GROUND) t = 9;
        break;
    case 3: case 4:           /* missiles */
        if (flags & FX_HIT_GROUND) t = 0x0c;
        else if (flags & FX_HIT_PLAYER) t = 0x11;
        else if (flags & FX_HIT_STRUCT) t = t == 3 ? 0x13 : 0x14;
        break;
    case 5:                   /* bullets */
        if (flags & FX_HIT_UNIT) t = 8;
        else if (flags & FX_HIT_GROUND) t = 9;
        else if (flags & FX_HIT_PLAYER) t = 0x12;
        break;
    case 6:                   /* PPC */
        if (flags & FX_HIT_GROUND) t = 0x15;
        break;
    case 0x0b:                /* slugs: their own handler (0x10046d40), not decoded */
        return -1;
    default:
        break;
    }
    return t;
}

/* a 0x8000 / 0xb000 card's polygons (0x10026e80), not the 0x3000 billboard: pool slot si of the type */
static void card_polys(fx_defs *d, int type, int si, const wtb_object *o, const float sc3[3])
{
    int pq, vq, cq;
    if (o->poly_count <= 0 || !(o->polys[0].color & 0x8000)) return;
    for (pq = 0; pq < o->poly_count && d->card_n[type][si] < 2; pq++) {
        const wtb_poly *pp = &o->polys[pq];
        int ci = d->card_n[type][si];
        if (pp->n < 3 || pp->n > 4) continue;
        for (vq = 0; vq < pp->n; vq++) {
            const wtb_vertex *vv = &o->verts[pp->idx[vq]];
            d->card_v[type][si][ci][vq][0] = (float)vv->x * sc3[0];
            d->card_v[type][si][ci][vq][1] = (float)vv->y * sc3[1];
            d->card_v[type][si][ci][vq][2] = (float)vv->z * sc3[2];
            d->card_uv[type][si][ci][vq][0] = (float)vv->a / 64.0f;
            d->card_uv[type][si][ci][vq][1] = (float)vv->b / 64.0f;
        }
        if (pp->n == 3) for (cq = 0; cq < 3; cq++) { d->card_v[type][si][ci][3][cq] = d->card_v[type][si][ci][2][cq]; if (cq < 2) d->card_uv[type][si][ci][3][cq] = d->card_uv[type][si][ci][2][cq]; }
        for (cq = 0; cq < 3; cq++) d->card_nrm[type][si][ci][cq] = -pp->normal[cq];
        d->card_nv[type][si][ci] = pp->n;
        d->card_n[type][si]++;
    }
}

int fx_load(prj_archive *a, fx_defs *d)
{
    prj_record r;
    bwd_chunk *c;
    int n, k, model = -1, scale = 1, run[FX_FRAMES], nrun = 0, run_closed = 0;
    float sc3[3] = {1, 1, 1};
    memset(d, 0, sizeof *d);
    for (k = 0; k < FX_TYPES; k++) d->anim[k] = -1;

    /* frames: mw2_xpl1 */
    if (prj_read_named(a, "BWD", "MW2_XPL1", &r) == 0) {
        n = bwd_chunks(r.data, r.size, NULL, 0);
        c = n > 0 ? calloc((size_t)n, sizeof *c) : NULL;
        if (c) {
            bwd_chunks(r.data, r.size, c, n);
            for (k = 0; k < n; k++) {
                if (strcmp(c[k].tag, "BMPJ") == 0 && c[k].size >= 2) {
                    if (run_closed) { nrun = 0; run_closed = 0; }   /* a new run of frames */
                    if (nrun < FX_FRAMES) run[nrun++] = (uint16_t)rd16(c[k].data);
                } else if (strcmp(c[k].tag, "BMID") == 0 && c[k].size >= 2) {
                    fx_anim *an = &d->tex[(uint16_t)rd16(c[k].data) & 0x1FF];
                    memcpy(an->cel, run, sizeof run);
                    an->frames = nrun;
                    an->rate = 45;   /* engine default (0x1002d150) */
                    run_closed = 1;
                } else if (strcmp(c[k].tag, "BSEC") == 0 && c[k].size >= 4) {
                    int slot = (uint16_t)rd16(c[k].data) & 0x1FF, rate = rd16(c[k].data + 2);
                    if (rate > 0) d->tex[slot].rate = rate;
                }
            }
            free(c);
        }
        prj_record_free(&r);
    }

    /* pool objects: MW2_SHT1 (OBJ then XPLO) */
    if (prj_read_named(a, "BWD", "MW2_SHT1", &r) != 0) return -1;
    n = bwd_chunks(r.data, r.size, NULL, 0);
    c = n > 0 ? calloc((size_t)n, sizeof *c) : NULL;
    if (!c) { prj_record_free(&r); return -1; }
    bwd_chunks(r.data, r.size, c, n);
    for (k = 0; k < n; k++) {
        if (strcmp(c[k].tag, "OBJ") == 0 && c[k].size >= 52) {
            model = (uint16_t)rd16(c[k].data + 48);
            scale = (int32_t)(c[k].data[6] | c[k].data[7] << 8 | c[k].data[8] << 16 | (uint32_t)c[k].data[9] << 24);   /* OBJ scale x */
            if (scale < 1) scale = 1;
            {
                int q;
                for (q = 0; q < 3; q++) {
                    const uint8_t *d3 = c[k].data + 6 + 4 * q;
                    sc3[q] = (float)(int32_t)(d3[0] | d3[1] << 8 | d3[2] << 16 | (uint32_t)d3[3] << 24);
                    if (sc3[q] == 0) sc3[q] = 1;   /* 0x100420b0: 0 -> 1 */
                }
            }
        }
        else if (strcmp(c[k].tag, "XPLO") == 0 && c[k].size >= 6) {
            int texslot = rd16(c[k].data + 2), type = rd16(c[k].data + 4);
            int si;
            if (type < 0 || type >= FX_TYPES) continue;
            si = d->slots[type]++;
            if (si > 0 && si < FX_CARD_SLOTS && d->card_n[type][0] > 0 && model >= 0) {   /* a card type's later pool slot: its own model */
                prj_record mr;
                if (prj_read(a, prj_find_type(a, "POLY"), model, &mr) == 0) {
                    wtb_model m;
                    if (wtb_parse(mr.data, mr.size, &m) == 0 && m.object_count > 0) { card_polys(d, type, si, &m.objects[0], sc3); wtb_free(&m); }
                    prj_record_free(&mr);
                }
            }
            if (si == 0) {
                prj_record mr;
                d->anim[type] = texslot >= 0 && texslot < 512 ? texslot : -1;
                if (model >= 0 && prj_read(a, prj_find_type(a, "POLY"), model, &mr) == 0) {   /* card size */
                    wtb_model m;
                    if (wtb_parse(mr.data, mr.size, &m) == 0 && m.object_count > 0) {
                        const wtb_object *o = &m.objects[0];
                        int mnx = 1 << 30, mxx = -(1 << 30), mny = 1 << 30, mxy = -(1 << 30), v;
                        for (v = 0; v < o->vert_count; v++) {
                            if (o->verts[v].x < mnx) mnx = o->verts[v].x;
                            if (o->verts[v].x > mxx) mxx = o->verts[v].x;
                            if (o->verts[v].y < mny) mny = o->verts[v].y;
                            if (o->verts[v].y > mxy) mxy = o->verts[v].y;
                        }
                        if (o->vert_count) { d->half_w[type] = (float)(mxx - mnx) * 0.5f * (float)scale; d->half_h[type] = (float)(mxy - mny) * 0.5f * (float)scale; }
                        if (o->poly_count > 0 && (o->polys[0].color & 0xf000) == 0x3000) {   /* 0x10026fc7 -> 0x1002a8e0 */
                            int ft = -1, tp = -1, q;
                            for (q = 0; q < o->polys[0].n; q++) {
                                const wtb_vertex *vv = &o->verts[o->polys[0].idx[q]];
                                if (vv->a != 0) continue;
                                if (vv->b == 0) tp = o->polys[0].idx[q]; else ft = o->polys[0].idx[q];
                            }
                            if (ft >= 0 && tp >= 0) {
                                d->bb[type] = 1;
                                d->bb_foot[type][0] = (float)(o->verts[ft].x * scale); d->bb_foot[type][1] = (float)(o->verts[ft].y * scale); d->bb_foot[type][2] = (float)(o->verts[ft].z * scale);
                                d->bb_top[type][0] = (float)(o->verts[tp].x * scale); d->bb_top[type][1] = (float)(o->verts[tp].y * scale); d->bb_top[type][2] = (float)(o->verts[tp].z * scale);
                            }
                        }
                        /* the card's colour picks its texture slot: bit 15 set -> 0x100 + low byte (DEATH0,
                         * JET0, MZL0); otherwise bits 4-11 (LAS0, XPL0, PPC0, BUL0: equal to XPLO's field) */
                        if (o->poly_count > 0 && (o->polys[0].color & 0x8000)) d->anim[type] = 0x100 | (o->polys[0].color & 0xFF);
                        card_polys(d, type, 0, o, sc3);
                        wtb_free(&m);
                    }
                    prj_record_free(&mr);
                }
            }
        }
    }
    free(c);
    prj_record_free(&r);
    return 0;
}

int fx_detail_low;
int fx_spawn(fx_world *w, const fx_defs *d, int type, const float pos[3], int32_t now)
{
    int k, alive = 0, near = 0, free_i = -1, slot;
    uint32_t used = 0;
    if (type < 0 || type >= FX_TYPES || d->slots[type] <= 0 || fx_duration(type) <= 0) return -1;
    for (k = 0; k < FX_LIVE; k++) {
        const fx_live *e = &w->e[k];
        if (!e->alive) { if (free_i < 0) free_i = k; continue; }
        if (e->type != type) continue;
        alive++;
        if (e->slot >= 0 && e->slot < 32) used |= 1u << e->slot;
        if (type < 0x17) {   /* 0x10045d00: neighbours of the type within 5 m (0x102485f0) - DISPLAY DETAIL high allows two
                              * (three together), low none */
            float dx = e->pos[0] - pos[0], dy = e->pos[1] - pos[1], dz = e->pos[2] - pos[2];
            if (dx * dx + dy * dy + dz * dz < 500.0f * 500.0f && ++near > (fx_detail_low ? 0 : 2)) return -1;
        }
    }
    if (alive >= d->slots[type] || free_i < 0) return -1;   /* no free pool slot of the type */
    for (slot = 0; slot < 31 && (used >> slot & 1); slot++) {}   /* 0x10045d00: the type's first free pool entry (pool order) */
    w->e[free_i].slot = slot;
    w->e[free_i].type = type;
    w->e[free_i].alive = 1;
    memcpy(w->e[free_i].pos, pos, sizeof w->e[free_i].pos);
    w->e[free_i].start = now;
    w->e[free_i].end = now + fx_duration(type);
    return free_i;
}

void fx_update(fx_world *w, int32_t now)
{
    int k;
    for (k = 0; k < FX_LIVE; k++)
        if (w->e[k].alive && now >= w->e[k].end) w->e[k].alive = 0;
}

int fx_frame(const fx_defs *d, const fx_live *e, int32_t now)
{
    const fx_anim *an;
    int f;
    if (!e->alive || d->anim[e->type] < 0) return -1;
    an = &d->tex[d->anim[e->type]];
    if (an->frames <= 0) return -1;
    f = (int)((now - e->start) / (an->rate > 0 ? an->rate : 45)) % an->frames;   /* 0x1002ce50: loops */
    return an->cel[f];
}
