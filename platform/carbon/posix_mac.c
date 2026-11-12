/*
 * posix_mac.c - the game's POSIX file calls on the Mac File Manager (see include/posix_mac.h). Built WITHOUT the
 * force-included header (these are the real implementations). Files are data forks opened through FSSpecs; fopen
 * returns a stdio FILE made with funopen over the fork's reference number, so fread / fwrite / fseek / ftell / fclose
 * and buffering are the C library's. New files: type 'BINA', creator 'MW2P'.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <Carbon.h>
#include "carbon_extra.h"   /* (what Multiversal lacks) */

#include "dirent.h"

int mw2_hfs_path(const char *in, char *out, int outn);
const char *SDL_GetBasePath(void);

static OSErr spec(const char *path, FSSpec *fs)
{
    char h[512];
    Str255 p;
    if (mw2_hfs_path(path, h, (int)sizeof h) != 0) return bdNamErr;
    p[0] = (unsigned char)strlen(h); memcpy(p + 1, h, p[0]);
    return FSMakeFSSpec(0, 0, p, fs);   /* fnfErr: the folder exists, the file doesn't (fs still valid) */
}
static int err(OSErr e) { errno = e == fnfErr || e == dirNFErr ? ENOENT : e == dupFNErr ? EEXIST : e == permErr || e == wrPermErr ? EACCES : EIO; return -1; }

/* --------------------------------------------------------------------------------------------------------- files */
static int f_read(void *c, char *buf, int n) { SInt32 k = n; OSErr e = FSRead((short)(long)c, &k, buf); return e == noErr || (e == eofErr && k > 0) ? (int)k : e == eofErr ? 0 : -1; }
static int f_write(void *c, const char *buf, int n) { SInt32 k = n; return FSWrite((short)(long)c, &k, buf) == noErr ? (int)k : -1; }
static fpos_t f_seek(void *c, fpos_t off, int whence)
{
    short ref = (short)(long)c, mode = whence == SEEK_SET ? fsFromStart : whence == SEEK_CUR ? fsFromMark : fsFromLEOF;
    SInt32 pos = 0;
    if (SetFPos(ref, mode, (long)off) != noErr) return -1;
    GetFPos(ref, &pos);
    return (fpos_t)pos;
}
static int f_close(void *c) { return FSClose((short)(long)c) == noErr ? 0 : -1; }

int strcasecmp(const char *a, const char *b);   /* (in Retro68's libc; its headers don't show it) */

/* text files (.cfg .txt .env .ini .log .md) are SimpleText documents: created as TEXT / ttxt, and an existing one
 * without that type (copied from another computer) given it when opened - so a double-click opens it in SimpleText */
static int text_name(const char *path)
{
    static const char *const ext[] = {".cfg", ".txt", ".env", ".ini", ".log", ".md"};
    const char *d = strrchr(path, '.');
    size_t k;
    if (!d || strchr(d, '/') || strchr(d, ':')) return 0;
    for (k = 0; k < sizeof ext / sizeof ext[0]; k++) if (strcasecmp(d, ext[k]) == 0) return 1;
    return 0;
}
static void make_text(const FSSpec *fs)
{
    FInfo fi;
    if (FSpGetFInfo(fs, &fi) == noErr && (fi.fdType != 'TEXT' || fi.fdCreator == 0 || fi.fdCreator == '????')) {
        fi.fdType = 'TEXT';
        if (fi.fdCreator == 0 || fi.fdCreator == '????' || fi.fdCreator == 'MW2P') fi.fdCreator = 'ttxt';
        FSpSetFInfo(fs, &fi);
    }
}

FILE *mw2_fopen(const char *path, const char *mode)
{
    FSSpec fs;
    OSErr e = spec(path, &fs);
    short ref;
    int w = strchr(mode, 'w') != NULL, a = strchr(mode, 'a') != NULL, plus = strchr(mode, '+') != NULL;
    FILE *f;
    if (e != noErr && e != fnfErr) { err(e); return NULL; }
    if (e == fnfErr) {
        if (!w && !a) { err(e); return NULL; }
        if ((e = FSpCreate(&fs, text_name(path) ? 'ttxt' : 'MW2P', text_name(path) ? 'TEXT' : 'BINA', smSystemScript)) != noErr) { err(e); return NULL; }
    } else if (text_name(path)) make_text(&fs);
    if ((e = FSpOpenDF(&fs, (w || a || plus) ? fsRdWrPerm : fsRdPerm, &ref)) != noErr) { err(e); return NULL; }
    if (w) SetEOF(ref, 0);
    if (a) SetFPos(ref, fsFromLEOF, 0);
    f = funopen((void *)(long)ref, (w || a) && !plus ? NULL : f_read, (w || a || plus) ? f_write : NULL, f_seek, f_close);
    if (!f) FSClose(ref);
    return f;
}

/* --------------------------------------------------------------------------------------------- catalog calls */
static OSErr catinfo(const FSSpec *fs, CInfoPBRec *pb, Str255 name)
{
    memcpy(name, fs->name, (size_t)fs->name[0] + 1);
    memset(pb, 0, sizeof *pb);
    pb->hFileInfo.ioNamePtr = name;
    pb->hFileInfo.ioVRefNum = fs->vRefNum;
    pb->hFileInfo.ioDirID = fs->parID;
    pb->hFileInfo.ioFDirIndex = 0;
    return PBGetCatInfoSync(pb);
}
int mw2_stat(const char *path, struct stat *st)
{
    FSSpec fs;
    CInfoPBRec pb;
    Str255 name;
    OSErr e = spec(path, &fs);
    if (e != noErr) return err(e);
    if ((e = catinfo(&fs, &pb, name)) != noErr) return err(e);
    memset(st, 0, sizeof *st);
    if (pb.hFileInfo.ioFlAttrib & ioDirMask) st->st_mode = S_IFDIR | 0755;
    else { st->st_mode = S_IFREG | 0644; st->st_size = pb.hFileInfo.ioFlLgLen; }
    st->st_mtime = (time_t)(pb.hFileInfo.ioFlMdDat - 2082844800UL);   /* (1904 -> 1970) */
    return 0;
}
int mw2_mkdir(const char *path, int mode)
{
    FSSpec fs;
    SInt32 id;
    OSErr e = spec(path, &fs);
    (void)mode;
    if (e == noErr) { errno = EEXIST; return -1; }
    if (e != fnfErr) return err(e);
    return (e = FSpDirCreate(&fs, smSystemScript, &id)) == noErr ? 0 : err(e);
}
int mw2_remove(const char *path) { FSSpec fs; OSErr e = spec(path, &fs); if (e != noErr) return err(e); return (e = FSpDelete(&fs)) == noErr ? 0 : err(e); }
int mw2_rename(const char *from, const char *to)
{
    FSSpec a, b;
    OSErr e = spec(from, &a);
    if (e != noErr) return err(e);
    e = spec(to, &b);
    if (e == noErr) FSpDelete(&b);            /* POSIX: the target replaced */
    else if (e != fnfErr) return err(e);
    if (b.parID != a.parID || b.vRefNum != a.vRefNum) return err(paramErr);   /* (the game renames within a folder) */
    return (e = FSpRename(&a, b.name)) == noErr ? 0 : err(e);
}
/* an absolute '/' path: relative ones from the application's folder (the default directory) */
char *mw2_realpath(const char *path, char *resolved)
{
    char *out = resolved ? resolved : malloc(1024);
    struct stat st;
    if (!out) return NULL;
    if (path[0] == '/') snprintf(out, 1024, "%s", path);
    else snprintf(out, 1024, "%s%s", SDL_GetBasePath(), path[0] == '.' && path[1] == '/' ? path + 2 : path);
    { size_t n = strlen(out); if (n > 1 && out[n - 1] == '/') out[n - 1] = 0; }
    if (mw2_stat(out, &st) != 0) { if (!resolved) free(out); return NULL; }
    return out;
}

/* ------------------------------------------------------------------------------------------------- directories */
struct mw2_dir { short vref; long dir; short index; struct dirent ent; };
DIR *mw2_opendir(const char *path)
{
    char p[1024];
    FSSpec fs;
    CInfoPBRec pb;
    Str255 name;
    DIR *d;
    OSErr e;
    snprintf(p, sizeof p, "%s", path);
    { size_t n = strlen(p); if (n > 1 && p[n - 1] == '/') p[n - 1] = 0; }
    if ((e = spec(p, &fs)) != noErr || (e = catinfo(&fs, &pb, name)) != noErr) { err(e); return NULL; }
    if (!(pb.hFileInfo.ioFlAttrib & ioDirMask)) { errno = ENOTDIR; return NULL; }
    d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->vref = fs.vRefNum; d->dir = pb.dirInfo.ioDrDirID; d->index = 1;
    return d;
}
struct dirent *mw2_readdir(DIR *d)
{
    CInfoPBRec pb;
    Str255 name;
    memset(&pb, 0, sizeof pb);
    name[0] = 0;
    pb.hFileInfo.ioNamePtr = name;
    pb.hFileInfo.ioVRefNum = d->vref;
    pb.hFileInfo.ioDirID = d->dir;
    pb.hFileInfo.ioFDirIndex = d->index++;
    if (PBGetCatInfoSync(&pb) != noErr) return NULL;
    memcpy(d->ent.d_name, name + 1, name[0]); d->ent.d_name[name[0]] = 0;
    return &d->ent;
}
int mw2_closedir(DIR *d) { free(d); return 0; }

/* ------------------------------------------------------------------------------------------------ nanosleep */
/* (miniaudio's sleep helper; with MA_NO_THREADING nothing should call it - a plain wait on the microsecond clock) */
int nanosleep(const struct timespec *rq, struct timespec *rm)
{
    UnsignedWide a, b;
    unsigned long long start, now, want = (unsigned long long)rq->tv_sec * 1000000ULL + (unsigned long long)rq->tv_nsec / 1000ULL;
    Microseconds(&a);
    start = ((unsigned long long)a.hi << 32) | a.lo;
    do { Microseconds(&b); now = ((unsigned long long)b.hi << 32) | b.lo; } while (now - start < want);
    if (rm) { rm->tv_sec = 0; rm->tv_nsec = 0; }
    return 0;
}
