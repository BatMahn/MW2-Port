/* mektool - read mech definitions out of MW2.PRJ
 *   mektool MW2.PRJ list          every MEK record, one line each
 *   mektool MW2.PRJ NAME          full record sheet, e.g. TBR00STD
 */
#include "mek.h"
#include "prj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int load(prj_archive *a, int type, int id, mek_def *m)
{
    prj_record r;
    int rc = prj_read(a, type, id, &r);
    if (rc != PRJ_OK) return -1;
    rc = mek_parse(r.data, r.size, m);
    prj_record_free(&r);
    return rc;
}

static void sheet(const char *name, const mek_def *m)
{
    char buf[64];
    int l, k, i, armor = 0;
    for (l = 0; l < MEK_LOC_COUNT; l++) armor += (int)(m->loc[l].armor + m->loc[l].rear_armor);
    printf("%s - %s\n%u tons, walk %u, jump %u, heat sinking %u, armor %d\n\n", name, m->config_name,
           m->tonnage, m->walk_mp, m->jump_mp, m->heat_sinking, armor);
    for (l = 0; l < MEK_LOC_COUNT; l++) {
        const mek_loc *c = &m->loc[l];
        printf("%-13s armor %3u", mek_location_name(l), c->armor);
        if (c->rear_armor) printf(" (rear %u)", c->rear_armor); else printf("         ");
        printf("  internal %3u\n", c->internal);
        for (k = 0; k < c->slot_count; k++)
            if (c->slots[k]) printf("    %2d  %s\n", k + 1, mek_item_name(c->slots[k], buf, sizeof buf));
    }
    printf("\nWeapons\n");
    for (i = 0; i < m->item_count; i++) {
        const mek_weapon *w = mek_weapon_for(m->items[i].id);
        if (!w) continue;
        printf("  %-22s heat %2d  dmg %3d  range %4d m  %5.2f t\n", w->name, w->heat, w->damage,
               w->max_range_m, w->weight_x100 / 100.0);
    }
    for (i = m->item_count; i < m->item_count + m->link_count; i++)
        printf("  %s\n", mek_item_name(m->items[i].id, buf, sizeof buf));
}

int main(int argc, char **argv)
{
    char err[256];
    prj_archive *a;
    int t, i, ok = 0, bad = 0;
    mek_def m;

    if (argc < 3) { fprintf(stderr, "usage: %s MW2.PRJ list|NAME\n", argv[0]); return 2; }
    if (!(a = prj_open(argv[1], err, sizeof err))) { fprintf(stderr, "%s\n", err); return 1; }
    t = prj_find_type(a, "MEK");
    if (strcmp(argv[2], "list") == 0) {
        for (i = 0; i < prj_symbol_count(a, t); i++) {
            const char *nm = prj_symbol_name(a, t, i);
            if (load(a, t, prj_symbol_id(a, t, i), &m) != 0) { printf("%-10s PARSE ERROR\n", nm); bad++; continue; }
            printf("%-10s %4u t  walk %2u jump %u  %2d items  %s\n", nm, m.tonnage, m.walk_mp, m.jump_mp,
                   m.item_count, m.config_name);
            ok++;
        }
        printf("%d parsed, %d failed\n", ok, bad);
    } else {
        int id = prj_find_id(a, t, argv[2]);
        if (id < 0 || load(a, t, id, &m) != 0) { fprintf(stderr, "cannot load %s\n", argv[2]); prj_close(a); return 1; }
        sheet(argv[2], &m);
    }
    prj_close(a);
    return bad != 0;
}
