# Known issues

What is still open in the port as of 2026-10-20. Technical detail, engine addresses and evidence for each item are in
[ASSUMPTIONS.md](ASSUMPTIONS.md) under "Open items". None of these blocks play.

## Needs confirming
- **Zoomed turning** - short left/right taps while zoomed sometimes didn't register. Tap timing was fixed; if it still
  happens, note the keys used and the frame rate (macOS Mission Control takes Ctrl+arrows).

## Gameplay
- **AQUA enemy fire** - in AQUASCN1 the player dies a few seconds sooner than in DOS on some seeds.

## Visuals
- **Titanium PPC / gauss cards** - drawn at full brightness; the original's billboard tints them to 200 / 255.

## Sound
- **Glancing** - while a glance is held, your own footsteps and start-up sound pan the opposite way to the original.

## Other
- **Autopilot** - drives straight to the nav point with no obstacle avoidance, as the original does. Why the original's
  avoidance code never steers is still unknown.
- **PINKSCN1** - one building group's rotation is a guess.
- **Tests** - `node_task_guard_patrol` fails and is disabled.
- **MARO** - can't be started directly in DOSBox for comparison; needs a launch through the game's menus.
- **macOS downloads** - the app isn't notarised: run `xattr -dr com.apple.quarantine "MechWarrior 2.app"` once (README).

## Not planned
- Rendition renderer (Titanium's `zredline.dll`).
