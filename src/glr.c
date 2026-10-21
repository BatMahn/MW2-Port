/* glr.c - see glr.h. OpenGL 3.3 core only. */
#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#elif defined(_WIN32)
#include "glload.h"   /* GL 2.0+ looked up at runtime (opengl32.dll exports only 1.1) */
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include "glr.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLOATS_PER_VERTEX 13 /* pos3 normal3 uv2 color3 texsel1 edge1 */

#define GLR_LAYERS 2
/* batches: 0..255 the surface slots, +0 untextured, +1 shadows, +2 unlit plain colours, then GLR_B0 + n the bank-0
 * bitmaps (colour type 3 billboards); textures: tex[0..255] surfaces, tex[256 + n] bank 0 */
#define GLR_B0      (GLR_SLOTS + 3)
#define GLR_TI      (GLR_B0 + GLR_SLOTS)    /* + n: the Titanium models' slot n (glr_set_titanium); textures tex[512 + n] */
#define GLR_TB      (GLR_TI + GLR_SLOTS)    /* + n: the Titanium bank-0 bitmap n (its models' colour type 3 billboards,
                                             * glr_set_titanium_extra); textures tex[768 + n] */
#define GLR_BATCHES (GLR_TB + GLR_SLOTS)   /* 0: main mech or static world, 1: actors (re-uploaded per frame) */

/* A Titanium polygon coloured per vertex on the CPU, as mercsw does (glr_draw -> ti_colours): its fan's vertices are
 * first .. first + 3 (n - 2) - 1 in the layer's buffer (corner 0, j, j + 1 per triangle) */
typedef struct {
    int     first, n;
    uint8_t kind;      /* 1 lit (textured kinds 5-7: Gouraud, 0x1002ecaa + 0x1002f238), 2 colour kind 1 (a palette shade,
                        * 0x1002f5e3), 3 colour kind 2 (the shade with intensity 0xa0 / 0xd0; textures off only) */
    uint8_t c;         /* kinds 1 / 2: the shade's intensity >> 1; 1 (textured): the switched-off shade's (0xa0 / 0xd0) */
    uint8_t ramp;      /* kinds 1 / 2: the palette ramp x 16; 1 (textured): the switched-off shade's */
    uint8_t cls;       /* 1 (textured): its texture switch class (1 OBJECT, 2 TERRAIN, 0 none) - switched off, the kind 1
                        * shade (0x1001b47a..0x1001b4f3), drawn as kind 3 */
    float   nrm[3], v0[3];   /* kinds 1 / 2: the polygon's normal and corner 0 in the model's frame (game coordinates) */
} glr_tipoly;

typedef struct {
    GLuint  vao, vbo;
    GLuint  tbo;       /* the DOS look: vbo as a buffer texture (R32F), so a Gouraud fragment can read its whole polygon */
    GLuint  sph_vbo;   /* per vertex: its object's bounding sphere (GL centre, radius) for the view-distance cull */
    GLuint  tic_vbo;   /* per vertex: the Titanium CPU colour (rgb) and its use (a: 0 none, 1 x the texel (lit), 2 the
                        * colour (a palette shade), 3 kind 2: the colour, drawn only with its textures off) */
    GLint   batch_first[GLR_BATCHES];   /* + GLR_SLOTS: untextured; + 1: shadows; + 2: unlit plain colours (type 0); GLR_B0 + n bank 0 */
    GLsizei batch_count[GLR_BATCHES];
    GLsizei vertex_count;
    float      *tic;       /* vertex_count x 4: what tic_vbo holds */
    float      *tipn;      /* vertex_count x 6: the GL position and the normal the lights use (lit polygons) */
    float      *tistat;    /* vertex_count x 3: the static lights' colour (lit polygons) */
    glr_tipoly *tip;
    int         tip_n, tic_static_ok, tic_dyn;
    float       tic_key[24];   /* the static lights tistat was made with */
} glr_layer;

struct glr {
    GLuint prog, tex[GLR_SLOTS * 4];
    texture *const *bank0;                  /* glr_set_bank0 (the caller's array) */
    glr_layer layer[GLR_LAYERS];
    GLuint env_vao, env_vbo, env_tex[2];   /* 0 ground, 1 sky */
    GLuint ground_flat;                    /* 1x1: the ground texture's average colour (terrain textures off) */
    GLuint sky_flat;                       /* 1x1: the sky texture's average colour (Textured Sky off) */
    int    env_has[2];
    float  tile_sky, tile_ground;
    int    tex_w[GLR_SLOTS * 4], tex_h[GLR_SLOTS * 4], has_tex[GLR_SLOTS * 4];
    const texture *tex_src[GLR_SLOTS * 4];  /* skip re-uploading the same texture */
    const texture *slot_frame[GLR_SLOTS];   /* glr_slot_frame: the current frame of an animated slot (NULL: the slot's own) */
    texture *const *ti_slots;               /* glr_set_titanium (the caller's array), NULL off */
    int    ti_camo[8];
    texture *const *ti_bank0;               /* glr_set_titanium_extra: the Titanium bank-0 bitmaps (billboards) */
    glr_ti_shade ti_shade;                  /* glr_set_titanium_extra: the palette shade of its colour type 1 polygons */
    uint8_t ti_pal[2][256][3];              /* per layer: the palette its colour kinds 1 / 2 shade in (set_layer) */
    GLint  u_tilit, u_tiamb, u_tilt, u_tilv, u_tilc, u_tignd, u_tidn, u_tidp, u_tidc;

    float  center[3], radius;
    GLint  u_point, u_lpos;
    GLint  u_mono, u_mvp, u_view, u_light, u_ambient, u_fog_density, u_fog_color, u_texsize, u_tex, u_textured, u_unlit, u_wire, u_shadow;
    /* the DOS edition */
    int    dos;
    GLuint dos_tex;                         /* 256 x 17: rows 0-15 the LUMA table (as indices), row 16 the palette (RGB) */
    GLint  u_dos, u_dos_tab, u_dos_lpos, u_dos_amb, u_dos_fogdist, u_dos_dir, u_dos_vtx, u_dos_smooth, u_vpo;
    GLint  u_far, u_vp;
    /* the palette fade of the 3D editions (0x1002ad50): textured polygons in a flat colour, untextured ones toward a
     * target palette by their palette index (glr_set_world_fade) */
    GLuint ptab_tex;
    GLint  u_tint, u_pfade, u_ptab, u_notex, u_tinton, u_dsign;
    int    dsign;                           /* glr_set_damage_shade */
    float  tint[3], pfade;
    int    tint_on;
    GLint  u_alpha, u_mga_lod, u_horizon, u_hz_r, u_hz_u, u_hz_f, u_hz_c, u_hz_band, u_hz_x, u_hz_smooth, u_tifog, u_blend;
    /* the Titanium edition's sky dome (glr_set_skydome): triangles (pos, texel uv / (size - 1), slot) by slot */
    GLuint dome_vao, dome_vbo, dome_tex[GLR_SLOTS];
    float *dome_v;                          /* x, y, z, u, v per vertex (game coordinates, GL z) */
    int    dome_n, dome_first[GLR_SLOTS], dome_count[GLR_SLOTS], dome_has[GLR_SLOTS];
    int   *dome_pfirst, *dome_pcount, *dome_pslot, dome_np;   /* per polygon, in the record's order */
    uint8_t dos_img[17][256][4];
    /* sprites (weapon effects) */
    GLuint spr_vao, spr_vbo;
    glr_particle *par;                      /* glr_set_particles */
    int    par_n, par_cap;
    float  par_alpha;
    glr_sprite spr[GLR_MAX_SPRITES];
    int    spr_n;
    const texture *spr_src[64];
    GLuint spr_tex[64];
    int    spr_next;
};

static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec3 a_normal;\n"
    "layout(location=2) in vec2 a_uv;\n"
    "layout(location=3) in vec3 a_color;\n"
    "layout(location=4) in float a_texsel;\n"
    "layout(location=5) in float a_edge;\n"
    "layout(location=6) in vec4 a_sphere;\n"
    "layout(location=7) in vec4 a_tic;\n"
    "uniform mat4 u_mvp, u_view;\n"
    "uniform float u_far; uniform vec2 u_vp;\n"
    "out vec3 v_normal; out vec2 v_uv; out vec3 v_color; flat out int v_texsel; out vec3 v_viewpos; out vec3 v_world;\n"
    "out vec3 v_bary; flat out int v_emask, v_bb, v_tif, v_dnib; flat out vec4 v_sph;\n"
    "out vec3 v_tic; flat out int v_ticm;\n"
    "noperspective out vec2 v_gs;\n"   /* the DOS Gouraud shade (a_uv), screen-linear as MW2.EXE 0x59e77 steps it */
    "void main() {\n"
    "  int e = int(a_edge + 0.5);\n"
    "  v_dnib = e / 2048; e -= v_dnib * 2048;\n"   /* a mech location's damage nibble (glr_set_damage_shade) */
    "  v_bb = e >= 1024 ? 1 : 0;\n"
    /* Titanium (edge code + 256: its model normal, length kept; + 512: a flat palette colour, unlit) */
    "  v_tif = 0;\n"
    "  if (e < 1024 && e >= 512) { v_tif |= 2; e -= 512; }\n"
    "  if (e < 1024 && e >= 256) { v_tif |= 1; e -= 256; }\n"
    "  vec3 p = a_pos, nrm = a_normal;\n"
    "  gl_Position = u_mvp * vec4(p, 1.0);\n"
    /* a colour type 3 billboard (edge code + 1024; engine 0x10026fc7 -> 0x1002a8e0, mode 0): a_pos the foot (texel u 0,
     * v > 0), a_normal the top (u 0, v 0), a_texsel the corner 0-3. Both are projected; with A the top's and B the foot's
     * screen point (y down) and d = (B - A) / 2 the corners are A + (-d.y, d.x), A + (d.y, -d.x), B + (d.y, -d.x),
     * B + (-d.y, d.x) - a square of the edge's screen length, centred on it, whatever the view's pitch or roll - texel
     * corners (0,0) (1,0) (1,1) (0,1); screen-linear (q = 1), no lighting (0x1002c880 with intensity -1: colour 255) */
    "  if (e >= 1024) {\n"
    "    vec4 cb = u_mvp * vec4(a_pos, 1.0), ca = u_mvp * vec4(a_normal, 1.0);\n"
    "    int k = int(a_texsel + 0.5), tib = k >= 4 ? 1 : 0;\n"
    "    e -= 1024; nrm = vec3(0.0); k -= 4 * tib;\n"
    "    if (ca.w <= 1.0 || cb.w <= 1.0) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);\n"
    "    else if (tib != 0) {\n"
    /* Titanium (mercsw 0x1004ef4c, mode 0): T the top's and F the foot's screen point, dx = (F.y - T.y) / 2, dy = (F.x -
     * T.x) / 2: corner 0 (T.x - dx, T.y - dy), 1 (F.x - dx, F.y + dy), 2 (T.x + dx, F.y - dy), 3 (F.x + dx, T.y + dy) -
     * the square of 0x10026fc7 for an upright edge, not square once it leans (the engine's own mix of T and F); the
     * corners' sz: 0 and 3 the top's, 1 and 2 the foot's (+0x18 of each, 0x1004f121..0x1004f1a5) */
    "      vec2 h = 0.5 * u_vp, T = ca.xy / ca.w * h, F = cb.xy / cb.w * h;\n"
    "      T.y = -T.y; F.y = -F.y;\n"
    "      float dx = 0.5 * (F.y - T.y), dy = 0.5 * (F.x - T.x);\n"
    "      vec2 c = k == 0 ? vec2(T.x - dx, T.y - dy) : k == 1 ? vec2(F.x - dx, F.y + dy) : k == 2 ? vec2(T.x + dx, F.y - dy) : vec2(F.x + dx, T.y + dy);\n"
    "      gl_Position = vec4(c.x / h.x, -c.y / h.y, k == 0 || k == 3 ? ca.z / ca.w : cb.z / cb.w, 1.0);\n"
    "      p = k == 0 || k == 3 ? a_normal : a_pos;\n"
    "    } else {\n"
    "      vec2 h = 0.5 * u_vp, A = ca.xy / ca.w * h, B = cb.xy / cb.w * h;\n"
    "      A.y = -A.y; B.y = -B.y;\n"
    "      vec2 d = 0.5 * (B - A), c = k == 0 ? A + vec2(-d.y, d.x) : k == 1 ? A + vec2(d.y, -d.x) : k == 2 ? B + vec2(d.y, -d.x) : B + vec2(-d.y, d.x);\n"
    "      gl_Position = vec4(c.x / h.x, -c.y / h.y, k < 2 ? ca.z / ca.w : cb.z / cb.w, 1.0);\n"
    "      p = k < 2 ? a_normal : a_pos;\n"
    "    }\n"
    "  }\n"
    /* the view distance (the planet's VIEW far; DOS MW2.EXE 0x3f500, 3Dfx 0x1002fba0): an object whose bounding sphere
     * lies wholly beyond it - centre distance > far + r, or depth - r > far - is not drawn (all its vertices off screen) */
    "  if (u_far > 0.0 && a_sphere.w > 0.0) {\n"
    "    vec3 sc = (u_view * vec4(a_sphere.xyz, 1.0)).xyz;\n"
    "    if (dot(sc, sc) > (u_far + a_sphere.w) * (u_far + a_sphere.w) || -sc.z - a_sphere.w > u_far) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);\n"
    "  }\n"
    "  v_viewpos = (u_view * vec4(p, 1.0)).xyz; v_world = p;\n"
    "  v_normal = nrm; v_uv = a_uv; v_color = a_color; v_texsel = int(floor(a_texsel + 0.5)); v_sph = a_sphere;\n"
    "  v_tic = a_tic.rgb; v_ticm = int(a_tic.a + 0.5); v_gs = a_uv;\n"
    "  int hi = e / 32; e -= hi * 32; int corner = e - 3 * (e / 3);\n"
    "  v_bary = vec3(corner == 0 ? 1.0 : 0.0, corner == 1 ? 1.0 : 0.0, corner == 2 ? 1.0 : 0.0); v_emask = (e / 3) | (hi << 3);\n"
    "}\n";

static const char *FS =
    "#version 330 core\n"
    "in vec3 v_normal; in vec2 v_uv; in vec3 v_color; flat in int v_texsel; in vec3 v_viewpos; in vec3 v_world; flat in int v_dnib;\n"
    "in vec3 v_bary; flat in int v_emask, v_bb, v_tif; flat in vec4 v_sph;\n"
    "in vec3 v_tic; flat in int v_ticm;\n"
    "uniform int u_point; uniform vec3 u_lpos;\n"
    /* the Titanium lights (mercsw 0x1002ecaa): u_tilit 1 objects, 2 the ground (one colour, 0x1005527e); the ambient, two
     * world lights (type 1 directional - the unit vector the light travels along -, 2 point - its position), the
     * explosion lights (u_tidp: position, radius; 0x1002f238) */
    "uniform int u_tilit; uniform vec3 u_tiamb, u_tignd; uniform ivec2 u_tilt; uniform vec3 u_tilv[2], u_tilc[2];\n"
    "uniform int u_tidn; uniform vec4 u_tidp[8]; uniform vec3 u_tidc[8];\n"
    /* the engine's vector length (0x100462fe): the largest component + a quarter of the other two */
    "float ti_len(vec3 v) { vec3 a = abs(v); float m = max(a.x, max(a.y, a.z)); return m + (a.x + a.y + a.z - m) * 0.25; }\n"
    "vec3 ti_light(vec3 nn) {\n"
    "  vec3 c = u_tiamb;\n"
    "  for (int i = 0; i < 2; i++) {\n"
    "    if (u_tilt[i] == 1) { float d = -dot(u_tilv[i], nn); if (d > 0.0) c += u_tilc[i] * d; }\n"
    "    else if (u_tilt[i] == 2) { vec3 l = u_tilv[i] - v_world; float d = dot(l, nn); if (d > 0.0) c += u_tilc[i] * d / max(ti_len(l), 1e-3); }\n"
    "    else c += u_tilc[i];\n"
    "  }\n"
    "  c = min(c, vec3(1.0));\n"
    /* an explosion / projectile light lights an object's polygon if one of its vertices is inside the light's +-radius
     * box (0x1002eb36): here the object's bounding sphere's box against it (INFERRED); N.L / |L|, no falloff. The engine's
     * sums (0x1002f3f9..0x1002f5d5) are never cleared after the static lights: each vertex gets the running total, which
     * starts at the polygon's LAST vertex's lit colour - so a light that reaches an object adds that colour again on top
     * of its own (here this point's colour, INFERRED: the vertices' lit colours alike), clamped */
    "  vec3 acc = c; bool hit = false;\n"
    "  for (int j = 0; j < 8; j++) {\n"
    "    if (j >= u_tidn) break;\n"
    "    vec3 dd = abs(v_sph.xyz - u_tidp[j].xyz);\n"
    "    if (v_sph.w > 0.0 && (dd.x > u_tidp[j].w + v_sph.w || dd.y > u_tidp[j].w + v_sph.w || dd.z > u_tidp[j].w + v_sph.w)) continue;\n"
    "    vec3 l = u_tidp[j].xyz - v_world; float d = dot(l, nn);\n"
    "    hit = true;\n"
    "    if (d > 0.0) acc += u_tidc[j] * d / max(ti_len(l), 1e-3);\n"
    "  }\n"
    "  return hit ? min(c + acc, vec3(1.0)) : c;\n"
    "}\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_texsize;\n"
    "uniform int u_textured, u_unlit;\n"
    "uniform vec4 u_wire, u_mono;\n"
    "uniform int u_shadow;\n"
    "uniform vec3 u_light, u_fog_color;\n"
    "uniform float u_ambient, u_fog_density;\n"
    "uniform mat4 u_view;\n"
    "uniform vec3 u_tint; uniform float u_pfade; uniform sampler2D u_ptab; uniform int u_notex, u_tinton, u_dsign;\n"
    "uniform int u_dos, u_dos_dir; uniform sampler2D u_dos_tab; uniform vec3 u_dos_lpos; uniform float u_dos_amb, u_dos_fogdist;\n"
    /* the DOS Gouraud fill (glr_dos_gouraud below): the layer's vertices (13 floats each) as a buffer texture, the MVP, the
     * viewport (origin, size); u_dos_smooth: the port's dos_shading=smooth */
    "uniform samplerBuffer u_dos_vtx; uniform mat4 u_mvp; uniform vec2 u_vp, u_vpo; uniform int u_dos_smooth;\n"
    "noperspective in vec2 v_gs;\n"
    "uniform float u_alpha; uniform int u_mga_lod;\n"
    /* the Titanium edition (mercsw.dll): its vertex fog - x 1 = on, y the full-fog depth Z (0: none), w the camera's
     * factor; u_blend: alpha-blended (no alpha test), the texel's alpha out */
    "uniform vec4 u_tifog; uniform int u_blend;\n"
    /* the DOS horizon fill (MW2.EXE 0x3c000): a pixel is sky when its view ray's world y >= 0 - ray = R (sx - cx) / fx +
     * U (cy - sy) / fy + F; above the line a haze band of u_hz_band pixels fades from 0xEE (on the line) to 0xE0;
     * u_hz_x: the window's left and right x; u_hz_smooth: the port's dos_sky=smooth (the band's colours blended) */
    "uniform int u_horizon, u_hz_smooth; uniform vec3 u_hz_r, u_hz_u, u_hz_f; uniform vec4 u_hz_c; uniform float u_hz_band; uniform vec2 u_hz_x;\n"
    "out vec4 o_color;\n"
    /* the DOS edition (MW2.EXE 0x38ae3 / 0x3e086 / 0x38ee0): v_color = (kind, base index, intensity C); kind 0 unlit
     * (base is the index), 1 flat lit (base = ramp x 16), 2 textured (the texel's index through LUMA). The shade level =
     * ((C >> 1) x ((f x (128 - amb) >> 7) + amb)) / 0x440, f = 127 x cos(light, normal), less one per fog distance of
     * depth, clamped 0-15 */
    "vec3 dos_colour(float idx) { return texture(u_dos_tab, vec2((idx + 0.5) / 256.0, 16.5 / 17.0)).rgb; }\n"
    /* DOS Gouraud (type 4: 0x3bb80 -> 0x3bd08 / 0x3bd69, the map mode's 0x12690 -> 0x1272d; both only with Gouraud on,
     * DAT_00097024, else the flat fill 0x59180). The vertices: projected to whole pixels about the window's centre,
     * ((p << s) / z + 2) >> 2 (0x3dfb7: quarter pixels, rounded), the shade c (16.16) = ramp x 16 + i (L + 1) / 16
     * (0x3bd22; a < 48 as it is). MW2.EXE 0x59e77 then fills row by row from the top vertex (the last of the least y,
     * 0x59f58) down two chains, one backward and one forward through the vertex list (0x59f9f / 0x5a03a, the first
     * edges of no height skipped; later ones count 1 row, 0x5a61d), each edge starting at its upper vertex's x << 16 +
     * 0x8000 and c + 0x8000 (0x5a01f / 0x5a0b7, 0x5a662) and stepping dx / dy, dc / dy per row (16.16, truncated; the
     * last row steps on without switching, 0x5a33f). A row's span runs from the smaller chain's x >> 16 to the
     * larger's (0x5a1d5), stepped per PIXEL PAIR: s = (cR - cL) / max(w >> 1, 1), w its last pixel - its first
     * (0x5a1ea..0x5a202). Two accumulators run the pairs: the even pixel (from the span's own left end) at cL + DA,
     * the odd at cL + DB, + s per pair (0x5a214..0x5a2fd); the colour is the byte of the integer part (the palette
     * index). DA = 0x7fff (the callers' push, 0x3bd6b / 0x12723), DB = 0, swapped after every row (0x5a310..0x5a320;
     * at the top clip by the rows skipped, 0x5a10b), the top row starting with DA; a left-clipped span keeps its
     * phase (0x5a5c4). So pixel k of row r (from the unclipped span's left end / the polygon's top row): idx =
     * (cL + floor(k / 2) s + (k + r even ? 0x7fff : 0)) >> 16 - a 1-pixel checkerboard of n / n + 1 where the shade's
     * fraction is under 0.5, flat n + 1 above, its phase per polygon (DOSBox YELLSCN1 1024 x 768: the cockpit's
     * checkerboards change phase at polygon edges). The port does it in integers per output pixel at any size (DOS
     * draws it at its own resolution - 1024 x 768 one checker cell per pixel): the fragment reads its polygon
     * (v_texsel - 1: its fan's first vertex in the layer's buffer; the triangles end at the one whose edge code has
     * the closing bit), near-clips it (GL's plane, not DOS's 0x3de4e - near-clipped polygons approximate), rounds the
     * vertices as 0x3dfb7 about the viewport's centre and walks the chains to its row; the pixels GL covers outside
     * the DOS span take its end. dos_shading=smooth (not the original): the colours blended at c + 0.25 - the
     * dither's mean - within the polygon's own shades */
    "vec4 dos_gclip(int i) { int b = i * 13; return u_mvp * vec4(texelFetch(u_dos_vtx, b).r, texelFetch(u_dos_vtx, b + 1).r, texelFetch(u_dos_vtx, b + 2).r, 1.0); }\n"
    "float dos_gshade(int i, float g) { int b = i * 13; return texelFetch(u_dos_vtx, b + 7).r + texelFetch(u_dos_vtx, b + 6).r * g; }\n"
    "int dos_idiv(int n, int d) { int q = abs(n) / abs(d); return (n < 0) != (d < 0) ? -q : q; }\n"   /* idiv: truncated */
    "int dos_q(float p) { return (int(trunc(p * 4.0)) + 2) >> 2; }\n"   /* 0x3dfb7: quarter pixels, + 2 >> 2 */
    "ivec2 dos_gvtx(vec4 q) { vec2 p = q.xy / q.w * 0.5 * u_vp; ivec2 c = ivec2(u_vp) / 2; return ivec2(c.x + dos_q(p.x), c.y - dos_q(p.y)); }\n"
    /* one chain (dir -1: backward, the first, 0x5a5eb; + 1 forward, 0x5a689) at DOS row y: (x, c) in 16.16 */
    "ivec2 dos_gchain(ivec2 V[20], int C[20], int m, int top, int dir, int y, int ybot) {\n"
    "  int a = top, b = (a + dir + m) % m, r0 = V[top].y;\n"
    "  for (int g = 0; g < 20 && V[b].y == V[a].y && b != top; g++) { a = b; b = (a + dir + m) % m; }\n"
    "  int cnt = V[b].y - V[a].y, yq = y == ybot && y > r0 ? y - 1 : y;\n"
    "  for (int g = 0; g < 20 && yq >= r0 + cnt && b != top; g++) { r0 += cnt; a = b; b = (a + dir + m) % m; cnt = V[b].y - V[a].y; if (cnt == 0) cnt = 1; }\n"
    "  if (cnt == 0) cnt = 1;\n"
    "  uint t = uint(y - r0);\n"
    "  return ivec2(int(uint(V[a].x * 65536 + 0x8000) + t * uint(dos_idiv((V[b].x - V[a].x) * 65536, cnt))),\n"
    "               int(uint(C[a] + 0x8000) + t * uint(dos_idiv(C[b] - C[a], cnt))));\n"
    "}\n"
    "vec3 dos_gouraud(float lev) {\n"
    "  float g = (lev + 1.0) / 16.0, px = floor(gl_FragCoord.x), py = floor(gl_FragCoord.y);\n"
    "  float cc = v_gs.y + v_gs.x * g + 0.5, cmin = -1e9, cmax = 1e9;\n"   /* (no polygon: interpolated, screen parity) */
    "  int k = int(px), r = int(py);\n"
    "  uint val = 0u; bool exact = false;\n"
    "  if (v_texsel > 0) {\n"
    "    vec4 P[18]; float C[18]; ivec2 V[20]; int CF[20]; int n = 0, m = 0, s = v_texsel - 1, L1 = int(lev) + 1;\n"
    "    for (int t = 0; t < 16; t++) {\n"
    "      int b = s + 3 * t;\n"
    "      if (t == 0) { P[0] = dos_gclip(b); C[0] = dos_gshade(b, g); n = 1; }\n"
    "      P[n] = dos_gclip(b + 1); C[n] = dos_gshade(b + 1, g); n++;\n"
    "      if (((int(texelFetch(u_dos_vtx, b * 13 + 12).r + 0.5) % 32 / 3) & 2) != 0) { P[n] = dos_gclip(b + 2); C[n] = dos_gshade(b + 2, g); n++; break; }\n"
    "    }\n"
    "    cmin = 1e9; cmax = -1e9;\n"
    "    for (int i = 0; i < n; i++) {\n"   /* the near plane (z + w >= 0), the shade carried */
    "      vec4 a = P[i], b = P[(i + 1) % n]; float da = a.z + a.w, db = b.z + b.w;\n"
    "      cmin = min(cmin, C[i]); cmax = max(cmax, C[i]);\n"
    "      if (da >= 0.0) { V[m] = dos_gvtx(a); CF[m] = int(floor(C[i] * 65536.0 + 0.5)); m++; }\n"
    "      if ((da >= 0.0) != (db >= 0.0)) { float t = da / (da - db); V[m] = dos_gvtx(mix(a, b, t)); CF[m] = int(floor(mix(C[i], C[(i + 1) % n], t) * 65536.0 + 0.5)); m++; }\n"
    "    }\n"
    "    if (m >= 3) {\n"
    "      int top = 0, ybot = V[0].y;\n"
    "      for (int i = 0; i < m; i++) { if (V[i].y <= V[top].y) top = i; ybot = max(ybot, V[i].y); }\n"   /* 0x59f58: the last of the least y */
    "      int y = clamp(int(u_vp.y) - 1 - (int(py) - int(u_vpo.y)), V[top].y, ybot);\n"   /* the DOS row (y down) */
    "      ivec2 e1 = dos_gchain(V, CF, m, top, -1, y, ybot), e2 = dos_gchain(V, CF, m, top, 1, y, ybot);\n"
    "      if (e2.x <= e1.x) { ivec2 t2 = e1; e1 = e2; e2 = t2; }\n"   /* 0x5a1d5 */
    "      int x0 = e1.x >> 16, w = (e2.x >> 16) - x0;\n"
    "      int st = w != 0 ? dos_idiv(e2.y - e1.y, max(w >> 1, 1)) : 0;\n"
    "      k = clamp(int(px) - int(u_vpo.x) - x0, 0, w); r = y - V[top].y;\n"
    "      val = uint(e1.y) + uint(k >> 1) * uint(st); exact = true;\n"
    "      cc = float(int(val)) / 65536.0;\n"
    "    }\n"
    "  }\n"
    "  if (u_dos_smooth != 0) {\n"
    "    float e = clamp(cc - 0.25, cmin, cmax), e0 = floor(e);\n"
    "    return mix(dos_colour(e0), dos_colour(e0 + 1.0), e - e0);\n"
    "  }\n"
    "  if (exact) return dos_colour(float(((val + (((k + r) & 1) == 0 ? 0x7fffu : 0u)) >> 16) & 0xffu));\n"
    "  return dos_colour(floor(cc + (((k + r) & 1) == 0 ? 32767.0 / 65536.0 : 0.0)));\n"
    "}\n"
    "void main() {\n"
    /* the wireframe (image enhancement): only the polygons' own edges, 1 px - corner k's barycentric is the distance
     * to the edge opposite it (bit 0: corner 0's, bit 1: corner 1's, bit 2: corner 2's) */
    "  if (u_wire.a > 0.0) {\n"
    "    vec3 w = fwidth(v_bary); float d = 1.0e9;\n"
    "    if ((v_emask & 1) != 0) d = min(d, v_bary.x / max(w.x, 1e-6));\n"
    "    if ((v_emask & 2) != 0) d = min(d, v_bary.y / max(w.y, 1e-6));\n"
    "    if ((v_emask & 4) != 0) d = min(d, v_bary.z / max(w.z, 1e-6));\n"
    /* hidden lines removed: a polygon's inside is drawn black (it hides what is behind it, as DOS); edges in the
     * layer's colour, an active building's (flag 8) in the mechs' blue */
    "    if (d > 1.0) { o_color = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
    "    int wc = (v_emask >> 3) & 7;\n"   /* 1 colour 7 blue, 2 0xb red, 3 3 yellow, 4 8 dark red (60,0,0) */
    "    o_color = wc == 1 ? vec4(0.0, 48.0 / 255.0, 212.0 / 255.0, 1.0) : wc == 2 ? vec4(252.0 / 255.0, 24.0 / 255.0, 24.0 / 255.0, 1.0)\n"
    "            : wc == 3 ? vec4(252.0 / 255.0, 204.0 / 255.0, 0.0, 1.0) : wc == 4 ? vec4(60.0 / 255.0, 0.0, 0.0, 1.0) : vec4(u_wire.rgb, 1.0); return; }\n"
    "  if (u_shadow != 0) { o_color = vec4(0.0, 0.0, 0.0, 0.45); return; }\n"
    "  if (u_horizon != 0) {\n"
    "    float sx = gl_FragCoord.x, sy = gl_FragCoord.y;\n"   /* GL y up: sy grows upward */
    "    vec3 ray = u_hz_r * (sx - u_hz_c.x) / u_hz_c.z + u_hz_u * (sy - u_hz_c.y) / u_hz_c.w + u_hz_f;\n"
    "    if (ray.y < 0.0) { o_color = vec4(dos_colour(239.0), 1.0); return; }\n"   /* 0xEF ground */
    "    float idx = 224.0;\n"                                                      /* 0xE0 sky */
    /* the band (0x3c000 -> 0x59e77, only with Gouraud on, DAT_00097024): a quad from the line (corners 0xEE << 16,
     * DAT_00096eac - 1) up u_hz_band rows (corners 0xE0 << 16, DAT_00096ea8), its colour c = 238 - 14 d / band (d: the
     * rows above the line) stepped down the edges from c + 0x8000 (0x5a0xx), each row's pixels in pairs as c + 0x8000 and
     * c (in_stack 8 = 0x8000 from 0x3cc8b), the pair swapped every row (0x5a310): idx = floor(c + 0.5 + 0.5 chk) - a
     * half-step checkerboard, counted from the span's left end and the quad's top row (DOSBox YELLSCN1 1024 x 768:
     * every step one flat band and one 1-pixel checkerboard band of it and the next, row for row as here; the phase -
     * which pixel of a pair is the brighter - set to that capture, the GL row count being one off the quad's own). dos_sky=smooth: the same
     * colours blended at e = c + 0.25 (the staircase's mean: each flat band's middle = its own colour) */
    "    if (u_hz_band > 0.0 && abs(u_hz_u.y) > 1e-6) {\n"
    "      float sh = u_hz_c.y - u_hz_c.w * ((sx - u_hz_c.x) * u_hz_r.y / u_hz_c.z + u_hz_f.y) / u_hz_u.y;\n"   /* the line's row here */
    "      float d = sy - sh;\n"
    "      if (d >= 0.0 && d < u_hz_band) {\n"
    "        float c = 238.0 - 14.0 * d / u_hz_band;\n"
    "        if (u_hz_smooth != 0) {\n"
    "          float e = clamp(c + 0.25, 224.0, 238.0), e0 = min(floor(e), 237.0);\n"
    "          o_color = vec4(mix(dos_colour(e0), dos_colour(e0 + 1.0), e - e0), 1.0); return;\n"
    "        }\n"
    "        float shl = u_hz_c.y - u_hz_c.w * ((u_hz_x.x - u_hz_c.x) * u_hz_r.y / u_hz_c.z + u_hz_f.y) / u_hz_u.y;\n"
    "        float shr = u_hz_c.y - u_hz_c.w * ((u_hz_x.y - u_hz_c.x) * u_hz_r.y / u_hz_c.z + u_hz_f.y) / u_hz_u.y;\n"
    "        int row = int(floor(max(shl, shr) + u_hz_band - sy)), col = int(floor(sx - u_hz_x.x));\n"
    "        idx = min(floor(c + (((row + col) & 1) != 0 ? 1.0 : 0.5)), 238.0);\n"
    "      }\n"
    "    }\n"
    "    o_color = vec4(dos_colour(idx), 1.0); return;\n"
    "  }\n"
    "  if (u_dos != 0) {\n"
    "    int kind = int(v_color.r + 0.5); float base = floor(v_color.g + 0.5), C = floor(v_color.b + 0.5);\n"
    "    if (kind == 0) { o_color = vec4(dos_colour(base), 1.0); return; }\n"
    /* kind 4: a type 3 billboard - its texel indices as they are (0x38ae3 returns a 0x3000 word unshaded) */
    "    if (kind == 4) { vec4 tb = texture(u_tex, v_uv / u_texsize); if (tb.a < 0.5) discard; o_color = vec4(dos_colour(floor(tb.r * 255.0 + 0.5)), 1.0); return; }\n"
    "    float nl = length(v_normal); vec3 Ld = u_dos_dir != 0 ? normalize(u_dos_lpos) : normalize(u_dos_lpos - v_world);\n"
    /* one-sided (0x3e1de): a polygon is drawn only when its normal faces the eye - (v0 - eye) . n < 0 - and lit by the
     * same normal, signed (0x3e086: 127 cos; behind the light below the ambient). The DOS normal (0x380a0 -> 0x3a4b9)
     * is the winding's: here -cross(v1 - v0, v2 - v0) of the file's vertices (the 3D editions' stored normals, which
     * the engine negates at load, 0x10042140, agree) */
    "    vec3 nf = nl > 0.0 ? -v_normal / nl : vec3(0.0);\n"
    "    if (nl > 0.0 && dot(v_viewpos, mat3(u_view) * nf) >= 0.0) discard;\n"
    "    float f = nl > 0.0 ? trunc(127.0 * dot(nf, Ld)) : 127.0;\n"
    "    float lev = floor(floor(C / 2.0) * (floor(f * (128.0 - u_dos_amb) / 128.0) + u_dos_amb) / 1088.0);\n"
    "    if (u_dos_fogdist > 0.0) lev -= floor(max(-v_viewpos.z, 0.0) / u_dos_fogdist);\n"
    "    lev = clamp(lev, 0.0, 15.0);\n"
    "    if (kind == 1) { o_color = vec4(dos_colour(base + lev), 1.0); return; }\n"
    /* Gouraud (0x3bb80): each vertex's baked index a = ramp | i, rescaled by the polygon's level - ramp + i (L + 1) / 16 -
     * when a >= 48 (16.16 a >= 0x300000), else taken as it is: the fixed part in the vertex's v (ramp, or a), the scaled
     * part in its u (i, or 0); filled and dithered as 0x59e77 does (dos_gouraud). Flat-lit (types 1 / 2, 0x59180) and
     * textured (5-7: 0x5b549 affine, 0x3fb20 perspective) polygons have no dither in DOS */
    "    if (kind == 3) { o_color = vec4(dos_gouraud(lev), 1.0); return; }\n"
    "    vec4 tx = texture(u_tex, v_uv / u_texsize); if (tx.a < 0.5) discard;\n"
    "    float ti = floor(tx.r * 255.0 + 0.5);\n"
    "    float si = lev >= 15.0 ? ti : floor(texture(u_dos_tab, vec2((ti + 0.5) / 256.0, (lev + 0.5) / 17.0)).r * 255.0 + 0.5);\n"
    "    o_color = vec4(dos_colour(si), 1.0); return;\n"
    "  }\n"
    /* a Titanium colour kind 2 polygon (mercsw 0x1001b466 / 0x10053280): drawn only as the kind 1 shade its object's
     * texture switch makes it (its class in -v_texsel switched off, 0x1001b47a..0x1001b4f3) - or as an outline (above) */
    "  if (v_ticm == 3 && ((-v_texsel) & u_notex) == 0) discard;\n"
    "  vec3 base = u_mono.a > 1.5 ? v_color : u_mono.a > 0.0 ? u_mono.rgb : v_ticm >= 2 ? v_tic : v_color; float ta = 1.0; bool tinted = false;\n"
    /* the Combat Variables texture switches (0x1002a8a0 render flags DAT_1024e228, 0x1002a748..): a textured polygon whose
     * object class (-v_texsel: 1 OBJECT, 2 TERRAIN) is switched off is drawn in its texture's average colour (v_color) */
    "  if (u_textured != 0 && u_mono.a <= 0.0 && (v_texsel >= 0 || ((-v_texsel) & u_notex) == 0)) {\n"
    "    vec4 t;\n"
    "    if (u_mga_lod != 0) { float z = -v_viewpos.z; t = textureLod(u_tex, v_uv / u_texsize, z < 2500.0 ? 0.0 : z < 5000.0 ? 1.0 : z < 15000.0 ? 2.0 : 3.0); }\n"
    "    else t = texture(u_tex, v_uv / u_texsize);\n"
    "    if (u_blend != 0) { if (t.a < 0.004) discard; } else if (t.a < 0.5) discard; base = t.rgb * u_tint; ta = t.a; tinted = u_tinton != 0; }\n"
    /* an untextured polygon's palette colour toward the fade's target palette (its index + 1 in v_texsel) */
    "  else if (u_pfade > 0.0 && v_texsel > 0 && v_bb == 0) base = mix(base, texture(u_ptab, vec2((float(v_texsel - 1) + 0.5) / 256.0, 0.5)).rgb, u_pfade);\n"
    "  float n = length(v_normal);\n"
    "  vec3 L = u_point != 0 ? normalize(u_lpos - v_world) : u_light;\n"
    "  float diff = n > 0.0 ? abs(dot(v_normal / n, L)) : 0.5;\n"   /* two-sided: winding unknown */
    /* a tinted textured polygon (the 3D editions' palette fades, 0x1002c880 with DAT_1024e494): the tint is its vertex
     * colour - unlit. A mech location (type word 0x100) by its damage nibble n (0x1002b9b0, DAT_1024e428 = PLNT +52):
     * the light x (1 - n / 15), or x (1 + n / 15) when the planet's flag is set, at most full */
    "  float ds = v_dnib > 0 ? (u_dsign != 0 ? 1.0 + float(v_dnib) / 15.0 : 1.0 - float(v_dnib) / 15.0) : 1.0;\n"
    /* Titanium: a polygon lit per vertex (v_ticm 1: the colour the lights gave its corners, interpolated - Gouraud, as
     * mercsw 0x1002ecaa / 0x1002f238 and dx5draw's SHADEMODE) */
    "  vec3 lit = u_unlit != 0 || v_bb != 0 || tinted || (v_tif & 2) != 0 || v_ticm == 3 ? base : u_tilit == 1 ? base * (v_ticm == 1 ? v_tic : ti_light((v_tif & 1) != 0 ? v_normal : n > 0.0 ? v_normal / n : vec3(0.0)))\n"
    "           : u_tilit == 2 ? base * u_tignd : base * min(1.0, (u_ambient + diff) * ds);\n"   /* SGL: ambient + parallel light, saturating */
    /* PowerVR PCX2 table fog (Imagination's Series 1 driver source, MIT: dlcamera.c sgl_set_fog, rncamera.c, simulat3
     * hwsabren.c Fog / texas.c): per pixel, from the depth 1/w: index i = depth >> shift = K / z (K below, z the view depth
     * in game units), the 8-bit fog f = PowerTable[i & 127] >> min(i >> 7, 9) with PowerTable[k] = 256 x 2^(-k/128)
     * (entry 0 = 255), so f = 0 from i = 1152 (near) up to 255 far; colour += (fog - colour) x f / 256 */
    "  float fz = max(-v_viewpos.z, 1.0);\n"
    "  float fi = u_fog_density > 0.0 ? floor(u_fog_density / fz) : 1.0e9;\n"
    "  float fk = mod(fi, 128.0), fs = floor(fi / 128.0);\n"
    "  float ft = fk == 0.0 ? 255.0 : floor(256.0 * exp2(-fk / 128.0));\n"
    "  float ff = fs >= 9.0 ? 0.0 : floor(ft / exp2(fs)) / 256.0;\n"
    /* Titanium (mercsw.dll): the vertex fog the D3D plugin takes as the specular alpha a (fog = 1 - a); the world, the
     * objects and the ground (0x10053474, 0x10001b53): a = (1 - min(1, z / Z)) x the camera's factor (density Y's
     * ground fog / low cloud, 0x1006a851); Z = 0: no distance fog */
    "  if (u_tifog.x > 0.5) ff = 1.0 - (u_tifog.y > 0.0 ? 1.0 - min(1.0, fz / u_tifog.y) : 1.0) * u_tifog.w;\n"
    "  o_color = vec4(lit + (u_fog_color - lit) * ff, u_blend != 0 ? ta * u_alpha : u_alpha);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "glr: shader compile failed:\n%s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

glr *glr_create(void)
{
    glr *r = calloc(1, sizeof *r);
    GLuint vs, fs;
    GLint ok = 0;
    if (!r) return NULL;
    vs = compile(GL_VERTEX_SHADER, VS);
    fs = compile(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs) { free(r); return NULL; }
    r->prog = glCreateProgram();
    glAttachShader(r->prog, vs);
    glAttachShader(r->prog, fs);
    glLinkProgram(r->prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(r->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(r->prog, sizeof log, NULL, log);
        fprintf(stderr, "glr: link failed:\n%s\n", log);
        glDeleteProgram(r->prog);
        free(r);
        return NULL;
    }
#define U(name) r->name = glGetUniformLocation(r->prog, #name)
    U(u_point); U(u_lpos);
    U(u_mvp); U(u_view); U(u_light); U(u_ambient); U(u_fog_density); U(u_fog_color);
    U(u_texsize); U(u_tex); U(u_textured); U(u_mono);
#undef U
    {
        int l;
        for (l = 0; l < GLR_LAYERS; l++) {
            glGenVertexArrays(1, &r->layer[l].vao);
            glGenBuffers(1, &r->layer[l].vbo);
            glGenTextures(1, &r->layer[l].tbo);
            glGenBuffers(1, &r->layer[l].sph_vbo);
            glGenBuffers(1, &r->layer[l].tic_vbo);
        }
    }
    glGenTextures(GLR_SLOTS * 4, r->tex);
    glGenVertexArrays(1, &r->env_vao);
    glGenBuffers(1, &r->env_vbo);
    glGenTextures(2, r->env_tex);
    glGenVertexArrays(1, &r->spr_vao);
    glGenBuffers(1, &r->spr_vbo);
    glGenTextures(64, r->spr_tex);
    r->u_far = glGetUniformLocation(r->prog, "u_far");
    r->u_vp = glGetUniformLocation(r->prog, "u_vp");
    r->u_tint = glGetUniformLocation(r->prog, "u_tint");
    r->u_tinton = glGetUniformLocation(r->prog, "u_tinton");
    r->u_dsign = glGetUniformLocation(r->prog, "u_dsign");
    r->u_pfade = glGetUniformLocation(r->prog, "u_pfade");
    r->u_ptab = glGetUniformLocation(r->prog, "u_ptab");
    r->u_notex = glGetUniformLocation(r->prog, "u_notex");
    glVertexAttrib4f(6, 0, 0, 0, 0);   /* the sky, ground and sprites: no sphere, never culled */
    glVertexAttrib4f(7, 0, 0, 0, 0);   /* nor a Titanium CPU colour */
    r->u_unlit = glGetUniformLocation(r->prog, "u_unlit");
    r->u_wire = glGetUniformLocation(r->prog, "u_wire");
    r->u_shadow = glGetUniformLocation(r->prog, "u_shadow");
    r->u_dos = glGetUniformLocation(r->prog, "u_dos");
    r->u_dos_tab = glGetUniformLocation(r->prog, "u_dos_tab");
    r->u_dos_lpos = glGetUniformLocation(r->prog, "u_dos_lpos");
    r->u_dos_amb = glGetUniformLocation(r->prog, "u_dos_amb");
    r->u_dos_fogdist = glGetUniformLocation(r->prog, "u_dos_fogdist");
    r->u_dos_dir = glGetUniformLocation(r->prog, "u_dos_dir");
    r->u_dos_vtx = glGetUniformLocation(r->prog, "u_dos_vtx");
    r->u_dos_smooth = glGetUniformLocation(r->prog, "u_dos_smooth");
    r->u_vpo = glGetUniformLocation(r->prog, "u_vpo");
    glUseProgram(r->prog);   /* the buffer sampler on its own unit always: a samplerBuffer left on unit 0 with u_tex's
                              * sampler2D makes every draw invalid */
    glUniform1i(r->u_dos_vtx, 3);
    glUseProgram(0);
    r->u_alpha = glGetUniformLocation(r->prog, "u_alpha");
    r->u_mga_lod = glGetUniformLocation(r->prog, "u_mga_lod");
    r->u_horizon = glGetUniformLocation(r->prog, "u_horizon");
    r->u_hz_r = glGetUniformLocation(r->prog, "u_hz_r");
    r->u_hz_u = glGetUniformLocation(r->prog, "u_hz_u");
    r->u_hz_f = glGetUniformLocation(r->prog, "u_hz_f");
    r->u_hz_c = glGetUniformLocation(r->prog, "u_hz_c");
    r->u_hz_band = glGetUniformLocation(r->prog, "u_hz_band");
    r->u_hz_x = glGetUniformLocation(r->prog, "u_hz_x");
    r->u_hz_smooth = glGetUniformLocation(r->prog, "u_hz_smooth");
    r->u_tifog = glGetUniformLocation(r->prog, "u_tifog");
#define U(name) r->name = glGetUniformLocation(r->prog, #name)
    U(u_tilit); U(u_tiamb); U(u_tilt); U(u_tilv); U(u_tilc); U(u_tignd); U(u_tidn); U(u_tidp); U(u_tidc);
#undef U
    r->u_blend = glGetUniformLocation(r->prog, "u_blend");
    glGenVertexArrays(1, &r->dome_vao);
    glGenBuffers(1, &r->dome_vbo);
    glGenTextures(GLR_SLOTS, r->dome_tex);
    glGenTextures(1, &r->dos_tex);
    glGenTextures(1, &r->ptab_tex);
    return r;
}

void glr_destroy(glr *r)
{
    if (!r) return;
    glDeleteTextures(GLR_SLOTS * 4, r->tex);
    glDeleteTextures(2, r->env_tex);
    glDeleteTextures(1, &r->dos_tex);
    glDeleteTextures(1, &r->ptab_tex);
    glDeleteBuffers(1, &r->env_vbo);
    glDeleteVertexArrays(1, &r->env_vao);
    glDeleteBuffers(1, &r->spr_vbo);
    glDeleteVertexArrays(1, &r->spr_vao);
    glDeleteTextures(64, r->spr_tex);
    glDeleteTextures(GLR_SLOTS, r->dome_tex);
    glDeleteBuffers(1, &r->dome_vbo);
    glDeleteVertexArrays(1, &r->dome_vao);
    free(r->dome_v);
    free(r->dome_pfirst); free(r->dome_pcount); free(r->dome_pslot);
    free(r->par);
    {
        int l;
        for (l = 0; l < GLR_LAYERS; l++) {
            glDeleteBuffers(1, &r->layer[l].vbo);
            glDeleteTextures(1, &r->layer[l].tbo);
            glDeleteBuffers(1, &r->layer[l].sph_vbo);
            glDeleteBuffers(1, &r->layer[l].tic_vbo);
            free(r->layer[l].tic); free(r->layer[l].tipn); free(r->layer[l].tistat); free(r->layer[l].tip);
            glDeleteVertexArrays(1, &r->layer[l].vao);
        }
    }
    glDeleteProgram(r->prog);
    free(r);
}

static const texture *asrc[GLR_SLOTS];   /* set_layer's texture average cache (glr_flush_textures clears it) */
static float aval[GLR_SLOTS][3];
void glr_flush_textures(glr *r)
{
    memset(asrc, 0, sizeof asrc);
    if (r) { memset(r->tex_src, 0, sizeof r->tex_src); memset(r->tex_w, 0, sizeof r->tex_w); memset(r->spr_src, 0, sizeof r->spr_src); memset(r->slot_frame, 0, sizeof r->slot_frame); }
}

static int g_billboards;   /* TEST ONLY: colour type 3 billboards in the last static (world) layer */

static void upload_texture(glr *r, int slot, const texture *t);
void glr_slot_frame(glr *r, int slot, const texture *t)
{
    if (!r || slot < 0 || slot >= GLR_SLOTS || r->slot_frame[slot] == t) return;
    r->slot_frame[slot] = t;
    if (t && r->has_tex[slot]) upload_texture(r, slot, t);   /* a slot the layers draw with */
}

static void upload_texture(glr *r, int slot, const texture *t)
{
    r->has_tex[slot] = t != NULL;
    if (!t || (r->tex_src[slot] == t && r->tex_w[slot] == t->w)) return;
    r->tex_src[slot] = t;
    r->tex_w[slot] = t->w;
    r->tex_h[slot] = t->h;
    glBindTexture(GL_TEXTURE_2D, r->tex[slot]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->w, t->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t->rgba);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

/* An object's bounding sphere in GL coordinates (engine 0x10029060: the centre of its vertices' box, the radius the
 * farthest vertex from it), for the view-distance cull */
static void part_sphere(const mech3d_part *part, float out[4])
{
    const wtb_object *o = &part->model.objects[0];
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f}, r2 = 0, (*w)[3];
    float cy = cosf(part->yaw * 3.14159265f / 180.0f), sy = sinf(part->yaw * 3.14159265f / 180.0f);
    int k, c;
    out[0] = out[1] = out[2] = out[3] = 0;
    if (o->vert_count <= 0 || !(w = malloc((size_t)o->vert_count * sizeof *w))) return;
    for (k = 0; k < o->vert_count; k++) {
        float vx = (float)o->verts[k].x, vy = (float)o->verts[k].y, vz = (float)o->verts[k].z;
        if (part->has_rot) {
            const float *R = part->rot;
            w[k][0] = R[0] * vx + R[1] * vy + R[2] * vz + (float)part->pos[0];
            w[k][1] = R[3] * vx + R[4] * vy + R[5] * vz + (float)part->pos[1];
            w[k][2] = -(R[6] * vx + R[7] * vy + R[8] * vz + (float)part->pos[2]);
        } else {
            w[k][0] = cy * vx + sy * vz + (float)part->pos[0];
            w[k][1] = vy + (float)part->pos[1];
            w[k][2] = -(-sy * vx + cy * vz + (float)part->pos[2]);
        }
        for (c = 0; c < 3; c++) { if (w[k][c] < mn[c]) mn[c] = w[k][c]; if (w[k][c] > mx[c]) mx[c] = w[k][c]; }
    }
    for (c = 0; c < 3; c++) out[c] = (mn[c] + mx[c]) * 0.5f;
    for (k = 0; k < o->vert_count; k++) {
        float dx = w[k][0] - out[0], dy = w[k][1] - out[1], dz = w[k][2] - out[2], d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > r2) r2 = d2;
    }
    out[3] = sqrtf(r2) + 1.0f;   /* > 0: culled by distance */
    free(w);
}

/* A Titanium colour kind 1 polygon's palette shade (mercsw 0x1001b5e4 -> 0x1002f5e3, the 16-bit build's DOS-style
 * level): L = the LITE ambient (0x10248b0c) + max(0, I x globalintensity / 128) - the LITE's term when it has one
 * (0x10097cb4), or a point light at the eye when its vector is 888888 x 3 (0x100ad320 -> the camera position
 * 0x1009e954, 0x1002f641) -; less (depth / LITE range) >> 4 when the range is > 1e-7 (0x1002f6cb; depth = the polygon's
 * deepest corner, 0x100575d3..0x1005761f), at least 0; then with Multiple Lights (0x10097cb8) and L < 0x80 each
 * explosion / projectile light whose +-radius box holds one of its corners (0x1002eb36) adds its I as a point light,
 * unscaled, until L reaches 0x80 (0x1002f70a..0x1002f7ba); level = L x ((w & 0xff) >> 1) / 0x440, 0..15. Without the
 * LITE: 8. The polygon's normal and corner 0 are in the MODEL's frame (poly +8, vertex +0), the lights in the world's,
 * as the engine has them. */
/* I (0x10056e9b -> 0x10056f13): 128 (n . v) clamped to +-127 (ftol, the short clamp), v a directional light's vector as
 * stored (0x10057a80), else (light - corner 0) / its engine length (0x100462fe; within 1e-7 of it: 127) */
static int ti_shade_i(const float n[3], const float v0[3], const float lv[3], int dir)
{
    float d;
    int I;
    if (dir) d = 128.0f * (n[0] * lv[0] + n[1] * lv[1] + n[2] * lv[2]);
    else {
        float dx = lv[0] - v0[0], dy = lv[1] - v0[1], dz = lv[2] - v0[2];
        float ax = fabsf(dx), ay = fabsf(dy), az = fabsf(dz), mx = ax > ay ? (ax > az ? ax : az) : (ay > az ? ay : az);
        float len = mx + (ax + ay + az - mx) * 0.25f;
        if (ax < 1e-7f && ay < 1e-7f && az < 1e-7f) return 127;
        d = 128.0f * (n[0] * dx + n[1] * dy + n[2] * dz) / len;
    }
    d = d < -32768.0f ? -32768.0f : d > 32767.0f ? 32767.0f : d;
    I = (int)d;
    return I < -127 ? -127 : I > 127 ? 127 : I;
}
/* L before the fog and the Multiple Lights terms; eye: the camera in game coordinates (NULL: none known) */
static int ti_shade_base(const glr_ti_shade *sh, const float n[3], const float v0[3], const float *eye)
{
    int L = (int)sh->amb, I;
    if (sh->eye && eye) I = ti_shade_i(n, v0, eye, 0);   /* (0x1002f641: the vector replaced, 0x10248b00 = 0: a point) */
    else if (sh->light) I = ti_shade_i(n, v0, sh->lvec, sh->dir);
    else return L;
    I = I * sh->gi / 128;   /* (imul, cdq / and 0x7f / sar 7: toward zero) */
    return I > 0 ? L + I : L;
}
static int ti_shade_finish(int L, int c)
{
    int lev = L * c / 0x440;
    return lev < 1 ? 0 : lev > 15 ? 15 : lev;
}
static int ti_shade_level(const glr *r, const wtb_object *o, const wtb_poly *q, int c)
{
    const glr_ti_shade *sh = &r->ti_shade;
    const wtb_vertex *v0 = &o->verts[q->idx[0]];
    const float c0[3] = {(float)v0->x, (float)v0->y, (float)v0->z};
    if (!sh->on) return 8;
    return ti_shade_finish(q->n >= 3 ? ti_shade_base(sh, q->normal, c0, NULL) : (int)sh->amb, c);
}

static int set_layer(glr *r, int li, const mech3d *m, const uint8_t palette[256][3],
                     texture *const slots[GLR_SLOTS], int camo, int clan, int bounds)
{
    const int ti_world_normals = getenv("MW2_TEST_TI_WORLDN") != NULL;   /* TEST ONLY: turn them as the 3D editions do */
    glr_layer *L = &r->layer[li];
    size_t cap = 0, n = 0, *count, *pos;
    float *buf = NULL, *sph = NULL, *tic = NULL, *tipn = NULL, psph[4] = {0, 0, 0, 0}, mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    glr_tipoly *tip = NULL;
    int pi, k, j, a, pass, b, tip_n = 0, tip_cap = 0;

    for (pi = 0; pi < m->part_count; pi++) {
        const wtb_model *pm = r->ti_slots && m->parts[pi].ti_model ? m->parts[pi].ti_model : &m->parts[pi].model;
        const wtb_object *o;
        if (pm->object_count <= 0) continue;   /* (a Titanium object with no model) */
        o = &pm->objects[0];
        for (k = 0; k < o->poly_count; k++)
            if (o->polys[k].n >= 3) cap += (size_t)(o->polys[k].n - 2) * 3 + (((o->polys[k].color >> 12) & 7) == 3 ? 6 : 0);
    }
    buf = malloc((cap ? cap : 1) * FLOATS_PER_VERTEX * sizeof *buf);
    sph = malloc((cap ? cap : 1) * 4 * sizeof *sph);
    tic = calloc((cap ? cap : 1) * 4, sizeof *tic);
    tipn = calloc((cap ? cap : 1) * 6, sizeof *tipn);
    count = calloc(GLR_BATCHES, sizeof *count);
    pos = calloc(GLR_BATCHES, sizeof *pos);
    if (!buf || !sph || !tic || !tipn || !count || !pos) { free(buf); free(sph); free(tic); free(tipn); free(count); free(pos); return -1; }
    if (r->ti_slots) memcpy(r->ti_pal[li], palette, sizeof r->ti_pal[li]);

    /* two passes: count triangles per texture batch, then write them grouped */
    for (pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            size_t acc = 0;
            for (b = 0; b < GLR_BATCHES; b++) { pos[b] = acc; acc += count[b]; }
        }
        for (pi = 0; pi < m->part_count; pi++) {
            const mech3d_part *part = &m->parts[pi];
            /* TEXTURES TITANIUM: a world object's Titanium model in place of its own; a Titanium mech's parts (ti) */
            const wtb_model *model = r->ti_slots && part->ti_model ? part->ti_model : &part->model;
            const int ti = r->ti_slots && (part->ti || part->ti_model);
            const wtb_object *o = model->object_count > 0 ? &model->objects[0] : NULL;
            /* image enhancement, mode 0 (engine 0x1002a4b0, enhancement branch): by the object's type word (OBJ +44 ->
             * object +2) - 0x200 colour 7 (blue), 0x400 0xb (red), 0x100 by its class nibble (0 blue, 1-11 3 yellow, 12+
             * red), others the layer's (world 8, dark red); an active building blue (DOS capture) */
            int wire_code = 0;
            if (part->wire_dmg) wire_code = part->wire_dmg;   /* a mech location (class 0x100): its damage level, set by the caller */
            else if (part->objtype & 0x200) wire_code = 1;
            else if (part->objtype & 0x400) wire_code = 2;
            else if (part->objtype & 0x100) { int cn = (part->objtype & 0xf0) >> 4; wire_code = cn < 1 ? 1 : cn < 12 ? 3 : 2; }
            else if (part->wire_hi) wire_code = 1;
            if (part->hidden || part->moving || part->facing || !o) continue;   /* moving: a path-carried world part, facing: a fire /
                                                                                 * smoke card - both drawn on the actor layer */
            const int dnib = !r->dos && !ti && part->dmg_nib > 0 && part->dmg_nib < 16 ? part->dmg_nib : 0;
            /* the texture switches by the object's type word (0x1002a768: DAT_1024e228 & type, and with the OBJECT bit 0x100
             * also class 5): OBJECT TEXTURES (0x100652f0: bits 0x100 mechs, 0x200) -> class 1, TERRAIN TEXTURES (0x10065340:
             * bit 0x800) -> class 2; other objects (type 0 rocks, buildings) keep their textures */
            int notex_cls = (li == 1 && part->objtype == 0) || (part->objtype & 0x300) || (part->objtype & 0xf0) == 0x50 ? 1
                          : (part->objtype & 0x800) ? 2 : 0;
            if (pass == 1) part_sphere(part, psph);
            for (k = 0; k < o->poly_count; k++) {
                const wtb_poly *q = &o->polys[k];
                float face[3], e1[3], e2[3], poly_col[3], ti_vmin;
                size_t pfirst;
                int c0, pidx, ti_flat, tk4, tk2, tik = 0, k2c = 0, k2cls = 0;
                const wtb_vertex *v0, *v1, *v2;
                int slot = q->color & 0xFF, batch = GLR_SLOTS;
                float ground_lift = 0.0f;
                if (q->n < 3) continue;
                /* Titanium (mercsw 0x10053280, the draw by colour kind in the normal view): kind 2 has no case there - not drawn
                 * (lattices, rotor blades, shields, the hex glass) unless its object's texture switch is off (then the kind 1
                 * shade 0x1001b050 makes of it, v_ticm 3 below) or in image enhancement (0x10055a5d: outlines); kind 3 only
                 * with low nibble 0 or 3 (0x1004eef0) */
                tk2 = ti && ((q->color >> 12) & 7) == 2;
                if (ti && ((q->color >> 12) & 7) == 3 && (q->color & 0xf) != 0 && (q->color & 0xf) != 3) continue;
                /* the 3D editions' colour words 0x8000-0xb000 (engine 0x10026e80) are textured polygons of slot 0x100 + the
                 * low byte (0x1002c880; bank 1, the surface slots) - 0xb000 unlit (light 0x80000000) - not the type 3
                 * billboard: the scrub records' ground patches (TNW1SCR1 0xbc38 V1_PLAIN, RUSTSCR1 / SABLSCR1 0xbc39) */
                const int hi_card = model->extended && !ti && (q->color & 0x8000) != 0;
                if (((q->color >> 12) & 7) == 3 && !hi_card && !part->shadow) {
                    /* colour type 3 (0x3240 ...): a billboard of bank-0 bitmap (w >> 4) & 0xff (engine 0x10026fc7 ->
                     * 0x1002a8e0): of the triangle's corners (by texel u / v) the foot (u 0, v > 0) and the top (u 0, v 0)
                     * make the vertical edge; the engine draws a screen square of that edge's projected length centred on
                     * it (the shader, edge code 1024), the whole bitmap, blended by its alpha, unlit (the third corner,
                     * u > 0, only orients the bitmap). The scrub records (TNJ1SCRI: tan_rk, tan_rk2, rrock, cact_2) are
                     * rocks and cacti; each is stored twice, once per winding (two-sided), and drawn once. No bitmap in
                     * the table: nothing drawn (0x1002c880 returns 0 for an empty bitmap entry, 0x1015f0f6 / f0f8) */
                    /* Titanium (0x1004ef4c, mode 0): the same corners (u 0: v 0 the top, v > 0 the foot) and square on
                     * the edge's screen length, corner UVs (0,0) (0,1) (1,1) (1,0) (0x1009dfe0); its bitmap from the
                     * Titanium bank 0 (0x10001375 without + 0x100), the vertex colour 0xc8 grey (drawn below) */
                    int bs = (q->color >> 4) & 0xff, ft = -1, tp = -1, k2;
                    texture *const *bk = ti ? r->ti_bank0 : r->bank0;
                    const int bb0 = ti ? GLR_TB : GLR_B0;
                    const texture *bt = bk ? bk[bs] : NULL;
                    if (!bt || !bt->rgba || bt->w <= 0 || bt->h <= 0) continue;
                    if (k > 0 && o->polys[k - 1].color == q->color && o->polys[k - 1].n == q->n) {   /* the other winding */
                        int same = 1, k3, k4;
                        for (k3 = 0; k3 < q->n && same; k3++) {
                            int hit = 0;
                            for (k4 = 0; k4 < q->n; k4++) if (o->polys[k - 1].idx[k4] == q->idx[k3]) hit = 1;
                            same = hit;
                        }
                        if (same) continue;
                    }
                    for (k2 = 0; k2 < q->n; k2++) {
                        const wtb_vertex *vv = &o->verts[q->idx[k2]];
                        if (vv->a != 0) continue;
                        if (vv->b == 0) tp = q->idx[k2]; else ft = q->idx[k2];
                    }
                    if (ft < 0 || tp < 0) continue;
                    if (pass == 0) { count[bb0 + bs] += 6; continue; }
                    {
                        const wtb_vertex *vf = &o->verts[ft], *vt = &o->verts[tp];
                        float dx = (float)(vt->x - vf->x), dy = (float)(vt->y - vf->y), dz = (float)(vt->z - vf->z);
                        float h = sqrtf(dx * dx + dy * dy + dz * dz), foot[3], col[3] = {1, 1, 1};
                        static const int BBK[6] = {0, 1, 2, 0, 2, 3};   /* the engine's corners: two triangles */
                        static const int CORNER[6] = {0, 1, 2, 0, 1, 2}, MASK[6] = {5, 5, 5, 3, 3, 3};
                        if (part->has_rot) {
                            const float *R = part->rot;
                            float vx = (float)vf->x, vy = (float)vf->y, vz = (float)vf->z;
                            foot[0] = R[0] * vx + R[1] * vy + R[2] * vz + (float)part->pos[0];
                            foot[1] = R[3] * vx + R[4] * vy + R[5] * vz + (float)part->pos[1];
                            foot[2] = -(R[6] * vx + R[7] * vy + R[8] * vz + (float)part->pos[2]);
                        } else {
                            float cy = cosf(part->yaw * 3.14159265f / 180.0f), sy = sinf(part->yaw * 3.14159265f / 180.0f);
                            foot[0] = cy * (float)vf->x + sy * (float)vf->z + (float)part->pos[0];
                            foot[1] = (float)(vf->y + part->pos[1]);
                            foot[2] = -(-sy * (float)vf->x + cy * (float)vf->z + (float)part->pos[2]);
                        }
                        float top[3];
                        if (part->has_rot) {
                            const float *R = part->rot;
                            float vx = (float)vt->x, vy = (float)vt->y, vz = (float)vt->z;
                            top[0] = R[0] * vx + R[1] * vy + R[2] * vz + (float)part->pos[0];
                            top[1] = R[3] * vx + R[4] * vy + R[5] * vz + (float)part->pos[1];
                            top[2] = -(R[6] * vx + R[7] * vy + R[8] * vz + (float)part->pos[2]);
                        } else {
                            float cy = cosf(part->yaw * 3.14159265f / 180.0f), sy = sinf(part->yaw * 3.14159265f / 180.0f);
                            top[0] = cy * (float)vt->x + sy * (float)vt->z + (float)part->pos[0];
                            top[1] = (float)(vt->y + part->pos[1]);
                            top[2] = -(-sy * (float)vt->x + cy * (float)vt->z + (float)part->pos[2]);
                        }
                        if (r->dos && !model->extended) { col[0] = 4; col[1] = 0; col[2] = 255; }   /* DOS: the texels unshaded */
                        else glr_texture_average(bt, col);   /* the untextured view: the bitmap's colour */
                        for (a = 0; a < 6; a++) {
                            float *f = buf + pos[bb0 + bs] * FLOATS_PER_VERTEX;
                            int c, kc = BBK[a];
                            memcpy(sph + pos[bb0 + bs] * 4, psph, sizeof psph);
                            f[0] = foot[0]; f[1] = foot[1]; f[2] = foot[2];
                            f[3] = top[0]; f[4] = top[1]; f[5] = top[2];
                            f[6] = (kc == 1 || kc == 2) ? (float)bt->w : 0.0f; f[7] = kc >= 2 ? (float)bt->h : 0.0f;
                            if (ti) { f[6] = kc >= 2 ? (float)bt->w : 0.0f; f[7] = (kc == 1 || kc == 2) ? (float)bt->h : 0.0f; }   /* (0x1009dfe0: corners (0,0) (0,1) (1,1) (1,0); the flip for a corner u or v < 0, 0x1004f29f..0x1004f2ea, swaps the u
                                                                                                                                              * of corners 0 / 1 and 2 / 3 - equal in that table: no change) */
                            for (c = 0; c < 3; c++) f[8 + c] = part->tint ? (float)((part->tint >> (16 - 8 * c)) & 0xff) / 255.0f : col[c];
                            f[11] = (float)(kc + (ti ? 4 : 0));   /* (+ 4: the Titanium corners, VS) */
                            f[12] = (float)(1024 + CORNER[a] + 3 * MASK[a] + 32 * wire_code);
                            for (c = 0; c < 3; c++) {
                                float lo = f[c] < f[3 + c] ? f[c] : f[3 + c], hi = f[c] < f[3 + c] ? f[3 + c] : f[c];
                                if (lo - 0.5f * h < mn[c]) mn[c] = lo - 0.5f * h;
                                if (hi + 0.5f * h > mx[c]) mx[c] = hi + 0.5f * h;
                            }
                            pos[bb0 + bs]++;
                            n++;
                        }
                    }
                    continue;
                }
                if (slot == GLR_SLOT_CAMO) slot += camo + part->tex_offset;
                else if (slot == GLR_SLOT_INSIGNIA) slot += clan + part->tex_offset;   /* engine 0x10042000 */
                if (model->extended && !part->flat && slot < GLR_SLOTS && slots && slots[slot] && (q->color & 0xFF) != 0xF0) batch = slot;
                if (ti) {
                    /* the Titanium colour word (mercsw 0x1001b050: kinds 2, 5, 6, 7 textured - 0x1001b346 -, the bitmap the low
                     * byte, 0x10053859); a unit's camouflage and insignia slots remapped by its star at load (0x100748ab: an
                     * even slot 0-0xa with the star's CAMO set c (map CAMO chunks {star, set}, 0x10073a6b) becomes slot + c
                     * for c 0 / 1, else base[c] + slot / 2, base 0x100a11e0 = {0, 1, 0x56, 0x66, 0x76, 0x86, 0xf3, 0xf9}; slot
                     * 0x3f + the star) - the star taken as the port's STAR value (tex_offset, INFERRED) */
                    static const int TI_CAMO_BASE[8] = {0, 1, 0x56, 0x66, 0x76, 0x86, 0xf3, 0xf9};
                    int ty = (q->color >> 12) & 7, ls = q->color & 0xff, star = part->tex_offset < 0 ? 0 : part->tex_offset > 7 ? 7 : part->tex_offset;
                    if (part->ti && ls <= 0xa && !(ls & 1)) { int cs = r->ti_camo[star] & 7; ls = cs < 2 ? ls + cs : TI_CAMO_BASE[cs] + ls / 2; }
                    else if (part->ti && ls == 0x3f) ls += star;
                    else if (!part->ti && ls == 0x3f) ls += part->ti_insignia;   /* a world object: its structure's team */
                    batch = GLR_SLOTS;
                    if (ty >= 5 && !part->flat && ls < GLR_SLOTS && r->ti_slots[ls]) batch = GLR_TI + ls;
                }
                if (r->dos && !model->extended) {   /* DOS bitmaps: types 5-7 bitmap = low byte; type 3 (scrub / effect cards,
                                                     * 0x3250 ...) bitmap = (word >> 4) & 0xff, transparent texels */
                    int ty = (q->color >> 12) & 7, bs = ty == 3 ? (q->color >> 4) & 0xff : q->color & 0xff;
                    batch = GLR_SLOTS;
                    if ((ty >= 5 || ty == 3) && !part->flat && slots && slots[bs]) batch = bs;
                }
                if (part->shadow) batch = GLR_SLOTS + 1;   /* a shadow: drawn last, translucent */
                /* the 3D editions' type-0 polygons (colour word top nibble 0: the laser bolts 0x0070 / 0x00f0 / 0x00b0):
                 * engine 0x1002a4b0 -> 0x10026e80 -> 0x100272e0 - a flat palette colour (c >> 4) & 0xff at full
                 * intensity, unlit, untextured (LASER1 blue 7, LASER2 green 15, LASER3 red 11) */
                else if (model->extended && (q->color >> 12) == 0 && (q->color & 0xF) == 0) batch = GLR_SLOTS + 2;
                /* type 1 (0x1xxx): never textured - 0x1002a4b0 makes it 0x1000 | ((w >> 8) & 0xf) << 4 and 0x10026e80 draws
                 * palette entry ((w >> 8) & 0xf) << 4 | 0xf, lit (the T_1 pad / runway markings, 0x1c70 -> 0xcf) */
                else if (model->extended && (q->color >> 12) == 1) batch = GLR_SLOTS;
                /* colour slot 0xf0: a plain palette colour (colour >> 8), never textured - the cockpit frames' (repr 4) and
                 * many low-detail polygons use it with every kind of material, never with the textured hull material
                 * (0x98); the mission maps' own texture in slot 0xf0 (a clan crest) is not meant for them */
                if (pass == 0) { count[batch] += (size_t)(q->n - 2) * 3; continue; }
                v0 = &o->verts[q->idx[0]]; v1 = &o->verts[q->idx[1]]; v2 = &o->verts[q->idx[2]];
                e1[0] = (float)(v1->x - v0->x); e1[1] = (float)(v1->y - v0->y); e1[2] = (float)(v1->z - v0->z);
                e2[0] = (float)(v2->x - v0->x); e2[1] = (float)(v2->y - v0->y); e2[2] = (float)(v2->z - v0->z);
                face[0] = e1[1] * e2[2] - e1[2] * e2[1];
                face[1] = e1[2] * e2[0] - e1[0] * e2[2];
                face[2] = e1[0] * e2[1] - e1[1] * e2[0];
                int wface = 0;   /* face[] already in world (game) coordinates */
                if (r->dos && !model->extended && part->has_rot) {
                    /* DOS (one-sided, 0x3e1de): the posed polygon's own normal - a scaled object (the laser bolts, 3 x 3 x
                     * 12) would tilt a rotated model normal and cull faces seen along it */
                    const float *R = part->rot;
                    float t1[3], t2[3];
                    for (c0 = 0; c0 < 3; c0++) { t1[c0] = R[c0 * 3] * e1[0] + R[c0 * 3 + 1] * e1[1] + R[c0 * 3 + 2] * e1[2]; t2[c0] = R[c0 * 3] * e2[0] + R[c0 * 3 + 1] * e2[1] + R[c0 * 3 + 2] * e2[2]; }
                    face[0] = t1[1] * t2[2] - t1[2] * t2[1];
                    face[1] = t1[2] * t2[0] - t1[0] * t2[2];
                    face[2] = t1[0] * t2[1] - t1[1] * t2[0];
                    wface = 1;
                }
                if (ti) { face[0] = -face[0]; face[1] = -face[1]; face[2] = -face[2]; }   /* (outward: see mech3d_ti_normals) */
                {   /* textured: the vertex colour = the polygon's average texel colour, so the untextured view
                     * (Combat Variables TEXTURES off) shows each polygon in its texture's colours */
                    float pc[3];
                    /* a Titanium flat polygon (mercsw 0x10053280, palette colours 0x10286d40 straight into the vertices,
                     * no lights): kind 0 entry ((w >> 8) & 0xf) << 4 | ((w >> 4) & 0xf) (0x1001b3dc), kind 1 entry
                     * ramp x 16 + the palette shade (0x1001b7bd, ti_shade_level), kind 4 each vertex's own entry (its
                     * texel u, 0x100535d2 - set per vertex below; ramp x 16 + 8 stands for the polygon) */
                    const int tk = (q->color >> 12) & 7;
                    const texture *tt = batch >= GLR_TI && batch < GLR_TB ? r->ti_slots[batch - GLR_TI] : NULL;
                    /* kind 2 with its texture switch off (0x1001b47a..0x1001b4f3, the switch mask 0x1009e9a8 - INFERRED to be the
                     * 3D editions' OBJECT 0x300 / TERRAIN 0x800 bits): kind 1, ramp (w >> 8) & 0xf, intensity 0xa0 for a
                     * mech (type 0x100) or class 0x50 object, else 0xd0 */
                    /* The textured kinds 5-7 take the same branch (0x1001b466 handles types 0x2000 and 0x5000-0x7000 alike):
                     * switched off, kind 1 with that shade; else their bitmap w & 0xff (0x1001b4fc / 0x1001b50a). A mech
                     * part (the port's type 0 on the actor layer) is the engine's type 0x100 (INFERRED, as notex_cls) */
                    if (ti && (tk2 || tk >= 5)) {
                        const int ot = li == 1 && part->objtype == 0 ? 0x100 : part->objtype;
                        k2cls = (ot & 0x300) || (ot & 0xf0) == 0x50 ? 1 : (ot & 0x800) ? 2 : 0;
                        k2c = ((ot & 0x100) || (ot & 0xf0) == 0x50 ? 0xa0 : 0xd0) >> 1;
                    }
                    pidx = ti ? (tk == 1 ? (((q->color >> 8) & 0xf) << 4) | ti_shade_level(r, o, q, (int)(q->color & 0xff) >> 1)
                                : tk2 ? (((q->color >> 8) & 0xf) << 4) | ti_shade_level(r, o, q, k2c)
                                : tk == 4 ? (((q->color >> 8) & 0xf) << 4) | 8 : (q->color >> 4) & 0xff)
                         : batch == GLR_SLOTS + 2 ? (q->color >> 4) & 0xff : model->extended && (q->color >> 12) == 1 ? (((q->color >> 8) & 0xf) << 4) | 0xf : q->color >> 8;
                    for (c0 = 0; c0 < 3; c0++) pc[c0] = (float)palette[pidx][c0] / 255.0f;
                    if (batch < GLR_SLOTS && slots[batch] && slots[batch]->rgba && slots[batch]->w > 0 && slots[batch]->h > 0)
                    {   /* the textures off: the whole bitmap's average (0x1002d330), cached by texture */
                        if (asrc[batch] != slots[batch]) { glr_texture_average(slots[batch], aval[batch]); asrc[batch] = slots[batch]; }
                        memcpy(pc, aval[batch], sizeof pc);
                    } else if (tt && tt->rgba && tt->w > 0 && tt->h > 0) {
                        float u0 = 1e9f, u1 = -1e9f, w0 = 1e9f, w1 = -1e9f, acc[3] = {0, 0, 0};
                        int k2, gx, gy, nsamp = 0;
                        for (k2 = 0; k2 < q->n; k2++) {
                            const wtb_vertex *vv = &o->verts[q->idx[k2]];
                            if (vv->a < u0) u0 = (float)vv->a;
                            if (vv->a > u1) u1 = (float)vv->a;
                            if (vv->b < w0) w0 = (float)vv->b;
                            if (vv->b > w1) w1 = (float)vv->b;
                        }
                        for (gy = 0; gy < 6; gy++)
                            for (gx = 0; gx < 6; gx++) {
                                int tx = (int)floorf(u0 + (u1 - u0) * (gx + 0.5f) / 6.0f), ty = (int)floorf(w0 + (w1 - w0) * (gy + 0.5f) / 6.0f);
                                const unsigned char *px;
                                tx %= tt->w; if (tx < 0) tx += tt->w;
                                ty %= tt->h; if (ty < 0) ty += tt->h;
                                px = (const unsigned char *)tt->rgba + ((size_t)ty * (size_t)tt->w + (size_t)tx) * 4;   /* RGBA bytes, as uploaded */
                                if (px[3] < 128) continue;
                                acc[0] += px[0]; acc[1] += px[1]; acc[2] += px[2]; nsamp++;
                            }
                        if (nsamp) for (c0 = 0; c0 < 3; c0++) pc[c0] = acc[c0] / (255.0f * (float)nsamp);
                    }
                    for (c0 = 0; c0 < 3; c0++) poly_col[c0] = pc[c0];
                }
                if (r->dos && !model->extended) {   /* the DOS colour word (MW2.EXE 0x38ae3): (kind, base index, intensity) */
                    int ty = (q->color >> 12) & 7;
                    if ((ty >= 5 || ty == 3) && batch < GLR_SLOTS) { poly_col[0] = 2; poly_col[1] = 0; poly_col[2] = 255; }
                    else if (ty == 0 || ty == 2 || ty == 3) { poly_col[0] = 0; poly_col[1] = (float)((q->color >> 4) & 0xff); poly_col[2] = 0; }
                    else { poly_col[0] = ty == 4 ? 3.0f : 1.0f; poly_col[1] = (float)(((q->color >> 8) & 0xf) * 16); poly_col[2] = ty >= 5 ? 255.0f : (float)(q->color & 0xff); }
                }
                {   /* a polygon lying in the ground plane (a building's shadow or footing at y 0) flickered against the
                     * ground: raised 3 cm */
                    int k3, gnd = 1;
                    for (k3 = 0; k3 < q->n && gnd; k3++) { long wy = (long)o->verts[q->idx[k3]].y + part->pos[1]; if (wy > 2 || wy < -2) gnd = 0; }
                    ground_lift = gnd && !part->has_rot ? 3.0f : 0.0f;
                }
                ti_flat = ti && batch == GLR_SLOTS && (((q->color >> 12) & 7) <= 1 || ((q->color >> 12) & 7) == 4 || tk2);
                /* coloured per vertex on the CPU each frame (glr_draw -> ti_colours): the lit polygons, colour kinds 1 / 2 */
                if (ti && !part->tint && !part->shadow && (batch == GLR_SLOTS || (batch >= GLR_TI && batch < GLR_TB)))
                    tik = tk2 ? 3 : !ti_flat ? 1 : ((q->color >> 12) & 7) == 1 ? 2 : 0;
                if (tik && pass == 1) {
                    glr_tipoly *tp;
                    if (tip_n == tip_cap) { glr_tipoly *g = realloc(tip, (size_t)(tip_cap = tip_cap ? 2 * tip_cap : 256) * sizeof *tip); if (!g) { tik = 0; goto no_tip; } tip = g; }
                    tp = &tip[tip_n++];
                    tp->first = (int)pos[batch]; tp->n = q->n; tp->kind = (uint8_t)tik;
                    tp->c = (uint8_t)(tik == 3 || tik == 1 ? k2c : (int)(q->color & 0xff) >> 1);
                    tp->ramp = (uint8_t)(((q->color >> 8) & 0xf) << 4);
                    tp->cls = (uint8_t)(tik == 1 && batch >= GLR_TI && batch < GLR_TB ? k2cls : 0);
                    memcpy(tp->nrm, q->normal, sizeof tp->nrm);
                    tp->v0[0] = (float)v0->x; tp->v0[1] = (float)v0->y; tp->v0[2] = (float)v0->z;
                }
                no_tip:
                tk4 = ((q->color >> 12) & 7) == 4;
                ti_vmin = 0.0f;
                if (batch >= GLR_TI && batch < GLR_TB) {   /* (the Titanium type > 0 rule below: the polygon's least texel v) */
                    int k4;
                    ti_vmin = 1e9f;
                    for (k4 = 0; k4 < q->n; k4++) if ((float)o->verts[q->idx[k4]].b < ti_vmin) ti_vmin = (float)o->verts[q->idx[k4]].b;
                }
                pfirst = pos[batch];
                for (j = 1; j + 1 < q->n; j++) {
                    int tri[3] = {0, j, j + 1};
                    float cy = cosf(part->yaw * 3.14159265f / 180.0f), sy = sinf(part->yaw * 3.14159265f / 180.0f);
                    for (a = 0; a < 3; a++) {
                        const wtb_vertex *v = &o->verts[q->idx[tri[a]]];
                        float *f = buf + pos[batch] * FLOATS_PER_VERTEX;
                        float nx, ny, nz, rx, rz;
                        int c;
                        memcpy(sph + pos[batch] * 4, psph, sizeof psph);
                        /* rotate (full matrix when posed, else yaw), place, then GL z = -game z */
                        if ((model->extended || ti) && (v->normal[0] != 0 || v->normal[1] != 0 || v->normal[2] != 0)) {
                            nx = v->normal[0]; ny = v->normal[1]; nz = v->normal[2];
                        } else {
                            nx = face[0]; ny = face[1]; nz = face[2];
                        }
                        if (part->has_rot) {
                            const float *R = part->rot;
                            float vx = (float)v->x, vy = (float)v->y, vz = (float)v->z;
                            f[0] = R[0] * vx + R[1] * vy + R[2] * vz + (float)part->pos[0];
                            f[1] = R[3] * vx + R[4] * vy + R[5] * vz + (float)part->pos[1];
                            f[2] = -(R[6] * vx + R[7] * vy + R[8] * vz + (float)part->pos[2]);
                            f[3] = R[0] * nx + R[1] * ny + R[2] * nz;
                            f[4] = R[3] * nx + R[4] * ny + R[5] * nz;
                            f[5] = -(R[6] * nx + R[7] * ny + R[8] * nz);
                            if (wface) { f[3] = nx; f[4] = ny; f[5] = -nz; }
                        } else {
                            rx = cy * (float)v->x + sy * (float)v->z;
                            rz = -sy * (float)v->x + cy * (float)v->z;
                            f[0] = rx + (float)part->pos[0];
                            f[1] = (float)(v->y + part->pos[1]) + ground_lift;
                            f[2] = -(rz + (float)part->pos[2]);
                            f[3] = cy * nx + sy * nz;
                            f[4] = ny;
                            f[5] = -(-sy * nx + cy * nz);
                        }
                        /* Titanium: the model's own normal, unturned (mercsw 0x1002ecaa reads the vertex's +0x40 as made at
                         * load, 0x10003df0; 0x10004934 turns only positions and polygon normals), its length kept */
                        if (ti && !ti_world_normals) { f[3] = nx; f[4] = ny; f[5] = -nz; }
                        f[6] = (float)v->a;
                        f[7] = (float)v->b;
                        if (batch >= GLR_TI && batch < GLR_TB) {   /* Titanium (mercsw 0x10001081, every textured kind via 0x10053280): a texture of
                                                  * type > 0 (not slot 0x8c) U = u / w, V = (v - the polygon's least v) / h
                                                  * (0x10001175..0x1000122a); type 0: texel / (size - 1) (0x1000124c); the
                                                  * shader divides by the size */
                            const texture *tt = r->ti_slots[batch - GLR_TI];
                            if (tt->type > 0 && batch - GLR_TI != 0x8c) f[7] -= ti_vmin;
                            else {
                                if (tt->w > 1) f[6] *= (float)tt->w / (float)(tt->w - 1);
                                if (tt->h > 1) f[7] *= (float)tt->h / (float)(tt->h - 1);
                            }
                        }
                        /* DOS Gouraud: the vertex's baked index a = ramp | intensity (0x4fef0 -> 0x38560); the shader rescales
                         * the intensity by the polygon's level */
                        if (r->dos && !model->extended && ((q->color >> 12) & 7) == 4) {   /* 0x3bb80: a < 48 not rescaled */
                            f[6] = v->a >= 48 ? (float)(v->a & 15) : 0.0f;
                            f[7] = v->a >= 48 ? (float)(v->a & ~15) : (float)v->a;
                        }
                        if (part->tint) { f[8] = (float)((part->tint >> 16) & 0xff) / 255.0f; f[9] = (float)((part->tint >> 8) & 0xff) / 255.0f; f[10] = (float)(part->tint & 0xff) / 255.0f; }
                        else if (ti_flat && tk4) for (c = 0; c < 3; c++) f[8 + c] = (float)palette[v->a & 0xff][c] / 255.0f;   /* 0x100535d2 */
                        else for (c = 0; c < 3; c++) f[8 + c] = poly_col[c];
                        /* a palette-coloured polygon (untextured, 3D editions): its index + 1 for the palette fades; a
                         * textured one: minus its texture switch class (notex_class) */
                        f[11] = batch >= GLR_SLOTS && !part->tint && !(r->dos && !model->extended) ? (float)((ti_flat && tk4 ? v->a & 0xff : pidx) + 1) : batch < GLR_SLOTS ? (float)-notex_cls : 0.0f;
                        if (tk2) f[11] = (float)-k2cls;   /* (v_ticm 3: drawn only with its class's textures off) */
                        if (r->dos && !model->extended && tk4 && batch == GLR_SLOTS) f[11] = (float)(pfirst + 1);   /* DOS Gouraud: its fan's first vertex + 1 (dos_gouraud) */
                        if (tik == 1 && batch >= GLR_TI && batch < GLR_TB) f[11] = (float)-k2cls;   /* (textured kinds 5-7: their switch class) */
                        if (tik) {
                            float *t = tic + pos[batch] * 4;
                            memcpy(tipn + pos[batch] * 6, f, 6 * sizeof *f);
                            if (tik == 1) t[0] = t[1] = t[2] = 1.0f;
                            else for (c = 0; c < 3; c++) t[c] = (float)palette[pidx][c] / 255.0f;
                            t[3] = (float)tik;
                        }
                        /* the wireframe's edges: this corner (0-2) + 3 x the polygon's own edges in this fan triangle
                         * (bit 0: corner 1 - 2, always; bit 1: corner 2 - 0 when it closes the polygon; bit 2: corner 0 - 1
                         * when it starts it), so the triangulation's diagonals are not drawn (DOS draws polygon outlines) */
                        f[12] = (float)(a + 3 * (1 | (j + 2 == q->n ? 2 : 0) | (j == 1 ? 4 : 0)) + 32 * wire_code + (ti ? 256 : 0) + (ti_flat || (hi_card && (q->color >> 12) == 0xb) ? 512 : 0) + 2048 * dnib);   /* (+ 512: unlit) */
                        for (c = 0; c < 3; c++) {
                            if (f[c] < mn[c]) mn[c] = f[c];
                            if (f[c] > mx[c]) mx[c] = f[c];
                        }
                        pos[batch]++;
                        n++;
                    }
                }
            }
        }
    }
    for (b = 0; b < GLR_BATCHES; b++) {
        L->batch_count[b] = (GLsizei)count[b];
        L->batch_first[b] = (GLint)(pos[b] - count[b]);
    }
    if (bounds && n > 0) {
        for (a = 0; a < 3; a++) r->center[a] = (mn[a] + mx[a]) / 2;
        r->radius = 0.5f * sqrtf((mx[0] - mn[0]) * (mx[0] - mn[0]) + (mx[1] - mn[1]) * (mx[1] - mn[1]) +
                                 (mx[2] - mn[2]) * (mx[2] - mn[2]));
        if (r->radius <= 0) r->radius = 1;
    }

    glBindVertexArray(L->vao);
    glBindBuffer(GL_ARRAY_BUFFER, L->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * FLOATS_PER_VERTEX * sizeof *buf), buf, GL_STATIC_DRAW);
    glBindTexture(GL_TEXTURE_BUFFER, L->tbo);   /* (the DOS Gouraud fill reads its polygon from it) */
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32F, L->vbo);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
    {
        static const int sizes[6] = {3, 3, 2, 3, 1, 1};
        size_t off = 0;
        for (a = 0; a < 6; a++) {
            glEnableVertexAttribArray((GLuint)a);
            glVertexAttribPointer((GLuint)a, sizes[a], GL_FLOAT, GL_FALSE, FLOATS_PER_VERTEX * sizeof(float),
                                  (const void *)(off * sizeof(float)));
            off += (size_t)sizes[a];
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, L->sph_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * 4 * sizeof *sph), sph, GL_STATIC_DRAW);
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void *)0);
    glBindBuffer(GL_ARRAY_BUFFER, L->tic_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * 4 * sizeof *tic), tic, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(7);
    glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void *)0);
    free(L->tic); free(L->tipn); free(L->tistat); free(L->tip);
    L->tic = tic; L->tipn = tipn; L->tistat = NULL; L->tip = tip; L->tip_n = tip_n;
    L->tic_static_ok = 0; L->tic_dyn = 1;
    tic = tipn = NULL; tip = NULL;
    glBindVertexArray(0);
    L->vertex_count = (GLsizei)n;
    for (b = 0; b < GLR_SLOTS; b++)
        if (L->batch_count[b] > 0) upload_texture(r, b, r->slot_frame[b] && slots[b] ? r->slot_frame[b] : slots[b]);
    for (b = 0; b < GLR_SLOTS; b++)
        if (L->batch_count[GLR_B0 + b] > 0) upload_texture(r, GLR_SLOTS + b, r->bank0 ? r->bank0[b] : NULL);
    for (b = 0; b < GLR_SLOTS; b++)
        if (L->batch_count[GLR_TI + b] > 0) upload_texture(r, 2 * GLR_SLOTS + b, r->ti_slots ? r->ti_slots[b] : NULL);
    for (b = 0; b < GLR_SLOTS; b++)
        if (L->batch_count[GLR_TB + b] > 0) upload_texture(r, 3 * GLR_SLOTS + b, r->ti_bank0 ? r->ti_bank0[b] : NULL);
    if (li == 0) {   /* tests: the static layer's billboards */
        g_billboards = 0;
        for (b = 0; b < GLR_SLOTS; b++) g_billboards += (int)L->batch_count[GLR_B0 + b] / 6;
    }
    free(buf); free(sph); free(count); free(pos); free(tic); free(tipn); free(tip);
    return 0;
}

/* A fan triangle vertex's corner: vertex first + 3 t + a of a polygon is corner 0 (a 0) or t + a */
static int ti_corner_vertex(const glr_tipoly *tp, int k)
{
    return k == 0 ? tp->first : k <= tp->n - 2 ? tp->first + 3 * (k - 1) + 1 : tp->first + 3 * (tp->n - 3) + 2;
}

/* the engine's vector length (0x100462fe): the largest component + a quarter of the other two */
static float ti_len3(float x, float y, float z)
{
    float ax = fabsf(x), ay = fabsf(y), az = fabsf(z), m = ax > ay ? (ax > az ? ax : az) : (ay > az ? ay : az);
    return m + (ax + ay + az - m) * 0.25f;
}

/* The Titanium per-vertex colours of a layer (mercsw's granularity), each frame before it is drawn:
 *  - lit polygons (kinds 5-7): each corner's static lights (0x1002ecaa: the ambient, then each light - directional
 *    -L.N, point N.(L - P) / |L - P| - x its colour, at most 255), interpolated across the polygon (Gouraud, dx5draw's
 *    SHADEMODE 0x10002b6f); then (Multiple Lights) each explosion / projectile light whose +-radius box holds one of the
 *    polygon's corners (0x1002eb36, per polygon) adds, corner by corner, a running sum S - started at the polygon's LAST
 *    corner's static colour and never reset, + the light's colour x N.(L - P) / |L - P| (when > 0) x 255 - to the corner
 *    (0x1002f3f9..0x1002f5d5), at most 255. All in GL coordinates (the shader's), the normal the model's own (unturned);
 *  - colour kinds 1 / 2 (flat): the palette shade of 0x1002f5e3 (ti_shade_base / _finish) with its fog term (the
 *    polygon's deepest corner, 0x100575d3) and Multiple Lights term, one colour per polygon. */
static void ti_colours(glr *r, int li, const glr_view *v, const float view[16], const float eye_gl[3])
{
    glr_layer *L = &r->layer[li];
    const glr_ti_shade *sh = &r->ti_shade;
    float key[24], lv[2][3], dp[GLR_TI_DYN][4], dc[GLR_TI_DYN][3];
    int lt[2], nd = v->ti_dyn_n < GLR_TI_DYN ? v->ti_dyn_n : GLR_TI_DYN, k, i, j, c, lo = -1, hi = -1;
    const int notex = (v->notex_actors ? 1 : 0) | (v->notex_world ? 2 : 0);   /* (as u_notex) */
    const float eye_g[3] = {eye_gl[0], eye_gl[1], -eye_gl[2]};
    if (L->tip_n <= 0 || !L->tic || !L->tipn) return;
    for (k = 0; k < 2; k++) {
        float x = v->ti_lvec[k][0], y = v->ti_lvec[k][1], z = -v->ti_lvec[k][2], l = sqrtf(x * x + y * y + z * z);
        lt[k] = v->ti_ltype[k];
        if (lt[k] == 1 && l > 0) { x /= l; y /= l; z /= l; }   /* (0x1002f9f5) */
        lv[k][0] = x; lv[k][1] = y; lv[k][2] = z;
    }
    for (k = 0; k < nd; k++) {
        dp[k][0] = v->ti_dyn[k][0]; dp[k][1] = v->ti_dyn[k][1]; dp[k][2] = -v->ti_dyn[k][2]; dp[k][3] = v->ti_dyn[k][3];
        dc[k][0] = v->ti_dyn[k][4]; dc[k][1] = v->ti_dyn[k][5]; dc[k][2] = v->ti_dyn[k][6];
    }
    memset(key, 0, sizeof key);
    memcpy(key, v->ti_amb, 3 * sizeof *key);
    memcpy(key + 3, lv, 6 * sizeof *key);
    memcpy(key + 9, v->ti_lcol, 6 * sizeof *key);
    key[15] = (float)lt[0]; key[16] = (float)lt[1];
    key[17] = (float)notex;   /* (a texture switch changed: every polygon rewritten) */
    if (!L->tistat) L->tistat = calloc((size_t)L->vertex_count * 3 + 1, sizeof *L->tistat);
    if (!L->tistat) return;
    if (!L->tic_static_ok || memcmp(key, L->tic_key, sizeof key) != 0) {   /* the static lights: once per mission */
        for (i = 0; i < L->tip_n; i++) {
            const glr_tipoly *tp = &L->tip[i];
            int e = tp->first + 3 * (tp->n - 2);
            if (tp->kind != 1) continue;
            for (j = tp->first; j < e; j++) {
                const float *P = L->tipn + (size_t)j * 6, *N = P + 3;
                float col[3];
                memcpy(col, v->ti_amb, sizeof col);
                for (k = 0; k < 2; k++) {
                    float d;
                    if (lt[k] == 1) { d = -(lv[k][0] * N[0] + lv[k][1] * N[1] + lv[k][2] * N[2]); if (d <= 0) continue; }
                    else if (lt[k] == 2) {
                        float lx = lv[k][0] - P[0], ly = lv[k][1] - P[1], lz = lv[k][2] - P[2], ll = ti_len3(lx, ly, lz);
                        d = lx * N[0] + ly * N[1] + lz * N[2];
                        if (d <= 0) continue;
                        d /= ll > 1e-3f ? ll : 1e-3f;
                    } else d = 1.0f;
                    for (c = 0; c < 3; c++) col[c] += v->ti_lcol[k][c] * d;
                }
                for (c = 0; c < 3; c++) L->tistat[(size_t)j * 3 + c] = col[c] > 1.0f ? 1.0f : col[c];
            }
        }
        memcpy(L->tic_key, key, sizeof key);
        L->tic_static_ok = 0;
    }
    for (i = 0; i < L->tip_n; i++) {
        const glr_tipoly *tp = &L->tip[i];
        int e = tp->first + 3 * (tp->n - 2), hit[GLR_TI_DYN], nh = 0;
        for (k = 0; k < nd; k++) {   /* 0x1002eb36: one of its corners inside the light's +-radius box (edges included) */
            hit[k] = 0;
            for (j = 0; j < tp->n && !hit[k]; j++) {
                const float *P = L->tipn + (size_t)ti_corner_vertex(tp, j) * 6;
                hit[k] = fabsf(P[0] - dp[k][0]) <= dp[k][3] && fabsf(P[1] - dp[k][1]) <= dp[k][3] && fabsf(P[2] - dp[k][2]) <= dp[k][3];
            }
            nh += hit[k];
        }
        /* a textured polygon (kinds 5-7) whose class is switched off: the kind 1 shade (0x1001b47a..0x1001b4f3) */
        const int off = tp->kind == 1 && (tp->cls & notex) != 0;
        if (tp->kind == 1 && !off) {
            float col[WTB_MAX_POLY_VERTS][3], S[3];
            if (!nh && L->tic_static_ok && !L->tic_dyn) continue;   /* (unchanged) */
            for (j = 0; j < tp->n; j++) memcpy(col[j], L->tistat + (size_t)ti_corner_vertex(tp, j) * 3, sizeof col[j]);
            memcpy(S, col[tp->n - 1], sizeof S);
            for (k = 0; k < nd; k++) {
                if (!hit[k]) continue;
                for (j = 0; j < tp->n; j++) {
                    const float *P = L->tipn + (size_t)ti_corner_vertex(tp, j) * 6, *N = P + 3;
                    float lx = dp[k][0] - P[0], ly = dp[k][1] - P[1], lz = dp[k][2] - P[2], d = lx * N[0] + ly * N[1] + lz * N[2];
                    if (d > 0) {
                        float ll = ti_len3(lx, ly, lz);
                        d /= ll > 1e-3f ? ll : 1e-3f;
                        for (c = 0; c < 3; c++) S[c] += dc[k][c] * d;
                    }
                    for (c = 0; c < 3; c++) { col[j][c] += S[c]; if (col[j][c] > 1.0f) col[j][c] = 1.0f; }
                }
            }
            for (j = tp->first; j < e; j++) {
                int t = (j - tp->first) / 3, a = (j - tp->first) % 3, cn = a == 0 ? 0 : t + a;
                memcpy(L->tic + (size_t)j * 4, col[cn], 3 * sizeof(float));
                L->tic[(size_t)j * 4 + 3] = 1.0f;
            }
        } else if (sh->on || off) {   /* colour kinds 1 / 2 (and switched-off textured kinds): the shade, flat */
            int lev, Lv, done = 0;
            float depth = -1e30f;
            Lv = tp->n >= 3 ? ti_shade_base(sh, tp->nrm, tp->v0, eye_g) : (int)sh->amb;
            /* the depth: the polygon's deepest corner. mercsw 0x1005751c..0x10057638 picks it by the render object's
             * flag word (0x10106c50 = node +0): 2 the corners' average, 4 the nearest, else the deepest, 1 + 2^28 (the
             * 3D DLL the same, 0x1002b9b0 at 0x1002bea3 / 0x10247730) - but every node is made with +0 = DAT_100a6298 | 0x8000
             * (0x10003848), DAT_100a6298 is 0 in the file and its only setter (0x1000354a) is never called (no
             * reference, no export); the 3D DLL's 0x10029590 likewise DAT_1024e400 (0, never written) | 0x8000. So
             * the deepest corner always */
            if (sh->range > 1e-7f) {   /* 0x1002f6cb: (depth / range) >> 4 off, at least 0 */
                for (j = 0; j < tp->n; j++) {
                    const float *P = L->tipn + (size_t)ti_corner_vertex(tp, j) * 6;
                    float z = -(view[2] * P[0] + view[6] * P[1] + view[10] * P[2] + view[14]);
                    if (z > depth) depth = z;
                }
                Lv -= (int)(depth / sh->range) >> 4;
                if (Lv < 0) Lv = 0;
            }
            for (k = 0; k < nd && Lv < 0x80 && !done && tp->n >= 3; k++) {   /* Multiple Lights: a point at the light */
                const float lg[3] = {dp[k][0], dp[k][1], -dp[k][2]};
                int I;
                if (!hit[k]) continue;
                I = ti_shade_i(tp->nrm, tp->v0, lg, 0);
                if (I > 0) { Lv += I; if (Lv >= 0x80) done = 1; }
            }
            lev = sh->on ? ti_shade_finish(Lv, tp->c) : 8;
            {
                const uint8_t *pe = r->ti_pal[li][(tp->ramp | lev) & 0xff];
                float cc[3] = {pe[0] / 255.0f, pe[1] / 255.0f, pe[2] / 255.0f};
                float *t0 = L->tic + (size_t)tp->first * 4;
                if (t0[0] == cc[0] && t0[1] == cc[1] && t0[2] == cc[2] && L->tic_static_ok) continue;
                for (j = tp->first; j < e; j++) {
                    memcpy(L->tic + (size_t)j * 4, cc, sizeof cc);
                    if (off) L->tic[(size_t)j * 4 + 3] = 3.0f;   /* (the colour, unlit, as kind 2's) */
                }
            }
        } else continue;
        if (lo < 0 || tp->first < lo) lo = tp->first;
        if (e > hi) hi = e;
    }
    if (getenv("MW2_DEBUG_TISHADE")) {   /* (tests) */
        static int last[2] = {-1, -1};
        int n_off = 0;
        for (i = 0; i < L->tip_n; i++) n_off += L->tip[i].kind == 1 && (L->tip[i].cls & notex) != 0;
        if (li >= 0 && li < 2 && n_off != last[li]) {
            last[li] = n_off;
            printf("titanium: %d textured polygons as the kind 1 shade\n", (last[0] > 0 ? last[0] : 0) + (last[1] > 0 ? last[1] : 0));
        }
    }
    L->tic_dyn = nd > 0;
    L->tic_static_ok = 1;
    if (lo >= 0 && hi > lo) {
        glBindBuffer(GL_ARRAY_BUFFER, L->tic_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)((size_t)lo * 4 * sizeof(float)), (GLsizeiptr)((size_t)(hi - lo) * 4 * sizeof(float)), L->tic + (size_t)lo * 4);
    }
}

int glr_set_mech(glr *r, const mech3d *m, const uint8_t palette[256][3],
                 texture *const slots[GLR_SLOTS], int camo, int clan)
{
    return set_layer(r, 0, m, palette, slots, camo, clan, 1);
}

int glr_set_actors(glr *r, const mech3d *m, const uint8_t palette[256][3],
                   texture *const slots[GLR_SLOTS], int camo, int clan)
{
    return set_layer(r, 1, m, palette, slots, camo, clan, 0);
}

void glr_set_titanium(glr *r, texture *const slots[GLR_SLOTS], const int camo[8])
{
    r->ti_slots = slots;
    if (camo) memcpy(r->ti_camo, camo, sizeof r->ti_camo); else memset(r->ti_camo, 0, sizeof r->ti_camo);
    if (!slots) { r->ti_bank0 = NULL; memset(&r->ti_shade, 0, sizeof r->ti_shade); }
}

void glr_set_titanium_extra(glr *r, texture *const bank0[GLR_SLOTS], const glr_ti_shade *shade)
{
    r->ti_bank0 = bank0;
    if (shade) r->ti_shade = *shade; else memset(&r->ti_shade, 0, sizeof r->ti_shade);
}

void glr_texture_average(const texture *t, float out[3])
{
    /* engine 0x1002d330: every texel of the bitmap (transparent ones too) as RGB555, the 5-bit sums / (texels / 8) - the
     * 0..248 scale Glide takes as 0..255 */
    unsigned long sum[3] = {0, 0, 0}, n, i, d;
    const unsigned char *by = (const unsigned char *)(t ? t->rgba : NULL);   /* RGBA bytes, as uploaded */
    int c;
    out[0] = out[1] = out[2] = 0.5f;
    if (!by || t->w <= 0 || t->h <= 0) return;
    n = (unsigned long)t->w * (unsigned long)t->h;
    for (i = 0; i < n; i++) for (c = 0; c < 3; c++) sum[c] += by[i * 4 + c] >> 3;
    d = n >> 3 ? n >> 3 : 1;
    for (c = 0; c < 3; c++) out[c] = (float)(sum[c] / d) / 255.0f;
}

void glr_set_environment(glr *r, const texture *ground, const texture *sky, float tile_sky, float tile_ground)
{
    const texture *t[2] = {ground, sky};
    int k;
    r->tile_sky = tile_sky;
    r->tile_ground = tile_ground;
    for (k = 0; k < 2; k++) {
        r->env_has[k] = t[k] != NULL;
        if (!t[k]) continue;
        glBindTexture(GL_TEXTURE_2D, r->env_tex[k]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t[k]->w, t[k]->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t[k]->rgba);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        {   /* the flat ground / sky: the texture's average colour */
            unsigned long sum[3] = {0, 0, 0}, n = (unsigned long)t[k]->w * (unsigned long)t[k]->h, i;
            unsigned char px[4];
            const unsigned char *by = (const unsigned char *)t[k]->rgba;   /* RGBA bytes, as uploaded */
            GLuint *ft = k == 0 ? &r->ground_flat : &r->sky_flat;
            for (i = 0; i < n; i++) { sum[0] += by[i * 4]; sum[1] += by[i * 4 + 1]; sum[2] += by[i * 4 + 2]; }
            px[0] = (unsigned char)(n ? sum[0] / n : 0); px[1] = (unsigned char)(n ? sum[1] / n : 0); px[2] = (unsigned char)(n ? sum[2] / n : 0); px[3] = 255;
            if (!*ft) glGenTextures(1, ft);
            glBindTexture(GL_TEXTURE_2D, *ft);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        }
    }
}

/* Append a vertex in the mesh layout: pos, normal, uv, colour, texsel. */
static float *env_vertex(float *f, float x, float y, float z, float nx, float ny, float nz, float u, float v)
{
    f[0] = x; f[1] = y; f[2] = z; f[3] = nx; f[4] = ny; f[5] = nz; f[6] = u; f[7] = v;
    f[8] = f[9] = f[10] = 1; f[11] = 0; f[12] = 0;
    return f + FLOATS_PER_VERTEX;
}

void glr_set_bank0(glr *r, texture *const bank0[GLR_SLOTS]) { if (r) r->bank0 = bank0; }

void glr_set_skydome(glr *r, const wtb_object *o, texture *const slots[GLR_SLOTS])
{
    int i, k, s, n = 0;
    free(r->dome_v);
    r->dome_v = NULL;
    r->dome_n = 0;
    free(r->dome_pfirst); free(r->dome_pcount); free(r->dome_pslot);
    r->dome_pfirst = r->dome_pcount = r->dome_pslot = NULL;
    r->dome_np = 0;
    memset(r->dome_count, 0, sizeof r->dome_count);
    memset(r->dome_has, 0, sizeof r->dome_has);
    if (!o || o->poly_count <= 0) return;
    for (i = 0; i < o->poly_count; i++) if (o->polys[i].n >= 3) n += (o->polys[i].n - 2) * 3;
    if (!(r->dome_v = malloc((size_t)(n ? n : 1) * 5 * sizeof *r->dome_v))) return;
    r->dome_pfirst = malloc((size_t)o->poly_count * sizeof *r->dome_pfirst);
    r->dome_pcount = malloc((size_t)o->poly_count * sizeof *r->dome_pcount);
    r->dome_pslot = malloc((size_t)o->poly_count * sizeof *r->dome_pslot);
    if (!r->dome_pfirst || !r->dome_pcount || !r->dome_pslot) return;
    for (s = 0; s < GLR_SLOTS; s++) {   /* grouped by texture */
        const texture *t = slots[s];
        r->dome_first[s] = r->dome_n;
        for (i = 0; i < o->poly_count; i++) {
            const wtb_poly *p = &o->polys[i];
            float vmin = 1e9f;
            if ((p->color & 0xff) != s || p->n < 3 || !t) continue;
            for (k = 0; k < p->n; k++) {
                const wtb_vertex *q = &o->verts[p->idx[k] < o->vert_count ? p->idx[k] : 0];
                if ((float)q->b < vmin) vmin = (float)q->b;
            }
            for (k = 1; k + 1 < p->n; k++) {   /* a fan */
                int c, id[3] = {p->idx[0], p->idx[k], p->idx[k + 1]};
                for (c = 0; c < 3; c++) {
                    const wtb_vertex *q = &o->verts[id[c] < o->vert_count ? id[c] : 0];
                    float *f = r->dome_v + (size_t)r->dome_n * 5;
                    f[0] = (float)q->x; f[1] = (float)q->y; f[2] = -(float)q->z;
                    /* mercsw 0x10001081 (kind 5, 0x10053883): a texture of type > 0 other than the clouds' slot 0x8c
                     * (mountains type 3, moon / sun type 7): U = u / w, V = (v - the polygon's least v) / h
                     * (0x10001175..0x1000122a); the clouds and type 0: texel / (size - 1) (0x1000124c) */
                    if (t->type > 0 && s != 0x8c) { f[3] = (float)q->a / (float)t->w; f[4] = ((float)q->b - vmin) / (float)t->h; }
                    else { f[3] = (float)q->a / (float)(t->w > 1 ? t->w - 1 : 1); f[4] = (float)q->b / (float)(t->h > 1 ? t->h - 1 : 1); }
                    r->dome_n++;
                }
            }
            r->dome_pfirst[i] = r->dome_n - (p->n - 2) * 3; r->dome_pcount[i] = (p->n - 2) * 3; r->dome_pslot[i] = s;
        }
        r->dome_count[s] = r->dome_n - r->dome_first[s];
        if (!r->dome_count[s]) continue;
        r->dome_has[s] = 1;
        glBindTexture(GL_TEXTURE_2D, r->dome_tex[s]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->w, t->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t->rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);   /* bilinear, no mips (dx5draw 0x100034a0) */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    r->dome_np = o->poly_count;
    for (i = 0; i < o->poly_count; i++)   /* (a polygon without a texture or with under 3 corners: none) */
        if (o->polys[i].n < 3 || !slots[o->polys[i].color & 0xff]) r->dome_pcount[i] = 0;
}
int glr_billboard_count(void) { return g_billboards; }

void glr_set_particles(glr *r, const glr_particle *p, int n, float alpha)
{
    if (n < 0) n = 0;
    if (n > r->par_cap) {
        glr_particle *g = realloc(r->par, (size_t)n * sizeof *g);
        if (!g) n = r->par_cap; else { r->par = g; r->par_cap = n; }
    }
    if (n) memcpy(r->par, p, (size_t)n * sizeof *p);
    r->par_n = n;
    r->par_alpha = alpha;
}

void glr_set_sprites(glr *r, const glr_sprite *s, int n)
{
    if (n > GLR_MAX_SPRITES) n = GLR_MAX_SPRITES;
    if (n < 0) n = 0;
    if (n) memcpy(r->spr, s, (size_t)n * sizeof *s);
    r->spr_n = n;
}

/* the GL texture for a sprite texture (small cache keyed by the texture) */
static GLuint sprite_texture(glr *r, const texture *t)
{
    int k;
    for (k = 0; k < 64; k++) if (r->spr_src[k] == t) return r->spr_tex[k];
    k = r->spr_next;
    r->spr_next = (r->spr_next + 1) & 63;
    r->spr_src[k] = t;
    glBindTexture(GL_TEXTURE_2D, r->spr_tex[k]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->w, t->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t->rgba);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return r->spr_tex[k];
}

void glr_default_view(glr_view *v)
{
    memset(v, 0, sizeof *v);
    v->yaw = 30;
    v->pitch = 10;
    v->vfov = 40;
    v->light[0] = 0.4f; v->light[1] = 0.8f; v->light[2] = 0.45f;
    v->ambient = 0.35f;
    v->sky_color[0] = 0.35f; v->sky_color[1] = 0.43f; v->sky_color[2] = 0.55f;
    v->fog_color[0] = v->sky_color[0]; v->fog_color[1] = v->sky_color[1]; v->fog_color[2] = v->sky_color[2];
    v->bilinear = 1;
}

/* column-major 4x4 helpers */
static void mat_mul(float *o, const float *a, const float *b)
{
    float t[16];
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            float s = 0;
            for (k = 0; k < 4; k++) s += a[k * 4 + j] * b[i * 4 + k];
            t[i * 4 + j] = s;
        }
    memcpy(o, t, sizeof t);
}

/* The PowerVR fog constant K (index x game units) for an sgl_set_fog density d (MW2 passes skygnd.par's value):
 * the driver takes x = -log2(1 - d), splits it by frexp into fraction m and power p, scales the camera depth Cd =
 * foreground x 0.1 by 1/m and shifts the depth by CdPower + p + 22 - (screen rescale shifts); the hardware depth is
 * Cd' / z x 2^31 (z in SGL units = game units x 0.1, MW2 0x101a7580). With MW2's foreground 50 x 0.1 = 5 (Cd 0.5:
 * fraction 0.5, power 0) all the powers cancel: i = 2^31 x 0.5 / m / 2^(p + 22) / (0.1 z) = 2560 / (x z) */
static float fog_k(float d)
{
    double x;
    if (d <= 0) return 0;
    if (d >= 1) return 1e9f;
    x = -log2(1.0 - (double)d);
    return x > 0 ? (float)(2560.0 / x) : 0;
}

static void perspective(float *m, float vfov_deg, float aspect, float zn, float zf)
{
    float f = 1.0f / tanf(vfov_deg * 3.14159265f / 360.0f);
    memset(m, 0, 16 * sizeof *m);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zf + zn) / (zn - zf);
    m[11] = -1;
    m[14] = 2 * zf * zn / (zn - zf);
}

static void look_at(float *m, const float eye[3], const float at[3])
{
    float f[3], s[3], u[3], up[3] = {0, 1, 0}, l;
    int i;
    for (i = 0; i < 3; i++) f[i] = at[i] - eye[i];
    l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (i = 0; i < 3; i++) f[i] /= l;
    s[0] = f[1] * up[2] - f[2] * up[1]; s[1] = f[2] * up[0] - f[0] * up[2]; s[2] = f[0] * up[1] - f[1] * up[0];
    l = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (i = 0; i < 3; i++) s[i] /= l;
    u[0] = s[1] * f[2] - s[2] * f[1]; u[1] = s[2] * f[0] - s[0] * f[2]; u[2] = s[0] * f[1] - s[1] * f[0];
    memset(m, 0, 16 * sizeof *m);
    m[0] = s[0]; m[4] = s[1]; m[8] = s[2];
    m[1] = u[0]; m[5] = u[1]; m[9] = u[2];
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
    m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    m[15] = 1;
}

void glr_set_world_fade(glr *r, const uint8_t pal[256][3], float k, const float tint[3])
{
    if (!r) return;
    r->tint_on = tint != NULL;
    if (tint) memcpy(r->tint, tint, sizeof r->tint);
    r->pfade = pal && k > 0 ? (k > 1 ? 1 : k) : 0;
    if (r->pfade > 0) {
        uint8_t img[256][4];
        int x;
        for (x = 0; x < 256; x++) { img[x][0] = pal[x][0]; img[x][1] = pal[x][1]; img[x][2] = pal[x][2]; img[x][3] = 255; }
        glBindTexture(GL_TEXTURE_2D, r->ptab_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
}

void glr_set_damage_shade(glr *r, int sign) { if (r) r->dsign = sign != 0; }

void glr_set_dos_palette(glr *r, const uint8_t pal[256][3])
{
    int x;
    for (x = 0; x < 256; x++) { r->dos_img[16][x][0] = pal[x][0]; r->dos_img[16][x][1] = pal[x][1]; r->dos_img[16][x][2] = pal[x][2]; }
    glBindTexture(GL_TEXTURE_2D, r->dos_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 16, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, r->dos_img[16]);
}

void glr_set_dos(glr *r, const uint8_t pal[256][3], const uint8_t *luma)
{
    uint8_t (*img)[256][4] = r->dos_img;
    int x, y;
    r->dos = pal != NULL;
    if (!pal) return;
    for (y = 0; y < 16; y++)
        for (x = 0; x < 256; x++) { uint8_t v = luma ? luma[y * 256 + x] : (uint8_t)x; img[y][x][0] = img[y][x][1] = img[y][x][2] = v; img[y][x][3] = 255; }
    for (x = 0; x < 256; x++) { img[16][x][0] = pal[x][0]; img[16][x][1] = pal[x][1]; img[16][x][2] = pal[x][2]; img[16][x][3] = 255; }
    glBindTexture(GL_TEXTURE_2D, r->dos_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 17, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void glr_draw(glr *r, const glr_view *v, int w, int h) { glr_draw_rect(r, v, 0, 0, w, h); }

/* Draw into the window rectangle x, y (from the bottom left), w x h: a cockpit viewport (rear / down view). */
void glr_draw_rect(glr *r, const glr_view *v, int x0, int y0, int w, int h)
{
    float proj[16], view[16], mvp[16], eye[3], L[3], ll, dist;
    float yaw = v->yaw * 3.14159265f / 180.0f, pitch = v->pitch * 3.14159265f / 180.0f;
    int s;
    GLenum minf = v->bilinear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST, magf = v->bilinear ? GL_LINEAR : GL_NEAREST;
    GLenum eminf, emagf;
    if (v->tex_min) minf = (GLenum)v->tex_min;   /* an edition's own filtering */
    if (v->tex_mag) magf = (GLenum)v->tex_mag;
    if (r->dos) minf = magf = GL_NEAREST;        /* DOS: texel indices, never blended */
    eminf = v->env_min ? (GLenum)v->env_min : minf; emagf = v->env_mag ? (GLenum)v->env_mag : magf;

    /* auto framing: fit the bounding sphere in the vertical field of view */
    if (v->use_target) {
        r->center[0] = v->target[0]; r->center[1] = v->target[1]; r->center[2] = -v->target[2];
        dist = v->distance_abs;
    } else {
        dist = v->distance > 0 ? v->distance * r->radius
                               : r->radius / sinf(v->vfov * 3.14159265f / 360.0f) * 1.05f;
    }
    eye[0] = r->center[0] + dist * cosf(pitch) * sinf(yaw);
    eye[1] = r->center[1] + dist * sinf(pitch);
    eye[2] = r->center[2] + dist * cosf(pitch) * cosf(yaw);
    if (v->free_cam) {
        /* first person: game coords -> GL (z negated); look direction from yaw/pitch, yaw 0 = +z (game forward) */
        float ly = v->look_yaw * 3.14159265f / 180.0f, lp = v->look_pitch * 3.14159265f / 180.0f;
        eye[0] = v->eye[0]; eye[1] = v->eye[1]; eye[2] = -v->eye[2];
        r->center[0] = eye[0] + cosf(lp) * sinf(ly) * 1000.0f;
        r->center[1] = eye[1] + sinf(lp) * 1000.0f;
        r->center[2] = eye[2] - cosf(lp) * cosf(ly) * 1000.0f;
        dist = 1200.0f; /* near plane = dist * 0.05 = 60 cm: the cockpit shell right at the eye isn't drawn (ASSUMED) */
    }
    perspective(proj, v->vfov, (float)w / (float)(h > 0 ? h : 1), dist * (v->use_target ? 0.01f : 0.05f),
                (r->env_has[0] ? 2.0e6f : dist * 4.0f + r->radius * 2));
    if (v->ortho_w > 0) {   /* x, y scaled to the width, depth over 0..2e6 */
        float hw = v->ortho_w * 0.5f, hh = hw * (float)h / (float)(w > 0 ? w : 1), zn = 1.0f, zf = 2.0e6f;
        memset(proj, 0, sizeof proj);
        proj[0] = 1.0f / hw; proj[5] = 1.0f / hh; proj[10] = -2.0f / (zf - zn); proj[14] = -(zf + zn) / (zf - zn); proj[15] = 1.0f;
    }
    look_at(view, eye, r->center);
    mat_mul(mvp, proj, view);

    ll = sqrtf(v->light[0] * v->light[0] + v->light[1] * v->light[1] + v->light[2] * v->light[2]);
    for (s = 0; s < 3; s++) L[s] = v->light[s] / (ll > 0 ? ll : 1);

    glViewport(x0, y0, w, h);
    if (x0 || y0) { glEnable(GL_SCISSOR_TEST); glScissor(x0, y0, w, h); }
    if (v->clip[2] > 0 && v->clip[3] > 0) { glEnable(GL_SCISSOR_TEST); glScissor(v->clip[0], v->clip[1], v->clip[2], v->clip[3]); }
    if (v->wire || v->solo) glClearColor(0, 0, 0, 1); else glClearColor(v->sky_color[0], v->sky_color[1], v->sky_color[2], 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE); /* winding isn't consistent in the data */
    glUseProgram(r->prog);
    glUniform1i(r->u_point, 0);                 /* the sky and ground layers: the sun only (explosions light objects) */
    glUniform3fv(r->u_lpos, 1, v->light_pos);
    glUniform3fv(r->u_light, 1, L);
    glUniform1f(r->u_ambient, v->ambient);
    glUniform3fv(r->u_fog_color, 1, v->fog_color);
    glUniform1i(r->u_tex, 0);
    glUniform1i(r->u_dos, 0);
    if (r->tint_on && !r->dos) glUniform3fv(r->u_tint, 1, r->tint); else glUniform3f(r->u_tint, 1, 1, 1);
    glUniform1i(r->u_tinton, r->tint_on && !r->dos && !r->ti_slots);   /* (not the Titanium look: its renderer not traced) */
    glUniform1i(r->u_dsign, r->dsign);
    glUniform1f(r->u_pfade, 0);
    glUniform1i(r->u_horizon, 0);
    glUniform1i(r->u_mga_lod, 0);
    glUniform1f(r->u_alpha, 1.0f);
    glUniform1i(r->u_blend, 0);
    glUniform4f(r->u_tifog, v->ortho_w > 0 ? 0.0f : v->ti_fog[0], v->ti_fog[1], 0, v->ti_fog[2]);
    glUniform1i(r->u_unlit, r->dos);            /* DOS: the sky and ground are flat palette colours, unlit (0x3c000) */
    {   /* the Titanium lights (glr_view.ti_light), in GL coordinates (z negated); off until the ground and the layers */
        float lv[2][3], lc[2][3], dp[GLR_TI_DYN][4], dc[GLR_TI_DYN][3];
        int lt[2], k2, nd = v->ti_dyn_n < GLR_TI_DYN ? v->ti_dyn_n : GLR_TI_DYN;
        for (k2 = 0; k2 < 2; k2++) {
            float x = v->ti_lvec[k2][0], y = v->ti_lvec[k2][1], z = -v->ti_lvec[k2][2], l = sqrtf(x * x + y * y + z * z);
            lt[k2] = v->ti_ltype[k2];
            if (lt[k2] == 1) { if (l > 0) { x /= l; y /= l; z /= l; } }   /* (0x1002f9f5 normalises a direction) */
            lv[k2][0] = x; lv[k2][1] = y; lv[k2][2] = z;
            memcpy(lc[k2], v->ti_lcol[k2], sizeof lc[k2]);
        }
        for (k2 = 0; k2 < nd; k2++) {
            dp[k2][0] = v->ti_dyn[k2][0]; dp[k2][1] = v->ti_dyn[k2][1]; dp[k2][2] = -v->ti_dyn[k2][2]; dp[k2][3] = v->ti_dyn[k2][3];
            dc[k2][0] = v->ti_dyn[k2][4]; dc[k2][1] = v->ti_dyn[k2][5]; dc[k2][2] = v->ti_dyn[k2][6];
        }
        glUniform1i(r->u_tilit, 0);
        glUniform3fv(r->u_tiamb, 1, v->ti_amb);
        glUniform3fv(r->u_tignd, 1, v->ti_gnd);
        glUniform2i(r->u_tilt, lt[0], lt[1]);
        glUniform3fv(r->u_tilv, 2, &lv[0][0]);
        glUniform3fv(r->u_tilc, 2, &lc[0][0]);
        glUniform1i(r->u_tidn, nd);
        if (nd > 0) { glUniform4fv(r->u_tidp, nd, &dp[0][0]); glUniform3fv(r->u_tidc, nd, &dc[0][0]); }
    }
    glUniform2f(r->u_texsize, 1, 1);            /* environment UVs are in texture repeats */
    glUniform1i(r->u_textured, 1);
    glActiveTexture(GL_TEXTURE0);
    {
        static const float off[4] = {0, 0, 0, 0};
        glUseProgram(r->prog);
        glUniform4fv(r->u_wire, 1, off);
        glUniform4fv(r->u_mono, 1, off);
    }
    if (!v->wire && !v->solo && (r->env_has[0] || r->env_has[1] || r->dos || r->dome_n > 0)) {
        /* Sky tent and ground quad, built around the camera each frame (vertex table
         * from the 3dfx engine). Sky: own projection, no depth, no fog. Ground:
         * world-anchored texture, fogged. GL z = -game z. */
        static const float SKY[16][5] = {
            {-64e6f, 2.2e6f, -64e6f, 0, 0}, {64e6f, 2.2e6f, -64e6f, 1, 0}, {76.8e6f, -0.4e6f, -76.8e6f, 1, 1}, {-76.8e6f, -0.4e6f, -76.8e6f, 0, 1},
            {-64e6f, 2.2e6f, 64e6f, 0, 0}, {64e6f, 2.2e6f, 64e6f, 1, 0}, {76.8e6f, -0.4e6f, 76.8e6f, 1, 1}, {-76.8e6f, -0.4e6f, 76.8e6f, 0, 1},
            {-64e6f, 2.2e6f, -64e6f, 0, 0}, {-64e6f, 2.2e6f, 64e6f, 1, 0}, {-76.8e6f, -0.4e6f, 76.8e6f, 1, 1}, {-76.8e6f, -0.4e6f, -76.8e6f, 0, 1},
            {64e6f, 2.2e6f, -64e6f, 0, 0}, {64e6f, 2.2e6f, 64e6f, 1, 0}, {76.8e6f, -0.4e6f, 76.8e6f, 1, 1}, {76.8e6f, -0.4e6f, -76.8e6f, 0, 1}};
        float env[(24 + 6 + 6 + 6 + 6) * FLOATS_PER_VERTEX], *f = env, sproj[16], sview[16], smvp[16], origin[3] = {0, 0, 0};
        float half = 2.0e6f, gs = r->tile_ground / 32.0e6f;  /* repeats per unit */
        float cx = eye[0], cz = eye[2];
        int q, c;
        static const int QUAD[6] = {0, 1, 2, 0, 2, 3};
        for (q = 0; q < 4; q++)
            for (c = 0; c < 6; c++) {
                const float *sv = SKY[q * 4 + QUAD[c]];
                f = env_vertex(f, sv[0], sv[1], -sv[2], 0, 0, 0, sv[3] * r->tile_sky, sv[4] * r->tile_sky * 0.2f);   /* the skirt does not drift (0x100278b0, fixed UVs) */
            }
        {   /* the sky ceiling (engine 0x10027ab0): y = 2e6, +-64e6, UVs 0..tile_sky, drift on both axes */
            static const float CX[4] = {-64e6f, 64e6f, 64e6f, -64e6f}, CZ[4] = {-64e6f, -64e6f, 64e6f, 64e6f};
            static const float CU[4] = {0, 1, 1, 0}, CV[4] = {0, 0, 1, 1};
            for (c = 0; c < 6; c++) {
                int k = QUAD[c];
                f = env_vertex(f, CX[k], 2.0e6f, -CZ[k], 0, 0, 0, CU[k] * r->tile_sky + v->sky_scroll, CV[k] * r->tile_sky + v->sky_scroll);
            }
        }
        {
            float gx[4] = {cx - half, cx + half, cx + half, cx - half}, gz[4] = {cz - half, cz - half, cz + half, cz + half};
            for (c = 0; c < 6; c++) {
                int k = QUAD[c];
                f = env_vertex(f, gx[k], 0, gz[k], 0, 1, 0, gx[k] * gs, -gz[k] * gs);
            }
        }
        {   /* the PowerVR fog veil (0x1005ED90): a plane at y 55555.55, world-fixed, here +-64e6 around the eye (vertices 36-41) */
            float vx[4] = {cx - 64e6f, cx + 64e6f, cx + 64e6f, cx - 64e6f}, vz[4] = {cz - 64e6f, cz - 64e6f, cz + 64e6f, cz + 64e6f};
            for (c = 0; c < 6; c++) { int k = QUAD[c]; f = env_vertex(f, vx[k], 55555.55f, vz[k], 0, -1, 0, 0, 0); }
        }
        {   /* a full-window quad in clip space for the DOS horizon fill (vertices 42-47) */
            static const float QX[4] = {-1, 1, 1, -1}, QY[4] = {-1, -1, 1, 1};
            for (c = 0; c < 6; c++) { int k = QUAD[c]; f = env_vertex(f, QX[k], QY[k], 0.999f, 0, 0, 0, 0, 0); }
        }
        glBindVertexArray(r->env_vao);
        glBindBuffer(GL_ARRAY_BUFFER, r->env_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof env, env, GL_STREAM_DRAW);
        {
            static const int sizes[6] = {3, 3, 2, 3, 1, 1};
            size_t off = 0;
            int a;
            for (a = 0; a < 6; a++) {
                glEnableVertexAttribArray((GLuint)a);
                glVertexAttribPointer((GLuint)a, sizes[a], GL_FLOAT, GL_FALSE, FLOATS_PER_VERTEX * sizeof(float),
                                      (const void *)(off * sizeof(float)));
                off += (size_t)sizes[a];
            }
        }
        if (r->dos) {
            /* DOS (MW2.EXE 0x3c000): no sky or ground geometry - the window is filled at the horizon, sky above (with
             * its haze band), ground below; the terrain and objects draw over it */
            static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            float Fv[3] = {r->center[0] - eye[0], r->center[1] - eye[1], r->center[2] - eye[2]}, Rv[3], Uv[3], fl, rl, fy;
            fl = sqrtf(Fv[0] * Fv[0] + Fv[1] * Fv[1] + Fv[2] * Fv[2]); Fv[0] /= fl; Fv[1] /= fl; Fv[2] /= fl;
            Rv[0] = -Fv[2]; Rv[1] = 0; Rv[2] = Fv[0];   /* F x (0, 1, 0) */
            rl = sqrtf(Rv[0] * Rv[0] + Rv[2] * Rv[2]); if (rl < 1e-6f) rl = 1; Rv[0] /= rl; Rv[2] /= rl;
            Uv[0] = Rv[1] * Fv[2] - Rv[2] * Fv[1]; Uv[1] = Rv[2] * Fv[0] - Rv[0] * Fv[2]; Uv[2] = Rv[0] * Fv[1] - Rv[1] * Fv[0];
            fy = (float)h * 0.5f / tanf(v->vfov * 3.14159265f / 360.0f);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, r->dos_tex);
            glUniform1i(r->u_dos_tab, 1);
            glActiveTexture(GL_TEXTURE0);
            glUniformMatrix4fv(r->u_mvp, 1, GL_FALSE, ident);
            glUniformMatrix4fv(r->u_view, 1, GL_FALSE, ident);
            glUniform3fv(r->u_hz_r, 1, Rv); glUniform3fv(r->u_hz_u, 1, Uv); glUniform3fv(r->u_hz_f, 1, Fv);
            glUniform4f(r->u_hz_c, (float)x0 + (float)w * 0.5f, (float)y0 + (float)h * 0.5f, fy, fy);
            glUniform1f(r->u_hz_band, v->dos_band * (float)w / 320.0f);   /* 0x14950: the height in 320-wide pixels */
            glUniform2f(r->u_hz_x, (float)x0, (float)(x0 + w));
            glUniform1i(r->u_hz_smooth, v->dos_sky_smooth);
            glUniform1i(r->u_horizon, 1);
            glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
            glDrawArrays(GL_TRIANGLES, 42, 6);
            glDepthMask(GL_TRUE); glEnable(GL_DEPTH_TEST);
            glUniform1i(r->u_horizon, 0);
        }
        if (r->env_has[1] && !r->dos) {
            /* rotation-only view so the tent stays centred on the eye */
            look_at(sview, origin, (float[3]){r->center[0] - eye[0], r->center[1] - eye[1], r->center[2] - eye[2]});
            perspective(sproj, v->vfov, (float)w / (float)(h > 0 ? h : 1), 1.0e5f, 2.0e8f);
            mat_mul(smvp, sproj, sview);
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glUniformMatrix4fv(r->u_mvp, 1, GL_FALSE, smvp);
            glUniformMatrix4fv(r->u_view, 1, GL_FALSE, sview);
            glUniform1f(r->u_fog_density, 0);
            glUniform4f(r->u_tifog, 0, 0, 0, 1);
            glUniform1f(r->u_ambient, 1);
            glBindTexture(GL_TEXTURE_2D, v->flat_sky && r->sky_flat ? r->sky_flat : r->env_tex[1]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)(v->mga_mip ? GL_NEAREST : eminf));   /* Mystique sky: one level */
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)emagf);
            glDrawArrays(GL_TRIANGLES, 0, 30);   /* walls + ceiling */
            glUniform4f(r->u_tifog, v->ortho_w > 0 ? 0.0f : v->ti_fog[0], v->ti_fog[1], 0, v->ti_fog[2]);
            glDepthMask(GL_TRUE);
            glEnable(GL_DEPTH_TEST);
            glUniform1f(r->u_ambient, v->ambient);
        }
        if (v->pvr_veil && v->fog_density >= 1e-7f && !r->dos) {
            /* the PowerVR fog veil: white, unlit, fogged by the table fog at its own depth, 40 % opaque, drawn over the
             * sky before the scene with no depth test (the terrain and mechs, all below it, overwrite it) */
            float vproj[16], vmvp[16];
            perspective(vproj, v->vfov, (float)w / (float)(h > 0 ? h : 1), 100.0f, 2.0e8f);
            mat_mul(vmvp, vproj, view);
            glUniformMatrix4fv(r->u_mvp, 1, GL_FALSE, vmvp);
            glUniformMatrix4fv(r->u_view, 1, GL_FALSE, view);
            glUniform1f(r->u_fog_density, v->ortho_w > 0 ? 0.0f : fog_k(v->fog_density));
            glUniform1i(r->u_textured, 0);
            glUniform1i(r->u_unlit, 1);
            glUniform1f(r->u_alpha, 0.4f);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
            glDrawArrays(GL_TRIANGLES, 36, 6);
            glDepthMask(GL_TRUE); glEnable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            glUniform1f(r->u_alpha, 1.0f);
            glUniform1i(r->u_unlit, r->dos);
            glUniform1i(r->u_textured, 1);
        }
        if (r->dome_n > 0 && !r->dos && v->ortho_w <= 0) {
            /* the Titanium sky dome: around the eye (its x, z; the dome's own height), no depth (the 16-bit engine has no
             * working depth test: its vertices' sz is the view depth in cm (objects, 0x10056753; the ground's / 4,
             * 0x1000180b), far outside D3D's 0..1, under ZFUNC LESSEQUAL (dx5draw 0x10002b95; a billboard's is 1.0,
             * 0x1004f117) - the order is its depth sort's (0x10057723), and the dome is drawn first,
             * 0x100557da, so behind everything), unlit, not fogged,
             * alpha-blended (the textures of type > 0: Render type 8, dx5draw 0x100044ff - SRCALPHA / INVSRCALPHA);
             * the cloud ceiling (slot 0x8c) drifts by the skyAnim offsets (mercsw 0x1000127c), not turned with the
             * heading: the sky is drawn in mode 2 (0x1009e974 = 2 at 0x100327ee), which skips that rotation (0x100012a0) */
            static float *dv;
            static int dcap;
            float dview[16], dproj[16], dmvp[16], *f;
            int k, sl;
            if (dcap < r->dome_n) { free(dv); dcap = r->dome_n; dv = malloc((size_t)dcap * FLOATS_PER_VERTEX * sizeof *dv); }
            if (dv) {
                f = dv;
                for (sl = 0; sl < GLR_SLOTS; sl++)
                    for (k = r->dome_first[sl]; k < r->dome_first[sl] + r->dome_count[sl]; k++) {
                        const float *q = r->dome_v + (size_t)k * 5;
                        float du = sl == 0x8c ? v->ti_scroll[0] : 0, dvv = sl == 0x8c ? v->ti_scroll[1] : 0;
                        f = env_vertex(f, q[0], q[1] - eye[1], q[2], 0, 0, 0, q[3] + du, q[4] + dvv);
                    }
                look_at(dview, origin, (float[3]){r->center[0] - eye[0], r->center[1] - eye[1], r->center[2] - eye[2]});
                perspective(dproj, v->vfov, (float)w / (float)(h > 0 ? h : 1), 100.0f, 2.0e8f);
                mat_mul(dmvp, dproj, dview);
                glBindVertexArray(r->dome_vao);
                glBindBuffer(GL_ARRAY_BUFFER, r->dome_vbo);
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)r->dome_n * FLOATS_PER_VERTEX * sizeof *dv), dv, GL_STREAM_DRAW);
                {
                    static const int sizes[6] = {3, 3, 2, 3, 1, 1};
                    size_t off = 0;
                    int a;
                    for (a = 0; a < 6; a++) {
                        glEnableVertexAttribArray((GLuint)a);
                        glVertexAttribPointer((GLuint)a, sizes[a], GL_FLOAT, GL_FALSE, FLOATS_PER_VERTEX * sizeof(float), (const void *)(off * sizeof(float)));
                        off += (size_t)sizes[a];
                    }
                }
                glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
                glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glUniformMatrix4fv(r->u_mvp, 1, GL_FALSE, dmvp);
                glUniformMatrix4fv(r->u_view, 1, GL_FALSE, dview);
                glUniform1f(r->u_fog_density, 0);
                glUniform4f(r->u_tifog, 0, 0, 0, 1);
                glUniform1i(r->u_unlit, 1);
                glUniform1i(r->u_blend, 1);
                /* the polygons in the record's order (mercsw 0x10055b90 draws an object's polygons in turn, no sorting; the
                 * z-buffer then keeps the dome - 14 km out, drawn after the ground, 6.4 km - behind everything: here
                 * without depth, first) */
                for (k = 0; k < r->dome_np; k++) {
                    if (r->dome_pcount[k] <= 0) continue;
                    glBindTexture(GL_TEXTURE_2D, r->dome_tex[r->dome_pslot[k]]);
                    glDrawArrays(GL_TRIANGLES, r->dome_pfirst[k], r->dome_pcount[k]);
                }
                glUniform1i(r->u_blend, 0);
                glUniform1i(r->u_unlit, r->dos);
                glUniform4f(r->u_tifog, v->ti_fog[0], v->ti_fog[1], 0, v->ti_fog[2]);
                glDisable(GL_BLEND);
                glDepthMask(GL_TRUE); glEnable(GL_DEPTH_TEST);
                glBindVertexArray(r->env_vao);
                glBindBuffer(GL_ARRAY_BUFFER, r->env_vbo);
            }
        }
        if (r->env_has[0] && !r->dos) {
            glUniformMatrix4fv(r->u_mvp, 1, GL_FALSE, mvp);
            glUniformMatrix4fv(r->u_view, 1, GL_FALSE, view);
            glUniform1f(r->u_fog_density, v->ortho_w > 0 ? 0.0f : fog_k(v->fog_density));
            glBindTexture(GL_TEXTURE_2D, v->flat_ground && r->ground_flat ? r->ground_flat : r->env_tex[0]);   /* TERRAIN TEXTURES: objects of type 0x800 only */
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)eminf);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)emagf);
            glUniform1i(r->u_mga_lod, v->mga_mip);
            if (v->ti_light) glUniform1i(r->u_tilit, 2);   /* Titanium: the ground's one colour (0x1005527e) */
            glDrawArrays(GL_TRIANGLES, 30, 6);
            glUniform1i(r->u_tilit, 0);
            glUniform1i(r->u_mga_lod, 0);
        }
        glBindVertexArray(0);
    }
    glUniformMatrix4fv(r->u_mvp, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(r->u_view, 1, GL_FALSE, view);
    glUniform3fv(r->u_light, 1, L);
    glUniform1i(r->u_point, v->point_light);
    glUniform1f(r->u_ambient, v->ambient);
    glUniform1f(r->u_fog_density, v->ortho_w > 0 ? 0.0f : fog_k(v->fog_density));
    glUniform3fv(r->u_fog_color, 1, v->fog_color);
    glUniform1f(r->u_far, v->ortho_w > 0 ? 0.0f : v->far_cull);   /* the world and actor layers carry their spheres */
    glUniform2f(r->u_vp, (float)w, (float)h);                     /* the billboards' screen squares */
    glUniform2f(r->u_vpo, (float)x0, (float)y0);                  /* (and the DOS Gouraud fill's polygon) */
    glUniform1f(r->u_pfade, r->dos ? 0.0f : r->pfade);
    glUniform1i(r->u_notex, (v->notex_actors ? 1 : 0) | (v->notex_world ? 2 : 0));
    if (r->pfade > 0 && !r->dos) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, r->ptab_tex);
        glUniform1i(r->u_ptab, 2);
        glActiveTexture(GL_TEXTURE0);
    }
    glUniform1i(r->u_tex, 0);
    glUniform1i(r->u_unlit, 0);
    if (r->dos) {   /* the DOS edition's shading: the LUMA / palette table on unit 1 */
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, r->dos_tex);
        glUniform1i(r->u_dos_tab, 1);
        glUniform1i(r->u_dos, v->wire ? 0 : 1);
        /* an explosion takes the light (0x51060: moved to the blast, the ambient 10 lower) */
        if (v->point_light) glUniform3fv(r->u_dos_lpos, 1, v->light_pos);   /* (already in GL coordinates, as u_lpos) */
        else glUniform3f(r->u_dos_lpos, v->dos_lpos[0], v->dos_lpos[1], -v->dos_lpos[2]);
        glUniform1f(r->u_dos_amb, v->point_light ? v->dos_amb - 10.0f : v->dos_amb);
        glUniform1f(r->u_dos_fogdist, v->dos_fogdist);
        glUniform1i(r->u_dos_dir, v->point_light ? 0 : v->dos_dir);
        glUniform1i(r->u_dos_smooth, v->dos_sky_smooth);   /* the port's dos_shading=smooth */
        glUniform1f(r->u_fog_density, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(r->u_tilit, v->ti_light && !r->dos ? 1 : 0);   /* Titanium: the objects' lights (0x1002ecaa) */
    for (int li = 0; li < GLR_LAYERS; li++) {
    const glr_layer *L = &r->layer[li];
    if (r->ti_slots && !r->dos) ti_colours(r, li, v, view, eye);   /* the Titanium per-vertex colours */
    if (v->solo && li == 0) continue;
    {   static const float off0[4] = {0, 0, 0, 0}; glUniform4fv(r->u_mono, 1, li == 1 ? v->mono : off0); }
    {   /* image enhancement: lines, world red, actors (mechs, cockpit) blue (DOS w) */
        static const float wr[4] = {60.0f / 255.0f, 0, 0, 1}, wb[4] = {0, 48.0f / 255.0f, 215.0f / 255.0f, 1}, off[4] = {0, 0, 0, 0};   /* DOS capture: world (60,0,0), mechs and cockpit (0,48,215) */
        glUniform4fv(r->u_wire, 1, v->wire ? (li == 0 ? wr : wb) : off);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);   /* the wireframe's edges come from the fragment shader */
    }
    glBindVertexArray(L->vao);
    if (r->dos) { glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_BUFFER, L->tbo); glActiveTexture(GL_TEXTURE0); }
    for (s = 0; s <= GLR_SLOTS; s++) {
        if (L->batch_count[s] <= 0) continue;
        if (s < GLR_SLOTS) {
            glBindTexture(GL_TEXTURE_2D, r->tex[s]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)minf);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)magf);
            glUniform2f(r->u_texsize, (float)r->tex_w[s], (float)r->tex_h[s]);
            glUniform1i(r->u_textured, 1);   /* Combat Variables textures: per object class (u_notex) */
        } else {
            glUniform1i(r->u_textured, 0);
        }
        glDrawArrays(GL_TRIANGLES, L->batch_first[s], L->batch_count[s]);
    }
    for (s = 0; s < GLR_SLOTS; s++) {   /* the Titanium models' textures */
        if (L->batch_count[GLR_TI + s] <= 0) continue;
        glBindTexture(GL_TEXTURE_2D, r->tex[2 * GLR_SLOTS + s]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)minf);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)magf);
        glUniform2f(r->u_texsize, (float)r->tex_w[2 * GLR_SLOTS + s], (float)r->tex_h[2 * GLR_SLOTS + s]);
        glUniform1i(r->u_textured, 1);   /* per object class (u_notex, v_texsel = -class): mercsw keeps the bitmap of an
                                          * object whose type is not in the switch mask (0x1001b4fc) */
        glDrawArrays(GL_TRIANGLES, L->batch_first[GLR_TI + s], L->batch_count[GLR_TI + s]);
    }
    for (s = 0; s < GLR_SLOTS; s++) {   /* bank-0 billboards (colour type 3: scrub rocks, cacti), alpha-blended: the 3Dfx
                                         * engine draws them with flag 2 - SRC_ALPHA / ONE_MINUS_SRC_ALPHA, alpha test
                                         * ALWAYS, depth writes left on (0x10067240 with DAT_10265714 = 1); the ARGB1555
                                         * archives' one-bit alpha gives the same picture as a key */
        int blend = !v->wire;
        if (L->batch_count[GLR_B0 + s] <= 0) continue;
        glBindTexture(GL_TEXTURE_2D, r->tex[GLR_SLOTS + s]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)minf);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)magf);
        glUniform2f(r->u_texsize, (float)r->tex_w[GLR_SLOTS + s], (float)r->tex_h[GLR_SLOTS + s]);
        glUniform1i(r->u_textured, 1);
        if (blend) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glUniform1i(r->u_blend, 1); }
        glDrawArrays(GL_TRIANGLES, L->batch_first[GLR_B0 + s], L->batch_count[GLR_B0 + s]);
        if (blend) { glDisable(GL_BLEND); glUniform1i(r->u_blend, 0); }
    }
    for (s = 0; s < GLR_SLOTS; s++) {   /* Titanium billboards (0x1004ef4c): the bitmap x the vertex colour 0xc8 (200 / 255),
                                         * Render type 8 (alpha-blended) for a texture of type > 0, else type 2 (0x1000131e);
                                         * the vertices' diffuse alpha 0x80 (0x1004f364) is not used: dx5draw never sets
                                         * TEXTUREMAPBLEND (states set: 2-9, 0x11-0x14, 0x16, 0x17, 0x1a-0x1d, 0x22, 0x29;
                                         * 0x10002b60, 0x100034xx-0x1000373b), so D3D's default MODULATE takes the alpha
                                         * from a texture that has one (the type > 0 ARGB4444) and the type 2 draw does
                                         * not blend */
        const texture *bt = r->ti_bank0 ? r->ti_bank0[s] : NULL;
        int blend = bt && bt->type > 0 && !v->wire;
        if (L->batch_count[GLR_TB + s] <= 0) continue;
        glBindTexture(GL_TEXTURE_2D, r->tex[3 * GLR_SLOTS + s]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)minf);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)magf);
        glUniform2f(r->u_texsize, (float)r->tex_w[3 * GLR_SLOTS + s], (float)r->tex_h[3 * GLR_SLOTS + s]);
        glUniform1i(r->u_textured, !(li == 0 ? v->notex_world : v->notex_actors));
        glUniform3f(r->u_tint, 200.0f / 255.0f, 200.0f / 255.0f, 200.0f / 255.0f);
        if (blend) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_FALSE); glUniform1i(r->u_blend, 1); }
        glDrawArrays(GL_TRIANGLES, L->batch_first[GLR_TB + s], L->batch_count[GLR_TB + s]);
        if (blend) { glDisable(GL_BLEND); glDepthMask(GL_TRUE); glUniform1i(r->u_blend, 0); }
        if (r->tint_on && !r->dos) glUniform3fv(r->u_tint, 1, r->tint); else glUniform3f(r->u_tint, 1, 1, 1);
    }
    if (L->batch_count[GLR_SLOTS + 2] > 0) {   /* unlit plain colours (type 0: the laser bolts) */
        glUniform1i(r->u_textured, 0);
        glUniform1i(r->u_unlit, 1);
        glDrawArrays(GL_TRIANGLES, L->batch_first[GLR_SLOTS + 2], L->batch_count[GLR_SLOTS + 2]);
        glUniform1i(r->u_unlit, 0);
    }
    if (L->batch_count[GLR_SLOTS + 1] > 0 && !v->wire) {   /* shadows: the ground darkened, not painted over (3D editions'
                                                         * flat shadow pieces - the dropship's, the buildings') */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -4.0f);
        glUniform1i(r->u_shadow, 1);
        glDrawArrays(GL_TRIANGLES, L->batch_first[GLR_SLOTS + 1], L->batch_count[GLR_SLOTS + 1]);
        glUniform1i(r->u_shadow, 0);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
    }
    glBindVertexArray(0);
    glUniform1i(r->u_dos, 0);
    { static const float off0[4] = {0, 0, 0, 0}; glUniform4fv(r->u_mono, 1, off0); }
    glUniform1i(r->u_tilit, 0);
    if (r->par_n > 0 && !v->solo && !v->wire && v->ortho_w <= 0) {
        /* the Titanium particles: each a square in clip space from its projected point, size_px x (height / 480) pixels
         * right and down (0x1004da51..0x1004db20), in runs of one texture */
        static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        static float *pb;
        static int pcap;
        static const int sizes[6] = {3, 3, 2, 3, 1, 1};
        float *f;
        int k, a, off = 0, first = 0, nv = 0;
        if (pcap < r->par_n) { free(pb); pcap = r->par_n; pb = malloc((size_t)pcap * 6 * FLOATS_PER_VERTEX * sizeof *pb); }
        if (pb) {
            f = pb;
            for (k = 0; k < r->par_n; k++) {
                const glr_particle *pp = &r->par[k];
                float x = pp->pos[0], y = pp->pos[1], z = -pp->pos[2], cx, cy, cz, cw, sx, sy;
                cx = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
                cy = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
                cz = mvp[2] * x + mvp[6] * y + mvp[10] * z + mvp[14];
                cw = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
                if (cw <= 1.0f) { pb[k * 6 * FLOATS_PER_VERTEX] = 1e9f; f += 6 * FLOATS_PER_VERTEX; continue; }   /* (behind: skipped below) */
                cx /= cw; cy /= cw; cz /= cw;
                sx = 2.0f * pp->size_px * ((float)h / 480.0f) / (float)(w > 0 ? w : 1);
                sy = 2.0f * pp->size_px * ((float)h / 480.0f) / (float)(h > 0 ? h : 1);
                f = env_vertex(f, cx, cy, cz, 0, 0, 0, 0, 0);
                f = env_vertex(f, cx + sx, cy, cz, 0, 0, 0, 1, 0);
                f = env_vertex(f, cx + sx, cy - sy, cz, 0, 0, 0, 1, 1);
                f = env_vertex(f, cx, cy, cz, 0, 0, 0, 0, 0);
                f = env_vertex(f, cx + sx, cy - sy, cz, 0, 0, 0, 1, 1);
                f = env_vertex(f, cx, cy - sy, cz, 0, 0, 0, 0, 1);
            }
            nv = (int)((f - pb) / FLOATS_PER_VERTEX);
            glBindVertexArray(r->spr_vao);
            glBindBuffer(GL_ARRAY_BUFFER, r->spr_vbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)nv * FLOATS_PER_VERTEX * sizeof *pb), pb, GL_STREAM_DRAW);
            for (a = 0; a < 6; a++) {
                glEnableVertexAttribArray((GLuint)a);
                glVertexAttribPointer((GLuint)a, sizes[a], GL_FLOAT, GL_FALSE, FLOATS_PER_VERTEX * sizeof(float), (void *)(size_t)(off * sizeof(float)));
                off += sizes[a];
            }
            glUniformMatrix4fv(r->u_mvp, 1, GL_FALSE, ident);
            glUniformMatrix4fv(r->u_view, 1, GL_FALSE, ident);
            glUniform1f(r->u_far, 0);
            glUniform1f(r->u_fog_density, 0);
            glUniform4f(r->u_tifog, 0, 0, 0, 1);
            glUniform1i(r->u_unlit, 1);
            glUniform1i(r->u_textured, 1);
            glUniform1i(r->u_blend, 1);
            glUniform1f(r->u_alpha, r->par_alpha);
            glUniform2f(r->u_texsize, 1, 1);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            for (k = 0; k <= r->par_n; k++) {   /* runs of one texture */
                if (k < r->par_n && k > first && r->par[k].tex == r->par[first].tex) continue;
                if (k > first) {
                    int q;
                    glBindTexture(GL_TEXTURE_2D, sprite_texture(r, r->par[first].tex));
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)minf);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)magf);
                    for (q = first; q < k; q++)
                        if (pb[q * 6 * FLOATS_PER_VERTEX] < 1e8f) glDrawArrays(GL_TRIANGLES, q * 6, 6);
                }
                first = k;
            }
            glDepthMask(GL_TRUE); glDisable(GL_BLEND);
            glUniform1i(r->u_blend, 0);
            glUniform1f(r->u_alpha, 1.0f);
            glUniform1i(r->u_unlit, 0);
            glUniformMatrix4fv(r->u_mvp, 1, GL_FALSE, mvp);
            glUniformMatrix4fv(r->u_view, 1, GL_FALSE, view);
            glUniform1f(r->u_fog_density, v->ortho_w > 0 ? 0.0f : fog_k(v->fog_density));
            glUniform4f(r->u_tifog, v->ortho_w > 0 ? 0.0f : v->ti_fog[0], v->ti_fog[1], 0, v->ti_fog[2]);
            glUniform1f(r->u_far, v->ortho_w > 0 ? 0.0f : v->far_cull);
            glBindVertexArray(0);
        }
    }
    if (r->spr_n > 0 && !v->solo) {   /* weapon effects: camera-facing cards, unlit, alpha-tested, fogged */
        static float buf[GLR_MAX_SPRITES * 6 * FLOATS_PER_VERTEX];
        static const int sizes[6] = {3, 3, 2, 3, 1, 1};
        float right[3] = {view[0], view[4], view[8]}, up[3] = {view[1], view[5], view[9]};
        float *f = buf;
        int k, a, off = 0;
        for (k = 0; k < r->spr_n; k++) {
            const glr_sprite *sp = &r->spr[k];
            float c[3] = {sp->pos[0], sp->pos[1], -sp->pos[2]}, q[4][3];
            float tw = (float)sp->tex->w, th = (float)sp->tex->h;
            int j;
            if (v->wire) {   /* image enhancement: the card's outline in its type's colour, its inside black (hidden lines) */
                int wc = sp->wire_code ? sp->wire_code : 4;
                if (sp->quad) {   /* the card's own quad */
                    static const int QK[6] = {0, 1, 2, 0, 2, 3}, CORNER[6] = {0, 1, 2, 0, 1, 2}, MASK[6] = {5, 5, 5, 3, 3, 3};
                    for (j = 0; j < 6; j++) {
                        f = env_vertex(f, sp->qv[QK[j]][0], sp->qv[QK[j]][1], -sp->qv[QK[j]][2], 0, 0, 0, 0, 0);
                        f[-1] = (float)(CORNER[j] + 3 * MASK[j] + 32 * wc);
                    }
                    continue;
                }
                if (sp->has_tri) {
                    for (j = 0; j < 6; j++) {
                        const float *t = sp->tri[j < 3 ? j : 2];
                        f = env_vertex(f, t[0], t[1], -t[2], 0, 0, 0, 0, 0);
                        f[-1] = (float)((j < 3 ? j : 2) + 3 * 7 + 32 * wc);
                    }
                    continue;
                }
                for (j = 0; j < 3; j++) {   /* the rectangle (a billboard's: about its foot-top edge) */
                    float cx = sp->bb ? 0.5f * (sp->pos[j] + sp->top[j]) * (j == 2 ? -1.0f : 1.0f) : c[j];
                    float hh = sp->bb ? 0.5f * sqrtf((sp->top[0] - sp->pos[0]) * (sp->top[0] - sp->pos[0]) + (sp->top[1] - sp->pos[1]) * (sp->top[1] - sp->pos[1]) + (sp->top[2] - sp->pos[2]) * (sp->top[2] - sp->pos[2])) : 0;
                    float hw = sp->bb ? hh : sp->half_w, hv = sp->bb ? hh : sp->half_h;
                    q[0][j] = cx - right[j] * hw - up[j] * hv;
                    q[1][j] = cx + right[j] * hw - up[j] * hv;
                    q[2][j] = cx + right[j] * hw + up[j] * hv;
                    q[3][j] = cx - right[j] * hw + up[j] * hv;
                }
                {
                    static const int QK[6] = {0, 1, 2, 0, 2, 3}, CORNER[6] = {0, 1, 2, 0, 1, 2}, MASK[6] = {5, 5, 5, 3, 3, 3};
                    for (j = 0; j < 6; j++) {
                        f = env_vertex(f, q[QK[j]][0], q[QK[j]][1], q[QK[j]][2], 0, 0, 0, 0, 0);
                        f[-1] = (float)(CORNER[j] + 3 * MASK[j] + 32 * wc);
                    }
                }
                continue;
            }
            for (j = 0; j < 3; j++) {
                q[0][j] = c[j] - right[j] * sp->half_w - up[j] * sp->half_h;
                q[1][j] = c[j] + right[j] * sp->half_w - up[j] * sp->half_h;
                q[2][j] = c[j] + right[j] * sp->half_w + up[j] * sp->half_h;
                q[3][j] = c[j] - right[j] * sp->half_w + up[j] * sp->half_h;
            }
            if (sp->quad) {   /* a card in its own orientation (0x1002c880): two triangles of its corners */
                static const int QK[6] = {0, 1, 2, 0, 2, 3};
                for (j = 0; j < 6; j++) {
                    const int qk = QK[j];
                    f = env_vertex(f, sp->qv[qk][0], sp->qv[qk][1], -sp->qv[qk][2], 0, 0, 0, sp->quv[qk][0] * tw, sp->quv[qk][1] * th);
                }
                continue;
            }
            if (sp->bb) {   /* the engine billboard (the vertex shader's edge code 1024): foot, top, corner */
                static const int BBK[6] = {0, 1, 2, 0, 2, 3}, CORNER[6] = {0, 1, 2, 0, 1, 2}, MASK[6] = {5, 5, 5, 3, 3, 3};
                for (j = 0; j < 6; j++) {
                    int kc = BBK[j];
                    f = env_vertex(f, sp->pos[0], sp->pos[1], -sp->pos[2], sp->top[0], sp->top[1], -sp->top[2],
                                   (kc == 1 || kc == 2) ? tw : 0.0f, kc >= 2 ? th : 0.0f);
                    if (sp->ti) { f[-7] = kc >= 2 ? tw : 0.0f; f[-6] = (kc == 1 || kc == 2) ? th : 0.0f; }   /* Titanium 0x1009dfe0: (0,0) (0,1) (1,1) (1,0) */
                    f[-2] = (float)(kc + (sp->ti ? 4 : 0));   /* (+ 4: the Titanium corners, VS) */
                    f[-1] = (float)(1024 + CORNER[j] + 3 * MASK[j]);
                }
                continue;
            }
            f = env_vertex(f, q[0][0], q[0][1], q[0][2], 0, 0, 0, 0, th);
            f = env_vertex(f, q[1][0], q[1][1], q[1][2], 0, 0, 0, tw, th);
            f = env_vertex(f, q[2][0], q[2][1], q[2][2], 0, 0, 0, tw, 0);
            f = env_vertex(f, q[0][0], q[0][1], q[0][2], 0, 0, 0, 0, th);
            f = env_vertex(f, q[2][0], q[2][1], q[2][2], 0, 0, 0, tw, 0);
            f = env_vertex(f, q[3][0], q[3][1], q[3][2], 0, 0, 0, 0, 0);
        }
        glBindVertexArray(r->spr_vao);
        glBindBuffer(GL_ARRAY_BUFFER, r->spr_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)(f - buf) * sizeof *buf), buf, GL_STREAM_DRAW);
        for (a = 0; a < 6; a++) {
            glEnableVertexAttribArray((GLuint)a);
            glVertexAttribPointer((GLuint)a, sizes[a], GL_FLOAT, GL_FALSE, FLOATS_PER_VERTEX * sizeof(float), (void *)(size_t)(off * sizeof(float)));
            off += sizes[a];
        }
        glUniform1i(r->u_unlit, 1);
        glUniform1i(r->u_textured, 1);
        if (v->wire) { static const float dr[4] = {60.0f / 255.0f, 0, 0, 1}; glUniform4fv(r->u_wire, 1, dr); }   /* the edges' colour codes decide */
        if (v->fx_blend) {   /* Titanium: Render type 8, SRCALPHA / INVSRCALPHA (dx5draw 0x100044ff, 0x100035ab) */
            glUniform1i(r->u_blend, 1);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }
        for (k = 0; k < r->spr_n; k++) {
            glBindTexture(GL_TEXTURE_2D, sprite_texture(r, r->spr[k].tex));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)minf);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)magf);
            glUniform2f(r->u_texsize, (float)r->spr[k].tex->w, (float)r->spr[k].tex->h);
            if (r->spr[k].ti) glUniform3f(r->u_tint, 200.0f / 255.0f, 200.0f / 255.0f, 200.0f / 255.0f);   /* mercsw 0x1004f33a: diffuse 0xc8 */
            glDrawArrays(GL_TRIANGLES, k * 6, 6);
            if (r->spr[k].ti) { if (r->tint_on && !r->dos) glUniform3fv(r->u_tint, 1, r->tint); else glUniform3f(r->u_tint, 1, 1, 1); }
        }
        if (v->fx_blend) { glUniform1i(r->u_blend, 0); glDisable(GL_BLEND); glDepthMask(GL_TRUE); }
        glUniform1i(r->u_unlit, 0);
        glBindVertexArray(0);
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_SCISSOR_TEST);
}
