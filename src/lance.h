/*
 * lance.h - the files the MW2 shell writes for the simulation when a mission launches
 * (MW2SHELL FUN_0001d540 / FUN_0001d310 / FUN_0001d140 / FUN_0001d6c0):
 *   userstar.bwd   the player's lance (GPS records; slot 0 = the player)
 *   en01..05star.bwd  the enemy lance, skill falling by one per file
 *   instmap1.bwd   clan camo: BMPJ (camo record) + BMID (slot 0x114 friendly, 0x115 enemy, 0x101 jscamoia)
 * Layout: "BWD\0", u32 file size, u32 largest chunk; REV "1.22"; DTBL (20 zero bytes); chunks.
 */
#ifndef MW2_LANCE_H
#define MW2_LANCE_H

#include "prj.h"

typedef struct {
    int  mech;            /* index into the shell mech list (lance_mech_*), -1 = empty */
    char loadout[9];      /* MEK record, e.g. "mdg00std" ("" = <code>00std) */
    char pilot[16];
} lance_slot;

int         lance_mech_count(void);                 /* 18 (15 without the extras) */
const char *lance_mech_code(int i);                 /* "mdg" */
const char *lance_mech_file(int i);                 /* "maddog" */
const char *lance_mech_name(int i);                 /* "Mad Dog" */

/* Write one star file: file_index 0 = userstar (player's lance), 1..5 = enemy files. */
int lance_write_star(prj_archive *a, const char *path, const lance_slot *slots, int n, int file_index, int skill);
/* Write the player's lance and the five enemy files into dir; difficulty 0 easy, 1 medium, 2 hard. */
int lance_write_all(prj_archive *a, const char *dir, const lance_slot friendly[3], const lance_slot enemy[3], int difficulty);
/* instmap1.bwd: clan indices 0 Wolf, 1 Jade Falcon, 2 Ghost Bear, 3 Smoke Jaguar, 4 Nova Cat, 5 Steel Viper. */
int lance_write_instmap(prj_archive *a, const char *dir, int friendly_clan, int enemy_clan);

#endif
