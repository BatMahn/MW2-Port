# Game folder setup - what it should look like

This is a real, working game folder (the author's), listed from top to bottom. If yours looks like this, the port
will find everything. Folder names are lower-case and sit **directly** inside `MW2-game/`.

**Only `3d/` comes from this repository** (`gamedata/3d/`), because the 3D-card editions are hard to find now.
**Everything else you supply from your own copies:** your original DOS CD and install (`cd/`, `install/`), your MT-32
ROMs, your Roland SC-55 ROM dumps, your CD audio rip, your Titanium Edition disc and your Gravis UltraSound package. None
of those are provided here.

```
MW2-game/
├── 3d/                      PROVIDED - copy of gamedata/3d/ from this repository
│   ├── ati/
│   ├── mga/
│   ├── pvr/
│   ├── s3/
│   ├── models.prj
│   ├── skygnd.par
│   └── textures.prj
│
├── cd/                      YOUR COPY - the ORIGINAL 1995 DOS CD, copied as-is (folders and all)
│   ├── CFGS/
│   ├── DEMODATA/
│   ├── GIDDI/
│   ├── KEATING/             training voices
│   ├── LAUNCH/
│   ├── MECH2/               the game data (MW2.PRJ, DATABASE.MW2, ...)
│   ├── SMK/                 movies
│   ├── SND/
│   ├── VFX/
│   ├── BINMOD.EXE  CPU.EXE  ENGLISH.LRM  FIXBOOT.EXE  INSTALL.EXE  INWIN.EXE
│   ├── MW2REG.EXE  OLD_HERC.DRV  README.TXT  REGISTER.BAT  SETSOUND.EXE  VESACHK.EXE
│
├── install/                 YOUR COPY - the CONTENTS of C:\MECH2 after running the game's INSTALL
│   ├── MEK/
│   ├── SETUP/
│   ├── SND/
│   ├── EN01STAR.BWD ... EN05STAR.BWD   USERSTAR.BWD
│   ├── MW2.ADV  MW2.PHX
│   ├── MW2CAR.CFG  MW2DIF.CFG  MW2MSN.CFG  MW2PRM.CFG  MW2REG.CFG  MW2SND.CFG  STAR0.CFG
│   └── SYMLOG.TXT  TMP.OUT
│
├── mt32/                    YOUR COPY - Roland MT-32 ROMs (menu music)
│   ├── MT32_CONTROL.ROM
│   └── MT32_PCM.ROM
│
├── sc55/                    YOUR COPY - optional, Roland SC-55 ROM dumps (menu music on an emulated Sound Canvas;
│   ├── rom1.bin             not in the Windows legacy build). Names as Nuked SC-55 uses them - an SC-55mk2 v1.01 here
│   ├── rom2.bin
│   ├── rom_sm.bin
│   ├── waverom1.bin
│   └── waverom2.bin
│
├── music/                   YOUR COPY - CD soundtrack ripped from your disc, optional (FLAC, MP3 or WAV)
│   ├── Track02.mp3
│   ├── Track03.mp3
│   │   ...
│   └── Track27.mp3          (26 files - there is no Track01, that's the data track)
│
├── titanium/                YOUR COPY - optional, 3 files from the Titanium Edition disc's Install\16bit\
│   ├── MW2.PRJ
│   ├── SKYGND.PAR
│   └── particle.PID
│
└── ultrasnd/                YOUR COPY - optional, Gravis UltraSound package (menu music without the MT-32 ROMs)
    ├── MIDI/                ULTRAMID.INI + ~280 .PAT files - this is the part that matters
    └── ARROW.SND  EMUSET.EXE  GRAVUTIL.EXE  GUSEMU.EXE  MEGAEM.EXE ... (rest of the package, harmless)
```

(The author's folder also has a `mw2.env` and a `README.txt` at the top level - personal notes, not needed.)

## Minimum to play

Only `install/` and `cd/` are required. Add `3d/` for the textured renderers, `mt32/` (or `sc55/`, or `ultrasnd/`) for
menu music, `music/` for the CD soundtrack and `titanium/` for the Titanium textures.

## The SC-55 ROM files (sc55/)

The emulator is Nuked SC-55; the files go straight into `sc55/` under the names it uses (upper or lower case both work).
The model is recognised from the names - put one set in the folder. The Roland part numbers are the chips' markings, as
Nuked SC-55's README lists them:

| Model | Files (Roland part) |
|---|---|
| SC-55mk2 / SC-155mk2 (v1.01) | `rom1.bin` (R15199858, H8/532 MCU), `rom2.bin` (R00233567, H8/532 extra code), `rom_sm.bin` (R15199880, M37450M2 MCU), `waverom1.bin` (R15209359, wave 16M), `waverom2.bin` (R15279813, wave 8M) |
| SC-55st (v1.01) | `rom1.bin` (R15199858), `rom2_st.bin` (R00561413), `rom_sm.bin` (R15199880), `waverom1.bin` (R15209359), `waverom2.bin` (R15279813) |
| SC-55 (mk1) v1.00 / v1.21 / v2.0 | `sc55_rom1.bin` (R15199748 / R15199778 / R15199799, H8/532 MCU), `sc55_rom2.bin` (R1544925800 / R15209363 / R15209387, extra code), `sc55_waverom1.bin` (R15209276, wave A), `sc55_waverom2.bin` (R15209277, wave B), `sc55_waverom3.bin` (R15209281, wave C) - no sub-MCU ROM |
| CM-300 / SCC-1 (v1.10 / v1.20), SCC-1A | `cm300_rom1.bin`, `cm300_rom2.bin`, `cm300_waverom1.bin` ... `cm300_waverom3.bin` |
| SCB-55 / RLP-3194 | `scb55_rom1.bin`, `scb55_rom2.bin`, `scb55_waverom1.bin`, `scb55_waverom2.bin` |
| RLP-3237 | `rlp3237_rom1.bin`, `rlp3237_rom2.bin`, `rlp3237_waverom1.bin` |
| SC-155 | `sc155_rom1.bin`, `sc155_rom2.bin`, `sc155_waverom1.bin` ... `sc155_waverom3.bin` |

(Nuked SC-55's README has the part numbers for every model and firmware version. The JV-880 set it also supports is
not used: the JV-880 is not a General MIDI module.)

## Common mistakes

| Wrong | Right |
|---|---|
| `MW2-game/MW2-game/install/...` (folder nested inside itself, e.g. from unzipping) | `MW2-game/install/...` |
| `install/MECH2/MW2REG.CFG` (copied the MECH2 folder *into* install) | `install/MW2REG.CFG` - copy what's *inside* C:\MECH2 |
| `cd/MW2.PRJ` (flattened the disc / copied only the MECH2 folder's contents) | `cd/MECH2/MW2.PRJ` - copy the whole disc as it is |
| Titanium disc copied into `cd/` | Titanium goes in `titanium/`, and only those 3 files. `cd/` is the 1995 DOS disc |
| Titanium files from `Install\8bit\` | Use `Install\16bit\` |
| `3d/3d/models.prj` (ran `cp -R gamedata/3d MW2-game/3d` when `3d/` already existed) | `3d/models.prj` |
| `Music/`, `CD/`, `Install/` | use the folder names exactly as shown: `music/`, `cd/`, `install/` |
| `music/01 - Title.mp3` with the music starting at 01 | Name from `Track02`, or set `MW2_CDA_TRACK_OFFSET=1` |

## Check it

From inside your game folder:

    ls                     # should show: 3d  cd  install  mt32  music  sc55  titanium  ultrasnd   (the ones you have)
    ls cd/MECH2/MW2.PRJ    # must exist
    ls install/MW2REG.CFG  # must exist

Then run `./mw2 /path/to/MW2-game` once (Windows: drag the folder onto `mw2.exe`); the port remembers the folder after that.

## Where the game looks for it

The game data is never inside the program or the macOS app: the port uses your own `MW2-game` folder. Without a folder
on the command line it uses, in order: the folder remembered in `mw2port.cfg` (`game=`), the current folder, the folder
next to the program (the macOS app: next to `MechWarrior 2.app`), `MW2-game` one level up, and `~/MW2-game`. If none of
those is a game folder (or the remembered one is gone), it asks for it with the system's folder picker - choose
`MW2-game` itself or the folder that holds it; a folder without the game is explained and asked again. The choice is
remembered.

- **macOS:** on first launch the app asks for your game folder (unless `MW2-game` sits next to the app or in your home
  folder). Move the app out of Downloads (into Applications or a folder of your own) with
  Finder first: opened where it was downloaded, macOS runs it from a temporary copy that cannot see a folder next to it
  (it tells you and asks for the game folder, which is remembered).
- **Windows:** `mw2.exe` asks with the folder browser, or drag the folder onto `mw2.exe`.
- **Linux:** the folder picker needs `zenity` or `kdialog`; without either, the game explains where to put the folder
  (or run `./mw2 /path/to/MW2-game` once).
