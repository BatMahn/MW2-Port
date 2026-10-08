# Game-logic audit against the decompiled engine

Every behaviour the port still carries as ASSUMED (from ASSUMPTIONS.md), to be traced in the 3Dfx engine (d3fx_all.c) and the DOS executable (mw2_decomp.c) and either confirmed or replaced. Engine addresses already known are listed; "-" means the routine still has to be found.

| # | Behaviour | Engine routines referenced | Status |
|---|---|---|---|
| 1 | Throttle model (true speed = walk MP x 300 cm/s; readout x 1.5, 0x10018470 - session 11) | 0x10018a20, 0x1001a180, 0x10034730, 0x10256738 | combat part (walking heat 0x1001a180, sinking 0x10015f50) DONE |
| 2 | AI behaviour | 0x1001cab0, 0x1001fcc0, 0x1001fdd0, 0x1001fe30 | conditions DONE (table 0x1024bc70) |
| 3 | Sky drift rate | 0x100277b0, 0x10027af9, 0x10038982 | DONE (skirt fixed; ceiling drift per frame, 0x10027ab0) |
| 4 | Compass tape | 0x10022b10, 0x10022960 | DONE |
| 5 | Reticle | 0x10022710, 0x10022c40 | DONE (centre: matches) |
| 6 | Weapon list | 0x1001e820, 0x10044350 | DONE (colours by state / group; one group per weapon) |
| 7 | Speed / heat bars | 0x1001c350, 0x1001c4a0, 0x1001c5b0 | DONE (throttle bar = setting, eased; palette bands) |
| 8 | Target bracket | - | DONE (0x10022830 corners TGTGP1-4, h = R F / depth; TGTOFF X) |
| 9 | Messages | 0x10002f20, 0x10003010, 0x10002e40 | DONE (two lines, durations, priorities) |
| 10 | Display damage | 0x1001e670, 0x10011720, 0x10021300, 0x10004710, 0x100022f0 | DONE (levels per instrument; only the target viewer and viewport window fuzz - SNOWCLR static, level 1 Markov 7/10 off / 3/10 on per frame, 3+ for good; radar level > 0 blocks light amplification; DOSBox-confirmed) |
| 11 | Meter bar | - | DONE (not drawn by the engine's bars) |
| 12 | Mixing | - | DONE (falloff 1 - (d / 500 m)^2, pan +-0.766); 8-voice stealing open |
| 13 | CPIT rect 0 | - | DONE (rect 0 replaced by the radar's own window, 0x10003250) |
| 14 | Bottom-centre bars | - | DONE (11: throttle / heat / dH/dT bars) |
| 15 | End sequence | 0x10031640, 0x100374e0, 0x10009f50 | DONE |
| 16 | On-screen texts | 0x10031440, 0x100086d0 | DONE (outcome text only with voice off; node texts go to a log) |
| 17 | Steps refused | 0x1000d520, 0x10010830, 0x10019310 | DONE (climb = MGEO int[0], lookahead int[6], 45 deg front faces) |
| 18 | Altitude tape | 0x100222e0 | DONE (absolute elevation) |
| 19 | Debris | 0x10015230, 0x100152e0, 0x100156f0, 0x10046d40 | DONE (engine, ASSUMPTIONS Debris row) |
| 20 | Bottom-centre bars | - | DONE (as 14) |
| 21 | Jets bar | 0x1001c750 | DONE |
| 22 | H T A L (F6) | 0x1001b890 | DONE |
| 23 | Shutdown / startup / autopilot | - | DONE (start-up lock 0x43e + rand 0x16a ticks; autopilot 0x10015c00) |
| 24 | Weapon camera (F9) | - | DONE (at the projectile, level, zoom 2; one shot followed) |
| 25 | Radar modes | - | DONE (1 -> 2 -> 0, descriptor windows, sprites); satellite map render open |
| 26 | Vision modes | - | traced (0xa6 palette fade to PALG 12, 0xa7 own-colour outlines); port keeps its tint / two-colour lines |
| 27 | Pilot keys | - | DONE (glances, pan, keys - ASSUMPTIONS View controls) |
| 28 | Return objectives | shell 0x21ee0 | DONE (raw priority byte) |
| 29 | Mech Lab | - | DONE (11f: masses, sinks, XL, armour, crits, limits, locations, numbering) |
| 30 | Star size per mission | shell 0x291b0 / 0x3ab50 | DONE (default / maximum) |
| 31 | Detail: OBJECT / TERRAIN TEXTURES (MW2SND +0x14 / +0x18) | 0x10011570, 0x100116d0, 0x1002a8a0 | traced (type-word bits, whole-texture average); port flat-shades by layer |
| 32 | Detail: DISPLAY DETAIL (+0x1c) | 0x1002ff90 | traced (LOD distance x 0.5, effect-neighbour rule); port forces repr 1 |
| 33 | Detail: CHUNKY EXPLOSIONS (+0x24) | 0x10025450 | traced: gates only GT 0x8000 debris nodes (not in the port); mech chunks ungated now |
| 34 | Formations | - | DONE (table 0x1024ed40: order confirmed, name voices added) |
| 35 | Commanded mates | 0x10012fd0, 0x10013f00, 0x10014920 | DONE |
| 36 | Task complete | 0x10013680, 0x10013090 | DONE |
| 37 | Engage message | 0x10014390 | DONE (none) |
| 38 | Texture-map merge | - | DONE (later BMID wins, 0x1002d060) |
| 39 | Destructible objects | 0x1003eb5c, 0x10045c30, 0x10046a90, 0x10046c20 | DONE (truncated damage, flag 0x0004, effect 0x0d / 3 + chunk at the hit) |
| 40 | PPC / gauss cards | - | DONE (slots 0x0b / 0x0d bank 0, 4 m / 2 m) |
| 41 | Used | - | open |
| 42 | Directional jets | 0x10019310 | DONE (airborne drag open) |
| 43 | INSPECT_TARGET | 0x102474d0 | DONE (ASSUMPTIONS INSPECT_TARGET) |
| 44 | Missile lock and homing | 0x100437a0, 0x10044560, 0x10045910 | DONE (0x10044560 range window, 16 degrees, 2 s; tones 0x1001e6c0) |
| 45 | Target bracket | 0x10022ea0 | DONE (as 8) |
| 46 | Laser colours | 0x10043cc0, 0x10044ac0 | traced (3Dfx meshes 3x3x12); port keeps the DOS look |
| 47 | Footsteps | 0x1000b3a0, 0x1000e600 | DONE (cockpit half volume; engine falloff) |
| 48 | Provoked mechs | 0x10012de0, 0x10014110 | DONE - original restored (off; MW2_PROVOKE=1 opt-in) |
| 49 | Walk cycle loop | - | DONE (key flags 0x8 / 0x4 -> 0x2) |
| 50 | Step rate | - | DONE (key time only, integer gait rule) |
| 51 | Opening | - | DONE (start-up lock); fade / beep user-reported |
| 52 | Gait sequences | - | DONE (0x1000b3a0: run from 3/4 speed, key time by gait) |
| 53 | Mission radio | 0x10009d60, 0x10031150 | DONE (table 0 only, once per node index; pacing kept per user) |
| 54 | Star alliances | 0x10013a50, 0x10014110 | DONE (neutral turns on its attacker) |
| 55 | Target viewer | - | DONE (1 wire / 2 shaded / 0 off; camera 3R, zoom 2) |
| 56 | Drop screen overlay | - | DONE (frames 0-4 / 330 ms, x 351 y 0, supanm) |
| 57 | Ground shadows | - | DONE (no shadows in the engine: T_1 pieces are type-1 flat 0xcf) |
| 58 | Player collision | - | damage DONE (0x1000c160: (dv - 3.069) x 0.0501 x 3), sounds DONE (0x10019cf8); contact DONE (3D spheres, MGEO radius, landing / bounce, 0x1000ba20) |
| 59 | AI collisions | - | as player collision (3D spheres, landing on mechs, bounce) |
| 60 | Heat bar colours | - | DONE (0x1001c350 / 0x1001c4a0) |
| 61 | AI keeps clear of the player | 0x100201c0 | DONE (mechs are obstacles; 15 m rule removed) |

Order of work: combat (damage, heat, weapons, lock) -> movement (throttle, gait, collision, jets) -> AI (states, targeting, provocation, formations) -> mission logic (node kinds, radio, results) -> HUD details.

## Area 1 (combat) - results

- Heat bar: engine 0x1001c350 - p = heat x width / 100; under 50 % yellow p at each end over blue; from 50 % yellow (width - p) at each end, red between; 100 % all red. Implemented; matches the DOSBox sustained-fire captures.
- dH/dT bar: engine 0x1001c4a0 - yellow over blue to full scale, then red over yellow for the excess. Implemented (full scale still the port's 10 heat/s - the engine's unit, 0x300 of a smoothed per-tick value, not yet converted).
- Display damage: engine 0x1001e670 - each instrument rand(10) < 2 (< 5 from the 0x10016e33 caller). Implemented.
- Heat model (sinking, walking heat, shutdown at 80 / restart under 65 / 2x sinking while down / destruction over 100 unless overridden / override ammo cook-off): already from the engine (0x10041410, 0x10015f50, 0x1001a180) - re-checked, unchanged.
- Weapon fire, recycle, per-shot damage and heat: from the engine weapon table (+0x30 damage, +0x34 heat, refire / interval) - re-checked, unchanged.
- Lock: 0x10044560 (range window +0x3c..+0x40, 16 degrees, 0x16a ticks) and the tones 0x1001e6c0 - done.
- Still open in combat: the weapon list's colours (the entry draw at 0x1001e9a0 / 0x1001e820 not yet decoded).

## Area 2 (movement) - results so far

- Gait: engine 0x1000b3a0 - speed over the stop level as a fraction of full: gait 1 under 1/4, 2 under 3/4, 3 above; the run sequence at gait 3 (the port had switched at 1/2), the walk below, the reverse walk backing up; key time x 1.5 / 1 / 0.75 by gait (the player had used the sequence index). Player and AI.
- Mech-mech collision: engine 0x1000c160 - damage from the relative speed, (dv - 3.069) x 0.0501 x 3, through the same location rules as walls (0x1000c1f0) - the port's formula already matched.
- Impact sounds: engine 0x10019cf8 - 0xc8 MECBLDC1 against an object (mechs, buildings), 0xe5 MECMTNHD against the terrain; volume = speed / 23.02 cm/tick, full above. Replaces KICKCHNK; walls and terrain now sound too.
- Step height: correction - the +0x50 / +0x54 / +0x58 values passed into the move segment (0x10019a78) are the unit's position, not a step height; the step height is still the port's 1 m (open).
- Throttle keys: INPUT.MAP controls map to state bytes (control table 0x10256720..: jumpjet_fire_backward 0x10159e71, throttle_plus 0x10159e72, throttle_minus 0x10159e73); the code reads them through a base + index, not yet followed to the ramp.
- Throttle keys: MEASURED in DOSBox (holding =): 35 % at 0.5 s, full before 1 s - an accelerating ramp (a = 2.76 / s^2), restarting from rest on release; replaces the linear 0.6 / s.
- Open: the contact radius (4.5 m), directional jets.

## Area 3 (AI) - first finding

- Engine 0x10014110 ("Mech %d has attacked"): records the attacker only when the PLAYER is hit (victim +0x14a = 3, +0x14e = attacker | 0x200) - it is the player's "who shot me" record, not AI provocation. The port's provocation rule (the hit mech's group and enemies within 600 m attack for 30 s) has no engine counterpart found yet; AI reactions to being hit must come from the AIT rule programs' conditions - next: check src/ai.c's conditions against the engine's condition table, then decide whether the provocation rule stays.

## Area 3 (AI) - conditions checked against the engine table 0x1024bc70

| cond | engine | what it does | port |
|---|---|---|---|
| 1 | 0x10012b40 | any living, visible enemy (iterator 0x10013a50, 0x10013c40 / 0x10013430) closer than R x 100 cm (R = -3: unlimited) | matches, plus the port's provocation override |
| 2 | 0x10012bf0 | farthest enemy beyond R | matches |
| 3 | 0x10012a80 | enemy within R (default 100 m) | matches, plus provocation |
| 4 | 0x10012a60 | returns the current target (keep it) | matches |
| 5 | 0x10012a70 | always 0 - fires only when posted as an event | matches |
| 6 | 0x10013090 | the current target has flag 4 (destroyed / gone; 0x10013c40 with 2 -> mask 4); also radios message 5 when the unit belongs to the player | matches (the message not sent) |
| 7 | 0x10012fd0 | the group target picker 0x10012de0 within R (0xfffe: unit +0x162 / 100, 0xffff: +0x15e / 100, 0: computed); never while unit +0x152 & 3 | matches in effect, plus provocation |

Result: the rule conditions match. The engine has no "attacked from beyond range" reaction for AI mechs - 0x10014110 records an attacker only on the player's own record (single player) - so an AI mech outside its rules' ranges does not react to being shot. The port's provocation (the hit mech's group and enemies within 600 m attack for 30 s) is a deviation the user asked for earlier; now OFF by default (the original, per the user); MW2_PROVOKE=1 turns it on.

- Formations: engine table 0x1024ed40 {voice, text}: "Formation change to" (11), then echelon left 79, echelon right 81, line abreast 82, line astern 83, vee form 86, wedge 87 - the port's 1-6 order is the engine's; the name's voice line now follows the first.
- Open in AI: Defend's 300 m engagement radius (the DEFEND program's own ranges to be checked), the "keep clear of the player" 15 m (a port addition from a user report).

## Session 11 (2026-10-06) - areas 3 (rest), 4 and most of 5

- AI: Defend = four patrol points at +-100 m round the defended mech, no engagement; task complete per the exit hook 0x10013680 (message 5 on a kill, 10 only from state 2 / an unreached defend); Engage at Will: no message, and the group assignment 0x10014920 for the player's star; mechs are avoidance obstacles (the 15 m rule removed); neutrals turn on the player after being hit.
- Mission logic: 0x1000a1d0 reproduced - 0x1000 instant, 0x400000 re-arm, timeouts fail kinds 1/2/8/0x20/0x100/0x2000, 0x20 needs the other 'M' nodes, strict whole-second timers, toggle / force / reset per the engine; radio only for table 0, once per node index; end sequence waits for the radio, CTRL-Q only, outcome lines from the MTBL header; MW2MSN entries [1]-[4] as the engine (raw priority); MW2CAR triples enemy / neutral / friendly; SDSC default vs maximum star size.
- HUD: weapon list colours, throttle bar, kph x 1.5, palette bands for heat / dH/dT / jets, HTAL, two message lines, directional jets.
- MOVEMENT FIX: the true speed is walk MP x 300 cm/s; the DOS 75 / 160 kph readings are the readout's x 1.5 - the open "mech speed vs DOS" report.
- Open: compass / reticle / altitude reference / laser drawing (area 5); weapon list column order; airborne jet drag; enemy groups' target assignment (0x10014920 for non-player groups); dH/dT full-scale unit.

### Session 11b - HUD remainder
- Compass (window 24, legs heading, twist band, bearing marker, elevation / side arrows), altitude tape (window 23, absolute
  elevation, ground and target markers), weapon list order by mount location, reticle confirmed. The HUD record's five
  positions are never read by the 3Dfx HUD.
- Laser bolts traced (3Dfx: long thin meshes, no glow); the port keeps the DOS look pending the user's choice.


### Session 11f - DOSBox checks and the open items
- DOSBox: top speed / gravity (engine 0x100190d0; JACK 75 = DOS, YELL 92 vs 90), voiced text hidden while voices play (SB16 run), compass marker on the target's side, ranges "1.01k" (0x10021150).
- Movement: start-up lock, per-mech contact radius / climb height (MGEO int[6] / int[0]), walk loop by key flags, integer key time, jet drag, autopilot, footstep / positional volume.
- AI: the player's star stays in formation on node changes (the JACK "stuck mate": they had walked off to en01Star).
- HUD: bracket corners, off-screen X, radar modes / window / sprites, F9 camera, target viewer modes, command panel without box, drop screen frames.
- World: later texture maps win, type-1 polygons flat (no shadow pass), PPC / gauss cards, destructibles, explosion light latched + PLNT gate, sky skirt fixed.
- Mech Lab: the shell's mass rules - 61 / 61 stock mechs within tonnage (`make check`); singles / doubles, XL never refused, SLOWER drops jets, armour buy / trim, crit placement messages, actuators, weapon / ammo limits, the shell's location indexing, user variants 00-99.
- Still open: enemy stars' per-member node assignment (0x10014ba0 count / lock bits), the attacker reaction 0x10012520, neutral iterator (conflicts with the session-11 neutral rule - left as is), node power-down states 10 / 11, image-enhancement colours per polygon, light amplification as a palette fade, satellite map as a top-down render, distance LOD (display detail), effect-neighbour rule, dH/dT exact unit (frame clock unpinned), 8-voice mixer with priority stealing, GT 0x8000 debris nodes, mek.h left / right names vs the shell.

### Session 11g
- Cockpit struts: SOLVED - the cockpit meshes on the full model's skeleton (engine LOD swap); DOSBox overlap 0.89-0.97 over seven mechs.
- Aggro: attacker reaction 0x10012520 and node member counts added; neutrals kept passive until hit (DOSBox TNW1).

### Session 11h
- Done: power-down states 10 / 11, light amplification (DOS palette), satellite map, distance LOD, effect neighbours, dH/dT, 8 sound channels, location side names.
- Open: enhancement colours for classes 0x400 / 0x100, GT 0x8000 debris nodes, per-sound caps, the original frame rate.

### Session 11i
- Done: building debris nodes, enhancement colours, sound priorities / caps, orthographic satellite map. Only the original frame rate remains an assumption.
- Frame rate: 30 fps agreed with the user - closed. The audit list is now empty.

### Session 11j
- User report (2000 x 1300 screenshots): the red X beside a visible target - the off-screen test only covered the 4:3 middle. Fixed.
- Esc quit the mission: now the original's MAIN MENU (menu 4) with its pages, pause, keys and look (DOSBox capture).
- In-game Video mode (user request) in the GRAPHICS page.
- User report: LRMs fired and the reticle raised mid-volley should fan / arc up - the player's shots aimed at the target in a 15-degree cone; now each leaves along the current aim line (0x10043cc0).
- User report: the target viewer's wireframe kept blown-off limbs - now left out.
- User report: no Mech Lab sounds - the shell's button / chassis-name sounds traced and added (lab, customize, star config, hall holoprojector).
- User reports: Ready Room MECH LAB whoosh (0x64), drop screen size in a window, menu MIDI under the mission, drop screen 2 s, enemies' walk animation (absolute-height tests, start-up jets, sequence restart). Locked missiles keep homing after the reticle moves (as reported for the original) - already so.
- User reports: lab whoosh cut short (now the holotable clip + whoosh, then the lab), ACCEPT MECH returned to Star Config (now where it came from), lab label hover, solid target view, muzzles on the wrong side.
- User reports: nav points shown by their record names (YELLNAV1) - now the NAVP name ("Nav Epsilon") as the engine's viewer (0x10020e40), and NEXT / PREV nav, the radar and autopilot skip the AI's own nav points (0x1001cea0); the DOS look's wireframe target now coloured by damage per location.
- User reports: image enhancement now shows mech damage per location; AI fire follows the engine (one weapon per decision, fire rolls) - it had fired everything at once; difficulty confirmed as heat-only in the engine. Mouse tilt reversal undone at the user's request.
- User request: engine audit (four areas) - damage model, heat and fire, AI aim / lock, AI decisions and movement brought in line with the engine (ASSUMPTIONS section 20). Test hook MW2_AI_TRACE=<actor>.
- User request: cleaner HUD font - whole-multiple pixel-exact text (sharp-bilinear shader, 1:1 atlas with gutters).
- User reports: vector menu text, keyboard tilt reversed, HUD dark while shut down, one-legged mechs kept walking (ramp rate bug), missiles pitched the wrong way.
- User report: shutdown should stop movement and shrink / fade the HUD - power state from the engine (0x1001e340 / 0x1001a180 / 0x100017f0 / 0x10001710) and DOSBox captures: radar / target display / viewport windows animate down and up (181 ticks), start-up shows the weapon names only, controls cleared while down (torso recentres, coasts to a stop), Ctrl+s restarts at once (s no longer does). Test hooks MW2_POW_TRACE, MW2_KEYS "ctrl+x".
- User reports: shutdown (controls, HUD shrink, s / Ctrl+s toggle, dead-end after a manual shutdown in a heat sequence), overridden overheat never fatal (DOS cook-off), wreck colour in image enhancement, chemical plant objective (child structures), snag near the plant (collision segment test), dead structure target dropped.
- User request: steps 1-3 of the automated sweeps (objectives, walking, sequences) built and run; bugs found and fixed: type-6 structures unhittable, vehicles frozen, unit / model drift, empty-target objectives waiting forever, reach on structures, AI wall contact with collision damage off, missile homing, volleys during start-up, target upkeep with the HUD off, pause / self-destruct / eject / auto-eject / MFD cycle / reset inputs.
- User report / request: Mech Lab choice persisting across missions (star reset on Ready Room entry); PTBL path movement; ejection camera, career status, AI auto-eject; F10 ordnance camera.
- User report: a jumping Jenner landing on the player took off both arms - mech-mech contact was a 2D circle test and an airborne mech kept its air speed after the push-back, so it rammed again every few ticks (damaging both each time). Now 3D spheres with the engine's landing rule, normal-based locations (head from above), relative speed and bounce (0x1000ba20 / 0x1000c1f0); sequence test mech_lands_on_player (MW2_TEST_DROP hook).
- User report: weapons / HUD / radar flashing after heavy damage, and the target camera fuzzing in and out - the port had flashed six instruments; the engine (and DOS, confirmed in DOSBox with patched levels) only fuzzes the target viewer and the viewport window with the SNOWCLR static (ASSUMPTIONS "Display damage (engine)"). Mech-mech contact now damages only the mover, every contact tick (0x10019d6a -> 0x1000c160). Tests: display_damage_static, mech_rams_mech, mech_lands_on_player updated; hooks MW2_TEST_DISPLAY, MW2_TEST_IMMOBILE, MW2_TEST_DROP dx.
- User reports: Jenner landing on the player (3D sphere contacts, landing rule, mover-only damage), nav arrival for hidden points, damaged-display flicker (only the target viewer / viewports fuzz with SNOWCLR).
- User report: lasers off the reticle (the aim ray now leaves from the eye object, 0x10044950 - shots hit within 16 cm of the reticle point at 100 / 300 m; the convergence range keeps its last value), no "Mission failed" on death (DOS 0x16e80: the unresolved star fails when the end flag is set - outcome 3 and the MTBL failure line for every death and for ejection), specks after an explosion (death camera: the engine's spinning external orbit, HUD gone; the wreck sequence once a frame, four chunks per 0x10b / 0x20b and per type-3/4 effect, a 32-chunk pool of its own). DOSBox self-destruct capture: docs/reference/dos_selfdestruct_mission_failed.png. Tests: death_mission_failed, override_death_mission_failed, eject_mission_failed, wreck_debris, aim_converges_on_reticle (hooks MW2_TEST_AIM, MW2_AIM_TRACE, MW2_AIM_BODY, RADIO dump line).
- User request: death camera smoothing and the red flash; enemy mech destruction. Death camera: the engine's servos (0x1003b7e0, tau 0.5 / 0.7 / 0.2 s) from the cockpit eye out to 3 x the radius, the own mech hidden while the camera is inside it. Red palette flash at death and on head / centre-torso breaches and ammunition explosions (0x1002ad50; DOS snaps back, the 3D editions turn the world red at once). Destroyed mechs: the old wreck model was the cockpit representation - now the mech as it stood at representation 1, and destroyed locations send their whole subtree flying (a centre-torso kill blows the mech apart, as DOSBox shows). DOSBox captures with a patched MW2.EXE (enemy damage x32). Tests: death_red_flash, damage_red_flash, enemy_blown_apart (hook MW2_TEST_AHIT); docs/reference/death_flash_dos_vs_port.png, death_camera_dos_vs_port.png, damage_flash_dos_vs_port.png, enemy_destroyed_dos_vs_port.png, enemy_destroyed_after.png.
- User reports (Cadet Training): "you keep spawning a mech in the same spot as me" - the scenario's own TNx#USS1 GPS (+0x0e == 0, the engine's player whatever record holds it) was spawned as an AI actor on the start point while the port flew a USERSTAR its shell had written (the original's training launch writes no star file); now the player comes from the mission tree, player GPSs are never actors, the training launch writes nothing; objective sweep: spawn-overlap check (unit within 10 m of the player = anomaly, AI units within 3 m = note), per-mission star size, and the aim swings sideways at small structures (the cadet Firemoth's arm lasers sit 2.8 m off the eye and passed either side of a centred training sphere). "Nav points are black dots too?" - the dark pyramids were TNJ1SCRI scrub (rocks / cacti, colour type 3): now the engine's billboards of bank-0 bitmaps (both looks; matches DOSBox); the MW2_SHT1 pool no longer drawn at the map origin. Tests: training_own_mech, training_scrub_billboards.
- Sweep failures (coordinator request): MARO with the default star - the unarmed limousine took the out-of-weapons flee (the port read 0x10020880 as "no weapon range"; the engine says an unarmed unit is not out of weapons) and, once routed, stuck in a building corner (the engine's avoidance follows the wall: 26.6-degree probe back toward the goal, +-409.6 turn while blocked, 0x100201c0). FUCH with a 3-mech star now passes with the avoidance fix (timing-sensitive: the harness waits at the mission-area edge for FUCHENS1 / 3 while other groups attack the firebase). GOLD with a 3-mech star: harness held fire for the protected palace forever - fallback after 40 s. Test hooks MW2_BLOCK_TRACE (what refused a unit's step), MW2_SWEEP_ACTOR, MW2_AI_TRACE adds heading / avoidance.
- User question "are we sure the startup sequence matches dos for picture/sound?": DOSBox YELLSCN1 / TNJ1SCN1 recorded as ZMBV video with audio (mapper bind Ctrl+F11 -> video capture, SB16 on), sounds identified by matched-filter cross-correlation against every SNDS record, the KEATING files and the CD tracks; traced 0x10007900 / 0x1002ad50 (fade), 0x1001ab71 (state 0 sounds), 0x1001e6c0 (online sound), 0x10021300 (viewer). Fixed: fade 3.5 s wall-clock -> engine frame-stepped 0x16a ticks from frame 3 (2.0 s); drop screen fades 0.86 s with the music starting then; MECOMBEP -> MECBSYRX at online; friendly / neutral units' MECTURX2 on frame 2; radio no 4 s start wait (mission time); spurious TORSLOOP at t = 0; target viewer black without a selected nav, contents not drawn while starting up. Open: the drop touch-down (cockpit dip + MECMTNSF) at the start. Test mission_startup; strip docs/reference/startup_dos_vs_port.png.
- Coordinator follow-up (mission start): the touch-down - start height from the start NAVP (YELL 25 m), landing sound by fall speed (0x1001c0a0: MECMTNSF / MECMTNHD, the port had 0xf0 for every landing), the camera jolt 0x1001c120 (keyframed servos, pitch sign from DOSBox: its slow frames show only the second keyframe, matching the model to ~1 degree), AI landings positional; MECSHTD1 on s, voiced messages queue behind the voice channel, MECTRDXX at death (not on restart, DOSBox). Spurious: TNJ1's DOSBox first-frame landing (long first frame). Tests touchdown, mission_startup; sweep 59/59.
- Coordinator: start damage from the drop (port: WHIT 5.6 / PLUM 3.1 / TEAL 0.3 per leg) - DOSBox WHIT / PLUM land hard too (MECMTNHD) but the display stays blue: the engine applies no damage at all until the player's first start-up is done (DAT_1024c570 in 0x100171d0, also BlowAmmo). Ported (combat_damage_live); damage_red_flash / enemy_blown_apart now run without the start-up (their hits come at 4 s); touchdown checks WHIT.
- User question (frame rate): sim time now carries sub-millisecond remainders (sim_ms); zero-length steps skipped.
- Coordinator (open item "path objects not obstacles; TSK types 0-4"): moving objects stay in the engine's one collision list (0x1000fbe0 -> 0x10029c10 moves their entry), so the port's walking test, ground (type 0 tops) and avoidance footprints now include the `moving` parts where they are each step (msim.c terrain_t mov / mov_refresh); nothing pushes units. TSK types decoded from 0x1025a630 and ported in mech3d.c world3d_paths: 0 spin (training targets, radar dishes), 1 colour frames (<< 4 flat palette colours: targets, dropship exhausts), 2 circling (wind carriers), 4 looping object sounds (glview obj_sounds, RIFF WAVE records decoded, sfx_pcm_level); type 3 is the mechs' animation. Test object_tasks (GOLD dropship blocks then is walked under; TNJ1 target spins; CYAN monorail loop); mission_startup ignores the zero-volume object loops.
- Coordinator (open items: throttle response, climbable avoidance, engine sound events): the speed chain traced - throttle servo +0x44 (T 36.2 ticks, every mech), drive servo +0x24 (36.2, then 90.5 ticks once a key flagged 0x10 runs), grounded velocity 1/45 per tick (0x1001a180 / 0x100190d0 / 0x10019310) - replaces the DOS-fitted ramp for the player and the AI (msim_drive_step); gait and walking heat by c[0x13], leg turn by c[0xb]. DOSBox YELLSCN1 re-measured from ZMBV frames (first start, second start, stop): the port matches within a frame. Avoidance: 0x100204b0 (types 0 / 5 face slope <= 40 degrees, class 0x50) via probe_nearest / probe_climbable. Sounds: 0xe9 MECOVHW1, 0xec MECRDWA1, 0xcd MECBSOXX, 0x147 VIEWZOOM, 0x100 MISLTRCK, 0xb2 IREDMODE, 0x6f ENEMYFIR traced to their events and played (glview / combat breach latch / msim on_sound NULL = cockpit sound); IREDMODE confirmed in DOSBox. Test masc_not_fitted presses v once the speed has settled. sweep_ai: BRONSCN1's Wolf Tank now backs into a steep rise and re-picks manoeuvre 10 on the contact (the engine's contact -> 10 rule; its route changed with the speed chain).
