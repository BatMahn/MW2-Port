# Third-party components

| Component | Use | Licence |
|-----------|-----|---------|
| miniaudio (third_party/miniaudio.*) | decoding the CD soundtrack (FLAC / MP3 / WAV) | public domain (Unlicense) or MIT-0 |
| stb_truetype (src/third_party/stb_truetype.h) | rendering the port's own settings text | public domain or MIT |
| Liberation Sans Bold (assets/fonts) | the port's own settings text, matched to the original artwork | SIL Open Font License 1.1 |
| SDL2 (system library) | window, input, audio output | zlib |

MechWarrior 2's own data (archives, art, music, text) is not part of the port: it is read from your copy of the game.

## mt32emu (Munt)

`third_party/mt32emu/` - the Roland MT-32 emulation library from the Munt project (https://github.com/munt/munt),
copyright Dean Beeler, Jerome Fisher, Sergey V. Mikayev and contributors, GNU Lesser General Public License 2.1 or later
(`third_party/mt32emu/COPYING.LESSER.txt`). Built as a static library for the menu music. The MT-32 ROMs are not
included: put your own MT32_CONTROL.ROM and MT32_PCM.ROM in the game folder's `mt32/` (or set `mt32=`).
