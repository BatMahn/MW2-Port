# DOS MW2.EXE emulated comparison (v1.1.39)

The port's DOS-look sim code is checked against the original DOS MW2.EXE (1.1, md5 51634b60...). The method is the
same one the Ghost Bear's Legacy port used: the original routines run under the unicorn x86 emulator, and the port's C
runs on the same inputs. Each test prints PASS or FAIL with the first differences.

## Running it

The executable is game data and is not in the repo. Make its ELF image once, then build the shims and run:

```
python3 tools/audit/le2elf.py MW2.EXE mw2dos.elf
tools/audit/build.sh
MW2_ELF=mw2dos.elf python3 tools/audit/simdiff.py [-n cases] [-s seed] [test ...]
```

- `mw2emu.py` loads the image and runs functions with Watcom's register calling convention. It runs the start-up
  maths tables (0x3aa90) first and restores the data segment between cases. A test can stub any callee.
- `shim_*.c` include the port's `combat.c`, `ai.c` and `msim.c` so their static functions can be called. The
  DOS-edition flags are switched on.

## Tests and results

| Test | MW2.EXE | Port | Result |
|---|---|---|---|
| heat_state | 0x24f90 (sinking, 66 warning, 81 sequence, shutdown, BlowAmmo, restart) | combat_tick | PASS |
| heat_sink_tiers | 0x4cd7b..0x4ce25 (dissipation by player, difficulty, temperature) | combat_set_player_difficulty | **fixed** |
| heat_override_roll | 0x250f0 (rand(500 x ticks)) | combat_tick | PASS (mean time within 15 %) |
| ai_nav_turn | 0x229e0 | ai_nav_turn | PASS |
| leg_turn_demand | 0x27c87..0x27d70 | msim_leg_demand | **fixed** |
| servo_step | 0x49b80 | drive_servo | PASS |
| servo_setup | 0x49b40 (T = chop(s x 182)) | servo T constants | PASS |
| turret_twist | 0x2edc3..0x2ee2a + 0x49b80 | msim_twist_round | PASS |
| collision_damage | 0x20890 / 0x20a50 -> 0x20930 | collide | PASS |
| manoeuvre_start | 0x2cbf0 | start_manoeuvre | PASS |

## Differences found and fixed

- **Hard difficulty in cold weather:** the player's dissipation below -30 degrees is 67 per heat sink point per
  tick in MW2.EXE (0x4cdf7). The port used the 3D-card engine's 67.5.
- **Leg turning when reversing fast:** MW2.EXE clamps the speed term only at the top (+80). The port also clamped
  reverse speeds at -80. In MW2.EXE a mech reversing faster than 80 km/h turns its legs less, and past 90 km/h the
  other way (the cosine goes negative).

## Differences kept

- **Invulnerable player and BlowAmmo:** MW2.EXE's shutdown-above-100 branch (0x25130) calls BlowAmmo even for an
  invulnerable player. The destruction it leads to (0x25340) then does nothing for that player. The port skips the
  branch for an invulnerable player, so a critical hit on an ammunition bin can't happen in that cheat mode either.

## Checked by reading, not emulated

The manoeuvre chooser (0x2c880), the missile warning (0x2e450), the damage display levels (0x26570), the sound
distance level (0x40bc0) and the message pick (0x11020) were ported in v1.1.38 by reading the disassembly. They need
more of the unit's state than the tests above set up.
