#version 450
// clear.vert - a clear of part of a target (glClear inside a scissor): one triangle over the viewport at the far depth
void main() { vec2 q = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)); gl_Position = vec4(q * 2.0 - 1.0, 1.0, 1.0); }
