MechWarrior 2 port - the LEGACY Windows build (windows-x86-legacy)

This build is for Windows XP-era PCs: 32-bit Windows XP SP3 or newer, a Pentium 4 / Athlon 64 or newer CPU (SSE2)
and a graphics card with an OpenGL 1.2 driver (or OpenGL 1.1 with GL_EXT_texture_edge_clamp) - GeForce 2 and newer,
Radeon 7000 and newer, Intel GMA 900 and newer, 3dfx Voodoo 3-5. It draws with OpenGL 1.2's fixed-function pipeline
and runs on SDL2 (SDL2.dll, beside mw2.exe).

A modern PC (Windows 10 / 11, 64-bit) should use the normal build (windows-x64): Direct3D 12 or Vulkan, multisampling.

Install the graphics card's own driver: Windows' built-in OpenGL (the "GDI Generic" software renderer, OpenGL 1.1)
is not enough. Not yet tested on real Windows XP hardware - reports welcome. See README.md (System requirements).
