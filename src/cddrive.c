/*
 * cddrive.c - CD-DA extraction from a real drive (see cddrive.h).
 */
#include "cddrive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(MW2_CLASSIC_MAC)

int  cddrive_open(cddrive **out, const char *dev, cddrive_toc *toc) { (void)dev; (void)toc; *out = NULL; return -1; }
int  cddrive_read(cddrive *d, uint32_t lba, int n, unsigned char *buf) { (void)d; (void)lba; (void)n; (void)buf; return -1; }
const char *cddrive_name(const cddrive *d) { (void)d; return ""; }
void cddrive_close(cddrive *d) { (void)d; }

#else

/* ---------------------------------------------------------------------------------------------------- per system */
#if defined(_WIN32)
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>
typedef HANDLE cdd_handle;
#define CDD_BAD INVALID_HANDLE_VALUE
#define CDD_READ_MAX 20   /* sectors a read (IOCTL_CDROM_RAW_READ: some drivers refuse more than 64 KiB) */
#elif defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/cdrom.h>
typedef int cdd_handle;
#define CDD_BAD (-1)
#define CDD_READ_MAX 25
#elif defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <IOKit/storage/IOCDMediaBSDClient.h>
typedef int cdd_handle;
#define CDD_BAD (-1)
#define CDD_READ_MAX 25
#else
typedef int cdd_handle;
#define CDD_BAD (-1)
#define CDD_READ_MAX 25
#endif

struct cddrive {
    cdd_handle h;
    char       name[64];
};

static void cdd_close_handle(cdd_handle h)
{
#if defined(_WIN32)
    if (h != CDD_BAD) CloseHandle(h);
#elif defined(__linux__) || defined(__APPLE__)
    if (h != CDD_BAD) close(h);
#else
    (void)h;
#endif
}

#if defined(_WIN32) || defined(__APPLE__)
static uint32_t msf_lba(int m, int s, int f) { return (uint32_t)((m * 60 + s) * 75 + f - 150); }
#endif

/* the TOC: 0 if it read and has at least one audio track */
static int cdd_read_toc(cdd_handle h, cddrive_toc *toc)
{
    int n, audio = 0;
    memset(toc, 0, sizeof *toc);
#if defined(_WIN32)
    {
        CDROM_TOC t;
        DWORD got = 0;
        memset(&t, 0, sizeof t);
        if (!DeviceIoControl(h, IOCTL_CDROM_READ_TOC, NULL, 0, &t, sizeof t, &got, NULL)) return -1;
        toc->first = t.FirstTrack;
        toc->last = t.LastTrack;
        if (toc->first < 1 || toc->last > CDD_MAX_TRACKS || toc->last < toc->first) return -1;
        for (n = 0; n <= toc->last - toc->first + 1; n++) {   /* the last entry is the lead-out (0xAA) */
            const TRACK_DATA *td = &t.TrackData[n];
            const int num = n <= toc->last - toc->first ? toc->first + n : toc->last + 1;
            toc->start[num] = msf_lba(td->Address[1], td->Address[2], td->Address[3]);
            toc->data[num] = (td->Control & 4) != 0;
        }
    }
#elif defined(__linux__)
    {
        struct cdrom_tochdr hdr;
        struct cdrom_tocentry e;
        if (ioctl(h, CDROMREADTOCHDR, &hdr) != 0) return -1;
        toc->first = hdr.cdth_trk0;
        toc->last = hdr.cdth_trk1;
        if (toc->first < 1 || toc->last > CDD_MAX_TRACKS || toc->last < toc->first) return -1;
        for (n = toc->first; n <= toc->last + 1; n++) {
            memset(&e, 0, sizeof e);
            e.cdte_track = (unsigned char)(n <= toc->last ? n : CDROM_LEADOUT);
            e.cdte_format = CDROM_LBA;
            if (ioctl(h, CDROMREADTOCENTRY, &e) != 0) return -1;
            toc->start[n] = (uint32_t)e.cdte_addr.lba;
            toc->data[n] = (e.cdte_ctrl & CDROM_DATA_TRACK) != 0;
        }
    }
#elif defined(__APPLE__)
    {
        /* the raw (full) TOC: 4-byte header, then 11-byte descriptors - session, adr/control, tno, point, ATIME (3),
         * zero, PTIME (3). Points 1-99 are tracks, 0xA0 / 0xA1 the first / last track, 0xA2 the lead-out. */
        unsigned char buf[4 + 11 * 110];
        dk_cd_read_toc_t r;
        int len, i, first = 99, last = 0;
        memset(&r, 0, sizeof r);
        memset(buf, 0, sizeof buf);
        r.format = kCDTOCFormatTOC;
        r.formatAsTime = 1;
        r.bufferLength = sizeof buf;
        r.buffer = buf;
        if (ioctl(h, DKIOCCDREADTOC, &r) != 0) return -1;
        len = (buf[0] << 8 | buf[1]) + 2;
        if (len > (int)sizeof buf) len = (int)sizeof buf;
        for (i = 4; i + 11 <= len; i += 11) {
            const unsigned char *d = buf + i;
            const int point = d[3];
            if (d[0] != 1) continue;   /* the first session only (the audio is there on a mixed-mode disc) */
            if (point >= 1 && point <= CDD_MAX_TRACKS) {
                toc->start[point] = msf_lba(d[8], d[9], d[10]);
                toc->data[point] = (d[1] & 4) != 0;
                if (point < first) first = point;
                if (point > last) last = point;
            } else if (point == 0xA2) toc->start[CDD_MAX_TRACKS + 1] = msf_lba(d[8], d[9], d[10]);
        }
        if (last < first) return -1;
        toc->first = first;
        toc->last = last;
        toc->start[last + 1] = toc->start[CDD_MAX_TRACKS + 1];
    }
#else
    (void)h;
    return -1;
#endif
    for (n = toc->first; n <= toc->last; n++) if (!toc->data[n]) audio++;
    return audio ? 0 : -1;
}

static cdd_handle cdd_open_device(const char *dev, char *name, size_t namen)
{
#if defined(_WIN32)
    char path[16];
    char letter = dev[0];
    HANDLE h;
    if (!((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z'))) return CDD_BAD;
    snprintf(path, sizeof path, "\\\\.\\%c:", letter);
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    snprintf(name, namen, "%c:", letter);
    return h;
#elif defined(__linux__)
    snprintf(name, namen, "%s", dev);
    return open(dev, O_RDONLY | O_NONBLOCK);
#elif defined(__APPLE__)
    snprintf(name, namen, "%s", dev);
    return open(dev, O_RDONLY);
#else
    (void)dev; (void)name; (void)namen;
    return CDD_BAD;
#endif
}

static int cdd_try(cddrive **out, const char *dev, cddrive_toc *toc)
{
    char name[64];
    cdd_handle h = cdd_open_device(dev, name, sizeof name);
    cddrive *d;
    if (h == CDD_BAD) return -1;
    if (cdd_read_toc(h, toc) != 0) { cdd_close_handle(h); return -1; }
    if (!(d = (cddrive *)calloc(1, sizeof *d))) { cdd_close_handle(h); return -1; }
    d->h = h;
    snprintf(d->name, sizeof d->name, "%s", name);
    *out = d;
    return 0;
}

int cddrive_open(cddrive **out, const char *dev, cddrive_toc *toc)
{
    *out = NULL;
    if (dev && dev[0] && strcmp(dev, "auto") != 0) return cdd_try(out, dev, toc);
#if defined(_WIN32)
    {
        DWORD mask = GetLogicalDrives();
        char root[4] = "A:\\";
        int i;
        for (i = 0; i < 26; i++) {
            if (!(mask & (1u << i))) continue;
            root[0] = (char)('A' + i);
            if (GetDriveTypeA(root) == DRIVE_CDROM && cdd_try(out, root, toc) == 0) return 0;
        }
    }
#elif defined(__linux__)
    {
        static const char *const devs[] = { "/dev/cdrom", "/dev/sr0", "/dev/sr1", "/dev/sr2", "/dev/sr3" };
        size_t i;
        for (i = 0; i < sizeof devs / sizeof devs[0]; i++) if (cdd_try(out, devs[i], toc) == 0) return 0;
    }
#elif defined(__APPLE__)
    {
        char p[32];
        int i;
        for (i = 1; i < 16; i++) {   /* (an optical disc's raw device belongs to the logged-in user) */
            snprintf(p, sizeof p, "/dev/rdisk%d", i);
            if (cdd_try(out, p, toc) == 0) return 0;
        }
    }
#endif
    return -1;
}

int cddrive_read(cddrive *d, uint32_t lba, int n, unsigned char *buf)
{
    int done = 0;
    while (done < n) {
        const int k = n - done > CDD_READ_MAX ? CDD_READ_MAX : n - done;
        unsigned char *dst = buf + (size_t)done * CDD_SECTOR_BYTES;
#if defined(_WIN32)
        RAW_READ_INFO ri;
        DWORD got = 0;
        memset(&ri, 0, sizeof ri);
        ri.DiskOffset.QuadPart = (LONGLONG)(lba + (uint32_t)done) * 2048;   /* (in 2048-byte units, by definition) */
        ri.SectorCount = (ULONG)k;
        ri.TrackMode = CDDA;
        if (!DeviceIoControl(d->h, IOCTL_CDROM_RAW_READ, &ri, sizeof ri, dst, (DWORD)(k * CDD_SECTOR_BYTES), &got, NULL))
            return done ? done : -1;
#elif defined(__linux__)
        struct cdrom_read_audio ra;
        memset(&ra, 0, sizeof ra);
        ra.addr.lba = (int)(lba + (uint32_t)done);
        ra.addr_format = CDROM_LBA;
        ra.nframes = k;
        ra.buf = dst;
        if (ioctl(d->h, CDROMREADAUDIO, &ra) != 0) return done ? done : -1;
#elif defined(__APPLE__)
        dk_cd_read_t r;
        memset(&r, 0, sizeof r);
        r.offset = (uint64_t)(lba + (uint32_t)done) * kCDSectorSizeCDDA;
        r.sectorArea = kCDSectorAreaUser;
        r.sectorType = kCDSectorTypeCDDA;
        r.bufferLength = (uint32_t)(k * CDD_SECTOR_BYTES);
        r.buffer = dst;
        if (ioctl(d->h, DKIOCCDREAD, &r) != 0) return done ? done : -1;
#else
        (void)d; (void)lba; (void)dst;
        return -1;
#endif
        done += k;
    }
    return done;
}

const char *cddrive_name(const cddrive *d) { return d ? d->name : ""; }

void cddrive_close(cddrive *d)
{
    if (!d) return;
    cdd_close_handle(d->h);
    free(d);
}

#endif /* !MW2_CLASSIC_MAC */
