# Known issues

What is still open in the port as of 2026-10-20. Technical detail, engine addresses and evidence for each item are in
[ASSUMPTIONS.md](ASSUMPTIONS.md) under "Open items". None of these blocks play.

## Needs confirming
- **macOS shortcuts** - text input is now off during play (no accent popup on Z + arrows) and Mission Control / Cmd+Tab
  are blocked while a mission runs (`system_shortcuts=allow` in mw2port.cfg to keep them). Needs a check on the Mac.

## Gameplay
- **AQUA enemy fire** - in AQUASCN1 the player is usually killed around 30 s; in the original that happens less often and
  many runs last much longer.
- **Fleeing trainees (ORANSCN1)** - the trainees at Nav Alpha run differently from DOS, so a different one tends to be
  nearest for E.

- **Trainer at the start (TNJ1 / TNJ4)** - the instructor stands dead ahead when the autopilot is engaged early, so the
  autopilot deflects off him; in the original he has usually moved away by then.

## Visuals
- **DOS look fires** - MARO's street fire is drawn as smoke in the DOS look; the original shows an orange flame there.

## Other
- **macOS downloads** - the app isn't notarised: run `xattr -dr com.apple.quarantine "MechWarrior 2.app"` once (README).

## Not planned
- Rendition renderer (Titanium's `zredline.dll`).
