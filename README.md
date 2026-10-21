# MechWarrior 2 (DOS, 1995) - 64-bit port

A native 64-bit reimplementation of MechWarrior 2: 31st Century Combat, rebuilt module by module from the original
MW2.EXE / MW2SHELL.EXE and the 3D editions' MW2.DLL. Portable C99, builds on macOS, Linux and Windows. You need your own copy of
the game (see "Setting up the game folder").

## Status (2026-10-20)
Complete and playable on macOS, Linux and Windows. Tagged releases (`v*`) are built by GitHub Actions: a macOS app,
a Linux tarball, a Fedora RPM and a Windows zip.

- **Shell:** clan halls, both campaigns, Instant Action, Cadet Training, Mech Lab, Combat Variables, Cockpit Controls,
  Hall of Honor, the roster and both clans' Archives. The pilot-name easter-egg mechs (Elemental, Tarantula, Battle
  Master IIC) are unlocked everywhere at their real tonnage.
- **Simulation:** every mission, with AI, mission logic, combat, heat, movement, jump jets, HUD, autopilot and
  path-driven dropships / convoys. Locked missiles home on the target's centre; destroyed buildings stay destroyed.
- **Look:** chosen in Combat Variables with two settings. RENDERER: ENHANCED (the default: 3Dfx look + PowerVR fog), DOS,
  3DFX, ATI RAGE, S3 VIRGE, POWERVR or MATROX MYSTIQUE. TEXTURES: ORIGINAL (each renderer's own textures, sky and
  ground) or TITANIUM (with the Titanium disc's files, its own models and lighting).
- **Sound:** the original's volumes, stereo pans and distance delay; CD music from your own rips; MIDI through the
  MT-32 emulator or Gravis UltraSound patches.
- **Input:** keyboard, mouse and joystick, with the original's key rates and mouse scaling.

Game values are traced from the original code or measured in DOSBox. What is still assumed is listed at the end of
ASSUMPTIONS.md ("Open items"); [KNOWN_ISSUES.md](KNOWN_ISSUES.md) is the plain-language list of what is still open.
AUDIT.md is the audit log. Test suites: `make check`, `make test_sequences` (scripted play sequences, 202 of them) and
`make sweep` (every mission's objectives).

## Build
Needs a C99 compiler, make, SDL2 (development files) and OpenGL 3.3.

    macOS:                  xcode-select --install; brew install sdl2
    Linux (Debian/Ubuntu):  sudo apt install build-essential libsdl2-dev libgl-dev

    make && make mw2        # the tools, then the game (mw2: menus + missions in one program)

Windows (64-bit) builds with MinGW-w64: in MSYS2's MINGW64 shell
(`pacman -S make mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2`) just `make mw2`; or cross-compile from Linux with
the SDL2 MinGW development package (as the release workflow does):

    make mw2 WINDOWS=1 CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ AR=x86_64-w64-mingw32-ar \
             SDL2_CONFIG=/path/to/SDL2-x.y.z/x86_64-w64-mingw32/bin/sdl2-config

That gives `mw2.exe`; put `SDL2.dll` beside it. Only the game (`mw2`) is set up for Windows, not the other tools.
Run `make clean` when switching between a Windows and a native build.

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
                  (without them the menus fall back to Gravis UltraSound patches in ultrasnd/, if present)
      music/      optional - the CD soundtrack (Track02..Track27 as FLAC, MP3 or WAV; see "CD music")
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
textures of the same name - the effects, the grounds and the sky come from the Titanium files.

### Gravis UltraSound patches (optional)
Without the MT-32 ROMs the menu music can play through the Gravis UltraSound's General MIDI patch set instead. Get the
GUS driver / install disks (the 4.x "ULTRASND" package, easy to find on VOGONS) and extract them; what the port needs is
the resulting `ULTRASND` folder - in particular `MIDI/ULTRAMID.INI` and the `MIDI/*.PAT` patch files. Copy the whole
folder in as `ultrasnd/`:

    MW2-game/ultrasnd/MIDI/ULTRAMID.INI
    MW2-game/ultrasnd/MIDI/ACPIANO.PAT  ... (about 280 .PAT files)

Running the DOS installer is not needed; extracting the archives is enough. With both present, the MT-32 is used.

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
- `glview 3DFX.PRJ ATI.PRJ ati @ CYAN` plays the mission in a Timber Wolf: W/S throttle, X stop, A/D legs,
  mouse torso twist/pitch, F or left mouse fire, C chase/cockpit, G free camera (then WASD/Q/E fly).
- `glview 3DFX.PRJ ATI.PRJ ati [MECH MISSION]` - interactive viewer (SDL2 + OpenGL 3.3). Drag to orbit, wheel to
  zoom, F fullscreen at desktop resolution, Tab mech, N mission, C camo (0-7), I clan insignia, B filtering,
  M anti-aliasing, [ ] fog, Space walk animation; in world view P runs the mission (camera = player). Build with `make gl` (needs SDL2 dev files).
- `glshot ... out.ppm W H` - the same renderer headless, any resolution (Linux/EGL).
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
The port's source code is the author's. miniaudio (third_party/) is public domain / MIT-0; Liberation Sans
(assets/fonts/) is under the SIL Open Font License; see THIRD_PARTY.md. The game data in gamedata/ is from MechWarrior 2
(Activision, 1995) and remains Activision's; it is here only so the owner can rebuild a working setup.

## Playing

    make mw2
    ./mw2 /path/to/MW2-game      # the first time; afterwards just ./mw2

`mw2` is the game: the original's menus, then the missions (it runs `glview` from its own folder). The game folder
holds your copy of the game:

    install/    your settings, pilots and saves (or a complete DOS install as the folder itself)
    cd/         the disc's files (or point cd= at an .iso)
    music/      the CD soundtrack (Track02..27, FLAC/MP3/WAV)
    mt32/       the MT-32 ROMs (menu music)
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
    renderer=enhanced         # enhanced | dos | 3dfx | ati | s3 | pvr | mga
    textures=original         # original | titanium (the old edition= is still read: edition=titanium becomes
                              # renderer=enhanced textures=titanium)
    # optional overrides: install= cd= music= ultrasnd= models= textures= skygnd= skygnd_fog= textures_kind=ati|3dfx
    #                     titanium= (the Titanium files' folder)

Resolution (with anti-aliasing), the display mode and the look are also in the game: the global menu (Esc / right
button) -> COMBAT VARIABLES (RESOLUTION, DISPLAY, RENDERER, TEXTURES - RENDERER and TEXTURES head the DETAIL group). Controls follow your install's GAMEKEY.MAP / INPUT.MAP; set them up in the
global menu -> COCKPIT CONTROLS, as in the original.
In a mission, Esc opens the original's MAIN MENU and pauses the game: Abort Mission, Graphics (Textured Sky / Ground and
the port's Renderer - Enhanced, 3Dfx, ATi, S3, PowerVR or Matrox - and Textures - Original or Titanium - switched on
the spot and kept for the next mission), Audio Ctrl (music / effects / voice sliders), Combat Variables (textures, detail, density, explosion chunks)
and Flee to Windows (leaves the game). Digits pick an item, arrows / Space change a value, 0 or Enter accepts, Esc
backs out. Ctrl+Q still aborts the mission directly. The DOS renderer keeps its own archive: choose it in the shell
(in a DOS mission Renderer and Textures stay as they are).
The MW2_* environment variables remain for development and tests only.

## Building a package

    make dist      # dist/mw2port-linux-x86_64.tar.gz: mw2, glview, assets/, README, THIRD_PARTY.md

Unpack anywhere and run `./mw2 /path/to/MW2-game` once. Needs SDL2 and OpenGL 3.3 from the system.
On macOS: `make mw2 && sh scripts/macos_app.sh` builds "MechWarrior 2.app" in the current folder, with its icon and
SDL2 bundled inside (no Homebrew needed to run it).

### Windows

Download the `windows-x64` zip from a GitHub release and unzip it anywhere: `mw2.exe`, `SDL2.dll` and `assets\`
must stay together. Then either put your `MW2-game` folder next to `mw2.exe`, put it in your user folder
(`C:\Users\<you>\MW2-game`), drag the folder onto `mw2.exe` once, or just start `mw2.exe` and choose the folder when
it asks - it is remembered in
`%APPDATA%\mw2port\mw2port.cfg`. Needs a graphics driver with OpenGL 3.3 (any GPU from about 2010 on, with the
vendor's driver installed rather than Windows' basic display driver).

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
