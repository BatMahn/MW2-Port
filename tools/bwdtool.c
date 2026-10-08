/* bwdtool - inspect MechWarrior 2 worlds and missions
 *   bwdtool MW2.PRJ verify            parse every BWD record
 *   bwdtool MW2.PRJ missions          every mission: parts, music, nav points
 *   bwdtool MW2.PRJ mission CYANSCN1  nav points, parts and briefing
 *   bwdtool MW2.PRJ chunks CYANWLD1   chunk list of one record
 * External parts (USERSTAR.BWD etc.) are found via MW2_INSTALL_DIR / MW2_CD_DIR.
 */
#include "bwd.h"
#include "mtbl.h"
#include "datapath.h"
#include "prj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int is_scene(const char *n) { size_t l = strlen(n); return l == 8 && strcasecmp(n + 4, "SCN1") == 0; }

int main(int argc, char **argv)
{
    char err[256];
    prj_archive *a;
    int t, i, rc = 0;

    if (argc < 3) { fprintf(stderr, "usage: %s MW2.PRJ verify|missions|mission NAME|chunks NAME\n", argv[0]); return 2; }
    if (!(a = prj_open(argv[1], err, sizeof err))) { fprintf(stderr, "%s\n", err); return 1; }
    dp_add_roots_from_env();
    t = prj_find_type(a, "BWD");

    if (strcmp(argv[2], "logic") == 0 && argc >= 4) {
        /* the scene's MTBL state tables, readable */
        prj_record rec;
        int n, k;
        bwd_chunk *c;
        if (prj_read_named(a, "BWD", argv[3], &rec) != PRJ_OK) { fprintf(stderr, "no record %s\n", argv[3]); return 1; }
        n = bwd_chunks(rec.data, rec.size, NULL, 0);
        c = calloc((size_t)(n > 0 ? n : 1), sizeof *c);
        bwd_chunks(rec.data, rec.size, c, n);
        for (k = 0; k < n; k++) {
            mtbl_table t;
            int i, j;
            char kbuf[16];
            if (strcmp(c[k].tag, "MTBL") != 0) continue;
            /* bwd_chunk.data is the payload; the decoder wants the chunk from its tag */
            if (mtbl_decode(c[k].data - 8, c[k].size + 8, &t) != 0) { printf("table: decode failed\n"); continue; }
            printf("== table %d (%d nodes)%s\n", t.index, t.node_count, t.index == 0 ? " - objectives and messages" : "");
            for (i = 0; i < t.node_count; i++) {
                const mtbl_node *nd = &t.nodes[i];
                printf("  %2d %-10s %c%s", i, strcmp(mtbl_kind_name(nd->kind), "?") ? mtbl_kind_name(nd->kind) : (snprintf(kbuf, sizeof kbuf, "k%#x", nd->kind), kbuf), nd->objective ? nd->objective : '-', nd->visible ? " shown " : "       ");
                if (nd->target[0] && strcmp(nd->target, "NULL")) printf(" target %-9s", nd->target);
                else printf("                 ");
                if (nd->kind >= MTBL_K_STAR_WIN) printf(" -> table %d node %d", nd->other_table, nd->other_node);
                if (nd->item_count) {
                    printf("  when %s:", nd->require_all ? "all" : "any");
                    for (j = 0; j < nd->item_count; j++)
                        printf(" %s(%d.%d)", nd->items[j].type == 'C' ? "done" : nd->items[j].type == 'S' ? "ok" : "failed",
                               nd->items[j].table, nd->items[j].node);
                }
                if (nd->delay && (nd->kind == MTBL_K_TIMER || nd->kind == MTBL_K_TIMER2 || nd->kind == MTBL_K_PROTECT)) printf("  delay %u", nd->delay);
                if (nd->text[0]) printf("  \"%s\"", nd->text);
                printf("\n");
            }
        }
        free(c);
        prj_record_free(&rec);
        prj_close(a);
        return 0;
    }
    if (strcmp(argv[2], "verify") == 0) {
        int ok = 0, bad = 0;
        long chunks = 0;
        for (i = 0; i < prj_symbol_count(a, t); i++) {
            prj_record r;
            int n;
            if (prj_read(a, t, prj_symbol_id(a, t, i), &r) != PRJ_OK) { bad++; continue; }
            n = bwd_chunks(r.data, r.size, NULL, 0);
            if (n < 0) { bad++; printf("malformed: %s\n", r.name); } else { ok++; chunks += n; }
            prj_record_free(&r);
        }
        printf("%d records parsed (%ld chunks), %d malformed\n", ok, chunks, bad);
        rc = bad != 0;
    } else if (strcmp(argv[2], "missions") == 0) {
        int n = 0, failed = 0;
        for (i = 0; i < prj_symbol_count(a, t); i++) {
            const char *nm = prj_symbol_name(a, t, i);
            bwd_mission m;
            int k;
            if (!is_scene(nm)) continue;
            n++;
            if (bwd_mission_load(a, nm, &m) != 0) { printf("%-9s LOAD FAILED\n", nm); failed++; continue; }
            {
                char music[8] = "-";
                if (m.music_track) snprintf(music, sizeof music, "%d", m.music_track);
                printf("%-9s %3d parts  music %-3s %2d nav points", nm, m.record_count, music, m.nav_count);
            }
            for (k = 0; k < m.missing_count; k++) printf("%s%s", k ? ", " : "  missing: ", m.missing[k]);
            printf("\n");
            bwd_mission_free(&m);
        }
        printf("%d missions, %d failed to load\n", n, failed);
        rc = failed != 0;
    } else if (strcmp(argv[2], "mission") == 0 && argc > 3) {
        bwd_mission m;
        char *text;
        int k;
        if (bwd_mission_load(a, argv[3], &m) != 0) { fprintf(stderr, "cannot load %s\n", argv[3]); prj_close(a); return 1; }
        printf("%s: %d parts, CD music track %d\n\nNav points\n", argv[3], m.record_count, m.music_track);
        for (k = 0; k < m.nav_count; k++)
            printf("  %-16s x %7d  y %6d  z %7d  flags %04x shown %d team %d\n", m.navs[k].name[0] ? m.navs[k].name : "(unnamed)",
                   m.navs[k].x, m.navs[k].y, m.navs[k].z, m.navs[k].flags, m.navs[k].shown, m.navs[k].team);
        printf("\nParts\n ");
        for (k = 0; k < m.record_count; k++) printf(" %s%s", m.records[k].name, m.records[k].external ? "(disk)" : "");
        for (k = 0; k < m.missing_count; k++) printf(" %s(missing)", m.missing[k]);
        text = bwd_mission_text(&m, "BRF1");
        printf("\n\nBriefing\n%s\n", text ? text : "");
        free(text);
        bwd_mission_free(&m);
    } else if (strcmp(argv[2], "chunks") == 0 && argc > 3) {
        prj_record r;
        bwd_chunk c[4096];
        int n, k;
        if (prj_read_named(a, "BWD", argv[3], &r) != PRJ_OK) { fprintf(stderr, "no record %s\n", argv[3]); prj_close(a); return 1; }
        n = bwd_chunks(r.data, r.size, c, 4096);
        for (k = 0; k < n && k < 4096; k++) printf("%-4s %6zu\n", c[k].tag, c[k].size);
        prj_record_free(&r);
    } else {
        fprintf(stderr, "bad command\n");
        rc = 2;
    }
    prj_close(a);
    return rc;
}
