/*
 * posix_mac.h - force-included (-include) in the classic Mac OS / Carbon build: the game's POSIX file calls, made with
 * '/' paths, go to posix_mac.c, which turns the path into an HFS one ("/Vol/a/b" -> "Vol:a:b", "a/b" -> ":a:b",
 * "../a" -> "::a") and works on the File Manager (fopen through funopen on a data fork; stat, mkdir, remove, rename,
 * realpath, and the directory reads in dirent.h). Function-like macros, so `struct stat` keeps its name.
 */
#ifndef MW2_POSIX_MAC_H
#define MW2_POSIX_MAC_H
#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp / strncasecmp (POSIX; newlib declares them here) */
#include <stddef.h>
/* declared outright as well: newlib implements them, but its headers show them only for some feature settings */
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
#include <sys/stat.h>
FILE *mw2_fopen(const char *path, const char *mode);
int mw2_stat(const char *path, struct stat *st);
int mw2_mkdir(const char *path, int mode);
int mw2_remove(const char *path);
int mw2_rename(const char *from, const char *to);
char *mw2_realpath(const char *path, char *resolved);
int mw2_hfs_path(const char *in, char *out, int outn);   /* the HFS path of a '/' one (0, or -1 if too long) */
#define fopen(p, m) mw2_fopen(p, m)
#define stat(p, s) mw2_stat(p, s)
#define mkdir(p, m) mw2_mkdir(p, m)
#define remove(p) mw2_remove(p)
#define rename(a, b) mw2_rename(a, b)
#define realpath(p, r) mw2_realpath(p, r)
#endif
