/*
 * cddrive.h - digital audio from a real CD drive (CD-DA extraction): the disc's table of contents and raw 2352-byte
 * audio sectors (16-bit stereo, little-endian, 44.1 kHz), read the way rippers read them. The original game had the
 * drive play the music itself, out of its analog jack into the sound card; modern machines have no such cable, so the
 * port reads the sectors and mixes them in like any other source.
 *
 *   Linux:   /dev/sr0.. (CDROMREADTOCENTRY / CDROMREADAUDIO)
 *   Windows: \\.\D: ... (IOCTL_CDROM_READ_TOC / IOCTL_CDROM_RAW_READ; Windows XP and newer)
 *   macOS:   /dev/rdiskN (DKIOCCDREADTOC / DKIOCCDREAD); cdaudio also looks for the AIFF tracks macOS mounts
 *   classic Mac OS (Carbon): not supported
 */
#ifndef MW2_CDDRIVE_H
#define MW2_CDDRIVE_H

#include <stdint.h>

#define CDD_SECTOR_BYTES 2352
#define CDD_MAX_TRACKS   99

typedef struct cddrive cddrive;

typedef struct {
    int      first, last;                    /* track numbers */
    uint32_t start[CDD_MAX_TRACKS + 2];      /* LBA of each track (index 1); start[last + 1] = lead-out */
    int      data[CDD_MAX_TRACKS + 2];       /* 1 = data track */
} cddrive_toc;

/* Open a drive: dev NULL / "" / "auto" takes the first drive holding a disc with audio tracks; else a device path
 * (Linux / macOS) or a drive letter ("E" / "E:"). 0 on success. */
int  cddrive_open(cddrive **out, const char *dev, cddrive_toc *toc);
/* Read n raw audio sectors from lba into buf (n * 2352 bytes). Returns the sectors read, or -1. Blocking. */
int  cddrive_read(cddrive *d, uint32_t lba, int n, unsigned char *buf);
const char *cddrive_name(const cddrive *d);
void cddrive_close(cddrive *d);

#endif
