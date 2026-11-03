# The OpenGL 1.2 renderer (`RB=gl12`)

The legacy build's renderer: `src/rb.h` on OpenGL 1.2's fixed-function pipeline, for the graphics cards of the
Windows XP years. `src/rb_gl12.c` (context, textures, buffers, off-screen targets, state, batching),
`src/rb_gl12_world.c` (the world and HUD programs), `src/rb_gl12.h` (its internal state). Chosen when building:
`make mw2 RB=gl12` (the legacy Windows build adds `SDL=2`, README "Build"). It draws at once, call by call; nothing is
recorded.

## What it needs from the driver

- **OpenGL 1.2** - or **1.1 with `GL_EXT_texture_edge_clamp` / `GL_SGIS_texture_edge_clamp`** (the same
  `GL_CLAMP_TO_EDGE`); a 1.1 driver then skips the Matrox look's per-band mip levels (`GL_TEXTURE_BASE/MAX_LEVEL` are
  1.2's). Anything less is refused with the reason (`rb_init`, the game's message box).
- **`GL_ARB_multitexture`, optional**: two units give the fog and the HUD flash in the same pass; with one unit they are
  a second pass.
- Only OpenGL **1.1** entry points are linked (on Windows `opengl32.dll` has them all); `glActiveTextureARB` and
  `glClientActiveTextureARB` are looked up. No buffer objects, framebuffer objects, shaders, non-power-of-two or float
  textures. The 39 GL 1.1 calls: `glAlphaFunc glBindTexture glBlendFunc glClear glClearColor glClearDepth glColorPointer
  glCopyTexSubImage2D glDeleteTextures glDepthFunc glDepthMask glDepthRange glDisable glDisableClientState glDrawArrays
  glEnable glEnableClientState glFinish glGenTextures glGetIntegerv glGetString glHint glLineWidth glLoadIdentity
  glMatrixMode glPixelStorei glPolygonOffset glPolygonStipple glReadPixels glScissor glShadeModel glTexCoordPointer
  glTexEnvfv glTexEnvi glTexImage1D glTexImage2D glTexParameteri glVertexPointer glViewport` + the two ARB ones.
- **The context**: SDL's default (no version asked: a legacy context, whatever 1.x the driver has), double-buffered,
  8-8-8 colour with a 24-bit depth buffer, else 5-6-5 with 16 (`rb_window_flags(low)`, tried in that order by
  `tools/glview.c`), swap interval 1 (0 on SDL's offscreen driver). The largest texture is the driver's
  `GL_MAX_TEXTURE_SIZE`.

## How it works

- **Vertex stage on the CPU**: `shaders/world.vert` in C (`vertex_stage`: the billboards' screen squares, the DOS
  cockpit order, the view-distance cull, the DOS fog key, ...). The clip-space positions go to GL with identity
  matrices, so GL clips and rasterizes as the GPU did; the depth stays OpenGL's (-w..w, depth range 0..1). Vertex
  buffers are CPU memory; client-side vertex arrays.
- **Fragment stage as per-triangle classes**, batched in draw order: texture x vertex colour (the lighting, tint, damage
  shade and palette fade worked out per vertex), the alpha test for the shader's discards (0.5, or any alpha for the
  blended kinds), and **texture variants** for the per-texel steps: alpha squared (the 3Dfx ARGB4444 entries), opaque
  (ARGB1555), square-rooted alpha (the HUD's smooth sprites), white (the one-unit flash), each texel an n x n block (the
  HUD font's "sharp bilinear" is bilinear filtering of that upscale - the same weights), and the DOS shade levels (each
  texel's palette index through level L of the DOS table, as colours).
- **PowerVR table fog**: the table is indexed by `density / z`, and `1/w` is screen-linear: the fog coordinate is sent as
  `(K, 0, 0, z)` at each corner (K = density / 2048), which GL's perspective interpolation turns into exactly `K / z` per
  pixel - also for triangles clipped at the near plane. A 2048-entry 1D texture (RGB the fog colour, A the factor) on
  unit 1 with `GL_DECAL`. The Titanium fog's per-pixel kind (linear in the view depth up to Z) uses a ramp texture the
  same way, the triangle split at Z; its per-vertex kind (Rendition) is sent screen-linear.
- **Screen-linear (affine) texels** (the ATi look's types): coordinates `(s w, t w, 0, w)`.
- **Matrox mip bands**: the ground's triangles split at view depths 2500 / 5000 / 15000, each piece at its level
  (`GL_TEXTURE_BASE_LEVEL` = `GL_TEXTURE_MAX_LEVEL`).
- **DOS look**: the shade level per polygon (normal, light, fog key). A Gouraud polygon is drawn as MW2.EXE's fill
  (0x59e77) makes it, as the SDL_GPU shader computes it per pixel: its corners projected in the DOS integers (0x3dfb7),
  its two chains walked row by row, each row's span from the left chain's x to the right's inclusive, the shade stepped
  per pixel pair, the dither's 0x7fff on the pixels with k + r even - each row's runs of one index pair go out as
  quads textured with a 2 x 256 "palette pair" texture (row n: colours n and n + 1) whose s coordinate is window
  x + y + phase, so the checkerboard comes from the texture (no stipple, no state change: the flat colours share the
  batch). A polygon crossing the near plane (and the smooth option) is cut along the shade's iso-lines in screen space
  instead, its checkerboard through complementary `glPolygonStipple` patterns in row bands of one phase. The horizon
  band (0x3c000) the same way.
- **Wireframe** (image enhancement): the insides black (pushed back with a polygon offset), each edge as the shader has
  it - the band of the triangle within |n.x| + |n.y| pixels of the edge (`bary / fwidth(bary) <= 1`), cut in screen
  space; `GL_LINES` (2 px) for a triangle that crosses the near plane.
- **HUD**: textures that aren't a power of two, or are larger than the card's limit (the heading tape is 2157 x 72), are
  kept at their own size in power-of-two tiles with a one-texel border; each HUD triangle is cut at the tiles' and the
  wrap's edges - every texel stays in place, point-sampled or bilinear. The flash: unit 1 in `GL_BLEND` (one unit: an
  added second pass). World textures are powers of two; one that isn't, or is larger than the limit, is resampled
  (bilinear) - `MW2_GL12_TRACE` lists them (the DOS look's weapon-effect sprites).
- **Off-screen targets** (the low-resolution frame, 640 x 480 / 320 x 200): drawn in the back buffer and kept in
  textures with `glCopyTexSubImage2D` (tiled the same way) while another is drawn; the window's picture is kept and put
  back the same way if it was drawn first. `rb_blit` draws from those textures; point-sampled, a pixel whose centre falls
  exactly on a texel edge takes the texel left of / below it, as the SDL_GPU backend's blit does (measured: lavapipe).
  A target must fit in the window (`render=1024x768` in a smaller window draws only its lower left part, said once on
  stderr).

## Accuracy

Against the SDL_GPU renderer (Vulkan, Mesa lavapipe) on Mesa llvmpipe (OpenGL 4.5 compatibility profile, used as 1.2),
640 x 480, YELLSCN1 at 3 s autopilot, `MW2_TEST_SKY_STILL=1`, `aa=0`: mean per-pixel difference (largest channel,
0-255) and the share of pixels more than 32 levels off.

| Look | mean | > 32 |
|---|---|---|
| ENHANCED (3Dfx + PowerVR fog) | 0.28 | 0.00 % |
| 3DFX | 0.23 | 0.00 % |
| ATI RAGE | 0.21 | 0.00 % |
| S3 VIRGE | 0.21 | 0.00 % |
| POWERVR (fog) | 0.22 | 0.00 % |
| MATROX MYSTIQUE | 0.20 | 0.05 % |
| DOS (dithered) | 0.01 | 0.00 % |
| DOS, smooth shading and sky | 0.50 | 0.46 % |
| TITANIUM textures | 0.88 | 0.00 % |
| wireframe (image enhancement): ENHANCED / DOS | 0.57 / 0.15 | 0.30 / 0.10 % |
| CYANSCN1 at 4 s: ENHANCED / POWERVR / DOS | 0.90 / 0.81 / 0.01 | 0.00 % |
| PROVSCN1 (dithered terrain): DOS | 0.03 | 0.00 % |
| `MW2_GL12_MAXTEX=256`: ENHANCED / POWERVR / DOS / TITANIUM | 0.40 / 0.39 / 0.01 / 0.88 | 0.00 % |
| `MW2_GL12_NO_MULTITEX=1`: ENHANCED / POWERVR / TITANIUM | 0.36 / 0.31 / 1.07 | 0.00 % |
| `render=320x200`: ENHANCED / DOS | 1.24 / 1.07 | 0.46 / 0.36 % |

What differs: polygon edges (GL's rasterizer vs Vulkan's, a few pixels). The 320 x 200 frame point-samples the
640 x 480 one, which magnifies them, and there the reference itself rounds unevenly: lavapipe's blit takes the texel
left of an exact edge for the first 81 columns and the right one for some after (its normalised coordinates in float),
this renderer always the left one. The DOS look's DOSBox-exact sequences (dos_shading_dither, dos_cockpit_projection,
dos_fog_polygon_key, ...) pass with this renderer too.

**Approximations**: a point light's direction per corner (3D looks) or per triangle (DOS look); the DOS depth fog
without a fog key per triangle; a near-clipped DOS Gouraud polygon (GL's coverage, the checker phase per row band from
its own chains); the wireframe's edges as `GL_LINES` for a triangle crossing the near plane; no multisampling; a
texture alpha-squared or resampled before filtering, not after.

**Test gotcha**: the PowerVR fog appears only with `MW2_SKYGND_FOG=<game>/3d/pvr/skygnd.par` (the shell sets it; a
hand-run `glview` doesn't) - without it fog comparisons compare nothing.

## Tests and switches

- `make test_sequences_gl12 SEQ_ARGS="--only NAME,..."`: the sequence suite against a glview built with this renderer
  (`build/glview_gl12`, the system's OpenGL; headless on Mesa's llvmpipe).
- `MW2_GL12_MAXTEX=n` (pretend the card's largest texture is n - 256 as a Voodoo 3), `MW2_GL12_NO_MULTITEX=1` (one
  texture unit), `MW2_GL12_NOFOG=1` (no fog), `MW2_GL12_TRACE=1` (the extensions, textures tiled / resampled, fog).

## Limits

- No multisampling (the RESOLUTION row's AA steps do nothing); `GPU OPENGL` in Combat Variables.
- CPU cost: the vertex stage runs per vertex per frame on the CPU; on a period CPU the per-vertex transforms dominate
  (a first place to optimise: static geometry cached between frames).
- Not yet run on a real OpenGL 1.x driver or Windows XP (KNOWN_ISSUES.md).
