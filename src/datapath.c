/* datapath.c - see datapath.h. POSIX (Linux, macOS). */
#include "datapath.h"

#include <dirent.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define ISO_SECTOR 2048

typedef struct {
    int      is_iso;
    char     path[1024];   /* folder, or image file */
    uint32_t iso_lba;      /* ISO: extent of the root (or subdir) directory */
    uint32_t iso_size;
} dp_root;

static dp_root g_roots[DP_MAX_ROOTS];
static int     g_root_count;

/* ---- path helpers -------------------------------------------------------- */

/* Split the next component off a DOS path. Returns 0 at end, -1 on a bad or
 * escaping component, 1 when comp holds a component. */
static int next_component(const char **pp, char *comp, size_t complen)
{
    const char *p = *pp;
    size_t len;
    for (;;) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) { *pp = p; return 0; }
        len = strcspn(p, "\\/");
        if (len >= complen) return -1;
        memcpy(comp, p, len);
        comp[len] = '\0';
        p += len;
        if (strcmp(comp, ".") == 0) continue;
        if (strcmp(comp, "..") == 0) return -1; /* never escape the root */
        *pp = p;
        return 1;
    }
}

static const char *skip_drive(const char *p)
{
    return (p[0] && p[1] == ':') ? p + 2 : p;
}

/* ---- folder roots -------------------------------------------------------- */

static int match_entry(const char *dir, const char *want, char *found, size_t foundlen)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int ok = -1;
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        if (strcasecmp(e->d_name, want) == 0 && strlen(e->d_name) < foundlen) {
            strcpy(found, e->d_name);
            ok = 0;
            if (strcmp(e->d_name, want) == 0) break; /* exact case wins */
        }
    }
    closedir(d);
    return ok;
}

static int resolve_in_dir(const char *root, const char *dos_path, char *out, size_t outlen)
{
    char cur[2048], comp[256], hit[256];
    const char *p = skip_drive(dos_path);
    int r;

    if (strlen(root) >= sizeof cur) return -1;
    strcpy(cur, root);
    while ((r = next_component(&p, comp, sizeof comp)) == 1) {
        if (match_entry(cur, comp, hit, sizeof hit) != 0) return -1;
        if (strlen(cur) + 1 + strlen(hit) >= sizeof cur) return -1;
        strcat(cur, "/");
        strcat(cur, hit);
    }
    if (r < 0 || strlen(cur) >= outlen) return -1;
    strcpy(out, cur);
    return 0;
}

/* ---- ISO 9660 roots ------------------------------------------------------ */

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* "MW2.PRJ;1" and "MW2.PRJ" both match "mw2.prj"; "README." matches "readme". */
static int iso_name_eq(const unsigned char *name, size_t len, const char *want)
{
    size_t n = 0, w = strlen(want);
    while (n < len && name[n] != ';') n++;
    if (n > 0 && name[n - 1] == '.') n--;
    return n == w && strncasecmp((const char *)name, want, n) == 0;
}

/* Find want in the directory at (lba, size). Fills the entry's extent. */
static int iso_find(FILE *f, uint32_t lba, uint32_t size, const char *want,
                    uint32_t *out_lba, uint32_t *out_size, int *is_dir)
{
    unsigned char sec[ISO_SECTOR];
    uint32_t done;
    for (done = 0; done < size; done += ISO_SECTOR) {
        size_t off = 0;
        if (fseek(f, (long)(lba + done / ISO_SECTOR) * ISO_SECTOR, SEEK_SET) != 0) return -1;
        if (fread(sec, 1, ISO_SECTOR, f) != ISO_SECTOR) return -1;
        while (off < ISO_SECTOR) {
            unsigned rl = sec[off];
            if (rl == 0) break;                         /* rest of sector is padding */
            if (rl < 34 || off + rl > ISO_SECTOR) return -1;
            {
                unsigned nl = sec[off + 32];
                if (33u + nl <= rl && iso_name_eq(sec + off + 33, nl, want)) {
                    *out_lba = rd32(sec + off + 2);
                    *out_size = rd32(sec + off + 10);
                    *is_dir = (sec[off + 25] & 2) != 0;
                    return 0;
                }
            }
            off += rl;
        }
    }
    return -1;
}

/* Walk dos_path from the root directory. */
static int iso_lookup(FILE *f, uint32_t lba, uint32_t size, const char *dos_path,
                      uint32_t *out_lba, uint32_t *out_size, int *is_dir)
{
    char comp[256];
    const char *p = skip_drive(dos_path);
    int r, dir = 1;
    while ((r = next_component(&p, comp, sizeof comp)) == 1) {
        if (!dir) return -1;
        if (iso_find(f, lba, size, comp, &lba, &size, &dir) != 0) return -1;
    }
    if (r < 0) return -1;
    *out_lba = lba;
    *out_size = size;
    *is_dir = dir;
    return 0;
}

int dp_add_iso_root(const char *iso_path, const char *subdir)
{
    unsigned char pvd[ISO_SECTOR];
    dp_root *r;
    FILE *f;
    int dir = 0, ok;

    if (!iso_path || !*iso_path || g_root_count >= DP_MAX_ROOTS) return -1;
    if (strlen(iso_path) >= sizeof g_roots[0].path) return -1;
    f = fopen(iso_path, "rb");
    if (!f) return -1;
    r = &g_roots[g_root_count];
    ok = fseek(f, 16L * ISO_SECTOR, SEEK_SET) == 0 && fread(pvd, 1, ISO_SECTOR, f) == ISO_SECTOR &&
         pvd[0] == 1 && memcmp(pvd + 1, "CD001", 5) == 0;
    if (ok) {
        r->iso_lba = rd32(pvd + 156 + 2);    /* root directory record */
        r->iso_size = rd32(pvd + 156 + 10);
        if (subdir && *subdir)
            ok = iso_lookup(f, r->iso_lba, r->iso_size, subdir, &r->iso_lba, &r->iso_size, &dir) == 0 && dir;
    }
    fclose(f);
    if (!ok) return -1;
    r->is_iso = 1;
    strcpy(r->path, iso_path);
    g_root_count++;
    return 0;
}

/* ---- public API ---------------------------------------------------------- */

int dp_add_root(const char *dir)
{
    struct stat st;
    size_t n;
    dp_root *r;
    if (!dir || !*dir || g_root_count >= DP_MAX_ROOTS) return -1;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    n = strlen(dir);
    if (n >= sizeof g_roots[0].path) return -1;
    r = &g_roots[g_root_count];
    memset(r, 0, sizeof *r);
    memcpy(r->path, dir, n + 1);
    while (n > 1 && r->path[n - 1] == '/') r->path[--n] = '\0';
    g_root_count++;
    return 0;
}

void dp_clear_roots(void) { g_root_count = 0; }

void dp_add_roots_from_env(void)
{
    static int done;
    if (done) return;   /* idempotent: the mission loader and the sim both call it */
    done = 1;
    const char *img = getenv("MW2_CD_IMAGE");
    const char *cd = getenv("MW2_CD_DIR");
    dp_add_root(getenv("MW2_INSTALL_DIR"));
    if (cd) {
        char sub[1100];
        snprintf(sub, sizeof sub, "%s/MECH2", cd);
        dp_add_root(sub);      /* game files live in \\MECH2 on the disc */
        dp_add_root(cd);
    }
    if (img) {
        dp_add_iso_root(img, "MECH2");
        dp_add_iso_root(img, NULL);
    }
}

int dp_resolve(const char *dos_path, char *out, size_t outlen)
{
    int i;
    for (i = 0; i < g_root_count; i++)
        if (!g_roots[i].is_iso && resolve_in_dir(g_roots[i].path, dos_path, out, outlen) == 0) return 0;
    return -1;
}

int dp_read_file(const char *dos_path, unsigned char **data, size_t *len)
{
    int i;
    *data = NULL;
    *len = 0;
    for (i = 0; i < g_root_count; i++) {
        const dp_root *r = &g_roots[i];
        unsigned char *buf;
        size_t n;
        FILE *f;

        if (r->is_iso) {
            uint32_t lba, size;
            int dir;
            f = fopen(r->path, "rb");
            if (!f) continue;
            if (iso_lookup(f, r->iso_lba, r->iso_size, dos_path, &lba, &size, &dir) != 0 || dir ||
                fseek(f, (long)lba * ISO_SECTOR, SEEK_SET) != 0) {
                fclose(f);
                continue;
            }
            n = size;
        } else {
            char path[2048];
            struct stat st;
            if (resolve_in_dir(r->path, dos_path, path, sizeof path) != 0) continue;
            if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) continue;
            f = fopen(path, "rb");
            if (!f) continue;
            n = (size_t)st.st_size;
        }
        buf = malloc(n ? n : 1);
        if (!buf || fread(buf, 1, n, f) != n) { free(buf); fclose(f); return -1; }
        fclose(f);
        *data = buf;
        *len = n;
        return 0;
    }
    return -1;
}

FILE *dp_fopen(const char *dos_path, const char *mode)
{
    char path[2048];
    int i;
    if (dp_resolve(dos_path, path, sizeof path) == 0) return fopen(path, mode);
    if (mode[0] == 'w' || mode[0] == 'a') {
        /* new file: create it in the first folder root, keeping the DOS name */
        for (i = 0; i < g_root_count; i++) {
            const char *p = skip_drive(dos_path);
            char comp[256];
            const char *q = p;
            size_t k, n;
            int r;
            if (g_roots[i].is_iso) continue;
            while ((r = next_component(&q, comp, sizeof comp)) == 1) {}
            if (r < 0) return NULL;
            while (*p == '\\' || *p == '/') p++;
            n = (size_t)snprintf(path, sizeof path, "%s/%s", g_roots[i].path, p);
            if (n >= sizeof path) return NULL;
            for (k = strlen(g_roots[i].path); path[k]; k++)
                if (path[k] == '\\') path[k] = '/';
            return fopen(path, mode);
        }
    }
    return NULL;
}
