#version 450
#extension GL_GOOGLE_include_directive : require
// world.vert - the world renderer's vertex shader (src/glr.c; the OpenGL 3.3 program's VS, ported to SDL_GPU).
// Compiled twice: DOS_LOOK 0 (the 3D looks) and DOS_LOOK 1 (the DOS look's Gouraud fill, projection and fog key).
// Differences from the OpenGL program, none of them in what it computes:
//  - gl_VertexID is u_first + gl_VertexIndex: every draw starts at vertex 0 of its buffers bound at the batch's offset
//    (SV_VertexID / [[vertex_id]] / gl_VertexIndex agree only then), u_first being the batch's first vertex in the layer
//  - the depth: GL's clip z (-w..w) to SDL_GPU's 0..w at the end, so the depth buffer holds GL's window depth
#define UBO_SET 1
#define SSBO_SET 0
#define SSBO_BINDING 0
#include "world_common.glsl"

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec3 a_color;
layout(location = 4) in float a_texsel;
layout(location = 5) in float a_edge;
layout(location = 6) in vec4 a_sphere;
layout(location = 7) in vec4 a_tic;
#define VARY out
V_LOCATIONS

void main() {
  int vertex_id = u_first + gl_VertexIndex;
  int e = int(a_edge + 0.5), pit = e / 65536;
  e -= 65536 * pit;
  v_dnib = e / 2048; e -= v_dnib * 2048;   // a mech location's damage nibble (glr_set_damage_shade)
  v_bb = e >= 1024 ? 1 : 0;
  // Titanium (edge code + 256: its model normal, length kept; + 512: a flat palette colour, unlit)
  v_tif = 0;
  if (e < 1024 && e >= 512) { v_tif |= 2; e -= 512; }
  if (e < 1024 && e >= 256) { v_tif |= 1; e -= 256; }
  vec3 p = a_pos, nrm = a_normal;
  gl_Position = u_mvp * vec4(p, 1.0);
  // a colour type 3 billboard (edge code + 1024; engine 0x10026fc7 -> 0x1002a8e0, mode 0): a_pos the foot (texel u 0,
  // v > 0), a_normal the top (u 0, v 0), a_texsel the corner 0-3. Both are projected; with A the top's and B the foot's
  // screen point (y down) and d = (B - A) / 2 the corners are A + (-d.y, d.x), A + (d.y, -d.x), B + (d.y, -d.x),
  // B + (-d.y, d.x) - a square of the edge's screen length, centred on it, whatever the view's pitch or roll - texel
  // corners (0,0) (1,0) (1,1) (0,1); screen-linear (q = 1), no lighting (0x1002c880 with intensity -1: colour 255)
  if (e >= 1024) {
    vec4 cb = u_mvp * vec4(a_pos, 1.0), ca = u_mvp * vec4(a_normal, 1.0);
    int k = int(a_texsel + 0.5), tib = k >= 4 ? 1 : 0;
    e -= 1024; nrm = vec3(0.0); k -= 4 * tib;
    if (ca.w <= 1.0 || cb.w <= 1.0) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    else if (tib != 0) {
      // Titanium (mercsw 0x1004ef4c, mode 0): T the top's and F the foot's screen point, dx = (F.y - T.y) / 2, dy = (F.x -
      // T.x) / 2: corner 0 (T.x - dx, T.y - dy), 1 (F.x - dx, F.y + dy), 2 (T.x + dx, F.y - dy), 3 (F.x + dx, T.y + dy) -
      // the square of 0x10026fc7 for an upright edge, not square once it leans (the engine's own mix of T and F); the
      // corners' sz: 0 and 3 the top's, 1 and 2 the foot's (+0x18 of each, 0x1004f121..0x1004f1a5)
      vec2 h = 0.5 * u_vp, T = ca.xy / ca.w * h, F = cb.xy / cb.w * h;
      T.y = -T.y; F.y = -F.y;
      float dx = 0.5 * (F.y - T.y), dy = 0.5 * (F.x - T.x);
      vec2 c = k == 0 ? vec2(T.x - dx, T.y - dy) : k == 1 ? vec2(F.x - dx, F.y + dy) : k == 2 ? vec2(T.x + dx, F.y - dy) : vec2(F.x + dx, T.y + dy);
      gl_Position = vec4(c.x / h.x, -c.y / h.y, k == 0 || k == 3 ? ca.z / ca.w : cb.z / cb.w, 1.0);
      p = k == 0 || k == 3 ? a_normal : a_pos;
    } else {
      vec2 h = 0.5 * u_vp, A = ca.xy / ca.w * h, B = cb.xy / cb.w * h;
      A.y = -A.y; B.y = -B.y;
      vec2 d = 0.5 * (B - A), c = k == 0 ? A + vec2(-d.y, d.x) : k == 1 ? A + vec2(d.y, -d.x) : k == 2 ? B + vec2(d.y, -d.x) : B + vec2(-d.y, d.x);
      gl_Position = vec4(c.x / h.x, -c.y / h.y, k < 2 ? ca.z / ca.w : cb.z / cb.w, 1.0);
      p = k < 2 ? a_normal : a_pos;
    }
  }
  // the DOS Gouraud fill's coverage (MW2.EXE 0x59e77): a row's span runs from the left chain's x >> 16 to the right
  // chain's INCLUSIVE (0x5a1dc..0x5a213: w = xR - xL, w + 1 pixels) and the rows from the top vertex's to the bottom
  // vertex's inclusive (0xa178c = ybot - ytop, 0x5a337 / 0x5a33d), each chain from its vertex's x + 0.5 (0x5a01f) - about
  // half a pixel more on the right and below than GL's centre rule. So the polygon (its fan, every triangle) is drawn
  // 2 px larger on screen - each vertex pushed out along its two edges' outward normals (miter, at most 8 px) - and the
  // fragment keeps exactly the DOS span's pixels (dos_gouraud); polygons crossing the near plane are left as they are
#if DOS_LOOK
  if (u_dos != 0 && int(a_color.r + 0.5) == 3 && a_texsel > 0.5 && e < 256) {
    int s = int(a_texsel + 0.5) - 1, rel = vertex_id - s, t = rel / 3, cn = rel - 3 * t;
    vec2 Q[18]; int n = 0; bool ok = true; vec2 h = 0.5 * u_vp;
    for (int k = 0; k < 16; k++) {
      int b = s + 3 * k;
      for (int c = (k == 0 ? 0 : 1); c < 3; c++) {
        if (c == 2 && ((int(dos_vf(b, 12) + 0.5) % 32 / 3) & 2) == 0) break;
        vec4 wp = vec4(dos_vpos(b + c), 1.0), q = u_mvp * wp;
        if (q.z + q.w < 0.0 || q.w <= 0.0) ok = false; else { ivec2 sp = dos_scr(wp.xyz); Q[n] = vec2(float(sp.x) + 0.5, u_vp.y - float(sp.y) - 0.5) - h; }
        n++;
      }
      if (((int(dos_vf(b, 12) + 0.5) % 32 / 3) & 2) != 0) break;
    }
    int j = cn == 0 ? 0 : t + cn;
    if (ok && n >= 3 && j < n && gl_Position.w > 0.0) {
      float area = 0.0;
      for (int i = 0; i < n; i++) { vec2 a = Q[i], b = Q[(i + 1) % n]; area += a.x * b.y - a.y * b.x; }
      vec2 A = Q[(j + n - 1) % n], B = Q[j], C = Q[(j + 1) % n], e1 = B - A, e2 = C - B;
      float sg = area > 0.0 ? 1.0 : -1.0;
      vec2 n1 = length(e1) > 1e-4 ? sg * vec2(e1.y, -e1.x) / length(e1) : vec2(0.0), n2 = length(e2) > 1e-4 ? sg * vec2(e2.y, -e2.x) / length(e2) : vec2(0.0);
      vec2 m = n1 + n2; float d = 1.0 + dot(n1, n2);
      vec2 off = d > 1e-3 ? 3.0 * m / d : 3.0 * m;
      if (length(off) > 24.0) off = off / length(off) * 24.0;
      gl_Position.xy += off / h * gl_Position.w;
    }
  }
#endif
  // the DOS cockpit set: painted over everything, in the DOS draw order (MW2.EXE 0x3cea0 / 0x3d040: its list sorted
  // by each polygon's key, the farthest first, and drawn in turn) - a later polygon nearer in depth; one the original
  // leaves out (wholly behind the eye or off the window, 0x3e1de) is not drawn
  if (pit > 0 && u_dos != 0) {
    int rk = u_pit_rank(min(pit - 1, 127));
    if (rk < 0) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    else gl_Position.z = (-1.0 + 0.01 * (1.0 - float(rk + 1) / 256.0)) * gl_Position.w;
  }
  // the view distance (the planet's VIEW far; DOS MW2.EXE 0x3f500, 3Dfx 0x1002fba0): an object whose bounding sphere
  // lies wholly beyond it - centre distance > far + r, or depth - r > far - is not drawn (all its vertices off screen)
  if (u_far > 0.0 && a_sphere.w > 0.0) {
    vec3 sc = (u_view * vec4(a_sphere.xyz, 1.0)).xyz;
    if (dot(sc, sc) > (u_far + a_sphere.w) * (u_far + a_sphere.w) || -sc.z - a_sphere.w > u_far) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
  }
  // the DOS fog key (MW2.EXE 0x3e1de -> *0x97080 = 0x38ae0 -> 0x38ee0): one per polygon - the largest of its corners'
  // camera z (quarter units: dos_camz; the list key DAT_0014fcc0 = 0) - and the level less ((key << 4) / LITE +24) >> 4,
  // not the pixel's depth in cm. The polygon's corners from this layer's buffer: its fan triangles back to the one that
  // starts it (edge bit 4) and on to the one that closes it (bit 2); -1 (the pixel depth) for a vertex not found there
  v_dkey = -1.0;
#if DOS_LOOK
  if (u_dos != 0) {
    int vid = vertex_id, t = vid / 3, k, c;
    if (abs(dos_vf(vid, 0) - a_pos.x) + abs(dos_vf(vid, 2) - a_pos.z) < 0.01) {
      for (k = 0; k < 16 && t > 0; k++) { if (((int(dos_vf(3 * t, 12) + 0.5) % 32 / 3) & 4) != 0) break; t--; }
      float km = -1e9;
      for (k = 0; k < 16; k++) {
        int b = 3 * (t + k);
        for (c = 0; c < 3; c++) km = max(km, dos_camz(dos_vpos(b + c)));
        if (((int(dos_vf(b, 12) + 0.5) % 32 / 3) & 2) != 0) break;
      }
      v_dkey = km;
    }
  }
#endif
  v_viewpos = (u_view * vec4(p, 1.0)).xyz; v_world = p;
  // the Titanium vertex fog worked out at the vertex (u_tifog.z = 1: the RENDITION renderer - zredline.dll sends each
  // vertex's fog byte, 0x100056d8, and the Verite iterates it across the polygon, screen-linear INFERRED)
  { float vz = max(-v_viewpos.z, 1.0); v_tivf = 1.0 - (u_tifog.y > 0.0 ? 1.0 - min(1.0, vz / u_tifog.y) : 1.0) * u_tifog.w; }
  v_normal = nrm; v_uv = a_uv; v_color = a_color; v_texsel = int(floor(a_texsel + 0.5)); v_sph = a_sphere;
  v_tic = a_tic.rgb; v_ticm = int(a_tic.a + 0.5); v_gs = a_uv; v_uvl = a_uv;
  int hi = e / 32; e -= hi * 32; int corner = e - 3 * (e / 3);
  v_bary = vec3(corner == 0 ? 1.0 : 0.0, corner == 1 ? 1.0 : 0.0, corner == 2 ? 1.0 : 0.0); v_emask = (e / 3) | (hi << 3);
  // the depth: GL's window z (z / w x 0.5 + 0.5, its viewport transform for the depth range 0..1) times w - SDL_GPU's
  // clip z runs 0..w and its rasterizer divides by w again, giving GL's depth back to within the last bit
  gl_Position.z = (gl_Position.z * (1.0 / gl_Position.w) * 0.5 + 0.5) * gl_Position.w;
}
