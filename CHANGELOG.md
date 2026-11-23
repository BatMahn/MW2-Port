# Changelog

## v1.1.56 (2026-11-23)

- **Dire Wolf and Elemental.** The Dire Wolf's right-arm Clan decal clears the armour, and both stock Elemental
  weapons fire with the normal trigger.
- **Cockpit and audio.** Target selection waits until the player's mech is online. The enemy power-up alert plays
  when the contact appears on radar. It pauses any speech already playing, then lets it resume.
- **Golden Spade.** The palace guards stay at their mission-defined positions inside the shield. Satellite View
  hides the Shield-Generator and shows the Force Shield in blue.
- **Starmates and jump jets.** Starmates aim at a target's body, including low turrets. Directional jets keep
  applying thrust while turning in flight.
- **Navigation lights.** The Aquiline Fire wreck beacon and the Cold Crescent monorail and base lights stay legible
  at range with a minimum apparent size that scales with display height.

## v1.1.55 (2026-11-22)

- **Classic Mac on OS X.** Use the native Navigation Services folder picker at startup with matching reply
  records and cleanup. This addresses the folder-picker crash reported on Mac OS X 10.3.9.
- **Classic Mac on OS 9.** Route fullscreen clicks to the game so Combat Variables can open from the menu.
  Automatic fullscreen mission resolution uses 640 × 480. Higher resolutions remain available in Combat Variables.
- **Classic Mac rendering.** Reuse the menu background and texture upload buffer to reduce the work each frame.

## v1.1.54 (2026-11-22)

- **Classic Mac.** Fix the Mac OS X game-folder picker when Navigation Services uses extended dialog and reply
  records. Cache rendered menu text to reduce texture uploads.
- **OpenGL 1.2.** Batch DOS dithered sky and polygon fills and keep small textures such as the DOS palette
  lookup at full colour precision.
- **Blade Splint.** Hide the inactive effect pool so its flame sprite no longer appears in the crossroads.
- **Aquiline Fire.** Draw Wolf Clan decals clear of the crashed dropship's hull so they remain visible.

## v1.1.53 (2026-11-22)

- **Cockpit.** Ctrl+Z resets zoom to 1× and holds it there until the reset key is released. Looking down in
  the Mad Dog no longer exposes the leg panels.
- **Satellite View and target display.** Destroyed structures show as solid black silhouettes. Satellite View
  uses the ground colour of the current renderer. Selected buildings and their attachments share a blue wireframe.
- **Light amplification.** Palette fades update HUD textures when they are drawn and skip redundant DOS world
  palette uploads.
- **Aquiline Fire.** The crashed Wolf dropship has its flashing wing light. Destroying it sends five separate
  fragments and four smaller chunks flying.
- **Combat.** Titanium textures darken on damaged limbs. DOS heat override uses the current frame's whole ticks
  for its cook-off roll.

## v1.1.52 (2026-11-22)

- **DOS cockpit displays.** The rear view stays level as the pilot looks up or down, and the down view looks
  straight down from the mech's centre. The weapon camera follows the missile's position in whole centimetres.
  The target wireframe uses the original integer projection, and off-screen markers handle signed offsets safely.
- **DOS movement and audio.** The drive servo preserves its signed rounding residual. The cockpit speed readout
  includes sideways motion from collisions. Positional sounds use the original distance, volume, pan and delay.
- **DOS missiles.** A volley fired on the frame the lock appears stays unguided. The next volley can track.
  Guided missiles and PPC shots can prompt the selected enemy to dodge when the pilot is looking at it.

## v1.1.51 (2026-11-22)

- **Projectile flight:** Live shots continue below elevation zero until they hit something or expire.
  This fixes shots vanishing on their way to Golden Spade's shield generator and other low targets.
- **Mission sweep:** Recalculate aim from the final firing position and use world collision to find
  clearer firing angles and detect protected structures in the way. The sweep follows the shield's
  object hierarchy to attack its generator first. A stalled attack can be reported with
  `-no-fallback` without applying direct damage.

## v1.1.50 (2026-11-22)

- **DOS weapons:** Use the original DOS projectile lifetimes, launch intervals, cooldowns, damage, heat and
  ammunition capacities. LRM-15 volleys fire at the DOS cadence, SRMs stay in flight longer, and each LRM-5
  ammo bin holds 60 missiles. DOS AI weapon selection uses the original firing-roll divisor.
- **DOS mission objects:** Circling-object tasks now reproduce the original x86 fixed-point conversion when
  a speed or turn rate overflows. The DOS mission record BRO2SCN1 hit undefined behavior in this calculation.
- **Mission audit:** The headless sweep supports `-dos` and shares the game's edition setup, including
  DOS unit placement, terrain physics and weapon data.

## v1.1.49 (2026-11-22)

- **Jump jets:** Jets recharge during startup even when the jet key is held. The infinite-fuel cheat
  keeps movement prediction and steering active with a nearly empty tank. Player and AI jet flames and
  the player's jet sound now follow the controller's thrust state.
- **DOS comparison:** Added 144 cases that run the original DOS jump-jet controller beside the port
  and compare fuel, firing state and heat. The port's movement prediction is checked against the same firing state.

## v1.1.48 (2026-11-22)

- **Mission sweep:** Reports NetMech records as skipped when their original scene data calls for a network roster.
  The single-player sweep previously attempted to run those records with Instant Action lances. A playable mission
  that stalls or fails now produces a nonzero exit status.
- **Aquiline Fire sweep:** The scripted pilot tries a different firing angle and aim height when shots fail to
  damage a structure. It now destroys the dropship wreckage with projectiles and completes the mission without
  a forced kill in the 3D archive sweep.

## v1.1.47 (2026-11-22)

- **Mission launch with anti-aliasing:** The GPU backend selects a sample count supported by both the colour
  and depth buffers before warming up the mission pipelines. An 8× setting falls back on devices that do
  not support it, including the M4 Pro from the crash report.
- **CD soundtrack:** Mixed-mode CUE images use each track's sector size for the disc layout and audio offsets.
  This fixes shifted audio track starts when a disc contains 2048-byte data sectors and 2352-byte audio sectors.
- **Settings:** DOS filenames and the SC-55 ROM folder resolve case-insensitively, with their on-disk spelling.
  The settings writer returns an error if a write fails.
- **Tests:** `make check` builds the game on a clean checkout and accepts fixture paths with spaces.

## v1.1.46 (2026-11-22)

- **Configuration:** Saving settings keeps explicit asset paths alongside `game=` and preserves
  `textures_kind=3dfx`.
- **Cockpit MFDs:** Rear, Down, Weapon and Target Camera keep target brackets aligned in the main cockpit view.
  Target Camera fits the Target Wireframe border and shows only the selected mech or building at full detail
  against a black background.
- **Satellite View:** Powered-down units and units under roofs are hidden. The overhead dome and the unnamed
  hangar doors are also excluded, clearing stray shapes from the Velvet Hammer map.
- **Velvet Hammer:** The Falcon Nova and Mad Dog can shoot their own hangar doors under the existing mission
  orders. Mission completion can proceed when the audio device is unavailable, even if the mission has radio lines.

## v1.1.45 (2026-11-21)

- **GitHub release pages show the changelog.** Each release's notes are now this file's section for its tag; until
  now the release pages had no notes at all.

## v1.1.44 (2026-11-21)

- **Blade Splint: the helicopter's searchlight runs ahead of it, as in DOS.** The light pool hangs under the
  helicopter on a tiny backwards circle (1 cm across). DOS works that circle out in whole centimetres each frame and
  rounds the backwards step down to a full centimetre, so the pool creeps away from under the helicopter all mission
  long and ends up well in front of it. Checked by reading the helicopter's and the light's positions straight out of
  DOSBox's memory: the port now matches them to within a few metres. The helicopter's own flight path follows DOS more
  closely for the same reason (other circling objects too).

## v1.1.43 (2026-11-21)

Fixes from the latest bug report.

- **Blade Splint: the cars' headlight beams lie on the road.** The beam is a flat shape at road level. The roads are
  lifted 4 cm above the ground, and so were the cars' beams, so the two fought and the beam broke up into the road.
  Moving vehicles' ground-level parts now sit 8 cm up, above the roads.
- **Blade Splint: both of the helicopter's running lights stand out.** Since 1.1.42 only the light on your side did;
  the other was sunk into the hull, and the two swapped as the helicopter turned. The original draws polygons by
  their farthest corner, so both small lights end up over the hull's big faces. Both now draw in front of the hull.
- **Scorching Sand: turrets sit in the middle of the target display.** Turrets and doors have their centre at their
  base, but the port read that as "no centre given" and used 5.5 m, so the target display framed the space above the
  turret and drew it at the lower edge. Their centre is now their base, as in the data.

## v1.1.42 (2026-11-20)

Fixes from the latest bug report, each checked against the original DOS game in DOSBox.

- **Mission briefing: SITUATION opens its own page, as in the original.** The briefing page now shows only the
  orders. SITUATION opens a separate page with the BearNet text and the story, with EXIT to go back (and PREV PAGE /
  NEXT PAGE when it runs past one page). The port had put both on one scrolling page, with SITUATION jumping
  half-way down it. That was never intended; it had been that way since the first release.
- **Mission briefing text laid out as in DOS.** The briefing and situation text now use the original's thin font,
  its line breaks (no extra blank line after every line), its tab columns ("Codename:" / "Planet:" values line up)
  and its word wrap. The buttons are grey and turn white under the pointer. The mission's aftermath page uses the
  same text layout.
- **Blade Splint: the helicopters' running lights show.** Each light is a small cube set into the helicopter's hull.
  The original draws whole objects nearest-last, so the light on your side is drawn over the hull. The port's depth
  buffer hid most of it (the green light looked flat, the red one was missing). The light nearer you is now drawn
  in front of the hull, as in DOS.
- **Blade Splint: the cars' headlight housings stand out of the car.** They are boxes that reach out of the nose from
  inside it, listed after the body. The original draws an object's polygons in that order, so they show whole. The
  port showed only the part outside the body. Small pieces attached to a moving vehicle are now drawn over its body
  when they are on your side of it.
- **Sable Flame: the Falcon dropships aim at your torso.** Turrets and turret-class dropships now aim their pitch
  from their eye at your centre, not your feet. Aiming at your feet could put the shots into the ground in front of
  you.

## v1.1.41 (2026-11-20)

A fuller audit of the DOS look against the DOS MW2.EXE, closer to the one done for the Ghost Bear's Legacy port. Six
more emulated tests (16 in all, about 18,500 checks), and a frame-by-frame comparison with DOSBox across 16 missions.
docs/AUDIT_DOS.md lists what was checked, what turned up, and what still isn't covered. All changes are to the DOS
look; the 3D looks are unchanged.

- **Marooned: the city dome's struts show at any distance.** The original draws a scene's sky object (here the dome,
  its struts and windows) with no distance limit. The port hid a strut that crosses the sky from the start position.
- **Enemy AI, from the original's code:**
  - Flanking (manoeuvre 1) picks one of eight slots round its target, 150 m out, and units share the slots out as in
    the original. Manoeuvre 7 now picks a point round the unit itself, as in DOS.
  - The patrol torso sweep runs whether the unit moves or not, at the original's timing.
  - The jump checks use the original's view test, and a turning jump lasts until the landing.
  - A unit just given a target waits for its first fire decision as in the original, and its rules run in more states.
  - A dropship or flanking unit's slot is freed when it stops attacking.
  - When an enemy runs out of weapons and jumps, a second jump no longer follows. The original clears the flag for it.
  - A missile dodge always fires the left jet, as in the original.
  - A jump is cancelled only by a target off to one side of the torso's twist limit, as in the original.
  - The missile warning reaches a unit that isn't attacking yet. Its dodge waits for the next manoeuvre.
- **Turret, dropship and mech aim:** the torso aim moves in the original's steps (1024ths of 180 degrees across, of 60
  up and down).
- **Missile lock:**
  - The timed lock takes the original's 364 ticks (it was 362).
  - A computer pilot of skill up to 3 can also lock at once when it fires a guided weapon, as in the original.
- **Moving scenery:** path-following objects spin and circle at the DOS engine's clock.

## v1.1.40 (2026-11-20)

Fixes from the latest bug report (on 1.1.37). Each one was checked against the original DOS MW2.EXE running in DOSBox.

- **Wolf 3: the cars' headlight beams show.** A car carries its headlight beam on the road surface. In the port's
  depth buffer the road hid it. The beam, and a car's underside, now sit 4 cm above the road.
- **Wolf 3: the cars' yellow headlamps no longer sink into the car.** The lamps sit about a centimetre in front of the
  nose, so the two fought in the depth buffer from some angles. Flat details lying on another face of a moving vehicle
  now stand 3 cm out from it.
- **Wolf 3: roof lights.**
  - They now go out of view with the rest of the scene, at the planet's view distance. Before, you could see them
    across the whole map before the buildings.
  - They are now pinpricks: one pixel of the DOS look's 640 x 480 screen, at any distance. Before, they were 1.2 m
    squares.
- **Sable Flame: the Sensor Array Dome's wireframe is grey.** The DOS models' outline colour was read from the wrong
  bits of the colour word, which made the dome red.
- **Sable Flame: the dome appears at the view distance.** Outlines are now culled like everything else, so the dome
  shows from about 900 m to 1 km, as in DOS, not from 1.3 km and beyond.
- **Sable Flame: the Falcon dropships hit what they aim at.** A dropship's eye, where its aim starts, sits 17 m out on
  its nose. It aimed from its centre, so the shots passed beside you, left or right. Turrets and dropships now aim
  from their eye.
- **Radar:**
  - The view cone's arms reach the circle on wide screens. Their ends were worked out with the whole window's aspect,
    which squashed them short of the circle.
  - While your mech powers up, the radar now uses its normal colours. Before, the circle and arms were a brighter yellow
    until the mech came online.
- **Abort Mission ejects you, as in the original.** The menu's Abort is the eject command: "Ejecting.", the camera
  rising and spinning over your mech, "Mission failed.", then "Press any key to exit...". The port had ended the
  mission at once with a plain fade to black.
- **Shell: no mouse pointer over the movies.** The pointer is hidden while an SMK movie plays and comes back after it.

## v1.1.39 (2026-11-20)

A full emulated comparison of the DOS look's sim against the DOS MW2.EXE, done the way the Ghost Bear's Legacy port
did its own. The original routines run in an x86 emulator next to the port's code on the same inputs (ten tests,
about 20,000 checks: heat, heat sinking, override roll, nav steering, leg turning, servos, turret twist, collision
damage and manoeuvre timings). The tools are in tools/audit, with the results in docs/AUDIT_DOS.md. Two differences
turned up, both now fixed for the DOS look:

- **Heat on Hard in cold weather:** below -30 degrees your heat sinks now shed heat at MW2.EXE's rate (67 in 16.16
  fixed point per sink a tick), not the 3D-card edition's 67.5. That is under 1 % slower.
- **Leg turning while reversing fast:** MW2.EXE limits the speed term of the leg turn only going forward. Reversing
  faster than 80 km/h, the legs now turn less, and past 90 km/h they turn the other way, as in the original. The port
  had treated fast reversing like 80 km/h.

## v1.1.38 (2026-11-20)

Fixes from the Ghost Bear's Legacy port. GBL runs on the same DOS engine. Its port tested GBL's MW2.EXE routines
under emulation and against DOSBox. Each fix below was checked in MechWarrior 2's own DOS MW2.EXE before it went in.
Some of GBL's fixes turned out to be GBL-only and were left out: the -100 degree heat tier, the distant-sound fade,
the CT hit spread, the vehicles' manoeuvres and the cheat codes.

Unless noted, these apply to the DOS look. The 3D looks keep the 3D-card edition's rules.

- **Leg turning eases in and out.** The legs turn through a servo, about 0.3 s each way, for you and the enemy
  (MW2.EXE servo +0x34, T 54 ticks). Before, a turn started and stopped at once. Computer-run units no longer turn
  on the spot. Nothing turns its legs in mid-air without jump jets.
- **Turrets:**
  - They aim at the original's slower rate: 0.8 s on both axes. Their gun tilt had been four times too quick.
  - A turret that can turn all the way round now turns the short way when you circle behind it.
  - Mechs' torso servos use the DOS timings (0.6 s / 0.2 s at 182 Hz).
- **Enemy manoeuvres:**
  - Every manoeuvre's timing is the DOS edition's (182 Hz clock).
  - An enemy backs off a contact only when it isn't already reversing or landing from a jump.
  - It attacks a building or nav point with its plain pattern.
  - A dodge (manoeuvre 11) uses the jet the original picks, from the side the shot came from.
  - Aircraft circle their target 100 m out (manoeuvre 12), dropships use the flanking slots, and other vehicles
    charge straight at you. Elementals use their jumping table. Before, every unit used the mechs' table.
  - Your own missiles and PPCs never make an enemy dodge. Only computer-run pilots' shots trigger a dodge
    (MW2.EXE 0x2e450).
- **Heat:** overriding a shutdown at high heat now risks an ammunition explosion about once every 1.5 minutes, as
  the DOS edition rolls it. The port had used the 3D-card edition's rate, 14 times higher.
- **Ammunition explosions:** an enemy mech whose ammunition explodes is out of the fight at once (MW2.EXE 0x25c7d).
  Before, that only happened with automatic ejection on.
- **Collisions:** ramming and landing on another mech do the DOS damage. The speed is measured the DOS way. Landing
  on a heavier mech's head does 4 x its tonnage ratio, not 2 x. The struck side is worked out from the torso's facing.
- **MASC** boosts speed only while your mech is striding: not standing still or with the jets firing. "MASC" now
  shows left of the kph readout while it is engaged.
- **Damage display:** a location turns yellow and red by its armour and internal structure together, as the
  original works it out (MW2.EXE 0x2f611). It no longer goes red while the structure is still whole. The same levels
  colour the target display and image enhancement.
- **HUD:**
  - When both message lines are busy, a new message replaces the one that would stay up longer. Toggling the
    autopilot no longer wipes "MASC engaged."
  - The heat bar always reads "Heat": the DOS edition has no "Shutdown..." or "Overridden" label.
- **Missile camera (F10 / F9), all looks:** it follows your newest missile. Before, a laser or gun shot fired after
  the missile left it nothing to follow.
- **Flee to DOS** from the in-mission menu sets off your mech's self-destruct, as the DOS edition does. The game
  closes once the mission has ended.
- **Sound:** object loops (machinery, fans) play on their own channels (MW2.EXE 0x40e90). They no longer use up the
  eight channels weapons and footsteps need.
- **Hall of Honor:** pilots with equal rank, honor and missions are listed in the order the original's own sort puts
  them (Watcom's qsort, MW2SHELL.EXE 0x48dc1).
- **All looks:**
  - The fade from black at the start of a mission lasts its full 2 seconds at any frame rate. At 144 fps it had been
    over almost at once.
  - Slow first frames no longer make the start-up jump ahead.
  - The graphics pipelines are built while the drop screen is up, so a new install's first mission doesn't stall.
    A Direct3D 12 driver that can't run the game's shaders now falls back to Vulkan instead of stopping the game.

## v1.1.37 (2026-11-20)

- **Re-release: the GitHub v1.1.36 was built from v1.1.29's sources.** The v1.1.36 commit on GitHub had the
  v1.1.29 files in it, probably because an old extracted folder was copied over the repository. So the v1.1.36
  downloads had none of the fixes from v1.1.30 to v1.1.36: the shell sounds stopping when you leave a screen
  (the title's fire loop), the map viewer, the LOD cap and the rest. This release has them all. No game changes
  beyond v1.1.36.
- Tests: `MW2_SHELL_PUSH` (scripted clicks for a shell running with a window) and a stop entry in the sound log, used to
  confirm that the title's fire loop stops on leaving the title.

## v1.1.36 (2026-11-20)

- **Level-of-detail distances capped at the original's largest screen.** The engine scales its level-of-detail
  distances with the screen size. The original's largest is DOS's 1024 x 768, where mechs switch to simpler
  models at 115 m, 272 m and 720 m. The port kept scaling above that: at 1440 lines a mech kept its second model
  out to 510 m, where DOS already shows its third (user report: DOS at 442 m against the port at 445 m). Larger
  windows now keep the 1024 x 768 distances; smaller ones still scale down as the original does.
- **DOS look: the horizon glow in missions without a horizon record.** Seven missions (Scorching Sand, Trial 1,
  Trial 2, Trial 3, Golden Spade, Trial 4, Arkham Bridge) have no HRZM record. The original then draws its default
  band, 36 lines at 320 wide, graduated from the horizon colour up into the sky. The port drew no band there, so the
  sky was one flat dark colour. Scorching Sand's dusk and pre-dawn skies now glow at the horizon.

## v1.1.35 (2026-11-20)

- **Shell sounds stop when you leave a screen.** The Clan Hall's ambience used to carry on into the Ready Room, the
  Register and cadet training. The title screen's fire loop carried on into the intro, the Register and the Trials
  of Grievance. Each screen's sounds now stop when you leave it, as in the original.
- **Jade Falcon Clan Hall ambience.** The Falcon hall's ambience is the sound track of its ball clip (AJFBALL), looped
  with the clip; it now plays. (The Wolf hall keeps its DATABASE ambience.)
- **Shell at 1920 x 1080: no more black bars on all four sides.** The 640 x 480 shell was only scaled by whole
  numbers, which gave 1280 x 960 on a 1080-line screen. It now fills the height at 4:3 and stays sharp: SDL 3.4's
  pixel-art scaling keeps every pixel the same size. In full screen the mouse pointer is kept on the picture instead
  of going behind the side bars.
- **AI can no longer shoot through hills and mesas.** The AI's line of sight only tested buildings, not the large
  terrain parts (the mesas, ridges and ice shelves). On Burning Chrome, the Mad Dog's and Summoner's LRMs and the turret
  on the ledge fired through rock. Those parts now block line of sight.
- **Sable Falcon dropship turrets aim down.** Turrets and dropship guns worked out their aim from their base, not
  from their gun height. Turrets high on the dropship couldn't depress their guns at a mech close by. They now can.
- **Scorching Sand: Jenners brake their drop with jump jets.** An AI mech with jump jets falling more than 2 m now
  fires its jets to slow its fall, instead of dropping off the mesa at full speed.
- **Wolf 3: the cars' roof lights blink.** Light bars are single-point polygons that cycle their colours; the
  renderer skipped polygons with fewer than three corners. They now draw as small lit dots.
- **Sable Flame: the Sensor Array Dome is see-through in the DOS look.** The DOS models' outline polygons (colour
  type 2) were filled in solid; they are now drawn as outlines, as in the 3D looks, so you can see into the dome and
  through it.
- **Satellite view:**
  - Fires drew as black lines in the DOS look. They now show their flames.
  - Lowering the range with a target outside the map left a red dot on your own mech. The target's marker is now
    placed at the edge of the map, in its direction.
- **Target display: turrets centred.** Scorching Sand's turrets sat at the bottom edge of the target MFD; turrets are
  now framed on their own height.
- **Light amplification: frame rate.** The palette table was uploaded to the GPU every frame while the fade or light
  amp was active. It is now uploaded only when it changes.

## v1.1.34 (2026-11-19)

- **Windows: stripped executables, after an antivirus false positive.** One scanner of 71 on VirusTotal (Microsoft
  Defender's machine-learning verdict `Program:Win32/Wacapew.C!ml`) flagged v1.1.33's `mw2.exe`. The port is not
  malicious; that verdict is a heuristic score that new, unsigned programs often trip. The build had shipped with
  its debugging data left in:
  - the DWARF sections, which have odd names like `/4` and `/19` that scanners tag as "obfuscated";
  - a 1.5 MB symbol table after the code, which they tag as an "overlay".

  The Windows `mw2.exe` (64-bit and legacy) and `mapview.exe` are now stripped. `mw2.exe` drops from 4.9 MB to
  3.0 MB, with no overlay and only the standard sections. Nothing else changes.

## v1.1.33 (2026-11-17)

- **Map viewer: export to Blender.** New keys in the map viewer:
  - **X** exports the selected building or unit as a glTF 2.0 file (.glb). A building comes with its child parts and
    its destroyed variant; a unit comes in its rest pose.
  - **Shift+X** exports the whole map as one .glb. It holds the world, the destroyed variants (in their own group),
    the units on the ground, the nav points (as named empties) and the ground.
  - **T** exports the mission's textures as PNG, with the animated slots' frames and the scrub bitmaps.

  Files go to the game folder's `exports/<SCENE>/`. Every .glb opens in Blender and passes the Khronos glTF
  validator. Every object and material keeps the game's names (record, OBJ node, model, texture slot) in its glTF
  extras, so edited models can later be matched back to the game; loading mods into the port is a later step
  (README "Map viewer").
- **Map viewer: units stand on the ground.** The units were drawn at their spawn records' height, the Stormcrow on
  Wolf 3 50 m up; they now stand on the ground under them, as the simulation puts them. Clicking now picks the
  polygon under the pointer, so tall buildings and the towers behind units can be selected.

## v1.1.32 (2026-11-17)

- **New: the map viewer** (`mapview`; README "Map viewer"). A separate program in the Windows, Linux and macOS
  downloads (macOS: `MW2 Map Viewer.app`) that shows every mission map (campaigns, training, Instant Action) in the 3D editions' look:
  - What it shows: the world as it stands at the start (destroyed variants hidden, the scrub patch, the fire cards
    animated), every star's units on the ground, the nav points and the mission boundary.
  - Two views: a free-flying camera, and a map straight down with north up.
  - Clicking an object, unit or nav point shows its mission data: the record and OBJ node, position and size, a
    building's name, side and hit points; a unit's loadout, group, state table, side, pilot level and alert ranges.
  - It is read-only, uses the game's own folder (mw2port.cfg), and needs the `3d/` files.

## v1.1.31 (2026-11-17)

From a playtest of Wolf Trial 1.

- **Trials never promoted the pilot, and the aftermath read "the rank of 0".** Two faults:
  - **No promotion.** The original raises the pilot's rank title after a won trial (MW2SHELL 0x1000f0d5, added at
    0x1000f3a2): one step per primary objective completed, capped at Khan. It only does this when the mission is a
    trial in the campaign table, heat tracking is on, and none of unlimited ammo, invulnerability or no collision
    damage is set (0x1000dd00). The port never promoted, so every pilot stayed Mechwarrior.
  - **Text codes not replaced.** The trial aftermath texts use the shell's codes (0x10002f00):
    - `\r0` / `\r1` / `\r2`: the rank title, plus 0 / 1 / 2;
    - `\h`: the pilot's honor;
    - `\q`: the debriefing's objectives table.
    The port dropped the backslash and printed the "0". These pages, and the briefings, now show the new rank (Trial 1:
    "Star Commander"), the objectives and the career honor.
- **A mech with a leg shot off could still turn on the spot.** Losing the first leg stopped it walking, but its legs
  kept turning, so it came about to face you wherever you went. Now a legged mech, AI or player, only twists its torso
  within its limits, so you can circle behind it. A turn on the jump jets still works, and Feet-to-Torso does nothing
  with a leg gone. Reported against the original; the engine's rule was not traced.

## v1.1.30 (2026-11-17)

From a follow-up to the v1.1.24 feedback list.

- **Satellite view: enemy buildings were shaded like the ground, not outlined in red.** v1.1.24's satellite view
  colours a structure by its side only when its object's type word has bit 0x200. The engine sets that bit at load on
  each object a GT (thing) record claims. The OBJ records themselves carry 0 or 0xc0, and the port never set the bit,
  so no building ever matched. The port now goes by the GT link: a thing's object is drawn black with its edges in
  its side's colour (enemy red, friendly green, unaligned grey). Seen on Wolf mission 3 (TEAL).

## v1.1.29 (2026-11-17)

From a GitHub report.

- **Satellite view: your mech drawn under a hill.** The view sorted whole objects by their highest point, so a hill
  taller than your mech covered it as you climbed its side. Polygons are now drawn one at a time, lowest first, and
  units always over the ground: seen from straight above, nothing stands over a mech on any map.
- **Falcon mechs wore the Wolf insignia at a distance** (Wolf mission 2's Stormcrow and every other enemy). The
  low-detail copies of a mech take its insignia from the mech's own parts, which were never given their star's
  value. They are now.
- **Mission Briefing clip played full screen.** Like the Archive's holoprojector in v1.1.24, the Ready Room's
  briefing clip now plays in the room at its own place (awobrief / ajfbrief), then the briefing opens.
- **Clip sounds in the shell.** The original plays every clip's own soundtrack. The port dropped it for clips played
  in place, so the holoprojector lost its chime (the "accessing" voice was already there). The holoprojector,
  briefing and Mech Lab holotable clips now play their sound.
- **Ambient sound in the Wolf clan hall and the Cadet Training room.** The hall loops DATABASE sound 0x4f; the
  training room loops 0x4c once its menu panel is in (both at the original's volume 0x32, from the Windows 95 shell).
  The Jade Falcon hall has no ambient loop in the original.
- **Levels of detail now follow the screen resolution.** The engine scales its level-of-detail distances with the
  view's size in pixels (m_detailScale = half-width × fov / 160). The port used the 640 × 480 distances at every
  size, so mechs dropped to their first low-detail model at 72 m. At 1080 lines the switches are now at 162 m,
  382 m and 1 km (2.25× the old distances), at 1440 lines 3×.
- **Trial 1: the second star woke for missiles passing over it.** The port treated the player as attacking whatever
  it had targeted as soon as it fired. The original counts the player as attacking a unit only once a shot hits it
  (RecordAttack on a hit), so a near miss no longer wakes a unit; a hit still does.
- **Sable Flame: the dropships woke at 750 m with "Enemy power-up detected".** Two fixes:
  - A resting unit (a hold node) now runs its rest rule, as the engine does for every AI unit. The rest rule wakes
    it when the player comes within its rest-alert range: 1 km for these dropships. The port had run no rules for a
    powered-down unit, so resting units woke only on their star's next node. This applies in every mission.
  - "Enemy power-up detected" is posted by the mech controller's power-up. Turrets and the turret-class dropships
    power up through the artillery controller, which posts nothing, so they no longer trigger it.
## v1.1.28 (2026-11-16)

- **"Flee to ..." names the system you're on.** The main menu said FLEE TO DOS (from the DOS shell) and the in-game
  menu said Flee to Windows (from the Windows 95 game). Both now say Windows, macOS or Linux, by build (Mac OS on the
  classic Mac build).

## v1.1.27 (2026-11-16)

After v1.1.26, every mission was checked for the same kinds of fault, not only the ones players reported: all 59
scenes (campaign, trials, training, Instant Action). The checks were the objective sweep, in which a scripted player
completes every objective (all 59 missions can be won, DOS data), and a new static audit run on both the DOS and the
3D data, `make audit_missions`. The audit looks for units placed far from their group or outside the mission boundary,
objects drawn at the map origin, destroyed variants shown early, and units left floating or underground once the
simulation settles. The v1.1.26 fixes covered what it found in the data. One more fault came from v1.1.24's turret
change:

- **Doors, aircraft and dropships no longer turn the way turrets do.** v1.1.24 made each unit's first two THNG nodes
  its torso and its pitch object, so turrets would turn. The engine does that only for the unit classes that use its
  mech or artillery controller. Doors (class 7) have their own controller, and their animation was being overridden.
  Where a model's torso and pitch objects are the same node (aircraft, dropships, the Tarantula), the engine applies
  the pitch first and the torso last, so the torso's rotation wins. The port had been adding both.

What the audit flagged but matches the original data:
- **Trial and starting-lance drops:** some trial opponents and lancemates start up to 46 m in the air and drop in at
  the start, as their start points place them.
- **Overlapping Summoners in Blade Splint, DOS look:** the DOS archive's "star" formation has only two follower
  positions (its other slots are 0,0), so the fourth Summoner of its group starts inside the leader. The 3D archives'
  "star" has six.
- **Aerospace and dropship units start outside the boundary:** they fly in.

## v1.1.26 (2026-11-16)

From a GitHub report on Burning Chrome (Wolf mission 7) and the Mech Lab. The first two were older than v1.1.24, not
caused by it; checking them turned up the same faults in other missions.

- **A wrecked dropship stood in the middle of Burning Chrome; you could walk and shoot through it.** A destructible
  object comes in two versions: intact, and the destroyed variant that replaces it when it is destroyed. The engine
  does not create the destroyed variant until then. The port only hid destroyed variants whose models are named SA*,
  so others were drawn from the start. This mission's dropship wreck and three crashed Aerotech fighters stood at the
  map centre. Destroyed variants (and everything attached to them) are now hidden until the swap, whatever they are
  called. Across the 32 campaign missions this hides 413 such pieces. Most are rubble inside the intact buildings.
  The rest are stray wrecks: dropship wrecks in 15 missions (most at the map centre or inside the landed dropship),
  crashed fighters, and wrecked cars at the map centre in Blade Splint (Wolf 3).
- **Four of Burning Chrome's turrets stood hundreds of metres from where they belong, one beyond the ice wall.** A
  formation's first slot is the leader's own, and the engine places every other slot relative to it. The stock
  formations have the leader at 0,0, but a mission can define its own, and Burning Chrome's turret formation gives
  map coordinates. The port ignored the leader's slot, so the followers stood at the leader's position plus the
  map coordinates. Fixed; this also moves turrets in Scorching Sand (Wolf 8, which were 2.6 km out) and Golden
  Spade (Wolf 15).
- **Mech Lab: switching to Endo Steel added the criticals but saved no weight.** The weight only counted Endo Steel
  once its 7 slots were assigned. It now counts as soon as it is chosen, and Ferro-Fibrous armour the same.
- **A weapon destroyed by a critical hit showed red** (as though recycling) instead of dark in the weapon list. The
  list now follows the engine: a destroyed slot is dark whatever else is true of it.

## v1.1.25 (2026-11-15)

- **Music from the original CD in a drive.** Put the disc in any drive (USB included) and the game reads the music
  digitally while it plays, with the disc's own table of contents: Windows XP and newer, Linux and macOS (the classic
  Mac build excepted). It is the last place the game looks, after ripped tracks and a BIN/CUE image. `MW2_CD_DRIVE`
  or `music=cd:E:` picks a drive. A reader thread stays about 3 s ahead of the music; when the game jumps to another
  track, the music waits for the drive to seek, as the original did.
- **BIN/CUE images: more places to put them.** If `cd=` names a disc image file, the game also looks for the `.cue`
  in that file's folder. The README now lists where the files go.
- `cdtool render` waits for a drive that is still reading ahead instead of stopping early.

## v1.1.24 (2026-11-15)

From a long feedback list on GitHub.

- **Sable Flame: groups, power-up order, radar, dropship logos.** The mission's MTBL tables were read by their order in
  the file, not by the table number each one carries. In SABL the two differ, so stars were spawned from the wrong
  tables. This caused the 3-2-3 groups (should be 2-2-4), groups waking in the wrong order and showing on radar early,
  the third group chasing the dropship, and Falcon dropships showing the Wolf insignia. Tables are now looked up by
  number.
- **Sable Flame: the sensor dome was opaque.** Its polygons are colour type 2, which the engine draws as outlines, not
  fills. In the 3D looks those polygons are now drawn as thin unlit lines in their palette colour.
- **Turrets did not turn (Wolf 2, Sable Flame dropships).** Turrets aim with two nodes, twist and pitch, named by the
  model's THNG chunks. The port only turned the twist node of mechs. Both nodes now follow the unit's aim.
- **Wolf Trial 1: the mech pairs started late.** The trial's player record is `jennstar`, not `UserStar`. The port
  matched the player's star by name, so the trial's logic never saw the player. The player's record is now found by
  the record that holds the player's start position.
- **Satellite view (radar mode 2) redone from the engine's map view.** Terrain is shaded by height. Units and buildings
  are drawn black with outlines in their side's colour (your mech included; buildings red). Shots are yellow lines,
  and fires and explosions show as their sprites. The reticle and target brackets are no longer drawn over it.
- **Ctrl+Up / Ctrl+Down looked the wrong way.** The pilot's look now uses the engine's sign: Ctrl+Up looks down, as
  in the original.
- **The reticle felt "floaty".** The view now follows half the torso pitch, as the engine does, while the reticle is
  projected along the weapons' full aim.
- **F5-F8 (lower display).** Each key now shows its display, and pressing it again clears the display, as in the
  original. The MFD cycle key uses the same modes.
- **Zoom (Z) hid the cockpit.** The cockpit frame now stays drawn while zoomed.
- **Light amplification dropped frames when turned on.** Repainting the HUD for the new palette took about 35 ms. The
  readings are now cached per sprite and prepared at mission start, which brings this to about 1 ms.
- **A red line beside the target brackets.** HUD sprites now clamp at their edges instead of wrapping.
- **Visited nav points now dim** in the target display, with the engine's visited emblem and colour.
- **Helicopter rotor missing in the target display.** The wireframe of a building or vehicle now includes its child
  parts (the fuselage and the parts in their own colours).
- **Weapon hits on buildings** now show the structure hit effect instead of the ground splash.
- **Menu cursor.** The shell now draws the game's own cursor (DATABASE.MW2 shape) instead of the system pointer.
- **Holoprojector.** In the clan hall, ARCHIVE now plays the holoprojector clip in place on the projector, as the
  original does, before opening the archive.
- **Original CD audio.** Besides FLAC / MP3 / WAV rips, the music now plays from a BIN/CUE image of the CD (table of
  contents from the cue sheet, pregaps included) and from AIFF tracks (as macOS shows an audio CD's tracks). A cue in
  `music/`, `cd/` or the game folder is found when there are no ripped tracks. Playing a physical drive is not
  supported. See "CD music" in the README.

## v1.1.23 (2026-11-15)

- **PPC ground hits sometimes showed a black square (TEXTURES TITANIUM, reported on GitHub).** A PPC that hits the
  ground plays a 28-frame blue explosion, A9EXP401-428. The Titanium archive stores frames 423-432 as type 0: no alpha
  channel, a blue cloud on solid black. The port took the Titanium frame whenever the archive had one, so the last six
  frames of the explosion were drawn as an opaque black card. Effect frames of type 0 now fall back to the 3D archive's
  frame of the same name, as the frames Titanium lacks already did. In the 3D archive those frames are fully
  transparent, so the explosion fades out as it does in the Win95 editions. The other looks were not affected.

## v1.1.22 (2026-11-15)

- **Jade Falcon mission 3 (Mirror Cage): the dropship could not be hit.** Reported on GitHub: the lasers passed
  straight through it. The dropship is a unit, TURRETE, whose model hangs its hull 93 m off the unit's position (the
  skeleton's OBJ 1: -8382, 1805, 4020). The model is drawn there, and the engine tests a shot against the pieces where
  they are. The port first culled units whose position was more than 20 m from the shot's path, so the hull was never
  tested. The cull now reaches as far as the unit's pieces do. TURRETE is the only unit model built this way.
- **Flickering ground with TEXTURES TITANIUM (same mission, reported on GitHub).** The Titanium terrain tiles have flat
  polygons lying on the ground plane, and they fought the ground fill in the depth buffer as the view moved. The original
  tiles have no such polygons. With Titanium textures the ground fill now writes no depth, so the terrain drawn over it
  always wins. The Titanium engine draws the ground first and has no working depth test of its own.

## v1.1.21 (2026-11-14)

- **Steam Deck: launching a mission could drop back to the Steam UI.** Reported on GitHub; "Resume Game" brought the
  mission back. The shell and the simulation are two processes. The shell hid its window before the simulation opened
  its own, and in that moment the game had no visible window. Under gamescope (the Deck's game mode) that can count as
  the game closing. It happened only some of the time because it was a race. Under gamescope (GAMESCOPE_WAYLAND_DISPLAY
  set, or XDG_CURRENT_DESKTOP=gamescope) the shell's window now stays up, cleared to black, under the simulation's
  fullscreen window, and it is raised again when the mission ends. MW2_KEEP_SHELL_WINDOW=1 or 0 forces either way on
  any system. Elsewhere nothing changes.

## v1.1.20 (2026-11-14)

Checked against the demech2 decompilation of the Windows 95 1.1 and 3Dfx DLLs. It was used as a reference only; none
of its code is in the port.

- **Cheat codes corrected.** Three were wrong in v1.1.17:
  - **`flygirl`** gives a mech with no jump jets three, with a full tank and three times gravity's thrust. Its flag is
    read through the steering record, which a search for the address missed.
  - **`enolagay`** sets off a nuke at the target. The NUKE effect plays, and a blast grows 200 cm a tick out to 4 km
    over twice the effect's life. Every unit inside takes ticks / 16 on each location, the player too.
  - **`lairdo`** widens orders to all points to every unit on the player's side.
- **`gankem`** now counts the kill for the player.
- **`lemmy` added.** It is the 3D editions' code: invulnerability, unlimited ammo, no heat, jets and endless jet fuel at
  once.
- **`dorcs` is the programmers' easter egg.** It was not the VFX1 headset set-up. The full sequence plays:
  - the view breaks into blocks and back;
  - it collapses like an old TV switching off;
  - the two pictures fade in from the `VFX` folder;
  - the "MW2 Programmer Dorcs Page" menu of the 13 programmers opens, each with a page of notes.

  The pictures come from the BattlePack or Mercenaries disc. The texts are read from a Windows 95 `MW2.DLL` in the game
  folder (README, FOLDER_SETUP).
- **Fires heat mechs.** This was missing until now. With heat tracking on, a mech near a fire or smoke card gains heat,
  more the closer it is (HeatMechsNearFires, Win95 0x1006c362 / 3Dfx 0x10046e70). YELLSCN1's burning wreck adds about
  5 heat in 6 s standing in it.
- **Area damage distance.** The 3D editions measure it with the octagonal estimate, as the DOS edition does
  (ApproximateVectorLength, 3Dfx 0x10030330). The port had used the exact distance outside the DOS look.
- **High frame rate bugs.** ReMech2 lists missile self-hits in zero-length frames, jet refill and the overheat random
  table. The port was checked and is not affected by any of them (ASSUMPTIONS §30).

## v1.1.19 (2026-11-13)

- **Wolf mission 2 (Flame Tongue): the targets of opportunity were missing near Nav Gamma.** The camp there, a tent, a
  radar, trucks and a 'Mech loader, is the briefing's tertiary objective. The port stood it at the map's origin, far
  outside the area you play in. The camp is in ORANARE4, which ORANMA03 includes with no BLKX placement of its own. The
  engine then places the camp where its includer is: ORANWLD1 puts ORANMA03 at (320000, -75000). The port only placed a
  record that had its own BLKX. Now an included record without one takes its includer's placement. Across every
  mission and every edition's archive, this is the only record affected.

## v1.1.18 (2026-11-13)

- **Group fire fired the wrong weapons.** Reported on GitHub: with group fire on, the trigger did not fire the weapons
  you expected, and missiles fired without their group being chosen. The port kept its own "current group". It started
  at group 1 and changed only with NEXT_WEAPON_GROUP, so the trigger fired group 1 whichever weapon was selected. Group 1
  is where every weapon starts, so moving lasers to group 2 left the missiles firing.
- **The original has no separate current group** (Win95 MW2.DLL 0x1005dec0 / 0x1005f3c8). In group fire the trigger
  becomes FIRE_WEAPON_GROUP and fires the selected weapon's group. The port now does the same:
  - **Trigger with group fire on, and FIRE_WEAPON_GROUP (;):** fire the group the selected (boxed) weapon is in.
  - **weapon_fire_group_1..3 (NumLock, keypad /, keypad \*):** select a weapon in that group, keeping the selection if
    it is already there, then fire the group (0x1005eec3). An empty group fires nothing. The port used to fire
    everything when the current group was empty.
  - **NEXT_WEAPON_GROUP (') and weapon_cycle_group:** select the first weapon of the next group that has one
    (0x1005ed6b).

## v1.1.17 (2026-11-12)

- **The Windows 95 edition's cheat codes.** MW2.DLL keeps 21 codes, stored XOR 0x1a. In a mission, with Ctrl, Alt and
  Shift held, each key goes into a 15-character buffer, and the end of the buffer is compared with every code in turn
  (0x1004b930 / 0x1004b977 / 0x1004b9f3). The port does the same, with the original's messages. The codes and what they
  do are listed under "Cheat codes" in the README.
  - `blorb`, `cia` and `coldmiser` toggle the Combat Variables switches: invulnerability, unlimited ammo and heat
    tracking. They change the mission only. MW2DIF.CFG is not written.
  - `mightymouse`: jetting burns no fuel (0x10026f30).
  - `zmak`: the frame's time is divided by 4.
  - `meepmeep`: unlocks CRACK_MODE (Ctrl+Alt+F, action 0x92), which multiplies the frame's time by 8.
  - `gankem`: destroys the target.
  - `enolagay`: destroys the target with a blast. The port uses explosion 7 for the blast; the original's own effect
    (0x100620f2) is INFERRED.
  - `icanthackit`: every star succeeds.
  - `hangaround`: a decided mission does not end (0x1000ffa9), and the debug keys work.
  - `idkfa`: shows its message, then ejects (action 0x3b).
  - `xray`: the image enhancement view.
  - `front`: the rear view looks forward.
  - `tinkerbell`: free-eye mode. The port's free camera stands in for it.
  - `flygirl`, `dei`, `dorcs`, `lairdo` and `michelin` only show their messages. In MW2.DLL, `flygirl`'s flag is never
    read, `dorcs` starts the VFX1 headset set-up, `michelin` draws the debug collision spheres, and `lairdo` changes a
    unit search for the blimp.
  - Two rude words get "Freebirth vulgarity will not be tolerated!".

## v1.1.16 (2026-11-12)

- **Menu text over the in-game videos.** A screen's text queued before a video started was drawn over it, for example
  "WOLF CLAN HALL" from the main menu's hovered hotspot over the landing cinematic. Shell text is a layer above the
  picture, and it was not cleared when the video took over. The text layer is now emptied as a video starts. This
  covers the landing, ending, registration and briefing clips and the intro.

## v1.1.15 (2026-11-11)

- **A classic Mac OS build - ALPHA.** A PowerPC Carbon application for Mac OS 9 with CarbonLib and Mac OS X
  10.1-10.4, built with Retro68 (`platform/carbon/`, docs/CARBON.md). It is the same game code with a small SDL3 subset
  over the Mac Toolbox (windows, AGL, the Sound Manager, the File Manager) and the OpenGL 1.2 renderer. The floor is an
  ATI Rage 128: 800 x 600 with 8 MB of video memory, 1024 x 768 with 16 MB. It has 16-bit textures, no SC-55 or MT-32
  emulation, and QuickTime's synthesizer as the fallback menu music. **It has had almost no testing on real
  hardware.** It builds with Retro68 (checked), and its shared code runs on Linux with an EGL stand-in for the Mac layer; treat it as alpha.
  The shared files change only inside `#ifdef MW2_CLASSIC_MAC`, plus `gus_song_open_sink` in `src/gusmid.c`, which the
  other builds don't call. The other builds are unchanged.
- GitHub Actions builds it on every tagged release (`build-classic-mac`, in Retro68's published toolchain image) as
  `MW2-Port-<tag>-classic-mac-ppc-ALPHA.zip`. While the build is new, that job is allowed to fail without holding up
  the other downloads.

## v1.1.14 (2026-11-11)

- **The nav marker and target brackets slid around in the external view.** The 3D looks projected the marker with the
  torso's yaw. The external camera (mode 1) turns after the mech through its servos and looks down at it, so while
  you turned or aimed, the marker left the nav point and moved with the aim. DOSBox keeps it on the point. The
  marker, brackets and off-screen arrows are now projected through the view as drawn: its eye, yaw and pitch. In the
  cockpit nothing changes, except that glances now keep them on the point too. The DOS look already used its own
  projection.

## v1.1.13 (2026-11-09)

- **The player's mech wore the Clan Wolf crest on Jade Falcon missions.** Every unit's insignia (and camouflage) slot
  is offset by its star's STAR value (entry +0 of its table; engine 0x10042000): the 3D editions' slot 0x114 is the
  Wolf crest and 0x115 the Jade Falcon's. The port gave the lancemates and enemies their star's value, but always 0 to
  the player. On the Jade Falcon missions the player's star has value 1 (PINK, GREE, ...). The player's mech now takes
  its own star's value (msim `player_tex`), as every other unit does. This also moves the player's camouflage to its
  star's set, as for the other units.
- **The DOS look drew every unit's crest from slot 0x14, the Wolf.** The DOS decals (colour 0x7814) now add the star
  value too; the DOS texture map holds the crests in the same order. INFERRED: MW2.EXE itself was not available to
  trace. The Mercenaries DOS sim, the same engine, adds the star value (its 0x62bf6 / 0x62e74).

## v1.1.12 (2026-11-09)

- KNOWN_ISSUES.md: the S3 VIRGE items are dropped. The texture-cache eviction and the linear alpha blend can't be
  checked without the real card, so they won't be done. ASSUMPTIONS.md records them as not to be done. No code
  changes.

## v1.1.11 (2026-11-06)

- **The global menu (Esc / right click) showed the screen's text through its panel.** This was seen over the
  briefing. The port draws text as a sharp layer above the 640x480 picture. The original draws its panel into the
  frame over the text, so the text stays visible beside the panel and is hidden under it. Before, the port hid only text
  lying wholly inside the panel. A line crossing it was drawn in full, over the menu. Each area drawn over earlier text
  (the menu's panel, a dialog's box) is now a hole in the text layer: earlier text is clipped to outside it. This
  applies to both renderers and the legacy build. Test hook: `MW2_SHELL_MENU_LIVE=2` with `MW2_SHELL_GRAB`.
- **Black flames with the Titanium textures.** The Titanium archive's effect cards (FIR*, SMO*, JET*, DEATH*) are
  colour kind 7 with light nibble 0xf (0x7fxx). They are the only Titanium models with that nibble. The port lit them
  like any textured polygon, so a fire card facing the eye took only the ambient and burned dark brown. They are now
  unlit, drawn with their texel as it is, as the 3D editions draw the same cards (0x8fxx, light 0xf unlit). INFERRED:
  the Titanium engine's own light-nibble rule is not traced.

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
