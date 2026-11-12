# The classic Mac OS / Carbon build (PowerPC)

> **ALPHA.** This build has had almost no testing on real hardware: it compiles, and its shared code runs on Linux
> with the EGL stand-in below, but it has barely been run on a PowerPC Mac. Expect problems; reports welcome.

One CFM application for **Mac OS 9 with CarbonLib** and **Mac OS X 10.1–10.4 on PowerPC**, drawing with the OpenGL
1.2 renderer (docs/RENDERER_GL12.md). Target hardware: a G3 with an ATI Rage 128 or better; a Rage Pro works with
the single-texture-unit fog path.

## Tracking the main port

The classic Mac build sits on the main port's source (merged into the main repository in v1.1.15, so it now tracks every release) - the game code is shared; what is the
Mac's own is platform/carbon/ and a few `#ifdef MW2_CLASSIC_MAC` blocks (src/portcfg.c, tools/mw2shell.c,
tools/glview.c, src/rb_gl12.c, src/hud.c, src/cdaudio.c). It builds as the legacy build does: the OpenGL 1.2
renderer (RB=gl12), no Roland SC-55 (MW2_NO_SC55), no MT-32 emulator (mt32_none.c). On top of the main port's
renderer the Mac adds 16-bit textures, HUD images cut into tight power-of-two tiles (pixel-identical to the main
port's tiling), the video-memory count (MW2_GL12_TRACE) and the Rage 128 floor; in the shell, QuickTime's
synthesizer as a fallback menu-music device. Changes to the shared files must keep the Mac build compiling: the
Linux check below, and the release workflow's `build-classic-mac` job (Retro68), catch it.

## How it fits together

The game's code is unchanged: it still includes `<SDL3/SDL.h>`, and on this build that is
`platform/carbon/SDL3/SDL.h`, the subset of SDL 3.4 the game uses (prototypes and constant values taken from SDL
3.4's own headers), implemented by:

| File | What |
|---|---|
| `sdl_core.c` | portable: event queue, keyboard (SDL scancodes, SDL3's default US keycodes), properties and hints, audio streams and their mix, the shell's 2D renderer on OpenGL 1.2, surfaces, display modes |
| `sdl_plat_carbon.c` | Carbon: windows, AGL, `WaitNextEvent` / `GetKeys` / `GetGlobalMouse`, the Sound Manager, alerts, the application's folder |
| `sdl_plat_egl.c` | Linux test stand-in for `sdl_plat_carbon.c` (EGL pbuffers, no input, audio drained to a file) |
| `gl_dyn.c` / `.h` | OpenGL loaded by name at start: OpenGLLibrary on Mac OS 9, OpenGL.framework on Mac OS X - no OpenGL SDK or import library needed |
| `posix_mac.c`, `posix_mac_path.c`, `include/` | the game's `/` paths on the File Manager: `fopen` (funopen over a data fork), `stat`, `mkdir`, `remove`, `rename`, `realpath`, `opendir` / `readdir` |
| `qtmidi.c`, `mt32_none.c` | the menu music: QuickTime's synthesizer; no MT-32 emulator |
| `atomic8.c` | the 64-bit `__atomic` helpers GCC needs on 32-bit PowerPC for miniaudio |
| `scancodes_mac.h` | Mac virtual key codes to SDL scancodes (SDL's own table) |

**Menu music**: no MT-32 emulator on this build (`mt32_none.c`). The shell plays the Gravis UltraSound song set
through the GUS patches when the `ultrasnd` folder is there, otherwise through **QuickTime's synthesizer**
(`qtmidi.c`): the same XMIDI songs - their General MIDI programs - timed by `gusmid.c`'s sequencer
(`gus_song_open_sink`) and sent to QuickTime's note allocator. `music=builtin` / `MW2_MUSIC=builtin` picks QuickTime
even with the patches present. (`qtmidi.c` is untested; the sequencer's MIDI output was checked on Linux: every song
balanced note-on / note-off, at most 12 notes at once.)

**Audio** runs on the main thread: the event pump and every present call the game's audio callbacks (the effects
mixer, the CD music decoder) and mix them into a ring of 16-bit frames. The Sound Manager's callback - at interrupt
time on Mac OS 9, where nothing may allocate or read a file - only copies from that ring. miniaudio is built
decoder-only (`MA_NO_DEVICE_IO`, `MA_NO_THREADING`, `MA_NO_ENGINE`, `MA_NO_RESOURCE_MANAGER`, `MA_NO_NODE_GRAPH`);
`src/cdaudio.c` plays through an SDL stream under `MW2_CLASSIC_MAC`.

## Building

Needs [Retro68](https://github.com/autc04/Retro68). Its default, open-source Multiversal headers are enough:
`platform/carbon/carbon_extra.h` declares the Carbon, QuickTime and AGL calls they lack (checked 2026-11-11: Retro68
master with Multiversal builds it cleanly). Apple's Universal Interfaces 3.4 (from MPW-GM, `--universal`) work too.
GitHub Actions builds it with Retro68's published toolchain image (`ghcr.io/autc04/retro68`, the release workflow's
`build-classic-mac` job).

    # Retro68, once (PowerPC and Carbon only)
    ../Retro68/build-toolchain.bash --no-68k

    # the game
    mkdir build-carbon && cd build-carbon
    cmake ../platform/carbon -DCMAKE_TOOLCHAIN_FILE=<Retro68 build>/toolchain/powerpc-apple-macos/cmake/retrocarbon.toolchain.cmake
    make

The result is `MechWarrior2.bin` / `.dsk` (MacBinary / disk image). Put the application next to your game folder
(`MW2-game`, as on the other systems) or choose the folder the first time it asks. Memory: the SIZE resource asks
for 160 MB (128 MB minimum). Measured at a mission's peak, the game's own heap is 106 MB in the 3Dfx / ATi / S3 looks,
66 MB DOS and 38 MB PowerVR - most of it decoded textures kept after upload, which could be freed to cut it (not yet).

## First launch, settings and the game folder

- **Settings** live in **System Folder:Preferences:MechWarrior 2 Prefs.cfg** (on OS X, ~/Library/Preferences) - a
  SimpleText document; throw it away to get the first-run settings back. First run: fullscreen 640 x 480, the DOS
  look, no anti-aliasing. The Mac build is always fullscreen (a saved `resolution=window` is played fullscreen); the
  shell's menus are 640 x 480 fullscreen too, and the display is only ever switched to a mode the card's video memory
  allows (640 x 480, 800 x 600, 1024 x 768 with 16 MB).
- **The game folder** is found next to the application (or one level up), else asked for with the Choose a Folder
  dialog, and remembered. **Hold Option while launching** to choose it again.
- **Text files** the port opens or writes (.cfg, .txt, .env, .ini, .log, .md) are made SimpleText documents (type
  TEXT, creator ttxt) - files copied from another computer are fixed the first time the game opens them.

## The application icon

The original MechWarrior 2 icon (the Windows game's 32 x 32, 16 colours: packaging/macos-classic-icon32.png, and a
16 x 16 drawn from it: macos-classic-icon16.png) as the Finder's icon family in `platform/carbon/icon.r` - ICN#,
icl4, icl8, ics#, ics4, ics8, mapped to the Mac's standard palettes - with the BNDL / FREF that bind it to the creator
MW2P. Regenerate with `tools/mac_icon.py packaging/macos-classic-icon32.png packaging/macos-classic-icon16.png >
platform/carbon/icon.r`. Retro68 writes the Finder flags as 0, so after each build `tools/macbin_bundle.py` sets
"has bundle" in **MechWarrior2.bin** (Python 3 from the Xcode command-line tools) - copy the .bin to the Mac, not the
.dsk. If the Finder still shows a generic icon, rebuild the desktop (hold Command-Option while the Mac starts).

## Settings file (mw2.env)

Mac OS 9 has no environment variables, so the switches the other builds take from the environment come from a text
file named `mw2.env` next to the application (`platform/carbon/mw2.env.sample` lists the useful ones): one
`KEY=VALUE` a line, `#` comments, an optional `export ` and quotes. It is read before `main()`, so every switch in this
document works on the Mac, test switches included (`MW2_AUTOPILOT`, `MW2_SHOT` ... for a fixed "start a mission, run
N frames, save a picture" run - see docs/DOSBOX_COMPARISON_METHOD.md section 6). Lines using `$` (the game folder's
own mw2.env) are skipped.

## Checking against the original

docs/DOSBOX_COMPARISON_METHOD.md is how the port is checked against DOS MechWarrior 2 in DOSBox. For the DOS look
run the port with the DOS archive as both models and textures (`MW2_EDITION=dos`, `MECH2/MW2.PRJ` twice) - the 3D
editions' texture archive on DOS models draws garbage. Two things the method doesn't spell out, needed with a disc
image built from the disc's files: the launcher wants `OLD_HERC.DRV` in the image's root marked hidden
(`genisoimage -hidden OLD_HERC.DRV`), and an install copy needs the disc's `VFX`, `GIDDI` and `SND` folders in its
MECH2 folder (the installer copies them; `MW2.EXE` stops with "Unable to find video driver file" / "Can't find
input device keyboard" without them).

## Graphics card floor and video memory

The floor is an **ATI Rage 128**: at least 8 MB of video memory and two texture units (GL_ARB_multitexture). Below
that (a Rage Pro, Rage II) the game says so in a box and quits; `MW2_GL12_ALLOW_ONE_UNIT=1` lets a one-unit card run
anyway (fog in a second pass). The video memory is read from AGL (`aglDescribeRenderer`, AGL_VIDEO_MEMORY; unknown
counts as 8 MB; `MW2_VRAM_MB` overrides it) and caps every look's resolution: **800 × 600 with 8 MB, 1024 × 768 with
16 MB or more** - the shell's resolution list offers only those, and a mission asked for more is lowered (said on the
console).

What fills the card (measured on the Linux stand-in, `MW2_GL12_TRACE=1` reports it):

| | 640 × 480 | 800 × 600 | 1024 × 768 |
|---|---|---|---|
| frame buffers (16-bit colour, 24-bit depth) | 2.3 MB | 3.7 MB | 6.0 MB |
| textures at their peak (a mission loading) | 3.1 MB | 3.1 MB | 3.1 MB |
| textures in play | 0.2 - 0.4 MB | | |

To get there the Mac build (each switch overrides it, for tests): uploads 16-bit textures - RGB5, RGB5_A1 or RGBA4
by the texture's alpha (`MW2_GL12_TEX16=0` for RGBA8); cuts large or odd-sized images (the cockpit screens, the HUD
strips) into power-of-two tiles that fit them instead of padding them whole; keeps the HUD sprites at their own size
instead of their Scale3x upscale (`MW2_HUD_SCALE3X=1` for it); asks for a 16-bit display when fullscreen.

## What has been checked, and how

On Linux, with the Carbon file swapped for the EGL stand-in and everything else exactly as the Mac build compiles it
(`MW2_CLASSIC_MAC`, GL through `gl_dyn`, decoder-only miniaudio):

- the whole game - shell and missions - builds against the subset header; nothing of real SDL3 is linked
- a mission's frame is bit-identical to the same renderer on real SDL3 (PowerVR look with fog, and the DOS look)
- the shell's menus through the subset's 2D renderer match real SDL3's to within text antialiasing (mean 0.5 of 255)
- audio: CD music and weapon effects mix through the ring (captured to a file)

On 32-bit big-endian PowerPC (Linux under QEMU): every data loader gives the same output as x86-64; the objective
sweep plays all 59 missions with identical logs; MP3 decoding matches native (float rounding apart).

**Not yet checked: `sdl_plat_carbon.c` and `posix_mac.c` have not been compiled** (Retro68 with the Universal
Interfaces wasn't available where they were written). Expect a round of compile fixes, then a test on hardware.

## Open items

- **Mouse look on Mac OS 9**: Carbon has no way to move the cursor on Mac OS 9, so relative motion stops at the
  screen's edge. Mac OS X uses `CGGetLastMouseDelta` with the cursor detached. Options: InputSprocket, or the
  keyboard/joystick for looking.
- **Switching resolution mid-mission**: fullscreen (640×480, 800×600 or 1024×768) is set through AGL
  (`aglSetFullScreen`) when the shell's or the mission's GL context is made, and restored when it goes; changing it
  while a context exists would need a new context, so it is declined for now. `AGL_FULLSCREEN` (54) and
  `aglSetFullScreen` are also from memory of agl.h — check them.
- **Joysticks**: none yet (InputSprocket).
- **Text input**: ASCII only (pilot names); MacRoman's upper half not mapped yet.
- **AGL constants** (`AGL_RGBA` 4, `AGL_DOUBLEBUFFER` 5, `AGL_DEPTH_SIZE` 12, `AGL_PIXEL_SIZE` 50,
  `AGL_FULLSCREEN` 54, `AGL_NO_RECOVERY` 72, `AGL_ACCELERATED` 73, `AGL_VIDEO_MEMORY` 120, `AGL_SWAP_INTERVAL` 222) are
  written from memory of agl.h: check them against the SDK's. Symptoms if wrong: no 16-bit fullscreen (falls back to
  the card's own depth), or "is more than 8 MB of video memory takes" on a 16 MB card (then `MW2_VRAM_MB=16` in mw2.env).
- The fragment names the OpenGL functions are looked up in on Mac OS 9 (`OpenGLLibrary`, `AGL`, `OpenGLUtility`):
  confirm against the Extensions folder; `gl_fn` tries each.

## Testing on Linux

    # the subset with the EGL stand-in, the Mac build's options
    mkdir -p build
    F="-std=c99 -D_DEFAULT_SOURCE -O2 -DMW2_CLASSIC_MAC -DMW2_NO_SC55 -DMA_NO_DEVICE_IO -DMA_NO_THREADING -Iplatform/carbon"
    gcc $F -c platform/carbon/sdl_core.c -o build/sdl_core.o
    gcc $F -c platform/carbon/sdl_plat_egl.c -o build/sdl_plat_egl.o
    gcc $F -c platform/carbon/gl_dyn.c -o build/gl_dyn.o
    gcc -O2 -DMA_NO_DEVICE_IO -DMA_NO_THREADING -c third_party/miniaudio.c -o build/miniaudio.o
    make RB=gl12 mw2 SC55_LIB= SC55_SRC= CFLAGS="$F" SDL3_CFLAGS="-Iplatform/carbon" \
         SDL3_LIBS="build/sdl_core.o build/sdl_plat_egl.o build/gl_dyn.o -lEGL" SDL3_CHECK=
    EGL_PLATFORM=surfaceless MW2_EGL_WAV=/tmp/out.raw ./mw2 --sim ...   # (MW2_AUTOPILOT / MW2_SHOT as the tests)
