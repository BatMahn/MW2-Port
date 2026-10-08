/* prjtool - inspect and extract MW2.PRJ
 *   prjtool types   MW2.PRJ
 *   prjtool list    MW2.PRJ TAG
 *   prjtool verify  MW2.PRJ               (reads + checksums every record)
 *   prjtool extract MW2.PRJ TAG NAME OUT  (writes payload; TEXT is decoded)
 *   prjtool dumpall MW2.PRJ OUTDIR        (every record, as OUTDIR/TAG/FILENAME)
 */
#include "prj.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int write_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno)); return 1; }
    if (fwrite(data, 1, len, f) != len) { fclose(f); return 1; }
    return fclose(f) != 0;
}

static int cmd_types(prj_archive *a)
{
    int t;
    for (t = 0; t < prj_type_count(a); t++)
        printf("%2d  %-4s  %5d symbols\n", t, prj_type_tag(a, t), prj_symbol_count(a, t));
    return 0;
}

static int cmd_list(prj_archive *a, const char *tag)
{
    int t = prj_find_type(a, tag), i;
    if (t < 0) { fprintf(stderr, "no type %s\n", tag); return 1; }
    for (i = 0; i < prj_symbol_count(a, t); i++) {
        int id = prj_symbol_id(a, t, i);
        printf("%5d  %-16s  %8u bytes\n", id, prj_symbol_name(a, t, i), (unsigned)prj_record_size(a, t, id));
    }
    return 0;
}

static int cmd_verify(prj_archive *a)
{
    long ok = 0, bad = 0, total_bytes = 0;
    int t, i;
    for (t = 0; t < prj_type_count(a); t++) {
        for (i = 0; i < prj_symbol_count(a, t); i++) {
            prj_record r;
            int id = prj_symbol_id(a, t, i), rc;
            if (prj_record_size(a, t, id) == 0) continue;
            rc = prj_read(a, t, id, &r);
            if (rc == PRJ_OK) {
                ok++;
                total_bytes += (long)r.size;
            } else {
                bad++;
                fprintf(stderr, "%s/%s (id %d): %s\n", prj_type_tag(a, t), prj_symbol_name(a, t, i), id,
                        prj_strerror(rc));
            }
            prj_record_free(&r);
        }
    }
    printf("verified %ld records (%ld payload bytes), %ld failed\n", ok, total_bytes, bad);
    return bad != 0;
}

static int cmd_extract(prj_archive *a, const char *tag, const char *name, const char *out)
{
    prj_record r;
    int rc = prj_read_named(a, tag, name, &r), res;
    if (rc != PRJ_OK) { fprintf(stderr, "%s/%s: %s\n", tag, name, prj_strerror(rc)); return 1; }
    if (strcmp(r.tag, "TEXT") == 0) prj_decode_text(r.data, r.size);
    printf("%s/%s id %u from %s, %zu bytes\n", r.tag, r.name, r.id, r.filename, r.size);
    res = write_file(out, r.data, r.size);
    prj_record_free(&r);
    return res;
}

static int cmd_dumpall(prj_archive *a, const char *dir)
{
    char path[1024];
    int t, i, n = 0;
    mkdir(dir, 0755);
    for (t = 0; t < prj_type_count(a); t++) {
        snprintf(path, sizeof path, "%s/%s", dir, prj_type_tag(a, t));
        mkdir(path, 0755);
        for (i = 0; i < prj_symbol_count(a, t); i++) {
            prj_record r;
            int id = prj_symbol_id(a, t, i);
            if (prj_record_size(a, t, id) == 0 || prj_read(a, t, id, &r) != PRJ_OK) continue;
            if (strcmp(r.tag, "TEXT") == 0) prj_decode_text(r.data, r.size);
            snprintf(path, sizeof path, "%s/%s/%s", dir, prj_type_tag(a, t),
                     r.filename[0] ? r.filename : r.name);
            if (write_file(path, r.data, r.size) == 0) n++;
            prj_record_free(&r);
        }
    }
    printf("wrote %d files to %s\n", n, dir);
    return 0;
}

int main(int argc, char **argv)
{
    char err[256];
    prj_archive *a;
    int rc = 2;

    if (argc < 3) {
        fprintf(stderr, "usage: %s types|list|verify|extract|dumpall MW2.PRJ [args]\n", argv[0]);
        return 2;
    }
    a = prj_open(argv[2], err, sizeof err);
    if (!a) { fprintf(stderr, "%s: %s\n", argv[2], err); return 1; }

    if (strcmp(argv[1], "types") == 0) rc = cmd_types(a);
    else if (strcmp(argv[1], "list") == 0 && argc > 3) rc = cmd_list(a, argv[3]);
    else if (strcmp(argv[1], "verify") == 0) rc = cmd_verify(a);
    else if (strcmp(argv[1], "extract") == 0 && argc > 5) rc = cmd_extract(a, argv[3], argv[4], argv[5]);
    else if (strcmp(argv[1], "dumpall") == 0 && argc > 3) rc = cmd_dumpall(a, argv[3]);
    else fprintf(stderr, "bad command\n");

    prj_close(a);
    return rc;
}
