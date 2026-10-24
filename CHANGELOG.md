# Changelog

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
