# Changelog

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
