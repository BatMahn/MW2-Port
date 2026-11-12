/*
 * rb_gl12.c - rb.h on OpenGL 1.2's fixed-function pipeline (the "legacy" backend: make ... RB=gl12). See
 * docs/RENDERER_GL12.md; the programs (the world's and the HUD's) are carried out in rb_gl12_world.c.
 *
 * This file: the context (SDL's OpenGL window), textures (a CPU copy of each, and the GL textures made from it - the
 * variants a per-texel step of the shaders needs, tiles for textures that aren't a power of two or are larger than
 * the card takes), vertex buffers (CPU memory), off-screen targets (drawn in the back buffer, kept in textures with
 * glCopyTexSubImage2D while another target is drawn), the GL state and the batching of the triangles the programs
 * make. Calls run at once (OpenGL's own order); nothing is recorded.
 *
 * Needs OpenGL 1.2 - or 1.1 with GL_EXT_texture_edge_clamp / GL_SGIS_texture_edge_clamp (then the Matrox look's
 * per-band mip levels, GL_TEXTURE_BASE/MAX_LEVEL, are not used). Optional: GL_ARB_multitexture (two units: the fog
 * in the same pass; one unit: a second pass). Only OpenGL 1.1 functions are linked.
 * Test switches: MW2_GL12_MAXTEX=n (the largest texture the card takes, e.g. 256 as a Voodoo 3), MW2_GL12_NO_MULTITEX=1,
 * MW2_GL12_NOFOG=1, MW2_GL12_TRACE=1 (textures padded / resampled / tiled, the context).
 */
#include "rb_gl12.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (APIENTRY *pfn_active_tex)(GLenum);

gl12_state G;
gl12_tex *g12_tex;
int g12_ntex;
int g12_maxtex = 256, g12_units = 1, g12_lodok, g12_trace, g12_nofog;

static int g_captex;
static gl12_buf *g_buf;
static int g_nbuf, g_capbuf;
static uint8_t *g_st;            /* rb_stream: the frame's transient vertices */
static size_t g_stn, g_stcap;
static SDL_Window *g_win, *g_own_win;
static SDL_GLContext g_ctx;
static char g_devname[256];
static pfn_active_tex p_active, p_client_active;
static rb_tex g_win_tex;         /* the window's picture (a target whose store keeps it while a target is drawn) */
static int g_win_w = 1, g_win_h = 1;
static rb_tex g_res;             /* the target whose picture is in the back buffer now */
static int g_dirty;              /* drawn into since it was put there */
static GLuint g_fogtab, g_fogramp, g_ramp;   /* 1D textures: the PowerVR fog table, the fog ramp, a luminance ramp */
static float g_fogrgb[3] = {-1, -1, -1};
static int g_fogn = 2048;
static int g_bigwarn;

static void *grow(void *p, int *cap, int need, size_t elem)
{
    if (need <= *cap) return p;
    {
        int nc = *cap ? *cap : 64;
        void *q;
        while (nc < need) nc *= 2;
        q = realloc(p, (size_t)nc * elem);
        if (!q) { fprintf(stderr, "rb: out of memory\n"); exit(1); }
        *cap = nc;
        return q;
    }
}

gl12_tex *gl12_T(rb_tex t) { return t > 0 && (int)t < g12_ntex && g12_tex[t].live ? &g12_tex[t] : NULL; }
static gl12_buf *B(rb_buf b) { return b > 0 && (int)b < g_nbuf && g_buf[b].live ? &g_buf[b] : NULL; }
static int pot(int v) { int p = 1; while (p < v) p <<= 1; return p; }
static void tex_gone(GLuint id);   /* (the video-memory count, below) */

/* ---- the GL state cache (the calls made only on a change) */
static struct {
    int init;
    GLuint bound[2];
    int t2d[2], t1d[2];    /* GL_TEXTURE_2D / 1D enabled per unit */
    int env[2];
    int carr1;             /* unit 1's texture coordinate array enabled */
    int blend, alpha_func; float alpha_ref;
    int dtest, dwrite, dfunc, stipple, offset, sc_on, sc[4], vp[4];
    float slope, units;
    int unit, cunit;
} C;

static void active(int u) { if (p_active && C.unit != u) { p_active(GL_TEXTURE0_ARB + (GLenum)u); C.unit = u; } }
static void client_active(int u) { if (p_client_active && C.cunit != u) { p_client_active(GL_TEXTURE0_ARB + (GLenum)u); C.cunit = u; } }
static void tex_enable(int u, int d2, int d1)
{
    if (C.t2d[u] == d2 && C.t1d[u] == d1) return;
    active(u);
    if (C.t2d[u] != d2) { if (d2) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D); C.t2d[u] = d2; }
    if (C.t1d[u] != d1) { if (d1) glEnable(GL_TEXTURE_1D); else glDisable(GL_TEXTURE_1D); C.t1d[u] = d1; }
}
static void tex_env(int u, int mode) { if (C.env[u] != mode) { active(u); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, mode); C.env[u] = mode; } }
static void bind2d(int u, GLuint id) { active(u); glBindTexture(GL_TEXTURE_2D, id); C.bound[u] = id; }
static void bind1d(int u, GLuint id) { active(u); glBindTexture(GL_TEXTURE_1D, id); C.bound[u] = id; }

static void set_blend(int mode)
{
    if (C.blend == mode) return;
    C.blend = mode;
    if (mode == RB_BLEND_OFF) { glDisable(GL_BLEND); return; }
    glEnable(GL_BLEND);
    if (mode == RB_BLEND_ALPHA) glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    else if (mode == RB_BLEND_MUL) glBlendFunc(GL_DST_COLOR, GL_ZERO);
    else if (mode == RB_BLEND_ADD) glBlendFunc(GL_ONE, GL_ONE);
    else glBlendFunc(GL_SRC_ALPHA, GL_ONE);   /* (internal: 4, the flash's additive pass) */
}
static void set_alpha(int func, float ref)
{
    if (C.alpha_func == func && (func == 0 || C.alpha_ref == ref)) return;
    if (!func) glDisable(GL_ALPHA_TEST);
    else { if (!C.alpha_func) glEnable(GL_ALPHA_TEST); glAlphaFunc((GLenum)func, ref); }
    C.alpha_func = func; C.alpha_ref = ref;
}
static void set_depth(int test, int write, int func)
{
    if (C.dtest != test) { if (test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST); C.dtest = test; }
    if (C.dwrite != write) { glDepthMask(write ? GL_TRUE : GL_FALSE); C.dwrite = write; }
    if (C.dfunc != func) { glDepthFunc((GLenum)func); C.dfunc = func; }
}
static void set_stipple(int s)
{
    static const GLubyte *cur;
    static GLubyte pat[2][128];
    if (C.stipple == s) return;
    if (!pat[0][0]) {
        int y, b;
        for (y = 0; y < 32; y++)
            for (b = 0; b < 4; b++) { pat[0][y * 4 + b] = (GLubyte)(y & 1 ? 0x55 : 0xAA); pat[1][y * 4 + b] = (GLubyte)(y & 1 ? 0xAA : 0x55); }
    }
    if (!s) glDisable(GL_POLYGON_STIPPLE);
    else {
        /* the first byte's top bit is window x 0 (GL_UNPACK_LSB_FIRST false): pattern 0 sets the pixels with x + y even */
        if (cur != pat[s - 1]) { glPolygonStipple(pat[s - 1]); cur = pat[s - 1]; }
        if (!C.stipple) glEnable(GL_POLYGON_STIPPLE);
    }
    C.stipple = s;
}
static void set_offset(int on, float slope, float units)
{
    if (C.offset == on && (!on || (C.slope == slope && C.units == units))) return;
    if (!on) glDisable(GL_POLYGON_OFFSET_FILL);
    else { if (!C.offset) glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(slope, units); }
    C.offset = on; C.slope = slope; C.units = units;
}
static void set_scissor(int on, int x, int y, int w, int h)
{
    if (C.sc_on != on) { if (on) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST); C.sc_on = on; }
    if (on && (C.sc[0] != x || C.sc[1] != y || C.sc[2] != w || C.sc[3] != h)) {
        glScissor(x, y, w > 0 ? w : 0, h > 0 ? h : 0);
        C.sc[0] = x; C.sc[1] = y; C.sc[2] = w; C.sc[3] = h;
    }
}
static void set_viewport(int x, int y, int w, int h)
{
    if (C.vp[0] != x || C.vp[1] != y || C.vp[2] != w || C.vp[3] != h) {
        glViewport(x, y, w, h);
        C.vp[0] = x; C.vp[1] = y; C.vp[2] = w; C.vp[3] = h;
    }
}
static void cache_reset(void)
{
    memset(&C, 0, sizeof C);
    C.blend = -1; C.alpha_func = -1; C.dtest = -1; C.dwrite = -1; C.dfunc = -1; C.stipple = -1; C.offset = -1; C.sc_on = -1;
    C.vp[2] = -1; C.unit = -1; C.cunit = -1; C.carr1 = -1;
    C.t2d[0] = C.t2d[1] = C.t1d[0] = C.t1d[1] = -1;
    C.env[0] = C.env[1] = -1;
    C.init = 1;
}

/* ---- device */
const char *rb_backend_name(void) { return g_ctx ? "OpenGL" : "none"; }
int rb_backend(void) { return RB_GPU_OPENGL; }
const char *rb_device_name(void) { return g_devname; }

uint64_t rb_window_flags(int low)
{
    /* 8-8-8 colour and a 24-bit depth buffer; low: 5-6-5 and 16 (cards / desktops of that era with no 24-bit modes) */
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, low ? 5 : 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, low ? 6 : 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, low ? 5 : 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, low ? 16 : 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    return SDL_WINDOW_OPENGL;
}

static int has_ext(const char *list, const char *name)
{
    size_t n = strlen(name);
    const char *p = list;
    while (p && (p = strstr(p, name)) != NULL) {
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return 1;
        p += n;
    }
    return 0;
}

int rb_init(SDL_Window *win, int pref, int samples, char *err, size_t errn)
{
    const char *ver, *ext, *ren, *ven, *env;
    int major = 0, minor = 0, edge;
    (void)pref; (void)samples;   /* (one API here; no multisampling) */
    if (err && errn) err[0] = 0;
    if (!win) {   /* (glshot: no window of its own - a hidden one, sized by rb_frame) */
        rb_window_flags(0);
        g_own_win = win = SDL_CreateWindow("rb", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (!win) { rb_window_flags(1); g_own_win = win = SDL_CreateWindow("rb", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN); }
        if (!win) { if (err && errn) SDL_snprintf(err, errn, "OpenGL window: %s", SDL_GetError()); return -1; }
    }
    /* (no context version asked: SDL's default makes a legacy context - an OpenGL 1.x driver gives what it has) */
    g_ctx = SDL_GL_CreateContext(win);
    if (!g_ctx) { if (err && errn) SDL_snprintf(err, errn, "OpenGL context: %s", SDL_GetError()); return -1; }
    SDL_GL_MakeCurrent(win, g_ctx);
    g_win = win;
    ver = (const char *)glGetString(GL_VERSION);
    ext = (const char *)glGetString(GL_EXTENSIONS);
    ren = (const char *)glGetString(GL_RENDERER);
    ven = (const char *)glGetString(GL_VENDOR);
    if (ver) sscanf(ver, "%d.%d", &major, &minor);
    edge = has_ext(ext, "GL_EXT_texture_edge_clamp") || has_ext(ext, "GL_SGIS_texture_edge_clamp");
    if (major < 1 || (major == 1 && minor < 2 && !(minor == 1 && edge))) {
        if (err && errn) SDL_snprintf(err, errn, "OpenGL %s (%s): needs OpenGL 1.2, or 1.1 with GL_EXT_texture_edge_clamp", ver ? ver : "?", ren ? ren : "?");
        SDL_GL_DestroyContext(g_ctx); g_ctx = NULL;
        return -1;
    }
    g12_lodok = major > 1 || minor >= 2;   /* GL_TEXTURE_BASE/MAX_LEVEL (the Matrox look's bands) */
    {
        GLint mt = 256, u = 1;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mt);
        g12_maxtex = mt >= 64 ? mt : 64;
        if ((env = getenv("MW2_GL12_MAXTEX")) && atoi(env) >= 16) { int m = pot(atoi(env)); if (m < g12_maxtex) g12_maxtex = m; }
        if (has_ext(ext, "GL_ARB_multitexture") && !getenv("MW2_GL12_NO_MULTITEX")) {
            p_active = (pfn_active_tex)(uintptr_t)SDL_GL_GetProcAddress("glActiveTextureARB");
            p_client_active = (pfn_active_tex)(uintptr_t)SDL_GL_GetProcAddress("glClientActiveTextureARB");
            if (p_active && p_client_active) glGetIntegerv(GL_MAX_TEXTURE_UNITS_ARB, &u);
            if (u < 2 || !p_active || !p_client_active) { p_active = p_client_active = NULL; u = 1; }
        }
        g12_units = u >= 2 ? 2 : 1;
#ifdef MW2_CLASSIC_MAC
        if (g12_units < 2 && !getenv("MW2_GL12_ALLOW_ONE_UNIT")) {   /* the classic Mac build's floor: an ATI Rage 128 */
            if (err && errn) SDL_snprintf(err, errn, "MechWarrior 2 needs an ATI Rage 128 or newer graphics card (this one, %s, has one texture unit)", ren ? ren : "?");
            SDL_GL_DestroyContext(g_ctx); g_ctx = NULL;
            return -1;
        }
#endif
    }
    g12_trace = getenv("MW2_GL12_TRACE") != NULL;
    g12_nofog = getenv("MW2_GL12_NOFOG") != NULL;
    g_fogn = g12_maxtex < 2048 ? g12_maxtex : 2048;
    {   /* vsync (none on SDL's offscreen driver: the tests) */
        const char *vd = SDL_GetCurrentVideoDriver();
        SDL_GL_SetSwapInterval(vd && strcmp(vd, "offscreen") == 0 ? 0 : 1);
    }
    {
        GLint rb = 0, gb = 0, bb = 0, db = 0;
        glGetIntegerv(GL_RED_BITS, &rb); glGetIntegerv(GL_GREEN_BITS, &gb); glGetIntegerv(GL_BLUE_BITS, &bb); glGetIntegerv(GL_DEPTH_BITS, &db);
        SDL_snprintf(g_devname, sizeof g_devname, "%s, %s, OpenGL %s; %d-%d-%d colour, %d-bit depth, textures up to %d, %d unit%s",
                     ren ? ren : "?", ven ? ven : "?", ver ? ver : "?", (int)rb, (int)gb, (int)bb, (int)db, g12_maxtex, g12_units, g12_units > 1 ? "s" : "");
    }
    g12_ntex = g_nbuf = 1;   /* handle 0: none */
    g12_tex = grow(g12_tex, &g_captex, 1, sizeof *g12_tex);
    g_buf = grow(g_buf, &g_capbuf, 1, sizeof *g_buf);
    memset(&g12_tex[0], 0, sizeof g12_tex[0]);
    memset(&g_buf[0], 0, sizeof g_buf[0]);
    memset(&G, 0, sizeof G);
    G.dtest = 0; G.dwrite = 1; G.dfunc = RB_LESS;
    cache_reset();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_CULL_FACE); glDisable(GL_LIGHTING); glDisable(GL_DITHER); glDisable(GL_FOG);
    glShadeModel(GL_SMOOTH);
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
    glDepthRange(0.0, 1.0);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    client_active(0);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    printf("gpu: OpenGL (%s)\n", g_devname);
    fflush(stdout);
    if (g12_trace) fprintf(stderr, "gl12: extensions %s\n", ext ? ext : "");
    return 0;
}

static void var_free(gl12_var *v)
{
    while (v) {
        gl12_var *n = v->next;
        int k;
        for (k = 0; k < v->ntx * v->nty; k++) if (v->t[k].id) { tex_gone(v->t[k].id); glDeleteTextures(1, &v->t[k].id); }
        free(v->t);
        free(v);
        v = n;
    }
}

void rb_quit(void)
{
    int k;
    if (!g_ctx) return;
    for (k = 1; k < g12_ntex; k++) if (g12_tex[k].live) rb_tex_delete((rb_tex)k);
    for (k = 1; k < g_nbuf; k++) if (g_buf[k].live) rb_buf_delete((rb_buf)k);
    if (g_fogtab) glDeleteTextures(1, &g_fogtab);
    if (g_fogramp) glDeleteTextures(1, &g_fogramp);
    if (g_ramp) glDeleteTextures(1, &g_ramp);
    g_fogtab = g_fogramp = g_ramp = 0;
    SDL_GL_DestroyContext(g_ctx);
    g_ctx = NULL;
    if (g_own_win) SDL_DestroyWindow(g_own_win);
    g_own_win = NULL;
    free(g12_tex); g12_tex = NULL; g12_ntex = g_captex = 0;
    free(g_buf); g_buf = NULL; g_nbuf = g_capbuf = 0;
    free(g_st); g_st = NULL; g_stn = g_stcap = 0;
    g_win_tex = g_res = 0;
}

/* ---- textures */
rb_tex rb_tex_new(void)
{
    int k;
    for (k = 1; k < g12_ntex; k++) if (!g12_tex[k].live) break;
    if (k == g12_ntex) { g12_tex = grow(g12_tex, &g_captex, g12_ntex + 1, sizeof *g12_tex); g12_ntex++; }
    memset(&g12_tex[k], 0, sizeof g12_tex[k]);
    g12_tex[k].live = 1;
    g12_tex[k].min = RB_NEAREST_MIPMAP_LINEAR; g12_tex[k].mag = RB_LINEAR;   /* GL's defaults */
    g12_tex[k].ws = g12_tex[k].wt = RB_REPEAT;
    g12_tex[k].ver = 1;
    return (rb_tex)k;
}

void rb_tex_delete(rb_tex h)
{
    gl12_tex *t = gl12_T(h);
    if (!t) return;
    gl12_flush();
    var_free(t->vars); var_free(t->store);
    free(t->rgba);
    if (g_res == h) g_res = 0;
    memset(t, 0, sizeof *t);
}

void rb_tex_filter(rb_tex h, int min, int mag) { gl12_tex *t = gl12_T(h); if (t && (t->min != min || t->mag != mag)) { gl12_flush(); t->min = min; t->mag = mag; } }
void rb_tex_wrap(rb_tex h, int s, int tw) { gl12_tex *t = gl12_T(h); if (t && (t->ws != s || t->wt != tw)) { gl12_flush(); t->ws = s; t->wt = tw; } }

void rb_tex_image(rb_tex h, int w, int hh, const void *rgba, int mipmaps)
{
    gl12_tex *t = gl12_T(h);
    size_t n;
    if (!t || w <= 0 || hh <= 0 || t->target) return;
    gl12_flush();   /* (a draw batched before keeps the old texels: OpenGL's order) */
    n = (size_t)w * (size_t)hh * 4;
    if (!t->rgba || t->w != w || t->h != hh) { free(t->rgba); t->rgba = malloc(n); }
    if (!t->rgba) { fprintf(stderr, "rb: out of memory\n"); exit(1); }
    if (rgba) memcpy(t->rgba, rgba, n); else memset(t->rgba, 0, n);
    t->w = w; t->h = hh; t->mips = mipmaps != 0;
    t->ver++;
}

const uint8_t *gl12_tex_texels(rb_tex h, int *w, int *hh)
{
    gl12_tex *t = gl12_T(h);
    if (!t || !t->rgba) return NULL;
    *w = t->w; *hh = t->h;
    return t->rgba;
}

/* the DOS table's identity (the texture on the world's unit 1 and its version): a DOS variant made from another is stale */
static uint32_t dos_key(void) { gl12_tex *d = gl12_T(G.tex[1]); return d ? (uint32_t)G.tex[1] * 1000003u + d->ver : 0; }

/* the texels of a variant (malloc'd; *w, *h its size) */
static uint8_t *variant_texels(const gl12_tex *t, int kind, int param, int *w, int *h)
{
    const size_t n = (size_t)t->w * (size_t)t->h;
    uint8_t *o;
    size_t i;
    int sq = kind == GV_SQRTA || kind == GV_SHARP_SQRTA;
    *w = t->w; *h = t->h;
    if (kind == GV_SHARP || kind == GV_SHARP_SQRTA) {   /* each texel an n x n block */
        int k = param < 1 ? 1 : param, x, y, W = t->w * k, H = t->h * k;
        o = malloc((size_t)W * (size_t)H * 4);
        if (!o) return NULL;
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++) {
                const uint8_t *s = t->rgba + ((size_t)(y / k) * (size_t)t->w + (size_t)(x / k)) * 4;
                uint8_t *d = o + ((size_t)y * (size_t)W + (size_t)x) * 4;
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                d[3] = sq ? (uint8_t)floor(sqrt(s[3] / 255.0) * 255.0 + 0.5) : s[3];
            }
        *w = W; *h = H;
        return o;
    }
    if (kind == GV_DOSPAIR) {   /* (t: the DOS table, 256 x 17) 2 x 256: row i = palette i, palette i + 1 */
        int i2, c;
        o = malloc(2 * 256 * 4);
        if (!o) return NULL;
        for (i2 = 0; i2 < 256; i2++)
            for (c = 0; c < 4; c++) {
                const int ok = t->w == 256 && t->h >= 17;
                o[(i2 * 2) * 4 + c] = ok ? (c == 3 ? 255 : t->rgba[((size_t)16 * 256 + (size_t)i2) * 4 + (size_t)c]) : 255;
                o[(i2 * 2 + 1) * 4 + c] = ok ? (c == 3 ? 255 : t->rgba[((size_t)16 * 256 + (size_t)((i2 + 1) & 255)) * 4 + (size_t)c]) : 255;
            }
        *w = 2; *h = 256;
        return o;
    }
    o = malloc(n * 4);
    if (!o) return NULL;
    memcpy(o, t->rgba, n * 4);
    if (kind == GV_DOS) {   /* the texel's index (red) through shade level param, as the palette colour (row 16) */
        int tw = 0, th = 0, lev = param;
        const uint8_t *tab = gl12_tex_texels(G.tex[1], &tw, &th);
        for (i = 0; i < n; i++) {
            int ti = o[i * 4], si = ti;
            if (tab && tw == 256 && th >= 17) {
                if (lev < 15) si = tab[((size_t)lev * 256 + (size_t)ti) * 4];
                o[i * 4] = tab[((size_t)16 * 256 + (size_t)si) * 4];
                o[i * 4 + 1] = tab[((size_t)16 * 256 + (size_t)si) * 4 + 1];
                o[i * 4 + 2] = tab[((size_t)16 * 256 + (size_t)si) * 4 + 2];
            }
        }
        return o;
    }
    for (i = 0; i < n; i++) {
        uint8_t *d = o + i * 4;
        if (kind == GV_ASQ) { int a = (d[3] * d[3] + 127) / 255; d[3] = (uint8_t)(d[3] && !a ? 1 : a); }   /* (a texel kept stays kept) */
        else if (kind == GV_OPAQUE) d[3] = 255;
        else if (kind == GV_SQRTA) d[3] = (uint8_t)floor(sqrt(d[3] / 255.0) * 255.0 + 0.5);
        else if (kind == GV_WHITE) d[0] = d[1] = d[2] = 255;
    }
    return o;
}

/* bilinear resample (clamped) of RGBA w x h to W x H */
static uint8_t *resample(const uint8_t *s, int w, int h, int W, int H)
{
    uint8_t *o = malloc((size_t)W * (size_t)H * 4);
    int x, y, c;
    if (!o) return NULL;
    for (y = 0; y < H; y++) {
        float fy = ((float)y + 0.5f) * (float)h / (float)H - 0.5f;
        int y0 = (int)floorf(fy), y1;
        float ty = fy - (float)y0;
        y1 = y0 + 1; if (y0 < 0) y0 = 0; if (y1 > h - 1) y1 = h - 1; if (y0 > h - 1) y0 = h - 1; if (y1 < 0) y1 = 0;
        for (x = 0; x < W; x++) {
            float fx = ((float)x + 0.5f) * (float)w / (float)W - 0.5f;
            int x0 = (int)floorf(fx), x1;
            float tx = fx - (float)x0;
            x1 = x0 + 1; if (x0 < 0) x0 = 0; if (x1 > w - 1) x1 = w - 1; if (x0 > w - 1) x0 = w - 1; if (x1 < 0) x1 = 0;
            for (c = 0; c < 4; c++) {
                float a = s[((size_t)y0 * (size_t)w + (size_t)x0) * 4 + (size_t)c], b = s[((size_t)y0 * (size_t)w + (size_t)x1) * 4 + (size_t)c];
                float d = s[((size_t)y1 * (size_t)w + (size_t)x0) * 4 + (size_t)c], e = s[((size_t)y1 * (size_t)w + (size_t)x1) * 4 + (size_t)c];
                float v = (a + (b - a) * tx) * (1.0f - ty) + (d + (e - d) * tx) * ty;
                o[((size_t)y * (size_t)W + (size_t)x) * 4 + (size_t)c] = (uint8_t)(v + 0.5f);
            }
        }
    }
    return o;
}

/* the next mip level (2 x 2 average, as SDL_GPU's linear blit halving) */
static uint8_t *half(const uint8_t *s, int w, int h, int *W, int *H)
{
    int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1, x, y, c;
    uint8_t *o = malloc((size_t)nw * (size_t)nh * 4);
    if (!o) return NULL;
    for (y = 0; y < nh; y++)
        for (x = 0; x < nw; x++)
            for (c = 0; c < 4; c++) {
                int x0 = w > 1 ? 2 * x : 0, x1 = w > 1 ? 2 * x + 1 : 0, y0 = h > 1 ? 2 * y : 0, y1 = h > 1 ? 2 * y + 1 : 0;
                int v = s[((size_t)y0 * (size_t)w + (size_t)x0) * 4 + (size_t)c] + s[((size_t)y0 * (size_t)w + (size_t)x1) * 4 + (size_t)c]
                      + s[((size_t)y1 * (size_t)w + (size_t)x0) * 4 + (size_t)c] + s[((size_t)y1 * (size_t)w + (size_t)x1) * 4 + (size_t)c];
                o[((size_t)y * (size_t)nw + (size_t)x) * 4 + (size_t)c] = (uint8_t)((v + 2) / 4);
            }
    *W = nw; *H = nh;
    return o;
}

static int wrapc(int v, int n, int repeat)
{
    if (repeat) { v %= n; return v < 0 ? v + n : v; }
    return v < 0 ? 0 : v >= n ? n - 1 : v;
}

#ifndef GL_RGB5
#define GL_RGB5 0x8050
#endif
#ifndef GL_RGBA4
#define GL_RGBA4 0x8056
#endif
#ifndef GL_RGB5_A1
#define GL_RGB5_A1 0x8057
#endif
/* 16-bit textures (the classic Mac build's default, for its 8 - 16 MB cards; MW2_GL12_TEX16=0 / 1 overrides): RGB5 for
 * an opaque image, RGB5_A1 when its alpha is only on / off (the palette art, the HUD), RGBA4 for alpha ramps */
static int tex16_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("MW2_GL12_TEX16");
#ifdef MW2_CLASSIC_MAC
        v = 1;
#else
        v = 0;
#endif
        if (e) v = atoi(e) != 0;
    }
    return v;
}
static GLint tex_format(const uint8_t *px, size_t n, int *bytes)
{
    size_t k;
    int opaque = 1, binary = 1;
    *bytes = 4;
    if (!tex16_on() || !px) return GL_RGBA8;
    for (k = 0; k < n && (opaque || binary); k++) {
        uint8_t a = px[k * 4 + 3];
        if (a != 255) opaque = 0;
        if (a != 0 && a != 255) binary = 0;
    }
    *bytes = 2;
    return opaque ? GL_RGB5 : binary ? GL_RGB5_A1 : GL_RGBA4;
}

/* video memory the textures take (at the card's format, mip levels included), live and peak by GL texture name;
 * MW2_GL12_TRACE reports it at exit - with the frame buffers, what a look needs of the card */
static unsigned long long g_tex_live, g_tex_peak, *g_tex_of;
static unsigned g_tex_cap;
static void tex_report(void) { fprintf(stderr, "gl12: textures in video memory %.1f MB at the peak (%.1f MB at exit)\n", g_tex_peak / 1048576.0, g_tex_live / 1048576.0); }
static void tex_note(GLuint id, int w, int h, int bytes, int level)
{
    static int armed;
    unsigned long long b = (unsigned long long)w * (unsigned long long)h * (unsigned long long)bytes;
    if (!armed) { armed = 1; if (getenv("MW2_GL12_TRACE")) atexit(tex_report); }
    if (id >= g_tex_cap) {
        unsigned n = g_tex_cap ? g_tex_cap : 256, i;
        unsigned long long *q;
        while (n <= id) n *= 2;
        if (!(q = realloc(g_tex_of, n * sizeof *q))) return;
        for (i = g_tex_cap; i < n; i++) q[i] = 0;
        g_tex_of = q; g_tex_cap = n;
    }
    if (level == 0) { g_tex_live -= g_tex_of[id]; g_tex_of[id] = 0; }
    g_tex_of[id] += b; g_tex_live += b;
    if (g_tex_live > g_tex_peak) g_tex_peak = g_tex_live;
}
static void tex_gone(GLuint id) { if (id < g_tex_cap) { g_tex_live -= g_tex_of[id]; g_tex_of[id] = 0; } }

/* a dimension of n texels in tile rows / columns (each a power of two with its one-texel border, the largest the rest
 * fills - a 640-wide cockpit: 512 + 128 + 8, not 1024): count; x0 / content / texture size per piece */
static int tile_split(int n, int maxtex, int *x0, int *cw, int *pw)
{
    int pos = 0, k = 0;
    while (pos < n) {
        int rem = n - pos, P = 16;
        while (P * 2 <= maxtex && P * 2 - 2 <= rem) P *= 2;
        if (P - 2 > rem) { P = pot(rem + 2); if (P > maxtex) P = maxtex; }
        x0[k] = pos; cw[k] = P - 2 < rem ? P - 2 : rem; pw[k] = P;
        pos += cw[k]; k++;
    }
    return k;
}

/* a variant as one GL texture (the world's textures: a power of two within the card's limit - resampled if not - with
 * its mip chain), or as tiles with a one-texel border (the HUD's and the targets': each texel kept in place) */
static gl12_var *var_make(gl12_tex *t, int kind, int param, int tiled)
{
    gl12_var *v = calloc(1, sizeof *v);
    uint8_t *px;
    int w, h;
    if (!v) return NULL;
    v->kind = kind; v->param = param; v->ver = t->ver; v->dosver = kind == GV_DOS ? dos_key() : 0; v->tiled = tiled;
    px = variant_texels(t, kind, param, &w, &h);
    if (!px) { free(v); return NULL; }
    if (!tiled) {
        int W = pot(w), H = pot(h), lv = 0;
        uint8_t *cur = px;
        if (W > g12_maxtex) W = g12_maxtex;
        if (H > g12_maxtex) H = g12_maxtex;
        if (W != w || H != h) {
            uint8_t *r = resample(px, w, h, W, H);
            if (g12_trace) fprintf(stderr, "gl12: texture %d x %d resampled to %d x %d\n", w, h, W, H);
            if (r) { free(px); px = cur = r; w = W; h = H; }
        }
        v->ntx = v->nty = 1;
        v->t = calloc(1, sizeof *v->t);
        if (!v->t) { free(px); free(v); return NULL; }
        glGenTextures(1, &v->t[0].id);
        bind2d(0, v->t[0].id);
        {
        int bytes;
        GLint fmt = tex_format(cur, (size_t)w * (size_t)h, &bytes);
        for (;;) {
            uint8_t *nx;
            int nw, nh;
            glTexImage2D(GL_TEXTURE_2D, lv, fmt, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, cur);
            tex_note(v->t[0].id, w, h, bytes, lv);
            lv++;
            if (!t->mips || (w == 1 && h == 1)) break;
            nx = half(cur, w, h, &nw, &nh);
            if (cur != px) free(cur);
            if (!nx) { cur = px; break; }
            cur = nx; w = nw; h = nh;
        }
        }
        if (cur != px) free(cur);
        v->t[0].pw = W; v->t[0].ph = H; v->t[0].w = W; v->t[0].h = H; v->t[0].levels = lv;
        v->t[0].min = v->t[0].mag = v->t[0].ws = v->t[0].wt = v->t[0].base = -1;
        C.bound[0] = 0;
    } else {
        int P[2], cnt[2], i, j, x, y, rep[2] = {t->ws == RB_REPEAT, t->wt == RB_REPEAT};
        int *xs = malloc(sizeof(int) * (size_t)(w / 14 + 2) * 3), *ys = malloc(sizeof(int) * (size_t)(h / 14 + 2) * 3);
        uint8_t *tile;
        if (!xs || !ys) { free(xs); free(ys); free(px); free(v); return NULL; }
        /* (x0, content, texture size) per column and row - tight power-of-two pieces */
        cnt[0] = tile_split(w, g12_maxtex, xs, xs + (w / 14 + 2), xs + 2 * (w / 14 + 2));
        cnt[1] = tile_split(h, g12_maxtex, ys, ys + (h / 14 + 2), ys + 2 * (h / 14 + 2));
        P[0] = xs[2 * (w / 14 + 2)]; P[1] = ys[2 * (h / 14 + 2)];   /* the largest piece (the first) */
        v->ntx = cnt[0]; v->nty = cnt[1];
        v->t = calloc((size_t)(cnt[0] * cnt[1]), sizeof *v->t);
        tile = malloc((size_t)P[0] * (size_t)P[1] * 4);
        if (!v->t || !tile) { free(v->t); free(tile); free(xs); free(ys); free(px); free(v); return NULL; }
        if (g12_trace && (cnt[0] * cnt[1] > 1 || P[0] != w || P[1] != h))
            fprintf(stderr, "gl12: texture %d x %d (variant %d) in %d x %d tile(s), the largest %d x %d\n", w, h, kind, cnt[0], cnt[1], P[0], P[1]);
        for (j = 0; j < cnt[1]; j++)
            for (i = 0; i < cnt[0]; i++) {
                gl12_tile *g = &v->t[j * cnt[0] + i];
                int bytes;
                GLint fmt;
                g->x0 = xs[i]; g->y0 = ys[j];
                g->w = xs[(w / 14 + 2) + i]; g->h = ys[(h / 14 + 2) + j];
                g->pw = xs[2 * (w / 14 + 2) + i]; g->ph = ys[2 * (h / 14 + 2) + j]; g->levels = 1;
                g->min = g->mag = g->ws = g->wt = g->base = -1;
                for (y = 0; y < g->ph; y++) {
                    int sy = wrapc(g->y0 - 1 + y, h, rep[1]);
                    for (x = 0; x < g->pw; x++) {
                        int sx = wrapc(g->x0 - 1 + x, w, rep[0]);
                        memcpy(tile + ((size_t)y * (size_t)g->pw + (size_t)x) * 4, px + ((size_t)sy * (size_t)w + (size_t)sx) * 4, 4);
                    }
                }
                glGenTextures(1, &g->id);
                bind2d(0, g->id);
                fmt = tex_format(tile, (size_t)g->pw * (size_t)g->ph, &bytes);
                glTexImage2D(GL_TEXTURE_2D, 0, fmt, g->pw, g->ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, tile);
                tex_note(g->id, g->pw, g->ph, bytes, 0);
            }
        free(tile); free(xs); free(ys);
        C.bound[0] = 0;
        v->param = param | (rep[0] << 16) | (rep[1] << 17);
    }
    free(px);
    v->next = t->vars;
    t->vars = v;
    return v;
}

static gl12_var *var_get(rb_tex h, int kind, int param, int tiled)
{
    gl12_tex *t = gl12_T(h);
    gl12_var *v, **pp;
    int rp = tiled ? ((t && t->ws == RB_REPEAT) << 16) | ((t && t->wt == RB_REPEAT) << 17) : 0;
    if (!t || !t->rgba) return NULL;
    for (pp = &t->vars; (v = *pp) != NULL; pp = &v->next) {
        if (v->kind != kind || v->tiled != tiled || v->param != (param | rp)) continue;
        if (v->ver == t->ver && (kind != GV_DOS || v->dosver == dos_key())) return v;
        *pp = v->next; v->next = NULL; var_free(v);   /* stale */
        break;
    }
    return var_make(t, kind, param, tiled);
}

/* a texture can be drawn directly (not tiled) by the HUD program: a power of two within the limit */
static int direct_ok(const gl12_tex *t, int kind, int param)
{
    int k = kind == GV_SHARP || kind == GV_SHARP_SQRTA ? (param < 1 ? 1 : param) : 1;
    int w = t->w * k, h = t->h * k;
    return w <= g12_maxtex && h <= g12_maxtex && pot(w) == w && pot(h) == h;
}

static GLint gl_min(int f, int levels)
{
    if (levels <= 1) return f == RB_LINEAR || f == RB_LINEAR_MIPMAP_NEAREST || f == RB_LINEAR_MIPMAP_LINEAR ? GL_LINEAR : GL_NEAREST;
    return f;
}
static void tile_params(gl12_tile *g, int min, int mag, int ws, int wt, int base)
{
    GLint mn = gl_min(min, base >= 0 ? g->levels : g->levels), mg = mag == RB_LINEAR ? GL_LINEAR : GL_NEAREST;
    if (g->min != mn) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mn); g->min = mn; }
    if (g->mag != mg) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mg); g->mag = mg; }
    if (g->ws != ws) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws); g->ws = ws; }
    if (g->wt != wt) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt); g->wt = wt; }
    if (g12_lodok && g->base != base) {
        int b = base < 0 ? 0 : base < g->levels ? base : g->levels - 1;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, b);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, base < 0 ? 1000 : b);
        g->base = base;
    }
}

/* ---- buffers */
rb_buf rb_buf_new(void)
{
    int k;
    for (k = 1; k < g_nbuf; k++) if (!g_buf[k].live) break;
    if (k == g_nbuf) { g_buf = grow(g_buf, &g_capbuf, g_nbuf + 1, sizeof *g_buf); g_nbuf++; }
    memset(&g_buf[k], 0, sizeof g_buf[k]);
    g_buf[k].live = 1;
    return (rb_buf)k;
}
void rb_buf_delete(rb_buf h)
{
    gl12_buf *b = B(h);
    if (!b) return;
    free(b->p);
    memset(b, 0, sizeof *b);
}
void rb_buf_data(rb_buf h, const void *data, size_t bytes)
{
    gl12_buf *b = B(h);
    if (!b) return;
    if (bytes > b->cap) {
        void *q = realloc(b->p, bytes);
        if (!q) { fprintf(stderr, "rb: out of memory\n"); exit(1); }
        b->p = q; b->cap = bytes;
    }
    if (bytes) { if (data) memcpy(b->p, data, bytes); else memset(b->p, 0, bytes); }
    b->n = bytes;
}
void *rb_stream(size_t bytes, size_t *off)
{
    size_t at = (g_stn + 15) & ~(size_t)15;
    if (at + bytes > g_stcap) {
        size_t nc = g_stcap ? g_stcap : 1u << 16;
        uint8_t *q;
        while (nc < at + bytes) nc *= 2;
        q = realloc(g_st, nc);
        if (!q) { fprintf(stderr, "rb: out of memory\n"); exit(1); }
        g_st = q; g_stcap = nc;
    }
    g_stn = at + bytes;
    *off = at;
    return g_st + at;
}
const void *gl12_stream_ptr(size_t off) { return g_st + off; }
const void *gl12_buf_ptr(rb_buf h, size_t *bytes)
{
    gl12_buf *b;
    if (h == RB_STREAM) { *bytes = g_stn; return g_st; }
    b = B(h);
    if (!b || !b->p) { *bytes = 0; return NULL; }
    *bytes = b->n;
    return b->p;
}

/* ---- targets: the back buffer holds one at a time (g_res); the others' pictures are kept in their stores */
static gl12_tex *cur_target(void)
{
    if (!G.target || !gl12_T(G.target)) {
        if (!g_win_tex) rb_frame(g_win_w, g_win_h);
        G.target = g_win_tex;
    }
    return &g12_tex[G.target];
}

static void store_make(gl12_tex *t)
{
    int P[2], cnt[2], k, i, j, dim[2] = {t->w, t->h};
    gl12_var *v;
    var_free(t->store);
    t->store = v = calloc(1, sizeof *v);
    if (!v) return;
    for (k = 0; k < 2; k++) {
        P[k] = pot(dim[k] + 2);
        if (P[k] > g12_maxtex) P[k] = g12_maxtex;
        cnt[k] = (dim[k] + P[k] - 3) / (P[k] - 2);
    }
    v->tiled = 1; v->ntx = cnt[0]; v->nty = cnt[1];
    v->t = calloc((size_t)(cnt[0] * cnt[1]), sizeof *v->t);
    if (!v->t) { free(v); t->store = NULL; return; }
    for (j = 0; j < cnt[1]; j++)
        for (i = 0; i < cnt[0]; i++) {
            gl12_tile *g = &v->t[j * cnt[0] + i];
            g->x0 = i * (P[0] - 2); g->y0 = j * (P[1] - 2);
            g->w = t->w - g->x0 < P[0] - 2 ? t->w - g->x0 : P[0] - 2;
            g->h = t->h - g->y0 < P[1] - 2 ? t->h - g->y0 : P[1] - 2;
            g->pw = P[0]; g->ph = P[1]; g->levels = 1;
            g->min = g->mag = g->ws = g->wt = g->base = -1;
            glGenTextures(1, &g->id);
            bind2d(0, g->id);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, P[0], P[1], 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        }
    C.bound[0] = 0;
    if (g12_trace) fprintf(stderr, "gl12: target %d x %d kept in %d x %d tile(s) of %d x %d\n", t->w, t->h, cnt[0], cnt[1], P[0], P[1]);
}

/* copy the back buffer's picture of t (its 0,0 .. w,h) into its store; the borders take the edge pixels (clamp) */
static void store_save(gl12_tex *t)
{
    int k, W = t->w < g_win_w ? t->w : g_win_w, H = t->h < g_win_h ? t->h : g_win_h;
    if (!t->store) store_make(t);
    if (!t->store) return;
    tex_enable(0, 1, 0);
    for (k = 0; k < t->store->ntx * t->store->nty; k++) {
        gl12_tile *g = &t->store->t[k];
        int x0 = g->x0 - 1, y0 = g->y0 - 1, x1 = g->x0 + g->w + 1, y1 = g->y0 + g->h + 1;   /* with the border */
        int cx0 = x0 < 0 ? 0 : x0, cy0 = y0 < 0 ? 0 : y0, cx1 = x1 > W ? W : x1, cy1 = y1 > H ? H : y1;
        bind2d(0, g->id);
        if (cx1 > cx0 && cy1 > cy0) glCopyTexSubImage2D(GL_TEXTURE_2D, 0, cx0 - x0, cy0 - y0, cx0, cy0, cx1 - cx0, cy1 - cy0);
        if (x0 < 0 && cy1 > cy0) glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, cy0 - y0, 0, cy0, 1, cy1 - cy0);
        if (y0 < 0 && cx1 > cx0) glCopyTexSubImage2D(GL_TEXTURE_2D, 0, cx0 - x0, 0, cx0, 0, cx1 - cx0, 1);
        if (x1 > W && cy1 > cy0 && W > 0) glCopyTexSubImage2D(GL_TEXTURE_2D, 0, W - x0, cy0 - y0, W - 1, cy0, 1, cy1 - cy0);
        if (y1 > H && cx1 > cx0 && H > 0) glCopyTexSubImage2D(GL_TEXTURE_2D, 0, cx0 - x0, H - y0, cx0, H - 1, cx1 - cx0, 1);
    }
    t->valid = 1;
}

static void tiled_draw(const gl12_key *k, gl12_var *var, int vw, int vh, int rs, int rt, const gl12_vtx *v, int n, float bx, float by);
static void tile_bind_params(gl12_var *var, int min, int mag);

/* draw t's store 1:1 into the back buffer (its picture back) */
static void store_load(gl12_tex *t)
{
    gl12_key k;
    gl12_vtx q[6];
    float X[4] = {0, (float)t->w, (float)t->w, 0}, Y[4] = {0, 0, (float)t->h, (float)t->h};
    static const int QI[6] = {0, 1, 2, 0, 2, 3};
    int i;
    if (!t->store || !t->valid) return;
    memset(&k, 0, sizeof k);
    k.blend = RB_BLEND_OFF; k.lod = -1; k.no_depth_write = 1; k.depth_func = GL_ALWAYS;
    k.tile_min = k.tile_mag = RB_NEAREST;
    for (i = 0; i < 6; i++) {
        const int c = QI[i];
        memset(&q[i], 0, sizeof q[i]);
        q[i].p[0] = X[c] / (float)t->w * 2.0f - 1.0f; q[i].p[1] = Y[c] / (float)t->h * 2.0f - 1.0f; q[i].p[3] = 1.0f;
        q[i].c[0] = q[i].c[1] = q[i].c[2] = q[i].c[3] = 1.0f;
        q[i].t0[0] = X[c]; q[i].t0[1] = Y[c]; q[i].t0[3] = 1.0f;
    }
    k.own_vp = 1;
    set_viewport(0, 0, t->w, t->h);
    set_scissor(0, 0, 0, 0, 0);
    tile_bind_params(t->store, RB_NEAREST, RB_NEAREST);
    tiled_draw(&k, t->store, t->w, t->h, 0, 0, q, 6, 0.0f, 0.0f);
    gl12_flush();
}

/* put target h's picture in the back buffer (the one there kept first, if drawn into) */
static void resident(rb_tex h)
{
    gl12_tex *t = gl12_T(h);
    if (g_res == h || !t) return;
    gl12_flush();
    if (g_res && g_dirty && gl12_T(g_res)) store_save(&g12_tex[g_res]);
    g_res = h;
    g_dirty = 0;
    if ((t->w > g_win_w || t->h > g_win_h) && !g_bigwarn) {
        fprintf(stderr, "rb (OpenGL): an off-screen target of %d x %d is larger than the window (%d x %d): only its lower left part is drawn\n", t->w, t->h, g_win_w, g_win_h);
        g_bigwarn = 1;
    }
    store_load(t);
}

rb_tex rb_target_new(int w, int h, int depth)
{
    rb_tex k = rb_tex_new();
    gl12_tex *t = &g12_tex[k];
    t->w = w; t->h = h; t->target = 1; t->depth = depth;
    t->min = t->mag = RB_NEAREST; t->ws = t->wt = RB_CLAMP_TO_EDGE;
    return k;
}

void rb_frame(int w, int h)
{
    gl12_tex *t;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (g_own_win && (w != g_win_w || h != g_win_h)) { SDL_SetWindowSize(g_own_win, w, h); SDL_SyncWindow(g_own_win); }
    g_win_w = w; g_win_h = h;
    if (!g_win_tex) { g_win_tex = rb_tex_new(); g12_tex[g_win_tex].target = 1; g12_tex[g_win_tex].depth = 1; }
    t = &g12_tex[g_win_tex];
    if (t->w != w || t->h != h) { var_free(t->store); t->store = NULL; t->valid = 0; }
    t->w = w; t->h = h;
    if (!G.target) G.target = g_win_tex;
}
void rb_samples(int samples) { (void)samples; }
void rb_target(rb_tex t) { gl12_flush(); G.target = t ? t : g_win_tex; }
int rb_target_height(void) { return cur_target()->h; }

/* the current target in the back buffer, drawn into from here on */
static gl12_tex *draw_target(void)
{
    gl12_tex *t = cur_target();
    resident(G.target);
    g_dirty = 1;
    return t;
}

/* ---- state */
void rb_viewport(int x, int y, int w, int h) { G.vp[0] = x; G.vp[1] = y; G.vp[2] = w; G.vp[3] = h; }
void rb_scissor(int on, int x, int y, int w, int h) { gl12_flush(); G.sc_on = on; G.sc[0] = x; G.sc[1] = y; G.sc[2] = w; G.sc[3] = h; }
void rb_blend(int mode) { gl12_flush(); G.blend = mode; }
void rb_depth_test(int on) { gl12_flush(); G.dtest = on; }
void rb_depth_write(int on) { gl12_flush(); G.dwrite = on; }
void rb_depth_func(int func) { gl12_flush(); G.dfunc = func; }
void rb_depth_bias(int on, float slope, float units) { gl12_flush(); G.bias = on; G.slope = slope; G.units = units; }
void rb_program(int prog) { G.prog = prog; }
void rb_texture(int unit, rb_tex t) { if (unit >= 0 && unit < 3) G.tex[unit] = t; }
void rb_vertex(int binding, rb_buf b, size_t offset) { if (binding >= 0 && binding < 3) { G.vb[binding] = b; G.voff[binding] = offset; } }
void rb_storage(rb_buf b) { G.sb = b; }
void rb_uniforms(int slot, const void *data, size_t bytes)
{
    if (G.prog == RB_PROG_HUD) { if (slot == 0 && bytes == sizeof G.H) memcpy(&G.H, data, bytes); return; }
    if (slot == 0 && bytes == sizeof G.F) memcpy(&G.F, data, bytes);
    else if (slot == 1 && bytes == sizeof G.D) memcpy(&G.D, data, bytes);
}

void rb_clear(const float rgba[4], int color, int depth)
{
    gl12_tex *t;
    GLbitfield m = 0;
    gl12_flush();
    t = draw_target();
    if (G.sc_on) set_scissor(1, G.sc[0], G.sc[1], G.sc[2], G.sc[3]);
    else set_scissor(1, 0, 0, t->w, t->h);   /* (the target only: the back buffer may be larger) */
    if (color) { glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]); m |= GL_COLOR_BUFFER_BIT; }
    if (depth && t->depth) { set_depth(C.dtest, 1, C.dfunc < 0 ? GL_LESS : C.dfunc); glClearDepth(1.0); m |= GL_DEPTH_BUFFER_BIT; }
    if (m) glClear(m);
}

/* ---- the fog textures (unit 1, or unit 0 in the second pass): RGB the fog colour, A the fog factor */
int gl12_fog_size(void) { return g_fogn; }
void gl12_fog_table(const float rgb[3], int kind)
{
    (void)kind;
    if (!g_fogtab || rgb[0] != g_fogrgb[0] || rgb[1] != g_fogrgb[1] || rgb[2] != g_fogrgb[2]) {
        uint8_t *tab = malloc((size_t)g_fogn * 4), ramp[256 * 4];
        int i, c, step = 2048 / g_fogn;
        if (!tab) return;
        for (c = 0; c < 3; c++) g_fogrgb[c] = rgb[c];
        for (i = 0; i < g_fogn; i++) {
            /* PowerVR PCX2 (world.frag): index fi = floor(density / z); f = PowerTable[fi & 127] >> (fi >> 7), 0 from 1152;
             * the texel i holds index i (fewer entries than 2048: the middle of its range) */
            int fi = i * step + step / 2, fk = fi % 128, fs = fi / 128;
            double ft = fk == 0 ? 255.0 : floor(256.0 * pow(2.0, -fk / 128.0)), ff = fs >= 9 ? 0.0 : floor(ft / pow(2.0, fs)) / 256.0;
            if (step == 1) fi = i;
            for (c = 0; c < 3; c++) tab[i * 4 + c] = (uint8_t)floor(rgb[c] * 255.0f + 0.5f);
            tab[i * 4 + 3] = (uint8_t)floor(ff * 255.0 + 0.5);
        }
        for (i = 0; i < 256; i++) {
            for (c = 0; c < 3; c++) ramp[i * 4 + c] = (uint8_t)floor(rgb[c] * 255.0f + 0.5f);
            ramp[i * 4 + 3] = (uint8_t)i;
        }
        if (!g_fogtab) glGenTextures(1, &g_fogtab);
        if (!g_fogramp) glGenTextures(1, &g_fogramp);
        active(0);
        glBindTexture(GL_TEXTURE_1D, g_fogtab);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA8, g_fogn, 0, GL_RGBA, GL_UNSIGNED_BYTE, tab);
        glBindTexture(GL_TEXTURE_1D, g_fogramp);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA8, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, ramp);
        C.bound[0] = 0;
        free(tab);
        if (g12_trace) fprintf(stderr, "gl12: fog colour %.3f %.3f %.3f, table %d entries\n", rgb[0], rgb[1], rgb[2], g_fogn);
    }
}
static GLuint lum_ramp(void)
{
    if (!g_ramp) {
        uint8_t r[256];
        int i;
        for (i = 0; i < 256; i++) r[i] = (uint8_t)i;
        glGenTextures(1, &g_ramp);
        active(1);
        glBindTexture(GL_TEXTURE_1D, g_ramp);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexImage1D(GL_TEXTURE_1D, 0, GL_LUMINANCE8, 256, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, r);
        C.bound[1] = 0;
    }
    return g_ramp;
}

/* ---- batching: triangles of one fixed-function state, sent when the state changes */
static gl12_vtx *g_bv;
static int g_bn, g_bcap;
static gl12_key g_bk;
static int g_bvalid;

static int key_eq(const gl12_key *a, const gl12_key *b) { return memcmp(a, b, sizeof *a) == 0; }

void gl12_batch(const gl12_key *k, const gl12_vtx *v, int n)
{
    if (n <= 0) return;
    if (g_bvalid && !key_eq(k, &g_bk)) gl12_flush();
    if (!g_bvalid) { g_bk = *k; g_bvalid = 1; }
    g_bv = grow(g_bv, &g_bcap, g_bn + n, sizeof *g_bv);
    memcpy(g_bv + g_bn, v, (size_t)n * sizeof *v);
    g_bn += n;
}

/* bind unit 0's texture for k (a world / HUD texture's variant, direct) */
static int bind_unit0(const gl12_key *k)
{
    gl12_tex *t;
    gl12_var *v;
    gl12_tile *g;
    if (k->tile_tex) {   /* a tile (tiled_draw) */
        tex_enable(0, 1, 0);
        if (C.bound[0] != (GLuint)k->tile_tex) bind2d(0, (GLuint)k->tile_tex);
        return 1;
    }
    if (!k->tex || !(t = gl12_T(k->tex)) || !t->rgba) { tex_enable(0, 0, 0); return 0; }
    v = var_get(k->tex, k->var, k->param, 0);
    if (!v) { tex_enable(0, 0, 0); return 0; }
    g = &v->t[0];
    tex_enable(0, 1, 0);
    if (C.bound[0] != g->id) bind2d(0, g->id);
    else active(0);
    if (k->var == GV_DOSPAIR) tile_params(g, RB_NEAREST, RB_NEAREST, GL_REPEAT, GL_CLAMP_TO_EDGE, -1);
    else tile_params(g, t->min, t->mag, t->ws == RB_REPEAT ? GL_REPEAT : GL_CLAMP_TO_EDGE, t->wt == RB_REPEAT ? GL_REPEAT : GL_CLAMP_TO_EDGE, k->lod);
    return 1;
}

static void draw_arrays(const gl12_vtx *v, int n, int prim, int tc1)
{
    glVertexPointer(4, GL_FLOAT, (GLsizei)sizeof *v, v->p);
    glColorPointer(4, GL_FLOAT, (GLsizei)sizeof *v, v->c);
    client_active(0);
    glTexCoordPointer(4, GL_FLOAT, (GLsizei)sizeof *v, tc1 == 2 ? v->t1 : v->t0);
    if (g12_units > 1) {
        client_active(1);
        if (tc1 == 1) { if (C.carr1 != 1) { glEnableClientState(GL_TEXTURE_COORD_ARRAY); C.carr1 = 1; } glTexCoordPointer(4, GL_FLOAT, (GLsizei)sizeof *v, v->t1); }
        else if (C.carr1 != 0) { glDisableClientState(GL_TEXTURE_COORD_ARRAY); C.carr1 = 0; }
        client_active(0);
    }
    glDrawArrays((GLenum)prim, 0, n);
}

void gl12_flush(void)
{
    const gl12_key *k = &g_bk;
    gl12_tex *t;
    int fog, dfunc, blend, prim, n = g_bn, two_pass_fog = 0;
    if (!g_bvalid || n <= 0) { g_bvalid = 0; g_bn = 0; return; }
    g_bvalid = 0; g_bn = 0;
    t = cur_target();
    /* the target's viewport and scissor (the back buffer may be larger than the target: clip to it) */
    if (!k->own_vp) {   /* (own_vp: a blit / a target's picture put back - the caller set them) */
        set_viewport(G.vp[0], G.vp[1], G.vp[2], G.vp[3]);
        if (G.sc_on) set_scissor(1, G.sc[0], G.sc[1], G.sc[2], G.sc[3]);
        else set_scissor(1, 0, 0, t->w, t->h);
    }
    blend = k->blend >= 0 ? k->blend : G.blend;
    dfunc = k->depth_func ? k->depth_func : G.dfunc == RB_LEQUAL ? GL_LEQUAL : GL_LESS;
    set_depth(k->depth_func == GL_ALWAYS ? 0 : G.dtest && t->depth, G.dwrite && !k->no_depth_write && t->depth, dfunc);
    set_offset(k->offset_fill ? 1 : G.bias, k->offset_fill ? 1.0f : G.slope, k->offset_fill ? 1.0f : G.units);
    if (k->lines) set_offset(0, 0, 0);
    set_alpha(k->alpha_func, k->alpha_ref);
    set_stipple(k->stipple);
    prim = k->lines ? GL_LINES : GL_TRIANGLES;
    if (k->lines) { static int lw; if (!lw) { glLineWidth(2.0f); lw = 1; } }   /* (an edge both its polygons draw: 2 px) */
    fog = k->fog && !g12_nofog ? k->fog : GF_NONE;
    if (fog && g12_units == 1 && !k->tex && !k->tile_tex) {   /* one unit, no texel: the fog on unit 0, one pass */
        tex_enable(0, 0, 1);
        if (C.bound[0] != (fog == GF_TABLE ? g_fogtab : g_fogramp)) bind1d(0, fog == GF_TABLE ? g_fogtab : g_fogramp);
        tex_env(0, GL_DECAL);
        set_blend(blend);
        draw_arrays(g_bv, n, prim, 2);
        return;
    }
    bind_unit0(k);
    tex_env(0, GL_MODULATE);
    if (g12_units > 1) {
        if (fog) {
            tex_enable(1, 0, 1);
            if (C.bound[1] != (fog == GF_TABLE ? g_fogtab : g_fogramp)) bind1d(1, fog == GF_TABLE ? g_fogtab : g_fogramp);
            tex_env(1, GL_DECAL);
        } else if (k->env_blend) {   /* the HUD flash: C (1 - a) + flash a, a from the luminance ramp */
            static float cur[3] = {-1, -1, -1};
            GLuint r = lum_ramp();
            tex_enable(1, 0, 1);
            if (C.bound[1] != r) bind1d(1, r);
            tex_env(1, GL_BLEND);
            if (cur[0] != k->flash[0] || cur[1] != k->flash[1] || cur[2] != k->flash[2]) {
                float col[4] = {k->flash[0], k->flash[1], k->flash[2], 1.0f};
                active(1);
                glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, col);
                memcpy(cur, k->flash, sizeof cur);
            }
        } else tex_enable(1, 0, 0);
    } else two_pass_fog = fog != GF_NONE;
    set_blend(blend);
    draw_arrays(g_bv, n, prim, g12_units > 1 && (fog || k->env_blend) ? 1 : 0);
    if (two_pass_fog) {   /* one texture unit: the fog over what was drawn, at the same depth (a blended polygon's by its
                           * vertex alpha: approximate) */
        int i;
        for (i = 0; i < n; i++) { g_bv[i].c[0] = g_bv[i].c[1] = g_bv[i].c[2] = 1.0f; if (blend == RB_BLEND_OFF) g_bv[i].c[3] = 1.0f; }
        tex_enable(0, 0, 1);
        bind1d(0, fog == GF_TABLE ? g_fogtab : g_fogramp);
        set_alpha(0, 0);
        set_blend(RB_BLEND_ALPHA);
        set_depth(C.dtest, 0, GL_EQUAL);
        draw_arrays(g_bv, n, prim, 2);
        tex_enable(0, 0, 0);
        C.bound[0] = 0;
    }
}

/* ---- tiled textures: triangles cut at the tiles' edges (and the wrap's), each piece from its tile */
typedef struct { float p[4], c[4], x, y; } cutv;
static int clip_poly(cutv *in, int n, cutv *out, int axis, float lim, int keep_greater)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const cutv *a = &in[i], *b = &in[(i + 1) % n];
        float va = axis ? a->y : a->x, vb = axis ? b->y : b->x;
        int ia = keep_greater ? va >= lim : va <= lim, ib = keep_greater ? vb >= lim : vb <= lim;
        if (ia) out[m++] = *a;
        if (ia != ib) {
            /* (the cut computed from the edge's ends in a fixed order: neighbouring pieces share it exactly) */
            const cutv *p = va < vb ? a : b, *q = va < vb ? b : a;
            float vp = axis ? p->y : p->x, vq = axis ? q->y : q->x, s = (lim - vp) / (vq - vp);
            cutv *o = &out[m++];
            int c;
            for (c = 0; c < 4; c++) { o->p[c] = p->p[c] + (q->p[c] - p->p[c]) * s; o->c[c] = p->c[c] + (q->c[c] - p->c[c]) * s; }
            o->x = p->x + (q->x - p->x) * s; o->y = p->y + (q->y - p->y) * s;
            if (axis) o->y = lim; else o->x = lim;
        }
    }
    return m;
}

/* the cut lines along one axis within (lo, hi): the tiles' edges in every wrap period (rep) or within the image */
static int cuts(float lo, float hi, int dim, const gl12_var *var, int axis, int rep, float *out, int cap)
{
    int n = 0, i, cnt = axis ? var->nty : var->ntx;
    long p, p0 = rep ? (long)floorf(lo / (float)dim) : 0, p1 = rep ? (long)floorf(hi / (float)dim) : 0;
    for (p = p0; p <= p1 && n < cap - 1; p++)
        for (i = (rep ? 0 : 1); i < cnt && n < cap - 1; i++) {
            float c = (float)(p * dim + (axis ? var->t[i * var->ntx].y0 : var->t[i].x0));
            if (c > lo && c < hi) out[n++] = c;
        }
    return n;
}

static void tiled_draw(const gl12_key *k0, gl12_var *var, int vw, int vh, int rs, int rt, const gl12_vtx *v, int n, float bx, float by)
{
    int tri;
    for (tri = 0; tri + 2 < n; tri += 3) {
        float xs[66], ys[66], lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
        int nx, ny, i, j, c;
        cutv P[3];
        for (i = 0; i < 3; i++) {
            memcpy(P[i].p, v[tri + i].p, sizeof P[i].p); memcpy(P[i].c, v[tri + i].c, sizeof P[i].c);
            P[i].x = v[tri + i].t0[0] + bx; P[i].y = v[tri + i].t0[1] + by;
            if (P[i].x < lo[0]) lo[0] = P[i].x;
            if (P[i].x > hi[0]) hi[0] = P[i].x;
            if (P[i].y < lo[1]) lo[1] = P[i].y;
            if (P[i].y > hi[1]) hi[1] = P[i].y;
        }
        xs[0] = lo[0]; nx = 1 + cuts(lo[0], hi[0], vw, var, 0, rs, xs + 1, 64); xs[nx++] = hi[0];
        ys[0] = lo[1]; ny = 1 + cuts(lo[1], hi[1], vh, var, 1, rt, ys + 1, 64); ys[ny++] = hi[1];
        for (j = 0; j + 1 < ny; j++)
            for (i = 0; i + 1 < nx; i++) {
                cutv A[12], Bq[12];
                gl12_vtx o[30];
                int m = 3, q, ti, tj, tile;
                float mx = 0.5f * (xs[i] + xs[i + 1]), my = 0.5f * (ys[j] + ys[j + 1]), ox, oy;
                long px = rs ? (long)floorf(mx / (float)vw) : 0, py = rt ? (long)floorf(my / (float)vh) : 0;
                float lx = mx - (float)(px * vw), ly = my - (float)(py * vh);
                const gl12_tile *g;
                gl12_key k = *k0;
                memcpy(A, P, sizeof P);
                if (i > 0) m = clip_poly(A, m, Bq, 0, xs[i], 1), memcpy(A, Bq, (size_t)m * sizeof *A);
                if (i + 2 < nx) m = clip_poly(A, m, Bq, 0, xs[i + 1], 0), memcpy(A, Bq, (size_t)m * sizeof *A);
                if (j > 0) m = clip_poly(A, m, Bq, 1, ys[j], 1), memcpy(A, Bq, (size_t)m * sizeof *A);
                if (j + 2 < ny) m = clip_poly(A, m, Bq, 1, ys[j + 1], 0), memcpy(A, Bq, (size_t)m * sizeof *A);
                if (m < 3) continue;
                for (ti = 0; ti + 1 < var->ntx && lx >= (float)var->t[ti + 1].x0; ti++) {}
                for (tj = 0; tj + 1 < var->nty && ly >= (float)var->t[(tj + 1) * var->ntx].y0; tj++) {}
                tile = tj * var->ntx + ti;
                g = &var->t[tile];
                ox = (float)(px * vw + g->x0 - 1); oy = (float)(py * vh + g->y0 - 1);
                k.tile_tex = (int)g->id;
                for (q = 1; q + 1 < m && q < 10; q++) {
                    const cutv *e[3] = {&A[0], &A[q], &A[q + 1]};
                    for (c = 0; c < 3; c++) {
                        gl12_vtx *d = &o[(q - 1) * 3 + c];
                        memset(d, 0, sizeof *d);
                        memcpy(d->p, e[c]->p, sizeof d->p); memcpy(d->c, e[c]->c, sizeof d->c);
                        d->t0[0] = (e[c]->x - ox) / (float)g->pw; d->t0[1] = (e[c]->y - oy) / (float)g->ph; d->t0[3] = 1.0f;
                        d->t1[0] = v[tri].t1[0]; d->t1[3] = 1.0f;
                    }
                }
                gl12_batch(&k, o, (m - 2) * 3);   /* (the tiles' filters: set by the caller, tile_bind_params) */
            }
    }
}

/* the batches through tiles need the tile's filters set: done in tile_bind before each flush */
static void tile_bind_params(gl12_var *var, int min, int mag)
{
    int i;
    for (i = 0; i < var->ntx * var->nty; i++) {
        bind2d(0, var->t[i].id);
        tile_params(&var->t[i], min, mag, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, -1);
    }
    C.bound[0] = 0;
}

void gl12_tiled_tris(const gl12_key *k, rb_tex h, const gl12_vtx *v, int n)
{
    gl12_tex *t = gl12_T(h);
    gl12_var *var;
    int kk = k->var, k2 = kk == GV_SHARP || kk == GV_SHARP_SQRTA ? (k->param < 1 ? 1 : k->param) : 1, i;
    gl12_vtx *w;
    gl12_key kt = *k;
    if (!t || !t->rgba) return;
    if (direct_ok(t, kk, k->param)) {   /* a power of two within the limit: drawn as it is */
        gl12_batch(k, v, n);
        return;
    }
    var = var_get(h, kk, k->param, 1);
    if (!var) return;
    w = malloc((size_t)n * sizeof *w);
    if (!w) return;
    for (i = 0; i < n; i++) { w[i] = v[i]; w[i].t0[0] = v[i].t0[0] * (float)(t->w * k2); w[i].t0[1] = v[i].t0[1] * (float)(t->h * k2); }
    gl12_flush();
    tile_bind_params(var, t->min, t->mag);
    kt.tex = 0;
    tiled_draw(&kt, var, t->w * k2, t->h * k2, t->ws == RB_REPEAT, t->wt == RB_REPEAT, w, n, 0.0f, 0.0f);
    gl12_flush();
    free(w);
}

/* ---- blits, read-back, the frame */
void rb_blit(rb_tex src, int sx0, int sy0, int sx1, int sy1, rb_tex dst, int dx0, int dy0, int dx1, int dy1, int linear)
{
    gl12_tex *s = gl12_T(src), *d;
    gl12_key k;
    gl12_vtx q[6];
    static const int QI[6] = {0, 1, 2, 0, 2, 3};
    float X[4], Y[4], U[4], V[4];
    int i, rt;
    if (!dst) dst = g_win_tex;
    d = gl12_T(dst);
    if (!s || !d || sx1 <= sx0 || sy1 <= sy0 || dx1 <= dx0 || dy1 <= dy0) return;
    gl12_flush();
    if (g_res == src && g_dirty) { store_save(s); g_dirty = 0; }
    if (!s->store || !s->valid) return;
    rt = G.target;
    G.target = dst;
    draw_target();
    memset(&k, 0, sizeof k);
    k.blend = RB_BLEND_OFF; k.lod = -1; k.no_depth_write = 1; k.depth_func = GL_ALWAYS; k.own_vp = 1;
    X[0] = (float)dx0; X[1] = (float)dx1; X[2] = (float)dx1; X[3] = (float)dx0;
    Y[0] = (float)dy0; Y[1] = (float)dy0; Y[2] = (float)dy1; Y[3] = (float)dy1;
    U[0] = (float)sx0; U[1] = (float)sx1; U[2] = (float)sx1; U[3] = (float)sx0;
    V[0] = (float)sy0; V[1] = (float)sy0; V[2] = (float)sy1; V[3] = (float)sy1;
    for (i = 0; i < 6; i++) {
        const int c = QI[i];
        memset(&q[i], 0, sizeof q[i]);
        q[i].p[0] = X[c] / (float)d->w * 2.0f - 1.0f; q[i].p[1] = Y[c] / (float)d->h * 2.0f - 1.0f; q[i].p[3] = 1.0f;
        q[i].c[0] = q[i].c[1] = q[i].c[2] = q[i].c[3] = 1.0f;
        q[i].t0[0] = U[c]; q[i].t0[1] = V[c]; q[i].t0[3] = 1.0f;
    }
    set_viewport(0, 0, d->w, d->h);
    set_scissor(0, 0, 0, 0, 0);   /* (glBlitFramebuffer would take the scissor; the SDL_GPU backend's blit does not) */
    tile_bind_params(s->store, linear ? RB_LINEAR : RB_NEAREST, linear ? RB_LINEAR : RB_NEAREST);
    /* point-sampled: a destination pixel whose centre falls exactly on a texel edge (640 -> 320 across: every column;
     * 480 -> 200: every fifth row) takes the texel left of / below it, as the SDL_GPU backend's blit does (measured on
     * Mesa's lavapipe, scratch test: columns 0 2 4 ..., rows from the bottom 1 3 5 8 10 ...) - a small bias keeps the
     * interpolated coordinate there */
    tiled_draw(&k, s->store, s->w, s->h, 0, 0, q, 6, linear ? 0.0f : -1.0f / 1024.0f, linear ? 0.0f : -1.0f / 1024.0f);
    gl12_flush();
    G.target = rt;
}

void rb_read_rgb(int x, int y, int w, int h, unsigned char *rgb)
{
    gl12_flush();
    cur_target();
    resident(G.target);
    glReadPixels(x, y, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb);
}

void rb_finish(void) { gl12_flush(); glFinish(); }

void rb_present(void)
{
    rb_tex t = G.target;
    gl12_flush();
    G.target = g_win_tex;
    cur_target();
    resident(g_win_tex);
    SDL_GL_SwapWindow(g_win);
    g12_tex[g_win_tex].valid = 0;   /* (the back buffer's contents are undefined after the swap) */
    g_dirty = 0;
    g_stn = 0;   /* the frame's transient vertices */
    G.target = t;
}

/* ---- drawing */
void rb_draw(int first, int count)
{
    if (count <= 0) return;
    draw_target();
    if (G.prog == RB_PROG_HUD) gl12_hud_draw(first, count);
    else gl12_world_draw(first, count);
}
