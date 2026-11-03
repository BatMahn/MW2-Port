/*
 * rb_gpu.c - rb.h on SDL3's GPU API (SDL_GPU): Direct3D 12 or Vulkan on Windows, Vulkan on Linux, Metal on macOS.
 *
 * The calls are recorded into a list (draws with the pipeline, buffers, textures and uniform blocks they use - the
 * SDL objects captured when recorded), which runs on a command buffer at rb_present / rb_read_rgb / rb_finish:
 *   1. one copy pass: the frame's transient vertex data (rb_stream) and every upload to a resource no recorded draw has
 *      used yet ("hoisted": they reach the GPU before every draw of the list, as OpenGL would have them by then);
 *      mipmaps made for those textures;
 *   2. the commands in order - targets, clears, draws, blits, and the uploads to resources an earlier draw of the list
 *      used (in place: the render pass ends, the copy runs with SDL's cycling - the earlier draws keep the old
 *      contents, OpenGL's semantics - and the pass resumes);
 *   3. the window's picture (an off-screen target, multisampled with anti-aliasing) blitted to the swapchain.
 * Pipelines (program x blend x depth state x target format) and samplers (filters x wraps) are made once and cached.
 * Consecutive draws with the same state and adjoining stream vertices are merged (the HUD's quads: ~900 draws a frame
 * become ~30). OpenGL's conventions are kept at this boundary: rectangles from the bottom left, the shaders working in
 * GL's clip and window coordinates (see shaders/world.vert / .frag).
 * Environment: MW2_GPU=vulkan|d3d12 overrides the configured backend; MW2_GPU_DEBUG=1 SDL's GPU debug mode (validation
 * layers when installed); MW2_GPU_STATS=1 prints each frame's commands, uploads and GPU time (waiting for it).
 */
#include "rb.h"
#include "rb_shaders.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXV 3   /* vertex bindings */
#define MAXT 3   /* texture units */
#define PROG_CLEAR RB_PROG_COUNT   /* the internal clear program (a partial glClear) */
#define COLOR_FMT SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM

typedef struct {
    SDL_GPUTexture *t;          /* the texture (a target's colour; a multisampled target's resolve) */
    SDL_GPUTexture *msaa;       /* a multisampled target's colour */
    SDL_GPUTexture *depth;      /* a target's depth buffer */
    int w, h, levels, samples, target, live;
    int min, mag, ws, wt;       /* the sampler state (GL enums) */
    SDL_GPUTextureFormat fmt;   /* a target's colour format (RGBA8; the window's picture: the swapchain's on Vulkan) */
    uint32_t used;              /* g_serial: a recorded draw / blit uses it (an upload after that goes in place) */
    uint32_t ref;               /* g_serial: a recorded command (an upload too) has its SDL objects (released after the run) */
} rtex;
typedef struct { SDL_GPUBuffer *b; uint32_t cap; uint32_t used, ref; int live; } rbuf;
typedef struct { SDL_GPUTexture *t, *msaa, *depth; int w, h, samples; SDL_GPUTextureFormat fmt; } rtgt;

enum { C_TARGET, C_CLEAR, C_DRAW, C_BLIT, C_UPTEX, C_UPBUF };
typedef struct {
    uint8_t type, prog, nv, nt;
    /* C_TARGET */
    rtgt tg;
    /* C_DRAW / C_CLEAR */
    SDL_GPUGraphicsPipeline *pipe;
    SDL_GPUBuffer *vb[MAXV];
    uint32_t voff[MAXV];
    uint8_t vstream[MAXV];
    SDL_GPUBuffer *sb;
    SDL_GPUTexture *tx[MAXT];
    SDL_GPUSampler *sm[MAXT];
    uint32_t uoff[2], ulen[2];
    uint8_t nu;
    SDL_GPUViewport vp;
    SDL_Rect sc;
    uint32_t count;
    float col[4];
    uint8_t cflags;             /* C_CLEAR: 1 colour, 2 depth */
    /* C_BLIT */
    SDL_GPUBlitInfo blit;
    /* C_UPTEX / C_UPBUF */
    SDL_GPUTexture *ut;
    SDL_GPUBuffer *ub;
    uint32_t aoff, asize, uw, uh;
    uint8_t mips;
} rcmd;

typedef struct {
    int prog, blend, dtest, dwrite, dfunc, bias, cmask, samples;
    float slope, units;
    SDL_GPUTextureFormat dfmt, cfmt;
    SDL_GPUGraphicsPipeline *p;
} rpipe;
typedef struct { int min, mag, ws, wt; SDL_GPUSampler *s; } rsamp;

static SDL_GPUDevice *g_dev;
static SDL_Window *g_win;
static int g_backend;
static SDL_GPUShaderFormat g_fmt;
static char g_devname[256];
static SDL_GPUTextureFormat g_depth_fmt = SDL_GPU_TEXTUREFORMAT_INVALID;
static SDL_GPUShader *g_sh[RB_SH_COUNT];
static rtex *g_tex;
static int g_ntex, g_captex;
static rbuf *g_buf;
static int g_nbuf, g_capbuf;
static uint32_t g_serial = 1;
static rb_tex g_win_tex;
static int g_win_samples = 1, g_win_made, g_win_w, g_win_h;
/* the window's picture's format: on Vulkan the swapchain's (shown with a copy - a blit is a draw, 7 ms a frame at 1920 x
 * 1080 in Mesa's software Vulkan), else RGBA8 (shown with a blit, the documented way on Direct3D 12 and Metal) */
static SDL_GPUTextureFormat g_win_fmt = COLOR_FMT;   /* g_win_made: the samples asked for when made */
static rpipe *g_pipes;
static int g_npipes, g_cappipes;
static rsamp *g_samps;
static int g_nsamps, g_capsamps;
/* the zero buffer: unbound vertex bindings (0, 0, 0, 0) and the storage buffer when none */
static SDL_GPUBuffer *g_zero;
static uint32_t g_zero_cap;

/* the recording */
static rcmd *g_cmd;
static int g_ncmd, g_capcmd;
static rcmd *g_pre;                  /* hoisted uploads */
static int g_npre, g_cappre;
static uint8_t *g_up;                /* upload data */
static size_t g_upn, g_upcap;
static uint8_t *g_st;                /* rb_stream data */
static size_t g_stn, g_stcap;
static uint8_t *g_un;                /* uniform snapshots */
static size_t g_unn, g_uncap;
static void **g_rel;                 /* SDL objects to release once the list has run: {kind, ptr} pairs */
static int g_nrel, g_caprel;
static rb_tex g_rec_target;          /* the target the last recorded command drew into (0: none yet in this list) */
static SDL_GPUTransferBuffer *g_xfer, *g_down;
static uint32_t g_xfer_cap, g_down_cap;
static SDL_GPUBuffer *g_stream;
static uint32_t g_stream_cap;

/* the state, as OpenGL's */
static struct {
    rb_tex target;
    int vp[4], sc_on, sc[4];
    int blend, dtest, dwrite, dfunc, bias;
    float slope, units;
    int prog;
    rb_tex tex[MAXT];
    rb_buf vb[MAXV];
    size_t voff[MAXV];
    rb_buf sb;
    uint32_t uoff[2], ulen[2];
    int uvalid[2];
} S;

static const int PROG_VS[RB_PROG_COUNT + 1] = {RB_SH_WORLD_VS, RB_SH_WORLD_DOS_VS, RB_SH_HUD_VS, RB_SH_CLEAR_VS};
static const int PROG_FS[RB_PROG_COUNT + 1] = {RB_SH_WORLD_FS, RB_SH_WORLD_DOS_FS, RB_SH_HUD_FS, RB_SH_CLEAR_FS};
static const int PROG_NV[RB_PROG_COUNT + 1] = {3, 3, 1, 0};
static const uint32_t PROG_STRIDE[RB_PROG_COUNT + 1][MAXV] = {{52, 16, 16}, {52, 16, 16}, {16, 0, 0}, {0, 0, 0}};

/* ---- small helpers */
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
static size_t arena_put(uint8_t **a, size_t *n, size_t *cap, const void *data, size_t bytes, size_t align)
{
    size_t at = (*n + align - 1) & ~(align - 1);
    if (at + bytes > *cap) {
        size_t nc = *cap ? *cap : 1u << 16;
        uint8_t *q;
        while (nc < at + bytes) nc *= 2;
        q = realloc(*a, nc);
        if (!q) { fprintf(stderr, "rb: out of memory\n"); exit(1); }
        *a = q; *cap = nc;
    }
    if (data) memcpy(*a + at, data, bytes); else memset(*a + at, 0, bytes);
    *n = at + bytes;
    return at;
}
static void release_later(int kind, void *p)
{
    if (!p) return;
    g_rel = grow(g_rel, &g_caprel, g_nrel + 2, sizeof *g_rel);
    g_rel[g_nrel++] = (void *)(intptr_t)kind;
    g_rel[g_nrel++] = p;
}
static void release_now(int kind, void *p)
{
    if (!p) return;
    if (kind == 0) SDL_ReleaseGPUTexture(g_dev, p); else SDL_ReleaseGPUBuffer(g_dev, p);
}
static rtex *T(rb_tex t) { return t > 0 && (int)t < g_ntex && g_tex[t].live ? &g_tex[t] : NULL; }
static rbuf *B(rb_buf b) { return b > 0 && (int)b < g_nbuf && g_buf[b].live ? &g_buf[b] : NULL; }
static rcmd *cmd_new(int type)
{
    rcmd *c;
    g_cmd = grow(g_cmd, &g_capcmd, g_ncmd + 1, sizeof *g_cmd);
    c = &g_cmd[g_ncmd++];
    memset(c, 0, sizeof *c);
    c->type = (uint8_t)type;
    return c;
}

/* ---- the device */
static const char *backend_sdl_name(int b) { return b == RB_GPU_D3D12 ? "direct3d12" : b == RB_GPU_VULKAN ? "vulkan" : "metal"; }
const char *rb_backend_name(void) { return !g_dev ? "none" : g_backend == RB_GPU_D3D12 ? "Direct3D 12" : g_backend == RB_GPU_VULKAN ? "Vulkan" : "Metal"; }
int rb_backend(void) { return g_backend; }
const char *rb_device_name(void) { return g_devname; }

static void order_for(int pref, int out[3], int *n)
{
    *n = 0;
#if defined(_WIN32)
    if (pref == RB_GPU_VULKAN) { out[(*n)++] = RB_GPU_VULKAN; out[(*n)++] = RB_GPU_D3D12; }
    else { out[(*n)++] = RB_GPU_D3D12; out[(*n)++] = RB_GPU_VULKAN; }
#elif defined(__APPLE__)
    (void)pref;
    out[(*n)++] = RB_GPU_METAL;
#else
    (void)pref;
    out[(*n)++] = RB_GPU_VULKAN;
#endif
}

static SDL_PropertiesID device_props(int b)
{
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetStringProperty(p, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, backend_sdl_name(b));
    SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, b == RB_GPU_VULKAN);
    SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, b == RB_GPU_D3D12);
    SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, b == RB_GPU_METAL);
    SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, getenv("MW2_GPU_DEBUG") != NULL);
    /* only what the renderer uses, so that older GPUs qualify: no clip distances, depth clamping (the pipelines clip),
     * indirect draws or anisotropic filtering (Vulkan); Direct3D 12 resource binding tier 1 (Haswell / Broadwell: the
     * shaders take 3 textures, 2 uniform blocks, 1 storage buffer); Metal GPU family Mac1 (no sRGB targets are drawn) */
#ifdef SDL_PROP_GPU_DEVICE_CREATE_FEATURE_CLIP_DISTANCE_BOOLEAN
    SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_CLIP_DISTANCE_BOOLEAN, false);
    SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_DEPTH_CLAMPING_BOOLEAN, false);
    SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_INDIRECT_DRAW_FIRST_INSTANCE_BOOLEAN, false);
    SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_ANISOTROPY_BOOLEAN, false);
#endif
#ifdef SDL_PROP_GPU_DEVICE_CREATE_D3D12_ALLOW_FEWER_RESOURCE_SLOTS_BOOLEAN
    SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_D3D12_ALLOW_FEWER_RESOURCE_SLOTS_BOOLEAN, true);
#endif
#ifdef SDL_PROP_GPU_DEVICE_CREATE_METAL_ALLOW_MACFAMILY1_BOOLEAN
    SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_METAL_ALLOW_MACFAMILY1_BOOLEAN, true);
#endif
    return p;
}

int rb_init(SDL_Window *win, int pref, int samples, char *err, size_t errn)
{
    int order[3], n, k;
    const char *env = getenv("MW2_GPU");   /* tests / troubleshooting: d3d12 | vulkan */
    if (env) pref = SDL_strcasecmp(env, "vulkan") == 0 ? RB_GPU_VULKAN : SDL_strcasecmp(env, "d3d12") == 0 ? RB_GPU_D3D12 : pref;
    order_for(pref, order, &n);
    if (err && errn) err[0] = 0;
    for (k = 0; k < n && !g_dev; k++) {
        SDL_PropertiesID p = device_props(order[k]);
        g_dev = SDL_CreateGPUDeviceWithProperties(p);
        SDL_DestroyProperties(p);
        if (!g_dev) {
            if (err && errn) { size_t at = strlen(err); SDL_snprintf(err + at, errn - at, "%s%s: %s", at ? "; " : "", backend_sdl_name(order[k]), SDL_GetError()); }
            continue;
        }
        if (win && !SDL_ClaimWindowForGPUDevice(g_dev, win)) {
            if (err && errn) { size_t at = strlen(err); SDL_snprintf(err + at, errn - at, "%s%s (window): %s", at ? "; " : "", backend_sdl_name(order[k]), SDL_GetError()); }
            SDL_DestroyGPUDevice(g_dev);
            g_dev = NULL;
            continue;
        }
        g_backend = order[k];
    }
    if (!g_dev) return -1;
    g_win = win;
    g_fmt = g_backend == RB_GPU_D3D12 ? SDL_GPU_SHADERFORMAT_DXIL : g_backend == RB_GPU_VULKAN ? SDL_GPU_SHADERFORMAT_SPIRV : SDL_GPU_SHADERFORMAT_MSL;
#if SDL_VERSION_ATLEAST(3, 4, 0)
    {   /* the GPU and its driver (SDL 3.4) */
        SDL_PropertiesID dp = SDL_GetGPUDeviceProperties(g_dev);
        const char *nm = SDL_GetStringProperty(dp, SDL_PROP_GPU_DEVICE_NAME_STRING, "");
        const char *dn = SDL_GetStringProperty(dp, SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING, "");
        const char *dv = SDL_GetStringProperty(dp, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, "");
        SDL_snprintf(g_devname, sizeof g_devname, "%s%s%s%s%s", nm, dn[0] ? ", " : "", dn, dv[0] ? " " : "", dv);
    }
#else
    SDL_snprintf(g_devname, sizeof g_devname, "SDL %s driver", SDL_GetGPUDeviceDriver(g_dev));
#endif
    {   /* the depth buffer: 24 bits as the OpenGL renderer had (Apple GPUs have none: 32-bit float) */
        static const SDL_GPUTextureFormat DF[] = {SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_TEXTUREFORMAT_D24_UNORM,
                                                  SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT};
        for (k = 0; k < 4 && g_depth_fmt == SDL_GPU_TEXTUREFORMAT_INVALID; k++)
            if (SDL_GPUTextureSupportsFormat(g_dev, DF[k], SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) g_depth_fmt = DF[k];
    }
    if (win) {   /* vsync, as the OpenGL renderer's swap interval 1; with no display (SDL's offscreen video driver: the tests)
                  * nothing to wait for - frames as fast as the GPU makes them, as its OpenGL contexts did */
        const char *vd = SDL_GetCurrentVideoDriver();
        SDL_GPUPresentMode pm = vd && strcmp(vd, "offscreen") == 0 && SDL_WindowSupportsGPUPresentMode(g_dev, win, SDL_GPU_PRESENTMODE_IMMEDIATE)
                              ? SDL_GPU_PRESENTMODE_IMMEDIATE : SDL_GPU_PRESENTMODE_VSYNC;
        SDL_SetGPUSwapchainParameters(g_dev, win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, pm);
        if (g_backend == RB_GPU_VULKAN) {
            SDL_GPUTextureFormat sf = SDL_GetGPUSwapchainTextureFormat(g_dev, win);
            if ((sf == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || sf == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)
                && SDL_GPUTextureSupportsFormat(g_dev, sf, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER))
                g_win_fmt = sf;
        }
    }
    g_ntex = g_nbuf = 1;   /* handle 0: none */
    g_tex = grow(g_tex, &g_captex, 1, sizeof *g_tex);
    g_buf = grow(g_buf, &g_capbuf, 1, sizeof *g_buf);
    memset(&g_tex[0], 0, sizeof g_tex[0]);
    memset(&g_buf[0], 0, sizeof g_buf[0]);
    memset(&S, 0, sizeof S);
    S.dtest = 0; S.dwrite = 1; S.dfunc = RB_LESS;
    g_win_samples = samples > 1 ? samples : 1;
    printf("gpu: %s (%s)\n", rb_backend_name(), g_devname);
    fflush(stdout);
    return 0;
}

static void exec_list(SDL_GPUCommandBuffer *cb);
void rb_quit(void)
{
    int k;
    if (!g_dev) return;
    rb_finish();
    for (k = 1; k < g_ntex; k++) if (g_tex[k].live) { release_now(0, g_tex[k].t); release_now(0, g_tex[k].msaa); release_now(0, g_tex[k].depth); }
    for (k = 1; k < g_nbuf; k++) if (g_buf[k].live) release_now(1, g_buf[k].b);
    for (k = 0; k < g_npipes; k++) SDL_ReleaseGPUGraphicsPipeline(g_dev, g_pipes[k].p);
    for (k = 0; k < g_nsamps; k++) SDL_ReleaseGPUSampler(g_dev, g_samps[k].s);
    for (k = 0; k < RB_SH_COUNT; k++) if (g_sh[k]) SDL_ReleaseGPUShader(g_dev, g_sh[k]);
    release_now(1, g_zero); release_now(1, g_stream);
    if (g_xfer) SDL_ReleaseGPUTransferBuffer(g_dev, g_xfer);
    if (g_down) SDL_ReleaseGPUTransferBuffer(g_dev, g_down);
    if (g_win) SDL_ReleaseWindowFromGPUDevice(g_dev, g_win);
    SDL_DestroyGPUDevice(g_dev);
    g_dev = NULL;
    free(g_tex); free(g_buf); free(g_pipes); free(g_samps); free(g_cmd); free(g_pre); free(g_up); free(g_st); free(g_un); free(g_rel);
    g_tex = NULL; g_buf = NULL; g_pipes = NULL; g_samps = NULL; g_cmd = NULL; g_pre = NULL; g_up = g_st = g_un = NULL; g_rel = NULL;
    g_captex = g_capbuf = g_cappipes = g_capsamps = g_capcmd = g_cappre = g_caprel = 0;
    g_ntex = g_nbuf = g_npipes = g_nsamps = g_ncmd = g_npre = g_nrel = 0;
    g_upn = g_upcap = g_stn = g_stcap = g_unn = g_uncap = 0;
    g_zero = g_stream = NULL; g_xfer = g_down = NULL; g_zero_cap = g_stream_cap = g_xfer_cap = g_down_cap = 0;
    g_win_tex = 0; g_rec_target = 0; g_win_made = 0; g_win_w = g_win_h = 0;
    memset(g_sh, 0, sizeof g_sh);
}

/* ---- shaders, pipelines, samplers */
static SDL_GPUShader *shader(int i)
{
    const rb_shader_blob *b = &rb_shader_blobs[i];
    SDL_GPUShaderCreateInfo ci;
    if (g_sh[i]) return g_sh[i];
    memset(&ci, 0, sizeof ci);
    ci.format = g_fmt;
    ci.stage = b->vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
    ci.num_samplers = (Uint32)b->samplers;
    ci.num_uniform_buffers = (Uint32)b->uniform_buffers;
    ci.num_storage_buffers = (Uint32)b->storage_buffers;
    if (g_fmt == SDL_GPU_SHADERFORMAT_SPIRV) { ci.code = b->spirv; ci.code_size = b->spirv_size; ci.entrypoint = "main"; }
    else if (g_fmt == SDL_GPU_SHADERFORMAT_DXIL) { ci.code = b->dxil; ci.code_size = b->dxil_size; ci.entrypoint = "main"; }
    else { ci.code = (const Uint8 *)b->msl; ci.code_size = b->msl_size; ci.entrypoint = "main0"; }
    g_sh[i] = SDL_CreateGPUShader(g_dev, &ci);
    if (!g_sh[i]) { fprintf(stderr, "rb: shader %s: %s\n", b->name, SDL_GetError()); exit(1); }
    return g_sh[i];
}

static SDL_GPUGraphicsPipeline *pipeline(int prog, int has_depth, int samples, int cmask, SDL_GPUTextureFormat cfmt)
{
    int k;
    const int blend = prog == PROG_CLEAR ? RB_BLEND_OFF : S.blend;
    const int dtest = has_depth && (prog == PROG_CLEAR ? (cmask & 16) != 0 : S.dtest);
    const int dwrite = dtest && (prog == PROG_CLEAR ? 1 : S.dwrite);
    const int dfunc = prog == PROG_CLEAR ? 2 : S.dfunc;
    const int bias = prog != PROG_CLEAR && dtest && S.bias;
    const SDL_GPUTextureFormat dfmt = has_depth ? g_depth_fmt : SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUGraphicsPipelineCreateInfo pi;
    SDL_GPUVertexBufferDescription vbd[MAXV];
    SDL_GPUVertexAttribute va[8];
    SDL_GPUColorTargetDescription ctd;
    rpipe *e;
    for (k = 0; k < g_npipes; k++) {
        e = &g_pipes[k];
        if (e->prog == prog && e->blend == blend && e->dtest == dtest && e->dwrite == dwrite && e->dfunc == (dtest ? dfunc : 0)
            && e->bias == bias && (!bias || (e->slope == S.slope && e->units == S.units)) && e->cmask == (cmask & 15)
            && e->samples == samples && e->dfmt == dfmt && e->cfmt == cfmt)
            return e->p;
    }
    memset(&pi, 0, sizeof pi);
    memset(vbd, 0, sizeof vbd);
    memset(va, 0, sizeof va);
    memset(&ctd, 0, sizeof ctd);
    pi.vertex_shader = shader(PROG_VS[prog]);
    pi.fragment_shader = shader(PROG_FS[prog]);
    for (k = 0; k < PROG_NV[prog]; k++) { vbd[k].slot = (Uint32)k; vbd[k].pitch = PROG_STRIDE[prog][k]; vbd[k].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX; }
    if (prog == RB_PROG_WORLD || prog == RB_PROG_WORLD_DOS) {
        static const struct { int buf, fmt, off; } A[8] = {{0, 3, 0}, {0, 3, 12}, {0, 2, 24}, {0, 3, 32}, {0, 1, 44}, {0, 1, 48}, {1, 4, 0}, {2, 4, 0}};
        for (k = 0; k < 8; k++) {
            va[k].location = (Uint32)k; va[k].buffer_slot = (Uint32)A[k].buf; va[k].offset = (Uint32)A[k].off;
            va[k].format = A[k].fmt == 1 ? SDL_GPU_VERTEXELEMENTFORMAT_FLOAT : A[k].fmt == 2 ? SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2
                         : A[k].fmt == 3 ? SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3 : SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        }
        pi.vertex_input_state.num_vertex_attributes = 8;
    } else if (prog == RB_PROG_HUD) {
        va[0].location = 0; va[0].buffer_slot = 0; va[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; va[0].offset = 0;
        va[1].location = 1; va[1].buffer_slot = 0; va[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; va[1].offset = 8;
        pi.vertex_input_state.num_vertex_attributes = 2;
    }
    pi.vertex_input_state.vertex_buffer_descriptions = vbd;
    pi.vertex_input_state.num_vertex_buffers = (Uint32)PROG_NV[prog];
    pi.vertex_input_state.vertex_attributes = va;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pi.rasterizer_state.enable_depth_clip = true;
    if (bias) {
        pi.rasterizer_state.enable_depth_bias = true;
        pi.rasterizer_state.depth_bias_slope_factor = S.slope;
        pi.rasterizer_state.depth_bias_constant_factor = S.units;
    }
    pi.multisample_state.sample_count = samples >= 8 ? SDL_GPU_SAMPLECOUNT_8 : samples >= 4 ? SDL_GPU_SAMPLECOUNT_4 : samples >= 2 ? SDL_GPU_SAMPLECOUNT_2 : SDL_GPU_SAMPLECOUNT_1;
    pi.depth_stencil_state.enable_depth_test = dtest != 0;
    pi.depth_stencil_state.enable_depth_write = dwrite != 0;
    pi.depth_stencil_state.compare_op = dfunc == 2 ? SDL_GPU_COMPAREOP_ALWAYS : dfunc == RB_LEQUAL ? SDL_GPU_COMPAREOP_LESS_OR_EQUAL : SDL_GPU_COMPAREOP_LESS;
    ctd.format = cfmt;
    if (blend != RB_BLEND_OFF) {
        SDL_GPUBlendFactor sf = blend == RB_BLEND_ALPHA ? SDL_GPU_BLENDFACTOR_SRC_ALPHA : blend == RB_BLEND_MUL ? SDL_GPU_BLENDFACTOR_DST_COLOR : SDL_GPU_BLENDFACTOR_ONE;
        SDL_GPUBlendFactor df = blend == RB_BLEND_ALPHA ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA : blend == RB_BLEND_MUL ? SDL_GPU_BLENDFACTOR_ZERO : SDL_GPU_BLENDFACTOR_ONE;
        ctd.blend_state.enable_blend = true;
        ctd.blend_state.src_color_blendfactor = sf; ctd.blend_state.dst_color_blendfactor = df; ctd.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        /* glBlendFunc sets the alpha's factors alike (DST_COLOR reads the destination's alpha for it) */
        ctd.blend_state.src_alpha_blendfactor = blend == RB_BLEND_MUL ? SDL_GPU_BLENDFACTOR_DST_ALPHA : sf;
        ctd.blend_state.dst_alpha_blendfactor = df; ctd.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    if ((cmask & 15) != 15) { ctd.blend_state.enable_color_write_mask = true; ctd.blend_state.color_write_mask = (SDL_GPUColorComponentFlags)(cmask & 15); }
    pi.target_info.color_target_descriptions = &ctd;
    pi.target_info.num_color_targets = 1;
    pi.target_info.has_depth_stencil_target = has_depth != 0;
    pi.target_info.depth_stencil_format = dfmt;
    g_pipes = grow(g_pipes, &g_cappipes, g_npipes + 1, sizeof *g_pipes);
    e = &g_pipes[g_npipes];
    memset(e, 0, sizeof *e);
    e->prog = prog; e->blend = blend; e->dtest = dtest; e->dwrite = dwrite; e->dfunc = dtest ? dfunc : 0; e->bias = bias;
    e->slope = bias ? S.slope : 0; e->units = bias ? S.units : 0; e->cmask = cmask & 15; e->samples = samples; e->dfmt = dfmt; e->cfmt = cfmt;
    e->p = SDL_CreateGPUGraphicsPipeline(g_dev, &pi);
    if (!e->p) { fprintf(stderr, "rb: pipeline (program %d): %s\n", prog, SDL_GetError()); exit(1); }
    g_npipes++;
    return e->p;
}

static SDL_GPUSampler *sampler(const rtex *t)
{
    int k;
    SDL_GPUSamplerCreateInfo si;
    rsamp *e;
    for (k = 0; k < g_nsamps; k++) {
        e = &g_samps[k];
        if (e->min == t->min && e->mag == t->mag && e->ws == t->ws && e->wt == t->wt) return e->s;
    }
    memset(&si, 0, sizeof si);
    si.min_filter = t->min == RB_NEAREST || t->min == RB_NEAREST_MIPMAP_NEAREST || t->min == RB_NEAREST_MIPMAP_LINEAR ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    si.mag_filter = t->mag == RB_NEAREST ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    si.mipmap_mode = t->min == RB_NEAREST_MIPMAP_LINEAR || t->min == RB_LINEAR_MIPMAP_LINEAR ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = t->ws == RB_CLAMP_TO_EDGE ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    si.address_mode_v = t->wt == RB_CLAMP_TO_EDGE ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    si.address_mode_w = si.address_mode_v;
    si.min_lod = 0;
    /* GL_NEAREST / GL_LINEAR minification samples level 0 only: the LOD clamped to 0.25 with the nearest level (the Vulkan
     * spec's emulation of them - the magnification / minification choice at LOD 0 stays, unlike a clamp to 0) */
    si.max_lod = t->min == RB_NEAREST || t->min == RB_LINEAR ? 0.25f : 1000.0f;
    g_samps = grow(g_samps, &g_capsamps, g_nsamps + 1, sizeof *g_samps);
    e = &g_samps[g_nsamps];
    e->min = t->min; e->mag = t->mag; e->ws = t->ws; e->wt = t->wt;
    e->s = SDL_CreateGPUSampler(g_dev, &si);
    if (!e->s) { fprintf(stderr, "rb: sampler: %s\n", SDL_GetError()); exit(1); }
    g_nsamps++;
    return e->s;
}

/* ---- textures and buffers */
rb_tex rb_tex_new(void)
{
    int k;
    for (k = 1; k < g_ntex; k++) if (!g_tex[k].live) break;
    if (k == g_ntex) { g_tex = grow(g_tex, &g_captex, g_ntex + 1, sizeof *g_tex); g_ntex++; }
    memset(&g_tex[k], 0, sizeof g_tex[k]);
    g_tex[k].live = 1;
    g_tex[k].min = RB_NEAREST_MIPMAP_LINEAR; g_tex[k].mag = RB_LINEAR;   /* GL's defaults */
    g_tex[k].ws = g_tex[k].wt = RB_REPEAT;
    return (rb_tex)k;
}

static void tex_release(rtex *t)
{
    if (t->used == g_serial || t->ref == g_serial) { release_later(0, t->t); release_later(0, t->msaa); release_later(0, t->depth); }
    else { release_now(0, t->t); release_now(0, t->msaa); release_now(0, t->depth); }
    t->t = t->msaa = t->depth = NULL;
}

void rb_tex_delete(rb_tex h)
{
    rtex *t = T(h);
    if (!t) return;
    tex_release(t);
    t->live = 0;
}

void rb_tex_filter(rb_tex h, int min, int mag) { rtex *t = T(h); if (t) { t->min = min; t->mag = mag; } }
void rb_tex_wrap(rb_tex h, int s, int tw) { rtex *t = T(h); if (t) { t->ws = s; t->wt = tw; } }

static int mip_levels(int w, int h) { int n = 1, m = w > h ? w : h; while (m > 1) { m >>= 1; n++; } return n; }

void rb_tex_image(rb_tex h, int w, int hh, const void *rgba, int mipmaps)
{
    rtex *t = T(h);
    int levels = mipmaps ? mip_levels(w, hh) : 1, inplace;
    rcmd *c;
    if (!t || w <= 0 || hh <= 0 || t->target) return;
    if (!t->t || t->w != w || t->h != hh || t->levels != levels) {
        SDL_GPUTextureCreateInfo ti;
        tex_release(t);
        memset(&ti, 0, sizeof ti);
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = COLOR_FMT;
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (levels > 1 ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
        ti.width = (Uint32)w; ti.height = (Uint32)hh; ti.layer_count_or_depth = 1; ti.num_levels = (Uint32)levels;
        ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
        t->t = SDL_CreateGPUTexture(g_dev, &ti);
        if (!t->t) { fprintf(stderr, "rb: texture %d x %d: %s\n", w, hh, SDL_GetError()); return; }
        t->w = w; t->h = hh; t->levels = levels;
        inplace = 0;   /* a new texture: no recorded draw has it */
    } else inplace = t->used == g_serial;
    t->ref = g_serial;
    if (inplace) c = cmd_new(C_UPTEX);
    else {
        g_pre = grow(g_pre, &g_cappre, g_npre + 1, sizeof *g_pre);
        c = &g_pre[g_npre++];
        memset(c, 0, sizeof *c);
        c->type = C_UPTEX;
    }
    c->ut = t->t; c->uw = (uint32_t)w; c->uh = (uint32_t)hh; c->mips = (uint8_t)(levels > 1);
    c->asize = (uint32_t)w * (uint32_t)hh * 4;
    c->aoff = (uint32_t)arena_put(&g_up, &g_upn, &g_upcap, rgba, c->asize, 512);
}

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
    rbuf *b = B(h);
    if (!b) return;
    if (b->used == g_serial || b->ref == g_serial) release_later(1, b->b); else release_now(1, b->b);
    b->b = NULL; b->live = 0;
}

static SDL_GPUBuffer *buffer_new(uint32_t size)
{
    SDL_GPUBufferCreateInfo bi;
    SDL_GPUBuffer *b;
    memset(&bi, 0, sizeof bi);
    bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    bi.size = size;
    b = SDL_CreateGPUBuffer(g_dev, &bi);
    if (!b) { fprintf(stderr, "rb: buffer %u bytes: %s\n", (unsigned)size, SDL_GetError()); exit(1); }
    return b;
}

static void upload_buffer(SDL_GPUBuffer *dst, int inplace, const void *data, uint32_t size)
{
    rcmd *c;
    if (inplace) c = cmd_new(C_UPBUF);
    else {
        g_pre = grow(g_pre, &g_cappre, g_npre + 1, sizeof *g_pre);
        c = &g_pre[g_npre++];
        memset(c, 0, sizeof *c);
        c->type = C_UPBUF;
    }
    c->ub = dst; c->asize = size;
    c->aoff = (uint32_t)arena_put(&g_up, &g_upn, &g_upcap, data, size, 16);
}

void rb_buf_data(rb_buf h, const void *data, size_t bytes)
{
    rbuf *b = B(h);
    uint32_t size = (uint32_t)((bytes + 3) & ~(size_t)3);
    int inplace;
    if (!b || size == 0) return;
    if (!b->b || size > b->cap) {
        uint32_t cap = b->cap ? b->cap : 4096;
        while (cap < size) cap *= 2;
        if (b->used == g_serial || b->ref == g_serial) release_later(1, b->b); else release_now(1, b->b);
        b->b = buffer_new(cap);
        b->cap = cap;
        inplace = 0;
    } else inplace = b->used == g_serial;
    b->ref = g_serial;
    if (bytes < size) {   /* (a length not a multiple of 4) */
        uint8_t *tmp = calloc(1, size);
        if (!tmp) return;
        memcpy(tmp, data, bytes);
        upload_buffer(b->b, inplace, tmp, size);
        free(tmp);
    } else upload_buffer(b->b, inplace, data, size);
}

/* the zero buffer, at least `bytes` long */
static SDL_GPUBuffer *zero_buffer(uint32_t bytes)
{
    if (!g_zero || g_zero_cap < bytes) {
        uint32_t cap = g_zero_cap ? g_zero_cap : 1u << 16;
        while (cap < bytes) cap *= 2;
        release_later(1, g_zero);   /* (a recorded draw may have it) */
        g_zero = buffer_new(cap);
        g_zero_cap = cap;
        upload_buffer(g_zero, 0, NULL, cap);
    }
    return g_zero;
}

void *rb_stream(size_t bytes, size_t *off)
{
    size_t at = arena_put(&g_st, &g_stn, &g_stcap, NULL, bytes, 16);
    *off = at;
    return g_st + at;
}

/* ---- targets */
static void target_make(rtex *t, int w, int h, int depth, int samples, SDL_GPUTextureFormat fmt)
{
    SDL_GPUTextureCreateInfo ti;
    memset(&ti, 0, sizeof ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = fmt;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = (Uint32)w; ti.height = (Uint32)h; ti.layer_count_or_depth = 1; ti.num_levels = 1;
    ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
    t->t = SDL_CreateGPUTexture(g_dev, &ti);
    if (samples > 1) {
        SDL_GPUSampleCount sc = samples >= 8 ? SDL_GPU_SAMPLECOUNT_8 : samples >= 4 ? SDL_GPU_SAMPLECOUNT_4 : SDL_GPU_SAMPLECOUNT_2;
        while (sc > SDL_GPU_SAMPLECOUNT_1 && !SDL_GPUTextureSupportsSampleCount(g_dev, fmt, sc)) sc = (SDL_GPUSampleCount)(sc - 1);
        samples = sc == SDL_GPU_SAMPLECOUNT_8 ? 8 : sc == SDL_GPU_SAMPLECOUNT_4 ? 4 : sc == SDL_GPU_SAMPLECOUNT_2 ? 2 : 1;
        if (samples > 1) {
            ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
            ti.sample_count = sc;
            t->msaa = SDL_CreateGPUTexture(g_dev, &ti);
        }
    }
    if (depth && g_depth_fmt != SDL_GPU_TEXTUREFORMAT_INVALID) {
        ti.format = g_depth_fmt;
        ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        ti.sample_count = t->msaa ? ti.sample_count : SDL_GPU_SAMPLECOUNT_1;
        t->depth = SDL_CreateGPUTexture(g_dev, &ti);
    }
    if (!t->t || (samples > 1 && !t->msaa) || (depth && !t->depth)) { fprintf(stderr, "rb: target %d x %d: %s\n", w, h, SDL_GetError()); exit(1); }
    t->w = w; t->h = h; t->levels = 1; t->samples = t->msaa ? samples : 1; t->target = 1; t->fmt = fmt;
    t->min = t->mag = RB_NEAREST; t->ws = t->wt = RB_CLAMP_TO_EDGE;
}

rb_tex rb_target_new(int w, int h, int depth)
{
    rb_tex k = rb_tex_new();
    target_make(&g_tex[k], w, h, depth, 1, COLOR_FMT);
    return k;
}

void rb_frame(int w, int h)
{
    rtex *t;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (!g_win_tex) g_win_tex = rb_tex_new();
    t = &g_tex[g_win_tex];
    if (!t->t || g_win_w != w || g_win_h != h || g_win_made != g_win_samples) {
        tex_release(t);
        target_make(t, w, h, 1, g_win_samples, g_win_fmt);
        g_win_w = w; g_win_h = h; g_win_made = g_win_samples;
        if (g_rec_target == g_win_tex) g_rec_target = 0;   /* (record the new one's target) */
    }
    if (!S.target) S.target = g_win_tex;
}

void rb_samples(int samples)
{
    g_win_samples = samples > 1 ? samples : 1;
    if (g_win_tex && g_win_w) rb_frame(g_win_w, g_win_h);
}

static rtex *cur_target(void)
{
    if (!S.target) { if (!g_win_tex) rb_frame(g_win_w ? g_win_w : 1, g_win_h ? g_win_h : 1); S.target = g_win_tex; }
    return &g_tex[S.target];
}
void rb_target(rb_tex t) { S.target = t ? t : g_win_tex; }
int rb_target_height(void) { return cur_target()->h; }

static void record_target(void)
{
    rtex *t = cur_target();
    if (g_rec_target != S.target) {
        rcmd *c = cmd_new(C_TARGET);
        c->tg.t = t->t; c->tg.msaa = t->msaa; c->tg.depth = t->depth; c->tg.w = t->w; c->tg.h = t->h; c->tg.samples = t->samples; c->tg.fmt = t->fmt;
        g_rec_target = S.target;
    }
    t->used = g_serial;
}

/* GL rectangle (bottom-left) -> top-left, clamped to the target */
static SDL_Rect rect_tl(const rtex *t, int x, int y, int w, int h)
{
    SDL_Rect r;
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > t->w) x1 = t->w;
    if (y1 > t->h) y1 = t->h;
    r.x = x; r.w = x1 > x ? x1 - x : 0;
    r.y = t->h - y1; r.h = y1 > y ? y1 - y : 0;
    if (r.y < 0) r.y = 0;
    return r;
}

/* ---- state */
void rb_viewport(int x, int y, int w, int h) { S.vp[0] = x; S.vp[1] = y; S.vp[2] = w; S.vp[3] = h; }
void rb_scissor(int on, int x, int y, int w, int h) { S.sc_on = on; S.sc[0] = x; S.sc[1] = y; S.sc[2] = w; S.sc[3] = h; }
void rb_blend(int mode) { S.blend = mode; }
void rb_depth_test(int on) { S.dtest = on; }
void rb_depth_write(int on) { S.dwrite = on; }
void rb_depth_func(int func) { S.dfunc = func; }
void rb_depth_bias(int on, float slope, float units) { S.bias = on; S.slope = slope; S.units = units; }
void rb_program(int prog) { S.prog = prog; }
void rb_texture(int unit, rb_tex t) { if (unit >= 0 && unit < MAXT) S.tex[unit] = t; }
void rb_vertex(int binding, rb_buf b, size_t offset) { if (binding >= 0 && binding < MAXV) { S.vb[binding] = b; S.voff[binding] = offset; } }
void rb_storage(rb_buf b) { S.sb = b; }

void rb_uniforms(int slot, const void *data, size_t bytes)
{
    if (slot < 0 || slot > 1) return;
    if (S.uvalid[slot] && S.ulen[slot] == bytes && memcmp(g_un + S.uoff[slot], data, bytes) == 0) return;
    S.uoff[slot] = (uint32_t)arena_put(&g_un, &g_unn, &g_uncap, data, bytes, 16);
    S.ulen[slot] = (uint32_t)bytes;
    S.uvalid[slot] = 1;
}

void rb_clear(const float rgba[4], int color, int depth)
{
    rtex *t;
    rcmd *c;
    SDL_Rect r;
    record_target();
    t = cur_target();
    r = S.sc_on ? rect_tl(t, S.sc[0], S.sc[1], S.sc[2], S.sc[3]) : rect_tl(t, 0, 0, t->w, t->h);
    if (r.w <= 0 || r.h <= 0) return;
    c = cmd_new(C_CLEAR);
    memcpy(c->col, rgba, sizeof c->col);
    c->cflags = (uint8_t)((color ? 1 : 0) | (depth && t->depth ? 2 : 0));
    c->sc = r;
    c->vp.x = (float)r.x; c->vp.y = (float)r.y; c->vp.w = (float)r.w; c->vp.h = (float)r.h; c->vp.min_depth = 0; c->vp.max_depth = 1;
    c->pipe = pipeline(PROG_CLEAR, t->depth != NULL, t->samples, (color ? 15 : 0) | (c->cflags & 2 ? 16 : 0), t->fmt);
    /* the whole target, colour and depth: the render pass's clear when it starts there */
    c->nv = (uint8_t)(r.x == 0 && r.y == 0 && r.w == t->w && r.h == t->h && c->cflags == (t->depth ? 3 : 1));
}

void rb_draw(int first, int count)
{
    rtex *t;
    rcmd *c;
    int k, prog = S.prog;
    if (count <= 0) return;
    record_target();
    t = cur_target();
    c = cmd_new(C_DRAW);
    c->prog = (uint8_t)prog;
    c->pipe = pipeline(prog, t->depth != NULL, t->samples, 15, t->fmt);
    c->nv = (uint8_t)PROG_NV[prog];
    for (k = 0; k < PROG_NV[prog]; k++) {
        const uint32_t st = PROG_STRIDE[prog][k], at = (uint32_t)first * st;
        if (S.vb[k] == RB_STREAM) { c->vstream[k] = 1; c->voff[k] = (uint32_t)S.voff[k] + at; }
        else if (B(S.vb[k]) && g_buf[S.vb[k]].b) { c->vb[k] = g_buf[S.vb[k]].b; c->voff[k] = (uint32_t)S.voff[k] + at; g_buf[S.vb[k]].used = g_serial; }
        else { c->vb[k] = zero_buffer(at + (uint32_t)count * st); c->voff[k] = at; }
    }
    if (prog == RB_PROG_WORLD_DOS) {
        if (B(S.sb) && g_buf[S.sb].b) { c->sb = g_buf[S.sb].b; g_buf[S.sb].used = g_serial; }
        else c->sb = zero_buffer(16);
    }
    c->nt = (uint8_t)rb_shader_blobs[PROG_FS[prog]].samplers;
    for (k = 0; k < c->nt; k++) {
        rtex *x = T(S.tex[k]);
        if (!x || !x->t) {   /* nothing bound: a 1 x 1 white texture (GL: an incomplete texture reads black - never relied on) */
            static rb_tex white;
            static const uint32_t W = 0xffffffffu;
            if (!white || !T(white)) { white = rb_tex_new(); rb_tex_image(white, 1, 1, &W, 0); rb_tex_filter(white, RB_NEAREST, RB_NEAREST); }
            x = T(white);
        }
        c->tx[k] = x->t; c->sm[k] = sampler(x);
        x->used = g_serial;
    }
    c->nu = (uint8_t)rb_shader_blobs[PROG_FS[prog]].uniform_buffers;
    for (k = 0; k < c->nu; k++) { c->uoff[k] = S.uoff[k]; c->ulen[k] = S.ulen[k]; }
    {   /* (the viewport unclamped: GL maps the whole rectangle, the target clips) */
        c->vp.x = (float)S.vp[0]; c->vp.y = (float)(t->h - S.vp[1] - S.vp[3]); c->vp.w = (float)S.vp[2]; c->vp.h = (float)S.vp[3];
        c->vp.min_depth = 0; c->vp.max_depth = 1;
        c->sc = S.sc_on ? rect_tl(t, S.sc[0], S.sc[1], S.sc[2], S.sc[3]) : rect_tl(t, 0, 0, t->w, t->h);
    }
    c->count = (uint32_t)count;
    if (g_ncmd >= 2) {   /* the same state as the draw before, its stream vertices just after: one draw (the HUD's quads) */
        rcmd *p = &g_cmd[g_ncmd - 2];
        if (p->type == C_DRAW && p->pipe == c->pipe && p->nv == c->nv && p->vstream[0] && c->vstream[0]
            && p->voff[0] + p->count * PROG_STRIDE[prog][0] == c->voff[0] && p->sb == c->sb && p->nt == c->nt && p->nu == c->nu
            && !memcmp(&p->vp, &c->vp, sizeof c->vp) && !memcmp(&p->sc, &c->sc, sizeof c->sc)) {
            int same = 1;
            for (k = 1; k < c->nv; k++) same &= !c->vstream[k] && !p->vstream[k] && p->vb[k] == c->vb[k] && p->voff[k] + p->count * PROG_STRIDE[prog][k] == c->voff[k];
            for (k = 0; k < c->nt; k++) same &= p->tx[k] == c->tx[k] && p->sm[k] == c->sm[k];
            for (k = 0; k < c->nu; k++) same &= p->uoff[k] == c->uoff[k] && p->ulen[k] == c->ulen[k];
            if (same) { p->count += c->count; g_ncmd--; }
        }
    }
}

static void blit_cmd(SDL_GPUTexture *st, int sx, int sy, int sw, int sh, SDL_GPUTexture *dt, int dx, int dy, int dw, int dh, int linear, int flip)
{
    rcmd *c = cmd_new(C_BLIT);
    c->blit.source.texture = st;
    c->blit.source.x = (Uint32)sx; c->blit.source.y = (Uint32)sy; c->blit.source.w = (Uint32)sw; c->blit.source.h = (Uint32)sh;
    c->blit.destination.texture = dt;
    c->blit.destination.x = (Uint32)dx; c->blit.destination.y = (Uint32)dy; c->blit.destination.w = (Uint32)dw; c->blit.destination.h = (Uint32)dh;
    c->blit.load_op = SDL_GPU_LOADOP_LOAD;
    c->blit.filter = linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    c->blit.flip_mode = flip ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE;
}
static int pow2_of(int v) { int n = 0; while (v > 0 && !(v & 1)) { v >>= 1; n++; } return n; }

void rb_blit(rb_tex src, int sx0, int sy0, int sx1, int sy1, rb_tex dst, int dx0, int dy0, int dx1, int dy1, int linear)
{
    rtex *s = T(src), *d = T(dst ? dst : g_win_tex);
    const int sw = sx1 - sx0, sh = sy1 - sy0, dw = dx1 - dx0, dh = dy1 - dy0;
    if (!s || !d || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    s->used = d->used = s->ref = d->ref = g_serial;
    /* A point-sampled scale whose rows can fall exactly on a texel edge - destination row j's centre (j + 0.5) sh / dh a
     * whole number, possible only when sh holds a higher power of 2 than 2 dh (640 x 480 -> 320 x 200: j = 2, 7, ...) -
     * takes, as OpenGL's glBlitFramebuffer, the row above that edge counted from the BOTTOM: the blit made on the
     * picture turned upside down (a 1:1 flip into a scratch target, the scale there, a 1:1 flip back) */
    if (!linear && sh != dh && pow2_of(sh) >= pow2_of(dh) + 1) {
        static rb_tex ta, tb;
        rtex *a = T(ta), *b = T(tb);
        if (!a || a->w != sw || a->h != sh) { if (a) rb_tex_delete(ta); ta = rb_target_new(sw, sh, 0); a = T(ta); }
        if (!b || b->w != dw || b->h != dh) { if (b) rb_tex_delete(tb); tb = rb_target_new(dw, dh, 0); b = T(tb); }
        a->used = b->used = a->ref = b->ref = g_serial;
        blit_cmd(s->t, sx0, s->h - sy1, sw, sh, a->t, 0, 0, sw, sh, 0, 1);
        blit_cmd(a->t, 0, 0, sw, sh, b->t, 0, 0, dw, dh, 0, 0);
        blit_cmd(b->t, 0, 0, dw, dh, d->t, dx0, d->h - dy1, dw, dh, 0, 1);
        return;
    }
    blit_cmd(s->t, sx0, s->h - sy1, sw, sh, d->t, dx0, d->h - dy1, dw, dh, linear, 0);
}

/* ---- running the list */
static SDL_GPURenderPass *P;
static SDL_GPUCommandBuffer *g_cb;
static rtgt g_pt;                     /* the pass's target */
static SDL_GPUGraphicsPipeline *g_lp;
static uint32_t g_pushed[2][2];       /* per stage (0 vertex, 1 fragment) and slot: the snapshot pushed + 1 */

static void pass_end(void) { if (P) { SDL_EndGPURenderPass(P); P = NULL; } }
static void pass_begin(int clear, const float *col)
{
    SDL_GPUColorTargetInfo ci;
    SDL_GPUDepthStencilTargetInfo di;
    memset(&ci, 0, sizeof ci);
    memset(&di, 0, sizeof di);
    ci.texture = g_pt.msaa ? g_pt.msaa : g_pt.t;
    if (g_pt.msaa) { ci.resolve_texture = g_pt.t; ci.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE; }
    else ci.store_op = SDL_GPU_STOREOP_STORE;
    ci.load_op = (clear & 1) ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    if (col) { ci.clear_color.r = col[0]; ci.clear_color.g = col[1]; ci.clear_color.b = col[2]; ci.clear_color.a = col[3]; }
    ci.cycle = clear == 3;
    if (g_pt.depth) {
        di.texture = g_pt.depth;
        di.load_op = (clear & 2) ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        di.store_op = SDL_GPU_STOREOP_STORE;
        di.stencil_load_op = (clear & 2) ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        di.stencil_store_op = SDL_GPU_STOREOP_STORE;
        di.clear_depth = 1.0f;
        di.cycle = clear == 3;
    }
    P = SDL_BeginGPURenderPass(g_cb, &ci, 1, g_pt.depth ? &di : NULL);
    g_lp = NULL;
    memset(g_pushed, 0, sizeof g_pushed);
}

static void copy_upload(SDL_GPUCopyPass *cp, const rcmd *c)
{
    if (c->type == C_UPTEX) {
        SDL_GPUTextureTransferInfo src;
        SDL_GPUTextureRegion dst;
        memset(&src, 0, sizeof src);
        memset(&dst, 0, sizeof dst);
        src.transfer_buffer = g_xfer; src.offset = c->aoff; src.pixels_per_row = c->uw; src.rows_per_layer = c->uh;
        dst.texture = c->ut; dst.w = c->uw; dst.h = c->uh; dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, true);
    } else {
        SDL_GPUTransferBufferLocation src;
        SDL_GPUBufferRegion dst;
        src.transfer_buffer = g_xfer; src.offset = c->aoff;
        dst.buffer = c->ub; dst.offset = 0; dst.size = c->asize;
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
    }
}

static void exec_list(SDL_GPUCommandBuffer *cb)
{
    int i, k;
    size_t total = g_upn + g_stn;
    g_cb = cb;
    if (total) {   /* the upload data and the stream in one transfer buffer */
        uint8_t *m;
        if (!g_xfer || g_xfer_cap < total) {
            SDL_GPUTransferBufferCreateInfo ti;
            uint32_t cap = g_xfer_cap ? g_xfer_cap : 1u << 20;
            while (cap < total) cap *= 2;
            if (g_xfer) SDL_ReleaseGPUTransferBuffer(g_dev, g_xfer);
            memset(&ti, 0, sizeof ti);
            ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; ti.size = cap;
            g_xfer = SDL_CreateGPUTransferBuffer(g_dev, &ti);
            if (!g_xfer) { fprintf(stderr, "rb: transfer buffer: %s\n", SDL_GetError()); exit(1); }
            g_xfer_cap = cap;
        }
        m = SDL_MapGPUTransferBuffer(g_dev, g_xfer, true);
        if (!m) { fprintf(stderr, "rb: map: %s\n", SDL_GetError()); exit(1); }
        if (g_upn) memcpy(m, g_up, g_upn);
        if (g_stn) memcpy(m + g_upn, g_st, g_stn);
        SDL_UnmapGPUTransferBuffer(g_dev, g_xfer);
    }
    if (g_stn && (!g_stream || g_stream_cap < g_stn)) {
        uint32_t cap = g_stream_cap ? g_stream_cap : 1u << 20;
        while (cap < g_stn) cap *= 2;
        release_now(1, g_stream);
        g_stream = buffer_new(cap);
        g_stream_cap = cap;
    }
    if (g_npre || g_stn) {
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
        if (g_stn) {
            SDL_GPUTransferBufferLocation src;
            SDL_GPUBufferRegion dst;
            src.transfer_buffer = g_xfer; src.offset = (Uint32)g_upn;
            dst.buffer = g_stream; dst.offset = 0; dst.size = (Uint32)g_stn;
            SDL_UploadToGPUBuffer(cp, &src, &dst, true);
        }
        for (i = 0; i < g_npre; i++) copy_upload(cp, &g_pre[i]);
        SDL_EndGPUCopyPass(cp);
        for (i = 0; i < g_npre; i++) if (g_pre[i].type == C_UPTEX && g_pre[i].mips) SDL_GenerateMipmapsForGPUTexture(cb, g_pre[i].ut);
    }
    memset(&g_pt, 0, sizeof g_pt);
    for (i = 0; i < g_ncmd; i++) {
        rcmd *c = &g_cmd[i];
        switch (c->type) {
        case C_TARGET:
            pass_end();
            g_pt = c->tg;
            break;
        case C_UPTEX: case C_UPBUF: {
            SDL_GPUCopyPass *cp;
            pass_end();
            cp = SDL_BeginGPUCopyPass(cb);
            copy_upload(cp, c);
            SDL_EndGPUCopyPass(cp);
            if (c->type == C_UPTEX && c->mips) SDL_GenerateMipmapsForGPUTexture(cb, c->ut);
            break;
        }
        case C_BLIT:
            pass_end();
            SDL_BlitGPUTexture(cb, &c->blit);
            break;
        case C_CLEAR:
            if (!P && c->nv) { pass_begin(3, c->col); break; }
            if (!P) pass_begin(0, NULL);
            {   /* a part of the target (or a pass already open): a triangle at the far depth, the clear colour */
                rb_clear_draw cd;
                memcpy(cd.u_color, c->col, sizeof cd.u_color);
                SDL_BindGPUGraphicsPipeline(P, c->pipe);
                g_lp = c->pipe;
                SDL_SetGPUViewport(P, &c->vp);
                SDL_SetGPUScissor(P, &c->sc);
                SDL_PushGPUFragmentUniformData(cb, 0, &cd, sizeof cd);
                g_pushed[1][0] = 0;
                SDL_DrawGPUPrimitives(P, 3, 1, 0, 0);
            }
            break;
        case C_DRAW: {
            SDL_GPUBufferBinding vb[MAXV];
            SDL_GPUTextureSamplerBinding ts[MAXT];
            if (!P) pass_begin(0, NULL);
            if (g_lp != c->pipe) { SDL_BindGPUGraphicsPipeline(P, c->pipe); g_lp = c->pipe; }
            SDL_SetGPUViewport(P, &c->vp);
            SDL_SetGPUScissor(P, &c->sc);
            for (k = 0; k < c->nv; k++) { vb[k].buffer = c->vstream[k] ? g_stream : c->vb[k]; vb[k].offset = c->voff[k]; }
            if (c->nv) SDL_BindGPUVertexBuffers(P, 0, vb, c->nv);
            if (c->sb) { SDL_BindGPUVertexStorageBuffers(P, 0, &c->sb, 1); SDL_BindGPUFragmentStorageBuffers(P, 0, &c->sb, 1); }
            for (k = 0; k < c->nt; k++) { ts[k].texture = c->tx[k]; ts[k].sampler = c->sm[k]; }
            if (c->nt) SDL_BindGPUFragmentSamplers(P, 0, ts, c->nt);
            for (k = 0; k < c->nu; k++) {
                if (c->prog != RB_PROG_HUD && g_pushed[0][k] != c->uoff[k] + 1) { SDL_PushGPUVertexUniformData(cb, (Uint32)k, g_un + c->uoff[k], c->ulen[k]); g_pushed[0][k] = c->uoff[k] + 1; }
                if (g_pushed[1][k] != c->uoff[k] + 1) { SDL_PushGPUFragmentUniformData(cb, (Uint32)k, g_un + c->uoff[k], c->ulen[k]); g_pushed[1][k] = c->uoff[k] + 1; }
            }
            SDL_DrawGPUPrimitives(P, c->count, 1, 0, 0);
            break;
        }
        }
    }
    pass_end();
    g_cb = NULL;
}

/* after the list has run: start the next */
static void list_reset(void)
{
    int i;
    for (i = 0; i + 1 < g_nrel; i += 2) release_now((int)(intptr_t)g_rel[i], g_rel[i + 1]);
    g_nrel = 0;
    g_ncmd = g_npre = 0;
    g_upn = g_stn = g_unn = 0;
    S.uvalid[0] = S.uvalid[1] = 0;
    g_rec_target = 0;
    g_serial++;
}

static SDL_GPUCommandBuffer *acquire(void)
{
    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(g_dev);
    if (!cb) { fprintf(stderr, "rb: command buffer: %s\n", SDL_GetError()); exit(1); }
    return cb;
}

void rb_finish(void)
{
    SDL_GPUCommandBuffer *cb;
    SDL_GPUFence *f;
    if (!g_dev) return;
    cb = acquire();
    exec_list(cb);
    f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    list_reset();
    if (f) { SDL_WaitForGPUFences(g_dev, true, &f, 1); SDL_ReleaseGPUFence(g_dev, f); }
}

void rb_present(void)
{
    SDL_GPUCommandBuffer *cb;
    SDL_GPUTexture *sc = NULL;
    Uint32 sw = 0, sh = 0;
    static int stats = -1;
    Uint64 t0 = 0, t1 = 0, t2 = 0;
    int ndraw = 0, npass = 0, i;
    if (!g_dev) return;
    if (stats < 0) stats = getenv("MW2_GPU_STATS") != NULL;
    if (stats) {
        for (i = 0; i < g_ncmd; i++) { ndraw += g_cmd[i].type == C_DRAW && g_cmd[i].prog != RB_PROG_HUD; npass += g_cmd[i].type != C_DRAW && g_cmd[i].type != C_CLEAR; }
        t0 = SDL_GetPerformanceCounter();
    }
    cb = acquire();
    exec_list(cb);
    if (stats) t1 = SDL_GetPerformanceCounter();
    if (g_win && g_win_tex && SDL_WaitAndAcquireGPUSwapchainTexture(cb, g_win, &sc, &sw, &sh) && sc) {
        const rtex *t = &g_tex[g_win_tex];
        if (g_backend == RB_GPU_VULKAN && t->fmt == SDL_GetGPUSwapchainTextureFormat(g_dev, g_win) && (Uint32)t->w == sw && (Uint32)t->h == sh) {
            {   /* the swapchain's format and size: a copy */
                SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
                SDL_GPUTextureLocation src, dst;
                memset(&src, 0, sizeof src);
                memset(&dst, 0, sizeof dst);
                src.texture = t->t; dst.texture = sc;
                SDL_CopyGPUTextureToTexture(cp, &src, &dst, sw, sh, 1, false);
                SDL_EndGPUCopyPass(cp);
                sc = NULL;
            }
        }
        if (sc) {
            SDL_GPUBlitInfo bi;
            memset(&bi, 0, sizeof bi);
            bi.source.texture = t->t; bi.source.w = (Uint32)t->w; bi.source.h = (Uint32)t->h;
            bi.destination.texture = sc; bi.destination.w = sw; bi.destination.h = sh;
            bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
            bi.filter = SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(cb, &bi);
        }
        g_tex[g_win_tex].used = g_serial;
    }
    if (stats) {
        SDL_GPUFence *f;
        t2 = SDL_GetPerformanceCounter();
        f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
        if (f) { SDL_WaitForGPUFences(g_dev, true, &f, 1); SDL_ReleaseGPUFence(g_dev, f); }
        {
            const double fr = (double)SDL_GetPerformanceFrequency();
            printf("gpu stats: %d commands, %d world draws, %d targets/blits/uploads, %d pipelines; upload %lu + stream %lu bytes; record %.2f ms, swapchain %.2f ms, GPU %.2f ms\n",
                   g_ncmd, ndraw, npass, g_npipes, (unsigned long)g_upn, (unsigned long)g_stn, 1000.0 * (double)(t1 - t0) / fr,
                   1000.0 * (double)(t2 - t1) / fr, 1000.0 * (double)(SDL_GetPerformanceCounter() - t2) / fr);
        }
    } else SDL_SubmitGPUCommandBuffer(cb);
    list_reset();
}

void rb_read_rgb(int x, int y, int w, int h, unsigned char *rgb)
{
    rtex *t;
    SDL_GPUCommandBuffer *cb;
    SDL_GPUCopyPass *cp;
    SDL_GPUTextureRegion src;
    SDL_GPUTextureTransferInfo dst;
    SDL_GPUFence *f;
    uint32_t need = (uint32_t)w * (uint32_t)h * 4;
    const uint8_t *m;
    int r, k, bgra;
    if (!g_dev || w <= 0 || h <= 0) return;
    t = cur_target();
    memset(rgb, 0, (size_t)w * (size_t)h * 3);
    if (x < 0 || y < 0 || x + w > t->w || y + h > t->h) return;
    if (!g_down || g_down_cap < need) {
        SDL_GPUTransferBufferCreateInfo ti;
        if (g_down) SDL_ReleaseGPUTransferBuffer(g_dev, g_down);
        memset(&ti, 0, sizeof ti);
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; ti.size = need;
        g_down = SDL_CreateGPUTransferBuffer(g_dev, &ti);
        g_down_cap = need;
        if (!g_down) return;
    }
    cb = acquire();
    exec_list(cb);
    cp = SDL_BeginGPUCopyPass(cb);
    memset(&src, 0, sizeof src);
    memset(&dst, 0, sizeof dst);
    src.texture = t->t; src.x = (Uint32)x; src.y = (Uint32)(t->h - y - h); src.w = (Uint32)w; src.h = (Uint32)h; src.d = 1;
    dst.transfer_buffer = g_down; dst.offset = 0; dst.pixels_per_row = (Uint32)w; dst.rows_per_layer = (Uint32)h;
    SDL_DownloadFromGPUTexture(cp, &src, &dst);
    SDL_EndGPUCopyPass(cp);
    t->used = g_serial;
    f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    list_reset();
    if (!f) return;
    SDL_WaitForGPUFences(g_dev, true, &f, 1);
    SDL_ReleaseGPUFence(g_dev, f);
    m = SDL_MapGPUTransferBuffer(g_dev, g_down, false);
    if (!m) return;
    bgra = t->fmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    for (r = 0; r < h; r++) {   /* GL's order: the bottom row first */
        const uint8_t *s = m + (size_t)(h - 1 - r) * (size_t)w * 4;
        unsigned char *d = rgb + (size_t)r * (size_t)w * 3;
        if (bgra) for (k = 0; k < w; k++) { d[k * 3] = s[k * 4 + 2]; d[k * 3 + 1] = s[k * 4 + 1]; d[k * 3 + 2] = s[k * 4]; }
        else for (k = 0; k < w; k++) { d[k * 3] = s[k * 4]; d[k * 3 + 1] = s[k * 4 + 1]; d[k * 3 + 2] = s[k * 4 + 2]; }
    }
    SDL_UnmapGPUTransferBuffer(g_dev, g_down);
}
