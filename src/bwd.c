/* bwd.c - see bwd.h. */
#include "bwd.h"
#include "datapath.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void copy_str(char *dst, size_t dstlen, const uint8_t *src, size_t srclen)
{
    size_t i;
    for (i = 0; i < srclen && i + 1 < dstlen && src[i]; i++) dst[i] = (char)src[i];
    dst[i] = '\0';
    while (i > 0 && dst[i - 1] == ' ') dst[--i] = '\0'; /* names are often space-padded */
}

int bwd_chunks(const uint8_t *rec, size_t len, bwd_chunk *chunks, int max)
{
    size_t off = 12;
    int n = 0;
    if (len < 12 || memcmp(rec, "BWD\0", 4) != 0 || rd32(rec + 4) != len) return -1;
    while (off < len) {
        uint32_t size;
        int k;
        if (len - off < 8) return -1;
        size = rd32(rec + off + 4);
        if (size < 8 || size > len - off) return -1;
        for (k = 0; k < 4; k++) {
            uint8_t c = rec[off + (size_t)k];
            if (c != 0 && (c < 32 || c > 126)) return -1;
        }
        if (chunks && n < max) {
            memcpy(chunks[n].tag, rec + off, 4);
            chunks[n].tag[4] = '\0';
            chunks[n].data = rec + off + 8;
            chunks[n].size = size - 8;
        }
        n++;
        off += size;
    }
    return n;
}

int bwd_incl(const bwd_chunk *c, uint16_t *id, char name[11])
{
    if (strcmp(c->tag, "INCL") != 0 || c->size < 3) return -1;
    *id = rd16(c->data);
    copy_str(name, 11, c->data + 2, c->size - 2);
    return name[0] ? 0 : -1;
}

int bwd_navp(const bwd_chunk *c, bwd_navpoint *out)
{
    if (strcmp(c->tag, "NAVP") != 0 || c->size < 28) return -1;
    out->x = (int32_t)rd32(c->data);
    out->y = (int32_t)rd32(c->data + 4);
    out->z = (int32_t)rd32(c->data + 8);
    out->heading = (float)(int32_t)rd32(c->data + 12) / 65536.0f;
    out->radius = (int)(rd32(c->data + 24) & 0xFFFF);
    out->type = (int)(rd32(c->data + 24) >> 24);
    out->flags = (int)(c->data[18] | (c->data[19] << 8));
    out->shown = (int16_t)(c->data[16] | (c->data[17] << 8));
    out->team = (int16_t)(c->data[22] | (c->data[23] << 8));
    copy_str(out->name, sizeof out->name, c->data + 28, c->size - 28);
    return 0;
}

int bwd_ptbl_decode(const bwd_chunk *c, bwd_path *out)
{
    size_t n, i;
    if (strcmp(c->tag, "PTBL") != 0 || c->size < 64) return -1;
    n = (c->size - 64) / 28;   /* 0x10042b00: (size - 0x48) / 0x1c, accepted below 0x41 */
    if (n > BWD_PATH_MAX_POINTS) return -1;
    memset(out, 0, sizeof *out);
    copy_str(out->name, sizeof out->name, c->data, 64);
    out->count = (int)n;
    for (i = 0; i < n; i++) {
        const uint8_t *p = c->data + 64 + i * 28;
        bwd_path_point *q = &out->pt[i];
        q->p[0] = (float)(int32_t)rd32(p); q->p[1] = (float)(int32_t)rd32(p + 4); q->p[2] = (float)(int32_t)rd32(p + 8);
        q->pitch = (float)(int32_t)rd32(p + 12); q->yaw = (float)(int32_t)rd32(p + 16); q->roll = (float)(int32_t)rd32(p + 20);
        q->ticks = (int32_t)rd32(p + 24);
    }
    return 0;
}

int bwd_tsk_decode(const bwd_chunk *c, bwd_tsk *out)
{
    char text[128];
    const char *semi;
    if (strcmp(c->tag, "TSK") != 0 || c->size < 6) return -1;
    memset(out, 0, sizeof *out);
    out->type = (int16_t)rd16(c->data);
    out->period = rd32(c->data + 2);
    copy_str(text, sizeof text, c->data + 6, c->size - 6);
    semi = strchr(text, ';');
    out->node = semi ? atoi(text) : -1;
    snprintf(out->params, sizeof out->params, "%s", semi ? semi + 1 : text);
    return 0;
}

int bwd_musi(const bwd_chunk *c, int *cd_track)
{
    char name[16];
    if (strcmp(c->tag, "MUSI") != 0 || c->size < 3) return -1;
    copy_str(name, sizeof name, c->data + 2, c->size - 2);
    if (strncasecmp(name, "track", 5) != 0 || !isdigit((unsigned char)name[5])) return -1;
    *cd_track = atoi(name + 5);
    return 0;
}

int bwd_obj_decode(const bwd_chunk *c, bwd_obj *out)
{
    int k;
    if (strcmp(c->tag, "OBJ") != 0 || c->size < 52) return -1;
    out->index = (int16_t)rd16(c->data);
    out->parent = (int16_t)rd16(c->data + 2);
    out->coll = (int16_t)rd16(c->data + 4);
    for (k = 0; k < 3; k++) {
        out->scale[k] = (int32_t)rd32(c->data + 6 + k * 4);
        out->rotation[k] = (int32_t)rd32(c->data + 18 + k * 4);
        out->offset[k] = (int32_t)rd32(c->data + 30 + k * 4);
    }
    out->model = rd16(c->data + 48);
    out->objtype = rd16(c->data + 44);
    return 0;
}

int bwd_gps_decode(const bwd_chunk *c, bwd_gps *out)
{
    size_t o = 0x1C;
    char *fields[4];
    size_t lens[4];
    int f;
    memset(out, 0, sizeof *out);
    if (strcmp(c->tag, "GPS") != 0 || c->size < 0x1C) return -1;
    out->mek_id = rd16(c->data);
    out->skeleton_id = rd16(c->data + 2);
    out->table = c->data[4];
    out->leader = c->data[5];
    out->is_player = c->data[6] == 0;
    out->skill = c->data[8];
    out->ranges[0] = rd16(c->data + 10);
    out->ranges[1] = rd16(c->data + 12);
    out->ranges[2] = rd16(c->data + 14);
    out->pilot_level = c->size > 16 ? c->data[16] : 0;
    if (out->pilot_level < 1 || out->pilot_level > 4) out->pilot_level = 1;
    fields[0] = out->skeleton; lens[0] = sizeof out->skeleton;
    fields[1] = out->loadout;  lens[1] = sizeof out->loadout;
    fields[2] = out->name;     lens[2] = sizeof out->name;
    fields[3] = out->pilot;    lens[3] = sizeof out->pilot;
    /* NUL-separated strings, possibly padded: take the next non-empty runs */
    for (f = 0; f < 4 && o < c->size; f++) {
        size_t start, n;
        while (o < c->size && c->data[o] == 0) o++;
        start = o;
        while (o < c->size && c->data[o] >= 32 && c->data[o] < 127) o++;
        n = o - start;
        if (n == 0) break;
        if (n >= lens[f]) n = lens[f] - 1;
        memcpy(fields[f], c->data + start, n);
        fields[f][n] = '\0';
    }
    return 0;
}

int bwd_texmap_decode(const uint8_t *rec, size_t len, bwd_texmap *out)
{
    bwd_chunk *c;
    int n = bwd_chunks(rec, len, NULL, 0), k, have = 0;
    uint16_t id = 0;
    char name[13] = "";

    memset(out, 0, sizeof *out);
    if (n < 0 || !(c = calloc((size_t)(n ? n : 1), sizeof *c))) return -1;
    bwd_chunks(rec, len, c, n);
    for (k = 0; k < n; k++) {
        if (strcmp(c[k].tag, "BMPJ") == 0 && c[k].size >= 3) {
            id = rd16(c[k].data);
            copy_str(name, sizeof name, c[k].data + 2, c[k].size - 2);
            have = 1;
        } else if (strcmp(c[k].tag, "BMID") == 0 && c[k].size >= 2 && have) {
            int slot = c[k].data[0];
            have = 0;
            if (c[k].data[1] == 0) {   /* bank 0: effect animation frames (BSEC/BMEN) and scrub bitmaps, not surfaces */
                out->b0_present[slot] = 1;
                memcpy(out->b0_name[slot], name, sizeof name);
                continue;
            }
            if (c[k].data[1] != 1) continue;
            if (!out->present[slot]) out->count++;
            out->present[slot] = 1;
            out->cel_id[slot] = id;
            memcpy(out->name[slot], name, sizeof name);
        }
    }
    free(c);
    return 0;
}

/* ---- mission loading ------------------------------------------------------ */

static int find_record(const bwd_mission *m, const char *name)
{
    int i;
    for (i = 0; i < m->record_count; i++)
        if (strcasecmp(m->records[i].name, name) == 0) return i;
    return -1;
}

static int add_record(bwd_mission *m, const char *name, int external, uint8_t *data, size_t size)
{
    bwd_record *r;
    int n = bwd_chunks(data, size, NULL, 0);
    if (n < 0) { free(data); return -1; }
    r = realloc(m->records, (size_t)(m->record_count + 1) * sizeof *r);
    if (!r) { free(data); return -1; }
    m->records = r;
    r = &m->records[m->record_count];
    memset(r, 0, sizeof *r);
    snprintf(r->name, sizeof r->name, "%s", name);
    r->external = external;
    r->data = data;
    r->size = size;
    r->chunks = calloc((size_t)(n ? n : 1), sizeof *r->chunks);
    if (!r->chunks) { free(data); return -1; }
    r->chunk_count = bwd_chunks(data, size, r->chunks, n);
    m->record_count++;
    return 0;
}

static void note_missing(bwd_mission *m, const char *name)
{
    int i;
    for (i = 0; i < m->missing_count; i++)
        if (strcasecmp(m->missing[i], name) == 0) return;
    if (m->missing_count < 16) snprintf(m->missing[m->missing_count++], 11, "%s", name);
}

static int load_one(prj_archive *a, int type, bwd_mission *m, uint16_t id, const char *name)
{
    char upper[17];
    size_t i;
    for (i = 0; name[i] && i < 16; i++) upper[i] = (char)toupper((unsigned char)name[i]);
    upper[i] = '\0';
    if (find_record(m, upper) >= 0) return 0;

    if (id == BWD_EXTERNAL_ID) {
        char file[32];
        unsigned char *data;
        size_t len;
        snprintf(file, sizeof file, "%s.BWD", upper);
        if (dp_read_file(file, &data, &len) != 0) { note_missing(m, upper); return 0; }
        return add_record(m, upper, 1, data, len);
    } else {
        prj_record r;
        if (prj_read(a, type, id, &r) != PRJ_OK) { note_missing(m, upper); return 0; }
        if (strcasecmp(r.name, upper) != 0) { prj_record_free(&r); return -1; } /* id/name disagree */
        return add_record(m, upper, 0, r.data, r.size);
    }
}

int bwd_mission_load(prj_archive *a, const char *scene, bwd_mission *m)
{
    int type = prj_find_type(a, "BWD"), id, i, k;

    memset(m, 0, sizeof *m);
    if (type < 0) return -1;
    dp_add_roots_from_env();   /* external includes (UserStar, en01star...) come from the install dir */
    id = prj_find_id(a, type, scene);
    if (id < 0) return -1;
    if (load_one(a, type, m, (uint16_t)id, scene) != 0) goto fail;

    /* breadth-first over includes; records array grows as we go */
    for (i = 0; i < m->record_count; i++) {
        for (k = 0; k < m->records[i].chunk_count; k++) {
            const bwd_chunk *c = &m->records[i].chunks[k];
            uint16_t inc_id;
            char inc_name[11];
            if (strcmp(c->tag, "INCL") == 0) {
                if (bwd_incl(c, &inc_id, inc_name) != 0) goto fail;
                if (load_one(a, type, m, inc_id, inc_name) != 0) goto fail;
                c = &m->records[i].chunks[k]; /* records may have moved */
            } else if (strcmp(c->tag, "MUSI") == 0 && !m->music_track) {
                bwd_musi(c, &m->music_track);
            } else if (strcmp(c->tag, "NAVP") == 0) {
                bwd_navpoint *nv = realloc(m->navs, (size_t)(m->nav_count + 1) * sizeof *nv);
                if (!nv) goto fail;
                m->navs = nv;
                if (bwd_navp(c, &m->navs[m->nav_count]) == 0) m->nav_count++;
            }
        }
    }
    return 0;
fail:
    bwd_mission_free(m);
    return -1;
}

void bwd_mission_free(bwd_mission *m)
{
    int i;
    for (i = 0; i < m->record_count; i++) {
        free(m->records[i].data);
        free(m->records[i].chunks);
    }
    free(m->records);
    free(m->navs);
    memset(m, 0, sizeof *m);
}

char *bwd_mission_text(const bwd_mission *m, const char *record_suffix)
{
    size_t total = 1, used = 0;
    char *out;
    int i, k;
    for (i = 0; i < m->record_count; i++)
        for (k = 0; k < m->records[i].chunk_count; k++)
            if (strcmp(m->records[i].chunks[k].tag, "ORDR") == 0) total += m->records[i].chunks[k].size + 1;
    out = malloc(total);
    if (!out) return NULL;
    for (i = 0; i < m->record_count; i++) {
        size_t nl = strlen(m->records[i].name), sl = record_suffix ? strlen(record_suffix) : 0;
        if (record_suffix && (nl < sl || strcasecmp(m->records[i].name + nl - sl, record_suffix) != 0)) continue;
        for (k = 0; k < m->records[i].chunk_count; k++) {
            const bwd_chunk *c = &m->records[i].chunks[k];
            size_t j;
            if (strcmp(c->tag, "ORDR") != 0) continue;
            for (j = 0; j < c->size && c->data[j]; j++) out[used++] = (char)c->data[j];
            out[used++] = '\n';
        }
    }
    out[used] = '\0';
    return out;
}
