#version 450
// hud.vert - the cockpit HUD's sprites, text and lines (src/hud.c): positions already in clip space
layout(location = 0) in vec2 p;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec2 t;
void main() { t = uv; gl_Position = vec4(p, 0.5, 1.0); }
