/* mechview - render MechWarrior 2 objects from MW2.PRJ
 *   mechview MW2.PRJ verify                  parse every POLY model; assemble every
 *                                            skeleton record (all representations)
 *   mechview MW2.PRJ RECORD out.ppm [yaw] [pitch] [repr] [palette]
 *        e.g. mechview MW2.PRJ TIMBRWLF tw.ppm 30 10 0 CYAN_DA
 */
#include "mech3d.h"
#include "prj.h"
#include "render.h"
#include "wtb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nearest(const r_target *t, int r, int g, int b)
{
    int i, best = 0, bd = 1 << 30;
    for (i = 0; i < 256; i++) {
        int d = (t->palette[i][0] - r) * (t->palette[i][0] - r) + (t->palette[i][1] - g) * (t->palette[i][1] - g) +
                (t->palette[i][2] - b) * (t->palette[i][2] - b);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static int verify(prj_archive *a)
{
    int t = prj_find_type(a, "POLY"), g = prj_find_type(a, "MGEO"), i, ok = 0, bad = 0, objs = 0, empty = 0;
    int mechs = 0, mech_bad = 0, reps = 0;
    for (i = 0; i < prj_symbol_count(a, t); i++) {
        prj_record r;
        wtb_model m;
        if (prj_read(a, t, prj_symbol_id(a, t, i), &r) != PRJ_OK) { bad++; continue; }
        if (r.size == 0) { empty++; prj_record_free(&r); continue; }
        if (wtb_parse(r.data, r.size, &m) == 0) { ok++; objs += m.object_count; wtb_free(&m); }
        else { bad++; printf("model %s: parse failed\n", r.name); }
        prj_record_free(&r);
    }
    printf("models: %d parsed (%d objects), %d empty records, %d failed\n", ok, objs, empty, bad);
    /* every object with an MGEO entry has a skeleton record of the same name */
    for (i = 0; i < prj_symbol_count(a, g); i++) {
        const char *nm = prj_symbol_name(a, g, i);
        mech3d m;
        int rp, n;
        if (prj_find_id(a, prj_find_type(a, "BWD"), nm) < 0) { printf("(%s: no skeleton record of that name)\n", nm); continue; }
        mechs++;
        if (mech3d_load(a, nm, 0, &m) != 0) { printf("skeleton %s: failed\n", nm); mech_bad++; continue; }
        n = m.repr_count;
        mech3d_free(&m);
        for (rp = 0; rp < n; rp++) {
            if (mech3d_load(a, nm, rp, &m) != 0) { printf("skeleton %s repr %d: failed\n", nm, rp); mech_bad++; continue; }
            reps++;
            mech3d_free(&m);
        }
    }
    printf("skeletons: %d objects, %d representations assembled, %d failures\n", mechs, reps, mech_bad);
    return bad || mech_bad;
}

int main(int argc, char **argv)
{
    char err[256], palname[32];
    prj_archive *a;
    prj_record pal, tbl;
    r_target t;
    mech3d m;
    float light[3] = {0.4f, 0.8f, 0.45f};
    int repr, rc;

    if (argc < 3) { fprintf(stderr, "usage: %s MW2.PRJ verify | RECORD out.ppm [yaw pitch repr palette]\n", argv[0]); return 2; }
    if (!(a = prj_open(argv[1], err, sizeof err))) { fprintf(stderr, "%s\n", err); return 1; }
    if (strcmp(argv[2], "verify") == 0) { rc = verify(a); prj_close(a); return rc; }
    if (argc < 4) { prj_close(a); return 2; }

    repr = argc > 6 ? atoi(argv[6]) : 0;
    snprintf(palname, sizeof palname, "%s", argc > 7 ? argv[7] : "CYAN_DA");
    if (mech3d_load(a, argv[2], repr, &m) != 0) { fprintf(stderr, "cannot assemble %s\n", argv[2]); prj_close(a); return 1; }
    if (prj_read_named(a, "PAL", palname, &pal) != PRJ_OK || prj_read_named(a, "LUMA", "STANDARD", &tbl) != PRJ_OK) {
        fprintf(stderr, "missing palette %s or STANDARD shading table\n", palname);
        mech3d_free(&m); prj_close(a); return 1;
    }
    r_init(&t, 640, 640);
    r_set_palette(&t, pal.data, pal.size);
    r_set_shade_table(&t, tbl.data, tbl.size);
    r_clear(&t, (uint8_t)nearest(&t, 90, 110, 140));
    r_draw_mech(&t, &m, argc > 4 ? (float)atof(argv[4]) : 30.0f, argc > 5 ? (float)atof(argv[5]) : 10.0f, light);
    rc = r_write_ppm(&t, argv[3]);
    printf("%s: %d parts (%d nodes, %d representations) -> %s\n", argv[2], m.part_count, m.node_count, m.repr_count, argv[3]);
    r_free(&t);
    prj_record_free(&pal);
    prj_record_free(&tbl);
    mech3d_free(&m);
    prj_close(a);
    return rc != 0;
}
