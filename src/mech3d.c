/* mech3d.c - see mech3d.h. */
#include "mech3d.h"
#include "bwd.h"
#include "anim.h"
#include "mek.h"
#include <ctype.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_NODES 256

typedef struct {
    int     present;
    int16_t parent;
    int32_t offset[3];
    uint16_t model;
} node;

/* Position of node i; parents missing from a reduced set come from set 0. */
static int world_pos(const node *nodes, const node *base, int i, int32_t out[3], int depth)
{
    const node *nd;
    int k;
    if (i < 0 || i >= MAX_NODES || depth > MAX_NODES) return -1;
    nd = nodes[i].present ? &nodes[i] : (base[i].present ? &base[i] : NULL);
    if (!nd) return -1;
    if (nd->parent < 0 || nd->parent == i) {
        for (k = 0; k < 3; k++) out[k] = nd->offset[k];
        return 0;
    }
    if (world_pos(nodes, base, nd->parent, out, depth + 1) != 0) return -1;
    for (k = 0; k < 3; k++) out[k] += nd->offset[k];
    return 0;
}

int mech3d_lod;
int world3d_density_low;
static int mech3d_load_repr(prj_archive *a, const char *record, int repr, mech3d *out);
static void mounts_local(mech3d *m);
/* The engine keeps ONE skeleton per object and a level of detail only swaps each node's geometry (3Dfx 0x10026430: node
 * table +0x08 + repr x 4 = the geometry for that representation; the node transforms stay) - so a lower representation,
 * and the cockpit set (repr 4), hang on the full model's (repr 0) node offsets, animation bindings and eye node. DOSBox:
 * the Timber Wolf's struts on its own repr-4 skeleton matched the DOS capture at IoU 0.05 (0.48 with the eye moved),
 * on the repr-0 skeleton 0.74. */
void mech3d_use_skeleton(mech3d *dst, const mech3d *src)
{
    dst->skel_count = src->skel_count;
    memcpy(dst->skel_index, src->skel_index, sizeof src->skel_index);
    memcpy(dst->skel_parent, src->skel_parent, sizeof src->skel_parent);
    memcpy(dst->skel_offset, src->skel_offset, sizeof src->skel_offset);
    dst->bind_count = src->bind_count;
    memcpy(dst->bind_node, src->bind_node, sizeof src->bind_node);
    memcpy(dst->bind_track, src->bind_track, sizeof src->bind_track);
    dst->eye_node = src->eye_node; dst->twist_node = src->twist_node;
    memcpy(dst->pofo_node, src->pofo_node, sizeof src->pofo_node);
}
static int load_on_full_skeleton(prj_archive *a, const char *record, int repr, mech3d *out)
{
    mech3d full;
    if (mech3d_load_repr(a, record, repr, out) != 0) return -1;
    if (repr > 0 && mech3d_load_repr(a, record, 0, &full) == 0) {
        if (full.skel_count > 0) mech3d_use_skeleton(out, &full);
        mech3d_free(&full);
    }
    return 0;
}
int mech3d_load(prj_archive *a, const char *record, int repr, mech3d *out)
{
    if (repr == 0 && mech3d_lod > 0 && load_on_full_skeleton(a, record, mech3d_lod, out) == 0) { mounts_local(out); return 0; }   /* DISPLAY DETAIL low */
    if (mech3d_load_repr(a, record, repr, out) != 0) return -1;
    mounts_local(out);
    return 0;
}
static int mech3d_load_repr(prj_archive *a, const char *record, int repr, mech3d *out)
{
    prj_record rec;
    bwd_chunk *chunks = NULL;
    node *nodes = NULL, *root_set = NULL;
    int n, k, set = 0, prev = 0x7fff, poly_type, rc = -1, i;

    memset(out, 0, sizeof *out);
    if (prj_read_named(a, "BWD", record, &rec) != PRJ_OK) return -1;
    poly_type = prj_find_type(a, "POLY");
    n = bwd_chunks(rec.data, rec.size, NULL, 0);
    nodes = calloc(MAX_NODES, sizeof *nodes);
    root_set = calloc(MAX_NODES, sizeof *root_set);
    if (n <= 0 || !nodes || !root_set || !(chunks = calloc((size_t)n, sizeof *chunks))) goto done;
    bwd_chunks(rec.data, rec.size, chunks, n);

    /* Skeleton sets, sized by DTBL: the first holds N nodes (DTBL word 2), each
     * further detail level N-1 (they share the root), and the last, reduced set
     * whatever remains. Verified against every mech record's OBJ total. */
    {
        int per = 0, seen = 0, set_size;
        for (k = 0; k < n; k++)
            if (strcmp(chunks[k].tag, "DTBL") == 0 && chunks[k].size >= 8)
                per = chunks[k].data[4] | (chunks[k].data[5] << 8);
        if (per <= 1) goto done;
        set_size = per;
        for (k = 0; k < n; k++) {
            bwd_obj o;
            node *dst = NULL;
            if (bwd_obj_decode(&chunks[k], &o) != 0) continue;
            if (seen == set_size) { set++; seen = 0; set_size = per - 1; }
            seen++;
            if (o.index < 0 || o.index >= MAX_NODES) goto done;
            if (set == 0) dst = root_set;
            else if (set == repr) dst = nodes;
            if (!dst) continue;
            dst[o.index].present = 1;
            dst[o.index].parent = o.parent;
            memcpy(dst[o.index].offset, o.offset, sizeof o.offset);
            dst[o.index].model = o.model;
        }
    }
    (void)prev;
    out->repr_count = set + 1;
    if (repr < 0 || repr > set) goto done;
    if (repr == 0) memcpy(nodes, root_set, MAX_NODES * sizeof *nodes);

    for (i = 0; i < MAX_NODES; i++) {
        prj_record mr;
        mech3d_part *p;
        if (!nodes[i].present) continue;
        out->node_count++;
        if (prj_read(a, poly_type, nodes[i].model, &mr) != PRJ_OK) continue;
        if (strcasecmp(mr.name, "DUMMY") == 0) { prj_record_free(&mr); continue; }
        p = realloc(out->parts, (size_t)(out->part_count + 1) * sizeof *p);
        if (!p) { prj_record_free(&mr); goto done; }
        out->parts = p;
        p = &out->parts[out->part_count];
        memset(p, 0, sizeof *p);
        snprintf(p->model_name, sizeof p->model_name, "%s", mr.name);
        p->node = i;
        if (world_pos(nodes, root_set, i, p->pos, 0) != 0 || wtb_parse(mr.data, mr.size, &p->model) != 0) {
            prj_record_free(&mr);
            goto done;
        }
        prj_record_free(&mr);
        out->part_count++;
    }
    /* keep the node tree (requested set, falling back to set 0 for missing parents) */
    for (i = 0; i < MAX_NODES && out->skel_count < MECH3D_MAX_NODES; i++) {
        const node *nd = nodes[i].present ? &nodes[i] : (root_set[i].present ? &root_set[i] : NULL);
        if (!nd) continue;
        out->skel_index[out->skel_count] = (int16_t)i;
        out->skel_parent[out->skel_count] = nd->parent;
        memcpy(out->skel_offset[out->skel_count], nd->offset, sizeof nd->offset);
        out->skel_count++;
    }
    /* the pilot's eye node: EYEO { i16 node } (engine 0x1003eabe) */
    out->eye_node = -1;
    for (k = 0; k < n; k++)
        if (strcmp(chunks[k].tag, "EYEO") == 0 && chunks[k].size >= 2)
            out->eye_node = (int16_t)(chunks[k].data[0] | (chunks[k].data[1] << 8));
    for (k = 0; k < 8; k++) out->pofo_node[k] = -1;
    for (k = 0; k < n; k++)
        if (strcmp(chunks[k].tag, "POFO") == 0 && chunks[k].size >= 4) {
            int nd = (int16_t)(chunks[k].data[0] | (chunks[k].data[1] << 8)), sl = (int16_t)(chunks[k].data[2] | (chunks[k].data[3] << 8));
            if (sl >= 0 && sl < 8) out->pofo_node[sl] = nd;
        }
    /* damage location groups: OBJL { i16 node, i16 group } */
    for (k = 0; k < n; k++)
        if (strcmp(chunks[k].tag, "OBJL") == 0 && chunks[k].size >= 4) {
            int nodei = (int16_t)(chunks[k].data[0] | (chunks[k].data[1] << 8));
            int grp = (int16_t)(chunks[k].data[2] | (chunks[k].data[3] << 8));
            for (i = 0; i < out->part_count; i++) if (out->parts[i].node == nodei) out->parts[i].group = grp;
        }
    /* pieces with no OBJL group (JN1DECLL / JN1DECLR: the Jenner's arm decals) belong to the location of the nearest
     * grouped piece up the node tree - they went on floating when their arm was shot off */
    for (i = 0; i < out->part_count; i++) {
        int nd = out->parts[i].node, depth;
        if (out->parts[i].group) continue;
        for (depth = 0; depth < MECH3D_MAX_NODES && nd >= 0 && !out->parts[i].group; depth++) {
            int q, par = -1;
            for (q = 0; q < out->skel_count; q++) if (out->skel_index[q] == nd) { par = out->skel_parent[q]; break; }
            if (par < 0 || par == nd) break;
            for (q = 0; q < out->part_count; q++) if (out->parts[q].node == par && out->parts[q].group > 0) { out->parts[i].group = out->parts[q].group; break; }
            nd = par;
        }
    }
    /* torso node for twisting: the node carrying the cockpit/torso part (*_HEAD) */
    out->twist_node = -1;
    for (i = 0; i < out->part_count; i++) {
        size_t nl = strlen(out->parts[i].model_name);
        if (nl >= 5 && strcasecmp(out->parts[i].model_name + nl - 5, "_HEAD") == 0) { out->twist_node = out->parts[i].node; break; }
    }
    /* animation id and the TSK bindings "node;70,flags,track" */
    for (k = 0; k < n; k++) {
        const bwd_chunk *c = &chunks[k];
        if (strcmp(c->tag, "ANIM") == 0 && c->size >= 2) out->anim_id = c->data[0] | (c->data[1] << 8);
        if (strcmp(c->tag, "TSK") == 0 && c->size > 6 && out->bind_count < MECH3D_MAX_BINDS) {
            char txt[64];
            size_t l = c->size - 6 < sizeof txt - 1 ? c->size - 6 : sizeof txt - 1;
            int nodei, cmd, flags, track;
            memcpy(txt, c->data + 6, l);
            txt[l] = '\0';
            if (sscanf(txt, "%d;%d,%d,%d", &nodei, &cmd, &flags, &track) == 4) {
                out->anim_rate = cmd;   /* ticks per key (engine 0x1000dec0 "%ld,%d,%d"): 70 Timber Wolf, 45 Kit Fox, 37 Firemoth */
                out->bind_node[out->bind_count] = (int16_t)nodei;
                out->bind_track[out->bind_count] = (int16_t)track;
                out->bind_count++;
            }
        }
    }
    rc = out->part_count > 0 ? 0 : -1;
done:
    if (rc != 0) mech3d_free(out);
    free(chunks);
    free(nodes);
    free(root_set);
    prj_record_free(&rec);
    return rc;
}

void mech3d_free(mech3d *m)
{
    int i;
    for (i = 0; i < m->part_count; i++) { wtb_free(&m->parts[i].model); free(m->parts[i].hull); }
    free(m->parts);
    memset(m, 0, sizeof *m);
}

/* ---- world objects ----------------------------------------------------------- */

#include <math.h>

typedef struct {
    int     present;
    int16_t parent;
    int32_t offset[3];
    float   yaw;
    uint16_t model;
    int coll;
    int objtype;
} wnode;

/* world transform of node i: position and accumulated yaw */
static int wpos(const wnode *n, int i, float out[3], float *yaw, int depth)
{
    float pp[3], py, c, s;
    if (i < 0 || i >= MAX_NODES * 4 || !n[i].present || depth > 64) return -1;
    if (n[i].parent < 0 || n[i].parent == i || !n[n[i].parent].present) {
        out[0] = (float)n[i].offset[0]; out[1] = (float)n[i].offset[1]; out[2] = (float)n[i].offset[2];
        *yaw = n[i].yaw;
        return 0;
    }
    if (wpos(n, n[i].parent, pp, &py, depth + 1) != 0) return -1;
    c = cosf(py * 3.14159265f / 180.0f);
    s = sinf(py * 3.14159265f / 180.0f);
    out[0] = pp[0] + c * (float)n[i].offset[0] + s * (float)n[i].offset[2];
    out[1] = pp[1] + (float)n[i].offset[1];
    out[2] = pp[2] - s * (float)n[i].offset[0] + c * (float)n[i].offset[2];
    *yaw = py + n[i].yaw;
    return 0;
}

int world3d_append(prj_archive *a, const char *record, int with_damaged, mech3d *out)
{
    prj_record rec;
    bwd_chunk *chunks = NULL;
    wnode *nodes = NULL;
    int n, k, i, poly_type, rc = -1, max = MAX_NODES * 4;

    if (prj_read_named(a, "BWD", record, &rec) != PRJ_OK) return -1;
    poly_type = prj_find_type(a, "POLY");
    n = bwd_chunks(rec.data, rec.size, NULL, 0);
    nodes = calloc((size_t)max, sizeof *nodes);
    if (n <= 0 || !nodes || !(chunks = calloc((size_t)n, sizeof *chunks))) goto done;
    bwd_chunks(rec.data, rec.size, chunks, n);
    for (k = 0; k < n; k++) {
        bwd_obj o;
        if (bwd_obj_decode(&chunks[k], &o) != 0 || o.index < 0 || o.index >= max) continue;
        nodes[o.index].present = 1;
        nodes[o.index].parent = o.parent;
        memcpy(nodes[o.index].offset, o.offset, sizeof o.offset);
        nodes[o.index].yaw = (float)o.rotation[1] / 65536.0f;
        nodes[o.index].model = o.model;
        nodes[o.index].coll = o.coll;
        nodes[o.index].objtype = o.objtype;
    }
    for (i = 0; i < max; i++) {
        prj_record mr;
        mech3d_part *p;
        float pos[3], yaw;
        if (!nodes[i].present) continue;
        out->node_count++;
        if (prj_read(a, poly_type, nodes[i].model, &mr) != PRJ_OK) continue;
        if (strcasecmp(mr.name, "DUMMY") == 0 || strncasecmp(mr.name, "S_1DUMMY", 8) == 0 || mr.size == 0 ||
            (!with_damaged && strncasecmp(mr.name, "SA", 2) == 0)) { prj_record_free(&mr); continue; }
        if (wpos(nodes, i, pos, &yaw, 0) != 0) { prj_record_free(&mr); continue; }
        p = realloc(out->parts, (size_t)(out->part_count + 1) * sizeof *p);
        if (!p) { prj_record_free(&mr); goto done; }
        out->parts = p;
        p = &out->parts[out->part_count];
        memset(p, 0, sizeof *p);
        snprintf(p->model_name, sizeof p->model_name, "%s", mr.name);
        p->pos[0] = (int32_t)lrintf(pos[0]); p->pos[1] = (int32_t)lrintf(pos[1]); p->pos[2] = (int32_t)lrintf(pos[2]);
        p->yaw = yaw;
        p->node = -1;
        p->coll = nodes[i].coll;
        p->objtype = nodes[i].objtype;
        snprintf(p->rec, sizeof p->rec, "%.10s", record);
        p->obj_index = i;
        p->parent_obj = nodes[i].parent;
        p->hidden = with_damaged && strncasecmp(mr.name, "SA", 2) == 0;   /* destroyed variants wait (GT B) */
        /* the projectile / effect pool (MW2_SHT1: lasers, bullets, explosion and PPC cards at 0,0,0) is never linked into
         * the world until used (the front end draws shots and effects itself): its cards were drawn at the map origin */
        if (strncasecmp(record, "MW2_SHT", 7) == 0) p->hidden = 1;
        /* OBJECT DENSITY: the type 0xc0 objects come and go (engine 0x1000de50 unlinks / relinks them; Combat Variables
         * and the in-game menu) - loaded always, hidden while it is LOW */
        p->sparse = (nodes[i].objtype & 0xf0) == 0xc0;
        if (p->sparse && world3d_density_low) p->hidden = 1;
        if (wtb_parse(mr.data, mr.size, &p->model) != 0) { prj_record_free(&mr); continue; }
        prj_record_free(&mr);
        if (p->model.object_count) {   /* flat pieces lying in the ground plane (T_1LINE, T_1SPOTA ...) raised 4 cm */
            const wtb_object *o = &p->model.objects[0];
            int q, flat = o->vert_count > 2;
            for (q = 1; q < o->vert_count && flat; q++) if (o->verts[q].y != o->verts[0].y) flat = 0;
            if (flat) {
                /* only the T_1... pieces (the training base's dropship shadows) - other flat pieces are real surfaces
                 * (TL1SIDWK a sidewalk, V_AFLAM* flames ...) */
                /* (the T_1 pieces had been drawn as translucent shadows: the engine has no shadow pass - they are
                 * painted pad / runway markings, type-1 colour 0x1c70, drawn flat in palette 0xcf; see glr) */
                if (p->pos[1] + o->verts[0].y <= 2 && p->pos[1] + o->verts[0].y >= -2) p->pos[1] += 4;
            }
        }
        if (getenv("MW2_DEBUG_WORLD")) fprintf(stderr, "world %s obj %d %s at %d,%d,%d type 0x%x\n", record, i, p->model_name, p->pos[0], p->pos[1], p->pos[2], (unsigned)p->objtype);   /* tests */
        out->part_count++;
    }
    {   /* a destroyed variant's children (its SCD / SCE chunks) wait with it: hidden while any ancestor is hidden */
        int q, first = 0, changed = 1;
        for (q = 0; q < out->part_count; q++) if (strcmp(out->parts[q].rec, record) == 0) { first = q; break; }
        while (changed) {
            changed = 0;
            for (q = first; q < out->part_count; q++) {
                mech3d_part *c = &out->parts[q];
                int j;
                if (c->hidden || c->parent_obj < 0 || strcmp(c->rec, record) != 0) continue;
                for (j = first; j < out->part_count; j++)
                    if (out->parts[j].obj_index == c->parent_obj && strcmp(out->parts[j].rec, record) == 0) {
                        if (out->parts[j].hidden) { c->hidden = 1; changed = 1; }
                        break;
                    }
            }
        }
    }
    rc = 0;
done:
    free(chunks);
    free(nodes);
    prj_record_free(&rec);
    return rc;
}

/* BLKX (36 bytes, in world records before an INCL): the included block's placement - i32 scale x / y / z (always 1),
 * i32 pitch / yaw / roll in degrees (only yaw used: 0, +-45, +-90, 359), i32 x / y / z. E.g. YELLWLD1 sets its terrain
 * tiles 90 m down (y -9000) and its firebase block at Nav Zeta; nested blocks add up along the include chain. */
typedef struct { char name[11]; float yaw, t[3]; int set; } blk_place;
static int32_t rd32s(const uint8_t *p) { return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24); }

int world3d_mission(prj_archive *a, const char *scene, int with_damaged, mech3d *out)
{
    bwd_mission m;
    int i, k, used = 0, nb = 0;
    blk_place *bp;
    if (bwd_mission_load(a, scene, &m) != 0) return -1;
    bp = calloc((size_t)(m.record_count > 0 ? m.record_count : 1) * 8, sizeof *bp);
    /* the placements: each record's BLKX -> the next INCL's record, composed with the including record's own */
    for (i = 0; bp && i < m.record_count; i++) {
        const bwd_record *r = &m.records[i];
        float pyaw = 0, pt[3] = {0, 0, 0};
        int have = 0, q;
        const uint8_t *bx = NULL;
        for (q = 0; q < nb; q++) if (strcasecmp(bp[q].name, r->name) == 0) { pyaw = bp[q].yaw; memcpy(pt, bp[q].t, sizeof pt); break; }
        for (k = 0; k < r->chunk_count; k++) {
            const bwd_chunk *c = &r->chunks[k];
            if (strcmp(c->tag, "BLKX") == 0 && c->size >= 36) { bx = c->data; have = 1; }
            else if (strcmp(c->tag, "INCL") == 0) {
                uint16_t id;
                char nm[11];
                if (bwd_incl(c, &id, nm) == 0 && have && nb < m.record_count * 8) {
                    float yaw = (float)rd32s(bx + 16), x = (float)rd32s(bx + 24), y = (float)rd32s(bx + 28), z = (float)rd32s(bx + 32);
                    float h = pyaw * 3.14159265f / 180.0f;
                    blk_place *e = &bp[nb++];
                    snprintf(e->name, sizeof e->name, "%s", nm);
                    e->yaw = pyaw + yaw;   /* the parent's turn, then its offset */
                    e->t[0] = pt[0] + cosf(h) * x + sinf(h) * z;
                    e->t[1] = pt[1] + y;
                    e->t[2] = pt[2] - sinf(h) * x + cosf(h) * z;
                    e->set = 1;
                }
                have = 0;
            }
        }
    }
    for (i = 0; i < m.record_count; i++) {
        int objs = 0, repr = 0, first = out->part_count, q;
        if (m.records[i].external) continue;
        for (k = 0; k < m.records[i].chunk_count; k++) {
            if (strcmp(m.records[i].chunks[k].tag, "OBJ") == 0) objs++;
            else if (strcmp(m.records[i].chunks[k].tag, "REPR") == 0) repr++;
        }
        if (objs == 0 || repr) continue;
        if (world3d_append(a, m.records[i].name, with_damaged, out) == 0) used++;
        for (q = 0; bp && q < nb; q++)
            if (bp[q].set && strcasecmp(bp[q].name, m.records[i].name) == 0) {
                float h = bp[q].yaw * 3.14159265f / 180.0f, ch = cosf(h), sh = sinf(h);
                int pi;
                for (pi = first; pi < out->part_count; pi++) {
                    mech3d_part *p = &out->parts[pi];
                    float x = (float)p->pos[0], z = (float)p->pos[2];
                    p->pos[0] = (int32_t)lrintf(ch * x + sh * z + bp[q].t[0]);
                    p->pos[1] = (int32_t)lrintf((float)p->pos[1] + bp[q].t[1]);
                    p->pos[2] = (int32_t)lrintf(-sh * x + ch * z + bp[q].t[2]);
                    if (bp[q].yaw != 0) {
                        if (p->has_rot) {   /* R = Ryaw * R */
                            float R[9];
                            int c3;
                            memcpy(R, p->rot, sizeof R);
                            for (c3 = 0; c3 < 3; c3++) {
                                p->rot[c3] = ch * R[c3] + sh * R[6 + c3];
                                p->rot[6 + c3] = -sh * R[c3] + ch * R[6 + c3];
                            }
                        } else p->yaw += bp[q].yaw;
                    }
                }
                break;
            }
    }
    free(bp);
    bwd_mission_free(&m);
    return used;
}

/* Formation member offsets (FTBL chunk): char name[24], i32 leader yaw (16.16 deg),
 * then per follower { i32 x, i32 z, i32 yaw 16.16 }. Looked up by name in the
 * mission's records first, then the shared FORMATNS record. */
typedef struct { int found; float leader_yaw; int count; int32_t x[16], z[16]; float yaw[16]; } formation;

static int ftbl_match(const bwd_chunk *c, const char *name, formation *f)
{
    char nm[25];
    size_t k, n;
    if (strcmp(c->tag, "FTBL") != 0 || c->size < 28) return 0;
    memcpy(nm, c->data, 24); nm[24] = '\0';
    if (strcasecmp(nm, name) != 0) return 0;
    f->found = 1;
    f->leader_yaw = (float)(int32_t)((uint32_t)c->data[24] | ((uint32_t)c->data[25] << 8) | ((uint32_t)c->data[26] << 16) | ((uint32_t)c->data[27] << 24)) / 65536.0f;
    n = (c->size - 28) / 12;
    if (n > 16) n = 16;
    for (k = 0; k < n; k++) {
        const uint8_t *q = c->data + 28 + k * 12;
        f->x[k] = (int32_t)((uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24));
        f->z[k] = (int32_t)((uint32_t)q[4] | ((uint32_t)q[5] << 8) | ((uint32_t)q[6] << 16) | ((uint32_t)q[7] << 24));
        f->yaw[k] = (float)(int32_t)((uint32_t)q[8] | ((uint32_t)q[9] << 8) | ((uint32_t)q[10] << 16) | ((uint32_t)q[11] << 24)) / 65536.0f;
    }
    f->count = (int)n;
    return 1;
}

static void find_formation(prj_archive *a, const bwd_mission *m, const char *name, formation *f)
{
    int i, k;
    prj_record rec;
    memset(f, 0, sizeof *f);
    for (i = 0; i < m->record_count && !f->found; i++)
        for (k = 0; k < m->records[i].chunk_count && !f->found; k++) ftbl_match(&m->records[i].chunks[k], name, f);
    if (!f->found && prj_read_named(a, "BWD", "FORMATNS", &rec) == PRJ_OK) {
        int n = bwd_chunks(rec.data, rec.size, NULL, 0);
        bwd_chunk *c = n > 0 ? calloc((size_t)n, sizeof *c) : NULL;
        if (c) {
            bwd_chunks(rec.data, rec.size, c, n);
            for (k = 0; k < n && !f->found; k++) ftbl_match(&c[k], name, f);
            free(c);
        }
        prj_record_free(&rec);
    }
}

int world3d_formation(prj_archive *a, const char *name, int slot, float *x, float *z)
{
    formation f;
    bwd_mission none;
    float ly, c, sn;
    memset(&none, 0, sizeof none);
    find_formation(a, &none, name, &f);
    if (!f.found || slot < 1 || slot - 1 >= f.count) return -1;
    ly = f.leader_yaw * 3.14159265f / 180.0f;   /* slot relative to the leader's facing, as world3d_actors stores it */
    c = cosf(ly); sn = sinf(ly);
    *x = c * (float)f.x[slot - 1] - sn * (float)f.z[slot - 1];
    *z = sn * (float)f.x[slot - 1] + c * (float)f.z[slot - 1];
    return 0;
}

int world3d_actors(prj_archive *a, const char *scene, world_actor **actors, int *count)
{
    *actors = NULL;
    *count = 0;
    bwd_mission m;
    const bwd_record *scn = NULL;
    const bwd_chunk *tables[64];
    int ntables = 0, i, k, per_table[64], bwd_type = prj_find_type(a, "BWD");

    if (bwd_mission_load(a, scene, &m) != 0) return -1;
    for (i = 0; i < m.record_count; i++)
        if (strcasecmp(m.records[i].name, scene) == 0) scn = &m.records[i];
    char star[64][17];
    int star_tex[64], star_all[64], nstar_set = 0;
    memset(star, 0, sizeof star);
    memset(star_tex, 0, sizeof star_tex);
    if (scn)
        for (k = 0; k < scn->chunk_count; k++) {
            if (strcmp(scn->chunks[k].tag, "MTBL") == 0 && ntables < 64) tables[ntables++] = &scn->chunks[k];
            /* STAR: per state table, 24-byte entries { u32, u32, char formation[16] } */
            if (strcmp(scn->chunks[k].tag, "STAR") == 0) {
                size_t e;
                nstar_set = 1;
                for (e = 0; e < 64 && (e + 1) * 24 <= scn->chunks[k].size; e++) {
                    const uint8_t *q = scn->chunks[k].data + e * 24;
                    memcpy(star[e], q + 8, 16);
                    star_tex[e] = (int)((uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24);
                    star_all[e] = (int)((uint32_t)q[4] | (uint32_t)q[5] << 8 | (uint32_t)q[6] << 16 | (uint32_t)q[7] << 24);
                    if (getenv("MW2_DEBUG_STARALL")) fprintf(stderr, "star %d %.16s alliance %d tex %d\n", (int)e, star[e], star_all[e], star_tex[e]);
                }
            }
        }
    memset(per_table, 0, sizeof per_table);

    for (i = 0; i < m.record_count; i++) {
        for (k = 0; k < m.records[i].chunk_count; k++) {
            bwd_gps g;
            uint16_t start_id;
            const char *start_name, *skel;
            int r2, c2, found = 0;
            bwd_navpoint nav;
            mech3d piece;
            if (bwd_gps_decode(&m.records[i].chunks[k], &g) != 0) continue;
            if (g.is_player) {   /* the human player's GPS (+0x0e == 0, engine 0x1003f36b: whatever record holds it - the
                                  * shell's USERSTAR slot 0 in the campaign / Instant Action, the mission's own TNx#USS1
                                  * in Cadet Training) is the player, never an actor. The player leads the star (engine:
                                  * starmates' "my leader" 0x2201 is the player), so the starmates take follower slots 1, 2.. */
                if (g.table >= 0 && g.table < 64) per_table[g.table]++;
                continue;
            }
            if (g.table < 0 || g.table >= ntables || tables[g.table]->size < 106) continue;
            start_id = (uint16_t)(tables[g.table]->data[102] | (tables[g.table]->data[103] << 8));
            start_name = prj_symbol_name(a, bwd_type, -1); /* placeholder, resolved below */
            start_name = NULL;
            for (r2 = 0; r2 < prj_symbol_count(a, bwd_type); r2++)
                if (prj_symbol_id(a, bwd_type, r2) == start_id) { start_name = prj_symbol_name(a, bwd_type, r2); break; }
            if (!start_name) continue;
            /* the start record's nav point (it is part of the mission tree) */
            for (r2 = 0; r2 < m.record_count && !found; r2++) {
                if (strcasecmp(m.records[r2].name, start_name) != 0) continue;
                for (c2 = 0; c2 < m.records[r2].chunk_count; c2++)
                    if (bwd_navp(&m.records[r2].chunks[c2], &nav) == 0) { found = 1; break; }
            }
            if (!found) continue;
            skel = NULL;
            for (r2 = 0; r2 < prj_symbol_count(a, bwd_type); r2++)
                if (prj_symbol_id(a, bwd_type, r2) == g.skeleton_id) { skel = prj_symbol_name(a, bwd_type, r2); break; }
            if (!skel || mech3d_load(a, skel, 0, &piece) != 0) continue;
            piece.origin[0] = nav.x; piece.origin[1] = nav.y; piece.origin[2] = nav.z;
            {
                /* leader at the start point; follower n at formation offset n-1 */
                int slot = per_table[g.table]++;
                float fx = 0, fz = 0, yaw, c = 0, sn = 0;
                formation f;
                void *grown_unused = NULL;
                {   /* the shell's chosen formations (-of= friendly / -oe= enemy) override the star's */
                    const char *ov = getenv(g.table == 0 ? "MW2_OF" : "MW2_OE");
                    find_formation(a, &m, ov && *ov ? ov : star[g.table][0] ? star[g.table] : "star", &f);
                    if (!f.found && ov && *ov) find_formation(a, &m, star[g.table][0] ? star[g.table] : "star", &f);
                }
                /* group heading = the start nav's heading; formation slots and facings are relative to it */
                yaw = nav.heading + (f.found ? f.leader_yaw : 0);
                if (slot > 0) {
                    float hx = cosf(nav.heading * 3.14159265f / 180.0f), hz = sinf(nav.heading * 3.14159265f / 180.0f), ox, oz;
                    if (f.found && slot - 1 < f.count) {
                        ox = (float)f.x[slot - 1]; oz = (float)f.z[slot - 1]; yaw = nav.heading + f.yaw[slot - 1];
                    } else {
                        ox = (float)(((slot + 1) / 2) * 2000 * ((slot & 1) ? 1 : -1)); oz = 0; /* no table: side by side */
                    }
                    fx = hx * ox + hz * oz;      /* rotate the slot by the group heading (same convention as yaw) */
                    fz = -hz * ox + hx * oz;
                }
                {
                    world_actor *grown = realloc(*actors, (size_t)(*count + 1) * sizeof *grown), *ac;
                    (void)c; (void)sn; (void)grown_unused;
                    if (!grown) { mech3d_free(&piece); continue; }
                    *actors = grown;
                    ac = &(*actors)[(*count)++];
                    memset(ac, 0, sizeof *ac);
                    if (getenv("MW2_DEBUG_SPAWN")) fprintf(stderr, "spawn %s table %d slot %d formation %s (found %d, %d slots) start %s %d,%d off %.0f,%.0f %s\n", m.records[i].name, g.table, slot,
                                                            star[g.table][0] ? star[g.table] : "star", f.found, f.count, start_name, nav.x, nav.z, (double)fx, (double)fz, skel);   /* tests */
                    piece.origin[0] += (int32_t)lrintf(fx);
                    piece.origin[2] += (int32_t)lrintf(fz);
                    piece.heading = yaw;
                    ac->mech = piece;
                    ac->table = g.table;
                    ac->alliance = g.table >= 0 && g.table < 64 && nstar_set ? star_all[g.table] : 1;
                    ac->friendly = (m.records[i].external && strcasecmp(m.records[i].name, "USERSTAR") == 0) || (nstar_set && ac->alliance == 0);
                    ac->leader = slot == 0;
                    {   /* slot relative to the leader's facing, for the AI to hold while moving */
                        float lh = (nav.heading + (f.found ? f.leader_yaw : 0)) * 3.14159265f / 180.0f;
                        ac->form_x = cosf(lh) * fx - sinf(lh) * fz;
                        ac->form_z = sinf(lh) * fx + cosf(lh) * fz;
                        ac->form_yaw = yaw - (nav.heading + (f.found ? f.leader_yaw : 0));
                    }
                    snprintf(ac->group, sizeof ac->group, "%s", m.records[i].name);
                    snprintf(ac->skel, sizeof ac->skel, "%s", skel);
                    ac->ai_skill = g.skill;
                    ac->obj_class = m.records[i].chunks[k].size >= 26 ? (unsigned)(m.records[i].chunks[k].data[24] | m.records[i].chunks[k].data[25] << 8) : 6u;
                    ac->tex_offset = g.table >= 0 && g.table < 64 && star_tex[g.table] >= 0 && star_tex[g.table] < 16 ? star_tex[g.table] : 0;
                    ac->ai_level = g.pilot_level;
                    {   int q; for (q = 0; q < 3; q++) ac->ai_range[q] = (float)(g.ranges[q] ? g.ranges[q] : 250) * 100.0f; }
                    snprintf(ac->loadout, sizeof ac->loadout, "%s", g.loadout);
                    snprintf(ac->name, sizeof ac->name, "%s", g.name);
                    ac->have_anim = piece.anim_id && anim_load_id(a, piece.anim_id, &ac->anim) == 0 &&
                                    ac->anim.track_count >= 3;   /* vehicles/turrets: stub sets */
                    mech3d_pose(&ac->mech, NULL, 0);
                    {   /* the unit's class: GP chunk +2 of the skeleton record (engine 0x1003e740) */
                        prj_record sr;
                        ac->unit_class = 1;
                        if (prj_read_named(a, "BWD", skel, &sr) == PRJ_OK) {
                            int nc = bwd_chunks(sr.data, sr.size, NULL, 0), q2;
                            bwd_chunk *cc = nc > 0 ? calloc((size_t)nc, sizeof *cc) : NULL;
                            if (cc) {
                                bwd_chunks(sr.data, sr.size, cc, nc);
                                for (q2 = 0; q2 < nc; q2++)
                                    if (strcmp(cc[q2].tag, "GP") == 0 && cc[q2].size >= 4) ac->unit_class = (int16_t)(cc[q2].data[2] | cc[q2].data[3] << 8);
                                free(cc);
                            }
                            prj_record_free(&sr);
                        }
                    }
                    {   /* top speed for every class from the MEK (engine 0x10041410: controller +0x88 = walk MP x 105.49,
                         * whatever the unit) - vehicles have no walk animation but drive at their MEK's speed */
                        char up[16];
                        size_t q;
                        for (q = 0; g.loadout[q] && q < sizeof up - 1; q++) up[q] = (char)toupper((unsigned char)g.loadout[q]);
                        up[q] = '\0';
                        mech3d_walk_rate(a, &ac->mech, ac->have_anim ? &ac->anim : NULL, up, &ac->speed, &ac->keys_per_s);
                    }
                }
            }
        }
    }
    bwd_mission_free(&m);
    return 0;
}

int world3d_spawns(prj_archive *a, const char *scene, mech3d *out)
{
    world_actor *ac;
    int n, i, k;
    if (world3d_actors(a, scene, &ac, &n) != 0) return -1;
    for (i = 0; i < n; i++) {
        mech3d_part *grown = realloc(out->parts, (size_t)(out->part_count + ac[i].mech.part_count) * sizeof *grown);
        if (!grown) break;
        out->parts = grown;
        for (k = 0; k < ac[i].mech.part_count; k++) out->parts[out->part_count++] = ac[i].mech.parts[k];
        free(ac[i].mech.parts);           /* models now owned by `out` */
        ac[i].mech.parts = NULL;
        ac[i].mech.part_count = 0;
    }
    free(ac);
    return n;
}

void world3d_free_actors(world_actor *actors, int count)
{
    int i;
    for (i = 0; i < count; i++) mech3d_free(&actors[i].mech);
    free(actors);
}

int world3d_compose(const world_actor *actors, int count, mech3d *out)
{
    int i, k, total = 0;
    memset(out, 0, sizeof *out);
    for (i = 0; i < count; i++) total += actors[i].mech.part_count;
    out->parts = malloc((size_t)(total ? total : 1) * sizeof *out->parts);
    if (!out->parts) return -1;
    for (i = 0; i < count; i++)
        for (k = 0; k < actors[i].mech.part_count; k++) {
            out->parts[out->part_count] = actors[i].mech.parts[k];
            out->parts[out->part_count++].tex_offset = actors[i].tex_offset;
        }
    return 0;
}

/* ---- posing ------------------------------------------------------------------ */

static void mat3_mul(float *o, const float *a, const float *b)
{
    float t[9];
    int r, c;
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++) t[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
    memcpy(o, t, sizeof t);
}

static void mat3_axis(float *m, int axis, float deg)
{
    float c = cosf(deg * 3.14159265f / 180.0f), s = sinf(deg * 3.14159265f / 180.0f);
    static const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    memcpy(m, I, sizeof I);
    if (axis == 0) { m[4] = c; m[5] = -s; m[7] = s; m[8] = c; }
    else if (axis == 1) { m[0] = c; m[2] = s; m[6] = -s; m[8] = c; }
    else { m[0] = c; m[1] = -s; m[3] = s; m[4] = c; }
}

static void solve_nodes(const mech3d *m, const anim_set *anim, float t,
                        float wrot[MECH3D_MAX_NODES][9], float wpos[MECH3D_MAX_NODES][3], int done[MECH3D_MAX_NODES])
{
    float hr[9];
    int i, k, pass;
    static const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    mat3_axis(hr, 1, m->heading);
    memset(done, 0, MECH3D_MAX_NODES * sizeof *done);
    /* resolve parents first: repeat until every node is placed (tree depth is small) */
    for (pass = 0; pass < MECH3D_MAX_NODES; pass++) {
        int progress = 0;
        for (i = 0; i < m->skel_count; i++) {
            float lrot[9], lpos[3], tmp[9];
            int pi = -1;
            if (done[i]) continue;
            if (m->skel_parent[i] >= 0 && m->skel_parent[i] != m->skel_index[i]) {
                for (k = 0; k < m->skel_count; k++) if (m->skel_index[k] == m->skel_parent[i]) pi = k;
                if (pi >= 0 && !done[pi]) continue;
            }
            memcpy(lrot, I, sizeof I);
            for (k = 0; k < 3; k++) lpos[k] = (float)m->skel_offset[i][k];
            if (m->twist_node >= 0 && m->skel_index[i] == m->twist_node && m->twist != 0) {
                mat3_axis(tmp, 1, m->twist);
                mat3_mul(lrot, lrot, tmp);
            }
            if (anim)
                for (k = 0; k < m->bind_count; k++) {
                    int tr = m->bind_track[k];
                    float v;
                    if (m->bind_node[k] != m->skel_index[i] || tr < 0 || tr >= anim->track_count) continue;
                    v = anim_value(anim, tr, t);
                    if (anim->tracks[tr].type <= 2) lpos[anim->tracks[tr].type] += v;
                    else { mat3_axis(tmp, anim->tracks[tr].type - 3, v); mat3_mul(lrot, lrot, tmp); }
                }
            if (pi < 0) {
                /* root: mech heading and world origin */
                mat3_mul(wrot[i], hr, lrot);
                for (k = 0; k < 3; k++)
                    wpos[i][k] = hr[k * 3] * lpos[0] + hr[k * 3 + 1] * lpos[1] + hr[k * 3 + 2] * lpos[2] + (float)m->origin[k];
            } else {
                mat3_mul(wrot[i], wrot[pi], lrot);
                for (k = 0; k < 3; k++)
                    wpos[i][k] = wpos[pi][k] + wrot[pi][k * 3] * lpos[0] + wrot[pi][k * 3 + 1] * lpos[1] + wrot[pi][k * 3 + 2] * lpos[2];
            }
            done[i] = 1;
            progress = 1;
        }
        if (!progress) break;
    }
}

static void solve_nodes(const mech3d *m, const anim_set *anim, float t,
                        float wrot[MECH3D_MAX_NODES][9], float wpos[MECH3D_MAX_NODES][3], int done[MECH3D_MAX_NODES]);
static void mounts_local(mech3d *m)
{
    mech3d tmp = *m;
    float wrot[MECH3D_MAX_NODES][9], wpos[MECH3D_MAX_NODES][3];
    int done[MECH3D_MAX_NODES], k, q;
    tmp.origin[0] = tmp.origin[1] = tmp.origin[2] = 0;
    tmp.heading = 0; tmp.twist = 0;
    solve_nodes(&tmp, NULL, 0, wrot, wpos, done);
    m->mount_local_ok = 0;
    for (k = 0; k < m->skel_count; k++)
        for (q = 0; q < 8; q++)
            if (m->pofo_node[q] >= 0 && m->skel_index[k] == m->pofo_node[q] && done[k]) {
                memcpy(m->mount_local[q], wpos[k], sizeof m->mount_local[q]);
                m->mount_local_ok |= 1u << q;
            }
}
int mech3d_mount_world(const mech3d *m, int slot, const float origin[3], float heading, float out[3])
{
    float c, sn;
    const float *l;
    if (slot < 0 || slot > 7) return -1;
    if (m->mount_ok & (1u << slot)) {   /* posed: animation and twist as drawn */
        out[0] = origin[0] + m->mount_pos[slot][0] - m->mount_from[0];
        out[1] = origin[1] + m->mount_pos[slot][1] - m->mount_from[1];
        out[2] = origin[2] + m->mount_pos[slot][2] - m->mount_from[2];
        return 0;
    }
    if (!(m->mount_local_ok & (1u << slot))) return -1;
    l = m->mount_local[slot];
    c = cosf(heading * 3.14159265f / 180.0f); sn = sinf(heading * 3.14159265f / 180.0f);
    /* the skeleton's yaw convention (mat3_axis about y): x' = c x + s z, z' = -s x + c z */
    out[0] = origin[0] + c * l[0] + sn * l[2];
    out[1] = origin[1] + l[1];
    out[2] = origin[2] - sn * l[0] + c * l[2];
    return 0;
}

int mech3d_mount_slot(int loc)
{
    /* POFO slot = the location index (head, RT, CT, LT, RA, LA, RL, LL): the model's slot 4 sits on the arm its OBJL group
     * 5 (RA) parts are on (+x, MADDOG 380 / parts 223), slot 1 on the right torso. The table had swapped the sides (and
     * left the legs out): right-side weapons fired from the left */
    return loc >= 0 && loc < 8 ? loc : -1;
}

void mech3d_pose(mech3d *m, const anim_set *anim, float t)
{
    float wrot[MECH3D_MAX_NODES][9], wpos[MECH3D_MAX_NODES][3];
    int done[MECH3D_MAX_NODES], i, k;
    solve_nodes(m, anim, t, wrot, wpos, done);
    m->pose_anim = anim; m->pose_t = t;
    m->eye_ok = 0;
    m->mount_ok = 0;
    m->mount_from[0] = (float)m->origin[0]; m->mount_from[1] = (float)m->origin[1]; m->mount_from[2] = (float)m->origin[2];
    for (k = 0; k < m->skel_count; k++) {
        int q;
        if (m->eye_node >= 0 && m->skel_index[k] == m->eye_node && done[k]) {
            memcpy(m->eye_pos, wpos[k], sizeof m->eye_pos);
            m->eye_ok = 1;
        }
        for (q = 0; q < 8; q++)
            if (m->pofo_node[q] >= 0 && m->skel_index[k] == m->pofo_node[q] && done[k]) {
                memcpy(m->mount_pos[q], wpos[k], sizeof m->mount_pos[q]);
                m->mount_ok |= 1u << q;
            }
    }
    for (i = 0; i < m->part_count; i++) {
        mech3d_part *p = &m->parts[i];
        for (k = 0; k < m->skel_count; k++)
            if (m->skel_index[k] == p->node && done[k]) {
                p->pos[0] = (int32_t)lrintf(wpos[k][0]);
                p->pos[1] = (int32_t)lrintf(wpos[k][1]);
                p->pos[2] = (int32_t)lrintf(wpos[k][2]);
                memcpy(p->rot, wrot[k], sizeof p->rot);
                p->has_rot = 1;
                break;
            }
    }
}

float mech3d_stride(const mech3d *m, const anim_set *anim, int first, int count)
{
    /* body travel per key = backward motion of the planted (lowest) foot */
    mech3d tmp = *m;
    float wrot[MECH3D_MAX_NODES][9], a[MECH3D_MAX_NODES][3], b[MECH3D_MAX_NODES][3], total = 0;
    int da[MECH3D_MAX_NODES], db[MECH3D_MAX_NODES], k, i, keys = 0;
    tmp.origin[0] = tmp.origin[1] = tmp.origin[2] = 0;
    tmp.heading = 0;
    for (k = first; k < first + count; k++) {
        int low = -1;
        solve_nodes(&tmp, anim, (float)k, wrot, a, da);
        solve_nodes(&tmp, anim, (float)k + 1.0f, wrot, b, db);
        for (i = 0; i < tmp.skel_count; i++) {
            int bound = 0, j;
            for (j = 0; j < tmp.bind_count; j++) if (tmp.bind_node[j] == tmp.skel_index[i]) bound = 1;
            if (!bound || !da[i] || !db[i]) continue;
            if (low < 0 || a[i][1] + b[i][1] < a[low][1] + b[low][1]) low = i;
        }
        if (low < 0) continue;
        total += -(b[low][2] - a[low][2]);   /* forward is +z */
        keys++;
    }
    return keys ? total / (float)keys : 0;
}

int mech3d_walk_rate(prj_archive *a, const mech3d *m, const anim_set *anim, const char *loadout,
                     float *speed, float *keys_per_s)
{
    mek_def md;
    int first = 0, count = 12;
    float stride;
    *speed = 0;
    *keys_per_s = 6.0f;
    if (mek_load(a, loadout, &md) != 0) return -1;   /* the archive, else a user design MEK\\<name>.MEK */
    /* full throttle = walk MP x 105.4945 / 64 cm per tick at 182 ticks/s = walk MP x 300 cm/s (engine 0x10041410 /
     * 0x1001a180). The cockpit's "NN kph" (0x10018470) prints |v| x 6.516 x 1.5 - half as much again as the true
     * speed - which is why DOS reads 160 kph for the 10-MP training mech (10 x 300 cm/s x 1.5 = 162) and 75 for the
     * Timber Wolf while it still closes the last 7 % to 80. The port had calibrated the true speed to the readout
     * (floor(1.5 x walk) x 300), a mech 1.4 - 1.5 x too fast. */
    *speed = (float)md.walk_mp * 300.0f;
    if (!anim || anim_sequence(anim, 0, &first, &count) != 0) return -1;
    stride = mech3d_stride(m, anim, first, count);
    if (stride > 1.0f && *speed > 0) *keys_per_s = *speed / stride;
    return 0;
}

int mech3d_default_loadout(prj_archive *a, const char *record, char *out, size_t outlen)
{
    /* MTAB entries hold NUL-separated short id, prefix, record name, display name */
    prj_record mt;
    char lower[16];
    size_t i, o;
    for (i = 0; record[i] && i < sizeof lower - 1; i++) lower[i] = (char)tolower((unsigned char)record[i]);
    lower[i] = '\0';
    if (prj_read_named(a, "MTAB", "MECH", &mt) != PRJ_OK) return -1;
    for (o = 2; o + i < mt.size; o++) {
        size_t p;
        if (mt.data[o - 1] != 0 || strcmp((const char *)mt.data + o, lower) != 0) continue;
        p = o - 1;
        while (p > 0 && mt.data[p - 1]) p--;              /* start of the preceding string (prefix) */
        snprintf(out, outlen, "%s00STD", (const char *)mt.data + p);
        for (i = 0; out[i]; i++) out[i] = (char)toupper((unsigned char)out[i]);
        prj_record_free(&mt);
        return 0;
    }
    prj_record_free(&mt);
    return -1;
}

void mech3d_hit_weights(const mech3d *m, float w[8])
{
    int i, k;
    for (i = 0; i < 8; i++) w[i] = 0;
    for (i = 0; i < m->part_count; i++) {
        const mech3d_part *p = &m->parts[i];
        const wtb_object *o;
        float mn[2] = {1e30f, 1e30f}, mx[2] = {-1e30f, -1e30f};
        if (p->group < 1 || p->group > 8 || p->model.object_count < 1) continue;
        o = &p->model.objects[0];
        for (k = 0; k < o->vert_count; k++) {
            float x = (float)o->verts[k].x, y = (float)o->verts[k].y;
            if (x < mn[0]) mn[0] = x;
            if (x > mx[0]) mx[0] = x;
            if (y < mn[1]) mn[1] = y;
            if (y > mx[1]) mx[1] = y;
        }
        if (o->vert_count) w[p->group - 1] += (mx[0] - mn[0]) * (mx[1] - mn[1]);
    }
}

int mech3d_head_point(const mech3d *m, float out[3])
{
    int i, k, n = 0;
    if (m->eye_ok) { memcpy(out, m->eye_pos, sizeof m->eye_pos); return 0; }
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (i = 0; i < m->part_count; i++) {
        const mech3d_part *p = &m->parts[i];
        const wtb_object *o;
        if (p->group != 1 || p->model.object_count < 1) continue;
        o = &p->model.objects[0];
        for (k = 0; k < o->vert_count; k++) {
            const wtb_vertex *v = &o->verts[k];
            float w[3], a;
            if (p->has_rot) {
                const float *R = p->rot;
                w[0] = R[0] * (float)v->x + R[1] * (float)v->y + R[2] * (float)v->z + (float)p->pos[0];
                w[1] = R[3] * (float)v->x + R[4] * (float)v->y + R[5] * (float)v->z + (float)p->pos[1];
                w[2] = R[6] * (float)v->x + R[7] * (float)v->y + R[8] * (float)v->z + (float)p->pos[2];
            } else {
                a = p->yaw * 3.14159265f / 180.0f;
                w[0] = cosf(a) * (float)v->x + sinf(a) * (float)v->z + (float)p->pos[0];
                w[1] = (float)v->y + (float)p->pos[1];
                w[2] = -sinf(a) * (float)v->x + cosf(a) * (float)v->z + (float)p->pos[2];
            }
            for (n = 0; n < 3; n++) { if (w[n] < lo[n]) lo[n] = w[n]; if (w[n] > hi[n]) hi[n] = w[n]; }
            n = 1;
        }
    }
    if (lo[0] > hi[0]) return -1;
    for (k = 0; k < 3; k++) out[k] = 0.5f * (lo[k] + hi[k]);
    return 0;
}

static void build_hull(mech3d_part *p)
{
    /* The part's convex hull (the engine tests "inside every hull plane", 0x10029db0 / 0x10010650,
     * which only works for convex volumes; the render polygons of legs and torsos are concave).
     * Brute force: a vertex triple is a hull face when every vertex lies on one side of its plane. */
    const wtb_object *o;
    int a, b, c2, k, j, n = 0, cap, nv;
    float c[3] = {0, 0, 0}, r = 0;
    if (p->hull || p->model.object_count < 1) return;
    o = &p->model.objects[0];
    nv = o->vert_count;
    if (nv < 4) return;
    for (k = 0; k < nv; k++) { c[0] += (float)o->verts[k].x; c[1] += (float)o->verts[k].y; c[2] += (float)o->verts[k].z; }
    for (j = 0; j < 3; j++) c[j] /= (float)nv;
    for (k = 0; k < nv; k++) {
        float dx = (float)o->verts[k].x - c[0], dy = (float)o->verts[k].y - c[1], dz = (float)o->verts[k].z - c[2];
        float dd = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dd > r) r = dd;
    }
    cap = 256;
    p->hull = malloc(sizeof(float) * 4 * (size_t)cap);
    if (!p->hull) return;
    for (a = 0; a < nv; a++)
        for (b = a + 1; b < nv; b++)
            for (c2 = b + 1; c2 < nv; c2++) {
                const wtb_vertex *A = &o->verts[a], *B = &o->verts[b], *C = &o->verts[c2];
                float u[3] = {(float)(B->x - A->x), (float)(B->y - A->y), (float)(B->z - A->z)};
                float v[3] = {(float)(C->x - A->x), (float)(C->y - A->y), (float)(C->z - A->z)};
                float nrm[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
                float len = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]), d;
                int pos = 0, neg = 0, dup = 0;
                if (len < 1e-3f) continue;
                for (j = 0; j < 3; j++) nrm[j] /= len;
                d = nrm[0] * (float)A->x + nrm[1] * (float)A->y + nrm[2] * (float)A->z;
                for (k = 0; k < nv && !(pos && neg); k++) {
                    float sd = nrm[0] * (float)o->verts[k].x + nrm[1] * (float)o->verts[k].y + nrm[2] * (float)o->verts[k].z - d;
                    if (sd > 0.5f) pos = 1;
                    else if (sd < -0.5f) neg = 1;
                }
                if (pos && neg) continue;
                if (!pos && !neg) continue;                 /* all coplanar: a flat part */
                if (pos) { for (j = 0; j < 3; j++) nrm[j] = -nrm[j]; d = -d; }   /* outward: every vertex behind */
                for (k = 0; k < n && !dup; k++)
                    if (fabsf(p->hull[k * 4] - nrm[0]) < 1e-3f && fabsf(p->hull[k * 4 + 1] - nrm[1]) < 1e-3f &&
                        fabsf(p->hull[k * 4 + 2] - nrm[2]) < 1e-3f && fabsf(p->hull[k * 4 + 3] - 1.0f - d) < 1.0f) dup = 1;
                if (dup) continue;
                if (n == cap) {
                    float *g = realloc(p->hull, sizeof(float) * 4 * (size_t)(cap * 2));
                    if (!g) break;
                    p->hull = g; cap *= 2;
                }
                p->hull[n * 4 + 0] = nrm[0]; p->hull[n * 4 + 1] = nrm[1]; p->hull[n * 4 + 2] = nrm[2]; p->hull[n * 4 + 3] = d + 1.0f;
                n++;
            }
    p->hull_count = n;
    memcpy(p->hull_center, c, sizeof c);
    p->hull_radius = r;
}

/* world point -> the part's model space */
static void to_local(const mech3d_part *p, const float w[3], float l[3])
{
    float q[3] = {w[0] - (float)p->pos[0], w[1] - (float)p->pos[1], w[2] - (float)p->pos[2]};
    if (p->has_rot) {
        const float *R = p->rot;   /* local = R^T q */
        l[0] = R[0] * q[0] + R[3] * q[1] + R[6] * q[2];
        l[1] = R[1] * q[0] + R[4] * q[1] + R[7] * q[2];
        l[2] = R[2] * q[0] + R[5] * q[1] + R[8] * q[2];
    } else {
        float a = p->yaw * 3.14159265f / 180.0f, c = cosf(a), sn = sinf(a);
        l[0] = c * q[0] - sn * q[2];
        l[1] = q[1];
        l[2] = sn * q[0] + c * q[2];
    }
}

static void build_sphere(mech3d_part *p)
{
    const wtb_object *o;
    float c[3] = {0, 0, 0}, r = 0;
    int k, j;
    p->sphere_ok = 1;
    if (p->model.object_count < 1) return;
    o = &p->model.objects[0];
    if (o->vert_count < 1) return;
    for (k = 0; k < o->vert_count; k++) { c[0] += (float)o->verts[k].x; c[1] += (float)o->verts[k].y; c[2] += (float)o->verts[k].z; }
    for (j = 0; j < 3; j++) c[j] /= (float)o->vert_count;
    for (k = 0; k < o->vert_count; k++) {
        float dx = (float)o->verts[k].x - c[0], dy = (float)o->verts[k].y - c[1], dz = (float)o->verts[k].z - c[2];
        float dd = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dd > r) r = dd;
    }
    memcpy(p->hull_center, c, sizeof c);
    p->hull_radius = r;
}

/* point-in-polygon on the XZ plane (engine 0x10010a90) */
static int in_poly_xz(const wtb_object *o, const wtb_poly *q, float x, float z)
{
    int i, j, in = 0;
    for (i = 0, j = q->n - 1; i < q->n; j = i++) {
        float xi = (float)o->verts[q->idx[i]].x, zi = (float)o->verts[q->idx[i]].z;
        float xj = (float)o->verts[q->idx[j]].x, zj = (float)o->verts[q->idx[j]].z;
        if (((zi > z) != (zj > z)) && (x < (xj - xi) * (z - zi) / (zj - zi) + xi)) in = !in;
    }
    return in;
}

/* Engine type 2 (0x1000d6b0), in the part's model space: the first polygon facing down (engine normal
 * = -stored normal, y < 0) whose XZ outline holds the point decides: inside when the point is above
 * that polygon's plane. Models without stored normals (DOS-only) fall back to a convex hull. */
static int part_inside(mech3d_part *p, const float l[3])
{
    const wtb_object *o = &p->model.objects[0];
    int k, any_normal = 0;
    for (k = 0; k < o->poly_count; k++) {
        const wtb_poly *q = &o->polys[k];
        float nx, ny, nz, vx, vy, vz, yp;
        if (!q->has_normal || q->n < 3) continue;
        any_normal = 1;
        nx = -q->normal[0]; ny = -q->normal[1]; nz = -q->normal[2];
        if (!(ny < 0)) continue;
        if (!in_poly_xz(o, q, l[0], l[2])) continue;
        vx = (float)o->verts[q->idx[0]].x; vy = (float)o->verts[q->idx[0]].y; vz = (float)o->verts[q->idx[0]].z;
        yp = vy - ((l[2] - vz) * nz + (l[0] - vx) * nx) / ny;
        return yp < l[1];
    }
    if (!any_normal) {   /* DOS-only model: convex hull of the vertices */
        int inside = 1;
        build_hull(p);
        if (!p->hull || p->hull_count < 4) return 0;
        for (k = 0; k < p->hull_count && inside; k++)
            if (p->hull[k * 4] * l[0] + p->hull[k * 4 + 1] * l[1] + p->hull[k * 4 + 2] * l[2] > p->hull[k * 4 + 3]) inside = 0;
        return inside;
    }
    return 0;
}

int mech3d_hull_hit(mech3d *m, const float o[3], const float d[3], float *t)
{
    int s, i;
    for (s = 1; s <= 4; s++) {
        float w[3] = {o[0] + d[0] * (float)s / 4.0f, o[1] + d[1] * (float)s / 4.0f, o[2] + d[2] * (float)s / 4.0f};
        for (i = 0; i < m->part_count; i++) {
            mech3d_part *p = &m->parts[i];
            float l[3], dx, dy, dz;
            if (p->model.object_count < 1) continue;
            if (!p->sphere_ok) build_sphere(p);
            if (p->hull_radius <= 0) continue;
            to_local(p, w, l);
            dx = l[0] - p->hull_center[0]; dy = l[1] - p->hull_center[1]; dz = l[2] - p->hull_center[2];
            if (dx * dx + dy * dy + dz * dz > p->hull_radius * p->hull_radius) continue;   /* sphere (0x10029db0) */
            if (part_inside(p, l)) {
                *t = (float)s / 4.0f;
                return p->group > 0 ? p->group : 3;
            }
        }
    }
    return 0;
}

/* ---- path tasks (TSK type 5 over PTBL paths): see mech3d.h -------------------------------------------------- */

#define WP_NODES (MAX_NODES * 4)
#define WP_TAU (0.3f * 181.0f)   /* the filters' time constant: 0.3 x 181.0 (0x10248490) ticks */
typedef struct { float R[9], t[3]; } wp_xf;
typedef struct { float last, target, cur; } wp_filt;
typedef struct {
    char     rec[11];
    int16_t  parent[WP_NODES];
    uint8_t  present[WP_NODES], dyn[WP_NODES], over[WP_NODES];
    uint8_t  anim[WP_NODES];      /* turned / moved in place by a spin (type 0) or circling (type 2) task: carries its subtree */
    wp_xf    local[WP_NODES];     /* the node's local transform: its OBJ placement (yaw, as world3d_append), or its path's */
    wp_xf    over_xf[WP_NODES];   /* a destroyed variant put in its intact object's place: this world transform */
    wp_xf    place;               /* the record's placement in the world (BLKX), recovered from a placed part */
} wp_rec;
typedef struct {
    int      type;                /* TSK type: 5 path, 0 spin, 1 colour frames, 2 circling, 4 sound */
    int      rec, node, mode, rotate, ended;   /* mode 0 loop, 1 repeat, 2 oneshot */
    uint32_t period;
    float    due, start;
    int      path;
    wp_filt  pos[3], ang[3];      /* x y z; pitch yaw roll */
    float    k[4], last;          /* type 0: degrees (pitch, yaw, roll) per k[3] ticks; type 2: yaw deg / s, cm / s, the
                                   * circling angle, on; last = the previous run (ticks) */
    uint16_t frames[16];          /* type 1: the colour words (index << 4) */
    int      nframes, *cparts, ncparts;   /* type 1: the frame count; the world parts of the node's model */
    int      range;               /* type 4: audible range, cm */
    char     snd[24];             /* type 4: the sound's SNDTABLE name */
} wp_task;
struct world3d_paths {
    wp_rec   *recs;
    int       nrec;
    bwd_path *paths;              /* the mission's path table in load order (0x101e8e20: 64 at most) */
    int       npath;
    wp_task  *tasks;
    int       ntask;
    int      *part_rec, *part_node;   /* per world part: the record / OBJ node it hangs on when carried, -1 */
    int       nparts;
};

static void wp_yaw(wp_xf *x, float deg)
{
    float c = cosf(deg * 3.14159265f / 180.0f), s = sinf(deg * 3.14159265f / 180.0f);
    x->R[0] = c;  x->R[1] = 0; x->R[2] = s;
    x->R[3] = 0;  x->R[4] = 1; x->R[5] = 0;
    x->R[6] = -s; x->R[7] = 0; x->R[8] = c;
}

/* 0x1002f070 mode 0: R = Ry(yaw) Rx(pitch) Rz(roll), column vectors */
static void wp_pyr(float *R, float pitch, float yaw, float roll)
{
    float a = pitch * 0.017453292f, b = yaw * 0.017453292f, c = roll * 0.017453292f;
    float s1 = sinf(a), c1 = cosf(a), s2 = sinf(b), c2 = cosf(b), s3 = sinf(c), c3 = cosf(c);
    R[0] = s2 * s1 * s3 + c3 * c2; R[1] = s2 * s1 * c3 - s3 * c2; R[2] = s2 * c1;
    R[3] = s3 * c1;                R[4] = c3 * c1;                R[5] = -s1;
    R[6] = s1 * s3 * c2 - c3 * s2; R[7] = s1 * c3 * c2 + s3 * s2; R[8] = c2 * c1;
}

static void wp_mul(const wp_xf *a, const wp_xf *b, wp_xf *o)
{
    wp_xf r;
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) r.R[i * 3 + j] = a->R[i * 3] * b->R[j] + a->R[i * 3 + 1] * b->R[3 + j] + a->R[i * 3 + 2] * b->R[6 + j];
        r.t[i] = a->R[i * 3] * b->t[0] + a->R[i * 3 + 1] * b->t[1] + a->R[i * 3 + 2] * b->t[2] + a->t[i];
    }
    *o = r;
}

static int wp_root(const wp_rec *r, int i) { int p = r->parent[i]; return p < 0 || p >= WP_NODES || p == i || !r->present[p]; }

static int wp_world(const wp_rec *r, int i, wp_xf *out, int depth)
{
    wp_xf p;
    if (i < 0 || i >= WP_NODES || !r->present[i] || depth > 64) return -1;
    if (r->over[i]) { *out = r->over_xf[i]; return 0; }
    /* a root's local matrix is block matrix x OBJ (0x10025450: 0x101dd738[block] -> 0x1002ef10 -> 0x1000f880); a path
     * replaces it, so a root's path is in world coordinates (CYANARE1's monorail: OBJ + BLKX = its path's point 0) */
    if (wp_root(r, i)) { if (r->dyn[i]) *out = r->local[i]; else wp_mul(&r->place, &r->local[i], out); return 0; }
    if (wp_world(r, r->parent[i], &p, depth + 1) != 0) return -1;
    wp_mul(&p, &r->local[i], out);
    return 0;
}

static int wp_under(const wp_rec *r, int i, int anc)   /* i is anc or below it */
{
    int d;
    for (d = 0; d < 64 && i >= 0 && i < WP_NODES && r->present[i]; d++) {
        if (i == anc) return 1;
        if (wp_root(r, i)) break;
        i = r->parent[i];
    }
    return 0;
}

static int wp_carried(const wp_rec *r, int i)   /* a path (or spin / circling) node at or above i */
{
    int d;
    for (d = 0; d < 64 && i >= 0 && i < WP_NODES && r->present[i]; d++) {
        if (r->dyn[i] || r->anim[i]) return 1;
        if (wp_root(r, i)) break;
        i = r->parent[i];
    }
    return 0;
}

static void wp_reset(wp_task *t, const bwd_path *ph, float now)
{
    int k;
    for (k = 0; k < 3; k++) {
        t->pos[k].last = t->ang[k].last = now;
        t->pos[k].target = t->pos[k].cur = ph->pt[0].p[k];
        t->ang[k].target = t->ang[k].cur = 0;
    }
}

/* 0x1003b7e0 / 0x1003b900 */
static float wp_filter(wp_filt *f, float now, int angle)
{
    float d = f->target - f->cur, step;
    if (angle) { d = fmodf(d, 360.0f); if (d > 180.0f) d -= 360.0f; else if (d < -180.0f) d += 360.0f; }
    else if (fabsf(d) < 1e-7f) { f->cur = f->target; f->last = now; return f->cur; }
    step = (now - f->last) * d / WP_TAU;
    if ((d < 0 && step <= d) || (d > 1e-7f && d <= step)) f->cur = f->target;
    else f->cur += step;
    if (angle) f->cur = fmodf(f->cur, 360.0f);
    f->last = now;
    return f->cur;
}

/* aim the angle filter at a (its value moved by whole turns to within 180 degrees of it, 0x1000f40b) */
static void wp_aim(wp_filt *f, float a)
{
    float d = a - f->cur;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    f->target = a;
    f->cur = a - d;
}

/* 0x10030120: the heading of (dx, dz), degrees from +z towards +x; 45 when both are (near) 0 */
static float wp_heading(float dx, float dz)
{
    if (fabsf(dx) < 1e-7f && fabsf(dz) < 1e-7f) return 45.0f;
    return atan2f(dx, dz) * 57.29578f;
}

/* one run of the task (0x1000ee10 mode 1); 0 = the task ends */
static int wp_run(world3d_paths *P, wp_task *t, float now)
{
    const bwd_path *ph = &P->paths[t->path];
    const bwd_path_point *c, *nx;
    wp_rec *r = &P->recs[t->rec];
    float el = now - t->start, acc = 0, before = 0, f, v[3], a[3];
    int n = ph->count, i, k;
    if (n <= 1) return 0;
    for (i = 0; i < n; i++) {
        before = acc;
        acc += (float)ph->pt[i].ticks;
        if (acc >= el) break;
    }
    if (i >= n - 1) {
        if (t->mode == 2) return 0;                         /* oneshot: done at the last point */
        if (t->mode == 1) {                                  /* repeat: back to point 0 */
            t->start = now;
            wp_reset(t, ph, now);
            i = 0; el = 0; before = 0;
        } else if (i == n) {                                 /* loop: past the closing segment, wrap */
            el -= acc;
            t->start = now - el;
            i = 0; before = 0;
        }
    }
    c = &ph->pt[i];
    nx = n - i == 1 ? &ph->pt[0] : &ph->pt[i + 1];
    f = c->ticks > 0 ? (el - before) / (float)c->ticks : 1.0f;
    for (k = 0; k < 3; k++) { t->pos[k].target = c->p[k] + (nx->p[k] - c->p[k]) * f; v[k] = wp_filter(&t->pos[k], now, 0); }
    a[1] = c->yaw;
    a[0] = c->pitch;
    a[2] = c->roll;
    if (t->rotate) {
        float s = (nx->p[1] - c->p[1]) / 65536.0f;          /* asin of dy x 2^-16 (0x102471fc), as the engine */
        a[1] += wp_heading(nx->p[0] - c->p[0], nx->p[2] - c->p[2]);
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        a[0] -= asinf(s) * 57.29578f;
    }
    for (k = 0; k < 3; k++) { wp_aim(&t->ang[k], a[k]); a[k] = wp_filter(&t->ang[k], now, 1); }
    memcpy(r->local[t->node].t, v, sizeof v);
    wp_pyr(r->local[t->node].R, a[0], a[1], a[2]);
    return 1;
}

/* the node's local rotation turned in its own frame: R = R x Rpyr (0x1000f9d0 -> 0x1000f940: 0x1002ed60 local x M), then
 * re-orthonormalised (the engine does that every 64-191 turns, 0x1002f500) */
static void wp_turn(wp_xf *x, float pitch, float yaw, float roll)
{
    float M[9], o[9], l;
    int i, j;
    wp_pyr(M, pitch, yaw, roll);
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) o[i * 3 + j] = x->R[i * 3] * M[j] + x->R[i * 3 + 1] * M[3 + j] + x->R[i * 3 + 2] * M[6 + j];
    /* Gram-Schmidt on the columns */
    l = sqrtf(o[0] * o[0] + o[3] * o[3] + o[6] * o[6]);
    if (l > 1e-9f) { o[0] /= l; o[3] /= l; o[6] /= l; }
    l = o[0] * o[1] + o[3] * o[4] + o[6] * o[7];
    o[1] -= l * o[0]; o[4] -= l * o[3]; o[7] -= l * o[6];
    l = sqrtf(o[1] * o[1] + o[4] * o[4] + o[7] * o[7]);
    if (l > 1e-9f) { o[1] /= l; o[4] /= l; o[7] /= l; }
    o[2] = o[3] * o[7] - o[6] * o[4]; o[5] = o[6] * o[1] - o[0] * o[7]; o[8] = o[0] * o[4] - o[3] * o[1];
    memcpy(x->R, o, sizeof o);
}

/* one run of a type 0-4 task; 0 = the task ends */
static int wp_run_other(world3d_paths *P, mech3d *world, wp_task *t, float now)
{
    wp_rec *r = &P->recs[t->rec];
    wp_xf *x = &r->local[t->node];
    if (t->type == 0) {   /* spin (0x1000e880): each angle turns by its rate x elapsed / (seconds x 181) */
        float q = (now - t->last) / t->k[3];
        t->last = now;
        wp_turn(x, t->k[0] * q, t->k[1] * q, t->k[2] * q);
        return 1;
    }
    if (t->type == 2) {   /* circling (0x1000ea50): a step along the heading + 90, then a yaw turn */
        float dts = (now - t->last) * (1.0f / 181.0f), d, a = t->k[2] * 0.017453292f, dy;   /* 0x102471e4: 1 / 181 */
        if (t->k[3] == 0) return 0;
        t->last = now;
        d = t->k[1] * dts;
        x->t[0] += cosf(a) * d;   /* 0x1000f850: (sin(a + 90), 0, -sin(a)) x d added to the local translation */
        x->t[2] -= sinf(a) * d;
        dy = t->k[0] * dts;
        wp_turn(x, 0.0f, dy, 0.0f);
        t->k[2] = fmodf(t->k[2] + dy, 360.0f);
        return 1;
    }
    if (t->type == 1) {   /* colour frames (0x1000e6e0): polygon i takes frame (i + now / period) mod n */
        int per = t->period ? (int)t->period : 1, ph = ((int)now / per) % t->nframes, j, o, i;
        for (j = 0; j < t->ncparts; j++) {
            mech3d_part *pp;
            if (t->cparts[j] >= world->part_count) continue;
            pp = &world->parts[t->cparts[j]];
            for (o = 0; o < pp->model.object_count; o++)
                for (i = 0; i < pp->model.objects[o].poly_count; i++) pp->model.objects[o].polys[i].color = t->frames[(i + ph) % t->nframes];
        }
        return 1;
    }
    return 1;             /* type 4 (sound): played by the caller (world3d_paths_sounds) */
}

static void wp_place_parts(world3d_paths *P, mech3d *world, int only_rec, int only_under)
{
    int q;
    for (q = 0; q < P->nparts && q < world->part_count; q++) {
        mech3d_part *pp = &world->parts[q];
        wp_xf w;
        int ri = P->part_rec[q], node = P->part_node[q];
        if (only_rec >= 0) {   /* a destroyed variant's subtree */
            if (strcmp(pp->rec, P->recs[only_rec].rec) != 0 || !wp_under(&P->recs[only_rec], pp->obj_index, only_under)) continue;
            ri = only_rec; node = pp->obj_index;
        } else if (ri < 0) continue;
        if (wp_world(&P->recs[ri], node, &w, 0) != 0) continue;
        pp->pos[0] = (int32_t)lrintf(w.t[0]); pp->pos[1] = (int32_t)lrintf(w.t[1]); pp->pos[2] = (int32_t)lrintf(w.t[2]);
        memcpy(pp->rot, w.R, sizeof w.R);
        pp->has_rot = 1;
    }
}

world3d_paths *world3d_paths_load(prj_archive *a, const char *scene, mech3d *world)
{
    bwd_mission m;
    world3d_paths *P;
    int i, k, q;
    if (!a || !world || bwd_mission_load(a, scene, &m) != 0) return NULL;
    P = calloc(1, sizeof *P);
    if (!P || !(P->paths = calloc(64, sizeof *P->paths))) { free(P); bwd_mission_free(&m); return NULL; }
    for (i = 0; i < m.record_count; i++) {
        const bwd_record *br = &m.records[i];
        int ri = -1;
        if (br->external) continue;
        for (k = 0; k < br->chunk_count; k++) {
            const bwd_chunk *c = &br->chunks[k];
            bwd_tsk tk;
            char mode[96] = "", rot[96] = "", name[96] = "";
            int pi;
            wp_task *t;
            if (strcmp(c->tag, "PTBL") == 0) {
                if (P->npath < 64 && bwd_ptbl_decode(c, &P->paths[P->npath]) == 0) P->npath++;
                continue;
            }
            if (bwd_tsk_decode(c, &tk) != 0 || tk.type < 0 || tk.type == 3 || tk.type > 5 || tk.node < 0 || tk.node >= WP_NODES) continue;
            pi = -1;
            if (tk.type == 5) {
                if (sscanf(tk.params, "%95[^,],%95[^,],%95[^,]", mode, rot, name) < 3) continue;
                for (pi = 0; pi < P->npath && strcmp(P->paths[pi].name, name) != 0; pi++) {}
                if (pi == P->npath) continue;                /* no such path (yet): the task ends at its first run */
            }
            if (ri < 0) {   /* the record's nodes */
                wp_rec *nr = realloc(P->recs, (size_t)(P->nrec + 1) * sizeof *nr);
                int j;
                if (!nr) continue;
                P->recs = nr;
                ri = P->nrec++;
                memset(&P->recs[ri], 0, sizeof P->recs[ri]);
                snprintf(P->recs[ri].rec, sizeof P->recs[ri].rec, "%.10s", br->name);
                for (j = 0; j < br->chunk_count; j++) {
                    bwd_obj o;
                    wp_rec *r = &P->recs[ri];
                    if (bwd_obj_decode(&br->chunks[j], &o) != 0 || o.index < 0 || o.index >= WP_NODES) continue;
                    r->present[o.index] = 1;
                    r->parent[o.index] = o.parent;
                    wp_yaw(&r->local[o.index], (float)o.rotation[1] / 65536.0f);
                    r->local[o.index].t[0] = (float)o.offset[0]; r->local[o.index].t[1] = (float)o.offset[1]; r->local[o.index].t[2] = (float)o.offset[2];
                }
            }
            if (!P->recs[ri].present[tk.node]) continue;
            t = realloc(P->tasks, (size_t)(P->ntask + 1) * sizeof *t);
            if (!t) continue;
            P->tasks = t;
            t = &P->tasks[P->ntask++];
            memset(t, 0, sizeof *t);
            t->type = tk.type; t->rec = ri; t->node = tk.node; t->path = pi; t->period = tk.period;
            if (tk.type != 5) {   /* created at load (0x10008280 mode 0): the start time 0 */
                int ok = 1;
                if (tk.type == 0) {        /* "%f,%f,%f,%f": pitch, yaw, roll degrees per <4th> seconds (x 181 ticks) */
                    ok = sscanf(tk.params, "%f,%f,%f,%f", &t->k[0], &t->k[1], &t->k[2], &t->k[3]) == 4;
                    t->k[3] *= 181.0f;     /* 0x102471c4 */
                    if (fabsf(t->k[3]) < 1e-7f) t->k[3] = 181.0f;
                    if (ok) P->recs[ri].anim[tk.node] = 1;
                } else if (tk.type == 2) { /* "%f,%f,%d,%d": radius, seconds per turn, on (the 4th unused) */
                    float rad = 0, secs = 0;
                    int on = 0;
                    ok = sscanf(tk.params, "%f,%f,%d", &rad, &secs, &on) >= 3 && secs != 0 && on;
                    t->k[0] = 360.0f / secs;                       /* yaw degrees per second (0x102471d0: 360.0) */
                    t->k[1] = rad * 6.2831853f / secs;             /* cm per second (0x102471d8: 2 pi) */
                    t->k[3] = (float)on;
                    if (ok) P->recs[ri].anim[tk.node] = 1;
                } else if (tk.type == 1) { /* up to 16 comma-separated palette indices, stored << 4 */
                    char buf[128], *s, *e;
                    snprintf(buf, sizeof buf, "%s", tk.params);
                    for (s = strtok_r(buf, ",", &e); s && t->nframes < 16; s = strtok_r(NULL, ",", &e))
                        t->frames[t->nframes++] = (uint16_t)(strtol(s, NULL, 0) << 4);
                    ok = t->nframes > 0;
                } else if (tk.type == 4) { /* "%d,%[^,],%d": range (m x 100), the sound's name, on (absent: on, ASSUMED) */
                    int on = 1, n = sscanf(tk.params, "%d,%23[^,],%d", &t->range, t->snd, &on);
                    t->range *= 100;
                    ok = n >= 2 && on;
                }
                if (!ok) { P->ntask--; continue; }
                continue;
            }
            t->mode = strcmp(mode, "loop") == 0 ? 0 : strcmp(mode, "repeat") == 0 ? 1 : 2;
            t->rotate = strcmp(rot, "rotate") == 0;
            wp_reset(t, &P->paths[pi], 0.0f);
            P->recs[ri].dyn[tk.node] = 1;
        }
    }
    bwd_mission_free(&m);
    if (P->ntask == 0) { world3d_paths_free(P); return NULL; }
    P->nparts = world->part_count;
    P->part_rec = malloc((size_t)(P->nparts > 0 ? P->nparts : 1) * sizeof *P->part_rec);
    P->part_node = malloc((size_t)(P->nparts > 0 ? P->nparts : 1) * sizeof *P->part_node);
    if (!P->part_rec || !P->part_node) { world3d_paths_free(P); return NULL; }
    for (i = 0; i < P->nrec; i++) {   /* the placement: from a part of the record placed by world3d_append (+ BLKX) */
        wp_rec *r = &P->recs[i];
        wp_yaw(&r->place, 0);
        for (q = 0; q < world->part_count; q++) {
            const mech3d_part *pp = &world->parts[q];
            wp_xf rest;
            float yaw = 0;
            int node = pp->obj_index, d;
            if (pp->has_rot || strcmp(pp->rec, r->rec) != 0 || node < 0 || node >= WP_NODES || !r->present[node]) continue;
            memset(rest.t, 0, sizeof rest.t);
            for (d = 0; d < 64; d++) {   /* the rest chain (offsets turned by the parents' yaw), as wpos() */
                yaw += atan2f(r->local[node].R[2], r->local[node].R[0]) * 57.29578f;
                if (d == 0) rest = r->local[node];
                else wp_mul(&r->local[node], &rest, &rest);
                if (wp_root(r, node)) break;
                node = r->parent[node];
            }
            wp_yaw(&r->place, pp->yaw - yaw);   /* part = place x rest: yaw adds, the offset turns */
            r->place.t[0] = (float)pp->pos[0] - (r->place.R[0] * rest.t[0] + r->place.R[2] * rest.t[2]);
            r->place.t[1] = (float)pp->pos[1] - rest.t[1];
            r->place.t[2] = (float)pp->pos[2] - (r->place.R[6] * rest.t[0] + r->place.R[8] * rest.t[2]);
            break;
        }
    }
    for (q = 0; q < world->part_count; q++) {
        mech3d_part *pp = &world->parts[q];
        P->part_rec[q] = P->part_node[q] = -1;
        for (i = 0; i < P->nrec; i++)
            if (strcmp(pp->rec, P->recs[i].rec) == 0 && pp->obj_index >= 0 && pp->obj_index < WP_NODES && wp_carried(&P->recs[i], pp->obj_index)) {
                P->part_rec[q] = i; P->part_node[q] = pp->obj_index;
                pp->moving = 1;
                break;
            }
    }
    for (k = 0; k < P->ntask; k++) {   /* colour frames: the node's own model (its render object, 0x10029970) */
        wp_task *t = &P->tasks[k];
        if (t->type != 1) continue;
        for (q = 0; q < world->part_count; q++) {
            mech3d_part *pp = &world->parts[q];
            int *g;
            if (pp->obj_index != t->node || strcmp(pp->rec, P->recs[t->rec].rec) != 0) continue;
            if (!(g = realloc(t->cparts, (size_t)(t->ncparts + 1) * sizeof *g))) break;
            t->cparts = g;
            t->cparts[t->ncparts++] = q;
            pp->moving = 1;   /* redrawn every frame (the actor layer); its collision follows the moving parts' rules */
        }
    }
    world3d_paths_step(P, world, 0.0f);
    return P;
}

int world3d_paths_tasks(const world3d_paths *p) { return p ? p->ntask : 0; }

int world3d_paths_step(world3d_paths *P, mech3d *world, float now)
{
    int i, q, ended = 0, settled = 0;
    if (!P || !world) return 0;
    for (i = 0; i < P->ntask; i++) {
        wp_task *t = &P->tasks[i];
        if (t->ended) { ended |= t->ended == 1; t->ended = 2; continue; }   /* 1: ended since the last step */
        if (now < t->due) continue;
        t->due = now + (float)t->period;                    /* 0x100083d0: next run at now + period */
        if (!(t->type == 5 ? wp_run(P, t, now) : wp_run_other(P, world, t, now))) { t->ended = 2; ended = 1; }
    }
    wp_place_parts(P, world, -1, -1);
    for (q = 0; ended && q < P->nparts && q < world->part_count; q++) {   /* parts no running task carries: fixed again */
        int ri = P->part_rec[q], live = 0;
        if (ri < 0) continue;
        for (i = 0; i < P->ntask && !live; i++)
            if (P->tasks[i].rec == ri && !P->tasks[i].ended && (P->tasks[i].type == 5 || P->tasks[i].type == 0 || P->tasks[i].type == 2)
                && wp_under(&P->recs[ri], P->part_node[q], P->tasks[i].node)) live = 1;
        if (live) continue;
        P->part_rec[q] = P->part_node[q] = -1;
        for (i = 0; i < P->ntask && !live; i++) {   /* still colour-animated: stays on the per-frame layer */
            int j;
            if (P->tasks[i].type != 1 || P->tasks[i].ended) continue;
            for (j = 0; j < P->tasks[i].ncparts; j++) if (P->tasks[i].cparts[j] == q) live = 1;
        }
        if (live) continue;
        world->parts[q].moving = 0;
        settled++;
    }
    return settled;
}

int world3d_paths_destroyed(world3d_paths *P, mech3d *world, int intact, int destroyed)
{
    wp_rec *r;
    wp_xf w;
    int ri, node, i, dn;
    if (!P || !world || intact < 0 || intact >= P->nparts || intact >= world->part_count) return 0;
    if ((ri = P->part_rec[intact]) < 0) {   /* not carried: its own tasks (colour frames, sounds) and those below it end */
        for (ri = 0; ri < P->nrec && strcmp(P->recs[ri].rec, world->parts[intact].rec) != 0; ri++) {}
        node = world->parts[intact].obj_index;
        if (ri == P->nrec || node < 0 || node >= WP_NODES) return 0;
        for (i = 0; i < P->ntask; i++)
            if (P->tasks[i].rec == ri && !P->tasks[i].ended && wp_under(&P->recs[ri], P->tasks[i].node, node)) P->tasks[i].ended = 1;
        return 0;
    }
    r = &P->recs[ri];
    node = P->part_node[intact];
    for (i = 0; i < P->ntask; i++)
        if (P->tasks[i].rec == ri && !P->tasks[i].ended && wp_under(r, P->tasks[i].node, node)) P->tasks[i].ended = 1;
    if (destroyed < 0 || destroyed >= world->part_count || strcmp(world->parts[destroyed].rec, r->rec) != 0) return 0;
    dn = world->parts[destroyed].obj_index;
    if (dn < 0 || dn >= WP_NODES || !r->present[dn] || wp_world(r, node, &w, 0) != 0) return 0;
    r->over[dn] = 1;
    r->over_xf[dn] = w;
    wp_place_parts(P, world, ri, dn);
    return 1;
}

void world3d_paths_free(world3d_paths *P)
{
    int i;
    if (!P) return;
    for (i = 0; i < P->ntask; i++) free(P->tasks[i].cparts);
    free(P->recs); free(P->paths); free(P->tasks); free(P->part_rec); free(P->part_node);
    free(P);
}

int world3d_paths_sounds(const world3d_paths *P, world3d_sound *out, int max)
{
    int i, n = 0;
    if (!P) return 0;
    for (i = 0; i < P->ntask && n < max; i++) {
        const wp_task *t = &P->tasks[i];
        wp_xf w;
        if (t->type != 4 || t->ended || wp_world(&P->recs[t->rec], t->node, &w, 0) != 0) continue;
        memcpy(out[n].pos, w.t, sizeof w.t);
        out[n].range = t->range;
        out[n].task = i;
        snprintf(out[n].name, sizeof out[n].name, "%s", t->snd);
        n++;
    }
    return n;
}

unsigned mech3d_fly_mask(const int loc_gone[8])
{
    unsigned m = 0;
    int l;
    for (l = 0; l < 8; l++) if (loc_gone[l]) m |= 1u << (l + 1);
    return m & MECH3D_FLY_LOCS;
}

int mech3d_fly_loc(const mech3d *m, int q, unsigned mask)
{
    int node, guard, k, j;
    if (!mask || q < 0 || q >= m->part_count) return 0;
    if (m->parts[q].group >= 1 && m->parts[q].group <= 8 && (mask >> m->parts[q].group & 1)) return m->parts[q].group;
    node = m->parts[q].node;
    for (guard = 0; node >= 0 && guard < MECH3D_MAX_NODES; guard++) {
        int par = -1;
        for (j = 0; j < m->part_count; j++)   /* the location of the part on this node */
            if (m->parts[j].node == node && m->parts[j].group >= 1 && m->parts[j].group <= 8) {
                if (mask >> m->parts[j].group & 1) return m->parts[j].group;
                break;
            }
        for (k = 0; k < m->skel_count; k++) if (m->skel_index[k] == node) { par = m->skel_parent[k]; break; }
        if (par == node) break;
        node = par;
    }
    return 0;
}
