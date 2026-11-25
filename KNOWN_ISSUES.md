# Known issues

Open issues in the port. Technical details and evidence are in [ASSUMPTIONS.md](ASSUMPTIONS.md) and
[the DOS parity audit](docs/AUDIT_DOS.md).

## Open
- **DOS mission parity**. Bronze Anvil's two gunships, simultaneous contact ordering and debris landing
  on slopes have recorded checks. Live comparisons with the original DOS game still have gaps for
  other aircraft routes, AI target handoffs and debris spawn timing. See the DOS parity audit.
- **Titanium simulation**. The 161 raw scene entries include 151 verified objective completions.
  All 54 base-game scenes pass. ZACK and Station Defense pass with the default Timber Wolf.
  Apache and Cali pass with local AI opponents. Live network sessions still need verification.
  The ten `MIS0SCN1` through `MIS9SCN1` templates require output from the original Mercenaries
  mission generator. The port can load external mission geometry and reports missing generator files.
  Native Windows combat timing still needs a live comparison.
- **Classic Mac build (ALPHA)** - new, and has had almost no testing on a real PowerPC Mac: crashes, sound / music
  (QuickTime's synthesizer is untested) and display problems are expected. docs/CARBON.md lists what still needs
  checking on real hardware.
- **3DFX look** - which of the two overlapping training-mission flags shows depends on the route played in the
  original, and the port picks one.
- **RENDITION look** - anti-aliased lines, the 16-bit screen and the ground's three fog bands aren't reproduced (no
  hardware to compare against).

## Other
- **Graphics interfaces** - the game draws through SDL3's GPU API (Direct3D 12 / Vulkan on Windows, Vulkan on Linux,
  Metal on macOS). Vulkan is tested (Mesa's software driver, the test suite's pictures) and Metal runs on Apple Silicon; Direct3D 12 is
  built but not yet run on real hardware - report anything that looks different from Vulkan. Cards without Direct3D 12 or
  Vulkan (older than the GeForce GTX 600 / Radeon HD 7700 / Intel Haswell) can't run the normal builds: they need the
  legacy build.
- **Legacy build (Windows XP, OpenGL 1.2)** - not yet tested on real Windows XP hardware or a real OpenGL 1.x driver: it is
  built and its imports checked against XP SP3, and its pictures are compared with the normal renderer's on Mesa's
  software OpenGL only. Its renderer approximates a few per-pixel details (docs/RENDERER_GL12.md): a point light's
  direction per corner, polygons cut by the near plane in the DOS look, no multisampling; an off-screen picture larger than the window (`render=1024x768` in a smaller
  window) is cut to the window. Report anything that looks wrong, with the card and the `gpu: OpenGL (...)` line.
- **Roland SC-55 menu music (v1.1.3)** - built and its path tested with stub ROM files only: no SC-55 firmware was
  available, so the emulated unit's actual sound, its boot time (the port waits 1 s of emulated time, then sends a GS
  reset) and its CPU use with the real firmware are untested. Report the start-up line (`menu music Roland ...`), how
  the music sounds and whether it stutters, with your CPU.
- **macOS downloads** - the app isn't notarised: run `xattr -dr com.apple.quarantine "MechWarrior 2.app"` once (README).

