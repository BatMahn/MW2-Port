#version 450
// clear.frag - the clear colour
layout(location = 0) out vec4 c;
layout(std140, set = 3, binding = 0) uniform ClearDraw { vec4 u_color; };
void main() { c = u_color; }
