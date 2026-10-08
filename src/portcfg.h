/* The port's configuration: one per-user file,
 *   Linux   $XDG_CONFIG_HOME/mw2port/mw2port.cfg  (~/.config/mw2port/mw2port.cfg)
 *   macOS   ~/Library/Application Support/mw2port/mw2port.cfg
 * plain "key=value" lines:
 *   game=<folder>          the game folder: install/ (settings, pilots, saves), cd/ (the disc's files) or an .iso,
 *                          music/ (CD tracks), ultrasnd/ (Gravis patches), 3d/ (3D-edition models, textures, sky)
 *   install= cd= music= ultrasnd= mt32= models= textures= skygnd=     optional overrides of the folder's parts
 *                          (mt32/: the MT-32 ROMs MT32_CONTROL.ROM + MT32_PCM.ROM for the menu music)
 *   textures_kind=ati | 3dfx
 *   edition=enhanced | dos | 3dfx | ati | s3 | pvr | mga   (the look of the simulation: the port's enhanced mix, or one of
 *                          the original editions - its own archive, sky file, fog and texture filtering)
 *   resolution=desktop | window | <w>x<h>     (desktop: native fullscreen, the default)
 *   aa=0 | 2 | 4 | 8
 *   render=native | 320x200 | 640x480 | 1024x768   (the sim's picture: the display's resolution, or one of the
 *                          original's RESOLUTION choices, drawn at that size and scaled up in blocks to 4:3)
 * The game folder is found on first run (the command line, the current folder, next to the executable, ~/MW2-game)
 * and remembered. */
#ifndef PORTCFG_H
#define PORTCFG_H
#define PORTCFG_PATH 1024
typedef struct {
    int mode;        /* 0 desktop fullscreen, 1 windowed, 2 exclusive fullscreen at w x h */
    int w, h;
    int aa;
    int render_w, render_h;   /* 0 = native */
    float speed;              /* game speed: 1.0 = real time (speed= in the file; default 0.85) */
    char game[PORTCFG_PATH], install[PORTCFG_PATH], cd[PORTCFG_PATH], music[PORTCFG_PATH], ultrasnd[PORTCFG_PATH], mt32[PORTCFG_PATH];
    char models[PORTCFG_PATH], textures[PORTCFG_PATH], skygnd[PORTCFG_PATH];
    char skygnd_fog[PORTCFG_PATH];   /* the PowerVR edition's skygnd.par (the fog densities): 3d/skygnd_pvr.par */
    int  textures_ati;
    int  edition;             /* PORTCFG_ED_*: the look of the simulation */
} portcfg;
enum { PORTCFG_ED_ENHANCED, PORTCFG_ED_DOS, PORTCFG_ED_3DFX, PORTCFG_ED_ATI, PORTCFG_ED_S3, PORTCFG_ED_PVR, PORTCFG_ED_MGA, PORTCFG_ED_COUNT };
extern const char *const PORTCFG_ED_NAME[PORTCFG_ED_COUNT];   /* "enhanced", "dos", ... (the file's words) */
/* An edition's files: the archive for models and data, the archive for textures and its kind ("ati", "3dfx", "pvr",
 * "dos"), the sky / ground file and the fog file ("" = no fog). Returns 0 if the edition's files are present (else the
 * outputs describe the enhanced mix, or the DOS archive without the 3D files). */
int  portcfg_edition(const portcfg *c, int ed, char *models, char *textures, char *kind, char *sky, char *fog);
void portcfg_defaults(portcfg *c);
const char *portcfg_file(void);                       /* the per-user file's path */
int  portcfg_load(const char *unused, portcfg *c);    /* 0 if read; defaults otherwise (the argument is ignored) */
int  portcfg_save(const char *unused, const portcfg *c);
/* Find the game folder (if not configured) and fill in every part left empty. exe_dir: the executable's folder;
 * hint: a folder from the command line, or NULL. Returns 0 if an install folder was found. */
int  portcfg_resolve(portcfg *c, const char *exe_dir, const char *hint);
/* Is this a game folder (or a plain DOS install)? */
int  portcfg_is_game_dir(const char *dir);
#endif
