# Changelog

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
