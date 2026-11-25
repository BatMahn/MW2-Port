# DOS MW2.EXE comparison

The port's DOS look is checked against the original DOS MW2.EXE (1.1, md5 51634b60...) in two ways:

- **Emulated sim tests.** The original routines run under the unicorn x86 emulator, and the port's C runs on the same
  inputs. Each test prints PASS or FAIL with the first differences. This is the method the Ghost Bear's Legacy port
  used.
- **Render comparison.** The same mission runs in DOSBox (the BattlePack CD's DOS install) and in the port's DOS look,
  and the frames are put side by side. DOSBox's screenshot key saves the frame as an indexed PNG with the live VGA
  palette, so colours can be compared exactly. The method is in docs/DOSBOX_COMPARISON_METHOD.md.

This file says what was checked, what turned up, and what was not checked.

## Running the sim tests

The executable is game data and is not in the repo. Make its ELF image once, then build the shims and run:

```
python3 tools/audit/le2elf.py MW2.EXE mw2dos.elf
tools/audit/build.sh
MW2_ELF=mw2dos.elf python3 tools/audit/simdiff.py [-n cases] [-s seed] [test ...]
```

The suite runs in Linux with Python and Unicorn. Use a Linux container or VM if macOS terminates Unicorn's JIT.

- `mw2emu.py` loads the image and runs functions with Watcom's register calling convention. It runs the start-up
  maths tables (0x3aa90) first and restores the data segment between cases. A test can stub any callee.
- `shim_*.c` include the port's `combat.c`, `ai.c` and `msim.c` so their static functions can be called. The
  DOS-edition flags are switched on.
- Where a routine draws random numbers, MW2.EXE's `rand` (0x4a070) is stubbed with the port's own generator, so both
  sides see the same numbers. The chooser and missile-warning tests also check that both sides make the same number
  of draws. The random-stream test calls the original initialization and `rand` directly.

## Sim tests and results

For v1.1.49, all 17 tests passed at 100 cases each across seeds 47 and 149. The jump-jet test
found 35 mismatches in v1.1.48 and none in v1.1.49. The geometry test records crossing points within
2 cm of an edge, plane or segment end separately from its pass count.

| Test | MW2.EXE | Port | Result |
|---|---|---|---|
| heat_state | 0x24f90 (sinking, 66 warning, 81 sequence, shutdown, BlowAmmo, restart) | combat_tick | PASS |
| heat_sink_tiers | 0x4cd7b..0x4ce25 (dissipation by player, difficulty, temperature) | combat_set_player_difficulty | **fixed** (v1.1.39) |
| heat_override_roll | 0x250f0 (rand(500 x ticks)) | combat_tick | PASS (same 15-bit samples and frame tick counts) |
| weapon_parameters | 0x9ee58 (31 weapon records); 0x2ea14 (AI firing-roll divisor) | weapon data and combat_ai_select | **fixed** (v1.1.50) |
| jump_jet_fuel | 0x27ba7..0x27c87 (fuel, heat and effect dispatch) | combat_jets, combat_jets_firing, combat_jets_dy | **fixed** (v1.1.49) |
| ai_nav_turn | 0x229e0 | ai_nav_turn | PASS |
| leg_turn_demand | 0x27c87..0x27d70 | msim_leg_demand | **fixed** (v1.1.39) |
| servo_step | 0x49b80 | drive_servo | PASS |
| dos_drive_servo | 0x49b80 | DOS drive speed servo, including signed sub-threshold values | PASS |
| dos_drive_input_order | 0x27ecf, 0x49b80 | Previous throttle feeds the drive target before movement steps both servos | PASS |
| servo_setup | 0x49b40 (T = chop(s x 182)) | servo T constants | PASS |
| turret_twist | 0x2edc3..0x2ee2a + 0x49b80 | msim_twist_round | PASS |
| collision_damage | 0x20890 / 0x20a50 -> 0x20930 | collide | PASS |
| manoeuvre_start | 0x2cbf0 (limits, reverse, 11's jet, the +0x17e flag) | start_manoeuvre | **fixed** (v1.1.41) |
| manoeuvre_choose | 0x2c880 (every class, the Elemental table) | choose_manoeuvre | **fixed** (v1.1.41) |
| missile_warning | 0x2e450 (from the shooter test on; see below) | ai_missile_warning | **fixed** (v1.1.41) |
| ai_weapon_pick | 0x2e980..0x2ea63 | combat_ai_select, stock AI weapon groups | PASS |
| torso_inputs | 0x2e930..0x2e971 -> 0x22970 / 0x229a0 / 0x23a40 | dos_twist_q / dos_pitch_q | **fixed** (v1.1.41) |
| patrol_sweep | 0x24710 (runs of 300 frames) | patrol_sweep_dos | PASS (new in v1.1.41) |
| flank_pick | 0x2e6f0 | flank_pick | PASS (new in v1.1.41) |
| seg_poly | 0x1ca00 / 0x1cd60 / 0x1cbb0 | cpoly_in | PASS (new in v1.1.41) |

## Differences found and fixed

### v1.1.51

- The DOS projectile update checks the ground at negative height (0x50b4e..0x50b69). A shot above
  that surface proceeds to the lifetime check and position update (0x50bc6..0x50c1e). The port
  treated any shot below elevation zero as removed. In Golden Spade this discarded live shots
  before impact with the shield generator, whose collider is below elevation zero.
- Original GOLDARE8 data places the TL1SHIEL collision shape (OBJ 3, type 6) under its
  destructible generator (OBJ 2). The strict Golden Spade sweep destroys the generator and then
  the palace with projectiles. Use `sweep_objectives MW2.PRJ -dos -no-fallback GOLDSCN1` to reproduce it.
- The sweep now aims from the permitted firing position after boundary clamping. It probes the world
  collision list to choose firing angles and waits for a clear approach while guarding protected targets.

### v1.1.50

- Compare each weapon's cooldown (+0x44), launch interval (+0x48), lifetime (+0x4c), damage (+0x30),
  heat (+0x34) and ammunition capacity (+0x20) to the DOS table at 0x9ee58. Original consumers include
  0x523a0 / 0x52449 / 0x524a5 (weapon state and heat), 0x52777 / 0x5278b (projectile), and 0x4cbb9 (ammo bins).
  The native comparison found differences for 30 of 31 weapons in v1.1.49. All 31 matched after the fix.
- The DOS circling-object initializer at 0x1b66e converts speed and turn rate with x87 FRNDINT and FISTP.
  Masked overflow stores 0x80000000. The DOS archive's BRO2SCN1 produced a value above INT32_MAX and the
  host cast raised an undefined-behavior diagnostic. The port now handles that value explicitly.
- Use `sweep_objectives MW2.PRJ -dos` to test the campaign with DOS simulation rules. This selects the
  same initialization as the DOS renderer, including the world placement rules needed by the Velvet Hammer escort.

### v1.1.39

- **Hard difficulty in cold weather:** the player's dissipation below -30 degrees is 67 per heat sink point per tick
  in MW2.EXE (0x4cdf7). The port used the 3D-card engine's 67.5.
- **Leg turning when reversing fast:** MW2.EXE clamps the speed term only at the top (+80). The port also clamped
  reverse speeds at -80.

### v1.1.41, sim

- **Jump after a jump.** When an enemy runs out of weapons, the out-of-weapons step forces a jump (4) and sets a flag
  that should make a second jump (5) follow it. In MW2.EXE every manoeuvre start clears that flag (0x2cc00), so the
  5 never comes. The port kept the flag and made the second jump.
- **Missile dodge direction.** The missile warning stores which way to dodge (0x2e6a6). The manoeuvre start copies
  the cleared flag over it (0x2cc11) before the dodge (11) reads it, so in MW2.EXE the dodge always fires the left
  jet. The port dodged forward for some quadrants.
- **A jump with the target off to one side.** The chooser drops a jump (4) when the target is beyond the torso's twist
  limit. MW2.EXE compares the signed bearing (0x230d0), so only a target off to the plus side cancels it. The port
  used the absolute angle.
- **One-row classes still draw a random number.** Aircraft (always 12), dropships (1) and other vehicles (0) have a
  one-row record. MW2.EXE still calls rand(1) for it, and the port now does too, so the unit's later draws line up.
- **The chooser's ground test for 5.** MW2.EXE drops a 5 only on the ground (+0x78). The port also dropped it without
  jets or fuel.
- **The missile warning outside the attack state.** MW2.EXE sets the forced manoeuvre whatever the target's state; it
  waits for the next chooser call. The port ignored the warning unless the target was attacking.
- **Torso aim inputs.** The fire decision writes the torso inputs in steps: the bearing in 1024ths of 180 degrees
  (chopped), the elevation in 1024ths of 60. The port used the exact angles. This applies to turrets and dropships
  as much as mechs.
- **Missile lock.**
  - The timed lock (0x52ea0, for the player and the computer's units) takes 0x16c ticks (364) in MW2.EXE. The port
    used the 3D-card DLL's 362.
  - When the fire decision picks a guided weapon, MW2.EXE also gives the unit an instant lock on a roll:
    rand(range byte + 1) == 0, for range bytes up to 4 (0x2ea99..0x2eab0). The volley fired that frame homes. The
    port had only the timed lock.
- The earlier AI work in this release (manoeuvres 1, 6, 7, 9 and 12, the flank slots, the patrol sweep, the jump
  checks and the rule timings) was ported by reading the disassembly, the GBL port's audit and its `E_sim_unmatched.tsv`
  mapping. The patrol sweep, the flank slot pick and the point-in-polygon rule have emulated tests. The manoeuvre
  steps themselves do not (see below).

### v1.1.41, render

- **Sky-object tree drawn at any distance.** A scene's first shape of kind 0x90 is the sky object. MW2.EXE detaches
  its object tree from the scene and draws it with the far plane at 0x7fffffff and only the frustum test (the
  engine's SecondRender and the frame's draw; demech2's render.c). In Marooned that tree is the city dome, its eight
  struts and its windows. The port culled the struts at the planet's view distance (40,000), so a strut that DOS
  draws across the sky was missing. Only the first such shape counts: GOLD has two, and its second (a walkway) still
  culls.

### v1.1.42

- **Small objects hung on a moving one** (Blade Splint's helicopter running lights: 1 m cubes in the hull). The
  engines draw whole objects in depth order, so the light nearer the eye than the helicopter is drawn over it. The
  port now moves that light toward the eye and scales it by the same factor (the same pixels, in front of the hull).
- **A moving vehicle's small attached pieces** (Blade Splint's car headlight housings: boxes reaching 1 m out of the
  nose from inside it, listed after the body). The engines draw an object's polygons in list order, so they show
  whole. Pieces that share no vertex with the body and are under 2.5 m across, on the eye's side of the body's centre,
  have their vertices moved along their own eye rays by up to 1 m.
- **Turret and dropship pitch** (Sable Flame's Falcon dropships). MW2.EXE takes the elevation between the two
  objects' positions (0x2a210: +0x50..+0x58). The port aims from the eye (v1.1.40), now at the target's centre rather
  than its feet, over the distance from the eye. Not compared frame by frame with DOSBox: the reported shots into the
  ground did not reproduce on flat ground, where both aims hit.
- **Shell briefing** (MW2SHELL.EXE FUN_0001c080, now read from an ELF made of MW2SHELL.EXE): the situation page and
  the text layout, compared with DOSBox. See docs/SHELL.md.

### v1.1.43

- **Both small hung objects in front.** v1.1.42 drew only the light nearer the eye than the helicopter over it. The
  engines' polygon sort key is the farthest corner (0x1002a080), so the hull's big faces go down before both small
  cubes: both are now pulled in front.
- **Moving vehicles' ground-level vertices 8 cm up**, above the roads (S_BSTRTE), which the ground-plane rule lifts 4 cm.
- **MGEO int[0] = 0** (doors, TURRET*): the centre at the base, not the 550 cm default. Used by the target viewer's
  camera (DOS frames the object's position), the contact sphere and the turret / dropship aim at a target.
- **Blade Splint's helicopter searchlight:** see v1.1.44.

### v1.1.44

- **Circling tasks (type 2) in integers, as DOS (parse 0x1b5e0, run 0x1b6ef).** Load: yaw rate and speed in 16.16,
  trunc(360 / s x 65536 + 0.5) and trunc(2 pi r / s x 65536 + 0.5). Each run: dt = (now - last) << 16 / 182; the step
  d = speed x dt, then whole centimetres by sar 16 (floored); MoveObj (0x1dfc0) adds (cos a x d, 0, -sin a x d) >> 29
  (each floored) to the node's local translation; the turn (0x1e140) by yaw x dt; a += turn, remainder of 360.
  Blade Splint's searchlight discs (LIGHTA / B / C, TEALARE3 nodes 10-12) run "1,-30,1": a 1 cm circle backwards, so
  every run steps -1 cm on each axis whose term is negative and the discs creep off in the helicopter's frame without
  end. Measured with dosbox-debug MEMDUMPBIN (the node array at 0x393f7c, the task at 0x411fbc: angle, yaw, speed,
  on, last tick): at tick 5491 the helicopter at -6102, 8200, -15045 and the disc's local offset -771, -8500, -745.
  DOS runs tasks once a frame, so the drift goes with the frame rate; the port runs these steps at a fixed 52 a
  second, which matches the dumps (port at the same tick: helicopter -6096, -15297, disc 814, 752 from it; DOS 796,
  718). The 3D looks keep the float step.

## Differences kept

- **Invulnerable player and BlowAmmo:** MW2.EXE's shutdown-above-100 branch (0x25130) calls BlowAmmo even for an
  invulnerable player. The destruction it leads to (0x25340) then does nothing for that player. The port skips the
  branch for an invulnerable player, so a critical hit on an ammunition bin can't happen in that cheat mode either.
- **Sky-object draw order.** MW2.EXE draws the sky-object tree before the scene, so the scene always covers it. The
  port draws it with the scene in the depth buffer. A building beyond a dome strut would cover the strut in DOS and
  not in the port. No such view was found in the comparisons.
## Render comparison

Sixteen missions were run to about 9 seconds in both, in the cockpit view at 640 x 480: AQUA, BLON, BROW, CYAN, GOLD,
GREE, IREN, JACK, JENN, KIM_, MARO, PINK, PLUM, SABL, TAUP and YELL.

- **Match:** sky and haze bands, ground colours, terrain and buildings, vehicles, the HUD and its palette colours,
  radar, weapon list, heat bars and damage display in all of them except as listed below.
- **Fixed:** MARO's dome strut (above).
- **Timing, not a defect:**
  - TAUP: a convoy truck crosses in front of the player at 9 s in DOSBox and at about 11 s in the port. DOSBox's
    wall-clock time includes its load time.
  - Friendly radar blips and the odd enemy blip sit in different places, because units have moved for different
    lengths of time.
- **Not compared:**
  - JACK: DOS can't start it in this install (it needs en01star.bwd).
  - The first capture batch left out the F-key views. The follow-up below uses `usescancodes=false` on the Xvfb display.
  - External and free-eye views: they have no key in the BattlePack's GAMEKEY.MAP.
- **Open:** In a matched PINKSCN1 cockpit capture at rest, DOS draws "0 kph" in blue and the port draws it in green.
  A private DOSBox trace recorded three zero velocity components and a drive servo of -1 at 0x32106.
  The terrain query at 0x1d7d2 returned height zero without finding a polygon and reused the previous collision
  normal, which an axis-aligned box query set at 0x1edbd. That fed the slope correction at 0x2750f on startup.
  The port preserves the servo's signed residual. Reproducing the prior query's normal on the terrain fallback remains open.

v1.1.42 added TEAL (Blade Splint): the start view and a drive to the first crossroads match DOSBox, its fire cards
included (FIR1_2 by the road at the start, FIR1_1 at the crossroads). A reported stray flame at a later crossroads
was not identified.

Earlier render checks against DOSBox (v1.1.40): Wolf 3's roof lights and car headlights, the Sable Flame dome's
outline colour and its view distance, the radar arms and power-up colours, Abort Mission's eject camera.

### Cockpit display follow-up

The reference is DOS MW2.EXE 1.1 (`51634b60...`), YELLSCN1, a stock Mad Dog at rest and native 1024 × 768 captures.

- F7 uses the eye position and the legs' rear bearing, with zero pitch (0x30c30–0x30c70). The rear horizon now stays
  level when the cockpit tilts. F8 points straight down from the unit's centre (0x30ad7–0x30b57).
- The DOS insets use zoom 2 over their own viewport width (0x28810), and their haze band uses the full frame width.
  F9 uses the missile's whole-centimetre model position (0x50c1e and 0x50c52). Live missile flight and return to the
  idle display were checked. The missile's own visible fin edges still differ from DOSBox.
- The selected Falcon Nova's target view now uses the DOS angle table, integer projection and solid palette pixels.
  In the captured wireframe crop, the overlap divided by the union of the original and port blue pixels rose from
  34% to 92%. This is a matched pose comparison, and some hidden-edge coverage still differs.

### Drive, audio and missile follow-up

- The DOS drive servo steps in 16.16 cm/tick and keeps the signed residual when integer division yields zero.
  The movement demand is zero strictly between -32 and 32 as at 0x26ae0. The cockpit readout also includes the
  velocity across the heading after a collision.
- DOS positional audio uses the original integer 3D range calculation (0x2ab60), whole-metre gain and rounded tick
  delay (0x40bc0), and signed sine pan (0x40d10). A differential check matched all 3,954 vectors, including the range
  cutoff, cockpit half volume and vertical-only vectors.
- DOS player weapon dispatch (0x27836) uses the prior frame's missile lock before the update (0x27a55).
  The sequence test fires on the lock acquisition frame and on the next frame. The first volley flies unguided
  and the next homes.
- The player can trigger the selected unit's dodge with a guided launch or PPC shot. DOS checks whether the
  convergence point and the unit's position project onto the main view, and whether the whole-degree bearing
  off the legs is strictly between -15 and 15. A cockpit glance away suppresses the warning. The emulator
  checks the angle boundaries and the order of random draws. Type 2 computer-controlled shooters skip the
  projection, as seen in the player and AI records from a live PINKSCN1 dump.
- The player outline's front and rear damage levels (0x2f611..0x2f6e4) and message replacement (0x11020)
  each matched 3,000 differential cases. The message comparison used the active or expired status after
  normal message upkeep and the priorities used by the game.
- At v1.1.52 all 20 differential test groups passed across seeds 47 and 149, covering 137,705 assertions.
  One segment case within 2 cm of a boundary remains outside the assertions. The heat override command's
  sampled mean at six ticks per frame was 92 seconds in the port and 99 in DOS.
- ORANSCN1's pump station was inspected at 205 m in the DOS look and rejected in the 3Dfx look. The sequence
  selects the same station in both looks before pressing `i`, so it exercises inspection independently of
  target cycling. All 59 DOS and 54 3Dfx mission objective sweeps succeeded without a fallback.
- The headless OpenGL sequence sweep passed 273 of 279 sequences across the shards and fixture reruns.
  The remaining failures cover reticle convergence at 300 m, the final autopilot heading from the right in
  TNJ4, the zero-time completion log for PROVSCN1 without actors, and three shell screenshot comparisons.

### Heat override follow-up

- DOS weapon heat above 100 with the shutdown overridden rolls a 15-bit value modulo `500 * frame_ticks`
  (0x250f0, 0x4a070). The port uses whole elapsed ticks from the simulator clock. With no elapsed tick or
  player invulnerability, the roll is skipped.
- The differential test supplies the same raw sample to the original table entry and the port's per-unit
  generator. The port still uses its own random sequence. DOS initializes a 127-entry table from the Watcom
  generator at 0x49fe0 and shares the index with other simulation code.

### Cockpit, Satellite View and Aquiline Fire reports

- Ctrl+Z releases the zoom ramp and suppresses zoom input from the reset key until it is released. In a held
  reset test the previous build climbed back to 4×. The reset now holds at 1× and a fresh Z press zooms normally.
- The Mad Dog cockpit's four single-face leg proxies (WTB flags 2, polygon colour 0x7800) are omitted from
  the cockpit draw. The original DOSBox look-down capture showed the struts without the leg panels.
- The DOS satellite background uses palette entry 0xef from the current cockpit palette. Aquiline Fire,
  Pyre Light and Flame Tongue were checked. The 3D map uses the currently installed renderer's ground texture
  average. Destroyed structures and their children draw solid black on the map. The Silver Staff Chemical Plant
  draws in blue throughout the left target display.
- DOS palette fades refresh only HUD sprites that are actually drawn, and unchanged palette data skips the world
  texture-table upload. The palette and vision sequences passed. The performance comparison used software OpenGL.
- Aquiline Fire's wreck in AQUAARE2 uses a colour task on the S_1DUMMY model for the wing light.
  The VCDDROPX debris record has five WTB objects marked 0x2000. Its destruction now launches five independently
  moving fragments and four small chunks. The mission's wreck is named Hermione in the GT record.
- Titanium texture lighting takes the damage nibble of the rendered location. The chase-view test confirmed that
  the damaged arm darkens while the opposite arm retains its colour.

### Crossroads flame, wreck decals and LOD follow-up

- Blade Splint's second crossroads is at the map origin. TEALSHT1 registers eight explosion models there with
  XPLO chunks. The loader now hides such inactive effects. The same view in DOSBox had no flame in the
  crossroads. The four mission fire cards from TEALARE1 still draw at their authored positions.
- The intact Hermione's DOS model uses two pairs of Wolf insignia triangles (0x5814, texture slot 0x14).
  The DOSBox capture shows the emblem, but the port's depth test clipped it where the triangles sat behind
  the hull. The renderer offsets these triangles by 325 cm along their outward normals on V_BDROPX only.
  Both sides were checked in the port. The asset data and collision mesh are unchanged.
- At native 1024 × 768 and 1× zoom, MW2.EXE's LOD scale was 209715 in 16.16 units. Blade Splint's Jenner used
  representation 2 at 449.96 m, and the port selected the same model at 450 m. It has 94 polygons across 15 parts,
  compared with 264 polygons across 15 parts at full detail. The selector at 0x37580 uses 3600, 8500 and 22500 cm times
  the scale, rounded, and ranks nearby units before assigning representations. The port now rounds those
  thresholds and measures from the camera to each unit's centre as DOS does. The page 7 pose, mech and
  settings remain to be matched.

### Projectile flight

- MW2.EXE 0x506f0 and 0x50750 advance each projectile once per frame using the whole tick delta. The
  position step rounds down to whole centimetres, and the impact check at 0x5091c precedes expiry at
  0x50bc6. The port follows this order in DOS mode and retains the projectile's 16.16 velocity between
  frames. The differential test covers flight and expiry endpoints over varying tick deltas.
- Launch direction (0x527dc), gravity (0x4e62e and 0x52806), missile launch offsets (0x53410), and
  missile steering and proximity (0x50d50) have differential coverage. Launch offsets consume one
  random roll for missiles. Other projectiles consume none in this routine. In the DOS TNJ6 sequence,
  all ten locked LRMs hit the stationary instructor at about 5.4 m above its feet.
- DOS reserves projectile slots in BTHG record order (0x4f326). The retail MW2_SHT1 record contains 159
  slots, with a limit of 175 in the loader. The allocator scans for the first free slot of the weapon's
  projectile type (0x52710). The active list is also processed in slot order, including after a slot is
  reused. The career shot count advances even when the pool cannot supply a slot. The flamer's zero
  launch speed exits at 0x526fc before allocating a projectile.
- DOS 0x1c3f0 searches the collision list in order and replaces a hit only with a strictly nearer one.
  World tests use the integer sphere (0x3a745), box clip (0x1ec60), and polygon plane crossing (0x1c7a0).
  The segment uses the octagonal length up through 3,000 cm and the Euclidean length above it. The height
  fallback at 0x50b4e applies to negative endpoints. Ground above zero is covered by the terrain face test.
  Differential checks cover both segment length modes, spheres, boxes and oblique face crossings.
  The bullet impact on a unit uses X and Z from the 100 cm offset calculated at 0x5093c. The impact
  handler at 0x510c1 retains the contact height. Other tested impacts use the contact position.
- Mech pieces use the original integer bounding sphere (0x37f60) and round the transformed centre
  after the 3.29 matrix products (0x3aefc). The port scans current mech representations and scenery
  together. Differential cases exercise tied contacts between two parts and a scenery object, and verify
  the shooter is excluded. Blown-off parts leave the scan. Solid wrecks still block it. Aquiline Fire passes
  the DOSBox damage window with the combined scan in use.
- The reticle and gun convergence pass a fixed 150,000 cm ray from the pilot's eye (0x1d8a0). AI line-of-fire
  and jump sightlines instead use the original octagonal segment length (0x2bf20 and 0x2e150).
  Player and AI avoidance also use that collision scan. Their step endpoints, lateral offsets, and
  velocity-scaled probe range follow 0x2db80 and 0x2df30. The TNJ4 wall-follow sequence covers the resulting
  slowdown, turn, and return to the nav route.
- DOS 0x37580 uses the horizontal and slant lengths from 0x2ab60. The original trigonometry tables depend on
  the x87 control word set by Watcom startup. The emulator now sets that word before building the tables.
  The comparison covers representations around their boundaries, wrecks, the cockpit and external cameras,
  and the order of simultaneous representation changes. Original 0x15da7 calls 0x35870 after projectile upkeep.
  The port now ranks collision hulls from the active draw camera at that point. The ordnance-camera sequence
  follows a missile far enough to switch the player's exterior representation, then returns to the cockpit.
- Representation swaps now retain the posed torso pitch and animation sample. The DOS player also passes the
  current pilot pose to the cockpit and exterior collision meshes.

### AI manoeuvre timing and model damage

- The attack dispatcher at 0x2c210 starts a manoeuvre on one update and runs its movement on following
  updates. The differential test exercises start, movement input, and the ending frame for manoeuvres
  0, 4, 8, 10, and 11 with fixed target geometry. A separate comparison runs manoeuvre 9 with supplied
  sightline results and checks the jet phases, descent brake, steering input, and attack deadline.
  The approach and glide calculations divide by whole elapsed ticks and hold the previous distance on
  updates with zero ticks. The approach end threshold uses the fixed-point scale at 0x2d42f. Comparisons
  exercise approach, weave, and charge inputs with and without an obstacle. The weave still changes
  sign on schedule while deflected. A charge ignores a deflection when its own target is the obstacle,
  and its self-destruct check still runs at 0x2d4e4.
- Additional comparisons exercise the glide at 0x2d7b0, flank steering at 0x2d290 and circling at 0x2d990.
  They cover fuel and heat boundaries, glide thrust direction, the waypoint-to-target steering transition,
  and waypoint advancement after the current steering input. The DOS obstacle avoidance cache now uses an
  absolute tick deadline, so two queries in a single update see the same deadline.
- DOS type 2 missile and PPC warnings use the unit tracked by the shooter's AI state (0x2e4c5)
  separately from the projectile's homing lock. The sweep pilot moves clear of a training instructor's
  firing lane if a protected target is nearby.
- The model shade uses the nibble written by 0x26567 and 0x1e7e0. The MEK loader computes separate
  front and rear scales at 0x4ca25. Hits use the side that took the damage and can only raise the
  recorded shade for that location. The cockpit readout evaluates current values for both sides.
  The differential runs the original MEK scale and hit routines.
- All 41 executable comparison groups passed across seeds 47 and 149 at 100 cases per test. The objective
  sweeps completed 59 DOS and 54 Enhanced scenarios without fallbacks. The DOS sweep also passed with
  AddressSanitizer and UndefinedBehaviorSanitizer. The OpenGL suite reproduced the same 23 failures as
  v1.1.59 after the new ordnance fixture was corrected to select the LRM explicitly. The corrected sequence
  passed with both OpenGL and Vulkan. Aquiline Fire uses a DOSBox timing window because its firing
  sequence varies with the random seed and frame timing.

### Skeletal transforms and flank coordinates

- DOS node update 0x1e400 composes parent and child transforms through 0x3b3c5. The port uses the
  original mode-0 rotation construction (0x3b522), three-product matrix rounding (0x3b20c), and
  whole-centimetre point transform (0x3aefc). The collision sphere reads the resulting matrix before
  conversion to floating point for drawing.
- The skeletal comparison runs the original node update on a three-piece chain. The samples include
  fractional animation poses, mixed yaw and pitch at the torso, gun elevation, and a downstream
  collision point. It supplies the sampled keys to DOS, so the result covers composition from the
  sample through the hierarchy.
- Obstacle probe endpoints (0x2df30) and all 16 flank waypoint slots (0x2d070) match the original
  with the corresponding input yaw matrix. The waypoint test varies the target's origin, heading,
  and offset distance.
- All 43 differential groups passed on seeds 47 and 149. The 59 DOS and 54 Enhanced mission sweeps
  passed without fallbacks, and the DOS sweep also passed with address and undefined-behavior sanitizers.
  OpenGL passed 277 of 300 sequences with the same 23 baseline failures. Three Vulkan smoke tests passed.

### Animation track timing and live matrices

- The DOS track player (0x1aad0) applies a signed integer share of the remaining key delta each frame.
  A transition adds the next key’s delta to any remainder. A held frame drops its elapsed ticks.
- Rotating tracks multiply the previous local matrix by that frame’s rotation (0x1e140 and 0x1e0b0).
  The countdown seeded at node creation (0x1de60) controls normalization of the rows and columns
  (0x3ad00). Collision and rendering use the same accumulated transform through LOD changes.
- The frame comparison executes the original track player, node initialization, normalization and world
  transform. It uses the same initial phase and a moving root on both sides. Setting
  `MW2_ANIM_RECORD_DIR` to a directory of raw ANIM payloads extracted with `prjtool extract MW2.PRJ ANIM <name> <output>`
  also replays each moving track in the 23 DOS gait records, including walk, run, reverse and holds.
  All 63,120 recorded-data frame comparisons matched.
- Instrumented DOSBox runs of YELLSCN1 captured the Mad Dog's animated node updates at 640x480
  and 1024x768. Given the captured starting matrices, countdowns and angular deltas, the port
  matched all 4,626 local and world updates across 514 animation frames, including 31
  normalizations. Replaying the game's tick spacing and gait requests also matched the master
  track's key, sequence, pending delta and timers on all 486 live frames in the 640x480 run.
  A HEIDSCN1 Nova replay also held animation key 15 with 33 ticks left during 20 airborne frames
  and resumed from that key on landing.

### Aircraft routes and leg turns

- Circling aircraft refresh the route after 1,456 ticks if the target is beyond 150 m (0x2d990).
  DOS compacts the existing navigation table (0x233b0) and uses the resulting current waypoint's
  position and 30 m radius for the new square (0x23420). The replay executes the original table
  insertion, removal and selection across route changes. Flank arrival uses the same navigation
  distance as steering (0x2d550).
- The leg servo uses the original integer update (0x49b80, T = 54 ticks) followed by the
  16.16 heading increment (0x27561). The replay compares both after rounds of changing turn demand
  and frame spacing.
- All 46 differential groups passed on seeds 47 and 149. The 59 DOS and 54 Enhanced mission sweeps
  completed without fallbacks, with the DOS sweep also passing under AddressSanitizer and
  UndefinedBehaviorSanitizer. OpenGL passed 277 of 300 sequences with the same 23 baseline failures.
  Three Vulkan smoke tests passed.

### Mission allocation order and movement

- DOS processes BWD includes in place. Its scene node allocation can precede the player in training
  and fall between unit spawns on Golden Spade. Replaying allocation and animation binding from
  DOSBox matched all 240 distinct animated node countdowns across 23 units in HEIDSCN1, YELLSCN1,
  TNJ1SCN1 and GOLDSCN1. The next shared phase after scene loading also matched in all four.
- Movement integrates signed 16.16 world velocity in fixed game ticks and rounds each displacement
  down to whole centimetres before updating heading (0x26ada–0x275b0). Using the captured controller
  state, pre-contact velocity and displacement matched in 3,099 walking and coasting frames from
  HEIDSCN1 and YELLSCN1, with 245 heading changes committed after integration. The Nova's jump jet
  replay matched 49 lift updates and 79 updates with a directional jet or turn. A turn suppressed
  thrust until the command cleared, even as the leg servo continued turning. Fuel burn, refill and
  jet heat use the frame's actual tick count (0x27be8–0x27c7d).
- Differential tests run the original machine code for horizontal and vertical integration and for
  terrain and unit responses. Live DOS inputs reproduced 297 terrain face rebound velocities and
  218 residual displacements (0x1fee6 and 0x20013) and 36 unit projections and rebounds from training
  (0x2043a). These comparisons feed in the original collision inputs. The port's full contact path
  also tests the projected position a second time and handles contacts reported by earlier movers
  (0x20141 and 0x27109).
- All 50 differential groups passed on seeds 47 and 149. The mission sweeps completed all 59 DOS
  and 54 Enhanced scenes under AddressSanitizer and UndefinedBehaviorSanitizer with zero fallbacks.
  Four Vulkan smoke sequences passed. OpenGL passed 273 of 300 sequences. The 27 failures include
  the previous 23 and four training contact checks: the TNJ1 slide at 30 fps, the TNJ1 frame-rate
  comparison, the TNJ6 slide, and the contact retest comparison between editions.

### Runtime scenery normalization and training state

- The spin task (0x1b55b) takes the elapsed whole ticks since its last run, divides by the task period
  in 16.16, and multiplies each encoded angle by that fraction. It calls the same local matrix update
  (0x1e140) as the rotating joints. In HEIDSCN1, the CINDWLD1 spinner starts with a countdown of 138
  and consumes the first shared normalization period before any unit does. In a replay using the
  original update ticks and observed phase at each reset, all 687 local matrices and countdowns
  matched, including six normalizations. At tick 2,766 the original updated a player joint before
  the scenery task, as the port does.
- In the TNJ1SCN1 DOSBox trace, the instructor receives the player's target reference during startup
  (0x2421f) and then enters attack through an AI rule (0x21750). DOS target search excludes neutral
  candidates (0x22fc1), and an observer with neutral alliance can still select another side. Committing
  the rule clears the saved state at 0x2333f. The script returns the instructor to idle just after its
  startup (0x23c48). The port now allows that search and clears the saved state at the rule transition.

### Titanium archive coverage

- The Titanium archive contains 133 MEK records with eight zero bytes after the 50-byte name. All 133
  round-trip byte for byte with the new parser. Using the raw Titanium archive and the same 54 base-game
  scenes as the Enhanced objective sweep, 51 scenes completed without fallbacks, up from 13 with the old
  parser. BRONSCN1, GOUDSCN1 and MAROSCN1 still fail in this raw archive configuration.
- The normal Titanium and Rendition looks use the base mission and equipment data alongside the Titanium
  graphics resources. The objective sweep exercises load compatibility separately from that normal path.
  Native Windows AI, weapon tables and combat timing still need a live comparison. Proton's Wineserver
  could not bind its socket in the sandbox, so the original Windows simulation did not start.

### Verification

- The DOS sweep completed 59 of 59 scenes under AddressSanitizer and UndefinedBehaviorSanitizer with
  zero fallbacks. All 50 executable comparison groups passed. OpenGL passed 296 of the 300 existing
  sequences, and the new instructor state regression passed separately. The four remaining failures
  all cover instructor contact trajectories and timing. The Vulkan selection passed all 24 checks,
  including all 17 Titanium, Rendition and texture switching checks.

### DOS startup and training contact

- The Watcom generator starts at seed zero (0x49fe0). DOS caches its first 127 values and cycles through
  them (0x4a070). In TNJ1SCN1 the player and instructor drew entries 0 and 2 for startup. Their startup
  sounds drew entries 1 and 3 through 0x40d50. The instructor's initial target-state timer, flank choice,
  and flank side drew entries 4–6. The port shares this sequence between startup, AI and missile
  offsets. Damage and effects still have their own generators in the port.
- The original waits until the tick after `0x444 + rand(0x16c)` (0x2817c and 0x2823a). The player made
  its first draw at ticks 167–171 in TNJ1SCN1, TNJ6SCN1, and GOLDSCN1. The port uses tick 168 as the
  initial controller reference. A restart counts from the current game tick. Fixed turrets use the
  same deadline rule (0x2ee85) and consume no startup sound draw.
- In TNJ1SCN1, the original instructor moved 91 cm west before the player's throttle command at tick
  1750. During contact the Firemoth slid west and pushed the instructor north. In TNJ6SCN1, the Mad Dog
  slid east. The training regressions check these directions, timing, recoil, and armour against the
  instructor's current position. The port limits the DOS update rate to 30 Hz above that display rate
  so both units use the same interval for centimetre rounding and reciprocal contacts.
- The 301 OpenGL sequences passed across the full run and focused reruns. Three Vulkan training
  sequences passed. The 51 executable comparison groups passed, including two complete wraps of the
  DOS random table. The sanitized DOS mission sweep completed all 59 scenes without fallbacks. Its
  scripted player now checks for protected structures behind the target before firing. This keeps
  missed shots from destroying Sable Flame's Sensor Array Dome during the sweep.

### Mission animation order and volley warnings

- The DOSBox traces in HEIDSCN1, TEALSCN1, YELLSCN1, GOLDSCN1, AQUASCN1, CYANSCN1 and
  PLUMSCN1 show the animated AI units in reverse creation order, then the player joints, scenery
  and pooled debris. Golden Spade and Aquiline Fire show several mech skeletons in the same frame.
  The port applies the joint, scenery and pooled debris passes in that order.
- Blade Splint's helicopter has three circling tasks on one node. The native task list applies
  its three TSK rotations in reverse record order. All three decrement the same node countdown.
  Circling now follows the DOS simulation frame schedule and rounds each movement step to whole
  centimetres. As a result the searchlight discs' cumulative drift depends on the frame schedule.
- The native debris pass (0x15da2 → 0x2b740 → 0x2b930) rotates active chunks after scenery.
  The port seeds pooled chunks from the type-11 XPLO allocations and keeps their countdowns across
  reuse. A replay of the captured input angles, starting matrices and phase matched 9,748 circling
  updates in Blade Splint and 17,295 chunk rotations across Aquiline Fire, Golden Spade and
  Plum Wine. The replay checks the rotation, countdown and next phase. It excludes the first
  observation of each matrix and any update without a usable previous snapshot.
- Native combat traces in Aquiline Fire and Golden Spade captured flank manoeuvres and live
  volleys. The warning code (0x2e450) rolls before it tests the weapon and reads its target
  reference at the start of the volley. The port now also consumes this draw for ineligible
  weapons or recipients. The executable comparison varied pilot skill, weapon and recipient,
  including handoffs between two units and a missing recipient.


- The DOS Gaussian table (0x49fe0 and 0x4a0b0), initial debris velocity and spin (0x2b3f0),
  and airborne and landing updates (0x2b930) have executable comparisons. The body pass runs
  in physics-slot order. A detached part starts from its world orientation at the break.
- Damage uses the shared DOS uniform stream. The executable comparison checks draw order for
  centre-torso hits and penetrations. The fire decision reads horizontal range (0x2a43d) and
  checks the torso's forward hemisphere before tracing line of fire (0x2bf20).
- In Bronze Anvil, native gunships enter manoeuvre 12 while attacking the moving convoy.
  The first gunship takes the high helicopter listed first in BRONALY1. The port had chosen a
  lower, closer helicopter and calculated pitch from the ground. The regression checks the
  target assignment, changing pitch from flight altitude, and the start of circling.

### Missile lock and weapon selection

- The DOS player and AI lock check shares the original whole-centimetre squared range and
  16.16 bearing and elevation calculations (0x52ea0 and 0x2ab60). Executable comparisons cover
  each guided weapon, both range boundaries, and one increment to either side of the 16-degree cone.
- After a successful weapon selection, the executable checks the guided flag, rolls for an
  immediate lock when the pilot's range byte is at most four, then plays ENEMYFIR for a player
  target (0x2ea63–0x2eae8). The comparison checks the lock bit, warning, and random cursor.
  The caller replaces the temporary pitch additions for weapon IDs 0 and 4 at 0x2e971.
- DOS AI sets the trigger during the decision (0x2e860). The controller dispatches weapons
  during the same update (0x27812 then 0x27836, and 0x2ed2e then 0x2ed44). The port follows that
  order, including ongoing volleys whose target has disappeared. Native frame snapshots show unit
  movement in the first callback pass (0x15d84 → 0x24810), projectile flight at 0x15d8e, and the
  controller in the second callback pass (0x15d9d → 0x24860). The port follows this order and
  evaluates objectives after combat. Shots launched during the controller pass first move on the
  following update.
  When an AI fires, the eye and mounts are resolved from the updated root and servos with the
  previous animation key still active. The rendered pose advances later in the animation pass.

### Live route, contact, and sound comparisons

- Captured DOS frame schedules in Bronze Anvil cover circling and pursuit for both gunships at
  their recorded 30 m and 45 m flight heights. The one-track CHPRANIM sequences have zero pose
  deltas and carry the `0x10` key flag that selects the 91-tick drive servo (0x1b0d5 and 0x27fdf).
  With those keys running, the first pass reduced the horizontal differences to 5.69 m and
  3.12 m. The current controller starts the player first and interleaves one order assignment
  per leader visit with the remaining startup draws. With movement using the previous controller
  inputs, the maximum differences are 1 cm and 5 cm over 327 combined samples. The original
  only permits a level-one class-1 mech to charge after running out of weapons (0x2c72f and
  0x2c56c). Aircraft and vehicles now take the native retreat branch after the same random draws.
- The DOS drive servo uses the previous throttle value (0x27ecf), then movement updates the
  speed and throttle servos (0x26a51 and 0x26a5c). Gait selection also reads the previous throttle,
  and steering reads the previous speed (0x27ce3). Power-up retains the steering input while
  clearing the leg servo demand. Active movement starts on the update after the startup transition.
  The drive comparison matches 3,000 consecutive native frame steps across seven nonzero tick
  intervals. With this order, TNJ6's player and instructor match all 543 horizontal positions on
  the captured schedule. The controller's separate contact-state check (0x1fd0c) applies to both
  player and AI movement. Powered-down targets remain in the scan.
- The TNJ1 replay from the stock mission matches all 597 horizontal player positions. The
  instructor's maximum horizontal difference is 1 cm. It begins startup on the second controller
  visit, and the estimated boot interval accounts for time already on the mission clock.
- In Silver Staff the Chemical Plant's class-7 scenery starts with the last model in each
  object's detail chain (0x383bb). The world bounding sphere is valid from load. The selected
  model's face vertices remain local until it enters the camera's view (0x3f500 and 0x3ce25).
  Native movement queries through the plant returned clear while it was beyond the view range.
  The port keeps those initial faces until the camera is in range, then retains the posed faces.
  On the 359-frame replay the Gargoyle followed the route to within 1 cm and the Dire Wolf
  matched every recorded horizontal position. A nearby player still hits the plant's posed wall.
- An isolated replay of 78 recorded player contacts used the original attempted displacement
  and the recorded target position on each frame. The port reproduced the selected contact
  and final horizontal position to the centimetre in all 78 cases. The full-run divergence
  remains with the state feeding the contact calculation or with cases outside that sample.
- Native traces recorded positional sound draws between startup and live AI decisions.
  The positional wrapper (0x40bc0 → 0x40c90) draws a pitch for in-range events before sample
  allocation, even when allocation fails. The one-shot mixer (0x409b0) uses the configured
  sample rate. Scenery loops use the drawn rate after acquiring a channel (0x41016).
  The executable comparisons check gain, pan, propagation delay, rate selection, and the
  shared random cursor. Startup cues follow the timer draw and advance the cursor only in range.
  Footstep events survive from the previous animation task until the controller consumes them
  after weapon dispatch (0x27b5d → 0x1f4f0). The player's gait cue uses the sequence then playing.
  The load-time placement pass leaves DOS sound tasks unstarted. Scenery sound runs with the
  world tasks before pooled debris.

- The animation comparison ran 66,720 checks across all 38 original animation records. The
  DUMBANIM tracks have a loop flag without a matching marker. The executable returns key `-1`
  on that update (0x1b016–0x1b02b) and starts the track again on its next visit.
- The Golden Spade turret's live aim uses the target and turret object centres (0x2a210).
  DOS clears both servo demands while offline (0x2ee36 for turrets and 0x280af for mechs).
  Across 77 paired online turret samples, yaw and pitch differed by at most 0.31 and 0.004 degrees
  respectively. The projectile convergence ray still starts at the model's eye.
- The stationary Aquiline Fire capture puts the first hit on the player's Mad Dog at tick 2887
  (15.86 s). Replaying the same frame boundaries reproduces that tick. The sequence test uses
  the captured schedule for the first hit and six fixed rates for weapon reach, armour distribution,
  and time to destruction.
- All 306 OpenGL sequences passed, including the DOS shell audio and volume preview checks.
  The DOS objective sweep completed all 59 missions without fallbacks under AddressSanitizer and
  UndefinedBehaviorSanitizer. All 62 executable comparison groups and four focused software
  Vulkan sequences passed.

## Not checked

- **Live manoeuvre execution.** Bronze Anvil has a paired flight replay. The cause of its
  remaining horizontal drift and the full paths through terrain in other missions remain open.
- **Remaining pose comparisons.** The captured task order and pooled rotations are covered above.
  Full playback comparisons across every mission, the live flight of detached mech parts and world
  fragments, and their spawn timing remain open. Debris landing was compared on flat ground.
- **Scenery detail changes.** The Silver Staff replay covers scenery before its first camera-range
  update. Side-of-view culling and switches between the nearer scenery models still need paired
  comparisons.
- **Simultaneous contacts.** Contact cases outside the recorded training replays still need
  comparison.
- **Mission-wide random ordering.** Startup, AI, damage, missile offsets and positional sounds
  use the DOS table. The complete live order, especially for simultaneous effects, remains open.
- **Live AI warning timing.** Controlled executable tests cover a changing recipient at volley
  start. The stock mission captures still lack a natural target handoff during an active volley.
- **The 3D looks' point-in-polygon rule.** The DOS rule (seg_poly) is used in the DOS look only. demech2's
  IsPointInFaceXZ shows the 3D engines use the same rule in floats; switching the 3D looks to it changed where a
  jetting mech comes to rest under AQUA's dropship wing (the airborne_move_3d sequence), so they keep the even-odd rule
  until they get an audit of their own.
- **Dropship aim and projectile convergence.** The Golden Spade capture covers live turret servo
  inputs. The full dropship firing path and projectile convergence across other missions still
  need paired captures.
