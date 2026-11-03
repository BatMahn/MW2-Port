# Third-party components

The port's own source is GPL-2.0-or-later (LICENSE). The components it includes keep their licences:

| Component | Use | Licence |
|-----------|-----|---------|
| Nuked SC-55 (third_party/nuked-sc55/; below) | the Roland SC-55 menu music (not in the Windows legacy build) | GPL 2.0 or later |
| mt32emu, Munt (third_party/mt32emu/; below) | the Roland MT-32 menu music | LGPL 2.1 or later |
| miniaudio (third_party/miniaudio.*) | decoding the CD soundtrack (FLAC / MP3 / WAV) | public domain (Unlicense) or MIT-0 |
| stb_truetype (src/third_party/stb_truetype.h) | rendering the port's own settings text | public domain or MIT |
| Liberation Sans Bold (assets/fonts) | the port's own settings text, matched to the original artwork | SIL Open Font License 1.1 |
| SDL3 3.2 or newer (system library; libSDL3.so.0 / SDL3.dll / libSDL3.0.dylib shipped beside the game in the release builds) | window, input, audio output, the graphics (its GPU API: Direct3D 12, Vulkan, Metal) | zlib |

The shaders (shaders/) are compiled ahead of time into src/rb_shaders.c by scripts/gen_shaders.py with
glslangValidator (Khronos, BSD-3-Clause and others), SPIRV-Cross (Khronos, Apache 2.0) and the DirectX Shader Compiler
(Microsoft, University of Illinois/NCSA). None of them is part of the build or shipped: only their output - the port's
own shaders as SPIR-V, Metal source and DXIL - is in the source.

MechWarrior 2's own data (archives, art, music, text) is read from your copy of the game. The exceptions, all
Activision's and included only so owners of the game can rebuild a working setup: the 3D editions' data
(`gamedata/3d/`), the original game icon (`packaging/windows/`, the Windows exe icon) and the box art the macOS app
icon is made from (`packaging/macos/mw2-boxart.jpg`).

## mt32emu (Munt)

`third_party/mt32emu/` - the Roland MT-32 emulation library from the Munt project (https://github.com/munt/munt),
copyright Dean Beeler, Jerome Fisher, Sergey V. Mikayev and contributors, GNU Lesser General Public License 2.1 or later
(`third_party/mt32emu/COPYING.LESSER.txt`). Built as a static library for the menu music. The MT-32 ROMs are not
included: put your own MT32_CONTROL.ROM and MT32_PCM.ROM in the game folder's `mt32/` (or set `mt32=`).

## Nuked SC-55

`third_party/nuked-sc55/` - the Roland SC-55 emulator by nukeykt (https://github.com/nukeykt/Nuked-SC55, commit
cc801b26a83a25e6d8d1ed5edf2a91fdcc822ab6, 2026-09-12 - the archived repository's last), GNU General Public License
version 2.0 or later (`third_party/nuked-sc55/LICENSE`; upstream relicensed the project to GPL-2.0-or-later in 2026).
Copyright (C) 2021, 2024 nukeykt. Upstream's credits: John McMaster (SC-55 PCM chip decap), org/ogamespec (deroute tool),
the SDL team, Wohlstand (Linux / macOS port), mattw, HardWareMan, giulioz (JV-880 support), Cloudschatze, NikitaLita,
Karmeck (`third_party/nuked-sc55/README.md`).

Only the emulator core is taken - the H8/532 MCU (mcu*.cpp), the M37450 sub-MCU (submcu.cpp) and the PCM chip (pcm.cpp),
with the files' copyright headers. Left out: the SDL front end (audio output, the LCD window and its font, the button
handling), MIDI input (rtmidi / win32) and main(). Changed: mcu.cpp's SDL parts removed (the LCD writes go to no-op stubs,
the button state is a plain variable), its work-thread step and ROM loading turned into a small C interface for the port
(the end of mcu.cpp, `sc55_core.h`); mcu.h / submcu.cpp no longer include SDL headers. The emulation is unchanged. Built
as a static library (`build/libnsc55.a`) for the menu music (src/sc55mid.c); not in the Windows legacy build. The SC-55
ROMs are not included: put your own dumps in the game folder's `sc55/` (or set `sc55=`; FOLDER_SETUP.md).
