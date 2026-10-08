/*
 * prj.h - reader for MechWarrior 2's MW2.PRJ resource archive.
 *
 * Clean reimplementation from the on-disk format (decoded from the retail
 * file and checked against every record's checksum). Portable C99, no
 * assumptions about pointer size or host byte order.
 *
 * File layout (all integers little-endian):
 *
 *   0x00  "PROJ"  u32 size_minus_8  u16 ?  u16 ?
 *   0x0C  "DDIT"  u32 ?  u32 ?  u16 type_count
 *   0x1A  type_count x 24-byte type entries:
 *           tag[4]  u32 indx_off  u32 indx_size  u32 symb_off  u32 symb_size
 *           u16 ?   u16 ?
 *         (type 0 is "FREE", the free-space list, and has no INDX/SYMB.)
 *
 *   INDX block:  "INDX" u32 size-8 u32 ? tag[4] u16 capacity u16 count u16 ?
 *                then capacity x { u32 file_offset, u32 record_size }
 *                Slot 0 is unused; resource ids index this table directly.
 *   SYMB block:  "SYMB" u32 size-8 u32 ? tag[4] u16 capacity u16 count u16 ?
 *                then capacity x { char name[16], u16 id }
 *
 *   Record (at file_offset), 62-byte header then payload:
 *     "DATA" u32 size-8 u32 checksum tag[4] u8 stamp[4] u32 year
 *     u16 id u16 ? u16 ? char name[16] char filename[16]
 *   checksum = byte sum of everything from offset 12 to the record's end.
 */
#ifndef MW2_PRJ_H
#define MW2_PRJ_H

#include <stddef.h>
#include <stdint.h>

#define PRJ_NAME_LEN 16
#define PRJ_RECORD_HEADER 62

typedef struct prj_archive prj_archive;

typedef struct prj_record {
    char     tag[5];                    /* resource type, NUL-terminated */
    uint16_t id;
    char     name[PRJ_NAME_LEN + 1];    /* symbol name, e.g. "AA1BLADE" */
    char     filename[PRJ_NAME_LEN + 1];/* original source file, e.g. "AA1BLADE.WTB" */
    uint8_t  stamp[4];                  /* build timestamp bytes (meaning unconfirmed) */
    uint32_t year;
    uint32_t checksum;
    uint8_t *data;                      /* payload, malloc'd; free with prj_record_free */
    size_t   size;                      /* payload size in bytes */
} prj_record;

enum {
    PRJ_OK = 0,
    PRJ_ERR_IO = -1,
    PRJ_ERR_FORMAT = -2,
    PRJ_ERR_NOT_FOUND = -3,
    PRJ_ERR_CHECKSUM = -4,
    PRJ_ERR_NOMEM = -5
};

prj_archive *prj_open(const char *path, char *err, size_t errlen);
void         prj_close(prj_archive *a);

int          prj_type_count(const prj_archive *a);
const char  *prj_type_tag(const prj_archive *a, int type);      /* e.g. "POLY" */
int          prj_find_type(const prj_archive *a, const char *tag); /* -1 if absent */

int          prj_symbol_count(const prj_archive *a, int type);
const char  *prj_symbol_name(const prj_archive *a, int type, int index);
int          prj_symbol_id(const prj_archive *a, int type, int index);
int          prj_find_id(const prj_archive *a, int type, const char *name); /* case-insensitive */

/* Size of the record for (type, id), header included; 0 if the slot is empty. */
uint32_t     prj_record_size(const prj_archive *a, int type, int id);

/* Read and checksum-verify one record. Returns PRJ_OK or a negative error. */
int          prj_read(prj_archive *a, int type, int id, prj_record *out);
int          prj_read_named(prj_archive *a, const char *tag, const char *name, prj_record *out);
void         prj_record_free(prj_record *r);

/* TEXT resources (.XXT) are stored with each byte negated; this decodes in place. */
void         prj_decode_text(uint8_t *buf, size_t len);

const char  *prj_strerror(int code);

#endif
