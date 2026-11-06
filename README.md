# MechWarrior 2 (DOS, 1995) - native port

A native reimplementation of MechWarrior 2: 31st Century Combat, rebuilt module by module from the original
MW2.EXE / MW2SHELL.EXE and the 3D editions' MW2.DLL. Portable C99, builds on macOS, Linux and Windows - 64-bit, and
(from v1.1.2) a 32-bit legacy build for Windows XP-era PCs, so the port is no longer 64-bit only. You need your own copy
of the game (see "Setting up the game folder").

## Status (2026-11-03, v1.1.0 in testing)
Complete and playable on macOS, Linux and Windows. Tagged releases (`v*`) are built by GitHub Actions: a macOS app,
a Linux tarball, a Fedora RPM, a Windows zip and a Windows legacy zip (32-bit, Windows XP and newer, OpenGL 1.2).

- **Shell:** clan halls, both campaigns, Instant Action, Cadet Training, Mech Lab, Combat Variables, Cockpit Controls,
  Hall of Honor, the roster and both clans' Archives. The pilot-name easter-egg mechs (Elemental, Tarantula, Battle
  Master IIC) are unlocked everywhere at their real tonnage.
- **Simulation:** every mission, with AI, mission logic, combat, heat, movement, jump jets, HUD, autopilot and
  path-driven dropships / convoys; dropships and gunships fly at their original heights. Locked missiles home on the
  target's centre; destroyed buildings stay destroyed.
- **Look:** chosen in Combat Variables with two settings. RENDERER: ENHANCED (the default: 3Dfx look + PowerVR fog), DOS,
  3DFX, ATI RAGE, S3 VIRGE, POWERVR, MATROX MYSTIQUE or RENDITION (the Titanium disc's Rendition Verite renderer; needs the Titanium files). TEXTURES: ORIGINAL (each renderer's own textures, sky and
  ground) or TITANIUM (with the Titanium disc's files, its own models and lighting). Each look follows its own edition's
  code: the DOS look's cockpit and HUD match DOSBox pixel for pixel; the 3D looks draw, filter and light their textures
  as their editions did (the Rendition look from the code only - no hardware to compare with).
- **Graphics (from v1.1.0):** drawn through SDL3's GPU API - Direct3D 12 or Vulkan on Windows, Vulkan on Linux, Metal
  on macOS. The legacy Windows build (from v1.1.2) draws with OpenGL 1.2's fixed-function pipeline instead, for the
  cards of the XP years (see "System requirements").
- **Sound:** the original's volumes, stereo pans and distance delay; CD music from your own rips; MIDI through the
  MT-32 emulator, the Roland SC-55 emulator (Nuked SC-55, from v1.1.3) or Gravis UltraSound patches (the GUS synth
  follows the original ULTRA.MDI driver).
- **Input:** keyboard, mouse and joystick, with the original's key rates and mouse scaling; the external camera (C) and
  every view control as in the original.

Game values are traced from the original code or measured in DOSBox. What is still assumed is listed at the end of
ASSUMPTIONS.md ("Open items"); [KNOWN_ISSUES.md](KNOWN_ISSUES.md) is the plain-language list of what is still open.
AUDIT.md is the audit log. Test suites: `make check`, `make test_sequences` (scripted play sequences, 270 of them) and
`make sweep` (every mission's objectives).

## System requirements

The normal builds (64-bit) draw through SDL3's GPU API, so they need a graphics card and driver for one of these:

| System | Graphics interface | Minimum |
|---|---|---|
| Windows 10 / 11 | **Direct3D 12** (the default) or **Vulkan** - Combat Variables (the RESOLUTION row's right half) or `gpu=` in mw2port.cfg; if the chosen one can't start, the other is used | Direct3D 12 feature level 11_0: NVIDIA GeForce GTX 600 series (Kepler, 2012) or newer, AMD Radeon HD 7700 series (GCN, 2012) or newer, Intel HD Graphics 4200-5200 (Haswell, 4th-gen Core, 2013) or newer. Vulkan 1.0: NVIDIA GeForce GTX 600 or newer, AMD Radeon HD 7700 or newer, Intel HD Graphics 500 (Skylake, 6th-gen Core, 2015) or newer |
| Linux | **Vulkan** | Vulkan 1.0 with `VK_KHR_swapchain` and `VK_KHR_maintenance1`, and the features independentBlend, imageCubeArray and sampleRateShading - every desktop Vulkan driver: NVIDIA GeForce GTX 600 or newer, AMD Radeon HD 7700 (GCN) or newer (Mesa's RADV), Intel Broadwell (5th-gen Core) or newer (Mesa's ANV; Ivy Bridge / Haswell through Mesa's hasvk, untested). Needs the Vulkan loader (`libvulkan1` / `vulkan-loader`) and the driver (`mesa-vulkan-drivers`, or NVIDIA's) |
| macOS | **Metal** | macOS 10.14 or newer on a Metal GPU (Apple Silicon, or an Intel Mac from 2012 on). The release build is for Apple Silicon |

Older cards - the GeForce 8 to 500 series, Radeon HD 2000 to 6000, Intel before Haswell (Windows) - can't run it.
On Windows, install the card maker's driver: Windows' basic display driver has neither Direct3D 12 nor Vulkan. When
neither starts, the game says so in a message box; on Linux, `vulkaninfo --summary` (package vulkan-tools) shows the
Vulkan drivers installed. The game prints which interface it started (`gpu: Vulkan (...)` on stdout). These minimums
are with SDL3 3.4 (the release builds); built against an older SDL3, Vulkan also needs depthClamp, shaderClipDistance and
drawIndirectFirstInstance, Direct3D 12 resource binding tier 2 (no Haswell / Broadwell) and Metal a Mac2-family GPU. A grey sky and
ground with untextured models means the `3d/` folder is missing, not an old card (see "The 3D data" below).

### The legacy build (older PCs)

For PCs those minimums leave out there is a legacy Windows build (`windows-x86-legacy` zip): the same game, drawn with
OpenGL 1.2's fixed-function pipeline (docs/RENDERER_GL12.md) and running on SDL2 (SDL3 does not run on Windows XP).
A modern PC should use the normal build.

| | Legacy build (`windows-x86-legacy`) |
|---|---|
| System | Windows XP SP3 or newer, 32-bit (runs on 64-bit Windows too) |
| CPU | Pentium 4 / Athlon 64 or newer (SSE2: the simulation's arithmetic is the 64-bit builds') |
| Graphics | an OpenGL 1.2 driver, or OpenGL 1.1 with `GL_EXT_texture_edge_clamp` (then the Matrox look's mip bands are left out) - the card maker's driver, not Windows' built-in OpenGL 1.1 software renderer. Typical cards: GeForce 2 and newer, Radeon 7000 and newer, Intel GMA 900 and newer, 3dfx Voodoo 3-5 (256 x 256 textures: the HUD's larger art is tiled, larger world textures scaled down) |
| Colour | 32-bit desktop with a 24-bit depth buffer, else 16-bit (5-6-5) with a 16-bit depth buffer |

The legacy build has no multisampling (the RESOLUTION row's AA steps do nothing) and shows `GPU OPENGL`; the menus
draw with SDL's OpenGL renderer (its software renderer if that can't start). It prints `gpu: OpenGL (...)` with the
card, the driver's version, the colour / depth bits and the texture limit. Not yet tested on real Windows XP hardware
(KNOWN_ISSUES.md). The legacy build has no Roland SC-55 menu music (the emulator needs most of a modern CPU core, and it
keeps that build lean): it plays the menu music on the MT-32 or the Gravis UltraSound patches.

## Build
Needs a C99 compiler, make, pkg-config and SDL3 3.2.0 or newer (development files; SDL3 brings its GPU API's Direct3D 12,
Vulkan and Metal backends and loads them at run time - no graphics headers or libraries to install). SDL3 is found
with `pkg-config sdl3`. The shaders are compiled into the source (src/rb_shaders.c): no shader compiler is needed; after
changing one (shaders/), `make shaders` regenerates it (glslangValidator, spirv-cross and dxc - see
scripts/gen_shaders.py).

    macOS:                  xcode-select --install; brew install sdl3 pkgconf
    Linux (Fedora 42+):     sudo dnf install gcc gcc-c++ make pkgconf-pkg-config SDL3-devel
    Linux (Debian 13 / Ubuntu 25.04+):  sudo apt install build-essential pkg-config libsdl3-dev

    make && make mw2        # the tools, then the game (mw2: menus + missions in one program)

Older Linux releases (Ubuntu 24.04 and earlier) have no SDL3 package: build it from the release source
(https://github.com/libsdl-org/SDL/releases, `SDL3-<version>.tar.gz`) with CMake - `cmake -S SDL3-<version> -B build
-DCMAKE_INSTALL_PREFIX=$HOME/sdl3 && cmake --build build && cmake --install build` - then `make mw2 SDL3_DIR=$HOME/sdl3`
(`SDL3_DIR` = the prefix whose `lib/pkgconfig` holds `sdl3.pc`; `PKG_CONFIG_PATH` works too). A build left over from
the SDL2 version: `make clean` first.

Windows (64-bit) builds with MinGW-w64: in MSYS2's MINGW64 shell
(`pacman -S make mingw-w64-x86_64-gcc mingw-w64-x86_64-pkgconf mingw-w64-x86_64-sdl3`) just `make mw2`; or
cross-compile from Linux with the SDL3 MinGW development package (`SDL3-devel-<version>-mingw.tar.gz`, as the release
workflow does):

    make mw2 WINDOWS=1 CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ AR=x86_64-w64-mingw32-ar \
             SDL3_DIR=/path/to/SDL3-<version>/x86_64-w64-mingw32

That gives `mw2.exe`; put `SDL3.dll` (the package's `x86_64-w64-mingw32/bin/SDL3.dll`) beside it. Only the game
(`mw2`) is set up for Windows, not the other tools. Run `make clean` when switching between a Windows and a native
build.

The legacy build (32-bit, Windows XP SP3 and newer) cross-compiles with MinGW-w64's i686 compiler and the SDL2 MinGW
development package (`SDL2-devel-2.32.10-mingw.tar.gz`, its `i686-w64-mingw32` folder; SDL2 2.0.18 is the oldest the
port builds with):

    make mw2 WINDOWS=1 RB=gl12 SDL=2 CC=i686-w64-mingw32-gcc CXX=i686-w64-mingw32-g++ AR=i686-w64-mingw32-ar \
             SDL2_DIR=/path/to/SDL2-2.32.10/i686-w64-mingw32

and put `SDL2.dll` (that folder's `bin/SDL2.dll`) beside `mw2.exe`. `RB=gl12` picks the OpenGL 1.2 renderer
(src/rb_gl12.c), `SDL=2` SDL2 (src/sdlcompat.h maps the SDL3 calls); the Makefile then adds `-msse2 -mfpmath=sse
-mstackrealign` and `_WIN32_WINNT=0x0501` (XP). Each also works on its own for testing: `make mw2 RB=gl12` on Linux or
macOS (system OpenGL; Mesa's llvmpipe headless), `SDL=2` with the system's SDL2.

## Setting up the game folder
The port needs the original game's files in one folder (called `MW2-game` below). Copy these from your own copy.
Only the 3D editions' data (`3d/`) is included in this repository; everything else comes from your own copies.
**See [FOLDER_SETUP.md](FOLDER_SETUP.md) for a complete example of a working folder and the common mistakes.**

    MW2-game/
      install/    REQUIRED - the contents of your installed DOS game folder (C:\MECH2 after running the game's
                  INSTALL): the *.CFG settings, MW2REG.CFG (pilots), the *.BWD lance files, SETUP\, SND\ ... Copying
                  the whole folder is fine. The port writes pilots, saves and settings here.
      cd/         REQUIRED - the contents of the ORIGINAL DOS game CD (1995, not the Titanium disc): the disc's
                  MECH2\ folder (MW2.PRJ, DATABASE.MW2, ...), SMK\ (movies), KEATING\ (training voices) and the
                  rest, copied as they are on the disc. Or set cd=/path/to/mw2cd.iso in the settings file to read
                  straight from a disc image (see "Your disc image")
      mt32/       REQUIRED for the menu music - the Roland MT-32 ROMs: MT32_CONTROL.ROM and MT32_PCM.ROM
                  (without them the menus fall back to an SC-55 in sc55/ or Gravis UltraSound patches in
                  ultrasnd/, if present)
      music/      optional - the CD soundtrack (Track02..Track27 as FLAC, MP3 or WAV; see "CD music")
      sc55/       optional - Roland SC-55 ROM dumps (menu music on an emulated Sound Canvas; see below)
      ultrasnd/   optional - Gravis UltraSound patches (menu music without the MT-32 ROMs; see below)
      3d/         the 3D editions' data - included in this repository under gamedata/3d/: copy that folder here
      titanium/   optional - two files from the TITANIUM EDITION disc (1998), for the Titanium textures (see below)

### Which disc goes where
- **The original DOS CD** (MechWarrior 2: 31st Century Combat, 1995) goes in `cd/`, and its installed folder in
  `install/`. This is the game itself: the missions, mechs, movies, voices and settings all come from it.
- **The Titanium Edition disc** (1998, Windows) goes in `titanium/` - only two files from it, and only for the TITANIUM
  textures. It can't stand in for the original CD: its data is a different, later build. Copy just these files:

      Titanium disc  Install\16bit\MW2.PRJ     ->  MW2-game/titanium/MW2.PRJ
      Titanium disc  Install\16bit\SKYGND.PAR  ->  MW2-game/titanium/SKYGND.PAR
      Titanium disc  Install\16bit\particle.PID ->  MW2-game/titanium/particle.PID   (missile smoke; optional)

  Without `titanium/` everything works; TEXTURES just stays ORIGINAL.
- **The 3D-card editions' files** (3Dfx, ATi, S3, PowerVR, Matrox) are included in this repository - copy
  `gamedata/3d/` to `3d/` (next section).

### The 3D data (included here)
The textured looks come from the 3D-accelerator editions of MechWarrior 2 - the 3Dfx, ATi Rage, S3 ViRGE, PowerVR and
Matrox Mystique bundles that shipped with graphics cards in 1996-97. Those discs are hard to find now, so their data
files are included in this repository under `gamedata/3d/` rather than leaving you to track them down. Copy the folder
into your game folder:

    cp -R gamedata/3d /path/to/MW2-game/3d

`gamedata/3d/` holds models.prj (the 3Dfx edition's MW2.PRJ), textures.prj (the ATi edition's MW2.PRJ), skygnd.par, and
the S3 / PowerVR / Matrox editions' archives and settings (s3/, pvr/, mga/, ati/) for those looks. Without 3d/ the
game uses the DOS models only. These files are Activision's (see "Licences"); they are here only so owners of the game
can rebuild a working setup.

**The 3D renderers need the `3d/` folder** (`gamedata/3d/` from this repository, copied into your game folder - see
[FOLDER_SETUP.md](FOLDER_SETUP.md)). Without it the game uses the DOS renderer whatever is chosen, and COMBAT VARIABLES
shows RENDERER as "DOS - NO 3D FOLDER". (Before v1.0.2 a 3D renderer without it drew grey sky and ground and untextured
models.)

### The Titanium textures (optional, from your own disc)
If you own *MechWarrior 2: 31st Century Combat - Titanium Edition* (1998), the game can also use its textures, sky dome
(clouds, mountains, sun and moon), ground, fog, lights, models and missile smoke. Copy these files from the disc's
`Install\16bit\` folder (the 16-bit build, not `Install\8bit\`) into a `titanium/` folder in your game folder:

    MW2-game/titanium/MW2.PRJ       (Install/16bit/MW2.PRJ, 64 MB)
    MW2-game/titanium/SKYGND.PAR    (Install/16bit/SKYGND.PAR)
    MW2-game/titanium/particle.PID  (Install/16bit/particle.PID - the missile smoke; optional)

(or set `titanium=/path/to/folder` in the settings file). COMBAT VARIABLES -> TEXTURES (and Textures in the
in-mission Graphics menu) then switches between ORIGINAL and TITANIUM; without the folder, and with the DOS renderer,
TEXTURES stays ORIGINAL (greyed). TITANIUM goes with any 3D renderer: the Titanium textures, sky dome and ground drawn
with that renderer's filtering; the renderers that have fog (ENHANCED, POWERVR) draw the Titanium fog (its own colours
and depths, made for its sky), the others (3DFX, ATI RAGE, S3 VIRGE, MATROX MYSTIQUE) none, as in their editions; the
Titanium effects are always alpha-blended (the Titanium engine's choice for its textures). The missions, models and
data stay the renderer's own: Titanium renamed most of its mech and building textures, so those keep the renderer's
textures of the same name - the effects, the grounds and the sky come from the Titanium files. RENDERER RENDITION is the
Titanium edition's own Rendition Verite renderer: it always uses the Titanium textures (TEXTURES shows TITANIUM,
greyed), with bilinear filtering without mipmaps, the Titanium fog worked out per vertex, and missile smoke drawn
without a colour key, as that card drew it.

### Gravis UltraSound patches (optional)
Without the MT-32 ROMs the menu music can play through the Gravis UltraSound's General MIDI patch set instead. Get the
GUS driver / install disks (the 4.x "ULTRASND" package, easy to find on VOGONS) and extract them; what the port needs is
the resulting `ULTRASND` folder - in particular `MIDI/ULTRAMID.INI` and the `MIDI/*.PAT` patch files. Copy the whole
folder in as `ultrasnd/`:

    MW2-game/ultrasnd/MIDI/ULTRAMID.INI
    MW2-game/ultrasnd/MIDI/ACPIANO.PAT  ... (about 280 .PAT files)

Running the DOS installer is not needed; extracting the archives is enough. With both present, the MT-32 is used
(unless Combat Variables' MUSIC entry or `menu_music=gus` chooses the GUS - see "Roland SC-55" below).

### Roland SC-55 (optional)
The DOS game also plays its menu music in General MIDI, through Miles' MPU401.MDI driver ("the Roland Sound Canvas,
RAP-10, and compatibles"). The port can play that song set on an emulated Roland SC-55 - Nuked SC-55, built in (not in
the Windows legacy build). The SC-55's firmware and wave ROMs are Roland's and are **not included**: put your own dumps
in a `sc55/` folder in the game folder (or set `sc55=/path/to/folder`), named as Nuked SC-55 names them, e.g. for an
SC-55mk2 v1.01:

    MW2-game/sc55/rom1.bin  rom2.bin  rom_sm.bin  waverom1.bin  waverom2.bin

(an original SC-55: `sc55_rom1.bin sc55_rom2.bin sc55_waverom1.bin sc55_waverom2.bin sc55_waverom3.bin`; every model's
names and Roland part numbers are in [FOLDER_SETUP.md](FOLDER_SETUP.md)). The model is recognised from the file names.

Which device plays the menu music: in the game, Combat Variables - the entry right of DIFFICULTY, under the MUSIC
slider (MUSIC AUTO / MUSIC MT-32 / MUSIC SC-55 / MUSIC GUS; each click the next, only the devices whose files are there;
greyed when there is only one). The change takes effect when you leave Combat Variables: the song stops, the chosen
device starts and the song playing before starts again on it (the SC-55 after its boot, about a second). Or
`menu_music=` in the settings file - `auto` (the default) takes the first whose files are there, in the order MT-32,
SC-55, Gravis UltraSound, so adding `sc55/` changes nothing while `mt32/` is there; `menu_music=sc55` (or `mt32`, `gus`)
picks one, and falls back to auto's choice if its files are missing. The start-up line says which: `menu music Roland
SC-55mk2 (ROMs from ...)`.

The emulator runs the unit's chips cycle by cycle and needs a good part of one modern CPU core while music plays (it
runs on its own thread, a short queue ahead of the sound output; it sleeps when no music plays, e.g. during missions).
It boots for about a second of emulated time when the game starts, so the first menu song can start a moment late.

Then run the game once with the folder; it remembers it:

    ./mw2 /path/to/MW2-game     # afterwards just ./mw2

If no folder is given, `mw2` looks in the current folder, next to itself (the macOS app: next to
`MechWarrior 2.app`), one level up (`MW2-game`) and in `~/MW2-game`; if it finds none it asks for the folder with the
system's folder picker (Linux: zenity or kdialog, when installed). Settings live in `~/Library/Application Support/mw2port/mw2port.cfg` (macOS) or
`~/.config/mw2port/mw2port.cfg` (Linux).

Optional: `make check MUSIC=/path/to/MW2-game/music MW2=/path/to/MW2-game/install` runs the test suite against your
files.

## Your disc image
Convert a BIN/CUE image once:

    ./cdrip mw2.cue ~/mw2-cd
    # -> ~/mw2-cd/mw2cd.iso and ~/mw2-cd/music/Track02.wav ... Track27.wav

cdrip repairs a known ripping fault: audio stored as scrambled data sectors,
with the first 3 samples of each sector overwritten. It descrambles them,
rebuilds the lost samples and conceals read-error clicks, and reports which
tracks needed it. Clean tracks are copied bit-exact. Convert the WAVs to FLAC
or MP3 if you like; the drive plays any of the three.

## Game data (no CD needed)
    MW2_INSTALL_DIR=~/games/MECH2      install folder (searched first)
    MW2_CD_IMAGE=~/mw2-cd/mw2cd.iso    disc data, read straight from the image
    MW2_CD_DIR=...                     or a folder copy of the disc instead

Filenames match case-insensitively.

## CD music
Put your rip in one folder as FLAC, MP3 or WAV. Track numbers come from the
filenames (`Track02.flac`, `02 - Title.mp3`, `mw2_02.wav` all mean track 2).
On the retail disc the music starts at track 2, because track 1 is data.
If your rip renumbered the music from 1, set `MW2_CDA_TRACK_OFFSET=1`.
For a table of contents identical to your disc, set `MW2_CDA_DATA_SECTORS` to
the value cdrip prints (56187 for the MECH2_16B pressing).

    export MW2_MUSIC_DIR=~/mw2-music   # where the game will look for the soundtrack
    ./cdtool toc  ~/mw2-music          # show the TOC the game will see
    ./cdtool play ~/mw2-music 2        # listen to a track

The game plays music by sector address from the TOC, not by track number.
cdaudio builds a disc-accurate TOC from the files' lengths, so those requests
land on the right music. Play/pause/resume, track-to-track continuation,
the "busy" flag the game polls to loop music, and per-channel volume all
behave as MSCDEX does.

## Tools
- `glview 3DFX.PRJ ATI.PRJ ati @ CYAN` plays the mission in a Timber Wolf with the game's controls (GAMEKEY.MAP /
  INPUT.MAP, see "Customising controls"); with `MW2_DEBUG_KEYS=1` G toggles a free camera (then WASD/Q/E fly) and [ ] the fog.
- `glview 3DFX.PRJ ATI.PRJ ati [MECH MISSION]` - interactive viewer (SDL3's GPU API). Drag to orbit, wheel to
  zoom, F fullscreen at desktop resolution, Tab mech, N mission, C camo (0-7), I clan insignia, B filtering,
  M anti-aliasing, [ ] fog, Space walk animation; in world view P runs the mission (camera = player). Build with `make gl` (needs SDL3 dev files).
- `glshot ... out.ppm W H` - the same renderer headless, any resolution (no window: the GPU's off-screen picture;
  `SDL_VIDEODRIVER=offscreen` on a machine without a display).
- Sky/ground/fog: set `MW2_SKYGND` to the 3dfx edition's skygnd.par and `MW2_SKYGND_FOG`
  to the PowerVR edition's (the only one with fog values).
- `mechview3d 3DFX.PRJ ATI.PRJ ati TIMBRWLF JSCAMO_F L1WOLFCL out.ppm` renders a mech in
  the 3D-edition look (models from the first archive, textures from the second).
- `mechview MW2.PRJ TIMBRWLF out.ppm [yaw pitch repr palette]` renders a mech;
  `mechview MW2.PRJ verify` checks every model and skeleton.
- `bwdtool MW2.PRJ missions` lists all 59 missions; `bwdtool MW2.PRJ mission CYANSCN1`
  shows a mission's nav points, parts, CD music track and briefing.
- `mektool MW2.PRJ list` / `mektool MW2.PRJ TBR00STD` prints mech stats or a full
  record sheet: armor, internals, critical slots, weapons and ammo.
- `prjtool types|list|verify|extract|dumpall MW2.PRJ ...` lets you browse or
  extract resources. TEXT resources are stored byte-negated; extract decodes them.

## Licences
The port's source code is the author's, released under the GNU General Public License, version 2 or (at your option) any
later version (GPL-2.0-or-later; the text is in [LICENSE](LICENSE)). The game data is not covered: MechWarrior 2's files -
your own copy, and the 3D editions' data in gamedata/ - are Activision's (1995) and remain Activision's; gamedata/ is here
only so owners of the game can rebuild a working setup. Third-party components keep their own licences (THIRD_PARTY.md):
Nuked SC-55 (third_party/nuked-sc55/) GPL-2.0-or-later; Munt's mt32emu (third_party/mt32emu/) LGPL-2.1-or-later;
miniaudio (third_party/) public domain / MIT-0; stb_truetype (src/third_party/) public domain / MIT; Liberation Sans
(assets/fonts/) SIL Open Font License 1.1. No ROMs are included: the MT-32 and SC-55 firmware are Roland's.

## Playing

    make mw2
    ./mw2 /path/to/MW2-game      # the first time; afterwards just ./mw2

`mw2` is the game: the original's menus, then the missions (it runs `glview` from its own folder). The game folder
holds your copy of the game:

    install/    your settings, pilots and saves (or a complete DOS install as the folder itself)
    cd/         the disc's files (or point cd= at an .iso)
    music/      the CD soundtrack (Track02..27, FLAC/MP3/WAV)
    mt32/       the MT-32 ROMs (menu music)
    sc55/       optional: Roland SC-55 ROM dumps (menu music on an emulated SC-55)
    ultrasnd/   Gravis UltraSound patches (menu music without the ROMs)
    3d/         the 3D editions' data - copy gamedata/3d/ from this repository (without 3d/: the DOS models, untextured)
    titanium/   optional: MW2.PRJ + SKYGND.PAR from the Titanium disc's Install/16bit/ (the TITANIUM textures)

If no folder is given, `mw2` looks in the current folder, next to itself (the macOS app: next to
`MechWarrior 2.app`), one level up (`MW2-game`) and in `~/MW2-game`; if it finds none (or the remembered folder is
gone) it asks for it with the system's folder picker - choose `MW2-game` itself or the folder that holds it. It
remembers the folder in the per-user settings file:

    Linux   ~/.config/mw2port/mw2port.cfg        macOS   ~/Library/Application Support/mw2port/mw2port.cfg

    game=/path/to/MW2-game
    resolution=desktop        # desktop (native fullscreen) | window | 2560x1440 ...
    aa=4                      # anti-aliasing: 0 2 4 8
    render=native             # native | 320x200 | 640x480 | 1024x768 (the original's resolutions, scaled up in blocks)
    renderer=enhanced         # enhanced | dos | 3dfx | ati | s3 | pvr | mga | rendition
    textures=original         # original | titanium (the old edition= is still read: edition=titanium becomes
                              # renderer=enhanced textures=titanium)
    menu_music=auto           # auto | mt32 | sc55 | gus (the menu music's device; auto: the first present, in that order)
    # optional overrides: install= cd= music= ultrasnd= mt32= sc55= models= textures= skygnd= skygnd_fog=
    #                     textures_kind=ati|3dfx  titanium= (the Titanium files' folder)

Resolution (with anti-aliasing), the display mode and the look are also in the game: the global menu (Esc / right
button) -> COMBAT VARIABLES (RESOLUTION, DISPLAY, RENDERER, TEXTURES - RENDERER and TEXTURES head the DETAIL group). Controls follow your install's GAMEKEY.MAP / INPUT.MAP; set them up in the
global menu -> COCKPIT CONTROLS, as in the original.
In a mission, Esc opens the original's MAIN MENU and pauses the game: Abort Mission, Graphics (Textured Sky / Ground and
the port's Renderer - Enhanced, 3Dfx, ATi, S3, PowerVR, Matrox or Rendition - and Textures - Original or Titanium - switched on
the spot and kept for the next mission), Audio Ctrl (music / effects / voice sliders), Combat Variables (textures, detail, density, explosion chunks, the menu music device)
and Flee to Windows (leaves the game). Digits pick an item, arrows / Space change a value, 0 or Enter accepts, Esc
backs out. Ctrl+Q still aborts the mission directly. The DOS renderer keeps its own archive: choose it in the shell
(in a DOS mission Renderer and Textures stay as they are).
The MW2_* environment variables remain for development and tests only.

## Customising controls

The port uses the original game's two control files, exactly as MechWarrior 2 reads them:

| File | What it holds | Syntax |
|---|---|---|
| `GAMEKEY.MAP` | one-press commands: menus, targeting, views, throttle presets (1-0), autopilot, shutdown, vision modes ... | one `COMMAND key` per line |
| `INPUT.MAP` | everything held or moved: throttle up / down, turning the legs, torso twist / tilt, looking, jump jets, firing - keyboard, mouse and joystick | `control {` then `+ device key` lines, then `}` |

**Where they live.** The game looks in your install folder first (`MW2-game/install/`), then in the disc copy
(`MW2-game/cd/MECH2/`, where the originals are). Upper or lower case file names both work. To customise, **copy
`GAMEKEY.MAP` and `INPUT.MAP` from `cd/MECH2/` into `install/` and edit those copies** (the disc copies stay as they
were). The folder layout is the same on macOS, Windows and Linux; where `MW2-game` itself is depends on where you put
it (next to the app / `mw2.exe`, or in your home folder - see FOLDER_SETUP.md). The game's log names the two files it
read: `controls: GAMEKEY.MAP <path>, INPUT.MAP <path>`.

**Cockpit Controls** (global menu) writes INPUT.MAP for you, as in the original: ACCEPT on the first page rewrites it
only if you changed the selected devices; CUSTOM CONFIGURATION starts from the selected devices' defaults (Load Custom
brings back a saved set), and ACCEPT CONFIG AND EXIT writes INPUT.MAP (the old one becomes INPUT.BAK) and remembers the
set as the current one (`install/GIDDI/CONFIG00.CPC`; Save Custom 1-4 go to `CONFIG01-04.CPC`). It never touches
GAMEKEY.MAP. If you write INPUT.MAP by hand, don't accept a configuration in Cockpit Controls afterwards (or keep a copy).

**GAMEKEY.MAP rules.** A key is one character (`t`, `;`, `` ` ``, `\` ...) or F1-F12, Esc, Tab, Enter, BSP, Insert,
Pause, Plus, with `CTRL+`, `ALT+`, `SHIFT+` in front (any case). Each key does one command: if two lines name the same
key, the first wins. A command can have several keys, and deleting its lines unbinds it. Anything after `#` is a
comment. A misspelt command makes the game ignore the rest of the file (as the original did) - the log says which line.

**INPUT.MAP rules.** Keyboard key names are the ones listed in `GIDDI`'s keyboard driver: letters `A`-`Z`, `One` ...
`Zero`, `Space`, `Tab`, `Enter`, `Minus`, `Equal`, `Comma`, `Period`, `Slash`, `LeftShift`, `LeftControl`, `LeftAlt`,
`Shift` / `Control` / `Alt` (either side), `GreyUpArrow` / `GreyLeftArrow` ... (the arrow keys), while the plain
`UpArrow`, `Home`, `Insert` ... are the numeric keypad. `KEYBOARD.MAP`, `MOUSE.MAP` and `JOYSTICK.MAP` next to INPUT.MAP
are the original's templates with every control listed at the top.

**Keys that are not yours to rebind:** Alt+Enter (fullscreen), and on macOS the Cmd shortcuts. Everything else in a
mission comes from the two files (the port's development keys only work with `MW2_DEBUG_KEYS=1`).

**Example: WASD + mouse.** In GAMEKEY.MAP, move the three commands that sit on W, A and S:

    TOGGLE_AUTOPILOT                p          (was a)
    ENHANCED_VISION                 h          (was w)
    # SHUTDOWN_MECH                 s          (deleted: Ctrl+s - STARTUP_MECH - switches the mech off and on as well)

In INPUT.MAP, add (tab or spaces between the words):

    throttle_plus {
        + keyboard  W
    }
    throttle_minus {
        + keyboard  S
    }
    legs_pan_minus {
        + keyboard  A
    }
    legs_pan_plus {
        + keyboard  D
    }

The mouse: as long as INPUT.MAP has no `mouse` lines, the port reads the mouse the way the original's MOUSE.MAP sets it
(move = torso twist and tilt, left button = fire). Once you add any mouse line, only your mouse lines count, so add the
whole set:

    torso_pan {
        + mouse  Left/Right
    }
    torso_tilt {
        - mouse  Up/Down
    }
    weapon_fire {
        + mouse  LeftBtn
    }
    weapon_cycle {
        + mouse  RightBtn
    }

The same can be clicked together in Cockpit Controls: select Mouse (the keyboard is always on), CUSTOM CONFIGURATION,
click Throttle then W and S (in the order its "+/-" shows), Chassis then A and D (L/R), Save Custom 1, ACCEPT CONFIG AND EXIT
(GAMEKEY.MAP still needs the edit above).

## Building a package

    make dist      # dist/mw2port-linux-x86_64.tar.gz: mw2, glview, assets/, README, THIRD_PARTY.md

Unpack anywhere and run `./mw2 /path/to/MW2-game` once. Needs SDL3 and a Vulkan driver from the system (the release
workflow's Linux tarball carries its own `libSDL3.so.0` beside `mw2`, found through the binary's `$ORIGIN` run path).
On macOS: `make mw2 && sh scripts/macos_app.sh` builds "MechWarrior 2.app" in the current folder, with its icon and
SDL3 bundled inside (no Homebrew needed to run it).

### Windows

Download the `windows-x64` zip from a GitHub release (or, for an XP-era PC, the `windows-x86-legacy` zip - `SDL2.dll`
in place of `SDL3.dll`; see "The legacy build") and unzip it anywhere: `mw2.exe`, `SDL3.dll` and `assets\`
must stay together. Then either put your `MW2-game` folder next to `mw2.exe`, put it in your user folder
(`C:\Users\<you>\MW2-game`), drag the folder onto `mw2.exe` once, or just start `mw2.exe` and choose the folder when
it asks - it is remembered in
`%APPDATA%\mw2port\mw2port.cfg`. Needs a graphics driver with Direct3D 12 or Vulkan (see "System requirements"), with
the vendor's driver installed rather than Windows' basic display driver.

The exe isn't code-signed, so Windows SmartScreen may say "Windows protected your PC": click **More info**, then
**Run anyway**.

### DOS look shading

`dos_shading=dithered|smooth` in mw2port.cfg (the older `dos_sky=` key is still read; in a DOS mission: GRAPHICS page,
first row "DOS shading"; in the shell: Combat Variables, DOS DITHERED / SMOOTH right of TEXTURES). *dithered* (the
default) is the original: the sky haze in 15 steps and every shaded polygon (cockpit, mechs, buildings) with the DOS
renderer's fine checkerboard between palette steps. *smooth* blends the same colours into even gradients; it follows the
time of day, light amplification and the red damage flash. Other renderers are not affected.

### HUD layout

`hud_layout=wide|original` in mw2port.cfg (in a mission: GRAPHICS page > HUD layout; in the shell: Combat Variables >
DISPLAY row, right half). *wide* (the default) moves the cockpit instruments to the screen's corners on screens wider
than 4:3: radar, altitude tape and target display on the left; weapons, damage display, throttle bar and speed on the
right. The compass, reticle and heat bars stay centred. *original* keeps the whole HUD in the 4:3 middle, as the original
game drew it. At 4:3 the two are identical.

### Movie Options

The full-screen movies - the intro, the clan landings and the endings (the 320x200 cinematics) - can be shown in a
different look. In the shell: Combat Variables, the two rows right of OBJECT DENSITY and CHUNKY EXPLOSIONS (each click
the next choice; the next movie uses it); or in mw2port.cfg:

- `videos=original|smooth|scanlines|crt|crt_curved` - **FMV ORIGINAL** (the default) is the original's plain pixels,
  exactly as before. **SMOOTH** filters the picture (Mitchell-Netravali). **SCANLINES** draws each movie line as a
  narrow beam with dark gaps between. **CRT** adds a glow and an aperture grille (brighter lines bloom wider).
  **CURVED CRT** bends the picture like a tube's face, with soft black edges.
- `video_fit=4:3|fill|stretch` - **FMV 4:3** (the default) shows the whole picture at 4:3 with black bars; **FILL**
  keeps 4:3 and fills the window, cropping the picture's top and bottom (or sides); **STRETCH** fills the window and
  stretches the picture.

The looks other than ORIGINAL (and ORIGINAL with FILL / STRETCH) draw the movie at the window's real pixel size,
Retina / 4K included, so the scanlines and the grille are sharp. They are a port presentation option, not the original
game's behaviour. The work is done on the CPU over all its cores; if a slow computer can't keep up at a very high
resolution, the picture is drawn smaller for the rest of that movie and scaled up by the GPU. The small clips inside the
menu screens (and the registration and briefing-room clips, which are menu artwork) always stay as they are. Any key or
click skips a movie, as before.

### macOS: the app is not signed

The app (built here or downloaded from a GitHub release) is only ad-hoc signed, not signed with an Apple Developer ID or
notarised, so macOS blocks a downloaded copy the first time ("Apple could not verify..." or "is damaged and can't be
opened"). First move the app out of Downloads (see below), then either open System Settings > Privacy & Security and
click "Open Anyway" for MechWarrior 2, or clear the download flag once in Terminal, from the folder holding the app:

    xattr -dr com.apple.quarantine "MechWarrior 2.app"

Then open it normally. A copy you built yourself is not flagged and opens directly. Your game data is not inside the
app: on first launch the app looks for your `MW2-game` folder next to `MechWarrior 2.app`, one level up and in
`~/MW2-game`, and if it is in none of those it asks for your game folder (choose `MW2-game` itself or the folder that
holds it). The choice is remembered in `~/Library/Application Support/mw2port/mw2port.cfg`; if the folder later moves,
the app asks again. Move `MechWarrior 2.app` out of Downloads (into Applications, or a folder of your own) with Finder
before you use it: a downloaded app opened where it was downloaded runs from a temporary copy (macOS "App
Translocation") and cannot see a folder next to it. It then says so and asks for the game folder, which is remembered.

### macOS: keyboard shortcuts

During a mission the game turns off the system's keyboard shortcuts (Mission Control's Ctrl+Left/Right, Spotlight)
while its window is in front; holding Cmd gives them back, so Cmd+Tab and the Cmd+Shift+3/4/5 screenshots work. They come back in the MAIN MENU (Esc), while paused and when the game ends. Add
`system_shortcuts=allow` to `~/Library/Application Support/mw2port/mw2port.cfg` to keep them. If Ctrl+Left/Right still
switches desktops, go to System Settings > Keyboard > Keyboard Shortcuts... > Mission Control and untick "Move left a
space" / "Move right a space". If F1-F12 change brightness or volume instead of reaching the game, hold Fn, or turn on
"Use F1, F2, etc. keys as standard function keys" under Keyboard Shortcuts... > Function Keys.
