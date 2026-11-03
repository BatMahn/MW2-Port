#version 450
#extension GL_GOOGLE_include_directive : require
// world.frag - the world renderer's fragment shader (src/glr.c; the OpenGL 3.3 program's FS, ported to SDL_GPU).
// Compiled twice: DOS_LOOK 0 and 1 (see world.vert). Differences from the OpenGL program, none in what it computes:
//  - gl_FragCoord is SDL_GPU's (top-left origin): glFragCoord below is GL's (bottom-left), u_fbh the target's height
//  - samplers: u_tex, u_dos_tab, u_ptab are fragment textures 0, 1, 2; the DOS buffer is a storage buffer
#define UBO_SET 3
#define SSBO_SET 2
#define SSBO_BINDING 3
#include "world_common.glsl"

layout(set = 2, binding = 0) uniform sampler2D u_tex;
layout(set = 2, binding = 1) uniform sampler2D u_dos_tab;
layout(set = 2, binding = 2) uniform sampler2D u_ptab;
#define VARY in
V_LOCATIONS
layout(location = 0) out vec4 o_color;

vec4 glFragCoord;   // gl_FragCoord with GL's origin (set first thing in main)

// the Titanium lights (mercsw 0x1002ecaa): u_tilit 1 objects, 2 the ground (one colour, 0x1005527e); the ambient, two
// world lights (type 1 directional - the unit vector the light travels along -, 2 point - its position), the
// explosion lights (u_tidp: position, radius; 0x1002f238)
// the engine's vector length (0x100462fe): the largest component + a quarter of the other two
float ti_len(vec3 v) { vec3 a = abs(v); float m = max(a.x, max(a.y, a.z)); return m + (a.x + a.y + a.z - m) * 0.25; }
vec3 ti_light(vec3 nn) {
  vec3 c = u_tiamb;
  for (int i = 0; i < 2; i++) {
    if (u_tilt[i] == 1) { float d = -dot(u_tilv[i].xyz, nn); if (d > 0.0) c += u_tilc[i].xyz * d; }
    else if (u_tilt[i] == 2) { vec3 l = u_tilv[i].xyz - v_world; float d = dot(l, nn); if (d > 0.0) c += u_tilc[i].xyz * d / max(ti_len(l), 1e-3); }
    else c += u_tilc[i].xyz;
  }
  c = min(c, vec3(1.0));
  // an explosion / projectile light lights an object's polygon if one of its vertices is inside the light's +-radius
  // box (0x1002eb36): here the object's bounding sphere's box against it (INFERRED); N.L / |L|, no falloff. The engine's
  // sums (0x1002f3f9..0x1002f5d5) are never cleared after the static lights: each vertex gets the running total, which
  // starts at the polygon's LAST vertex's lit colour - so a light that reaches an object adds that colour again on top
  // of its own (here this point's colour, INFERRED: the vertices' lit colours alike), clamped
  vec3 acc = c; bool hit = false;
  for (int j = 0; j < 8; j++) {
    if (j >= u_tidn) break;
    vec3 dd = abs(v_sph.xyz - u_tidp[j].xyz);
    if (v_sph.w > 0.0 && (dd.x > u_tidp[j].w + v_sph.w || dd.y > u_tidp[j].w + v_sph.w || dd.z > u_tidp[j].w + v_sph.w)) continue;
    vec3 l = u_tidp[j].xyz - v_world; float d = dot(l, nn);
    hit = true;
    if (d > 0.0) acc += u_tidc[j].xyz * d / max(ti_len(l), 1e-3);
  }
  return hit ? min(c + acc, vec3(1.0)) : c;
}
// the 3D engines' texturing per colour type (glr_view.ati_tex, bits 1 / 2 / 4): a bank-1 textured polygon's bits in
// -v_texsel above its texture switch class (set_layer): 4 affine, 8 no alpha mask, 16 colour types 5-11
// the Titanium edition (mercsw.dll): its vertex fog (u_tifog) - x 1 = on, y the full-fog depth Z (0: none), z 1 = per
// vertex (v_tivf, RENDITION) else per pixel, w the camera's factor; u_blend: alpha-blended (no alpha test), the
// texel's alpha out
// the DOS horizon fill (MW2.EXE 0x3c000): a pixel is sky when its view ray's world y >= 0 - ray = R (sx - cx) / fx +
// U (cy - sy) / fy + F; above the line a haze band of u_hz_band pixels fades from 0xEE (on the line) to 0xE0;
// u_hz_x: the window's left and right x; u_hz_smooth: the port's dos_sky=smooth (the band's colours blended)
// the DOS edition (MW2.EXE 0x38ae3 / 0x3e086 / 0x38ee0): v_color = (kind, base index, intensity C); kind 0 unlit
// (base is the index), 1 flat lit (base = ramp x 16), 2 textured (the texel's index through LUMA). The shade level =
// ((C >> 1) x ((f x (128 - amb) >> 7) + amb)) / 0x440, f = 127 x cos(light, normal), less one per fog distance of
// depth, clamped 0-15
vec3 dos_colour(float idx) { return texture(u_dos_tab, vec2((idx + 0.5) / 256.0, 16.5 / 17.0)).rgb; }
// DOS Gouraud (type 4: 0x3bb80 -> 0x3bd08 / 0x3bd69, the map mode's 0x12690 -> 0x1272d; both only with Gouraud on,
// DAT_00097024, else the flat fill 0x59180). The vertices: projected to whole pixels about the window's centre,
// ((p << s) / z + 2) >> 2 (0x3dfb7: quarter pixels, rounded), the shade c (16.16) = ramp x 16 + i (L + 1) / 16
// (0x3bd22; a < 48 as it is). MW2.EXE 0x59e77 then fills row by row from the top vertex (the last of the least y,
// 0x59f58) down two chains, one backward and one forward through the vertex list (0x59f9f / 0x5a03a, the first
// edges of no height skipped; later ones count 1 row, 0x5a61d), each edge starting at its upper vertex's x << 16 +
// 0x8000 and c + 0x8000 (0x5a01f / 0x5a0b7, 0x5a662) and stepping dx / dy, dc / dy per row (16.16, truncated; the
// last row steps on without switching, 0x5a33f). A row's span runs from the smaller chain's x >> 16 to the
// larger's (0x5a1d5), stepped per PIXEL PAIR: s = (cR - cL) / max(w >> 1, 1), w its last pixel - its first
// (0x5a1ea..0x5a202). Two accumulators run the pairs: the even pixel (from the span's own left end) at cL + DA,
// the odd at cL + DB, + s per pair (0x5a214..0x5a2fd); the colour is the byte of the integer part (the palette
// index). DA = 0x7fff (the callers' push, 0x3bd6b / 0x12723), DB = 0, swapped after every row (0x5a310..0x5a320;
// at the top clip by the rows skipped, 0x5a10b), the top row starting with DA; a left-clipped span keeps its
// phase (0x5a5c4). So pixel k of row r (from the unclipped span's left end / the polygon's top row): idx =
// (cL + floor(k / 2) s + (k + r even ? 0x7fff : 0)) >> 16 - a 1-pixel checkerboard of n / n + 1 where the shade's
// fraction is under 0.5, flat n + 1 above, its phase per polygon (DOSBox YELLSCN1 1024 x 768: the cockpit's
// checkerboards change phase at polygon edges). The port does it in integers per output pixel at any size (DOS
// draws it at its own resolution - 1024 x 768 one checker cell per pixel): the fragment reads its polygon
// (v_texsel - 1: its fan's first vertex in the layer's buffer; the triangles end at the one whose edge code has
// the closing bit), near-clips it (GL's plane, not DOS's 0x3de4e - near-clipped polygons approximate), rounds the
// vertices as 0x3dfb7 about the viewport's centre and walks the chains to its row; the pixels GL covers outside
// the DOS span take its end. dos_shading=smooth (not the original): the colours blended at c + 0.25 - the
// dither's mean - within the polygon's own shades
#if DOS_LOOK
vec4 dos_gclip(int i) { return u_mvp * vec4(dos_vpos(i), 1.0); }
float dos_gshade(int i, float g) { return dos_vf(i, 7) + dos_vf(i, 6) * g; }
int dos_q(float p) { return (int(trunc(p * 4.0)) + 2) >> 2; }   // 0x3dfb7: quarter pixels, + 2 >> 2
ivec2 dos_gvtx(vec4 q) { vec2 p = q.xy / q.w * 0.5 * u_vp; ivec2 c = ivec2(u_vp) / 2; return ivec2(c.x + dos_q(p.x), c.y - dos_q(p.y)); }
// one chain (dir -1: backward, the first, 0x5a5eb; + 1 forward, 0x5a689) at DOS row y: (x, c) in 16.16
ivec2 dos_gchain(ivec2 V[20], int C[20], int m, int top, int dir, int y, int ybot) {
  int a = top, b = (a + dir + m) % m, r0 = V[top].y;
  for (int g = 0; g < 20 && V[b].y == V[a].y && b != top; g++) { a = b; b = (a + dir + m) % m; }
  int cnt = V[b].y - V[a].y, yq = y == ybot && y > r0 ? y - 1 : y;
  for (int g = 0; g < 20 && yq >= r0 + cnt && b != top; g++) { r0 += cnt; a = b; b = (a + dir + m) % m; cnt = V[b].y - V[a].y; if (cnt == 0) cnt = 1; }
  if (cnt == 0) cnt = 1;
  uint t = uint(y - r0);
  return ivec2(int(uint(V[a].x * 65536 + 0x8000) + t * uint(dos_idiv((V[b].x - V[a].x) * 65536, cnt))),
               int(uint(C[a] + 0x8000) + t * uint(dos_idiv(C[b] - C[a], cnt))));
}
vec3 dos_gouraud(float lev) {
  float g = (lev + 1.0) / 16.0, px = floor(glFragCoord.x), py = floor(glFragCoord.y);
  float cc = v_gs.y + v_gs.x * g + 0.5, cmin = -1e9, cmax = 1e9;   // (no polygon: interpolated, screen parity)
  int k = int(px), r = int(py);
  uint val = 0u; bool exact = false;
  if (v_texsel > 0) {
    vec4 P[18]; float C[18]; int I[18]; ivec2 V[20]; int CF[20]; int n = 0, m = 0, s = v_texsel - 1, L1 = int(lev) + 1;
    for (int t = 0; t < 16; t++) {
      int b = s + 3 * t;
      if (t == 0) { P[0] = dos_gclip(b); C[0] = dos_gshade(b, g); I[0] = b; n = 1; }
      P[n] = dos_gclip(b + 1); C[n] = dos_gshade(b + 1, g); I[n] = b + 1; n++;
      if (((int(dos_vf(b, 12) + 0.5) % 32 / 3) & 2) != 0) { P[n] = dos_gclip(b + 2); C[n] = dos_gshade(b + 2, g); I[n] = b + 2; n++; break; }
    }
    cmin = 1e9; cmax = -1e9; bool nclip = false;
    for (int i = 0; i < n; i++) {   // the near plane (z + w >= 0), the shade carried
      vec4 a = P[i], b = P[(i + 1) % n]; float da = a.z + a.w, db = b.z + b.w;
      cmin = min(cmin, C[i]); cmax = max(cmax, C[i]);
      if (da < 0.0) nclip = true;   // (a clipped quad can keep its corner count: m == n is no test)
      if (da >= 0.0) { V[m] = dos_scr(dos_vpos(I[i])); CF[m] = int(floor(C[i] * 65536.0 + 0.5)); m++; }
      if ((da >= 0.0) != (db >= 0.0)) { float t = da / (da - db); V[m] = dos_gvtx(mix(a, b, t)); CF[m] = int(floor(mix(C[i], C[(i + 1) % n], t) * 65536.0 + 0.5)); m++; }
    }
    if (m >= 3) {
      int top = 0, ybot = V[0].y;
      for (int i = 0; i < m; i++) { if (V[i].y <= V[top].y) top = i; ybot = max(ybot, V[i].y); }   // 0x59f58: the last of the least y
      int y0 = int(u_vp.y) - 1 - (int(py) - int(u_vpo.y)), y = clamp(y0, V[top].y, ybot);   // the DOS row (y down)
      if (!nclip && y0 != y) discard;   // not near-clipped: the DOS span's rows only (see the vertex shader)
      ivec2 e1 = dos_gchain(V, CF, m, top, -1, y, ybot), e2 = dos_gchain(V, CF, m, top, 1, y, ybot);
      if (e2.x <= e1.x) { ivec2 t2 = e1; e1 = e2; e2 = t2; }   // 0x5a1d5
      int x0 = e1.x >> 16, w = (e2.x >> 16) - x0;
      int st = w != 0 ? dos_idiv(e2.y - e1.y, max(w >> 1, 1)) : 0;
      k = int(px) - int(u_vpo.x) - x0;
      if (!nclip && (k < 0 || k > w)) discard;   // and its pixels, both ends inclusive (0x5a213)
      k = clamp(k, 0, w); r = y - V[top].y;
      val = uint(e1.y) + uint(k >> 1) * uint(st); exact = true;
      cc = float(int(val)) / 65536.0;
    }
  }
  if (u_dos_smooth != 0) {
    float e = clamp(cc - 0.25, cmin, cmax), e0 = floor(e);
    return mix(dos_colour(e0), dos_colour(e0 + 1.0), e - e0);
  }
  if (exact) return dos_colour(float(((val + (((k + r) & 1) == 0 ? 0x7fffu : 0u)) >> 16) & 0xffu));
  return dos_colour(floor(cc + (((k + r) & 1) == 0 ? 32767.0 / 65536.0 : 0.0)));
}
#endif
void main() {
  glFragCoord = vec4(gl_FragCoord.x, u_fbh - gl_FragCoord.y, gl_FragCoord.zw);
  // the wireframe (image enhancement): only the polygons' own edges, 1 px - corner k's barycentric is the distance
  // to the edge opposite it (bit 0: corner 0's, bit 1: corner 1's, bit 2: corner 2's)
  if (u_wire.a > 0.0) {
    vec3 w = fwidth(v_bary); float d = 1.0e9;
    if ((v_emask & 1) != 0) d = min(d, v_bary.x / max(w.x, 1e-6));
    if ((v_emask & 2) != 0) d = min(d, v_bary.y / max(w.y, 1e-6));
    if ((v_emask & 4) != 0) d = min(d, v_bary.z / max(w.z, 1e-6));
    // hidden lines removed: a polygon's inside is drawn black (it hides what is behind it, as DOS); edges in the
    // layer's colour, an active building's (flag 8) in the mechs' blue
    if (d > 1.0) { o_color = vec4(0.0, 0.0, 0.0, 1.0); return; }
    int wc = (v_emask >> 3) & 7;   // 1 colour 7 blue, 2 0xb red, 3 3 yellow, 4 8 dark red (60,0,0)
    o_color = wc == 1 ? vec4(0.0, 48.0 / 255.0, 212.0 / 255.0, 1.0) : wc == 2 ? vec4(252.0 / 255.0, 24.0 / 255.0, 24.0 / 255.0, 1.0)
            : wc == 3 ? vec4(252.0 / 255.0, 204.0 / 255.0, 0.0, 1.0) : wc == 4 ? vec4(60.0 / 255.0, 0.0, 0.0, 1.0) : vec4(u_wire.rgb, 1.0); return; }
  if (u_shadow != 0) { o_color = vec4(0.0, 0.0, 0.0, 0.45); return; }
  if (u_horizon != 0) {
    float sx = glFragCoord.x, sy = glFragCoord.y;   // GL y up: sy grows upward
    vec3 ray = u_hz_r * (sx - u_hz_c.x) / u_hz_c.z + u_hz_u * (sy - u_hz_c.y) / u_hz_c.w + u_hz_f;
    if (ray.y < 0.0) { o_color = vec4(dos_colour(239.0), 1.0); return; }   // 0xEF ground
    float idx = 224.0;   // 0xE0 sky
    // the band (0x3c000 -> 0x59e77, only with Gouraud on, DAT_00097024): a quad from the line (corners 0xEE << 16,
    // DAT_00096eac - 1) up u_hz_band rows (corners 0xE0 << 16, DAT_00096ea8), its colour c = 238 - 14 d / band (d: the
    // rows above the line) stepped down the edges from c + 0x8000 (0x5a0xx), each row's pixels in pairs as c + 0x8000 and
    // c (in_stack 8 = 0x8000 from 0x3cc8b), the pair swapped every row (0x5a310): idx = floor(c + 0.5 + 0.5 chk) - a
    // half-step checkerboard, counted from the span's left end and the quad's top row (DOSBox YELLSCN1 1024 x 768:
    // every step one flat band and one 1-pixel checkerboard band of it and the next, row for row as here; the phase -
    // which pixel of a pair is the brighter - set to that capture, the GL row count being one off the quad's own). dos_sky=smooth: the same
    // colours blended at e = c + 0.25 (the staircase's mean: each flat band's middle = its own colour)
    if (u_hz_band > 0.0 && abs(u_hz_u.y) > 1e-6) {
      float sh = u_hz_c.y - u_hz_c.w * ((sx - u_hz_c.x) * u_hz_r.y / u_hz_c.z + u_hz_f.y) / u_hz_u.y;   // the line's row here
      float d = sy - sh;
      if (d >= 0.0 && d < u_hz_band) {
        float c = 238.0 - 14.0 * d / u_hz_band;
        if (u_hz_smooth != 0) {
          float e = clamp(c + 0.25, 224.0, 238.0), e0 = min(floor(e), 237.0);
          o_color = vec4(mix(dos_colour(e0), dos_colour(e0 + 1.0), e - e0), 1.0); return;
        }
        float shl = u_hz_c.y - u_hz_c.w * ((u_hz_x.x - u_hz_c.x) * u_hz_r.y / u_hz_c.z + u_hz_f.y) / u_hz_u.y;
        float shr = u_hz_c.y - u_hz_c.w * ((u_hz_x.y - u_hz_c.x) * u_hz_r.y / u_hz_c.z + u_hz_f.y) / u_hz_u.y;
        int row = int(floor(max(shl, shr) + u_hz_band - sy)), col = int(floor(sx - u_hz_x.x));
        idx = min(floor(c + (((row + col) & 1) != 0 ? 1.0 : 0.5)), 238.0);
      }
    }
    o_color = vec4(dos_colour(idx), 1.0); return;
  }
#if DOS_LOOK
  if (u_dos != 0) {
    int kind = int(v_color.r + 0.5); float base = floor(v_color.g + 0.5), C = floor(v_color.b + 0.5);
    if (kind == 0) { o_color = vec4(dos_colour(base), 1.0); return; }
    // kind 4: a type 3 billboard - its texel indices as they are (0x38ae3 returns a 0x3000 word unshaded)
    if (kind == 4) { vec4 tb = texture(u_tex, v_uv / u_texsize); if (tb.a < 0.5) discard; o_color = vec4(dos_colour(floor(tb.r * 255.0 + 0.5)), 1.0); return; }
    float nl = length(v_normal); vec3 Ld = u_dos_dir != 0 ? normalize(u_dos_lpos) : normalize(u_dos_lpos - v_world);
    // one-sided (0x3e1de): a polygon is drawn only when its normal faces the eye - (v0 - eye) . n < 0 - and lit by the
    // same normal, signed (0x3e086: 127 cos; behind the light below the ambient). The DOS normal (0x380a0 -> 0x3a4b9)
    // is the winding's: here -cross(v1 - v0, v2 - v0) of the file's vertices (the 3D editions' stored normals, which
    // the engine negates at load, 0x10042140, agree)
    vec3 nf = nl > 0.0 ? -v_normal / nl : vec3(0.0);
    if (nl > 0.0 && dot(v_viewpos, mat3(u_view) * nf) >= 0.0) discard;
    float f = nl > 0.0 ? trunc(127.0 * dot(nf, Ld)) : 127.0;
    float lev = floor(floor(C / 2.0) * (floor(f * (128.0 - u_dos_amb) / 128.0) + u_dos_amb) / 1088.0);
    if (u_dos_fogdist > 0.0) lev -= v_dkey >= 0.0 ? float((int(v_dkey) * 16 / int(u_dos_fogdist)) >> 4) : floor(max(-v_viewpos.z, 0.0) / u_dos_fogdist);
    lev = clamp(lev, 0.0, 15.0);
    if (kind == 1) { o_color = vec4(dos_colour(base + lev), 1.0); return; }
    // Gouraud (0x3bb80): each vertex's baked index a = ramp | i, rescaled by the polygon's level - ramp + i (L + 1) / 16 -
    // when a >= 48 (16.16 a >= 0x300000), else taken as it is: the fixed part in the vertex's v (ramp, or a), the scaled
    // part in its u (i, or 0); filled and dithered as 0x59e77 does (dos_gouraud). Flat-lit (types 1 / 2, 0x59180) and
    // textured (5-7: 0x5b549 affine, 0x3fb20 perspective) polygons have no dither in DOS
    if (kind == 3) { o_color = vec4(dos_gouraud(lev), 1.0); return; }
    vec4 tx = texture(u_tex, v_uv / u_texsize); if (tx.a < 0.5) discard;
    float ti = floor(tx.r * 255.0 + 0.5);
    float si = lev >= 15.0 ? ti : floor(texture(u_dos_tab, vec2((ti + 0.5) / 256.0, (lev + 0.5) / 17.0)).r * 255.0 + 0.5);
    o_color = vec4(dos_colour(si), 1.0); return;
  }
#endif
  // a Titanium colour kind 2 polygon (mercsw 0x1001b466 / 0x10053280): drawn only as the kind 1 shade its object's
  // texture switch makes it (its class in -v_texsel switched off, 0x1001b47a..0x1001b4f3) - or as an outline (above)
  if (v_ticm == 3 && ((-v_texsel) & u_notex) == 0) discard;
  vec3 base = u_mono.a > 1.5 ? v_color : u_mono.a > 0.0 ? u_mono.rgb : v_ticm >= 2 ? v_tic : v_color; float ta = 1.0; bool tinted = false;
  // the Combat Variables texture switches (0x1002a8a0 render flags DAT_1024e228, 0x1002a748..): a textured polygon whose
  // object class (-v_texsel: 1 OBJECT, 2 TERRAIN) is switched off is drawn in its texture's average colour (v_color)
  if (u_textured != 0 && u_mono.a <= 0.0 && (v_texsel >= 0 || ((-v_texsel) & u_notex) == 0)) {
    vec4 t; int af = u_ati != 0 && v_texsel < 0 ? -v_texsel : 0;
    // (u_ati bit 1) an affine polygon - q = 1 at every vertex (3Dfx 0x1002c880 / S3 0x1001da40 / Matrox 0x1005104c: oow
    // 1.0; ATi 0x1002def4 / 0x1002df03) or C3D_ERS_TMAP_PERSP_COR 0 (ATi 0x100686b0) - takes its texel uv screen-linear;
    // (bit 2) DISPLAY DETAIL low (3Dfx 0x1024e224 / S3 0x10061fc4 / Matrox 0x100adecc / ATi 0x1018a84c = 1) every type 5-11
    vec2 uv = ((u_ati & 1) != 0 && (af & 4) != 0) || ((u_ati & 2) != 0 && (af & 16) != 0) ? v_uvl : v_uv;
    if (u_mga_lod != 0) { float z = -v_viewpos.z; t = textureLod(u_tex, uv / u_texsize, z < 2500.0 ? 0.0 : z < 5000.0 ? 1.0 : z < 15000.0 ? 2.0 : 3.0); }
    else t = texture(u_tex, uv / u_texsize);
    // (u_ati bit 4) types 5-7 and the sky / ground fills drawn without the alpha flag: no alpha test / key - the alpha-0
    // texels in their own colour (ATi C3D_ERS_TMAP_TEXOP none; 3Dfx 0x10067240 / 0x100674c0 blend only with flag 2;
    // Matrox 0x100624a0 keys only with it, 0x1006256a)
    // u_blend 2 / 3 (glr_view.alpha_blend, 3Dfx 0x10067240): 2 an ARGB4444 cache entry, the texel's alpha squared
    // (grAlphaCombine(3, 4, 1, 1, 0)); 3 an ARGB1555 entry, the vertex alpha alone (u_alpha 230.4 / 255)
    if (u_blend == 3) {} else if (u_blend != 0) { if (t.a < 0.004) discard; } else if (t.a < 0.5 && ((af & 8) == 0 || (u_ati & 4) == 0)) discard; base = t.rgb * u_tint; ta = u_blend == 2 ? t.a * t.a : u_blend == 3 ? 1.0 : t.a; tinted = u_tinton != 0; }
  // an untextured polygon's palette colour toward the fade's target palette (its index + 1 in v_texsel)
  else if (u_pfade > 0.0 && v_texsel > 0 && v_bb == 0) base = mix(base, texture(u_ptab, vec2((float(v_texsel - 1) + 0.5) / 256.0, 0.5)).rgb, u_pfade);
  float n = length(v_normal);
  vec3 L = u_point != 0 ? normalize(u_lpos - v_world) : u_light;
  float diff = n > 0.0 ? abs(dot(v_normal / n, L)) : 0.5;   // two-sided: winding unknown
  // a tinted textured polygon (the 3D editions' palette fades, 0x1002c880 with DAT_1024e494): the tint is its vertex
  // colour - unlit. A mech location (type word 0x100) by its damage nibble n (0x1002b9b0, DAT_1024e428 = PLNT +52):
  // the light x (1 - n / 15), or x (1 + n / 15) when the planet's flag is set, at most full
  float ds = v_dnib > 0 ? (u_dsign != 0 ? 1.0 + float(v_dnib) / 15.0 : 1.0 - float(v_dnib) / 15.0) : 1.0;
  // Titanium: a polygon lit per vertex (v_ticm 1: the colour the lights gave its corners, interpolated - Gouraud, as
  // mercsw 0x1002ecaa / 0x1002f238 and dx5draw's SHADEMODE)
  vec3 lit = u_unlit != 0 || v_bb != 0 || tinted || (v_tif & 2) != 0 || v_ticm == 3 ? base : u_tilit == 1 ? base * (v_ticm == 1 ? v_tic : ti_light((v_tif & 1) != 0 ? v_normal : n > 0.0 ? v_normal / n : vec3(0.0)))
           : u_tilit == 2 ? base * u_tignd : base * min(1.0, (u_ambient + diff) * ds);   // SGL: ambient + parallel light, saturating
  // PowerVR PCX2 table fog (Imagination's Series 1 driver source, MIT: dlcamera.c sgl_set_fog, rncamera.c, simulat3
  // hwsabren.c Fog / texas.c): per pixel, from the depth 1/w: index i = depth >> shift = K / z (K below, z the view depth
  // in game units), the 8-bit fog f = PowerTable[i & 127] >> min(i >> 7, 9) with PowerTable[k] = 256 x 2^(-k/128)
  // (entry 0 = 255), so f = 0 from i = 1152 (near) up to 255 far; colour += (fog - colour) x f / 256
  float fz = max(-v_viewpos.z, 1.0);
  float fi = u_fog_density > 0.0 ? floor(u_fog_density / fz) : 1.0e9;
  float fk = mod(fi, 128.0), fs = floor(fi / 128.0);
  float ft = fk == 0.0 ? 255.0 : floor(256.0 * exp2(-fk / 128.0));
  float ff = fs >= 9.0 ? 0.0 : floor(ft / exp2(fs)) / 256.0;
  // Titanium (mercsw.dll): the vertex fog the D3D plugin takes as the specular alpha a (fog = 1 - a); the world, the
  // objects and the ground (0x10053474, 0x10001b53): a = (1 - min(1, z / Z)) x the camera's factor (density Y's
  // ground fog / low cloud, 0x1006a851); Z = 0: no distance fog
  if (u_tifog.x > 0.5) ff = u_tifog.z > 0.5 ? v_tivf : 1.0 - (u_tifog.y > 0.0 ? 1.0 - min(1.0, fz / u_tifog.y) : 1.0) * u_tifog.w;
  o_color = vec4(lit + (u_fog_color - lit) * ff, u_blend != 0 ? ta * u_alpha : u_alpha);
}
