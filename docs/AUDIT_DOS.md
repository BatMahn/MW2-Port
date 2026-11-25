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
  of draws.

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

## Not checked

- **Live manoeuvre execution.** Glide, flank, and circling inputs have controlled comparisons. Their paths through
  live terrain, including rebuilding patrol points around a moving target, still need DOS traces.
- **Full hull poses.** Bounds, representation changes, and hit ordering have executable comparisons.
  The port still derives the animation hierarchy from its pose solver. The original's fixed-point matrix
  composition and full per-frame pose changes need direct coverage.
- **Live AI warning timing.** The recipient now comes from the AI's tracked target. A live trace is still
  needed across a target handoff during an active volley.
- **The weapon pick's follow-up** after 0x2ea63. Selection through 0x2ea63 has differential coverage for
  range, heat, readiness, ammunition and the random roll. The pitch additions for weapon IDs 0 and 4
  are overwritten at 0x2e971 in the caller before the torso controller reads the input.
- **The 3D looks' point-in-polygon rule.** The DOS rule (seg_poly) is used in the DOS look only. demech2's
  IsPointInFaceXZ shows the 3D engines use the same rule in floats; switching the 3D looks to it changed where a
  jetting mech comes to rest under AQUA's dropship wing (the airborne_move_3d sequence), so they keep the even-odd rule
  until they get an audit of their own.
- **Dropship and turret aim end to end** was not compared frame by frame against DOSBox. Its parts are tested: the
  torso inputs (torso_inputs), the twist servo (turret_twist). The fire decision's interval (rand(range byte) x 0x16 ticks) was checked by reading. The eye position the
  aim starts from (v1.1.40) was checked by reading only.
