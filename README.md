# MechWarrior 2 (DOS, 1995) - 64-bit port

A native 64-bit reimplementation of MechWarrior 2: 31st Century Combat, rebuilt module by module from the original
MW2.EXE / MW2SHELL.EXE and the 3D editions' MW2.DLL. Portable C99, builds on macOS and Linux. You need your own copy of
the game (see "Setting up the game folder").

## Status (2026-10-08)
Complete and playable: the shell (clan halls, both campaigns, Instant Action, Cadet Training, Mech Lab, Combat Variables,
Cockpit Controls, Hall of Honor, the roster), the simulation (every mission; AI, mission logic, combat, heat, movement,
HUD, path-driven dropships / convoys) and seven looks selectable in Combat Variables > RENDERER: ENHANCED (3Dfx look +
PowerVR fog), DOS, 3DFX, ATI RAGE, S3 VIRGE, POWERVR, MATROX MYSTIQUE. Game values are traced from the original code or
measured in DOSBox; what is still assumed is listed at the end of ASSUMPTIONS.md ("Open items"). AUDIT.md is the audit
log. Test suites: `make check`, `make test_sequences` (scripted play sequences), `make sweep` (every mission's objectives).

## Build
Needs a C99 compiler, make, SDL2 (development files) and OpenGL 3.3.

    macOS:                  xcode-select --install; brew install sdl2
    Linux (Debian/Ubuntu):  sudo apt install build-essential libsdl2-dev libgl-dev

    make && make mw2        # the tools, then the game (mw2: menus + missions in one program)

## Setting up the game folder
The port needs the original game's files in one folder (called `MW2-game` below). Copy these from your own copy:

    MW2-game/
      install/    REQUIRED - the contents of your installed DOS game folder (C:\MECH2 after running the game's
                  INSTALL): the *.CFG settings, MW2REG.CFG (pilots), the *.BWD lance files, SETUP\, SND\ ... Copying
                  the whole folder is fine. The port writes pilots, saves and settings here.
      cd/         REQUIRED - the contents of the game CD: the disc's MECH2\ folder (MW2.PRJ, DATABASE.MW2, ...),
                  SMK\ (movies), KEATING\ (training voices) and the rest, copied as they are on the disc. Or set
                  cd=/path/to/mw2cd.iso in the settings file to read straight from a disc image (see "Your disc image")
      mt32/       REQUIRED for the menu music - the Roland MT-32 ROMs: MT32_CONTROL.ROM and MT32_PCM.ROM
                  (without them the menus fall back to Gravis UltraSound patches in ultrasnd/, if present)
      music/      optional - the CD soundtrack (Track02..Track27 as FLAC, MP3 or WAV; see "CD music")
      ultrasnd/   optional - Gravis UltraSound patches (menu music without the MT-32 ROMs)
      3d/         the 3D editions' data - included in this repository under gamedata/3d/: copy that folder here

Copy the 3D data from this repository:

    cp -R gamedata/3d /path/to/MW2-game/3d

`gamedata/3d/` holds models.prj (the 3Dfx edition's MW2.PRJ), textures.prj (the ATi edition's MW2.PRJ), skygnd.par, and
the S3 / PowerVR / Matrox editions' archives and settings (s3/, pvr/, mga/, ati/) for those looks. Without 3d/ the
game uses the DOS models only.

Then run the game once with the folder; it remembers it:

    ./mw2 /path/to/MW2-game     # afterwards just ./mw2

If no folder is given, `mw2` looks in the current folder, next to itself, one level up (`MW2-game`) and in
`~/MW2-game`. Settings live in `~/Library/Application Support/mw2port/mw2port.cfg` (macOS) or
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
    ultrasnd/   Gravis UltraSound patches (menu music)
    3d/         models.prj, textures.prj, skygnd.par from a 3D edition (3dfx / ATi), and the PowerVR edition's
                skygnd.par copied in as skygnd_pvr.par for the fog (without 3d/: the DOS models, untextured)

If no folder is given, `mw2` looks in the current folder, next to itself, one level up (`MW2-game`) and in
`~/MW2-game`, and remembers what it found in the per-user settings file:

    Linux   ~/.config/mw2port/mw2port.cfg        macOS   ~/Library/Application Support/mw2port/mw2port.cfg

    game=/path/to/MW2-game
    resolution=desktop        # desktop (native fullscreen) | window | 2560x1440 ...
    aa=4                      # anti-aliasing: 0 2 4 8
    render=native             # native | 320x200 | 640x480 | 1024x768 (the original's resolutions, scaled up in blocks)
    # optional overrides: install= cd= music= ultrasnd= models= textures= skygnd= skygnd_fog= textures_kind=ati|3dfx

Resolution (with anti-aliasing) and the display mode are also in the game: the global menu (Esc / right button) ->
COMBAT VARIABLES (RESOLUTION, DISPLAY). Controls follow your install's GAMEKEY.MAP / INPUT.MAP; set them up in the
global menu -> COCKPIT CONTROLS, as in the original.
In a mission, Esc opens the original's MAIN MENU and pauses the game: Abort Mission, Graphics (Textured Sky / Ground and
the port's Video mode - Enhanced, 3Dfx, ATi, S3, PowerVR or Matrox, switched on the spot and kept for the next
mission), Audio Ctrl (music / effects / voice sliders), Combat Variables (textures, detail, density, explosion chunks)
and Flee to Windows (leaves the game). Digits pick an item, arrows / Space change a value, 0 or Enter accepts, Esc
backs out. Ctrl+Q still aborts the mission directly. The DOS look keeps its own archive: choose it in the shell.
The MW2_* environment variables remain for development and tests only.

## Building a package

    make dist      # dist/mw2port-linux-x86_64.tar.gz: mw2, glview, assets/, README, THIRD_PARTY.md

Unpack anywhere and run `./mw2 /path/to/MW2-game` once. Needs SDL2 and OpenGL 3.3 from the system.
On macOS: `make mw2 gl && scripts/macos_app.sh` builds "MechWarrior 2.app" (untested so far).
