#version 450
// hud.frag - the cockpit HUD (src/hud.c; the OpenGL program's FS). u_sharp > 0 (the font): "sharp bilinear" - each
// texel a solid block, only its edges blended over one screen pixel (u_sharp = screen pixels per texel), so 1-pixel
// strokes stay crisp at any scale, as DirectDraw blitted them 1:1
layout(location = 0) in vec2 t;
layout(location = 0) out vec4 c;
layout(set = 2, binding = 0) uniform sampler2D u_tex;
layout(std140, set = 3, binding = 0) uniform HudDraw {
    vec4  u_tint;
    vec4  u_flash;
    int   u_textured; int u_smooth; float u_sharp;
};
vec2 sharp(vec2 uv) { vec2 sz = vec2(textureSize(u_tex, 0)); vec2 p = uv * sz - 0.5; vec2 i = floor(p);
  vec2 f = clamp((fract(p) - 0.5) * u_sharp + 0.5, 0.0, 1.0); return (i + f + 0.5) / sz; }
void main() { c = (u_textured != 0 ? texture(u_tex, u_sharp > 0.0 ? sharp(t) : t) : vec4(1.0)); if (u_smooth != 0) c.a = sqrt(c.a); c *= u_tint; if (c.a < 0.01) discard; c.rgb = mix(c.rgb, u_flash.rgb, u_flash.a); }
