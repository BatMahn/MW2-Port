/*
 * datapath.h - where the game's files come from.
 *
 * The original reads some files from the install folder and the rest from the
 * CD (it refuses to start without the disc). The port has no CD check: it
 * searches an ordered list of folders instead, typically
 *
 *   1. the install folder (MW2.PRJ, configs, saves)
 *   2. a folder holding a copy of the CD's files
 *
 * DOS filenames are case-insensitive and use backslashes; Linux and macOS
 * paths are not. dp_resolve() maps "MEK\\MDG00STD.MEK" onto whatever case the
 * files actually have on disk (e.g. mek/mdg00std.mek).
 */
#ifndef MW2_DATAPATH_H
#define MW2_DATAPATH_H

#include <stddef.h>
#include <stdio.h>

#define DP_MAX_ROOTS 8

/* Add a search folder; earlier roots take priority. Returns 0 on success. */
int   dp_add_root(const char *dir);
void  dp_clear_roots(void);

/* Add an ISO 9660 image (2048-byte sectors, e.g. cdrip's mw2cd.iso) as a
 * read-only root. subdir, if not NULL, is a folder inside the image to treat
 * as the root (e.g. "MECH2"). Returns 0 on success. */
int   dp_add_iso_root(const char *iso_path, const char *subdir);

/* Adds roots from the environment, in this order:
 *   MW2_INSTALL_DIR   install folder
 *   MW2_CD_DIR        folder holding a copy of the disc
 *   MW2_CD_IMAGE      the disc's data track as an .iso
 * Disc roots are searched in their MECH2 folder first, then at the top. */
void  dp_add_roots_from_env(void);

/* Read a whole file from any root (folders or ISO images) into a malloc'd
 * buffer. Returns 0 on success; free(*data) when done. */
int   dp_read_file(const char *dos_path, unsigned char **data, size_t *len);

/* Resolve a DOS-style relative path to a real file in a folder root (ISO roots
 * are skipped). Returns 0 and fills out on success. */
int   dp_resolve(const char *dos_path, char *out, size_t outlen);

/* fopen() through dp_resolve(). Writes ("w"/"a") go to the first root, creating
 * the file with the original DOS spelling if it doesn't exist yet. */
FILE *dp_fopen(const char *dos_path, const char *mode);

#endif
