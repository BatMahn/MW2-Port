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
| heat_override_roll | 0x250f0 (rand(500 x ticks)) | combat_tick | PASS (mean time within 15 %) |
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
- The player outline's front and rear damage levels (0x2f611..0x2f6e4) and message replacement (0x11020)
  each matched 3,000 differential cases. The message comparison used the active or expired status after
  normal message upkeep and the priorities used by the game.

## Not checked

- **The manoeuvre steps** (the step table 0x2c1d0: 0 to 12 per tick) are not emulated. They need a live unit, its
  target and the world for the line-of-sight and probe calls. They were ported by reading the disassembly; the
  chooser, the starts, the flank pick and the patrol sweep around them are emulated.
- **Missile warning projection and target selection** (0x2e50d..0x2e5a7). DOS projects the shooter's convergence
  point and the target's position through the current camera (0x29860 and 0x3f320), then checks bearing and a
  scaled elevation. A live PINKSCN1 dump has type 0 for the player and type 2 for the computer-controlled units.
  Type 2 skips this projection. The player had the warning eligibility bit set in this dump. The port still omits
  the player warning and uses the computer-controlled shooter's current combat target.
- **The weapon pick's follow-up** after 0x2ea63, including the pitch offset for weapon types 0 and 4. Selection
  through 0x2ea63 now has differential coverage for the range, heat, readiness, ammunition and random roll.
- **The damage nibble written into the world model on a hit** (0x26570) remains a disassembly comparison.
- **The 3D looks' point-in-polygon rule.** The DOS rule (seg_poly) is used in the DOS look only. demech2's
  IsPointInFaceXZ shows the 3D engines use the same rule in floats; switching the 3D looks to it changed where a
  jetting mech comes to rest under AQUA's dropship wing (the airborne_move_3d sequence), so they keep the even-odd rule
  until they get an audit of their own.
- **Dropship and turret aim end to end** was not compared frame by frame against DOSBox. Its parts are tested: the
  torso inputs (torso_inputs), the twist servo (turret_twist). The fire decision's interval (rand(range byte) x 0x16 ticks) was checked by reading. The eye position the
  aim starts from (v1.1.40) was checked by reading only.
