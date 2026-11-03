/*
 * rb_gl12.h - the OpenGL 1.2 fixed-function render backend's internal state (src/rb_gl12.c, src/rb_gl12_world.c);
 * not part of rb.h. See docs/RENDERER_GL12.md.
 *
 * The backend draws at once (no recording): every rb_draw runs the program's vertex stage on the CPU (the shaders in
 * shaders/ ported to C), sorts its triangles into fixed-function classes and sends them as client-side vertex arrays
 * with clip-space positions and identity matrices. Only OpenGL 1.1 entry points are linked (opengl32.dll on Windows);
 * glActiveTextureARB / glClientActiveTextureARB are looked up when the driver has GL_ARB_multitexture.
 */
#ifndef MW2_RB_GL12_H
#define MW2_RB_GL12_H

#include "rb.h"
#include "rb_shaders.h"
#include "sdlcompat.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <stddef.h>
#include <stdint.h>

/* OpenGL 1.2 / ARB_multitexture / EXT_texture_edge_clamp values (not in every OpenGL 1.1 header) */
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_TEXTURE_BASE_LEVEL
#define GL_TEXTURE_BASE_LEVEL 0x813C
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
#ifndef GL_TEXTURE0_ARB
#define GL_TEXTURE0_ARB 0x84C0
#endif
#ifndef GL_TEXTURE1_ARB
#define GL_TEXTURE1_ARB 0x84C1
#endif
#ifndef GL_MAX_TEXTURE_UNITS_ARB
#define GL_MAX_TEXTURE_UNITS_ARB 0x84E2
#endif
#ifndef APIENTRY
#define APIENTRY
#endif

/* texture variants: the texels a per-texel step of the shaders makes, uploaded as their own GL texture */
enum {
    GV_PLAIN,      /* as uploaded */
    GV_ASQ,        /* alpha squared (world u_blend 2: the 3Dfx ARGB4444 cache entry) */
    GV_OPAQUE,     /* alpha 1 (world u_blend 3) */
    GV_SQRTA,      /* alpha square-rooted (HUD u_smooth) */
    GV_WHITE,      /* colour white, alpha kept (the HUD flash's second pass) */
    GV_SHARP,      /* each texel a param x param block (HUD u_sharp: "sharp bilinear" as bilinear on the upscale) */
    GV_DOS,        /* DOS: the texel's palette index through shade level param (0-15) of the DOS table, as colours */
    GV_SHARP_SQRTA,
    GV_DOSPAIR,    /* the DOS table's palette (row 16) as pairs: row n = colours n, n + 1 (checkerboards by s = x + y) */
    GV_COUNT
};

/* a GL texture holding (part of) a variant: a power-of-two image with the region x0..x0+w, y0..y0+h of the variant's
 * texels at (b, b) (b = 1 for a tiled variant: a border of the neighbouring texels, for exact bilinear filtering) */
typedef struct {
    GLuint id;
    int pw, ph;           /* the GL texture's size */
    int x0, y0, w, h;     /* the variant texels it holds */
    int levels;           /* mip levels uploaded */
    int min, mag, ws, wt, base;   /* the sampler state set on it (GL enums; base: GL_TEXTURE_BASE/MAX_LEVEL) */
} gl12_tile;

typedef struct gl12_var {
    int kind, param;
    uint32_t ver, dosver;  /* the texture's / the DOS table's version it was made from */
    int tiled;             /* 1: tiles with borders, drawn only through gl12_tiled_tris (the HUD, blits) */
    int ntx, nty;
    gl12_tile *t;          /* ntx x nty tiles (1 when not tiled) */
    struct gl12_var *next;
} gl12_var;

typedef struct {
    int live, w, h, mips;
    uint8_t *rgba;         /* level 0 (rows top first, as uploaded); a target: NULL */
    int min, mag, ws, wt;  /* GL enums */
    uint32_t ver;
    gl12_var *vars;
    /* off-screen targets (and the window's picture): its content kept in tiles while another target is drawn */
    int target, depth, valid;
    gl12_var *store;       /* rows bottom first (glCopyTexSubImage2D) */
} gl12_tex;

typedef struct { void *p; size_t n, cap; int live; } gl12_buf;

/* one vertex as sent: clip position, colour, texture unit 0 and unit 1 coordinates (s, t, r, q) */
typedef struct { float p[4], c[4], t0[4], t1[4]; } gl12_vtx;

/* a batch's fixed-function state (everything a triangle class sets) */
enum { GF_NONE, GF_TABLE, GF_RAMP };   /* unit 1 fog: the PowerVR table (s/q = density / z), a ramp (s/q = fog) */
typedef struct {
    rb_tex tex; int var, param;   /* unit 0 (tex 0: none) */
    int lod;                      /* -1, else the mip level (GL_TEXTURE_BASE/MAX_LEVEL; Matrox's bands) */
    int alpha_func; float alpha_ref;   /* GL_GEQUAL / GL_GREATER / 0 (off) */
    int fog;                      /* GF_* */
    int stipple;                  /* 0 none, 1 the pixels (x + y) even, 2 odd */
    int lines;                    /* GL_LINES */
    int offset_fill;              /* glPolygonOffset(1, 1) on the fill (the wireframe's insides) */
    int depth_func;               /* 0: the current state's, else a GL enum (GL_LEQUAL / GL_EQUAL) */
    int no_depth_write;
    int blend;                    /* -1: the current state's, else RB_BLEND_* */
    int env_blend;                /* the HUD flash: unit 1 GL_BLEND with constant flash_rgb, weight flash_a */
    float flash[4];
    int tile_tex;                 /* a GL texture name bound directly (tiled draws), 0: none */
    int tile_min, tile_mag;
    int own_vp;                   /* the viewport / scissor set by the caller (blits)  */
} gl12_key;

/* the state, as OpenGL's (rb.h's model) */
typedef struct {
    rb_tex target;
    int vp[4], sc_on, sc[4];
    int blend, dtest, dwrite, dfunc, bias;
    float slope, units;
    int prog;
    rb_tex tex[3];
    rb_buf vb[3];
    size_t voff[3];
    rb_buf sb;
    rb_world_frame F;
    rb_world_draw D;
    rb_hud_draw H;
} gl12_state;

extern gl12_state G;
extern gl12_tex *g12_tex;
extern int g12_ntex;
extern int g12_maxtex, g12_units, g12_lodok, g12_trace, g12_nofog;

/* rb_gl12.c */
gl12_tex *gl12_T(rb_tex t);
const void *gl12_buf_ptr(rb_buf b, size_t *bytes);   /* a bound vertex buffer's data (RB_STREAM: the frame's) */
const void *gl12_stream_ptr(size_t off);
void gl12_batch(const gl12_key *k, const gl12_vtx *v, int n);   /* triangles (or lines), drawn in order */
void gl12_flush(void);
void gl12_fog_table(const float rgb[3], int kind);   /* the fog texture's colour and table (GF_TABLE / GF_RAMP) */
int  gl12_fog_size(void);                            /* its entries (the table's index range) */
const uint8_t *gl12_tex_texels(rb_tex t, int *w, int *h);   /* a texture's level 0 (CPU copy) */
/* triangles in window coordinates of the current target with texel coordinates of a texture's tiled variant: cut at
 * the tiles' (and wrap) edges, each piece drawn from its tile (exact, also bilinear) */
void gl12_tiled_tris(const gl12_key *k, rb_tex t, const gl12_vtx *v, int n);

/* rb_gl12_world.c */
void gl12_world_draw(int first, int count);
void gl12_hud_draw(int first, int count);

#endif
