/* posix_mac_path.c - '/' paths to HFS paths (portable: tested on the host by tests in docs/CARBON.md) */
#include <string.h>
/* "/Vol/a/b" -> "Vol:a:b"; "a/b" -> ":a:b"; "./a" -> ":a"; "../a" -> "::a"; "a/" -> ":a:"; "/" -> "" (no volume) */
int mw2_hfs_path(const char *in, char *out, int outn)
{
    int n = 0, abs = in[0] == '/';
    const char *p = in + abs;
    if (!abs) { if (n + 1 >= outn) return -1; out[n++] = ':'; }
    while (*p) {
        const char *e = strchr(p, '/');
        int len = e ? (int)(e - p) : (int)strlen(p);
        if (len == 0) { p = e ? e + 1 : p + len; continue; }                   /* "//" */
        if (len == 1 && p[0] == '.') { p = e ? e + 1 : p + len; continue; }    /* "." */
        if (len == 2 && p[0] == '.' && p[1] == '.') {                          /* ".." : one more colon */
            if (n + 1 >= outn) return -1;
            out[n++] = ':';
        } else {
            if (n + len + 1 >= outn) return -1;
            if (n > 0 && out[n - 1] != ':') out[n++] = ':';
            memcpy(out + n, p, (size_t)len); n += len;
        }
        if (!e) break;
        p = e + 1;
        if (!*p) { if (n + 1 >= outn) return -1; out[n++] = ':'; }            /* trailing '/': a folder */
    }
    out[n] = 0;
    return n > 255 ? -1 : 0;   /* (a Str255 holds the File Manager's paths) */
}
