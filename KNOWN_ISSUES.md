# Known issues

What is still open in the port as of 2026-10-26. Technical detail, engine addresses and evidence for each item are in
[ASSUMPTIONS.md](ASSUMPTIONS.md) under "Open items". None of these blocks play.

## Open
- **3DFX / S3 looks** - the S3 edition's texture-cache eviction (it depends on the card's video memory) and its linear
  alpha blend are not reproduced; which of the two overlapping training-mission flags shows in 3DFX / S3 depends on the
  route played in the original, and the port picks one.
- **RENDITION look** - anti-aliased lines, the 16-bit screen and the ground's three fog bands aren't reproduced (no
  hardware to compare against).

## Other
- **macOS downloads** - the app isn't notarised: run `xattr -dr com.apple.quarantine "MechWarrior 2.app"` once (README).

