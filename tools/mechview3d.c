/* mechview3d - textured render in the 3D-edition style
 *   mechview3d MODELS.PRJ TEXTURES.PRJ ati|3dfx RECORD CAMO DECAL out.ppm [yaw] [pitch] [palette]
 * e.g. mechview3d 3dfx/MW2.PRJ ati/MW2.PRJ ati TIMBRWLF JSCAMO_F L1WOLFCL tw.ppm 30 10
 * Models (and the palette for untextured polygons) come from the first archive,
 * textures from the second. ATi textures are the full-precision copy. */
#include "mech3d.h"
#include "prj.h"
#include "render.h"
#include "tex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int load_tex(prj_archive *a, const char *name, int enc, texture *t)
{
    prj_record r;
    int rc;
    if (prj_read_named(a, "CEL", name, &r) != PRJ_OK) return -1;
    rc = tex_decode(r.data, r.size, enc, t);
    prj_record_free(&r);
    return rc;
}

int main(int argc, char **argv)
{
    char err[256];
    prj_archive *ma, *ta;
    mech3d m;
    texture camo, decal;
    rt_target t;
    r_target pal_holder;
    prj_record pal;
    int ati, rc;
    float light[3] = {0.4f, 0.8f, 0.45f};

    if (argc < 8) { fprintf(stderr, "usage: %s MODELS.PRJ TEXTURES.PRJ ati|3dfx RECORD CAMO DECAL out.ppm [yaw pitch palette]\n", argv[0]); return 2; }
    if (!(ma = prj_open(argv[1], err, sizeof err)) || !(ta = prj_open(argv[2], err, sizeof err))) { fprintf(stderr, "%s\n", err); return 1; }
    ati = strcmp(argv[3], "ati") == 0;
    if (mech3d_load(ma, argv[4], 0, &m) != 0) { fprintf(stderr, "cannot assemble %s\n", argv[4]); return 1; }
    if (load_tex(ta, argv[5], ati ? TEX_ATI : TEX_3DFX_555, &camo) != 0 ||
        load_tex(ta, argv[6], ati ? TEX_ATI : TEX_3DFX_4444, &decal) != 0) { fprintf(stderr, "missing texture\n"); return 1; }
    r_init(&pal_holder, 1, 1);
    if (prj_read_named(ma, "PAL", argc > 10 ? argv[10] : "CYAN_DA", &pal) == PRJ_OK) {
        r_set_palette(&pal_holder, pal.data, pal.size);
        prj_record_free(&pal);
    }
    rt_init(&t, 640, 640, 90, 110, 140);
    rt_draw_mech(&t, &m, argc > 8 ? (float)atof(argv[8]) : 30.0f, argc > 9 ? (float)atof(argv[9]) : 10.0f, light,
                 &camo, &decal, (const uint8_t (*)[3])pal_holder.palette);
    rc = rt_write_ppm(&t, argv[7]);
    printf("%s: %d parts, camo %s %dx%d, decal %s %dx%d -> %s\n", argv[4], m.part_count, argv[5], camo.w, camo.h,
           argv[6], decal.w, decal.h, argv[7]);
    rt_free(&t); r_free(&pal_holder); tex_free(&camo); tex_free(&decal); mech3d_free(&m);
    prj_close(ma); prj_close(ta);
    return rc != 0;
}
