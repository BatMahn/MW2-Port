/* prj.c - see prj.h for the format description. */
#include "prj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    uint32_t offset;
    uint32_t size;
} prj_slot;

typedef struct {
    char     name[PRJ_NAME_LEN + 1];
    uint16_t id;
} prj_symbol;

typedef struct {
    char        tag[5];
    prj_slot   *slots;
    int         slot_count;
    prj_symbol *symbols;
    int         symbol_count;
} prj_type;

struct prj_archive {
    FILE     *fp;
    uint64_t  file_size;
    prj_type *types;
    int       type_count;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void copy_name(char *dst, const uint8_t *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = (char)src[i];
    dst[i] = '\0';
}

static void tag_from(char dst[5], const uint8_t *src)
{
    copy_name(dst, src, 4);
}

static int read_at(prj_archive *a, uint64_t off, void *buf, size_t len)
{
    if (off > a->file_size || len > a->file_size - off) return PRJ_ERR_FORMAT;
    if (fseek(a->fp, (long)off, SEEK_SET) != 0) return PRJ_ERR_IO;
    if (fread(buf, 1, len, a->fp) != len) return PRJ_ERR_IO;
    return PRJ_OK;
}

/* Reads an INDX or SYMB block into a malloc'd buffer after validating its header. */
static int load_block(prj_archive *a, uint32_t off, uint32_t size, const char *magic,
                      const char *tag, uint8_t **out, uint16_t *capacity)
{
    uint8_t *buf;
    int rc;

    *out = NULL;
    if (size < 22) return PRJ_ERR_FORMAT;
    buf = (uint8_t *)malloc(size);
    if (!buf) return PRJ_ERR_NOMEM;
    rc = read_at(a, off, buf, size);
    if (rc != PRJ_OK) { free(buf); return rc; }
    if (memcmp(buf, magic, 4) != 0 || rd32(buf + 4) + 8u != size ||
        strncmp((const char *)buf + 12, tag, strlen(tag)) != 0) {
        free(buf);
        return PRJ_ERR_FORMAT;
    }
    *capacity = rd16(buf + 16);
    *out = buf;
    return PRJ_OK;
}

static int load_type(prj_archive *a, prj_type *t, const uint8_t *ent)
{
    uint32_t indx_off = rd32(ent + 4), indx_size = rd32(ent + 8);
    uint32_t symb_off = rd32(ent + 12), symb_size = rd32(ent + 16);
    uint8_t *blk;
    uint16_t cap;
    int rc, i;

    tag_from(t->tag, ent);
    if (strcmp(t->tag, "FREE") == 0) return PRJ_OK; /* free list: nothing to index */

    rc = load_block(a, indx_off, indx_size, "INDX", t->tag, &blk, &cap);
    if (rc != PRJ_OK) return rc;
    if (22u + (uint32_t)cap * 8u > indx_size) { free(blk); return PRJ_ERR_FORMAT; }
    t->slots = (prj_slot *)calloc(cap ? cap : 1, sizeof *t->slots);
    if (!t->slots) { free(blk); return PRJ_ERR_NOMEM; }
    t->slot_count = cap;
    for (i = 0; i < cap; i++) {
        t->slots[i].offset = rd32(blk + 22 + i * 8);
        t->slots[i].size = rd32(blk + 22 + i * 8 + 4);
    }
    free(blk);

    rc = load_block(a, symb_off, symb_size, "SYMB", t->tag, &blk, &cap);
    if (rc != PRJ_OK) return rc;
    if (22u + (uint32_t)cap * 18u > symb_size) { free(blk); return PRJ_ERR_FORMAT; }
    {
        uint16_t count = rd16(blk + 18);
        if (count > cap) count = cap;
        t->symbols = (prj_symbol *)calloc(count ? count : 1, sizeof *t->symbols);
        if (!t->symbols) { free(blk); return PRJ_ERR_NOMEM; }
        for (i = 0; i < count; i++) {
            const uint8_t *e = blk + 22 + i * 18;
            copy_name(t->symbols[i].name, e, PRJ_NAME_LEN);
            t->symbols[i].id = rd16(e + 16);
        }
        t->symbol_count = count;
    }
    free(blk);
    return PRJ_OK;
}

prj_archive *prj_open(const char *path, char *err, size_t errlen)
{
    prj_archive *a;
    uint8_t hdr[26];
    uint8_t *ents = NULL;
    int count, i, rc;

#define FAIL(msg) do { if (err) snprintf(err, errlen, "%s", msg); goto fail; } while (0)

    a = (prj_archive *)calloc(1, sizeof *a);
    if (!a) { if (err) snprintf(err, errlen, "out of memory"); return NULL; }
    a->fp = fopen(path, "rb");
    if (!a->fp) FAIL("cannot open archive");
    if (fseek(a->fp, 0, SEEK_END) != 0) FAIL("seek failed");
    a->file_size = (uint64_t)ftell(a->fp);

    if (read_at(a, 0, hdr, sizeof hdr) != PRJ_OK) FAIL("file too short");
    if (memcmp(hdr, "PROJ", 4) != 0) FAIL("not a PROJ archive");
    if (rd32(hdr + 4) + 8u != a->file_size) FAIL("size field does not match file size");
    if (memcmp(hdr + 12, "DDIT", 4) != 0) FAIL("missing DDIT directory");

    count = rd16(hdr + 24);
    if (count <= 0 || count > 256) FAIL("implausible type count");
    ents = (uint8_t *)malloc((size_t)count * 24);
    if (!ents) FAIL("out of memory");
    if (read_at(a, 26, ents, (size_t)count * 24) != PRJ_OK) FAIL("truncated type table");

    a->types = (prj_type *)calloc((size_t)count, sizeof *a->types);
    if (!a->types) FAIL("out of memory");
    a->type_count = count;
    for (i = 0; i < count; i++) {
        rc = load_type(a, &a->types[i], ents + i * 24);
        if (rc != PRJ_OK) {
            if (err) snprintf(err, errlen, "type %d (%.4s): %s", i, (const char *)(ents + i * 24),
                              prj_strerror(rc));
            goto fail;
        }
    }
    free(ents);
    return a;
#undef FAIL
fail:
    free(ents);
    prj_close(a);
    return NULL;
}

void prj_close(prj_archive *a)
{
    int i;
    if (!a) return;
    if (a->types) {
        for (i = 0; i < a->type_count; i++) {
            free(a->types[i].slots);
            free(a->types[i].symbols);
        }
        free(a->types);
    }
    if (a->fp) fclose(a->fp);
    free(a);
}

int prj_type_count(const prj_archive *a) { return a->type_count; }

const char *prj_type_tag(const prj_archive *a, int type)
{
    return (type >= 0 && type < a->type_count) ? a->types[type].tag : NULL;
}

int prj_find_type(const prj_archive *a, const char *tag)
{
    int i;
    for (i = 0; i < a->type_count; i++)
        if (strcasecmp(a->types[i].tag, tag) == 0) return i;
    return -1;
}

int prj_symbol_count(const prj_archive *a, int type)
{
    return (type >= 0 && type < a->type_count) ? a->types[type].symbol_count : 0;
}

const char *prj_symbol_name(const prj_archive *a, int type, int index)
{
    if (type < 0 || type >= a->type_count) return NULL;
    if (index < 0 || index >= a->types[type].symbol_count) return NULL;
    return a->types[type].symbols[index].name;
}

int prj_symbol_id(const prj_archive *a, int type, int index)
{
    if (type < 0 || type >= a->type_count) return -1;
    if (index < 0 || index >= a->types[type].symbol_count) return -1;
    return a->types[type].symbols[index].id;
}

int prj_find_id(const prj_archive *a, int type, const char *name)
{
    int i;
    if (type < 0 || type >= a->type_count) return -1;
    for (i = 0; i < a->types[type].symbol_count; i++)
        if (strcasecmp(a->types[type].symbols[i].name, name) == 0) return a->types[type].symbols[i].id;
    return -1;
}

uint32_t prj_record_size(const prj_archive *a, int type, int id)
{
    if (type < 0 || type >= a->type_count) return 0;
    if (id <= 0 || id >= a->types[type].slot_count) return 0;
    return a->types[type].slots[id].offset ? a->types[type].slots[id].size : 0;
}

int prj_read(prj_archive *a, int type, int id, prj_record *out)
{
    const prj_type *t;
    uint8_t *buf;
    uint32_t off, size, sum = 0;
    size_t i;
    int rc;

    memset(out, 0, sizeof *out);
    if (type < 0 || type >= a->type_count) return PRJ_ERR_NOT_FOUND;
    t = &a->types[type];
    if (id <= 0 || id >= t->slot_count || t->slots[id].offset == 0) return PRJ_ERR_NOT_FOUND;
    off = t->slots[id].offset;
    size = t->slots[id].size;
    if (size < PRJ_RECORD_HEADER) return PRJ_ERR_FORMAT;

    buf = (uint8_t *)malloc(size);
    if (!buf) return PRJ_ERR_NOMEM;
    rc = read_at(a, off, buf, size);
    if (rc != PRJ_OK) { free(buf); return rc; }

    if (memcmp(buf, "DATA", 4) != 0 || rd32(buf + 4) + 8u != size ||
        strncmp((const char *)buf + 12, t->tag, strlen(t->tag)) != 0 || rd16(buf + 24) != id) {
        free(buf);
        return PRJ_ERR_FORMAT;
    }
    for (i = 12; i < size; i++) sum += buf[i];
    out->checksum = rd32(buf + 8);
    if (sum != out->checksum) { free(buf); return PRJ_ERR_CHECKSUM; }

    tag_from(out->tag, buf + 12);
    memcpy(out->stamp, buf + 16, 4);
    out->year = rd32(buf + 20);
    out->id = (uint16_t)id;
    copy_name(out->name, buf + 30, PRJ_NAME_LEN);
    copy_name(out->filename, buf + 46, PRJ_NAME_LEN);

    out->size = size - PRJ_RECORD_HEADER;
    memmove(buf, buf + PRJ_RECORD_HEADER, out->size);
    out->data = buf;
    return PRJ_OK;
}

int prj_read_named(prj_archive *a, const char *tag, const char *name, prj_record *out)
{
    int type = prj_find_type(a, tag);
    int id = prj_find_id(a, type, name);
    if (type < 0 || id < 0) { memset(out, 0, sizeof *out); return PRJ_ERR_NOT_FOUND; }
    return prj_read(a, type, id, out);
}

void prj_record_free(prj_record *r)
{
    if (!r) return;
    free(r->data);
    r->data = NULL;
    r->size = 0;
}

void prj_decode_text(uint8_t *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) buf[i] = (uint8_t)(0u - buf[i]);
}

const char *prj_strerror(int code)
{
    switch (code) {
    case PRJ_OK: return "ok";
    case PRJ_ERR_IO: return "I/O error";
    case PRJ_ERR_FORMAT: return "malformed archive data";
    case PRJ_ERR_NOT_FOUND: return "resource not found";
    case PRJ_ERR_CHECKSUM: return "checksum mismatch";
    case PRJ_ERR_NOMEM: return "out of memory";
    default: return "unknown error";
    }
}
