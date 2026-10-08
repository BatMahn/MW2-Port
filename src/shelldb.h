/*
 * shelldb.h - the MW2 shell's data: DATABASE.MW2 / ARCH*.MW2 containers, their LZSS
 * compression, and PCX images. See docs/SHELL.md.
 */
#ifndef MW2_SHELLDB_H
#define MW2_SHELLDB_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t  *data;     /* whole file */
    size_t    size;
    int       count;
    uint32_t *offs;     /* count + 1 (end) */
} shelldb;

typedef struct {
    int      w, h;
    uint8_t *pix;       /* w*h palette indices */
    uint8_t  pal[256][3];
} shell_image;

int  shelldb_open(const char *path, shelldb *db);
void shelldb_close(shelldb *db);
/* Entry i, decompressed if it is LZSS-packed. Caller frees *out. Returns length or -1. */
long shelldb_entry(const shelldb *db, int i, uint8_t **out);
/* 1 if entry i is stored LZSS-compressed (u32 size + stream). */
int  shelldb_is_compressed(const shelldb *db, int i);
/* LZSS: flag byte LSB-first (1 = literal); match a,b: offset a | (b & 15) << 8, length (b >> 4) + 3;
 * 4096-byte zero window, write position from 0. Returns bytes produced. */
size_t shell_lzss(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen);
/* 8-bit PCX (run-length, trailing 0x0c + 768-byte palette). Returns 0 on success; with exact = 1
 * when the run-length data ends exactly at the palette. */
int  shell_pcx(const uint8_t *data, size_t len, shell_image *img, int *exact);
void shell_image_free(shell_image *img);

#endif
