# Known issues

Open issues in the port. Technical details and evidence are in [ASSUMPTIONS.md](ASSUMPTIONS.md) and
[the DOS parity audit](docs/AUDIT_DOS.md).

## Open
- **DOS mission parity**. The Umber Wall comparison now follows the aircraft into the hangar
  attack. Golden Spade matches its first 589 random draws and 24 debris spawns using the original
  frame schedule with audio mixing disabled. The DOS dispatcher comparison verifies overlapping
  LRM volleys through an AI target switch. Later Bronze Anvil routes and Golden Spade debris
  bursts still diverge. Natural warning handoffs in stock missions still need a paired capture.
  [DOS parity audit](docs/AUDIT_DOS.md)
- **Titanium simulation**. The 8-bit and 16-bit editions use their original combat tables. All 54
  single-player scenes pass the objective sweep with each disc archive. Isolated firing routines
  match both DLLs. Full mission comparisons in the original Windows game and live NetMech
  sessions still need verification. Apache and Cali pass with local AI opponents using the
  configured Titanium game data.
- **Classic Mac build (ALPHA)** - new, and has had almost no testing on a real PowerPC Mac: crashes, sound / music
  (QuickTime's synthesizer is untested) and display problems are expected. docs/CARBON.md lists what still needs
  checking on real hardware.
- **3Dfx and S3 looks**. Equal-depth training flags now use the visible polygon sort order. Paired native
  captures are needed to confirm the first texture upload across different approaches.
- **Rendition look**. The GPU renderer uses an RGB565 frame when the device supports it. Antialiased lines,
  hardware dithering, and 8-bit fog precision remain open.

## Other
- **Graphics interfaces** - the game draws through SDL3's GPU API (Direct3D 12 / Vulkan on Windows, Vulkan on Linux,
  Metal on macOS). Vulkan is tested (Mesa's software driver, the test suite's pictures) and Metal runs on Apple Silicon; Direct3D 12 is
  built but not yet run on real hardware - report anything that looks different from Vulkan. Cards without Direct3D 12 or
  Vulkan (older than the GeForce GTX 600 / Radeon HD 7700 / Intel Haswell) can't run the normal builds: they need the
  legacy build.
- **Legacy build (Windows XP, OpenGL 1.2)** - not yet tested on real Windows XP hardware or a real OpenGL 1.x driver: it is
  built and its imports checked against XP SP3, and its pictures are compared with the normal renderer's on Mesa's
  software OpenGL only. Its renderer approximates a few per-pixel details (docs/RENDERER_GL12.md): a point light's
  direction per corner and polygons cut by the near plane in the DOS look. It runs without multisampling.
- **Roland SC-55 menu music (v1.1.3)** - built and its path tested with stub ROM files only: no SC-55 firmware was
  available, so the emulated unit's actual sound, its boot time (the port waits 1 s of emulated time, then sends a GS
  reset) and its CPU use with the real firmware are untested. Report the start-up line (`menu music Roland ...`), how
  the music sounds and whether it stutters, with your CPU.
- **macOS downloads** - the app isn't notarised: run `xattr -dr com.apple.quarantine "MechWarrior 2.app"` once (README).

