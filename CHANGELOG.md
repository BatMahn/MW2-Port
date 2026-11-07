# Changelog

## v1.1.10 (2026-11-06)

- FOLDER_SETUP.md: a section on the Windows 95 menu voices. It covers where the Windows 95 CD's `DATABASE.MW2` goes
  (`install/`), how to tell the two editions' files apart by size, two "Common mistakes" rows and a check command.
  No code changes.

## v1.1.9 (2026-11-06)

- **"MechWarrior 2... choose your clan" and "Report to cadet training"** come from the Windows 95 edition. Its
  DATABASE.MW2 is the DOS one plus two WAVs: 0x68 and 0x69. Its shell (MW2SHELL.DLL) plays them, and the port now
  does too when they are there:
  - **0x68 "MechWarrior 2... choose your clan"** plays at volume 0x78 when the main menu opens (0x10005200). The
    menu's background loop (0x4a) starts only after the line ends (0x10005342). It stops when you leave the menu.
  - **0x69 "Report to cadet training"** plays at volume 0x7f in the clan hall after the roster makes a new pilot. The
    roster sets a flag at 0x10018723 and the hall checks it at 0x100047eb. Wolf plays the line at once. Falcon plays it
    after its door clips (ajf8orl1 / ajf8orr1), which the port does not show, so the port plays it at once there too.
  - **To get them:** copy `DATABASE.MW2` from the root of the Windows 95 CD into your `install/` folder. The port
    reads `install/` first. The file is the DOS one with the two WAVs added; all its other entries are byte for byte
    the same. Without it, the menus behave as the DOS game.
- **v1.1.8's logo-clip sound on the main menu is removed.** Neither edition plays that clip's sound track.
- **Shell WAVs read their sample rate from the `fmt` chunk.** It was read from a fixed offset, which is wrong for
  0x68 / 0x69: they carry a LIST chunk first.

## v1.1.8 (2026-11-06)

- **The main menu said "A call to trial... justice by combat".** DATABASE 0x52 is the Trials of Grievance line, not
  "choose your clan". It plays again where the DOS shell plays it: once, on entering Trials of Grievance from the main
  menu (FUN_00029680). This reverts v1.1.7's move.
- **"MechWarrior 2... choose your clan" on the main menu.** No DATABASE sound holds this line. The main menu plays its
  logo clip (amwlogo1, FUN_00039ad0), and the port now plays that clip's own sound track once when the menu opens.
  INFERRED: the line is expected in that track, which has not been checked against the disc's file.
  `MW2_SMK_TRACE=1` logs the track's length when the menu opens.

## v1.1.7 (2026-11-06)

- **"MechWarrior... choose your clan" now plays on the main menu**, once each time the menu opens (DATABASE 0x52, volume
  0x1e). This is a PORT CHOICE made on players' reports. The DOS MW2SHELL.EXE plays the line on entering Trials of
  Grievance from the main menu instead (FUN_00029680), which v1.1.5 followed. Trials of Grievance no longer plays it.
- **The main menu's background loop is quieter.** It plays at the original's volume 0x1e, the voice's level (0x2a4c9),
  instead of half volume, which buried the line.

## v1.1.6 (2026-11-06)

- v1.1.5's notes corrected: the roster's line is "Register SibKo identity". No code changes.

## v1.1.5 (2026-11-06)

- **The menu voices were missing.** Both lines are DATABASE sounds that the original's shell plays. Traced in
  MW2SHELL.EXE:
  - **"MechWarrior... choose your clan"** (DATABASE 0x52) plays once, at volume 0x1e, when Trials of Grievance is
    entered from the main menu (FUN_00029680, 0x29763 / 0x29a2f: the previous screen 8). The port never played it.
  - **"Register SibKo identity"** (DATABASE 0x51) plays on entering the pilot roster (0x384f8). The port played it
    there, but reached the roster on its own only when no pilot was on file. The original keeps a "registered this
    session" flag in its main loop (0x36700). The main menu clears the flag and choosing a pilot in the roster sets it.
    A clan hall entered with the flag clear plays the registration clip and goes to the roster (FUN_0001dbe0). So every
    time a clan is chosen from the main menu, the clip and the line come first, as in the original. Starting the shell
    anywhere else, such as the return from a mission, keeps the pilot on file.

## v1.1.4 (2026-11-06)

- **Jade Falcon dropships wore the Clan Wolf crest** (the 3D editions' textures). The insignia slot is offset by a clan
  value (engine 0x10042000) that the port always left at 0, Wolf. World objects now add the player's clan: the
  campaign's or training's, or Instant Action's friendly clan. The shell passes it to the mission as `MW2_CLAN`. Mechs
  keep the insignia their star gives them. Which value the original uses is INFERRED from the crest the players
  expect; it is not traced.
- **A flat blue sky with Titanium textures** (reported on Vulkan and Direct3D 12). With the Titanium textures, the sky
  is the mission's SKYDOME object from the Titanium archive. When that archive has no SKYDOME, the port drew no sky,
  and only SKYGND.PAR's plain `sky=` colour showed. That happens with an `MW2.PRJ` that is not the 16-bit build's;
  the disc's 8-bit archive has no SKYDOME. The port now falls back to the mission's own cloud sky and logs:
  "titanium: no SKYDOME record ... is titanium= the 16-bit build's MW2.PRJ?". The reporter's exact cause is not
  confirmed.

## v1.1.3 (2026-11-03)

- **Roland SC-55 menu music** (Nuked SC-55 built in): the menus' General MIDI song set - the one the DOS game plays
  through Miles' MPU401.MDI on a Sound Canvas - on an emulated Roland SC-55, as a third music device beside the MT-32
  and the Gravis UltraSound. Bring your own ROM dumps: a `sc55/` folder in the game folder (or `sc55=`), with the file
  names Nuked SC-55 uses (SC-55mk2 v1.01: rom1.bin, rom2.bin, rom_sm.bin, waverom1.bin, waverom2.bin; SC-55: sc55_rom1.bin
  ... sc55_waverom3.bin; also the SC-55st, CM-300 / SCC-1, SCB-55, RLP-3237, SC-155). The model is recognised from the
  files. `menu_music=auto|mt32|sc55|gus` chooses the device; auto (the default) keeps the MT-32 first, then the SC-55,
  then the GUS - nothing changes for an existing setup. In the game: Combat Variables, the entry right of DIFFICULTY
  (MUSIC AUTO / MT-32 / SC-55 / GUS - the devices whose files are there), applied on leaving the screen: the song
  restarts on the chosen device. The emulator runs on its own thread a short queue ahead of the
  sound output and sleeps when no music plays. Not in the Windows legacy build. README "Roland SC-55 (optional)",
  FOLDER_SETUP.md.
- **Licence: the port is now GPL-2.0-or-later** (LICENSE; README "Licences"). The game data stays Activision's; the
  third-party components keep their licences (THIRD_PARTY.md). The release packages carry LICENSE.

## v1.1.2 (unreleased)

- **Movie looks** (a port presentation option): the full-screen movies (intro, landings, endings) can be shown as
  ORIGINAL (the default, unchanged), SMOOTH, SCANLINES, CRT or CURVED CRT, fitted 4:3 (default), FILL or STRETCH -
  Combat Variables (the FMV rows right of OBJECT DENSITY / CHUNKY EXPLOSIONS) or `videos=` / `video_fit=` in
  mw2port.cfg. The filtered looks are drawn at the window's real pixel size (the shell window now uses Retina / high
  pixel density pixels on macOS). Filters adapted from the Mercenaries fork (src/vidfx.c). README "Movie Options".
- **Legacy build for older PCs** (`MW2-Port-<tag>-windows-x86-legacy.zip`): 32-bit, Windows XP SP3 or newer, a
  Pentium 4 / Athlon 64 or newer, an OpenGL 1.2 graphics driver (GeForce 2, Radeon 7000, Intel GMA 900, Voodoo 3 and
  newer). The port is no longer 64-bit only. The game is the same; what differs underneath:
  - **OpenGL 1.2 renderer** (src/rb_gl12.c, src/rb_gl12_world.c; build with `make mw2 RB=gl12`): the renderer interface
    (src/rb.h) on OpenGL 1.2's fixed-function pipeline - the shaders' vertex stage on the CPU, their per-pixel steps as
    texture variants, a fog table texture, triangles split for the Matrox mip bands and cut along the DOS dither's
    iso-lines. Every look matches the normal renderer to within one level per pixel on average (docs/RENDERER_GL12.md).
    It replaces the OpenGL 2.1 renderer that was planned.
  - **SDL2** (`SDL=2`): SDL3 does not run on XP; src/sdlcompat.h maps the port's SDL3 calls onto SDL2 2.0.18+ (the
    release uses SDL2 2.32.10). The normal builds are unchanged (SDL3, SDL_GPU).
  - **Same simulation**: built with SSE2 arithmetic, as the 64-bit builds - all 59 missions' objective sweeps identical
    between a 32-bit and a 64-bit build.
  - Not yet tested on real XP hardware (KNOWN_ISSUES.md).

## v1.1.0 (2026-11-03)

- **SDL3.** The port now uses SDL3 (3.2.0 or newer) instead of SDL2 for its window, input and sound; OpenGL 3.3 is still
  the renderer. The game looks, sounds and plays the same (all checks and picture comparisons identical).
  - Building: macOS `brew install sdl3 pkgconf`, Fedora `SDL3-devel`, Debian 13 / Ubuntu 25.04+ `libsdl3-dev`; older
    Linux: build SDL3 from its release source and `make mw2 SDL3_DIR=<prefix>`. Run `make clean` once after updating.
  - Windows cross build: `make mw2 WINDOWS=1 ... SDL3_DIR=/path/to/SDL3-<version>/x86_64-w64-mingw32`; the zip ships
    `SDL3.dll` (was `SDL2.dll`).
  - The macOS app bundles SDL3 (no sdl2-compat any more); the Linux tarball carries its own `libSDL3.so.0`.
- **New renderer: SDL3's GPU API replaces OpenGL.** Windows draws with Direct3D 12 (default) or Vulkan (Combat
  Variables, the RESOLUTION row's right half, or `gpu=` in mw2port.cfg; the other is used if the chosen one can't
  start), Linux with Vulkan, macOS with Metal. Every look renders as before (the test suite's picture checks are
  unchanged; GL vs Vulkan differences are single-pixel edges and +-1-2 levels of filtering). The menus draw through
  the same API. No OpenGL is linked or used any more.
  - New minimums: Direct3D 12 feature level 11_0 or Vulkan 1.0 (GeForce GTX 600 / Radeon HD 7700 / Intel Haswell
    (D3D12) or Skylake (Vulkan) and newer), Metal on macOS 10.14+. Older cards can't run it; a legacy OpenGL 2.1
    renderer may be added later (the renderer sits behind src/rb.h, docs/RENDERER.md).
  - Linux needs the Vulkan loader and a driver (libvulkan1 / vulkan-loader, mesa-vulkan-drivers or NVIDIA's).
  - Shaders are written once (shaders/, GLSL) and checked in compiled for every API (src/rb_shaders.c);
    `make shaders` regenerates them (glslangValidator, spirv-cross, dxc).
- Fixes found while reviewing the Mercenaries fork, each confirmed in MechWarrior 2's own code:
  - The torso servo sound plays only while the torso lags its aim by more than 2 degrees in the cockpit view; turning
    the legs is silent, as in the original.
  - Sound decoding: half- and quarter-rate sound blocks are expanded exactly as the original does.
  - Instructor voice lines keep the Voice volume when Sound Effects is at 0 (they were silent).
  - An AI mech out of weapons reacts at once even if another manoeuvre was just chosen, and not while fleeing.
  - DOS look: no more sky showing through the joins of the cockpit struts.
  - Mission time limits run out one second before the limit, and the objectives' Time Remaining matches.

## v1.0.2 (2026-10-30)

- **Missing `3d/` folder** (player report: grey sky and ground, untextured models): without the 3D editions' files
  the game now uses the DOS renderer, which draws the DOS data properly, and COMBAT VARIABLES shows RENDERER as
  "DOS - NO 3D FOLDER". Copy `gamedata/3d/` from the repository into your game folder for the 3D renderers
  (FOLDER_SETUP.md).

## v1.0.1 (2026-10-29)

- **Falling through hills fixed** (player report, Silent Thunder): on maps whose hills are stretched versions of a
  model (Silent Thunder, Umber Wall, the PROV mission and the training missions) the ground under you used the
  unstretched hill's slope, so you and the enemies sank into and fell through the hills. The ground now follows the
  drawn hills, as in the original (checked in DOSBox). The DOS look was not affected.
- **Mechs destroyed in mid-air** no longer fly through buildings and hillsides: as in the original, a falling wreck
  that hits a wall stops there (player report: enemies falling through geometry).

## v1.0.0 (2026-10-27)

- **Version 1.0.** The port now covers the whole original game - both campaigns, Instant Action, the shell, every
  renderer look (DOS, ENHANCED, 3DFX, S3, MATROX, ATI, RENDITION, Titanium textures), CD, Gravis and MT-32
  music - checked against the original code and DOSBox throughout. What is left is listed in KNOWN_ISSUES.md: a few
  details of the S3 and Rendition renderers that need the original hardware to check.
- Version numbers: the macOS app and the Windows program now report the release version (was 0.1 / 0.4).

## v0.4.17 (2026-10-26)

- **Missile reticle while reloading** (player report, checked in DOSBox): while the selected launcher fires its volley or
  reloads, the reticle is the plain yellow ring with no lock colours, as in the original. The lock keeps building
  underneath, so the red cross-hair returns the moment the launcher is ready. Releasing the trigger still moves to the
  group's next weapon, as before.

## v0.4.16 (2026-10-26)

- **DOS-look HUD now matches the original pixel for pixel** (the old "won't fix" items): the nav marker and target
  bracket use the original's integer projection; radar blips and the radar's edge use its own top-down view (Nav Alpha
  was 2 px low at the 2 km range); the message bar is clipped exactly as DOS clips it at 640x480 and 1024x768; the
  dH/dT bar reads the original's net heat value; the Wolf Dropship's blip in the first mission sits where DOS puts it.
- **Heat, throttle and jets bars** are smoothed as in the original (each bar moves half the remaining way per frame);
  the heat bar was drawn raw before.
- **Targets, radar and missiles aim at a building's or vehicle's centre**, not its model origin, as the original does.
- **3DFX look**: fire, smoke, flag and other see-through polygons are blended as the 3Dfx card drew them (softer edges,
  drawn farthest first) instead of cut out.
- **Cockpit Controls**: you can type the configuration's name (as in DOS: up to 61 characters, Backspace, Enter / Esc).
- **Joystick / mouse menu files**: the GIDDI\<driver>.STD files are read as the original reads them, so a stick or
  mouse set up there can work the in-mission menu.

## v0.4.15 (2026-10-24)

- **macOS frame rate (found with the new readout)**: the HUD was taking over 100 ms a frame on a Mac once the cockpit
  powered up. It makes about 1,300 small draws a frame, and each one rewrote the same small buffer, so every draw waited
  for the GPU to finish the one before. Apple's OpenGL enforces that wait strictly; Linux and Windows drivers mostly hide
  it. The HUD now writes each draw to a fresh part of a larger buffer. Also, the DOS look's extra shader work is now
  compiled only into the DOS look's shader. No visual change.
- **Performance readout**: start the game with `MW2_PERF=1` to print the frame rate and where each frame's time goes
  (simulation, world draw, HUD, buffer swap), plus the GL driver and drawing size, every 2 seconds. `MW2_PERF=2` also
  times the GPU's world draw.

## v0.4.14 (2026-10-24)

- **macOS frame rate**: since v0.4.12 the renderer could drop to under 20 fps on Macs (Terminal showed "UNSUPPORTED ...
  GLD_TEXTURE_INDEX_BUFFER is unloadable"). The DOS look's polygon buffer was only bound in the DOS look, so in the other
  looks Apple's OpenGL found nothing on that sampler and took its slow path. A small placeholder buffer is now always
  bound. No visual change.

## v0.4.13 (2026-10-23)

- **Controls (player report: controls kept resetting)**: GAMEKEY.MAP and INPUT.MAP are now read from your install
  folder or the disc copy, in any letter case - edited files had been ignored, so the default keys came back every time.
  Both files are read exactly as the original reads them (the first line for a key wins, every keyboard key name incl.
  LeftControl and One..Zero, up to 8 keys per control). Cockpit Controls remembers your current configuration
  (GIDDI/CONFIG00.CPC) and ACCEPT no longer rewrites INPUT.MAP unless you changed devices. The leftover debug keys G
  (free camera) and [ ] (fog) no longer act in missions. See "Customising controls" in the README for a WASD + mouse
  setup.

## v0.4.12 (2026-10-22)

- **3D looks (3DFX, S3, MATROX, ENHANCED)**: fire / smoke cards and decals are textured without perspective correction
  (everything at Display detail Low) as in those editions; the ENHANCED look draws the RUST and TEAL grounds again; fire,
  smoke and jet cards are no longer lit; the 3DFX / S3 TEAL fire card no longer shows as a blue square.
- **DOS-look HUD** at 640x480 and 1024x768 now matches the original pixel for pixel: the selected weapon framed as its
  whole slot, the damage outline centred, the target display (range shown as "1.08k" as in DOS), the heat / jets /
  throttle bars, the rear view, the radar circle and the off-radar nav marker.
- **3DFX / S3 looks**: a texture keeps the format of the first polygon that draws it, as those editions' texture cache
  does - in the Wolf training missions the mechs' plain dark parts show orange-brown, as in the original.
- **Gravis Ultrasound music** follows the original driver exactly: pitch and vibrato in its integer steps, pitch bend
  re-tunes held notes with the bend-range setting, volume changes glide from where the envelope had got to, and samples
  play to their end after the envelope - drums no longer cut off after 3 ms.
- **Won't fix**: a few DOS-look HUD items (on-screen nav marker / target bracket, radar cone end and message line at
  640x480) can sit 1-2 px off the original.

## v0.4.11 (2026-10-22)

- **Dropships and gunships fly**: dropships and aircraft are drawn at their flying height, as in the original - the
  dropship on TEMPER EDGE (TAUP) now hovers about 75 m up and flies away over the hills instead of ploughing through
  them; the gunships on UMBER WALL and the aircraft on BRONSCN1 fly at their heights too.
- **External camera (C)** works as in the original: an orbit camera behind your mech that you steer - Shift+Z / Z out /
  in, Ctrl+grey Up / Down higher / lower, Ctrl+grey Left / Right or Keypad 7 / 9 round the mech; "External camera
  engaged.", HUD kept, no reticle; C again returns to the cockpit.
- **Radar zoom per mode**: the small and large radar each keep their own range (250 m - 2 km); X does nothing with the
  radar off.
- **/ (RESET_INPUTS)** recentres torso, view and mouse like Keypad 5.
- **Nav points without the autopilot**: walking into the selected nav marks it reached and selects the next one.
- **Inspect**: in the DOS look a building can be inspected from 200 m plus its own size, as in DOS.
- **Overheat alarm (DOS look)** follows the DOS heat rules: the alarm with the shutdown sequence, and again with "Heat
  level critical" when heat keeps rising.
- **Looping world sounds** (wind, machinery) start on their second update, at a quarter volume and a random pitch from the
  original's table, as in the original.
- **Shell**: the in-menu Esc / right-click menu now matches the original on every screen (layout, keys, return paths);
  Hall of Honor rebuilt from the original; Yes/No dialogs for exit, delete pilot and replay as in DOS; hover labels and
  button animations in Star Configuration / Mech Lab.
- **Mech Lab**: the variant list shows only the original's standard / alternate configurations plus your designs; header
  and callsign colours, the armour diagram, MASC slots, anti-missile counted as a weapon and the critical-slot texts as
  in DOS.
- **ATi look**: the ground and sky are no longer grainy and sparkling toward the horizon - they use the ATi edition's own
  mip levels (models, terrain and scrub still point-sampled, as the ATi edition draws them).
- **DOS look**: the cockpit struts are now pixel-identical to the original (whole-number projection and the original's
  draw order); distance fog is applied per polygon as DOS does (PROVSCN1's mountains, far terrain); your eye sits at the
  original's height (the MARO start view now lines up).
- **DOS look autopilot and AI** steer to nav points in whole degrees as DOS does - no more brief swerve near TNJ4's Beta.
- **Nav points** are only marked reached the way the original does it (the autopilot, the selected nav, or an objective) -
  walking near one no longer ticks it off.
- **Hit tests** use the model detail level a mech is drawn at (distant mechs are hit on their simpler shape, as in the
  original); mech-to-mech contact slides as long as in DOSBox.
- **Targeting**: unit targeting flags come from the mission data (e.g. the dropship bomb turrets can't be targeted).
- **Red damage flash** is skipped when a palette fade is already running, as in the original.
- **External camera**: the camera tilt control (bindable in Cockpit Controls) and the zoom / height controls also work on
  the death camera.
- **Star Configuration**: click a starmate's name to type a new one, as in the original; Instant Action names come from
  the star records as in DOS.
- **Mech Lab CUSTOMIZE**: ammo bins are numbered as in the original.
- **Gravis Ultrasound music** follows the original driver's volume curve, envelopes and vibrato.
- **Titanium**: animated textures step as Titanium's own engine does.
- **Rendition renderer**: RENDERER RENDITION draws the Titanium edition the way its Rendition Verite plugin
  (zredline.dll) did: Titanium textures, bilinear filtering without mipmaps, fog worked out per vertex, and missile smoke
  drawn as unkeyed squares. Needs the titanium/ folder.
- **ATi look**: the RUST and TEAL grounds are textured again; polygons drawn without transparency show their textures'
  transparent texels as the ATi edition does; fire / smoke cards and decals are textured without perspective correction,
  and everything is with Display detail Low, as in the ATi edition.
- **HUD at 1024x768**: the altitude ladder, compass, reticle and speed readout sit exactly where the original puts them
  (the ladder was 27 px too high); nav arrows are red as in the original; the speed turns blue while reversing.

## v0.4.10 (2026-10-21)

- **Tab no longer restarts the mission**: a leftover model-viewer key (Tab = next mech, reload) was live during missions,
  so Cmd+Tab restarted the mission.
- **Cmd shortcuts on macOS**: while Cmd is held the system gets its shortcuts back (Cmd+Tab, Cmd+Shift+3/4/5
  screenshots), and Cmd+key never reaches the game; Mission Control's Ctrl+arrows stay blocked in a mission.

## v0.4.9 (2026-10-21)

- **Mac app icon**: the small Finder icon (16 / 32 px) is no longer garbled - those sizes are now stored in the classic
  format macOS reads reliably.
- **Mac first launch**: the message for an app opened straight from Downloads now says to move it to Applications (or
  any folder) and asks for the game folder; README / FOLDER_SETUP updated (System Settings > Privacy & Security > Open
  Anyway, or the `xattr` command).

## v0.4.8 (2026-10-21)

- **DOS look shading as the original**: every shaded polygon (cockpit, mechs, buildings) now gets the DOS renderer's
  fine checkerboard between palette steps, at the original brightness (it was half a shade darker). The option is now
  `dos_shading=dithered|smooth` (covers the sky too; `dos_sky=` still works).

## v0.4.7 (2026-10-21)

- **DOS look sky**: the default now matches the original exactly (15 haze steps with the original's fine checkerboard
  between them); new option `dos_sky=smooth` blends them into an even gradient (GRAPHICS page / Combat Variables).
- **Laser range as in the original**: a laser shot loses its weapon's recycle time from its flight time, so medium lasers
  reach about 360 m, not their full table range (player and AI). AQUA's attackers no longer land laser hits from 500 m.
- **Damage display (H T A L)** scaled and ordered as the original: the best-armoured location fills half the window;
  the left / right bars were mirrored.
- **AI holds fire when another mech is in the way**; DOS-look area damage gives the head a quarter, as DOS does.
- **AQUA**: deaths now match the original (median about 43 s vs 45 s in DOSBox).

## v0.4.6 (2026-10-21)

- **macOS app finds your game folder**: it looks next to `MechWarrior 2.app` and, if the folder isn't there, asks for it
  with the system folder picker and remembers it (Windows and Linux ask too). No Terminal launch needed.
- **Wide-screen HUD**: on screens wider than 4:3 the instruments sit in the left and right corners (`hud_layout=original`
  for the centred 4:3 layout).
- **Legs hold still in the air**: the walk animation pauses while airborne (jumping or high drops) and resumes on landing,
  as the original does (player and AI).
- **AI fires its weapons in turn** (one weapon per trigger press, as the original): AQUA's Stormcrow no longer empties
  its AC/20 in seconds; deaths there now spread like DOSBox's.
- **ORANSCN1 trainees** start facing and steering as in the original; AI obstacle probes use the original's length (and
  the DOS look's own probe quirk).
- **World objects use their placement scale**: fires / smoke at the right size and turned to face you (MARO's street
  fire), training mountains and other scaled objects at the original size.

## v0.4.5 (2026-10-20)

- **Autopilot at obstacles**: drives straight, and after bumping into a building or mech turns away, follows the wall
  and carries on, as the original does.
- **Autopilot nav to nav**: walks on from one nav point to the next and stops only after the last one, as in the
  original; disengaging (turning) keeps your throttle.
- **macOS keys**: no accent popup on Z + arrows (text input off during play); Mission Control / Cmd+Tab blocked while a
  mission runs (`system_shortcuts=allow` to keep them).
- **Animated fires and smoke** in every map (bank-1 texture frame groups were not animated or textured).
- **Radar** hides powered-down units and shows the original's flagged things (e.g. MARO's cars).
- **PINKSCN1** ground scrub is back (a placement record with nothing to place was applied anyway).
- **Titanium PPC / gauss** shots tinted as in the original.
- **Mac app icon** replaced.
- Confirmed as the original: E takes the nearest enemy on every press (T cycles); the autopilot voice waits behind
  queued radio messages.

## v0.4.4 (2026-10-20)

Every change below was traced in the original game code and, where possible, checked against the original running in
DOSBox. Details and evidence: ASSUMPTIONS.md. Still open: KNOWN_ISSUES.md.

### Fixes from play reports
- **Static "fire" squares** at the start of the Wolf training missions (and RUST / SABL) are gone: they were flat ground
  patches drawn as standing explosion sprites.
- **Reverse keeps momentum**: the mech keeps its forward drive while the legs change stride (about 1.6 s), then backs
  up at half speed, as in the original.
- **N selects Nav Alpha first**: nothing is selected at mission start, so the first N (or Shift+N) picks the first nav
  point.

### Gameplay
- **Targeting as in the original**: an ordinary building needs Q twice; powered-down mechs are skipped by T / R (Q twice
  takes them); E and F follow the original's side rules.
- **No marker unless something is selected**: no nav marker at mission start or after a kill until you press N or
  engage the autopilot.
- **Jets while turning**: the jets give no lift while the legs turn - they double the turn rate instead (player and AI).
- **AI**: no more 250 m vertical jet climbs (the AI glides on its jets); AI ground jets build up speed; AI reverse keeps
  momentum.
- **Hit detection**: shots hit a mech's parts by their bounding spheres, nearest hit wins (units and buildings alike).
- **Torso easing**: the torso glides to its new twist / pitch over 0.2 s; Keypad 5 eases back to centre.
- **Jet fuel by edition**: the DOS look uses the DOS tank size and its slower whole-tick refill.
- **Airborne impacts**: the velocity after hitting a face uses the frame's end speed; sideways speed is kept in the air.

### Visuals
- **RENDERER = 3DFX**: ground scrub no longer shows as opaque squares with teal borders.
- **TEXTURES OFF** now works for mechs and the affected object classes (a rounding bug made it do nothing); with
  Titanium textures it shows the original's flat shading.
- **Titanium**: PPC and gauss shots use Titanium's own effect models.
- **3D start time of day**: every mission starts as on a fresh engine load, as in the original.

### Sound
- **Heard from the camera**: in chase and outside views, positional sounds come from the camera, with the original's
  left / right swap for combat sounds.
- **Menu sounds** in the in-mission menu at the original's volume and pan.
- **Death sound** is the explosion, halved when the death happens in the cockpit view, as in DOSBox.

### Shell
- **Archive**: the grey topics stay unclickable - confirmed they are not links in the original either.
