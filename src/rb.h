/*
 * rb.h - the port's render backend: the small set of drawing operations the world renderer (glr.c), the HUD (hud.c)
 * and the simulation's frame (tools/glview.c) use. One implementation today, rb_gpu.c on SDL3's GPU API (SDL_GPU):
 * Direct3D 12 or Vulkan on Windows, Vulkan on Linux, Metal on macOS. The calls keep OpenGL's model - a current
 * target, viewport, scissor, blend and depth state, textures with their own filtering, whole-buffer uploads, draws
 * that see everything set before them - so a second implementation (an OpenGL 2.1 renderer for older PCs is planned)
 * can sit behind the same header. Coordinates are OpenGL's: viewport, scissor, blit and read-back rectangles from the
 * target's bottom-left corner; texture rows top first as uploaded.
 *
 * rb_gpu.c records the frame and runs it at rb_present / rb_read_rgb / rb_finish: every upload made before a draw that
 * uses the resource reaches the GPU before that draw (a texture or buffer changed after a draw that used it is
 * uploaded in place, the earlier draw keeping the old contents), the frame's transient vertex data (rb_stream) goes
 * up in one copy, and uniform blocks are pushed only when they change.
 */
#ifndef MW2_RB_H
#define MW2_RB_H

#include <stddef.h>
#include <stdint.h>

struct SDL_Window;

typedef uint32_t rb_tex;   /* 0: none */
typedef uint32_t rb_buf;   /* 0: none (rb_vertex: zeros) */

/* texture filters and wraps: OpenGL's values, so settings written as GL enums read the same */
#define RB_NEAREST                0x2600
#define RB_LINEAR                 0x2601
#define RB_NEAREST_MIPMAP_NEAREST 0x2700
#define RB_LINEAR_MIPMAP_NEAREST  0x2701
#define RB_NEAREST_MIPMAP_LINEAR  0x2702
#define RB_LINEAR_MIPMAP_LINEAR   0x2703
#define RB_REPEAT                 0x2901
#define RB_CLAMP_TO_EDGE          0x812F

enum { RB_BLEND_OFF, RB_BLEND_ALPHA /* SRC_ALPHA, ONE_MINUS_SRC_ALPHA */, RB_BLEND_MUL /* DST_COLOR, ZERO */,
       RB_BLEND_ADD /* ONE, ONE */ };
enum { RB_LESS, RB_LEQUAL };
/* the programs (src/rb_shaders.c): the world (its 3D looks / the DOS look) and the HUD. Vertex bindings: the world
 * 0 = 13 floats (pos 3, normal 3, uv 2, colour 3, texsel, edge), 1 = the bounding sphere (4 floats), 2 = the Titanium
 * colour (4 floats); the HUD 0 = x, y, u, v. Uniform slots: the world 0 rb_world_frame, 1 rb_world_draw (both stages);
 * the HUD 0 rb_hud_draw (fragment). Texture units: the world 0 u_tex, 1 u_dos_tab, 2 u_ptab; the HUD 0 */
enum { RB_PROG_WORLD, RB_PROG_WORLD_DOS, RB_PROG_HUD, RB_PROG_COUNT };
/* the GPU backends (mw2port.cfg gpu=): AUTO = the platform's first (Windows Direct3D 12, then Vulkan) */
enum { RB_GPU_AUTO, RB_GPU_D3D12, RB_GPU_VULKAN, RB_GPU_METAL };
#define RB_STREAM 0xffffffffu   /* rb_vertex: the frame's transient data (rb_stream) */

/* Start: a GPU device for the window (NULL: none, off-screen targets only), the backend `pref` if it starts here,
 * else the next; samples > 1: the window's picture multisampled. Says on stdout which backend started. 0 if one did,
 * else -1 with the reason in err */
int  rb_init(struct SDL_Window *win, int pref, int samples, char *err, size_t errn);
void rb_quit(void);
const char *rb_backend_name(void);       /* "Direct3D 12", "Vulkan", "Metal" */
int  rb_backend(void);                   /* RB_GPU_D3D12 / VULKAN / METAL */
const char *rb_device_name(void);        /* the GPU and driver, as the backend reports them */

/* the window's picture: its size in pixels this frame, and multisampling (samples 1: off) */
void rb_frame(int w, int h);
void rb_samples(int samples);
/* an off-screen target (colour, with a depth buffer if depth), usable as a texture */
rb_tex rb_target_new(int w, int h, int depth);
void rb_target(rb_tex t);                /* draw into t (0: the window's picture) */
int  rb_target_height(void);
/* glBlitFramebuffer: colour from src's rectangle to dst's (0: the window's picture), bottom-left coordinates */
void rb_blit(rb_tex src, int sx0, int sy0, int sx1, int sy1, rb_tex dst, int dx0, int dy0, int dx1, int dy1, int linear);
/* glReadPixels(GL_RGB) of the current target: w x h RGB bytes, the bottom row first */
void rb_read_rgb(int x, int y, int w, int h, unsigned char *rgb);
void rb_finish(void);                    /* run everything recorded and wait for the GPU */
void rb_present(void);                   /* run the frame and show the window's picture */

/* state, as OpenGL's */
void rb_viewport(int x, int y, int w, int h);
void rb_scissor(int on, int x, int y, int w, int h);
void rb_clear(const float rgba[4], int color, int depth);   /* inside the scissor when it is on (glClear) */
void rb_blend(int mode);
void rb_depth_test(int on);
void rb_depth_write(int on);
void rb_depth_func(int func);
void rb_depth_bias(int on, float slope, float units);   /* glPolygonOffset(slope, units) */

/* textures: RGBA8, rows top first; mipmaps: the chain made from it */
rb_tex rb_tex_new(void);
void rb_tex_delete(rb_tex t);
void rb_tex_image(rb_tex t, int w, int h, const void *rgba, int mipmaps);
void rb_tex_filter(rb_tex t, int min, int mag);
void rb_tex_wrap(rb_tex t, int s, int tw);

/* vertex (and, for the DOS look, storage) buffers: the whole contents each time */
rb_buf rb_buf_new(void);
void rb_buf_delete(rb_buf b);
void rb_buf_data(rb_buf b, const void *data, size_t bytes);
/* the frame's transient vertex data: bytes to fill now, bound as rb_vertex(k, RB_STREAM, *off) */
void *rb_stream(size_t bytes, size_t *off);

/* drawing: the program, its uniform blocks (copied; compared with the last ones), textures, vertex buffers (offset in
 * bytes), the DOS look's storage buffer; rb_draw: triangles, vertices first .. first + count - 1 of the bound buffers */
void rb_program(int prog);
void rb_uniforms(int slot, const void *data, size_t bytes);
void rb_texture(int unit, rb_tex t);
void rb_vertex(int binding, rb_buf b, size_t offset);
void rb_storage(rb_buf b);
void rb_draw(int first, int count);

#endif
