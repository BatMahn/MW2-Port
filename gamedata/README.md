# gamedata/3d

The data files from the 3D-accelerator editions of MechWarrior 2, which the port uses for its textured looks.

## Why they're here

In 1996-97 MechWarrior 2 was re-released in versions written for the new 3D graphics cards: 3Dfx Voodoo, ATi Rage,
S3 ViRGE, PowerVR and Matrox Mystique. Most were only bundled with the cards themselves, never sold on their own, and
those discs are hard to find today. Without these files the port can only show the original DOS models, untextured. So
the files are included here rather than leaving everyone to hunt them down.

They are Activision's, not part of the port, and are here only so owners of the game can rebuild a working setup. I may
remove them if there's a copyright complaint.

## What's in it

| File | From |
|---|---|
| `models.prj` | the 3Dfx edition's MW2.PRJ (models) |
| `textures.prj` | the ATi edition's MW2.PRJ (textures) |
| `skygnd.par` | the 3Dfx edition's sky / ground settings |
| `ati/` | the ATi edition's settings |
| `s3/`, `pvr/`, `mga/` | the S3, PowerVR and Matrox editions' archives and settings (PowerVR also supplies the fog) |

## Use

Copy the folder into your game folder as `3d/`:

    cp -R gamedata/3d /path/to/MW2-game/3d

See the main README, "Setting up the game folder", for the rest of the game files you need from your own copy.
