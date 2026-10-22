/* The port's configuration: one per-user file,
 *   Linux   $XDG_CONFIG_HOME/mw2port/mw2port.cfg  (~/.config/mw2port/mw2port.cfg)
 *   macOS   ~/Library/Application Support/mw2port/mw2port.cfg
 * plain "key=value" lines:
 *   game=<folder>          the game folder: install/ (settings, pilots, saves), cd/ (the disc's files) or an .iso,
 *                          music/ (CD tracks), ultrasnd/ (Gravis patches), 3d/ (3D-edition models, textures, sky)
 *   install= cd= music= ultrasnd= mt32= models= textures= skygnd=     optional overrides of the folder's parts
 *                          (mt32/: the MT-32 ROMs MT32_CONTROL.ROM + MT32_PCM.ROM for the menu music)
 *   textures_kind=ati | 3dfx
 *   renderer=enhanced | dos | 3dfx | ati | s3 | pvr | mga | rendition   (how the simulation is drawn: the port's enhanced
 *                          mix, or one of the original editions - its own archive, sky file, fog and texture filtering;
 *                          rendition: the Titanium edition's Rendition Verite plugin, zredline.dll - Titanium textures only)
 *   textures=original | titanium   (original: the renderer's own textures, sky and ground; titanium: the Titanium
 *                          edition's 16-bit textures, sky dome and ground with the renderer's states - never with dos,
 *                          always with rendition)
 *                          (the old edition=<renderer> is still read; edition=titanium = renderer=enhanced textures=titanium;
 *                          textures= with any other value is the texture archive's path, as below)
 *   titanium=<folder>      the Titanium edition's 16-bit files (MW2.PRJ, SKYGND.PAR from the disc's Install/16bit);
 *                          default: the game folder's titanium/
 *   resolution=desktop | window | <w>x<h>     (desktop: native fullscreen, the default)
 *   aa=0 | 2 | 4 | 8
 *   render=native | 320x200 | 640x480 | 1024x768   (the sim's picture: the display's resolution, or one of the
 *                          original's RESOLUTION choices, drawn at that size and scaled up in blocks to 4:3)
 *   hud_layout=wide | original   (wide, the default: on a window wider than 4:3 the HUD's left-hand instruments - radar,
 *                          altitude tape, target display - sit at the window's left edge and the right-hand ones - weapon
 *                          list, damage display / viewport, throttle bar, speed - at its right edge, each as far from
 *                          the edge as from the 4:3 area's edge; the compass, reticle, markers, bars and messages stay
 *                          centred. original: the whole HUD in the 4:3 middle, as the original drew it)
 *   dos_shading=dithered | smooth   (the DOS renderer's Gouraud shading - the shaded polygons (cockpit, mechs, buildings)
 *                          and the sky's haze band: dithered, the default, as the original drew them - each palette step a
 *                          flat band and a 1-pixel checkerboard of it and the next (MW2.EXE 0x59e77; the haze band in the
 *                          sky palette's 15 colours 0xE0-0xEE, 0x3c000); smooth: the same colours blended into an even
 *                          gradient. dos_sky=banded | smooth, its older name, is still read. The other renderers are not
 *                          affected)
 *   system_shortcuts=block | allow   (macOS: block, the default, turns the system's keyboard shortcuts - Mission
 *                          Control's Ctrl+arrows, Cmd+Tab, Spotlight - off while a mission is played with the window in
 *                          front (not in the MAIN MENU or paused); allow leaves them on. Other systems: no effect)
 * The game folder is found on first run (the command line, the current folder, next to the executable - in a macOS
 * bundle: next to the .app -, one level up, ~/MW2-game; else the shell asks with the system's folder picker) and
 * remembered. */
#ifndef PORTCFG_H
#define PORTCFG_H
#include <stddef.h>
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
    char titanium[PORTCFG_PATH];     /* the Titanium edition's folder (MW2.PRJ + SKYGND.PAR of its 16-bit build) */
    int  textures_ati;
    int  renderer;            /* PORTCFG_ED_*: how the simulation is drawn (Combat Variables RENDERER) */
    int  texset;              /* PORTCFG_TX_*: the texture set (Combat Variables TEXTURES) */
    int  system_shortcuts;    /* 0 block (default): the system's shortcuts off while playing (macOS); 1 allow */
    int  hud_original;        /* hud_layout=: 0 wide (default), 1 original (the 4:3 layout centred) */
    int  dos_sky_smooth;      /* dos_shading= (dos_sky=): 0 dithered (default, the original), 1 smooth (the DOS renderer's
                               * Gouraud polygons and haze band blended) */
} portcfg;
/* RENDITION: the Titanium edition's own renderer plugin for the Rendition Verite (plugin/zredline.dll, loaded by mercsw.dll):
 * the enhanced mix's archives beside the Titanium archive, whose textures, sky dome, ground and fog it always draws */
enum { PORTCFG_ED_ENHANCED, PORTCFG_ED_DOS, PORTCFG_ED_3DFX, PORTCFG_ED_ATI, PORTCFG_ED_S3, PORTCFG_ED_PVR, PORTCFG_ED_MGA,
       PORTCFG_ED_RENDITION, PORTCFG_ED_COUNT };
extern const char *const PORTCFG_ED_NAME[PORTCFG_ED_COUNT];   /* "enhanced", "dos", ... (the file's words) */
enum { PORTCFG_TX_ORIGINAL, PORTCFG_TX_TITANIUM, PORTCFG_TX_COUNT };
extern const char *const PORTCFG_TX_NAME[PORTCFG_TX_COUNT];   /* "original", "titanium" */
/* An edition's files: the archive for models and data, the archive for textures and its kind ("ati", "3dfx", "pvr",
 * "dos"), the sky / ground file and the fog file ("" = no fog). Returns 0 if the edition's files are present (else the
 * outputs describe the enhanced mix, or the DOS archive without the 3D files). */
int  portcfg_edition(const portcfg *c, int ed, char *models, char *textures, char *kind, char *sky, char *fog);
/* The Titanium edition's files: its archive and its SKYGND.PAR (either may be NULL). Returns 0 if the archive is there.
 * (TEXTURES TITANIUM keeps the renderer's archives - its models and data, its textures for the names the Titanium
 * archive lacks - which portcfg_edition reports; the Titanium archive is opened beside them.) */
int  portcfg_titanium(const portcfg *c, char *prj, char *sky);
/* Can this texture set go with this renderer? ORIGINAL always but with RENDITION (a Titanium plugin: Titanium textures
 * only); TITANIUM not with the DOS renderer (palette-indexed textures) and only with the Titanium folder there. */
int  portcfg_texset_ok(const portcfg *c, int renderer, int texset);
/* The texture set this renderer draws with for a chosen one: TITANIUM with RENDITION, ORIGINAL when TITANIUM can't be had */
int  portcfg_texset_for(const portcfg *c, int renderer, int texset);
/* A renderer + texture set's files (portcfg_edition, then with TITANIUM the Titanium SKYGND.PAR as the sky file and no
 * fog file - the Titanium fog comes from its own SKYGND.PAR). Returns 0 if both are available. */
int  portcfg_look(const portcfg *c, int renderer, int texset, char *models, char *textures, char *kind, char *sky, char *fog);
void portcfg_defaults(portcfg *c);
const char *portcfg_file(void);                       /* the per-user file's path */
int  portcfg_load(const char *unused, portcfg *c);    /* 0 if read; defaults otherwise (the argument is ignored) */
int  portcfg_save(const char *unused, const portcfg *c);
/* Find the game folder (if not configured) and fill in every part left empty. exe_dir: the executable's folder;
 * hint: a folder from the command line, or NULL. Returns 0 if an install folder was found. */
int  portcfg_resolve(portcfg *c, const char *exe_dir, const char *hint);
/* Is this a game folder (or a plain DOS install)? */
int  portcfg_is_game_dir(const char *dir);
/* A chosen folder: the game folder itself, or a folder holding MW2-game/. Returns 1 with the game folder's absolute
 * path in out, 0 if neither. */
int  portcfg_game_in(const char *dir, char *out, size_t n);
/* exe_dir inside a macOS bundle (".../X.app/Contents/..."): 1 with the folder holding X.app in out, else 0 */
int  portcfg_app_folder(const char *exe_dir, char *out, size_t n);
/* exe_dir is a macOS App Translocation copy (a quarantined download run in place: a random read-only path) */
int  portcfg_translocated(const char *exe_dir);
#endif
