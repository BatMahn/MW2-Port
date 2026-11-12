# The OpenGL 1.2 render backend — notes for a 32-bit x86 legacy build

This covers only the OpenGL 1.2 fixed-function backend (`RB=gl12`), written for the classic Mac / PowerPC build and
usable as-is for an x86 build for old PCs. Nothing here depends on the Mac platform layer (`platform/carbon/` is not
needed). Status: complete for every look; tested headless on Mesa (llvmpipe) only, never on real legacy hardware.

## 1. What to take

| File | Change |
|---|---|
| `src/rb_gl12.c` | **new** — device, textures, buffers, off-screen targets, state, the SDL3 context (bottom of the file) |
| `src/rb_gl12_world.c` | **new** — the world and HUD programs carried out on the fixed pipeline |
| `src/rb_gl12.h` | **new** — the backend's internal state (not part of `rb.h`) |
| `src/rb.h` | `RB_GPU_OPENGL` added to the backend enum; `uint64_t rb_window_flags(void)` added (the SDL_WindowFlags the backend's window needs) |
| `src/rb_gpu.c` | `rb_window_flags()` returning 0 (one line) |
| `tools/glview.c` | `sim_window()` ORs `rb_window_flags()` into the window flags (`SDL_WINDOW_OPENGL` for this backend) |
| `Makefile` | `RB=gl12` selects `src/rb_gl12.c src/rb_gl12_world.c` instead of `src/rb_gpu.c src/rb_shaders.c` and links the system GL (`-lGL`, `-lopengl32`, `-framework OpenGL`) |
| `docs/RENDERER_GL12.md` | **new** — the backend's own document (accuracy table, test switches) |

`rb_shaders.h` is still included, only for the uniform block structs (`rb_world_frame`, `rb_world_draw`,
`rb_hud_draw`); `rb_shaders.c` (the compiled shaders) is not linked. The backend is chosen at **build time**: a
legacy build is `make mw2 RB=gl12`. Both backends in one binary would need `rb_*` renamed per backend plus a
dispatch table — not done.

## 2. What it needs from the driver

- **OpenGL 1.2** (checked in `rb_init`; it refuses less): it uses `GL_CLAMP_TO_EDGE` and `GL_TEXTURE_BASE_LEVEL` /
  `GL_TEXTURE_MAX_LEVEL` (the Matrox look's per-band mip level).
- **GL_ARB_multitexture, optional**: two units give per-pixel fog in one pass; with one unit the fog is a second pass.
- Every other call is OpenGL **1.1**: on Windows `opengl32.dll` exports all of them; only `glActiveTextureARB` and
  `glClientActiveTextureARB` are looked up (`SDL_GL_GetProcAddress`).
- No buffer objects, no framebuffer objects, no shaders, no non-power-of-two textures, no float textures.
- The context: `SDL_GL_CreateContext` on the window; `SDL_GL_CONTEXT_MAJOR/MINOR_VERSION` are set to 2.1
  (`gl12_ctx_create`) — SDL makes a legacy context for < 3.0; **check on a real 1.x driver**, and if creation fails
  there, remove those two attribute calls. Depth 24, double buffered, swap interval 1 (0 on SDL's offscreen driver).

The 49 GL entry points used: `glAlphaFunc glBegin glBindTexture glBlendFunc glClear glClearColor glClearDepth
glColor3fv glColorMask glColorPointer glCopyTexSubImage2D glDeleteTextures glDepthFunc glDepthMask glDepthRange
glDisable glDisableClientState glDrawArrays glEnable glEnableClientState glEnd glFinish glGenTextures glGetIntegerv
glGetString glHint glLoadIdentity glMatrixMode glOrtho glPixelStorei glPolygonOffset glPolygonStipple glReadPixels
glScalef glScissor glShadeModel glTexCoord2f glTexCoordPointer glTexEnvi glTexImage1D glTexImage2D glTexParameteri
glTexSubImage2D glVertex2f glVertex4fv glVertexPointer glViewport` + the two ARB ones.

## 3. How it works (summary — details in docs/RENDERER_GL12.md)

- **Vertex stage on the CPU**: `world.vert` ported to C (`vertex_stage`); clip-space positions go to GL with identity
  matrices, so GL clips and rasterizes as the GPU did. Buffers stay in CPU memory.
- **Fragment stage as per-triangle fixed-function classes**, batched in order: texture × vertex colour (lighting per
  vertex), alpha test for the discards, texture *variants* for per-texel transforms (alpha², opaque, √alpha, white,
  sharp-upscaled, DOS shade levels).
- **Fog**: PowerVR's table is indexed by `density / z`, and `1/w` is screen-linear — the fog coordinate is sent as
  `(K, 0, 0, w)` (constant × w), which GL interpolates into exactly the table index per pixel, including for triangles
  clipped behind the eye. 1D texture on unit 1 with `GL_DECAL`.
- **Screen-linear (affine) texturing** for the ATi look: texture coordinates sent as `(s·w, t·w, 0, w)`.
- **Matrox LOD bands**: triangles split at view depths 2500 / 5000 / 15000, each piece at its mip level.
- **DOS look**: shade levels per polygon; Gouraud dither and the horizon band's dither by cutting along the shade's
  iso-lines and drawing the second index through a checkerboard `glPolygonStipple`.
- **HUD**: textures that aren't a power of two, or are larger than the card's limit (the heading tape is 2157×72),
  are kept at their own size in power-of-two tiles; each HUD triangle is cut at tile and wrap edges, so point-sampled
  sprites keep every texel in place. (World textures are all powers of two.)
- **Off-screen targets** (the low-resolution world, 320×200): drawn in the back buffer and copied to a texture
  (`glCopyTexSubImage2D`); the window's picture is copied aside and restored if it was drawn first.

## 4. Accuracy (vs the SDL_GPU renderer, Vulkan/lavapipe, 640×480, `MW2_TEST_SKY_STILL=1`)

Mean per-pixel difference (max channel, 0–255): 3Dfx 0.77, ATi 0.77, S3 0.76, PowerVR with fog 0.86 / 1.01 (YELL /
CYAN), Matrox 0.72 / 0.77, DOS 0.69 / 0.44. Under 1% of pixels differ by more than 32 levels — polygon edges, and in
the DOS look the span-coverage rule and the Gouraud checker's phase. Unchanged with a 1024 texture limit; with one
texture unit the PowerVR look is ~5 (fog per vertex on the sky's large triangles).

**Test gotcha**: the PowerVR fog only appears with `MW2_SKYGND_FOG=<game>/3d/pvr/skygnd.par` set (the shell sets it;
a hand-run `glview` doesn't) — without it fog comparisons silently compare nothing.

## 5. Test switches

`MW2_GL12_MAXTEX=n` (pretend the card's largest texture is n — 256 for Voodoo-class cards), `MW2_GL12_NO_MULTITEX=1`
(one texture unit), `MW2_GL12_NOFOG=1`, `MW2_GL12_TRACE=1` (padded / resampled textures, fog parameters).

## 6. Things specific to a 32-bit x86 build

- **x87 floating point vs the simulation**: the game's values are traced from the original and the 64-bit builds
  use SSE arithmetic. A 32-bit build defaults to x87 (80-bit intermediates), which can change float results and so
  simulation outcomes. Prefer `-msse2 -mfpmath=sse` if the minimum CPU allows it (Pentium 4 / Athlon 64); for older
  CPUs keep `-std=c99` (GCC then uses standard excess-precision rules) and **measure**: build `sweep_objectives` for
  32-bit and compare all 59 missions' logs with the 64-bit build's (the PowerPC build was checked this way — all 59
  identical). The renderer itself is not sensitive to this.
- **32-bit cleanliness**: the data loaders and the simulation already run identically as 32-bit big-endian PowerPC,
  so pointer-size assumptions are not expected. `rb_shaders.h` carries size checks on the uniform structs (floats and
  int32 only), so a layout difference would fail to compile rather than misrender.
- **Texture size limits**: Voodoo-class cards take 256×256 at most. HUD textures are tiled (exact); a world texture
  larger than the limit is downsampled (bilinear) — check the largest world texture against your minimum card.
- **Drivers reporting 1.1**: some old ICDs report OpenGL 1.1 but have `GL_EXT_texture_edge_clamp` /
  `GL_SGIS_texture_edge_clamp`. `rb_init` currently refuses them; relaxing it means accepting 1.1 + one of those
  extensions (same enum value) and skipping the Matrox look's LOD bands (`GL_TEXTURE_BASE/MAX_LEVEL` are 1.2-only).
- **16-bit colour**: the context asks for 8-8-8 colour and a 24-bit depth buffer; cards of that era may only offer
  16-bit colour / 16-bit depth — consider asking for 5-6-5 and 16 when 8-8-8 / 24 fail.
- **SDL3's own minimums**: SDL3 needs a reasonably recent OS (on Windows, not XP); a legacy Windows target older than
  that would need SDL2 instead, which `rb_gl12.c` doesn't use beyond window and context creation.
- **CPU cost**: the vertex stage runs on the CPU (per vertex, per frame); on period CPUs the per-vertex `sqrtf` and
  divides dominate — a cheaper path (cached static geometry, reciprocal estimates) is the first place to optimise.

## 7. Known approximations

A point light's direction is per vertex (3D looks) or per triangle (DOS look); the DOS depth fog without a fog key
is per triangle; the DOS span coverage (half a pixel more right/below) and the Gouraud checker phase per span are not
reproduced; wireframe edges are `GL_LINES`; no multisampling.
