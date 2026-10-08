/* hud.c - see hud.h. */
#include "hud.h"

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "shp.h"

struct hud_s {
    GLuint prog, vao, vbo, lvbo;   /* lvbo: hud_lines batches */
    GLint  u_tex, u_tint, u_textured, u_smooth, u_sharp, u_flash;
    float  flash[4];       /* the palette flash: every HUD colour mixed toward rgb by a (hud_set_flash) */
    uint8_t pal[256][3];
    int    have_pal, vw, vh;
    float  ox, k, ky;      /* 4:3 area: left edge (px) and px per virtual unit across (k) and down (ky) */
    float  n;              /* screen pixels per sprite pixel: 1, or lines / 768 above 768 lines (the K set at the
                            * size it has at 1024 x 768, its Scale3x texture filtered linearly) */
    GLuint font_tex, font_tex3;   /* the atlas 1:1 (whole-number scales) and Scale3x'd (in-between scales) */
    int    font_h, cell_w, glyph_w[128], font_sw, font_sh;   /* font_sw / sh: atlas cell strides (1 px gutter) */
};

static GLuint compile(GLenum t, const char *src)
{
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

hud *hud_create(prj_archive *a)
{
    static const char *vs = "#version 330 core\nlayout(location=0) in vec2 p; layout(location=1) in vec2 uv; out vec2 t;"
                            "void main(){ t = uv; gl_Position = vec4(p, 0.0, 1.0); }\n";
    static const char *fs = "#version 330 core\nin vec2 t; out vec4 c; uniform sampler2D u_tex; uniform vec4 u_tint; uniform int u_textured; uniform int u_smooth; uniform float u_sharp; uniform vec4 u_flash;"
                            /* u_sharp > 0 (the font): "sharp bilinear" - each texel a solid block, only its edges blended
                             * over one screen pixel (u_sharp = screen pixels per texel), so 1-pixel strokes stay crisp at
                             * any scale, as DirectDraw blitted them 1:1 */
                            "vec2 sharp(vec2 uv){ vec2 sz = vec2(textureSize(u_tex, 0)); vec2 p = uv * sz - 0.5; vec2 i = floor(p);"
                            " vec2 f = clamp((fract(p) - 0.5) * u_sharp + 0.5, 0.0, 1.0); return (i + f + 0.5) / sz; }"
                            "void main(){ c = (u_textured != 0 ? texture(u_tex, u_sharp > 0.0 ? sharp(t) : t) : vec4(1.0)); if (u_smooth != 0) c.a = sqrt(c.a); c *= u_tint; if (c.a < 0.01) discard; c.rgb = mix(c.rgb, u_flash.rgb, u_flash.a); }\n";
    hud *h = calloc(1, sizeof *h);
    prj_record r;
    if (!h) return NULL;
    h->prog = glCreateProgram();
    glAttachShader(h->prog, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(h->prog, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(h->prog);
    h->u_tex = glGetUniformLocation(h->prog, "u_tex");
    h->u_tint = glGetUniformLocation(h->prog, "u_tint");
    h->u_textured = glGetUniformLocation(h->prog, "u_textured");
    h->u_smooth = glGetUniformLocation(h->prog, "u_smooth");
    h->u_sharp = glGetUniformLocation(h->prog, "u_sharp");
    h->u_flash = glGetUniformLocation(h->prog, "u_flash");
    glGenVertexArrays(1, &h->vao);
    glGenBuffers(1, &h->vbo);
    glBindVertexArray(h->vao);
    glBindBuffer(GL_ARRAY_BUFFER, h->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 24, NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    glBindVertexArray(0);
    /* the COCKPIT palette (6-bit VGA values when all are < 64) */
    if (prj_read_named(a, "PAL", "COCKPIT", &r) == PRJ_OK) {
        size_t off = r.size >= 768 ? r.size - 768 : 0;
        int i, six = 1;
        for (i = 0; i < 768 && off + (size_t)i < r.size; i++) if (r.data[off + (size_t)i] > 63) six = 0;
        for (i = 0; i < 256 && off + (size_t)i * 3 + 2 < r.size; i++) {
            int k;
            for (k = 0; k < 3; k++) h->pal[i][k] = (uint8_t)(six ? r.data[off + (size_t)i * 3 + (size_t)k] << 2 : r.data[off + (size_t)i * 3 + (size_t)k]);
        }
        h->have_pal = 1;
        prj_record_free(&r);
    }
    return h;
}

void hud_destroy(hud *h)
{
    if (!h) return;
    if (h->font_tex) glDeleteTextures(1, &h->font_tex);
    if (h->font_tex3) glDeleteTextures(1, &h->font_tex3);
    glDeleteBuffers(1, &h->vbo);
    glDeleteVertexArrays(1, &h->vao);
    glDeleteProgram(h->prog);
    free(h);
}

int hud_sprite_load(hud *h, prj_archive *a, const char *name, int frame, hud_sprite *out)
{
    return hud_sprite_load_remap(h, a, name, frame, -1, -1, out);
}

/* Scale3x (AdvMAME3x): each pixel becomes a 3x3 block whose corners follow matching neighbours - diagonal edges
 * stay sharp, the centre texel is always the original. Returns a malloc'd (3w x 3h) image or NULL. */
static uint32_t *scale3x(const uint32_t *in, int aw, int ah)
{
    uint32_t *out = malloc((size_t)aw * (size_t)ah * 9 * 4);
    int x, y;
    if (!out) return NULL;
    for (y = 0; y < ah; y++)
        for (x = 0; x < aw; x++) {
#define PX(xx, yy) in[((yy) < 0 ? 0 : (yy) >= ah ? ah - 1 : (yy)) * aw + ((xx) < 0 ? 0 : (xx) >= aw ? aw - 1 : (xx))]
            uint32_t A = PX(x - 1, y - 1), B = PX(x, y - 1), C = PX(x + 1, y - 1), D = PX(x - 1, y), E = PX(x, y), F = PX(x + 1, y),
                     G = PX(x - 1, y + 1), H = PX(x, y + 1), I = PX(x + 1, y + 1), o[9];
#undef PX
            int k;
            if (B != H && D != F) {
                o[0] = D == B ? D : E;
                o[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
                o[2] = B == F ? F : E;
                o[3] = (D == B && E != G) || (D == H && E != A) ? D : E;
                o[4] = E;
                o[5] = (B == F && E != I) || (H == F && E != C) ? F : E;
                o[6] = D == H ? D : E;
                o[7] = (D == H && E != I) || (H == F && E != G) ? H : E;
                o[8] = H == F ? F : E;
            } else for (k = 0; k < 9; k++) o[k] = E;
            for (k = 0; k < 9; k++) out[(size_t)(y * 3 + k / 3) * (size_t)(aw * 3) + (size_t)(x * 3 + k % 3)] = o[k];
        }
    return out;
}

int hud_sprite_load_remap(hud *h, prj_archive *a, const char *name, int frame, int from, int to, hud_sprite *out)
{
    prj_record r;
    shp_frame f;
    uint8_t *rgba;
    int i, tw, th;
    memset(out, 0, sizeof *out);
    if (prj_read_named(a, "SHP", name, &r) != PRJ_OK) return -1;
    if (shp_decode(r.data, r.size, frame, &f) != 0) { prj_record_free(&r); return -1; }
    prj_record_free(&r);
    rgba = malloc((size_t)f.w * (size_t)f.h * 4);
    if (!rgba) { shp_frame_free(&f); return -1; }
    tw = f.w; th = f.h;
    for (i = 0; i < f.w * f.h; i++) {
        int c = (from >= 0 && f.pix[i] == from) ? to : f.pix[i];
        rgba[i * 4 + 0] = h->pal[c][0];
        rgba[i * 4 + 1] = h->pal[c][1];
        rgba[i * 4 + 2] = h->pal[c][2];
        rgba[i * 4 + 3] = f.mask[i] ? 255 : 0;
    }
    {   /* the texture is the Scale3x upscale (see bind_sprite); transparent texels are all 0 here, so they match */
        uint8_t *up;
        for (i = 0; i < f.w * f.h; i++) if (!rgba[i * 4 + 3]) rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 0;
        up = (uint8_t *)scale3x((const uint32_t *)rgba, f.w, f.h);
        if (up) { free(rgba); rgba = up; tw = f.w * 3; th = f.h * 3; }
    }
    {   /* bleed colour into transparent texels so bilinear edges don't darken (alpha stays 0) */
        int x, y, pass;
        for (pass = 0; pass < 2; pass++)
            for (y = 0; y < th; y++)
                for (x = 0; x < tw; x++) {
                    uint8_t *t = rgba + ((size_t)y * (size_t)tw + (size_t)x) * 4;
                    int dx, dy, n = 0, acc[3] = {0, 0, 0};
                    if (t[3] == 255 || (pass == 1 && (t[0] | t[1] | t[2]))) continue;
                    for (dy = -1; dy <= 1; dy++)
                        for (dx = -1; dx <= 1; dx++) {
                            const uint8_t *q;
                            if (x + dx < 0 || y + dy < 0 || x + dx >= tw || y + dy >= th) continue;
                            q = rgba + ((size_t)(y + dy) * (size_t)tw + (size_t)(x + dx)) * 4;
                            if (q[3] == 255 || (pass == 1 && (q[0] | q[1] | q[2]))) { acc[0] += q[0]; acc[1] += q[1]; acc[2] += q[2]; n++; }
                        }
                    if (n) { t[0] = (uint8_t)(acc[0] / n); t[1] = (uint8_t)(acc[1] / n); t[2] = (uint8_t)(acc[2] / n); }
                }
    }
    glGenTextures(1, &out->tex);
    glBindTexture(GL_TEXTURE_2D, out->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    out->w = f.w; out->h = f.h; out->left = f.left; out->top = f.top;
    free(rgba);
    shp_frame_free(&f);
    return 0;
}

void hud_sprite_free(hud_sprite *s)
{
    if (s->tex) glDeleteTextures(1, &s->tex);
    memset(s, 0, sizeof *s);
}

void hud_begin_stretched(hud *h, int w, int hp)
{
    hud_begin(h, w, hp);
    h->k = (float)w / 640.0f;
    h->ky = (float)hp / 480.0f;
    h->ox = 0;
    h->n = 1;
}
float hud_px_x(const hud *h, float px) { return px * h->n / h->k; }
float hud_px_y(const hud *h, float px) { return px * h->n / h->ky; }

void hud_begin(hud *h, int w, int hp)
{
    glViewport(0, 0, w, hp);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(h->prog);
    glUniform1i(h->u_tex, 0);
    glUniform4fv(h->u_flash, 1, h->flash);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(h->vao);
    h->vw = w; h->vh = hp;
    h->k = (float)hp * 4.0f / 3.0f / 640.0f;
    if ((float)w < (float)hp * 4.0f / 3.0f) h->k = (float)w / 640.0f;
    h->ox = ((float)w - 640.0f * h->k) * 0.5f;
    h->ky = h->k;
    h->n = hp > 768 ? (float)hp / 768.0f : 1.0f;
}

/* screen-pixel quad (top-left origin) */
static void quad_px(hud *h, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1)
{
    float X0 = x0 / (float)h->vw * 2.0f - 1.0f, X1 = x1 / (float)h->vw * 2.0f - 1.0f;
    float Y0 = 1.0f - y0 / (float)h->vh * 2.0f, Y1 = 1.0f - y1 / (float)h->vh * 2.0f;
    float v[24] = {X0, Y0, u0, v0, X1, Y0, u1, v0, X1, Y1, u1, v1, X0, Y0, u0, v0, X1, Y1, u1, v1, X0, Y1, u0, v1};
    glBindBuffer(GL_ARRAY_BUFFER, h->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}
static float sx_(const hud *h, float x) { return floorf(h->ox + x * h->k + 0.5f); }
static float sy_(const hud *h, float y) { return floorf(y * h->ky + 0.5f); }

void hud_end(hud *h)
{
    (void)h;
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

/* 640x480 virtual pixels -> clip space (keeps 4:3 centred horizontally on wide windows) */
static void quad(hud *h, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1)
{
    float ox = h->ox, k = h->k, ky = h->ky;   /* the same mapping as sx_ / sy_ */
    float X0 = (ox + x0 * k) / (float)h->vw * 2.0f - 1.0f, X1 = (ox + x1 * k) / (float)h->vw * 2.0f - 1.0f;
    float Y0 = 1.0f - y0 * ky / (float)h->vh * 2.0f, Y1 = 1.0f - y1 * ky / (float)h->vh * 2.0f;
    float v[24] = {X0, Y0, u0, v0, X1, Y0, u1, v0, X1, Y1, u1, v1, X0, Y0, u0, v0, X1, Y1, u1, v1, X0, Y1, u0, v1};
    glBindBuffer(GL_ARRAY_BUFFER, h->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* Bind a sprite. Its texture is the Scale3x upscale of the art (sprite_upload): drawn at 1:1 it is sampled at each 3x3
 * block's centre (the original pixel); drawn larger it is filtered linearly - crisp, smooth edges on the digits and
 * ticks rather than blocky or uneven pixel doubling */
static void bind_sprite(const hud *h, const hud_sprite *s, float scale)
{
    GLint f = s->smooth || scale * h->n > 1.01f ? GL_LINEAR : GL_NEAREST;
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
}

void hud_draw(hud *h, const hud_sprite *s, float x, float y, const float tint[4])
{
    static const float one[4] = {1, 1, 1, 1};
    if (!s->tex) return;
    bind_sprite(h, s, 1.0f);
    glUniform1i(h->u_textured, 1);
    glUniform1i(h->u_smooth, s->smooth);
    glUniform4fv(h->u_tint, 1, tint ? tint : one);
    {
        float X = sx_(h, x) + (float)s->left * h->n, Y = sy_(h, y) + (float)s->top * h->n;
        quad_px(h, X, Y, X + (float)s->w * h->n, Y + (float)s->h * h->n, 0, 0, 1, 1);
    }
}

void hud_draw_scaled(hud *h, const hud_sprite *s, float x, float y, int k)
{
    static const float one[4] = {1, 1, 1, 1};
    if (!s->tex || k < 1) return;
    bind_sprite(h, s, (float)k);
    glUniform1i(h->u_textured, 1);
    glUniform1i(h->u_smooth, s->smooth);
    glUniform4fv(h->u_tint, 1, one);
    {
        float X = sx_(h, x) + (float)s->left * h->n * (float)k, Y = sy_(h, y) + (float)s->top * h->n * (float)k;
        quad_px(h, X, Y, X + (float)s->w * h->n * (float)k, Y + (float)s->h * h->n * (float)k, 0, 0, 1, 1);
    }
}

void hud_draw_f(hud *h, const hud_sprite *s, float x, float y, float k)
{
    static const float one[4] = {1, 1, 1, 1};
    if (k < 0) k = (float)h->vh / 768.0f / (float)h->n;   /* the DOS size: 1:1 at 768 lines */
    if (!s->tex || k <= 0) return;
    bind_sprite(h, s, k);
    glUniform1i(h->u_textured, 1);
    glUniform1i(h->u_smooth, s->smooth);
    glUniform4fv(h->u_tint, 1, one);
    {
        float X = sx_(h, x) + (float)s->left * (float)h->n * k, Y = sy_(h, y) + (float)s->top * (float)h->n * k;
        quad_px(h, X, Y, X + (float)s->w * (float)h->n * k, Y + (float)s->h * (float)h->n * k, 0, 0, 1, 1);
    }
}

void hud_draw_stretch_top(hud *h, const hud_sprite *s, float height_px) { hud_draw_stretch_row(h, s, height_px, 0); }

void hud_draw_stretch_row(hud *h, const hud_sprite *s, float height_px, int bottom)
{
    static const float one[4] = {1, 1, 1, 1};
    if (!s->tex) return;
    bind_sprite(h, s, 2.0f);
    glUniform1i(h->u_textured, 1);
    glUniform1i(h->u_smooth, s->smooth);
    glUniform4fv(h->u_tint, 1, one);
    if (height_px <= 0) height_px = (float)s->h * (float)h->vh / ((float)s->w * 0.75f);   /* the original's: the bar spans a 4:3 screen's width */
    {   /* three pieces: the bevelled end caps at their own proportions, the middle stretched (stretching the whole
         * sprite smeared the caps across the corners) */
        float k = height_px / (float)s->h, cap = (float)s->h * 0.6f, capx = cap * k, u = cap / (float)s->w;
        float y0 = bottom ? (float)h->vh - height_px : 0, y1 = y0 + height_px;
        if (capx * 2.0f >= (float)h->vw) { quad_px(h, 0, y0, (float)h->vw, y1, 0, 0, 1, 1); return; }
        quad_px(h, 0, y0, capx, y1, 0, 0, u, 1);
        quad_px(h, capx, y0, (float)h->vw - capx, y1, u, 0, 1.0f - u, 1);
        quad_px(h, (float)h->vw - capx, y0, (float)h->vw, y1, 1.0f - u, 0, 1, 1);
    }
}

float hud_left_x(const hud *h) { return h->k > 0 ? -h->ox / h->k : 0; }

void hud_draw_window(hud *h, const hud_sprite *s, float u0, float width, float x, float y, const float tint[4])
{
    static const float one[4] = {1, 1, 1, 1};
    if (!s->tex || s->w <= 0) return;
    bind_sprite(h, s, 1.0f);
    glUniform1i(h->u_textured, 1);
    glUniform1i(h->u_smooth, s->smooth);
    glUniform4fv(h->u_tint, 1, tint ? tint : one);
    {
        float X = sx_(h, x) - floorf(width * 0.5f) * (float)h->n, Y = sy_(h, y);
        quad_px(h, X, Y, X + width * (float)h->n, Y + (float)s->h * h->n, u0 / (float)s->w, 0, (u0 + width) / (float)s->w, 1);
    }
}

void hud_rect(hud *h, float x, float y, float w, float hg, const float rgba[4])
{
    glUniform1i(h->u_textured, 0);
    glUniform1i(h->u_smooth, 0);
    glUniform4fv(h->u_tint, 1, rgba);
    quad(h, x, y, x + w, y + hg, 0, 0, 1, 1);
}

/* Whole-window colour pass: out = dst x mul + add (light amplification). Call right after hud_begin. */
void hud_set_flash(hud *h, float r, float g, float b, float k)
{
    h->flash[0] = r; h->flash[1] = g; h->flash[2] = b; h->flash[3] = k < 0 ? 0 : k > 1 ? 1 : k;
    glUseProgram(h->prog);
    glUniform4fv(h->u_flash, 1, h->flash);
}

float hud_flash_level(const hud *h) { return h->flash[3]; }

void hud_tint(hud *h, int w, int hp, const float mul[3], const float add[3])
{
    static const float none[4] = {0, 0, 0, 0};
    float m4[4] = {mul[0], mul[1], mul[2], 1}, a4[4] = {add[0], add[1], add[2], 1};
    glUniform4fv(h->u_flash, 1, none);
    glUniform1i(h->u_textured, 0);
    glUniform1i(h->u_smooth, 0);
    glBlendFunc(GL_DST_COLOR, GL_ZERO);
    glUniform4fv(h->u_tint, 1, m4);
    quad_px(h, 0, 0, (float)w, (float)hp, 0, 0, 1, 1);
    glBlendFunc(GL_ONE, GL_ONE);
    glUniform4fv(h->u_tint, 1, a4);
    quad_px(h, 0, 0, (float)w, (float)hp, 0, 0, 1, 1);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUniform4fv(h->u_flash, 1, h->flash);
}

int hud_layout(prj_archive *a, int rects[][4], int max)
{
    prj_record r;
    int n = 0;
    size_t o;
    if (prj_read_named(a, "CPIT", "MW2MECH", &r) != PRJ_OK) return 0;
    for (o = 0x28; o + 8 <= r.size && n < max; o += 8) {
        int k;
        for (k = 0; k < 4; k++) rects[n][k] = (int16_t)(r.data[o + (size_t)k * 2] | (r.data[o + (size_t)k * 2 + 1] << 8));
        if (rects[n][2] == 0 && rects[n][3] == 0) break;
        n++;
    }
    prj_record_free(&r);
    return n;
}

void hud_draw_region(hud *h, const hud_sprite *s, float x, float y, float rx, float ry, float rw, float rh)
{
    static const float one[4] = {1, 1, 1, 1};
    if (!s->tex || s->w <= 0 || s->h <= 0 || rw <= 0 || rh <= 0) return;
    bind_sprite(h, s, 1.0f);
    glUniform1i(h->u_textured, 1);
    glUniform1i(h->u_smooth, s->smooth);
    glUniform4fv(h->u_tint, 1, one);
    {
        float n = (float)h->n, X = sx_(h, x) + (float)s->left * n + rx * n, Y = sy_(h, y) + (float)s->top * n + ry * n;
        quad_px(h, X, Y, X + rw * n, Y + rh * n, rx / (float)s->w, ry / (float)s->h, (rx + rw) / (float)s->w, (ry + rh) / (float)s->h);
    }
}

int hud_font_load(hud *h, prj_archive *a, const char *name)
{
    prj_record r;
    uint32_t cnt, fh, key, i;
    uint8_t *atlas;
    int maxw = 1;
    if (prj_read_named(a, "FONT", name, &r) != PRJ_OK) return -1;
    if (r.size < 16 + 128 * 4) { prj_record_free(&r); return -1; }
    cnt = (uint32_t)(r.data[4] | (r.data[5] << 8)); fh = (uint32_t)(r.data[8] | (r.data[9] << 8)); key = r.data[12];
    if (cnt > 128) cnt = 128;
    for (i = 0; i < cnt; i++) {
        uint32_t o = (uint32_t)(r.data[16 + i * 4] | (r.data[17 + i * 4] << 8) | (r.data[18 + i * 4] << 16));
        int w = o + 4 <= r.size ? (int)(r.data[o] | (r.data[o + 1] << 8)) : 0;
        if (w > 32) w = 0;
        h->glyph_w[i] = w;
        if (w > maxw) maxw = w;
    }
    h->font_h = (int)fh; h->cell_w = maxw; h->font_sw = maxw + 1; h->font_sh = (int)fh + 1;
    atlas = calloc((size_t)(16 * h->font_sw) * (size_t)(8 * h->font_sh) * 4, 1);
    if (!atlas) { prj_record_free(&r); return -1; }
    for (i = 0; i < cnt; i++) {
        uint32_t o = (uint32_t)(r.data[16 + i * 4] | (r.data[17 + i * 4] << 8) | (r.data[18 + i * 4] << 16));
        int w = h->glyph_w[i], x, y, cx = (int)(i % 16) * h->font_sw, cy = (int)(i / 16) * h->font_sh;
        for (y = 0; y < (int)fh; y++)
            for (x = 0; x < w; x++) {
                size_t src = o + 4 + (size_t)y * (size_t)w + (size_t)x;
                uint8_t c = src < r.size ? r.data[src] : (uint8_t)key;
                size_t dst = ((size_t)(cy + y) * (size_t)(16 * h->font_sw) + (size_t)(cx + x)) * 4;
                if (c == key) continue;
                atlas[dst] = 255; atlas[dst + 1] = 255; atlas[dst + 2] = 255; atlas[dst + 3] = 255;   /* white ink: the tint gives the colour */
            }
    }
    if (h->font_tex) glDeleteTextures(1, &h->font_tex);
    if (h->font_tex3) { glDeleteTextures(1, &h->font_tex3); h->font_tex3 = 0; }
    glGenTextures(1, &h->font_tex);
    glBindTexture(GL_TEXTURE_2D, h->font_tex);
    {   /* the atlas at the font's own size, filtered linearly and sampled "sharp bilinear" by hud_text (the
         * Windows editions blitted it 1:1 with DirectDraw; Scale3x + linear had softened the 1-pixel strokes) */
        int aw = 16 * h->font_sw, ah = 8 * h->font_sh;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, aw, ah, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        {   /* in-between scales: Scale3x and linear filtering keep the strokes even and bold */
            uint32_t *out = scale3x((const uint32_t *)atlas, aw, ah);
            if (out) {
                glGenTextures(1, &h->font_tex3);
                glBindTexture(GL_TEXTURE_2D, h->font_tex3);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, aw * 3, ah * 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                free(out);
            }
        }
    }
    free(atlas);
    prj_record_free(&r);
    return 0;
}

/* the font's scale: whole screen pixels per font pixel (the nearest to the HUD's scale, at least 1), so every stroke
 * is the same width - the Windows editions' DirectDraw text, 1:1 at 1024 x 768 */
static float font_scale(const hud *h)
{
    float r = floorf(h->n + 0.5f);
    if (r < 1.0f) r = 1.0f;
    /* within 15 % of a whole multiple: that multiple (pixel-exact); otherwise the true scale, sampled sharp bilinear */
    return fabsf(h->n - r) <= 0.15f * h->n ? r : h->n;
}

float hud_text(hud *h, float x, float y, const char *str, const float rgba[4])
{
    float X = sx_(h, x), Y = sy_(h, y), X0 = X, aw, ah, n = font_scale(h);
    if (!h->font_tex) return 0;
    aw = (float)(16 * h->font_sw); ah = (float)(8 * h->font_sh);
    {   /* a whole-number scale: the 1:1 atlas, every font pixel an exact block (DirectDraw's look); else Scale3x */
        int whole = fabsf(n - floorf(n + 0.5f)) < 0.01f || !h->font_tex3;
        glBindTexture(GL_TEXTURE_2D, whole ? h->font_tex : h->font_tex3);
        glUniform1f(h->u_sharp, whole ? n : 0.0f);
    }
    glUniform1i(h->u_textured, 1);
    glUniform1i(h->u_smooth, 0);
    X = floorf(X + 0.5f); Y = floorf(Y + 0.5f);   /* on the pixel grid */
    glUniform4fv(h->u_tint, 1, rgba);
    for (; *str; str++) {
        int c = (unsigned char)*str & 127, w = h->glyph_w[c];
        float u = (float)((c % 16) * h->font_sw), v = (float)((c / 16) * h->font_sh), gx = floorf(X + 0.5f);
        if (w <= 0) { X += (float)h->cell_w * 0.5f * n; continue; }
        quad_px(h, gx, Y, gx + (float)w * n, Y + (float)h->font_h * n, u / aw, v / ah, (u + (float)w) / aw, (v + (float)h->font_h) / ah);
        X += (float)w * n;
    }
    glUniform1f(h->u_sharp, 0.0f);
    return (X - X0) / h->k;
}

void hud_sprite_smooth(hud_sprite *s, int on)
{
    if (!s->tex) return;
    s->smooth = on;
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, on ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, on ? GL_LINEAR : GL_NEAREST);
}

void hud_draw_window_v(hud *h, const hud_sprite *s, float v0, float height, float x, float y)
{
    static const float one[4] = {1, 1, 1, 1};
    if (!s->tex || s->h <= 0) return;
    if (v0 < 0) v0 = 0;
    if (v0 + height > (float)s->h) v0 = (float)s->h - height;
    bind_sprite(h, s, 1.0f);
    glUniform1i(h->u_textured, 1);
    glUniform1i(h->u_smooth, s->smooth);
    glUniform4fv(h->u_tint, 1, one);
    {
        float X = sx_(h, x), Y = sy_(h, y);
        quad_px(h, X, Y, X + (float)s->w * h->n, Y + height * (float)h->n, 0, v0 / (float)s->h, 1, (v0 + height) / (float)s->h);
    }
}

void hud_lines(hud *h, const float *seg, int n, float wpx, const float rgba[4])
{
    float *v;
    int i, m = 0;
    if (n <= 0) return;
    v = malloc((size_t)n * 24 * sizeof *v);
    if (!v) return;
    for (i = 0; i < n; i++) {
        float X0 = h->ox + seg[i * 4] * h->k, Y0 = seg[i * 4 + 1] * h->ky, X1 = h->ox + seg[i * 4 + 2] * h->k, Y1 = seg[i * 4 + 3] * h->ky;
        float dx = X1 - X0, dy = Y1 - Y0, len = sqrtf(dx * dx + dy * dy), nx, ny, *q = v + m * 24;
        float ax, ay, bx, by, cx, cy, ex, ey;
        if (len < 1e-3f) continue;
        nx = -dy / len * wpx * 0.5f; ny = dx / len * wpx * 0.5f;
        ax = X0 + nx; ay = Y0 + ny; bx = X1 + nx; by = Y1 + ny; cx = X1 - nx; cy = Y1 - ny; ex = X0 - nx; ey = Y0 - ny;
#define CX(x) ((x) / (float)h->vw * 2.0f - 1.0f)
#define CY(y) (1.0f - (y) / (float)h->vh * 2.0f)
        q[0] = CX(ax); q[1] = CY(ay); q[4] = CX(bx); q[5] = CY(by); q[8] = CX(cx); q[9] = CY(cy);
        q[12] = CX(ax); q[13] = CY(ay); q[16] = CX(cx); q[17] = CY(cy); q[20] = CX(ex); q[21] = CY(ey);
#undef CX
#undef CY
        q[2] = q[3] = q[6] = q[7] = q[10] = q[11] = q[14] = q[15] = q[18] = q[19] = q[22] = q[23] = 0;
        m++;
    }
    if (m) {   /* one upload and one draw for the lot (a draw per line stalled on the shared buffer) */
        GLint cur = 0;
        glUniform1i(h->u_textured, 0);
        glUniform1i(h->u_smooth, 0);
        glUniform4fv(h->u_tint, 1, rgba);
        if (!h->lvbo) {
            glGenBuffers(1, &h->lvbo);
        }
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &cur);
        glBindBuffer(GL_ARRAY_BUFFER, h->lvbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)m * 24 * sizeof *v), v, GL_STREAM_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
        glDrawArrays(GL_TRIANGLES, 0, m * 6);
        glBindBuffer(GL_ARRAY_BUFFER, h->vbo);   /* back to the shared quad buffer */
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
        (void)cur;
    }
    free(v);
}

void hud_line(hud *h, float x0, float y0, float x1, float y1, float wpx, const float rgba[4])
{
    float X0 = h->ox + x0 * h->k, Y0 = y0 * h->ky, X1 = h->ox + x1 * h->k, Y1 = y1 * h->ky;
    float dx = X1 - X0, dy = Y1 - Y0, len = sqrtf(dx * dx + dy * dy), nx, ny, v[24];
    float ax, ay, bx, by, cx, cy, ex, ey;
    if (len < 1e-3f) return;
    nx = -dy / len * wpx * 0.5f; ny = dx / len * wpx * 0.5f;
    ax = X0 + nx; ay = Y0 + ny; bx = X1 + nx; by = Y1 + ny; cx = X1 - nx; cy = Y1 - ny; ex = X0 - nx; ey = Y0 - ny;
#define CX(x) ((x) / (float)h->vw * 2.0f - 1.0f)
#define CY(y) (1.0f - (y) / (float)h->vh * 2.0f)
    v[0] = CX(ax); v[1] = CY(ay); v[4] = CX(bx); v[5] = CY(by); v[8] = CX(cx); v[9] = CY(cy);
    v[12] = CX(ax); v[13] = CY(ay); v[16] = CX(cx); v[17] = CY(cy); v[20] = CX(ex); v[21] = CY(ey);
#undef CX
#undef CY
    v[2] = v[3] = v[6] = v[7] = v[10] = v[11] = v[14] = v[15] = v[18] = v[19] = v[22] = v[23] = 0;
    glUniform1i(h->u_textured, 0);
    glUniform1i(h->u_smooth, 0);
    glUniform4fv(h->u_tint, 1, rgba);
    glBindBuffer(GL_ARRAY_BUFFER, h->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void hud_circle(hud *h, float cx, float cy, float r, float wpx, const float rgba[4])
{
    int i, n = 64;
    for (i = 0; i < n; i++) {
        float a0 = (float)i / (float)n * 6.2831853f, a1 = (float)(i + 1) / (float)n * 6.2831853f;
        hud_line(h, cx + cosf(a0) * r, cy + sinf(a0) * r, cx + cosf(a1) * r, cy + sinf(a1) * r, wpx, rgba);
    }
}

float hud_text_width(hud *h, const char *str)
{
    float w = 0;
    if (!h->font_tex) return 0;
    for (; *str; str++) {
        int c = (unsigned char)*str & 127;
        w += h->glyph_w[c] > 0 ? (float)h->glyph_w[c] : (float)h->cell_w * 0.5f;
    }
    return w * font_scale(h) / h->k;
}
