# The renderer

The game draws through one small interface, `src/rb.h` ("render backend"). Everything above it - the world renderer
(`src/glr.c`), the HUD (`src/hud.c`), the simulation's frame, low-resolution target, drop screens and screenshots
(`tools/glview.c`), the headless renderer (`tools/glshot.c`) - calls only `rb_*`. One backend exists today:

| Backend | File | APIs |
|---|---|---|
| SDL_GPU | `src/rb_gpu.c` | Windows: Direct3D 12 (default) or Vulkan; Linux: Vulkan; macOS: Metal |

A second backend (an OpenGL 2.1 "legacy" renderer for older PCs is planned) is a new `src/rb_<name>.c` implementing the
same functions, chosen in the Makefile (or at run time by `rb_init`). Nothing else changes.

## The interface's model

The calls keep OpenGL's model, so the renderers above it read like the OpenGL 3.3 code they came from:

- **State is current** until changed: target, viewport, scissor, blend mode, depth test / write / function / bias,
  program, textures, vertex buffers, storage buffer, uniform blocks. `rb_draw(first, count)` draws triangles with what is
  set at that moment.
- **Coordinates are OpenGL's**: viewport, scissor, blit and read-back rectangles count from the target's bottom-left
  corner; `rb_read_rgb` returns rows bottom first (as `glReadPixels`). Texture uploads are rows top first.
- **Uploads are ordered as in OpenGL**: a texture or buffer changed after a draw that used it keeps the old contents
  for that draw (the backend copies in place between draws); data set before the first draw that uses it is there for it.
- **Texture filters and wraps use OpenGL's enum values** (`RB_LINEAR_MIPMAP_LINEAR` = `GL_LINEAR_MIPMAP_LINEAR`, ...),
  per texture, as `glTexParameteri` - `rb_tex_filter` / `rb_tex_wrap`.
- **Programs are fixed**: `RB_PROG_WORLD` (the 3D looks), `RB_PROG_WORLD_DOS` (the DOS look), `RB_PROG_HUD`, with the
  vertex layouts, uniform blocks and texture units listed in `rb.h`. The uniform blocks are C structs generated from the
  shaders (`src/rb_shaders.h`: `rb_world_frame`, `rb_world_draw`, `rb_hud_draw`), set with `rb_uniforms(slot, &block,
  sizeof block)`.
- **Transient vertices** (`rb_stream`) live for the frame: fill the returned memory, bind with
  `rb_vertex(k, RB_STREAM, offset)`.
- **Frame**: `rb_frame(w, h)` sizes the window's picture, `rb_present()` shows it, `rb_finish()` waits for the GPU.
  Off-screen targets (`rb_target_new`) can be drawn into, blitted (`rb_blit`) and sampled as textures.

| Group | Functions |
|---|---|
| device | `rb_init`, `rb_quit`, `rb_backend`, `rb_backend_name`, `rb_device_name` |
| frame / targets | `rb_frame`, `rb_samples`, `rb_target_new`, `rb_target`, `rb_target_height`, `rb_blit`, `rb_read_rgb`, `rb_finish`, `rb_present` |
| state | `rb_viewport`, `rb_scissor`, `rb_clear`, `rb_blend`, `rb_depth_test`, `rb_depth_write`, `rb_depth_func`, `rb_depth_bias` |
| textures | `rb_tex_new`, `rb_tex_delete`, `rb_tex_image`, `rb_tex_filter`, `rb_tex_wrap` |
| buffers | `rb_buf_new`, `rb_buf_delete`, `rb_buf_data`, `rb_stream` |
| drawing | `rb_program`, `rb_uniforms`, `rb_texture`, `rb_vertex`, `rb_storage`, `rb_draw` |

## The SDL_GPU backend (`src/rb_gpu.c`)

- **Recording**: calls are recorded into a list and run on one command buffer at `rb_present` / `rb_read_rgb` /
  `rb_finish`: one copy pass first (the frame's stream vertices and every upload to a resource no recorded draw used yet,
  from one transfer buffer, mip chains generated), then the commands in order (uploads to resources an earlier draw used
  go in place with SDL's cycling), then the window's picture to the swapchain (a copy on Vulkan when the formats match,
  else a blit).
- **Caches**: pipelines (program x blend x depth state x bias x target format x samples) and samplers are made once.
  Consecutive draws with the same state and adjoining stream vertices are merged (the HUD: about 900 quads a frame
  become about 30 draws). Uniform blocks are pushed only when they change.
- **Backend choice**: `mw2port.cfg` `gpu=auto|d3d12|vulkan` (Windows only; Linux is always Vulkan, macOS always Metal).
  On Windows the chosen one is tried first, then the other; if none starts the game says why (stderr and a message box)
  and exits. `MW2_GPU=vulkan|d3d12` overrides the setting (tests, troubleshooting).
- **The shell** (`tools/mw2shell.c`) draws with SDL's 2D renderer on the same API (`SDL_HINT_RENDER_DRIVER`
  `direct3d12` / `vulkan` / `metal`, with SDL's software renderer as the last resort).
- **Diagnostics**: `MW2_GPU_DEBUG=1` (SDL's GPU debug mode, validation layers when installed), `MW2_GPU_STATS=1`
  (each frame's commands, uploads and GPU time), `MW2_PERF=1` / `2` (frame timing; 2 waits for the GPU after the world).

### Conventions handled in the shaders

- **Clip depth**: the shaders compute OpenGL's clip z (-w..w) and turn it into SDL_GPU's 0..w at the end of the vertex
  shader, so the depth buffer holds OpenGL's window depth (0..1, depth range 0..1). Depth buffer: 24-bit where the GPU
  has it (D24), else 32-bit float (Apple GPUs).
- **Y**: SDL_GPU's clip space is y-up with a top-left framebuffer origin on every API, like OpenGL's picture once read
  back. Fragment-shader code that used `gl_FragCoord.y` (bottom-left origin) uses `u_fbh - gl_FragCoord.y`.
- **Vertex index**: every draw starts at vertex 0 of buffers bound at the batch's byte offset; the shader's vertex number
  is `u_first + gl_VertexIndex` (Vulkan's `gl_VertexIndex`, D3D12's `SV_VertexID` and Metal's `[[vertex_id]]` agree only
  with a zero first vertex). The DOS look's per-layer vertex table (an OpenGL buffer texture before) is a read-only
  storage buffer.
- **Texture level 0 only** (`RB_NEAREST` / `RB_LINEAR` minification on a mipmapped texture): sampler max LOD 0.25 with the
  nearest mip mode (the Vulkan specification's emulation of OpenGL's non-mipmapped minification).
- **Binding slots** (SDL_CreateGPUShader's rules): SPIR-V vertex set 0 = storage buffers, set 1 = uniform blocks;
  fragment set 2 = samplers then storage buffers, set 3 = uniform blocks. DXIL: the same as register spaces 0-3
  (t / s / b registers). MSL: `[[buffer]]` uniform blocks first, then storage buffers; `[[texture]]` / `[[sampler]]` by
  unit.

## Shaders

Written once in GLSL 4.50 (`shaders/*.vert`, `*.frag`, `world_common.glsl`), compiled offline by
`scripts/gen_shaders.py` (`make shaders`) into `src/rb_shaders.c` / `.h`, which are checked in: building the game needs
no shader tools.

    GLSL 450 --glslangValidator (-V, Vulkan 1.0)--> SPIR-V          Vulkan
    SPIR-V   --spirv-cross --msl (MSL 2.1)------> Metal source      Metal (compiled by the driver at start)
    SPIR-V   --spirv-cross --hlsl (SM 6.0)------> HLSL --dxc------> DXIL (signed by dxc's built-in validator)  Direct3D 12

The world shader is built twice (`DOS_LOOK` 0 / 1). The uniform blocks' std140 offsets are read back from SPIR-V
(`spirv-cross --reflect`) and written as C structs with size checks. Tools used for the checked-in files (any later
version should do): glslangValidator 15.1.0 (Ubuntu 24.04 `glslang-tools`), SPIRV-Cross git 15142bb
(2026-09-28, C API 0.68.0), DirectXShaderCompiler 1.10.2605 (libdxcompiler / built-in validator). `make shaders SHADER_TOOLS=<folder>`
finds them in `<folder>/bin` (dxc's `lib/` beside it) when they are not on PATH. The output is deterministic:
regenerating with the same tools gives byte-identical files.

## Adding a backend (e.g. OpenGL 2.1)

1. Implement every function in `rb.h` in `src/rb_<name>.c` (keep OpenGL's model above; the GL 2.1 path can map it almost
   one to one - `rb_storage` as a texture-based vertex table or a CPU-side fallback for the DOS look).
2. Provide shaders for its programs (GLSL 1.20 versions of `shaders/`, or generated by `spirv-cross --version 120`
   where the features allow) and fill the same uniform blocks.
3. Add the choice to `rb_init` / `gpu=` (`RB_GPU_*`, `PORTCFG_GPU_*`, the Combat Variables row) and the Makefile.
