/* glr.c - see glr.h. OpenGL 3.3 core only. */
#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
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
#define GLR_BATCHES (GLR_B0 + GLR_SLOTS)   /* 0: main mech or static world, 1: actors (re-uploaded per frame) */

typedef struct {
    GLuint  vao, vbo;
    GLuint  sph_vbo;   /* per vertex: its object's bounding sphere (GL centre, radius) for the view-distance cull */
    GLint   batch_first[GLR_BATCHES];   /* + GLR_SLOTS: untextured; + 1: shadows; + 2: unlit plain colours (type 0); GLR_B0 + n bank 0 */
    GLsizei batch_count[GLR_BATCHES];
    GLsizei vertex_count;
} glr_layer;

struct glr {
    GLuint prog, tex[GLR_SLOTS * 2];
    texture *const *bank0;                  /* glr_set_bank0 (the caller's array) */
    glr_layer layer[GLR_LAYERS];
    GLuint env_vao, env_vbo, env_tex[2];   /* 0 ground, 1 sky */
    GLuint ground_flat;                    /* 1x1: the ground texture's average colour (terrain textures off) */
    GLuint sky_flat;                       /* 1x1: the sky texture's average colour (Textured Sky off) */
    int    env_has[2];
    float  tile_sky, tile_ground;
    int    tex_w[GLR_SLOTS * 2], tex_h[GLR_SLOTS * 2], has_tex[GLR_SLOTS * 2];
    const texture *tex_src[GLR_SLOTS * 2];  /* skip re-uploading the same texture */

    float  center[3], radius;
    GLint  u_point, u_lpos;
    GLint  u_mono, u_mvp, u_view, u_light, u_ambient, u_fog_density, u_fog_color, u_texsize, u_tex, u_textured, u_unlit, u_wire, u_shadow;
    /* the DOS edition */
    int    dos;
    GLuint dos_tex;                         /* 256 x 17: rows 0-15 the LUMA table (as indices), row 16 the palette (RGB) */
    GLint  u_dos, u_dos_tab, u_dos_lpos, u_dos_amb, u_dos_fogdist, u_dos_dir;
    GLint  u_far;
    GLint  u_alpha, u_mga_lod, u_horizon, u_hz_r, u_hz_u, u_hz_f, u_hz_c, u_hz_band;
    uint8_t dos_img[17][256][4];
    /* sprites (weapon effects) */
    GLuint spr_vao, spr_vbo;
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
    "uniform mat4 u_mvp, u_view;\n"
    "uniform float u_far;\n"
    "out vec3 v_normal; out vec2 v_uv; out vec3 v_color; flat out int v_texsel; out vec3 v_viewpos; out vec3 v_world;\n"
    "out vec3 v_bary; flat out int v_emask;\n"
    "void main() {\n"
    "  int e = int(a_edge + 0.5);\n"
    /* a colour type 3 billboard (edge code + 1024; engine 0x1002a8e0): a_pos is the sprite's foot, a_normal.xy the
     * corner's offset - across along the camera's right, up along the world vertical - and a_texsel the (horizontal)
     * normal's angle of the stored vertical triangle, which the engine lights the card by (0x10026e80's intensity) */
    "  vec3 p = a_pos, nrm = a_normal;\n"
    "  if (e >= 1024) { e -= 1024; p = a_pos + vec3(u_view[0][0], u_view[1][0], u_view[2][0]) * a_normal.x + vec3(0.0, a_normal.y, 0.0); nrm = vec3(cos(a_texsel), 0.0, sin(a_texsel)); }\n"
    "  gl_Position = u_mvp * vec4(p, 1.0);\n"
    /* the view distance (the planet's VIEW far; DOS MW2.EXE 0x3f500, 3Dfx 0x1002fba0): an object whose bounding sphere
     * lies wholly beyond it - centre distance > far + r, or depth - r > far - is not drawn (all its vertices off screen) */
    "  if (u_far > 0.0 && a_sphere.w > 0.0) {\n"
    "    vec3 sc = (u_view * vec4(a_sphere.xyz, 1.0)).xyz;\n"
    "    if (dot(sc, sc) > (u_far + a_sphere.w) * (u_far + a_sphere.w) || -sc.z - a_sphere.w > u_far) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);\n"
    "  }\n"
    "  v_viewpos = (u_view * vec4(p, 1.0)).xyz; v_world = p;\n"
    "  v_normal = nrm; v_uv = a_uv; v_color = a_color; v_texsel = int(a_texsel + 0.5);\n"
    "  int hi = e / 32; e -= hi * 32; int corner = e - 3 * (e / 3);\n"
    "  v_bary = vec3(corner == 0 ? 1.0 : 0.0, corner == 1 ? 1.0 : 0.0, corner == 2 ? 1.0 : 0.0); v_emask = (e / 3) | (hi << 3);\n"
    "}\n";

static const char *FS =
    "#version 330 core\n"
    "in vec3 v_normal; in vec2 v_uv; in vec3 v_color; flat in int v_texsel; in vec3 v_viewpos; in vec3 v_world;\n"
    "in vec3 v_bary; flat in int v_emask;\n"
    "uniform int u_point; uniform vec3 u_lpos;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_texsize;\n"
    "uniform int u_textured, u_unlit;\n"
    "uniform vec4 u_wire, u_mono;\n"
    "uniform int u_shadow;\n"
    "uniform vec3 u_light, u_fog_color;\n"
    "uniform float u_ambient, u_fog_density;\n"
    "uniform int u_dos, u_dos_dir; uniform sampler2D u_dos_tab; uniform vec3 u_dos_lpos; uniform float u_dos_amb, u_dos_fogdist;\n"
    "uniform float u_alpha; uniform int u_mga_lod;\n"
    /* the DOS horizon fill (MW2.EXE 0x3c000): a pixel is sky when its view ray's world y >= 0 - ray = R (sx - cx) / fx +
     * U (cy - sy) / fy + F; above the line a haze band of u_hz_band pixels fades from 0xEE (on the line) to 0xE0 */
    "uniform int u_horizon; uniform vec3 u_hz_r, u_hz_u, u_hz_f; uniform vec4 u_hz_c; uniform float u_hz_band;\n"
    "out vec4 o_color;\n"
    /* the DOS edition (MW2.EXE 0x38ae3 / 0x3e086 / 0x38ee0): v_color = (kind, base index, intensity C); kind 0 unlit
     * (base is the index), 1 flat lit (base = ramp x 16), 2 textured (the texel's index through LUMA). The shade level =
     * ((C >> 1) x ((f x (128 - amb) >> 7) + amb)) / 0x440, f = 127 x cos(light, normal), less one per fog distance of
     * depth, clamped 0-15 */
    "vec3 dos_colour(float idx) { return texture(u_dos_tab, vec2((idx + 0.5) / 256.0, 16.5 / 17.0)).rgb; }\n"
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
    "    if (u_hz_band > 0.0 && abs(u_hz_u.y) > 1e-6) {\n"
    "      float sh = u_hz_c.y - u_hz_c.w * ((sx - u_hz_c.x) * u_hz_r.y / u_hz_c.z + u_hz_f.y) / u_hz_u.y;\n"   /* the line's row here */
    "      float d = sy - sh;\n"
    "      if (d >= 0.0 && d < u_hz_band) idx = floor(238.0 - 14.0 * d / u_hz_band);\n"
    "    }\n"
    "    o_color = vec4(dos_colour(idx), 1.0); return;\n"
    "  }\n"
    "  if (u_dos != 0) {\n"
    "    int kind = int(v_color.r + 0.5); float base = floor(v_color.g + 0.5), C = floor(v_color.b + 0.5);\n"
    "    if (kind == 0) { o_color = vec4(dos_colour(base), 1.0); return; }\n"
    "    float nl = length(v_normal); vec3 Ld = u_dos_dir != 0 ? normalize(u_dos_lpos) : normalize(u_dos_lpos - v_world);\n"
    "    float f = nl > 0.0 ? floor(127.0 * abs(dot(v_normal / nl, Ld))) : 127.0;\n"   /* two-sided: winding unknown */
    "    float lev = floor(floor(C / 2.0) * (floor(f * (128.0 - u_dos_amb) / 128.0) + u_dos_amb) / 1088.0);\n"
    "    if (u_dos_fogdist > 0.0) lev -= floor(max(-v_viewpos.z, 0.0) / u_dos_fogdist);\n"
    "    lev = clamp(lev, 0.0, 15.0);\n"
    "    if (kind == 1) { o_color = vec4(dos_colour(base + lev), 1.0); return; }\n"
    /* Gouraud (0x3bb80): each vertex's baked index a = ramp | i, rescaled by the polygon's level: ramp + i (L + 1) / 16,
     * interpolated across the polygon (i in v_uv.x) */
    "    if (kind == 3) { o_color = vec4(dos_colour(base + floor(v_uv.x * (lev + 1.0) / 16.0)), 1.0); return; }\n"
    "    vec4 tx = texture(u_tex, v_uv / u_texsize); if (tx.a < 0.5) discard;\n"
    "    float ti = floor(tx.r * 255.0 + 0.5);\n"
    "    float si = lev >= 15.0 ? ti : floor(texture(u_dos_tab, vec2((ti + 0.5) / 256.0, (lev + 0.5) / 17.0)).r * 255.0 + 0.5);\n"
    "    o_color = vec4(dos_colour(si), 1.0); return;\n"
    "  }\n"
    "  vec3 base = u_mono.a > 1.5 ? v_color : u_mono.a > 0.0 ? u_mono.rgb : v_color;\n"
    "  if (u_textured != 0 && u_mono.a <= 0.0) {\n"
    "    vec4 t;\n"
    "    if (u_mga_lod != 0) { float z = -v_viewpos.z; t = textureLod(u_tex, v_uv / u_texsize, z < 2500.0 ? 0.0 : z < 5000.0 ? 1.0 : z < 15000.0 ? 2.0 : 3.0); }\n"
    "    else t = texture(u_tex, v_uv / u_texsize);\n"
    "    if (t.a < 0.5) discard; base = t.rgb; }\n"
    "  float n = length(v_normal);\n"
    "  vec3 L = u_point != 0 ? normalize(u_lpos - v_world) : u_light;\n"
    "  float diff = n > 0.0 ? abs(dot(v_normal / n, L)) : 0.5;\n"   /* two-sided: winding unknown */
    "  vec3 lit = u_unlit != 0 ? base : base * min(1.0, u_ambient + diff);\n"   /* SGL: ambient + parallel light, saturating */
    /* PowerVR PCX2 table fog (Imagination's Series 1 driver source, MIT: dlcamera.c sgl_set_fog, rncamera.c, simulat3
     * hwsabren.c Fog / texas.c): per pixel, from the depth 1/w: index i = depth >> shift = K / z (K below, z the view depth
     * in game units), the 8-bit fog f = PowerTable[i & 127] >> min(i >> 7, 9) with PowerTable[k] = 256 x 2^(-k/128)
     * (entry 0 = 255), so f = 0 from i = 1152 (near) up to 255 far; colour += (fog - colour) x f / 256 */
    "  float fz = max(-v_viewpos.z, 1.0);\n"
    "  float fi = u_fog_density > 0.0 ? floor(u_fog_density / fz) : 1.0e9;\n"
    "  float fk = mod(fi, 128.0), fs = floor(fi / 128.0);\n"
    "  float ft = fk == 0.0 ? 255.0 : floor(256.0 * exp2(-fk / 128.0));\n"
    "  float ff = fs >= 9.0 ? 0.0 : floor(ft / exp2(fs)) / 256.0;\n"
    "  o_color = vec4(lit + (u_fog_color - lit) * ff, u_alpha);\n"
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
            glGenBuffers(1, &r->layer[l].sph_vbo);
        }
    }
    glGenTextures(GLR_SLOTS * 2, r->tex);
    glGenVertexArrays(1, &r->env_vao);
    glGenBuffers(1, &r->env_vbo);
    glGenTextures(2, r->env_tex);
    glGenVertexArrays(1, &r->spr_vao);
    glGenBuffers(1, &r->spr_vbo);
    glGenTextures(64, r->spr_tex);
    r->u_far = glGetUniformLocation(r->prog, "u_far");
    glVertexAttrib4f(6, 0, 0, 0, 0);   /* the sky, ground and sprites: no sphere, never culled */
    r->u_unlit = glGetUniformLocation(r->prog, "u_unlit");
    r->u_wire = glGetUniformLocation(r->prog, "u_wire");
    r->u_shadow = glGetUniformLocation(r->prog, "u_shadow");
    r->u_dos = glGetUniformLocation(r->prog, "u_dos");
    r->u_dos_tab = glGetUniformLocation(r->prog, "u_dos_tab");
    r->u_dos_lpos = glGetUniformLocation(r->prog, "u_dos_lpos");
    r->u_dos_amb = glGetUniformLocation(r->prog, "u_dos_amb");
    r->u_dos_fogdist = glGetUniformLocation(r->prog, "u_dos_fogdist");
    r->u_dos_dir = glGetUniformLocation(r->prog, "u_dos_dir");
    r->u_alpha = glGetUniformLocation(r->prog, "u_alpha");
    r->u_mga_lod = glGetUniformLocation(r->prog, "u_mga_lod");
    r->u_horizon = glGetUniformLocation(r->prog, "u_horizon");
    r->u_hz_r = glGetUniformLocation(r->prog, "u_hz_r");
    r->u_hz_u = glGetUniformLocation(r->prog, "u_hz_u");
    r->u_hz_f = glGetUniformLocation(r->prog, "u_hz_f");
    r->u_hz_c = glGetUniformLocation(r->prog, "u_hz_c");
    r->u_hz_band = glGetUniformLocation(r->prog, "u_hz_band");
    glGenTextures(1, &r->dos_tex);
    return r;
}

void glr_destroy(glr *r)
{
    if (!r) return;
    glDeleteTextures(GLR_SLOTS * 2, r->tex);
    glDeleteTextures(2, r->env_tex);
    glDeleteTextures(1, &r->dos_tex);
    glDeleteBuffers(1, &r->env_vbo);
    glDeleteVertexArrays(1, &r->env_vao);
    glDeleteBuffers(1, &r->spr_vbo);
    glDeleteVertexArrays(1, &r->spr_vao);
    glDeleteTextures(64, r->spr_tex);
    {
        int l;
        for (l = 0; l < GLR_LAYERS; l++) {
            glDeleteBuffers(1, &r->layer[l].vbo);
            glDeleteBuffers(1, &r->layer[l].sph_vbo);
            glDeleteVertexArrays(1, &r->layer[l].vao);
        }
    }
    glDeleteProgram(r->prog);
    free(r);
}

void glr_flush_textures(glr *r)
{
    if (r) { memset(r->tex_src, 0, sizeof r->tex_src); memset(r->tex_w, 0, sizeof r->tex_w); }
}

static int g_billboards;   /* TEST ONLY: colour type 3 billboards in the last static (world) layer */

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

static int set_layer(glr *r, int li, const mech3d *m, const uint8_t palette[256][3],
                     texture *const slots[GLR_SLOTS], int camo, int clan, int bounds)
{
    glr_layer *L = &r->layer[li];
    size_t cap = 0, n = 0, *count, *pos;
    float *buf = NULL, *sph = NULL, psph[4] = {0, 0, 0, 0}, mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    int pi, k, j, a, pass, b;

    for (pi = 0; pi < m->part_count; pi++) {
        const wtb_object *o = &m->parts[pi].model.objects[0];
        for (k = 0; k < o->poly_count; k++)
            if (o->polys[k].n >= 3) cap += (size_t)(o->polys[k].n - 2) * 3 + (((o->polys[k].color >> 12) & 7) == 3 ? 6 : 0);
    }
    buf = malloc((cap ? cap : 1) * FLOATS_PER_VERTEX * sizeof *buf);
    sph = malloc((cap ? cap : 1) * 4 * sizeof *sph);
    count = calloc(GLR_BATCHES, sizeof *count);
    pos = calloc(GLR_BATCHES, sizeof *pos);
    if (!buf || !sph || !count || !pos) { free(buf); free(sph); free(count); free(pos); return -1; }

    /* two passes: count triangles per texture batch, then write them grouped */
    for (pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            size_t acc = 0;
            for (b = 0; b < GLR_BATCHES; b++) { pos[b] = acc; acc += count[b]; }
        }
        for (pi = 0; pi < m->part_count; pi++) {
            const mech3d_part *part = &m->parts[pi];
            const wtb_model *model = &part->model;
            const wtb_object *o = &model->objects[0];
            /* image enhancement, mode 0 (engine 0x1002a4b0, enhancement branch): by the object's type word (OBJ +44 ->
             * object +2) - 0x200 colour 7 (blue), 0x400 0xb (red), 0x100 by its class nibble (0 blue, 1-11 3 yellow, 12+
             * red), others the layer's (world 8, dark red); an active building blue (DOS capture) */
            int wire_code = 0;
            if (part->wire_dmg) wire_code = part->wire_dmg;   /* a mech location (class 0x100): its damage level, set by the caller */
            else if (part->objtype & 0x200) wire_code = 1;
            else if (part->objtype & 0x400) wire_code = 2;
            else if (part->objtype & 0x100) { int cn = (part->objtype & 0xf0) >> 4; wire_code = cn < 1 ? 1 : cn < 12 ? 3 : 2; }
            else if (part->wire_hi) wire_code = 1;
            if (part->hidden || part->moving) continue;   /* moving: a path-carried world part, drawn on the actor layer */
            if (pass == 1) part_sphere(part, psph);
            for (k = 0; k < o->poly_count; k++) {
                const wtb_poly *q = &o->polys[k];
                float face[3], e1[3], e2[3], poly_col[3];
                int c0;
                const wtb_vertex *v0, *v1, *v2;
                int slot = q->color & 0xFF, batch = GLR_SLOTS;
                float ground_lift = 0.0f;
                if (q->n < 3) continue;
                if (((q->color >> 12) & 7) == 3 && !part->shadow) {
                    /* colour type 3 (0x3240 ...): a billboard of bank-0 bitmap (w >> 4) & 0xff (engine 0x10026fc7 ->
                     * 0x1002a8e0): of the triangle's corners (by texel u / v) the foot (u 0, v > 0) and the top (u 0, v 0)
                     * make the vertical edge; the engine draws a screen square of that edge's length centred on it, the
                     * whole bitmap, alpha-keyed (the third corner, u > 0, only orients the bitmap). The scrub records
                     * (TNJ1SCRI: tan_rk, tan_rk2, rrock, cact_2) are rocks and cacti; each is stored twice, once per
                     * winding (two-sided), and drawn once. No bitmap in the table: nothing drawn (ASSUMED) */
                    int bs = (q->color >> 4) & 0xff, ft = -1, tp = -1, k2;
                    const texture *bt = r->bank0 ? r->bank0[bs] : NULL;
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
                    if (pass == 0) { count[GLR_B0 + bs] += 6; continue; }
                    {
                        const wtb_vertex *vf = &o->verts[ft], *vt = &o->verts[tp];
                        float dx = (float)(vt->x - vf->x), dy = (float)(vt->y - vf->y), dz = (float)(vt->z - vf->z);
                        float h = sqrtf(dx * dx + dy * dy + dz * dz), foot[3], col[3] = {1, 1, 1};
                        static const float CX[6] = {-0.5f, 0.5f, 0.5f, -0.5f, 0.5f, -0.5f}, CY[6] = {0, 0, 1, 0, 1, 1};
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
                        float nang;
                        {   /* the stored triangle's face normal (GL space, horizontal): its angle */
                            const wtb_vertex *w0 = &o->verts[q->idx[0]], *w1 = &o->verts[q->idx[1]], *w2 = &o->verts[q->idx[2]];
                            float a1[3] = {(float)(w1->x - w0->x), (float)(w1->y - w0->y), (float)(w1->z - w0->z)};
                            float a2[3] = {(float)(w2->x - w0->x), (float)(w2->y - w0->y), (float)(w2->z - w0->z)};
                            float nx = a1[1] * a2[2] - a1[2] * a2[1], nz = a1[0] * a2[1] - a1[1] * a2[0], gx, gz;
                            if (part->has_rot) { gx = part->rot[0] * nx + part->rot[2] * nz; gz = -(part->rot[6] * nx + part->rot[8] * nz); }
                            else {
                                float cy = cosf(part->yaw * 3.14159265f / 180.0f), sy = sinf(part->yaw * 3.14159265f / 180.0f);
                                gx = cy * nx + sy * nz; gz = -(-sy * nx + cy * nz);
                            }
                            nang = atan2f(gz, gx);
                        }
                        if (r->dos && !model->extended) { col[0] = 2; col[1] = 0; col[2] = 255; }   /* DOS: textured, full level */
                        else glr_texture_average(bt, col);   /* the untextured view: the bitmap's colour */
                        for (a = 0; a < 6; a++) {
                            float *f = buf + pos[GLR_B0 + bs] * FLOATS_PER_VERTEX;
                            int c;
                            memcpy(sph + pos[GLR_B0 + bs] * 4, psph, sizeof psph);
                            f[0] = foot[0]; f[1] = foot[1]; f[2] = foot[2];
                            f[3] = CX[a] * h; f[4] = CY[a] * h; f[5] = 0;
                            f[6] = (CX[a] + 0.5f) * (float)bt->w; f[7] = (1.0f - CY[a]) * (float)bt->h;
                            for (c = 0; c < 3; c++) f[8 + c] = part->tint ? (float)((part->tint >> (16 - 8 * c)) & 0xff) / 255.0f : col[c];
                            f[11] = nang;
                            f[12] = (float)(1024 + CORNER[a] + 3 * MASK[a] + 32 * wire_code);
                            for (c = 0; c < 3; c++) {
                                float pc2 = f[c] + (c == 1 ? CY[a] * h : 0.0f);
                                if (pc2 < mn[c]) mn[c] = pc2;
                                if (pc2 > mx[c]) mx[c] = pc2;
                            }
                            pos[GLR_B0 + bs]++;
                            n++;
                        }
                    }
                    continue;
                }
                if (slot == GLR_SLOT_CAMO) slot += camo + part->tex_offset;
                else if (slot == GLR_SLOT_INSIGNIA) slot += clan + part->tex_offset;   /* engine 0x10042000 */
                if (model->extended && !part->flat && slot < GLR_SLOTS && slots && slots[slot] && (q->color & 0xFF) != 0xF0) batch = slot;
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
                {   /* textured: the vertex colour = the polygon's average texel colour, so the untextured view
                     * (Combat Variables TEXTURES off) shows each polygon in its texture's colours */
                    float pc[3];
                    for (c0 = 0; c0 < 3; c0++) pc[c0] = (float)palette[batch == GLR_SLOTS + 2 ? (q->color >> 4) & 0xff
                                                           : model->extended && (q->color >> 12) == 1 ? (((q->color >> 8) & 0xf) << 4) | 0xf : q->color >> 8][c0] / 255.0f;
                    if (batch < GLR_SLOTS && slots[batch] && slots[batch]->rgba && slots[batch]->w > 0 && slots[batch]->h > 0) {
                        const texture *tt = slots[batch];
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
                        if (model->extended && (v->normal[0] != 0 || v->normal[1] != 0 || v->normal[2] != 0)) {
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
                        f[6] = (float)v->a;
                        f[7] = (float)v->b;
                        /* DOS Gouraud: the vertex's baked index a = ramp | intensity (0x4fef0 -> 0x38560); the shader rescales
                         * the intensity by the polygon's level */
                        if (r->dos && !model->extended && ((q->color >> 12) & 7) == 4) f[6] = (float)(v->a & 15);
                        if (part->tint) { f[8] = (float)((part->tint >> 16) & 0xff) / 255.0f; f[9] = (float)((part->tint >> 8) & 0xff) / 255.0f; f[10] = (float)(part->tint & 0xff) / 255.0f; }
                        else for (c = 0; c < 3; c++) f[8 + c] = poly_col[c];
                        f[11] = 0;
                        /* the wireframe's edges: this corner (0-2) + 3 x the polygon's own edges in this fan triangle
                         * (bit 0: corner 1 - 2, always; bit 1: corner 2 - 0 when it closes the polygon; bit 2: corner 0 - 1
                         * when it starts it), so the triangulation's diagonals are not drawn (DOS draws polygon outlines) */
                        f[12] = (float)(a + 3 * (1 | (j + 2 == q->n ? 2 : 0) | (j == 1 ? 4 : 0)) + 32 * wire_code);
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
    glBindVertexArray(0);
    L->vertex_count = (GLsizei)n;
    for (b = 0; b < GLR_SLOTS; b++)
        if (L->batch_count[b] > 0) upload_texture(r, b, slots[b]);
    for (b = 0; b < GLR_SLOTS; b++)
        if (L->batch_count[GLR_B0 + b] > 0) upload_texture(r, GLR_SLOTS + b, r->bank0 ? r->bank0[b] : NULL);
    if (li == 0) {   /* tests: the static layer's billboards */
        g_billboards = 0;
        for (b = 0; b < GLR_SLOTS; b++) g_billboards += (int)L->batch_count[GLR_B0 + b] / 6;
    }
    free(buf); free(sph); free(count); free(pos);
    return 0;
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

void glr_texture_average(const texture *t, float out[3])
{
    double acc[3] = {0, 0, 0};
    size_t i, n = (size_t)t->w * (size_t)t->h, used = 0;
    for (i = 0; i < n; i++) {
        uint32_t p = t->rgba[i];
        if ((p >> 24) < 128) continue;
        acc[0] += p & 255; acc[1] += (p >> 8) & 255; acc[2] += (p >> 16) & 255;
        used++;
    }
    for (i = 0; i < 3; i++) out[i] = used ? (float)(acc[i] / (double)used / 255.0) : 0.5f;
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
int glr_billboard_count(void) { return g_billboards; }

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
    glUniform1i(r->u_horizon, 0);
    glUniform1i(r->u_mga_lod, 0);
    glUniform1f(r->u_alpha, 1.0f);
    glUniform1i(r->u_unlit, r->dos);            /* DOS: the sky and ground are flat palette colours, unlit (0x3c000) */
    glUniform2f(r->u_texsize, 1, 1);            /* environment UVs are in texture repeats */
    glUniform1i(r->u_textured, 1);
    glActiveTexture(GL_TEXTURE0);
    {
        static const float off[4] = {0, 0, 0, 0};
        glUseProgram(r->prog);
        glUniform4fv(r->u_wire, 1, off);
        glUniform4fv(r->u_mono, 1, off);
    }
    if (!v->wire && !v->solo && (r->env_has[0] || r->env_has[1] || r->dos)) {
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
            glUniform1f(r->u_ambient, 1);
            glBindTexture(GL_TEXTURE_2D, v->flat_sky && r->sky_flat ? r->sky_flat : r->env_tex[1]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)(v->mga_mip ? GL_NEAREST : eminf));   /* Mystique sky: one level */
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)emagf);
            glDrawArrays(GL_TRIANGLES, 0, 30);   /* walls + ceiling */
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
        if (r->env_has[0] && !r->dos) {
            glUniformMatrix4fv(r->u_mvp, 1, GL_FALSE, mvp);
            glUniformMatrix4fv(r->u_view, 1, GL_FALSE, view);
            glUniform1f(r->u_fog_density, v->ortho_w > 0 ? 0.0f : fog_k(v->fog_density));
            glBindTexture(GL_TEXTURE_2D, (v->notex_world || v->flat_ground) && r->ground_flat ? r->ground_flat : r->env_tex[0]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)eminf);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)emagf);
            glUniform1i(r->u_mga_lod, v->mga_mip);
            glDrawArrays(GL_TRIANGLES, 30, 6);
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
        glUniform1f(r->u_fog_density, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    for (int li = 0; li < GLR_LAYERS; li++) {
    const glr_layer *L = &r->layer[li];
    if (v->solo && li == 0) continue;
    {   static const float off0[4] = {0, 0, 0, 0}; glUniform4fv(r->u_mono, 1, li == 1 ? v->mono : off0); }
    {   /* image enhancement: lines, world red, actors (mechs, cockpit) blue (DOS w) */
        static const float wr[4] = {60.0f / 255.0f, 0, 0, 1}, wb[4] = {0, 48.0f / 255.0f, 215.0f / 255.0f, 1}, off[4] = {0, 0, 0, 0};   /* DOS capture: world (60,0,0), mechs and cockpit (0,48,215) */
        glUniform4fv(r->u_wire, 1, v->wire ? (li == 0 ? wr : wb) : off);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);   /* the wireframe's edges come from the fragment shader */
    }
    glBindVertexArray(L->vao);
    for (s = 0; s <= GLR_SLOTS; s++) {
        if (L->batch_count[s] <= 0) continue;
        if (s < GLR_SLOTS) {
            glBindTexture(GL_TEXTURE_2D, r->tex[s]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)minf);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)magf);
            glUniform2f(r->u_texsize, (float)r->tex_w[s], (float)r->tex_h[s]);
            glUniform1i(r->u_textured, !(li == 0 ? v->notex_world : v->notex_actors));   /* Combat Variables textures */
        } else {
            glUniform1i(r->u_textured, 0);
        }
        glDrawArrays(GL_TRIANGLES, L->batch_first[s], L->batch_count[s]);
    }
    for (s = 0; s < GLR_SLOTS; s++) {   /* bank-0 billboards (colour type 3: scrub rocks, cacti), alpha-keyed */
        if (L->batch_count[GLR_B0 + s] <= 0) continue;
        glBindTexture(GL_TEXTURE_2D, r->tex[GLR_SLOTS + s]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)minf);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)magf);
        glUniform2f(r->u_texsize, (float)r->tex_w[GLR_SLOTS + s], (float)r->tex_h[GLR_SLOTS + s]);
        glUniform1i(r->u_textured, !(li == 0 ? v->notex_world : v->notex_actors));
        glDrawArrays(GL_TRIANGLES, L->batch_first[GLR_B0 + s], L->batch_count[GLR_B0 + s]);
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
            for (j = 0; j < 3; j++) {
                q[0][j] = c[j] - right[j] * sp->half_w - up[j] * sp->half_h;
                q[1][j] = c[j] + right[j] * sp->half_w - up[j] * sp->half_h;
                q[2][j] = c[j] + right[j] * sp->half_w + up[j] * sp->half_h;
                q[3][j] = c[j] - right[j] * sp->half_w + up[j] * sp->half_h;
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
        for (k = 0; k < r->spr_n; k++) {
            glBindTexture(GL_TEXTURE_2D, sprite_texture(r, r->spr[k].tex));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)minf);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)magf);
            glUniform2f(r->u_texsize, (float)r->spr[k].tex->w, (float)r->spr[k].tex->h);
            glDrawArrays(GL_TRIANGLES, k * 6, 6);
        }
        glUniform1i(r->u_unlit, 0);
        glBindVertexArray(0);
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_SCISSOR_TEST);
}
