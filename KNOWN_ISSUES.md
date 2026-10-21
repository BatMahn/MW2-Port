# Known issues

What is still open in the port as of 2026-10-21. Technical detail, engine addresses and evidence for each item are in
[ASSUMPTIONS.md](ASSUMPTIONS.md) under "Open items". None of these blocks play.

## Needs confirming
- **macOS folder picker** - the app asks for the game folder when it isn't next to it; needs a check on the Mac.

## Visuals
- **DOS-look cockpit edges** - a few cockpit edges sit a pixel off the original.

## Other
- **macOS downloads** - the app isn't notarised: run `xattr -dr com.apple.quarantine "MechWarrior 2.app"` once (README).

## Not planned
- Rendition renderer (Titanium's `zredline.dll`).
