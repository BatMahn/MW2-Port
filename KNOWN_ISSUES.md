# Known issues

What is still open in the port as of 2026-10-14. Technical detail, engine addresses and evidence for each item are in
[ASSUMPTIONS.md](ASSUMPTIONS.md) under "Open items". None of these blocks play.

## Needs confirming
- **Zoomed turning** - short left/right taps while zoomed sometimes didn't register. Tap timing was fixed; if it still
  happens, note the keys used and the frame rate (macOS Mission Control takes Ctrl+arrows).
- **Mac intro movie audio** - the Mac RAVE disc's `mintro.smk` has 22 kHz 16-bit audio (PC: 11 kHz 8-bit). Not used yet;
  could be picked up optionally when the file is in the game folder.

## Gameplay
- **AI ground jets** - AI mechs using directional jets on the ground don't build up speed the way the player does.
- **AI jumping** - one AI manoeuvre can send a mech 250 m straight up when it isn't closing in (seen in PLUM and FUCH).
- **Hit detection** - shots against mechs may test a rough sphere per part rather than the part's shape. Needs a re-check.
- **Wall slide** - sliding along the wall in training mission 6 takes about 5 s in DOS and about 10 s in the port.
- **Mid-air creep** - a mech pushing against a steep slope in the air can hover or creep instead of dropping.
- **Untargetable buildings** - the original's rules for which buildings can't be targeted aren't ported.
- **Destroyed target marker** - after the target is destroyed, DOS shows no marker; the port falls back to the nav marker.
- **Jet fuel** - DOS and the 3D editions differ by about half a percent; the port uses the 3D value.
- **Keypad 5 (torso recentre)** - in DOSBox it didn't fully recentre the torso; the port recentres fully, as the code reads.
- **Torso servo** - the player's torso easing (0.2 s) between the demanded and actual twist isn't modelled.

## Visuals
- **Start time of day (3D editions)** - CHED, TEAL, RED and TAUP start at night only when they're the first mission after
  launch; in the original their start depends on what ran before them in the same session.
- **Titanium fog** - one depth rule for the flat-shading fog term isn't ported (no visible effect in current missions).
- **Titanium with textures off** - textured polygons show their texture's average colour; the original uses its flat shade.
- **Titanium PPC / gauss shots** - still use the 3D editions' effect models.

## Sound
- **In-game menu sounds** - two menu sounds (0xe1 / 0xdb) aren't traced.
- **Chase-view sound** - positional sounds are heard from the player's mech; the original hears them from the camera.
- **Death sound in the cockpit** - the cockpit-view rule for one death sound isn't applied.

## Other
- **Autopilot** - drives straight to the nav point with no obstacle avoidance, as the original does. Why the original's
  avoidance code never steers is still unknown.
- **PINKSCN1** - one building group's rotation is a guess.
- **Tests** - `node_task_guard_patrol` fails and is disabled.
- **MARO** - can't be started directly in DOSBox for comparison; needs a launch through the game's menus.
- **macOS downloads** - the app isn't notarised: run `xattr -dr com.apple.quarantine "MechWarrior 2.app"` once (README).

## Not planned
- Rendition renderer (Titanium's `zredline.dll`).
