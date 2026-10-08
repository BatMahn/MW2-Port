#include "portcfg.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"   /* paths are bounded by PORTCFG_PATH by design */
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>

void portcfg_defaults(portcfg *c) { memset(c, 0, sizeof *c); c->mode = 0; c->aa = 4; c->textures_ati = 1; c->speed = 0.85f; }

static int exists(const char *p) { struct stat st; return p && p[0] && stat(p, &st) == 0; }
static int is_dir(const char *p) { struct stat st; return p && p[0] && stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static void mkdirs(const char *path)
{
    char tmp[PORTCFG_PATH], *q;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (q = tmp + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(tmp, 0755); *q = '/'; }
    mkdir(tmp, 0755);
}
/* a file in a folder, matching the name case-insensitively (DOS names) */
static int find_ci(const char *dir, const char *name, char *out, size_t outlen)
{
    char p[PORTCFG_PATH * 3];
    char lower[256], upper[256];
    size_t i;
    for (i = 0; name[i] && i < 255; i++) { lower[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]); upper[i] = (char)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]); }
    lower[i] = upper[i] = 0;
    snprintf(p, sizeof p, "%s/%s", dir, name); if (exists(p)) { snprintf(out, outlen, "%s", p); return 1; }
    snprintf(p, sizeof p, "%s/%s", dir, upper); if (exists(p)) { snprintf(out, outlen, "%s", p); return 1; }
    snprintf(p, sizeof p, "%s/%s", dir, lower); if (exists(p)) { snprintf(out, outlen, "%s", p); return 1; }
    return 0;
}

const char *portcfg_file(void)
{
    static char path[PORTCFG_PATH];
    const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
    if (getenv("MW2_CONFIG")) { snprintf(path, sizeof path, "%s", getenv("MW2_CONFIG")); return path; }   /* tests */
#if defined(__APPLE__)
    snprintf(path, sizeof path, "%s/Library/Application Support/mw2port/mw2port.cfg", home ? home : ".");
    (void)xdg;
#else
    if (xdg && xdg[0]) snprintf(path, sizeof path, "%s/mw2port/mw2port.cfg", xdg);
    else snprintf(path, sizeof path, "%s/.config/mw2port/mw2port.cfg", home ? home : ".");
#endif
    return path;
}

int portcfg_load(const char *unused, portcfg *c)
{
    char line[PORTCFG_PATH + 32];
    FILE *f;
    (void)unused;
    portcfg_defaults(c);
    if (!(f = fopen(portcfg_file(), "r"))) return -1;
    while (fgets(line, sizeof line, f)) {
        char *v = strchr(line, '=');
        if (!v || line[0] == '#') continue;
        *v++ = 0;
        v[strcspn(v, "\r\n")] = 0;
#define STR(k) else if (strcmp(line, #k) == 0) snprintf(c->k, sizeof c->k, "%s", v)
        if (strcmp(line, "resolution") == 0) {
            if (strcmp(v, "desktop") == 0) c->mode = 0;
            else if (strcmp(v, "window") == 0) c->mode = 1;
            else if (sscanf(v, "%dx%d", &c->w, &c->h) == 2 && c->w > 0 && c->h > 0) c->mode = 2;
        } else if (strcmp(line, "speed") == 0) {
            float sp = (float)atof(v);
            if (sp >= 0.5f && sp <= 1.5f) c->speed = sp;
        } else if (strcmp(line, "render") == 0) {
            int rw = 0, rh = 0;
            if (sscanf(v, "%dx%d", &rw, &rh) == 2 && ((rw == 320 && rh == 200) || (rw == 640 && rh == 480) || (rw == 1024 && rh == 768))) { c->render_w = rw; c->render_h = rh; }
            else c->render_w = c->render_h = 0;
        } else if (strcmp(line, "aa") == 0) {
            int a = atoi(v);
            c->aa = a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : 0;
        } else if (strcmp(line, "textures_kind") == 0) c->textures_ati = strcmp(v, "3dfx") != 0;
        else if (strcmp(line, "edition") == 0) {
            int e;
            for (e = 0; e < PORTCFG_ED_COUNT; e++) if (strcmp(v, PORTCFG_ED_NAME[e]) == 0) c->edition = e;
        }
        STR(game); STR(install); STR(cd); STR(music); STR(ultrasnd); STR(mt32); STR(models); STR(textures); STR(skygnd); STR(skygnd_fog);
#undef STR
    }
    fclose(f);
    return 0;
}

int portcfg_save(const char *unused, const portcfg *c)
{
    char dir[PORTCFG_PATH], *slash;
    FILE *f;
    (void)unused;
    snprintf(dir, sizeof dir, "%s", portcfg_file());
    if ((slash = strrchr(dir, '/'))) { *slash = 0; mkdirs(dir); }
    if (!(f = fopen(portcfg_file(), "w"))) return -1;
    fprintf(f, "# MechWarrior 2 port settings (see portcfg.h). Paths left out are taken from the game folder.\n");
    if (c->game[0]) fprintf(f, "game=%s\n", c->game);
#define OPT(k) if (c->k[0] && !c->game[0]) fprintf(f, #k "=%s\n", c->k)
    OPT(install); OPT(cd); OPT(music); OPT(ultrasnd); OPT(mt32); OPT(models); OPT(textures); OPT(skygnd); OPT(skygnd_fog);
#undef OPT
    if (c->mode == 0) fprintf(f, "resolution=desktop\n");
    else if (c->mode == 1) fprintf(f, "resolution=window\n");
    else fprintf(f, "resolution=%dx%d\n", c->w, c->h);
    fprintf(f, "aa=%d\n", c->aa);
    if (c->render_w) fprintf(f, "render=%dx%d\n", c->render_w, c->render_h); else fprintf(f, "render=native\n");
    fprintf(f, "speed=%.2f\n", (double)(c->speed > 0 ? c->speed : 0.85f));
    fprintf(f, "edition=%s\n", PORTCFG_ED_NAME[c->edition >= 0 && c->edition < PORTCFG_ED_COUNT ? c->edition : 0]);
    fclose(f);
    return 0;
}

int portcfg_is_game_dir(const char *dir)
{
    char p[PORTCFG_PATH * 2], tmp[PORTCFG_PATH];
    if (!is_dir(dir)) return 0;
    snprintf(p, sizeof p, "%s/install", dir);
    if (is_dir(p)) return 1;                                            /* a game folder */
    return find_ci(dir, "DATABASE.MW2", tmp, sizeof tmp) && find_ci(dir, "MW2.PRJ", tmp, sizeof tmp);   /* a plain install */
}

int portcfg_resolve(portcfg *c, const char *exe_dir, const char *hint)
{
    char p[PORTCFG_PATH * 2];
    const char *home = getenv("HOME");
    if (hint && portcfg_is_game_dir(hint)) snprintf(c->game, sizeof c->game, "%s", hint);
    if (!c->game[0] && !c->install[0]) {   /* look for it */
        char cand[5][PORTCFG_PATH];
        int i;
        snprintf(cand[0], PORTCFG_PATH, ".");
        snprintf(cand[1], PORTCFG_PATH, "%sMW2-game", exe_dir ? exe_dir : "");
        snprintf(cand[2], PORTCFG_PATH, "%s../MW2-game", exe_dir ? exe_dir : "");
        snprintf(cand[3], PORTCFG_PATH, "%s/MW2-game", home ? home : ".");
        snprintf(cand[4], PORTCFG_PATH, "%s", exe_dir ? exe_dir : ".");
        for (i = 0; i < 5 && !c->game[0]; i++)
            if (portcfg_is_game_dir(cand[i])) {
                char *r = realpath(cand[i], NULL);
                snprintf(c->game, sizeof c->game, "%s", r ? r : cand[i]);
                free(r);
            }
    }
    if (c->game[0]) {   /* the folder's parts, unless overridden */
        snprintf(p, sizeof p, "%s/install", c->game);
        if (!c->install[0]) snprintf(c->install, sizeof c->install, "%s", is_dir(p) ? p : c->game);   /* a plain install is its own */
        snprintf(p, sizeof p, "%s/cd", c->game);
        if (!c->cd[0] && is_dir(p)) snprintf(c->cd, sizeof c->cd, "%s", p);
        snprintf(p, sizeof p, "%s/music", c->game);
        if (!c->music[0] && is_dir(p)) snprintf(c->music, sizeof c->music, "%s", p);
        snprintf(p, sizeof p, "%s/ultrasnd", c->game);
        if (!c->ultrasnd[0] && is_dir(p)) snprintf(c->ultrasnd, sizeof c->ultrasnd, "%s", p);
        snprintf(p, sizeof p, "%s/mt32", c->game);
        if (!c->mt32[0] && is_dir(p)) snprintf(c->mt32, sizeof c->mt32, "%s", p);
        snprintf(p, sizeof p, "%s/3d/models.prj", c->game);
        if (!c->models[0] && exists(p)) snprintf(c->models, sizeof c->models, "%s", p);
        snprintf(p, sizeof p, "%s/3d/textures.prj", c->game);
        if (!c->textures[0] && exists(p)) snprintf(c->textures, sizeof c->textures, "%s", p);
        snprintf(p, sizeof p, "%s/3d/skygnd.par", c->game);
        if (!c->skygnd[0] && exists(p)) snprintf(c->skygnd, sizeof c->skygnd, "%s", p);
        snprintf(p, sizeof p, "%s/3d/skygnd_pvr.par", c->game);
        if (!c->skygnd_fog[0] && exists(p)) snprintf(c->skygnd_fog, sizeof c->skygnd_fog, "%s", p);
        snprintf(p, sizeof p, "%s/3d/pvr/skygnd.par", c->game);   /* the PowerVR edition's own file: the fog densities */
        if (!c->skygnd_fog[0] && exists(p)) snprintf(c->skygnd_fog, sizeof c->skygnd_fog, "%s", p);
    }
    /* without the 3D editions' files: the DOS archive (models only, untextured) */
    if (!c->models[0] && c->install[0]) {
        if (!find_ci(c->install, "MW2.PRJ", c->models, sizeof c->models) && c->cd[0]) {
            snprintf(p, sizeof p, "%s/MECH2", c->cd);
            if (!find_ci(p, "MW2.PRJ", c->models, sizeof c->models)) find_ci(c->cd, "MW2.PRJ", c->models, sizeof c->models);
        }
    }
    if (!c->textures[0]) snprintf(c->textures, sizeof c->textures, "%s", c->models);
    (void)errno;
    return c->install[0] ? 0 : -1;
}

const char *const PORTCFG_ED_NAME[PORTCFG_ED_COUNT] = {"enhanced", "dos", "3dfx", "ati", "s3", "pvr", "mga"};

static void cp(char *dst, const char *src) { snprintf(dst, PORTCFG_PATH, "%s", src ? src : ""); }

int portcfg_edition(const portcfg *c, int ed, char *models, char *textures, char *kind, char *sky, char *fog)
{
    char a[PORTCFG_PATH * 2], b[PORTCFG_PATH * 2];
    /* the enhanced mix: the 3Dfx archive's models and data, the ATi archive's textures (the best-kept copy), the 3Dfx sky
     * file, the PowerVR fog */
    cp(models, c->models); cp(textures, c->textures); cp(kind, c->textures_ati ? "ati" : "3dfx"); cp(sky, c->skygnd); cp(fog, c->skygnd_fog);
    if (ed == PORTCFG_ED_ENHANCED) return 0;
    if (ed == PORTCFG_ED_DOS) {   /* the DOS archive (the install's, else the disc's) */
        if (!find_ci(c->install, "MW2.PRJ", a, sizeof a)) {
            snprintf(b, sizeof b, "%s/MECH2", c->cd);
            if (!c->cd[0] || (!find_ci(b, "MW2.PRJ", a, sizeof a) && !find_ci(c->cd, "MW2.PRJ", a, sizeof a))) return -1;
        }
        cp(models, a); cp(textures, a); cp(kind, "dos"); cp(sky, ""); cp(fog, "");
        return 0;
    }
    if (!c->game[0]) return -1;
    if (ed == PORTCFG_ED_3DFX) {
        if (!c->models[0]) return -1;
        cp(textures, c->models); cp(kind, "3dfx"); cp(fog, "");
        return 0;
    }
    if (ed == PORTCFG_ED_ATI) {
        snprintf(a, sizeof a, "%s/3d/ati/skygnd.par", c->game);
        if (!c->textures[0] || !c->textures_ati) return -1;
        cp(models, c->textures); cp(kind, "ati"); cp(fog, "");
        if (exists(a)) cp(sky, a);
        return 0;
    }
    {
        const char *dir = ed == PORTCFG_ED_S3 ? "s3" : ed == PORTCFG_ED_MGA ? "mga" : "pvr";
        snprintf(a, sizeof a, "%s/3d/%s/MW2.PRJ", c->game, dir);
        snprintf(b, sizeof b, "%s/3d/%s/skygnd.par", c->game, dir);
    }
    if (!exists(a)) return -1;
    cp(models, a); cp(textures, a); cp(sky, exists(b) ? b : c->skygnd);
    /* the S3 edition's textures are the 3Dfx edition's, byte for byte; the Mystique's RGB555 with 0x0000 keyed */
    cp(kind, ed == PORTCFG_ED_S3 ? "3dfx" : ed == PORTCFG_ED_MGA ? "mga" : "pvr");
    cp(fog, ed == PORTCFG_ED_PVR && exists(b) ? b : "");
    return 0;
}
