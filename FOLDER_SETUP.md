# Game folder setup - what it should look like

This is a real, working game folder (the author's), listed from top to bottom. If yours looks like this, the port
will find everything. Folder names are lower-case and sit **directly** inside `MW2-game/`.

**Only `3d/` comes from this repository** (`gamedata/3d/`), because the 3D-card editions are hard to find now.
**Everything else you supply from your own copies:** your original DOS CD and install (`cd/`, `install/`), your MT-32
ROMs, your CD audio rip, your Titanium Edition disc and your Gravis UltraSound package. None of those are provided here.

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

Only `install/` and `cd/` are required. Add `3d/` for the textured renderers, `mt32/` (or `ultrasnd/`) for menu music,
`music/` for the CD soundtrack and `titanium/` for the Titanium textures.

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

    ls                     # should show: 3d  cd  install  mt32  music  titanium  ultrasnd   (the ones you have)
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
