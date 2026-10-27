/* glview - interactive viewer for the enhanced (3D-edition) look. SDL2 + OpenGL 3.3 core.
 *   glview MODELS.PRJ TEXTURES.PRJ ati|3dfx [RECORD] [MISSION]
 *   RECORD "@" = walk/fly the mission's world (W toggles world <-> mech view)
 *   World view: WASD move, mouse drag look, Q/E down/up, Shift faster
 *
 * Mouse drag: orbit    Wheel: zoom     F / Alt+Enter: fullscreen (desktop resolution)
 * Tab: next mech       N: next mission (its textures and camo set)
 * C: next camo (0-7)   I: next clan insignia
 * B: bilinear on/off   M: anti-aliasing on/off    Space: walk animation on/off
 * World view: P runs the mission: its logic (objectives, triggers) and the groups' orders, with
 * the camera standing in for the player (fly to a nav point to trigger it). Log on stdout.
 * You pilot a Timber Wolf from the mission start: W/S throttle, X stop, 1..9 / 0 throttle presets
 * (engine: 10 steps STOP, 2..9, FULL), A/D turn the legs, T = feet to torso, J (hold) = jump jets,
 * O = override a shutdown sequence,
 * mouse = torso twist and view pitch, F or left mouse (hold) = fire at the enemy nearest
 * the crosshair (15 deg cone, in range), C = chase / cockpit view, G = free camera.
 * Alt+Enter: fullscreen.
 * Testing: MW2_AUTOPILOT="seconds,throttle,twist,fire,out.ppm[,cockpit]" holds those
 * controls (simulated in 1/30 s steps), writes the last frame and exits.
 * [ ]: fog density -/+ (0 = off)       Ctrl+W: world <-> mech view      Esc: quit (in a mission: the MAIN MENU)
 * Sky/ground/fog: MW2_SKYGND=3dfx skygnd.par, MW2_SKYGND_FOG=PowerVR skygnd.par
 * Any window size or aspect ratio; wider screens see more to the sides.
 */
#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#include <dlfcn.h>   /* the system-shortcut switch, looked up at run time (syskeys_apply) */
#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void *)-2)   /* <dlfcn.h>'s value, hidden under strict POSIX */
#endif
#elif defined(_WIN32)
#include "glload.h"   /* GL 2.0+ looked up at runtime (opengl32.dll exports only 1.1) */
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#endif
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common3d.h"
#include "datapath.h"
#include "inputmap.h"
#include "inputmap_defaults.h"
#include "msim.h"
#include "fx.h"
#include "glr.h"
#include "hud.h"
#include "portcfg.h"
#include "shp.h"
#include "bwd.h"
#include "cdaudio.h"
#include "sfx.h"
#include "wtable.h"

static const char *MECHS[] = {"TIMBRWLF", "DIREWOLF", "MADDOG", "SUMMONER", "KITFOX", "WARHAWK", "FIREMOTH",
                              "NOVA", "STRMCROW", "HELLBRGR", "GARGOYLE", "JENNER", "RIFLEMAN", "WARHAMMR", "MARAUDER"};
static const char *MISSIONS[] = {"CYAN", "PINK", "GOLD", "GREE", "TAN_", "WHIT", "RED_", "PLUM", "BRON", "FUCH",
                                 "MARO", "PUCE", "RUST", "UMBE", "YELL", "BLON"};
#define COUNT(a) (int)(sizeof a / sizeof a[0])

static glr_view *g_view;

static int       g_world, g_camo, g_clan;
static inputmap   g_im;   /* GAMEKEY.MAP + INPUT.MAP from the game directory (the original's controls) */
static float      g_hud_s = 1.0f;       /* the set's pixel scale against the 6 set (the 320x200 set: 0.5) */
static const char *g_hud_sfx = "6";   /* the HUD sprite set: "" 320x200, "6" 640x480, "K" 1024x768 */
static int        g_win_lines = 480;     /* the window's drawable height (the native set choice) */
static int        g_win_cols = 640;      /* and its width */
static float      g_hp[256][4];          /* the HUD palette's colours (hud_mission_palette) */
#define HP(i) ((const float *)g_hp[(i) & 0xff])
#define HUD_BEGIN(hd, w, h) (g_render_w == 320 && g_lr_fbo[0] ? hud_begin_stretched(hd, w, h) : hud_begin(hd, w, h))
/* the port's HUD layout (mw2port.cfg hud_layout=, the GRAPHICS page's HUD layout; not the original's - it drew the 4:3
 * layout only): wide (1, the default) puts the left-hand instruments at the window's left edge and the right-hand ones
 * at its right edge when the window is wider than 4:3 (hud_side); original (0) keeps the whole HUD in the 4:3 middle */
static int        g_hud_wide = 1;
/* the port's DOS shading (mw2port.cfg dos_shading=, the GRAPHICS page's DOS shading in a DOS mission; not the original's):
 * 0 dithered (the default: the Gouraud polygons and the sky's haze band stepped and dithered as MW2.EXE 0x59e77 drew
 * them), 1 smooth (their colours blended) */
static int        g_dos_sky_smooth;
#define HUD_SIDE(s) hud_side(g_hud, g_hud_wide ? (s) : 0)
/* the same move in window pixels for the 3D pictures drawn into HUD windows (w x h the 3D frame): the side margin */
static int hud_side_px(int w, int h, int side)
{
    int m = (w - h * 4 / 3) / 2;
    return g_hud_wide && m > 0 ? side * m : 0;
}
static const char *HN(const char *base) { static char b[8][16]; static int k; k = (k + 1) & 7; snprintf(b[k], 16, "%s%s", base, g_hud_sfx); return b[k]; }
static int        g_cockpit_frame;   /* the cockpit frame (the eye's hull piece): off until its rule is solved (ASSUMPTIONS
                                      * "Cockpit frames"); MW2_COCKPIT_FRAME=1 shows the current attempt */
/* the key ramp (engine 0x10039ee0, DOS 0x486e0), run per frame at the agreed 30 fps reference: a held _plus key sets a
 * negative velocity to 0, adds (frame ticks x 3) << shift (16.16; 182 / 30 ticks a frame) capped at 0x8000, then adds the
 * velocity to the value (capped at 0x10000 = 1); _minus the same downwards (applied after _plus); with neither held the
 * velocity is zeroed. The shift is the control table's +0x18 (0x10256540, copied to the sink's +0x1e by 0x10039140):
 * legs_pan_delta 7, pilot_pan / pilot_tilt 5, zoom_factor 6 (throttle 6, torso_pan 3, torso_tilt 4, eyepoint 4). A
 * "_delta" control's velocity is its value (0x10039140 points +4 at +0x1a): each frame value = 2 x min(value + step,
 * 0.5), and it drops to 0 with the keys up - legs_pan_delta reaches full turn on the 4th frame (0.13 s), so a 0.2 s
 * tap turns ~9 degrees and 0.35 s ~22 (DOSBox: 7-9 / 21). pilot_pan / tilt reach full after ~0.5 s and keep their value
 * (DOS: 0.15 s a few degrees, 0.6 s well round); the zoom 0.12 s 1.00 -> 1.17 (DOS 1.17) */
typedef struct { float vel, val, acc; int on; } key_ramp_t;
static void key_ramp(key_ramp_t *r, int plus, int minus, int shift, int delta, float dt)
{
    const float step = 3.0f * (182.0f / 30.0f) * (float)(1 << shift) / 65536.0f;
    /* the engine steps on every frame a key is seen held - the first one too - and zeroes the velocity (a _delta
     * control: its value) on the first frame none is: the 30 fps steps start on the press and stop on the release
     * (a free-running 30 fps clock gave a press up to 1/30 s late, a short tap between two ticks no step at all, and
     * a _delta control kept turning up to 1/30 s after the release) */
    if (dt <= 0) return;   /* paused: the controls are off */
    if (!plus && !minus) {
        if (delta) r->val = 0;
        r->vel = 0; r->acc = 0; r->on = 0;
        return;
    }
    if (!r->on) r->acc = 1.0f / 30.0f;   /* the press: a step this frame */
    else r->acc += dt;
    r->on = 1;
    if (r->acc > 0.5f) r->acc = 0.5f;
    while (r->acc >= 1.0f / 30.0f) {
        r->acc -= 1.0f / 30.0f;
        if (delta) {
            if (plus) { if (r->val < 0) r->val = 0; r->val += step; if (r->val > 0.5f) r->val = 0.5f; r->val *= 2.0f; if (r->val > 1) r->val = 1; }
            if (minus) { if (r->val > 0) r->val = 0; r->val -= step; if (r->val < -0.5f) r->val = -0.5f; r->val *= 2.0f; if (r->val < -1) r->val = -1; }
            if (!plus && !minus) r->val = 0;
            continue;
        }
        if (plus) { if (r->vel < 0) r->vel = 0; r->vel += step; if (r->vel > 0.5f) r->vel = 0.5f; r->val += r->vel; if (r->val > 1) r->val = 1; }
        if (minus) { if (r->vel > 0) r->vel = 0; r->vel -= step; if (r->vel < -0.5f) r->vel = -0.5f; r->val += r->vel; if (r->val < -1) r->val = -1; }
        if (!plus && !minus) r->vel = 0;
    }
}
static int        g_lite_level;   /* the planet's LITE ambient level (<4 letters>PLT1, payload +0x14) */
static int        g_render_w, g_render_h;   /* MW2PORT.CFG render=: 0 native, else the original's 320x200 / 640x480 / 1024x768 */
/* the full-width bars: the set's own at the original resolutions; drawing natively between 480 and 768 lines the K
 * bars, so that they still span the wider 4:3 area at 1:1 pixels (port) */
static const char *HB(const char *base)
{
    static char b[4][16]; static int k;
    k = (k + 1) & 3;
    snprintf(b[k], 16, "%s%s", base, !g_render_w && g_win_lines > 480 ? "K" : g_hud_sfx);
    return b[k];
}
static int        g_im_ok, g_im_mouse;   /* g_im_mouse: INPUT.MAP uses the mouse (else the port reads it as MOUSE.MAP does) */
static int        g_im_menu_mouse;       /* a menu control (menu_item / menu_value ...) reads the mouse: it moves while the menu is up */
static Uint32     g_mm_timer;            /* the MAIN MENU's timer (0x102653e8): reset on opening and after a control's key */
static int        g_mm_prev_en, g_mm_prev_ab;   /* menu_enter / menu_abort last state (on the press only) */
/* a game file by its DOS path for reading, found like the control files: the install folder, then the disc (folders,
 * then an .iso image), any case of the name */
static FILE *im_open_game_file(const char *rel)
{
    const char *dir = getenv("MW2_INSTALL_DIR") ? getenv("MW2_INSTALL_DIR") : ".";
    char p[2048];
    unsigned char *data = NULL;
    size_t len = 0;
    FILE *f;
    if (dp_resolve_in(dir, rel, p, sizeof p) == 0 || dp_resolve(rel, p, sizeof p) == 0) return fopen(p, "rb");
    if (dp_read_file(rel, &data, &len) != 0) return NULL;
    if ((f = tmpfile())) { if (fwrite(data, 1, len, f) != len) { fclose(f); f = NULL; } else rewind(f); }
    free(data);
    return f;
}
static float      g_eye_pan, g_eye_tilt, g_zoom = 1.0f;   /* pilot_pan / pilot_tilt / zoom_factor (the cockpit view) */
#define A(name) (g_im_ok && im_action(&g_im, name, k, e.key.keysym.mod))
/* the port's own development keys in a mission (G free camera, [ ] fog density): only with MW2_DEBUG_KEYS=1 - in play
 * every key belongs to GAMEKEY.MAP / INPUT.MAP (user report 2026-10-23: rebinding to G dropped the piloting) */
static int dbg_keys(void) { static int v = -1; if (v < 0) v = getenv("MW2_DEBUG_KEYS") && atoi(getenv("MW2_DEBUG_KEYS")) > 0; return v; }
#define H(name) (g_im_ok && im_held(&g_im, name, ks))

static mech3d    g_mech;          /* the viewed mech, kept for posing */
static float     g_test_vhead;    /* TEST ONLY (MW2_TEST_VIEWER's third value): the viewed mech's heading */
static c3d_scene g_scene;
static anim_set  g_anim;
static int       g_have_mech, g_have_anim, g_walk = 1, g_patrol;
static float     g_kps = 6.0f;                 /* viewer mech: keys/s matched to its walk speed */
static world_actor *g_actors;
static int       g_actor_count;
static msim      g_sim;
/* GAMEKEY.MAP actions: selected target (t / r / e / f / q, Ctrl+t), nav point (n / Shift+n), radar zoom
 * (x / Shift+x), HUD (F11), damage display (F5), target display (F4), group fire (\\ ' ;) */
static int       g_tgt_thing = -1;   /* a targeted structure (index into g_sim.bld), when g_tgt_sel < 0 */
static int32_t   g_dhdt_s;           /* the dH/dT bar's smoothed value (tests: STATE dhdt=) */
/* the heat / throttle / jets bars' smoothing filters ({s, x, shift} at DOS 0x96858 / 0x96864 / 0x9687c, 3Dfx 0x1024c818 /
 * 0x1024c828 / 0x1024c848, all shift 1, s = 0 at load): s += (x - s) >> 1 on each draw (DOS 0x49cf0, 3Dfx 0x1003ba60); acc:
 * the last 1/30 s step (the agreed 30 fps for per-frame engine effects); the values for tests (STATE heatbar= thrbar=
 * jetbar=, the shown lengths in pixels) */
static int32_t   g_heat_s, g_heat_x, g_heat_acc = -1, g_heat_p, g_thr_s, g_thr_acc = -1, g_thr_p, g_jet_s, g_jet_acc = -1, g_jet_p;
static unsigned  g_frame_no, g_heat_seen, g_thr_seen, g_jet_seen;   /* frames shown; each bar's last drawn frame */
/* the filter steps due since the last draw: a first draw, or one after frames without the bar (an instrument only steps
 * its filter while it draws: 0x100185e0 skips 0x1001c350 when hidden), one step; else one per 1/30 s */
static int hud_filter_steps(int32_t *acc, unsigned *seen)
{
    int n = 0;
    const int gap = *seen + 1 != g_frame_no;
    *seen = g_frame_no;
    if (*acc < 0 || g_sim.now < *acc || gap) { *acc = g_sim.now; return 1; }
    while (g_sim.now - *acc >= 33) { n++; *acc += 33; if (g_sim.now - *acc > 1000) *acc = g_sim.now; }
    return n;
}
static int       g_bracket;          /* the HUD's target mark last drawn (tests): 0 none, 1 a mech's corners, 2 a thing's, 3 the nav
                                      * marker, 4 the off-screen X */
static const char *const RADAR_LABEL[4] = {"R: 250.0m", "R: 500.0m", "R: 1.0km", "R: 2.0km"};
static const float RADAR_CM[4] = {25000.0f, 50000.0f, 100000.0f, 200000.0f};
static int       g_tgt_sel = -1, g_nav_sel = -1, g_hud_off, g_dmg_off, g_tgtdisp_off, g_tgtdisp = 1;   /* target viewer (0x10021300): 1 wireframe, 2 shaded, 0 off */
static int       g_group_fire, g_cur_group, g_htal;   /* F6: TOGGLE_HTAL */
static int       g_pit_first = -1;   /* this frame: index of the first cockpit-piece part in the actor layer */
static mech3d    g_frame_layer;     /* this frame's actor layer, kept for the viewport pass */
static int       g_sat_i;   /* satellite map range 1000 m / 2^i full width (descriptor 4: 100000 .. 6250 cm) */
static int       g_radar_big, g_satmap;   /* F2 NEXT_RADAR_MODE (small / large centred), F3 RADAR_MAP_TOGGLE */
static unsigned char g_nav_seen[256];   /* nav points reached (engine nav flag 0x20: RRNP on the radar, autopilot skips them) */
static int       g_radar_mode = 1;        /* engine 0x10003fb0: 1 small -> 2 large -> 0 off -> 1 */
/* the radar range per radar mode (index into RADAR_CM): each descriptor keeps its own (0x1024a760[mode] +0x18: small 1 and
 * large 2 both {range 200000, reset 200000, min 50000, max 400000} - the label shows half: 1.0 km), all reset once at the
 * mission's load (0x10003380 -> 0x10003250 -> 0x10004040(0)); x / Shift+x act on the shown one only (0x10004040; DOS
 * 0x12410 the same) - nothing with the radar off (descriptor 0 none). DOSBox YELLSCN1: x 500 m, F2 large "R: 1.0km", F2
 * off, x, F2 small still 500 m */
static int       g_radar_ri[3] = {2, 2, 2};
static int       g_reset_inputs;   /* RESET_INPUTS (/): the reset sinks set for the next input pass (action 1) */
#define g_radar_i g_radar_ri[g_radar_mode]
static hud_sprite g_hs_rgp[3], g_hs_rtgp[3], g_hs_rtgt[3], g_hs_rtof[3], g_hs_ruser, g_hs_rnp[2], g_hs_rtnp[2];   /* radar sprites, side F / E / N */
static int       g_autopilot;   /* a: TOGGLE_AUTOPILOT (engine 0x10015c00) */
static float     g_ap_turn;   /* the autopilot's turn input (+0xc, of 1024) */
static int       g_ap_avoiding;   /* +0x17e: the avoidance (0x2db80) steered last time - its throttle is undone when it lets go */
static float     g_ap_saved_thr;  /* +0x17a: the pilot's throttle saved by the autopilot while the avoidance is idle */
static int       g_wcam_live;
   /* this frame: the weapon camera has a missile to follow */   /* l INFRARED: 1 light amplification; w ENHANCED_VISION: 2 image enhancement */
static int       g_vport;   /* F7 rear / F8 down / F9 weapon viewport: 0 none, 1 rear, 2 down, 3 weapon */
static hud_sprite g_hs_vp[3];   /* VP_RER6, VP_DWN6, VP_WPN6 labels */
static int       g_sim_ok;
static int       g_results_written;
static int       g_notex_world, g_notex_actors;   /* Combat Variables: TERRAIN / OBJECT TEXTURES off */

/* MW2MSN.CFG / MW2CAR.CFG for the shell: when the mission ends, or on leaving early (the state as it stands:
 * not a success) */
static void write_results(void)
{
    char rp[1100];
    if (g_results_written || !g_sim_ok || !getenv("MW2_INSTALL_DIR")) return;
    snprintf(rp, sizeof rp, "%s/MW2MSN.CFG", getenv("MW2_INSTALL_DIR"));
    if (msim_write_results(&g_sim, rp) == 0) printf("results: %s (%s)\n", rp, msim_outcome_text(&g_sim) ? msim_outcome_text(&g_sim) : "left early");
    snprintf(rp, sizeof rp, "%s/MW2CAR.CFG", getenv("MW2_INSTALL_DIR"));
    msim_write_career(&g_sim, rp);
    g_results_written = 1;
}

/* the player's mech */
static mech3d    g_pl;
static anim_set  g_pl_anim;
static int       g_pl_ok, g_pilot = 1, g_cockpit = 1, g_pl_target = -1, g_reverse;   /* g_reverse: REVERSE_DIRECTION's flag (input +0x2f) */   /* missions start in the cockpit, as the original */
static int       g_frame_mech;
static int       g_win_w = 1280, g_win_h = 720;   /* bumped whenever the player's mech is (re)loaded */
static float     g_pl_stride = 100, g_pl_walk = 1500, g_throttle, g_pl_speed, g_pl_t, g_pitch;
static float     g_pl_frac[2];         /* the sub-cm rest (x, z) of the player's last mech-contact projection */
static float     g_pl_across;          /* cm/s: a mech contact's velocity across the heading (+ = to the right), easing off */
static int       g_pl_contact = -2;    /* the mech last touched (-2 none; < -2 landed on mech -contact - 3) */
static float     g_pl_contact_acc;     /* touching a mech or a wall: the frame time not yet run in 1/30 s moves (pl_locomote) */
static int       g_pl_contact_hold;    /* 1/30 s moves left in contact mode without a touch (pl_locomote) */
static float     g_jet_vx, g_jet_vz;   /* cm/s: the velocity beyond the walking one (g_pl_speed along the heading) - the
                                        * engine keeps one velocity vector (+0xf4 / +0xfc); the port splits it in two */
static int       g_pl_free;            /* airborne or jetting with fuel: no walking servo on the velocity (0x10019310) */
static int       g_pl_jet_fire;        /* the jets fire this frame with fuel left after the burn (0x1001a180): the walk keys hold */
static int       g_pl_anim_held;       /* the walk keys held this frame (anim_player_hold; state dump "aheld") */
static float     g_pl_free_h = 1e9f;   /* the heading at the last free frame (1e9: none) */
static int       g_pl_legs_turn;       /* this frame's leg-turn input (+0xc) is not 0 - keys, axis or the autopilot's
                                        * (0x1001a180: +0xc = the pilot's +0x10 or the autopilot's turn) */
static anim_player g_pl_ap;            /* the player's walk animation key player (engine 0x1000dec0); g_pl_t = its pose */
static int       g_pl_ap_ok;
static int       g_feet_to_torso;
static float     g_twist_dem, g_pitch_dem;   /* the torso demands c[2] / c[6] (the sinks'); g_pl.twist / g_pitch are the servos' values c[3] / c[7] */
static float     g_twist_srv = 1e9f, g_pitch_srv = 1e9f;   /* the servos' values as the servo step last left them */
static hud      *g_hud;
static int g_rcl = -1;   /* the reticle sprite chosen last frame (state dump): -1 RETICLE, else the g_hs_rcl index */
static hud_sprite g_hs_rcl[6], g_hs_rcl_dos[6], g_hs_reticle_dos, g_hs_tgt_dos[3];   /* the DOS (base set) reticle and brackets */   /* RCLINOP RCLLOCK RCLNOLK RCLPLOC RCLTGT RCLGLOC */
static hud_sprite g_hs_compass, g_hs_reticle, g_hs_damage, g_hs_dmg_lvl[4];
static hud_sprite g_hs_cmpmk[4], g_hs_altmk[2];   /* CMPMKR1-4, ALTMKR1-2 */
/* a 16.16 product rounded as the engine rounds it (bit 15 rounds up: 0x10005db0 / 0x10005ec0, DOS 0x13400 / 0x13510) */
static int fx16_round(long long v) { return (int)(v >> 16) + (int)((v >> 15) & 1); }
/* a CPIT coordinate (320x200 units) -> screen pixels as the engine converts a window: (c << 16) / 319 (or 199) at load
 * (0x10006000), then x (w - 1) (or h - 1), rounded (0x10005b70; screen 0x10244c64 / 0x10244cb0 = w - 1 / h - 1) */
static int cpit_px(int c, int n1, int div) { return fx16_round((long long)(((long long)c << 16) / div) * n1); }
/* the altitude tape's pixel offset for a height difference d (cm): the tape's height / 232 m a metre (0x1007c3f4 =
 * h x 1/232, 0x10247620; DOS DAT_000fb54c = (h << 16) / 232). 3Dfx (0x100222e0): float, truncated (__ftol); DOS
 * (0x28ed0): imul / idiv 100 then sar 16 - floored */
static int alt_tape_px(float d, int tape_h, int dos)
{
    if (dos) {
        long long sc = ((long long)tape_h << 16) / 232, v = sc * (long long)lrintf(d) / 100;
        return (int)v >> 16;
    }
    return (int)(d * ((float)tape_h * 0.004310344811528921f) * 0.01f);
}
/* an instrument window (its CPIT corners in 320x200, DOS table 0x968e0) and its reference point (16.16 fractions of
 * the window's size: 0x10005ec0 / DOS 0x13510, rounded) in screen pixels as the engine computes them for the screen the
 * HUD is drawn at (640 x 480 / 1024 x 768 / 320 x 200 render; native: the 4:3 area's pixels), plus ux / uy (HUD units a
 * pixel) to place them: positions in the engine's pixels, sprite offsets in sprite pixels. The port had kept the 640 x
 * 480 numbers in HUD units, which put the 1024 x 768 K set's references off by up to 27 px */
typedef struct { int x0, y0, x1, y1, rx, ry; float ux, uy; } hud_win;
static hud_win hud_window(int cx0, int cy0, int cx1, int cy1, int fx, int fy)
{
    hud_win w;
    int W1, H1;
    w.ux = hud_px_x(g_hud, 1.0f); w.uy = hud_px_y(g_hud, 1.0f);
    W1 = (int)lrintf(640.0f / w.ux) - 1; H1 = (int)lrintf(480.0f / w.uy) - 1;   /* the engine's w - 1, h - 1 */
    w.x0 = cpit_px(cx0, W1, 319); w.x1 = cpit_px(cx1, W1, 319); w.y0 = cpit_px(cy0, H1, 199); w.y1 = cpit_px(cy1, H1, 199);
    w.rx = w.x0 + fx16_round((long long)(w.x1 - w.x0) * fx); w.ry = w.y0 + fx16_round((long long)(w.y1 - w.y0) * fy);
    return w;
}
/* a CPIT record's window k ({x, y, w, h} in 320x200: the instruments 0-14 of DOS table 0x968e0 take their corners from
 * the record at load) as the engine's window: corners (x, y)-(x + w - 1, y + h - 1). DOSBox YELLSCN1: weapon window 3
 * (214, 15, 52, 8) x 686-850, y 58-85 at 1024 x 768 (429-531, 36-53 at 640 x 480); the target viewer (13) 32-196 x
 * 559-686 (20-122 x 349-428) */
static int g_layout[24][4];   /* the CPIT record's rectangles (hud_layout) */
static hud_win cpit_window(int k, int fx, int fy)
{
    const int *r = g_layout[k];
    return hud_window(r[0], r[1], r[0] + r[2] - 1, r[1] + r[3] - 1, fx, fy);
}
/* engine-pixel drawing in a window's pixel grid: a filled span, both ends inclusive (the engine's lines 0x10061763 /
 * DOS 0x53821 are inclusive) */
static void px_fill(const hud_win *w, int x0, int y0, int x1, int y1, const float *c)
{
    if (x1 < x0 || y1 < y0) return;
    hud_rect(g_hud, (float)x0 * w->ux, (float)y0 * w->uy, (float)(x1 - x0 + 1) * w->ux, (float)(y1 - y0 + 1) * w->uy, c);
}
/* the bars' band painters, in colour c's ramp (c - 1, c, c - 1, c - 2), coordinates relative to the window's corner.
 * Horizontal (0x1001c960 / DOS 0x31670): columns x..x + len (inclusive) when len > 0, rows y..y + h - 1 split at h / 4,
 * h / 2 and h / 2 + (h - h / 2) / 2. Vertical (0x1001c810 / DOS 0x31520): rows y - len..y when len > 0, columns
 * x..x + wd - 1 split the same way. DOSBox YELLSCN1 heat bar at 1024 x 768: rows 685-701 as 4 / 4 / 4 / 5 */
static void px_band_h(const hud_win *w, int x, int y, int len, int h, int c)
{
    const int h2 = h / 2, h4 = h2 / 2, h3 = h2 + (h - h2) / 2;
    if (len <= 0) return;
    x += w->x0; y += w->y0;
    px_fill(w, x, y, x + len, y + h4 - 1, HP(c - 1));
    px_fill(w, x, y + h4, x + len, y + h2 - 1, HP(c));
    px_fill(w, x, y + h2, x + len, y + h3 - 1, HP(c - 1));
    px_fill(w, x, y + h3, x + len, y + h - 1, HP(c - 2));
}
static void px_band_v(const hud_win *w, int x, int y, int wd, int len, int c)
{
    const int w2 = wd / 2, w4 = w2 / 2, w3 = w2 + (wd - w2) / 2;
    if (len <= 0) return;
    x += w->x0; y += w->y0;
    px_fill(w, x, y - len, x + w4 - 1, y, HP(c - 1));
    px_fill(w, x + w4, y - len, x + w2 - 1, y, HP(c));
    px_fill(w, x + w2, y - len, x + w3 - 1, y, HP(c - 1));
    px_fill(w, x + w3, y - len, x + wd - 1, y, HP(c - 2));
}
/* a line in screen pixels, both ends drawn: x or y steps one pixel a step along the longer axis, the other when the error
 * (starting at half the longer length) goes below 0 (the VFX line, 0x10061763 / DOS 0x53821: DOSBox YELLSCN1 radar cone
 * (143,138)-(229,53) at 1024 x 768 doubles row 96 at x 185 / 186) */
static void px_line(const hud_win *w, int x0, int y0, int x1, int y1, const float *c)
{
    int dx = abs(x1 - x0), dy = abs(y1 - y0), sx = x1 >= x0 ? 1 : -1, sy = y1 >= y0 ? 1 : -1, k, err;
    if (dx >= dy) {
        for (k = 0, err = dx / 2; k <= dx; k++, x0 += sx) {
            px_fill(w, x0, y0, x0, y0, c);
            err -= dy;
            if (err < 0) { y0 += sy; err += dx; }
        }
    } else {
        for (k = 0, err = dy / 2; k <= dy; k++, y0 += sy) {
            px_fill(w, x0, y0, x0, y0, c);
            err -= dx;
            if (err < 0) { x0 += sx; err += dy; }
        }
    }
}
/* an ellipse outline about (cx, cy) with radii a, b in screen pixels, clipped to the window (the VFX ellipse, 3Dfx
 * 0x1006298a / DOS 0x56868: the midpoint algorithm in two regions, four points a step) */
static void px_ellipse(const hud_win *w, int cx, int cy, int a, int b, const float *c)
{
    long long x = 0, y = b, a2 = (long long)a * a, b2 = (long long)b * b, dx = 0, dy = 2 * a2 * b, d = (a2 >> 2) + b2 - a2 * b;
    int k;
    if (a == 0 || b == 0) { px_line(w, cx - a, cy - b, cx + a, cy + b, c); return; }
#define PX4 for (k = 0; k < 4; k++) { int X = cx + (int)((k & 1) ? -x : x), Y = cy + (int)((k & 2) ? -y : y); \
                if (X >= w->x0 && X <= w->x1 && Y >= w->y0 && Y <= w->y1) px_fill(w, X, Y, X, Y, c); }
    while (dx - dy < 0) {
        PX4
        if (d >= 0) { y--; dy -= 2 * a2; d -= dy; }
        x++; dx += 2 * b2; d += dx + b2;
    }
    d += (((a2 - b2) >> 1) + (a2 - b2) - dx - dy) >> 1;
    do {
        PX4
        if (d < 0) { x++; dx += 2 * b2; d += dx; }
        y--; dy -= 2 * a2; d -= dy - a2;
    } while (y >= 0);
#undef PX4
}
/* a window's frame (0x10006450: four lines through its corners) */
static void px_frame(const hud_win *w, const float *c)
{
    px_fill(w, w->x0, w->y0, w->x1, w->y0, c); px_fill(w, w->x0, w->y1, w->x1, w->y1, c);
    px_fill(w, w->x0, w->y0, w->x0, w->y1, c); px_fill(w, w->x1, w->y0, w->x1, w->y1, c);
}
static hud_sprite g_hs_gp[3][4], g_hs_off[3];   /* TGTGP1-4 (+E / N) corner pieces, TGTOFFF / E / N (0x10022830) */
static hud_sprite g_hs_tgt[3], g_hs_tgtoff, g_hs_blip, g_hs_alt, g_hs_alttop, g_hs_msgbar, g_hs_mbar;
static hud_sprite g_hs_paused;   /* SHP PAUSED (+6 / K): resource 0x61 + the resolution set, drawn while PAUSE_GAME holds */
/* the two message lines (engine 0x10249850, 0x24 bytes each; 0x10002f20 fills them, 0x10003010 expires them):
 * line 0 an MSGBAR at the top, line 1 at the bottom; a new message takes a free line, else the lowest-priority
 * line whose priority is <= its own; each lasts its own number of ticks */
static struct { char text[96]; int32_t until; int pri; } g_msg_line[2];
static float     g_msg_t;   /* > 0 while any line shows (tests and the display-damage reset clear it) */
static void hud_message_ex(const char *m, int ticks, int pri)
{
    int k, pick = -1;
    int32_t now = g_sim.now;
    for (k = 0; k < 2 && pick < 0; k++) if (g_msg_line[k].until <= now) pick = k;
    if (pick < 0)
        for (k = 0; k < 2; k++)
            if (g_msg_line[k].pri <= pri && (pick < 0 || g_msg_line[k].pri < g_msg_line[pick].pri)) pick = k;
    if (pick < 0) return;
    snprintf(g_msg_line[pick].text, sizeof g_msg_line[pick].text, "%s", m);
    g_msg_line[pick].until = now + ticks * 1000 / 182;
    g_msg_line[pick].pri = pri;
    g_msg_t = 1;
    if (getenv("MW2_STATE_DUMP")) printf("MSG now=%d %s\n", (int)now, m);   /* TEST hook (tests/sequences) */
}
/* a message line's text (0x10002e40): in the line's window - the bar across the top (line 0) or the bottom (1), as tall as
 * the MSGBAR drawn there - at x = (0.01) of the width (0x10249854 = 0x28f, 0x10005ec0 rounded) and y = (the bar's height -
 * the font's) / 2 (C division). DOSBox YELLSCN1 l ("Light amplification engaged."): the text at (10, 17) at 1024 x 768 (bar
 * 49 rows), (6, 7) at 640 x 480 (29 rows); the port had (12, 21) / (8, 13) */
/* the DOS look's message line window (MW2.EXE 0x10f00, at the mission start): the descriptor's window (0x95530: the
 * whole screen, 0x13300) scaled about its centre to the bar's size (0x13860 -> 0x137a0: x by ((bounds >> 16) << 16) /
 * W, y by ((bounds & 0xffff) << 16) / H, the VFX shape's bounds word = (w - 1) << 16 | (h - 1); each corner's offset
 * from the centre ((x1 - x0 + 1) >> 1) x the factor, >> 16 rounded), then moved to x 0 and to the top (line 0: y - y0)
 * or down by y0 (line 1); MSGBAR is drawn at the window's corner, clipped to it. 640 x 480 (MSGBAR6 0x27f001d): x
 * 0-637, rows 0-28 (DOSBox YELLSCN1: the bar's first 29 of its 30 rows and 638 of its 640 columns, the text at y 7);
 * 1024 x 768 (MSGBARK 0x3ff0030): x 0-1022, rows 0-48. Only at the sets' own sizes (the bar as wide as the screen);
 * returns 0 otherwise (the port's stretched bar) */
static int g_tex_kind;   /* (the look: defined below) */
static int dos_msg_win(int k, int sw, int sh, int win[4])
{
    const hud_sprite *s = &g_hs_msgbar;
    int x0, y0, x1, y1, d;
    int64_t p5, p6;
    if (g_tex_kind != 3 || s->w != sw || s->h <= 0 || sw < 2 || sh < 2) return 0;
#define R16(v) ((int)(((int64_t)(v) >> 16) + (((int64_t)(v) >> 15) & 1)))
    p5 = ((int64_t)(s->w - 1) << 16) / sw; p6 = ((int64_t)(s->h - 1) << 16) / sh;
    {
        const int cx = sw >> 1, cy = sh >> 1;
        x0 = R16((0 - cx) * p5) + cx; x1 = R16((sw - 1 - cx) * p5) + cx;
        y0 = R16((0 - cy) * p6) + cy; y1 = R16((sh - 1 - cy) * p6) + cy;
    }
#undef R16
    d = x0; x0 -= d; x1 -= d;
    d = k ? y0 : -y0; y1 += d; y0 += d;
    win[0] = x0; win[1] = y0; win[2] = x1; win[3] = y1;
    return 1;
}
/* a message line's bar: the DOS look's clipped to its window (dos_msg_win), else stretched across the top / bottom */
static void msg_bar(int k, int sw, int sh)
{
    int win[4];
    if (dos_msg_win(k, sw, sh, win)) {
        const float ux = hud_px_x(g_hud, 1.0f), uy = hud_px_y(g_hud, 1.0f);
        int rh = win[3] - win[1] + 1;
        if (win[1] + rh > sh) rh = sh - win[1];
        hud_draw_region(g_hud, &g_hs_msgbar, hud_left_x(g_hud) + (float)win[0] * ux, (float)win[1] * uy, 0, 0,
                        (float)(win[2] - win[0] + 1), (float)rh);
    } else hud_draw_stretch_row(g_hud, &g_hs_msgbar, 0.0f, k);
}
static void msg_text(int k, int sw, int sh, const float *col)
{
    const hud_sprite *s = &g_hs_msgbar;
    const float n = sh > 768 ? (float)sh / 768.0f : 1.0f, ux = hud_px_x(g_hud, 1.0f), uy = hud_px_y(g_hud, 1.0f);
    const float kx = n / ux, ky = n / uy;   /* screen pixels a HUD unit (hud_begin's k / ky) */
    const int bh = s->w > 0 ? (int)lrintf((float)s->h * (float)sh / ((float)s->w * 0.75f)) : 0;   /* hud_draw_stretch_row's height */
    const int fh = (int)lrintf(hud_text_height(g_hud) * ky);
    int x = fx16_round(655LL * (sw - 1)), y = (k ? sh - bh : 0) + (bh - fh) / 2, win[4];
    if (dos_msg_win(k, sw, sh, win)) {   /* 0x10f00: (y1 - y0 + 1 - the font's height) / 2 in the line's window */
        x = win[0] + fx16_round(655LL * (win[2] - win[0]));
        y = win[1] + (win[3] - win[1] + 1 - fh) / 2;
    }
    hud_text(g_hud, hud_left_x(g_hud) + (float)x / kx, (float)y / ky, g_msg_line[k].text, col);
}
/* display damage (engine 0x1001e670): each of the 26 cockpit instruments (0x101de800..0x101de868) has a damage level
 * (+6), raised by one when a display critical picks it. Only three instruments read it (engine draws 0x10004710,
 * 0x10011720, 0x10021300; DOS 0x12a70 the same - DOSBox with every level forced to 1: only the target viewer and the
 * viewport window fuzz): 0 the radar (level > 0: light amplification off and refused, 0x10007900 / 0x10007a50; its
 * static overlay needs a window animation slot the engine never sets), 2 the bottom-right window in its viewport modes
 * (rear / down / weapon; not the damage outline or H T A L) and 13 the target viewer: level 1 fuzzes in and out, 2
 * draws normally, 3+ static for good. The static is the SNOWCLR animation (ANM2 slot 0 = mw2pit's {0, 18, 1, 0xaf}:
 * SHP 0xaf + the resolution set, 10 frames, 18 ticks each, looping from its first draw) */
#define INST_RADAR 0
#define INST_AUX   2
#define INST_TGT   13
static short     g_inst_lvl[26];
static int       g_snow_flag[2], g_snow_show[2];   /* [0] the viewport window (0x1024b444), [1] the target viewer (0x1024d05c) */
static int32_t   g_snow_t0 = -1, g_snow_step = -1;  /* the animation's first draw; the last 1/30 s roll */
static hud_sprite g_hs_snow[10];                    /* SNOWCLR(6/K) frames */
static int       g_snow_n[2], g_snow_on_n[2], g_snow_flips[2];   /* TEST counters: rolled frames, static frames, changes */
static int       g_last_display_hits, g_last_display_hits5, g_last_override;
static sfx      *g_sfx;
/* ---- mission music: the original plays the mission's CD track (MUSI -> MUS "trackNN" -> disc track NN) and
 * starts it again when it ends (DOS engine 0x43c10 / 0x429d0). Here the ripped tracks (Track02..27 as
 * FLAC/MP3/WAV) are a virtual disc (cdaudio) mixed in as the music stream. */
static cdaudio  *g_cd;
static int       g_cd_track;
static void cd_music_cb(void *u, float *out, int frames)
{
    size_t got = 0;
    (void)u;
    if (g_cd) got = cdaudio_render(g_cd, out, (size_t)frames);
    if (got < (size_t)frames) memset(out + got * 2, 0, ((size_t)frames - got) * 2 * sizeof(float));
    if (getenv("MW2_MUSIC_PROBE")) {   /* tests: the level reaching the mixer, after ~1.5 s */
        static long total;
        static double acc;
        int i;
        total += frames;
        if (total > 66150 && total <= 66150 + frames) {
            for (i = 0; i < frames * 2; i++) acc += (double)out[i] * out[i];
            fprintf(stderr, "music probe: RMS %.3f over %d frames\n", sqrt(acc / (frames * 2)), frames);
        }
    }
}
static void music_start(int track)
{
    const char *dir = getenv("MW2_MUSIC_DIR");
    portcfg pc;
    if (track <= 0) return;
    if (!dir || !dir[0]) {   /* run by hand: the configured game folder's music/ */
        char *bp = SDL_GetBasePath();
        portcfg_load(NULL, &pc);
        portcfg_resolve(&pc, bp, NULL);
        SDL_free(bp);
        dir = pc.music[0] ? pc.music : NULL;
    }
    if (!dir || !g_sfx) return;
    if (!g_cd) {
        cdaudio_options o;
        memset(&o, 0, sizeof o);
        if (cdaudio_open(&g_cd, dir, &o) != 0) { fprintf(stderr, "music: no CD tracks in %s\n", dir); g_cd = NULL; return; }
        {   /* the MUSIC slider (MW2SND.CFG +0xc, 16.16) */
            unsigned char snd[60];
            char p[1100];
            FILE *f;
            int vol = 255;
            if (getenv("MW2_INSTALL_DIR")) {
                snprintf(p, sizeof p, "%s/MW2SND.CFG", getenv("MW2_INSTALL_DIR"));
                if ((f = fopen(p, "rb"))) {
                    if (fread(snd, 1, sizeof snd, f) == sizeof snd) {
                        long v = (long)(snd[12] | snd[13] << 8 | snd[14] << 16 | (unsigned long)snd[15] << 24);
                        vol = (int)(v < 0 ? 0 : v > 65536 ? 255 : v * 255 / 65536);
                    }
                    fclose(f);
                }
            }
            cdaudio_set_volume(g_cd, (uint8_t)vol, (uint8_t)vol);
        }
    }
    g_cd_track = track;
    sfx_lock(g_sfx);
    cdaudio_play_track(g_cd, track);
    sfx_unlock(g_sfx);
    sfx_set_music(g_sfx, cd_music_cb, NULL);
    printf("music: CD track %d\n", track);
    if (getenv("MW2_SFX_LOG")) fprintf(stderr, "SFXLOG %.3f music %d\n", sfx_log_time, track);   /* TEST ONLY */
}
static void music_poll(void)   /* the track ended: again (the original's loop) */
{
    cdaudio_status st;
    if (!g_cd || !g_cd_track) return;
    sfx_lock(g_sfx);
    cdaudio_get_status(g_cd, &st);
    if (!st.busy && !st.paused) cdaudio_play_track(g_cd, g_cd_track);
    sfx_unlock(g_sfx);
}
static void music_stop(void)
{
    if (!g_cd) return;
    if (g_sfx) sfx_set_music(g_sfx, NULL, NULL);
    cdaudio_close(g_cd);
    g_cd = NULL; g_cd_track = 0;
}
/* weapon effects (fx.h): definitions, live effects, frame textures by CEL id */
static fx_defs     g_fxd;
static fx_world    g_fxw;
static int         g_fx_ok, g_fx_ati;
static prj_archive *g_fx_ta;
static texture    *g_fxtex[4096];

static prj_archive *g_ma;
/* levels of detail (engine 0x100267d0): each frame every mech takes representation 0 / 1 / 2 / 3 by its distance from the
 * camera against camera +0xa0 x {3600, 8500, 22500} (0x10247694..9c), camera +0xa0 = focal / (LOD quality x 160): at the
 * 640-wide reference (focal 320 x zoom) 72 / 170 / 450 m, halved with DISPLAY DETAIL low. The representations share the
 * full model's skeleton (mech3d_use_skeleton), so a level of detail is posed exactly as the full model. */
static int g_lodq = 1;
static struct { mech3d m[4]; int ok[4], tried; char skel[17]; } g_alod[64];
static mech3d *actor_lod(int k, const mech3d *full, const char *skel, float dist)
{
    float a0 = 2.0f * (g_zoom > 1 ? g_zoom : 1.0f) / (float)g_lodq;
    int r = dist < 0 ? (int)-dist : dist < a0 * 3600.0f ? 0 : dist < a0 * 8500.0f ? 1 : dist < a0 * 22500.0f ? 2 : 3;   /* dist -r: representation r */
    mech3d *m;
    if (getenv("MW2_NO_LOD") && dist >= 0) r = 0;
    if (r == 0 || k < 0 || k >= 64 || !g_ma) return NULL;
    if (!g_alod[k].tried || strcmp(g_alod[k].skel, skel) != 0) {
        int q;
        for (q = 1; q < 4; q++) if (g_alod[k].ok[q]) { mech3d_free(&g_alod[k].m[q]); g_alod[k].ok[q] = 0; }
        g_alod[k].tried = 1;
        snprintf(g_alod[k].skel, sizeof g_alod[k].skel, "%s", skel);
        for (q = 1; q < 4; q++) {
            g_alod[k].ok[q] = mech3d_load(g_ma, skel, q, &g_alod[k].m[q]) == 0 && g_alod[k].m[q].part_count > 0;
            if (g_alod[k].ok[q]) mech3d_use_skeleton(&g_alod[k].m[q], full);
        }
    }
    while (r > 0 && !g_alod[k].ok[r]) r--;
    if (getenv("MW2_LOD_TRACE")) fprintf(stderr, "lod actor %d %s %.0f m -> repr %d (%d parts)\n", k, skel, dist / 100.0f, r, r ? g_alod[k].m[r].part_count : 0);
    if (r == 0) return NULL;
    m = &g_alod[k].m[r];
    memcpy(m->origin, full->origin, sizeof m->origin);
    m->heading = full->heading; m->twist = full->twist; m->pose_off = full->pose_off;
    mech3d_pose(m, (const anim_set *)full->pose_anim, full->pose_t);
    if (full->part_count > 0) { int q; for (q = 0; q < m->part_count; q++) m->parts[q].tex_offset = full->parts[0].tex_offset; }   /* the star's camo / insignia */
    return m;
}

/* TEXTURES TITANIUM: a mech or vehicle drawn as the Titanium archive's model of its record (mech3d_ti_load: its own meshes,
 * skeleton offsets and texture slots; the ANIM records are byte-identical, so it is posed with the actor's animation and
 * time) at the actor's level of detail; the simulation keeps the original model. k 0-63 an actor, 64 the player. */
static struct { mech3d m[4]; int ok[4], tried; char skel[17]; } g_tiact[65];
static mech3d *ti_actor(int k, const mech3d *full, const char *skel, int repr)
{
    mech3d *m;
    int q;
    if (!c3d_ti || k < 0 || k > 64 || !skel || !skel[0]) return NULL;
    if (!g_tiact[k].tried || strcmp(g_tiact[k].skel, skel) != 0) {
        for (q = 0; q < 4; q++) if (g_tiact[k].ok[q]) { mech3d_free(&g_tiact[k].m[q]); g_tiact[k].ok[q] = 0; }
        g_tiact[k].tried = 1;
        snprintf(g_tiact[k].skel, sizeof g_tiact[k].skel, "%s", skel);
        for (q = 0; q < 4; q++) {
            g_tiact[k].ok[q] = mech3d_ti_load(c3d_ti, skel, q, &g_tiact[k].m[q]) == 0 && g_tiact[k].m[q].part_count > 0;
            if (q > 0 && g_tiact[k].ok[q] && g_tiact[k].ok[0]) mech3d_use_skeleton(&g_tiact[k].m[q], &g_tiact[k].m[0]);
        }
    }
    if (repr < 0) repr = 0;
    if (repr > 3) repr = 3;
    while (repr > 0 && !g_tiact[k].ok[repr]) repr--;
    if (!g_tiact[k].ok[repr]) return NULL;
    m = &g_tiact[k].m[repr];
    memcpy(m->origin, full->origin, sizeof m->origin);
    m->heading = full->heading; m->twist = full->twist;
    mech3d_pose(m, (const anim_set *)full->pose_anim, full->pose_t);
    if (full->part_count > 0) for (q = 0; q < m->part_count; q++) m->parts[q].tex_offset = full->parts[0].tex_offset;   /* its star */
    return m;
}
/* TEXTURES TITANIUM: the missile smoke (mercsw.dll's particles, its "Particles" option 0x1009dbc8 - on: the shell's
 * MW2SND.CFG default block +0x28 = 1 (Titanium MW2SHELL 0x10052040, written by 0x100015d4; the engine applies it at the
 * mission's start, 0x1004ec63 -> 0x1004ecc1; 0 only without any MW2SND.CFG, 0x1008b898). Each projectile step of a missile (projectile kind 3 or 4 - and the Titanium-only 9, 10, 12 -, 0x10011a18)
 * starts an emitter (0x1004df04) at its previous position: 20 puffs at i x step / 16 (i = 1..20) + (rand(10) - 5,
 * rand(50) - 25, rand(10) - 5); refused beyond the view distance or with the pool (up to 1000 objects, 0x1004cff9) full.
 * Each update (0x1004d6ec) moves the emitter by step / 8, plus rand(10) up; it ends past 90 ticks; its colour is the
 * mission palette's entry {0x15, 0x15, 0x3f, 0x3f, 0x3e, 0x3e .. 0x38, 0x38}[age x 17 / 90] (0x1004d377) x 200 / 256
 * (the vertex intensity 0xc8). Drawn (0x1004d7ba) as screen squares of 5 - z / 4000 pixels (2 at least, 2 beyond 200 m;
 * z the emitter's depth) textured with particle.PID (a 32 x 32 PCX-RLE disc, index 244 in a grey ramp; every palette
 * entry alpha 200 -> the plugin's inverted 4-bit alpha 3 / 15; colour key on for Render type 7, 0x10003722 state 0x29:
 * black - index 0 - INFERRED as the key), Render type 7 (alpha blend, clamped UVs), no fog. The port steps the emitters
 * and the emission at the 30 fps reference (the engine's per-frame update) and takes the depth as the distance.
 * RENDITION (zredline.dll) keys nothing: mercsw makes the texture as format 1 (desc +0x18 = 1, 0x10002728; PreloadTexture
 * 0x1000274d), which the plugin converts to ARGB4444 without its chroma substitution (0x1000413f), and its Render type 7
 * takes chroma keying only for a texture of another format (0x100030d2: format 1 -> states 0x2a, blend + modulate +
 * clamp, no 0x10 chroma bit) - so the black around the disc is drawn too, at the same 3 / 15: dark squares. */
#define TI_EMIT_MAX 1000
static struct { float pos[3], v[3], off[20][3]; float born; } g_tip[TI_EMIT_MAX];
static int g_tip_n, g_tip_state;   /* g_tip_state: 0 not loaded, 1 loaded, -1 no particle.PID */
static int g_tip_keyed = -1;        /* the textures were made with index 0 keyed out (all but RENDITION) */
static int g_edition;               /* (below: the renderer, PORTCFG_ED_*) */
static texture g_tip_tex[8 + 1];    /* palette 0x15 and 0x3f..0x38 */
static char g_ti_dir[PORTCFG_PATH];
static const uint8_t TI_TIP_COL[18] = {0x15, 0x15, 0x3f, 0x3f, 0x3e, 0x3e, 0x3d, 0x3d, 0x3c, 0x3c, 0x3b, 0x3b, 0x3a, 0x3a, 0x39, 0x39, 0x38, 0x38};
static int ti_tip_load(const char *mission)
{
    char path[PORTCFG_PATH + 16];
    uint8_t buf[4096], img[32 * 32];
    size_t len, i = 0x20, n = 0;
    FILE *f;
    uint8_t pal[256][3];
    int k, w, h;
    snprintf(path, sizeof path, "%s/particle.PID", g_ti_dir);
    if (!(f = fopen(path, "rb"))) { snprintf(path, sizeof path, "%s/PARTICLE.PID", g_ti_dir); f = fopen(path, "rb"); }
    if (!f) { printf("titanium: no particle.PID in %s: no missile smoke\n", g_ti_dir); return -1; }
    len = fread(buf, 1, sizeof buf, f);
    fclose(f);
    w = len >= 16 ? buf[8] | buf[9] << 8 : 0; h = len >= 16 ? buf[12] | buf[13] << 8 : 0;   /* header +8 w, +0xc h (0x100025d4) */
    if (w != 32 || h != 32) return -1;
    while (n < sizeof img && i < len) {   /* PCX run-length: a byte >= 0xc0 repeats the next (b & 0x3f) times */
        uint8_t b = buf[i++];
        if (b >= 0xc0) { int c = b & 0x3f; uint8_t v2 = i < len ? buf[i++] : 0; while (c-- > 0 && n < sizeof img) img[n++] = v2; }
        else img[n++] = b;
    }
    memcpy(pal, g_scene.palette, sizeof pal);
    {   /* the mission's palette: the Titanium archive's (0x10286d40) */
        prj_record rec;
        r_target holder;
        char nm[16];
        snprintf(nm, sizeof nm, "%.4s_DA", mission);
        if (c3d_ti && prj_read_named(c3d_ti, "PAL", nm, &rec) == PRJ_OK) {
            if (r_init(&holder, 1, 1) == 0) { r_set_palette(&holder, rec.data, rec.size); memcpy(pal, holder.palette, sizeof pal); r_free(&holder); }
            prj_record_free(&rec);
        }
    }
    for (k = 0; k < 9; k++) {
        int ix = k == 0 ? 0x15 : 0x40 - k, q;   /* 0x3f .. 0x38 */
        texture *t = &g_tip_tex[k];
        free(t->rgba);
        t->w = t->h = 32;
        if (!(t->rgba = malloc(32 * 32 * sizeof *t->rgba))) return -1;
        for (q = 0; q < 32 * 32; q++) {
            unsigned g = img[q], c[3], j;
            for (j = 0; j < 3; j++) c[j] = (unsigned)pal[ix][j] * g / 255u * 200u / 256u;   /* palette x texel grey x 0xc8 */
            t->rgba[q] = g == 0 && g_tip_keyed ? 0 : (c[0] | c[1] << 8 | c[2] << 16 | 0xffu << 24);
        }
    }
    return 1;
}
static void ti_particles(glr *r, const glr_view *v, float now_ticks, const char *mission)
{
    static float last = -1;
    static glr_particle *pp;
    static int pcap;
    int k, q, n = 0;
    if (!c3d_ti || !g_sim_ok || getenv("MW2_NO_PARTICLES")) { glr_set_particles(r, NULL, 0, 0); g_tip_n = 0; last = -1; return; }
    if (g_tip_state > 0 && g_tip_keyed != (g_edition != PORTCFG_ED_RENDITION)) g_tip_state = 0;   /* (the renderer changed) */
    if (g_tip_state == 0) { g_tip_keyed = g_edition != PORTCFG_ED_RENDITION; g_tip_state = ti_tip_load(mission); }
    if (g_tip_state < 0) { glr_set_particles(r, NULL, 0, 0); return; }
    if (last < 0 || now_ticks < last) last = now_ticks;
    while (now_ticks - last >= 182.0f / 30.0f) {   /* the 30 fps reference steps */
        const float dt = 182.0f / 30.0f;
        last += dt;
        for (k = 0; k < g_tip_n; k++) {
            if (last - g_tip[k].born > 90.0f) { g_tip[k--] = g_tip[--g_tip_n]; continue; }
            g_tip[k].pos[0] += g_tip[k].v[0]; g_tip[k].pos[1] += g_tip[k].v[1] + (float)(rand() % 10); g_tip[k].pos[2] += g_tip[k].v[2];
        }
        for (q = 0; q < g_sim.shot_count; q++) {
            const msim_shot *sh = &g_sim.shots[q];
            int kind = combat_weapon_kind(sh->weapon), i;
            float d[3], o[3], dx, dy, dz;
            if (kind != 3 && kind != 4) continue;
            if (g_tip_n + 1 >= TI_EMIT_MAX) break;
            d[0] = sh->vx * dt; d[1] = sh->vy * dt; d[2] = sh->vz * dt;
            o[0] = sh->x - d[0]; o[1] = sh->y - d[1]; o[2] = sh->z - d[2];
            dx = o[0] - v->eye[0]; dy = o[1] - v->eye[1]; dz = o[2] - v->eye[2];
            if (v->far_cull > 0 && dx * dx + dy * dy + dz * dz > v->far_cull * v->far_cull) continue;
            memcpy(g_tip[g_tip_n].pos, o, sizeof o);
            for (i = 0; i < 3; i++) g_tip[g_tip_n].v[i] = d[i] / 8.0f;
            for (i = 0; i < 20; i++) {
                g_tip[g_tip_n].off[i][0] = d[0] * (float)(i + 1) / 16.0f + (float)(rand() % 10) - 5.0f;
                g_tip[g_tip_n].off[i][1] = d[1] * (float)(i + 1) / 16.0f + (float)(rand() % 50) - 25.0f;
                g_tip[g_tip_n].off[i][2] = d[2] * (float)(i + 1) / 16.0f + (float)(rand() % 10) - 5.0f;
            }
            g_tip[g_tip_n].born = last;
            g_tip_n++;
            {   /* (tests) */
                static float said = -1;
                if (said < 0 || last < said) { said = last; printf("titanium: missile smoke from %.1f s\n", (double)g_sim.now / 1000.0); }
            }
        }
    }
    if (pcap < g_tip_n * 20) { free(pp); pcap = g_tip_n * 20; pp = malloc((size_t)pcap * sizeof *pp); if (!pp) pcap = 0; }
    for (k = 0; pp && k < g_tip_n; k++) {
        float age = now_ticks - g_tip[k].born, dx = g_tip[k].pos[0] - v->eye[0], dy = g_tip[k].pos[1] - v->eye[1], dz = g_tip[k].pos[2] - v->eye[2];
        float z = sqrtf(dx * dx + dy * dy + dz * dz), size = 3.0f;
        int ai = (int)(age / 90.0f * 17.0f), ci;
        if (ai < 0) ai = 0;
        if (ai > 17) ai = 17;
        ci = TI_TIP_COL[ai] == 0x15 ? 0 : 0x40 - TI_TIP_COL[ai];
        if (z > 0) { size = z > 20000.0f ? 2.0f : 5.0f - z * 0.00025f; if (size < 2.0f) size = 2.0f; }
        for (q = 0; q < 20; q++) {
            pp[n].pos[0] = g_tip[k].pos[0] + g_tip[k].off[q][0];
            pp[n].pos[1] = g_tip[k].pos[1] + g_tip[k].off[q][1];
            pp[n].pos[2] = g_tip[k].pos[2] + g_tip[k].off[q][2];
            pp[n].size_px = size;
            pp[n].tex = &g_tip_tex[ci];
            n++;
        }
    }
    glr_set_particles(r, pp, n, 3.0f / 15.0f);   /* (the texture's alpha: 15 - (200 >> 4) = 3, dx5draw 0x10006713) */
}

/* the world objects' Titanium models (world3d_ti_attach on the world's parts) */
static wtb_model *g_ti_world;
static int g_ti_world_n;
static const mech3d_part *g_ti_world_for;   /* the parts array they were attached to */
static void ti_world_attach(mech3d *world)
{
    world3d_ti_free(g_ti_world, g_ti_world_n);
    g_ti_world = NULL; g_ti_world_n = 0; g_ti_world_for = NULL;
    if (!c3d_ti || !world) return;
    g_ti_world_for = world->parts;
    {
        int got = world3d_ti_attach(c3d_ti, world, &g_ti_world), k, scrub = 0;
        g_ti_world_n = world->part_count;
        for (k = 0; k < world->part_count; k++) scrub += world->parts[k].scrg != 0;
        printf("titanium: %d of %d world objects with a Titanium model, %d scrub-patch objects not drawn\n", got, world->part_count, scrub);
    }
}

static texture *fx_texture(int cel)
{
    prj_record r;
    texture *t;
    if (cel < 0 || cel >= 4096 || !g_fx_ta) return NULL;
    if (g_fxtex[cel]) return g_fxtex[cel]->rgba ? g_fxtex[cel] : NULL;
    t = g_fxtex[cel] = calloc(1, sizeof *t);
    if (!t) return NULL;
    if (prj_read(g_fx_ta, prj_find_type(g_fx_ta, "CEL"), cel, &r) == PRJ_OK) {
        prj_record tr;
        /* the Titanium look: the frame of this name in its archive (the effect frames are among the names it has) */
        if (c3d_ti && g_fx_ati != 3 && prj_read_named(c3d_ti, "CEL", r.name, &tr) == PRJ_OK) {
            int ok = tex_decode(tr.data, tr.size, TEX_TITANIUM, t) == 0;
            prj_record_free(&tr);
            if (ok) { prj_record_free(&r); return t; }
        }
        /* ATi: one format; 3dfx/S3: effect frames carry transparency (ARGB4444) */
        if (tex_decode(r.data, r.size, g_fx_ati == 1 ? TEX_ATI : g_fx_ati == 2 ? TEX_PVR : g_fx_ati == 3 ? TEX_DOS : g_fx_ati == 4 ? TEX_MGA : TEX_3DFX_4444, t) != 0) t->rgba = NULL;
        else if (t->alt) { free(t->alt); t->alt = NULL; }   /* (drawn as cards: the ARGB4444 reading only) */
        if (t->rgba && g_fx_ati == 3) {   /* DOS: the effect frames in the mission's palette (index 0xff transparent) */
            size_t q;
            for (q = 0; q < (size_t)t->w * (size_t)t->h; q++) {
                unsigned ix = t->rgba[q] & 0xff, al = t->rgba[q] >> 24;
                t->rgba[q] = (uint32_t)g_scene.palette[ix][0] | ((uint32_t)g_scene.palette[ix][1] << 8) | ((uint32_t)g_scene.palette[ix][2] << 16) | ((uint32_t)al << 24);
            }
        }
        prj_record_free(&r);
    }
    return t->rgba ? t : NULL;
}

static int view_in_cockpit(void);   /* VIEW_IN_COCKPIT (DAT_1024e678, the engine's halve-in-the-cockpit flag) */
/* The listener of every positional sound is the CAMERA (PTR_DAT_1024e1cc: position +0 / +4 / +8, yaw +0xc - the view's,
 * glances included), not the player's mech: 0x10032ad0 takes the vector its caller made against the camera and
 * 0x10032c10 pans by the camera's yaw. The camera is placed at the frame's start (0x1000907f -> 0x1002dc90, before the
 * player's input 0x10018880 and the unit updates 0x10018840), so the sounds of a frame hear last frame's camera - the
 * port's g_snd_* below, set where the frame's camera is final (camera_modes). The cockpit flag DAT_1024e678 (the
 * "halve" argument of most callers) is set there too (0x1002dc90: 0, the cockpit camera 0x1002e640: 1) - g_snd_pit. */
static float g_snd_eye[3], g_snd_yaw;
static int   g_snd_ok, g_snd_pit = 1;
static void snd_listener(float L[3], float *yaw)
{
    if (g_snd_ok) { memcpy(L, g_snd_eye, sizeof g_snd_eye); *yaw = g_snd_yaw; return; }
    /* before the first camera (the start-up's first frame): the cockpit eye, as the camera will be */
    if (g_sim.player_eye_ok) memcpy(L, g_sim.player_eye, sizeof g_snd_eye);
    else { L[0] = (float)g_pl.origin[0]; L[1] = g_sim.player_unit.y; L[2] = (float)g_pl.origin[2]; }
    *yaw = g_pl.heading + g_pl.twist;
}
static int snd_cockpit(void) { return g_snd_ok ? g_snd_pit : view_in_cockpit(); }   /* DAT_1024e678 as the sounds see it */
/* a positional sound at pos (world cm): mirror = 0 the caller passes position - camera (start-up 0x1001ac01, KICKCHNK
 * 0x1000bff2, landing / jets 0x1001c115 / 0x1001c090, MECSHTD1 0x1001b068), 1 camera - position (footsteps / gait
 * 0x1000b370, weapons 0x10043b59, effects 0x10045fca, object loops 0x10032cc0): the pan mirrored (sfx_play_at3) */
static void snd_at(int snd, const float pos[3], int mirror, int half)
{
    float L[3], yaw, d[3];
    int k;
    if (!g_sfx || snd <= 0) return;
    snd_listener(L, &yaw);
    for (k = 0; k < 3; k++) d[k] = mirror ? L[k] - pos[k] : pos[k] - L[k];
    sfx_play_at3(g_sfx, snd, d[0], d[1], d[2], yaw, half);
}
/* the player's mech position (unit +0x50 / +0x54 / +0x58) */
static void snd_player_pos(float p[3]) { p[0] = (float)g_pl.origin[0]; p[1] = g_sim.player_unit.y; p[2] = (float)g_pl.origin[2]; }
/* an effect starts (msim on_effect): pool, then its sound at its position (engine table 0x1025b168) */
static void on_effect(void *u, int type, const float pos[3])
{
    int snd;
    (void)u;
    if (!g_fx_ok) return;
    {
        int fi = fx_spawn(&g_fxw, &g_fxd, type, pos, (int32_t)((int64_t)g_sim.now * 182 / 1000));
        if (fi < 0) { g_sim.fx_refused = 1; return; }   /* nor its chunks */
        g_fxw.e[fi].has_rot = g_sim.fx_rot_ok;   /* a muzzle flash / jet flame: its mount node's orientation (0x10044a70) */
        if (g_sim.fx_rot_ok) memcpy(g_fxw.e[fi].rot, g_sim.fx_rot, sizeof g_fxw.e[fi].rot);
    }
    if (getenv("MW2_FX_TRACE")) fprintf(stderr, "fx %.1fs type %x at %.0f,%.0f,%.0f\n", g_sim.now / 1000.0, type, pos[0], pos[1], pos[2]);
    snd = fx_sound(type);
    /* 0x10045d00 end: halved in the cockpit view (camera mode 0, 0x1002df50) unless the type carries flag 0x4000 - a hit
     * on the player still alive (0x10045557) */
    /* (0x10045fca: the vector camera - position, the pan mirrored; DOSBox YELLSCN1, LRMs fired straight ahead with the
     * view glanced right: their explosion L 0.095 R 0.435 - pan 15, halved) */
    if (snd > 0 && g_sfx) snd_at(snd, pos, 1, view_in_cockpit() && !g_sim.fx_player_hit);
}
static void on_sound(void *user, int snd, const float pos[3])
{
    if (snd == -1) {   /* the collision clang (KICKCHNK) */
        static int kid = -2;
        if (kid == -2) { int st = g_ma ? prj_find_type(g_ma, "SNDS") : -1; kid = st >= 0 ? prj_find_id(g_ma, st, "MECBLDC1") : -1; }
        snd = kid;
        if (snd <= 0) return;
    }
    if (!pos) { if (g_sfx) sfx_play(g_sfx, snd, SFX_V64, 0.0f); return; }   /* not positional: 0x10032b60 (ENEMYFIR 0x6f, 0x1002342f: volume 0x64, priority 0x32) */
    (void)user;
    /* positional (0x10032bf0): every caller passes the cockpit-view flag DAT_1024e678 as "halve" - footsteps 0x1000b4b9,
     * gait 0x1000b567, KICKCHNK 0x1000bff2 / 0x1000c53a, NONPSTRT 0x1001ac01, landing 0x1001c115 ... - except a unit's
     * start-up MECTURX2, halved OUTSIDE the cockpit (0x1001ac19: edx = DAT_1024e678 == 0). Heard from the camera; the
     * footsteps (NONFOOT 0x103, 0x1000b4b9 -> 0x1000b370) pass camera - mech: mirrored */
    snd_at(snd, pos, snd == 0x103, snd == 0xf7 ? !snd_cockpit() : snd_cockpit());
}
/* object sound tasks (TSK type 4: 0x1000ec90 -> 0x10032cc0, once per run of the task - every TSK period): a looping
 * sample at the node, at most 8 at once (sample channels 8-15, the first to ask keeps one); the task's first run does
 * nothing (+0x18 = 1 at creation, cleared by that run: 0x10032cd8; created 0x1000ecf2); ended beyond the task's range or 500 m (0x10032f60
 * 0); AIL volume = 0x10032f60 (100 - 100 (d / 500 m)^2, d the 3D distance from the camera in whole metres) x EFFECTS
 * (16.16, rounded) >> 2 - the same AIL_set_sample_volume scale a one-shot's +0x468 reaches (0x10032700), so exactly a
 * quarter of a one-shot at that distance (DOS 0x40fbc..0x40fcb the same); pan 0x10032c10 as sfx_play_at3 (camera -
 * object: mirrored); playback rate re-drawn every run from the rate table 0x1024f738 [rand(10)] (9922..11907 Hz,
 * 0x10032c50; DOS 0x41016 table 0x970d0) - it replaces the record's own rate (PWRCVTR's 22050 Hz WAV plays at about
 * half speed). The engine's rand(10) is the shared random table (0x1003d910); the port draws from its own generator
 * so the simulation's sequence is not disturbed. A name missing from SNDTABLE (TABL 11: VULTURES, JNG, MECDSLXX,
 * MECINDS1-3, ...) plays nothing (0x10025eb0 -1 -> 0x100243f0 refuses id < 0). */
static const int OBJ_SND_RATE[10] = { 9922, 10143, 10364, 10584, 10804, 11025, 11246, 11466, 11686, 11907 };   /* 0x1024f738 */
/* an SNDS record as unsigned 8-bit mono: SFLX (sfx_decode, 11025 Hz) or, as most ambient loops of the 3D editions
 * are, a RIFF WAVE of 8-bit PCM (stereo averaged); bytes, or -1 */
static int obj_snd_pcm(int id, unsigned char **out, int *rate)
{
    prj_record r;
    int st = prj_find_type(g_ma, "SNDS"), n = -1, ch = 0, bits = 0;
    size_t p = 12;
    *out = NULL; *rate = 11025;
    if (st < 0 || prj_read(g_ma, st, id, &r) != PRJ_OK) return -1;
    if (r.size >= 12 && memcmp(r.data, "RIFF", 4) == 0 && memcmp(r.data + 8, "WAVE", 4) == 0) {
        while (p + 8 <= r.size) {
            const uint8_t *d = r.data + p;
            size_t len = (size_t)d[4] | (size_t)d[5] << 8 | (size_t)d[6] << 16 | (size_t)d[7] << 24, k;
            if (len > r.size - p - 8) len = r.size - p - 8;
            if (memcmp(d, "fmt ", 4) == 0 && len >= 16) {
                if ((d[8] | d[9] << 8) != 1) break;   /* PCM only */
                ch = d[10]; *rate = d[12] | d[13] << 8 | d[14] << 16; bits = d[22];
            } else if (memcmp(d, "data", 4) == 0 && ch >= 1 && ch <= 2 && bits == 8 && *rate > 0) {
                n = (int)(len / (size_t)ch);
                if (n <= 0 || !(*out = malloc((size_t)n))) { n = -1; break; }
                for (k = 0; k < (size_t)n; k++) (*out)[k] = ch == 1 ? d[8 + k] : (uint8_t)((d[8 + 2 * k] + d[9 + 2 * k]) / 2);
                break;
            }
            p += 8 + len + (len & 1);
        }
        prj_record_free(&r);
        return n;
    }
    prj_record_free(&r);
    return sfx_decode(g_ma, id, out);
}
static void obj_sounds(void)
{
    static struct { int task; unsigned char *pcm; } ch[8];
    static const void *owner;
    static unsigned char bad[4096];
    static int seen[4096];          /* per task: the run count last acted on */
    static unsigned rng = 0x2545F491u;
    world3d_sound s[64];
    int n, i, k;
    if (owner != (const void *)g_sim.paths || !g_sfx) {   /* a new mission (or none): the loops end */
        for (k = 0; k < 8; k++) if (ch[k].pcm) { if (g_sfx) sfx_stop_pcm(g_sfx, ch[k].pcm); free(ch[k].pcm); ch[k].pcm = NULL; }
        memset(bad, 0, sizeof bad);
        memset(seen, 0, sizeof seen);
        owner = g_sim.paths;
    }
    if (!g_sfx || !g_sim.paths || !g_ma) return;
    n = world3d_paths_sounds(g_sim.paths, s, 64);
    for (k = 0; k < 8; k++) {   /* tasks that ended (their object destroyed) */
        int live = 0;
        for (i = 0; i < n && !live; i++) live = ch[k].pcm && s[i].task == ch[k].task;
        if (ch[k].pcm && !live) { sfx_stop_pcm(g_sfx, ch[k].pcm); free(ch[k].pcm); ch[k].pcm = NULL; }
    }
    for (i = 0; i < n; i++) {
        /* 0x10032cc0: the camera minus the object (pan mirrored, sfx_play_at3), its 3D distance (0x1001d970 local_4,
         * 0x10032d4d) for the range and the level */
        float L[3], yaw, dx, dy, dz, d, pan;
        int m, vol, c = -1, rate, first;
        if (s[i].task >= 4096 || s[i].runs == seen[s[i].task]) continue;   /* no run since the last one */
        first = seen[s[i].task] == 0 && s[i].runs == 1;
        seen[s[i].task] = s[i].runs;
        if (first) continue;   /* 0x10032cd8: the first run only clears +0x18 */
        snd_listener(L, &yaw);
        dx = L[0] - s[i].pos[0]; dy = L[1] - s[i].pos[1]; dz = L[2] - s[i].pos[2];
        d = sqrtf(dx * dx + dy * dy + dz * dz);
        m = (int)(d / 100.0f); vol = m >= 500 ? 0 : 100 - m * m * 100 / 250000;
        for (k = 0; k < 8; k++) if (ch[k].pcm && ch[k].task == s[i].task) c = k;
        if (d > (float)s[i].range || vol <= 0) {
            if (c >= 0) { sfx_stop_pcm(g_sfx, ch[c].pcm); free(ch[c].pcm); ch[c].pcm = NULL; }
            continue;
        }
        rng = rng * 1103515245u + 12345u;
        rate = OBJ_SND_RATE[(rng >> 16) % 10];   /* 0x10032c50, set before the start on a new channel */
        if (c < 0) {
            int st = prj_find_type(g_ma, "SNDS"), id, len, frate = 11025;
            unsigned char *pcm = NULL;
            if (bad[s[i].task]) continue;
            for (k = 0; k < 8 && ch[k].pcm; k++) {}
            if (k == 8) continue;   /* every channel taken: retried at its next run */
            id = st >= 0 ? prj_find_id(g_ma, st, s[i].name) : -1;
            if (id <= 0 || (len = obj_snd_pcm(id, &pcm, &frate)) <= 0) { free(pcm); bad[s[i].task] = 1; continue; }
            c = k; ch[c].task = s[i].task; ch[c].pcm = pcm;
            sfx_play_pcm(g_sfx, pcm, len, rate, 0.0f, 1);   /* the record's own rate (frate) is overridden */
        }
        pan = sfx_pos_pan(dx, dz, yaw);
        sfx_pcm_level(g_sfx, ch[c].pcm, (float)vol / 100.0f * 0.8f * 0.25f, pan);
        sfx_pcm_rate(g_sfx, ch[c].pcm, rate);
        if (getenv("MW2_SFX_LOG")) fprintf(stderr, "LOOPLOG %.3f task %d %s run %d vol %d rate %d\n", sfx_log_time, s[i].task, s[i].name, s[i].runs, vol, rate);   /* TEST ONLY */
    }
}
/* engine 0x10037880: the display toggles (MFD cycle, HTAL, rear / down view, weapon / damage / target display, HUD)
 * click with sound 0xdc at volume 0x64, centred (0x10032b60(0xdc, 0x64, 0x40, n, 0x32); n 0-8 the rate-table index,
 * unused by 0x100328e0 - DOS 0x40c50 the same), only with the mech up (unit +0x15 & 0x20: set at the first online,
 * kept while shut down) */
static int ui_click(void)
{
    if (g_sfx && g_sim_ok && !g_sim.player_unit.destroyed && !(g_sim.now < g_sim.player_online_at && !getenv("MW2_NO_STARTUP")))
        sfx_play(g_sfx, 0xdc, SFX_V64, 0.0f);
    return 1;
}
static float g_fade_level = 1.0f;   /* the opening palette fade: 0 black .. 1 done (see the frame end) */
static int   g_voice_off;   /* the VOICE slider at 0: the outcome is shown as text instead (0x10031440) */
static int   g_radio_qn;    /* radio lines waiting (the msim end sequence waits for none, DAT_1024f6bc) */
static int   g_radio_waiting;   /* mission radio lines queued, not yet playing (the voice queue lets them go first) */
static int   g_masc;   /* MASC engaged (engine DAT_1024c554) */
/* mission radio messages (mtbl node start / finish SNDS names): on the voice channel, one at a time - a message that
 * arrives while another plays waits for it */
static void on_radio(void *user, const char *name)
{
    static char queue[8][12];
    static int qn, qpri[8], flushed;
    (void)user;
    g_radio_qn = qn + (g_sfx && sfx_voice_busy(g_sfx));
    g_radio_waiting = qn;
    if (!flushed && g_sim.end_flush_at && g_sim.now >= g_sim.end_flush_at) {
        /* the end sequence's flush (0x10031640(1)): queued lines below priority 0x50 dropped (the outcome line is 0x50) */
        int a = 0, b;
        for (b = 0; b < qn; b++) if (qpri[b] >= 0x50) { memcpy(queue[a], queue[b], sizeof queue[0]); qpri[a++] = qpri[b]; }
        qn = a;
        flushed = 1;
    }
    if (!name) {   /* drain: start the next queued line when the voice channel is free */
        int st, id;
        static int32_t t_free = -100000;
        int32_t now = g_sim.now;
        /* pacing (mission time): a line starts as soon as it is queued - DOSBox: YELLSCN1's opener (yell00bS, queued by
         * the start node at t = 0) one frame after the power-up sound, TNJ1SCN1's trn1_01S as its node completes (2 s);
         * the port had waited 4 s from the start. A 1.2 s pause after each line (port, kept: the engine plays them back
         * to back) */
        if (!g_sfx || sfx_voice_busy(g_sfx)) { t_free = now; return; }
        if (!qn || !g_ma || now - t_free < 1200) return;
        st = prj_find_type(g_ma, "SNDS");
        id = st >= 0 ? prj_find_id(g_ma, st, queue[0]) : -1;
        if (getenv("MW2_STATE_DUMP")) printf("RADIO now=%d %s id=%d\n", (int)g_sim.now, queue[0], id);   /* TEST hook (tests/sequences) */
        if (id > 0) sfx_play_voice(g_sfx, id, 1.0f);
        else {   /* not in the archive: the disc's voice files - KEATING\<name>.SFL (the training instructor) */
            static unsigned char *pcm;   /* the line playing (one at a time) */
            const char *cd = getenv("MW2_CD_DIR");
            char path[1200], up[16];
            FILE *f;
            int k;
            for (k = 0; queue[0][k] && k < 15; k++) up[k] = (char)toupper((unsigned char)queue[0][k]);
            up[k] = '\0';
            if (!cd || !cd[0]) { static portcfg pc; static int got; if (!got) { char *bp = SDL_GetBasePath(); portcfg_load(NULL, &pc); portcfg_resolve(&pc, bp, NULL); SDL_free(bp); got = 1; } cd = pc.cd; }
            snprintf(path, sizeof path, "%s/KEATING/%s.SFL", cd ? cd : ".", up);
            if ((f = fopen(path, "rb"))) {
                long n;
                unsigned char *raw;
                fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
                if (n > 14 && (raw = malloc((size_t)n))) {
                    if (fread(raw, 1, (size_t)n, f) == (size_t)n) {
                        unsigned char *dec = NULL;
                        int len = sfx_decode_sflx(raw, (size_t)n, &dec);
                        if (len > 0) { sfx_stop_pcm(g_sfx, pcm); free(pcm); pcm = dec; sfx_play_pcm_voice(g_sfx, pcm, len, 1.0f); }
                        else free(dec);
                    }
                    free(raw);
                }
                fclose(f);
            }
        }
        memmove(queue[0], queue[1], sizeof queue[0] * 7); memmove(qpri, qpri + 1, sizeof qpri[0] * 7); qn--;
        return;
    }
    if (getenv("MW2_RADIO_TRACE")) fprintf(stderr, "radio %s\n", name);
    if (qn < 8) {   /* lines queued together play in their numbered order (trn1_01S, the 41 s introduction, before trn1_02S) */
        int q2 = qn++;
        snprintf(queue[q2], sizeof queue[0], "%s", name);
        qpri[q2] = g_sim.ending ? 0x50 : 0x32;   /* the outcome line (queued as the end begins) outranks the flush */
        while (q2 > 0 && strncasecmp(queue[q2 - 1], queue[q2], 5) == 0 && strcasecmp(queue[q2 - 1], queue[q2]) > 0) {   /* same series only */
            char tmp[12];
            int tp = qpri[q2 - 1];
            memcpy(tmp, queue[q2 - 1], 12); memcpy(queue[q2 - 1], queue[q2], 12); memcpy(queue[q2], tmp, 12);
            qpri[q2 - 1] = qpri[q2]; qpri[q2] = tp;
            q2--;
        }
    }
}
static float g_proj_len[3];
/* the laser bolt's launch offset: 2 x its model's radius (0x10044ac0 / DOS 0x53410: the object put at the mount plus the
 * course x 2 x model +0x40; +0x40 the farthest vertex from the vertex box's centre, 0x10029060 / DOS 0x37f60, of the
 * vertices scaled by the OBJ scale at load, 0x100420b0 -> 0x10042140 / DOS 0x501a9) */
static float g_proj_off[3];
static int   g_dos_lasers;
static int   g_tex_kind;   /* 0 3Dfx / S3, 1 ATi, 2 PowerVR, 3 DOS, 4 Matrox Mystique */
static int   g_edition;    /* PORTCFG_ED_* from MW2_EDITION (the shell's RENDERER choice) */   /* MW2_DOS_LASERS=1: the DOS edition's green balls instead of the 3D editions' bolts */   /* LASER1-3 model lengths (z extent), cm */
static int   g_texset;     /* PORTCFG_TX_* from MW2_TEXTURES (the shell's TEXTURES choice; TITANIUM: c3d_ti is open) */
/* Does the renderer draw fog? The PowerVR edition (its sgl fog) and the enhanced mix (which takes it); the 3Dfx, ATi,
 * S3 and Mystique editions draw none (no fog file). With the Titanium textures that renderer draws the Titanium fog
 * (its SKYGND.PAR colours and depths, made for its sky dome) or none. RENDITION, the Titanium plugin zredline.dll, fogs:
 * mercsw opens it with the fog flag (FirstDevice flags 0x202, mercsw 0x100473ba; zredline 0x100026e1 -> fog state on,
 * VL_SetFogColorRGB with SKYGND.PAR's fog= at 0x10002716) and hands it the vertex fog per vertex. */
static int look_fog(int ed) { return ed == PORTCFG_ED_PVR || ed == PORTCFG_ED_ENHANCED || ed == PORTCFG_ED_RENDITION; }
static mech3d_part g_proj[9];   /* projectile models: LASER1-3, MISSILE1, BULLET, PPCSHOT1, MISSILE2; debris CHUNKER1-2 */
static float     g_proj_scale[7][3];   /* the pool object's OBJ scale (x, y, z) for slots 0-6 (MW2_SHT1) */
/* the PPC (kind 6) and gauss (kind 7) cards: their pool object's colour 0x3xxx -> texture slot (w >> 4) & 0xff (3D 0x0b /
 * 0x0d, DOS 0x52 a9ppcs01 / 0x5a a9gaus01), and half the card's height (foot to top) x its OBJ y scale */
/* [0] the mission's archive (3D 0x0b / 0x0d, DOS 0x52 / 0x5a); [1] TEXTURES TITANIUM: the Titanium archive's own pool
 * (PPCSHOT1 0x3520 -> 0x52, GAS0_0 0x35a0 -> 0x5a, of the Titanium bank 0): mercsw's projectile maker takes the first
 * free pool entry of the weapon's kind for every kind, cards included (0x1006ca96..0x1006cac3: entry +0 = weapon +0x10,
 * +0x48 free, +0x3c an object), and the 16-bit engine has only its own archive */
static int       g_card_slot[2][2] = {{0x0b, 0x0d}, {0x52, 0x5a}};
static float     g_card_half[2][2] = {{200.0f, 100.0f}, {199.0f, 100.0f}};
static float     g_card_tri[2][2][3][3];   /* the card's triangle in the object's frame (x right, y up, z along the course), x its
                                         * OBJ scale - image enhancement draws this outline (DOSBox: red triangles) */
static int       g_card_tri_ok[2][2];
static int       g_proj_ok;
/* TEXTURES TITANIUM: the Titanium archive's own pool objects (its MW2_SHT1, laid out differently - the effect objects
 * first, the debris and torpedo / Narc / Arrow IV pools added, kinds 3 and 4 both MISSLE1): the 16-bit engine has only
 * its own archive, so its projectiles take the pool entry of the weapon's kind (BTHG, 0x10072a7d / the projectile maker
 * 0x1006ca99) and its debris chunks the type 0x0b XPLO entries (CHUNKER1 / CHUNKER2, 0x10072b53) from it */
static mech3d_part g_tiproj[9];
static float     g_tiproj_scale[7][3], g_tiproj_len[3], g_tiproj_off[3];
static prj_archive *g_tiproj_src;
static void load_projectiles_into(prj_archive *a, mech3d_part *proj, float (*pscale)[3], float *plen, float *poff, int cs);
static void load_projectiles(prj_archive *a)
{
    if (g_proj_ok) return;
    load_projectiles_into(a, g_proj, g_proj_scale, g_proj_len, g_proj_off, 0);
    g_proj_ok = 1;
}
static void load_ti_projectiles(void)
{
    int i;
    if (!c3d_ti || g_tiproj_src == c3d_ti) return;
    if (g_tiproj_src) for (i = 0; i < 9; i++) wtb_free(&g_tiproj[i].model);
    load_projectiles_into(c3d_ti, g_tiproj, g_tiproj_scale, g_tiproj_len, g_tiproj_off, 1);
    for (i = 0; i < 9; i++) g_tiproj[i].ti = 1;   /* (its colour words read the Titanium way) */
    g_tiproj_src = c3d_ti;
    printf("titanium: pool models %s %s %s %s %s %s %s, debris %s %s\n", g_tiproj[0].model_name, g_tiproj[1].model_name, g_tiproj[2].model_name,
           g_tiproj[3].model_name, g_tiproj[4].model_name, g_tiproj[5].model_name, g_tiproj[6].model_name, g_tiproj[7].model_name, g_tiproj[8].model_name);
    printf("titanium: cards PPC slot 0x%02x half %.0f, gauss slot 0x%02x half %.0f\n", g_card_slot[1][0], g_card_half[1][0], g_card_slot[1][1], g_card_half[1][1]);
}
static void load_projectiles_into(prj_archive *a, mech3d_part *proj, float (*pscale)[3], float *plen, float *poff, int cs)
{
    int seen[2] = {0, 0};   /* the cards: the first pool entry of each kind */
    /* the projectile objects come from the pool record MW2_SHT1 (engine 0x10043dc5 / DOS 0x50xxx: the first free pool
     * entry whose BTHG kind equals the weapon table's +0x00): each OBJ (its POLY model and scale) followed by a BTHG
     * {u16 id, u16 kind}. The archives name the models differently - the 3D ones MISSLE2 (kind 3) / MISSLE1 (kind 4),
     * the DOS one MISSLE1 for both (no MISSILE1 / MISSILE2 records at all: DOS missiles were never drawn) - so the
     * model is taken by the pool's id, not by a name. Slots: kind 0-2 -> 0-2, 4 -> 3, 5 -> 4, 6 -> 5, 3 -> 6. The
     * names below are the fallback (an archive without the pool) and the debris chunks (7, 8). */
    static const char *names[9] = {"LASER1", "LASER2", "LASER3", "MISSILE1", "BULLET", "PPCSHOT1", "MISSILE2", "CHUNKER1", "CHUNKER2"};
    static const int SLOT_OF_KIND[7] = {0, 1, 2, 6, 3, 4, 5};
    int i, have[9] = {0};
    prj_record r;
    for (i = 0; i < 7; i++) pscale[i][0] = pscale[i][1] = pscale[i][2] = 1.0f;
    pscale[0][0] = pscale[0][1] = pscale[1][0] = pscale[1][1] = pscale[2][0] = pscale[2][1] = 3.0f;
    pscale[0][2] = pscale[1][2] = pscale[2][2] = 12.0f;   /* the lasers' (3, 3, 12) if no pool */
    for (i = 0; i < 9; i++) {
        memset(&proj[i], 0, sizeof proj[i]);
        snprintf(proj[i].model_name, sizeof proj[i].model_name, "%s", names[i]);
        proj[i].node = -1;
    }
    if (prj_read_named(a, "BWD", "MW2_SHT1", &r) == PRJ_OK) {
        int n = bwd_chunks(r.data, r.size, NULL, 0), k, pt = prj_find_type(a, "POLY"), sl;
        bwd_chunk *c = n > 0 ? calloc((size_t)n, sizeof *c) : NULL;
        bwd_obj ob;
        int got = 0;
        if (c) bwd_chunks(r.data, r.size, c, n);
        for (k = 0; c && k < n; k++) {
            if (strcmp(c[k].tag, "OBJ") == 0) got = bwd_obj_decode(&c[k], &ob) == 0;
            else if (strcmp(c[k].tag, "XPLO") == 0 && got && c[k].size >= 6 && (c[k].data[4] | c[k].data[5] << 8) == 0x0b) {
                /* the debris chunks: the pool's effect type 0x0b objects (XPLO {u16 id, i16, u16 type}, 0x10072b06 /
                 * 3D 0x1003ed54), alternately (CHUNKER1 / CHUNKER2 in both archives) - the first two models */
                prj_record mr;
                got = 0;
                sl = !have[7] ? 7 : !have[8] ? 8 : -1;
                if (sl < 0 || pt < 0 || prj_read(a, pt, ob.model, &mr) != PRJ_OK) continue;
                if (wtb_parse(mr.data, mr.size, &proj[sl].model) == 0 && proj[sl].model.object_count > 0) {
                    int q;
                    for (q = 0; q < prj_symbol_count(a, pt); q++)
                        if (prj_symbol_id(a, pt, q) == ob.model) { snprintf(proj[sl].model_name, sizeof proj[sl].model_name, "%s", prj_symbol_name(a, pt, q)); break; }
                    if (sl == 8 && strcmp(proj[8].model_name, proj[7].model_name) == 0) { wtb_free(&proj[8].model); proj[8].model_name[0] = 0; }
                    else have[sl] = 1;
                }
                prj_record_free(&mr);
            }
            else if (strcmp(c[k].tag, "BTHG") == 0 && got && c[k].size >= 4) {
                int kind = c[k].data[2] | c[k].data[3] << 8;
                prj_record mr;
                got = 0;
                if (kind == 6 || kind == 7) {
                    wtb_model cm;   /* a card (set cs): its texture slot and size (the first pool entry of the kind) */
                    if (seen[kind - 6]++ || pt < 0 || prj_read(a, pt, ob.model, &mr) != PRJ_OK) continue;
                    if (wtb_parse(mr.data, mr.size, &cm) == 0 && cm.object_count > 0 && cm.objects[0].poly_count > 0 &&
                        (cm.objects[0].polys[0].color & 0x7000) == 0x3000) {
                        const wtb_object *o = &cm.objects[0];
                        int q, lo = 1 << 30, hi = -(1 << 30);
                        g_card_slot[cs][kind - 6] = (o->polys[0].color >> 4) & 0xff;
                        for (q = 0; q < o->vert_count; q++) if (o->verts[q].a == 0) { if (o->verts[q].y < lo) lo = o->verts[q].y; if (o->verts[q].y > hi) hi = o->verts[q].y; }
                        if (hi > lo) g_card_half[cs][kind - 6] = (float)(hi - lo) * 0.5f * (ob.scale[1] > 0 ? (float)ob.scale[1] : 1.0f);
                        if (o->polys[0].n == 3) {
                            for (q = 0; q < 3; q++) {
                                const wtb_vertex *vv = &o->verts[o->polys[0].idx[q]];
                                g_card_tri[cs][kind - 6][q][0] = (float)vv->x * (ob.scale[0] > 0 ? (float)ob.scale[0] : 1.0f);
                                g_card_tri[cs][kind - 6][q][1] = (float)vv->y * (ob.scale[1] > 0 ? (float)ob.scale[1] : 1.0f);
                                g_card_tri[cs][kind - 6][q][2] = (float)vv->z * (ob.scale[2] > 0 ? (float)ob.scale[2] : 1.0f);
                            }
                            g_card_tri_ok[cs][kind - 6] = 1;
                        }
                        wtb_free(&cm);
                    }
                    prj_record_free(&mr);
                    continue;
                }
                if (kind < 0 || kind > 6 || have[sl = SLOT_OF_KIND[kind]]) continue;
                if (pt >= 0 && prj_read(a, pt, ob.model, &mr) == PRJ_OK) {
                    if (wtb_parse(mr.data, mr.size, &proj[sl].model) == 0 && proj[sl].model.object_count > 0) {
                        int q;
                        have[sl] = 1;
                        for (q = 0; q < prj_symbol_count(a, pt); q++)
                            if (prj_symbol_id(a, pt, q) == ob.model) { snprintf(proj[sl].model_name, sizeof proj[sl].model_name, "%s", prj_symbol_name(a, pt, q)); break; }
                        for (q = 0; q < 3; q++) pscale[sl][q] = ob.scale[q] > 0 ? (float)ob.scale[q] : 1.0f;
                    }
                    prj_record_free(&mr);
                }
            }
        }
        free(c);
        prj_record_free(&r);
    }
    for (i = 0; i < 9; i++) {
        if (!have[i] && prj_read_named(a, "POLY", names[i], &r) == PRJ_OK) {
            wtb_parse(r.data, r.size, &proj[i].model);
            prj_record_free(&r);
        }
        if (i < 3 && proj[i].model.object_count) {
            const wtb_object *o = &proj[i].model.objects[0];
            int k2;
            float lo = 1e9f, hi = -1e9f;
            for (k2 = 0; k2 < o->vert_count; k2++) { if (o->verts[k2].z < lo) lo = (float)o->verts[k2].z; if (o->verts[k2].z > hi) hi = (float)o->verts[k2].z; }
            plen[i] = hi - lo;
            {
                float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f}, ctr[3], r2 = 0;
                int c2;
                for (k2 = 0; k2 < o->vert_count; k2++) {
                    const float vv[3] = {o->verts[k2].x * pscale[i][0], o->verts[k2].y * pscale[i][1], o->verts[k2].z * pscale[i][2]};
                    for (c2 = 0; c2 < 3; c2++) { if (vv[c2] < mn[c2]) mn[c2] = vv[c2]; if (vv[c2] > mx[c2]) mx[c2] = vv[c2]; }
                }
                for (c2 = 0; c2 < 3; c2++) ctr[c2] = (mn[c2] + mx[c2]) * 0.5f;
                for (k2 = 0; k2 < o->vert_count; k2++) {
                    const float d0 = o->verts[k2].x * pscale[i][0] - ctr[0], d1 = o->verts[k2].y * pscale[i][1] - ctr[1], d2 = o->verts[k2].z * pscale[i][2] - ctr[2];
                    if (d0 * d0 + d1 * d1 + d2 * d2 > r2) r2 = d0 * d0 + d1 * d1 + d2 * d2;
                }
                poff[i] = 2.0f * sqrtf(r2);
            }
        }
    }
}
/* TEXTURES TITANIUM: a blown-off mech part as its Titanium piece - the Titanium engine has only its own models, the pieces
 * being its mech's own object nodes. The piece on the same skeleton node, the n-th there as the original part is (the
 * Titanium records hang their meshes on the same ANIM nodes); NULL none (the original piece then). k 0-63 an actor, 64
 * the player. */
static const mech3d_part *ti_debris_part(int k, const mech3d *orig, int part)
{
    const mech3d *tm;
    int nth = 0, q, node;
    if (!c3d_ti || g_tex_kind == 3 || k < 0 || k > 64 || !orig || part < 0 || part >= orig->part_count || !g_tiact[k].tried || !g_tiact[k].ok[0]) return NULL;
    tm = &g_tiact[k].m[0];
    node = orig->parts[part].node;
    for (q = 0; q < part; q++) if (orig->parts[q].node == node) nth++;
    for (q = 0; q < tm->part_count; q++) if (tm->parts[q].node == node && nth-- == 0) return &tm->parts[q];
    return NULL;
}
/* TEXTURES TITANIUM: a projectile's light (mercsw 0x1006cd6c..0x1006ced3, the projectile maker every weapon but the AMS
 * uses): with the "Multiple Lights" option, the first projectile of a weapon's volley (0x100b19b8: set when the slot's
 * shots left equal the type's volley size, 0x1006c470; cleared after it for the types with flag 1 or 2, 0x1006c579 -
 * all but the AMS) carries a point light (type 1) of radius 10000 (+-100 m box), colour (0.2, 0.2, 0.2) - or, when its
 * object's model starts with a colour kind 4 polygon, the palette entry of its vertex 0's texel u (0x10286d40: the
 * palette's 6-bit values x 4) / 128 - for as long as it flies (no life, 0x1002e94f mask 0x107; moved with it,
 * 0x1001194c; freed at its end, 0x10011e45). The model: the Titanium MW2_SHT1 pool entry of the weapon's kind (BTHG). */
static float   g_tipl[8][3];
static uint8_t g_tipl_pal[256][3];
static int     g_tipl_ok;
static void ti_proj_light(int w, float out[3])
{
    int kind = combat_weapon_kind(w), q;
    out[0] = out[1] = out[2] = 0.2f;
    if (!c3d_ti || !c3d_ti_have_scene || kind < 0 || kind > 7) return;
    if (!g_tipl_ok || memcmp(g_tipl_pal, c3d_ti_scene.palette, sizeof g_tipl_pal) != 0) {   /* per mission (its palette) */
        prj_record r;
        int seen[8] = {0};
        for (q = 0; q < 8; q++) g_tipl[q][0] = g_tipl[q][1] = g_tipl[q][2] = 0.2f;
        memcpy(g_tipl_pal, c3d_ti_scene.palette, sizeof g_tipl_pal);
        g_tipl_ok = 1;
        if (prj_read_named(c3d_ti, "BWD", "MW2_SHT1", &r) == PRJ_OK) {
            int n = bwd_chunks(r.data, r.size, NULL, 0), k, pt = prj_find_type(c3d_ti, "POLY"), got = 0;
            bwd_chunk *c = n > 0 ? calloc((size_t)n, sizeof *c) : NULL;
            bwd_obj ob;
            if (c) bwd_chunks(r.data, r.size, c, n);
            for (k = 0; c && k < n; k++) {
                if (strcmp(c[k].tag, "OBJ") == 0) got = bwd_obj_decode(&c[k], &ob) == 0;
                else if (strcmp(c[k].tag, "BTHG") == 0 && got && c[k].size >= 4) {
                    int bk = c[k].data[2] | c[k].data[3] << 8;
                    prj_record mr;
                    wtb_model wm;
                    got = 0;
                    if (bk < 0 || bk > 7 || seen[bk]++ || pt < 0 || prj_read(c3d_ti, pt, ob.model, &mr) != PRJ_OK) continue;
                    if (wtb_parse(mr.data, mr.size, &wm) == 0) {
                        if (wm.object_count > 0 && wm.objects[0].poly_count > 0 && wm.objects[0].vert_count > 0 &&
                            (wm.objects[0].polys[0].color & 0xf000) == 0x4000) {
                            int e = wm.objects[0].verts[0].a & 0xff;
                            for (q = 0; q < 3; q++) g_tipl[bk][q] = (float)c3d_ti_scene.palette[e][q] / 128.0f;
                        }
                        wtb_free(&wm);
                    }
                    prj_record_free(&mr);
                }
            }
            free(c);
            prj_record_free(&r);
        }
    }
    memcpy(out, g_tipl[kind], sizeof g_tipl[kind]);
}
/* weapon table +0x04 -> model slot */
/* the projectile pool object for a weapon (engine 0x10043dc5: the MW2_SHT1 pool entry whose BTHG kind equals the weapon
 * table's +0x00): 0-2 LASER1-3, 3 SRM / Narc MISSILE2, 4 LRM / Streak MISSILE1, 5 BULLET, 6 PPCSHOT1 card, 7 GAS0_0
 * card (the gauss rifle; the flamer has no object, +0x14 = 0); -1 = none. Cards are drawn as sprites below. */
static int proj_slot_w(int w)
{
    if (!combat_weapon_has_object(w)) return -1;
    switch (combat_weapon_kind(w)) {
    case 0: return 0; case 1: return 1; case 2: return 2;
    case 3: return 6; case 4: return 3; case 5: return 4;
    default: return -1;                         /* 6 / 7: cards */
    }
}
/* message + Betty's voice line (message table 0x1024f144: text, voice SNDS index) */
static void hud_message_v(const char *m, int voice);
/* ---- the original resolutions (render=320x200 / 640x480 / 1024x768): an off-screen frame, then shown in blocks */
static GLuint g_lr_fbo[2], g_lr_col[2], g_lr_depth;
static int g_lr_w, g_lr_h, g_out_w, g_out_h;
static int lowres_target(int w, int h)
{
    int k;
    if (g_lr_fbo[0] && g_lr_w == w && g_lr_h == h) return 0;
    if (g_lr_fbo[0]) { glDeleteFramebuffers(2, g_lr_fbo); glDeleteTextures(2, g_lr_col); glDeleteRenderbuffers(1, &g_lr_depth); }
    glGenFramebuffers(2, g_lr_fbo); glGenTextures(2, g_lr_col); glGenRenderbuffers(1, &g_lr_depth);
    for (k = 0; k < 2; k++) {
        int tw = k == 0 ? w : 320, th = k == 0 ? h : 200;
        glBindTexture(GL_TEXTURE_2D, g_lr_col[k]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, tw, th, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, g_lr_fbo[k]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_lr_col[k], 0);
        if (k == 0) {
            glBindRenderbuffer(GL_RENDERBUFFER, g_lr_depth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_lr_depth);
        }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { glBindFramebuffer(GL_FRAMEBUFFER, 0); g_lr_fbo[0] = 0; return -1; }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_lr_w = w; g_lr_h = h;
    return 0;
}
static void lowres_present(int ow, int oh)
{
    int sw = g_render_w == 320 ? 320 : g_lr_w, sh = g_render_w == 320 ? 200 : g_lr_h;
    GLuint src = g_render_w == 320 ? g_lr_fbo[1] : g_lr_fbo[0];   /* 320 x 200: the frame the HUD was drawn into */
    int vw, vh, x0, y0, k;
    /* the 4:3 picture, as large as fits; whole multiples of the low-res pixel where they fit (crisp, even blocks) */
    vh = oh; vw = oh * 4 / 3;
    if (vw > ow) { vw = ow; vh = ow * 3 / 4; }
    k = vh / sh;
    if (g_render_w == 320) { if (vh / 240 >= 2) { vh = vh / 240 * 240; vw = vh * 4 / 3; } }   /* 200 lines at 4:3: multiples of 240 */
    else if (k >= 2) { vh = sh * k; vw = sw * k; }   /* whole multiples when at least 2 fit, else the largest 4:3 that fits */
    x0 = (ow - vw) / 2; y0 = (oh - vh) / 2;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, ow, oh);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, 0, sw, sh, x0, y0, x0 + vw, y0 + vh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
/* the inspection's result in the viewer's text line (0x10020e40, DAT_1024c8ac): for 0x16a ticks after an inspection
 * (1, or 3 when inspected before) the line shows what the target holds - a mech its GPS 4th string (unit +0xfe, the
 * pilot: "St.Cpt.Haines"), a structure its sub-name (thing +0x2a), "Contents unknown" when empty - with sound 0xdc; out of
 * range (2) "Out of range"; then the name again. Kept for the target it was made on (0x1024d058) */
static int32_t g_inspect_until;
static int     g_inspect_res, g_inspect_tgt = -1, g_inspect_thing = -1;
static const char *inspect_text(int actor, int thing);
static void inspect_now(void)
{
    int r = msim_inspect(&g_sim, g_tgt_sel, g_tgt_sel < 0 ? g_tgt_thing : -1);
    /* the message table's "Inspection successful" (entry 19, voice 68) is never posted: success shows in the viewer's
     * line only; out of range the viewer (0x10020eac, DAT_1024c8ac == 2) posts entry 20 (0x10031870(0x14, -1)) */
    if (r == 2) hud_message_v("Target is beyond inspection radius", 69);
    if (r) {
        g_inspect_res = r; g_inspect_tgt = g_tgt_sel; g_inspect_thing = g_tgt_sel < 0 ? g_tgt_thing : -1;
        g_inspect_until = g_sim.now + 0x16a * 1000 / 182;
        if (r != 2 && g_sfx) sfx_play(g_sfx, 0xdc, SFX_V64, 0.0f);   /* 0x10020f63: 0x10032b60(0xdc, 0x64, 0x40, 5, 0x32) */
    }
    if (getenv("MW2_STATE_DUMP")) {   /* TEST hook (tests/sequences) */
        const char *t = r ? inspect_text(g_tgt_sel, g_tgt_sel < 0 ? g_tgt_thing : -1) : NULL;
        printf("INSPECT r=%d text=%s\n", r, t ? t : "-");
    }
}
/* the viewer's text for the current target: the inspection result while it lasts, else NULL (the name) */
static const char *inspect_text(int actor, int thing)
{
    const char *t;
    if (g_inspect_until <= g_sim.now || actor != g_inspect_tgt || thing != g_inspect_thing) return NULL;
    if (g_inspect_res == 2) return "Out of range";
    t = actor >= 0 ? g_actors[actor].pilot : thing >= 0 && thing < g_sim.bld_count ? g_sim.bld[thing].sub : "";
    return t[0] ? t : "Contents unknown";
}
/* DAT_1024c8b0 (DOS 0x96290): 0 at the load; 0x1001cca0 - every cycling key's (t / r, e, f, n / Shift+n) - clears it
 * for the player as it starts, TARGET_AT_RETICLE 0x1001d870 sets it as it ENDS (also with nothing under the reticle);
 * RESET_TARGETTING (0x1001ce80) leaves it. While it is 0 the player may take only things flagged 0x400 / 0x1000 */
static int g_tgt_any;
/* may the player target thing b (engine 0x1001d2d0 for the player, DOS 0x2a4xx the same)? not destroyed (record flag 4,
 * the runtime destroyed bit - no GT record carries it), flag 0x400 or 0x1000 unless g_tgt_any, never flag 0x800 (TNJ4's
 * 16 "none" markers), its object there (0x10025760 / 0x10025780), and within 1750 m (0x482ae600 / DOS 0x2ab98) of the
 * player's feet (+0x50) at its object's position (0x100257a0) unless flagged 0x1000. Game data: 0x400 on the dropships,
 * gunships and engagement spheres (65 GT records), 0x1000 on PLUM's shielded dropships and UMBE's hovertrain (3), 0x800
 * on 16; every other thing is 0. dist: the range (the 3D distance; 0x1001d970's range ASSUMED 3D) */
static int thing_targetable(int b, float *dist)
{
    float p[3], dx, dy, dz, d;
    int fl;
    if (!g_sim_ok || b < 0 || b >= g_sim.bld_count || g_sim.bld[b].hp <= 0) return 0;
    fl = g_sim.bld[b].flags;
    if (!g_tgt_any && !(fl & 0x1400)) return 0;
    if (fl & 0x800) return 0;
    if (!msim_thing_point(&g_sim, b, p)) return 0;
    dx = p[0] - g_sim.player[0]; dy = p[1] - g_sim.player_unit.y; dz = p[2] - g_sim.player[2];
    d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!(fl & 0x1000) && d > 175000.0f) return 0;
    if (dist) *dist = d;
    return 1;
}
/* may the player target unit q (engine 0x1001d050 for the player; DOS the same rules)? side -1 any (t / r: flags 8, q:
 * 0), 0 the player's side only (f: 0x20008 - 0x10019040 == 0), 1 the enemy only (e: 0x40008 - 0x10019040 == 1); the
 * team test is the only difference, so t / r also take friendly and neutral units. Refused: destroyed (+0x14 & 6);
 * powered down (+0x14 bit 0x10: shut down - state 3 - or resting / held by a node, states 10 / 11 at 0x100137a0) while
 * DAT_1024c8b0 is 0 - so after t / r / e / f a powered-down unit is skipped and a second q takes it; beyond 1750 m
 * (0x482ae600) of the player unless it carries 0x1000; with neither 0x400 nor 0x1000 (+0x15 & 0x14) while DAT_1024c8b0
 * is 0, or with 0x800 (+0x15 & 8), refused. The bits are the GPS chunk's word +0x1a (0x1003f3e5; world_actor.unit_flags):
 * 0x400 nearly every unit, 0x1400 the GREE / RUST / WHIT enemies (taken beyond 1750 m), 0 the CYAN doors (a second q),
 * 0xc00 the dropship bomb turrets TUR00DRP (never). The owner test (+4 == the player: itself) cannot arise (the player is
 * no actor); the "hidden" tests (+0x40 the node tree, 0x1000f760 its root's object +0x6c) hold for every placed unit. DOSBox YELLSCN1:
 * t cycles Nova, Jenner, Firemoth - the resting Firemoth (table 2, "rest", 1.40 km) never; TNJ1SCN1: t takes the
 * (neutral) Instructor only, not the resting Timber Wolf. dist: the range (3D) */
static int unit_side(int q) { return g_actors[q].friendly ? 0 : g_actors[q].alliance; }
static int unit_targetable(int q, int side, float *dist)
{
    float dx, dy, dz, d;
    if (!g_sim_ok || q < 0 || q >= g_actor_count || !g_sim.armed[q] || g_sim.units[q].destroyed) return 0;
    if (!g_tgt_any && ((g_sim.powered_down && g_sim.powered_down[q]) || g_sim.units[q].shutdown)) return 0;
    if (side >= 0 && unit_side(q) != side) return 0;
    if (!g_tgt_any && !(g_actors[q].unit_flags & 0x1400)) return 0;   /* +0x15 & 0x14 */
    if (g_actors[q].unit_flags & 0x800) return 0;                     /* +0x15 & 8 */
    dx = (float)g_actors[q].mech.origin[0] - (float)g_pl.origin[0]; dz = (float)g_actors[q].mech.origin[2] - (float)g_pl.origin[2];
    dy = g_sim.units[q].y - g_sim.player_unit.y;
    d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d > 175000.0f && !(g_actors[q].unit_flags & 0x1000)) return 0;   /* +0x15 & 0x10 */
    if (dist) *dist = d;
    return 1;
}
/* the selector walk 0x1001cca0 for t / r (side -1), f (0) and e's steps (1): the units, then the things, then the nav
 * points (refused by every cycling key: 0x1001cea0 refuses flags & 0xe), round again - from the target's entry one step
 * on; with nothing selected (+0xdc 0x1000) from the first unit going forward whichever key; from a selected nav point
 * the rest of the navs, so forward the first unit, backward the last thing (or unit). Returns the ring entries passing
 * in walk order (units 0.., things n2..) into out[] (at most one round), their count. step 0: 0x1001cca0 with param_2
 * 0 - restart at the first unit going forward whatever is selected (e's first call, 0x1001dba4 xor ecx / DOS 0x2acf0
 * uVar5 = 0) */
static int target_walk(int step, int side, int *out, int max)
{
    int n2 = g_actor_count, nb = g_sim.bld_count, n = n2 + nb, cur, i, k = 0;
    if (n <= 0) return 0;
    if (step == 0) { cur = 0; step = 1; }
    else if (g_tgt_sel >= 0) cur = g_tgt_sel + step;
    else if (g_tgt_thing >= 0) cur = n2 + g_tgt_thing + step;
    else if (g_nav_sel >= 0 && step < 0) cur = n - 1;
    else { cur = 0; step = 1; }
    for (i = 0; i < n && k < max; i++, cur += step) {
        int q = ((cur % n) + n) % n;
        if (q < n2 ? unit_targetable(q, side, NULL)
                   : (thing_targetable(q - n2, NULL) && (side < 0 || g_sim.bld[q - n2].side == side))) out[k++] = q;
    }
    return k;
}
/* target upkeep, every simulated frame whatever the HUD shows (HUD hidden, target display off, shut down): a destroyed
 * mech or structure stops being the target - the engine clears the target DAT_1025a664 in the simulation, in the
 * mech-destroyed handler 0x10016280 and the structure-destroyed handler 0x10046a90 */
static void target_upkeep(void)
{
    if (g_tgt_sel >= g_actor_count || (g_tgt_sel >= 0 && g_sim.armed[g_tgt_sel] && g_sim.units[g_tgt_sel].destroyed)) {
        g_tgt_sel = -1;
        g_sim.player_lock_target = -1;
    }
    /* 0x1001d510 re-tests a unit target with 0x1001d050 (flags 0) every frame: one that may no longer be taken - beyond
     * 1750 m, or powered down while DAT_1024c8b0 is 0 (after t / r / e / f) - is dropped (+0xdc |= 0x1000) */
    else if (g_tgt_sel >= 0 && g_sim.armed[g_tgt_sel] && !unit_targetable(g_tgt_sel, -1, NULL)) {
        g_tgt_sel = -1;
        g_sim.player_lock_target = -1;
    }
    if (g_tgt_thing >= g_sim.bld_count) g_tgt_thing = -1;
    else if (g_tgt_thing >= 0 && (g_sim.bld[g_tgt_thing].hp <= 0 ||
             (g_world && g_sim.bld[g_tgt_thing].intact >= 0 && g_sim.bld[g_tgt_thing].intact < g_mech.part_count && g_mech.parts[g_sim.bld[g_tgt_thing].intact].hidden)))
        g_tgt_thing = -1;
    /* the engine's upkeep (0x1001d510 every frame) re-tests a thing target with 0x1001d2d0: one that may no longer be
     * taken (beyond 1750 m unflagged; after t / r with DAT_1024c8b0 cleared it is no longer the target anyway) is dropped */
    else if (g_tgt_thing >= 0 && !thing_targetable(g_tgt_thing, NULL)) g_tgt_thing = -1;
}
/* GAMEKEY actions handled outside the simulation's own code:
 *  PAUSE_GAME (action 0x58 -> DAT_1024acd4; WinMain 0x10009740): the same pause timer as the MAIN MENU (sim, time and
 *    sounds stand still, the controls off); SHP PAUSED (resource 0x61 + the resolution set) drawn by 0x10036370 every
 *    frame, unscaled, centred in the viewport rect (0, 0.2)-(1.0, 0.4) of the screen (DOSBox 1024x768: x 445-578,
 *    y 211-248); any key (DAT_10159e64, the last key) ends it.
 *  SELF_DESTRUCT (0x57 -> input +0x43; 0x1001a180): only with the mech up (controller state 2): message 4 (voice 16),
 *    state 7 with a deadline 0x16a ticks on; case 7 then calls 0x100174f0(c, 0). The DOS game (0x000265f0 called at
 *    once from the same check, mw2_decomp.c 22042) has no delay.
 *  EJECT (0x3b -> 0x100174f0(c, 1)): unless already dead / ejected: planet breathable (PLNT, DAT_1024a9ac == 0):
 *    state 5, sound 0xc5; hostile: message 0x20 "Atmosphere hostile : ejection aborted" (voice 63); then the mech is
 *    destroyed (0x10016280), which for state 5 says message 0xc "Ejecting" (voice 12), end status 2 (DAT_102441dd).
 *  TOGGLE_AUTOEJECT (0x3c, DAT_1024bfbc, default off): with the mech up, "Automatic ejection ON / OFF" (0x16a ticks at
 *    0x32); an ammunition explosion (0x100167b0) with it on: state 5 + 0x10016280 - "Ejecting" (end status 2), or on a
 *    hostile planet "Atmosphere hostile : ejection aborted" (end status 4). The switch is global: AI mechs eject too
 *    (combat.c, combat_set_auto_eject).
 *  State 5 (the player ejected) in the controller 0x1001adf0: camera mode 4 (0x1002df20(4)) for 0x389 ticks, then the
 *    end flag DAT_1024c568 (msim: eject_at). The mech stays as it stood (no death sequence, no wreck); the HUD is gone
 *    (DOSBox), the message lines stay. */
static int     g_paused;
static int32_t g_sd_at = -1;                       /* the self-destruct's deadline (sim ms; -1 none) */
static void player_eject(int by_key)   /* 0x100174f0 */
{
    combat_unit *pu = &g_sim.player_unit;
    if (pu->destroyed) return;   /* state 4 / 5: already gone */
    if (by_key) {
        if (!g_sim.planet.hostile) { pu->ejected = 1; pu->intact_out = 1; if (g_sfx) sfx_play(g_sfx, 0xc5, SFX_V64, 0.0f); }   /* state 5; 0x10017536 / DOS: (0xc5, 0x64, 0x40, 5, 0x32) */
        else hud_message_v("Atmosphere hostile : ejection aborted", 63);   /* message 0x20; then an ordinary death */
    }
    pu->destroyed = 1;   /* 0x10016280 ("Ejecting" for state 5: player_upkeep); the mission's end follows (msim) */
}
/* External camera modes (engine DAT_1024e66c, 0x1002dc90): 0 the cockpit (the port's chase camera, g_cockpit 0, stands in
 * for the engine's orbit mode 1), 3 the ordnance camera (0x1002e210), 4 the ejection camera (0x1002e7d0) */
static int      g_camode, g_cam_prev, g_cam_last;   /* the mode; the one ordnance returns to (DAT_1024e664); last frame's (DAT_1024e660) */
/* mode 1, the external orbit (COCKPIT_VIEW 'c', and the death camera): distance DAT_1024e644 and height DAT_1024e650 set once
 * per mission on the first entry (3 x the player's MGEO contact radius +0xe8, 0.25 x that) and kept; the orbit angle
 * DAT_1024e654 (kept, starts at 0); this frame's held controls (sinks -1..1): track_distance_delta, track_height_delta,
 * eyepoint_pan_delta (control table 0x10256540: ranges 3600 / 3600 / 72 a second, key-ramp shift 6 / 6 / 4) */
static float    g_orb_d, g_orb_h, g_orb_ang, g_orb_in[3];
/* eyepoint_tilt (sink 0x10159e18, -16..16): mode 1 aims the camera's pitch at -(eyepoint_tilt x 0.5 + the pitch to the target)
 * (0x1002e280: FUN_1003b9f0(-(param_3 x 0.5 + pitch)), param_3 = the sink as it stands, not x ticks) - the plus side looks up */
static float    g_orb_tilt;
static int      g_reset_tilt_eye;                   /* RESET_INPUTS this frame (eyepoint_tilt_reset, 0x10037880 case 1) */
static float    g_cam_zoom = 1.0f;                  /* DAT_1024e65c: the external modes' zoom (1.0 at start, 2.0 from mode 3 on) */
static unsigned g_follow;                           /* DAT_1025a6a0: the projectile the weapon view / ordnance camera follow (shot id) */
static float    g_cam_eye[3], g_cam_yaw, g_cam_pitch;   /* last frame's camera (the ejection / death cameras start from it) */
static float    g_ej_vy;                            /* the ejection camera: climb speed (cm per tick, DAT_1024e694) */
static int32_t  g_ej_t0, g_cam_now;                 /* its start (sim ms, DAT_1024e69c); the sim time last seen */
static int      g_ej_ending;
/* the camera in the cockpit (mode 0, not the chase camera): cockpit model, own muzzle flashes, halved sounds */
#define VIEW_IN_COCKPIT (g_cockpit && g_camode == 0)
static int view_in_cockpit(void) { return VIEW_IN_COCKPIT; }
/* the mission start (DOSBox YELLSCN1 / TNJ1SCN1 audio + frames, docs/reference/startup_dos_vs_port.png): the first pass of
 * the unit updates (0x10009093 -> 0x10018840: every unit in index order, +0x30 = 0x1001a180) takes each unit's state
 * 0 -> 1 (0x1001ab71) in the same frame, after that frame's mission nodes (0x1000906b: a resting group is already powered
 * down): MECTURX2 0xf7 at the mech (0x10032bf0, positional, halved outside the cockpit) - the player's and every other
 * unit's the mech controller drives whose alliance is not 1 (0x10019040: friendly 0 and neutral 2 - TNJ1's instructor;
 * enemies, alliance 1, sound NONPSTRT only once the player is up - none at the start). A positional sound starts
 * distance / 331 m/s late (sfx_play_at): TNJ1's instructor at 50 m 0.15 s after the player's - DOSBox 0.19 s (taken
 * for "one frame later" before). Called after the first simulation step. The opener line (the start node's radio)
 * plays as soon as it is queued (on_radio). The palette fade from black (frames 1-3 black): see g_fade_*. */
static void start_up_sounds(void)
{
    static int done;
    int a;
    if (done || !g_sim_ok || !g_world) return;
    done = 1;
    if (g_sfx) { float p3[3]; snd_player_pos(p3); snd_at(0xf7, p3, 0, !snd_cockpit()); }   /* 0x1001ac01: the mech minus the camera */
    for (a = 0; g_sfx && a < g_actor_count; a++) {
        const world_actor *ac = &g_actors[a];
        int uc = ac->unit_class;
        if (ac->alliance == 1 && !ac->friendly) continue;
        if (g_sim.powered_down && g_sim.powered_down[a]) continue;   /* a resting group (0x10014520 states 10 / 11, set by its node at the start): no start-up */
        if (!(uc == 1 || uc == 2 || uc == 4 || uc == 5 || uc == 8)) continue;
        if (getenv("MW2_SFX_LOG")) fprintf(stderr, "SFXLOG start-up %s class %d alliance %d friendly %d at %.0f m\n", ac->name, uc, ac->alliance, ac->friendly,
                                             hypot((double)ac->mech.origin[0] - g_pl.origin[0], (double)ac->mech.origin[2] - g_pl.origin[2]) / 100.0);   /* TEST ONLY */
        {
            float p3[3] = {(float)ac->mech.origin[0], a < g_sim.actor_count ? g_sim.units[a].y : (float)ac->mech.origin[1], (float)ac->mech.origin[2]};
            on_sound(NULL, 0xf7, p3);
        }
    }
}
/* the HUD and its windows: not once ejected (DOSBox: the HUD goes; the ordnance camera keeps it) */
#define HUD_VIEW (g_cockpit && g_camode != 4 && !(g_camode == 1 && g_sim.player_unit.destroyed) && !g_sim.player_unit.intact_out)
/* the HUD's message lines alone (DOSBox: ejected - "Ejecting"; destroyed - "Mission failed", "Press any key to exit...") */
#define MSG_ONLY_VIEW (g_camode == 4 || (g_camode == 1 && g_sim.player_unit.destroyed) || g_sim.player_unit.intact_out)
/* (the external view 'c' alive keeps the whole HUD - DOSBox YELLSCN1: radar, compass, weapon list, bars, damage outline) */
static int shot_by_id(unsigned id)
{
    int k;
    for (k = 0; id && k < g_sim.shot_count; k++) if (g_sim.shots[k].id == id) return k;
    return -1;
}
/* 0x10046a00: the player's newest projectile (DAT_1025a69c - kept only when it is a missile, projectile type 3 / 4,
 * 0x10044xxx) becomes the followed one; else none is followed */
static int follow_newest(void)
{
    int k = shot_by_id(g_sim.player_last_shot), w = k >= 0 ? g_sim.shots[k].weapon : -1;
    if (k < 0 || g_sim.shots[k].owner != -1 || w < 0 || w >= 31 || (sim_weapons[w].type != 3 && sim_weapons[w].type != 4)) { g_follow = 0; return 0; }
    g_follow = g_sim.shots[k].id;
    return 1;
}
static int followed(void)   /* 0x100469b0: the followed projectile while it flies, else none */
{
    int k = shot_by_id(g_follow);
    if (k < 0) g_follow = 0;
    return k;
}
/* the player's sim events -> messages + Betty, every sim step (the engine posts them from the simulation, 0x10015f50 /
 * 0x100167b0 / 0x1001e4fe - not from the HUD: they had waited for the HUD to be drawn, so the F11 HUD-off and the outside
 * views held them back) */
static void hud_message_v2(const char *m, int voice, int suffix);
static void player_messages(void)
{
    const combat_unit *pu = &g_sim.player_unit;
    /* events -> messages + Betty (message table 0x1024f144) */
    /* 0x10015f50: "Heat level critical" once per latch (65-80, net gain), "Shutting down" (message 0xd) when the
     * sequence shuts the mech down */
    /* DOS 0x00024f90 also sounds MECOVHW1 with this warning (FUN_00040c50(0xe9, 100, 0x40, 5, 0x50) after message
     * 0); the 3Dfx 0x10015f50 dropped the call (only 0x100161cf message 0). DOSBox YELLSCN1 (held group fire, SB16
     * via the AVI): MECOVHW1 at the warning AND 1.2 s later at the sequence (shutdown 6.0 s after the second) -
     * so the DOS look plays both, the 3D looks the 3Dfx rule. DOS reaches the warning at any heat from 66 once a
     * sequence is pending (combat_heat_dos): a jump past 80 in one frame gives the sequence's alarm, then the warning's
     * on the next frame of net gain */
    if (pu->heat_warning) { hud_message_v("Heat level critical", 64); if (combat_heat_dos && g_sfx) sfx_play(g_sfx, 0xe9, SFX_V64, 0.0f); g_sim.player_unit.heat_warning = 0; }
    if (pu->auto_shutdown) { hud_message_v("Shutting down", 14); g_sim.player_unit.auto_shutdown = 0; }
    /* 0x10016175: with message 3 the overheat alarm MECOVHW1 0xe9 (0x10032b60: edx 0x64 volume, pan 0x40 centre,
     * 0x50 the channel priority) - only when the sequence starts with the latch 0x1000 clear (combat.c seq_alarm: cleared
     * at or below 65 with no sequence pending, so a sequence restarted before such a frame is silent) */
    if (pu->seq_alarm) { hud_message_v("Shutdown sequence initiated", 15); if (g_sfx) sfx_play(g_sfx, 0xe9, SFX_V64, 0.0f); g_sim.player_unit.seq_alarm = 0; }   /* message 3 (DOS) */
    /* 0x1001e4fe: OVERRIDE while a sequence is pending (flag 4, not 8) and not shut down: MECBSOXX 0xcd (0x32) */
    if (pu->override && !g_last_override) { hud_message_v("Shutdown sequence overridden", 78); if (g_sfx) sfx_play(g_sfx, 0xcd, SFX_V64, 0.0f); }   /* 0x1001e505: volume 0x64 */
    /* critical hits on the player (0x100167b0 -> 0x100318a0(part)): "Critical hit : " (voice 54, 0x1024f1c4) and
     * the part from table 0x1024f4e8 {voice, text} - both said, the texts joined by a space and a "." (so a double
     * space after the colon, as the engine's strcat) */
    if (pu->ammo_blown && g_sfx && !pu->shutdown && !pu->offline) sfx_play(g_sfx, 0xd0, SFX_V64, 0.0f);   /* 0x10016afc (state 2): (0xd0, 0x64, 0x40, 5, 0x50) */
    if (pu->ammo_blown) { hud_message_v2("Critical hit :  internal ammo explosion detected.", 54, 48); g_sim.player_unit.ammo_blown = 0; }   /* entry 1 (0x10016b5d) */
    {
        static const struct { short v; const char *t; } PART[0x24] = {
            {20, "weapon destroyed"}, {48, "internal ammo explosion detected"}, {58, "NARC missle probe"}, {59, "anti-missle suite"},
            {21, "jump-jet exhaust port"}, {22, "heat-sink"}, {23, "life-support"}, {24, "engine"}, {25, "gyro"}, {26, "sensors"},
            {27, "leg"}, {28, "hip"}, {29, "arm"}, {30, "torso"}, {31, "left leg"}, {32, "right leg"}, {33, "left arm"},
            {36, "right arm"}, {37, "left torso"}, {38, "right torso"}, {39, "center torso"}, {40, "head"}, {41, "LRM"},
            {43, "SRM"}, {44, "streak SRM"}, {45, "machine gun"}, {46, "gauss rifle"}, {47, "LBX auto cannon"},
            {49, "Ultra auto cannon"}, {50, "particle cannon"}, {51, "ER large laser"}, {52, "ER medium laser"},
            {53, "ER small laser"}, {55, "large pulse laser"}, {56, "medium pulse laser"}, {57, "small pulse laser"}};
        /* a weapon (item / 100 < 50): its type (byte table 0x100171ac -> 0x16..0x23; -1 none), then "weapon destroyed";
         * equipment by item / 10 x 10: 5300 sensors, 5350-5450 arm, 5500 hip, 5550-5650 leg, 5700 sensors, 5800 gyro,
         * 5850 engine, 5900 life-support, 6000 heat-sink, 7000 jump-jet exhaust port (0x100167b0's branches) */
        static const signed char WPN[28] = {0x16, 0x16, 0x16, 0x16, 0x17, 0x17, 0x17, 0x18, 0x18, 0x18, 0x19, 0x1a, 0x1b, 0x1b,
                                            0x1b, 0x1b, 0x1c, 0x1c, 0x1c, 0x1c, -1, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23};
        int q;
        for (q = 0; q < pu->crit_said_n && q < 8; q++) {
            uint32_t id = pu->crit_said[q], eq = id / 10 * 10;
            int parts[2] = {-1, -1}, np, j;
            if (id / 100 < 50) { if (id / 100 < 28) parts[0] = WPN[id / 100]; parts[1] = 0; }
            else parts[0] = eq == 5300 || eq == 5700 ? 9 : eq <= 5450 ? 0xc : eq == 5500 ? 0xb : eq <= 5650 ? 0xa :
                            eq == 5800 ? 8 : eq == 5850 ? 7 : eq == 5900 ? 6 : eq == 6000 ? 5 : eq == 7000 ? 4 : -1;
            for (np = 0; np < 2; np++) {
                char line[80];
                if ((j = parts[np]) < 0) continue;
                if (np == 1 || parts[1] < 0)   /* the alarm 0x10032b60(0xd0) before the last call, mech up (state 2) */
                    if (g_sfx && !pu->shutdown && !pu->offline) sfx_play(g_sfx, 0xd0, SFX_V64, 0.0f);   /* volume 0x64, centred (0x10016a46..0x10017097) */
                snprintf(line, sizeof line, "Critical hit :  %s.", PART[j].t);
                hud_message_v2(line, 54, PART[j].v);
            }
        }
        g_sim.player_unit.crit_said_n = 0;
    }
    g_last_override = pu->override;
}
/* per simulated frame: the self-destruct countdown (state 7); the ejection's messages (0x10016280: state 5 on a
 * breathable planet message 0xc "Ejecting", end status 2; automatic ejection on a hostile one message 0x20, status 4)
 * and camera mode 4 for state 5 (0x1001adf0) */
static void player_upkeep(void)
{
    static int last_ej, last_out;
    combat_unit *pu = &g_sim.player_unit;
    if (g_sd_at >= 0 && g_sim.now > g_sd_at) { g_sd_at = -1; player_eject(0); }
    if (pu->ejected != last_ej) {
        if (pu->ejected == 1) hud_message_v("Ejecting", 12);
        else if (pu->ejected == 2) hud_message_v("Atmosphere hostile : ejection aborted", 63);
    }
    if (pu->intact_out && !last_out) g_camode = 4;
    /* destroyed (state 4, 0x1001a180): the end flag, then 0x1002df20 - with the flag set the camera goes to mode 1, the
     * external orbit (DAT_1024e67c, -1 -> 1), which spins by itself (0x1002dc90); DOSBox: the view leaves the cockpit at
     * once and circles the player's (standing) mech through the explosions; the HUD goes, the message lines stay */
    if (pu->destroyed && !pu->intact_out && g_camode != 1 && g_camode != 4) g_camode = 1;
    last_ej = pu->ejected; last_out = pu->intact_out;
}
/* msim: the player's death sequence is starting (before its explosion). The engine's player input 0x1001adf0 (state 4:
 * 0x1001bd80, the end flag, 0x1002df20 -> camera mode 1) runs before the unit updates in each frame (0x1000908e,
 * 0x10009093), so a death that happened before them (weapons, area damage) has the camera mode switched before case 4's
 * explosion - its sound not halved (0x1004643b: camera mode 0 only); a death inside the player's own update (heat; the
 * DOS self-destruct, at once in 0x1001a180's input check) explodes first, still in the cockpit (DOSBox: halved), and
 * the camera follows (player_upkeep). The 3D editions' self-destruct (state 7 -> 0x100174f0 in case 7) is seen by
 * the next frame's input handler first (player_upkeep switches at once). */
static void on_player_down(void *u)
{
    (void)u;
    if (g_sim.player_unit.down_in_update || g_sim.player_unit.intact_out) return;
    if (g_camode != 1 && g_camode != 4) g_camode = 1;
}
/* the engine's servo (0x1003b7a0 sets it up: {time, target, value, tau = seconds x 181 ticks, wrap}): each call moves
 * the value by (now - time) x (target - value) / tau, never past the target; 0x1003b900 is the same for an angle (the
 * difference taken the short way round, the result wrapped) */
typedef struct { int32_t t; float target, value, tau; } cam_servo;
static cam_servo g_srv_x, g_srv_y, g_srv_z, g_srv_yaw, g_srv_pitch;
static void servo_set(cam_servo *s, float value, int32_t now, float secs) { s->t = now; s->target = s->value = value; s->tau = secs * 181.0f; }
static float servo(cam_servo *s, float target, int32_t now)
{
    float diff = target - s->value, step = (float)(now - s->t) * 0.182f * diff / s->tau;
    s->target = target; s->t = now;
    if (fabsf(diff) < 1e-7f || fabsf(step) >= fabsf(diff)) s->value = target;
    else s->value += step;
    return s->value;
}
static float servo_angle(cam_servo *s, float target, int32_t now)
{
    float diff = fmodf(target - s->value, 360.0f), step;
    if (diff > 180.0f) diff -= 360.0f; else if (diff < -180.0f) diff += 360.0f;
    step = (float)(now - s->t) * 0.182f * diff / s->tau;
    s->target = target; s->t = now;
    if (fabsf(step) >= fabsf(diff)) s->value += diff; else s->value += step;
    s->value = fmodf(s->value, 360.0f);
    return s->value;
}
/* the camera jolt (engine 0x1001c120 -> 0x1002daf0 keyframes, run by 0x1002d7c0 / 0x1002d920 each frame): started only
 * while none runs (0x1002db60). The direction (x, y, z) is normalised; the angle (camera +0x10, the pitch, positive up)
 * = y x 30 when x and z are 0, else (y - z) x 5 (+0x10 positive = nose DOWN: a landing nods the view down 30, then up 15);
 * the offsets = the direction x -25 cm. Three keyframes: the offsets /
 * angle over 0.2 s, then -0.5 x them over 0.5 s, then 0 over 0.2 s (durations x 181 ticks, truncated: 36 / 90 / 36);
 * each keyframe's servo (0x1003b7a0, tau = its duration) starts from where the last one left off, and a keyframe
 * ends when the frame ticks summed since it began reach its duration. The camera is the base view + the servos
 * while it runs. DOSBox YELLSCN1 (31-tick frames): the first keyframe passes before its servo moves (started with
 * dt 0, over after the next frame), so only the -15 / +12.5 keyframe shows - the view tilts up 4 / 7 / 10 degrees
 * over three frames and snaps back (measured horizon 37 / 64 / 89 px at 1024x768; this model at those frame times:
 * 5.2 / 8.6 / 10.8 degrees). At 30 fps all three keyframes show */
static float g_jolt_view[4];   /* this frame's jolt offsets */
static struct { int n, cur, active; int32_t acc, prev; float key[3][4]; int32_t dur[3]; cam_servo s[4]; float off[4]; } g_jolt;
static void jolt_key(int k, int32_t now)
{
    int c;
    for (c = 0; c < 4; c++) { g_jolt.s[c].t = now; g_jolt.s[c].target = g_jolt.key[k][c]; g_jolt.s[c].tau = (float)g_jolt.dur[k]; g_jolt.s[c].value = g_jolt.off[c]; }
}
static void jolt_start(float x, float y, float z, int32_t now)
{
    float l = sqrtf(x * x + y * y + z * z), a;
    int c;
    if (g_jolt.active) return;
    if (l > 1e-4f) { x /= l; y /= l; z /= l; }
    a = (x == 0.0f && z == 0.0f) ? y * 30.0f : (y - z) * 5.0f;
    g_jolt.key[0][0] = x * -25.0f; g_jolt.key[0][1] = y * -25.0f; g_jolt.key[0][2] = z * -25.0f; g_jolt.key[0][3] = a;
    for (c = 0; c < 4; c++) { g_jolt.key[1][c] = -0.5f * g_jolt.key[0][c]; g_jolt.key[2][c] = 0.0f; g_jolt.off[c] = 0.0f; }
    g_jolt.dur[0] = 36; g_jolt.dur[1] = 90; g_jolt.dur[2] = 36;
    g_jolt.n = 3; g_jolt.cur = 0; g_jolt.acc = 0; g_jolt.prev = now; g_jolt.active = 1;
    jolt_key(0, now);
}
/* once a frame: advance (0x1002d7c0) and return the offsets (x, y, z cm, pitch degrees); 0 when none runs */
static int jolt_step(int32_t now, float off[4])
{
    int c;
    if (!g_jolt.active) { off[0] = off[1] = off[2] = off[3] = 0.0f; return 0; }
    g_jolt.acc += (int32_t)lrintf((float)(now - g_jolt.prev) * 0.182f);
    g_jolt.prev = now;
    if (g_jolt.acc >= g_jolt.dur[g_jolt.cur]) {
        g_jolt.acc = 0;
        if (++g_jolt.cur >= g_jolt.n) { g_jolt.active = 0; off[0] = off[1] = off[2] = off[3] = 0.0f; return 0; }
        jolt_key(g_jolt.cur, now);
    }
    for (c = 0; c < 4; c++) off[c] = g_jolt.off[c] = servo(&g_jolt.s[c], g_jolt.key[g_jolt.cur][c], now);
    return 1;
}
/* The red palette flash (engine 0x1002ad50 / 0x1002acf0, DOS 0x3e7d0 / 0x3e760): a fade of the palette to the mission's
 * PALG slot 0x11 (every colour pure red but the grey ramp 0xf0-0xff) over dur / 2 ticks (the frames counted from the
 * frame's ticks when it starts), then back to the palette before:
 *  - at death (controller state 4, 0x1001bd80 / DOS 0x28910, once): dur 0x16a ticks (DOS 0x16c);
 *  - on DAT_1024cce0 (combat_unit red_flash: the player's ammunition exploding, or more than 2 points into the head /
 *    centre torso structure; 0x1001e6c0 / DOS 0x32fc0): n = 3 x the flashes so far this mission (DAT_1024cd5c), dur =
 *    n / 15 x 0x16a (0x16c), at most 0x16a - 0.2 s for the first, a full second from the fifth on.
 * The way back: the 3D editions over the same dur / 2 ticks (DAT_1024e46c); DOS passes the fade's FRAME count as its
 * duration in ticks (DAT_000970b4), so its palette snaps back within a few frames (DOSBox: the self-destruct's red builds
 * up for ~3 s of slow explosion frames, then is gone in ~5).
 * The 3D editions draw their textured polygons in the fade palette's fog colour while a fade to 0x11 runs (DAT_1024e494,
 * DAT_10160cf0: (255, 8, 8)), so the world turns red at once and stays so until the fade back begins; nothing else
 * changes - the palette fade itself goes to the Glide driver's empty set-palette entry, and the HUD table and the flat
 * colours are only reloaded (with the same slot) when the fade back ends (world_fade / pal3_upkeep). DOS fades
 * everything through the palette. Both callers pass mode 1 (there and back: 0x1001bd80 / 0x1001bdd0 push 1, DOS 0x28915 /
 * 0x28960 BL = 1), and a mode-1 call while a fade runs (DAT_1024e470 / DAT_000970b8 > 0) loads no source palette and
 * returns 0 (0x1002ad50: lpMem = 0; DOS 0x3e7f4..0x3e80b: esi = 0 -> 0x3e830): the flash is dropped - during another
 * flash (its way back included), the light amplification fade (0xb5 ticks) or the DOS time-of-day fades; the counter
 * DAT_1024cd5c still counts it (0x1001e6c0). Only a mode-0 call (the time of day, light amplification, 0x1002b090)
 * starts from the colours reached (DAT_1007c6b8 / DAT_0014fe0c) with no way back (DAT_1024e468 = -1). */
static int32_t g_flash_t0 = -1;          /* the fade in's start (sim ms); -1 none */
static float   g_flash_from, g_flash_in, g_flash_back;   /* start level; fade in / back lengths (ticks) */
static int     g_flash_n;                /* DAT_1024cd5c */
static float   g_frame_ticks = 182.0f / 30.0f;   /* the last frame's ticks (DAT_1024ab4c) */
static float flash_level(int32_t now, int *in_phase)
{
    float k;
    if (in_phase) *in_phase = 0;
    if (g_flash_t0 < 0) return 0;
    k = (float)(now - g_flash_t0) * 0.182f;
    if (k < g_flash_in) { if (in_phase) *in_phase = 1; return g_flash_from + (1.0f - g_flash_from) * k / g_flash_in; }
    k -= g_flash_in;
    if (k < g_flash_back) return 1.0f - k / g_flash_back;
    return 0;
}
/* Leaving the mission (DOS MW2.EXE 0x15110 -> 0x28980 once the loop ends): the screen's colours fade (VFX 0x58e0d, 0x5a
 * steps) to the PALG slot 0x10 palette - all black - or, when the end status has bit 4 (DAT_000a564d = 4: an automatic
 * ejection aborted by a hostile atmosphere, 0x25340), to slot 0x11 (every colour pure red but the grey ramp), and that
 * palette is set (0x3e710). Only the colours in the 3D window's buffer (0xa46bc) fade: DOSBox (YELLSCN1 eject, "Press any
 * key to exit...", a key) - the ground 125 -> 93 -> 52 -> 8 -> 0 at 0.5 s steps, the message bar's grey unchanged until
 * the screen goes black. 0x5a steps taken as vertical retraces at 70 Hz: 1.29 s (INFERRED from DOSBox's ~1.3 s). The 3Dfx
 * DLL has no such fade. Returns the level 0-1 (1 when no fade runs); *red: towards red. */
static int32_t g_exit_fade_t0 = -1;   /* sim ms; -1 none */
static float exit_fade_level(int *red)
{
    float k;
    if (red) *red = g_sim.player_unit.ejected == 2;   /* career status 4 (0x25340) */
    if (g_exit_fade_t0 < 0) return 1.0f;
    k = (float)(g_sim.now - g_exit_fade_t0) / 1290.0f;
    return k < 0 ? 0 : k > 1 ? 1 : k;
}
static int32_t g_dos_fade_len;          /* (the DOS time-of-day / light amplification fade, dos_time_of_day) */
static int32_t g_amp_fade_end = -1;      /* the 3D editions' light amplification fade (0x1002b090(.., 0xb5)): its end, sim ms */
static int fade_running(int32_t now)     /* DAT_1024e470 / DAT_000970b8 > 0 */
{
    if (g_flash_t0 >= 0 && (float)(now - g_flash_t0) * 0.182f < g_flash_in + g_flash_back) return 1;
    if (g_tex_kind == 3) return g_dos_fade_len > 0;
    return g_amp_fade_end >= 0 && now < g_amp_fade_end;
}
static void red_flash(int dur, int32_t now)
{
    int dos = g_tex_kind == 3;
    float half = (float)(dur >> 1), frames = g_frame_ticks > 0 ? half / g_frame_ticks : 20.0f;
    if (fade_running(now)) {   /* mode 1 while a fade runs: dropped (0x1002ad50 / DOS 0x3e7d0 return 0) */
        if (getenv("MW2_STATE_DUMP")) printf("FLASH_DROPPED now=%d dur=%d n=%d\n", (int)now, dur, g_flash_n);   /* TEST hook */
        return;
    }
    g_flash_from = flash_level(now, NULL);
    if (dos) frames = floorf(frames);   /* the 16.16 frame count, at least 1 */
    if (frames < 1) frames = 1;
    g_flash_t0 = now;
    g_flash_in = half;
    g_flash_back = dos ? frames : half;
    if (getenv("MW2_STATE_DUMP")) printf("FLASH now=%d dur=%d in=%.0f back=%.0f from=%.2f n=%d\n", (int)now, dur, (double)g_flash_in, (double)g_flash_back, (double)g_flash_from, g_flash_n);   /* TEST hook */
}
static void flash_upkeep(void)
{
    static int dead_done;
    static int32_t last_now = -1;
    combat_unit *pu = &g_sim.player_unit;
    int32_t now = g_sim.now;
    if (last_now < 0 || now < last_now) { g_flash_t0 = -1; g_flash_n = 0; dead_done = 0; g_amp_fade_end = -1; }   /* a new mission */
    if (now > last_now && last_now >= 0) g_frame_ticks = (float)(now - last_now) * 0.182f;
    last_now = now;
    /* 0x1001e6c0 -> 0x1001bda0 (DOS 0x32fc0 -> 0x28930): taken by the cockpit update (0x1001e340 / DOS 0x32c50), which
     * runs only while the controller state is neither 4 (destroyed) nor 5 (ejected) - a flash pending as the player dies
     * is never shown (nor counted); the death's own fade below is the only one */
    if (pu->red_flash && pu->destroyed) pu->red_flash = 0;
    if (pu->red_flash) {
        int n = 3 * ++g_flash_n, dur;
        pu->red_flash = 0;
        if (g_tex_kind == 3) dur = n < 16 ? (int)(((int64_t)((n << 16) / 15) * 0x16c + 0x8000) >> 16) : 0x16c;
        else dur = n < 16 ? (int)((float)n * 0.06666667f * 362.0f) : 0x16a;
        red_flash(dur, now);
    }
    /* the death's fade: controller state 4's first frame (0x1001adf0 -> 0x1001bd80) */
    if (pu->destroyed && !pu->intact_out && !dead_done) {   /* state 4: 0x1001bd80 (DOS 0x28910) */
        dead_done = 1;
        red_flash(g_tex_kind == 3 ? 0x16c : 0x16a, now);
    }
}
/* The time of day (3Dfx 0x10007870 / 0x10007900, DOS 0x14ee0 / 0x14f80): the group at t seconds into a day of `day`
 * seconds is the last of the boundaries day / 24 x {5, 7, 17, 19} (dawn, day, dusk, night) at or before t, else night
 * (so also for a negative t: both engines take t with idiv's remainder); each group's PALG slot {4, 0, 4, 8} */
static const int TOD_SLOT[4] = {4, 0, 4, 8};
static int tod_group(int tt, int day)
{
    int H = day / 24, g = 3, i;
    const int B[4] = {5 * H, 7 * H, 17 * H, 19 * H};
    for (i = 0; i < 4; i++) if (B[i] <= tt) g = i;
    return g;
}
/* the mission's time of day and palettes as loaded (hud_mission_palette): the PALG slots' PAL records (6-bit), the day
 * length PLNT +12 (payload 3), the start (seconds into the day), PLNT +52 (payload 13: the 3D editions' damage shading
 * sign, DAT_1024e428) */
static uint8_t g_pal_slot[20][768];
static int     g_pal_slot_ok[20], g_tod_day, g_tod_start, g_plnt52;
static int     g_hud_slot = -1;   /* the slot the HUD's colours are from */
static void hud_slot(int slot)    /* the engine's 0x1002ac40: the HUD table (and g_hp) from the PALG slot's record */
{
    int k;
    if (slot < 0 || slot >= 20 || !g_pal_slot_ok[slot] || !g_hud) return;
    hud_repalette(g_hud, g_pal_slot[slot]);
    for (k = 0; k < 256; k++) hud_pal_color(g_hud, k, g_hp[k]);
    g_hud_slot = slot;
}
/* The 3D editions' palette (engine 0x1002ad50 / 0x1002acf0 / 0x1002ac00 / 0x1002ac40). A fade steps the 8-bit palette
 * buffer 0x1007c6b8 once a frame (0x10061480) and hands it to the display driver's set-palette entry (+0x18) - an empty
 * function in the Glide driver (table 0x1024a778: 0x10005b20 xor eax / ret; +0x1c 0x10005b30 the same): nothing on screen
 * fades. What shows:
 *  - DAT_1024e494 / DAT_10160cf0, set when a fade STARTS by its target slot (0xc light amplification (0, 255, 8), 0x10
 *    black, 0x11 the red flash (255, 8, 8), any other slot: off): every polygon drawn by 0x1002c880 (the textured ones,
 *    billboards, effect cards) and the sky / ground fills (0x1002d460 / 0x1002d590) take that colour as their vertex
 *    colour - unlit, at once;
 *  - the flat polygons' colours 0x101528c0 (0x100272e0, 0x10027490..: the palette's colours as floats, 0x10066840) and the
 *    HUD's 16-bit table 0x10260324 change only when 0x1002ac40 reloads them from a PALG slot's PAL record: when a fade
 *    that does not return ends (0x1002acf0 sets DAT_1024e458, 0x1002ac00 the next frame), with the slot of the last
 *    0x1002b090 call (DAT_1024e460).
 * So light amplification (0x10007a50 -> 0x1002b090(0xc, 0xb5)): the textures green at once, the flat colours and the HUD
 * in the INFRARED record 0xb5 ticks later; off (0x1002b090(0, 0xb5), then the time of day re-evaluated the next frame,
 * 0x100079d0 -> 0x1002b090(its slot, 0xb5)): the textures at once, the flat colours and the HUD 0xb5 ticks later in the
 * slot of the time of day THEN. The red flash (0x1002ad50(0x11, dur, 1) from 0x1001bd80 / 0x1001bda0): the textures red
 * for the fade in (dur / 2), then the fade back to the slot before (DAT_1024e468) - green again if that was 0xc; the flat
 * colours and the HUD never change (reloaded with the same slot when the fade back ends). A fade started while another
 * runs replaces it (DAT_1024e470). The time of day itself is evaluated once in the 3D editions: 0x100079d0 acts only
 * while DAT_1024a980 is -1, which it sets to the group - only light amplification's off resets it (0x10007a50) - so the
 * day / night cycle does not run (the DOS edition's does, dos_time_of_day). */
static struct { int cur, want, flash_green; int32_t reload_at, flash_t0; float amp_t0; } g_pal3 = {-1, -1, 0, -1, -1, 0};
static void pal3_upkeep(int amp_now, int amp_was)
{
    int32_t now = g_sim.now;
    if (amp_now != amp_was) {   /* 0x10007a50 */
        if (amp_now) g_pal3.want = 0xc;
        else if (g_tod_day > 0) g_pal3.want = TOD_SLOT[tod_group((int)((int64_t)now * 182 / 1000 / 0xb5 + g_tod_start) % g_tod_day, g_tod_day)];   /* 0x10007900 */
        else g_pal3.want = 0;
        if (!g_pal_slot_ok[g_pal3.want]) g_pal3.want = 0;
        g_pal3.reload_at = now + 0xb5 * 1000 / 182;
        g_amp_fade_end = g_pal3.reload_at;      /* (a red flash meanwhile is dropped, red_flash) */
        if (g_flash_t0 >= 0) g_flash_t0 = -1;   /* the running flash fade is replaced (mode 0 from the colours reached, DAT_1024e470) */
    }
    if (g_flash_t0 >= 0 && g_flash_t0 != g_pal3.flash_t0) {   /* a red flash starts: its fade back ends with the reload */
        g_pal3.flash_t0 = g_flash_t0;
        g_pal3.flash_green = g_pal3.cur == 0xc && g_pal3.reload_at < 0;   /* the slot before (DAT_1024e468) is 0xc */
        g_pal3.reload_at = g_flash_t0 + (int32_t)((g_flash_in + g_flash_back) * 1000.0f / 182.0f);
    }
    if (g_pal3.reload_at >= 0 && now >= g_pal3.reload_at) {   /* 0x1002ac00 -> 0x1002ac40 */
        g_pal3.reload_at = -1;
        /* the S3 engine's reload (0x1003ec50 -> 0x1003ec90, also with the same slot) ends its texture cache's readings:
         * each texture is uploaded again in the format of the next polygon that draws it (c3d_texture_cache_reset) */
        if (g_edition == PORTCFG_ED_S3) c3d_texture_cache_reset(&g_scene);
        if (g_pal3.want != g_pal3.cur) {
            g_pal3.cur = g_pal3.want;
            hud_slot(g_pal3.cur);
            if (getenv("MW2_STATE_DUMP")) printf("PALSLOT now=%d slot=%d\n", (int)now, g_pal3.cur);   /* TEST hook */
        }
    }
}
static int g_vision;   /* (defined with the DOS time of day) */
static void world_fade(glr *r)
{
    static int amp;
    static int32_t prev = -1;
    int in_ph, want = g_vision == 1 && g_cockpit && g_sim_ok && g_world;
    float k;
    const float *tint = NULL;
    static const float RED[3] = {1.0f, 8.0f / 255.0f, 8.0f / 255.0f}, GREEN[3] = {0.0f, 1.0f, 8.0f / 255.0f};
    if (g_tex_kind == 3) { glr_set_world_fade(r, NULL, 0, NULL); return; }
    if (!g_sim_ok || !g_world) { glr_set_world_fade(r, NULL, 0, NULL); return; }
    if (c3d_ti) {   /* TEXTURES TITANIUM (mercsw.dll, not traced here): as before - the palette colours faded k of the way
                     * to slot 0x11 / 0xc, the textures tinted (the flash while it fades in, light amplification at once) */
        static int tamp;
        static int32_t tamp_t0 = -1;
        static float tamp_from;
        float ak;
        if (tamp_t0 >= 0 && g_sim.now < tamp_t0) { tamp = 0; tamp_t0 = -1; }
        ak = tamp_t0 < 0 ? 0.0f : tamp_from + ((tamp ? 1.0f : 0.0f) - tamp_from) * fminf(1.0f, (float)(g_sim.now - tamp_t0) * 0.182f / 0xb5);
        if (want != tamp) { tamp_from = ak; tamp = want; tamp_t0 = g_sim.now; }
        k = flash_level(g_sim.now, &in_ph);
        if (k > 0) glr_set_world_fade(r, g_scene.slot_ok[0x11] ? (const uint8_t (*)[3])g_scene.slotpal[0x11] : NULL, k, in_ph ? RED : NULL);
        else glr_set_world_fade(r, g_scene.slot_ok[0xc] ? (const uint8_t (*)[3])g_scene.slotpal[0xc] : NULL, ak, tamp ? GREEN : NULL);
        return;
    }
    if (prev < 0 || g_sim.now < prev || g_pal3.cur < 0) {   /* a new mission: the start slot (hud_mission_palette) */
        amp = 0;
        g_pal3.cur = g_pal3.want = g_hud_slot >= 0 ? g_hud_slot : 0;
        g_pal3.reload_at = -1; g_pal3.flash_t0 = -1; g_pal3.flash_green = 0;
    }
    prev = g_sim.now;
    pal3_upkeep(want, amp);
    amp = want;
    k = flash_level(g_sim.now, &in_ph);
    if (k > 0 && in_ph) tint = RED;                              /* the fade to 0x11 */
    else if (k > 0) tint = g_pal3.flash_green ? GREEN : NULL;   /* the fade back to the slot before */
    else if (amp) tint = GREEN;                                  /* the fade to 0xc and after */
    /* the flat colours: the slot 0x1002ac40 last loaded (the scene's palette is the mission's _DA record) */
    glr_set_world_fade(r, g_pal3.cur >= 0 && g_scene.slot_ok[g_pal3.cur] ? (const uint8_t (*)[3])g_scene.slotpal[g_pal3.cur] : NULL, 1.0f, tint);
}
/* The scrub patch (a record's SCRG object, e.g. TNJ1SCRI: 3 x 3 tiles of TNJ1SCR1 sprites; engine 0x10011570 every frame,
 * DOS 0x1f270): it follows the camera on a grid of world3d_scrg_step - once the camera is more than (step / 8 + step) / 2
 * from the patch's position on either axis, the patch moves to the camera's nearest grid point (rounded half away from
 * zero). TERRAIN TEXTURES off (0x100116d0 / 0x10065340): the patch is unlinked (not drawn) and stops following; on again:
 * linked and following. The rounding branch is the object's +4 non-zero one (0x100114c2 / DOS 0x1f120: +4 is the object's
 * first child) - every SCRG object in both archives (17 DOS, 18 3D records) has its 8 tiles as children, so it is always
 * taken; a patch has no collision. */
static void scrub_follow(glr *r, float camx, float camz)
{
    static const mech3d_part *seen;
    static int gx, gz, cellx, cellz, off;
    int k, any = 0, step = world3d_scrg_step, h, x = (int)camx, z = (int)camz, nx, nz, moved = 0;
    if (seen != g_mech.parts) { seen = g_mech.parts; gx = gz = cellx = cellz = 0; off = 0; }   /* a new world */
    for (k = 0; k < g_mech.part_count; k++) if (g_mech.parts[k].scrg) { any = 1; break; }
    if (!any || step <= 0) return;
    if (g_notex_world != off) {   /* detach / attach */
        off = g_notex_world;
        for (k = 0; k < g_mech.part_count; k++) if (g_mech.parts[k].scrg) g_mech.parts[k].hidden = off;
        moved = 1;
    }
    if (!off) {
        h = ((step >> 3) + step) >> 1;
        nx = cellx; nz = cellz;
        if (abs(x - gx) > h || abs(z - gz) > h) {
            nx = (x + (x < 0 ? -step : step) / 2) / step;
            nz = (z + (z < 0 ? -step : step) / 2) / step;
        }
        if (nx != cellx || nz != cellz) {
            int dx = nx * step - gx, dz = nz * step - gz;
            for (k = 0; k < g_mech.part_count; k++)
                if (g_mech.parts[k].scrg) { g_mech.parts[k].pos[0] += dx; g_mech.parts[k].pos[2] += dz; }
            gx += dx; gz += dz; cellx = nx; cellz = nz;
            moved = 1;
            if (getenv("MW2_STATE_DUMP")) {   /* TEST hook: the grid point and the patch's first piece */
                const mech3d_part *pp = NULL;
                for (k = 0; k < g_mech.part_count && !pp; k++) if (g_mech.parts[k].scrg) pp = &g_mech.parts[k];
                printf("SCRG now=%d at %d,%d piece %d,%d\n", (int)g_sim.now, gx, gz, pp ? (int)pp->pos[0] : 0, pp ? (int)pp->pos[2] : 0);
            }
        }
    }
    if (moved) glr_set_mech(r, &g_mech, (const uint8_t (*)[3])g_scene.palette, g_scene.slot, g_camo, g_clan);
}
/* The animated bank-1 slots (fires, smoke, flags: MAROMAP1 0x9b..0x9e a9fir201..217 at 10..16 ticks a frame), engine
 * 0x1002ce50 every frame (0x1000915e): from the slot's first update a frame every rate ticks round its group (a cel
 * id < 1 ends it), mode 2 once - back to frame 0 and still at the wrap; rate 0 or mode 0: still. Here from the mission
 * clock (182 ticks a second). The Titanium engine does the same with its own map's groups: mercsw 0x10001c74 (from
 * 0x100475b8, every frame; slot table 0x102e7de0 x 0x12: group, frame, rate, mode, state, time; groups 0x102879c0, 32
 * frames of 0x18) steps (now - time) / rate frames, wrapping at a cel id < 1, mode 2 stopping at frame 0; its chunk
 * handlers 0x100719c8.. (BMPJ 0x10001de2, BMID 0x10001efc: frame 0, mode 1, rate 0x2d for 2+ frames, BSEC 0x10002182 +
 * 0x10002094(1), BMEN 0x10002094) as the 3D editions' - so the Titanium table (c3d_ti_draw) is stepped alike */
static void scene_anim(glr *r)
{
    int i;
    long t = (long)((int64_t)g_sim.now * 182 / 1000);
    for (i = 0; c3d_ti && c3d_ti_have_scene && i < GLR_SLOTS; i++) {   /* the Titanium look's own table */
        long st;
        int f;
        if (c3d_ti_scene.nfr[i] < 2 || c3d_ti_scene.rate[i] <= 0 || c3d_ti_scene.mode[i] == 0) continue;
        st = t / c3d_ti_scene.rate[i];
        f = c3d_ti_scene.mode[i] == 2 && st >= c3d_ti_scene.nfr[i] ? 0 : (int)(st % c3d_ti_scene.nfr[i]);
        if (c3d_ti_scene.fr[i][f]) c3d_ti_draw[i] = c3d_ti_scene.fr[i][f];
        if (getenv("MW2_ANIM_LOG") && i == (int)strtol(getenv("MW2_ANIM_LOG"), NULL, 0)) {   /* TEST hook: one slot's frame */
            static int last = -1;
            if (f != last) { last = f; printf("TIANIM t=%.3f slot %#x frame %d of %d rate %d\n", g_sim.now / 1000.0, i, f, c3d_ti_scene.nfr[i], c3d_ti_scene.rate[i]); }
        }
    }
    for (i = 0; i < GLR_SLOTS; i++) {
        long st;
        int f;
        if (g_scene.nfr[i] < 2 || g_scene.rate[i] <= 0 || g_scene.mode[i] == 0) continue;
        st = t / g_scene.rate[i];
        f = g_scene.mode[i] == 2 && st >= g_scene.nfr[i] ? 0 : (int)(st % g_scene.nfr[i]);
        glr_slot_frame(r, i, g_scene.fr[i][f]);
        if (getenv("MW2_ANIM_LOG") && i == (int)strtol(getenv("MW2_ANIM_LOG"), NULL, 0)) {   /* TEST hook: one slot's frame */
            static int last = -1;
            if (f != last) { last = f; printf("ANIM t=%.3f slot %#x frame %d of %d rate %d\n", g_sim.now / 1000.0, i, f, g_scene.nfr[i], g_scene.rate[i]); }
        }
    }
}
/* the camera modes, after the cockpit / chase camera is set (engine 0x1002dc90) */
static void camera_modes(glr_view *v)
{
    float dt = (float)(g_sim.now - g_cam_now) * 0.182f;   /* ticks this frame (0 while paused) */
    int32_t now = g_sim.now;
    g_cam_now = now;
    if (g_camode == 3) {   /* 0x1002e210: AT the projectile, level, along its course (0x10045b90 -> DAT_1007dc50), zoom 2.0 */
        int k = followed();
        if (k < 0) g_camode = g_cam_prev;   /* gone: back to the mode before (DAT_1024e664) */
        else {
            const msim_shot *ms = &g_sim.shots[k];
            v->free_cam = 1;
            v->eye[0] = ms->x; v->eye[1] = ms->y; v->eye[2] = ms->z;
            v->look_yaw = atan2f(ms->vx, ms->vz) * 180.0f / 3.14159265f;
            v->look_pitch = 0.0f;
        }
    }
    if (g_camode == 4) {
        /* 0x1002e7d0: over the unit (+0x50 / +0x58), looking straight down (pitch 90); the height climbs from the camera's
         * at an acceleration of DAT_1024e698 = 0.2368 cm per tick^2 (78 m/s^2: 10 m in 0.5 s, 1 km in 5 s); the yaw
         * turns by 0.007 deg x the ticks since the start EVERY FRAME (frame-rate bound: 30 fps assumed, DOSBox ~7 fps
         * measured 35 deg by 2.8 s). Restarted (speed, spin) when the end sequence begins (0x100374e0 -> 0x1002e070
         * clears DAT_1024e660) */
        int restart = g_cam_last != 4 || (g_sim.ending && !g_ej_ending);
        if (g_sim.ending) g_ej_ending = 1;
        if (restart) { g_ej_t0 = now; g_ej_vy = 0; v->eye[1] = g_cam_eye[1]; v->look_yaw = g_cam_yaw; dt = 0; }
        else { v->eye[1] = g_cam_eye[1]; v->look_yaw = g_cam_yaw; }
        {
            float el = (float)(now - g_ej_t0) * 0.182f;
            v->eye[1] += (0.5f * 0.2368f * dt + g_ej_vy) * dt;
            g_ej_vy += 0.2368f * dt;
            v->look_yaw -= el * 0.007f * dt / 6.0f;   /* the picture turns clockwise (DOSBox) */
            v->look_yaw = fmodf(v->look_yaw, 360.0f);
        }
        v->free_cam = 1;
        v->eye[0] = (float)g_pl.origin[0]; v->eye[2] = (float)g_pl.origin[2];
        v->look_pitch = -89.9f;
    }
    if (g_camode == 1) {
        /* 0x1002e280 (mode 1, called by 0x1002dc90 with the end flag set): an orbit about the target point - the eye
         * object's origin plus the controller's +0xc4 offset (0x1002e0f0 -> 0x1002eb50: the cockpit eye, g_sim.player_eye)
         * - with the eye object's yaw (0x1002f3b0; DOSBox: the orbit starts BEHIND the mech). Distance DAT_1024e644 =
         * 3 x the unit's radius (+0xe8, the MGEO contact radius; mouse limits 0.5-4 x), height DAT_1024e650 = 0.25 x the
         * distance over the target point, the angle DAT_1024e654 turning 0.25 deg a tick by itself (DAT_102477bc; kept
         * across deaths, starts at 0). Every value goes through the engine's servos (0x1003b7e0 / 0x1003b900, set up by
         * 0x1002db70 with time constants x 181 ticks): the offsets from the target x / z 0.5 s, y 0.7 s, the yaw / pitch
         * 0.2 s (wrapped at 360) - started from the camera as it was (the cockpit eye), so the view glides out of the
         * cockpit, turning to face the mech. The height is kept 5 m over the terrain (2 m where it is at or below 0,
         * 0x1002ded0) */
        float tgt[3], d, h, a, want_yaw, want_pitch, dx, dy, dz, gy, d0;
        if (g_sim.player_eye_ok) memcpy(tgt, g_sim.player_eye, sizeof tgt);
        else { tgt[0] = (float)g_pl.origin[0]; tgt[1] = g_sim.player_unit.y + 950.0f; tgt[2] = (float)g_pl.origin[2]; }
        d0 = 3.0f * (g_sim.player_radius > 0 ? g_sim.player_radius : 545.0f);
        if (g_cam_last != 1 && !g_sim.player_unit.destroyed) hud_message_v("External camera engaged.", 66);   /* 0x1002e2c7: entering, no end flag */
        if (g_orb_d == 0) { g_orb_d = d0; g_orb_h = 0.25f * d0; }   /* the first entry (|DAT_1024e644| == 0): 3 x, 0.25 x the radius */
        if (g_cam_last != 1) {   /* entering mode 1: the servos start from the camera as it stands */
            servo_set(&g_srv_x, g_cam_eye[0] - tgt[0], now, 0.5f);
            servo_set(&g_srv_y, g_cam_eye[1] - tgt[1], now, 0.7f);
            servo_set(&g_srv_z, g_cam_eye[2] - tgt[2], now, 0.5f);
            servo_set(&g_srv_yaw, g_cam_yaw, now, 0.2f);
            servo_set(&g_srv_pitch, g_cam_pitch, now, 0.2f);
            g_cam_zoom = 1.0f;
        }
        /* the held controls x the frame's ticks / 181 (0x1002dc90: x DAT_1024ab4c x 0.0055249); with the end flag (the player
         * destroyed, DAT_1024c568) the angle turns by itself 0.25 deg a tick instead (DAT_102477bc) */
        g_orb_d += g_orb_in[0] * 3600.0f * dt / 181.0f;
        if (g_orb_d > 4.0f * d0) g_orb_d = 4.0f * d0;          /* DAT_1024e64c = 4 x (0x102477d4) */
        else if (g_orb_d < 0.5f * d0) g_orb_d = 0.5f * d0;     /* DAT_1024e648 = 0.5 x (0x102477c8) */
        d = g_orb_d;
        h = g_orb_h + g_orb_in[1] * 3600.0f * dt / 181.0f;
        g_orb_ang = fmodf(g_orb_ang + (g_sim.player_unit.destroyed ? 0.25f * dt : g_orb_in[2] * 72.0f * dt / 181.0f), 360.0f);
        g_orb_in[0] = g_orb_in[1] = g_orb_in[2] = 0;   /* this frame's (the input pass sets them again) */
        /* the view turns toward the target from last frame's camera (0x1001d970) */
        dx = tgt[0] - g_cam_eye[0]; dy = tgt[1] - g_cam_eye[1]; dz = tgt[2] - g_cam_eye[2];
        want_yaw = atan2f(dx, dz) * 180.0f / 3.14159265f;
        want_pitch = atan2f(dy, sqrtf(dx * dx + dz * dz)) * 180.0f / 3.14159265f;
        v->look_yaw = servo_angle(&g_srv_yaw, want_yaw, now);
        v->look_pitch = servo_angle(&g_srv_pitch, want_pitch + 0.5f * g_orb_tilt, now);   /* the engine's pitch is down-positive */
        a = (g_pl.heading + g_pl.twist - g_orb_ang) * 3.14159265f / 180.0f;
        v->free_cam = 1;
        v->eye[0] = tgt[0] + servo(&g_srv_x, -sinf(a) * d, now);
        v->eye[2] = tgt[2] + servo(&g_srv_z, -cosf(a) * d, now);
        gy = msim_ground(&g_sim, v->eye[0], v->eye[2], g_cam_eye[1] + 10000.0f);
        gy += gy > 1e-7f ? 500.0f : 200.0f;
        v->eye[1] = tgt[1] + servo(&g_srv_y, tgt[1] + h < gy ? gy - tgt[1] : h, now);
        /* then the height kept between 200 - the centre height (DAT_1015f034) and 4 x the start distance (DAT_1015efd0) */
        g_orb_h = h > 4.0f * d0 ? 4.0f * d0 : h < 200.0f - g_sim.player_centre_h ? 200.0f - g_sim.player_centre_h : h;
    }
    if (g_camode == 1 || g_camode == 3 || g_camode == 4)
        v->vfov = 2.0f * atanf(tanf(73.74f * 3.14159265f / 360.0f) / g_cam_zoom) * 180.0f / 3.14159265f;
    g_cam_last = g_camode;
    memcpy(g_cam_eye, v->eye, sizeof g_cam_eye);
    g_cam_yaw = v->look_yaw; g_cam_pitch = v->look_pitch;
    /* the sounds' listener and cockpit flag for the next frame (0x1002dc90 / 0x1002e640, see snd_listener) */
    memcpy(g_snd_eye, v->eye, sizeof g_snd_eye);
    g_snd_yaw = v->look_yaw; g_snd_pit = VIEW_IN_COCKPIT; g_snd_ok = 1;
}
/* The voice queue (engine 0x10031150 posts, 0x100313d0 advances, 0x10031440 starts; head DAT_1024f6bc): an entry is one
 * message = up to two voice lines said back to back - a message-table line and its suffix ("Autopilot" + "disengaged",
 * 0x10031870), a point and an order ("Point 2" + "destroyed", 0x10031810), "Critical hit :" + the part (0x100318a0).
 * Priority (entry +0x18) and a duplicate limit from the first voice's record (table 0x1024e6a8 {id, priority, limit}, loaded
 * by 0x10030590 over 0xffff: a voice not listed has priority -1 and limit -1); a new entry goes in before the first queued
 * one of lower priority. Walking the queued entries (the playing head excluded) up to that point, one with the same first
 * voice and the same second (or none) drops the new one when the limit is -1, or when it is the limit-th such (asm
 * 0x1003135b: 0xffff -> drop). The end sequence's flush (0x10031640(1), 264 ticks in) drops queued entries below 0x50,
 * cuts a playing one below 0x50, and from then on nothing is posted (DAT_1024f6c4). */
static const short VQ_TBL[][3] = {   /* 0x1024e6a8 (the later of two records for one voice wins: 64 -> limit -1) */
    {3,80,-1}, {4,80,-1}, {5,80,-1}, {9,80,-1}, {12,80,-1}, {14,80,1}, {15,80,1}, {16,80,-1}, {62,80,-1}, {64,80,-1},
    {68,80,-1}, {75,80,-1}, {88,80,-1}, {89,80,-1}, {90,80,-1}, {91,80,-1}, {92,80,-1}, {93,80,-1}, {95,80,-1},
    {96,80,-1}, {97,80,-1}, {98,80,-1}, {99,80,-1}, {100,80,-1}, {101,80,-1}, {102,80,-1}, {103,80,-1}, {104,80,-1},
    {105,80,-1}, {106,80,-1}, {107,80,-1}, {108,80,-1}, {109,80,-1}, {110,100,-1}, {112,80,-1}, {114,80,-1},
    {115,80,-1}, {116,80,-1}, {117,80,-1}, {118,80,-1}, {120,80,-1}, {121,80,-1}, {131,80,-1}, {135,80,-1},
    {136,80,-1}, {165,80,-1}, {166,80,-1}, {168,80,-1}, {169,80,-1}, {170,80,-1}, {171,80,-1}, {174,80,-1},
    {175,80,-1}, {176,80,-1}, {177,80,-1}, {181,-1,1}, {182,80,-1}, {183,80,-1}, {184,80,-1}, {185,80,-1}, {186,80,-1},
    {188,80,2}, {189,80,-1}, {197,100,1}, {200,80,-1}, {208,100,-1}, {209,80,2}, {210,80,2}, {211,80,2}, {223,0,-1},
    {226,-1,-1}, {227,100,4}, {231,80,-1}, {233,80,-1}, {235,80,2}, {236,80,-1}, {240,80,-1}, {246,80,2}, {249,80,-1},
    {252,80,-1}, {254,80,-1}, {255,80,-1}, {258,80,-1}, {261,80,-1}, {262,80,-1}, {263,80,-1}, {267,80,-1},
    {268,80,-1}, {269,80,-1}, {270,80,-1}, {271,80,-1}, {272,80,-1}, {273,80,-1}, {274,80,-1}, {275,80,-1},
    {276,80,-1}, {279,80,-1}, {280,80,-1}, {283,80,-1}, {284,80,-1}, {285,80,-1}, {286,80,-1}, {287,80,-1},
    {288,80,-1}, {289,80,-1}, {290,80,-1}, {292,80,-1}, {293,80,-1}, {294,80,-1}, {295,80,-1}, {296,80,-1},
    {297,80,-1}, {298,80,2}, {299,80,2}, {307,80,-1}, {308,80,-1}, {309,80,-1}, {310,80,-1}, {311,80,-1}, {312,80,-1},
    {313,80,-1}, {314,80,-1}, {315,80,-1}, {316,0,1}, {320,80,-1}, {321,80,-1}, {322,80,-1}, {323,80,-1}, {324,80,-1},
    {326,80,-1}, {327,10,1}, {330,80,-1}, {331,80,-1}, {332,80,-1}, {333,80,-1}, {334,80,-1}, {336,80,-1}, {337,80,-1},
    {338,80,-1}
};
static int vq_rec(int v, int field)
{
    int lo = 0, hi = (int)(sizeof VQ_TBL / sizeof VQ_TBL[0]) - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (VQ_TBL[m][0] == v) return VQ_TBL[m][field]; if (VQ_TBL[m][0] < v) lo = m + 1; else hi = m - 1; }
    return -1;
}
static struct { int v1, v2, pri; } g_vq[16];
static int g_vq_n;          /* entries; g_vq[0] is the head once started */
static int g_vq_phase;      /* the head: 0 not started, 1 its first line playing, 2 its second */
static int32_t g_vq_flush_done;   /* the end_flush_at the flush was done for */
static int vq_flushed(void) { return g_sim.end_flush_at && g_sim.now >= g_sim.end_flush_at; }
static void vq_pop(void) { if (g_vq_n > 0) { memmove(g_vq, g_vq + 1, sizeof g_vq[0] * (size_t)(g_vq_n - 1)); g_vq_n--; } g_vq_phase = 0; }
/* once a frame: 0x100313d0 - the head done (its lines said) -> the next one starts */
static void vq_drain(void)
{
    static int32_t last_now;
    if (g_sim.now < last_now) { g_vq_n = 0; g_vq_phase = 0; }   /* a new mission's clock: the old queue goes */
    last_now = g_sim.now;
    if (!g_sfx) return;
    if (vq_flushed() && g_vq_flush_done != g_sim.end_flush_at) {   /* 0x10031640(1) */
        int a = 0, b;
        g_vq_flush_done = g_sim.end_flush_at;
        if (g_vq_n && g_vq_phase && g_vq[0].pri < 0x50) {
            if (sfx_voice_busy(g_sfx)) sfx_stop_voice(g_sfx, g_vq_phase == 2 ? g_vq[0].v2 : g_vq[0].v1);
            vq_pop();
        }
        for (b = 0; b < g_vq_n; b++) if ((b == 0 && g_vq_phase) || g_vq[b].pri >= 0x50) g_vq[a++] = g_vq[b];
        g_vq_n = a;
    }
    if (!g_vq_n || sfx_voice_busy(g_sfx)) return;
    if (g_vq_phase == 1 && g_vq[0].v2 > 0) { sfx_play_voice(g_sfx, g_vq[0].v2, 0.9f); g_vq_phase = 2; return; }   /* the second line (sample user data 3) */
    if (g_vq_phase) vq_pop();
    if (!g_vq_n || (g_radio_waiting && g_vq[0].pri < 0x50)) return;   /* queued radio (0x50) goes first */
    if (g_vq[0].v1 > 0) { sfx_play_voice(g_sfx, g_vq[0].v1, 0.9f); g_vq_phase = 1; }
    else { sfx_play_voice(g_sfx, g_vq[0].v2, 0.9f); g_vq_phase = 2; }
}
/* 0x10031150 for a voiced message: text (the parts joined, "." added) shown only when the voice can't play (0x10031440:
 * 0x712 ticks at priority 0x32, MW2_VOICE_TEXT=1 always); the voices queued */
static void hud_message_v2(const char *m, int voice, int suffix)
{
    char txt[96];
    int pri, lim, k, n = 0, at, v1 = voice > 0 ? voice : 0, v2 = suffix > 0 ? suffix : 0;
    size_t l = strlen(m);
    snprintf(txt, sizeof txt, "%s%s", m, l && m[l - 1] != '.' && (v1 || v2) ? "." : "");
    if (getenv("MW2_STATE_DUMP")) {   /* TEST hook (tests/sequences): the text as posted, " +N" the second voice */
        if (v2) printf("MSGV now=%d voice=%d %s +%d\n", (int)g_sim.now, v1, m, v2);
        else printf("MSGV now=%d voice=%d %s\n", (int)g_sim.now, v1, m);
    }
    if (vq_flushed()) return;   /* DAT_1024f6c4: nothing posted once the end sequence has flushed the queue */
    if (!(v1 || v2) || !sfx_voice_audible(g_sfx, v1 ? v1 : v2)) {
        /* DOSBox (SB16 on / off, the S key): "Shutting down" is spoken without text when voices play, shown when silent */
        hud_message_ex(txt, v1 || v2 ? 0x712 : 0x16a, 0x32);
        return;
    }
    if (getenv("MW2_VOICE_TEXT")) hud_message_ex(txt, 0x712, 0x32);
    pri = v1 ? vq_rec(v1, 1) : -1;   /* +0x18 from the first voice's record (param -1) */
    lim = v1 ? vq_rec(v1, 2) : -1;
    for (k = g_vq_phase ? 1 : 0, at = g_vq_n; k < g_vq_n; k++) {
        if (v1 && g_vq[k].v1 == v1 && (g_vq[k].v2 == 0 || g_vq[k].v2 == v2) && (lim == -1 || lim <= ++n)) {
            if (getenv("MW2_STATE_DUMP")) printf("VQDROP now=%d voice=%d suffix=%d\n", (int)g_sim.now, v1, v2);   /* TEST hook */
            return;
        }
        if (g_vq[k].pri < pri) { at = k; break; }
    }
    if (g_vq_n >= 16) return;
    memmove(g_vq + at + 1, g_vq + at, sizeof g_vq[0] * (size_t)(g_vq_n - at));
    g_vq[at].v1 = v1; g_vq[at].v2 = v2; g_vq[at].pri = pri;
    g_vq_n++;
    g_sim.radio_busy = 1;   /* posted inside a sim step (a kill): the mission's end waits for it this step too (0x1000a9b0) */
    vq_drain();   /* an empty queue starts it at once (0x10031150 -> 0x10031440) */
}
/* a message with one voice line (0 = none: a plain acknowledgement, 0x16a ticks at 0x32) */
static void hud_message_v(const char *m, int voice)
{
    if (voice <= 0) {
        if (getenv("MW2_STATE_DUMP")) printf("MSGV now=%d voice=0 %s\n", (int)g_sim.now, m);   /* TEST hook (tests/sequences) */
        hud_message_ex(m, 0x16a, 0x32);
        return;
    }
    hud_message_v2(m, voice, 0);
}

/* The cockpit's power state as the HUD sees it (engine 0x1001e340, DAT_101de868 = controller c[0x28]): 1 starting (the
 * instruments' +0x78 callbacks), 2 up (+0x7c, everything), 3 shut down (+0x80). Only three things draw while not up:
 *  - the radar (cockpit mode 1 / 2 window, 0x10004560 / 0x100047d0 -> 0x100044b0 -> 0x10001710): its window grows from
 *    the centre point to full size on start-up and shrinks back on shutdown, both axes together, linearly over 181 ticks
 *    (anim records 0x1024a580 / 0x1024a628: {181, (0.5,0.5)-(0.5,0.5), (0,0)-(1,1)}); the circle (radius w / 2 - 1)
 *    shrinks with it, gone when it ends;
 *  - the target display (window 13, 0x10021730 / 0x100217f0) and the viewport (window 2, 0x10011aa0 / 0x10011b70,
 *    viewport modes only) by 0x100017f0, two phases of 90.5 ticks (records 0x1024cbf8 / 0x1024cc58, the same rects):
 *    start-up first widens a line from the centre point, then opens it vertically; shutdown first squashes the box to
 *    a horizontal line, then shortens the line to the centre;
 *  - in start-up the weapon list's names (LAB_1001e9a0), no ammunition and no selection box, each from its window's
 *    time (table 0x1024cce8: windows 3-7 at 543 / 573 / 603 / 633 / 663 ticks, 8-12 at 814 / 784 / 754 / 724 / 693 -
 *    mission ticks, so on a restart they are all there at once).
 * Each animation restarts when the state changes (0x10001610 on a +0x3c mismatch) and runs on the frame ticks. */
static int     g_pow = 2, g_pow_last = -1;
static int32_t g_pow_since;   /* sim ms when g_pow last changed */
static void pow_update(int state, int32_t now)
{
    if (state != g_pow_last) { g_pow_since = g_pow_last < 0 && state == 1 ? 0 : now; g_pow_last = state; }
    g_pow = state;
}
/* the shown part of a window, as fractions of its width and height about its centre; 0 = not drawn.
 * two_phase: 0x100017f0 (target display / viewport), else 0x10001710 (radar) */
static int pow_window(int two_phase, float *wf, float *hf)
{
    float t = g_pow == 2 ? 1.0f : (float)(g_sim.now - g_pow_since) * 0.182f / 181.0f;
    *wf = *hf = 1.0f;
    if (g_pow == 2) return 1;
    if (t < 0) t = 0;
    if (t >= 1.0f) return g_pow == 1;   /* start-up: full size; shutdown: gone */
    if (g_pow == 1) {                    /* centre point -> full */
        if (!two_phase) { *wf = *hf = t; }
        else if (t <= 0.5f) { *wf = 2.0f * t; *hf = 0.0f; }
        else { *wf = 1.0f; *hf = 2.0f * t - 1.0f; }
    } else {                             /* full -> centre point */
        if (!two_phase) { *wf = *hf = 1.0f - t; }
        else if (t <= 0.5f) { *wf = 1.0f; *hf = 1.0f - 2.0f * t; }
        else { *wf = 2.0f - 2.0f * t; *hf = 0.0f; }
    }
    return 1;
}

/* ---- the COMMAND COMPUTER and SYSTEMS STATUS panels (3D edition menu table 0x102653f0: menu 1 the COMMAND COMPUTER
 * root, 7 / 8 points 2 / 3, 5 SYSTEMS STATUS; pages 0x10260950..0x10261600 and 0x10264cb8). Keys: ALL_PT_MENU b /
 * Ctrl+F1 the root, PT_2_MENU / PT_3_MENU Ctrl+F2 / F3 the point pages, USER_MENU u SYSTEMS STATUS - each opens its
 * menu or, when that one is up, closes it (0x10065470; the 3Dfx action table 0x1003827c sends USER_MENU 0x34 to menu 5,
 * STARMATE_MENU / _ONE / _TWO 0x42-0x44 to menus 1 / 7 / 8; DOSBox YELLSCN1: u SYSTEMS STATUS, b COMMAND COMPUTER, Ctrl+F2
 * COMMAND POINT 2). Navigation 0x10065df0: a digit picks the numbered item, 0 the "(Esc to exit)" row, Esc goes back.
 * Pages: -1 closed, 0 the root, 1 Change Formation, 2 + p the commands for point p (0 = all), CC_SYSTEMS. */
#define CC_SYSTEMS 100
static const int CC_ORDER[5] = {MSIM_CMD_ATTACK, MSIM_CMD_DEFEND, MSIM_CMD_JOIN, MSIM_CMD_ENGAGE, MSIM_CMD_SHUTDOWN};
static const char *const CC_ORDER_TEXT[5] = {"Attack My Target", "Defend My Target", "Join Formation", "Engage at Will", "Shutdown"};
static const char *const CC_LAST_TEXT[8] = {"Change Formation", "Engage at Will", "Attack", "Join Formation", "Defend",
                                            "Disengage", "Shutdown", "No Cmd"};   /* 0x10260998 */
static int g_cc_page = -1, g_cc_from_root;   /* g_cc_from_root: the page was opened from the root (Esc goes back to it) */
static int g_vision;
/* one page's rows as the engine builds them (0x10064390 the root, 0x10064480 a point page): type 3 rows are labels
 * (no number), the accept row "(Esc to exit)" is numbered 0, the rest 1, 2, ... in order; act: 1 + page to open,
 * -(1 + order / formation / system item) to apply, 0 for labels and the Esc row */
typedef struct { int num, act; char text[40], val[24]; } cc_row;
static int cc_rows(int page, char *title, size_t tn, cc_row *r, int *rows, float *rect, float *vfrac)
{
    int n = 0, q, m = 0, alive = 0, k = 1;
    static const float CC_RECT[4] = {0.22f, 0.30f, 1.0f, 0.60f}, SYS_RECT[4] = {0.55f, 0.33f, 1.0f, 0.62f};
    memcpy(rect, page == CC_SYSTEMS ? SYS_RECT : CC_RECT, sizeof CC_RECT);
    *rows = page == CC_SYSTEMS ? 6 : 8; *vfrac = page == CC_SYSTEMS ? 0.8f : 0.4f;   /* records +0x3c, +0x60 */
    memset(r, 0, sizeof(cc_row) * 10);
#define ROW(nm, a, tx, vl) do { r[n].num = (nm); r[n].act = (a); snprintf(r[n].text, 40, "%s", (tx)); snprintf(r[n].val, 24, "%s", (vl)); n++; } while (0)
    if (page == CC_SYSTEMS) {   /* 0x10264cb8: four toggles (On / Off), then the Esc row - at the bottom (flags 0x2) */
        snprintf(title, tn, "SYSTEMS STATUS");
        ROW(1, -1, "Light Amplification", g_vision == 1 ? "On" : "Off");
        ROW(2, -2, "Image Enhancement", g_vision == 2 ? "On" : "Off");
        ROW(3, -3, "HUD", g_hud_off ? "Off" : "On");
        ROW(4, -4, "Auto Eject", combat_auto_eject() ? "On" : "Off");
        ROW(0, 0, "(Esc to exit)", "");
        return n;
    }
    for (q = 1; q <= 4; q++) if (msim_point_status(&g_sim, q) != 0) { m = q; if (msim_point_actor(&g_sim, q) >= 0) alive++; }
    if (page == 0) {   /* 0x10064390: Change Formation, the star's points, Command All, Esc; no live mate: NO STAR MATES */
        snprintf(title, tn, "COMMAND COMPUTER");
        if (!alive) { ROW(0, 0, "NO STAR MATES", ""); ROW(0, 0, "(Esc to exit)", ""); return n; }
        ROW(k++, 2, "Change Formation", MSIM_FORMATION_TEXT[g_sim.star_formation >= 0 ? g_sim.star_formation : 6]);
        for (q = 1; q <= m; q++) {
            char t[40];
            snprintf(t, sizeof t, "Command Point %d", q + 1);
            ROW(k++, 3 + q, t, MSIM_STATUS_TEXT[msim_point_status(&g_sim, q)]);
        }
        ROW(k++, 3, "Command All", CC_LAST_TEXT[g_sim.last_cmd[0]]);
        ROW(0, 0, "(Esc to exit)", "");
        return n;
    }
    if (page == 1) {   /* 0x10260d40 */
        snprintf(title, tn, "CHANGE FORMATION");
        ROW(0, 0, "Current Form:", MSIM_FORMATION_TEXT[g_sim.star_formation >= 0 ? g_sim.star_formation : 6]);
        for (q = 0; q < 6; q++) ROW(q + 1, -(1 + q), MSIM_FORMATION_TEXT[q], "");
        ROW(0, 0, "(Esc to exit)", "");
        return n;
    }
    {   /* 0x10260e98 COMMAND ALL / 0x10260ff0.. COMMAND POINT n; a point that is not there or destroyed: NOT AVAILABLE
         * (0x10064480) */
        int pt = page - 2;
        if (pt == 0) snprintf(title, tn, "COMMAND ALL");
        else snprintf(title, tn, "COMMAND POINT %d", pt + 1);
        if (pt > 0 && msim_point_actor(&g_sim, pt) < 0) { ROW(0, 0, "NOT AVAILABLE", ""); ROW(0, 0, "(Esc to exit)", ""); return n; }
        ROW(0, 0, "Status:", pt == 0 ? CC_LAST_TEXT[g_sim.last_cmd[0]] : MSIM_STATUS_TEXT[msim_point_status(&g_sim, pt)]);
        for (q = 0; q < 5; q++) ROW(q + 1, -(1 + q), CC_ORDER_TEXT[q], "");
        ROW(0, 0, "(Esc to exit)", "");
        return n;
    }
#undef ROW
}
static void hud_message_v2(const char *m, int voice, int suffix);
static void cc_message(void *u, const char *t, int voice)
{
    (void)u;
    if (voice < 0) hud_message_ex(t, -voice, 100);   /* a timed system line (the end sequence's CTRL-Q prompt) */
    else if (voice >> 16) hud_message_v2(t, voice & 0xffff, voice >> 16);
    else hud_message_v(t, voice);
}
static void on_shot(void *u, int weapon, int owner, int first, float x, float z);
static int       g_dmg_regions[15][4];
/* damage outlines of other mechs (the target viewer shows the target's): xxxxDMG6 remapped per level */
typedef struct { char skel[17]; hud_sprite lvl[4]; int regions[15][4]; int ok; } dmg_set;
static dmg_set    g_tdmg[16];
static hud_sprite g_hs_vtgt_nav;   /* VTGT_NP6: the nav point emblem in the target viewer */
static hud_sprite g_hs_navmk, g_hs_edge[4];   /* TGTNP6 ("NAV" marker); CRTLFT6 / CRTRGHT6 / CRTUP6 / CRTDOWN6 (red, a nav too) */
/* the current nav point: the first visible reach objective not yet done; returns the nav index, -1 none */
/* The cockpit frame: the flat-coloured polygons (material 0x61xx) of the piece carrying the eye (EYEO's
 * parent) - each mech's canopy struts, beside the camo-textured shell (0x0Bxx) that isn't drawn from
 * inside (Marauder: 38 strut polygons, its geodesic canopy). Built once per mech. */
static mech3d      g_plpit;     /* the player's cockpit representation (repr 4) */
static int         g_plpit_ok;
static mech3d_part g_pit[4];
static int         g_pit_n, g_pit_for = -1;
static void build_pit(void)
{
    int q, pnode = -1, k;
    for (k = 0; k < g_pit_n; k++) wtb_free(&g_pit[k].model);
    g_pit_n = 0;
    if (!g_pl_ok || !g_pl.eye_ok) return;

    for (q = 0; q < g_pl.skel_count; q++) if (g_pl.skel_index[q] == g_pl.eye_node) { pnode = g_pl.skel_parent[q]; break; }
    for (q = 0; q < g_pl.part_count && g_pit_n < 4 && pnode >= 0; q++) {
        const mech3d_part *src = &g_pl.parts[q];
        mech3d_part *dst;
        const wtb_object *o;
        wtb_object *no;
        float e[3], l[3];
        int pi, n = 0;
        if (src->node != pnode || src->model.object_count < 1) continue;
        o = &src->model.objects[0];
        for (k = 0; k < 3; k++) e[k] = g_pl.eye_pos[k] - (float)src->pos[k];
        if (src->has_rot) for (k = 0; k < 3; k++) l[k] = src->rot[k] * e[0] + src->rot[3 + k] * e[1] + src->rot[6 + k] * e[2];
        else {
            float h = src->yaw * 3.14159265f / 180.0f, c = cosf(h), sn = sinf(h);
            l[0] = c * e[0] - sn * e[2]; l[1] = e[1]; l[2] = sn * e[0] + c * e[2];
        }
        dst = &g_pit[g_pit_n++];
        *dst = *src;
        if (getenv("MW2_DEBUG_PIT")) fprintf(stderr, "cockpit piece: %s (node %d, eye node %d)\n", src->model_name, src->node, g_pl.eye_node);
        memset(&dst->model, 0, sizeof dst->model);
        dst->model.object_count = 1;
        dst->model.extended = src->model.extended;
        dst->model.objects = calloc(1, sizeof *dst->model.objects);
        no = dst->model.objects;
        *no = *o;
        {   /* the canopy as the original's hardware draws any object: back faces culled by vertex winding
             * (screen-space, Glide / SGL), so from the eye only the shell polygons wound toward it remain -
             * solid, in their own camo (Timber Wolf: the V of struts; Marauder: the hexagon and side panels) */
            no->verts = malloc((size_t)o->vert_count * sizeof *no->verts);
            memcpy(no->verts, o->verts, (size_t)o->vert_count * sizeof *no->verts);
            no->polys = malloc((size_t)(o->poly_count ? o->poly_count : 1) * sizeof *no->polys);
            for (pi = 0; pi < o->poly_count; pi++) {
                const wtb_poly *pg = &o->polys[pi];
                const wtb_vertex *a0 = &o->verts[pg->idx[0]], *a1 = &o->verts[pg->idx[1]], *a2 = &o->verts[pg->idx[2]];
                float u[3] = {(float)(a1->x - a0->x), (float)(a1->y - a0->y), (float)(a1->z - a0->z)};
                float w[3] = {(float)(a2->x - a0->x), (float)(a2->y - a0->y), (float)(a2->z - a0->z)};
                float nn[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
                float d = nn[0] * (l[0] - (float)a0->x) + nn[1] * (l[1] - (float)a0->y) + nn[2] * (l[2] - (float)a0->z);
                {
                    int keep = d > 0;
                    if (pg->n >= 3 && keep && !strstr(src->model_name, "WINSH")) no->polys[n++] = *pg;
                }
            }
        }

        no->poly_count = n;
    }
}

/* a DOS HUD bar: 15 px tall at 1024x768 in stripes - base 4, light 4, base 4, dark 3 (measured) */
/* the weapon list's window per weapon (engine 0x1001dcc0), by its mount location (slot +0x28: the MEK location, a leg
 * folded into its side torso - 0x10041410): locations 5 / 3 (left arm / left torso) fill windows 3-7 (the left column) in
 * weapon order, 4 / 1 (right arm / right torso) windows 8-12 (the right column), each running on into the other column when
 * full; centre torso
 * and head weapons alternate between the two columns' next free windows */
static int weapon_window(const combat_unit *pu, int wi)
{
    int pos[10], k, L = 3, R, nright = 0, b = 1, n = pu->weapon_count < 10 ? pu->weapon_count : 10;
    int ty[10];
    for (k = 0; k < n; k++) {
        int loc = pu->weapons[k].location;
        ty[k] = loc == MEK_RL ? MEK_RT : loc == MEK_LL ? MEK_LT : loc;
        pos[k] = 3 + (k % 2) * 5 + k / 2;
        if (ty[k] == MEK_RA || ty[k] == MEK_RT) nright++;
    }
    R = 8 + nright;
    for (k = 0; k < n; k++) if (ty[k] == MEK_LA || ty[k] == MEK_LT) { pos[k] = L++; if (L == 8) L = R; }
    R = 8;
    for (k = 0; k < n; k++) if (ty[k] == MEK_RA || ty[k] == MEK_RT) { pos[k] = R++; if (R > 12) R = L; }
    for (k = 0; k < n; k++) {
        int nl = L, nr = R;
        if (ty[k] != MEK_CT && ty[k] != MEK_HEAD) continue;
        if (b && L < R) { if (R == 13) nl = L + 1; else { nr = R + 1; L = R; } b = 0; }
        else { if (L == 8) { L = R; nr = R + 1; } else nl = L + 1; b = 1; }
        pos[k] = L;
        L = nl; R = nr;
    }
    k = wi < n ? pos[wi] : 3 + (wi % 2) * 5 + wi / 2;
    return k < 3 ? 3 : k > 12 ? 12 : k;
}

/* the weapon after `wi` in the panel's order - row by row, left then right (windows 3-7 the left column, 8-12 the
 * right): the engine's selector (0x10044030) steps through its 10 panel slots in order, skipping empty slots and
 * destroyed weapons (state -1); the port keeps the weapons in record order and places them by location
 * (weapon_window), so the step follows the windows. ready_only: the next ready weapon with ammunition from wi on
 * (chain fire), -1 if none. */
static int weapon_next(const combat_unit *pu, int wi, int include_self, int ready_only)
{
    int n = pu->weapon_count < 10 ? pu->weapon_count : 10, ord[10], k, j, at = -1, step;
    if (n <= 0) return -1;
    for (k = 0; k < n; k++) ord[k] = k;
    for (k = 1; k < n; k++) {   /* by row, then column */
        int v = ord[k], wv = weapon_window(pu, v), kv = ((wv - 3) % 5) * 2 + (wv >= 8);
        for (j = k - 1; j >= 0; j--) {
            int wj = weapon_window(pu, ord[j]), kj = ((wj - 3) % 5) * 2 + (wj >= 8);
            if (kj <= kv) break;
            ord[j + 1] = ord[j];
        }
        ord[j + 1] = v;
    }
    for (k = 0; k < n; k++) if (ord[k] == wi) at = k;
    if (at < 0) at = n - 1;
    for (step = include_self ? 0 : 1; step <= n; step++) {
        const combat_weapon *cw = &pu->weapons[ord[(at + step) % n]];
        if (cw->state < 0 || (cw->location >= 0 && cw->location < 8 && pu->loc_gone[cw->location])) continue;
        if (ready_only && (cw->state != CW_READY || cw->ammo == 0)) continue;
        return ord[(at + step) % n];
    }
    return ready_only ? -1 : wi;
}

/* NEXT/PREV_NAVPOINT (engine 0x1001cca0 -> 0x1001cea0): the next nav point the player can select, wrapping */
static int nav_step(int cur, int dir)
{
    int i, n = g_sim.nav_count;
    for (i = 1; i <= n; i++) {
        int k = ((cur < 0 ? (dir > 0 ? -1 : 0) : cur) + dir * i + n * 4) % n;
        if (g_sim.navs[k].selectable) return k;
    }
    return -1;
}
/* the target viewer's nav text (engine 0x10020e40): the NAVP's own name, "Nav Point" when it has none */
static const char *nav_label(int nv)
{
    return g_sim.navs[nv].label[0] ? g_sim.navs[nv].label : "Nav Point";
}
/* the nav point the HUD marks (with no unit / thing target): only one the player's selector holds (+0xdc kind 0x100 -
 * NEXT / PREV_NAVPOINT, the autopilot's 0x1001dac0). The marks - the view's TGTNP / off-screen X (0x10022830), the
 * compass marker (0x10022960), the altitude tape's (0x100222e0), the radar's RTGTNP - all go by the selector, and with it
 * empty (+0xdc 0x1000: at the load 0x100237fe, after Ctrl+t 0x1001ce80, a cycle that found nothing, a target lost by the
 * upkeep 0x1001d510 - destroyed) none is drawn. DOSBox: no marker after the target is destroyed (YELLSCN1 tank, Falcon
 * kills: docs/reference/lock_structure_dos_vs_port.png, enemy_destroyed_after.png), none at the start (TNJ1). The port
 * had fallen back to an open REACH objective's nav, else the first nav */
static int current_nav(const char **label)
{
    if (g_nav_sel >= 0 && g_nav_sel < g_sim.nav_count) {   /* NEXT_NAVPOINT / PREV_NAVPOINT, the autopilot */
        if (label) *label = nav_label(g_nav_sel);
        return g_nav_sel;
    }
    return -1;
}
/* the damage outline's SHP name for a skeleton: HUD record value 11 (+0x28) is the outline's SHP resource ID in the
 * 320x200 set, the 640x480 and 1024x768 sets at ID + 1 / + 2 (loader 0x100403a0 -> 0x101de8e0; 0x1001b230 /
 * 0x1001b620 load ID + the set index 0x102453c8). Values 12 / 13 (0x101de8e4 / e8) are 0 in every record and nothing
 * reads them. -1 when the ID names no shape (ELEMENTA 295, TARANTUL / ELEMENTL -1: the engine draws no outline) */
static int outline_shp_name(prj_archive *ma, const char *skel, char *out, size_t n)
{
    prj_record hr;
    int id, t, i, set = g_hud_sfx[0] == 'K' ? 2 : g_hud_sfx[0] == '6' ? 1 : 0;
    if (prj_read_named(ma, "HUD", skel, &hr) != PRJ_OK) return -1;
    id = hr.size >= 44 ? (int)(int32_t)(hr.data[40] | (hr.data[41] << 8) | (hr.data[42] << 16) | ((uint32_t)hr.data[43] << 24)) : -1;
    prj_record_free(&hr);
    if (id < 0 || (t = prj_find_type(ma, "SHP")) < 0) return -1;
    for (i = 0; i < prj_symbol_count(ma, t); i++)
        if (prj_symbol_id(ma, t, i) == id + set) { snprintf(out, n, "%s", prj_symbol_name(ma, t, i)); return 0; }
    return -1;
}
static dmg_set *target_outline(prj_archive *ma, const char *skel)
{
    static const int to[4] = {6, 3, 11, 0};   /* engine 0x1001b620: colour 6 by damage level */
    char dmg[16];
    prj_record hr;
    int k, q;
    for (k = 0; k < 16 && g_tdmg[k].skel[0]; k++) if (strcmp(g_tdmg[k].skel, skel) == 0) return g_tdmg[k].ok ? &g_tdmg[k] : NULL;
    if (k == 16 || !g_hud) return NULL;
    snprintf(g_tdmg[k].skel, sizeof g_tdmg[k].skel, "%s", skel);
    if (outline_shp_name(ma, skel, dmg, sizeof dmg) != 0) return NULL;
    for (q = 0; q < 4; q++) {
        if (hud_sprite_load_remap(g_hud, ma, dmg, 0, 6, to[q], &g_tdmg[k].lvl[q]) != 0) return NULL;
        hud_sprite_smooth(&g_tdmg[k].lvl[q], 1);
    }
    if (prj_read_named(ma, "HUD", skel, &hr) == PRJ_OK) {
        for (q = 0; q < 60 && 13 * 4 + q * 4 + 4 <= (int)hr.size; q++) {
            const uint8_t *pp = hr.data + 13 * 4 + q * 4;
            g_tdmg[k].regions[q / 4][q % 4] = (int32_t)(pp[0] | (pp[1] << 8) | (pp[2] << 16) | ((uint32_t)pp[3] << 24));
        }
        prj_record_free(&hr);
    }
    g_tdmg[k].ok = 1;
    return &g_tdmg[k];
}
/* a unit's outline: 15 regions coloured by its locations' damage (as the player's, engine 0x1001b620) */
static void draw_outline(const hud_sprite lvl[4], const int regions[15][4], const combat_unit *u, float dx, float dy)
{
    static const int REG_LOC[15] = {1, 3, 3, 2, 2, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8};
    int q;
    hud_draw(g_hud, &lvl[0], dx, dy, NULL);   /* the whole outline, then the damaged regions over it (0x1001b620) */
    for (q = 0; q < 15; q++) {
        int loc = REG_LOC[q] - 1, lf = 0, lr = 0, lv, pick;
        const int *rg = regions[q];
        if (rg[2] <= 0 || rg[3] <= 0) continue;
        if (u->armor_max[loc] > 0) lf = 15 - 15 * (int)u->armor[loc] / u->armor_max[loc];
        if (u->rear_max[loc] > 0) lr = 15 - 15 * (int)u->rear[loc] / u->rear_max[loc];
        lv = lf > lr ? lf : lr;
        pick = u->loc_gone[loc] ? 3 : lv <= 0 ? 0 : lv < 12 ? 1 : 2;
        if (pick == 0) continue;
        hud_draw_region(g_hud, &lvl[pick], dx, dy, (float)rg[0] * 2.0f * g_hud_s, (float)rg[1] * 2.0f * g_hud_s, (float)rg[2] * 2.0f * g_hud_s, (float)rg[3] * 2.0f * g_hud_s);
    }
}
static int       g_layout_n;   /* g_layout (the CPIT windows) is declared with cpit_window */

static int       g_sel_weapon;
static int       g_end_announced;
static int       g_show_objectives;   /* F12 */
static int       g_fire_was;   /* g_fire_was: the trigger was down last frame (chain fire is per press) */   /* chain fire: move the selection on once the selected weapon has fired */
static uint32_t  g_groups[3] = {0xFFFFFFFFu, 0, 0};   /* fire groups 1-3 (bit i = weapon i); every weapon is in exactly one
                                       * group, group 1 at the start (engine: weapon slot +0xc = 0 / 1 / 2, 0x10044350) */
static void set_weapon_group(int w, int g)
{
    int k;
    for (k = 0; k < 3; k++) g_groups[k] &= ~(1u << w);
    g_groups[g] |= 1u << w;
}   /* selected weapon (boxed in the list) */
static float     g_throttle_eff;
static float     g_thr_srv, g_spd_srv;   /* the player's current throttle c[0x13] (fraction of full) and drive speed c[0xb] (cm/s): msim_drive_step */
static int       g_drive_slow;           /* a walk key flagged 0x10 has run: the drive servo's T is 90.5 ticks (msim_drive_step) */
static float     g_twist_limit = 90.0f;   /* MGEO +0x14 (16.16 deg): 90 most mechs, 15 Kit Fox/Nova, 361 turrets */

static int   g_auto_on, g_auto_fire, g_auto_cockpit, g_auto_jet;
static float g_auto_secs, g_auto_throttle, g_auto_twist, g_auto_elapsed;
/* MW2_PERF=1 (a diagnostic for slow machines): every 2 s the frame rate and where the frame's time went - before the
 * world draw (events, simulation), the world draw's CPU, the rest (HUD, cockpit), the buffer swap (on most drivers the
 * wait for the GPU); MW2_PERF=2 also waits for the GPU after the world draw (its GPU time then shows in "world") */
static int    g_perf = -1;
static double g_perf_t[5], g_perf_sum[4], g_perf_max, g_perf_t0;
static int    g_perf_n;
static double perf_now(void) { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }
static void perf_frame_start(void)
{
    if (g_perf < 0) g_perf = getenv("MW2_PERF") ? atoi(getenv("MW2_PERF")) : 0;
    if (g_perf <= 0) return;
    g_perf_t[4] = perf_now();
}
static void perf_mark(int k)
{
    if (g_perf <= 0) return;
    if (k == 1 && g_perf >= 2) glFinish();
    g_perf_t[k] = perf_now();
}
static void perf_report(int w, int h)
{
    double now, fr;
    if (g_perf <= 0) return;
    if (g_perf_t0 == 0) {
        g_perf_t0 = g_perf_t[4];
        printf("perf: GL %s / %s / %s, drawable %d x %d\n", (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
               (const char *)glGetString(GL_VERSION), w, h);
    }
    g_perf_sum[0] += g_perf_t[0] - g_perf_t[4];
    g_perf_sum[1] += g_perf_t[1] - g_perf_t[0];
    g_perf_sum[2] += g_perf_t[2] - g_perf_t[1];
    g_perf_sum[3] += g_perf_t[3] - g_perf_t[2];
    fr = g_perf_t[3] - g_perf_t[4];
    if (fr > g_perf_max) g_perf_max = fr;
    g_perf_n++;
    now = perf_now();
    if (now - g_perf_t0 >= 2.0 && g_perf_n > 0) {
        double n = g_perf_n;
        printf("perf: %.1f fps (%d x %d), ms per frame: before world %.2f, world %.2f, HUD etc %.2f, swap %.2f, worst frame %.1f\n",
               n / (now - g_perf_t0), w, h, 1000 * g_perf_sum[0] / n, 1000 * g_perf_sum[1] / n, 1000 * g_perf_sum[2] / n,
               1000 * g_perf_sum[3] / n, 1000 * g_perf_max);
        fflush(stdout);
        memset(g_perf_sum, 0, sizeof g_perf_sum); g_perf_max = 0; g_perf_n = 0; g_perf_t0 = now;
    }
}

static float g_auto_dt = 1.0f / 30.0f;   /* the autopilot's frame time (TEST ONLY: MW2_TEST_FPS=N runs 1/N s frames) */
static char  g_auto_out[256];

static void sim_log(void *u, const char *line)
{
    (void)u;
    /* (the kill messages come from msim's unit_messages: every enemy unit's death, by its class - 0x10016280) */
    printf("%s\n", line);
    fflush(stdout);
}

/* the drop screen (DOSBox: the simulation's own loading screen): the mission's launch picture (its brief record's SUPS
 * chunk names the disc's LAUNCH\<name>6.SHP, a 640 x 480 shape with its own palette) and LAUNCH6.SHP's "DROP PROCEDURE
 * INITIATED" at (368, 20) in light grey; drawn 4:3 in the window before the mission loads and held 2 s from launch (user choice) */
static GLuint g_drop_tex, g_drop_texs[5];   /* the picture; with each LAUNCH6 frame 0-4 (0: the text alone) */
static int    g_drop_dos;                  /* the DOS edition: the overlay where the DOS capture has it */
static Uint32 g_drop_t0;
static float  g_drop_level = 1.0f;         /* the drop screen's brightness (its fade-out) */
static int drop_shp(const char *disc, const char *name, shp_frame *fr, uint8_t pal[256][3])
{
    char path[1200];
    FILE *f;
    long n;
    uint8_t *d;
    uint32_t po, cnt, i2;
    snprintf(path, sizeof path, "%s/LAUNCH/%s.SHP", disc, name);
    if (!(f = fopen(path, "rb"))) return -1;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 32 || !(d = malloc((size_t)n))) { fclose(f); return -1; }
    if (fread(d, 1, (size_t)n, f) != (size_t)n || shp_decode(d, (size_t)n, 0, fr) != 0) { fclose(f); free(d); return -1; }
    fclose(f);
    memset(pal, 0, 768);
    po = (uint32_t)d[12] | (uint32_t)d[13] << 8 | (uint32_t)d[14] << 16 | (uint32_t)d[15] << 24;
    if (po + 4 <= (uint32_t)n) {
        cnt = (uint32_t)d[po] | (uint32_t)d[po + 1] << 8;
        for (i2 = 0; i2 < cnt && po + 8 + i2 * 4 <= (uint32_t)n; i2++) {
            const uint8_t *e = d + po + 4 + i2 * 4;
            pal[e[0]][0] = (uint8_t)(e[1] << 2); pal[e[0]][1] = (uint8_t)(e[2] << 2); pal[e[0]][2] = (uint8_t)(e[3] << 2);
        }
    }
    free(d);
    return 0;
}
static void drop_screen_build(prj_archive *ma, const char *mission)
{
    char brf[16], sups[16] = "", nm[24], disc[1024];
    const char *cd = getenv("MW2_CD_DIR");
    prj_record r;
    shp_frame pic, txt;
    uint8_t pal[256][3], tpal[256][3];
    uint32_t *rgba;
    int x, y;
    if (getenv("MW2_NO_LOADING") || !mission || !mission[0]) return;
    snprintf(brf, sizeof brf, "%.4sBRF2", mission);
    if (prj_read_named(ma, "BWD", brf, &r) == PRJ_OK) {
        bwd_chunk ch[96];
        int nc = bwd_chunks(r.data, r.size, ch, 96), q;
        for (q = 0; q < nc; q++) if (strcmp(ch[q].tag, "SUPS") == 0 && ch[q].size > 0) { snprintf(sups, sizeof sups, "%.*s", (int)(ch[q].size < 15 ? ch[q].size : 15), (const char *)ch[q].data); break; }
        prj_record_free(&r);
    }
    if (!sups[0]) snprintf(sups, sizeof sups, "supanm");   /* 0x100073b0: no SUPS name -> supanm6 */
    if (cd && cd[0]) snprintf(disc, sizeof disc, "%s", cd);
    else {   /* the game folder's cd/ beside the models archive */
        portcfg pc;
        char *bp = SDL_GetBasePath();
        portcfg_load(NULL, &pc); portcfg_resolve(&pc, bp, NULL); SDL_free(bp);
        if (pc.cd[0]) snprintf(disc, sizeof disc, "%s", pc.cd); else return;
    }
    snprintf(nm, sizeof nm, "%s6", sups);
    for (x = 0; nm[x]; x++) if (nm[x] >= 'a' && nm[x] <= 'z') nm[x] = (char)(nm[x] - 32);
    if (drop_shp(disc, nm, &pic, pal) != 0) return;
    rgba = calloc(640 * 480, 4);
    if (!rgba) { shp_frame_free(&pic); return; }
    for (y = 0; y < pic.h && y < 480; y++)
        for (x = 0; x < pic.w && x < 640; x++)
            if (pic.mask[y * pic.w + x]) { const uint8_t *c = pal[pic.pix[y * pic.w + x]]; rgba[y * 640 + x] = 0xff000000u | (uint32_t)c[2] << 16 | (uint32_t)c[1] << 8 | c[0]; }
    for (y = 0; y < 480; y++) for (x = 0; x < 640; x++) if (!rgba[y * 640 + x]) rgba[y * 640 + x] = 0xff000000u;
    {   /* LAUNCH6.SHP: five shapes at one hotspot - the text alone, then the text with the spinner (four diamonds
         * turning, right of the text) - drawn in the simulation's fixed interface colours (indices 1-7, 10, 13, 14; the
         * file has no palette): matched to the DOS capture's bevelled grey letters */
        static const uint8_t UI[16][3] = {{0, 0, 0}, {207, 211, 219}, {158, 162, 174}, {130, 130, 142}, {109, 113, 121}, {93, 101, 109},
                                          {85, 85, 93}, {20, 20, 20}, {0, 0, 0}, {0, 0, 0}, {158, 162, 174}, {0, 0, 0}, {0, 0, 0},
                                          {207, 211, 219}, {16, 16, 20}, {0, 0, 0}};
        char path[1200];
        FILE *f;
        snprintf(path, sizeof path, "%s/LAUNCH/LAUNCH6.SHP", disc);
        (void)tpal; (void)txt;
        if ((f = fopen(path, "rb"))) {
            long n;
            uint8_t *d;
            fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
            if (n > 16 && (d = malloc((size_t)n)) && fread(d, 1, (size_t)n, f) == (size_t)n) {
                int fr;
                for (fr = 0; fr <= 4; fr++) {   /* one texture per frame: 0x10007730 cycles all five, 0 included */
                    shp_frame sf;
                    uint32_t *img = malloc(640 * 480 * 4);
                    if (!img) break;
                    memcpy(img, rgba, 640 * 480 * 4);
                    if (shp_decode(d, (size_t)n, fr, &sf) == 0) {
                        for (y = 0; y < sf.h; y++)
                            for (x = 0; x < sf.w; x++) {
                                /* 3D editions: x = trunc(639 x 0.55) = 351 (0x102470e0), y 0, by the hotspot; DOS: as captured */
                                int px = (g_drop_dos ? 348 : 351) + sf.left + x, py = (g_drop_dos ? -4 : 0) + sf.top + y, ci = sf.pix[y * sf.w + x];
                                if (!sf.mask[y * sf.w + x] || px < 0 || py < 0 || px >= 640 || py >= 480 || ci > 15) continue;
                                img[py * 640 + px] = 0xff000000u | (uint32_t)UI[ci][2] << 16 | (uint32_t)UI[ci][1] << 8 | UI[ci][0];
                            }
                        shp_frame_free(&sf);
                    }
                    glGenTextures(1, &g_drop_texs[fr]);
                    glBindTexture(GL_TEXTURE_2D, g_drop_texs[fr]);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 640, 480, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    free(img);
                }
                free(d);
            }
            fclose(f);
        }
    }
    shp_frame_free(&pic);
    glGenTextures(1, &g_drop_tex);
    glBindTexture(GL_TEXTURE_2D, g_drop_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 640, 480, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    free(rgba);
}
/* draw the drop screen into the window (4:3, centred) through a framebuffer blit */
static void drop_screen_draw(SDL_Window *win)
{
    int w, h, vw, vh;
    GLuint tex = g_drop_tex;
    if (!g_drop_tex) return;
    {   /* the AIL timer (330000 us, 0x10007730): the next of the five frames every 330 ms */
        int fr = (int)((SDL_GetTicks() / 330) % 5);
        if (g_drop_texs[fr]) tex = g_drop_texs[fr];
    }
    SDL_GL_GetDrawableSize(win, &w, &h);
    vh = h; vw = h * 4 / 3; if (vw > w) { vw = w; vh = w * 3 / 4; }
    {   /* a textured quad (a framebuffer blit into a multisampled window - anti-aliasing on - is not allowed and
         * left the screen black) */
        static GLuint prog, vao, vbo;
        float x0, y0, x1, y1, q[16];
        if (!prog) {
            static const char *VS2 = "#version 330 core\nlayout(location=0) in vec2 p; layout(location=1) in vec2 t; out vec2 uv;\n"
                                     "void main() { uv = t; gl_Position = vec4(p, 0.0, 1.0); }\n";
            static const char *FS2 = "#version 330 core\nin vec2 uv; uniform sampler2D tex; uniform float k; out vec4 o;\n"
                                     "void main() { o = vec4(texture(tex, uv).rgb * k, 1.0); }\n";
            GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
            glShaderSource(vs, 1, &VS2, NULL); glCompileShader(vs);
            glShaderSource(fs, 1, &FS2, NULL); glCompileShader(fs);
            prog = glCreateProgram(); glAttachShader(prog, vs); glAttachShader(prog, fs); glLinkProgram(prog);
            glDeleteShader(vs); glDeleteShader(fs);
            glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
            glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof q, NULL, GL_DYNAMIC_DRAW);
            glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
            glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
        }
        x0 = -(float)vw / (float)w; x1 = -x0; y0 = -(float)vh / (float)h; y1 = -y0;
        q[0] = x0; q[1] = y0; q[2] = 0; q[3] = 1;   q[4] = x1; q[5] = y0; q[6] = 1; q[7] = 1;
        q[8] = x0; q[9] = y1; q[10] = 0; q[11] = 0; q[12] = x1; q[13] = y1; q[14] = 1; q[15] = 0;
        glViewport(0, 0, w, h);
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
        glUseProgram(prog);
        glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof q, q);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(glGetUniformLocation(prog, "tex"), 0);
        glUniform1f(glGetUniformLocation(prog, "k"), g_drop_level);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0); glUseProgram(0);
    }
    SDL_GL_SwapWindow(win);
}
/* the target viewer's range (DOS 0x30173, 3Dfx 0x10021150): whole metres (cm / 100, truncated) "%3ldm" up to 1000, then
 * "%2.2fk" km. The 3Dfx edition: (float)m x 0.001f (0x102475d0) through the C library's %f. The DOS edition: m x 0.001
 * (double 0x90ba3, x87 product stored as a double) through Watcom's %f, which rounds the value's 17 significant
 * digits half to even. DOSBox YELLSCN1, Nav Epsilon at 1075 m (a copy patched to "%3ldm" shows 1075m): "1.08k" -
 * 1.07499999999999995559 to 17 digits is the tie 1.0750000000000000, to even 1.08 (glibc's exact rounding gives 1.07); with
 * the constant patched so the double is exactly 1.125: "1.12k" (half to even, not up); so that it is
 * 1.03499999999999992006: "1.03k" (17 digits, not 16) */
static void range_text(char *out, size_t n, float cm)
{
    long m = (long)cm / 100;
    if (m < 1001) snprintf(out, n, "%3ldm", m);
    else if (g_tex_kind == 3) {
        char e[40];
        int dig[17], k, ex, keep;
        long long v = 0;
        snprintf(e, sizeof e, "%.16e", (double)m * 0.001);   /* d.dddddddddddddddde+XX: 17 significant digits */
        for (k = 0; k < 17; k++) dig[k] = e[k == 0 ? 0 : k + 1] - '0';
        ex = atoi(strchr(e, 'e') + 1);
        keep = ex + 3;   /* the digits up to the hundredths */
        for (k = 0; k < keep && k < 17; k++) v = v * 10 + dig[k];
        for (; k < keep; k++) v *= 10;
        if (keep < 17) {   /* round the rest half to even */
            int rest = 0;
            for (k = keep + 1; k < 17; k++) rest |= dig[k];
            if (dig[keep] > 5 || (dig[keep] == 5 && (rest || (v & 1)))) v++;
        }
        snprintf(out, n, "%lld.%02lldk", v / 100, v % 100);
    } else snprintf(out, n, "%2.2fk", (double)((float)m * 0.001f));
}

/* the target viewer's wireframe (DOS footage: the targeted mech or structure drawn as a blue line model in the box,
 * turning as you move round it): the model's polygon edges, seen from the player's bearing, a little from above,
 * scaled to fit the box. view_yaw: the player's bearing from the target, relative to the target's heading (degrees) */
static float g_wire_r[2] = {1, 0}, g_wire_f[2] = {0, 1};   /* the camera's right / forward on the ground (x, z), set per frame */
/* the viewer's camera (DOS 0x30220 -> 0x28810(7, 0x20000, camera, object); 3Dfx 0x10021300 -> 0x1001bc50(2.0f, object)):
 * the target's point (a mech: its object position +0x50..+0x58 copied to the player's +0xc8..+0xd0 by 0x1001d050; a
 * structure: its bounding sphere's centre, 0x10029890) less 3 x its radius (+0xe8 / the sphere's, 0x102475d4 = 3.0) along
 * the bearing from the player (+0xd4, 0x1001d970), at that point's height, heading the bearing, pitch and roll 0 (level),
 * zoom 2: focal length = 2 x half the window's width (0x1002f740). g_wire_persp: draw_target_wire projects through it */
static float g_wire_eye[3];
static int   g_wire_persp;
static void wire_camera(float tx, float ty, float tz, float R)
{
    float lx = tx - (float)g_pl.origin[0], lz = tz - (float)g_pl.origin[2], ll = sqrtf(lx * lx + lz * lz);
    if (ll < 1.0f) { lx = 0; lz = 1; ll = 1; }
    g_wire_f[0] = lx / ll; g_wire_f[1] = lz / ll; g_wire_r[0] = g_wire_f[1]; g_wire_r[1] = -g_wire_f[0];
    g_wire_eye[0] = tx - g_wire_f[0] * 3.0f * R; g_wire_eye[1] = ty; g_wire_eye[2] = tz - g_wire_f[1] * 3.0f * R;
    g_wire_persp = 1;
}
/* the target viewer's mech: its blown-off locations left out, as the view draws it (the engine renders the object
 * itself, 0x10021300, so lost limbs are gone there too) */
static const combat_unit *g_wire_unit;
/* a location's colour in the target view, as the damage outline (0x1001b620: levels 6 / 3 / 11 / 0 - here the HUD
 * blue, yellow, red, black): armour lost front or rear in fifteenths, 0 intact, 1-11 yellow, 12+ red; the centre torso
 * of a destroyed unit black. Returns 0 blue, 1 yellow, 2 red, 3 black; tu NULL or gq outside 1-8: blue */
/* a location's damage nibble 0-15 (the fifteenths of armour lost, front or rear - the location objects' +0 bits 4-7) */
static int loc_nibble(const combat_unit *tu, int gq)
{
    int loc, lf = 0, lr = 0;
    if (!tu || gq < 1 || gq > 8) return 0;
    loc = gq - 1;
    if (tu->armor_max[loc] > 0) lf = 15 - 15 * (int)tu->armor[loc] / tu->armor_max[loc];
    if (tu->rear_max[loc] > 0) lr = 15 - 15 * (int)tu->rear[loc] / tu->rear_max[loc];
    lf = lf > lr ? lf : lr;
    return lf < 0 ? 0 : lf > 15 ? 15 : lf;
}
static int loc_level(const combat_unit *tu, int gq)
{
    int loc, lv;
    if (!tu || gq < 1 || gq > 8) return 0;
    loc = gq - 1;
    lv = loc_nibble(tu, gq);
    if (tu->destroyed && loc == 2) return 3;
    return lv >= 12 ? 2 : lv > 0 ? 1 : 0;
}
static const unsigned LOC_RGB[4] = {0x0030D7u, 0xFFCE00u, 0xFF1818u, 0x000000u};
/* the viewer's model through its camera (wire_camera; DOS 0x28810 with colour 7): each polygon filled with the window's
 * black and outlined in the colour, drawn back to front (by its mean depth) - so the edges behind are hidden, as DOSBox
 * YELLSCN1 shows the Nova: its outline and front faces only, not every edge (the port had drawn every edge, seen
 * through) - clipped to the window. A location's outline colour by its damage (loc_level), a destroyed centre torso black */
typedef struct { float d; int n, lvl; float xy[2 * WTB_MAX_POLY_VERTS]; } wire_poly;
static int wire_poly_cmp(const void *a, const void *b)
{
    float da = ((const wire_poly *)a)->d, db = ((const wire_poly *)b)->d;
    return da < db ? 1 : da > db ? -1 : 0;
}
static void draw_target_view(const mech3d *m, int part_lo, int part_hi, float bx, float by, float bw, float bh, const float col[4])
{
    static wire_poly *P;
    static int cap;
    int np = 0, pi, q, a, i;
    const float *bg = HP(0);
    for (pi = part_lo; pi < part_hi; pi++) {
        const mech3d_part *pt = &m->parts[pi];
        const wtb_object *o;
        int lvl;
        if (pt->hidden || pt->model.object_count <= 0) continue;
        if (g_wire_unit && pt->group >= 1 && pt->group <= 8 && pt->group != 3 && g_wire_unit->loc_gone[pt->group - 1]) continue;
        lvl = loc_level(g_wire_unit, pt->group);
        o = &pt->model.objects[0];
        for (q = 0; q < o->poly_count; q++) {
            const wtb_poly *pg = &o->polys[q];
            wire_poly *w;
            float dsum = 0;
            if (pg->n < 2) continue;
            if (np == cap) { wire_poly *g2 = realloc(P, (size_t)(cap ? cap * 2 : 1024) * sizeof *g2); if (!g2) break; P = g2; cap = cap ? cap * 2 : 1024; }
            w = &P[np];
            w->n = pg->n < WTB_MAX_POLY_VERTS ? pg->n : WTB_MAX_POLY_VERTS; w->lvl = lvl;
            for (a = 0; a < w->n; a++) {
                const wtb_vertex *v = &o->verts[pg->idx[a]];
                float x, y, z, dx, dy, dz, dep;
                if (pt->has_rot) {
                    const float *R = pt->rot;
                    x = R[0] * (float)v->x + R[1] * (float)v->y + R[2] * (float)v->z + (float)pt->pos[0];
                    y = R[3] * (float)v->x + R[4] * (float)v->y + R[5] * (float)v->z + (float)pt->pos[1];
                    z = R[6] * (float)v->x + R[7] * (float)v->y + R[8] * (float)v->z + (float)pt->pos[2];
                } else {
                    float c2 = cosf(pt->yaw * 3.14159265f / 180.0f), s2 = sinf(pt->yaw * 3.14159265f / 180.0f);
                    x = c2 * (float)v->x + s2 * (float)v->z + (float)pt->pos[0];
                    y = (float)v->y + (float)pt->pos[1];
                    z = -s2 * (float)v->x + c2 * (float)v->z + (float)pt->pos[2];
                }
                dx = x - g_wire_eye[0]; dy = y - g_wire_eye[1]; dz = z - g_wire_eye[2];
                dep = g_wire_f[0] * dx + g_wire_f[1] * dz;
                if (dep < 10.0f) dep = 10.0f;
                dsum += dep;
                w->xy[a * 2] = bx + bw * 0.5f + bw * (g_wire_r[0] * dx + g_wire_r[1] * dz) / dep;   /* focal = 2 x half the width */
                w->xy[a * 2 + 1] = by + bh * 0.5f - bw * dy / dep;
            }
            w->d = dsum / (float)w->n;
            np++;
        }
    }
    qsort(P, (size_t)np, sizeof *P, wire_poly_cmp);
    hud_clip(g_hud, bx, by, bw, bh);
    for (i = 0; i < np; i++) {
        float seg[4 * WTB_MAX_POLY_VERTS], c[4];
        const float *cc = col;
        if (P[i].n >= 3) hud_poly(g_hud, P[i].xy, P[i].n, bg);
        if (P[i].lvl == 3) continue;
        if (P[i].lvl) { c[0] = (float)(LOC_RGB[P[i].lvl] >> 16) / 255.0f; c[1] = (float)((LOC_RGB[P[i].lvl] >> 8) & 255) / 255.0f; c[2] = (float)(LOC_RGB[P[i].lvl] & 255) / 255.0f; c[3] = 1.0f; cc = c; }
        for (a = 0; a < P[i].n; a++) {
            int b2 = (a + 1) % P[i].n;
            seg[a * 4] = P[i].xy[a * 2]; seg[a * 4 + 1] = P[i].xy[a * 2 + 1]; seg[a * 4 + 2] = P[i].xy[b2 * 2]; seg[a * 4 + 3] = P[i].xy[b2 * 2 + 1];
        }
        hud_lines(g_hud, seg, P[i].n, 1.0f, cc);
    }
    hud_clip_off(g_hud);
}
static void draw_target_wire(const mech3d *m, int part_lo, int part_hi, float view_yaw, float bx, float by, float bw, float bh, const float col[4])
{
    float cy = cosf(view_yaw * 3.14159265f / 180.0f), sy = sinf(view_yaw * 3.14159265f / 180.0f);
    float ce = cosf(0.25f), se = sinf(0.25f);   /* ~14 degrees from above */
    float mn[2] = {1e30f, 1e30f}, mx[2] = {-1e30f, -1e30f}, k = 1, ox = 0, oy = 0;
    static float *segs[4];   /* the edges, one batch per damage colour (DOS: each location by its damage level) */
    static int caps[4];
    int pass, pi, nsegs[4] = {0, 0, 0, 0}, lvl = 0;
    if (!m || m->part_count <= 0) return;
    if (part_hi > m->part_count) part_hi = m->part_count;
    if (g_wire_persp) { draw_target_view(m, part_lo, part_hi, bx, by, bw, bh, col); return; }
    for (pass = 0; pass < 2; pass++) {   /* bounds, then lines */
        for (pi = part_lo; pi < part_hi; pi++) {
            const mech3d_part *pt = &m->parts[pi];
            const wtb_object *o;
            int q, a;
            if (pt->hidden || pt->model.object_count <= 0) continue;
            if (g_wire_unit && pt->group >= 1 && pt->group <= 8 && pt->group != 3 && g_wire_unit->loc_gone[pt->group - 1]) continue;
            lvl = loc_level(g_wire_unit, pt->group);
            o = &pt->model.objects[0];
            for (q = 0; q < o->poly_count; q++) {
                const wtb_poly *pg = &o->polys[q];
                float px[WTB_MAX_POLY_VERTS], py[WTB_MAX_POLY_VERTS];
                for (a = 0; a < pg->n && a < WTB_MAX_POLY_VERTS; a++) {
                    const wtb_vertex *v = &o->verts[pg->idx[a]];
                    float x, y, z, rx, rz;
                    if (pt->has_rot) {
                        const float *R = pt->rot;
                        x = R[0] * (float)v->x + R[1] * (float)v->y + R[2] * (float)v->z + (float)pt->pos[0];
                        y = R[3] * (float)v->x + R[4] * (float)v->y + R[5] * (float)v->z + (float)pt->pos[1];
                        z = R[6] * (float)v->x + R[7] * (float)v->y + R[8] * (float)v->z + (float)pt->pos[2];
                    } else {
                        float c2 = cosf(pt->yaw * 3.14159265f / 180.0f), s2 = sinf(pt->yaw * 3.14159265f / 180.0f);
                        x = c2 * (float)v->x + s2 * (float)v->z + (float)pt->pos[0];
                        y = (float)v->y + (float)pt->pos[1];
                        z = -s2 * (float)v->x + c2 * (float)v->z + (float)pt->pos[2];
                    }
                    /* into the camera's frame: across = along its right, depth = along its forward (the same basis
                     * the target bracket projects with) */
                    (void)cy; (void)sy;
                    rx = g_wire_r[0] * x + g_wire_r[1] * z; rz = g_wire_f[0] * x + g_wire_f[1] * z;
                    px[a] = rx;
                    py[a] = -(y * ce - rz * se);
                    if (pass == 0) {
                        if (px[a] < mn[0]) mn[0] = px[a];
                        if (px[a] > mx[0]) mx[0] = px[a];
                        if (py[a] < mn[1]) mn[1] = py[a];
                        if (py[a] > mx[1]) mx[1] = py[a];
                    }
                }
                if (pass == 1)
                    for (a = 0; a < pg->n && a < WTB_MAX_POLY_VERTS; a++) {
                        int b2 = (a + 1) % pg->n, nseg = nsegs[lvl];
                        float *seg;
                        if (nseg == caps[lvl]) { float *g2 = realloc(segs[lvl], (size_t)(caps[lvl] ? caps[lvl] * 2 : 4096) * 4 * sizeof *g2); if (!g2) continue; segs[lvl] = g2; caps[lvl] = caps[lvl] ? caps[lvl] * 2 : 4096; }
                        seg = segs[lvl];
                        seg[nseg * 4] = ox + px[a] * k; seg[nseg * 4 + 1] = oy + py[a] * k;
                        seg[nseg * 4 + 2] = ox + px[b2] * k; seg[nseg * 4 + 3] = oy + py[b2] * k;
                        nsegs[lvl]++;
                    }
            }
        }
        if (pass == 0) {
            float w = mx[0] - mn[0], h = mx[1] - mn[1];
            if (w <= 0 || h <= 0) return;
            k = 0.85f * (bw / w < bh / h ? bw / w : bh / h);
            ox = bx + bw * 0.5f - (mn[0] + mx[0]) * 0.5f * k;
            oy = by + bh * 0.5f - (mn[1] + mx[1]) * 0.5f * k;
        }
    }
    for (lvl = 0; lvl < 3; lvl++) {   /* black (a destroyed centre torso) is the background: nothing to draw */
        float c[4];
        if (!nsegs[lvl]) continue;
        if (lvl == 0) { memcpy(c, col, sizeof c); }
        else { c[0] = (float)(LOC_RGB[lvl] >> 16) / 255.0f; c[1] = (float)((LOC_RGB[lvl] >> 8) & 255) / 255.0f; c[2] = (float)(LOC_RGB[lvl] & 255) / 255.0f; c[3] = 1.0f; }
        hud_lines(g_hud, segs[lvl], nsegs[lvl], 1.0f, c);
    }
}
/* The DOS edition's scene (MW2.EXE): the palette - the mission's PALG chunk lists PAL record ids per time-of-day slot,
 * slot 0 the day's (0x4e870 -> 0x3eaa0; JACK: 54 = JACK_NI); the shading table - the planet's LTBL names the LUMA record
 * (JACK: 2, STANDARD); the light - the planet's LITE {+8 x, +12 y, +16 z, +20 ambient, +22 directional, +24 the distance
 * per darker level}. The time-of-day fades between slots are not reproduced. */
static float g_dos_lite[6] = {0, 20000, 0, 72, 0, 0};
/* the DOS time of day (0x14ee0 / 0x14f80): the PALG slots' PAL records (6-bit), the start time (INIT +8), the day length
 * (PLNT +12), the haze band height (HRZM +4, shown unless PLNT +68) */
static uint8_t g_dos_slotpal[20][768];
static int     g_dos_slot_ok[20], g_dos_t0, g_dos_day = 0, g_dos_group = -1, g_dos_evals;
static float   g_dos_band;
static uint8_t g_dos_from[768], g_dos_to[768], g_dos_cur[768];
static int32_t g_dos_fade_t0, g_dos_fade_len, g_dos_next_eval;
static void dos_apply_palette(glr *r, const uint8_t *p6)
{
    r_target holder;
    if (r_init(&holder, 1, 1) != 0) return;
    r_set_palette(&holder, p6, 768);
    memcpy(g_scene.palette, holder.palette, sizeof g_scene.palette);
    r_free(&holder);
    glr_set_dos_palette(r, (const uint8_t (*)[3])g_scene.palette);
    if (g_hud) {   /* the HUD is drawn in the same VGA palette (the time of day, light amplification) */
        int k;
        hud_repalette(g_hud, p6);
        for (k = 0; k < 256; k++) hud_pal_color(g_hud, k, g_hp[k]);
    }
}
/* every 0x71c ticks: t = (T0 + clock / 182) % day; hour = day / 24; groups: night before 5 h, dawn 5-7, day 7-17, dusk
 * 17-19, night after; group -> slot {dawn 4, day 0, dusk 4, night 8} faded over {2730, 2730, 3640, 2730} ticks; the first
 * evaluation fades over 0x16c ticks from slot 0's palette. The fade is linear in the 6-bit values, truncated. */
static int g_vision;
static void dos_time_of_day(glr *r, int32_t now_ms)
{
    /* light amplification (0x10007a50, the DOS edition's palette): the scene palette fades to PALG slot 12 (INFRARED)
     * over 0xb5 ticks and the day / night cycle stops (DAT_1007b060 = 0); off: back to the time of day's slot over the
     * same 0xb5 ticks */
    static int amp;
    int want_amp = g_vision == 1 && g_dos_slot_ok[12];
    if (want_amp != amp) {
        static const int SLOT[4] = {4, 0, 4, 8};
        amp = want_amp;
        memcpy(g_dos_from, g_dos_evals ? g_dos_cur : g_dos_slotpal[0], 768);
        memcpy(g_dos_to, amp ? g_dos_slotpal[12] : g_dos_slotpal[g_dos_group >= 0 ? SLOT[g_dos_group] : 0], 768);
        g_dos_fade_t0 = now_ms; g_dos_fade_len = 0xb5 * 1000 / 182;
        if (!g_dos_evals) g_dos_evals = 1;
    }
    if (amp) g_dos_next_eval = now_ms + 0x71c * 1000 / 182;   /* the cycle frozen */
    if (!g_dos_day && !amp && g_dos_fade_len <= 0) return;
    if (g_dos_day && now_ms >= g_dos_next_eval) {
        static const int SLOT[4] = {4, 0, 4, 8}, TICKS[4] = {2730, 2730, 3640, 2730};
        int tt = (g_dos_t0 + now_ms / 1000) % g_dos_day, H = g_dos_day / 24, grp;
        grp = tt < 5 * H ? 3 : tt < 7 * H ? 0 : tt < 17 * H ? 1 : tt < 19 * H ? 2 : 3;
        g_dos_next_eval = now_ms + 0x71c * 1000 / 182;
        if (grp != g_dos_group && g_dos_slot_ok[SLOT[grp]]) {
            memcpy(g_dos_from, g_dos_evals ? g_dos_cur : g_dos_slotpal[0], 768);
            memcpy(g_dos_to, g_dos_slotpal[SLOT[grp]], 768);
            g_dos_fade_t0 = now_ms;
            g_dos_fade_len = (g_dos_evals ? TICKS[grp] : 0x16c) * 1000 / 182;
            g_dos_group = grp;
        }
        g_dos_evals++;
    }
    if (g_dos_fade_len > 0) {
        int32_t k = now_ms - g_dos_fade_t0;
        int i;
        if (k >= g_dos_fade_len) { k = g_dos_fade_len; }
        for (i = 0; i < 768; i++) g_dos_cur[i] = (uint8_t)((int)g_dos_from[i] + ((int)g_dos_to[i] - (int)g_dos_from[i]) * k / g_dos_fade_len);
        dos_apply_palette(r, g_dos_cur);
        if (k >= g_dos_fade_len) g_dos_fade_len = 0;
    }
}
static void dos_scene(glr *r, prj_archive *ma, const char *mission)
{
    bwd_mission bm;
    char name[16];
    int k, c, pal_id = -1, luma_id = 2;
    static uint8_t luma[4096];
    int have_luma = 0;
    snprintf(name, sizeof name, "%.4sSCN1", mission);
    g_dos_day = 0; g_dos_group = -1; g_dos_evals = 0; g_dos_band = 0; g_dos_next_eval = 0; g_dos_fade_len = 0; g_dos_t0 = 0;
    if (bwd_mission_load(ma, name, &bm) == 0) {
        for (k = 0; k < bm.record_count; k++)
            for (c = 0; c < bm.records[k].chunk_count; c++) {
                const bwd_chunk *ch = &bm.records[k].chunks[c];
                if (strcmp(ch->tag, "PALG") == 0 && ch->size >= 2 && pal_id < 0) {
                    int s2, pt = prj_find_type(ma, "PAL");
                    pal_id = ch->data[0] | ch->data[1] << 8;
                    for (s2 = 0; s2 < 20 && (size_t)(s2 * 2 + 1) < ch->size; s2++) {
                        prj_record sp;
                        g_dos_slot_ok[s2] = 0;
                        if (pt >= 0 && prj_read(ma, pt, ch->data[s2 * 2] | ch->data[s2 * 2 + 1] << 8, &sp) == PRJ_OK) {
                            if (sp.size >= 768) { memcpy(g_dos_slotpal[s2], sp.data, 768); g_dos_slot_ok[s2] = 1; }
                            prj_record_free(&sp);
                        }
                    }
                }
                else if (strcmp(ch->tag, "INIT") == 0 && ch->size >= 12) g_dos_t0 = (int32_t)(ch->data[8] | ch->data[9] << 8 | ch->data[10] << 16 | (uint32_t)ch->data[11] << 24);
                else if (strcmp(ch->tag, "PLNT") == 0 && ch->size >= 72) {
                    g_dos_day = (int32_t)(ch->data[12] | ch->data[13] << 8 | ch->data[14] << 16 | (uint32_t)ch->data[15] << 24);
                    if (ch->data[68] | ch->data[69] | ch->data[70] | ch->data[71]) g_dos_band = -1;   /* the band switched off */
                }
                else if (strcmp(ch->tag, "HRZM") == 0 && ch->size >= 8 && g_dos_band >= 0)
                    g_dos_band = (float)(int32_t)(ch->data[4] | ch->data[5] << 8 | ch->data[6] << 16 | (uint32_t)ch->data[7] << 24);
                else if (strcmp(ch->tag, "LTBL") == 0 && ch->size >= 2) luma_id = ch->data[0] | ch->data[1] << 8;
                else if (strcmp(ch->tag, "LITE") == 0 && ch->size >= 28) {
                    const uint8_t *d = ch->data;
                    g_dos_lite[0] = (float)(int32_t)(d[8] | d[9] << 8 | d[10] << 16 | (uint32_t)d[11] << 24);
                    g_dos_lite[1] = (float)(int32_t)(d[12] | d[13] << 8 | d[14] << 16 | (uint32_t)d[15] << 24);
                    g_dos_lite[2] = (float)(int32_t)(d[16] | d[17] << 8 | d[18] << 16 | (uint32_t)d[19] << 24);
                    g_dos_lite[3] = (float)(int16_t)(d[20] | d[21] << 8);
                    g_dos_lite[4] = (float)(int16_t)(d[22] | d[23] << 8);
                    g_dos_lite[5] = (float)(int32_t)(d[24] | d[25] << 8 | d[26] << 16 | (uint32_t)d[27] << 24);
                }
            }
        bwd_mission_free(&bm);
    }
    if (getenv("MW2_TEST_TOD_START")) g_dos_t0 = atoi(getenv("MW2_TEST_TOD_START"));   /* TEST ONLY: INIT +8 */
    if (pal_id >= 0) {
        prj_record pr;
        int pt = prj_find_type(ma, "PAL");
        if (pt >= 0 && prj_read(ma, pt, pal_id, &pr) == PRJ_OK) {
            r_target holder;
            if (r_init(&holder, 1, 1) == 0) {
                r_set_palette(&holder, pr.data, pr.size);
                memcpy(g_scene.palette, holder.palette, sizeof g_scene.palette);
                r_free(&holder);
            }
            prj_record_free(&pr);
        }
    }
    {
        prj_record lr;
        int lt = prj_find_type(ma, "LUMA");
        if (lt >= 0 && prj_read(ma, lt, luma_id, &lr) == PRJ_OK) {
            if (lr.size >= 4096) { memcpy(luma, lr.data + lr.size - 4096, 4096); have_luma = 1; }
            prj_record_free(&lr);
        }
    }
    if (getenv("MW2_DEBUG_DOS")) fprintf(stderr, "dos: PAL %d, LUMA %d%s, light %.0f %.0f %.0f amb %.0f dir %.0f fog %.0f\n", pal_id, luma_id,
                                         have_luma ? "" : " (missing)", (double)g_dos_lite[0], (double)g_dos_lite[1], (double)g_dos_lite[2],
                                         (double)g_dos_lite[3], (double)g_dos_lite[4], (double)g_dos_lite[5]);
    if (getenv("MW2_DEBUG_DOS")) fprintf(stderr, "dos: %d textures\n", g_scene.textures_loaded);
    if (getenv("MW2_DEBUG_DOS")) { int s2, q; for (s2 = 0; s2 < 20; s2++) if (g_dos_slot_ok[s2]) { fprintf(stderr, "slot %d:", s2); for (q = 0; q < 256; q++) if (s2 != 17 || g_dos_slotpal[s2][q * 3] != 63 || g_dos_slotpal[s2][q * 3 + 1]) fprintf(stderr, " %02x=%d,%d,%d", q, g_dos_slotpal[s2][q * 3], g_dos_slotpal[s2][q * 3 + 1], g_dos_slotpal[s2][q * 3 + 2]); fprintf(stderr, "\n"); } }
    glr_set_dos(r, (const uint8_t (*)[3])g_scene.palette, have_luma ? luma : NULL);
}

/* the HUD's palette (both editions): the 3D editions draw every HUD sprite and fill through the 16-bit table 0x10260324,
 * filled only by 0x1002ac40 (RGB555 of the PAL record in the mission's current PALG slot; 0x101b0d20 holds the slots'
 * record ids, set from the PALG chunk by 0x1003e104), so the HUD takes the mission's palette - YELL_DA's indices 6 / 7 / 13
 * are (0,0,49) / (0,12,53) / (0,39,0) where the COCKPIT record has (0,0,47) / (15,15,63) / (0,31,0); the DOS edition draws
 * the HUD in the VGA palette, the same records (DOSBox YELLSCN1: viewport frame 0,0,199 and the bars' light blue
 * 0,48,215 = YELL_DA 6 / 7). The slot: StartPalettes (0x1002b0c0) shows slot 0x10 (BLACK), then the first time-of-day
 * evaluation (0x10007900 -> 0x100079d0 -> 0x1002b090) fades to the start time's slot - dawn 4, day 0, dusk 4, night 8 -
 * and loads the table when the fade ends (0x1002ac00). Later slots: the 3D editions reload the table at the end of each
 * palette fade that does not return (pal3_upkeep: light amplification, the time of day it re-evaluates); the DOS
 * edition's HUD follows its VGA palette (dos_apply_palette). g_hp: the palette's colours for the HUD's fills (the band
 * painters 0x1001c810 / 0x1001c960 take indices c-1, c, c-1, c-2).
 * The start time: DOS takes INIT +8 as seconds (0x4e856 -> 0x957c8; t = (start + clock s) % day, 0x14f80). The 3D
 * editions' loader stores INIT +8 in the group variable DAT_1024a980 instead (0x1003e0ed; the network game's MW2DIF +0xb
 * when DAT_1024ffcc), and 0x10007870 makes the start the boundary table 0x1007b068[INIT +8]: dawn's 5 h for INIT +8 = 0
 * (BLON, BRON, GOLD, JACK, SHEP), 7 / 17 / 19 h for 1-3 - the data's other values (50 - 20000, and -20000) index past the
 * four entries into the DLL's .bss / .text (tod3d_start): most read 0 (night), the 100 - 300 group ".SFL" (dawn at
 * day 1000, MARO day, SIOU night), PROV code bytes (dusk). */
static int tod3d_start(int t0, int day)
{
    /* 0x10007870: start = the dword at 0x1007b068 + 4 x INIT +8 (MW2.DLL .bss, read at FirstEnvironment(), after
     * LoadWorld() and CollectMissionAudio()), as a freshly loaded DLL holds it - which every mission of a play session
     * is: SimMain (0x10008850, export 1) plays one mission per DLL load. Its loop runs while DAT_1024ac64 < 3, and
     * DAT_1024ac64 (.data, 0 in the file) is only ever raised (= 3 / + 1 / + 2 / + 0x29a: 0x10008e15, 0x1000925f,
     * 0x100094e7, 0x10033acc, 0x100376b0 ..., 0x10037f8d), never set back, nor is DAT_1024ac60 - so a second SimMain in
     * the same load would end at once: the host (MECH2.EXE, which runs ShellMain and SimMain; the DLL says "This program
     * must be launched from MECH2.EXE") loads MW2.DLL afresh for each mission, as the DOS MECH2.EXE runs MW2.EXE.
     * 0..3: the boundary table it has just
     * written; past it (every writer of those addresses in d3fx.asm):
     *  - 0x1007b078 the 0x100-byte path buffer of 0x100085d0 ("keating\*.sfl" / "keating"): +0xb8 (50), +0xe0 (60) never
     *    written -> 0;
     *  - 0x1007b178 the KEATING file table (0x100084e0: 200 x 20-byte {8.3 name, next}, the CD's 51 *.SFL names): every
     *    INIT +8 that is a multiple of 5 lands on byte 8 of an entry - ".SFL" (0x4c46532e) for entries 0-50 (100, 150, 225,
     *    250, 300), 0 past them (350 - 800);
     *  - 0x1007c118 the table's 101 hash buckets: 1100 -> 32, 1150 -> 82, both empty for the 51 names -> 0;
     *  - 0x1007c3f8 the texture-group list (0x10023f10, at most 7 of 32 entries): 1400 -> entry 29 -> 0;
     *  - 0x1007c718 the camera-shake list (0x1002daf0, at most 3 of 10): 1500 -> entry 6's ticks -> 0;
     *  - 0x1007c890 the 8 radio-message slots (0x10031150, 0x53 bytes): 1570 -> slot 1 +13: 0 (only an earlier mission
     *    of the same DLL load - which a play session never has, above - could have put a message there);
     *  - 0x1007dc88 the mw2.mip index (0x10066570): 4700 -> texture 625's file offset, -1 (not cached);
     *  - 0x100819d8 the Glide GrState (grGlideGetState, 0x10067240, flag-2 polygons): 6750 -> +8, 0 (no such polygon
     *    is drawn before FirstEnvironment in a fresh load);
     *  - 0x1008a008 the texus quantizer (0x1006c1c0, only for P8 / AP88 / NCC; the engine converts to 0xb / 0xc):
     *    17000, 20000 -> 0;
     *  - -20000 reads .text 0x100677e8: the bytes 05 78 dc 07 of "movl $0x41000000, 0x1007dc78" (at the DLL's preferred
     *    base) = 0x07dc7805.
     * Then (0x10007900) the group of (ticks / 0xb5 + start) % day, C remainder (negative: night). Other values: the DOS
     * reading (seconds) */
    static const struct { int v; int32_t start; } PAST[] = {
        {50, 0}, {60, 0}, {100, 0x4c46532e}, {150, 0x4c46532e}, {225, 0x4c46532e}, {250, 0x4c46532e}, {300, 0x4c46532e},
        {350, 0}, {400, 0}, {500, 0}, {600, 0}, {750, 0}, {800, 0}, {1100, 0}, {1150, 0}, {1400, 0}, {1500, 0},
        {1570, 0}, {4700, -1}, {6750, 0}, {17000, 0}, {20000, 0}, {-20000, 0x07dc7805}};
    static const int B[4] = {5, 7, 17, 19};
    size_t k;
    if (t0 >= 0 && t0 <= 3) return day / 24 * B[t0];
    for (k = 0; k < sizeof PAST / sizeof PAST[0]; k++) if (PAST[k].v == t0) return PAST[k].start;
    return t0;
}
static void hud_mission_palette(prj_archive *ma, const char *mission)
{
    bwd_mission bm;
    char name[16];
    int k, c, q, slot = 0, t0 = 0, day = 0;
    memset(g_pal_slot_ok, 0, sizeof g_pal_slot_ok);
    g_plnt52 = 0;
    snprintf(name, sizeof name, "%.4sSCN1", mission);
    if (mission && bwd_mission_load(ma, name, &bm) == 0) {
        for (k = 0; k < bm.record_count; k++)
            for (c = 0; c < bm.records[k].chunk_count; c++) {
                const bwd_chunk *ch = &bm.records[k].chunks[c];
                if (strcmp(ch->tag, "PALG") == 0 && !g_pal_slot_ok[0]) {
                    int pt = prj_find_type(ma, "PAL");
                    for (q = 0; q < 20 && (size_t)(q * 2 + 1) < ch->size; q++) {
                        prj_record sp;
                        if (pt >= 0 && prj_read(ma, pt, ch->data[q * 2] | ch->data[q * 2 + 1] << 8, &sp) == PRJ_OK) {
                            if (sp.size >= 768) { memcpy(g_pal_slot[q], sp.data + sp.size - 768, 768); g_pal_slot_ok[q] = 1; }
                            prj_record_free(&sp);
                        }
                    }
                }
                else if (strcmp(ch->tag, "INIT") == 0 && ch->size >= 12) t0 = (int32_t)(ch->data[8] | ch->data[9] << 8 | ch->data[10] << 16 | (uint32_t)ch->data[11] << 24);
                else if (strcmp(ch->tag, "PLNT") == 0 && ch->size >= 16) {
                    day = (int32_t)(ch->data[12] | ch->data[13] << 8 | ch->data[14] << 16 | (uint32_t)ch->data[15] << 24);
                    if (ch->size >= 56) g_plnt52 = (int32_t)(ch->data[52] | ch->data[53] << 8 | ch->data[54] << 16 | (uint32_t)ch->data[55] << 24);
                }
            }
        bwd_mission_free(&bm);
    }
    if (getenv("MW2_TEST_TOD_START")) t0 = atoi(getenv("MW2_TEST_TOD_START"));   /* TEST ONLY: INIT +8 */
    g_tod_day = day;
    g_tod_start = t0;
    if (day > 0) {   /* the time of day at the start (0x10007900: (ticks / 0xb5 + start) % day; hour = day / 24) */
        if (g_tex_kind != 3) g_tod_start = tod3d_start(t0, day);   /* 0x10007870 */
        slot = TOD_SLOT[tod_group(g_tod_start % day, day)];
        if (!g_pal_slot_ok[slot]) slot = 0;
    }
    g_hud_slot = -1;
    if (g_pal_slot_ok[slot]) hud_slot(slot);
    else for (k = 0; k < 256; k++) hud_pal_color(g_hud, k, g_hp[k]);
    if (getenv("MW2_DEBUG_HUDPAL")) fprintf(stderr, "hud palette: %s slot %d (%s) start %d day %d plnt52 %d\n", name, slot, g_pal_slot_ok[slot] ? "PALG" : "COCKPIT", g_tod_start, day, g_plnt52);
}

/* the panel (0x10065cb0 -> 0x10065df0), laid out by 0x10065580 from the menu record: rect -> pixels (0x10005b70: fraction
 * x (w - 1), rounded); row pitch = rect height x 0x10000 / (rows + 2) (16.16, rounded, 0x100654f0); the title at the
 * rect's left, half a pitch down, underlined in colour 1 one font height down for its width (0x10006520); the rows from
 * two pitches down, numbers at 0.02 of the rect's width (+0x58), texts after the width of '.' + 2 x '0' (labels too),
 * values at +0x60 (0.4; SYSTEMS STATUS 0.8); text colour 0xe (+0x34 / +0x38); the Esc row on the last row when the flags
 * lack 8 (SYSTEMS STATUS 0x2), else next (0x0a). DOSBox YELLSCN1 1024 x 768: COMMAND POINT 2 title 225,241, underline
 * y 255, NOT AVAILABLE 261,276, "0" 241,299; SYSTEMS STATUS 563,267, rows from 309 every 28, Esc row 449 - all as
 * computed here. The 3Dfx DLL scales the offsets x 0.8 and adds 0x1e px on screens wider than 701 px (its 800 x 600);
 * the DOS layout is kept at every size (the in-game MAIN MENU follows the DOS capture as well) */
static void cc_draw(void)
{
    cc_row r[10];
    char title[32];
    float rect[4], vf;
    int rows, n = cc_rows(g_cc_page, title, sizeof title, r, &rows, rect, &vf), q, num = 0;
    float SH = g_render_w ? (float)g_render_h : (float)g_win_lines, SW = g_render_w ? (float)g_render_w : SH * 4.0f / 3.0f;
    float rx0 = floorf(rect[0] * (SW - 1.0f) + 0.5f), ry0 = floorf(rect[1] * (SH - 1.0f) + 0.5f);
    float rw = floorf(rect[2] * (SW - 1.0f) + 0.5f) - rx0, rh = floorf(rect[3] * (SH - 1.0f) + 0.5f) - ry0;
    int32_t step = 0x10000 / (rows + 2);
#define OFS(len, f16) floorf((float)(((int64_t)(len) * (f16) + 0x8000) >> 16))
    float pitch = OFS(rh, step), ux = 640.0f / SW, uy = 480.0f / SH;
    float tx = rx0 * ux, ty = (ry0 + OFS(rh, step / 2)) * uy, nx = (rx0 + OFS(rw, 0x51f)) * ux, vx = (rx0 + OFS(rw, (int32_t)(vf * 65536.0f + 0.5f))) * ux;
    float iy = ry0 + OFS(rh, step * 2), lx = hud_text_width(g_hud, ".") + 2.0f * hud_text_width(g_hud, "0"), fh = hud_text_height(g_hud);
#undef OFS
    const float *col = HP(0xe);
    hud_text(g_hud, tx, ty, title, col);
    hud_line(g_hud, tx, ty + fh, tx + hud_text_width(g_hud, title), ty + fh, 1.0f, HP(1));
    for (q = 0; q < n; q++) {
        int esc = r[q].num == 0 && strcmp(r[q].text, "(Esc to exit)") == 0, line = q;
        float y;
        if (esc && g_cc_page == CC_SYSTEMS) line = rows - 1;   /* flags without 8: the accept row at the bottom */
        y = (iy + (float)line * pitch) * uy;
        if (r[q].num > 0 || esc) {
            char nb[12];
            snprintf(nb, sizeof nb, "%d", r[q].num);
            hud_text(g_hud, nx, y, nb, col);
            num++;
        }
        hud_text(g_hud, nx + lx, y, r[q].text, col);
        if (r[q].val[0]) hud_text(g_hud, vx, y, r[q].val, col);
    }
    (void)num;
    if (getenv("MW2_CC_TRACE")) fprintf(stderr, "cc layout: title %.1f,%.1f rows %.1f,%.1f pitch %.2f value x %.1f\n", tx, ty, nx, iy * uy, pitch * uy, vx);
}

/* ---- the in-game MAIN MENU (GAMEKEY.MAP MAIN_MENU, Esc). The 3D editions' menu system (MW2.DLL 0x10065xxx): menu 4,
 * table 0x10264980 - MAIN MENU: 1 Abort Mission, 2 Graphics, 3 Audio Ctrl, 4 Combat Variables, 5 Flee to Windows,
 * 0 Accept (Esc to cancel); flags 0x11 (the controls off while open, 0x1003a3c0); while it is open the game is paused
 * (WinMain 0x10009770: "pause timer TRUE", the effect samples end, voice and music pause - 0x10031f30).
 * Pages (the item records {type, text, draw, -, submenu}: type 0 a submenu, 1 an item, 2 Accept, 3 a label):
 *   ABORT MISSION  0x102646d0: "Confirmation requested", 1 Accept -> 0x10064280: action 0x3b (EXIT_SIM)
 *   FLEE TO WINDOWS 0x10264828: "Confirm your cowardice", 1 Accept -> 0x100642e0: leave the game
 *   GRAPHICS 0x10265570: Textured Sky, Textured Ground (Off / On; off = flat in the texture's average colour,
 *            0x10027c80 / 0x1002d330), Resolution 800x600 (No / Yes; its commit is empty) - the port: Renderer and
 *            Textures in its place (the shell's RENDERER / TEXTURES) and HUD layout (Wide / Original: mw2port.cfg
 *            hud_layout=, the port's own; Accept stays on the sixth row). In a DOS mission the first row is the port's
 *            DOS shading (Dithered / Smooth: mw2port.cfg dos_shading=) - Textured Sky has nothing to switch there (the DOS look
 *            fills the sky flat at the horizon, 0x3c000, and draws no sky texture)
 *   SET AUDIO VOLUME 0x10265290: Music, Sound Effects, Voice - sliders 0..1 in 0x199a steps (0x10064780), applied as
 *            they move (0x10031960), restored by Esc (0x10031b50)
 *   COMBAT VARIABLES 0x10265068: Object textmaps, Terrain textmaps (Off / On), Display detail, Object density
 *            (Low / High), Explosion chunks (Off / On) - set on Accept (0x100652f0 ...), Esc keeps the old ones
 * Keys (0x10065a70 / 0x10065d40 / 0x10065df0): a digit picks an item (0 = Accept) - a submenu opens, a value item
 * steps up; Up / Shift+Tab and Down / Tab move; Right / Space step a value up, Left down; Enter takes the item; Esc
 * goes back without keeping the page's changes. Sounds: 0xdb (MECGCLX1) on a move, 0xe1 (MECMENU1) on opening a page.
 * Look and place: DOSBox (the DOS edition's same menu, at 1024 x 768) - WESCSIM's frame at (160, 78), text in
 * (170, 170, 170), the chosen item (211, 0, 0), the title's underline (121, 97, 0); text x 180 (numbers) / 192, the
 * title at y 113.75, rows from 167.5 every 36.25 (Accept on the sixth); values at x 385.6; sliders from 386.9 -
 * MENDL, MBAR, MENDR with the MSLIDE knob, centred on the row. */
enum { MI_SUB, MI_VAL, MI_ACCEPT, MI_LABEL, MI_CONFIRM, MI_SLIDER };
enum { MP_MAIN, MP_ABORT, MP_GRAPHICS, MP_AUDIO, MP_COMBAT, MP_FLEE, MP_GRAPHICS_DOS, MP_COUNT };
enum { MV_SKY, MV_GROUND, MV_VIDEO, MV_TEXSET, MV_MUSIC, MV_EFFECTS, MV_VOICE, MV_OBJTEX, MV_TERTEX, MV_DETAIL, MV_DENSITY, MV_CHUNKS, MV_HUDLAYOUT, MV_DOSSKY, MV_COUNT };
typedef struct { int type; const char *text; int arg; } mm_item;
typedef struct { const char *title; int n; mm_item it[6]; } mm_page;
static const mm_page MM_PAGE[MP_COUNT] = {
    {"MAIN MENU", 6, {{MI_SUB, "Abort Mission", MP_ABORT}, {MI_SUB, "Graphics", MP_GRAPHICS}, {MI_SUB, "Audio Ctrl", MP_AUDIO},
                      {MI_SUB, "Combat Variables", MP_COMBAT}, {MI_SUB, "Flee to Windows", MP_FLEE}, {MI_ACCEPT, "Accept (Esc to cancel)", 0}}},
    {"ABORT MISSION", 2, {{MI_LABEL, "Confirmation requested", 0}, {MI_CONFIRM, "Accept (Esc to cancel)", 0}}},
    {"GRAPHICS", 6, {{MI_VAL, "Textured Sky", MV_SKY}, {MI_VAL, "Textured Ground", MV_GROUND}, {MI_VAL, "Renderer", MV_VIDEO},
                     {MI_VAL, "Textures", MV_TEXSET}, {MI_VAL, "HUD layout", MV_HUDLAYOUT}, {MI_ACCEPT, "Accept (Esc to cancel)", 0}}},
    {"SET AUDIO VOLUME", 4, {{MI_SLIDER, "Music", MV_MUSIC}, {MI_SLIDER, "Sound Effects", MV_EFFECTS}, {MI_SLIDER, "Voice", MV_VOICE},
                             {MI_ACCEPT, "Accept (Esc to cancel)", 0}}},
    {"COMBAT VARIABLES", 6, {{MI_VAL, "Object textmaps", MV_OBJTEX}, {MI_VAL, "Terrain textmaps", MV_TERTEX}, {MI_VAL, "Display detail", MV_DETAIL},
                             {MI_VAL, "Object density", MV_DENSITY}, {MI_VAL, "Explosion chunks", MV_CHUNKS}, {MI_ACCEPT, "Accept (Esc to cancel)", 0}}},
    {"FLEE TO WINDOWS", 2, {{MI_LABEL, "Confirm your cowardice", 0}, {MI_CONFIRM, "Accept (Esc to cancel)", 1}}},
    {"GRAPHICS", 6, {{MI_VAL, "DOS shading", MV_DOSSKY}, {MI_VAL, "Textured Ground", MV_GROUND}, {MI_VAL, "Renderer", MV_VIDEO},   /* a DOS mission's */
                     {MI_VAL, "Textures", MV_TEXSET}, {MI_VAL, "HUD layout", MV_HUDLAYOUT}, {MI_ACCEPT, "Accept (Esc to cancel)", 0}}},
};
static const char *const MM_OFFON[2] = {"Off", "On"}, *const MM_LOWHIGH[2] = {"Low", "High"};
static const char *const MM_ED_TEXT[PORTCFG_ED_COUNT] = {"Enhanced", "DOS", "3Dfx", "ATi", "S3", "PowerVR", "Matrox", "Rendition"};
static const char *const MM_TX_TEXT[PORTCFG_TX_COUNT] = {"Original", "Titanium"};
static const char *const MM_HUD_TEXT[2] = {"Wide", "Original"};
static const char *const MM_DOSSKY_TEXT[2] = {"Dithered", "Smooth"};
static int  g_mm_stack[8], g_mm_depth;          /* open pages; empty = closed */
static int  g_mm_sel;                           /* the chosen item of the top page */
static int  g_mm_v[MV_COUNT], g_mm_old[MV_COUNT];   /* the pages' values (sliders 0..0x10000), as opened */
static int  g_mm_eds[PORTCFG_ED_COUNT], g_mm_ned = -1;   /* the renderers the Renderer item offers */
static int  g_mm_tx_lock = -1;                  /* Textures locked to Original: a DOS mission, or no titanium/ folder */
static int  g_mm_vol[3] = {-1, -1, -1};         /* music, effects, voice in force (16.16; -1 = not read yet) */
static int  g_mm_ed_req = -1, g_mm_tx_req = -1, g_mm_world_dirty, g_mm_quit;
static void mm_read_volumes(void)
{
    unsigned char snd[60];
    char p[1100];
    FILE *f;
    int k;
    for (k = 0; k < 3; k++) g_mm_vol[k] = 0x10000;
    if (!getenv("MW2_INSTALL_DIR")) return;
    snprintf(p, sizeof p, "%s/MW2SND.CFG", getenv("MW2_INSTALL_DIR"));
    if (!(f = fopen(p, "rb"))) return;
    if (fread(snd, 1, sizeof snd, f) == sizeof snd)
        for (k = 0; k < 3; k++) {   /* MW2SND.CFG +0xc music, +4 effects, +8 voice (16.16) */
            static const int OFS[3] = {12, 4, 8};
            const unsigned char *q = snd + OFS[k];
            long v = (long)(q[0] | q[1] << 8 | q[2] << 16 | (unsigned long)q[3] << 24);
            g_mm_vol[k] = v < 0 ? 0 : v > 0x10000 ? 0x10000 : (int)v;
        }
    fclose(f);
}
/* MW2SND.CFG: int32 fields at these offsets (the shell's Combat Variables screen and the original keep them there) */
static void mm_write_cfg(const int *ofs, const int *val, int n)
{
    unsigned char buf[4096];
    char p[1100];
    FILE *f;
    size_t len = 0;
    int k;
    if (!getenv("MW2_INSTALL_DIR")) return;
    snprintf(p, sizeof p, "%s/MW2SND.CFG", getenv("MW2_INSTALL_DIR"));
    memset(buf, 0, sizeof buf);
    if ((f = fopen(p, "rb"))) { len = fread(buf, 1, sizeof buf, f); fclose(f); }
    for (k = 0; k < n; k++) {
        uint32_t v = (uint32_t)val[k];
        buf[ofs[k]] = (unsigned char)v; buf[ofs[k] + 1] = (unsigned char)(v >> 8); buf[ofs[k] + 2] = (unsigned char)(v >> 16); buf[ofs[k] + 3] = (unsigned char)(v >> 24);
        if ((size_t)ofs[k] + 4 > len) len = (size_t)ofs[k] + 4;
    }
    if ((f = fopen(p, "wb"))) { fwrite(buf, 1, len, f); fclose(f); }
}
static void mm_apply_volumes(const int *mus_fx_vo)
{
    if (g_sfx) sfx_set_gains(g_sfx, (float)mus_fx_vo[1] / 65536.0f, (float)mus_fx_vo[2] / 65536.0f);
    if (g_cd) { int v = mus_fx_vo[0] * 255 / 0x10000; sfx_lock(g_sfx); cdaudio_set_volume(g_cd, (uint8_t)v, (uint8_t)v); sfx_unlock(g_sfx); }
    g_voice_off = mus_fx_vo[2] <= 0;
}
/* Textures can't change: locked to Original (g_mm_tx_lock), or to Titanium with the RENDITION renderer (a Titanium plugin) */
static int mm_tx_locked(void) { return g_mm_tx_lock > 0 || g_edition == PORTCFG_ED_RENDITION; }
static int mm_value_count(int mv) { return mv == MV_VIDEO ? (g_mm_ned > 0 ? g_mm_ned : 1) : mv == MV_TEXSET && mm_tx_locked() ? 1 : 2; }
/* an item that can't change here (the port's Textures when locked): drawn dim, passed over by the arrows */
static int mm_locked(const mm_item *it) { return it->type == MI_VAL && it->arg == MV_TEXSET && mm_tx_locked(); }
static const char *mm_value_text(int mv)
{
    if (mv == MV_VIDEO) return MM_ED_TEXT[g_mm_ned > 0 ? g_mm_eds[g_mm_v[mv]] : g_edition];
    if (mv == MV_TEXSET) return MM_TX_TEXT[g_mm_v[mv] ? 1 : 0];
    if (mv == MV_HUDLAYOUT) return MM_HUD_TEXT[g_mm_v[mv] ? 1 : 0];
    if (mv == MV_DOSSKY) return MM_DOSSKY_TEXT[g_mm_v[mv] ? 1 : 0];
    return (mv == MV_DETAIL || mv == MV_DENSITY ? MM_LOWHIGH : MM_OFFON)[g_mm_v[mv] ? 1 : 0];
}
/* a page's values as they stand (0x10064d90 state 1: each item's getter) */
static void mm_load_page(int pg)
{
    int k;
    if (pg == MP_GRAPHICS || pg == MP_GRAPHICS_DOS) {
        g_mm_v[MV_DOSSKY] = g_dos_sky_smooth;
        g_mm_v[MV_SKY] = !(g_view && g_view->flat_sky);
        g_mm_v[MV_GROUND] = !(g_view && g_view->flat_ground);
        if (g_mm_ned < 0) {   /* the 3D editions whose files are present (none to switch to in a DOS mission) */
            portcfg pc;
            char a[PORTCFG_PATH], b[PORTCFG_PATH], c[PORTCFG_PATH], d[PORTCFG_PATH], e[PORTCFG_PATH], *bp = SDL_GetBasePath();
            portcfg_load(NULL, &pc); portcfg_resolve(&pc, bp, NULL); SDL_free(bp);
            g_mm_ned = 0;
            if (g_tex_kind != 3)
                for (k = 0; k < PORTCFG_ED_COUNT; k++)
                    if (k != PORTCFG_ED_DOS && (k == g_edition || portcfg_edition(&pc, k, a, b, c, d, e) == 0)) g_mm_eds[g_mm_ned++] = k;
        }
        g_mm_v[MV_VIDEO] = 0;
        for (k = 0; k < g_mm_ned; k++) if (g_mm_eds[k] == g_edition) g_mm_v[MV_VIDEO] = k;
        if (g_mm_tx_lock < 0) {   /* the Titanium textures: not in a DOS mission, only with the titanium/ folder */
            portcfg pc;
            char *bp = SDL_GetBasePath();
            portcfg_load(NULL, &pc); portcfg_resolve(&pc, bp, NULL); SDL_free(bp);
            g_mm_tx_lock = g_tex_kind == 3 || !portcfg_texset_ok(&pc, g_edition, PORTCFG_TX_TITANIUM);
        }
        g_mm_v[MV_TEXSET] = g_texset == PORTCFG_TX_TITANIUM && (!g_mm_tx_lock || g_edition == PORTCFG_ED_RENDITION);
        g_mm_v[MV_HUDLAYOUT] = !g_hud_wide;
    } else if (pg == MP_AUDIO) {
        if (g_mm_vol[0] < 0) mm_read_volumes();
        g_mm_v[MV_MUSIC] = g_mm_vol[0]; g_mm_v[MV_EFFECTS] = g_mm_vol[1]; g_mm_v[MV_VOICE] = g_mm_vol[2];
    } else if (pg == MP_COMBAT) {
        g_mm_v[MV_OBJTEX] = !g_notex_actors; g_mm_v[MV_TERTEX] = !g_notex_world;
        g_mm_v[MV_DETAIL] = g_lodq == 1; g_mm_v[MV_DENSITY] = !world3d_density_low;
        g_mm_v[MV_CHUNKS] = g_sim_ok ? g_sim.chunky : 1;
    }
    memcpy(g_mm_old, g_mm_v, sizeof g_mm_v);
}
/* Accept: the page's values taken (the items' commit functions) */
static void mm_commit_page(int pg)
{
    if (pg == MP_GRAPHICS || pg == MP_GRAPHICS_DOS) {
        if (g_view) { g_view->flat_sky = !g_mm_v[MV_SKY]; g_view->flat_ground = !g_mm_v[MV_GROUND]; }
        if (g_mm_ned > 0 && g_mm_eds[g_mm_v[MV_VIDEO]] != g_edition) g_mm_ed_req = g_mm_eds[g_mm_v[MV_VIDEO]];
        if (!mm_tx_locked() && g_mm_v[MV_TEXSET] != (g_texset == PORTCFG_TX_TITANIUM)) g_mm_tx_req = g_mm_v[MV_TEXSET] ? PORTCFG_TX_TITANIUM : PORTCFG_TX_ORIGINAL;
        if (g_mm_v[MV_HUDLAYOUT] != !g_hud_wide) {   /* at once, and kept in mw2port.cfg (the file's own values) */
            portcfg raw;
            g_hud_wide = !g_mm_v[MV_HUDLAYOUT];
            portcfg_load(NULL, &raw);
            raw.hud_original = !g_hud_wide;
            portcfg_save(NULL, &raw);
        }
        if (g_mm_v[MV_DOSSKY] != g_dos_sky_smooth) {   /* the same: at once and in mw2port.cfg (dos_shading=) */
            portcfg raw;
            g_dos_sky_smooth = g_mm_v[MV_DOSSKY];
            portcfg_load(NULL, &raw);
            raw.dos_sky_smooth = g_dos_sky_smooth;
            portcfg_save(NULL, &raw);
        }
    } else if (pg == MP_AUDIO) {
        static const int OFS[3] = {12, 4, 8};
        g_mm_vol[0] = g_mm_v[MV_MUSIC]; g_mm_vol[1] = g_mm_v[MV_EFFECTS]; g_mm_vol[2] = g_mm_v[MV_VOICE];
        mm_apply_volumes(g_mm_vol);
        mm_write_cfg(OFS, g_mm_vol, 3);
    } else if (pg == MP_COMBAT) {
        static const int OFS[5] = {0x14, 0x18, 0x1c, 0x20, 0x24};
        int val[5], k;
        g_notex_actors = !g_mm_v[MV_OBJTEX]; g_notex_world = !g_mm_v[MV_TERTEX];
        g_lodq = g_mm_v[MV_DETAIL] ? 1 : 2; fx_detail_low = g_lodq == 2;
        if (world3d_density_low != !g_mm_v[MV_DENSITY]) {   /* 0x1000de50: the type 0xc0 objects unlinked / linked again */
            world3d_density_low = !g_mm_v[MV_DENSITY];
            for (k = 0; k < g_mech.part_count; k++) if (g_mech.parts[k].sparse) g_mech.parts[k].hidden = world3d_density_low;
            g_mm_world_dirty = 1;
        }
        if (g_sim_ok) g_sim.chunky = g_mm_v[MV_CHUNKS];
        for (k = 0; k < 5; k++) val[k] = g_mm_v[MV_OBJTEX + k] ? 1 : 0;
        mm_write_cfg(OFS, val, 5);
    }
}
static void mm_revert_page(int pg) { if (pg == MP_AUDIO) mm_apply_volumes(g_mm_vol); }   /* 0x10031b50 */
static int mm_open(void)
{
    if (g_mm_depth) return 0;
    g_mm_stack[0] = MP_MAIN; g_mm_depth = 1; g_mm_sel = 0;
    g_mm_timer = SDL_GetTicks();   /* 0x10065880: the menu's timer reset as it opens */
    if (g_sfx) sfx_pause(g_sfx, 1);
    return 1;
}
static void mm_pop(void)
{
    if (g_mm_depth > 0) g_mm_depth--;
    g_mm_sel = 0;   /* (0x10065df0 state > 3: the selection back to the first item) */
    if (!g_mm_depth && g_sfx) sfx_pause(g_sfx, 0);
}
static void mm_close_all(void) { while (g_mm_depth) mm_pop(); }
/* the numbered items: every type but labels, Accept as 0 */
static int mm_item_of_digit(const mm_page *p, int d)
{
    int k, n = 0;
    for (k = 0; k < p->n; k++) {
        if (p->it[k].type == MI_LABEL) continue;
        if (p->it[k].type == MI_ACCEPT) { if (d == 0) return k; continue; }
        if (++n == d) return mm_locked(&p->it[k]) ? -1 : k;
    }
    return -1;
}
static void mm_step(const mm_item *it, int dir)
{
    int mv = it->arg;
    if (it->type == MI_SLIDER) {
        int v = g_mm_v[mv] + dir * 0x199a;
        g_mm_v[mv] = v < 0 ? 0 : v > 0x10000 ? 0x10000 : v;
        {   /* applied as it moves (0x10031960) */
            int t[3] = {g_mm_v[MV_MUSIC], g_mm_v[MV_EFFECTS], g_mm_v[MV_VOICE]};
            mm_apply_volumes(t);
        }
    } else if (it->type == MI_VAL) {
        int n = mm_value_count(mv);
        g_mm_v[mv] = (g_mm_v[mv] + dir + n) % n;
    }
}
static void mm_activate(int k)
{
    int pg = g_mm_stack[g_mm_depth - 1];
    const mm_item *it = &MM_PAGE[pg].it[k];
    if (it->type == MI_SUB && g_mm_depth < 8) {
        int sub = it->arg == MP_GRAPHICS && g_tex_kind == 3 ? MP_GRAPHICS_DOS : it->arg;   /* a DOS mission: DOS shading first */
        g_mm_stack[g_mm_depth++] = sub; g_mm_sel = 0;
        while (g_mm_sel < MM_PAGE[sub].n && MM_PAGE[sub].it[g_mm_sel].type == MI_LABEL) g_mm_sel++;
        mm_load_page(sub);
        if (g_sfx) { float L[3], yaw; snd_listener(L, &yaw); sfx_play_at3(g_sfx, 0xe1, 0, 0, 0, yaw, 0); }   /* see mm_key */
    } else if (it->type == MI_ACCEPT) { mm_commit_page(pg); mm_pop(); }
    else if (it->type == MI_CONFIRM) {
        mm_close_all();
        if (it->arg) g_mm_quit = 1;                                          /* Flee: leave the game */
        if (g_sim_ok && g_world && !g_sim.over) msim_end_now(&g_sim);         /* Abort: as EXIT_SIM (action 0x3b) */
    }
}
static void mm_key(SDL_Keycode k, Uint16 mod)
{
    int pg = g_mm_stack[g_mm_depth - 1], prev = g_mm_sel, it;
    const mm_page *p = &MM_PAGE[pg];
    if (k == SDLK_ESCAPE) { mm_revert_page(pg); mm_pop(); return; }
    if ((k >= SDLK_0 && k <= SDLK_9) || (k >= SDLK_KP_1 && k <= SDLK_KP_0)) {
        int d = k >= SDLK_0 && k <= SDLK_9 ? (int)(k - SDLK_0) : k == SDLK_KP_0 ? 0 : (int)(k - SDLK_KP_1) + 1;
        if ((it = mm_item_of_digit(p, d)) < 0) return;
        g_mm_sel = it;
        if (p->it[it].type == MI_VAL || p->it[it].type == MI_SLIDER) mm_step(&p->it[it], 1);   /* its own digit steps it */
        else mm_activate(it);
    } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) mm_activate(g_mm_sel);
    else if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_TAB) {
        int dir = k == SDLK_UP || (k == SDLK_TAB && (mod & KMOD_SHIFT)) ? -1 : 1, n;
        for (n = 0; n < p->n; n++) {
            g_mm_sel = (g_mm_sel + dir + p->n) % p->n;
            if (p->it[g_mm_sel].type != MI_LABEL && !mm_locked(&p->it[g_mm_sel])) break;
        }
    } else if (k == SDLK_RIGHT || k == SDLK_SPACE || k == SDLK_LEFT) mm_step(&p->it[g_mm_sel], k == SDLK_LEFT ? -1 : 1);
    /* the menu's sounds (move 0x10065f6d, a page opening 0x100662a9; DOS 0x181ec / 0x18543) are positional sounds at the
     * zero vector, not halved (edx 0): level 100 (0.8), the pan by the camera's yaw against the zero vector's 45 degrees
     * (sfx_pos_pan). DOSBox YELLSCN1 (heading 158): MECGCLX1 / MECMENU1 L 0.790 / 0.794, R 0.197 / 0.200 (pan 111);
     * the view glanced right (yaw ~215): L 0.72 R 0.59 */
    if (g_mm_depth && g_mm_sel != prev && g_mm_stack[g_mm_depth - 1] == pg && g_sfx) { float L[3], yaw; snd_listener(L, &yaw); sfx_play_at3(g_sfx, 0xdb, 0, 0, 0, yaw, 0); }
}
static void mm_draw(void)
{
    static hud_sprite frame, endl, bar, endr, knob;
    static int loaded;
    static const float grey[4] = {170 / 255.0f, 170 / 255.0f, 170 / 255.0f, 1}, red[4] = {211 / 255.0f, 0, 0, 1},
                       gold[4] = {121 / 255.0f, 97 / 255.0f, 0, 1},
                       dim[4] = {85 / 255.0f, 85 / 255.0f, 85 / 255.0f, 1};   /* the port's locked item (half the text grey) */
    int pg, k, num = 0;
    const mm_page *p;
    if (!g_mm_depth || !g_hud) return;
    if (!loaded && g_ma) {
        /* the K (1024 x 768) set drawn at the DOS size (one sprite pixel per 1/768 of the height): the capture's sizes */
        hud_sprite_load(g_hud, g_ma, "WESCSIMK", 0, &frame);
        hud_sprite_load(g_hud, g_ma, "MENDLK", 0, &endl); hud_sprite_load(g_hud, g_ma, "MBARK", 0, &bar);
        hud_sprite_load(g_hud, g_ma, "MENDRK", 0, &endr); hud_sprite_load(g_hud, g_ma, "MSLIDEK", 0, &knob);
        loaded = 1;
    }
    pg = g_mm_stack[g_mm_depth - 1];
    p = &MM_PAGE[pg];
    if (frame.tex) hud_draw_f(g_hud, &frame, 160.0f, 78.0f, -1.0f);
    else { static const float black[4] = {0, 0, 0, 1}; hud_rect(g_hud, 160.0f, 78.0f, 277.5f, 324.0f, black); }
    hud_text(g_hud, 180.0f, 113.75f, p->title, grey);
    hud_line(g_hud, 179.5f, 122.5f, 180.0f + hud_text_width(g_hud, p->title), 122.5f, 1.0f, gold);
    for (k = 0; k < p->n; k++) {
        const mm_item *it = &p->it[k];
        float y = 167.5f + 36.25f * (float)(it->type == MI_ACCEPT ? 5 : k);
        const float *c = mm_locked(it) ? dim : k == g_mm_sel ? red : grey;
        char d[12];
        if (it->type == MI_LABEL) { hud_text(g_hud, 192.0f, y, it->text, grey); continue; }
        snprintf(d, sizeof d, "%d", it->type == MI_ACCEPT ? 0 : ++num);
        hud_text(g_hud, 180.0f, y, d, c);
        hud_text(g_hud, 192.0f, y, it->text, c);
        if (it->type == MI_VAL) hud_text(g_hud, 385.6f, y, mm_value_text(it->arg), c);
        else if (it->type == MI_SLIDER && bar.tex) {
            const float u = 0.625f;   /* units per K pixel */
            float cy = y + 3.4f, x = 386.9f, wl = (float)endl.w * u, wb = (float)bar.w * u;
            hud_draw_f(g_hud, &endl, x, cy - (float)endl.h * u * 0.5f, -1.0f);
            hud_draw_f(g_hud, &bar, x + wl, cy - (float)bar.h * u * 0.5f, -1.0f);
            hud_draw_f(g_hud, &endr, x + wl + wb, cy - (float)endr.h * u * 0.5f, -1.0f);
            hud_draw_f(g_hud, &knob, x + wl + wb * (float)g_mm_v[it->arg] / 65536.0f - (float)knob.w * u * 0.5f, cy - (float)knob.h * u * 0.5f, -1.0f);
        }
    }
}

/* the view distance: the planet record's VIEW chunk {i32 near, i32 far} (DOS MW2.EXE 0x4e741 -> camera +0x3c / +0x40,
 * 3Dfx 0x1003dfbc; YELLPLT1 64 / 50000, GREE 130000, JACK 250000 cm) - objects wholly beyond far are not drawn (0x3f500 /
 * 0x1002fba0), so from high up (the ejection camera) the terrain pieces go and the ground fill alone is left */
static float g_view_far;
static void load_view_far(prj_archive *ma, const char *scene)
{
    bwd_mission bm;
    int k, c;
    g_view_far = 0;
    if (bwd_mission_load(ma, scene, &bm) != 0) return;
    for (k = 0; k < bm.record_count; k++)
        for (c = 0; c < bm.records[k].chunk_count; c++) {
            const bwd_chunk *ch = &bm.records[k].chunks[c];
            if (strcmp(ch->tag, "VIEW") == 0 && ch->size >= 8 && g_view_far <= 0)
                g_view_far = (float)(int32_t)(ch->data[4] | ch->data[5] << 8 | ch->data[6] << 16 | (uint32_t)ch->data[7] << 24);
        }
    bwd_mission_free(&bm);
    if (getenv("MW2_NO_FAR_CULL")) g_view_far = 0;   /* comparison shots */
}

static int load(glr *r, prj_archive *ma, prj_archive *ta, int ati, const char *rec, const char *mission, int camo, int clan)
{
    mech3d m;
    g_snd_ok = 0; g_snd_pit = 1;   /* no camera yet: the sounds hear the cockpit eye (snd_listener) */
    if (g_have_mech) { mech3d_free(&g_mech); c3d_free_scene(&g_scene); g_have_mech = 0; }
    if (g_actors) { if (g_sim_ok) msim_free(&g_sim); g_sim_ok = 0; world3d_free_actors(g_actors, g_actor_count); g_actors = NULL; g_actor_count = 0; }
    g_have_anim = 0;
    if (g_world) {
        char scene[16];
        memset(&m, 0, sizeof m);
        snprintf(scene, sizeof scene, "%.4sSCN1", mission);
        if (world3d_mission(ma, scene, 1, &m) <= 0 || m.part_count == 0) { fprintf(stderr, "no world for %s\n", mission); return -1; }
        world3d_actors(ma, scene, &g_actors, &g_actor_count);   /* posable, drawn on the actor layer */
        if (getenv("MW2_LIST_ACTORS")) {   /* TEST ONLY: "actor <k> <group> <skeleton> flags <GPS +0x1a> at <x> <z>" */
            int k;
            for (k = 0; k < g_actor_count; k++) printf("actor %d %s %s flags %#x at %d %d\n", k, g_actors[k].group, g_actors[k].skel, g_actors[k].unit_flags, g_actors[k].mech.origin[0], g_actors[k].mech.origin[2]);
        }
        load_view_far(ma, scene);
        g_exit_fade_t0 = -1;
        combat_jet_fuel_max = g_edition == PORTCFG_ED_DOS ? 1820.0f : 1810.0f;   /* the full jet tank: DOS 0x71c (0x4ce84), 3Dfx 0x712 */
        combat_jet_refill_dos = g_edition == PORTCFG_ED_DOS;   /* DOS: whole ticks >> 2 a 30 fps frame (0x27c63) */
        g_sim_ok = msim_init(&g_sim, ma, scene, g_actors, g_actor_count) == 0;
        if (g_sim_ok && !g_drop_tex) music_start(g_sim.music_track);   /* with the drop screen: when it fades (main loop) */
        g_sim.log = sim_log;
        g_sim.dos_end = g_sim.sd_at_once = g_edition == PORTCFG_ED_DOS; combat_ct_split = g_edition == PORTCFG_ED_DOS ? 2 : 3; combat_heat_dos = g_edition == PORTCFG_ED_DOS; msim_dos_int = ai_dos_int = g_edition == PORTCFG_ED_DOS;   /* an AI self-destruct: DOS at once, 3D editions 0x16a ticks on; DOS: the integer throttle servo (0x49b80) */
        g_sim.on_message = cc_message;                 /* lancemate acknowledgements on the message bar, with voice */
        if (getenv("MW2_CC_PAGE")) g_cc_page = atoi(getenv("MW2_CC_PAGE"));   /* tests: open a COMMAND COMPUTER page */
        g_sim.on_shot = on_shot;
        g_sim.on_effect = on_effect;
        g_sim.on_sound = on_sound;
        g_sim.on_player_down = on_player_down;
        g_sim.on_radio = on_radio;
        g_sim.debug_invulnerable = getenv("MW2_DEBUG_INVULN") != NULL;   /* tests: the player survives */
        memset(&g_fxw, 0, sizeof g_fxw);
        if (!g_fx_ok) {
            g_fx_ok = fx_load(ma, &g_fxd) == 0;
            g_fx_ta = ta; g_fx_ati = ati;   /* before the dump: it loads frames */
            if (g_fx_ok && getenv("MW2_DUMP_FXTEX")) {   /* tests: list effect texture slots and dump the first frame of some */
                int q;
                for (q = 0; q < 512; q++) if (g_fxd.tex[q].frames > 0) {
                    texture *t0 = fx_texture(g_fxd.tex[q].cel[0]);
                    fprintf(stderr, "fxtex %03x frames %d rate %d %dx%d\n", q, g_fxd.tex[q].frames, g_fxd.tex[q].rate, t0 ? t0->w : 0, t0 ? t0->h : 0);
                    if (t0) { char pn[64]; FILE *tf; int i3; snprintf(pn, sizeof pn, "%s/fxtex_%03x.ppm", getenv("MW2_DUMP_FXTEX"), q); tf = fopen(pn, "wb"); fprintf(tf, "P6 %d %d 255\n", t0->w, t0->h); for (i3 = 0; i3 < t0->w * t0->h; i3++) { const unsigned char *px = (const unsigned char *)t0->rgba + i3 * 4; fputc(px[3] < 128 ? 255 : px[0], tf); fputc(px[3] < 128 ? 0 : px[1], tf); fputc(px[3] < 128 ? 255 : px[2], tf); } fclose(tf); }
                }
            }
            g_fx_ta = ta; g_fx_ati = ati;
            g_ma = ma;
        }
        g_sim.player_unit.hold_fire_above = 0;     /* a human pilot fires as they choose */
        g_sim.player_unit.human = 1;               /* edge-triggered fire, no range check (0x100437a0) */
        if (getenv("MW2_HUD_TEST")) {             /* debug: show the damage colours */
            g_sim.player_unit.armor[MEK_RA] = 0;
            g_sim.player_unit.armor[MEK_LT] /= 2;
            g_sim.player_unit.loc_gone[MEK_LL] = 1; g_sim.player_unit.internal[MEK_LL] = 0; g_sim.player_unit.armor[MEK_LL] = 0;
        }

        if (g_pl_ok) { mech3d_free(&g_pl); g_pl_ok = 0; }
        int lod_keep = mech3d_lod, pl_loaded;
        mech3d_lod = 0;   /* the player's own mech (the cockpit) always at full detail */
        pl_loaded = g_sim_ok && mech3d_load(ma, g_sim.player_skel, 0, &g_pl) == 0;
        if (pl_loaded && getenv("MW2_FLY_DUMP")) { int l, q; for (l = 1; l <= 8; l++) { fprintf(stderr, "loc %d:", l); for (q = 0; q < g_pl.part_count; q++) if (mech3d_fly_loc(&g_pl, q, 1u << l)) fprintf(stderr, " %s(%d)", g_pl.parts[q].model_name, g_pl.parts[q].group); fprintf(stderr, "\n"); } }   /* TEST ONLY */
        if (g_plpit_ok) { mech3d_free(&g_plpit); g_plpit_ok = 0; }
        /* the cockpit view (engine 0x100267d0: in the cockpit the player's object takes level of detail 4, the
         * record's fifth representation - KF5_HEAD, the upper legs and arms - drawn like any object from the eye) */
        if (pl_loaded) g_plpit_ok = mech3d_load(ma, g_sim.player_skel, 4, &g_plpit) == 0 && g_plpit.part_count > 0;
        if (g_plpit_ok && !getenv("MW2_PIT_OWN_SKEL")) mech3d_use_skeleton(&g_plpit, &g_pl);   /* the engine's single skeleton (mech3d.c) */
        if (getenv("MW2_DEBUG_PIT")) fprintf(stderr, "cockpit repr 4: %d (%d parts, repr_count %d)\n", g_plpit_ok, g_plpit.part_count, g_pl.repr_count);
        mech3d_lod = lod_keep;
        if (pl_loaded) {   /* the player's mech (USERSTAR.BWD) */
            float kps;
            int first = 0, cnt = 12;
            g_pl_ok = 1;
            g_frame_mech++;
            if (getenv("MW2_START")) {   /* debug: start the player at "x,z" (or "x,z,y": the feet's height too) */
                float sx0 = 0, sz0 = 0, sy0 = 0;
                int nst = sscanf(getenv("MW2_START"), "%f,%f,%f", &sx0, &sz0, &sy0);
                if (nst >= 2) { g_sim.player[0] = sx0; g_sim.player[2] = sz0; }
                if (nst == 3) { g_sim.player_unit.y = sy0; g_sim.player_unit.vy = 0; }
                if (getenv("MW2_START_HEADING")) g_sim.player_heading = (float)atof(getenv("MW2_START_HEADING"));   /* debug */
            }
            g_pl.origin[0] = (int32_t)g_sim.player[0]; g_pl.origin[2] = (int32_t)g_sim.player[2];
            g_pl.heading = g_sim.player_heading;
            g_throttle = 0; g_reverse = 0; g_pl_speed = 0; g_jet_vx = g_jet_vz = 0; g_pl_free = 0; g_pl_jet_fire = 0; g_pl_anim_held = 0; g_pl_free_h = 1e9f; g_pl_frac[0] = g_pl_frac[1] = 0; g_pl_across = 0; g_pl_contact = -2; g_pl_contact_acc = 0; g_pl_contact_hold = 0; g_thr_srv = 0; g_spd_srv = 0; g_drive_slow = 0; g_pl_t = 0; g_pl_ap_ok = 0; g_pitch = 0; g_pl.twist = 0; g_masc = 0; g_twist_dem = g_pitch_dem = 0; g_tgt_any = 0;
            g_pow_last = -1;   /* the start-up animation from the mission's tick 0 */
            g_sel_weapon = g_sim_ok && g_sim.player_unit.weapon_count > 0 ? weapon_next(&g_sim.player_unit, -1, 0, 0) : 0;   /* the panel's first (top left) */
            if (getenv("MW2_WEAPON_ORDER") && g_sim_ok) {   /* tests: the selector's order, with each weapon's panel window */
                int q, w = g_sel_weapon;
                for (q = 0; q < g_sim.player_unit.weapon_count; q++) { printf("weapon %d %s window %d\n", w, sim_weapon_short[g_sim.player_unit.weapons[w].weapon], weapon_window(&g_sim.player_unit, w)); w = weapon_next(&g_sim.player_unit, w, 0, 0); }
            }
            if (g_pl.anim_id && anim_load_id(ma, g_pl.anim_id, &g_pl_anim) == 0 && anim_sequence(&g_pl_anim, 0, &first, &cnt) == 0) {
                float st = mech3d_stride(&g_pl, &g_pl_anim, first, cnt);
                if (st > 1) g_pl_stride = st;
            }
            mech3d_walk_rate(ma, &g_pl, &g_pl_anim, g_sim.player_loadout, &g_pl_walk, &kps);
            if (g_pl_walk <= 0) g_pl_walk = 1500;
            if (g_sim_ok && g_sim.planet.gravity_g > 0) g_pl_walk = msim_top_speed((int)lrintf(g_pl_walk / 300.0f), g_sim.planet.gravity_g);   /* speed / gravity in DOS's integers (see msim) */
            mech3d_pose(&g_pl, &g_pl_anim, 0);
            {   /* HUD sprites and the CPIT layout. Each piece exists three times: <name> (the 320x200 set), <name>6 (640x480) and
                 * <name>K (1024x768), the font too (BASE6X7, BASE6X76, BASE6X7K) - SHP resource IDs n, n + 1, n + 2. The
                 * engine (0x100024b0, at the screen's set-up 0x10004910) takes the set index 0x102453c8 = the entry of
                 * {319,199}, {639,479}, {1023,767} (0x102492a0) nearest the screen's w x h: |(ew + 1) - w| + |(eh + 1) - h|,
                 * the first of equals; every SHP load adds it to the ID. render=320x200 the plain set drawn 1:1 into the
                 * 320x200 frame, 640x480 the 6 set, 1024x768 the K set; native the same rule on the drawable size, the
                 * 320x200 set's own layout only with render=320x200 (port: native windows nearer 320x200 take the 6 set) */
                char dmg[16];
                if (!g_hud) g_hud = hud_create(ma);
                hud_mission_palette(ma, mission);   /* the mission's palette for the HUD (0x10260324, see there) */
                glr_set_damage_shade(r, getenv("MW2_TEST_DSIGN") ? atoi(getenv("MW2_TEST_DSIGN")) : g_plnt52);  /* PLNT +52 (0x1003ded8 -> DAT_1024e428, 0x1002bf85); TEST ONLY override */
                g_hud_s = g_render_w == 320 ? 0.5f : 1.0f;
                g_hud_sfx = g_render_w == 320 ? "" : g_render_w == 640 ? "6" : g_render_w == 1024 ? "K" :
                            abs(1024 - g_win_cols) + abs(768 - g_win_lines) < abs(640 - g_win_cols) + abs(480 - g_win_lines) ? "K" : "6";
                if (getenv("MW2_DEBUG_HUDPAL")) fprintf(stderr, "hud set: '%s' for %dx%d\n", g_hud_sfx, g_win_cols, g_win_lines);
                if (g_hud) {
                    hud_sprite_free(&g_hs_compass); hud_sprite_free(&g_hs_reticle); hud_sprite_free(&g_hs_damage);
                    hud_sprite_load(g_hud, ma, HN("COMPASS"), 0, &g_hs_compass);
                    hud_sprite_load(g_hud, ma, HN("RETICLE"), 0, &g_hs_reticle);
                    {   /* the reticle's states (engine 0x10022710 picks one per frame) */
                        static const char *const RN[6] = {"RCLINOP", "RCLLOCK", "RCLNOLK", "RCLPLOC", "RCLTGT", "RCLGLOC"};
                        int q;
                        for (q = 0; q < 6; q++) { hud_sprite_free(&g_hs_rcl[q]); hud_sprite_load(g_hud, ma, HN(RN[q]), 0, &g_hs_rcl[q]); }
                        for (q = 0; q < 6; q++) { hud_sprite_free(&g_hs_rcl_dos[q]); hud_sprite_load(g_hud, ma, RN[q], 0, &g_hs_rcl_dos[q]); }
                        hud_sprite_free(&g_hs_reticle_dos); hud_sprite_load(g_hud, ma, "RETICLE", 0, &g_hs_reticle_dos);
                        {
                            static const char *const TB[3] = {"TGTGTE", "TGTGTF", "TGTGTN"};
                            for (q = 0; q < 3; q++) { hud_sprite_free(&g_hs_tgt_dos[q]); hud_sprite_load(g_hud, ma, TB[q], 0, &g_hs_tgt_dos[q]); }
                        }
                    }
                    /* damage outline: the SHP named by the HUD record's value 11 (TIMBDMG6 ...; see outline_shp_name) */
                    if (outline_shp_name(ma, g_sim.player_skel, dmg, sizeof dmg) != 0) dmg[0] = 0;
                    if (hud_sprite_load(g_hud, ma, dmg, 0, &g_hs_damage) != 0) dmg[0] = 0;
                    {   /* engine 0x1001b620: the outline's colour 6 remapped by damage level: 6 / 3 / 11 / 0 */
                        static const int to[4] = {6, 3, 11, 0};
                        int q;
                        prj_record hr;
                        for (q = 0; q < 4; q++) {
                            hud_sprite_free(&g_hs_dmg_lvl[q]);
                            hud_sprite_load_remap(g_hud, ma, dmg, 0, 6, to[q], &g_hs_dmg_lvl[q]);
                            hud_sprite_smooth(&g_hs_dmg_lvl[q], 1);   /* anti-aliased outline */
                        }
                        hud_sprite_smooth(&g_hs_damage, 1);
                        memset(g_dmg_regions, 0, sizeof g_dmg_regions);
                        if (prj_read_named(ma, "HUD", g_sim.player_skel, &hr) == PRJ_OK) {   /* 15 regions after 13 values */
                            for (q = 0; q < 60 && 13 * 4 + q * 4 + 4 <= (int)hr.size; q++) {
                                const uint8_t *pp = hr.data + 13 * 4 + q * 4;
                                g_dmg_regions[q / 4][q % 4] = (int32_t)(pp[0] | (pp[1] << 8) | (pp[2] << 16) | ((uint32_t)pp[3] << 24));
                            }
                            prj_record_free(&hr);
                        }
                    }
                    g_layout_n = hud_layout(ma, g_layout, 24);
                    hud_sprite_free(&g_hs_vtgt_nav);
                    hud_sprite_load(g_hud, ma, HN("VTGT_NP"), 0, &g_hs_vtgt_nav);
                    hud_sprite_free(&g_hs_paused);
                    hud_sprite_load(g_hud, ma, HN("PAUSED"), 0, &g_hs_paused);
                    { int sq; for (sq = 0; sq < 10; sq++) { hud_sprite_free(&g_hs_snow[sq]); hud_sprite_load(g_hud, ma, HN("SNOWCLR"), sq, &g_hs_snow[sq]); } }
                    {
                        const char *en[4] = {HN("CRTLFT"), HN("CRTRGHT"), HN("CRTUP"), HN("CRTDOWN")};
                        int q;
                        hud_sprite_free(&g_hs_navmk); hud_sprite_load(g_hud, ma, HN("TGTNP"), 0, &g_hs_navmk);
                        {
                            const char *vpn[3] = {HN("VP_RER"), HN("VP_DWN"), HN("VP_WPN")};
                            int q3;
                            for (q3 = 0; q3 < 3; q3++) { hud_sprite_free(&g_hs_vp[q3]); hud_sprite_load(g_hud, ma, vpn[q3], 0, &g_hs_vp[q3]); }
                        }
                        for (q = 0; q < 4; q++) {
                            /* one sprite whatever the kind (0x10023180 / DOS 0x29bb0 by id): red for a nav point too
                             * (DOSBox YELLSCN1 n: the compass and altitude arrows red; the port had a green nav copy) */
                            hud_sprite_free(&g_hs_edge[q]); hud_sprite_load(g_hud, ma, en[q], 0, &g_hs_edge[q]);
                        }
                    }
                    hud_font_load(g_hud, ma, HN("BASE6X7"));
                    {
                        const char *tg[3] = {HN("TGTGTE"), HN("TGTGTF"), HN("TGTGTN")};
                        int q;
                        for (q = 0; q < 3; q++) { hud_sprite_free(&g_hs_tgt[q]); hud_sprite_load(g_hud, ma, tg[q], 0, &g_hs_tgt[q]); }
                        hud_sprite_free(&g_hs_tgtoff); hud_sprite_load(g_hud, ma, HN("TGTOFFE"), 0, &g_hs_tgtoff);
                        {   /* radar (descriptor +0x70 rows x side F / E / N) */
                            static const char *const S3[3] = {"F", "E", "N"};
                            int sd;
                            for (sd = 0; sd < 3; sd++) {
                                char nm[16];
                                snprintf(nm, sizeof nm, "RGP%s", S3[sd]); hud_sprite_free(&g_hs_rgp[sd]); hud_sprite_load(g_hud, ma, HN(nm), 0, &g_hs_rgp[sd]);
                                snprintf(nm, sizeof nm, "RTGTGP%s", S3[sd]); hud_sprite_free(&g_hs_rtgp[sd]); hud_sprite_load(g_hud, ma, HN(nm), 0, &g_hs_rtgp[sd]);
                                snprintf(nm, sizeof nm, "RTGTGT%s", S3[sd]); hud_sprite_free(&g_hs_rtgt[sd]); hud_sprite_load(g_hud, ma, HN(nm), 0, &g_hs_rtgt[sd]);
                                snprintf(nm, sizeof nm, "RTGTOF%s", S3[sd]); hud_sprite_free(&g_hs_rtof[sd]); hud_sprite_load(g_hud, ma, HN(nm), 0, &g_hs_rtof[sd]);
                            }
                            hud_sprite_free(&g_hs_ruser); hud_sprite_load(g_hud, ma, HN("RUSERGP"), 0, &g_hs_ruser);
                            hud_sprite_free(&g_hs_rnp[0]); hud_sprite_load(g_hud, ma, HN("RNP"), 0, &g_hs_rnp[0]);
                            hud_sprite_free(&g_hs_rnp[1]); hud_sprite_load(g_hud, ma, HN("RRNP"), 0, &g_hs_rnp[1]);
                            hud_sprite_free(&g_hs_rtnp[0]); hud_sprite_load(g_hud, ma, HN("RTGTNP"), 0, &g_hs_rtnp[0]);
                            hud_sprite_free(&g_hs_rtnp[1]); hud_sprite_load(g_hud, ma, HN("RRTGTNP"), 0, &g_hs_rtnp[1]);
                        }
                        {   /* side 0 friendly (plain), 1 enemy (E), 2 neutral (N) */
                            static const char *const SUF[3] = {"", "E", "N"}, *const OFF[3] = {"TGTOFFF", "TGTOFFE", "TGTOFFN"};
                            int sd, cn;
                            for (sd = 0; sd < 3; sd++) {
                                char nm[16];
                                for (cn = 0; cn < 4; cn++) {
                                    snprintf(nm, sizeof nm, "TGTGP%d%s", cn + 1, SUF[sd]);
                                    hud_sprite_free(&g_hs_gp[sd][cn]); hud_sprite_load(g_hud, ma, HN(nm), 0, &g_hs_gp[sd][cn]);
                                }
                                hud_sprite_free(&g_hs_off[sd]); hud_sprite_load(g_hud, ma, HN(OFF[sd]), 0, &g_hs_off[sd]);
                            }
                        }
                        hud_sprite_free(&g_hs_blip); hud_sprite_load(g_hud, ma, HN("RTGTGTE"), 0, &g_hs_blip);
                        hud_sprite_free(&g_hs_alt); hud_sprite_load(g_hud, ma, HN("ALTTAPE"), 0, &g_hs_alt);
                        hud_sprite_free(&g_hs_alttop); hud_sprite_load(g_hud, ma, HN("ALTTOP"), 0, &g_hs_alttop);
                        {
                            const char *cm[4] = {HN("CMPMKR1"), HN("CMPMKR2"), HN("CMPMKR3"), HN("CMPMKR4")}, *am[2] = {HN("ALTMKR1"), HN("ALTMKR2")};
                            for (q = 0; q < 4; q++) { hud_sprite_free(&g_hs_cmpmk[q]); hud_sprite_load(g_hud, ma, cm[q], 0, &g_hs_cmpmk[q]); }
                            for (q = 0; q < 2; q++) { hud_sprite_free(&g_hs_altmk[q]); hud_sprite_load(g_hud, ma, am[q], 0, &g_hs_altmk[q]); }
                        }
                        hud_sprite_free(&g_hs_msgbar); hud_sprite_load(g_hud, ma, HB("MSGBAR"), 0, &g_hs_msgbar);
                        hud_sprite_free(&g_hs_mbar); hud_sprite_load(g_hud, ma, HB("MBAR"), 0, &g_hs_mbar);
                        memset(g_inst_lvl, 0, sizeof g_inst_lvl);
                        memset(g_snow_flag, 0, sizeof g_snow_flag); memset(g_snow_show, 0, sizeof g_snow_show);
                        g_snow_t0 = g_snow_step = -1;
                        g_last_display_hits = g_last_display_hits5 = 0; g_msg_t = 0;
                    }
                }
            }
            {   /* torso twist limit from the mech's MGEO record (field 5 -> controller +0xe0 = c[0x38], MGEO loader 0x100400b0) */
                prj_record mg;
                if (prj_read_named(ma, "MGEO", g_sim.player_skel, &mg) == PRJ_OK) {
                    if (mg.size >= 24)
                        g_twist_limit = (float)(int32_t)(mg.data[20] | (mg.data[21] << 8) | (mg.data[22] << 16) | ((uint32_t)mg.data[23] << 24)) / 65536.0f;
                    prj_record_free(&mg);
                }
            }
        }
        if (g_view) {   /* start above the centre of the structures, looking out */
            double cx = 0, cz = 0;
            int k;
            for (k = 0; k < m.part_count; k++) { cx += m.parts[k].pos[0]; cz += m.parts[k].pos[2]; }
            g_view->free_cam = 1;
            g_view->eye[0] = (float)(cx / m.part_count); g_view->eye[1] = 2500; g_view->eye[2] = (float)(cz / m.part_count);
            g_view->look_yaw = 0; g_view->look_pitch = -10;
        }
    } else {
        if (g_view) g_view->free_cam = 0;
        if (mech3d_load(ma, rec, 0, &m) != 0) { fprintf(stderr, "cannot load %s\n", rec); return -1; }
        g_have_anim = m.anim_id && anim_load_id(ma, m.anim_id, &g_anim) == 0;
        if (g_have_anim) {
            char lo[16];
            float speed;
            g_kps = 6.0f;
            if (mech3d_default_loadout(ma, rec, lo, sizeof lo) == 0) mech3d_walk_rate(ma, &m, &g_anim, lo, &speed, &g_kps);
        }
    }
    c3d_load_scene(ma, ta, ati, mission, &g_scene);
    glr_set_bank0(r, g_scene.bank0);   /* scrub billboards (colour type 3) */
    if (ati == 3) dos_scene(r, ma, mission);   /* the DOS edition: its palette, LUMA table, light and flat sky / ground */
    else glr_set_dos(r, NULL, NULL);
    {   /* the planet's LITE: payload {.., +8 x, +12 y, +16 z, +20 ambient level (short), +22 flag, +24 range, ..} */
        char pn[16];
        prj_record pr;
        g_lite_level = 0;
        snprintf(pn, sizeof pn, "%.4sPLT1", mission);
        if (prj_read_named(ma, "BWD", pn, &pr) == PRJ_OK) {
            bwd_chunk lc[64];
            int nlc = bwd_chunks(pr.data, pr.size, lc, 64), q;
            for (q = 0; q < nlc; q++)
                if (strcmp(lc[q].tag, "LITE") == 0 && lc[q].size >= 22) g_lite_level = (int16_t)(lc[q].data[20] | lc[q].data[21] << 8);
            prj_record_free(&pr);
        }
    }
    glr_set_mech(r, &m, (const uint8_t (*)[3])g_scene.palette, g_scene.slot, camo, clan);
    /* the ambient term: LITE level x 3/256 (PowerVR 0x100607d0: sgl ambient = render-config +0x2a x 0.01171875 x the
     * light colour; the fog colour's ambient term is the same value) */
    if (g_view && g_world && g_lite_level > 0) g_view->ambient = (float)g_lite_level * 3.0f / 256.0f;
    if (g_view) c3d_environment(r, &g_scene, mission, g_view, -1.0f);
    if (g_view && c3d_ti && ati != 3) {   /* the Titanium textures */
        c3d_ti_environment(r, &g_scene, mission, g_view, look_fog(g_edition));
        printf("titanium: %d textures from the Titanium archive, %d from the mission's texture archive\n", c3d_ti_hits, c3d_ti_miss);
        if (g_world) { ti_world_attach(&m); glr_set_mech(r, &m, (const uint8_t (*)[3])g_scene.palette, g_scene.slot, camo, clan); }
        else {   /* the single-mech viewer: the record's Titanium model (its own meshes and skeleton offsets, the same ANIM)
                  * - the Titanium engine has no other */
            mech3d tm;
            memset(&tm, 0, sizeof tm);
            if (mech3d_ti_load(c3d_ti, rec, 0, &tm) == 0 && tm.part_count > 0) {
                printf("titanium: viewer %s as its Titanium model (%d parts, the original %d)\n", rec, tm.part_count, m.part_count);
                mech3d_free(&m);
                m = tm;
                if (g_have_anim) mech3d_pose(&m, &g_anim, 0);
                glr_set_mech(r, &m, (const uint8_t (*)[3])g_scene.palette, g_scene.slot, camo, clan);
            } else mech3d_free(&tm);
        }
    }
    if (g_view && ati == 3) {   /* DOS (MW2.EXE 0x3c000): the view split at the horizon, sky palette 0xE0 above, ground 0xEF
                                 * below, flat and unlit; no fog colour (distance only darkens the shading) */
        static texture skyt, gndt;
        static uint32_t skyp, gndp;
        skyp = (uint32_t)g_scene.palette[0xe0][0] | ((uint32_t)g_scene.palette[0xe0][1] << 8) | ((uint32_t)g_scene.palette[0xe0][2] << 16) | 0xff000000u;
        gndp = (uint32_t)g_scene.palette[0xef][0] | ((uint32_t)g_scene.palette[0xef][1] << 8) | ((uint32_t)g_scene.palette[0xef][2] << 16) | 0xff000000u;
        skyt.w = skyt.h = gndt.w = gndt.h = 1; skyt.rgba = &skyp; gndt.rgba = &gndp;
        glr_set_environment(r, &gndt, &skyt, 1.0f, 1.0f);
        g_view->fog_density = 0;
        g_view->sky_color[0] = g_scene.palette[0xe0][0] / 255.0f; g_view->sky_color[1] = g_scene.palette[0xe0][1] / 255.0f; g_view->sky_color[2] = g_scene.palette[0xe0][2] / 255.0f;
        g_view->dos_lpos[0] = g_dos_lite[0]; g_view->dos_lpos[1] = g_dos_lite[1]; g_view->dos_lpos[2] = g_dos_lite[2];
        g_view->dos_amb = g_dos_lite[3]; g_view->dos_dir = (int)g_dos_lite[4]; g_view->dos_fogdist = g_dos_lite[5];
        g_view->dos_band = g_dos_band > 0 ? g_dos_band : 0;
        dos_time_of_day(r, 0);
    }
    if (getenv("MW2_DUMP_PAL")) { int q; for (q = 0; q < 256; q++) fprintf(stderr, "pal %02x: %d %d %d\n", q, g_scene.palette[q][0], g_scene.palette[q][1], g_scene.palette[q][2]); }
    if (getenv("MW2_DUMP_SLOTS")) {   /* tests: write the listed texture slots as PPM */
        const char *q = getenv("MW2_DUMP_SLOTS");
        while (*q) {
            int sl = (int)strtol(q, (char **)&q, 16);
            if (sl >= 0 && sl < 256 && g_scene.slot[sl]) {
                char pn[64]; FILE *tf; const texture *tt = g_scene.slot[sl]; int i3;
                snprintf(pn, sizeof pn, "/tmp/slot_%02x.ppm", sl);
                if ((tf = fopen(pn, "wb"))) { fprintf(tf, "P6 %d %d 255\n", tt->w, tt->h); for (i3 = 0; i3 < tt->w * tt->h; i3++) { const unsigned char *px = (const unsigned char *)tt->rgba + i3 * 4; fputc(px[0], tf); fputc(px[1], tf); fputc(px[2], tf); } fclose(tf); }
                fprintf(stderr, "slot %02x: %dx%d\n", sl, tt->w, tt->h);
            } else fprintf(stderr, "slot %02x: none\n", sl);
            while (*q == ',' || *q == ' ') q++;
        }
    }
    if (!g_world && g_test_vhead != 0) {   /* TEST ONLY */
        m.heading = g_test_vhead;
        mech3d_pose(&m, g_have_anim ? &g_anim : NULL, 0);
        glr_set_mech(r, &m, (const uint8_t (*)[3])g_scene.palette, g_scene.slot, camo, clan);
    }
    g_mech = m;                 /* keep: posed every frame while walking */
    if (g_world && g_sim_ok) msim_set_world(&g_sim, &g_mech);   /* world faces for AI obstacle avoidance */
    if (g_world && g_sim_ok && (g_sim.bld_count > 0 || g_sim.paths)) {   /* active buildings show blue in image enhancement (DOS),
                                                                          * path-carried parts leave the world layer: re-upload */
        int b;
        for (b = 0; b < g_sim.bld_count; b++) {
            if (g_sim.bld[b].intact >= 0 && g_sim.bld[b].intact < g_mech.part_count) g_mech.parts[g_sim.bld[b].intact].wire_hi = 1;
            if (g_sim.bld[b].destroyed >= 0 && g_sim.bld[b].destroyed < g_mech.part_count) g_mech.parts[g_sim.bld[b].destroyed].wire_hi = 1;
        }
        glr_set_mech(r, &g_mech, (const uint8_t (*)[3])g_scene.palette, g_scene.slot, camo, clan);
    }
    g_have_mech = 1;
    g_camo = camo;
    g_clan = clan;
    return 0;
}

/* the 3D engines' texturing per colour type (glr_view.ati_tex bits, with the renderer's own textures - not the Titanium
 * set): 3Dfx, S3, Matrox and ATi draw types 6 / 7 / 8 / 10 affine (oow / q = 1: 3Dfx 0x1002c880 with 0x10026e80's 4th
 * argument 0 or 0x1024e224 set; S3 0x1001dc0d / 0x1001dc70; Matrox 0x10051266 / 0x1005133e; ATi 0x1002def4) and all of
 * them with DISPLAY DETAIL low (3Dfx 0x10065390 -> 0x1002a8d0; S3 0x1002d9a0 -> 0x100476b0; Matrox 0x1001715d; ATi
 * 0x1002bc50); the alpha test / blend / key only with the alpha flag, types 8-11 (3Dfx 0x10067240, S3 0x100107a0, Matrox
 * 0x100624a0, ATi 0x10068830). The enhanced look is the 3Dfx engine's. PowerVR (SGL) is perspective-correct in hardware
 * and its textures carry their own type (opaque / translucent): 0 */
/* the 3Dfx / S3 texture cache (glr_view.tex_cache): 1 the 3Dfx engine's (also the enhanced look, the 3Dfx engine's - its
 * ATi textures have one reading each, so nothing changes there), 2 the S3 engine's; not with the Titanium textures (one
 * reading) */
static int look_cache(int ed)
{
    if (c3d_ti || getenv("MW2_TEST_NO_TEXCACHE")) return 0;   /* (TEST ONLY: each polygon its own flag's reading, as before) */
    return ed == PORTCFG_ED_3DFX || ed == PORTCFG_ED_ENHANCED ? 1 : ed == PORTCFG_ED_S3 ? 2 : 0;
}

/* the 3Dfx engine's alpha polygons blended (glr_view.alpha_blend; 0x10067240): RENDERER 3DFX with its own textures. S3
 * blends by the texel's alpha, not squared (0x100107a0: S3DTK state 0x13 = 0x80000, the ViRGE's texture-alpha blend,
 * INFERRED from the command bits; not modelled), Matrox keys (0x100624a0), ATi masks (0x10068830); the enhanced look keeps
 * the ATi textures' mask */
static int look_blend(int ed)
{
    return !c3d_ti && ed == PORTCFG_ED_3DFX;
}

static int look_tex(int ed)
{
    const int low = g_lodq == 2 ? 2 : 0;
    if (c3d_ti) return 0;
    if (ed == PORTCFG_ED_ATI || ed == PORTCFG_ED_3DFX || ed == PORTCFG_ED_S3 || ed == PORTCFG_ED_ENHANCED || ed == PORTCFG_ED_MGA) return 1 | low | 4;
    return 0;
}

/* the renderer's look on the view: lasers, texture filtering, the PowerVR veil, the effects' blending (at start, and the
 * in-game Renderer / Textures) */
static void edition_look(glr_view *v)
{
    v->tex_min = v->tex_mag = v->env_min = v->env_mag = 0; v->mga_mip = 0;
    /* the 3D editions' laser bolts by default (user choice); MW2_DOS_LASERS=1: glowing balls (a port option). The DOS
     * edition draws its own LASER1-3 objects (DOS MW2_SHT1: scale 3, 3, 12; Gouraud vertices a 33 / 37, 13 / 15, 45 / 47 -
     * all under 48, so drawn as those palette indices unscaled, 0x3bb80) like any object */
    g_dos_lasers = getenv("MW2_DOS_LASERS") != NULL;
    /* each edition's texture filtering (traced in its renderer DLL):
     *   3Dfx  guTexAllocateMemory(min = mag = bilinear, GR_MIPMAP_NEAREST_DITHER): linear, nearest mip
     *   ATi   "Filter Textures" on (the default): C3D_ETFILT_MINPNT_MAG2BY2 - point minification, bilinear magnification
     *         (off: point; one filter state for everything, 0x10068570); models, terrain and scrub without mips, the sky
     *         and ground fills with a mip chain (0x1002e690 / 0x1002e7a0 -> 0x1002bc60 mip flag 1 -> 0x10068830 ->
     *         0x10068450 -> 0x10068240: 2x2 box levels 0x10068080, TextureReg bMipMap 0x10067db0) - point sampled in
     *         the nearest level
     *   S3    "Filter Textures" on (default): TEX4TPP - bilinear without mips; the sky and ground with mips (M4TPP)
     *   PVR   bilinear by the driver (no filter call in the game); mips taken as nearest
     *   DOS   texel indices, point sampled (glr forces it) */
    if (g_edition == PORTCFG_ED_3DFX || g_edition == PORTCFG_ED_PVR) { v->tex_min = GL_LINEAR_MIPMAP_NEAREST; v->tex_mag = GL_LINEAR; }
    else if (g_edition == PORTCFG_ED_ATI) { v->tex_min = GL_NEAREST; v->tex_mag = GL_LINEAR; v->env_min = GL_NEAREST_MIPMAP_NEAREST; v->env_mag = GL_LINEAR; }
    else if (g_edition == PORTCFG_ED_S3) { v->tex_min = GL_LINEAR; v->tex_mag = GL_LINEAR; v->env_min = GL_LINEAR_MIPMAP_NEAREST; v->env_mag = GL_LINEAR; }
    else if (g_edition == PORTCFG_ED_MGA) {
        /* Matrox Mystique (MW2.DLL, Msi95): no filtering at all ("Filter Textures" is a dead option, 0x1005fca0 = ret);
         * the ground alone has mips - 2x2 box levels picked per triangle by view depth (2500 / 5000 / 15000 cm, from
         * MYSTIQUE.PAR: thresholds x MIPmaxZ), the sky one level */
        v->tex_min = GL_NEAREST; v->tex_mag = GL_NEAREST; v->env_min = GL_NEAREST_MIPMAP_NEAREST; v->env_mag = GL_NEAREST; v->mga_mip = 1;
    }
    /* (the Titanium edition's own filtering - dx5draw 0x100034a0 D3DFILTER_LINEAR, no mips 0x100056ae - belongs to its
     * renderer plugins: with TEXTURES TITANIUM the chosen renderer's filtering applies.) RENDITION is one of those plugins,
     * zredline.dll: VL_SetSrcFilter((flags >> 9) & 1) = 1 at FirstDevice (0x10002623; mercsw passes flags 0x202,
     * 0x100473a5, and never calls SetState 3, whose 0x10002972 would change it) - bilinear, INFERRED from the Verite
     * filter mode 1 - for every texture, the sky dome and ground too; no mips: the plugin makes each surface's levels
     * (0x100065d0, down to 4 texels) but picks one per polygon only with 0x10017420 set, which nothing sets (0 at
     * 0x100023d5; 0x10004390 -> 0x1000450c takes level 0). Its vertex fog is per vertex, as the plugin draws it: the
     * fog byte (the vertex's specular alpha, +0x1b) sent with each vertex (VL_SetF / the F of the 0x13001b command,
     * 0x100056d8..0x10005755) and interpolated across the polygon (screen-linear, INFERRED from the Verite's iterated F) */
    if (g_edition == PORTCFG_ED_RENDITION) {
        v->tex_min = v->tex_mag = v->env_min = v->env_mag = GL_LINEAR;
        printf("rendition: bilinear without mips, the Titanium fog per vertex, particles unkeyed\n");
    }
    v->ti_fog_vtx = g_edition == PORTCFG_ED_RENDITION;
    /* the PowerVR fog's veil over the sky: with the PowerVR edition and the enhanced mix (which takes its fog); not over
     * the Titanium sky dome (drawn unfogged, mercsw) */
    v->pvr_veil = (g_edition == PORTCFG_ED_PVR || g_edition == PORTCFG_ED_ENHANCED) && !c3d_ti;
    /* the Titanium textures: the effect frames (textures of type > 0, with their alpha) alpha-blended - the engine picks
     * Render type 8 by the texture's type (mercsw 0x1000131e; dx5draw 0x100044ff), whatever the plugin */
    v->fx_blend = c3d_ti != NULL;
    v->ati_tex = look_tex(g_edition);
    v->tex_cache = look_cache(g_edition);
    v->alpha_blend = look_blend(g_edition);
}

/* The Titanium textures' archive (portcfg_titanium: the game folder's titanium/, or titanium= in the configuration): opened
 * beside the mission's archives, its textures taken by name (common3d.h c3d_ti). Returns 0 if it is open. */
static int titanium_open(int on)
{
    char prj[PORTCFG_PATH], err[256], *bp;
    portcfg pc;
    if (!on) { if (c3d_ti) prj_close(c3d_ti); c3d_ti = NULL; return 0; }
    if (c3d_ti) return 0;
    bp = SDL_GetBasePath(); portcfg_load(NULL, &pc); portcfg_resolve(&pc, bp, NULL); SDL_free(bp);
    if (portcfg_titanium(&pc, prj, NULL) != 0) { fprintf(stderr, "titanium: no Titanium archive (the game folder's titanium/)\n"); return -1; }
    if (!(c3d_ti = prj_open(prj, err, sizeof err))) { fprintf(stderr, "titanium: %s\n", err); return -1; }
    {   /* the folder, for particle.PID beside MW2.PRJ */
        char *sl = strrchr(prj, '/');
        snprintf(g_ti_dir, sizeof g_ti_dir, "%.*s", sl ? (int)(sl - prj) : 1, sl ? prj : ".");
        g_tip_state = 0;
    }
    c3d_ti_hits = c3d_ti_miss = 0;
    return 0;
}
/* the effect frames decoded so far (their archive changed): decoded again on use */
static void fx_flush(void)
{
    int k;
    for (k = 0; k < 4096; k++) if (g_fxtex[k]) { tex_free(g_fxtex[k]); free(g_fxtex[k]); g_fxtex[k] = NULL; }
}

/* The in-game Renderer and Textures (the GRAPHICS menu; a port addition in place of the 3Dfx edition's "Resolution
 * 800x600", whose commit 0x10066520 -> 0x100667e0 does nothing): another 3D edition's look and / or the other texture
 * set mid-mission - the renderer's texture archive, sky file, fog and filtering, the Titanium textures, sky dome and
 * ground beside it. The mission keeps the archive it was started with for models and data (an edition's archive
 * differs in record ids, and the simulation lives on them); textures are found by name. The DOS look has its own
 * palette-indexed archive and stays a shell choice. The choice is saved as the next mission's renderer= / textures=. */
static int switch_look(glr *r, glr_view *v, prj_archive *ma, prj_archive **ta, int *ati, const char *mission, int ed, int ts)
{
    static char mo[PORTCFG_PATH], tx[PORTCFG_PATH], kind[PORTCFG_PATH], sky[PORTCFG_PATH], fog[PORTCFG_PATH];
    char err[256], *bp;
    portcfg pc, raw;
    prj_archive *nt;
    int k;
    if (ed == PORTCFG_ED_RENDITION) ts = PORTCFG_TX_TITANIUM;   /* (the Titanium plugin: its textures only) */
    if (ed == PORTCFG_ED_DOS || g_tex_kind == 3 || (ed == g_edition && ts == g_texset)) return -1;
    bp = SDL_GetBasePath(); portcfg_load(NULL, &pc); portcfg_resolve(&pc, bp, NULL); SDL_free(bp);
    if (portcfg_look(&pc, ed, ts, mo, tx, kind, sky, fog) != 0) return -1;
    if (titanium_open(ts == PORTCFG_TX_TITANIUM) != 0) return -1;   /* (the Titanium textures: their archive beside) */
    if (!(nt = prj_open(tx, err, sizeof err))) { fprintf(stderr, "video mode: %s\n", err); titanium_open(g_texset == PORTCFG_TX_TITANIUM); return -1; }
    fx_flush();
    k = strcmp(kind, "ati") == 0 ? 1 : strcmp(kind, "pvr") == 0 ? 2 : strcmp(kind, "mga") == 0 ? 4 : 0;
    if (sky[0]) setenv("MW2_SKYGND", sky, 1); else unsetenv("MW2_SKYGND");
    if (fog[0]) setenv("MW2_SKYGND_FOG", fog, 1); else unsetenv("MW2_SKYGND_FOG");
    setenv("MW2_EDITION", PORTCFG_ED_NAME[ed], 1);
    setenv("MW2_TEXTURES", PORTCFG_TX_NAME[ts], 1);
    if (*ta != g_fx_ta) prj_close(*ta);   /* the effect frames stay with the mission's first texture archive (CEL ids) */
    *ta = nt; *ati = k; g_tex_kind = k; g_edition = ed; g_texset = ts;
    mech3d_dos_hips = k == 3;
    edition_look(v);
    c3d_free_scene(&g_scene);
    {   /* the texture maps (BMID) of the edition's own archive when it has one: its sky and ground can differ */
        prj_archive *em = strcmp(mo, tx) == 0 ? nt : prj_open(mo, err, sizeof err);
        c3d_load_scene(em ? em : ma, nt, k, mission, &g_scene);
        if (em && em != nt) prj_close(em);
    }
    glr_set_bank0(r, g_scene.bank0);
    glr_flush_textures(r);
    glr_set_mech(r, &g_mech, (const uint8_t (*)[3])g_scene.palette, g_scene.slot, g_camo, g_clan);
    c3d_environment(r, &g_scene, mission, v, -1.0f);
    if (c3d_ti) c3d_ti_environment(r, &g_scene, mission, v, look_fog(ed)); else glr_set_skydome(r, NULL, g_scene.slot);
    if (c3d_ti && g_world && g_ti_world_for != g_mech.parts) ti_world_attach(&g_mech);
    glr_set_mech(r, &g_mech, (const uint8_t (*)[3])g_scene.palette, g_scene.slot, g_camo, g_clan);   /* (with / without the Titanium models) */
    if (g_lite_level > 0) v->ambient = (float)g_lite_level * 3.0f / 256.0f;
    if (c3d_ti) printf("titanium: %d textures from the Titanium archive, %d from %s\n", c3d_ti_hits, c3d_ti_miss, tx);
    portcfg_load(NULL, &raw);   /* the file's own values (unresolved), the look changed: the next mission's */
    raw.renderer = ed; raw.texset = ts;
    portcfg_save(NULL, &raw);
    printf("video mode: %s, textures %s (%s)\n", PORTCFG_ED_NAME[ed], PORTCFG_TX_NAME[ts], tx);
    return 0;
}

/* ---- TEST ONLY (tools/sweep_walk.py): MW2_WALK_TRACE - why the player's step was refused, and what it stands on ---- */
static float (*g_wt_box)[6];   /* per world part: world AABB min x,y,z, max x,y,z */
static void wt_boxes(void)
{
    int i, k;
    if (g_wt_box) return;
    g_wt_box = calloc((size_t)(g_mech.part_count > 0 ? g_mech.part_count : 1), sizeof *g_wt_box);
    for (i = 0; i < g_mech.part_count; i++) {
        const mech3d_part *p = &g_mech.parts[i];
        float *b = g_wt_box[i];
        b[0] = b[1] = b[2] = 1e30f; b[3] = b[4] = b[5] = -1e30f;
        if (p->model.object_count < 1) continue;
        for (k = 0; k < p->model.objects[0].vert_count; k++) {
            const wtb_vertex *v = &p->model.objects[0].verts[k];
            float q[3];
            if (p->has_rot) { const float *R = p->rot; q[0] = R[0] * v->x + R[1] * v->y + R[2] * v->z + p->pos[0]; q[1] = R[3] * v->x + R[4] * v->y + R[5] * v->z + p->pos[1]; q[2] = R[6] * v->x + R[7] * v->y + R[8] * v->z + p->pos[2]; }
            else { float c = cosf(p->yaw * 3.14159265f / 180.0f), sn = sinf(p->yaw * 3.14159265f / 180.0f); q[0] = c * v->x + sn * v->z + p->pos[0]; q[1] = (float)v->y + p->pos[1]; q[2] = -sn * v->x + c * v->z + p->pos[2]; }
            { int j; for (j = 0; j < 3; j++) { if (q[j] < b[j]) b[j] = q[j]; if (q[j] > b[3 + j]) b[3 + j] = q[j]; } }
        }
    }
}
static void wt_part(int i) { const mech3d_part *p = &g_mech.parts[i]; const float *b = g_wt_box[i];
    fprintf(stderr, " part %d '%s' rec %s coll %d box %.0f..%.0f,%.0f..%.0f,%.0f..%.0f", i, p->model_name, p->rec, p->coll, b[0], b[3], b[1], b[4], b[2], b[5]); }
static void wt_block(float x0, float z0, float nx, float nz)
{
    float y = g_sim.player_unit.y, H = g_sim.player_centre_h, R = g_sim.player_radius, n[3], n2[3], g, g2, dx = nx - x0, dz = nz - z0, dl = sqrtf(dx * dx + dz * dz);
    int i, why = 0;
    wt_boxes();
    g = msim_ground_n(&g_sim, nx, nz, y, n);
    fprintf(stderr, "walkblk %.2f at %.0f,%.0f y %.0f ground %.0f H %.0f R %.0f:", g_sim.now / 1000.0, x0, z0, y, g, H, R);
    if (!msim_can_step_hr(&g_sim, x0, z0, nx, nz, y, H, R)) {   /* what the collision list hit (msim clist) */
        float bn[3];
        msim_block_normal(bn);
        fprintf(stderr, " hit '%s' n %.2f,%.2f,%.2f", msim_block_what() ? msim_block_what() : "?", bn[0], bn[1], bn[2]); why = 1;
    }
    if (n[1] < 0.70710677f && g - y > H && n[0] * dx + n[2] * dz < 0) { fprintf(stderr, " steep_here n %.2f,%.2f,%.2f rise %.0f", n[0], n[1], n[2], g - y); why = 1; }
    if (dl > 1e-3f) { float ax = nx + dx / dl * R, az = nz + dz / dl * R; g2 = msim_ground_n(&g_sim, ax, az, y, n2);
        if (n2[1] < 0.70710677f && g2 - y > H && n2[0] * dx + n2[2] * dz < 0) { fprintf(stderr, " steep_ahead n %.2f,%.2f,%.2f rise %.0f", n2[0], n2[1], n2[2], g2 - y); why = 1; } }
    if (msim_wall_at(&g_sim, nx, nz, NULL)) { fprintf(stderr, " wall1"); why = 1;
        for (i = 0; i < g_mech.part_count; i++) { const float *b = g_wt_box[i]; if (g_mech.parts[i].coll == 1 && !g_mech.parts[i].hidden && nx >= b[0] && nx <= b[3] && nz >= b[2] && nz <= b[5]) wt_part(i); } }
    if (y - g < -9999.99f) { fprintf(stderr, " under_ground"); why = 1; }
    for (i = 0; i < g_mech.part_count; i++) {   /* solids (types 0, 2, 7): the one whose removal frees the step */
        mech3d_part *p = &g_mech.parts[i];
        const float *b = g_wt_box[i];
        if (p->hidden || (p->coll != 0 && p->coll != 2 && p->coll != 7)) continue;
        if (fminf(x0, nx) - R > b[3] || fmaxf(x0, nx) + R < b[0] || fminf(z0, nz) - R > b[5] || fmaxf(z0, nz) + R < b[2]) continue;
        p->hidden = 1;
        if (msim_can_step_hr(&g_sim, x0, z0, nx, nz, y, H, R)) { fprintf(stderr, " solid"); wt_part(i); why = 1; }
        p->hidden = 0;
    }
    if (!why) fprintf(stderr, " unknown");
    fputc('\n', stderr);
}
static void wt_on(float x, float z, float y)   /* the type 0 box tops / type 5 parts the player stands on */
{
    int i;
    wt_boxes();
    for (i = 0; i < g_mech.part_count; i++) {
        const float *b = g_wt_box[i];
        int c = g_mech.parts[i].coll;
        if (!g_mech.parts[i].hidden && (c == 1 || c == 2 || c == 7) && x > b[0] && x < b[3] && z > b[2] && z < b[5] && (c == 1 || (y + g_sim.player_centre_h > b[1] && y < b[4] - 60.0f))) { fprintf(stderr, " inside%d", c); wt_part(i); }
        if (g_mech.parts[i].hidden || (c != 0 && c != 5) || x < b[0] || x > b[3] || z < b[2] || z > b[5]) continue;
        if (c == 0 && fabsf(b[4] - y) < 60.0f) { fprintf(stderr, " on0"); wt_part(i); }
        if (c == 0 && y + g_sim.player_centre_h > b[1] && y + g_sim.player_centre_h < b[4] - 60.0f) { fprintf(stderr, " inside0"); wt_part(i); }
        if (c == 5 && b[3] - b[0] < 50000.0f && b[5] - b[2] < 50000.0f && y > b[1] + 50.0f) fprintf(stderr, " on5 %d '%s'", i, g_mech.parts[i].model_name);   /* small type 5 objects (ramps, bridges) */
    }
}
/* ---- end TEST ONLY ---- */

/* the player's vertical move this frame in the move test (engine 0x10019310 -> 0x1000b5e0: one 3D segment, as the AI's in
 * msim.c): the frame's (v + a t / 2) t (combat_jets_dy, set before pl_locomote), the part used by the moves so far, and
 * - once a move was refused in 3D - the impact's vertical velocity, which then stands after combat_jets */
static float g_pl_dy_frame, g_pl_dy_done, g_pl_vy3, g_pl_dt;
static int g_pl_imp3, g_pl_land3;
/* one move of the player's locomotion: the drive (throttle -> speed), the move by the mean velocity, mech contact and
 * the terrain / object impact (engine 0x10019310, 0x1000b5e0); 1 when it touched a mech or was refused */
static int pl_move(float dt, float top)
{
    float speed0 = g_pl_speed, h = g_pl.heading * 3.14159265f / 180.0f;
    int touched = 0;
    /* the throttle servo c[0x13] follows the unsigned setting (input +8 is 0..1024; 0x1001a180 target 1/64 + input x
     * c[0x2c]); REVERSE_DIRECTION only asks the gait code for the reverse sequence (input +0x2f -> +0x88 = 2,
     * 0x1000b3a0), and the drive turns x -0.5 once that sequence PLAYS (+0x84 == 2, 0x1001a90a):
     * reversing from a run keeps the forward drive while the key player walks run -> walk -> reverse (DOSBox TNJ1
     * Firemoth, FULL then BSP: 153 kph held 1.7 s, then 133 / 114 / 93 / 61 / 39 at 0.07 / 0.18 / 0.28 / 0.35 s and
     * backwards; the port had flipped the drive at the key - stopped and backing within 1 s) */
    msim_drive_step(&g_thr_srv, &g_spd_srv, &g_pl_speed, fabsf(g_throttle_eff), g_pl_ap.cur == 2 ? -0.5f : 1.0f, top,
                    !(g_sim_ok && (g_sim.player_unit.shutdown || g_sim.player_unit.immobile)), g_drive_slow, dt * 182.0f);
    if (g_pl_free) g_pl_speed = speed0;   /* airborne / jetting: the servos run, the velocity is held (0x10019310) */
    {   /* locomotion on the terrain (engine 0x10019310): steep or out-of-window steps refused */
        /* the move is the frame's mean velocity x dt - the engine's (v + a dt / 2) dt (0x10019310 local_40) */
        float nx, nz, along = 0.5f * (speed0 + g_pl_speed), across = g_pl_across;
        if (g_pl_across != 0.0f) {   /* a mech contact's or wall impact's velocity across the heading (below), on the ground: x (1 - ticks / 45),
                                      * zero under 0.0625 cm/tick (0x10019310, 0x102473b4 / 0x102473d0); airborne or jetting it is
                                      * held with the rest of the one velocity vector (no approach in the air, 0x10019310 0x78 == 0) -
                                      * the port had dropped it in the air, so an airborne impact's slide along the face was lost */
            float k = g_pl_free ? 0.0f : dt * 182.0f / 45.0f;
            g_pl_across *= k < 1.0f ? 1.0f - k : 0.0f;
            across = 0.5f * (across + g_pl_across);
            if (fabsf(g_pl_across) < 0.0625f * 182.0f) g_pl_across = 0;
        }
        nx = (float)g_pl.origin[0] + (sinf(h) * along + cosf(h) * across) * dt + g_pl_frac[0];
        nz = (float)g_pl.origin[2] + (cosf(h) * along - sinf(h) * across) * dt + g_pl_frac[1];
        float fx = g_pl_frac[0], fz = g_pl_frac[1];   /* the sub-cm rest of the last move, used once */
        g_pl_frac[0] = g_pl_frac[1] = 0;
        {   /* mech-mech contact in 3D, as the AI's (msim.c; engine 0x1000ba20 spheres, 0x1000c160 / 0x1000c1f0):
             * landing on top of a mech (normal y above 0.7071, feet within 10 m of the terrain) sets the player
             * down beside it, no damage; any other contact damages the player by the normal (head from above x
             * the tonnage ratio, legs from below, else arm / torso) at the engine's relative speed on every
             * contact tick - the player only: the other mech is damaged only by its own movement's contacts
             * (0x10019d6a -> 0x1000c160 takes the mover alone) - and clangs once per contact (latch 0x1007c338); walking or airborne the player
             * is set on the contact sphere and bounces off along the normal at half that speed (at least 1 cm/tick) */
            float ticks = dt * 182.0f, air = g_sim.player_unit.y > g_sim.player_unit.ground + 1.0f, vy = air ? g_sim.player_unit.vy : 0.0f;
            float ch = g_sim.player_centre_h, x0 = (float)g_pl.origin[0], z0 = (float)g_pl.origin[2];
            float p0[3] = {x0, g_sim.player_unit.y + ch, z0}, p1[3] = {nx, g_sim.player_unit.y + ch + vy * ticks, nz};
            float n[3], c[3], rr = 0, ov[3], sv[3] = {(sinf(h) * g_pl_speed + cosf(h) * g_pl_across) / 182.0f, vy, (cosf(h) * g_pl_speed - sinf(h) * g_pl_across) / 182.0f};
            int hit = g_sim_ok ? msim_unit_contact(&g_sim, -1, p0, p1, n, c, &rr, ov) : -2, pushout = 0;
            g_sim.player_vel[0] = sv[0]; g_sim.player_vel[1] = sv[1]; g_sim.player_vel[2] = sv[2];
            /* set down inside the sphere of the mech it landed on (contact < -2): the move ends inside it, whatever
             * its direction - the engine's push-out (0x1000ba20) */
            if (hit == -2 && g_sim_ok && g_pl_contact < -2 && msim_unit_inside(&g_sim, -1, -g_pl_contact - 3, p1, n, c, &rr, ov)) { hit = -g_pl_contact - 3; pushout = 1; }
            if (hit >= 0) {
                /* the second try (0x10019a61): refused by rounding, the velocity doubled (msim_contact_retest) */
                const float vx2 = msim_contact_retest(p0, p1, c, rr) ? 2.0f : 1.0f;
                touched = 1;
                /* the engine's speeds (0x1000ba20 copies the other's z velocity over its own): damage at
                 * |(dvx, dvy)|, bounce at half |(dvx, dvy, the other's vz)|, at least 1 cm/tick */
                float dvx = sv[0] - ov[0], dvy = sv[1] - ov[1], sp = sqrtf(dvx * dvx + dvy * dvy), bs = 0.5f * sqrtf(dvx * dvx + dvy * dvy + ov[2] * ov[2]);
                float py = c[1] + n[1] * rr - ch, hl = sqrtf(n[0] * n[0] + n[2] * n[2]);
                float gnd = msim_ground(&g_sim, c[0] + n[0] * rr, c[2] + n[2] * rr, py + ch);   /* 0x10010080 at the centre */
                if (n[1] > 0.70710677f && py - gnd < 1000.0f && py - gnd > -10000.0f) {   /* landed on it (0x10019c4e) */
                    /* the engine: projected onto the sphere (0x1000ba20), the feet taken as 1 cm under the terrain
                     * there (local_5c = -1): stood on the terrain at that point, inside the sphere, vy 0, the
                     * velocity n x max(1, dv / 2); the next frame's move pushes it out (above) */
                    float gx = c[0] + n[0] * rr, gz = c[2] + n[2] * rr;
                    (void)hl;
                    if (bs < 1.0f) bs = 1.0f;
                    bs *= vx2;
                    g_pl.origin[0] = (int32_t)lrintf(gx); g_pl.origin[2] = (int32_t)lrintf(gz);
                    g_sim.player_unit.ground = gnd;
                    g_sim.player_unit.y = g_sim.player_unit.ground; g_sim.player_unit.vy = 0;
                    g_pl_speed = (n[0] * sinf(h) + n[2] * cosf(h)) * bs * 182.0f;   /* along the heading */
                    g_pl_contact = -hit - 3;   /* landed on it: pushed out next frame */
                } else {
                    if (g_sim.collision_damage)   /* every contact tick, the player only */
                        combat_collision_unit(&g_sim.player_unit, sp, n, g_pl.heading + g_pl.twist, g_sim.units[hit].tons, &g_sim.rng);
                    if (g_pl_contact != hit) {
                        if (g_sfx && sp > 0.5f) {
                            static int kid = -2;
                            if (kid == -2) { int st = g_ma ? prj_find_type(g_ma, "SNDS") : -1; kid = st >= 0 ? prj_find_id(g_ma, st, "MECBLDC1") : -1; }
                            if (kid > 0) sfx_play(g_sfx, kid, SFX_VOL((int)((sp > 23.02f ? 23.02f : sp) * 0.04344f * 200.0f)), 0.0f);   /* 0x10019d10: volume min(speed, 23.02) x 0.04344 x 200 (0x102473f8 / 0x102473fc), the mixer clamps it at 127 */
                        }
                        g_pl_contact = hit;
                    }
                    {   /* bounced off / pushed out along the normal, walking or airborne alike (0x1000ba20: the
                         * end of the move projected onto the r1 + r2 sphere, the velocity n x max(1, dv / 2)) -
                         * pressing on, each frame's move slides round the other mech, so how long it takes depends
                         * on the frame rate (DOSBox TNJ1, the Firemoth at full into the instructor: 8 s at ~32 fps,
                         * 1.5 s at ~5 fps; the port at 30 fps 8 s, its moves in contact run in 1/30 s steps at any frame
                         * rate - pl_locomote); the port had pushed a walking player back 60 cm
                         * at -30 % speed (ASSUMED). Walking, the velocity's part across the heading is kept too
                         * (g_pl_across), as are the projection's fractions of a cm (g_pl_frac) */
                        float rp = air || pushout ? rr + 1.0f : rr;   /* walking: onto the sphere itself (0x1000ba20) */
                        float gx = c[0] + n[0] * rp, gz = c[2] + n[2] * rp;
                        if (bs < 1.0f) bs = 1.0f;
                        bs *= vx2;
                        g_pl.origin[0] = (int32_t)lrintf(gx); g_pl.origin[2] = (int32_t)lrintf(gz);
                        /* the engine's positions are not whole cm: kept, the slide's fraction of a cm a frame adds up
                         * (rounded away, a nearly head-on contact - TNJ6's Mad Dog - never got round) */
                        g_pl_frac[0] = gx - (float)g_pl.origin[0]; g_pl_frac[1] = gz - (float)g_pl.origin[2];
                        /* the projection is in 3D (0x1000ba20): walking too the centre goes to the sphere's surface,
                         * lifted by n.y x the penetration (or lowered: back on the ground) - with the velocity's n.y
                         * part a small hop when the other's centre is lower */
                        g_sim.player_unit.y = c[1] + n[1] * rp - ch;
                        g_sim.player_unit.ground = msim_ground(&g_sim, gx, gz, g_sim.player_unit.y + ch);
                        if (g_sim.player_unit.y < g_sim.player_unit.ground) g_sim.player_unit.y = g_sim.player_unit.ground;
                        g_sim.player_unit.vy = g_sim.player_unit.y <= g_sim.player_unit.ground + 1.0f ? 0.0f : n[1] * bs;   /* on the ground: vy 0 */
                        g_pl_speed = (n[0] * sinf(h) + n[2] * cosf(h)) * bs * 182.0f;
                        {   /* the velocity's part across the heading (the engine sets its one velocity vector to n x
                             * bs): carried into the next moves, easing at 1/45 a tick toward the walking velocity as
                             * the engine's does (0x10019310) - the slide round a nearly head-on contact */
                            float across = n[0] * cosf(h) - n[2] * sinf(h);
                            g_pl_across = !pushout ? across * bs * 182.0f : 0.0f;   /* airborne too: held (0x10019310) */
                        }
                    }
                }
                nx = (float)g_pl.origin[0]; nz = (float)g_pl.origin[2];
            } else g_pl_contact = -2;
        }
        /* tested airborne too, in 3D (0x10019310 runs 0x1000b5e0 on every move with the frame's vertical part) */
        float dy_sub = g_pl_imp3 ? g_pl_vy3 * dt * 182.0f : g_pl_dt > 0.0f ? g_pl_dy_frame * dt / g_pl_dt : 0.0f;   /* this move's share */
        if (msim_can_move(&g_sim, (float)g_pl.origin[0], (float)g_pl.origin[2], nx, nz, g_sim.player_unit.y + g_pl_dy_done, dy_sub, g_sim.player_centre_h, g_sim.player_radius)) {
            g_pl_dy_done += dy_sub;
            g_pl.origin[0] = (int32_t)lrintf(nx);
            g_pl.origin[2] = (int32_t)lrintf(nz);
            /* the engine's positions are floats: the fraction of a cm is kept for the next move (a wall slide moves a cm
             * or less a frame, rounded away it had crept or stuck) */
            if (!touched) { g_pl_frac[0] = nx - (float)g_pl.origin[0]; g_pl_frac[1] = nz - (float)g_pl.origin[2]; }   /* a mech contact keeps its own */
            g_sim.player_unit.blocked = 0;
        } else {   /* walked into the world (engine 0x10019310 -> 0x1000b5e0): the move cut back to the touching point and the
                    * velocity reflected and cut to a quarter (msim_world_impact) - the whole vector, its part across the
                    * heading kept in g_pl_across (easing off at 1/45 a tick, above); unless harmless (a climbable face, the
                    * feet in the ground window: 0x10019c4e), collision damage at the attempted speed on every contact frame
                    * (0x10019d84 -> 0x1000c3c0 / 0x1000c1f0), the object walked into takes its share and the impact sounds */
            float wn[3], sp3 = sqrtf(g_pl_speed * g_pl_speed + across * across) / 182.0f;   /* |the attempted velocity|, cm/tick */
            float x0 = (float)g_pl.origin[0] + (touched ? g_pl_frac[0] : fx), z0 = (float)g_pl.origin[2] + (touched ? g_pl_frac[1] : fz), bx = nx, bz = nz;
            int imp;
            if (getenv("MW2_WALK_TRACE")) { static int32_t wtl = -1000; if (g_sim.now - wtl >= 500) { wtl = g_sim.now; wt_block((float)g_pl.origin[0], (float)g_pl.origin[2], nx, nz); } }   /* TEST ONLY */
            {
                /* the attempted velocity is the frame's END velocity: 0x10019310 adds the acceleration before the move
                 * (param_1[0x40..0x42] = v + a t), whose |v| and reflection the impact takes - vy + a t = 2 dy / t - vy */
                float tf = g_pl_dt * 182.0f, dyi = dy_sub, vyi = dy_sub == 0.0f ? 0.0f : g_pl_imp3 ? g_pl_vy3 :
                                                           tf > 0.0f ? 2.0f * g_pl_dy_frame / tf - g_sim.player_unit.vy : g_sim.player_unit.vy;
                imp = msim_world_impact3(&g_sim, x0, z0, &bx, &bz, g_sim.player_unit.y + g_pl_dy_done, &dyi, g_sim.player_centre_h,
                                         g_sim.player_radius, g_pl.heading, &g_pl_speed, &g_pl_across, &vyi);
                if (dy_sub != 0.0f) { g_pl_imp3 = 1; g_pl_vy3 = vyi; g_pl_land3 = imp & 2; }   /* airborne: the 3D impact's (walking stays level) */
                g_pl_dy_done += dyi;
            }
            msim_block_normal(wn);   /* the hit's normal (the second try's when refused): the engine's DAT_1024b420.. */
            g_pl.origin[0] = (int32_t)lrintf(bx); g_pl.origin[2] = (int32_t)lrintf(bz);
            g_pl_frac[0] = bx - (float)g_pl.origin[0]; g_pl_frac[1] = bz - (float)g_pl.origin[2];
            if (imp & 2) g_sim.player_unit.blocked = 0;   /* harmless: the contact cleared */
            else {
                if (!g_sim.player_unit.blocked && g_sfx && sp3 > 3.0693676f) {
                    /* engine 0x10019cf8: the impact sound - 0xc8 MECBLDC1 against a destructible object, 0xe5 MECMTNHD
                     * against anything else; AIL volume min(speed, 23.02 cm/tick) x 0.04344 x 200 (0x102473f8 / 0x102473fc:
                     * 200 at 23 cm/tick, clamped at 127 by the mixer - full from 14.6 cm/tick), centred; once per contact
                     * (0x1007c338) */
                    sfx_play(g_sfx, msim_impact_destructible(&g_sim) ? 0xc8 : 0xe5, SFX_VOL((int)((sp3 > 23.02f ? 23.02f : sp3) * 0.04344f * 200.0f)), 0.0f);
                }
                combat_collision(&g_sim.player_unit, sp3, wn[1], atan2f(-wn[0], -wn[2]) * 57.29578f - (g_pl.heading + g_pl.twist), &g_sim.rng);
                msim_impact_damage(&g_sim, -1, sp3);
            }
            touched = 1;
        }
    }
    return touched;
}
/* the player's locomotion for a frame of dt seconds. The engine's contact responses run per frame and their result
 * depends on the frame rate (0x1000ba20's projection and n x max(1, dv / 2), 0x1000b5e0's r x untravelled x 0.25 and
 * r x |v| x -0.25, each restored at 1/45 a tick - DOSBox TNJ1 slides round the instructor in ~8 s at ~32 fps, 4.6 s at
 * ~60); the port's frame-rate rule takes the 30 fps reference, so in contact the moves run in 1/30 s steps, the
 * remainder carried to the next frame (a frame shorter than the rest moves nothing), until CONTACT_HOLD steps in a row
 * have touched nothing (sliding round a mech, a few steps between touches are free: TNJ1 at most 3) - the slide is the
 * 30 fps one at 60 / 120 / 144 Hz. Out of contact the frame is one move (the mean-velocity move is exact at any rate). */
#define CONTACT_HOLD 10
static void pl_locomote(float dt, float top)
{
    const float step = 1.0f / 30.0f;
    if (g_pl_contact_hold <= 0) {   /* free: one move */
        g_pl_contact_acc = 0;
        if (pl_move(dt, top)) g_pl_contact_hold = CONTACT_HOLD;
        return;
    }
    g_pl_contact_acc += dt;
    while (g_pl_contact_acc >= step - 1e-5f) {
        g_pl_contact_acc -= step;
        if (pl_move(step, top)) g_pl_contact_hold = CONTACT_HOLD;
        else if (--g_pl_contact_hold <= 0) {   /* out of contact: the rest of the frame as one move */
            if (g_pl_contact_acc > 1e-5f) pl_move(g_pl_contact_acc, top);
            g_pl_contact_acc = 0;
            return;
        }
    }
}
/* the frame's time for msim_step in whole milliseconds, the fractions carried over (truncating each frame's dt made the
 * simulation run slower than the clock at high frame rates: 6.94 ms -> 6 at 144 fps, 13 % slow) */
static int32_t sim_ms(float dt)
{
    static double acc;
    int32_t ms;
    if (dt <= 0) return 0;
    acc += (double)dt * 1000.0;
    ms = (int32_t)acc;
    acc -= ms;
    return ms;
}
/* display damage, once per frame: new display criticals raise instrument levels (0x1001e670: per instrument rand(10) < 2,
 * < 5 for critical 5700), then the two static windows roll their visibility as the engine's draw does every frame
 * (0x10011720 / 0x10021300; frames at 30 a second ASSUMED, the 3Dfx loop is uncapped): level 1 - static showing:
 * rand(10) < 7 clears it (this frame still static); clear: rand(10) < 3 sets it (this frame still normal); level 2
 * normal; 3+ static */
static void inst_update(int aux_on, int tgt_on)
{
    const combat_unit *pu = &g_sim.player_unit;
    int q, k;
    while (g_last_display_hits < pu->display_hits || g_last_display_hits5 < pu->display_hits5) {
        int lim = g_last_display_hits < pu->display_hits ? 2 : 5;
        for (q = 0; q < 26; q++) if (rand() % 10 < lim) g_inst_lvl[q]++;
        if (lim == 2) g_last_display_hits++; else g_last_display_hits5++;
    }
    if (g_snow_step < 0 || g_sim.now - g_snow_step > 1000) g_snow_step = g_sim.now - 34;
    while (g_sim.now - g_snow_step >= 33) {   /* one engine frame */
        g_snow_step += 33;
        for (k = 0; k < 2; k++) {
            int lvl = g_inst_lvl[k ? INST_TGT : INST_AUX], on = k ? tgt_on : aux_on;
            if (!on) continue;                                          /* the draw routine returns before the roll */
            if (lvl == 1) {
                g_snow_show[k] = g_snow_flag[k];
                if (g_snow_flag[k]) { if (rand() % 10 < 7) g_snow_flag[k] = 0; }
                else if (rand() % 10 < 3) g_snow_flag[k] = 1;
            } else g_snow_show[k] = lvl >= 3;
            {   /* TEST counters (tests/sequences) */
                static int prev[2];
                g_snow_n[k]++; g_snow_on_n[k] += g_snow_show[k]; g_snow_flips[k] += g_snow_show[k] != prev[k]; prev[k] = g_snow_show[k];
            }
        }
    }
    if (!aux_on) g_snow_show[0] = 0;
    if (!tgt_on) g_snow_show[1] = 0;
}
/* the static in the window (x, y, w, h) (640x480 units; while it opens / closes the moving box): SNOWCLR frame
 * (ticks since its first draw / 18) % 10 at the window's top-left, clipped to it (0x10021710 / 0x10011a40 ->
 * 0x1001e800 -> 0x100022f0 with the window record +0x30, which 0x10021730 / 0x10011aa0 set to the moving rectangle;
 * the draw 0x10062241 clips to the window). Screens wider than 701 pixels (800x600, 1024x768) tile the frame across
 * the window (0x10002250: steps of the frame's width / height from its left / top edge) */
static void draw_snow(float x, float y, float w, float h)
{
    int f;
    float tx, ty, sw, sh;
    if (g_snow_t0 < 0) g_snow_t0 = g_sim.now;
    f = (int)(((int64_t)(g_sim.now - g_snow_t0) * 182 / 1000) / 18) % 10;
    if (f < 0) f = 0;
    if (!g_hs_snow[f].tex || w <= 0 || h <= 0) return;
    hud_clip(g_hud, x, y, w, h);
    if ((g_render_w ? g_render_w : g_win_cols) - 1 > 700) {   /* DAT_10244c64 = the screen's width - 1 (0x10004870) */
        sw = hud_px_x(g_hud, (float)g_hs_snow[f].w); sh = hud_px_y(g_hud, (float)g_hs_snow[f].h);   /* the frame's size */
        if (sw < 1.0f || sh < 1.0f) sw = sh = 1e9f;
        for (ty = y; ty < y + h; ty += sh)
            for (tx = x; tx < x + w; tx += sw) hud_draw(g_hud, &g_hs_snow[f], tx, ty, NULL);
    } else hud_draw(g_hud, &g_hs_snow[f], x, y, NULL);
    hud_clip_off(g_hud);
}

/* TARGET_AT_RETICLE (engine 0x1001d870): the unit the player's aim ray last struck (msim reticle_actor / reticle_bld,
 * 0x10044810 -> DAT_1025a664 - kept while the ray strikes nothing, cleared when that unit goes) becomes the target when
 * it may be targeted (a mech 0x1001d050: not destroyed, within 1750 m - 0x482ae600; a thing thing_targetable with the
 * flag as it stood); otherwise the target stays as it was. Then DAT_1024c8b0 = 1: a plain structure is taken only by
 * a second q, or a first one after another q / Ctrl+t - DOSBox SILVSCN1, the reticle on the Airbase Hanger (878 m):
 * q nothing, q "Airbase Hanger"; t "Wolf Marauder", q still the Marauder, q the hangar; Ctrl+t nothing, q the hangar */
static void target_at_reticle(void)
{
    int a = g_sim.reticle_actor, b = g_sim.reticle_bld;
    if (a >= 0 && a < g_actor_count && g_sim.armed[a] && !g_sim.units[a].destroyed) {
        if (unit_targetable(a, -1, NULL)) { g_tgt_sel = a; g_tgt_thing = -1; }   /* 0x1001d050(0) with DAT_1024c8b0 as it stood */
    } else if (thing_targetable(b, NULL)) { g_tgt_sel = -1; g_tgt_thing = b; }
    g_tgt_any = 1;
}
/* ---- TEST ONLY (tests/sequences): held keys, a machine-readable state dump, timed screenshots, player hits ----
 * MW2_HOLD="t0-t1:key,..."  keys held between autopilot times t0 and t1 (names as MW2_KEYS, + shift+/ctrl+/alt+)
 * MW2_STATE_DUMP="1,2.5,..." or "/0.5"  print "STATE ..." lines at those autopilot times (or every 0.5 s), and at exit;
 *                                        hud messages print as "MSG ..." lines
 * MW2_SHOT="t:file.ppm,..."  write the frame at those times
 * MW2_TEST_AHIT="t:actor:loc:weapon:count,..."  the same for an actor (stops once it is destroyed)
 * MW2_TEST_PARMOR="t:factor"  the player's armour x factor at t (0.25: the MEK's points, as before the loader's x 4)
 * MW2_TEST_WATCH="t:actor"  the actor's state dumped as a_* (a_armor: armour + structure per location 1-8)
 * MW2_TEST_SHOTSEG="t:k:dy:dx,..."  a shot step through actor k (50 m south to north, dy over its feet, dx east): testshot lines
 * MW2_TEST_FIRELINE="t:k,..."  actor k's fire-decision line to the player (msim_test_fire_line): "fireline <k> <1 clear / 0 a unit
 *                                        first / -1 the world>"
 * MW2_TEST_PHIT="t:loc:weapon:count,..."  the player takes `count` hits of weapon `weapon` on location loc (1-8) at t
 * MW2_TEST_WHEAT="weapon:heat"  weapon `weapon` (table index) adds `heat` per projectile (combat_test_wheat; a patched table)
 * MW2_TEST_RFLASH="t:1,..."  the player's red flash request (DAT_1024cce0) raised at t
 * MW2_TEST_HEAT="t:value,..."  set the player's heat at t ("t:a/b": a at t, b on the next frame)
 * MW2_TEST_RNG="t:value"  set the simulation's random state at t (deterministic criticals)
 * MW2_TEST_IMMOBILE="t:actor"  the actor stands still from t (its unit immobile)
 * MW2_TEST_DISARM="t:actor[:level]"  the actor's weapons all spent at t (out of weapons: jump / charge and self-destruct /
 *                                        flee); level: its pilot level too, and no jump jets
 * MW2_TEST_MAN="t:actor:man"  force the actor's next attack manoeuvre at t (the running one ends)
 * MW2_TEST_AIM="k,dist,dy"   stand dist cm south of actor k (north if negative) with the reticle held on its centre + dy
 * MW2_AIM_TRACE=1            print AIMHIT lines: each player impact against the reticle line it was fired along
 * MW2_AIM_BODY=1             the old aim ray from the body point (comparison only)
 * MW2_TEST_DISPLAY="t:instrument:level,..."  set a cockpit instrument's display-damage level at t (0 radar, 2 viewport
 *                                        window, 13 target viewer); the dump adds lvl= (0/2/13), snow= / snow_n= counts
 * MW2_TEST_KILL_ALL="t:0"  destroy every armed enemy (not friendly, not neutral) at t
 * MW2_TEST_APOWER="t:actor:up"  power an actor down (up 0) / wake it (1) at t (msim_test_power)
 * MW2_TEST_THING="t:bld,..." target structure bld at t;  MW2_TEST_BLD_KILL="t:bld,..." destroy structure bld at t
 * MW2_TEST_NODE_OK="t:node,t:node,..."  table-0 node succeeds at t; the dump's objs= lists the F12 rows (node:priority:state)
 * MW2_TEST_DROP="t:actor:dz:height:vy:hspeed[:dx]"  put an actor over / beside the player in the air (see test_drop); the
 *                                        dump then adds a_* fields for it, and parmor= (the player's armour + structure) */
static SDL_Keycode test_keycode(const char *kn, int *mods)
{
    static const struct { const char *n; SDL_Keycode k; } T[] = {
        {"esc", SDLK_ESCAPE}, {"up", SDLK_UP}, {"down", SDLK_DOWN}, {"left", SDLK_LEFT}, {"right", SDLK_RIGHT},
        {"space", SDLK_SPACE}, {"enter", SDLK_RETURN}, {"tab", SDLK_TAB}, {"comma", SDLK_COMMA}, {"period", SDLK_PERIOD},
        {"minus", SDLK_MINUS}, {"equal", SDLK_EQUALS}, {"backslash", SDLK_BACKSLASH}, {"semicolon", SDLK_SEMICOLON},
        {"quote", SDLK_QUOTE}, {"backquote", SDLK_BACKQUOTE}, {"backspace", SDLK_BACKSPACE}, {"kpenter", SDLK_KP_ENTER},
        {"home", SDLK_HOME}, {"end", SDLK_END}, {"pageup", SDLK_PAGEUP}, {"pagedown", SDLK_PAGEDOWN}, {"delete", SDLK_DELETE},
        {"insert", SDLK_INSERT}, {"numlock", SDLK_NUMLOCKCLEAR}, {"kpdivide", SDLK_KP_DIVIDE}, {"kpmultiply", SDLK_KP_MULTIPLY},
        {"kp5", SDLK_KP_5}, {"kp9", SDLK_KP_9}, {"kp7", SDLK_KP_7}, {"slash", SDLK_SLASH}, {"pause", SDLK_PAUSE}};
    int i;
    *mods = 0;
    for (;;) {
        if (strncmp(kn, "ctrl+", 5) == 0) { *mods |= KMOD_LCTRL; kn += 5; }
        else if (strncmp(kn, "shift+", 6) == 0) { *mods |= KMOD_LSHIFT; kn += 6; }
        else if (strncmp(kn, "alt+", 4) == 0) { *mods |= KMOD_LALT; kn += 4; }
        else break;
    }
    for (i = 0; i < (int)(sizeof T / sizeof T[0]); i++) if (strcmp(kn, T[i].n) == 0) return T[i].k;
    if ((kn[0] == 'f' || kn[0] == 'F') && kn[1] >= '1' && kn[1] <= '9') { int n = atoi(kn + 1); if (n >= 1 && n <= 12) return SDLK_F1 + n - 1; }
    return (SDL_Keycode)(unsigned char)kn[0];
}
/* SDL's keyboard state plus the MW2_HOLD keys held now */
static const Uint8 *test_ks(void)
{
    static Uint8 ks[SDL_NUM_SCANCODES];
    const char *q = getenv("MW2_HOLD");
    int n = 0;
    const Uint8 *real = SDL_GetKeyboardState(&n);
    if (!q || !g_auto_on) return real;
    memcpy(ks, real, (size_t)(n < SDL_NUM_SCANCODES ? n : SDL_NUM_SCANCODES));
    while (*q) {
        float t0 = 0, t1 = 0;
        char kn[32] = "";
        int mods = 0;
        SDL_Keycode k;
        if (sscanf(q, "%f-%f:%31[^,]", &t0, &t1, kn) == 3 && g_auto_elapsed >= t0 && g_auto_elapsed < t1) {
            k = test_keycode(kn, &mods);
            ks[SDL_GetScancodeFromKey(k)] = 1;
            if (mods & KMOD_LCTRL) ks[SDL_SCANCODE_LCTRL] = 1;
            if (mods & KMOD_LSHIFT) ks[SDL_SCANCODE_LSHIFT] = 1;
            if (mods & KMOD_LALT) ks[SDL_SCANCODE_LALT] = 1;
        }
        q = strchr(q, ',');
        if (!q) break;
        q++;
    }
    return ks;
}
/* "t:a:b:c,..." entries whose time has come, once each (up to 32 per variable): calls fn with the numbers after t */
static void test_timed(const char *var, int *done, void (*fn)(const char *rest))
{
    const char *q = getenv(var);
    int idx = 0;
    while (q && *q && idx < 32) {
        float t = (float)atof(q);
        const char *c = strchr(q, ':');
        if (!c) break;
        if (!(*done & (1 << idx)) && g_auto_elapsed >= t) { *done |= 1 << idx; fn(c + 1); }
        idx++;
        q = strchr(c, ',');
        if (!q) break;
        q++;
    }
}
static void test_phit(const char *r)
{
    int loc = 0, w = 0, n = 0, i;
    if (sscanf(r, "%d:%d:%d", &loc, &w, &n) == 3) for (i = 0; i < n; i++) combat_hit(&g_sim.player_unit, w, loc, 180.0f, &g_sim.rng);
}
/* MW2_TEST_RFLASH="t:1,..."  raise the player's red flash request (DAT_1024cce0) at t, as a breaching hit does */
static void test_rflash(const char *r) { (void)r; g_sim.player_unit.red_flash = 1; }
/* MW2_TEST_AHIT="t:actor:loc:weapon:count"  actor takes `count` hits of `weapon` on location loc (1-8) at t */
static void test_ahit(const char *r)
{
    int a = -1, loc = 0, w = 0, n = 0, i;
    if (sscanf(r, "%d:%d:%d:%d", &a, &loc, &w, &n) == 4 && a >= 0 && a < g_actor_count && g_sim.armed[a])
        for (i = 0; i < n && !g_sim.units[a].destroyed; i++) combat_hit(&g_sim.units[a], w, loc, 180.0f, &g_sim.rng);
}
/* MW2_TEST_DISARM="t:actor[:level]"  every weapon of the actor spent (state -1) at t: out of weapons (0x10020880); with a
 * level, its GPS pilot level set too (1: the charge is allowed, +0x19e bit 0x20) and its jump jets taken away (no jump
 * manoeuvre 4: the choice is then charge or flee, rand(2)) */
static void test_disarm(const char *r)
{
    int a = -1, lv = 0, k;
    if (sscanf(r, "%d:%d", &a, &lv) < 1 || a < 0 || a >= g_actor_count || !g_sim.armed[a]) return;
    for (k = 0; k < g_sim.units[a].weapon_count; k++) g_sim.units[a].weapons[k].state = -1;
    if (lv) { g_actors[a].ai_level = lv; g_sim.units[a].jets = 0; g_sim.units[a].jet_fuel = 0; }   /* no jets: no jump option */
}
/* MW2_TEST_MAN="t:actor:man"  force the actor's next attack manoeuvre (+0x174, as 0x10020ac0 / 0x1001ea40 do): the running one
 * ends and `man` starts (attack state only) */
static void test_man(const char *r) { int a = -1, mn = -1; if (sscanf(r, "%d:%d", &a, &mn) == 2 && a >= 0 && a < g_actor_count && g_sim.minds) g_sim.minds[a].forced_man = mn; }
static void test_immobile(const char *r) { int a = atoi(r); if (a >= 0 && a < g_actor_count && g_sim.armed[a]) { g_sim.units[a].immobile = 1; if (g_sim.minds) g_sim.minds[a].speed = 0; } }
static void test_display(const char *r) { int i = -1, l = 0; if (sscanf(r, "%d:%d", &i, &l) == 2 && i >= 0 && i < 26) g_inst_lvl[i] = (short)l; }
static void test_rng(const char *r) { g_sim.rng = (unsigned)strtoul(r, NULL, 0); }
static void test_kill_all(const char *r) { int i; (void)r; for (i = 0; i < g_actor_count; i++) if (g_sim.armed[i] && !g_actors[i].friendly && g_actors[i].alliance != 2) g_sim.units[i].destroyed = 1; }
/* MW2_TEST_APOWER="t:actor:up"  power the actor down (up 0: a node's power-down, 0x10014520) or wake it (1) at t */
static void test_apower(const char *r) { int a = -1, up = 0; if (sscanf(r, "%d:%d", &a, &up) == 2 && a >= 0 && a < g_actor_count) msim_test_power(&g_sim, a, up); }
static float test_heat_next = -1;   /* MW2_TEST_HEAT "a/b": b, set on the following frame */
static void test_heat(const char *r)
{
    const char *sl = strchr(r, '/'), *cm = strchr(r, ',');
    g_sim.player_unit.heat = g_sim.player_unit.heat_mark = (float)atof(r);
    if (sl && (!cm || sl < cm)) test_heat_next = (float)atof(sl + 1);
}
static void test_thing(const char *r) { g_tgt_sel = -1; g_tgt_thing = atoi(r); g_tgt_any = 1; }   /* as q would */
static void test_bld_kill(const char *r) { int b = atoi(r); if (b >= 0 && b < g_sim.bld_count) msim_damage_building(&g_sim, b, 1e6f); }
static int test_drop_actor = -1;   /* MW2_TEST_DROP's actor (its state is dumped as a_*) */
/* MW2_TEST_PARMOR="t:factor"  the player's armour (front and rear) x factor at t (0.25: back to the MEK's points) */
static void test_parmor(const char *r) { float f = (float)atof(r); if (f > 0) combat_set_armor_mult(&g_sim.player_unit, f); }
/* MW2_TEST_WATCH="t:actor"  dump that actor's state as a_* (as MW2_TEST_DROP's) without moving it */
static void test_watch(const char *r) { int a = atoi(r); if (a >= 0 && a < g_actor_count) test_drop_actor = a; }
/* MW2_TEST_SHOTSEG="t:k:dy:dx"  one shot step through actor k at t: from 50 m south of it to 50 m north, dy above its feet,
 * dx east of it (msim_test_shot, the shots' own unit test); prints "testshot <hit> unit <u> loc <l> z <hit z - actor z> ...
 * repr <the level of detail actor k is in the collision list at>" */
static void test_shotseg(const char *r)
{
    int k = -1, u, g;
    float dy = 0, dx = 0, t;
    if (sscanf(r, "%d:%f:%f", &k, &dy, &dx) < 3 || k < 0 || k >= g_actor_count) return;
    {
        float o[3] = {(float)g_sim.actors[k].mech.origin[0] + dx, g_sim.units[k].y + dy, (float)g_sim.actors[k].mech.origin[2] - 5000.0f};
        float dv[3] = {0, 0, 10000.0f};
        int h = msim_test_shot(&g_sim, o, dv, &u, &g, &t);
        printf("testshot %d unit %d loc %d z %.0f (dy %.0f dx %.0f) repr %d\n", h, u, g, h ? t * 10000.0f - 5000.0f : 0.0f, dy, dx, msim_unit_repr(&g_sim, k));
    }
}
/* MW2_TEST_FIRELINE="t:k"  actor k's line of fire to the player as its fire decision tests it (0x10015090) */
static void test_fireline(const char *r) { int k = atoi(r); printf("fireline %d %d\n", k, msim_test_fire_line(&g_sim, k)); }
/* MW2_TEST_DROP="t:actor:dz:height:vy:hspeed"  actor put dz cm ahead (+z) of the player, its feet `height` cm above the
 * player's, falling at vy cm/tick (negative down), carrying hspeed cm/s toward the player */
/* MW2_TEST_PROBE="t:dz:dy:len"  one AI avoidance probe at t from dz ahead (+z) of the player, dy above its centre, len cm
 * back along -z (msim_test_probe); prints "testprobe <kind> at <distance>" (the units' piece hits with MW2_PROBE_TRACE) */
static void test_probe(const char *r)
{
    float dz = 0, dy = 0, len = 0, ox = 0, oz = 0;
    if (sscanf(r, "%f:%f:%f", &dz, &dy, &len) < 3) return;
    {
        float p0[3] = {(float)g_pl.origin[0], g_sim.player_unit.y + g_sim.player_centre_h + dy, (float)g_pl.origin[2] + dz};
        float p1[3] = {p0[0], p0[1], p0[2] - len};
        int k = msim_test_probe(&g_sim, p0, p1, &ox, &oz);
        printf("testprobe %d at %.0f,%.0f (dz %.0f dy %.0f)\n", k, ox, oz, dz, dy);
    }
}
static void test_drop(const char *r)
{
    int a = -1;
    float dz = 0, hgt = 0, vy = 0, hs = 0, dx = 0;
    if (sscanf(r, "%d:%f:%f:%f:%f:%f", &a, &dz, &hgt, &vy, &hs, &dx) < 3 || a < 0 || a >= g_actor_count) return;
    test_drop_actor = a;
    g_actors[a].mech.origin[0] = g_pl.origin[0] + (int32_t)dx;
    g_actors[a].mech.origin[2] = g_pl.origin[2] + (int32_t)dz;
    g_actors[a].mech.heading = dz > 0 ? 180.0f : 0.0f;
    g_sim.units[a].y = g_sim.player_unit.y + hgt;
    g_actors[a].mech.origin[1] = (int32_t)msim_draw_y(&g_sim, a);
    g_sim.units[a].vy = vy;
    if (g_sim.air_speed) g_sim.air_speed[a] = hs;
    if (g_sim.minds) g_sim.minds[a].speed = hs;
}
/* MW2_TEST_NODE_OK="t:node,..."  (one t:node per entry) table-0 node `node` succeeds at t (state 5, as its condition passing would - the nodes
 * depending on it run in the next logic step: objectives_dustoff_shown) */
static void test_node_ok(const char *r)
{
    int n = atoi(r);
    if (n < 0 || n >= g_sim.logic.tables[0].node_count) return;
    g_sim.logic.state[0][n] = MTBL_OK;
    g_sim.logic.ended[0][n] = g_sim.now;
}
static int test_fired;   /* volleys the player has fired (a weapon leaving READY for FIRING / REPEAT / RECYCLE) */
static void test_dump(const char *tag)
{
    const combat_unit *pu = &g_sim.player_unit;
    int i, gone = 0, dead = 0, armed = 0, intact = 0;
    if (!getenv("MW2_STATE_DUMP") || !g_sim_ok) return;
    for (i = 0; i < 8; i++) gone |= (pu->loc_gone[i] != 0) << i;
    for (i = 0; i < g_actor_count; i++) if (g_sim.armed[i]) { armed++; dead += g_sim.units[i].destroyed != 0; intact += g_sim.units[i].intact_out != 0; }
    printf("STATE %s t=%.2f now=%d pow=%d shut=%d pend=%d ovr=%d manual=%d dead=%d heat=%.1f kmh=%.1f heading=%.1f twist=%.1f "
           "pitch=%.1f throttle=%.2f x=%d z=%d y=%.0f vy=%.2f fuel=%.0f jets=%d nojump=%d gone=0x%02x immobile=%d gain=%.2f health=%d "
           "tgt=%d tgt_dead=%d tgt_health=%d tgt_range=%.0f thing=%d thing_hp=%d aim=%d sel=%d group=%d curgrp=%d lock=0x%x attacks=%d shots=%d hits=%d "
           "vision=%d hud_off=%d satmap=%d radar_mode=%d radar_i=%d sat_i=%d vport=%d tgtdisp=%d dmg_off=%d menu=%d cc=%d autopilot=%d nav=%d "
           "ending=%d over=%d outcome=%d lost=%d enemies=%d enemies_dead=%d bld_destroyed=%d msg=%d fired=%d "
           "camode=%d ejected=%d intact=%d autoeject=%d cam_x=%.0f cam_y=%.0f cam_z=%.0f cam_yaw=%.1f follow=%u enemies_intact=%d weapons=",
           tag, (double)g_auto_elapsed, g_sim.now, g_pow, pu->shutdown, pu->shutdown_pending, pu->override, pu->manual_down, pu->destroyed,
           (double)pu->heat, (double)(g_pl_speed * 0.036f), (double)g_pl.heading, (double)g_pl.twist, (double)g_pitch, (double)g_throttle,
           g_pl.origin[0], g_pl.origin[2], (double)pu->y, (double)pu->vy, (double)pu->jet_fuel, pu->jets, pu->no_jump, gone, pu->immobile,
           (double)pu->speed_gain, combat_health(pu),
           g_tgt_sel, g_tgt_sel >= 0 && g_tgt_sel < g_actor_count && g_sim.armed[g_tgt_sel] ? g_sim.units[g_tgt_sel].destroyed : -1,
           g_tgt_sel >= 0 && g_tgt_sel < g_actor_count && g_sim.armed[g_tgt_sel] ? combat_health(&g_sim.units[g_tgt_sel]) : -1,
           g_tgt_sel >= 0 && g_tgt_sel < g_actor_count ? (double)hypotf((float)g_actors[g_tgt_sel].mech.origin[0] - (float)g_pl.origin[0], (float)g_actors[g_tgt_sel].mech.origin[2] - (float)g_pl.origin[2]) : -1.0,
           g_tgt_thing, g_tgt_thing >= 0 && g_tgt_thing < g_sim.bld_count ? g_sim.bld[g_tgt_thing].hp : -1, g_pl_target,
           g_sel_weapon, g_group_fire, g_cur_group, g_sim.player_lock, g_sim.player_attacks, g_sim.shot_count, g_sim.hits,
           g_vision, g_hud_off, g_satmap, g_radar_mode, g_radar_i, g_sat_i, g_vport, g_tgtdisp, g_dmg_off, g_mm_depth, g_cc_page, g_autopilot, g_nav_sel,
           g_sim.ending, g_sim.over, g_sim.outcome, g_sim.player_lost, armed, dead, g_sim.buildings_destroyed, g_msg_t > 0, test_fired,
           g_camode, pu->ejected, pu->intact_out, combat_auto_eject(), (double)g_cam_eye[0], (double)g_cam_eye[1], (double)g_cam_eye[2],
           (double)g_cam_yaw, g_follow, intact);
    for (i = 0; i < pu->weapon_count; i++) printf("%s%d:%d:%d:%d", i ? ";" : "", pu->weapons[i].weapon, pu->weapons[i].state, pu->weapons[i].ammo, pu->weapons[i].location);
    printf(" orb=%.0f,%.0f,%.1f orbtilt=%.2f campitch=%.2f pilot=%d fog=%g", (double)g_orb_d, (double)g_orb_h, (double)g_orb_ang, (double)g_orb_tilt, (double)g_cam_pitch,
           g_pilot, g_view ? (double)g_view->fog_density : -1.0);
    printf(" lvl=%d,%d,%d dhits=%d,%d snow=%d,%d snow_n=%d,%d snow_on=%d,%d snow_flips=%d,%d", g_inst_lvl[INST_RADAR], g_inst_lvl[INST_AUX], g_inst_lvl[INST_TGT],
           pu->display_hits, pu->display_hits5, g_snow_show[0], g_snow_show[1], g_snow_n[0], g_snow_n[1], g_snow_on_n[0], g_snow_on_n[1], g_snow_flips[0], g_snow_flips[1]);
    {   /* spawn overlap (training report) and the scrub billboards: units within 10 m of the player, the player's mech */
        int near = 0;
        for (i = 0; i < g_actor_count; i++)
            if (hypotf((float)g_actors[i].mech.origin[0] - (float)g_pl.origin[0], (float)g_actors[i].mech.origin[2] - (float)g_pl.origin[2]) < 1000.0f) near++;
        printf(" near=%d pmech=%s pload=%s billboards=%d", near, g_sim.player_skel, g_sim.player_loadout, glr_billboard_count());
        printf(" viewfar=%.0f exitfade=%.2f", (double)g_view_far, (double)(g_exit_fade_t0 >= 0 ? exit_fade_level(NULL) : 0.0f));
        printf(" fade=%.3f gnd=%.0f lvy=%.2f jolt=%.2f", (double)g_fade_level, (double)g_sim.player_unit.ground, (double)g_sim.player_unit.landed_vy, (double)g_jolt_view[3]);   /* the opening palette fade (1 = done); the ground under the player, its touch-down speed, the jolt pitch */
    }
    /* the walk-cycle keys: the player's and MW2_KILL_ACTOR's actor's (a destroyed mech keeps its pose: standing_wreck_frozen) */
    printf(" pkey=%.3f aheld=%d bracket=%d zoom=%.2f dhdt=%d", (double)g_pl_t, g_pl_anim_held, g_bracket, (double)g_zoom, (int)g_dhdt_s);
    printf(" rcl=%d", g_rcl);   /* the reticle drawn: -1 RETICLE, 0 RCLINOP 1 RCLLOCK 2 RCLNOLK 3 RCLPLOC 4 RCLTGT 5 RCLGLOC */
    printf(" heatbar=%d heats=%d heatx=%d thrbar=%d jetbar=%d", (int)g_heat_p, (int)g_heat_s, (int)g_heat_x, (int)g_thr_p, (int)g_jet_p);
    printf(" jetsm=%d", (int)g_jet_s);
    {   /* the navs' visited flags 0x20 (g_nav_seen), bit n = nav n */
        unsigned m = 0;
        for (i = 0; i < g_sim.nav_count && i < 32; i++) if (g_nav_seen[i]) m |= 1u << i;
        printf(" navseen=0x%x", m);
    }
    {   /* per unit: the range e would see (unit_targetable side 1, cm; -1 refused) and its weapon count */
        printf(" erng=");
        for (i = 0; i < g_actor_count; i++) { float dd = -1.0f; if (!unit_targetable(i, 1, &dd)) dd = -1.0f; printf("%s%.0f", i ? ";" : "", (double)dd); }
        printf(" uwpn=");
        for (i = 0; i < g_actor_count; i++) printf("%s%d", i ? ";" : "", g_sim.armed[i] ? g_sim.units[i].weapon_count : -1);
    }
    {   /* the player's carried pose offset (anim_player_step): the largest track's, and its track */
        int q, qm = -1; float om = 0;
        for (q = 0; q < ANIM_MAX_TRACKS; q++) if (fabsf(g_pl_ap.off[q]) > om) { om = fabsf(g_pl_ap.off[q]); qm = q; }
        printf(" poff=%.2f poff_tr=%d pseq=%d", (double)om, qm, g_pl_ap.cur);
    }
    if (getenv("MW2_WATCH_ACTOR") && g_sim_ok && g_sim.anim_pl) {   /* TEST ONLY: an actor's walk animation (idle_mech_rests) */
        int wa = atoi(getenv("MW2_WATCH_ACTOR"));
        if (wa >= 0 && wa < g_actor_count)
            printf(" w_key=%.3f w_seq=%d w_on=%d w_thr=%.3f w_x=%d w_z=%d w_hd=%.1f w_st=%d", (double)g_actors[wa].t, g_sim.anim_pl[wa].cur, g_sim.anim_pl[wa].on,
                   (double)(g_sim.minds ? g_sim.minds[wa].thr_servo : 0.0f), g_actors[wa].mech.origin[0], g_actors[wa].mech.origin[2],
                   (double)g_actors[wa].mech.heading, g_sim.minds ? g_sim.minds[wa].state : -1);
    }
    if (getenv("MW2_KILL_ACTOR")) {
        int ka = atoi(getenv("MW2_KILL_ACTOR"));
        if (ka >= 0 && ka < g_actor_count)
            printf(" k_key=%.3f k_dead=%d k_gone=0x%02x", (double)g_actors[ka].t, g_sim.units[ka].destroyed,
                   (g_sim.units[ka].loc_gone[0] != 0) | (g_sim.units[ka].loc_gone[2] != 0) << 2);
    }
    {   /* the F12 objectives list in display order (engine 0x10017b80: priorities 1, 2, 4, 0, 8; shown flag +0x74):
         * node:priority:state per row */
        static const int ORDER[5] = {1, 2, 4, 0, 8};
        int p, n, k = 0;
        printf(" objs=");
        for (p = 0; p < 5; p++)
            for (n = 0; n < g_sim.logic.tables[0].node_count; n++)
                if (g_sim.logic.shown[0][n] && g_sim.logic.tables[0].nodes[n].priority == ORDER[p])
                    printf("%s%d:%d:%d", k++ ? ";" : "", n, ORDER[p], g_sim.logic.state[0][n]);
        if (!k) printf("-");
    }
    /* the HUD's palette: colour 13 (the index the time-of-day records differ in) as RRGGBB, and its slot (3D editions) */
    printf(" hudc13=0x%02x%02x%02x hudslot=%d", (int)lrintf(g_hp[13][0] * 255.0f), (int)lrintf(g_hp[13][1] * 255.0f), (int)lrintf(g_hp[13][2] * 255.0f), g_hud_slot);
    printf(" parmor=");   /* armour + structure per location 1-8 */
    for (i = 0; i < 8; i++) printf("%s%.1f", i ? "," : "", (double)(pu->armor[i] + pu->internal[i]));
    if (test_drop_actor >= 0) {   /* MW2_TEST_DROP's actor */
        const combat_unit *au = &g_sim.units[test_drop_actor];
        int ag = 0;
        for (i = 0; i < 8; i++) ag |= (au->loc_gone[i] != 0) << i;
        printf(" a_draw=%d a_y=%.0f a_vy=%.2f a_ground=%.0f a_gone=0x%02x a_health=%d a_x=%d a_z=%d a_dead=%d a_armor=", g_actors[test_drop_actor].mech.origin[1], (double)au->y, (double)au->vy, (double)au->ground, ag, combat_health(au),
               g_actors[test_drop_actor].mech.origin[0], g_actors[test_drop_actor].mech.origin[2], au->destroyed);
        for (i = 0; i < 8; i++) printf("%s%.1f", i ? "," : "", (double)(au->armor[i] + au->internal[i]));
    }
    printf("\n");
    fflush(stdout);
}
static void test_frame(int w, int h)
{
    static int d_phit, d_heat, d_thing, d_bld, d_shot, dump_idx, d_node;
    static float next_every = -1;
    const char *sd = getenv("MW2_STATE_DUMP"), *sh = getenv("MW2_SHOT");
    if (!g_auto_on || !g_sim_ok || !g_world) return;
    {
        static int prev[COMBAT_MAX_WEAPONS];
        int i;
        for (i = 0; i < g_sim.player_unit.weapon_count && i < COMBAT_MAX_WEAPONS; i++) {
            int st = g_sim.player_unit.weapons[i].state;
            if (prev[i] == CW_READY && st >= 0 && st != CW_READY) test_fired++;
            prev[i] = st;
        }
    }
    if (getenv("MW2_TEST_BLD_LIST")) {   /* the structures: index, hit points, type, name, position */
        static int listed;
        int b;
        for (b = 0; !listed && b < g_sim.bld_count; b++)
            printf("BLD %d hp=%d type=%d x=%.0f z=%.0f %s/%s flags=0x%x side=%d\n", b, g_sim.bld[b].hp, g_sim.bld[b].type, (double)((g_sim.bld[b].mn[0] + g_sim.bld[b].mx[0]) * 0.5f),
                   (double)((g_sim.bld[b].mn[2] + g_sim.bld[b].mx[2]) * 0.5f), g_sim.bld[b].name, g_sim.bld[b].sub, g_sim.bld[b].flags, g_sim.bld[b].side);
        listed = 1;
    }
    test_timed("MW2_TEST_PHIT", &d_phit, test_phit);
    test_timed("MW2_TEST_NODE_OK", &d_node, test_node_ok);
    { static int d_ahit; test_timed("MW2_TEST_AHIT", &d_ahit, test_ahit); }
    { static int d_shotseg; test_timed("MW2_TEST_SHOTSEG", &d_shotseg, test_shotseg); }
    { static int d_fireline; test_timed("MW2_TEST_FIRELINE", &d_fireline, test_fireline); }
    { static int d_watch; test_timed("MW2_TEST_WATCH", &d_watch, test_watch); }
    { static int d_parmor; test_timed("MW2_TEST_PARMOR", &d_parmor, test_parmor); }
    if (getenv("MW2_TEST_WHEAT")) {   /* TEST ONLY */
        int w = -1; float h = 0;
        if (sscanf(getenv("MW2_TEST_WHEAT"), "%d:%f", &w, &h) == 2 && w >= 0 && w < 31) combat_test_wheat[w] = h;
    }
    { static int d_rfl; test_timed("MW2_TEST_RFLASH", &d_rfl, test_rflash); }
    if (test_heat_next >= 0) { g_sim.player_unit.heat = g_sim.player_unit.heat_mark = test_heat_next; test_heat_next = -1; }
    test_timed("MW2_TEST_HEAT", &d_heat, test_heat);
    { static int d_ka; test_timed("MW2_TEST_KILL_ALL", &d_ka, test_kill_all); }
    { static int d_ap; test_timed("MW2_TEST_APOWER", &d_ap, test_apower); }
    { static int d_rng; test_timed("MW2_TEST_RNG", &d_rng, test_rng); }
    { static int d_disp; test_timed("MW2_TEST_DISPLAY", &d_disp, test_display); }
    { static int d_imm; test_timed("MW2_TEST_IMMOBILE", &d_imm, test_immobile); }
    { static int d_dis; test_timed("MW2_TEST_DISARM", &d_dis, test_disarm); }
    { static int d_man; test_timed("MW2_TEST_MAN", &d_man, test_man); }
    test_timed("MW2_TEST_THING", &d_thing, test_thing);
    test_timed("MW2_TEST_BLD_KILL", &d_bld, test_bld_kill);
    { static int d_drop; test_timed("MW2_TEST_DROP", &d_drop, test_drop); }
    { static int d_probe; test_timed("MW2_TEST_PROBE", &d_probe, test_probe); }
    if (sd && sd[0] == '/') {
        float every = (float)atof(sd + 1);
        if (every <= 0) every = 1;
        if (next_every < 0) next_every = every;
        if (g_auto_elapsed >= next_every) { next_every += every; test_dump("tick"); }
    } else if (sd) {
        const char *q = sd;
        int idx = 0;
        while (*q) {
            if (idx == dump_idx && g_auto_elapsed >= (float)atof(q)) { char tg[16]; snprintf(tg, sizeof tg, "at%d", idx); test_dump(tg); dump_idx++; }
            idx++;
            q = strchr(q, ',');
            if (!q) break;
            q++;
        }
    }
    {
        const char *q = sh;
        int idx = 0;
        while (q && *q && idx < 32) {
            char fn[256] = "";
            float t = (float)atof(q);
            const char *c = strchr(q, ':');
            if (!c) break;
            sscanf(c + 1, "%255[^,]", fn);
            if (!(d_shot & (1 << idx)) && g_auto_elapsed >= t) {
                unsigned char *px = malloc((size_t)w * (size_t)h * 3);
                FILE *of = fopen(fn, "wb");
                d_shot |= 1 << idx;
                if (px && of) {
                    int y;
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);   /* tight rows: a width not a multiple of 4 overran px */
                    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
                    fprintf(of, "P6\n%d %d\n255\n", w, h);
                    for (y = h - 1; y >= 0; y--) fwrite(px + (size_t)y * (size_t)w * 3, 1, (size_t)w * 3, of);
                }
                if (of) fclose(of);
                free(px);
            }
            idx++;
            q = strchr(c, ',');
            if (!q) break;
            q++;
        }
    }
}
/* ---- end TEST ONLY ---- */

/* port (not an original behaviour - the DOS game had the whole keyboard): the system's keyboard shortcuts while a
 * mission is played (mw2port.cfg system_shortcuts=block, the default | allow). macOS serves its own shortcuts
 * (Mission Control's Ctrl+Left / Ctrl+Right "Move a space", Cmd+Tab, Spotlight) before the game sees the keys. SDL's
 * keyboard grab would turn them off with CGSSetGlobalHotKeyOperatingMode, but SDL2 2.30 and SDL3 3.2 make that call
 * only when built with SDL_MAC_NO_SANDBOX, which no SDL build defines (Cocoa_SetWindowKeyboardGrab in both
 * SDL_cocoakeyboard.m) - so SDL_SetWindowKeyboardGrab does nothing on a Mac and the port makes the same call itself,
 * looked up at run time (no link change; skipped if missing). The shortcuts come back in the MAIN MENU, while paused,
 * without the window's focus and at exit. Other systems: nothing changes. */
static int g_syskeys_allow;   /* system_shortcuts=allow */
static int g_syskeys_off;     /* the shortcuts are off now */
static void syskeys_apply(int off)
{
#if defined(__APPLE__)
    static int (*conn)(void);
    static int (*mode)(int, int);
    static int looked;
    if (!looked) {   /* CoreGraphics (SkyLight underneath) is already loaded with AppKit; else open it */
        void *h = RTLD_DEFAULT;
        looked = 1;
        if (!dlsym(h, "CGSSetGlobalHotKeyOperatingMode")) h = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY);
        if (h) { conn = (int (*)(void))dlsym(h, "_CGSDefaultConnection"); mode = (int (*)(int, int))dlsym(h, "CGSSetGlobalHotKeyOperatingMode"); }
        if (!conn || !mode) fprintf(stderr, "system shortcuts: CGSSetGlobalHotKeyOperatingMode not found - left on\n");
    }
    if (conn && mode) mode(conn(), off ? 1 : 0);   /* CGSGlobalHotKeyDisable 1 / CGSGlobalHotKeyEnable 0 (SDL's enum) */
#else
    (void)off;
#endif
}
static void syskeys_restore(void) { if (g_syskeys_off) { syskeys_apply(0); g_syskeys_off = 0; } }
static void syskeys_update(int want)
{
    static int registered;
    want = want && !g_syskeys_allow;
    if (want == g_syskeys_off) return;
    if (!registered) { atexit(syskeys_restore); registered = 1; }   /* any exit gives them back */
    syskeys_apply(want);
    g_syskeys_off = want;
    if (getenv("MW2_STATE_DUMP")) printf("SYSKEYS now=%d %s text=%d\n", (int)g_sim.now, want ? "off" : "on", (int)SDL_IsTextInputActive());   /* TEST hook */
}

/* the DOS look's radar (MW2.EXE 0x11950 -> 0x12c20): a view looking straight down from above the player's eye object (x,
 * z: 0x1df60 of unit +0x44, the EYEO node - msim player_eye, not the unit's origin), yaw = the legs' heading (+0x60), pitch 90 (0x5a0000), its rows from the sine table (0x3f230 ->
 * 0x3b522, mode 0) x the mantissas 0x2000 / (aspect >> 3) (+0x9c / +0xa0, shifts 3 / 3: 0x12c20, 0x3ece0); a point
 * through 0x13130: d = point - camera (whole cm), x = (R0.d >> 27 rounded << 3) / (range / W) + 2 >> 2 from the window's
 * centre (W >> 1), y = (H - 1) - ((R1.d ...) + (H >> 1)) - window-relative; range = the descriptor's +0x18 (the full
 * width: 200000 for "R: 1.0km"), W x H the window. DOSBox YELLSCN1 "R: 2.0km": the navs ((100, 152), (114, 119),
 * (119, 56) at 640 x 480; (159, 244), (181, 190), (189, 90) at 1024 x 768) and the enemies' blips as this to the pixel
 * (the port's float rule from the unit's origin had Nav Alpha 2 px low and blips 1 px off; its scale 122 px a km against
 * W / range = 123.5 at 1024 x 768; from the origin rather than the eye, (189, 91) for (189, 90)) */
static void dos_radar_point(int W, int H, float wx, float wz, float ex, float ez, float heading, int range, int sw, int sh, int *ox, int *oy)
{
    const int32_t asp = (int32_t)(((int64_t)sh * 65536 * 0x15555) / ((int64_t)sw * 65536));
    const int32_t b = (int32_t)lrint((double)heading * 65536.0), a = 0x5a0000;
    const int64_t cA = glr_dos_cos(a), sA = glr_dos_sin(a), cB = glr_dos_cos(b), sB = glr_dos_sin(b), cC = 0x20000000;
    int64_t m[9], d0, d2, R0[3], R1[3], t;
    int k, a4 = range / (W > 0 ? W : 1);
#define R29(v) (((v) >> 29) + (((v) >> 28) & 1))
#define R16(v) (((v) >> 16) + (((v) >> 15) & 1))
    m[0] = R29(cC * cB); m[3] = 0; m[6] = -R29(cC * sB);   /* 0x3b522 mode 0, roll 0 (sC = 0) */
    m[2] = R29(cA * sB); m[5] = -sA; m[8] = R29(cA * cB);
    m[1] = R29(m[5] * m[6] - m[3] * m[8]); m[4] = R29(m[8] * m[0] - m[6] * m[2]); m[7] = R29(m[2] * m[3] - m[0] * m[5]);
    for (k = 0; k < 3; k++) { R0[k] = R16(0x2000 * m[k * 3]); R1[k] = R16((int64_t)(asp >> 3) * m[k * 3 + 1]); }
    d0 = (int64_t)floor((double)wx + 0.5) - (int64_t)floor((double)ex + 0.5);
    d2 = (int64_t)floor((double)wz + 0.5) - (int64_t)floor((double)ez + 0.5);   /* (the height: row terms 0 looking down) */
    if (a4 < 1) a4 = 1;
    t = R0[0] * d0 + R0[2] * d2; t = (int32_t)((t >> 27) + ((t >> 26) & 1));
    *ox = (W >> 1) + (int)(((t * 8) / a4 + 2) >> 2);   /* (C division truncates as idiv) */
    t = R1[0] * d0 + R1[2] * d2; t = (int32_t)((t >> 27) + ((t >> 26) & 1));
    *oy = (H - 1) - ((int)(((t * 8) / a4 + 2) >> 2) + (H >> 1));
#undef R29
#undef R16
}
/* MW2.EXE 0x14530: a window-relative point inside the round radar - (x - W >> 1)^2 + ((y - H >> 1) << 16 / aspect)^2 <=
 * (W / 2 - 1)^2 */
static int dos_radar_inside(int W, int H, int x, int y, int sw, int sh)
{
    const int32_t asp = (int32_t)(((int64_t)sh * 65536 * 0x15555) / ((int64_t)sw * 65536));
    const int64_t dx = x - (W >> 1), dy = ((int64_t)(y - (H >> 1)) * 65536) / asp, r = W / 2 - 1;
    return dx * dx + dy * dy <= r * r;
}
/* MW2.EXE 0x145a0 -> 0x147c0: an out-of-range target's rim point - the angle of the point from the centre (0x3a9e5 of
 * ((hh - y) << 16 / aspect, hw - x)) to the rim as the cone's ends; straight up / down (dx 0 or past 16 bits) y = hh -+
 * the radius, x = hw (a point to the right that steep: 0, 0) */
static void dos_radar_edge(int W, int H, int x, int y, int sw, int sh, int *ox, int *oy)
{
    const int32_t asp = (int32_t)(((int64_t)sh * 65536 * 0x15555) / ((int64_t)sw * 65536));
    const int hw = W >> 1, hh = H >> 1, r = W / 2, dx = hw - x, dy = hh - y;
    const int fl = (dx >= 0 ? 1 : 0) | (dy >= 0 ? 2 : 0) | ((dx == 0 || dy / dx > 0x7fff || dy / dx < -0x8000) ? 4 : 0);
    const int32_t th = glr_dos_atan2((int32_t)(((int64_t)dy * 65536) / asp), dx);
#define R16(v) ((int32_t)(((int64_t)(v) >> 16) + (((int64_t)(v) >> 15) & 1)))
    const int32_t i1 = R16((int64_t)(r - 1) * asp) - 1;
    *ox = x; *oy = y;
    if (fl < 4) {
        *ox = hw - (R16((int64_t)(r - 2) * glr_dos_cos(th)) >> 13);
        *oy = hh - (R16((int64_t)i1 * glr_dos_sin(th)) >> 13);
    } else if (fl == 5) { *ox = hw; *oy = hh + i1; }
    else if (fl == 7) { *ox = hw; *oy = hh - i1; }
    else { *ox = 0; *oy = 0; }
#undef R16
}

/* MW2.EXE 0x14670 -> 0x147c0: the radar's rim point at angle a (16.16 degrees, 0 = right, 90 = up) in a window W x H
 * (x1 - x0 + 1 ...), window-relative: c = cos a >> 13, s = (sin a >> 13) x the view's aspect (+0x44 = (H << 16) x
 * 0x15555 / (W << 16) of the screen, 0x148a0: 0xffff at 640 x 480 and 1024 x 768) rounded; the angle again as the
 * arctangent of (s, -c) (0x3a9e5); x = hw - ((W / 2 - 2) x cos >> 16 rounded >> 13), y = hh - ((round16((W / 2 - 1) x
 * aspect) - 1) x sin >> 16 rounded >> 13), hw / hh = (W / H) >> 1; straight up / down (c 0 or s / c past 16 bits) y = hh
 * -+ that radius. The cone (0x120c0): a = 90 - the torso twist -+ the arctangent of (1, zoom) (45 at zoom 1) */
static void dos_radar_rim(int W, int H, int32_t a, int sw, int sh, int *ox, int *oy)
{
    const int32_t asp = (int32_t)(((int64_t)sh * 65536 * 0x15555) / ((int64_t)sw * 65536));
    const int hw = W >> 1, hh = H >> 1, r = W / 2;
    int32_t c, sn, th, i1;
    int fl, steep;
#define R16(v) ((int32_t)(((int64_t)(v) >> 16) + (((int64_t)(v) >> 15) & 1)))
    a = (int32_t)(((int64_t)a % 0x1680000 + 0x1680000) % 0x1680000);
    c = glr_dos_cos(a) >> 13;
    sn = R16((int64_t)(glr_dos_sin(a) >> 13) * asp);
    steep = c == 0 || sn / c > 0x7fff || sn / c < -0x8000;
    fl = (a >= 0x5a0000 && a <= 0x10e0000 ? 1 : 0) | (a < 0xb40001 ? 2 : 0) | (steep ? 4 : 0);
    th = glr_dos_atan2(sn, -c);
    i1 = R16((int64_t)(r - 1) * asp) - 1;
    *ox = hw; *oy = hh;
    if (fl < 4) {
        *ox = hw - (R16((int64_t)(r - 2) * glr_dos_cos(th)) >> 13);
        *oy = hh - (R16((int64_t)i1 * glr_dos_sin(th)) >> 13);
    } else if (fl == 5) *oy = hh + i1;
    else if (fl == 7) *oy = hh - i1;
    else { *ox = 0; *oy = 0; }
#undef R16
}

/* MW2.EXE 0x14020 -> 0x14240: an off-view point clipped to the screen window (0, 0)-(W - 1, H - 1) (DAT_000a46a8) along
 * the line from its half size (hw, hh) = ((x1 - x0 + 1) >> 1, ...): the slope (dy << 16) / dx (dx = hw - x, dy = hh - y;
 * none when dx is 0 or the quotient is out of 16 bits), by the quadrant the point lies in, onto the edge it leaves by -
 * x 0 or 2 hw, y 0 or 2 hh (one past the last pixel) */
static void dos_edge_point(int W, int H, int x, int y, int *ox, int *oy)
{
    const int p0 = W >> 1, p1 = H >> 1, dx = p0 - x, dy = p1 - y, i1 = p1 * 2;
    int fl = (dx >= 0 ? 1 : 0) | (dy >= 0 ? 2 : 0), sl = 0, i2 = p0 ? (int)(((int64_t)p1 << 16) / p0) : 0;
    if (dx == 0 || dy / dx > 0x7fff || dy / dx < -0x8000) fl |= 4;
    else sl = (int)(((int64_t)dy << 16) / dx);
#define R16(v) ((int)(((int64_t)(v) >> 16) + (((int64_t)(v) >> 15) & 1)))
    switch (fl) {
    case 0: if (i2 < sl) { *ox = p0 + (int)(((int64_t)p1 << 16) / sl); *oy = i1; return; } break;
    case 1: if (sl < -i2) { *ox = p0 + (int)(((int64_t)p1 << 16) / sl); *oy = i1; return; }
            *ox = 0; *oy = p1 - R16((int64_t)p0 * sl); return;
    case 2: if (sl < -i2) { *ox = p0 - (int)(((int64_t)p1 << 16) / sl); *oy = 0; return; } break;
    case 3: if (sl <= i2) { *ox = 0; *oy = p1 - R16((int64_t)p0 * sl); return; }
            *ox = p0 - (int)(((int64_t)p1 << 16) / sl); *oy = 0; return;
    case 5: *ox = p0; *oy = i1; return;
    case 7: *ox = p0; *oy = 0; return;
    default: *ox = 0; *oy = 0; return;
    }
    *ox = p0 * 2; *oy = p1 + R16((int64_t)p0 * sl);   /* (cases 0 / 2 past the corner's slope: the right edge) */
#undef R16
}

/* the DOS look's target bracket / nav marker in MW2.EXE's integers (dispatcher 0x29470): the point through 0x3f320
 * (glr_dos_project: the frame's own projection integers); in view - a nav point TGTNP (0xe5) at it, a mech (0x298c0)
 * the corners TGTGP1-4 at (x -+ q, y -+ q), q = ((zoom x half width (+0x94) x radius (geometry +0xe8)) >> 8) / z >> 6
 * (z the camera's, quarter cm), a thing (0x29a30) the same with half its sphere's radius, q at most a quarter of the
 * width ((+0x84) >> 1); out of view - TGTOFF (0xeb / 0xe8 / 0xee, a nav point 0xeb) at dos_edge_point. The 3D looks keep
 * the 3D engines' float rule. 0: no DOS frame (the float rule then) */
static int dos_hud_mark(const glr *r, int w, int h, const float wp[3], int isnav, int isthing, int side, float rad)
{
    const float gp_[3] = {wp[0], wp[1], -wp[2]};
    const float k = ((float)w < (float)h * 4.0f / 3.0f ? (float)w : (float)h * 4.0f / 3.0f) / 640.0f, ox = ((float)w - 640.0f * k) * 0.5f;
    int sx, sy, sz, vis = glr_dos_project(r, gp_, &sx, &sy, &sz);
    uint32_t zhw;
    int vw, vh;
    if (vis < 0) return 0;
    glr_dos_view(r, &zhw, &vw, &vh);
#define HX(px) (((float)(px) - ox) / k)
#define HY(py) ((float)(py) / k)
    if (vis) {
        g_bracket = isnav ? 3 : isthing ? 2 : 1;
        if (isnav) hud_draw(g_hud, &g_hs_navmk, HX(sx), HY(sy), NULL);
        else {
            int R = (int)lrintf(rad), q;
            if (isthing) R >>= 1;
            q = (int)((((int64_t)(int32_t)zhw * R) >> 8) / sz) >> 6;
            if (isthing && q > ((vw >> 1) >> 1)) q = (vw >> 1) >> 1;
            if (g_hs_gp[side][0].tex) {
                const hud_sprite *gp = g_hs_gp[side];
                hud_draw(g_hud, &gp[0], HX(sx - q), HY(sy - q), NULL); hud_draw(g_hud, &gp[1], HX(sx + q), HY(sy - q), NULL);
                hud_draw(g_hud, &gp[2], HX(sx - q), HY(sy + q), NULL); hud_draw(g_hud, &gp[3], HX(sx + q), HY(sy + q), NULL);
            } else hud_draw(g_hud, &g_hs_tgt[side == 0 ? 1 : side == 2 ? 2 : 0], HX(sx), HY(sy), NULL);
        }
    } else {
        int ex, ey;
        dos_edge_point(vw, vh, sx, sy, &ex, &ey);
        g_bracket = 4;
        hud_draw(g_hud, &g_hs_off[isnav ? 0 : side], HX(ex), HY(ey), NULL);
    }
#undef HX
#undef HY
    return 1;
}

int main(int argc, char **argv)
{
    char err[256], title[256];
    prj_archive *ma, *ta;
    SDL_Window *win;
    SDL_GLContext ctx;
    glr *r;
    glr_view v;
    int ati, mi = 0, ni = 0, camo = 0, clan = 0, running = 1, dragging = 0, msaa = 1, w, h, exit_code = 0;
    const char *rec = argc > 4 ? argv[4] : MECHS[0], *mission = argc > 5 ? argv[5] : MISSIONS[0];

    if (argc < 4) { fprintf(stderr, "usage: %s MODELS.PRJ TEXTURES.PRJ ati|3dfx [RECORD MISSION]\n", argv[0]); return 2; }
    if (!(ma = prj_open(argv[1], err, sizeof err)) || !(ta = prj_open(argv[2], err, sizeof err))) { fprintf(stderr, "%s\n", err); return 1; }
    /* the texture archive's kind: 0 3Dfx / S3, 1 ATi, 2 PowerVR, 3 DOS (the shell passes the edition's) */
    ati = strcmp(argv[3], "ati") == 0 ? 1 : strcmp(argv[3], "pvr") == 0 ? 2 : strcmp(argv[3], "dos") == 0 ? 3 : strcmp(argv[3], "mga") == 0 ? 4 : 0;
    g_drop_dos = ati == 3;
    g_tex_kind = ati;
    mech3d_dos_hips = ati == 3;   /* the DOS edition's unit placement */

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    {   /* MW2PORT.CFG (install dir): resolution desktop (native fullscreen, default) / window / WxH; aa samples.
         * Tests (an explicit MW2_SIZE, or the offscreen driver) stay windowed at that size. */
        portcfg pc;
        int ww = 1280, wh = 720, flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI, aa, tries;
        const char *drv = getenv("SDL_VIDEODRIVER");
        int test = getenv("MW2_SIZE") || (drv && strcmp(drv, "offscreen") == 0) || getenv("MW2_AUTOPILOT");
        portcfg_load(getenv("MW2_INSTALL_DIR"), &pc);
        if (getenv("MW2_SIZE")) sscanf(getenv("MW2_SIZE"), "%dx%d", &ww, &wh);   /* e.g. 1024x768 to compare with DOS */
        if (!test) {
            SDL_DisplayMode dm;
            if (pc.mode == 0 && SDL_GetDesktopDisplayMode(0, &dm) == 0) { ww = dm.w; wh = dm.h; flags |= SDL_WINDOW_FULLSCREEN_DESKTOP; }
            else if (pc.mode == 2) { ww = pc.w; wh = pc.h; flags |= SDL_WINDOW_FULLSCREEN; }
        }
        g_win_w = ww; g_win_h = wh;
        aa = pc.aa;
        g_render_w = pc.render_w; g_render_h = pc.render_h;
        g_cockpit_frame = getenv("MW2_COCKPIT_FRAME") ? atoi(getenv("MW2_COCKPIT_FRAME")) : 0;   /* 0: the engine's (repr 4), -1: none, 1: the old attempt */
        if (getenv("MW2_RENDER")) { g_render_w = g_render_h = 0; sscanf(getenv("MW2_RENDER"), "%dx%d", &g_render_w, &g_render_h); }   /* tests */
        if (g_render_w) aa = 0;   /* the original resolutions: no multisampling (the window is only blitted to) */
        win = NULL;
        for (tries = 0; tries < 5 && !win; tries++) {   /* the chosen samples, stepping down if the GPU refuses */
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, aa > 0);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, aa);
            win = SDL_CreateWindow("MechWarrior 2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, g_win_w, g_win_h, (Uint32)flags);
            if (win && !test && pc.mode == 2) {   /* exclusive fullscreen at the chosen mode */
                SDL_DisplayMode want = {0}, got;
                want.w = pc.w; want.h = pc.h;
                if (SDL_GetClosestDisplayMode(0, &want, &got)) SDL_SetWindowDisplayMode(win, &got);
            }
            if (!win) aa = aa > 2 ? aa / 2 : 0;
        }
        if (!win) {   /* last resort: windowed, no multisampling */
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
            win = SDL_CreateWindow("MechWarrior 2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        }
        if (!aa) msaa = 0;
        g_syskeys_allow = pc.system_shortcuts;
        g_hud_wide = !pc.hud_original;
        if (getenv("MW2_HUD_LAYOUT")) g_hud_wide = strcmp(getenv("MW2_HUD_LAYOUT"), "original") != 0;   /* tests */
        g_dos_sky_smooth = pc.dos_sky_smooth;
        if (getenv("MW2_DOS_SKY")) g_dos_sky_smooth = strcmp(getenv("MW2_DOS_SKY"), "smooth") == 0;   /* tests */
        if (getenv("MW2_DOS_SHADING")) g_dos_sky_smooth = strcmp(getenv("MW2_DOS_SHADING"), "smooth") == 0;   /* tests */
    }
    if (!win || !(ctx = SDL_GL_CreateContext(win))) {
        fprintf(stderr, "OpenGL 3.3: %s\n", SDL_GetError());
#ifdef _WIN32   /* a GUI program on Windows has no console: say it in a box */
        { char m[512]; snprintf(m, sizeof m, "Could not start OpenGL 3.3:\n%s\n\nUpdate your graphics driver and try again.", SDL_GetError());
          SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MechWarrior 2", m, win); }
#endif
        return 1;
    }
#ifdef _WIN32
    if (mw2gl_load() != 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MechWarrior 2", "This graphics driver does not provide OpenGL 3.3.\nUpdate your graphics driver and try again.", win);
        return 1;
    }
#endif
    SDL_GL_SetSwapInterval(1);
    /* port: no text input in the simulation - SDL2 starts it with the video (SDL_VideoInit) and sdl2-compat on every new
     * window (FinishWindowCreation), and on macOS active text input passes the keys through the input method
     * (Cocoa_HandleKeyEvent -> interpretKeyEvents): holding z (zoom) opened the press-and-hold accent picker and the
     * arrow keys then moved in it. Nothing here reads SDL_TEXTINPUT (the keys come as SDL_KEYDOWN) */
    SDL_StopTextInput();
    if (getenv("MW2_STATE_DUMP")) printf("TEXTINPUT active=%d\n", (int)SDL_IsTextInputActive());   /* TEST hook */
    { int dw0, dh0; SDL_GL_GetDrawableSize(win, &dw0, &dh0); g_win_lines = dh0; g_win_cols = dw0; }   /* the HUD set for native rendering */
    glr_default_view(&v);
    g_view = &v;
    if (rec[0] == '@') { g_world = 1; rec = MECHS[0]; SDL_SetRelativeMouseMode(SDL_TRUE); }
    if (!g_world && getenv("MW2_TEST_VIEWER")) {   /* TEST ONLY: the single-mech viewer's orbit "yaw,pitch" (with MW2_AUTOPILOT's
                                                    * seconds and file: a picture of it) */
        float vy = 0, vp = 0, vh = 0;
        if (sscanf(getenv("MW2_TEST_VIEWER"), "%f,%f,%f", &vy, &vp, &vh) >= 1) { v.yaw = vy; v.pitch = vp; g_test_vhead = vh; }
        g_walk = 0;   /* (a still pose) */
    }
    if (getenv("MW2_AUTOPILOT")) {
        char out[256] = "autopilot.ppm";
        int fire = 0, ck = 0;
        int jt = 0;
        if (sscanf(getenv("MW2_AUTOPILOT"), "%f,%f,%f,%d,%255[^,],%d,%d", &g_auto_secs, &g_auto_throttle, &g_auto_twist, &fire, out, &ck, &jt) >= 4) {
            g_auto_on = 1; g_auto_fire = fire; g_auto_cockpit = ck; g_auto_jet = jt;
            if (getenv("MW2_TEST_FPS") && atof(getenv("MW2_TEST_FPS")) >= 1.0) g_auto_dt = (float)(1.0 / atof(getenv("MW2_TEST_FPS")));   /* TEST ONLY */
            snprintf(g_auto_out, sizeof g_auto_out, "%s", out);
        }
    }
    {   /* the original's controls: GAMEKEY.MAP and INPUT.MAP as the sim opens them ("gamekey.map" / "input.map", MW2.EXE
         * 0x484e0 / 0x483b0) - found like every game file (datapath: the install folder, then the disc's MECH2 folder, any
         * case of the name; the shell's Cockpit Controls writes INPUT.MAP into the install folder); without them, the files
         * as the game installs them (tools/inputmap_defaults.h) */
        const char *dir = getenv("MW2_INSTALL_DIR") ? getenv("MW2_INSTALL_DIR") : ".";
        char gk[2048], in[2048];
        int hk, hi;
        dp_add_roots_from_env();
        hk = dp_resolve_in(dir, "GAMEKEY.MAP", gk, sizeof gk) == 0 || dp_resolve("GAMEKEY.MAP", gk, sizeof gk) == 0;
        hi = dp_resolve_in(dir, "INPUT.MAP", in, sizeof in) == 0 || dp_resolve("INPUT.MAP", in, sizeof in) == 0;
        im_load(&g_im, hk ? gk : NULL, hi ? in : NULL);
        printf("controls: GAMEKEY.MAP %s, INPUT.MAP %s\n", g_im.have_gamekey ? gk : "missing - the installed bindings",
               g_im.have_input ? in : "missing - the installed bindings");
        if (!g_im.have_gamekey) { FILE *f = fmemopen((void *)IM_DEFAULT_GAMEKEY, strlen(IM_DEFAULT_GAMEKEY), "r"); if (f) { im_parse_gamekey(&g_im, f); fclose(f); } }
        if (!g_im.have_input) { FILE *f = fmemopen((void *)IM_DEFAULT_INPUT, strlen(IM_DEFAULT_INPUT), "r"); if (f) { im_parse_input(&g_im, f); fclose(f); } }
        {   /* then each driver's giddi\<name>.std (MW2.EXE 0x483b0; the built-in set stands for the installed INPUT.MAP) */
            int ns = im_load_std(&g_im, im_open_game_file);
            if (ns) printf("controls: %d driver map file(s) (GIDDI\\<driver>.STD)\n", ns);
        }
        g_im_ok = g_im.nbinds > 0 && g_im.nrules > 0;
        g_im_mouse = g_im_ok && im_uses(&g_im, 2);
        {
            int i, k;
            for (i = 0; i < g_im.nrules; i++)
                for (k = 0; k < g_im.rules[i].ncond; k++)
                    if (strncmp(g_im.rules[i].name, "menu_", 5) == 0 && g_im.rules[i].c[k].device == 2) g_im_menu_mouse = 1;
        }
        if (g_im_ok && im_uses(&g_im, 1) && SDL_InitSubSystem(SDL_INIT_JOYSTICK) == 0 && SDL_NumJoysticks() > 0)
            g_im_dev.joy = SDL_JoystickOpen(0);   /* the configured stick: the first one SDL finds */
        printf("controls: %d key commands, %d control rules (%s)\n", g_im.nbinds, g_im.nrules, dir);
    }
    if (getenv("MW2_INSTALL_DIR")) {   /* Combat Variables detail (MW2SND.CFG int32 +0x14 object textures, +0x18 terrain textures,
                                         * +0x1c display detail, +0x20 object density, +0x24 chunky explosions) */
        char p2[1100];
        FILE *fs;
        unsigned char snd[60];
        snprintf(p2, sizeof p2, "%s/MW2SND.CFG", getenv("MW2_INSTALL_DIR"));
        if ((fs = fopen(p2, "rb"))) {
            if (fread(snd, 1, sizeof snd, fs) == sizeof snd) {
                g_notex_actors = (snd[0x14] | snd[0x15] | snd[0x16] | snd[0x17]) == 0;
                g_notex_world = (snd[0x18] | snd[0x19] | snd[0x1a] | snd[0x1b]) == 0;
                g_lodq = (snd[0x1c] | snd[0x1d] | snd[0x1e] | snd[0x1f]) == 0 ? 2 : 1;   /* DISPLAY DETAIL low: "LOD quality" 2 (0x1002ff90) */
                fx_detail_low = g_lodq == 2;
                world3d_density_low = (snd[0x20] | snd[0x21] | snd[0x22] | snd[0x23]) == 0;
            }
            fclose(fs);
        }
    }
    if (mission && mission[0] && !getenv("MW2_AUTOPILOT")) {   /* the drop screen while the mission loads */
        drop_screen_build(ma, mission);
        g_drop_t0 = SDL_GetTicks();
        {   /* a new window settles over its first events (macOS: a Retina window's drawable reports its point size until
             * then, and the first picture landed in a corner, held there while the mission loaded): pump and draw a few
             * frames before loading */
            int q;
            for (q = 0; q < 8; q++) { SDL_PumpEvents(); drop_screen_draw(win); SDL_Delay(16); }
        }
    }
    {   /* the look (the shell's Combat Variables RENDERER and TEXTURES, mw2port.cfg renderer= / textures=): known before
         * the mission loads (the Titanium textures are taken as it does) */
        const char *e = getenv("MW2_EDITION"), *t = getenv("MW2_TEXTURES");
        int k;
        g_edition = g_tex_kind == 3 ? PORTCFG_ED_DOS : PORTCFG_ED_ENHANCED;
        for (k = 0; e && k < PORTCFG_ED_COUNT; k++) if (strcmp(e, PORTCFG_ED_NAME[k]) == 0) g_edition = k;
        if (g_tex_kind == 3) g_edition = PORTCFG_ED_DOS;   /* the DOS renderer is the DOS edition, whatever MW2_EDITION says: its
                                                            * rules follow (CT spread rand(2), MW2.EXE 0x263c8; the self-destruct
                                                            * at once) - the shell sets both together (portcfg_edition) */
        /* (MW2_EDITION=titanium: the old single look - the enhanced mix with the Titanium textures) */
        g_texset = (t && strcmp(t, "titanium") == 0) || (e && strcmp(e, "titanium") == 0) || g_edition == PORTCFG_ED_RENDITION ? PORTCFG_TX_TITANIUM : PORTCFG_TX_ORIGINAL;
        if (ati == 3 || (g_texset == PORTCFG_TX_TITANIUM && titanium_open(1) != 0)) g_texset = PORTCFG_TX_ORIGINAL;   /* (DOS; no folder) */
        if (g_edition == PORTCFG_ED_RENDITION && g_texset != PORTCFG_TX_TITANIUM) g_edition = PORTCFG_ED_ENHANCED;   /* (a Titanium plugin: no folder, no RENDITION) */
    }
    if (!(r = glr_create()) || load(r, ma, ta, ati, rec, mission, camo, clan) != 0) return 1;
    g_sfx = sfx_create(ma);   /* NULL without an audio device: silent */
    if (g_world && g_sim_ok && !g_drop_tex) music_start(g_sim.music_track);   /* the mission was loaded before the mixer existed (with the drop screen: as it fades) */
    if (g_sfx && getenv("MW2_INSTALL_DIR")) {   /* the EFFECTS and VOICE sliders: MW2SND.CFG +4 / +8, 16.16 */
        unsigned char snd[60];
        char p[1100];
        FILE *f;
        snprintf(p, sizeof p, "%s/MW2SND.CFG", getenv("MW2_INSTALL_DIR"));
        if ((f = fopen(p, "rb"))) {
            if (fread(snd, 1, sizeof snd, f) == sizeof snd) {
                long fx = (long)(snd[4] | snd[5] << 8 | snd[6] << 16 | (unsigned long)snd[7] << 24), vo = (long)(snd[8] | snd[9] << 8 | snd[10] << 16 | (unsigned long)snd[11] << 24);
                sfx_set_gains(g_sfx, fx > 65536 ? 1.0f : (float)fx / 65536.0f, vo > 65536 ? 1.0f : (float)vo / 65536.0f);
                g_voice_off = vo <= 0;
            }
            fclose(f);
        }
    }
    g_show_objectives = getenv("MW2_SHOW_OBJECTIVES") != NULL;   /* tests: open the F12 display */
    g_sim.dos_end = g_sim.sd_at_once = g_edition == PORTCFG_ED_DOS; combat_ct_split = g_edition == PORTCFG_ED_DOS ? 2 : 3; combat_heat_dos = g_edition == PORTCFG_ED_DOS; msim_dos_int = ai_dos_int = g_edition == PORTCFG_ED_DOS;   /* DOS: the integer throttle servo (0x49b80) */
    edition_look(&v);
    if (getenv("MW2_GROUPS")) {   /* tests: "g:w,w;g:w" e.g. "1:2,3" puts weapons 2 and 3 in group 1 */
        const char *p = getenv("MW2_GROUPS");
        while (*p) {
            int g = atoi(p) - 1;
            while (*p && *p != ':') p++;
            if (*p) p++;
            while (*p && *p != ';') { if (g >= 0 && g < 3) set_weapon_group(atoi(p), g); while (*p && *p != ',' && *p != ';') p++; if (*p == ',') p++; }
            if (*p == ';') p++;
        }
    }
    load_projectiles(ma);
    if (getenv("MW2_SFX_TEST")) {   /* debug: play SNDS records at start ("199,75") */
        const char *p = getenv("MW2_SFX_TEST");
        while (*p) { sfx_play(g_sfx, atoi(p), 0.9f, 0.0f); while (*p && *p != ',') p++; if (*p) p++; SDL_Delay(700); }
    }

    while (running) {
        SDL_Event e;
        int reload = 0;
        perf_frame_start();
        if (g_drop_tex) {   /* hold the drop screen until 2 s after launch (user choice; was 5 s); the mission waits. Then
                             * as the original: the mission's CD track starts and the drop screen fades to black over 0.86 s
                             * before the first mission frame (DOSBox YELLSCN1 / TNJ1SCN1: the music from the start of the
                             * fade). DOS MW2.EXE 0x15d49 -> 0x3eb10(0) -> VFX 0x58e0d(window 0xa46bc, palette, 0x3c): a DDA
                             * fade, every colour linear to its target over 0x3c steps - 60 / 70 Hz retraces = 0.857 s */
            Uint32 el = SDL_GetTicks() - g_drop_t0;
            static int music_on;
            if (el >= 2000 && !music_on) { music_on = 1; if (g_world && g_sim_ok) music_start(g_sim.music_track); }
            if (el < 2000 + 860) {
                g_drop_level = el <= 2000 ? 1.0f : 1.0f - (float)(el - 2000) / 860.0f;
                while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) running = 0;
                drop_screen_draw(win);
                SDL_Delay(15);
                continue;
            }
            glDeleteTextures(1, &g_drop_tex); g_drop_tex = 0;
            glDeleteTextures(5, g_drop_texs); memset(g_drop_texs, 0, sizeof g_drop_texs);
        }
        sfx_log_time = g_sim_ok && g_world ? g_sim.now / 1000.0 : -1;   /* TEST ONLY (MW2_SFX_LOG) */
        vq_drain();   /* the voice queue (0x100313d0) */
        if (g_sim_ok && g_world && g_sfx) {   /* missile lock tones: MECBYHLX as a guided weapon starts acquiring (0x40),
                                               * MECXLOCK when it locks (0x80) - by their names */
            static int t_trk = -1, t_lock = -1;
            int lk = g_sim.player_lock;
            /* engine 0x1001e6c0 (DOS 0x33030), only in the cockpit view (DAT_1024e678; the latches hold outside it):
             * locked (0x80) with a missile weapon selected (slot +8 == 1) and the mech up (+0xa0 == 2): 0xfe MECXLOCK
             * once (latch 0x1024cd54, cleared while not locked) at volume 0x64, pan 0x5f; not locked but acquiring
             * (0x40): 0xcf MECBYHLX once (latch 0x1024cd58, cleared when 0x40 drops - a brief break that keeps 0x40
             * does not repeat it) at 0x64, pan 0x1f. The port's lock exists only with a guided weapon selected */
            static int l_lock, l_trk;
            const combat_unit *pu3 = &g_sim.player_unit;
            int up = !pu3->destroyed && !pu3->shutdown && !(g_sim.now < g_sim.player_online_at && !getenv("MW2_NO_STARTUP"));
            if (t_trk < 0) { int st = g_ma ? prj_find_type(g_ma, "SNDS") : -1; t_trk = st >= 0 ? prj_find_id(g_ma, st, "MECBYHLX") : 0; t_lock = st >= 0 ? prj_find_id(g_ma, st, "MECXLOCK") : 0; }
            if (VIEW_IN_COCKPIT) {
                if (lk & 0x80) { if (!l_lock && up) { l_lock = 1; if (t_lock > 0) sfx_play(g_sfx, t_lock, SFX_V64, sfx_pan(0x5f)); } }
                else {
                    l_lock = 0;
                    if (!(lk & 0x40)) l_trk = 0;
                    else if (!l_trk && up) { l_trk = 1; if (t_trk > 0) sfx_play(g_sfx, t_trk, SFX_V64, sfx_pan(0x1f)); }
                }
            }
        }
        if (g_sim_ok && g_world) on_radio(NULL, NULL);   /* the radio queue */
        if (g_auto_on && getenv("MW2_MOUSE")) {   /* TEST ONLY: "secs:dx:dy,..." mouse motion (SDL relative units) at autopilot times */
            static int msent;
            const char *q = getenv("MW2_MOUSE");
            int idx = 0, dx, dy;
            float at;
            while (*q && sscanf(q, "%f:%d:%d", &at, &dx, &dy) == 3) {
                if (idx == msent && g_auto_elapsed >= at) {
                    SDL_Event me;
                    memset(&me, 0, sizeof me);
                    me.type = SDL_MOUSEMOTION; me.motion.xrel = dx; me.motion.yrel = dy;
                    SDL_PushEvent(&me);
                    msent++;
                }
                idx++;
                q = strchr(q, ',');
                if (!q) break;
                q++;
            }
        }
        if (g_auto_on && getenv("MW2_KEYS")) {   /* tests: "secs:key,..." key presses at autopilot times (esc, 0-9, up, down,
                                                  * left, right, space, enter, tab, or a letter; "ctrl+" before one holds Ctrl) */
            static int sent;
            const char *q = getenv("MW2_KEYS");
            int idx = 0;
            while (*q) {
                float at = (float)atof(q);
                char kn[32] = "";
                const char *c = strchr(q, ':');
                if (!c) break;
                sscanf(c + 1, "%31[^,]", kn);
                if (idx == sent && g_auto_elapsed >= at) {
                    SDL_Event ke;
                    memset(&ke, 0, sizeof ke);
                    ke.type = SDL_KEYDOWN; ke.key.state = SDL_PRESSED;
                    {   /* TEST hook (tests/sequences): named keys incl. f1-f12, ctrl+ / shift+ / alt+ */
                        int mods = 0;
                        ke.key.keysym.sym = test_keycode(kn, &mods);
                        ke.key.keysym.mod = (Uint16)mods;
                    }
                    ke.key.keysym.scancode = SDL_GetScancodeFromKey(ke.key.keysym.sym);
                    SDL_PushEvent(&ke);
                    sent++;
                }
                idx++;
                q = strchr(c, ',');
                if (!q) break;
                q++;
            }
        }
        /* the system's shortcuts off while the mission is played in front (see syskeys_update) */
        syskeys_update(g_world && !g_mm_depth && !g_paused && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)
                      && !(SDL_GetModState() & KMOD_GUI));   /* Cmd held: the system gets its shortcuts back (Cmd+Tab, Cmd+Shift+4 ...) */
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if ((g_mm_depth || g_paused) && (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEWHEEL)
                     && !(e.type == SDL_MOUSEMOTION && g_mm_depth && !g_paused && g_im_menu_mouse)) continue;   /* controls off (0x1003a3c0); the mouse driver still moves for menu controls on it */
            else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) dragging = 1;
            else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) dragging = 0;
            else if (e.type == SDL_MOUSEMOTION && g_world && g_pilot && g_pl_ok) {
                /* the mouse driver's box (MOUSE.DLL, see im_devstate): a mickey a unit, stopped at the edges; read by the
                 * controls INPUT.MAP gives it (the torso sinks below) - with no mouse in INPUT.MAP the port reads it as
                 * MOUSE.MAP does (torso_pan + Left/Right, torso_tilt - Up/Down) */
                int x = g_im_dev.mouse_x + e.motion.xrel, y = g_im_dev.mouse_y + e.motion.yrel;
                g_im_dev.mouse_x = x < -512 ? -512 : x > 511 ? 511 : x;
                g_im_dev.mouse_y = y < -512 ? -512 : y > 511 ? 511 : y;
            } else if (e.type == SDL_MOUSEMOTION && dragging && v.free_cam) {
                v.look_yaw += (float)e.motion.xrel * 0.25f;
                v.look_pitch -= (float)e.motion.yrel * 0.25f;
                if (v.look_pitch > 89) v.look_pitch = 89;
                if (v.look_pitch < -89) v.look_pitch = -89;
            } else if (e.type == SDL_MOUSEMOTION && dragging) {
                v.yaw -= (float)e.motion.xrel * 0.4f;
                v.pitch += (float)e.motion.yrel * 0.3f;
                if (v.pitch > 85) v.pitch = 85;
                if (v.pitch < -85) v.pitch = -85;
            } else if (e.type == SDL_MOUSEWHEEL) {
                if (v.distance <= 0) v.distance = 3.0f;
                v.distance *= e.wheel.y > 0 ? 0.9f : 1.1f;
            } else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
#ifdef __APPLE__
                if (e.key.keysym.mod & KMOD_GUI) continue;   /* Cmd+key belongs to macOS (Cmd+Tab, screenshots), never the game */
#endif
                if (g_mm_depth) { mm_key(k, e.key.keysym.mod); continue; }   /* the MAIN MENU has the keys while open */
                if (g_paused) {   /* 0x10009740: paused, the next key (DAT_10159e64, a character) ends the pause; nothing else */
                    if (k != SDLK_LSHIFT && k != SDLK_RSHIFT && k != SDLK_LCTRL && k != SDLK_RCTRL && k != SDLK_LALT && k != SDLK_RALT &&
                        k != SDLK_LGUI && k != SDLK_RGUI && k != SDLK_CAPSLOCK) {
                        g_paused = 0;
                        if (g_sfx) sfx_pause(g_sfx, 0);
                    }
                    continue;
                }
                if (g_world && g_sim_ok && g_cc_page >= 0 && (k == SDLK_ESCAPE || (k >= SDLK_0 && k <= SDLK_9))) {
                    /* 0x10065df0: a digit picks the row with that number (0 the Esc row); Esc / the Esc row go back a page
                     * (0x10066330), from the first page close the menu */
                    cc_row rr[10];
                    char tt[32];
                    float rc[4], vf;
                    int nrow, nr = cc_rows(g_cc_page, tt, sizeof tt, rr, &nrow, rc, &vf), item = k == SDLK_ESCAPE ? -1 : (int)(k - SDLK_0), q, act = 0;
                    for (q = 0; q < nr && item > 0; q++) if (rr[q].num == item) act = rr[q].act;
                    if (item <= 0) g_cc_page = g_cc_page == 1 || (g_cc_page >= 2 && g_cc_page < CC_SYSTEMS && g_cc_from_root) ? 0 : -1;
                    else if (act > 0) { g_cc_page = act - 1; g_cc_from_root = 1; }
                    else if (act < 0 && g_cc_page == CC_SYSTEMS) {   /* the toggles (0x10064d90 -> the value record's setter); the panel stays */
                        int it = -act;
                        if (it <= 2) {   /* as INFRARED / ENHANCED_VISION (0x10007a50) */
                            int want = it, was = g_vision;
                            if (want == 1 && g_inst_lvl[INST_RADAR] > 0 && g_vision != 1) want = 0;
                            if (want) g_vision = g_vision == want ? 0 : want;
                            if (g_vision == 1 && was != 1 && g_sfx) sfx_play(g_sfx, 0xb2, SFX_V64, 0.0f);   /* 0x10007aa0: volume 0x64 */
                            if (g_vision == 1 && was != 1) hud_message_v2("Light amplification engaged.", 77, 80);
                            else if (g_vision == 2 && was != 2) hud_message_v2("Image enhancement engaged.", 76, 80);
                        } else if (it == 3) g_hud_off = !g_hud_off;
                        else if (!g_sim.player_unit.destroyed) {
                            combat_set_auto_eject(!combat_auto_eject());
                            hud_message_ex(combat_auto_eject() ? "Automatic ejection ON" : "Automatic ejection OFF", 0x16a, 0x32);
                        }
                    }
                    else if (act < 0 && g_cc_page == 1) { if (msim_set_formation(&g_sim, -act - 1) == 0) g_cc_page = -1; }
                    else if (act < 0) {
                        msim_command(&g_sim, g_cc_page - 2, CC_ORDER[-act - 1], g_tgt_sel);   /* the menu closes (0x100659a0) */
                        g_cc_page = -1;
                    }
                    continue;
                }
                if (g_world && g_sim_ok && (A("USER_MENU") || A("ALL_PT_MENU") || A("PT_2_MENU") || A("PT_3_MENU"))) {
                    /* each key's own menu: opened, or closed when it is the one up (0x10065470) */
                    int want = A("USER_MENU") ? CC_SYSTEMS : A("PT_2_MENU") ? 3 : A("PT_3_MENU") ? 4 : 0;
                    int up = g_cc_page < 0 ? -1 : g_cc_page == CC_SYSTEMS ? CC_SYSTEMS : g_cc_from_root ? 0 : g_cc_page;
                    if (up == want) g_cc_page = -1;
                    else if (g_cc_page < 0) { g_cc_page = want; g_cc_from_root = 0; }   /* one menu at a time (0x10065440) */
                    continue;
                }
                if (g_sim_ok && g_world && g_sim.dos_prompted && !g_sim.over && k != SDLK_LSHIFT && k != SDLK_RSHIFT && k != SDLK_LCTRL &&
                    k != SDLK_RCTRL && k != SDLK_LALT && k != SDLK_RALT && k != SDLK_LGUI && k != SDLK_RGUI && k != SDLK_CAPSLOCK) {
                    msim_end_now(&g_sim);   /* DOS 0x45d80: the prompt up (DAT_000982f4), any key ends the mission */
                    continue;
                }
                if (g_sim_ok && g_world && g_sim.ending && !g_sim.over && k == SDLK_ESCAPE && !A("EXIT_SIM")) continue;   /* the end phase: key dispatch off (0x100374e0) */
                if (g_sim_ok && g_world && !g_sim.ending && (A("MAIN_MENU") || (!g_im_ok && k == SDLK_ESCAPE))) {
                    mm_open();   /* 0x10037c34: menu 4 (opens only with no other menu up - the COMMAND COMPUTER takes Esc first) */
                    continue;
                }
                if ((k == SDLK_ESCAPE && !(g_sim_ok && g_world)) || A("EXIT_SIM")) {
                    if (g_sim_ok && g_world && !g_sim.over) msim_end_now(&g_sim);   /* abort: results are still written */
                    else running = 0;
                }
                else if (A("DISPLAY_OBJECTIVES")) g_show_objectives = !g_show_objectives;
                else if (g_world && (A("ADD_WEAPON_TO_GROUP_1") || A("ADD_WEAPON_TO_GROUP_2") || A("ADD_WEAPON_TO_GROUP_3")))
                    /* actions 0x97-0x99 (0x10044350): the selected weapon moves to that group */
                    set_weapon_group(g_sel_weapon, A("ADD_WEAPON_TO_GROUP_1") ? 0 : A("ADD_WEAPON_TO_GROUP_2") ? 1 : 2);
                else if (g_world && g_sim_ok && (A("NEXT_TARGET") || A("PREV_TARGET") || A("RESET_TARGETTING") || A("TARGET_NEAREST_ENEMY") ||
                                                 A("TARGET_FRIENDLY") || A("TARGET_AT_RETICLE"))) {
                    int best = -1, nxt = A("NEXT_TARGET");
                    float bd = 1e30f;
                    int had = g_tgt_sel * 65536 + g_tgt_thing;
                    if (A("TARGET_AT_RETICLE")) target_at_reticle();   /* keeps the target when nothing may be taken */
                    else if (!A("RESET_TARGETTING")) g_tgt_any = 0;   /* 0x1001cca0: the cycling keys clear DAT_1024c8b0 */
                    if (A("RESET_TARGETTING")) { g_tgt_sel = -1; g_tgt_thing = -1; }   /* 0x1001ce80: DAT_1024c8b0 left as it is */
                    else if (A("TARGET_AT_RETICLE")) {}
                    else if (A("TARGET_NEAREST_ENEMY")) {
                        /* 0x1001db60: walks 0x1001cca0 with 0x40008 (0x1001db40: units and things of alliance 1 -
                         * 0x10019040 / 0x10019060 - by the 0x1001d050 / 0x1001d2d0 rules) one full round from the FIRST
                         * entry (the first call with step 0, then +1; DOS 0x2acf0 the same), keeping the first nearest
                         * (+0xc4, strict <); none, or the nearest beyond 1750 m (a 0x1000 thing): the old selector put
                         * back - the target stays (DOSBox TNJ1SCN1: e with only the neutral Instructor and the resting
                         * Timber Wolf: nothing; t, then e: still the Instructor). So e never cycles: every press takes
                         * the nearest enemy, armed or not (DOSBox ORANSCN1 at Nav Alpha: e the Trainer Maddog / the
                         * unarmed Training Firemoth, whichever is nearer at the press; t cycles Maddog, Kitfox,
                         * Firemoth, Nova) */
                        int wl[512], nw = target_walk(0, 1, wl, 512), k;
                        int n2 = g_actor_count;
                        for (k = 0; k < nw; k++) {
                            float dd = 1e30f;
                            if (wl[k] < n2) unit_targetable(wl[k], 1, &dd); else thing_targetable(wl[k] - n2, &dd);
                            if (dd < bd) { bd = dd; best = wl[k]; }
                        }
                        if (best < 0 || bd > 175000.0f) had = -1;   /* the old selector stays: no change */
                        else if (best < n2) { g_tgt_sel = best; g_tgt_thing = -1; }
                        else { g_tgt_sel = -1; g_tgt_thing = best - n2; }
                    } else {                                                          /* NEXT / PREV_TARGET, TARGET_FRIENDLY */
                        /* 0x1001cca0 (t / r: +1 / -1 with flags 8; f: 0x1001db20(1), +1 with 0x20008 - the player's side
                         * only, one step, not the nearest - DOSBox TNJ1SCN1: f with no friendly unit: nothing, the target
                         * cleared): the first entry target_walk finds (units by unit_targetable, things by
                         * thing_targetable); none may be taken: no target */
                        int wl[1], fr = A("TARGET_FRIENDLY");
                        int nw = target_walk(nxt || fr ? 1 : -1, fr ? 0 : -1, wl, 1), n2 = g_actor_count;
                        g_tgt_sel = -1; g_tgt_thing = -1;
                        if (nw > 0) { if (wl[0] < n2) g_tgt_sel = wl[0]; else g_tgt_thing = wl[0] - n2; }
                    }
                    /* one selector (+0xdc): a unit or thing taken - or none left by Ctrl+t / a cycle that found nothing -
                     * replaces a selected nav point (q taking nothing and e finding nothing leave it) */
                    if (A("RESET_TARGETTING") || (A("TARGET_AT_RETICLE") ? g_tgt_sel * 65536 + g_tgt_thing != had : had != -1)) g_nav_sel = -1;
                }
                else if (g_world && g_sim_ok && (A("NEXT_NAVPOINT") || A("PREV_NAVPOINT")) && g_sim.nav_count > 0) {
                    /* nothing selected yet (the player's selector +0xdc = 0x1000 at the mission load, 0x100235e0
                     * at 0x100237fe): 0x1001cca0 restarts at the first entry going forward whatever the key (step 1), so
                     * NEXT and PREV both pick the first selectable nav point - Nav Alpha on TNJ1 (DOSBox: nothing in
                     * the target viewer at the start, N -> "Nav Alpha 528m", N -> "Nav Beta"); the port had stepped on
                     * from the nav the HUD shows (Alpha) to Beta */
                    g_nav_sel = g_nav_sel >= 0 ? nav_step(g_nav_sel, A("PREV_NAVPOINT") ? -1 : 1) : nav_step(-1, 1);
                    g_tgt_sel = -1; g_tgt_thing = -1;   /* the nav point shows in the target display (a selected target had hidden it) */
                    g_tgt_any = 0;   /* 0x1001dac0 -> 0x1001cca0 */
                }
                else if (g_world && g_satmap && (A("RADAR_ZOOM_IN") || A("RADAR_ZOOM_OUT")))   /* 0x10004040 on mode 4: halve / double, wrapping */
                    g_sat_i = (g_sat_i + (A("RADAR_ZOOM_OUT") ? 4 : 1)) % 5;
                else if (g_world && (A("RADAR_ZOOM_IN") || A("RADAR_ZOOM_OUT"))) {   /* 0x10004040: in halves the range, below
                    * the minimum (+0x24 50000) it wraps to the maximum (+0x28 400000); out doubles it, past the maximum to the minimum:
                    * 1.0km -x-> 500m -> 250m -> 2.0km -> 1.0km, X the other way (DOS capture); the radar off: nothing */
                    if (g_radar_mode != 0) g_radar_i = (g_radar_i + (A("RADAR_ZOOM_OUT") ? 1 : 3)) % 4;
                }
                else if (g_world && (A("TOGGLE_HUD") && ui_click())) g_hud_off = !g_hud_off;
                else if (g_world && (A("TOGGLE_DAMAGE_DISPLAY") && ui_click())) g_dmg_off = !g_dmg_off;
                else if (g_world && (A("TOGGLE_HTAL") && ui_click())) g_htal = !g_htal;
                else if (g_world && A("NEXT_RADAR_MODE") && !g_satmap) { g_radar_mode = (g_radar_mode + 1) % 3; g_radar_big = g_radar_mode == 2; }   /* 0x10003fb0 / DOS 0x12330: only
                    * while the radar mode is below 3 - F2 in the satellite map does nothing (DOSBox YELLSCN1: F3, F2, F3 -> the small radar) */
                else if (g_world && g_sim_ok && (A("SHUTDOWN_MECH") || A("STARTUP_MECH")) && g_pilot) {
                    /* SHUTDOWN_MECH s / STARTUP_MECH Ctrl+s: both actions are the same toggle (DOS key handler 0x4608a: table
                     * entries 0x3d and 0x3e both jump to 0x4658e; 3Dfx 0x10037880 cases 0x3d / 0x3e): a mech in state 3 (shut
                     * down) gets a start-up request, any other state a shutdown request (DOSBox: a second s powers the mech
                     * up like Ctrl+s). The request is acted on by 0x28325 / 0x1001adf0: shutdown - message 0xd "Shutting
                     * down", state 3 (c[0x23] untouched); start-up - only with flag 4 clear or flag 8 set: "Powering up...",
                     * state 0, the start-up again (a refused request just stays pending; the heat restart follows anyway) */
                    combat_unit *pu2 = &g_sim.player_unit;
                    if (pu2->destroyed) ;
                    else if (pu2->shutdown) {
                        if (msim_player_restart(&g_sim)) {
                            hud_message_v("Powering up...", 0);
                            /* state 0 sets c[0x23] to the online time (0x1001ab71), which the overheat timer counts from */
                            pu2->seq_ticks = -(g_sim.player_online_at - (float)g_sim.now) * 0.182f;
                        }
                    }
                    else { pu2->shutdown = 1; pu2->manual_down = 1; g_throttle = 0; hud_message_v("Shutting down", 14); if (g_sfx) { float p3[3]; snd_player_pos(p3); snd_at(0xf2, p3, 0, snd_cockpit()); } }   /* message 0xd {14, "Shutting down"}; MECSHTD1 0xf2 at the mech (0x1001b060, positional, halved in the cockpit 0x1001b04e; DOSBox: at s, not on a heat shutdown 0x10016054) */
                }
                else if (g_world && g_sim_ok && A("TOGGLE_AUTOPILOT") && g_pilot) {
                    g_autopilot = !g_autopilot;   /* 0x1001a4b5: toggles whether or not there is a nav (none: no steering) */
                    hud_message_v2(g_autopilot ? "Autopilot engaged." : "Autopilot disengaged.", 65, g_autopilot ? 80 : 84);
                }
                else if (g_world && (A("INFRARED") || A("ENHANCED_VISION")) && g_pilot) {
                    int want = A("INFRARED") ? 1 : 2;
                    if (want == 1 && g_inst_lvl[INST_RADAR] > 0 && g_vision != 1) want = 0;   /* refused with the radar (instrument 0) damaged (0x10007a50) */
                    int was = g_vision;
                    if (want) g_vision = g_vision == want ? 0 : want;
                    if (g_vision == 1 && was != 1 && g_sfx) sfx_play(g_sfx, 0xb2, SFX_V64, 0.0f);   /* 0x10007a99: IREDMODE on engaging (volume 0x64, priority 0x50) */
                    if (g_vision == 1) hud_message_v2("Light amplification engaged.", 77, 80);
                    else if (g_vision == 2) hud_message_v2("Image enhancement engaged.", 76, 80);
                }
                else if (g_world && A("RADAR_MAP_TOGGLE")) {
                    g_satmap = !g_satmap;
                    /* 0x100034ea: on - message 0x12 "Satellite link established" (67); 0x10003597: off - in camera mode 1
                     * (DAT_1024e660, the external view) message 0x11 "External camera engaged" (66) again */
                    if (g_satmap) hud_message_v("Satellite link established.", 67);
                    else if (g_camode == 1) hud_message_v("External camera engaged.", 66);
                    /* both ways the satellite descriptor's sound (0x1024a6d0 +0xc on, +0x10 off: 0xe8 MECOMBEP) through
                     * 0x10032b60 at volume 0x64, centred (0x10003521 / 0x100035c0) */
                    if (g_sfx) sfx_play(g_sfx, 0xe8, SFX_V64, 0.0f);
                }
                else if (g_world && ((A("TOGGLE_REAR_VIEW") && ui_click()) || (A("TOGGLE_DOWN_VIEW") && ui_click()) || (A("TOGGLE_WEAPON_DISPLAY") && ui_click()))) {
                    int want = A("TOGGLE_REAR_VIEW") ? 1 : A("TOGGLE_DOWN_VIEW") ? 2 : 3;
                    g_vport = g_vport == want ? 0 : want;
                }
                else if (g_world && (A("TOGGLE_TARGET_DISPLAY") && ui_click())) { g_tgtdisp = (g_tgtdisp + 1) % 3; g_tgtdisp_off = g_tgtdisp == 0; }   /* 1 -> 2 -> 0 */
                else if (g_world && A("COCKPIT_RESET_ZOOM")) g_zoom = 1.0f;
                else if (g_world && g_sim_ok && A("PAUSE_GAME")) {   /* action 0x58: DAT_1024acd4 -> the pause timer */
                    g_paused = 1;
                    if (g_sfx) sfx_pause(g_sfx, 1);
                }
                else if (g_world && g_sim_ok && A("SELF_DESTRUCT")) {   /* 0x57: input +0x43, taken by 0x1001a180 in state 2 */
                    const combat_unit *pu2 = &g_sim.player_unit;
                    if (!pu2->destroyed && !pu2->shutdown && g_sd_at < 0 && !(g_sim.now < g_sim.player_online_at && !getenv("MW2_NO_STARTUP"))) {
                        hud_message_v("Self-destruct sequence initiated", 16);   /* message 4 */
                        if (g_edition == PORTCFG_ED_DOS) { player_eject(0); g_sim.player_unit.down_in_update = 1; }   /* DOS: at once, inside the unit's update (its case 4 at once: msim on_player_down) */
                        else g_sd_at = g_sim.now + 0x16a * 1000 / 182;            /* state 7: 0x16a ticks */
                    }
                }
                else if (g_world && g_sim_ok && A("EJECT")) player_eject(1);   /* 0x3b: 0x100174f0(c, 1) */
                else if (g_world && g_sim_ok && A("TOGGLE_AUTOEJECT")) {   /* 0x3c: only with the mech up (+0x15 & 0x20) */
                    const combat_unit *pu2 = &g_sim.player_unit;
                    if (!pu2->destroyed && !(g_sim.now < g_sim.player_online_at && !getenv("MW2_NO_STARTUP"))) {
                        combat_set_auto_eject(!combat_auto_eject());
                        hud_message_ex(combat_auto_eject() ? "Automatic ejection ON" : "Automatic ejection OFF", 0x16a, 0x32);
                    }
                }
                else if (g_world && A("MFD_CYCLE")) {
                    /* action 2 (0x10011700): the lower display's mode DAT_10250520 + 1, 6 -> 1 (1 damage, 2 heat / HTAL,
                     * 3 rear, 4 down, 5 weapon view; 0 off), then the click (sound 0xdc) with the mech up */
                    int mode = g_vport ? g_vport + 2 : g_htal ? 2 : g_dmg_off ? 0 : 1;
                    if (++mode == 6) mode = 1;
                    g_vport = mode >= 3 ? mode - 2 : 0;
                    g_htal = mode == 2;
                    g_dmg_off = 0;
                    ui_click();
                }
                else if (g_world && A("RESET_INPUTS")) {
                    /* action 1 (0x10037880 case 1 / DOS 0x46060 case 1, the same six): sets the sinks of torso_tilt_reset (0x10159e68),
                     * torso_pan_reset (0x10159e6c), pilot_tilt_reset (0x10159e32), pilot_pan_reset (0x10159e35),
                     * eyepoint_tilt_reset (0x10159e3c) and eyepoint_pan_reset (0x10159e3f) - control table 0x10256540 -
                     * so the next input pass is Keypad 5's (the INPUT.MAP binds the torso and pilot resets to it): the
                     * demands to 0 (the torso eases back through its servos), the mouse recentred, the view straight */
                    g_reset_inputs = 1; g_reset_tilt_eye = 1;
                }
                else if (g_world && g_sim_ok && A("ORDINANCE_VIEW")) {
                    /* action 0xe: 0x10046a00 - the player's newest projectile, if a missile still in flight, becomes the
                     * followed one (else none: the weapon view's too) - then camera mode 3 (0x1002df20(3)); its first
                     * frame keeps the mode it came from and sets the zoom to 2.0 (0x1002e210). No other camera action
                     * is in GAMEKEY.MAP (the engine's free / track modes are not reachable from the keys) */
                    if (follow_newest()) {
                        if (g_camode != 3) { g_cam_prev = g_camode; g_cam_zoom = 2.0f; }
                        g_camode = 3;
                    }
                }
                else if (g_world && g_sim_ok && A("TOGGLE_MASC")) {
                    /* engine 0x1001aef0: not fitted -> the message (0x16a ticks at 0x32); else MASC off: MECBMAOF 0xca and
                     * 0x10031870(0x1f, 0) - suffix 0 posts nothing (0x10031872); MASC on: MECBMAON 0xcb and "MASC" (61)
                     * "engaged" (80). (The port had the two sounds swapped and said "MASC disengaged") */
                    if (!combat_has_masc(&g_sim.player_unit)) hud_message_v("Not equipped with MASC.", 0);
                    else { g_masc = !g_masc; if (g_sfx) sfx_play(g_sfx, g_masc ? 0xcb : 0xca, SFX_V64, 0.0f);   /* 0x1001af1e / 0x1001af4e: volume 0x64, centred */ if (g_masc) hud_message_v2("MASC engaged.", 61, 80); }
                }
                else if (g_world && g_sim_ok && A("JETTISON_AMMO")) {
                    /* engine 0x10044220: the selected weapon's ammunition, if it has any: sound 0xb3, the message */
                    if (combat_jettison(&g_sim.player_unit, g_sel_weapon)) {
                        if (g_sfx) sfx_play(g_sfx, 0xb3, SFX_V64, 0.0f);   /* 0x10044271 / 0x100442ae: volume 0x64 */
                        hud_message_v("Ammo for current weapon jettisoned.", 0);
                    }
                }   /* engine: zoom_factor_reset -> 1 (DOS Ctrl+Z) */
                else if (g_world && A("FEET_TO_TORSO")) g_feet_to_torso = 1;
                else if (g_world && g_sim_ok && A("INSPECT_TARGET")) inspect_now();
                else if (g_world && A("TOGGLE_GROUP_FIRE")) {
                    /* \ : chain fire <-> group fire - engine 0x100437d4 (DAT_1025a668 = chain): 0x10031870(7 or 6, 1),
                     * message "Group fire" (voice 19) / "Chain fire" (18) with suffix 1 "engaged" (80) - both ways;
                     * DOSBox YELLSCN1 (no sound): "Group fire engaged." then "Chain fire engaged." */
                    g_group_fire = !g_group_fire;
                    hud_message_v2(g_group_fire ? "Group fire engaged." : "Chain fire engaged.", g_group_fire ? 19 : 18, 80);
                }
                else if (g_world && A("NEXT_WEAPON_GROUP")) g_cur_group = (g_cur_group + 1) % 3;
                else if (g_world && A("COCKPIT_VIEW") && !(g_sim_ok && g_sim.player_unit.destroyed)) {   /* action 9: not once dead (+0x14 & 2) */
                    /* 0x10037a96 (action 9): in the cockpit (mode 0) with the satellite map off -> 0x1002df20(1), mode 1: the
                     * external orbit (0x1002e280 enters with message 0x11 "External camera engaged" (66) and zoom 1.0);
                     * otherwise the satellite map off (0x10003ff0), mode 0 and the reset sinks pilot_tilt_reset,
                     * pilot_pan_reset, zoom_factor_reset (0x10159e32 / 35 / 48) */
                    if (g_camode == 0 && g_cockpit && !g_satmap) g_camode = 1;
                    else {
                        if (g_satmap) { g_satmap = 0; if (g_sfx) sfx_play(g_sfx, 0xe8, SFX_V64, 0.0f); }   /* 0x10004000: as F3 off (MECOMBEP) */
                        g_camode = 0; g_cockpit = 1;
                        g_eye_pan = g_eye_tilt = 0; g_zoom = 1.0f;
                    }
                }
                else if (g_world && dbg_keys() && k == SDLK_g && !(e.key.keysym.mod & (KMOD_CTRL | KMOD_ALT))) { g_pilot = !g_pilot; SDL_SetRelativeMouseMode(g_pilot ? SDL_TRUE : SDL_FALSE); }   /* port: free camera (MW2_DEBUG_KEYS=1 only) */
                else if (g_world && (A("THROTTLE_STOP") || A("THROTTLE_2") || A("THROTTLE_3") || A("THROTTLE_4") || A("THROTTLE_5") ||
                                     A("THROTTLE_6") || A("THROTTLE_7") || A("THROTTLE_8") || A("THROTTLE_9") || A("THROTTLE_FULL"))) {
                    static const char *const TH[10] = {"THROTTLE_STOP", "THROTTLE_2", "THROTTLE_3", "THROTTLE_4", "THROTTLE_5",
                                                       "THROTTLE_6", "THROTTLE_7", "THROTTLE_8", "THROTTLE_9", "THROTTLE_FULL"};
                    int q;
                    for (q = 0; q < 10; q++) if (A(TH[q])) g_throttle = (g_reverse ? -1.0f : 1.0f) * msim_throttle_preset(q);   /* action 0x1a + q: DOS 0x4684c q x 113 / 0x400 of full (the 3Dfx DLL q / 576 = q/9); DOS bar: THROTTLE_6 = 5/9 */
                }
                else if (g_world && A("REVERSE_DIRECTION")) {   /* action 0x3f (0x10037d36): toggles the reverse flag, input +0x2f -
                                                                * the drive runs backwards at half (0x1001a180, x -0.5), the reverse
                                                                * walk (0x1000b3a0), the bar fills down (0x1001c5b0); the throttle
                                                                * setting stays (here its sign) */
                    g_reverse = !g_reverse; g_throttle = -g_throttle;
                }
                else if (g_world && A("OVERRIDE_SHUTDOWN") && g_sim_ok) combat_override(&g_sim.player_unit);
                else if ((k == SDLK_f && !g_world) || (k == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                    Uint32 fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                    SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                } else if (k == SDLK_TAB && !g_world) { mi = (mi + 1) % COUNT(MECHS); rec = MECHS[mi]; reload = 1; }   /* viewer only: in a mission Tab reloaded it (user 2026-10-21: Cmd+Tab restarted the mission) */
                else if (k == SDLK_n && !g_world) { ni = (ni + 1) % COUNT(MISSIONS); mission = MISSIONS[ni]; reload = 1; }
                else if (k == SDLK_c && !g_world) { camo = (camo + 1) % 8; reload = 1; }
                else if (k == SDLK_i && !g_world) { clan = (clan + 1) % 6; reload = 1; }
                else if (k == SDLK_b && !g_world) v.bilinear = !v.bilinear;
                else if (k == SDLK_SPACE && !g_world) g_walk = !g_walk;
                else if (k == SDLK_p && !g_world) g_patrol = !g_patrol;
                else if (k == SDLK_m && !g_world) { msaa = !msaa; if (msaa) glEnable(GL_MULTISAMPLE); else glDisable(GL_MULTISAMPLE); }
                else if (k == SDLK_RIGHTBRACKET && (!g_world || dbg_keys())) v.fog_density = v.fog_density <= 0 ? 0.0001f : v.fog_density * 1.25f;
                else if (k == SDLK_LEFTBRACKET && (!g_world || dbg_keys())) v.fog_density = v.fog_density < 0.00011f ? 0 : v.fog_density / 1.25f;
            }
        }
        if (reload) load(r, ma, ta, ati, rec, mission, camo, clan);
        if (g_have_anim && g_walk && !g_world) {
            /* walk sequence 0 on a loop. Key duration is provisional: ~6 keys/s scaled by
             * the mech's TSK rate (70 = Timber Wolf); the engine ties it to ground speed. */
            int first = 0, count = 12;
            anim_sequence(&g_anim, 0, &first, &count);
            mech3d_pose(&g_mech, &g_anim, (float)first + fmodf((float)SDL_GetTicks() / 1000.0f * g_kps, (float)count));
            glr_set_mech(r, &g_mech, (const uint8_t (*)[3])g_scene.palette, g_scene.slot, g_camo, g_clan);
        }
        if (g_world && (g_actors || g_sim_ok)) {   /* (a mission with no units still has its player: PROVSCN1 launched alone) */
            /* actors: animate (walking only while P is on), move along their heading */
            static Uint32 aprev;
            Uint32 now = SDL_GetTicks();
            float dt = aprev ? (float)(now - aprev) / 1000.0f : 0;
            if (g_auto_on) dt = g_auto_dt;
            else {   /* the game speed (mw2port.cfg speed=, default 0.85): the original ran slower than real time on its
                      * period hardware (a Pentium MMX), and the port's pace felt quick */
                static float speed = -1;
                if (speed < 0) {
                    portcfg pc;
                    portcfg_load(NULL, &pc);
                    speed = getenv("MW2_SPEED") ? (float)atof(getenv("MW2_SPEED")) : pc.speed;
                    if (speed < 0.5f || speed > 1.5f) speed = 1.0f;
                }
                dt *= speed;
            }
            if (g_im_ok) {   /* the menu controls (GIDDI\<driver>.STD, or INPUT.MAP): keys for the MAIN MENU (im_menu_key) */
                const Uint8 *mks = test_ks();   /* TEST hook: + MW2_HOLD */
                SDL_Keycode mk;
                if (g_im_menu_mouse) g_im_dev.mouse_buttons = SDL_GetMouseState(NULL, NULL);
                mk = im_menu_key(&g_im, mks, g_mm_depth > 0, (int)((SDL_GetTicks() - g_mm_timer) * 182u / 1000u), &g_mm_prev_en, &g_mm_prev_ab);
                if (im_held(&g_im, "menu_item_reset", mks)) im_menu_recentre(&g_im, "menu_item");
                if (im_held(&g_im, "menu_value_reset", mks)) im_menu_recentre(&g_im, "menu_value");
                if (mk) {   /* the timer reset, menu_item_reset / menu_value_reset set (0x10065bfd / DOS 0x17e6d) */
                    g_mm_timer = SDL_GetTicks();
                    im_menu_recentre(&g_im, "menu_item"); im_menu_recentre(&g_im, "menu_value");
                    mm_key(mk, 0);
                }
            }
            if (g_mm_depth || g_paused) dt = 0;   /* the MAIN MENU (and PAUSE_GAME) pauses the game (WinMain 0x10009770: "pause timer TRUE") */
            mech3d all;
            int i;
            aprev = now;
            (void)i;
            if (g_sim_ok) g_sim.player_nav = g_nav_sel;   /* Disengage goes to the player's selected nav point */
            if (g_sim_ok && getenv("MW2_KILL_ACTOR")) {   /* tests: "k,secs" - stand 60 m from actor k facing it; destroy it at secs */
                static int placed;
                int ka = atoi(getenv("MW2_KILL_ACTOR"));
                const char *kc = strchr(getenv("MW2_KILL_ACTOR"), ',');
                if (ka >= 0 && ka < g_actor_count && g_sim.armed[ka]) {
                    if (!placed) {
                        placed = 1;
                        g_pl.origin[0] = g_actors[ka].mech.origin[0];
                        g_pl.origin[2] = g_actors[ka].mech.origin[2] - (getenv("MW2_TEST_STANDOFF") ? atoi(getenv("MW2_TEST_STANDOFF")) : 6000);   /* TEST hook: distance (cm) */
                        g_pl.heading = 0;
                        g_sim.player[0] = (float)g_pl.origin[0]; g_sim.player[2] = (float)g_pl.origin[2];
                    }
                    if (kc && g_sim.now >= (int32_t)(atof(kc + 1) * 1000) && !g_sim.units[ka].destroyed) g_sim.units[ka].destroyed = 1;
                }
            }
            if (g_sim_ok && getenv("MW2_TEST_STRAFE")) {   /* TEST ONLY: "k,cm_per_s,t0,t1" - actor k slides sideways (world +x)
                                                             * at cm_per_s from t0 to t1 s (with MW2_TEST_HOLD_ACTOR: no own moves) */
                static int32_t x0;
                static int have;
                int ka = atoi(getenv("MW2_TEST_STRAFE"));
                float v = 0, t0 = 0, t1 = 1e9f, tn = (float)g_sim.now / 1000.0f;
                sscanf(getenv("MW2_TEST_STRAFE"), "%*d,%f,%f,%f", &v, &t0, &t1);
                if (ka >= 0 && ka < g_actor_count && g_sim.armed[ka] && !g_sim.units[ka].destroyed) {
                    if (!have || tn < t0) { have = 1; x0 = g_actors[ka].mech.origin[0]; }
                    else g_actors[ka].mech.origin[0] = x0 + (int32_t)lrintf(v * ((tn < t1 ? tn : t1) - t0));
                }
            }
            if (g_sim_ok && getenv("MW2_MISSILE_TRACE")) {   /* TEST ONLY: the player's missiles in flight (table types 3 / 4) */
                int q;
                for (q = 0; q < g_sim.shot_count; q++) {
                    const msim_shot *ms = &g_sim.shots[q];
                    int kd = combat_weapon_kind(ms->weapon);
                    if (ms->owner == -1 && (kd == 3 || kd == 4) && ms->life > 0 && ms->y >= 0)
                        fprintf(stderr, "missile %.3f id %u w %d at %.0f %.0f %.0f v %.1f %.1f %.1f homing %d\n", g_sim.now / 1000.0, ms->id, ms->weapon,
                                ms->x, ms->y, ms->z, ms->vx, ms->vy, ms->vz, ms->homing);
                }
                if (g_tgt_sel >= 0 && g_tgt_sel < g_actor_count)
                    fprintf(stderr, "mtarget %.3f %d at %d %.0f %d destroyed %d lock %#x\n", g_sim.now / 1000.0, g_tgt_sel, g_actors[g_tgt_sel].mech.origin[0],
                            g_sim.units[g_tgt_sel].y, g_actors[g_tgt_sel].mech.origin[2], g_sim.units[g_tgt_sel].destroyed, g_sim.player_lock);
            }
            if (g_sim_ok && getenv("MW2_HIT_BUILDINGS") && g_sim.now >= 1000 && !g_sim.buildings_destroyed) {   /* tests */
                int b3;
                for (b3 = 0; b3 < g_sim.bld_count; b3++) msim_damage_building(&g_sim, b3, 50.0f);
            }
            if (g_sim_ok && getenv("MW2_DEBRIS_TRACE")) {   /* tests */
                static int32_t last;
                if (g_sim.now - last >= 1000) { int q, n3 = 0, nh = 0; last = g_sim.now; for (q = 0; q < g_sim.debris_count; q++) n3 += g_sim.debris[q].kind == 3;
                    for (q = 0; q < g_mech.part_count; q++) nh += g_mech.parts[q].hidden;
                    fprintf(stderr, "debris %.1fs: %d pieces (%d world), %d hidden world parts, %d destroyed\n", g_sim.now / 1000.0, g_sim.debris_count, n3, nh, g_sim.buildings_destroyed);
                    if (atoi(getenv("MW2_DEBRIS_TRACE")) > 1) for (q = 0; q < g_sim.debris_count; q++) fprintf(stderr, "  piece %d kind %d at %.0f %.0f %.0f v %.1f %.1f %.1f moving %d ground %.0f\n", q, g_sim.debris[q].kind,
                        g_sim.debris[q].p[0], g_sim.debris[q].p[1], g_sim.debris[q].p[2], g_sim.debris[q].v[0], g_sim.debris[q].v[1], g_sim.debris[q].v[2], g_sim.debris[q].moving, msim_ground(&g_sim, g_sim.debris[q].p[0], g_sim.debris[q].p[2], g_sim.debris[q].p[1])); }
            }
            if (g_sim_ok && g_sim.world_dirty && g_world) {   /* a building was destroyed: its destroyed variant shows */
                glr_set_mech(r, &g_mech, (const uint8_t (*)[3])g_scene.palette, g_scene.slot, g_camo, g_clan);
                g_sim.world_dirty = 0;
            }
            if (g_auto_on && g_pl_ok) {   /* the held controls (a twist demand is discarded while the mech is down, as the keys') */
                if (g_auto_throttle >= -1.5f) { g_throttle = g_auto_throttle; g_reverse = g_throttle < 0; }   /* TEST hook: throttle -9 = the keys drive it */
                g_cockpit = g_auto_cockpit;
                if (g_auto_twist > -900.0f && !(g_sim_ok && (g_sim.player_unit.shutdown || (g_sim.now < g_sim.player_online_at && !getenv("MW2_NO_STARTUP"))))) g_pl.twist = g_auto_twist;   /* twist -999 = free */
            }
            g_sim.no_player_muzzle = VIEW_IN_COCKPIT;
            if (g_pl_ok && g_pilot && !g_sim.player_unit.destroyed) {
                /* Engine (3dfx MW2.DLL 0x1001a180 / 0x10041410): full throttle = walk MP x 105.49/64
                 * cm per 1/182 s tick = walk MP x 10.8 km/h; reverse at half; leg turn rate scales
                 * with cos(min(80, speed in km/h)) deg; FEET_TO_TORSO swings the legs at 55 deg/s.
                 * The throttle / torso keys and axes: the analog sinks below; base turn 90 deg/s (0x10247428). */
                const Uint8 *ks = test_ks();   /* TEST hook: + MW2_HOLD */
                /* start-up (engine 0x1001a180 states 0-1): until the player's mech is online (5.97-7.95 s) every tick clears
                 * the throttle and the leg turn - input is discarded, not held - and the weapons stay off */
                int starting = g_sim_ok && g_sim.now < g_sim.player_online_at && !getenv("MW2_NO_STARTUP");
                /* not up (engine 0x1001a180 LAB_1001aae9: controller state != 2 - starting or shut down): every tick the
                 * throttle input (+8), the twist demand c[2], the leg turn c[0xe], the drive c[10] and the pitch demand c[6]
                 * are cleared; the controls block runs only in state 2. DOSBox (GREESCN1, s at full speed): no leg turn or
                 * torso twist while down, the mech coasts to a stop at the throttle-off rate, a twisted / tilted torso
                 * swings back to the centre within ~0.5 s */
                int offline = starting || (g_sim_ok && g_sim.player_unit.shutdown);
                float heading_before = g_pl.heading;
                float top = g_pl_walk * (g_sim_ok ? g_sim.player_unit.speed_gain : 1.0f) * (g_masc ? 1.5f : 1.0f), kmh, turn;
                if (g_masc && g_sim_ok) {
                    /* engine 0x1001a180: MASC raises the target speed by half; while it runs and the mech moves forward,
                     * every 0xb5 ticks rand() % 60 == 12 is a malfunction (not for an invulnerable player, only while up -
                     * 0x1001a98f) - "MASC malfunction.", sound 0xc9, MASC off, and the heat (+0x98) rises by a quarter */
                    static float masc_t;
                    masc_t += dt * 182.0f;
                    if (g_pl_speed > 1.0f && masc_t >= 181.0f) {
                        masc_t = 0;
                        if (rand() % 60 == 12 && !g_sim.player_unit.invulnerable && !g_sim.player_unit.shutdown) {
                            g_masc = 0; if (g_sfx) sfx_play(g_sfx, 0xc9, SFX_V64, 0.0f);   /* 0x1001a9de: (0xc9, 0x64, 0x40, 5, 0x50) */ hud_message_v("MASC malfunction.", 0);
                            if (!g_sim.player_unit.no_heat) {   /* straight into the heat, not the frame's gain */
                                float add = g_sim.player_unit.heat * 0.25f;
                                g_sim.player_unit.heat += add; g_sim.player_unit.heat_mark += add;
                            }
                        }
                    }
                }
                /* the throttle and the torso as the engine's analog sinks (0x10039140 set-up, 0x1003a0a0 per frame,
                 * control table 0x10256540): each holds a value v in -1..1 (16.16) and gives min + (v + 1) / 2 x (max -
                 * min) - throttle 0..1024 (the integer: flags 0x10 -> >> 16), torso_pan -90..90 (then the mech's twist
                 * limit), torso_tilt -60..60 (positive down; then the port's +-30). Per frame: the reset key sets
                 * v to the initial value (the torso's: 0, centred) and recentres the mouse (the device's reset, +0x10);
                 * else the held _plus / _minus keys run the key ramp (0x10039ee0) at the table's shifts - throttle
                 * 6, torso_pan 3, torso_tilt 4 (key_ramp); only in a frame with no key acting, an axis (stick or mouse)
                 * whose reading has changed since the last such frame sets v to the reading (0x1003a282..0x1003a2c0: the axis
                 * never fights a held key, and a still axis leaves a key's setting alone). The ramp is frame-rate
                 * dependent in the engine (frame ticks x 3 << shift a frame, so the acceleration grows with the frame
                 * rate): the port runs it at the agreed 30 fps (throttle full in 0.5 s: 15 frames). DOSBox YELLSCN1 (the
                 * throttle bar = the setting eased half the gap a draw; ZMBV capture, game frames from the 70 Hz refreshes)
                 * follows the ramp at the measured frame lengths to 1 px at both 17 fps (cycles max: full in ~0.65 s)
                 * and 4 fps (cycles 20000: a 1 s hold -> 65 %), and the torso twist (shift 3) at 4 fps gives 1.4 / 4.4
                 * / 8.7 / 14.5 degrees a frame - measured 1 / 4 / 8 / 14.8; the old fit (35 % at 0.5 s, a = 2.76 / s^2)
                 * was the same ramp at that run's lower frame rate.
                 * Anything else that moves the throttle or the torso (presets, reverse, resets, FEET_TO_TORSO, power
                 * state, tests) re-seeds v from the value it left */
                {
                    static key_ramp_t sp, st, sth;
                    static float pan_out = 1e9f, tilt_out = 1e9f, thr_out = -1.0f, last_jp, last_jt, last_jth, last_mp, last_mt;   /* out: seed on the first frame */
                    float a;
                    int pr = 0, mp = 1, mt = 1, cp, ct, cth;
                    if (g_im_mouse) {   /* the mouse bound to the torso in INPUT.MAP (else the port's MOUSE.MAP reading) */
                        if (g_im_ok) { im_axis(&g_im, "torso_pan", ks, 2, &mp); im_axis(&g_im, "torso_tilt", ks, 2, &mt); }
                        else mp = mt = 0;
                    }
                    /* the twist / tilt set straight by something else (FEET_TO_TORSO sets c[2] = c[3], 0x1001a180; resets
                     * of the mission; tests): the demand goes with it */
                    if (fabsf(g_pl.twist - g_twist_srv) > 1e-4f) g_twist_dem = g_pl.twist;
                    if (fabsf(g_pitch - g_pitch_srv) > 1e-4f) g_pitch_dem = g_pitch;
                    if (fabsf(g_twist_dem - pan_out) > 1e-4f) sp.val = g_twist_dem / 90.0f;   /* moved by something else */
                    if (fabsf(g_pitch_dem - tilt_out) > 1e-4f) st.val = -g_pitch_dem / 60.0f;
                    if (fabsf(fabsf(g_throttle) - thr_out) > 1e-6f) sth.val = 2.0f * fabsf(g_throttle) - 1.0f;
                    /* the keys: torso_pan + Period / PgDn, - Comma / End; torso_tilt + Up (tilts the torso DOWN: the engine's
                     * torso_tilt grows downward - DOSBox YELLSCN1: Up held, the horizon rises and the reticle drops; and so
                     * "- mouse Up/Down" tilts up as the mouse comes back (down the desk) and "- joystick Up/Down" down as the
                     * stick goes forward), - Down; throttle + = / keypad +, - - / keypad - */
                    cp = H("torso_pan_plus") || H("torso_pan_minus") || H("torso_pan_reset") || g_reset_inputs;
                    if (H("torso_pan_reset") || g_reset_inputs) { sp.val = 0; sp.vel = 0; if (mp) g_im_dev.mouse_x = 0; }
                    else key_ramp(&sp, H("torso_pan_plus"), H("torso_pan_minus"), 3, 0, dt);
                    ct = H("torso_tilt_plus") || H("torso_tilt_minus") || H("torso_tilt_reset") || g_reset_inputs;
                    if (H("torso_tilt_reset") || g_reset_inputs) { st.val = 0; st.vel = 0; if (mt) g_im_dev.mouse_y = 0; }
                    else key_ramp(&st, H("torso_tilt_plus"), H("torso_tilt_minus"), 4, 0, dt);
                    cth = H("throttle_plus") || H("throttle_minus");
                    key_ramp(&sth, H("throttle_plus"), H("throttle_minus"), 6, 0, dt);
                    /* the axes, only in a frame with no key on the control, and only a reading that has changed */
                    if (!cp) {
                        a = g_im_ok ? im_axis(&g_im, "torso_pan", ks, 1, &pr) : 0;
                        if (pr == 2 && g_im_dev.joy) { if (a != last_jp) { sp.val = a; cp = 1; } last_jp = a; }
                        pr = 2;
                        a = g_im_mouse ? (g_im_ok ? im_axis(&g_im, "torso_pan", ks, 2, &pr) : 0) : (float)g_im_dev.mouse_x / 512.0f;
                        if (mp && pr == 2) { if (a != last_mp) { sp.val = a; cp = 1; } last_mp = a; }
                    }
                    if (!ct) {
                        a = g_im_ok ? im_axis(&g_im, "torso_tilt", ks, 1, &pr) : 0;
                        if (pr == 2 && g_im_dev.joy) { if (a != last_jt) { st.val = a; ct = 1; } last_jt = a; }
                        pr = 2;
                        a = g_im_mouse ? (g_im_ok ? im_axis(&g_im, "torso_tilt", ks, 2, &pr) : 0) : -(float)g_im_dev.mouse_y / 512.0f;
                        if (mt && pr == 2) { if (a != last_mt) { st.val = a; ct = 1; } last_mt = a; }
                    }
                    if (!cth) {   /* a throttle axis ("- fltstck Throttle": forward +1 = 1024, back -1 = 0, the centre half) */
                        a = g_im_ok ? im_axis(&g_im, "throttle", ks, 1, &pr) : 0;
                        if (pr == 2 && g_im_dev.joy) { if (a != last_jth) { sth.val = a; cth = 1; } last_jth = a; }
                    }
                    if (cp) { sp.val = sp.val > 1 ? 1 : sp.val < -1 ? -1 : sp.val; g_twist_dem = sp.val * 90.0f; }   /* limits: below */
                    if (ct) { st.val = st.val > 1 ? 1 : st.val < -1 ? -1 : st.val; g_pitch_dem = -st.val * 60.0f; }   /* g_pitch: up positive */
                    if (cth) {   /* the integer floor(0x400 x (v + 1) / 2); the sign the reverse flag's */
                        float mg;
                        sth.val = sth.val > 1 ? 1 : sth.val < -1 ? -1 : sth.val;
                        mg = floorf(512.0f * (sth.val + 1.0f)) / 1024.0f;
                        g_throttle = g_reverse ? -mg : mg;
                    }
                    /* past the mech's twist limit the engine clamps the demand and raises torso_pan_set (input +0x1b,
                     * 0x1001aaaf), which re-seeds the sink from the clamped value with its velocity zeroed (0x10039ee0
                     * set path): no travel is stored beyond the limit. The port's +-30 tilt the same way */
                    if (g_twist_limit < 360.0f && fabsf(g_twist_dem) > g_twist_limit) {
                        g_twist_dem = g_twist_dem > 0 ? g_twist_limit : -g_twist_limit;
                        sp.val = g_twist_dem / 90.0f; sp.vel = 0;
                    }
                    if (fabsf(g_pitch_dem) > 30.0f) { g_pitch_dem = g_pitch_dem > 0 ? 30.0f : -30.0f; st.val = -g_pitch_dem / 60.0f; st.vel = 0; }
                    /* not up: the demands c[2] / c[6] cleared every tick (0x1001a180 LAB_1001aae9) - the torso swings back */
                    if (offline) g_twist_dem = g_pitch_dem = 0;
                    pan_out = g_twist_dem; tilt_out = g_pitch_dem; thr_out = fabsf(g_throttle);
                    /* the torso servos (0x100190d0: controller +4 twist, +0x14 pitch, T = 0.2 s x 181 for the player (0.6 s
                     * for the AI's twist); stepped every frame in 0x10019310 by 0x1003b7e0: value += frame ticks x (demand -
                     * value) / T, never past it - a frame of T ticks or more lands on the demand). The view, the aim, the
                     * compass band and the radar cone follow the value c[3] / c[7]. DOS (0x266e0 / 0x49b80): the same servo in
                     * 16.16 degrees, T = 36 ticks, truncated (a negligible 1/65536 degree). DOSBox YELLSCN1 (ZMBV, ~17 fps):
                     * the compass band goes on easing 12.8 -> 15.6 degrees for ~0.4 s after Period is released, and a held
                     * Keypad 5 eases it 13.8 -> 0 in ~0.7 s at 0.71 of the gap a 10.4-tick frame (1 - 10.4 / 36.2) */
                    {
                        float k = dt * 182.0f / (msim_dos_int ? 36.0f : 36.2f);
                        if (k > 1.0f) k = 1.0f;
                        g_pl.twist += (g_twist_dem - g_pl.twist) * k;
                        g_pitch += (g_pitch_dem - g_pitch) * k;
                        g_twist_srv = g_pl.twist; g_pitch_srv = g_pitch;
                    }
                    if (getenv("MW2_SINK_TRACE")) printf("SINK t=%.3f pan=%.4f tilt=%.4f thr=%.4f twist=%.2f pitch=%.2f dtw=%.2f dpi=%.2f mouse=%d,%d\n", g_sim.now / 1000.0, (double)sp.val, (double)st.val, (double)sth.val,
                                                         (double)g_pl.twist, (double)g_pitch, (double)g_twist_dem, (double)g_pitch_dem, g_im_dev.mouse_x, g_im_dev.mouse_y);   /* tests */
                }
                {   /* the torso servo (engine 0x1001a180 end): sound 0x13c is TORSLOOP, a 0.29 s loop - held while the torso
                     * turns (the twist c[3] more than 2 degrees (0x102473d4) from the demand c[2]), stopped when it stops;
                     * restarting it every 0.29 s buzzed */
                    static float prev_twist, still;
                    static unsigned char *loop_pcm;
                    static int loop_len, playing;
                    static float prev_heading;
                    static int primed;
                    float dh = g_pl.heading - prev_heading, rate;
                    if (!primed) { prev_heading = g_pl.heading; prev_twist = g_pl.twist; dh = 0; primed = 1; }   /* the first frame: no turn (the start heading had played TORSLOOP at t = 0) */
                    while (dh > 180.0f) dh -= 360.0f;
                    while (dh < -180.0f) dh += 360.0f;
                    /* the torso lagging its demand by more than 2 degrees, or twisting, or the legs turning the whole mech
                     * (ASSUMED: the same gyro loop) */
                    rate = dt > 0 ? (fabsf(g_pl.twist - prev_twist) + fabsf(dh)) / dt : 0;
                    if (fabsf(g_twist_dem - g_pl.twist) > 2.0f && rate <= 8.0f) rate = 8.5f;
                    prev_twist = g_pl.twist; prev_heading = g_pl.heading;
                    if (!loop_pcm && g_ma) loop_len = sfx_decode(g_ma, 0x13c, &loop_pcm);
                    still = rate > 8.0f ? 0 : still + dt;
                    if (g_sfx && loop_pcm && loop_len > 0) {
                        /* 0x1001ad6d: only in the cockpit view (DAT_1024e678), at the mech, halved (0x1001adb2): 0.4 */
                        if (rate > 8.0f && !playing && VIEW_IN_COCKPIT) { sfx_play_pcm(g_sfx, loop_pcm, loop_len, 11025, 0.4f, 1); playing = 1; }
                        else if (playing && (still > 0.12f || !VIEW_IN_COCKPIT)) { sfx_stop_pcm(g_sfx, loop_pcm); playing = 0; }
                    }
                }
                {   /* analog controls (INPUT.MAP axis blocks): a stick sets the position over the control's range (control
                     * table 0x10256540: torso_pan -90..90, then the mech's twist limit; torso_tilt -60..60, then the port's
                     * +-30; pilot_pan -70..70; pilot_tilt -60..60), shaped as the joystick driver does (im_stick_shape);
                     * the throttle and the torso (stick and mouse) are the sinks above */
                    int pr = 0;
                    float a;
                    a = g_im_ok ? im_axis(&g_im, "pilot_pan", ks, 1, &pr) : 0;
                    if (pr && g_im_dev.joy) g_eye_pan = a * 70.0f;
                    a = g_im_ok ? im_axis(&g_im, "pilot_tilt", ks, 1, &pr) : 0;
                    if (pr && g_im_dev.joy) g_eye_tilt = a * 60.0f;
                }
                /* the pilot's view: pan / tilt keys, glances (held: pilot pan -70 / +70, tilt 50 up / 40 down, 0x1002e640), recentre, zoom */
                if (getenv("MW2_EYE_PAN")) g_eye_pan = (float)atof(getenv("MW2_EYE_PAN"));   /* tests */
                /* the engine's key-driven controls (0x10039ee0): while a _plus / _minus key is held the control's velocity
                 * grows at a constant acceleration (3 << shift per tick, velocity capped at half scale per tick) and the
                 * value (-1..1) moves by it; the velocity drops to 0 on release. pilot_pan spans -70..70, pilot_tilt
                 * -60..60 (control table 0x10256540) and keeps its value after release (DOS capture); shift 5 (key_ramp) */
                {
                    static key_ramp_t rp, rt;
                    int pm = H("pilot_pan_minus"), pp = H("pilot_pan_plus"), tm = H("pilot_tilt_minus"), tp = H("pilot_tilt_plus");
                    rp.val = g_eye_pan / 70.0f; rt.val = g_eye_tilt / 60.0f;   /* the value may be set elsewhere (glances, resets) */
                    key_ramp(&rp, pp, pm, 5, 0, dt);
                    key_ramp(&rt, tp, tm, 5, 0, dt);
                    g_eye_pan = rp.val * 70.0f; g_eye_tilt = rt.val * 60.0f;
                }
                if (H("pilot_pan_reset") || H("pilot_tilt_reset") || g_reset_inputs) g_eye_pan = g_eye_tilt = 0;
                g_reset_inputs = 0;   /* RESET_INPUTS: one input pass */
                {   /* zoom_factor (control table: 1..4, camera +0x18 -> the projection's zoom): the same key ramp at
                     * shift 6 (16 /s^2 on the -1..1 control value at 30 fps: a 0.12 s tap 1.00 -> 1.17; DOS: 1.17, then 1.39;
                     * 1.5 s -> 4; Shift+Z 0.25 s 3.97 -> 3.47, measured from the radar's view cone); COCKPIT_RESET_ZOOM
                     * (Ctrl+Z) -> 1 */
                    static key_ramp_t rz;
                    int zp = H("zoom_factor_plus"), zm = H("zoom_factor_minus");
                    rz.val = (g_zoom - 1.0f) / 1.5f - 1.0f;
                    key_ramp(&rz, zp, zm, 6, 0, dt);
                    g_zoom = 1.0f + (rz.val + 1.0f) * 1.5f;
                    if (getenv("MW2_RAMP_TRACE")) printf("RAMP t=%.3f pan=%.2f tilt=%.2f zoom=%.3f\n", g_sim.now / 1000.0, (double)g_eye_pan, (double)g_eye_tilt, (double)g_zoom);   /* tests */
                }
                if (g_eye_pan > 180) g_eye_pan = 180;
                if (g_eye_pan < -180) g_eye_pan = -180;
                if (g_eye_tilt > 45) g_eye_tilt = 45;
                if (g_eye_tilt < -45) g_eye_tilt = -45;
                if (g_twist_limit < 360.0f) {   /* the same limits as the mouse */
                    if (g_pl.twist > g_twist_limit) g_pl.twist = g_twist_limit;
                    if (g_pl.twist < -g_twist_limit) g_pl.twist = -g_twist_limit;
                }
                if (g_pitch > 30) g_pitch = 30;
                if (g_pitch < -30) g_pitch = -30;
                if (g_throttle > 1) g_throttle = 1;
                if (g_throttle < -1) g_throttle = -1;
                kmh = fabsf(g_spd_srv) * 0.036f;   /* c[0xb] x 6.516 (0x1001a180) */
                /* engine turn = input/1024 (0x10247438) x 90 (0x10247428) x cos(min(80, km/h)) (0x1001a180); input = the legs_pan_delta
                 * sink's value -1..1 x its range 1024 (control table 0x10256540) */
                turn = 90.0f * cosf((kmh < 80.0f ? kmh : 80.0f) * 3.14159265f / 180.0f);
                if (g_sim_ok) {   /* the jets held (input +0x1d, state 2, a tank: c[0x30] >= 0): the turn is input / 1024 x 90
                                   * x 2 (0x102473d4) with no speed term while the tank holds fuel after this frame's burn,
                                   * else 0 - an empty tank with the jets held does not turn (0x1001a180: bVar3 -> local_2c) */
                    const combat_unit *pu = &g_sim.player_unit;
                    int jh = (H("jumpjet_enabled") || (g_auto_on && g_auto_jet)) && !((g_sim.now < g_sim.player_online_at && !getenv("MW2_NO_STARTUP")) || pu->shutdown);
                    if (jh && pu->jets > 0 && !pu->no_jump && !pu->destroyed && pu->jet_fuel >= 0)
                        turn = pu->jet_fuel - dt * 182.0f > 0 ? 180.0f : 0.0f;
                }
                g_pl_legs_turn = 0;
                {   /* a manual turn cancels the autopilot, announced as the key does (engine 0x1001a480..0x1001a4fc): in
                     * state 2, a leg-turn input (|input +0x10| >= 1e-7, 0x102473bc) or FEET_TO_TORSO (DAT_1025051c, key
                     * action 0x11) with the autopilot on (+0xbc) raises the toggle request (+0x42) the TOGGLE_AUTOPILOT key
                     * raises; it turns the autopilot off and, for the player, posts message 0x1d with suffix 2
                     * (0x10031870): "Autopilot" (voice 65, table 0x1024f140) "disengaged" (voice 84, table 0x1024edb8) */
                    int prt = 0;
                    float axt = g_im_ok ? im_axis(&g_im, "legs_pan_delta", ks, 1, &prt) : 0;
                    if (g_autopilot && !offline && (H("legs_pan_minus") || H("legs_pan_plus") || (prt && fabsf(axt) >= 1e-7f) || g_feet_to_torso)) {
                        g_autopilot = 0;
                        hud_message_v2("Autopilot disengaged.", 65, 84);
                    }
                }
                if (!g_autopilot) {   /* 0x1001a180: autopilot off -> input +0xc = the pilot's +0x10. +0x17a is written only by
                                       * the autopilot (0x24ca0) and the unit record comes from a pool, not cleared: its value
                                       * at engagement is taken as the pilot's throttle (ASSUMED - 0 would stop the mech at
                                       * the first wall follow) */
                    g_ap_turn = 0; g_ap_avoiding = 0; g_ap_saved_thr = g_throttle;
                }
                if (g_autopilot) {   /* engine 0x10015c00 (state +0xbc 1): steer to the nav in the selector (+0xdc kind 0x100,
                                      * visited or not); with none there (nothing, a unit / thing target, or 0x1000) first take
                                      * the first selectable nav that is not visited (flag 0x20), stepping 0x1001dac0 - all
                                      * visited: nothing selected (0x1000) and no steering, the autopilot stays on and the leg
                                      * input +0xc keeps its last value. Arrival (0x10015cb4): the nav's horizontal distance
                                      * (+0xc0, 0x1001cea0 -> 0x1001d970) under its radius (NAVP +0x10, no minimum): flag 0x20
                                      * and MECNAVPT 0xe7 (0x10032b60(0xe7, 0x64, 0x40, 5, 0x50)) if not yet visited, then the
                                      * selector steps to the next selectable nav (0x1001dac0(0) step 1, visited or not,
                                      * wrapping to the first); only when that wraps to a LOWER index (0x10015d4d: past the
                                      * last nav) is the autopilot off and the throttle 0 (+0x4c +8 = 0, +0x24 = 1), silently -
                                      * otherwise it walks on to the next nav without stopping. DOSBox TNJ1 (a at 13 s, full
                                      * throttle): Nav Alpha passed at 150 kph, Nav Beta taken at once, still 150 kph; around
                                      * Nav Beta (radius 30 m) it circled at 35-220 m for 50 s without arriving. The pilot's
                                      * throttle is otherwise left alone, also on a manual disengage. Then (DOS 0x24ca0) the
                                      * obstacle avoidance 0x2db80 (msim_player_avoid): while it steers the nav steering 0x229e0
                                      * is skipped and +0x17e set; when it lets go the throttle saved before (+0x17a) comes
                                      * back, and +0x17a keeps the pilot's throttle of each frame it is idle. Its DOS probe
                                      * sees an object only at contact (see msim dos_broad_reject): DOSBox TNJ4 walks straight
                                      * into S_1BQDAS, 12 / 31 kph for ~2 s, turned away to the left, follows the face and
                                      * resumes; terrain (mountains) never deflects it */
                    int nv = g_nav_sel >= 0 && g_nav_sel < g_sim.nav_count && g_nav_sel < 256 ? g_nav_sel : -1;
                    if (nv < 0) {   /* 0x10015c2f-0x10015c93 */
                        for (nv = 0; nv < g_sim.nav_count && nv < 256 && (g_nav_seen[nv] || !g_sim.navs[nv].selectable); nv++) {}
                        if (nv >= g_sim.nav_count || nv >= 256) nv = -1;
                        else g_nav_sel = nv;   /* the autopilot's nav goes into the selector */
                    }
                    if (nv >= 0) {   /* 0x10015c94: arrival at the selected nav (the player only) */
                        float ax = g_sim.navs[nv].x - (float)g_pl.origin[0], az = g_sim.navs[nv].z - (float)g_pl.origin[2];
                        if (sqrtf(ax * ax + az * az) < g_sim.navs[nv].radius) {
                            int nx = nav_step(nv, 1);
                            if (!g_nav_seen[nv]) { g_nav_seen[nv] = 1; g_sim.navs[nv].visited = 1; if (g_sfx) sfx_play(g_sfx, 0xe7, SFX_V64, 0.0f); }   /* 0x10015cb4: the one flag 0x20 */
                            if (nx < nv) { g_autopilot = 0; g_throttle = 0; g_nav_sel = -1; nv = -2; }   /* 0x10015d4f: no message call */
                            else g_nav_sel = nv = nx;
                        }
                    }
                    if (getenv("MW2_TEST_NAVAUTO")) { static int once; if (!once++) fprintf(stderr, "nav autopilot: nav %d\n", nv); }
                    if (nv == -1) {   /* nothing to steer to: +0xc held */
                        g_pl.heading += turn * (g_ap_turn / 1024.0f) * dt;
                        if (fabsf(g_ap_turn) >= 1e-7f) g_pl_legs_turn = 1;
                    }
                    else if (nv >= 0) {
                        float dx2 = g_sim.navs[nv].x - (float)g_pl.origin[0], dz2 = g_sim.navs[nv].z - (float)g_pl.origin[2];
                        float want = atan2f(dx2, dz2) * 180.0f / 3.14159265f, dh = want - g_pl.heading, c;
                        while (dh > 180) dh -= 360;
                        while (dh < -180) dh += 360;
                        int wrote = 0;
                        float at = 0, ath = 0;
                        if (msim_player_avoid(&g_sim, g_pl.heading, g_pl_speed, g_pl_across, fabsf(g_throttle), g_reverse,
                                              g_pl_walk * g_sim.player_unit.speed_gain <= 0.0f, dh, g_ap_saved_thr, &wrote, &at, &ath)) {
                            if (wrote) { g_ap_turn = at; g_throttle = ath; }   /* input +0xc / +8; held between the checks */
                            g_ap_avoiding = 1;
                        } else {   /* 0x229e0 (3Dfx 0x10013600; the DOS edition in whole degrees, ai_nav_turn) */
                            c = ai_nav_turn(dh);
                            g_ap_turn = c;
                            if (g_ap_avoiding) { g_ap_avoiding = 0; g_throttle = g_ap_saved_thr; }
                            g_ap_saved_thr = g_throttle;
                        }
                        if (getenv("MW2_AP_TRACE")) printf("APTRACE t=%.3f x=%d z=%d hd=%.1f err=%.1f avoid=%d side=%d turn=%.1f thr=%.3f\n", g_sim.now / 1000.0,
                                                           g_pl.origin[0], g_pl.origin[2], (double)g_pl.heading, (double)dh, g_ap_avoiding, g_sim.pav_side, (double)g_ap_turn, (double)g_throttle);   /* tests */
                        g_pl.heading += turn * (g_ap_turn / 1024.0f) * dt;
                        if (fabsf(g_ap_turn) >= 1e-7f) g_pl_legs_turn = 1;
                    }
                }
                {   /* keyboard turning builds up (legs_pan_delta's key ramp, see key_ramp) and stops on release */
                    static key_ramp_t rl;
                    key_ramp(&rl, H("legs_pan_plus"), H("legs_pan_minus"), 7, 1, dt);
                    g_pl.heading += turn * rl.val * dt;
                    if (rl.val != 0) g_pl_legs_turn = 1;
                }
                { int pr = 0; float a = g_im_ok ? im_axis(&g_im, "legs_pan_delta", ks, 1, &pr) : 0; if (pr) g_pl.heading += a * turn * dt; if (pr && fabsf(a) >= 1e-7f) g_pl_legs_turn = 1; }
                if (g_feet_to_torso) {
                    float step = 55.0f * dt, d = g_pl.twist;
                    if (fabsf(d) <= step) { g_pl.heading += d; g_pl.twist = 0; g_feet_to_torso = 0; }
                    else { float sgn = d > 0 ? 1.0f : -1.0f; g_pl.heading += sgn * step; g_pl.twist -= sgn * step; }
                }
                /* speed follows the throttle (reverse at half speed) */
                {   /* engine 0x1001a180: no throttle but turning -> 12.5% of full (the legs step round) */
                    int turning = H("legs_pan_minus") || H("legs_pan_plus");
                    if (g_throttle == 0 && turning) g_throttle_eff = 0.125f; else g_throttle_eff = g_throttle;
                }
                if (starting) g_autopilot = 0;
                if (offline) {
                    /* inputs discarded; the torso servos (above) run on toward the cleared demands - twist and pitch back
                     * to 0 at the player's T = 0.2 s (0x1003b7e0) - and nothing else moves them */
                    g_pl.heading = heading_before; g_throttle = 0; g_throttle_eff = 0;
                    g_pl.twist = g_twist_srv; g_pitch = g_pitch_srv;
                }
                if (getenv("MW2_POW_TRACE")) {   /* tests: power state, speed, torso, heading every 0.25 s */
                    static int32_t last_tr = -1000;
                    if (g_sim.now - last_tr >= 250) { last_tr = g_sim.now; fprintf(stderr, "pow %.2fs state %d speed %.0f twist %.1f pitch %.1f heading %.1f throttle %.2f heat %.1f pend %d ovr %d down %d dead %d\n", g_sim.now / 1000.0, g_pow, g_pl_speed, g_pl.twist, g_pitch, g_pl.heading, g_throttle,
                                                                                    g_sim.player_unit.heat, g_sim.player_unit.shutdown_pending, g_sim.player_unit.override, g_sim.player_unit.shutdown, g_sim.player_unit.destroyed); }
                }
                /* walking heat follows the throttle (engine c13, 0x1001aac8), not the speed (MASC not counted) */
                if (g_sim_ok) g_sim.player_unit.throttle_frac = fabsf(g_thr_srv) > 1.0f ? 1.0f : fabsf(g_thr_srv);
                /* the engine's throttle -> speed chain (msim_drive_step: throttle servo T 36.2 ticks, drive servo T 90.5 ticks,
                 * velocity approach 1/45 per tick; 0x1001a180 / 0x100190d0), the throttle signed here (reverse at half) */
                {   /* the frame's vertical move for the move test (as combat_jets below will make it) */
                    const Uint8 *ks = test_ks();
                    int jet = (H("jumpjet_enabled") || (g_auto_on && g_auto_jet)) && !(g_sim_ok && ((g_sim.now < g_sim.player_online_at && !getenv("MW2_NO_STARTUP")) || g_sim.player_unit.shutdown));
                    if (g_sim_ok) {   /* the no-lift flag (0x100196cc..0x10019711: |+0xc| >= 1e-7 or a directional jet held) */
                        int dj = H("jumpjet_fire_left") || H("jumpjet_fire_right") || H("jumpjet_fire_forward") || H("jumpjet_fire_backward");
                        g_sim.player_unit.jet_forward = jet && (g_pl_legs_turn || dj);
                    }
                    g_pl_dy_frame = g_sim_ok ? combat_jets_dy(&g_sim.player_unit, jet, dt) : 0.0f;
                    /* engine 0x10019310 (DOS 0x26990): the velocity approaches heading x c[0xb] only on the ground with the
                     * jets not firing (input +0x1d and fuel c[0x30] > 0); airborne or jetting it is left alone - momentum,
                     * whatever the throttle - except for the directional jets' thrust and drag (the jets block below) */
                    g_pl_jet_fire = g_sim_ok && combat_jets_firing(&g_sim.player_unit, jet, dt);
                    g_pl_free = g_sim_ok && (g_sim.player_unit.y > g_sim.player_unit.ground + 1.0f ||
                                             (jet && g_sim.player_unit.jets > 0 && g_sim.player_unit.jet_fuel > 0));
                    g_pl_dy_done = 0; g_pl_vy3 = 0; g_pl_imp3 = 0; g_pl_land3 = 0; g_pl_dt = dt;
                }
                pl_locomote(dt, top);
                {   /* the gait code (engine 0x1000b3a0) asks for a sequence by the current throttle c[0x13]; the key player
                     * (0x1000dec0, anim_player_step - the AI's the same, msim.c) walks the keys toward it. c[0x13] more than
                     * 1/32 of full over the stop level: gait 1 under 1/4, 2 under 3/4, else 3 (key time r x 1.5 / 1 / 0.75,
                     * 0x1000e600); sequence 0 walk, 1 run at gait 3, 2 reverse (gait at most 2). At or under it: stop (-1,
                     * gait 0) - the stride runs on to its end key and the feet come together in the standing pose at the
                     * plain key time; it had frozen mid-stride whenever the ground speed fell under 1 cm/s */
                    float frac = fabsf(g_thr_srv);   /* the current throttle c[0x13] (0x1000b3a0) */
                    int r0 = g_pl.anim_rate > 0 ? g_pl.anim_rate : 70;
                    int gait = 0, seq = -1, ev;
                    static int gs_st = -2;   /* the gait sound state (DAT_10207f00[unit], below) */
                    if (!g_pl_ap_ok) { anim_player_init(&g_pl_ap, r0); g_pl_ap_ok = 1; gs_st = -2; }   /* a new mission: standing, nothing playing (0x1000b5a0) */
                    if (frac > 1.0f / 32.0f) {
                        gait = frac < 0.25f ? 1 : frac < 0.75f ? 2 : 3;
                        seq = gait == 3 ? 1 : 0;
                        if (g_pl_ap.cur == 2 && gait > 2) gait = 2;
                        if (g_reverse) seq = 2;   /* the reverse flag (input +0x2f) asks for the reverse walk (0x1000b3a0) */
                    }
                    const int seq_req = seq;   /* +0x88 as asked this frame (-1 stop) */
                    /* the walk keys hold in the air (user report 2026-10-20): the physics 0x10019310 clears object +0x80 bit 0
                     * whenever the feet end the frame more than the centre height c[0x33] (MGEO int[0]) over the ground
                     * (0x10019df2; DOS 0x26990 the same), and the controller 0x1001a180 whenever the jets fire with fuel
                     * left (0x1001a688) - both every frame, after the gait code and before the key player (an object task,
                     * 0x100249b0 at the frame's end); the key player then only sets the bit again (0x1000dec0): the key,
                     * the pose and the carried offset hold, the frame's time is dropped, no key starts (no footstep), and it
                     * plays on from there once landed. The gait request and its sounds (0x1000b4e0) run on meanwhile.
                     * DOSBox HEIDSCN1 Nova at walk, J held 1 s: the legs held from the first jet frame to touch-down */
                    g_pl_anim_held = g_sim_ok && (g_pl_jet_fire || g_sim.player_unit.y + g_pl_dy_frame - g_sim.player_unit.ground > g_sim.player_centre_h);
                    if (g_pl_anim_held) anim_player_hold(&g_pl_ap);
                    ev = anim_player_step(&g_pl_anim, &g_pl_ap, seq, gait, r0, dt * 182.0f);
                    g_pl_t = g_pl_ap.t;
                    if (seq < 0) seq = 0;
                    if (getenv("MW2_STEP_TRACE")) { static int lq = -9; if (lq != g_pl_ap.cur) { lq = g_pl_ap.cur; fprintf(stderr, "seq %d key %d\n", g_pl_ap.cur, g_pl_ap.key); } }
                    if (ev & ANIM_EV_SLOW) g_drive_slow = 1;   /* a key flagged 0x10: 0x1001a180: T 90.5 from now on */
                    if ((ev & ANIM_EV_STEP) && g_sfx && !g_sim.player_unit.destroyed)
                    {   /* the player's own footstep (a key flagged 0x800: +0x80 bit 3): the mech record's ASND chunk {u16
                         * sound, name} (Mad Dog, Nova, Timber Wolf MECFSMED 0xd6; Jenner, Kit Fox MECLIGHT 0xe0) - engine
                         * 0x1000b3a0 plays object +0x90, set from it (+0x44) for the player; other mechs NONFOOT 0x103 */
                        static int fs = -1;
                        if (fs < 0) {
                            prj_record ar;
                            fs = 0xd6;
                            if (g_ma && prj_read_named(g_ma, "BWD", g_sim.player_skel, &ar) == PRJ_OK) {
                                bwd_chunk ch[256];
                                int nc = bwd_chunks(ar.data, ar.size, ch, 256), q;
                                for (q = 0; q < nc; q++) if (strcmp(ch[q].tag, "ASND") == 0 && ch[q].size >= 2) { fs = ch[q].data[0] | ch[q].data[1] << 8; break; }
                                prj_record_free(&ar);
                            }
                        }
                        /* positional at the mech (0x1000b4b9 -> 0x10032bf0, the camera minus the mech +0x50: mirrored):
                         * level 100 at ~0 m = 0.8, halved in the cockpit view (DAT_1024e678); about centred from the eye.
                         * DOSBox YELLSCN1 Mad Dog, cockpit, SB16: MECFSMED L 0.40-0.44 R 0.40-0.42; Timber Wolf, the
                         * external view ('c', the camera behind): L 0.79 R 0.79 */
                        float p3[3];
                        snd_player_pos(p3);
                        snd_at(fs, p3, 1, snd_cockpit());
                        if (getenv("MW2_STEP_TRACE")) fprintf(stderr, "footstep key %d sound %d\n", g_pl_ap.key, fs);
                    }
                    {   /* gait sounds (engine 0x1000b4e0, from the gait code 0x1000b3a0 every frame, the player only): by the
                         * sequence playing cur (+0x84, -1 none) and the one asked for req (+0x88: -1 stop, 0 walk, 1 run
                         * at gait 3, 2 reverse - input +0x2f) the sound table[5 + req + 4 x cur] (0x1024b2b8, +0x14):
                         *   cur -1: -1, STOP2WLK 302, 302, -1   cur 0: WLK2STOP 335, -1, -1, 335
                         *   cur  1: RUN2WLK 282, 282, WALK2RUN 328, 282   cur 2: REV2STOP 281, 281, 281, STOP2REV 301
                         * (req -1 / 0 / 1 / 2). The last state st (DAT_10207f00[unit], -2 at load 0x1000b300): req != cur
                         * plays the change sound once per cur (st = cur); req == cur plays the steady entry (WALK2RUN as
                         * the run starts, STOP2REV as the reverse starts; cur 0 after st 2 - from reverse into the walk -
                         * table[1] STOP2WLK) once (st = -2). At the mech (0x10032bf0, positional: the port's 0.8 as MECTURX2) */
                        static const short tbl[16] = {-1, 302, 302, -1, 335, -1, -1, 335, 282, 282, 328, 282, 281, 281, 281, 301};
                        int st = gs_st;
                        const int cur = g_pl_ap.cur, req = seq_req;
                        int go = 0, snd2 = -1;
                        if (cur != req) go = st != cur;
                        else go = st != -2;
                        if (go && cur >= -1 && cur <= 2 && req >= -1 && req <= 2) {
                            snd2 = cur == 0 && st == 2 ? tbl[1] : tbl[5 + req + 4 * cur];
                            gs_st = cur == req ? -2 : cur;
                        }
                        if (snd2 > 0 && g_sfx && !g_sim.player_unit.destroyed) { float p3[3]; snd_player_pos(p3); snd_at(snd2, p3, 1, snd_cockpit()); }   /* 0x1000b567 (0x1000b370: camera - mech, mirrored): halved in the cockpit (edx DAT_1024e678) */
                    }
                }
                g_pl.pose_off = getenv("MW2_NO_POSE_OFF") ? NULL : g_pl_ap.off;   /* the carried offset (anim_player_step; TEST: MW2_NO_POSE_OFF) */
                mech3d_pose(&g_pl, &g_pl_anim, g_pl_t);   /* g_pl_t: the absolute key time (anim_player) */
                if (g_plpit_ok) {
                    memcpy(g_plpit.origin, g_pl.origin, sizeof g_pl.origin);
                    g_plpit.heading = g_pl.heading; g_plpit.twist = g_pl.twist; g_plpit.pose_off = g_pl_ap.off;
                    mech3d_pose(&g_plpit, getenv("MW2_PIT_NOANIM") ? NULL : &g_pl_anim, g_pl_t);
                }
                if (g_sim_ok) {   /* the mounts as drawn (walk animation, torso twist) for the shots and jet flames */
                    memcpy(g_sim.pl_shown.mount_pos, g_pl.mount_pos, sizeof g_pl.mount_pos);
                    memcpy(g_sim.pl_shown.mount_from, g_pl.mount_from, sizeof g_pl.mount_from);
                    g_sim.pl_shown.mount_ok = g_pl.mount_ok;
                    g_sim.pl_shown_ok = 1;
                }
            } else if (g_pl_ok && g_pilot && g_sim_ok) {
                /* destroyed (state 4): the physics 0x10019310 (DOS 0x26990) returns at once for a unit in state 4 that is on
                 * the ground (object +0x78, set on every ground contact and by the wreck's landing 0x1000c4d0) - no move, no
                 * servo, the velocity left as it was: it stops dead where it died. Killed in the air it flies on, its velocity
                 * unchanged (no drive: the controller cleared it, LAB_1001aae9), falls, and freezes on touching down */
                if (g_sim.player_unit.y > g_sim.player_unit.ground + 1.0f) {
                    float h = g_pl.heading * 3.14159265f / 180.0f;
                    g_pl.origin[0] = (int32_t)lrintf((float)g_pl.origin[0] + sinf(h) * g_pl_speed * dt);
                    g_pl.origin[2] = (int32_t)lrintf((float)g_pl.origin[2] + cosf(h) * g_pl_speed * dt);
                } else g_pl_speed = 0, g_pl_across = 0;
            }
            if (g_pl_ok && g_pilot) {   /* the external orbit's controls (camera mode 1, 0x1002dc90 -> 0x1002e280) - read alive
                 * or destroyed: the engine's sinks are updated every frame (0x10039ee0 from the input pass, skipped only
                 * while paused or in a menu, 0x1003a3c0) and 0x1002dc90 passes distance, height and tilt to the orbit
                 * whatever the state; only the pan gives way to the self-turn once the end flag is set (0x1002dc90:
                 * DAT_1024c568 set and DAT_1024e680 clear - e680 is the network game's watch mode) */
                const Uint8 *ks = test_ks();   /* TEST hook: + MW2_HOLD */
                {   /* the "_delta" controls: track_distance (Shift+Z out / Z in), track_height (Ctrl + grey up / down),
                     * eyepoint_pan (Ctrl + grey left / right, and Home / PageUp - the glance keys, which mode 1 does not
                     * read) - shifts 6 / 6 / 4 (control table 0x10256540) */
                    static key_ramp_t ro[3];
                    key_ramp(&ro[0], H("track_distance_plus"), H("track_distance_minus"), 6, 1, dt);
                    key_ramp(&ro[1], H("track_height_plus"), H("track_height_minus"), 6, 1, dt);
                    key_ramp(&ro[2], H("eyepoint_pan_plus"), H("eyepoint_pan_minus"), 4, 1, dt);
                    g_orb_in[0] = ro[0].val; g_orb_in[1] = ro[1].val; g_orb_in[2] = ro[2].val;
                }
                {   /* eyepoint_tilt (control table entry 60: sink 0x10159e18, -16..16, shift 4, not a "_delta": it keeps its
                     * value) - INPUT.MAP eyepoint_tilt_plus / _minus / _reset (the installed map and the Cockpit Controls
                     * bind none), or a stick axis "eyepoint_tilt"; reset by eyepoint_tilt_reset and RESET_INPUTS (0x10037880
                     * case 1 sets 0x10159e3c) */
                    static key_ramp_t rt;
                    int pr = 0;
                    float a = g_im_ok ? im_axis(&g_im, "eyepoint_tilt", ks, 1, &pr) : 0;
                    rt.val = g_orb_tilt / 16.0f;
                    if (pr && g_im_dev.joy) rt.val = a;
                    key_ramp(&rt, H("eyepoint_tilt_plus"), H("eyepoint_tilt_minus"), 4, 0, dt);
                    if (H("eyepoint_tilt_reset") || g_reset_tilt_eye) { rt.val = 0; rt.vel = 0; }
                    g_reset_tilt_eye = 0;
                    g_orb_tilt = rt.val * 16.0f;
                }
            }
            if (g_sim_ok && g_pl_ok && g_pilot) {
                float d;
                /* jump jets (hold J): engine physics in combat_jets */
                g_sim.player_unit.ground = msim_ground(&g_sim, (float)g_pl.origin[0], (float)g_pl.origin[2], g_sim.player_unit.y + g_sim.player_centre_h);   /* 0x10010080 at the centre */
                {
                    const Uint8 *ks = test_ks();   /* TEST hook: + MW2_HOLD */
                    int jet = (H("jumpjet_enabled") || (g_auto_on && g_auto_jet)) && !(g_sim_ok && ((g_sim.now < g_sim.player_online_at && !getenv("MW2_NO_STARTUP")) || g_sim.player_unit.shutdown));
                    {   /* directional jets (INPUT.MAP jumpjet_fire_left / right / forward / backward, each also enabling
                         * the jets; engine physics 0x10019310, DOS 0x26990): while jetting and the legs are not turning, the
                         * FIRST held direction in the order left, right, forward, back pushes at the jets' thrust (+0xec)
                         * along it - forward (sin h, cos h), left (-cos h, sin h), right (cos h, -sin h); no lift meanwhile.
                         * The engine has ONE velocity vector (+0xf4 / +0xfc): the thrust and the drag act on all of it, the
                         * walking part included, so the terminal speed is L = 2.5 x the top walking speed whether the jets
                         * start from a standstill or from a run (DOSBox ORANSCN1 Nova 403 kph both ways, Summoner 336; the
                         * port had dragged only the jets' share and added the walk on top: 559 from FULL). Airborne or
                         * jetting the velocity is held otherwise (g_pl_free: no walking servo, a turn does not swing it);
                         * on the ground with the jets off it eases toward heading x c[0xb] at 1/45 per tick (0x102473b4),
                         * so the jets' share eases to zero - it does not stop dead on landing. */
                        float jvx = g_jet_vx, jvz = g_jet_vz;
                        int l2 = H("jumpjet_fire_left"), r2 = !l2 && H("jumpjet_fire_right"), f2 = !l2 && !r2 && H("jumpjet_fire_forward");
                        int b2 = !l2 && !r2 && !f2 && H("jumpjet_fire_backward");
                        int legs_turning = g_pl_legs_turn;   /* input +0xc not 0 (keys, axis, autopilot) */
                        float hh = g_pl.heading * 3.14159265f / 180.0f, dv = 0, dx = 0, dz = 0;
                        float wx = sinf(hh) * g_pl_speed, wz = cosf(hh) * g_pl_speed;   /* the walking share (cm/s) */
                        if (g_pl_free && g_pl_free_h < 1e8f && g_pl_free_h != g_pl.heading) {   /* free: a turn leaves the
                                                                                                 * velocity where it was */
                            float h0 = g_pl_free_h * 3.14159265f / 180.0f;
                            jvx += sinf(h0) * g_pl_speed - wx; jvz += cosf(h0) * g_pl_speed - wz;
                        }
                        g_pl_free_h = g_pl_free ? g_pl.heading : 1e9f;
                        if (legs_turning) l2 = r2 = f2 = b2 = 0;
                        if (f2) { dx += sinf(hh); dz += cosf(hh); }
                        if (b2) { dx -= sinf(hh); dz -= cosf(hh); }
                        if (r2) { dx += cosf(hh); dz -= sinf(hh); }
                        if (l2) { dx -= cosf(hh); dz += sinf(hh); }
                        float vx = (wx + jvx) / 182.0f, vz = (wz + jvz) / 182.0f;   /* the velocity before this frame's step */
                        combat_jets_forward(&g_sim.player_unit, (f2 || b2 || l2 || r2) && jet, dt, &dv);
                        /* engine 0x100196cc..0x10019711: with the jets firing and the leg input +0xc not 0 (|+0xc| >=
                         * 1e-7, 0x102473bc) no directional thrust and no lift - the vertical acceleration 0, -g while
                         * rising (combat_jets: jet_forward); the jets only double the turn rate (above) */
                        if (legs_turning && jet) g_sim.player_unit.jet_forward = 1;
                        jvx += dx * dv * 182.0f; jvz += dz * dv * 182.0f;                      /* cm/tick -> cm/s */
                        if (dv > 0) {   /* the drag inside the directional branch (0x100194fa..0x10019618; DOS 0x26ba2), cm/tick:
                                         * T thrust, L = 2.5 x top ground speed (+0x88 x 0.0390625), S the whole velocity's
                                         * speed (DOS (4 max + min) >> 2 of its x / z) at the start of the frame - the step
                                         * is v += ticks x (thrust + drag(v)), so the terminal speed is L at any frame length
                                         * (the drag of v + thrust had settled 1.5 % short: 399 kph for DOSBox's 403) */
                            float ticks = dt * 182.0f, T = dv / (ticks > 1e-6f ? ticks : 1e-6f), kq, B;
                            float ax = fabsf(vx), az = fabsf(vz);
                            float S = ax > az ? ax + az * 0.25f : az + ax * 0.25f;
                            msim_jet_drag(T, g_pl_walk, &kq, &B);   /* L = 2.5 x the top walking speed, the DOS integers */
                            if (S > 1e-7f && (kq != 0 || B != 0)) {
                                float f = kq * S + B;
                                jvx -= vx * f * ticks * 182.0f; jvz -= vz * f * ticks * 182.0f;
                            }
                            if (fabsf(wx + jvx) < 0.0625f * 182.0f) jvx = -wx;   /* components under 0.0625 cm/tick: 0 */
                            if (fabsf(wz + jvz) < 0.0625f * 182.0f) jvz = -wz;
                        }
                        if (g_sim.player_unit.destroyed && g_sim.player_unit.y <= g_sim.player_unit.ground + 1.0f) jvx = jvz = 0;   /* state 4 on the ground: no physics (0x10019310) */
                        if (!g_pl_free) {   /* grounded, jets off: (target - v) / 45 a tick (the walking share has its own
                                             * servo in pl_move, msim_drive_step) */
                            float k = dt * 182.0f / 45.0f;
                            if (k > 1) k = 1;
                            jvx -= jvx * k; jvz -= jvz * k;
                            if (sqrtf(jvx * jvx + jvz * jvz) < 0.0625f * 182.0f) jvx = jvz = 0;
                        }
                        if (jvx || jvz) {
                            float nx = (float)g_pl.origin[0] + jvx * dt, nz = (float)g_pl.origin[2] + jvz * dt;
                            if (msim_can_step_hr(&g_sim, (float)g_pl.origin[0], (float)g_pl.origin[2], nx, nz, g_sim.player_unit.y, g_sim.player_centre_h, g_sim.player_radius)) { g_pl.origin[0] = (int32_t)lrintf(nx); g_pl.origin[2] = (int32_t)lrintf(nz); }
                            else jvx = jvz = 0;
                        }
                        g_jet_vx = jvx; g_jet_vz = jvz;
                    }
                    {
                        float y3 = g_sim.player_unit.y + g_pl_dy_done;
                        combat_jets(&g_sim.player_unit, jet, dt, &g_sim.rng);
                        if (g_pl_imp3 && g_pl_land3) combat_land(&g_sim.player_unit, g_pl_vy3, &g_sim.rng);   /* harmless: landed (0x10019c4e) */
                        else if (g_pl_imp3) {   /* a move refused in 3D (pl_move): where the impact left it, the reflected vertical velocity */
                            g_sim.player_unit.y = y3; g_sim.player_unit.vy = g_pl_vy3; g_sim.player_unit.landed_vy = 0;
                            if (g_sim.player_unit.y <= g_sim.player_unit.ground) combat_land(&g_sim.player_unit, g_pl_vy3, &g_sim.rng);
                        }
                        g_pl_imp3 = 0;
                    }
                    if (jet && g_sim.player_unit.jet_fuel > 0) msim_jet_flames_at(&g_sim, &g_pl, (float)g_pl.origin[0], g_sim.player_unit.y, (float)g_pl.origin[2], g_pl.heading);
                    /* the damage alarm 0xd0: with each announced critical (below, by the messages: 0x100167b0 is its only caller) */
                    /* touching down on the terrain from a fall or a jump (engine 0x10019310 at 0x10019e28: faster than 5.384
                     * cm/tick -> 0x1001c0a0): MECMTNSF 0xe6 up to 16.154 cm/tick, MECMTNHD 0xe5 faster (0xe5 + (vy >= -16.154)),
                     * at the mech (0x10032bf0, positional), and for the player the camera jolt 0x1001c120(0, -vy, 0); a
                     * destroyed mech lands as a wreck (0x1000c4d0) without either. The port had sounded 0xf0 (that is the
                     * object-landing branch, 0x10019c1d) */
                    if (g_sim.player_unit.landed_vy < -5.384f && !g_sim.player_unit.destroyed) {
                        /* the call passes DAT_1024e678 (set in camera mode 0, 0x1002e673) as 0x10032ad0's halving flag: half
                         * volume in the cockpit view; at the feet (+0xcc below the origin) ~10 m from the eye -> 0x10032f60
                         * 100. DOSBox WHITSCN1 / PLUMSCN1 drops: MECMTNHD 0.49 / 0.50x the start's MECTURX2 (which passes
                         * the flag inverted: full in the cockpit) */
                        if (g_sfx) { float p3[3]; snd_player_pos(p3); snd_at(g_sim.player_unit.landed_vy >= -16.154f ? 0xe6 : 0xe5, p3, 0, snd_cockpit()); }   /* 0x1001c115: the mech minus the camera */
                        jolt_start(0.0f, -g_sim.player_unit.landed_vy, 0.0f, g_sim.now);
                    }
                    {   /* the nav's visited flag 0x20 (RRNP on the radar, the autopilot skips it, the objectives' "visited"
                         * test 0x10009ab0) is set in three places only, each with MECNAVPT 0xe7 when new: the autopilot's
                         * arrival (0x10015cb4) and the selector's upkeep (0x1001d5fb) above / below, and an objective node's
                         * "within" test of a nav (0x10009b80 / DOS 0x16190, msim tgt_within: kinds 0x100 / 0x2000 while
                         * the node is evaluated, the player inside the nav's radius - radius <= 0: 200 m - as a 3D
                         * distance, NAVP +16 set). Walking through a nav that is neither selected nor in an evaluated
                         * objective marks nothing (the port's every-frame test of every shown nav with a 60 m minimum
                         * radius was not the engine's) */
                        unsigned char *seen = g_nav_seen;
                        static const msim *seen_for;
                        int nv;
                        if (seen_for != &g_sim) { memset(g_nav_seen, 0, sizeof g_nav_seen); seen_for = &g_sim;
                            if (getenv("MW2_TEST_NAVSEEN")) { const char *q = getenv("MW2_TEST_NAVSEEN"); while (*q) { int k = atoi(q); if (k >= 0 && k < 256) { g_nav_seen[k] = 1; if (k < g_sim.nav_count) g_sim.navs[k].visited = 1; } while (*q && *q != ',') q++; if (*q) q++; } }   /* TEST ONLY: navs already visited */
                        }
                        for (nv = 0; nv < g_sim.nav_count && nv < 256; nv++)
                            if (g_sim.navs[nv].visited && !seen[nv]) { seen[nv] = 1; if (g_sfx) sfx_play(g_sfx, 0xe7, SFX_V64, 0.0f); }   /* 0x10009b80: (0xe7, 0x64, 0x40, 5, 0x50) */
                        /* the selector's upkeep (0x1001d510 / DOS 0x2a6fe, every frame) with a nav selected and the autopilot
                         * off (+0xbc != 1; with it on the autopilot's arrival 0x10015c94 does this): the player within the
                         * selected nav's own radius (+0xc0 < NAVP +0x10, no minimum) marks it visited (0x20) with MECNAVPT 0xe7
                         * if it was not, then steps the selector to the next selectable nav (0x1001dac0(0) -> 0x1001cca0(1):
                         * as NEXT_NAVPOINT, wrapping) - the walked-to nav hands the marker on to the next */
                        nv = g_nav_sel;
                        if (!g_autopilot && nv >= 0 && nv < g_sim.nav_count && nv < 256 && g_sim.navs[nv].selectable) {
                            float ddx = g_sim.navs[nv].x - (float)g_pl.origin[0], ddz = g_sim.navs[nv].z - (float)g_pl.origin[2];
                            if (sqrtf(ddx * ddx + ddz * ddz) < g_sim.navs[nv].radius) {
                                if (!seen[nv]) { seen[nv] = 1; g_sim.navs[nv].visited = 1; if (g_sfx) sfx_play(g_sfx, 0xe7, SFX_V64, 0.0f); }   /* 0x1001d5fb */
                                g_nav_sel = nav_step(nv, 1);
                                g_tgt_any = 0;   /* 0x1001cca0 */
                            }
                        }
                    }
                    {   /* jet sounds (engine 0x1001bf90, every tick the jets burn): sound 0xdf at the jets, held while
                         * they fire; the player's fuel falling through 0x1c4 ticks (a quarter left) sounds the warning
                         * tone 0xde */
                        static float prev_fuel = -1;
                        float fuel = g_sim.player_unit.jet_fuel;
                        if (jet && fuel > 0 && g_sfx && !sfx_playing(g_sfx, 0xdf)) { float p3[3]; snd_player_pos(p3); snd_at(0xdf, p3, 0, snd_cockpit()); }   /* 0x1001c090: at the mech (the mech minus the camera; level 100 = 0.8), halved in the cockpit */
                        if (prev_fuel >= 452.0f && fuel < 452.0f && g_sfx) sfx_play(g_sfx, 0xde, SFX_V64, 0.0f);   /* 0x1001c06a: 0x10032b60(0xde, 0x64, 0x40, 5, 0x50) */
                        prev_fuel = fuel;
                    }
                }
                g_pl.origin[1] = (int32_t)g_sim.player_unit.y;
                if (getenv("MW2_TEST_PASSIVE")) { g_sim.player_unit.invulnerable = 1; g_sim.player_unit.no_heat = 1; }   /* TEST ONLY: no damage, no heat */
                if (getenv("MW2_WALK_TRACE")) {   /* TEST ONLY (tools/sweep_walk.py): navs once, then the walk every 0.5 s */
                    static int32_t wtl = -1000; static int hdr;
                    if (!hdr++) { int q; fprintf(stderr, "walkstart %.0f %.0f %.0f heading %.1f\n", (float)g_pl.origin[0], g_sim.player_unit.y, (float)g_pl.origin[2], g_pl.heading);
                        for (q = 0; q < g_sim.nav_count; q++) fprintf(stderr, "walknav %d %s sel %d %.0f %.0f r %.0f\n", q, g_sim.navs[q].name, g_sim.navs[q].selectable, g_sim.navs[q].x, g_sim.navs[q].z, g_sim.navs[q].radius); }
                    if (g_sim.now - wtl >= 500) {
                        int q, near = -1; float nd = 1e30f;
                        wtl = g_sim.now;
                        for (q = 0; q < g_actor_count; q++) { float ax = g_actors[q].mech.origin[0] - (float)g_pl.origin[0], az = g_actors[q].mech.origin[2] - (float)g_pl.origin[2], d = sqrtf(ax * ax + az * az); if (!g_sim.units[q].destroyed && d < nd) { nd = d; near = q; } }
                        fprintf(stderr, "walk %.2f %.0f %.0f %.0f g %.0f sp %.0f thr %.2f hd %.1f ap %d sel %d seen", g_sim.now / 1000.0, (float)g_pl.origin[0], g_sim.player_unit.y, (float)g_pl.origin[2],
                                msim_ground(&g_sim, (float)g_pl.origin[0], (float)g_pl.origin[2], g_sim.player_unit.y), g_pl_speed, g_throttle, g_pl.heading, g_autopilot, g_nav_sel);
                        for (q = 0; q < g_sim.nav_count && q < 256; q++) if (g_nav_seen[q]) fprintf(stderr, ",%d", q);
                        fprintf(stderr, " blk %d near %d %.0f out %d leg %d", g_sim.player_unit.blocked, near, nd, g_sim.outcome, g_sim.player_unit.immobile);
                        wt_on((float)g_pl.origin[0], (float)g_pl.origin[2], g_sim.player_unit.y);
                        fputc('\n', stderr);
                    }
                }
                g_sim.player[0] = (float)g_pl.origin[0]; g_sim.player[1] = 0; g_sim.player[2] = (float)g_pl.origin[2];
                g_sim.player_facing = g_pl.heading;
                g_pl_target = msim_aim_target(&g_sim, g_pl.heading + g_pl.twist, 15.0f, &d);
                g_sim.player_target = g_pl_target;
                {   /* INPUT.MAP weapon_fire (Space, KeypadEnter) and the left mouse button (port): the selected weapon, or the
                     * current group with group fire on; weapon_fire_group_1..3 (NumLock, keypad /, keypad *); GAMEKEY.MAP
                     * FIRE_WEAPON_GROUP (;) held: the current group */
                    const Uint8 *ks = test_ks();   /* TEST hook: + MW2_HOLD */
                    uint32_t mask = 0;
                    int all = g_auto_on && g_auto_fire;
                    g_im_dev.mouse_buttons = SDL_GetMouseState(NULL, NULL);
                    {   /* INPUT.MAP controls that act once per press, from any device: on the press (rising edge) */
                        static const char *const PRESS[] = {"weapon_cycle", "weapon_cycle_group", "toggle_group_fire", "advance_target",
                                                            "previous_target", "target_reticle", "target_friendly", "nearest_enemy",
                                                            "inspect_target", "advance_nav", "reset_target"};
                        static int was[16];
                        int q, n2 = g_actor_count;
                        for (q = 0; q < (int)(sizeof PRESS / sizeof PRESS[0]); q++) {
                            int now_on = H(PRESS[q]);
                            if (now_on && !was[q] && !g_sim.ending) {
                                switch (q) {
                                case 0: if (g_sim.player_unit.weapon_count > 0) g_sel_weapon = weapon_next(&g_sim.player_unit, g_sel_weapon, 0, 0);   /* panel order (0x10044030) */
                                        if (g_sfx) sfx_play(g_sfx, 0xfd, SFX_VOL(0x32), 0.0f);   /* engine 0x1001adf0: the selector click (0x1001ae5a / 0x1001ae96: volume 0x32, centred) */
                                        break;
                                case 1: g_cur_group = (g_cur_group + 1) % 3; break;
                                case 2: g_group_fire = !g_group_fire; hud_message_v2(g_group_fire ? "Group fire engaged." : "Chain fire engaged.", g_group_fire ? 19 : 18, 80); break;   /* as \ (0x100437d4) */
                                case 3: case 4: {
                                    int i2, qq, step = q == 3 ? 1 : n2 - 1;
                                    for (i2 = 1; i2 <= n2; i2++) {
                                        qq = ((g_tgt_sel < 0 ? (q == 3 ? -1 : 0) : g_tgt_sel) + step * i2 + n2 * 4) % n2;
                                        if (g_sim.armed[qq] && !g_sim.units[qq].destroyed && !g_actors[qq].friendly) { g_tgt_sel = qq; break; }
                                    }
                                    break; }
                                case 5: target_at_reticle(); break;   /* TARGET_AT_RETICLE (q) */
                                case 6: case 7: {
                                    int qq, best = -1;
                                    float bd = 1e30f;
                                    for (qq = 0; qq < n2; qq++) {
                                        float ddx, ddz, dd;
                                        if (!g_sim.armed[qq] || g_sim.units[qq].destroyed || g_actors[qq].friendly != (q == 6)) continue;
                                        ddx = (float)g_actors[qq].mech.origin[0] - (float)g_pl.origin[0]; ddz = (float)g_actors[qq].mech.origin[2] - (float)g_pl.origin[2];
                                        dd = ddx * ddx + ddz * ddz;
                                        if (dd < bd) { bd = dd; best = qq; }
                                    }
                                    g_tgt_sel = best;
                                    break; }
                                case 8: inspect_now(); break;
                                case 9: if (g_sim.nav_count > 0) g_nav_sel = nav_step(g_nav_sel, 1); break;   /* input +0x30, as N: from nothing the first (0x1001cca0) */
                                case 10: g_tgt_sel = -1; break;
                                }
                            }
                            was[q] = now_on;
                        }
                    }
                    /* engine 0x100437a0: the trigger acts on its press - the weapons it names fire if ready, once (only a
                     * repeating weapon fires again while it is held: combat.c); with group fire, the ready weapons of the
                     * group (0x10044440); in chain fire, the selected weapon only - not ready, a click (sound 0x149) and
                     * nothing fires. On the release the selection moves to the next weapon of its fire group (0x10043f80;
                     * stepped in the panel's order, row by row) */
                    if (H("weapon_fire") || (!g_im_mouse && (g_im_dev.mouse_buttons & SDL_BUTTON(SDL_BUTTON_LEFT)))) {
                        if (g_group_fire) mask |= g_groups[g_cur_group] ? g_groups[g_cur_group] : 0xFFFFFFFFu;   /* group: all of it */
                        else if (g_sel_weapon >= 0 && g_sel_weapon < g_sim.player_unit.weapon_count) {
                            const combat_unit *pu2 = &g_sim.player_unit;
                            const combat_weapon *cw2 = &pu2->weapons[g_sel_weapon];
                            if (!g_fire_was && !(g_sim.now < g_sim.player_online_at && !getenv("MW2_NO_STARTUP")) &&
                                !(cw2->state == CW_READY && cw2->ammo != 0 && !pu2->shutdown && !pu2->loc_gone[cw2->location]) && g_sfx)
                                sfx_play(g_sfx, 0x149, SFX_V64, 0.0f);   /* 0x100439c7: (0x149, 0x64, 0x40, 5, 0x32) */
                            mask |= 1u << g_sel_weapon;
                        }
                        g_fire_was = 1;
                    } else {
                        if (g_fire_was && g_sim.player_unit.weapon_count > 0) {
                            const combat_unit *pu2 = &g_sim.player_unit;
                            int gq, w2 = g_sel_weapon, k2;
                            for (gq = 0; gq < 3 && !(g_sel_weapon >= 0 && (g_groups[gq] & (1u << g_sel_weapon))); gq++) {}
                            for (k2 = 0; k2 < pu2->weapon_count; k2++) {
                                w2 = weapon_next(pu2, w2, 0, 0);
                                if (w2 == g_sel_weapon) break;
                                if (gq >= 3 || (g_groups[gq] & (1u << w2))) { g_sel_weapon = w2; break; }
                            }
                        }
                        g_fire_was = 0;
                    }
                    if (g_im_ok && im_action_held(&g_im, "FIRE_WEAPON_GROUP", ks)) mask |= g_groups[g_cur_group];
                    if (H("weapon_fire_group_1")) mask |= g_groups[0];
                    if (H("weapon_fire_group_2")) mask |= g_groups[1];
                    if (H("weapon_fire_group_3")) mask |= g_groups[2];
                    g_sim.player_fire_mask = all ? 0 : mask;
                    g_sim.player_attacks = (all || mask) != 0;   /* the trigger fires with or without a target (along the aim line) */
                    if (g_sim.now < g_sim.player_online_at && !getenv("MW2_NO_STARTUP")) { g_sim.player_fire_mask = 0; g_sim.player_attacks = 0; }   /* starting up */
                    {   /* the triggers held now (combat.c fires on their press); autopilot fire tests press every other frame */
                        static int auto_tog;
                        g_sim.player_unit.fire_held = !g_sim.player_attacks ? 0 : all ? ((auto_tog ^= 1) ? 0xFFFFFFFFu : 0) : mask;
                    }
                }
                g_sim.player_sel_w = g_sel_weapon;
                g_sim.player_aim_free = 1;
                if (getenv("MW2_TEST_AIM")) {   /* TEST ONLY: "k,dist,dy" - stand dist cm south of actor k facing north, the
                                                  * reticle held on its centre + dy cm (dy -1e6: the ground at dist instead) */
                    static int placed;
                    int ka = -1; float dist = 10000.0f, dy = 0.0f, ep[3];
                    sscanf(getenv("MW2_TEST_AIM"), "%d,%f,%f", &ka, &dist, &dy);
                    if (ka >= 0 && ka < g_actor_count) {
                        float tx = (float)g_actors[ka].mech.origin[0], tz = (float)g_actors[ka].mech.origin[2], ty;
                        if (!placed) { placed = 1; g_pl.origin[0] = (int32_t)tx; g_pl.origin[2] = (int32_t)(tz - dist); g_sim.player[0] = tx; g_sim.player[2] = tz - dist; }
                        g_pl.heading = g_sim.player_heading = dist > 0 ? 0.0f : 180.0f; g_pl.twist = 0;   /* dist < 0: north of it */
                        ty = dy < -1e5f ? msim_ground(&g_sim, (float)g_pl.origin[0], (float)g_pl.origin[2] + dist, 1e7f)
                                        : g_sim.units[ka].y + (g_sim.centre_h ? g_sim.centre_h[ka] : 500.0f) + dy;
                        if (dy < -1e5f) tz = (float)g_pl.origin[2] + dist;
                        if ((g_plpit_ok && mech3d_head_point(&g_plpit, ep) == 0) || mech3d_head_point(&g_pl, ep) == 0)
                            g_pitch = atan2f(ty - ep[1], sqrtf((tx - ep[0]) * (tx - ep[0]) + (tz - ep[2]) * (tz - ep[2]))) * 180.0f / 3.14159265f;
                    }
                }
                g_sim.player_aim_yaw = g_pl.heading + g_pl.twist;
                g_sim.player_aim_pitch = g_pitch;
                g_sim.player_twist = g_pl.twist;   /* poses the player's collision pieces */
                g_sim.player_cockpit = g_camode == 0 && !getenv("MW2_HULL_FULL");   /* 0x100267d0: LOD 4 in camera mode 0 (TEST ONLY: MW2_HULL_FULL the full model) */
                {   /* the aim ray's origin: the eye the cockpit camera rides (repr 4's EYEO, else the full model's) */
                    float ep[3];
                    g_sim.player_eye_ok = (g_plpit_ok && !getenv("MW2_EYE_FULL") && mech3d_head_point(&g_plpit, ep) == 0) || mech3d_head_point(&g_pl, ep) == 0;
                    if (g_sim.player_eye_ok) memcpy(g_sim.player_eye, ep, sizeof ep);
                }
                /* the lock works on the selected target (T / E / Q): a unit, or a thing (kind 0x400, 0x10044560) */
                g_sim.player_lock_target = g_tgt_sel >= 0 ? g_tgt_sel : g_tgt_thing >= 0 ? MSIM_TGT_THING(g_tgt_thing) : -1;
                g_sim.radio_busy = g_radio_qn > 0 || g_vq_n > 0;   /* the engine's one queue (DAT_1024f6bc): radio and messages */
                if (!g_mm_depth && !g_paused) msim_step(&g_sim, sim_ms(dt));
                if (!g_mm_depth && !g_paused) start_up_sounds();   /* the first frame's unit updates (0x1001ab71) */
                if (!g_mm_depth && !g_paused) player_upkeep();   /* self-destruct countdown, automatic ejection */
                if (!g_mm_depth && !g_paused) player_messages();   /* heat, shutdown, criticals: messages + voices */
                if (!g_mm_depth && !g_paused) flash_upkeep();   /* the red palette flash */
                if (!g_mm_depth && !g_paused) obj_sounds();    /* object sound tasks (TSK type 4) */
                target_upkeep();   /* the sim drops destroyed targets (0x10016280 / 0x10046a90), HUD or not */
                if (g_tex_kind == 3) dos_time_of_day(r, g_sim.now);   /* DOS: the time-of-day palette */
                if (g_sim.ending && !g_end_announced) {
                    /* the outcome is a voice line (msim queues it); its text shows only with the voice off (0x10031440:
                     * 0x712 ticks at priority 0x32) */
                    const char *t = msim_outcome_text(&g_sim);
                    if (g_voice_off && t) hud_message_ex(t, 0x712, 0x32);
                    g_end_announced = 1;
                }
            } else if (g_sim_ok && g_patrol) {
                /* free camera: the camera is the player */
                g_sim.player[0] = v.eye[0]; g_sim.player[1] = v.eye[1]; g_sim.player[2] = v.eye[2];
                g_sim.player_target = -1;
                g_sim.player_attacks = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_F] != 0;
                if (!g_mm_depth && !g_paused) msim_step(&g_sim, sim_ms(dt));
            }
            if (g_pl_ok && g_pilot) {
                /* camera: chase (behind and above the torso's facing) or cockpit */
                float aim = (g_pl.heading + g_pl.twist) * 3.14159265f / 180.0f;
                v.free_cam = 1;
                v.look_yaw = g_pl.heading + g_pl.twist;
                if (g_cockpit) {
                    float hp[3];
                    /* the eye: in the cockpit the player's object is at level of detail 4, so the camera rides that
                     * representation's EYEO node (engine 0x100267d0 + the camera's attached object, 0x395d0 in DOS) - for
                     * some mechs well above the full model's (Timber Wolf +1.7 m, Mad Dog +1.5 m) */
                    if ((g_plpit_ok && !getenv("MW2_EYE_FULL") && mech3d_head_point(&g_plpit, hp) == 0) || mech3d_head_point(&g_pl, hp) == 0) {
                        if (getenv("MW2_EYE_DY")) hp[1] += (float)atof(getenv("MW2_EYE_DY"));   /* tests */
                        if (getenv("MW2_EYE_DF")) { float df = (float)atof(getenv("MW2_EYE_DF")), hh2 = g_pl.heading * 3.14159265f / 180.0f; hp[0] += sinf(hh2) * df; hp[2] += cosf(hh2) * df; }
                        v.eye[0] = hp[0]; v.eye[1] = hp[1]; v.eye[2] = hp[2];
                        if (getenv("MW2_DEBUG_EYE")) fprintf(stderr, "eye %.0f %.0f %.0f  origin %d %d %d\n", (double)hp[0], (double)hp[1], (double)hp[2], g_pl.origin[0], g_pl.origin[1], g_pl.origin[2]);
                    } else {
                        v.eye[0] = (float)g_pl.origin[0] + sinf(aim) * 250.0f;
                        v.eye[1] = 1050.0f + g_sim.player_unit.y;
                        v.eye[2] = (float)g_pl.origin[2] + cosf(aim) * 250.0f;
                    }
                    {   /* glance_left / right / up / down held (angles from the engine, below) */
                        const Uint8 *ks = test_ks();   /* TEST hook: + MW2_HOLD */
                        /* about 67 degrees: DOS capture - the targeted enemy at radar bearing 52.6 deg shows 15 deg left of
                         * centre while glancing right (90 deg view); +-5 deg (which blip, the view angle) */
                        /* the engine (0x1002e640) sets pilot pan -70 / +70 for glance left / right and pilot tilt 50 up / 40
                         * down for glance up / down while held; the first frame without a glance recentres the view */
                        static int glanced;
                        int gl = H("glance_left") || H("glance_right") || H("glance_up") || H("glance_down");
                        float gp = H("glance_left") ? -70.0f : H("glance_right") ? 70.0f : 0;
                        float gt = H("glance_up") ? 50.0f : H("glance_down") ? -40.0f : 0;
                        if (gl) { glanced = 1; if (gp != 0) g_eye_pan = 0; if (gt != 0) g_eye_tilt = 0; }
                        else if (glanced) { glanced = 0; g_eye_pan = g_eye_tilt = 0; }
                        v.look_yaw += g_eye_pan + gp;
                        v.look_pitch = g_pitch + g_eye_tilt + gt + (getenv("MW2_EYE_PITCH") ? (float)atof(getenv("MW2_EYE_PITCH")) : 0.0f);
                    }
                } else {
                    v.eye[0] = (float)g_pl.origin[0] - sinf(aim) * 2600.0f;
                    v.eye[1] = 1500.0f + g_sim.player_unit.y;
                    v.eye[2] = (float)g_pl.origin[2] - cosf(aim) * 2600.0f;
                    v.look_pitch = g_pitch - 8.0f;
                }
                {   /* the camera jolt (0x1002d7c0): the base view + its servos */
                    float jo[4];
                    static int32_t jolt_at = -1;
                    if (g_sim.now != jolt_at) { jolt_at = g_sim.now; jolt_step(g_sim.now, jo); g_jolt_view[0] = jo[0]; g_jolt_view[1] = jo[1]; g_jolt_view[2] = jo[2]; g_jolt_view[3] = jo[3]; }
                    v.eye[0] += g_jolt_view[0]; v.eye[1] += g_jolt_view[1]; v.eye[2] += g_jolt_view[2]; v.look_pitch -= g_jolt_view[3];   /* camera +0x10 positive = down */
                }
            }
            all.parts = NULL;
            g_pit_first = -1;
            if (g_sim_ok) {   /* leave destroyed pieces out; add the player's mech (not in the cockpit) */
                int k, total = 0, show_pl = g_pl_ok && g_pilot && !VIEW_IN_COCKPIT && (!g_sim.player_unit.destroyed || g_sim.player_unit.intact_out || g_camode == 1);   /* state 5: it stands; state 4 seen from the orbit: standing too (DOSBox) */
                if (show_pl && g_camode == 1 && g_sim.player_eye_ok) {
                    /* the death camera gliding out of the cockpit: inside the mech, the engines draw only front faces (the
                     * DOSBox frames show the open view); the data's winding is not consistent enough to cull, so the mech
                     * is left out until the camera is a radius away from the eye */
                    float ex = g_cam_eye[0] - g_sim.player_eye[0], ey = g_cam_eye[1] - g_sim.player_eye[1], ez = g_cam_eye[2] - g_sim.player_eye[2];
                    if (ex * ex + ey * ey + ez * ez < g_sim.player_radius * g_sim.player_radius) show_pl = 0;
                }
                /* the levels of detail are the simulation's (msim collision_lods: 0x100267d0, DOS 0x37580 - a unit is hit as it is
                 * drawn): this frame's camera for its next step */
                memcpy(g_sim.cam_eye, v.eye, sizeof g_sim.cam_eye);
                g_sim.cam_a0 = 2.0f * (g_zoom > 1 ? g_zoom : 1.0f) / (float)g_lodq;
                g_sim.cam_ok = 1;
                for (k = 0; k < g_actor_count; k++) total += g_actors[k].mech.part_count;
                if (show_pl || (g_cockpit && g_pl_ok)) total += g_pl.part_count + (g_plpit_ok ? g_plpit.part_count : 0);
                total += g_sim.shot_count + g_sim.debris_count;   /* projectiles in flight, debris */
                memset(&all, 0, sizeof all);
                all.parts = malloc((size_t)(total ? total : 1) * sizeof *all.parts);
                for (k = 0; all.parts && k < g_actor_count; k++) {
                    int q;
                    {
                        const mech3d *src = &g_actors[k].mech;
                        float ddx = (float)src->origin[0] - v.eye[0], ddy = (float)src->origin[1] - v.eye[1], ddz = (float)src->origin[2] - v.eye[2];
                        /* a destroyed mech (engine 0x100267d0: flag 2 -> representation 1 whatever the distance, the
                         * player's 0) stays as it was, less the parts its destroyed locations sent flying (msim blow_offs:
                         * a centre torso carries the hips - all of it); the old "wreck" set was the cockpit representation */
                        const int dead = g_sim.armed[k] && g_sim.units[k].destroyed && !g_sim.units[k].intact_out;
                        const unsigned fly = g_sim.armed[k] ? mech3d_fly_mask(g_sim.units[k].loc_gone) : 0;
                        mech3d *lm = actor_lod(k, src, g_actors[k].skel, g_sim.col_repr ? -(float)msim_unit_repr(&g_sim, k) : dead ? -1.0f : sqrtf(ddx * ddx + ddy * ddy + ddz * ddz));
                        if (lm && lm->part_count > src->part_count) {
                            mech3d_part *grown = realloc(all.parts, (size_t)(total + lm->part_count) * sizeof *all.parts);
                            if (grown) { all.parts = grown; total += lm->part_count; } else lm = NULL;
                        }
                        if (lm) src = lm;
                        if (c3d_ti && g_tex_kind != 3) {   /* TEXTURES TITANIUM: its Titanium model, at the same level of detail */
                            mech3d *tm = ti_actor(k, &g_actors[k].mech, g_actors[k].skel, lm ? (int)(lm - g_alod[k].m) : 0);
                            if (tm) {
                                mech3d_part *grown = realloc(all.parts, (size_t)(total + tm->part_count) * sizeof *all.parts);
                                if (grown) { all.parts = grown; total += tm->part_count; src = tm; }
                            }
                        }
                        for (q = 0; q < src->part_count; q++) {   /* blown-off locations aren't drawn */
                            int gq = src->parts[q].group;
                            if (mech3d_fly_loc(src, q, fly)) continue;
                            all.parts[all.part_count] = src->parts[q];
                            /* the damage shading (0x1002b9b0, glr_set_damage_shade): a mech's location pieces (type word 0x100) */
                            all.parts[all.part_count].dmg_nib = g_sim.armed[k] && gq >= 1 && gq <= 8 ? loc_nibble(&g_sim.units[k], gq) : 0;
                            if (dead) {   /* image enhancement (0x1002a4b0 / DOS 0x38ae3): colour 8, the dark red */
                                if (g_vision == 2) all.parts[all.part_count].wire_dmg = 4;
                                all.part_count++;
                                continue;
                            }
                            /* image enhancement (0x1002a4b0, mode 0): a location's pieces by its damage nibble - under 1
                             * blue, under 12 yellow, else red (the nibble the target view grades, 0x1001b620) */
                            if (g_vision == 2 && g_sim.armed[k] && gq >= 1 && gq <= 8) {
                                static const int WC[4] = {1, 3, 2, 2};
                                all.parts[all.part_count].wire_dmg = WC[loc_level(&g_sim.units[k], gq)];
                            }
                            all.part_count++;
                        }
                    }
                }
                if (show_pl && all.parts) {
                    int q;
                    const unsigned fly = mech3d_fly_mask(g_sim.player_unit.loc_gone);
                    const mech3d *pm = &g_pl;
                    if (c3d_ti && g_tex_kind != 3) {   /* TEXTURES TITANIUM: the player's mech as its Titanium model */
                        mech3d *tm = ti_actor(64, &g_pl, g_sim.player_skel, 0);
                        mech3d_part *grown = tm ? realloc(all.parts, (size_t)(total + tm->part_count) * sizeof *all.parts) : NULL;
                        if (grown) { all.parts = grown; total += tm->part_count; pm = tm; }
                    }
                    for (q = 0; q < pm->part_count; q++) {
                        if (mech3d_fly_loc(pm, q, fly)) continue;
                        all.parts[all.part_count] = pm->parts[q];
                        all.parts[all.part_count++].dmg_nib = loc_nibble(&g_sim.player_unit, pm->parts[q].group);   /* (0x1002b9b0) */
                    }
                }
                const int tipool = c3d_ti && g_tex_kind != 3;   /* TEXTURES TITANIUM: its own pool models */
                mech3d_part *const projm = tipool ? g_tiproj : g_proj;
                float (*const projs)[3] = tipool ? g_tiproj_scale : g_proj_scale;
                const float *const projl = tipool ? g_tiproj_len : g_proj_len, *const projo = tipool ? g_tiproj_off : g_proj_off;
                if (tipool) load_ti_projectiles();
                for (k = 0; all.parts && g_proj_ok && k < g_sim.shot_count; k++) {
                    /* each shot: its weapon's model at its position, pointing along its velocity */
                    const msim_shot *sh = &g_sim.shots[k];
                    int ps = proj_slot_w(sh->weapon);
                    mech3d_part pp;
                    if (ps < 0) continue;
                    if (g_camode == 3 && g_follow && sh->id == g_follow) continue;   /* the ordnance camera rides it (DOSBox: not in view) */
                    if (ps <= 2 && g_dos_lasers) continue;   /* MW2_DOS_LASERS=1: the DOS glowing balls (cards, below) */
                    pp = projm[ps];
                    float hv = sqrtf(sh->vx * sh->vx + sh->vz * sh->vz), yw = atan2f(sh->vx, sh->vz), pt = atan2f(sh->vy, hv);
                    float cy = cosf(yw), sy = sinf(yw), cp = cosf(pt), sp = sinf(pt);
                    if (pp.model.object_count < 1) continue;
                    pp.pos[0] = (int32_t)sh->x; pp.pos[1] = (int32_t)sh->y; pp.pos[2] = (int32_t)sh->z;
                    pp.has_rot = 1;            /* R = Ryaw * Rpitch (model +z forward; a climbing shot's nose up - the
                                                * pitch had been applied the wrong way round) */
                    pp.rot[0] = cy;  pp.rot[1] = -sy * sp; pp.rot[2] = sy * cp;
                    pp.rot[3] = 0;   pp.rot[4] = cp;       pp.rot[5] = sp;
                    pp.rot[6] = -sy; pp.rot[7] = -cy * sp; pp.rot[8] = cy * cp;
                    {   /* the pool object's OBJ scale (MW2_SHT1: lasers 3, 3, 12; missiles 1, 1, 2; bullet 1) */
                        int q;
                        for (q = 0; q < 3; q++) { pp.rot[q * 3 + 0] *= projs[ps][0]; pp.rot[q * 3 + 1] *= projs[ps][1]; pp.rot[q * 3 + 2] *= projs[ps][2]; }
                    }
                    if (ps <= 2) {
                        /* the laser bolt (engine 0x10043cc0 / 0x10044ac0, DOS 0x52757 / 0x53410 - the same code): the LASER1-3
                         * mesh with its MW2_SHT1 OBJ scale (3, 3, 12) - long thin bolts (3D 43 / 30 / 19 m, DOS 13 m) - set
                         * along the velocity at launch, no glow; the model lies behind its origin (nose at z 0), and the
                         * engine puts that origin 2 x the model's radius (g_proj_off: about one scaled length) ahead of the
                         * muzzle, so the bolt's tail leaves the muzzle: drawn here that far ahead of the shot point (the shot
                         * itself, and its hits, still start at the muzzle) */
                        float L = projo[ps] > 0 ? projo[ps] : (projl[ps] > 0 ? projl[ps] : 300.0f) * projs[ps][2], vl = sqrtf(sh->vx * sh->vx + sh->vy * sh->vy + sh->vz * sh->vz);
                        if (vl > 0) {
                            pp.pos[0] = (int32_t)(sh->x + sh->vx / vl * L);
                            pp.pos[1] = (int32_t)(sh->y + sh->vy / vl * L);
                            pp.pos[2] = (int32_t)(sh->z + sh->vz / vl * L);
                        }
                    }
                    pp.group = 0;
                    /* the pool's projectile objects get type word 0x400 when loaded (BTHG handler: 3D 0x1003ed54 ->
                     * 0x1000fb20, DOS 0x4f3e8 -> 0x1e300): image enhancement draws them red (0x1002a4b0 / DOS 0x38ae3, mode
                     * 0: 0x400 -> colour 0xb; DOSBox YELLSCN1: missiles and lasers red outlines); their textures stay on with
                     * OBJECT TEXTURES off (0x1002a768 switches only 0x100 / 0x200 / 0x800 objects) */
                    pp.objtype = 0x400;
                    if (tipool) { static int said; if (!said) { said = 1; printf("titanium: shot drawn as pool model %s\n", pp.model_name); } }
                    all.parts[all.part_count++] = pp;
                }
                for (k = 0; all.parts && g_proj_ok && k < g_sim.debris_count; k++) {   /* debris: tumbling chunks */
                    const msim_debris *d = &g_sim.debris[k];
                    mech3d_part pp;
                    if (d->kind == 3) { if (!g_sim.world || d->part >= g_sim.world->part_count) continue; pp = g_sim.world->parts[d->part]; pp.hidden = 0; pp.moving = 0; }
                    else pp = d->kind == 2 ? (d->actor < 0 ? g_pl.parts[d->part] : g_actors[d->actor].mech.parts[d->part]) : projm[7 + d->kind];
                    if (d->kind == 2) {   /* TEXTURES TITANIUM: the Titanium mech's own piece */
                        const mech3d_part *tp = ti_debris_part(d->actor < 0 ? 64 : d->actor, d->actor < 0 ? &g_pl : &g_actors[d->actor].mech, d->part);
                        if (tp) {
                            static int said;   /* (tests) */
                            if (!said) { said = 1; printf("titanium: blown-off piece %s as %s\n", pp.model_name, tp->model_name); }
                            pp.model = tp->model; pp.ti = 1;
                        }
                    }
                    float a = d->ang[0] * 0.0174533f, b = d->ang[1] * 0.0174533f, c = d->ang[2] * 0.0174533f;
                    float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b), cc = cosf(c), sc = sinf(c);
                    if (pp.model.object_count < 1) continue;
                    pp.pos[0] = (int32_t)d->p[0]; pp.pos[1] = (int32_t)d->p[1]; pp.pos[2] = (int32_t)d->p[2];
                    pp.has_rot = 1;            /* R = Ry(b) Rx(a) Rz(c) */
                    pp.rot[0] = cb * cc + sb * sa * sc; pp.rot[1] = -cb * sc + sb * sa * cc; pp.rot[2] = sb * ca;
                    pp.rot[3] = ca * sc;                pp.rot[4] = ca * cc;                 pp.rot[5] = -sa;
                    pp.rot[6] = -sb * cc + cb * sa * sc; pp.rot[7] = sb * sc + cb * sa * cc; pp.rot[8] = cb * ca;
                    if (d->kind < 2) { int q; for (q = 0; q < 9; q++) pp.rot[q] *= 2.0f; }   /* the debris objects' OBJ scale 2 (MW2_SHT1) */
                    if (d->kind < 2 && tipool) { static int said; if (!said) { said = 1; printf("titanium: debris chunk drawn as pool model %s\n", pp.model_name); } }
                    if (d->kind == 2) {   /* a blown-off part tumbles from the orientation it had on the mech */
                        const mech3d_part *op = d->actor < 0 ? &g_pl.parts[d->part] : &g_actors[d->actor].mech.parts[d->part];
                        float o[9], rr[9];
                        int i2, j2;
                        if (op->has_rot) memcpy(o, op->rot, sizeof o);
                        else { float yr = op->yaw * 0.0174533f; o[0] = cosf(yr); o[1] = 0; o[2] = sinf(yr); o[3] = 0; o[4] = 1; o[5] = 0; o[6] = -sinf(yr); o[7] = 0; o[8] = cosf(yr); }
                        for (i2 = 0; i2 < 3; i2++) for (j2 = 0; j2 < 3; j2++) rr[i2 * 3 + j2] = pp.rot[i2 * 3] * o[j2] + pp.rot[i2 * 3 + 1] * o[3 + j2] + pp.rot[i2 * 3 + 2] * o[6 + j2];
                        memcpy(pp.rot, rr, sizeof rr);
                    }
                    pp.group = 0;
                    all.parts[all.part_count++] = pp;
                }
                if (all.parts && g_sim.paths && g_sim.world) {   /* world parts carried by path tasks (PTBL): as placed now */
                    int q, nm = 0;
                    mech3d_part *grown;
                    for (q = 0; q < g_sim.world->part_count; q++) nm += g_sim.world->parts[q].moving && !g_sim.world->parts[q].hidden;
                    if (nm && (grown = realloc(all.parts, (size_t)(total + nm) * sizeof *all.parts))) {
                        all.parts = grown; total += nm;
                        for (q = 0; q < g_sim.world->part_count; q++)
                            if (g_sim.world->parts[q].moving && !g_sim.world->parts[q].hidden) { all.parts[all.part_count] = g_sim.world->parts[q]; all.parts[all.part_count++].moving = 0; }
                    }
                }
                if (all.parts && g_sim.world) {
                    /* the fire / smoke cards (world objects of class 0x10 / 0x60): every frame, after the camera, the engine
                     * turns each about the vertical to face the eye - yaw = the heading of (eye - object) in x / z, pitch 0
                     * (0x1002ea80 from the camera update 0x1002dc90; DOS 0x39fa0, the same: 0x3a9e5 of the eye's x / z
                     * less the node's), replacing its OBJ rotation (while DAT_10244d3c / DOS 0xa4c48 is 4 - a display mode
                     * not identified, not in the port - yaw 180, pitch -45). The eye: this frame's (the ordnance / ejection
                     * cameras move it later in the frame - last frame's for them) */
                    int q, nf = 0;
                    mech3d_part *grown;
                    const float *eye = (g_camode == 3 || g_camode == 4) ? g_cam_eye : v.eye;
                    for (q = 0; q < g_sim.world->part_count; q++) nf += g_sim.world->parts[q].facing && !g_sim.world->parts[q].hidden;
                    if (nf && (grown = realloc(all.parts, (size_t)(total + nf) * sizeof *all.parts))) {
                        all.parts = grown; total += nf;
                        for (q = 0; q < g_sim.world->part_count; q++) {
                            mech3d_part *fp = &g_sim.world->parts[q];
                            if (!fp->facing || fp->hidden) continue;
                            all.parts[all.part_count] = *fp;
                            all.parts[all.part_count].facing = 0;
                            all.parts[all.part_count].has_rot = 0;
                            all.parts[all.part_count].yaw = mech3d_heading(eye[0] - (float)fp->pos[0], eye[2] - (float)fp->pos[2]);
                            if (getenv("MW2_FACING_LOG") && fp->model.object_count > 0) {   /* TEST hook: once per card */
                                static unsigned char said[4096];
                                if (q < 4096 && !said[q]) {
                                    const wtb_object *fo = &fp->model.objects[0];
                                    int k3, x0 = 1 << 30, x1 = -(1 << 30), y0 = 1 << 30, y1 = -(1 << 30);
                                    for (k3 = 0; k3 < fo->vert_count; k3++) {
                                        if (fo->verts[k3].x < x0) x0 = fo->verts[k3].x;
                                        if (fo->verts[k3].x > x1) x1 = fo->verts[k3].x;
                                        if (fo->verts[k3].y < y0) y0 = fo->verts[k3].y;
                                        if (fo->verts[k3].y > y1) y1 = fo->verts[k3].y;
                                    }
                                    said[q] = 1;
                                    printf("FACING %s pos=%d,%d eye=%.0f,%.0f yaw=%.3f w=%d h=%d type=%#x\n", fp->model_name, (int)fp->pos[0], (int)fp->pos[2],
                                           (double)eye[0], (double)eye[2], (double)all.parts[all.part_count].yaw, x1 - x0, y1 - y0, fp->objtype);
                                }
                            }
                            all.part_count++;
                        }
                    }
                }
                g_pit_first = all.part_count;   /* the cockpit piece goes last (left out of viewport views) */
                if (!show_pl && VIEW_IN_COCKPIT && !g_satmap && g_pl_ok && all.parts && !g_sim.player_unit.destroyed && g_zoom <= 1.0f) {
                    /* the cockpit frame: the player's representation 4 (engine 0x100267d0 / 0x10026330), posed as the
                     * mech, drawn whole from the eye (both faces); the original drops it while zoomed (DOS capture) */
                    int q;
                    if (g_plpit_ok && g_cockpit_frame == 0) {
                        /* back faces culled by vertex winding, as the hardware draws any object (the same test as the
                         * DOS reference fit, 0.60 overlap: the polygon's plane offset from the eye negative): per frame,
                         * since the arms and legs move about the eye */
                        static wtb_object cul[16];
                        static wtb_poly *cpoly[16];
                        static int ccap[16];
                        for (q = 0; q < g_plpit.part_count && q < 16; q++) {
                            const mech3d_part *src = &g_plpit.parts[q];
                            const wtb_object *o;
                            mech3d_part pp;
                            int k3, n = 0;
                            if (!src->model.object_count) continue;
                            o = &src->model.objects[0];
                            if (ccap[q] < o->poly_count) { free(cpoly[q]); cpoly[q] = malloc(sizeof(wtb_poly) * (size_t)o->poly_count); ccap[q] = cpoly[q] ? o->poly_count : 0; }
                            if (!cpoly[q]) continue;
                            for (k3 = 0; k3 < o->poly_count; k3++) {
                                const wtb_poly *pg = &o->polys[k3];
                                float wv[3][3], e1[3], e2[3], nn[3], dd;
                                int j3, c3;
                                if (pg->n < 3) continue;
                                for (j3 = 0; j3 < 3; j3++) {
                                    const wtb_vertex *vv = &o->verts[pg->idx[j3]];
                                    if (src->has_rot) for (c3 = 0; c3 < 3; c3++) wv[j3][c3] = src->rot[c3 * 3] * vv->x + src->rot[c3 * 3 + 1] * vv->y + src->rot[c3 * 3 + 2] * vv->z + (float)src->pos[c3];
                                    else {
                                        float yy = src->yaw * 3.14159265f / 180.0f;
                                        wv[j3][0] = cosf(yy) * vv->x + sinf(yy) * vv->z + (float)src->pos[0];
                                        wv[j3][1] = vv->y + (float)src->pos[1];
                                        wv[j3][2] = -sinf(yy) * vv->x + cosf(yy) * vv->z + (float)src->pos[2];
                                    }
                                }
                                for (c3 = 0; c3 < 3; c3++) { e1[c3] = wv[1][c3] - wv[0][c3]; e2[c3] = wv[2][c3] - wv[0][c3]; }
                                nn[0] = e1[1] * e2[2] - e1[2] * e2[1]; nn[1] = e1[2] * e2[0] - e1[0] * e2[2]; nn[2] = e1[0] * e2[1] - e1[1] * e2[0];
                                dd = -(nn[0] * (wv[0][0] - v.eye[0]) + nn[1] * (wv[0][1] - v.eye[1]) + nn[2] * (wv[0][2] - v.eye[2]));
                                if (dd < 0) cpoly[q][n++] = *pg;
                            }
                            cul[q] = *o;
                            cul[q].polys = cpoly[q];
                            cul[q].poly_count = n;
                            pp = *src;
                            pp.model.objects = &cul[q];
                            pp.model.object_count = 1;
                            pp.dos_pit = 1;
                            all.parts[all.part_count++] = pp;
                        }
                    } else if (g_cockpit_frame > 0) {   /* the old eye-piece attempt (MW2_COCKPIT_FRAME=1, mechs without repr 4) */
                        int k2;
                        if (g_pit_for != g_frame_mech) { build_pit(); g_pit_for = g_frame_mech; }
                        for (k2 = 0; k2 < g_pit_n; k2++)
                            for (q = 0; q < g_pl.part_count; q++)
                                if (g_pl.parts[q].node == g_pit[k2].node && strcmp(g_pl.parts[q].model_name, g_pit[k2].model_name) == 0) {
                                    mech3d_part pp = g_pl.parts[q];
                                    pp.model = g_pit[k2].model;
                                    pp.flat = 0;
                                    all.parts[all.part_count++] = pp;
                                    break;
                                }
                    }
                }
            }
            if (all.parts || world3d_compose(g_actors, g_actor_count, &all) == 0) {
                glr_set_actors(r, &all, (const uint8_t (*)[3])g_scene.palette, g_scene.slot, g_camo, g_clan);
                free(g_frame_layer.parts);
                g_frame_layer = all;   /* kept until the next frame (the viewport pass re-uploads it without the cockpit) */
            }
        }
        if (v.free_cam && !(g_world && g_pilot && g_pl_ok)) {   /* fly: ~20 m/s, Shift x5 (1 unit ~ 1 cm) */
            static Uint32 prev;
            const Uint8 *ks = test_ks();   /* TEST hook: + MW2_HOLD */
            Uint32 now = SDL_GetTicks();
            float dt = prev ? (float)(now - prev) / 1000.0f : 0, sp = 2000.0f * dt * (ks[SDL_SCANCODE_LSHIFT] ? 5.0f : 1.0f);
            float fy = v.look_yaw * 3.14159265f / 180.0f, fx = sinf(fy), fz = cosf(fy);
            prev = now;
            if (ks[SDL_SCANCODE_W]) { v.eye[0] += fx * sp; v.eye[2] += fz * sp; }
            if (ks[SDL_SCANCODE_S]) { v.eye[0] -= fx * sp; v.eye[2] -= fz * sp; }
            if (ks[SDL_SCANCODE_D]) { v.eye[0] += fz * sp; v.eye[2] -= fx * sp; }
            if (ks[SDL_SCANCODE_A]) { v.eye[0] -= fz * sp; v.eye[2] += fx * sp; }
            if (ks[SDL_SCANCODE_E]) v.eye[1] += sp;
            if (ks[SDL_SCANCODE_Q] && v.eye[1] - sp > 50) v.eye[1] -= sp;
        }
        {   /* sky drift: the engine adds tile_sky*anim_rate per frame; normalised here to 30 fps */
            static Uint32 last;
            Uint32 now = SDL_GetTicks();
            skygnd sg;
            skygnd_defaults(&sg);
            skygnd_lookup(getenv("MW2_SKYGND"), mission, &sg);
            if (last && !g_mm_depth && !g_paused && !getenv("MW2_TEST_SKY_STILL"))   /* (TEST ONLY: still - pictures compared pixel by pixel) */
                v.sky_scroll += sg.tile_sky * sg.anim_rate * (float)(now - last) * 0.03f;   /* held while paused */
            if (c3d_ti) {   /* Titanium (mercsw 0x1005580d): each tick adds skyAnim / 181 to the clouds' U and V, wrapped
                             * past 1; the camera-height fog terms (with a renderer that fogs) */
                int k2;
                if (last && !g_mm_depth && !g_paused)
                    for (k2 = 0; k2 < 2; k2++) {
                        v.ti_scroll[k2] += c3d_ti_sg.anim[k2] * (float)(now - last) * 0.182f;
                        v.ti_scroll[k2] -= floorf(v.ti_scroll[k2]);
                    }
                c3d_ti_frame(&v, v.free_cam ? v.eye[1] : 1000.0f);
            }
            last = now;
        }
        SDL_GL_GetDrawableSize(win, &w, &h);   /* real pixels, incl. HiDPI */
        g_out_w = w; g_out_h = h;
        g_win_lines = h; g_win_cols = w;
        if (g_render_w && g_world) {
            /* the original resolutions: the frame is drawn at 640 x 480 (1024 x 768 for that choice) - the scale the HUD
             * art and layout are made for - then, for 320 x 200, point-sampled down to 320 x 200; the result is shown
             * scaled up in whole blocks to fill 4:3 (non-square pixels at 320 x 200, as a VGA monitor showed them) */
            int dw = g_render_w == 320 ? 640 : g_render_w, dh = g_render_w == 320 ? 480 : g_render_h;
            if (lowres_target(dw, dh) == 0) { w = dw; h = dh; glBindFramebuffer(GL_FRAMEBUFFER, g_lr_fbo[0]); }
        }
        music_poll();
        {   /* TEST ONLY: MW2_TEST_VIDEO=t:look[,t:look] - the GRAPHICS menu's choice at t autopilot seconds; look = a
             * renderer (enhanced, 3dfx, ...), a texture set (original, titanium) or renderer+textures */
            static int vdone;
            const char *q = getenv("MW2_TEST_VIDEO");
            int idx = 0;
            while (q && *q) {
                char nm[32] = "", *plus;
                float t = (float)atof(q);
                const char *c2 = strchr(q, ':');
                if (!c2) break;
                sscanf(c2 + 1, "%31[^,]", nm);
                if (!(vdone & (1 << idx)) && g_auto_on && g_auto_elapsed >= t) {
                    int k;
                    vdone |= 1 << idx;
                    if ((plus = strchr(nm, '+'))) *plus++ = 0;
                    for (k = 0; k < PORTCFG_ED_COUNT; k++) if (strcmp(nm, PORTCFG_ED_NAME[k]) == 0) g_mm_ed_req = k;
                    for (k = 0; k < PORTCFG_TX_COUNT; k++) if (strcmp(plus ? plus : nm, PORTCFG_TX_NAME[k]) == 0) g_mm_tx_req = k;
                    break;
                }
                idx++;
                q = strchr(c2, ',');
                if (q) q++;
            }
        }
        if (g_mm_ed_req >= 0 || g_mm_tx_req >= 0) {   /* Renderer / Textures */
            switch_look(r, &v, ma, &ta, &ati, mission, g_mm_ed_req >= 0 ? g_mm_ed_req : g_edition, g_mm_tx_req >= 0 ? g_mm_tx_req : g_texset);
            g_mm_ed_req = g_mm_tx_req = -1;
        }
        if (g_mm_world_dirty) {   /* OBJECT DENSITY changed: the world layer again */
            glr_set_mech(r, &g_mech, (const uint8_t (*)[3])g_scene.palette, g_scene.slot, g_camo, g_clan);
            g_mm_world_dirty = 0;
        }
        if (g_mm_quit) { write_results(); running = 0; exit_code = 42; }   /* Flee to Windows: the shell leaves too */
        if (g_sim_ok && g_world && g_sim.over && g_tex_kind == 3 && g_exit_fade_t0 < 0 && !getenv("MW2_NO_EXIT_FADE"))
            g_exit_fade_t0 = g_sim.now;   /* DOS: the exit fade first (exit_fade_level) */
        if (g_sim_ok && g_world && g_sim.over && exit_fade_level(NULL) >= 1.0f) {   /* the mission has ended: results for the shell, then leave */
            test_dump("over");   /* TEST hook */
            write_results();
            running = 0;
        }
        if (g_world && g_sim_ok && getenv("MW2_DEBUG_KILL") && g_sim.now >= 1000) {   /* test: destroy actor n at 1 s */
            int q = atoi(getenv("MW2_DEBUG_KILL"));
            if (q >= 0 && q < g_actor_count) g_sim.units[q].destroyed = 1;
        }
        if (g_world && g_fx_ok && getenv("MW2_FX_TEST")) {   /* test: spawn an effect 30 m ahead of the player at 1 s ("type[:secs[:n]]":
                                                                * n - 1 of the type first, 200 m behind the player, so the
                                                                * shown one takes pool slot n - 1) */
            static int done;
            const char *at = strchr(getenv("MW2_FX_TEST"), ':'), *cnt = at ? strchr(at + 1, ':') : NULL;
            if (!done && g_sim.now >= (at ? (int32_t)(atof(at + 1) * 1000.0) : 1000)) {
                float h = (g_pl.heading + g_pl.twist) * 3.14159265f / 180.0f;
                float p[3] = {(float)g_pl.origin[0] + sinf(h) * 3000.0f, g_sim.player_unit.y + 950.0f, (float)g_pl.origin[2] + cosf(h) * 3000.0f};
                int q, nq = cnt ? atoi(cnt + 1) : 1, ty = (int)strtol(getenv("MW2_FX_TEST"), NULL, 16);
                for (q = 1; q < nq; q++) {
                    float pb[3] = {(float)g_pl.origin[0] - sinf(h) * 20000.0f, g_sim.player_unit.y + 950.0f, (float)g_pl.origin[2] - cosf(h) * 20000.0f - (float)q * 1000.0f};
                    on_effect(NULL, ty, pb);
                }
                on_effect(NULL, ty, p);
                for (q = 0; q < FX_LIVE; q++) if (g_fxw.e[q].alive && g_fxw.e[q].type == ty) printf("FXTEST type %x slot %d at %.0f,%.0f\n", ty, g_fxw.e[q].slot, (double)g_fxw.e[q].pos[0], (double)g_fxw.e[q].pos[2]);
                done = 1;
            }
        }
        if (g_world && g_fx_ok) {   /* live weapon effects as camera-facing cards */
            static glr_sprite spr[GLR_MAX_SPRITES];
            int32_t now = (int32_t)((int64_t)g_sim.now * 182 / 1000);
            int k, n = 0;
            fx_update(&g_fxw, now);
            for (k = 0; k < FX_LIVE && n < GLR_MAX_SPRITES; k++) {
                const fx_live *e = &g_fxw.e[k];
                texture *t;
                if (!e->alive || !(t = fx_texture(fx_frame(&g_fxd, e, now)))) continue;
                memcpy(spr[n].pos, e->pos, sizeof spr[n].pos);
                spr[n].quad = 0; spr[n].ti = 0;
                int cs = e->slot >= 0 && e->slot < FX_CARD_SLOTS ? e->slot : 0;   /* the pool slot's own model (MZL0_2 / _3: one polygon) */
                if (g_fxd.card_n[e->type][0] > 0 && g_tex_kind != 3 && !c3d_ti) {
                    /* a 0x8xxx / 0xbxxx card (muzzle flash, jet flame, death explosion): a 3D polygon in the effect object's
                     * orientation (0x10026e80 -> 0x1002c880) - the mount node's for the flashes and flames (0x10044a70),
                     * unrotated for the death explosion (0x10045c30: rotation 0), so that card stands in the world's x / y
                     * plane; each polygon drawn only from its front (0x1002b9b0: (corner 0 - eye) . normal < 0) - the
                     * flashes and flames hold both windings, the death card one (seen only from +z) */
                    int pq, c2, q2, drawn = 0;
                    for (pq = 0; pq < g_fxd.card_n[e->type][cs] && n < GLR_MAX_SPRITES && !drawn; pq++) {
                        float w4[4][3], nn[3];
                        const float *R = e->rot;
                        for (q2 = 0; q2 < 4; q2++)
                            for (c2 = 0; c2 < 3; c2++) {
                                const float *cv = g_fxd.card_v[e->type][cs][pq][q2];
                                w4[q2][c2] = e->pos[c2] + (e->has_rot ? R[c2 * 3] * cv[0] + R[c2 * 3 + 1] * cv[1] + R[c2 * 3 + 2] * cv[2] : cv[c2]);
                            }
                        for (c2 = 0; c2 < 3; c2++) {
                            const float *cn = g_fxd.card_nrm[e->type][cs][pq];
                            nn[c2] = e->has_rot ? R[c2 * 3] * cn[0] + R[c2 * 3 + 1] * cn[1] + R[c2 * 3 + 2] * cn[2] : cn[c2];
                        }
                        if ((w4[0][0] - v.eye[0]) * nn[0] + (w4[0][1] - v.eye[1]) * nn[1] + (w4[0][2] - v.eye[2]) * nn[2] >= 0) continue;
                        spr[n].quad = 1;
                        memcpy(spr[n].qv, w4, sizeof w4);
                        memcpy(spr[n].quv, g_fxd.card_uv[e->type][cs][pq], sizeof spr[n].quv);
                        drawn = 1;
                    }
                    if (getenv("MW2_FX_TEST") && getenv("MW2_STATE_DUMP") && e->pos[0] != 0)   /* TEST hook: the card's pool slot, front faces drawn */
                        printf("FXCARD now=%d type %x slot %d polys %d drawn %d\n", (int)g_sim.now, e->type, e->slot, g_fxd.card_n[e->type][cs], drawn);
                    if (!drawn) continue;
                }
                spr[n].half_w = g_fxd.half_w[e->type];
                spr[n].half_h = g_fxd.half_h[e->type];
                spr[n].tex = t;
                spr[n].bb = spr[n].quad ? 0 : g_fxd.bb[e->type];   /* type 3 cards: the engine billboard from the card's own corners */
                spr[n].wire_code = 0; spr[n].has_tri = 0;   /* image enhancement: dark red (the effect pool's type 0x20, DOS 0x4f4a6) */
                if (spr[n].bb) {
                    int c;
                    for (c = 0; c < 3; c++) { spr[n].top[c] = e->pos[c] + g_fxd.bb_top[e->type][c]; spr[n].pos[c] = e->pos[c] + g_fxd.bb_foot[e->type][c]; }
                }
                n++;
            }
            /* the PPC and gauss projectiles: cards (type 0x3000 -> 0x1002a8e0): texture slot (w & 0xff0) >> 4 of bank 0 -
             * 0x30b0 -> 0x0b a9ppcs01, 0x30d0 -> 0x0d a9gaus01, single frames; a square |CA| wide: PPCSHOT1 (centre to axis
             * 100 cm x scale 2) 4 m, GAS0_0 2 m */
            for (k = 0; g_sim_ok && k < g_sim.shot_count && n < GLR_MAX_SPRITES; k++) {
                const msim_shot *sh = &g_sim.shots[k];
                int kind = combat_weapon_kind(sh->weapon), slot, f, cs;
                const fx_anim *an;
                texture *t = NULL;
                if ((kind != 6 && kind != 7) || !combat_weapon_has_object(sh->weapon)) continue;
                /* TEXTURES TITANIUM: the Titanium pool's card - its bank-0 bitmap (0x1004ef4c -> 0x10001375, no + 0x100),
                 * one frame; the 3D card when that bitmap is missing */
                cs = c3d_ti && g_tex_kind != 3 && c3d_ti_have_scene && c3d_ti_scene.bank0[g_card_slot[1][kind == 6 ? 0 : 1]] ? 1 : 0;
                slot = g_card_slot[cs][kind == 6 ? 0 : 1];   /* from the pool object (load_projectiles) */
                if (cs) {
                    t = c3d_ti_scene.bank0[slot];
                    { static int said; if (!said) { said = 1; printf("titanium: card drawn from the Titanium pool, bank 0 slot 0x%02x, billboard 0x1004ef4c tint 0xc8\n", slot); } }
                } else {
                    an = &g_fxd.tex[slot];
                    if (an->frames <= 0) continue;
                    f = an->rate > 0 ? (int)((now / an->rate) % an->frames) : 0;
                    if (!(t = fx_texture(an->cel[f]))) continue;
                }
                if (!t->rgba) continue;
                spr[n].pos[0] = sh->x; spr[n].pos[1] = sh->y; spr[n].pos[2] = sh->z;
                spr[n].quad = 0;
                spr[n].ti = cs;   /* the Titanium card: mercsw 0x1004ef4c's corners and its 0xc8 tint */
                spr[n].half_w = spr[n].half_h = g_card_half[cs][kind == 6 ? 0 : 1];
                spr[n].tex = t;
                {   /* the card's foot (0, -100) and top (0, 100) x its scale (PPCSHOT1 2, GAS0_0 1) on the shot's own up axis
                     * (the projectile object turned to its course: up = the course's normal in its vertical plane) */
                    float hl = spr[n].half_w, vl = sqrtf(sh->vx * sh->vx + sh->vy * sh->vy + sh->vz * sh->vz), up[3] = {0, 1, 0};
                    float hz = sqrtf(sh->vx * sh->vx + sh->vz * sh->vz);
                    int c;
                    if (vl > 0 && hz > 0) { up[0] = -sh->vx / hz * sh->vy / vl; up[1] = hz / vl; up[2] = -sh->vz / hz * sh->vy / vl; }
                    spr[n].bb = 1;
                    spr[n].wire_code = 2;   /* image enhancement: red (type word 0x400) */
                    spr[n].has_tri = g_card_tri_ok[cs][kind == 6 ? 0 : 1];
                    if (spr[n].has_tri) {   /* the card's triangle on the object turned to the course: x right, y up, z ahead */
                        float zx = vl > 0 ? sh->vx / vl : 0, zy = vl > 0 ? sh->vy / vl : 0, zz = vl > 0 ? sh->vz / vl : 1;
                        float xr[3] = {hz > 0 ? sh->vz / hz : 1, 0, hz > 0 ? -sh->vx / hz : 0};
                        int q2;
                        for (q2 = 0; q2 < 3; q2++) {
                            const float *cv = g_card_tri[cs][kind == 6 ? 0 : 1][q2];
                            for (c = 0; c < 3; c++)
                                spr[n].tri[q2][c] = spr[n].pos[c] + xr[c] * cv[0] + up[c] * cv[1] + (c == 0 ? zx : c == 1 ? zy : zz) * cv[2];
                        }
                    }
                    for (c = 0; c < 3; c++) { spr[n].top[c] = spr[n].pos[c] + up[c] * hl; spr[n].pos[c] -= up[c] * hl; }
                }
                n++;
            }
            if (g_dos_lasers) {   /* MW2_DOS_LASERS=1 - the lasers as glowing beams: a row of camera-facing cards along the beam (1 per 75 cm, 12 m at least).
                 * DOS capture (Jack, taking enemy laser fire): the bolts are green - a white core inside bright green
                 * rings with a dark green rim, a ball when seen end on (DOS models LASER1/2 colour 0x40, LASER3 0x42) */
                static texture glow[3];
                int gi;
                if (!glow[0].rgba)
                    for (gi = 0; gi < 3; gi++) {
                        int x2, y2;
                        glow[gi].w = glow[gi].h = 16;
                        glow[gi].rgba = calloc(256, 4);
                        for (y2 = 0; y2 < 16; y2++) for (x2 = 0; x2 < 16; x2++) {
                            float dx2 = ((float)x2 - 7.5f) / 7.5f, dy2 = ((float)y2 - 7.5f) / 7.5f, r2 = sqrtf(dx2 * dx2 + dy2 * dy2);
                            uint32_t c;
                            if (r2 > 1.0f) continue;   /* outside: transparent */
                            if (r2 < 0.45f) c = 0xffe8fff0u;                       /* white core */
                            else if (r2 < 0.8f) c = gi == 2 ? 0xff40ff60u : 0xff20ff20u;   /* bright green (small: a lighter shade) */
                            else c = 0xff009000u;                                  /* dark green rim */
                            glow[gi].rgba[y2 * 16 + x2] = c;
                        }
                    }
                for (k = 0; g_sim_ok && k < g_sim.shot_count && n < GLR_MAX_SPRITES; k++) {
                    const msim_shot *sh = &g_sim.shots[k];
                    int kind = combat_weapon_kind(sh->weapon), c, ncard;
                    float vl, len, hw;
                    if (kind < 0 || kind > 2 || !combat_weapon_has_object(sh->weapon)) continue;
                    vl = sqrtf(sh->vx * sh->vx + sh->vy * sh->vy + sh->vz * sh->vz);
                    if (vl <= 0) continue;
                    len = g_proj_len[kind] > 0 ? g_proj_len[kind] : 400.0f;
                    hw = kind == 0 ? 35.0f : kind == 1 ? 28.0f : 22.0f;
                    if (len < 1200.0f) len = 1200.0f;   /* a 12 m bolt at least, so a beam reads at range */
                    ncard = (int)(len / 75.0f) + 1;
                    for (c = 0; c < ncard && n < GLR_MAX_SPRITES; c++) {
                        float b = (float)c * len / (float)ncard / vl;
                        spr[n].pos[0] = sh->x - sh->vx * b; spr[n].pos[1] = sh->y - sh->vy * b; spr[n].pos[2] = sh->z - sh->vz * b;
                        spr[n].half_w = hw; spr[n].half_h = hw;
                        spr[n].tex = &glow[kind];
                        spr[n].bb = 0; spr[n].quad = 0; spr[n].ti = 0;
                        spr[n].wire_code = 2; spr[n].has_tri = 0;
                        n++;
                    }
                }
            }
            glr_set_sprites(r, spr, n);
            ti_particles(r, &v, (float)((double)g_sim.now * 182.0 / 1000.0), mission);
            {   /* engine 0x10045d00 (3Dfx; the PowerVR edition the same at 0x1003e770): an effect whose type lights takes the
                 * scene light - the planet's LITE {position, ambient level, a camera-relative flag, a range} - to its
                 * position (the nearest to the camera when several are alive), clears the flag and the range (a world-fixed
                 * light without falloff), lowers the ambient level by 10 (out of range -> 64), and restores all when it
                 * ends (0x10046490). Objects only: the sky and ground layers keep the sun. */
                /* latched (0x10045d00): a NEW lighting blast takes the light only if it is free or the blast is nearer the
                 * camera than the one holding it; the holder keeps it until it ends - it never hops to another living
                 * effect. Only when the planet allows it (PLNT +0x38 == 0) */
                static int li = -1;
                static unsigned char seen[FX_LIVE];
                int bi;
                float base_ambient = g_lite_level > 0 ? (float)g_lite_level * 3.0f / 256.0f : 0.35f;
                if (li >= 0 && !g_fxw.e[li].alive) li = -1;
                for (k = 0; k < FX_LIVE; k++) {
                    const fx_live *e = &g_fxw.e[k];
                    float dx, dy, dz, d2;
                    if (!e->alive) { seen[k] = 0; continue; }
                    if (seen[k]) continue;
                    seen[k] = 1;
                    if (!fx_light(e->type) || !(g_sim_ok ? g_sim.fx_light_ok : 1)) continue;
                    /* nearer on the ground plane: 0x10045d00 compares (light - camera) and (effect - camera) by x and z
                     * only (PTR_DAT_1024e1cc +0 / +8 camera, +0x1c / +0x24 the light) */
                    dx = e->pos[0] - v.eye[0]; dy = 0.0f; dz = e->pos[2] - v.eye[2];
                    d2 = dx * dx + dy * dy + dz * dz;
                    if (li >= 0) {
                        float ex = g_fxw.e[li].pos[0] - v.eye[0], ez = g_fxw.e[li].pos[2] - v.eye[2];
                        if (d2 >= ex * ex + ez * ez) continue;
                    }
                    li = k;
                }
                bi = li;
                /* off by default: the PowerVR edition never showed it (its light is set once) and the relit terrain
                 * flickers light / dark through a firefight; MW2_FXLIGHT=1 turns the 3Dfx behaviour on */
                /* the editions that relit the scene per frame show it - 3Dfx, ATi, S3 (the same engine code) and DOS (0x51060
                 * moves its light to the blast); the PowerVR edition set its light once, and the enhanced mix follows it */
                {
                    int ed_light = g_edition == PORTCFG_ED_3DFX || g_edition == PORTCFG_ED_ATI || g_edition == PORTCFG_ED_S3 || g_edition == PORTCFG_ED_DOS ||
                                   g_edition == PORTCFG_ED_MGA;   /* the Mystique relights per frame too (~0x1006c7a0) */
                    if (!(ed_light || getenv("MW2_FXLIGHT")) || getenv("MW2_NO_FXLIGHT")) bi = -1;
                }
                v.point_light = bi >= 0;
                if (bi >= 0) {
                    int lvl = g_lite_level - 10;
                    if (lvl < 0 || lvl > 255) lvl = 64;
                    memcpy(v.light_pos, g_fxw.e[bi].pos, sizeof v.light_pos);
                    v.ambient = g_lite_level > 0 ? (float)lvl * 3.0f / 256.0f : base_ambient * 54.0f / 64.0f;
                }
                else v.ambient = base_ambient;
                /* TEXTURES TITANIUM: its explosion lights instead (mercsw 0x10011f58 -> 0x100123cd): an effect started with
                 * projectile visual 3 or 6 (and 0x22 / 0x23, Titanium-only weapons) - whatever it hit - makes a point
                 * light at the blast, radius 20000 (+-200 m box), colour (0.6, 0.6, 0.2), for the effect's duration
                 * (+0x38 life = the type's ticks), every one alive lighting the objects in its box (0x1002f238), none
                 * the ground; with the engine's "Multiple Lights" option (0x10097cb8 - on:
                 * the shell's MW2SND.CFG default +0x2c = 1, MW2SHELL 0x10052040, applied by 0x1004ec7f -> 0x1004ecf9).
                 * The port's effects keep only the resulting type: 3, 6, 0x13, 0x15 come from visual 3 / 6 alone, 0x0c and
                 * 0x11 also from visual 4 (which does not light) - taken as lighting (ASSUMED). The 4 nearest the eye. */
                v.ti_dyn_n = 0;
                if (c3d_ti && v.ti_light && !getenv("MW2_NO_FXLIGHT")) {
                    float dist2[GLR_TI_DYN];
                    for (k = 0; k < FX_LIVE; k++) {
                        const fx_live *e = &g_fxw.e[k];
                        float dx, dy, dz, d2;
                        int t = e->type, j, at;
                        if (!e->alive || !(t == 3 || t == 6 || t == 0x0c || t == 0x11 || t == 0x13 || t == 0x15)) continue;
                        dx = e->pos[0] - v.eye[0]; dy = e->pos[1] - v.eye[1]; dz = e->pos[2] - v.eye[2];
                        d2 = dx * dx + dy * dy + dz * dz;
                        at = v.ti_dyn_n < GLR_TI_DYN ? v.ti_dyn_n++ : -1;
                        if (at < 0) {   /* full: replace the farthest if nearer */
                            for (j = 0, at = 0; j < GLR_TI_DYN; j++) if (dist2[j] > dist2[at]) at = j;
                            if (d2 >= dist2[at]) continue;
                        }
                        dist2[at] = d2;
                        v.ti_dyn[at][0] = e->pos[0]; v.ti_dyn[at][1] = e->pos[1]; v.ti_dyn[at][2] = e->pos[2];
                        v.ti_dyn[at][3] = 20000.0f;
                        v.ti_dyn[at][4] = 0.6f; v.ti_dyn[at][5] = 0.6f; v.ti_dyn[at][6] = 0.2f;
                    }
                    {   /* (tests) */
                        static int said;
                        if (v.ti_dyn_n > 0 && !said) { said = 1; printf("titanium: explosion light at %.1f s\n", (double)g_sim.now / 1000.0); }
                    }
                    /* the projectiles' lights (ti_proj_light): every volley's first projectile in flight, the nearest
                     * the eye with the explosions' */
                    for (k = 0; k < g_sim.shot_count; k++) {
                        const msim_shot *sh = &g_sim.shots[k];
                        float dx, dy, dz, d2, col[3];
                        int j, at;
                        if (!sh->first || !combat_weapon_has_object(sh->weapon)) continue;
                        dx = sh->x - v.eye[0]; dy = sh->y - v.eye[1]; dz = sh->z - v.eye[2];
                        d2 = dx * dx + dy * dy + dz * dz;
                        at = v.ti_dyn_n < GLR_TI_DYN ? v.ti_dyn_n++ : -1;
                        if (at < 0) {
                            for (j = 0, at = 0; j < GLR_TI_DYN; j++) if (dist2[j] > dist2[at]) at = j;
                            if (d2 >= dist2[at]) continue;
                        }
                        dist2[at] = d2;
                        ti_proj_light(sh->weapon, col);
                        v.ti_dyn[at][0] = sh->x; v.ti_dyn[at][1] = sh->y; v.ti_dyn[at][2] = sh->z;
                        v.ti_dyn[at][3] = 10000.0f;
                        v.ti_dyn[at][4] = col[0]; v.ti_dyn[at][5] = col[1]; v.ti_dyn[at][6] = col[2];
                        {   /* (tests) */
                            static int said2;
                            if (!said2) { said2 = 1; printf("titanium: projectile light weapon %d colour %.2f %.2f %.2f at %.1f s\n", sh->weapon, col[0], col[1], col[2], (double)g_sim.now / 1000.0); }
                        }
                    }
                }
            }
        }
        /* cockpit: 90 degrees horizontal at 4:3 = 73.74 vertical (DOS, measured against the same scene); chase: 40 */
        if (g_world) v.vfov = g_cockpit ? 2.0f * atanf(tanf(73.74f * 3.14159265f / 360.0f) / g_zoom) * 180.0f / 3.14159265f : 40.0f;   /* zoom_factor narrows the cockpit view */
        if (g_world && g_sim_ok && g_pl_ok && g_pilot) camera_modes(&v);   /* ordnance (3) / ejection (4) camera */
        {   /* 0x1002df60 (each frame from 0x1002dc90, and on entering a camera mode): the camera's zoom (+0x18) changed -
             * VIEWZOOM 0x147 (0x10032b60 edx 0x64, pan 0x40, priority 0x50). The zoom is DAT_1024e658 (the cockpit zoom
             * factor) in mode 0 (the cockpit) and DAT_1024e65c in the others: mode 1 (external / death orbit, 0x1002e280)
             * sets it to 1.0, mode 3 (ordnance, 0x1002e210) 2.0, mode 4 (ejection, 0x1002e7d0) keeps it - so leaving a
             * zoomed cockpit, the ordnance camera and coming back all sound it */
            static float last_zoom = 1.0f;
            const float zoom = g_camode == 0 && g_cockpit ? g_zoom : g_cam_zoom;
            if (g_world && zoom != last_zoom && g_sfx) sfx_play(g_sfx, 0x147, SFX_V64, 0.0f);
            last_zoom = zoom;
        }
        v.ortho_w = 0;
        if (g_world && g_satmap && g_pl_ok) {   /* F3, radar mode 4 (0x100267d0): straight down, NORTH up, the window's width =
                                                * the range (1 km, halved by the zoom down to 62.5 m), orthographic as the engine's */
            float range = 100000.0f / (float)(1 << g_sat_i);
            v.free_cam = 1;
            v.eye[0] = (float)g_pl.origin[0]; v.eye[2] = (float)g_pl.origin[2];
            v.eye[1] = g_sim.player_unit.y + 500000.0f;
            v.look_yaw = 0.0f; v.look_pitch = -89.9f;
            v.ortho_w = range * (float)w / ((float)h * 4.0f / 3.0f);   /* the 4:3 area's width = the range */
        }
        if (getenv("MW2_VPORT") && g_vport == 0) g_vport = atoi(getenv("MW2_VPORT"));   /* tests: start with a viewport open */
        if (getenv("MW2_VISION")) g_vision = atoi(getenv("MW2_VISION"));   /* tests */
        if (getenv("MW2_TGTDISP")) { g_tgtdisp = atoi(getenv("MW2_TGTDISP")); g_tgtdisp_off = g_tgtdisp == 0; }   /* tests */
        if (getenv("MW2_TEST_NAVAUTO")) { static int set; if (!set++) g_autopilot = 1; }   /* tests */
        if (getenv("MW2_TEST_TARGET")) { static int set; if (!set++) { g_tgt_sel = atoi(getenv("MW2_TEST_TARGET")); g_tgt_any = 1; } }   /* tests (as q would) */
        if (getenv("MW2_TEST_DMG") && g_tgt_sel >= 0 && g_sim.armed[g_tgt_sel]) {   /* tests: right arm yellow, left leg red, left arm off */
            static int set; combat_unit *tu = &g_sim.units[g_tgt_sel];
            if (!set++) { tu->armor[4] = tu->armor_max[4] / 2; tu->armor[7] = tu->armor_max[7] / 10; tu->loc_gone[5] = 1; }
        }
        if (getenv("MW2_TEST_LEG") && g_tgt_sel >= 0 && g_sim.armed[g_tgt_sel] && g_sim.now > 3000) {   /* tests: shoot off the left leg at 3 s */
            static int set; static double last; combat_unit *tu = &g_sim.units[g_tgt_sel];
            if (!set++) { int q; for (q = 0; q < 60 && !tu->loc_gone[7]; q++) combat_hit(tu, 11, 8, 180.0f, &g_sim.rng); }
            if (g_sim.now - last >= 1000) { last = g_sim.now; fprintf(stderr, "leg test %.1fs: at %d,%d heading %.0f gone %d immobile %d destroyed %d\n", g_sim.now / 1000.0,
                g_actors[g_tgt_sel].mech.origin[0], g_actors[g_tgt_sel].mech.origin[2], g_actors[g_tgt_sel].mech.heading, tu->loc_gone[7], tu->immobile, tu->destroyed); }
        }
        if (getenv("MW2_TEST_MSG")) { static int sent; if (!sent++) hud_message_v(getenv("MW2_TEST_MSG"), 0); }   /* tests */
        if (getenv("MW2_TEST_AMMO") && g_sim_ok && g_sim.now >= (int32_t)(atof(getenv("MW2_TEST_AMMO")) * 1000.0)) {
            /* tests: "secs" an ammunition bin of the player explodes; "secs:ai" one of the nearest armed enemy with ammo */
            static int done;
            if (!done++) {
                combat_unit *tu = &g_sim.player_unit;
                int q, who = -1;
                if (strstr(getenv("MW2_TEST_AMMO"), "ai")) {
                    float bd = 1e30f;
                    tu = NULL;
                    for (q = 0; q < g_actor_count; q++) {
                        float dx = (float)(g_actors[q].mech.origin[0] - g_pl.origin[0]), dz = (float)(g_actors[q].mech.origin[2] - g_pl.origin[2]);
                        if (!g_sim.armed[q] || g_sim.units[q].destroyed || g_actors[q].friendly || g_sim.units[q].bin_count < 1) continue;
                        if (dx * dx + dz * dz < bd) { bd = dx * dx + dz * dz; who = q; }
                    }
                    if (who >= 0) tu = &g_sim.units[who];
                }
                q = tu ? combat_crit_ammo(tu) : 0;
                printf("AMMOTEST now=%d unit=%d crit=%d blown=%d dead=%d ejected=%d intact=%d\n", g_sim.now, who, q,
                       tu ? tu->ammo_blown : -1, tu ? tu->destroyed : -1, tu ? tu->ejected : -1, tu ? tu->intact_out : -1);
            }
        }
        v.wire = g_world && HUD_VIEW && g_vision == 2;
        v.notex_world = g_notex_world; v.notex_actors = g_notex_actors;
        v.far_cull = g_world ? g_view_far : 0.0f;   /* the planet's VIEW far (load_view_far) */
        v.dos_sky_smooth = g_dos_sky_smooth;        /* the port's dos_shading= (the DOS look's Gouraud fill and haze band) */
        /* the ATi look's affine polygons follow COMBAT VARIABLES DISPLAY DETAIL (ATi 0x10066570: high -> 0x1018a84c = 0,
         * types 5 / 9 / 11 perspective-correct; low -> 1, every textured polygon affine) */
        v.ati_tex = look_tex(g_edition);
        v.tex_cache = look_cache(g_edition);
        v.alpha_blend = getenv("MW2_TEST_NO_ALPHA_BLEND") ? 0 : look_blend(g_edition);   /* (TEST ONLY: the 0.5 alpha test, as before) */
        if (getenv("MW2_TEST_ATI_TEX")) { int tv = atoi(getenv("MW2_TEST_ATI_TEX")); v.ati_tex = tv == 0 ? 0 : (v.ati_tex & 4) | (tv == 2 ? 3 : 1); }   /* TEST ONLY: 0 every
                                                                    * polygon perspective-correct and masked (the port before), 2 all affine */
        if (getenv("MW2_RADARMODE")) { g_radar_big = atoi(getenv("MW2_RADARMODE")) == 1; g_satmap = atoi(getenv("MW2_RADARMODE")) == 2; g_radar_mode = g_radar_big ? 2 : 1; }   /* tests */
        world_fade(r);
        if (g_world) scrub_follow(r, v.free_cam ? v.eye[0] : (float)g_pl.origin[0], v.free_cam ? v.eye[2] : (float)g_pl.origin[2]);
        if (g_world) scene_anim(r);
        perf_mark(0);
        glr_draw(r, &v, w, h);
        perf_mark(1);
        if (g_sim_ok) {   /* the HUD's power state (see pow_update): 3 shut down, 1 starting, else 2 */
            const combat_unit *pp = &g_sim.player_unit;
            int st = pp->destroyed ? 4 : pp->shutdown ? 3 : (g_sim.now < g_sim.player_online_at && !getenv("MW2_NO_STARTUP")) ? 1 : 2;
            static int prev_st = -1;
            pow_update(st == 4 ? 2 : st, g_sim.now);
            /* engine 0x1001e6c0 (end): the player's controller state changing to 2 (operational - the start-up done, also
             * after a restart) sounds 0xce MECBSYRX (0x10032b60(0xce, 0x64, 0x2f, 5, 0x32)). DOSBox: YELLSCN1 / TNJ1SCN1,
             * at the frame the full HUD appears; SB16 per channel L 0.579 R 0.789 (MECTURX2 0.785 both): volume 0x64,
             * pan 0x2f. Like the lock tones these state sounds come only in the cockpit view (0x1001e6c0 / DOS 0x33030
             * skip them, the state still recorded, while DAT_1024e678 is 0) */
            if (prev_st == 1 && st == 2 && g_sfx && VIEW_IN_COCKPIT) sfx_play(g_sfx, 0xce, SFX_V64, sfx_pan(0x2f));
            /* a restart (shut down -> starting: the s key, 0x1001adf0 "Powering up..." -> state 0, or the heat restart
             * 0x10015f50 -> state 0) passes state 0 -> 1 again (0x1001ab71), which sounds the player's MECTURX2 0xf7 at
             * the mech as at the mission start (0x10032bf0; alliance-1 path 0x104 not the player's). DOSBox YELLSCN1 (SB16
             * capture, s / s): BET11_1 at the shutdown, MECTURX2 0.1 s after the restart key, MECBSYRX 6.4 s later */
            /* (0x1001ac19: the halving flag passed inverted - edx = (DAT_1024e678 == 0) - full in the cockpit, half outside) */
            if (prev_st == 3 && (st == 1 || st == 2) && g_sfx) { float p3[3]; snd_player_pos(p3); snd_at(0xf7, p3, 0, !snd_cockpit()); }
            /* ... and to 4 (destroyed) or 0 the table has 0xf6 MECTRDXX (0x1001e7bf: volume 0x64, pan 0x40; DOS 0x3310d),
             * which never sounds: the HUD update calling 0x1001e6c0 (0x1001e340 / DOS 0x32c86) returns before it while
             * the state is 4 or 5, and a state 0 becomes 1 in the mech's own update (0x1001a180) before the HUD runs
             * (DOSBox: none on Ctrl+s). The sound heard at a death is the death explosion's (effect 0x0d's sound 0xf6 /
             * type 3's MECEXPX2 - the same sample; msim wrecks -> on_effect): DOSBox YELLSCN1, cockpit, self-destruct
             * (DOS: at once): with the voice BET13, L 0.394 R 0.356 - the effect halved, the camera still in the cockpit
             * (on_player_down) - not a centred 0.79 */
            prev_st = st;
        }
        if (g_sim_ok && g_world) {   /* display damage: levels and the per-frame static rolls (0x1001e670, 0x10011720, 0x10021300) */
            int tv_has = g_tgt_sel >= 0 || g_tgt_thing >= 0 || (g_nav_sel >= 0 && current_nav(NULL) >= 0);   /* 0x10021300 returns with nothing to show */
            inst_update(g_vport != 0 && !g_hud_off, !g_tgtdisp_off && !g_hud_off && tv_has);
        }
        int wcam = -1;   /* F9 weapon camera: the followed missile in flight */
        if (g_world && HUD_VIEW && g_vport == 3 && g_sim_ok) {
            /* engine 0x100119xx: the followed projectile (0x100469b0); none: the player's newest projectile if it is a
             * missile (0x10046a00) - one projectile until it dies (the same one the ordnance camera follows) */
            wcam = followed();
            if (wcam < 0 && follow_newest()) wcam = followed();
        }
        float vp_wf = 1, vp_hf = 1, td_wf = 1, td_hf = 1;
        int vp_on = pow_window(1, &vp_wf, &vp_hf), td_on = pow_window(1, &td_wf, &td_hf);   /* windows 2 and 13 (see pow_update) */
        if (g_world && HUD_VIEW && ((g_vport >= 1 && g_vport <= 2) || wcam >= 0) && vp_on && !g_snow_show[0] && vp_wf * vp_hf > 0) {
            /* DOS viewport box x 827-991, y 559-686 at 1024x768 = 516.9..619.4 x 349.4..428.8 in 640x480 units */
            glr_view vv = v;
            float sx = (float)h * 4.0f / 3.0f, x0 = ((float)w - sx) * 0.5f;
            int px = (int)(x0 + 516.9f * sx / 640.0f) + hud_side_px(w, h, 1), pw = (int)(102.5f * sx / 640.0f);   /* right-hand */
            int ph = (int)(79.4f * (float)h / 480.0f), py = h - (int)(349.4f * (float)h / 480.0f) - ph;
            if (vp_wf < 1.0f || vp_hf < 1.0f) {
                /* powering up / down (0x10011aa0 / 0x10011b70): the view is drawn into the window as it stands - the camera's
                 * window is the moving rectangle (0x1002ab50), its centre the projection centre and its half width the focal
                 * scale (0x1002f740: focal x = zoom x hw, y = aspect x zoom x hw) - so the picture keeps the full window's
                 * shape scaled by the width, clipped to the rectangle's height */
                int npw = (int)((float)pw * vp_wf), nph = (int)((float)ph * vp_hf), sph = (int)((float)ph * vp_wf);
                if (npw < 1) npw = 1;
                if (nph < 1) nph = 1;
                if (sph < 1) sph = 1;
                vv.clip[0] = px + (pw - npw) / 2; vv.clip[1] = py + (ph - nph) / 2; vv.clip[2] = npw; vv.clip[3] = nph;
                px += (pw - npw) / 2; py += (ph - sph) / 2; pw = npw; ph = sph;
            }
            if (wcam >= 0) {   /* 0x10044f60: AT the projectile, yaw along its horizontal velocity, level (pitch and roll 0) */
                const msim_shot *ms = &g_sim.shots[wcam];
                vv.look_yaw = atan2f(ms->vx, ms->vz) * 180.0f / 3.14159265f;
                vv.look_pitch = 0.0f;
                vv.eye[0] = ms->x; vv.eye[1] = ms->y; vv.eye[2] = ms->z;
            } else if (g_vport == 1) vv.look_yaw = v.look_yaw + 180.0f; else vv.look_pitch = -89.0f;   /* rear / straight down */
            vv.vfov = 2.0f * atanf(tanf(73.74f * 3.14159265f / 360.0f) / 2.0f) * 180.0f / 3.14159265f;   /* all three at zoom 2.0 (0x1001bc50) */
            if (g_pit_first >= 0 && g_frame_layer.parts && g_pit_first < g_frame_layer.part_count) {
                mech3d nopit = g_frame_layer;
                nopit.part_count = g_pit_first;   /* without the cockpit piece */
                glr_set_actors(r, &nopit, (const uint8_t (*)[3])g_scene.palette, g_scene.slot, g_camo, g_clan);
            }
            glr_draw_rect(r, &vv, px, py, pw, ph);
            glViewport(0, 0, w, h);
        }
        g_wcam_live = wcam >= 0;
        if (g_world && HUD_VIEW && (g_tgtdisp == 2 || (g_tgtdisp == 1 && g_tex_kind != 3)) && g_layout_n > 13 && g_tgt_sel >= 0 && g_tgt_sel < g_actor_count && !g_hud_off && td_on && g_pow == 2 && !g_snow_show[1]) {
            /* (the model only with the mech up: 0x10021300 renders it when DAT_101de868 == 2 - starting up or shut down
             * the box stays black, also while it opens / closes) */
            /* the shaded target view (mode 2): the camera at the target's centre less 3 R along the player -> target
             * bearing (0x102475d4 = 3.0), level, zoom 2.0 */
            const world_actor *ta = &g_actors[g_tgt_sel];
            const int *rr = g_layout[13];
            glr_view vv = v;
            float R = g_sim.radius ? g_sim.radius[g_tgt_sel] : 545.0f, ch = g_sim.centre_h ? g_sim.centre_h[g_tgt_sel] : 550.0f;
            float lx = (float)ta->mech.origin[0] - (float)g_pl.origin[0], lz = (float)ta->mech.origin[2] - (float)g_pl.origin[2], ll = sqrtf(lx * lx + lz * lz);
            float sx = (float)h * 4.0f / 3.0f, x0 = ((float)w - sx) * 0.5f;
            int px = (int)(x0 + ((float)rr[0] * 2.0f) * sx / 640.0f) + hud_side_px(w, h, -1), pw = (int)(((float)rr[2] * 2.0f) * sx / 640.0f);   /* left-hand */
            int ph = (int)(((float)rr[3] * 2.4f) * (float)h / 480.0f), py = h - (int)(((float)rr[1] * 2.4f) * (float)h / 480.0f) - ph;
            {   /* powering up / down: the window about its centre (0x10021730 / 0x100217f0 draw it there) */
                int npw = (int)((float)pw * td_wf), nph = (int)((float)ph * td_hf);
                px += (pw - npw) / 2; py += (ph - nph) / 2; pw = npw > 1 ? npw : 1; ph = nph > 1 ? nph : 1;
            }
            if (ll < 1.0f) { lx = 0; lz = 1; ll = 1; }
            vv.free_cam = 1;
            vv.eye[0] = (float)ta->mech.origin[0] - lx / ll * 3.0f * R; vv.eye[2] = (float)ta->mech.origin[2] - lz / ll * 3.0f * R;
            vv.eye[1] = (g_tgt_sel < g_sim.actor_count ? g_sim.units[g_tgt_sel].y : (float)ta->mech.origin[1]) + ch;
            vv.look_yaw = atan2f(lx, lz) * 180.0f / 3.14159265f; vv.look_pitch = 0.0f;
            vv.vfov = 2.0f * atanf(tanf(73.74f * 3.14159265f / 360.0f) / 2.0f) * 180.0f / 3.14159265f;
            if (g_tgtdisp == 1) {
                /* mode 1 in the 3D editions: the target alone, solid in the HUD's dark blue (0, 48, 215), lit, on black
                 * (the engine renders the object with its wire flag, 0x10021300 / 0x1002a4b0 class colour 7; the
                 * user's 3D-edition memory: solid). Its blown-off locations left out */
                static mech3d one;
                static int cap;
                const mech3d *src = &ta->mech;
                int q2;
                if (cap < src->part_count) { mech3d_part *np = realloc(one.parts, (size_t)src->part_count * sizeof *np); if (np) { one.parts = np; cap = src->part_count; } }
                one.part_count = 0;
                for (q2 = 0; q2 < src->part_count && q2 < cap; q2++) {
                    int gq = src->parts[q2].group;
                    if (g_sim.armed[g_tgt_sel] && gq >= 1 && gq <= 8 && gq != 3 && g_sim.units[g_tgt_sel].loc_gone[gq - 1]) continue;
                    one.parts[one.part_count] = src->parts[q2];
                    {   /* each location in its damage colour, as the damage outline (0x1001b620: levels 6 / 3 / 11 -
                         * here the HUD blue, yellow, red): armour lost front or rear in fifteenths, 0 intact, 1-11 yellow,
                         * 12+ red; blown off: gone */
                        unsigned tint = 0x1000000u | LOC_RGB[loc_level(g_sim.armed[g_tgt_sel] ? &g_sim.units[g_tgt_sel] : NULL, gq)];
                        one.parts[one.part_count].tint = tint;
                    }
                    one.part_count++;
                }
                glr_set_actors(r, &one, (const uint8_t (*)[3])g_scene.palette, g_scene.slot, g_camo, g_clan);
                vv.solo = 1; vv.fog_density = 0; vv.wire = 0;
                vv.mono[0] = vv.mono[1] = vv.mono[2] = 0.0f; vv.mono[3] = 2.0f;   /* each part's tint */
                vv.ambient = 0.35f; vv.point_light = 0;
            } else if (g_pit_first >= 0 && g_frame_layer.parts && g_pit_first < g_frame_layer.part_count) {
                mech3d nopit = g_frame_layer;
                nopit.part_count = g_pit_first;
                glr_set_actors(r, &nopit, (const uint8_t (*)[3])g_scene.palette, g_scene.slot, g_camo, g_clan);
            }
            glr_draw_rect(r, &vv, px, py, pw, ph);
            glViewport(0, 0, w, h);
        }
        {
        }
        if (g_render_w == 320 && g_world && g_lr_fbo[0]) {
            /* 320 x 200: the 3D picture (and the inset views) point-sampled down to the 320 x 200 frame; the HUD is then
             * drawn into that frame with the original's 320x200 sprite set and font, 1:1, its layout stretched to the
             * frame (320 across, 200 down: the VGA picture the 320x200 art was made for) */
            glBindFramebuffer(GL_READ_FRAMEBUFFER, g_lr_fbo[0]);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_lr_fbo[1]);
            glBlitFramebuffer(0, 0, g_lr_w, g_lr_h, 0, 0, 320, 200, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, g_lr_fbo[1]);
            w = 320; h = 200;
        }
        if (g_hud && g_world && g_sim_ok) {   /* the red palette flash (red_flash): the world, then the HUD's colours */
            int in_ph;
            float k = flash_level(g_sim.now, &in_ph);
            int ex_red;
            float ek = exit_fade_level(&ex_red);
            if (g_exit_fade_t0 >= 0) {   /* DOS, leaving: the picture toward black / red (exit_fade_level), the bar not */
                const float mul[3] = {1.0f - ek, 1.0f - ek, 1.0f - ek}, add[3] = {ex_red ? ek : 0.0f, 0, 0};
                HUD_BEGIN(g_hud, w, h); hud_tint(g_hud, w, h, mul, add); hud_end(g_hud);
            }
            if (k > 0 && g_tex_kind == 3) {   /* DOS: the palette - every colour toward (63, 0, 0) */
                const float mul[3] = {1.0f - k, 1.0f - k, 1.0f - k}, add[3] = {k, 0, 0};
                HUD_BEGIN(g_hud, w, h); hud_tint(g_hud, w, h, mul, add); hud_end(g_hud);
            }   /* 3D editions: the world's own fade (world_fade, before the 3D draw) */
            /* DOS: the HUD's colours in the VGA palette fade with it; the 3D editions' HUD table 0x10260324 is not touched
             * by a fade (only reloaded with the same slot when the fade back ends, see pal3_upkeep): no HUD flash */
            hud_set_flash(g_hud, 1.0f, 0.0f, 0.0f, g_tex_kind == 3 ? k : 0.0f);
        }
        if (g_hud && g_world && g_pl_ok && g_pilot && !g_hud_off && (HUD_VIEW || (getenv("MW2_HUD") && g_camode != 4))) {   /* F11: TOGGLE_HUD */
            /* The cockpit HUD, laid out as the original (PowerVR edition screenshots): layout values are
             * 320x200 units mapped x2 across and x2.4 down (480 / 200), as the engine scales them (0x10005db0). */
#define LX(x) ((float)(x) * 2.0f)
#define LY(y) ((float)(y) * 2.4f)
            /* text: the engine's colour 0xe (0x10062226(0xe) before and after every HUD string; DOSBox YELLSCN1 "Heat" /
             * "dH/dT" 0,211,0), red texts 0xb, in the mission palette (0x10260324) */
            const float *green = HP(0xe), *red = HP(0xb);
            static const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f},
                               grey[4] = {0.28f, 0.28f, 0.28f, 1.0f}, yellow[4] = {0.95f, 0.72f, 0.25f, 1.0f},
                               black[4] = {0.0f, 0.0f, 0.0f, 0.9f}, blipc[4] = {1.0f, 0.9f, 0.3f, 1.0f};
            const combat_unit *pu = &g_sim.player_unit;
            float head = g_pl.heading + g_pl.twist, ppd, centre;
            int q, tgt = g_tgt_sel, l;
            float yaw = head * 3.14159265f / 180.0f, pit = v.look_pitch * 3.14159265f / 180.0f;
            float fx = sinf(yaw) * cosf(pit), fy = sinf(pit), fz = cosf(yaw) * cosf(pit), rx = cosf(yaw), rz = -sinf(yaw);
            float ux = -sinf(pit) * sinf(yaw), uy = cosf(pit), uz = -sinf(pit) * cosf(yaw);
            g_wire_r[0] = rx; g_wire_r[1] = rz; g_wire_f[0] = sinf(yaw); g_wire_f[1] = cosf(yaw);
            float th = tanf(v.vfov * 0.5f * 3.14159265f / 180.0f), aspect = (float)w / (float)(h > 0 ? h : 1);
            float tdist = -1.0f;
            /* not up (controller state 1 starting / 3 shut down, engine 0x1001e340): only the start-up (+0x78) / shutdown
             * (+0x80) callbacks draw - the radar, target display and viewport windows growing / shrinking, and in
             * start-up the weapon names (see pow_update); the reticle, compass, altitude tape, bars, ammunition and
             * damage display are dark; messages stay. The satellite map (cockpit mode 4) is not touched. */
            const int hud_dead = g_sim_ok && g_pow != 2;
            float rad_wf = 1, rad_hf = 1;
            const int rad_on = pow_window(0, &rad_wf, &rad_hf) && rad_wf > 0;
            HUD_BEGIN(g_hud, w, h);
            if (g_vision == 1 && g_inst_lvl[INST_RADAR] > 0) g_vision = 0;   /* 0x10007900: off when the radar (instrument 0) is damaged */
            /* light amplification, 3D editions: the world's fade (world_fade); the DOS edition fades its palette
             * (dos_time_of_day) */

            /* 0x1001732e: a location's armour used up for the first time: MECRDWA1 0xec (0x50) */
            if (pu->breach_sound) { if (g_sfx) sfx_play(g_sfx, 0xec, SFX_V64, 0.0f);   /* 0x10017335: volume 0x64 */ g_sim.player_unit.breach_sound = 0; }
            {   /* 0x10031e30: every 0x389 ticks (from the program's start) - the player not destroyed - a unit in state 2 /
                 * 3 (target / attack) whose target is the player within 1500 m (+0xc4): MISLTRCK 0x100 (0x32), once */
                static float trk_next = 0;
                float nowt = (float)g_sim.now * 0.182f;
                if (nowt < trk_next - 905.0f) trk_next = 0;   /* a new mission's clock */
                if (nowt >= trk_next) {
                    int q;
                    trk_next = nowt + 905.0f;
                    for (q = 0; g_sim_ok && g_sim.minds && !pu->destroyed && q < g_actor_count; q++) {
                        const ai_mind *mq = &g_sim.minds[q];
                        float ddx = (float)g_actors[q].mech.origin[0] - (float)g_pl.origin[0], ddz = (float)g_actors[q].mech.origin[2] - (float)g_pl.origin[2];
                        if (!g_sim.armed[q] || g_sim.units[q].destroyed || !mq->targets_player || (mq->state != AI_TARGET && mq->state != AI_ATTACK)) continue;
                        if (ddx * ddx + ddz * ddz > 150000.0f * 150000.0f) continue;
                        if (g_sfx) sfx_play(g_sfx, 0x100, SFX_V64, 0.0f);   /* 0x10031ed8: volume 0x64, priority 0x32 */
                        break;
                    }
                }
            }
            /* (the player's messages: player_messages, every sim step whatever the view) */
            if (!hud_dead) {   /* the instruments (dark while shut down) */
            /* target: the selected one only (the DOS footage shows the nav point with no selection) */
            if (tgt >= g_actor_count || (tgt >= 0 && g_sim.armed[tgt] && g_sim.units[tgt].destroyed)) tgt = g_tgt_sel = -1;
            if (tgt >= 0) {
                float ddx = (float)g_actors[tgt].mech.origin[0] - (float)g_pl.origin[0], ddz = (float)g_actors[tgt].mech.origin[2] - (float)g_pl.origin[2];
                tdist = sqrtf(ddx * ddx + ddz * ddz);   /* cm */
            }
            /* compass (engine 0x10022b10 / 0x10022960; window 24 = (115,1)-(205,35) in 320x200 -> 640 units x 230-411,
             * y 2-84; its reference point (0.5, 0.4) of the window = local (91, 33) -> (321, 35); the HUD record's
             * positions are never read). The tape shows the LEGS' heading: COMPASS by its hotspot (the 00 mark) at
             * 91 + heading x (width / 360) px, clipped to the window; CMPMKR3 at the centre; the torso twist as a green
             * band (0xf) 5 px tall above the centre line, 1 px per degree from the centre; the target's bearing: CMPMKR1
             * at its offset (torso-relative for a mech, legs-relative for a nav point), CMPMKR2 + CMPMKR4 when dead
             * centre; up / down arrows (CRTUP / CRTDOWN) by the target's elevation against the torso pitch and left /
             * right arrows beside the window, each shown within 3 degrees of its side (both when centred) */
            {
                float lhead = g_pl.heading, twist = g_pl.twist;
                while (lhead < 0) lhead += 360.0f;
                while (lhead >= 360.0f) lhead -= 360.0f;
                head = lhead;
                if (g_hs_compass.tex) {
                    /* window 24 (DOS table 0x968e0 entry 23) with its reference (0.5, 0.4) (DOS 0x96234) in the screen's
                     * pixels (hud_window): 640 x 480
                     * x 230-411, y 2-84, (321, 35); 1024 x 768 x 369-657, y 4-135, (513, 56) */
                    const hud_win cw = hud_window(115, 1, 205, 35, 0x8000, 0x6666);
                    float ux = cw.ux, uy = cw.uy, cxu = (float)cw.rx * ux, cyu = (float)cw.ry * uy;
                    const float wl = (float)cw.x0 * ux, wr = (float)cw.x1 * ux;   /* the window's left / right edges */
                    float hx;
                    int tx_ok = 0, nav_t = 0;
                    float tbx = 0, tbz = 0, tby = 0;
                    /* the DOS edition in whole degrees (0x32c50: heading +0x60 >> 16 mod 360, twist +0xc >> 16, the
                     * bearing +0xd4 >> 16) and 16.16 pixels a degree (DAT_000fb554 = (w << 16) / 360, rounded products,
                     * 0x29760 / 0x29570); the 3Dfx floats truncated (0x10022b10) */
                    const int dosc = g_tex_kind == 3;
                    const long long cpd = ((long long)g_hs_compass.w << 16) / 360;
                    int hdeg = (int)floorf(lhead) % 360;
                    if (hdeg < 0) hdeg += 360;
                    ppd = (float)g_hs_compass.w / 360.0f;
                    hx = dosc ? (float)(cw.rx + fx16_round(cpd * hdeg)) * ux
                              : cxu + ux * (float)(int)(lhead * ppd);                /* the hotspot's screen x */
                    centre = (wl - hx) / ux - (float)g_hs_compass.left;          /* texture u at the window's left */
                    /* the window's columns x0..x1 (+0x44 = x1 - x0 + 1); the tape one pixel right of its hotspot in the 3D
                     * editions (as the port had it), at it in the DOS one (DOSBox 640 x 480 and 1024 x 768: identical) */
                    hud_draw_window(g_hud, &g_hs_compass, dosc ? centre : centre - 1.0f, (float)(cw.x1 - cw.x0 + 1),
                                    (float)(cw.x0 + (cw.x1 - cw.x0 + 1) / 2) * ux, cyu + uy * (float)g_hs_compass.top, NULL);
                    if (g_hs_cmpmk[2].tex) hud_draw(g_hud, &g_hs_cmpmk[2], cxu, cyu, NULL);
                    if (fabsf(twist) >= 1e-7f) {
                        const float *b0 = HP(14), *b1 = HP(15), *b2 = HP(13);   /* green bands (0xf) in the mission palette */
                        float wv = ux * (float)(dosc ? abs((int)floorf(twist) % 360) : (int)fabsf(twist)), x0 = twist > 0 ? cxu : cxu - wv, y0 = cyu - 5.0f * uy;   /* DOS 0x29760: the whole-degree twist */
                        hud_rect(g_hud, x0, y0, wv, 1.25f * uy, b0); hud_rect(g_hud, x0, y0 + 1.25f * uy, wv, 1.25f * uy, b1);
                        hud_rect(g_hud, x0, y0 + 2.5f * uy, wv, 1.25f * uy, b0); hud_rect(g_hud, x0, y0 + 3.75f * uy, wv, 1.25f * uy, b2);
                    }
                    if (tgt >= 0) {
                        tbx = (float)g_actors[tgt].mech.origin[0]; tbz = (float)g_actors[tgt].mech.origin[2]; tby = g_sim.units[tgt].y; tx_ok = 1;
                    } else {
                        int nv = current_nav(NULL);
                        if (nv >= 0) { tbx = (float)g_sim.navs[nv].x; tbz = (float)g_sim.navs[nv].z; tby = (float)g_sim.navs[nv].y; tx_ok = 1; nav_t = 1; }
                    }
                    if (tx_ok) {
                        float ddx = tbx - (float)g_pl.origin[0], ddz = tbz - (float)g_pl.origin[2], dd = sqrtf(ddx * ddx + ddz * ddz);
                        float bear = atan2f(ddx, ddz) * 180.0f / 3.14159265f, rel = bear - lhead - (nav_t ? 0.0f : twist), mx, elev;
                        const hud_sprite *ar = g_hs_edge;   /* CRTLFT, CRTRGHT, CRTUP, CRTDOWN */
                        while (rel > 180.0f) rel -= 360.0f;
                        while (rel < -180.0f) rel += 360.0f;
                        if (dosc) {   /* DOS 0x32c50: whole degrees, each wrapped to -180..180 */
                            int bd = (int)floorf(bear) % 360, d = bd - hdeg;
                            if (d > 180) d -= 360; else if (d < -180) d += 360;
                            if (!nav_t) {
                                d -= (int)floorf(twist) % 360;
                                if (d > 180) d -= 360; else if (d < -180) d += 360;
                            }
                            rel = (float)d;
                        }
                        elev = atan2f(tby - pu->y, dd > 1 ? dd : 1) * 180.0f / 3.14159265f - g_pitch;
                        if (fabsf(rel) < 1e-7f) {
                            if (g_hs_cmpmk[1].tex) hud_draw(g_hud, &g_hs_cmpmk[1], cxu, cyu, NULL);
                            if (g_hs_cmpmk[3].tex) hud_draw(g_hud, &g_hs_cmpmk[3], cxu, cyu, NULL);
                        } else {
                            /* DOSBox (YELL, E on the Falcon 43 degrees right): CMPMKR1 right of the centre, CRTRGHT beside the
                             * tape - the marker sits on the target's own side, although the tape's numbers rise leftward */
                            mx = dosc ? (float)(cw.rx + fx16_round(cpd * (long long)rel)) * ux : cxu + ux * (float)(int)(rel * ppd);
                            if (getenv("MW2_DEBUG_CMP")) fprintf(stderr, "cmp rel %.1f mx %.1f tex %u\n", rel, mx, g_hs_cmpmk[0].tex);
                            if (mx >= wl && mx <= wr && g_hs_cmpmk[0].tex) hud_draw(g_hud, &g_hs_cmpmk[0], mx, cyu, NULL);
                        }
                        if (elev > -3.0f && ar[2].tex) hud_draw(g_hud, &ar[2], cxu, cyu - 11.0f * uy, NULL);
                        if (elev < 3.0f && ar[3].tex) hud_draw(g_hud, &ar[3], cxu, cyu + 25.0f * uy, NULL);
                        /* the side the marker lies on (screen right = positive rel): DOS 0x29570 at the window's left +
                         * its width (+0x44 = x1 - x0 + 1, 0x2f0e0) + the arrow's width (6) - 1 = x1 + 6 (the port had x1 + 5;
                         * DOSBox YELLSCN1 e: x 412-417 at 640 x 480, 658-663 at 1024 x 768), the second one more right past
                         * 90 degrees; the left one at x0 - 6 (- 7) */
                        if (rel > -3.0f && ar[1].tex) { hud_draw(g_hud, &ar[1], wr + 6.0f * ux, cyu + 5.0f * uy, NULL); if (rel > 90.0f) hud_draw(g_hud, &ar[1], wr + 7.0f * ux, cyu + 5.0f * uy, NULL); }
                        if (rel < 3.0f && ar[0].tex) { hud_draw(g_hud, &ar[0], wl - 6.0f * ux, cyu + 5.0f * uy, NULL); if (rel < -90.0f) hud_draw(g_hud, &ar[0], wl - 7.0f * ux, cyu + 5.0f * uy, NULL); }
                    }
                }
            }
            /* reticle */
            {
                /* the reticle (engine 0x10022710), by the selected weapon: not ready -> RCLINOP; guided (LRM, Streak,
                 * Narc): locked RCLGLOC, on target and acquiring RCLPLOC, else RCLNOLK; SRMs: RCLLOCK on target, else
                 * RCLNOLK; the rest: RCLTGT on target, else RETICLE. On target = the selected target within the weapon's
                 * range window and 3 degrees of the aim */
                const hud_sprite *rs = &g_hs_reticle;
                g_rcl = -1;
                /* the DOS look (default): the base-set sprites at the DOS pixel size - the original at 1024 x 768 drew
                 * them 1:1 (the bracket 23 px, measured 21); MW2_HUD_3D=1 keeps the 3D editions' 640 / 1024 set */
                int dos = getenv("MW2_HUD_BASE") && g_hs_reticle_dos.tex;   /* tests: the 320 x 200 set at the DOS size */
                if (g_sim_ok && g_sel_weapon >= 0 && g_sel_weapon < g_sim.player_unit.weapon_count) {
                    const combat_weapon *cw = &g_sim.player_unit.weapons[g_sel_weapon];
                    int wid = cw->weapon, kind = combat_weapon_kind(wid), on = 0;
                    if (g_tgt_sel >= 0 && g_tgt_sel < g_actor_count && g_sim.armed[g_tgt_sel] && !g_sim.units[g_tgt_sel].destroyed) {
                        float dx = (float)g_actors[g_tgt_sel].mech.origin[0] - (float)g_pl.origin[0], dz = (float)g_actors[g_tgt_sel].mech.origin[2] - (float)g_pl.origin[2];
                        float dy = (g_sim.units[g_tgt_sel].y + g_sim.height[g_tgt_sel] * 0.5f) - v.eye[1], dd = sqrtf(dx * dx + dz * dz + dy * dy);
                        float yw = atan2f(dx, dz) * 180.0f / 3.14159265f - (g_pl.heading + g_pl.twist), pt = atan2f(dy, sqrtf(dx * dx + dz * dz)) * 180.0f / 3.14159265f - g_pitch;
                        while (yw > 180.0f) yw -= 360.0f;
                        while (yw < -180.0f) yw += 360.0f;
                        on = dd < combat_weapon_range(wid) && fabsf(yw) < 3.0f && fabsf(pt) < 3.0f;
                    } else if (g_tgt_sel < 0 && g_tgt_thing >= 0) {
                        /* a thing (kind 0x400: 0x10022710 refuses only kinds 0x100 / 0x1000): its point is its object's
                         * position (0x1001d2d0 -> 0x100257a0), the bearing and distance from the player's feet (+0x50) */
                        float tp[3];
                        if (msim_thing_point(&g_sim, g_tgt_thing, tp)) {
                            float dx = tp[0] - (float)g_pl.origin[0], dz = tp[2] - (float)g_pl.origin[2], dy = tp[1] - g_sim.player_unit.y;
                            float dd = sqrtf(dx * dx + dz * dz + dy * dy);
                            float yw = atan2f(dx, dz) * 180.0f / 3.14159265f - (g_pl.heading + g_pl.twist), pt = atan2f(dy, sqrtf(dx * dx + dz * dz)) * 180.0f / 3.14159265f - g_pitch;
                            while (yw > 180.0f) yw -= 360.0f;
                            while (yw < -180.0f) yw += 360.0f;
                            on = dd < combat_weapon_range(wid) && fabsf(yw) < 3.0f && fabsf(pt) < 3.0f;
                        }
                    }
                    {
                        const hud_sprite *R = dos ? g_hs_rcl_dos : g_hs_rcl;
                        /* RCLINOP (the plain yellow ring): the selected weapon not ready - slot state +8 != 1 (0x10022710;
                         * DOS 0x29360 the same test): recycling, firing its volley, destroyed or out of ammunition. No lock
                         * colours then, though the lock (0x10044560, not gated by readiness) runs on: RCLGLOC at once when
                         * the weapon is ready again. DOSBox YELLSCN1 (ML_r6_*): the LRM's volley and its whole reload show
                         * RCLINOP; the chain-fire step on the release moves the selection to the next weapon */
                        int inop = cw->state != CW_READY || cw->ammo == 0 || (cw->location >= 0 && cw->location < 8 && g_sim.player_unit.loc_gone[cw->location]);
                        if (inop) rs = &R[0];
                        else if (msim_weapon_guided(wid)) rs = (g_sim.player_lock & 0x80) ? &R[5] : (g_sim.player_lock & 0x8000) ? &R[3] : &R[2];
                        else if (kind == 3) rs = on ? &R[1] : &R[2];
                        else if (on) rs = &R[4];
                        else if (dos) rs = &g_hs_reticle_dos;
                        g_rcl = rs >= R && rs < R + 6 ? (int)(rs - R) : -1;
                    }
                } else if (dos) rs = &g_hs_reticle_dos;
                /* only from the cockpit camera: the HUD pass (0x1001e5f2 -> 0x10022230) draws it while DAT_1024e674 is set,
                 * which only mode 0 sets (0x1002e750); the external orbit, ordnance and ejection cameras clear it
                 * (0x1002dca5). DOSBox YELLSCN1 'c': the HUD without the reticle */
                if (g_camode == 0) {
                    /* at the aim point's projection (DOS 0x29860 -> 0x3f320, 3Dfx 0x10022c40 -> 0x1002f9f0): x = centre + q,
                     * y = (y1 - y0) - centre - q with the view's centre (y0 + y1 + 1) >> 1 (DOS 0x3ef30, 3Dfx 0x1002f779) -
                     * one row above the middle: 640 x 480 (320, 239), 1024 x 768 (512, 383) (DOSBox YELLSCN1: the reticle
                     * 1 px higher than the port's (320, 240) at both) */
                    const float ux = hud_px_x(g_hud, 1.0f), uy = hud_px_y(g_hud, 1.0f);
                    const int W1 = (int)lrintf(640.0f / ux) - 1, H1 = (int)lrintf(480.0f / uy) - 1;
                    const float rx = (float)((W1 + 1) >> 1) * ux, ry = (float)(H1 - ((H1 + 1) >> 1)) * uy;
                    if (dos) hud_draw_f(g_hud, rs, rx, ry, -1.0f);
                    else hud_draw(g_hud, rs, rx, ry, NULL);
                }
            }
            /* target bracket (engine dispatcher 0x10022830): a mech (0x10022cc0) gets four corner pieces TGTGP1-4 (+E enemy,
             * +N neutral) at (x -+ h, y -+ h), h = R x F / depth - R its contact radius (MGEO int[6]), F the view's focal
             * length, the point its centre (MGEO int[0] above the feet), no minimum or cap; a nav point TGTNP. Off
             * screen (behind: |depth|): the ray from the screen centre through the point clipped to the view, TGTOFFF /
             * E / N (an X) there - TGTOFFF for a nav point. */
            {
                float wp[3], thr = 0;
                int have = 0, isnav = 0, side = 1, isthing = 0;
                if (tgt < 0 && g_tgt_thing >= 0 && g_tgt_thing < g_sim.bld_count && g_sim.bld[g_tgt_thing].hp > 0) {
                    /* a thing (kind 0x400 -> 0x10022ea0): the same four corners at its collision object's bounding
                     * sphere (0x10029890: centre, radius), by its side (0x10019060: its AFFL team's alliance, none 2).
                     * The engine's sphere always exists; should the port's model give none, the structure's box (the
                     * target display's centre and half diagonal) - a targeted thing never falls through to the nav marker */
                    if (!msim_thing_sphere(&g_sim, g_tgt_thing, wp, &thr)) {
                        const struct msim_building *gb = &g_sim.bld[g_tgt_thing];
                        float hx = (gb->mx[0] - gb->mn[0]) * 0.5f, hy = (gb->mx[1] - gb->mn[1]) * 0.5f, hz = (gb->mx[2] - gb->mn[2]) * 0.5f;
                        int k;
                        for (k = 0; k < 3; k++) wp[k] = (gb->mn[k] + gb->mx[k]) * 0.5f;
                        thr = sqrtf(hx * hx + hy * hy + hz * hz);
                    }
                    side = g_sim.bld[g_tgt_thing].side;
                    if (side < 0 || side > 2) side = 1;
                    have = isthing = 1;
                } else if (tgt >= 0) {
                    wp[0] = (float)g_actors[tgt].mech.origin[0];
                    wp[1] = (tgt < g_sim.actor_count ? g_sim.units[tgt].y : (float)g_actors[tgt].mech.origin[1]) + (g_sim.centre_h ? g_sim.centre_h[tgt] : 550.0f);
                    wp[2] = (float)g_actors[tgt].mech.origin[2];
                    side = g_actors[tgt].friendly ? 0 : g_actors[tgt].alliance == 2 ? 2 : 1;
                    have = 1;
                } else {
                    int nv = current_nav(NULL);
                    if (nv >= 0) {
                        wp[0] = g_sim.navs[nv].x; wp[2] = g_sim.navs[nv].z;
                        wp[1] = msim_ground(&g_sim, wp[0], wp[2], 1e7f);
                        /* DOS: the nav's own NAVP point (the mission loader 0x4f518 -> 0x35e20 / 0x3aefc: x, y, z through the
                         * world frame; the bracket's point +200 / +0xcc / +0xd0 from it, 0x2a0a0) - not the ground's height */
                        if (g_tex_kind == 3) wp[1] = g_sim.navs[nv].y;
                        have = isnav = 1; side = 0;
                    }
                }
                g_bracket = 0;
                if (have && g_tex_kind == 3 && dos_hud_mark(r, w, h, wp, isnav, isthing, side, isthing ? thr : g_sim.radius ? g_sim.radius[tgt] : 545.0f)) {
                    /* the DOS look: MW2.EXE's integers (dos_hud_mark) */
                } else if (have) {
                    float tx = wp[0] - v.eye[0], ty = wp[1] - v.eye[1], tz = wp[2] - v.eye[2];
                    float zf = tx * fx + ty * fy + tz * fz, xr = tx * rx + tz * rz, yu = tx * ux + ty * uy + tz * uz;
                    float sx = (float)h * 4.0f / 3.0f, kx = 320.0f * (float)w / sx;   /* 640-unit x per unit of nx */
                    float az = fabsf(zf) > 1.0f ? fabsf(zf) : 1.0f;
                    float nx = xr / az / (th * aspect), ny = yu / az / th;
                    float vx = 320.0f + nx * kx, vy = 240.0f - ny * 240.0f;
                    if (zf > 1.0f && vx > 320.0f - kx && vx < 320.0f + kx && vy > 0 && vy < 480) {   /* the whole view, not just its 4:3 middle */
                        g_bracket = isnav ? 3 : isthing ? 2 : 1;
                        if (isnav) hud_draw(g_hud, &g_hs_navmk, vx, vy, NULL);
                        else {
                            float R = isthing ? thr : g_sim.radius ? g_sim.radius[tgt] : 545.0f, hb = R / zf / (th * aspect) * kx;
                            /* a thing's (0x10022ea0): half the sphere's radius (x 0.5, 0x1024764c), at most half the view's
                             * width (+0x7c x 0.5) */
                            if (isthing) { hb *= 0.5f; if (hb > kx) hb = kx; }
                            const hud_sprite *gp = g_hs_gp[side];
                            if (gp[0].tex) {
                                hud_draw(g_hud, &gp[0], vx - hb, vy - hb, NULL); hud_draw(g_hud, &gp[1], vx + hb, vy - hb, NULL);
                                hud_draw(g_hud, &gp[2], vx - hb, vy + hb, NULL); hud_draw(g_hud, &gp[3], vx + hb, vy + hb, NULL);
                            } else hud_draw(g_hud, g_hs_tgt[side == 0 ? 1 : side == 2 ? 2 : 0].tex ? &g_hs_tgt[side == 0 ? 1 : side == 2 ? 2 : 0] : &g_hs_tgt[0], vx, vy, NULL);
                        }
                    } else {
                        float dx = vx - 320.0f, dy = vy - 240.0f, tt;
                        if (fabsf(dx) < 1e-3f && fabsf(dy) < 1e-3f) dx = 1e-3f;
                        tt = fminf(fabsf(dx) > 1e-6f ? kx / fabsf(dx) : 1e9f, fabsf(dy) > 1e-6f ? 240.0f / fabsf(dy) : 1e9f);
                        if (tt > 1.0f && zf > 1.0f) tt = 1.0f;
                        g_bracket = 4;
                        hud_draw(g_hud, &g_hs_off[isnav ? 0 : side], 320.0f + tt * dx, 240.0f + tt * dy, NULL);
                    }
                }
            }
            }
            /* radar, top left (CPIT rect 0): yellow circle, view cone, own marker above centre, blips; the range per mode (g_radar_ri) */
            if (g_layout_n > 0 && (g_radar_mode != 0 || g_satmap) && (!hud_dead || g_satmap || rad_on)) {
                /* the window from the mode descriptor 0x1024a760 (screen fractions; CPIT rect 0 is overwritten by it,
                 * 0x10003250): mode 1 (0.02, 0.02)-(0.26, 0.34), mode 2 (0.13, 0)-(0.87, 1.0); circle radius w / 2 - 1 */
                float cx = 89.6f, cy = 86.4f, rad = 75.8f;
                if (!g_radar_big && !g_satmap) HUD_SIDE(-1);   /* the corner radar (and its range label): left-hand */
                if (g_radar_big) { cx = 320.0f; cy = 240.0f; rad = 235.8f; }
                if (g_satmap) { cx = 320.0f; cy = 240.0f; rad = 420.0f; }       /* mode 4: full screen, north up */
                else if (hud_dead) rad *= rad_wf;   /* powering up / down: the window (and its circle) about the centre */
                /* DOS footage: own marker at the centre; the view cone is +-45 degrees about the torso's
                 * facing (it turns with the twist; the radar is aligned with the legs) */
                /* the corner and the large radar in the screen's pixels (rdw): the descriptor's window (mode 1 (1311, 1311)-
                 * (17039, 22282), mode 2 (8520, 0)-(57015, 65536) in 16.16 screen fractions, 0x10003250 -> 0x10005b70: x (w -
                 * 1) rounded); the circle about ((x1 - x0 + 1) >> 1, (y1 - y0 + 1) >> 1) with radius (x1 - x0 + 1) / 2 - 1
                 * (0x10006e80 / DOS 0x14410; DOS's y radius x the aspect 0xffff rounded, the same) drawn as the VFX
                 * ellipse in colour 2; the label at the window's fraction (0, 0.04) (mode 2 (0.08, 0.13); +0x60, 0x10003e50).
                 * DOSBox YELLSCN1: circle x 21-265, y 16-260 at 1024 x 768 (14-166, 11-163 at 640 x 480) in 190,146,0;
                 * label "R: 1.0km" at (20, 25) / (13, 16) - the port's float circle was 1.5 px wide in colour 3, its label
                 * 1 px off */
                const int rmode = g_radar_big ? 2 : 1, pxr = !g_satmap && !hud_dead;
                const hud_win rdw = rmode == 1 ? hud_window(0, 0, 0, 0, 0, 0xa3d) : hud_window(0, 0, 0, 0, 0x147b, 0x2148);
                hud_win rw = rdw;
                {
                    const int W1 = (int)lrintf(640.0f / rdw.ux) - 1, H1 = (int)lrintf(480.0f / rdw.uy) - 1;
                    rw.x0 = fx16_round((long long)(rmode == 1 ? 1311 : 8520) * W1); rw.x1 = fx16_round((long long)(rmode == 1 ? 17039 : 57015) * W1);
                    rw.y0 = fx16_round((long long)(rmode == 1 ? 1311 : 0) * H1); rw.y1 = fx16_round((long long)(rmode == 1 ? 22282 : 65536) * H1);
                    rw.rx = rw.x0 + fx16_round((long long)(rw.x1 - rw.x0) * (rmode == 1 ? 0 : 0x147b));
                    rw.ry = rw.y0 + fx16_round((long long)(rw.y1 - rw.y0) * (rmode == 1 ? 0xa3d : 0x2148));
                }
                const int rcx = rw.x0 + ((rw.x1 - rw.x0 + 1) >> 1), rcy = rw.y0 + ((rw.y1 - rw.y0 + 1) >> 1), rr = (rw.x1 - rw.x0 + 1) / 2 - 1;
                if (pxr) { cx = (float)rcx * rw.ux; cy = (float)rcy * rw.uy; rad = (float)rr * rw.ux; }
                float px = cx, py = cy, cone = atanf(1.0f / (g_zoom > 1 ? g_zoom : 1.0f)), tw = (g_pl.twist + (g_satmap ? g_pl.heading : 0.0f)) * 3.14159265f / 180.0f;   /* +-45 deg, narrowed by the zoom (DOS); north-up map: heading + twist */
                if (g_satmap) {   /* "Range: " (half the width) at (0.01, 0.01), "Bearing: %3.1lf" at (0.01, 0.05) */
                    char lb[32];
                    float hr = 500.0f / (float)(1 << g_sat_i), bh = fmodf(g_pl.heading + g_pl.twist + 720.0f, 360.0f);
                    if (hr >= 1000.0f) snprintf(lb, sizeof lb, "Range: %3.1lfkm", (double)hr / 1000.0); else snprintf(lb, sizeof lb, "Range: %3.1lfm", (double)hr);
                    hud_text(g_hud, 6.4f, 4.8f, lb, green);
                    snprintf(lb, sizeof lb, "Bearing: %3.1lf", (double)bh);
                    hud_text(g_hud, 6.4f, 24.0f, lb, green);
                }
                if (!pxr && !g_satmap) hud_circle(g_hud, cx, cy, rad, 1.5f, yellow);   /* (pxr: the circle and label after the blips) */
                if (pxr) {   /* the view cone (0x10003d40 / DOS 0x14670 -> 0x147c0): two lines in colour 1 (DOSBox 121,97,0) from the
                              * centre to the rim point (x1 - x0 + 1) / 2 - 2 out, floored, at the torso's bearing +-45 degrees
                              * (narrowed by the zoom). DOSBox YELLSCN1 at rest: (58, 53) and (229, 53) at 1024 x 768, exact; at 640 x
                              * 480 the right end (143, 35) is 1 px in from this (144, 34): DOS reaches the rim point through its own
                              * sine / arctangent tables with the aspect 0xffff (0x3a838, 0x3a9e5) - not reproduced */
                    int sd;
                    const float rim = (float)(rr - 1);
                    if (g_tex_kind == 3) {   /* the DOS look: MW2.EXE's integers (dos_radar_rim) - DOSBox 640 x 480: (143, 35) */
                        const int32_t half = glr_dos_atan2(0x10000, (int32_t)lrintf((g_zoom > 1 ? g_zoom : 1.0f) * 65536.0f));
                        const int32_t a0 = 0x5a0000 - (int32_t)((((int64_t)lrintf(g_pl.twist * 65536.0f)) % 0x1680000 + 0x1680000) % 0x1680000);
                        for (sd = -1; sd <= 1; sd += 2) {
                            int ex, ey;
                            dos_radar_rim(rw.x1 - rw.x0 + 1, rw.y1 - rw.y0 + 1, a0 + sd * half, w, h, &ex, &ey);
                            px_line(&rw, rcx, rcy, rw.x0 + ex, rw.y0 + ey, HP(1));
                        }
                    } else
                    for (sd = -1; sd <= 1; sd += 2) {
                        float ang = tw + cone * (float)sd;
                        px_line(&rw, rcx, rcy, rcx + (int)-floorf(-rim * sinf(ang)), rcy - (int)floorf(rim * cosf(ang)), HP(1));
                    }
                } else {   /* the view cone's sides end at the circle (DOS / PowerVR footage) */
                    int sd;
                    for (sd = -1; sd <= 1; sd += 2) {
                        float dx2 = sinf(tw + cone * (float)sd), dy2 = -cosf(tw + cone * (float)sd), ox = px - cx, oy = py - cy;
                        float b = dx2 * ox + dy2 * oy, cc = ox * ox + oy * oy - rad * rad, disc = b * b - cc, t2;
                        if (disc < 0) continue;
                        t2 = -b + sqrtf(disc);
                        hud_line(g_hud, px, py, px + dx2 * t2, py + dy2 * t2, 1.2f, yellow);
                    }
                }
                if (!g_satmap) { if (g_hs_ruser.tex) hud_draw(g_hud, &g_hs_ruser, px, py, NULL); else hud_rect(g_hud, px - 4.0f, py - 1.0f, 8.0f, 4.0f, green); }
                {   /* blips (0x10003830): nav points (0x10003c10) RNP (RRNP once reached), the targeted one RTGTNP /
                     * RRTGTNP; then 0x10003960: every unit but the player with +0x14 & 0x1400 and none of 0x4816 - so not
                     * destroyed (2 / 4) and not powered down (0x10: shut down, resting / held by a node) - and every thing
                     * with flags & 0x1400 (the dropships, gunships, engagement spheres...) and none of 0x1e, RGPF / E / N by
                     * side (0x10019040 / 0x10019060); then the target (0x10003a60, whatever its power state): RTGTGP (a
                     * plus; a thing without 0x1400: RTGTGT), RTGTOF at the edge when beyond the radar. DOSBox MAROSCN1: the
                     * resting Nova / Mad Dog (480 / 590 m) are not shown, the moving hover cars (things 0x400) are */
                    int nblip_u = 0, nblip_t = 0;
                    float ly = g_satmap ? 0.0f : g_pl.heading * 3.14159265f / 180.0f;
                    float rcm = g_satmap ? 50000.0f / (float)(1 << g_sat_i) : RADAR_CM[g_radar_i], rr2 = g_satmap ? 320.0f : rad;
                    int nvt = tgt < 0 ? current_nav(NULL) : -1;
                    for (q = -1 - g_sim.nav_count; q < g_actor_count + g_sim.bld_count; q++) {
                        float wx, wz, ddx, ddz, ax, ay, bx, by, dd2;
                        int sd = 1, istgt, nvq = -1, set3 = 0;
                        const hud_sprite *sp;
                        if (q >= g_actor_count) {   /* a thing (0x10003960's second list; the target 0x10003a60) */
                            const int b = q - g_actor_count;
                            float tp[3];
                            istgt = tgt < 0 && b == g_tgt_thing;
                            if (g_sim.bld[b].hp <= 0) continue;   /* destroyed (the thing's flag 4 / 2) */
                            if (!istgt && !(g_sim.bld[b].flags & 0x1400)) continue;   /* (bits 0x1e: runtime - destroyed - ASSUMED otherwise clear) */
                            if (!msim_thing_point(&g_sim, b, tp)) continue;
                            wx = tp[0]; wz = tp[2];
                            sd = g_sim.bld[b].side >= 0 && g_sim.bld[b].side <= 2 ? g_sim.bld[b].side : 2;
                            set3 = !(g_sim.bld[b].flags & 0x1400);
                        } else if (q >= 0) {
                            if (!g_sim.armed[q] || g_sim.units[q].destroyed) continue;
                            istgt = q == tgt;
                            if (!istgt && ((g_sim.powered_down && g_sim.powered_down[q]) || g_sim.units[q].shutdown)) continue;   /* +0x14 & 0x10 */
                            wx = (float)g_actors[q].mech.origin[0]; wz = (float)g_actors[q].mech.origin[2];
                            sd = g_actors[q].friendly ? 0 : g_actors[q].alliance == 2 ? 2 : 1;
                        } else if (q <= -2) {
                            nvq = -2 - q;
                            if (!g_sim.navs[nvq].selectable) continue;   /* engine 0x10003c10: shown, the player's side */
                            wx = g_sim.navs[nvq].x; wz = g_sim.navs[nvq].z; istgt = nvq == nvt;
                        } else continue;
                        ddx = (wx - (float)g_pl.origin[0]) / rcm;
                        ddz = (wz - (float)g_pl.origin[2]) / rcm;
                        ax = ddx * cosf(ly) - ddz * sinf(ly); ay = ddx * sinf(ly) + ddz * cosf(ly);   /* aligned with the legs (north on the map) */
                        bx = px + ax * rr2; by = py - ay * rr2;
                        dd2 = (bx - cx) * (bx - cx) + (by - cy) * (by - cy);
                        if (g_satmap) dd2 = (bx < 0 || bx > 640 || by < 0 || by > 480) ? 1e12f : 0.0f;
                        if (g_tex_kind == 3 && pxr) {   /* the DOS look: MW2.EXE's radar view (dos_radar_point) */
                            int rx2, ry2;
                            const int RW = rw.x1 - rw.x0 + 1, RH = rw.y1 - rw.y0 + 1;
                            dos_radar_point(RW, RH, wx, wz, g_sim.player_eye_ok ? g_sim.player_eye[0] : (float)g_pl.origin[0],
                                            g_sim.player_eye_ok ? g_sim.player_eye[2] : (float)g_pl.origin[2], g_pl.heading,
                                            (int)lrintf(rcm * 2.0f), w, h, &rx2, &ry2);
                            dd2 = dos_radar_inside(RW, RH, rx2, ry2, w, h) ? 0.0f : 1e12f;
                            if (dd2 > 0 && istgt) dos_radar_edge(RW, RH, rx2, ry2, w, h, &rx2, &ry2);
                            bx = (float)(rw.x0 + rx2) * rw.ux; by = (float)(rw.y0 + ry2) * rw.uy;
                        }
                        if (dd2 > rad * rad) {
                            /* beyond the radar: only the target, at the rim (0x10003a60: row 6 of the window's sprite table
                             * by side - a nav point's column 0, RTGTOFF; DOSBox YELLSCN1 n: Nav Epsilon at 1075 m on the 1 km
                             * radar as a green "+" on the circle - the port had dropped an out-of-range nav) */
                            if (!istgt) continue;
                            if (g_tex_kind == 3 && pxr) ;   /* (dos_radar_edge above) */
                            else if (pxr) {   /* the rim point as the cone's ends (DOS 0x14670 -> 0x147c0): (x1 - x0 + 1) / 2 - 2 out */
                                float ang = atan2f(bx - cx, cy - by), rim = (float)(rr - 1);
                                bx = (float)(rcx + (int)-floorf(-rim * sinf(ang))) * rw.ux; by = (float)(rcy - (int)floorf(rim * cosf(ang))) * rw.uy;
                            } else {   float l = sqrtf(dd2); bx = cx + (bx - cx) * rad / l; by = cy + (by - cy) * rad / l; }
                            sp = &g_hs_rtof[nvq >= 0 ? 0 : sd];
                        } else if (nvq >= 0) {
                            int rch = nvq < 256 && g_nav_seen[nvq];
                            sp = istgt ? &g_hs_rtnp[rch] : &g_hs_rnp[rch];
                        } else sp = istgt ? (set3 ? &g_hs_rtgt[sd] : &g_hs_rtgp[sd]) : &g_hs_rgp[sd];
                        if (sp->tex) hud_draw(g_hud, sp, bx, by, NULL);
                        else if (nvq < 0) hud_rect(g_hud, bx - 1.5f, by - 1.5f, 3.0f, 3.0f, blipc);
                        if (nvq < 0 && !istgt) { if (q >= g_actor_count) nblip_t++; else nblip_u++; }
                        if (nvq < 0 && !istgt && q >= g_actor_count && getenv("MW2_RADAR_LOG") && g_tex_kind == 3 && pxr && dd2 <= rad * rad)
                            printf("RADARTHING now=%d %d %d\n", (int)g_sim.now, (int)lrintf(bx / rw.ux), (int)lrintf(by / rw.uy));   /* TEST hook: a thing's blip, screen pixels */
                    }
                    if (getenv("MW2_RADAR_LOG")) {   /* TEST hook: the plain blips drawn (units, things) */
                        static double last = -1;
                        if (g_sim.now / 1000.0 >= last + 1.0) { last = g_sim.now / 1000.0; printf("RADAR t=%.1f units %d things %d\n", g_sim.now / 1000.0, nblip_u, nblip_t); }
                    }
                }
                /* the radar's order (0x10003790): the cone (0x10003d40), the blips, own marker and target (0x10003830), then
                 * the circle (+0x7c) and the label (0x10003e50) - DOSBox YELLSCN1 n: the ring's pixels over the nav's "+" */
                if (pxr) {
                    px_ellipse(&rw, rcx, rcy, rr, rr, HP(2));
                    hud_text(g_hud, (float)rw.rx * rw.ux, (float)rw.ry * rw.uy, RADAR_LABEL[g_radar_i], green);
                }
                HUD_SIDE(0);
            }
            /* altitude tape (engine 0x100222e0, DOS 0x28ed0; set-up 0x10022490 / DOS 0x290d0). Window 23 (DOS table entry 22) = (1,74)-(35,111) in
             * 320x200, converted to the screen's pixels as every window (0x10006000 then 0x10005b70: c << 16 / 319 or 199,
             * x (w - 1) or (h - 1), >> 16 rounded; 640x480: x 2-70, y 178-267; 1024x768: x 3-112, y 285-428); the reference
             * point (0.7, 0.5) of the window's size in PIXELS (0x10005ec0 / DOS 0x13510, rounded): 640x480 (48, 45),
             * 1024x768 (76, 72) - the window's middle in every sprite set (the port had kept the 640 set's (48, 45) as
             * sprite pixels, so with the 1024x768 K set the caret sat 27 px too high and 28 px too far left). It shows the
             * ABSOLUTE elevation of the feet (object y - controller +0xcc): ALTTAPE at x = the reference - its width
             * (0x10060b80 / DOS 0x58908: bbox w), y = the reference + (h - 201 m) x the tape's height / 232 m (4 px a
             * metre; 3Dfx float truncated by __ftol, DOS (v x scale16 / 100) >> 16 - floored), clipped to the window
             * with its bottom row (DOSBox: the tape drawn on row y1 too; the window's height +0x46 = y1 - y0 + 1, 0x2f0e0);
             * ALTTOP there above the reference line; ALTMKR1 the caret at the reference; ALTMKR2 the ground under the
             * mech one caret width right; the target's height: CRTLFT two caret widths right, or CRTUP / CRTDOWN three
             * widths right at the window's top / bottom when it lies above / below it */
            {
            if (!hud_dead && g_hs_alt.tex) {
                const hud_win aw = hud_window(1, 74, 35, 111, 0xb333, 0x8000);   /* DOS 0x9622c: (0.7, 0.5) */
                const float ux = aw.ux, uy = aw.uy, alt = pu->y / 100.0f, gnd = pu->ground / 100.0f;
                const int ry = aw.ry - aw.y0, wh = aw.y1 - aw.y0;
                const int mw = g_hs_altmk[0].tex ? g_hs_altmk[0].w : 6;   /* the caret's width (DOS DAT_00096224) */
                const float Y0 = (float)aw.y0 * uy, RX = (float)aw.rx * ux, RY = (float)aw.ry * uy;
                const int dos = g_tex_kind == 3;
                const int tpx = alt_tape_px(pu->y - 20100.0f, g_hs_alt.h, dos);
                HUD_SIDE(-1);   /* left-hand */
                float hy = RY + uy * (float)tpx;   /* the tape's hotspot */
                float v0 = (Y0 - hy) / uy - (float)g_hs_alt.top;
                hud_draw_window_v(g_hud, &g_hs_alt, v0, (float)(wh + 1), RX - (float)g_hs_alt.w * ux + ux * (float)g_hs_alt.left, Y0);
                if (tpx > 0) hud_draw(g_hud, &g_hs_alttop, RX - (float)g_hs_alt.w * ux, RY, NULL);
                if (g_hs_altmk[0].tex) hud_draw(g_hud, &g_hs_altmk[0], RX, RY, NULL);
                {
                    float gy = (float)(ry + alt_tape_px((alt - gnd) * 100.0f, g_hs_alt.h, dos));
                    if (g_hs_altmk[1].tex && gy >= 0 && gy <= (float)(wh + 1)) hud_draw(g_hud, &g_hs_altmk[1], RX + (float)mw * ux, Y0 + gy * uy, NULL);
                }
                {
                    int nv = tgt < 0 ? current_nav(NULL) : -1;
                    if (tgt >= 0 || nv >= 0) {
                        float ta = tgt >= 0 ? g_sim.units[tgt].y / 100.0f : (float)g_sim.navs[nv].y / 100.0f;
                        float ty = (float)(ry + alt_tape_px((alt - ta) * 100.0f, g_hs_alt.h, dos));
                        const hud_sprite *ar = g_hs_edge;   /* the same sprite for a nav (red) */
                        if (ty < 0) { if (ar[2].tex) hud_draw(g_hud, &ar[2], RX + (float)(3 * mw) * ux, Y0, NULL); }
                        else if (ty > (float)(wh + 1)) { if (ar[3].tex) hud_draw(g_hud, &ar[3], RX + (float)(3 * mw) * ux, Y0 + (float)(wh + 1) * uy, NULL); }
                        else if (ar[0].tex) hud_draw(g_hud, &ar[0], RX + (float)(2 * mw) * ux, Y0 + ty * uy, NULL);
                    }
                }
                HUD_SIDE(0);
            }
            /* weapon list, top right (CPIT rects 3-12): green ready, white ready but target out of range,
             * red recycling, grey empty / destroyed; box around the selected weapon */
            {
                int wi;
                HUD_SIDE(1);   /* right-hand */
                for (wi = 0; wi < pu->weapon_count && wi < 10 && g_layout_n > 12 && (!hud_dead || g_pow == 1); wi++) {
                    const combat_weapon *cw = &pu->weapons[wi];
                    const int win = weapon_window(pu, wi);
                    /* the weapon's window (CPIT rect 3-12, cpit_window) and its text point (0.07, 0.13) of the window
                     * (DOS 0x96ac0 entries 3-12 = 0x11ec / 0x2148, 3Dfx the same table at file 0x7ead8: +0x34, 0x10005ec0) */
                    const hud_win ww = cpit_window(win, 0x11ec, 0x2148);
                    char line[24];
                    const float *col;
                    if (hud_dead) {   /* start-up (LAB_1001e9a0): the name alone, green, once the mission reaches its window's
                                       * time (0x1024cce8), at the window's corner (0x10063e49 with 0, 0); a destroyed slot not at all */
                        static const int APPEAR[10] = {543, 573, 603, 633, 663, 814, 784, 754, 724, 693};
                        const float *sgreen = HP(14);
                        if (cw->state >= 0 && (float)g_sim.now * 0.182f >= (float)APPEAR[win - 3])
                            hud_text(g_hud, (float)ww.x0 * ww.ux, (float)ww.y0 * ww.uy, sim_weapon_short[cw->weapon], sgreen);
                        continue;
                    }
                    /* engine 0x1001e820, by the slot's state: destroyed / out of ammunition (-1) dark red (index 8);
                     * recycling red (0xb); a burst being fired green (0xe); ready: by its fire group - 1 green (0xe),
                     * 2 white (0xfe), 3 yellow (3) */
                    const float *wdark = HP(8), *wred = HP(0xb), *wgreen = HP(14), *wwhite = HP(0xfe), *wyellow = HP(3);   /* the mission
                                       * palette (0x10260324); the 0xe green drawn as 14 here (as before) */
                    (void)grey; (void)white;
                    if (cw->ammo == 0 || pu->loc_gone[cw->location]) col = wdark;
                    else if (cw->state == CW_FIRING) col = wgreen;
                    else if (cw->state != CW_READY) col = wred;
                    else if (g_groups[1] & (1u << wi)) col = wwhite;
                    else if (g_groups[2] & (1u << wi)) col = wyellow;
                    else col = wgreen;
                    if (cw->ammo >= 0) snprintf(line, sizeof line, "%s %d", sim_weapon_short[cw->weapon], cw->ammo);
                    else snprintf(line, sizeof line, "%s", sim_weapon_short[cw->weapon]);
                    hud_text(g_hud, (float)ww.rx * ww.ux, (float)ww.ry * ww.uy, line, col);
                    /* the selected weapon: the whole window framed in the text's colour (0x1001e980 -> 0x10006450; the
                     * port had boxed the text alone, 6-8 px left / 3-6 px up of DOSBox's text) */
                    if (wi == g_sel_weapon) px_frame(&ww, col);
                }
                HUD_SIDE(0);
            }
            }
            /* damage outline, bottom right (CPIT rect 2), per location (engine 0x1001b620); F5 hides it */
            HUD_SIDE(1);   /* right-hand: the viewport box, H T A L bars or damage outline */
            if (g_vport) {   /* the viewport box: blue frame, label sprite at the top centre (DOS) */
                const float *vblue = HP(6);   /* DOSBox YELLSCN1: 0,0,199 = YELL_DA 6 */
                /* window 2 (cpit_window): DOSBox YELLSCN1 F7 frame 827-991 x 559-686 at 1024 x 768 */
                const hud_win vw2 = cpit_window(2, 0, 0);
                float x0 = (float)vw2.x0 * vw2.ux, y0 = (float)vw2.y0 * vw2.uy, x1 = (float)vw2.x1 * vw2.ux, y1 = (float)vw2.y1 * vw2.uy, vwf = 1, vhf = 1;
                if (pow_window(1, &vwf, &vhf)) {   /* powering up / down: window 2 about its centre (0x10011aa0 / 0x10011b70) */
                    float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f, hw = (x1 - x0) * 0.5f * vwf, hh = (y1 - y0) * 0.5f * vhf;
                    int full = vwf >= 1.0f && vhf >= 1.0f;
                    x0 = mx - hw; x1 = mx + hw; y0 = my - hh; y1 = my + hh;
                    if (g_vport == 3 && !g_wcam_live) hud_rect(g_hud, x0, y0, x1 - x0, y1 - y0, black);   /* weapon view, nothing in flight: black (DOS) */
                    if (g_snow_show[0]) draw_snow(x0, y0, x1 - x0, y1 - y0);   /* display damage: SNOWCLR instead of the view (0x10011a40),
                                                                                  * in the moving box while it opens / closes */
                    if (full) px_frame(&vw2, vblue);
                    else {
                        hud_line(g_hud, x0, y0, x1, y0, 1.0f, vblue); hud_line(g_hud, x1, y0, x1, y1, 1.0f, vblue);
                        hud_line(g_hud, x1, y1, x0, y1, 1.0f, vblue); hud_line(g_hud, x0, y1, x0, y0, 1.0f, vblue);
                    }
                    if (!g_snow_show[0] && g_hs_vp[g_vport - 1].tex) {
                        /* the label (0x10011a70 -> 0x10023120: x = the window's left edge + half the instrument's full width
                         * (+0x44 / 2), y = its top + 2): while the window opens / closes it rides the moving left and top
                         * edges, clipped to the box */
                        if (!full) hud_clip(g_hud, x0, y0, x1 - x0, y1 - y0);
                        /* (DOSBox YELLSCN1 F7: REAR 7 px higher than the port's y0 + 6 units at 1024 x 768) */
                        hud_draw(g_hud, &g_hs_vp[g_vport - 1], x0 + (float)((vw2.x1 - vw2.x0 + 1) / 2) * vw2.ux, y0 + 2.0f * vw2.uy, NULL);
                        if (!full) hud_clip_off(g_hud);
                    }
                }
            } else if (g_htal && !g_dmg_off && !hud_dead) {
                /* H T A L (DOS, F6; engine 0x1001b890, DOS 0x2f790): armour bars hanging from a line a third down the
                 * lower window (0x5555 of its height, y 375.6), torsos as half-width front / rear pairs, blue letters
                 * (index 6). The scale is the mech's own: at the HUD's set-up (0x1001b230 / DOS 0x2f190) every
                 * location's starting front / rear armour is copied (0x1007c388 / DOS 0xfe0a8) and the largest FRONT
                 * value kept (0x1024c620 / DOS 0x96664); a bar is armour x half the window's height (0x8000 of it,
                 * converted to pixels at the set-up: 40 rows at 640x480) / that maximum, truncated (DOS 64-bit
                 * integer divide), so the most armoured location's bar is half the window whatever the mech (the
                 * port had drawn a fixed 1.125 units per MEK point: a Mad Dog's bars ~1.6x too short, every hit
                 * stripping too little). Each part is drawn as a vertical line run, both ends inclusive (0x1001c810 /
                 * DOS 0x31520): the armour left in green (0xf bands) from the line down to it, then the armour lost
                 * from there to the starting length - yellow (3) while more than a quarter of the start is left (DOS
                 * (start >> 2) < armour), else red (0xb), over the green's last row; a destroyed location all grey
                 * (0xf3). DOSBox AQUASCN1, Mad Dog (largest front 23 x 4 = CT / legs): 1024x768 full lengths
                 * H 26 / side torsos and arms 45 / CT and legs 65 / rears 20 px; an ER PPC hit on the CT -12 px */
                /* the bars' x (0x1007c340.. / DOS 0xfdf60..: 0x2323, 0x7373, 0x5a5a, 0x4141, 0xaaab, 0x9192, 0xe1e2,
                 * 0xc8c9 of the window's width for locations 0-7): left to right head, 3, 2, 1, 5, 4, 7, 6 - each pair
                 * the higher location first (the port had drawn 1..7 left to right, every pair mirrored; DOSBox
                 * AQUASCN1: the CT's partner in the DOS spread, location 1, is the RIGHT torso pair) */
                static const struct { int loc, rear; float x, w; } B[11] = {
                    {0, 0, 531.3f, 8.1f}, {3, 0, 543.1f, 3.75f}, {3, 1, 546.9f, 3.75f}, {2, 0, 553.1f, 3.75f}, {2, 1, 556.9f, 3.75f},
                    {1, 0, 563.1f, 3.75f}, {1, 1, 566.9f, 3.75f}, {5, 0, 575.0f, 8.1f}, {4, 0, 585.0f, 8.1f}, {7, 0, 597.5f, 8.1f}, {6, 0, 607.5f, 8.1f}};
                const float *hb = HP(14), *hl = HP(15), *hd = HP(13);   /* the band colours in the mission palette (0x10260324) */
                const float *yb = HP(2), *yl = HP(3), *ydk = HP(1);
                const float *rb = HP(10), *rl = HP(11), *rd = HP(9);
                const float *gb2 = HP(0xf2), *gl2 = HP(0xf3), *gd2 = HP(0xf1);
                const float *hblue = HP(6);
                const int64_t half = 40;   /* 0x8000 x the window's 80 rows (y 349.4-428.8, 0x10005ec0 rounds) */
                int64_t fmax = 0;
                int b2;
                for (b2 = 0; b2 < 8; b2++) if (pu->armor_max[b2] > fmax) fmax = pu->armor_max[b2];   /* front only, as both loops compare */
                hud_text(g_hud, 528.8f, 357.5f, "H", hblue); hud_text(g_hud, 549.4f, 357.5f, "T", hblue);
                hud_text(g_hud, 577.5f, 357.5f, "A", hblue); hud_text(g_hud, 601.3f, 357.5f, "L", hblue);
                for (b2 = 0; b2 < 11 && fmax > 0; b2++) {
                    int loc = B[b2].loc;
                    float cur = B[b2].rear ? pu->rear[loc] : pu->armor[loc];
                    int64_t mx = B[b2].rear ? pu->rear_max[loc] : pu->armor_max[loc];
                    int64_t len = (int64_t)(cur > 0 ? cur : 0) * half / fmax, full = mx * half / fmax;
                    float w3 = B[b2].w / 3.0f, y0 = 375.6f;
                    const float *mb = (float)mx * 0.25f < cur ? yb : rb, *ml = (float)mx * 0.25f < cur ? yl : rl,
                                *md = (float)mx * 0.25f < cur ? ydk : rd;
                    if (pu->loc_gone[loc]) { len = 0; mb = gb2; ml = gl2; md = gd2; }
                    if (len != 0) {   /* rows y0 .. y0 + len */
                        hud_rect(g_hud, B[b2].x, y0, w3, (float)len + 1.0f, hb);
                        hud_rect(g_hud, B[b2].x + w3, y0, w3, (float)len + 1.0f, hl);
                        hud_rect(g_hud, B[b2].x + 2 * w3, y0, w3, (float)len + 1.0f, hd);
                    }
                    if (len < full) {   /* rows y0 + len .. y0 + full */
                        hud_rect(g_hud, B[b2].x, y0 + (float)len, w3, (float)(full - len) + 1.0f, mb);
                        hud_rect(g_hud, B[b2].x + w3, y0 + (float)len, w3, (float)(full - len) + 1.0f, ml);
                        hud_rect(g_hud, B[b2].x + 2 * w3, y0 + (float)len, w3, (float)(full - len) + 1.0f, md);
                    }
                }
            } else if (g_layout_n > 2 && !g_dmg_off && !hud_dead) {
                static const int REG_LOC[15] = {1, 3, 3, 2, 2, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8};
                /* centred in window 2 (set-up 0x1001b230 / DOS 0x2f190): x0 + (W - the outline's width) / 2, y0 + (H -
                 * its height) / 2, W / H the window's +0x44 / +0x46 (x1 - x0 + 1, y1 - y0 + 1), the sprite's size its
                 * bounds (0x10060b80 / DOS 0x58908), C division. The port had drawn it at the window's corner: (16, 4)
                 * px off at 640 x 480, (47, 29) at 1024 x 768 (DOSBox YELLSCN1 Timber Wolf: 532, 352 / 873, 586) */
                const hud_win dw = cpit_window(2, 0, 0);
                const int sw = g_hs_dmg_lvl[0].w, sh = g_hs_dmg_lvl[0].h;
                float dx = (float)(dw.x0 + (dw.x1 - dw.x0 + 1 - sw) / 2) * dw.ux, dy = (float)(dw.y0 + (dw.y1 - dw.y0 + 1 - sh) / 2) * dw.uy;
                /* the whole outline first in its own colour 6 (0x1001b620: 0x10062241 at the window's 0, 0), then each region
                 * whose colour is not 6 again, remapped (DOSBox: the outline's pixels outside every region rectangle -
                 * the side torsos' lower edges - are drawn; the port had drawn the regions alone) */
                hud_draw(g_hud, &g_hs_dmg_lvl[0], dx, dy, NULL);
                for (q = 0; q < 15; q++) {
                    int loc = REG_LOC[q] - 1, lf = 0, lr = 0, lv, pick;
                    const int *rg = g_dmg_regions[q];
                    if (rg[2] <= 0 || rg[3] <= 0) continue;
                    if (pu->armor_max[loc] > 0) lf = 15 - 15 * (int)pu->armor[loc] / pu->armor_max[loc];
                    if (pu->rear_max[loc] > 0) lr = 15 - 15 * (int)pu->rear[loc] / pu->rear_max[loc];
                    lv = lf > lr ? lf : lr;
                    pick = pu->loc_gone[loc] ? 3 : lv <= 0 ? 0 : lv < 12 ? 1 : 2;
                    if (pick == 0) continue;
                    hud_draw_region(g_hud, &g_hs_dmg_lvl[pick], dx, dy, (float)rg[0] * 2.0f * g_hud_s, (float)rg[1] * 2.0f * g_hud_s, (float)rg[2] * 2.0f * g_hud_s, (float)rg[3] * 2.0f * g_hud_s);
                }
            }
            HUD_SIDE(0);
            if (!hud_dead && g_autopilot)   /* engine 0x100183d0: "AUTOPILOT" (0x1024c2d0 / 0x1024c2dc) in the HUD font's
                                             * colour (0xe) while the player's autopilot (controller +0xbc) is 1 or 2; DOSBox
                                             * YELLSCN1 a: caps 177-240 x 732-742 at 1024x768, gone once a turn cancels it */
                { HUD_SIDE(-1); hud_text(g_hud, 110.0f, 456.0f, "AUTOPILOT", green); HUD_SIDE(0); }   /* bottom left: left-hand */
            if (!hud_dead) {   /* the instruments (dark while shut down) */
            /* tall bar, right edge of CPIT rect 1 (engine 0x1001c5b0, geometry 0x1001c250): the THROTTLE SETTING, its
             * shown value easing halfway to the setting on every draw (cur += (target - cur) >> 1, 16.16; stepped at the
             * agreed 30 fps - the DOSBox ramp 57 / 67 / 74 / 76 px is DOSBox's ~12 fps). Zero line at bottom - H/2 - 2 (34 % up); forward fills up in green
             * bands (0xf), full throttle = the whole span; reverse fills down in blue bands (7), full reverse = the lower
             * span; the fill is h + 1 rows, so zero throttle leaves a single line; frame red (0xa) */
            if (g_layout_n > 1) {
                /* DOS: frame columns 1001-1017, rows 542-752 at 1024x768 */
                HUD_SIDE(1);   /* right-hand: the bar and the kph line */
                /* window 17 (DOS table 0x968e0 (275,130)-(317,195), the kph's; 3Dfx instrument 18) with (vx, vy) = (0.119,
                 * 0.554) of its size (DOS 0x96888 = 0x1e7a / 0x8dc9, 3Dfx 0x1024c858; set-up DOS 0x30fc0 / 0x1001c250),
                 * W / H = x1 - x0 + 1 / y1 - y0 + 1: the frame (colour 0xa) columns W - vx - 1 and W - 1, rows H - (vy +
                 * vy / 2) - 3 to H - 1; the zero line H - vy / 2 - 2. The fill (DOS 0x31300 / 0x1001c5b0): v = throttle x
                 * vy / 0x400 (16.16, eased, >> 16) capped at vy, from the zero line up v + 1 more rows in green bands
                 * (0xf), columns W - vx..W - 2; reversing (+0x2f) half that, capped at vy / 2, from the zero line + v
                 * up to it in blue (7). DOSBox YELLSCN1 at rest: frame 1001-1017 x 542-752 and the fill rows 681-682 at
                 * 1024 x 768, 625-635 x 338-469 and rows 424-425 at 640 x 480 (the port's 640-unit bar was 1 px off) */
                const hud_win tw = hud_window(275, 130, 317, 195, 0x1e7a, 0x8dc9);
                const int TW = tw.x1 - tw.x0 + 1, TH = tw.y1 - tw.y0 + 1, vx = tw.rx - tw.x0, vy = tw.ry - tw.y0;
                const int c0 = TW - vx, top = TH - (vy + vy / 2) - 3, zero = TH - vy / 2 - 2;
                float th2 = fabsf(g_throttle);
                if (th2 > 1) th2 = 1;
                {   /* x = (setting << 16) x vy / 0x400 (setting 0..1024: 3Dfx ftol(+8 x 65536 + 0.5), DOS +8), reversing
                     * -(x / 2); smoothed s += (x - s) >> 1 a frame (DOS 0x96864 / 3Dfx 0x1024c828, see g_heat_s), v = s >> 16
                     * (DOS 0x313ae..0x31419, 3Dfx 0x1001c67d..0x1001c6dc); the branch by the reverse flag (+0x2f), so a
                     * fill still on the other side of the zero line after a switch is not drawn (count v + 1 <= 0) */
                    const int32_t X = (int32_t)(((long long)(int32_t)(th2 * 1024.0f + 0.5f) << 16) * vy / 0x400);
                    int n = hud_filter_steps(&g_thr_acc, &g_thr_seen);
                    while (n-- > 0) g_thr_s += ((g_reverse ? -(X / 2) : X) - g_thr_s) >> 1;
                    if (!g_reverse) {
                        int v2 = g_thr_s >> 16;
                        if (v2 > vy) v2 = vy;
                        g_thr_p = v2;
                        px_band_v(&tw, c0, zero, vx - 1, v2 + 1, 0xf);
                    } else {
                        int r = -(g_thr_s >> 16);
                        if (r > vy / 2) r = vy / 2;
                        g_thr_p = -r;
                        px_band_v(&tw, c0, zero + r, vx - 1, r + 1, 7);
                    }
                }
                {
                    const float *fr = HP(10);   /* frame colour 0xa (0x1001c5b0) */
                    px_fill(&tw, tw.x0 + c0 - 1, tw.y0 + top, tw.x0 + c0 - 1, tw.y0 + TH - 1, fr);
                    px_fill(&tw, tw.x0 + TW - 1, tw.y0 + top, tw.x0 + TW - 1, tw.y0 + TH - 1, fr);
                    px_fill(&tw, tw.x0 + c0 - 1, tw.y0 + top, tw.x0 + TW - 1, tw.y0 + top, fr);
                    px_fill(&tw, tw.x0 + c0 - 1, tw.y0 + TH - 1, tw.x0 + TW - 1, tw.y0 + TH - 1, fr);
                }
                {   /* DOS footage: "NN kph" under it, right-aligned left of the bar */
                    char kph[16];
                    /* engine 0x10018470: |v| in cm/tick x 6.516 x 1.5, truncated; minus when reversing */
                    {   /* DOS 0x32085 (3Dfx 0x10018470 the same 1.5): |v| estimated as (4 max + mid + min) >> 2 of the velocity's
                         * |x|, |y|, |z| (16.16 cm/tick), / 10002 (= 1 km/h) truncated, x 1.5 truncated (0x5c3c2: frndint with
                         * the control word's rounding set to chop) - the port had rounded the last step (91.5 -> 92). The
                         * vector is the controller's whole velocity +0xf4 / +0xf8 / +0xfc (c[0x3d..0x3f]), the sign c[0xb]'s
                         * (+0x2c): the walking velocity plus the directional jets' drift plus the vertical speed - DOSBox
                         * ORANSCN1 Nova, forward jets (GreyHome) from a standstill: 60 / 141 / 232 / 298 / 364 / 391 kph
                         * (the port had left the drift out and read 0) */
                        float hh = g_pl.heading * 3.14159265f / 180.0f, k16 = 65536.0f / 182.0f;
                        float vx = (sinf(hh) * g_pl_speed + g_jet_vx) * k16, vz = (cosf(hh) * g_pl_speed + g_jet_vz) * k16;
                        int32_t ax = (int32_t)fabsf(vx), az = (int32_t)fabsf(vz), ay = (int32_t)(fabsf(g_sim.player_unit.vy) * 65536.0f), t3;
                        if (ax < az) { t3 = ax; ax = az; az = t3; }
                        if (ax < ay) { t3 = ax; ax = ay; ay = t3; }
                        snprintf(kph, sizeof kph, "%d kph", (int)((double)(((4 * ax + az + ay) >> 2) / 10002) * 1.5) * (g_spd_srv < 0 ? -1 : 1));   /* the sign c[0xb]'s (+0x2c, 0x100184bf) */
                    }
                    if (getenv("MW2_KPH_TRACE")) fprintf(stderr, "kph %.2fs %s (%.1f cm/s, heading %.2f)\n", g_sim.now / 1000.0, kph, g_pl_speed, g_pl.heading);   /* tests */
                    {   /* at instrument 17's window (DOS table entry 17: (275,130)-(317,195)) and its reference (0, 0.9) (0x96ac0 ->
                         * +0x34, drawn there by 0x32085; 3Dfx 0x10018470 the same): 640 x 480 (551, 453), 1024 x 768 (882, 727)
                         * (the port had (551, 453) in HUD units: 2 px high at 1024 x 768). Reversing (+0x2c < 0) in blue,
                         * colour 6 (DOS 0x3210d, 3Dfx 0x10018470), else 0xe */
                        const hud_win kw = hud_window(275, 130, 317, 195, 0, 0xe666);
                        hud_text(g_hud, (float)kw.rx * kw.ux, (float)kw.ry * kw.uy, kph, g_spd_srv < 0 ? HP(6) : green);
                    }
                }
                HUD_SIDE(0);
            }
            }
            /* target viewer, bottom left (CPIT rect 13); F4 hides it. Powering up / down: the window about its centre
             * (0x10021730 / 0x100217f0, see pow_update) - the box alone while it moves */
            float tv_wf = 1, tv_hf = 1;
            if (g_layout_n > 13 && !g_tgtdisp_off && pow_window(1, &tv_wf, &tv_hf)) {
                /* window 13 (cpit_window): DOSBox YELLSCN1 frame 20-122 x 349-428 at 640 x 480, 32-196 x 559-686 at
                 * 1024 x 768 (the port's 640-unit box was 2 px wider and 1-2 px taller); the text lines in window 14 */
                const hud_win tvw = cpit_window(13, 0, 0), txw = cpit_window(14, 0x28f, 0x28f);
                HUD_SIDE(-1);   /* left-hand: the box, its picture and text lines */
                float bx = (float)tvw.x0 * tvw.ux, by = (float)tvw.y0 * tvw.uy, bw = (float)(tvw.x1 - tvw.x0) * tvw.ux, bh = (float)(tvw.y1 - tvw.y0) * tvw.uy;
                int shaded = (g_tgtdisp == 2 || (g_tgtdisp == 1 && g_tex_kind != 3)) && tgt >= 0 && g_cockpit && g_pow == 2;   /* drawn in 3D before the HUD */
                int full = tv_wf >= 1.0f && tv_hf >= 1.0f;
                if (!full) { bx += bw * (1.0f - tv_wf) * 0.5f; by += bh * (1.0f - tv_hf) * 0.5f; bw *= tv_wf; bh *= tv_hf; }
                const int snow = g_snow_show[1];   /* display damage: SNOWCLR instead of the contents (0x10021710), also in the moving box */
                if (!shaded) {   /* cleared to colour 0, opaque (0x100628a1; DOSBox 0,0,0): the whole window, both edges */
                    if (full) px_fill(&tvw, tvw.x0, tvw.y0, tvw.x1, tvw.y1, HP(0));
                    else hud_rect(g_hud, bx, by, bw, bh, HP(0));
                }
                if (snow) draw_snow(bx, by, bw, bh);
                if (g_pow != 2 && tgt < 0 && g_nav_sel >= 0 && current_nav(NULL) >= 0 && g_hs_vtgt_nav.tex && !snow) {
                    /* starting up / shut down (0x10021730 / 0x100217f0 -> 0x10021300 with the window set to the moving
                     * rectangle): a selected nav point's emblem (0x109 / 0x106 at half the rectangle's width and height,
                     * 0x10023120) - centred, clipped to the box as it opens or closes; a mech or building target: black */
                    hud_clip(g_hud, bx, by, bw, bh);
                    hud_draw(g_hud, &g_hs_vtgt_nav, bx + bw * 0.5f, by + bh * 0.5f, NULL);
                    hud_clip_off(g_hud);
                }
                {   /* the frame in colour 8 (0x10006450 with edx 8 on every 0x10021300 path; DOSBox 60,0,0) */
                    const float *tfr = HP(8);
                    if (full) px_frame(&tvw, tfr);
                    else {   /* the moving box while it opens / closes */
                        hud_line(g_hud, bx, by, bx + bw, by, 1.2f, tfr); hud_line(g_hud, bx + bw, by, bx + bw, by + bh, 1.2f, tfr);
                        hud_line(g_hud, bx + bw, by + bh, bx, by + bh, 1.2f, tfr); hud_line(g_hud, bx, by + bh, bx, by, 1.2f, tfr);
                    }
                }
                if (full && g_pow == 2) {   /* (not while starting up / shut down: the box alone - DOSBox YELLSCN1 / TNJ1SCN1 start-up)
                     * DOS footage: a mech target shows its damage outline, name below in red, range in green;
                     * otherwise the current nav point: VTGT_NP6 emblem, name and range below in yellow */
                    char info[48];
                    /* the text in window 14 (DOS 0x2fc95 / 3Dfx 0x10021150): the name at the window's corner (its text
                     * routine +0x58 / 0x10063e49 with 0, 0); the range "\n%3ldm" / "\n%2.2fk" through the wrapping text
                     * routine (DOS 0x13ad0 / 3Dfx 0x100066c0): inset by (0.01, 0.01) of the window (DOS 0x95748 / 0x9574c
                     * = 0x28f, rounded), one font height a line, so on the second line. DOSBox YELLSCN1: name 32 / 705,
                     * range 35 / 720 at 1024 x 768 (20 / 440, 22 / 454 at 640 x 480); the port had 6 px below the box and
                     * an 18 px line */
                    const int fhp = (int)lrintf(hud_text_height(g_hud) / txw.uy);
                    const float ty = (float)txw.y0 * txw.uy, nx = (float)txw.x0 * txw.ux, rgx = (float)txw.rx * txw.ux,
                                rgy = (float)(txw.ry + fhp) * txw.uy;
                    if (tgt >= 0) {
                        dmg_set *ds = NULL;
                        if (getenv("MW2_TARGET_OUTLINE") && g_sim.armed[tgt]) ds = target_outline(g_ma ? g_ma : NULL, g_actors[tgt].skel);   /* the damage outline instead (tests) */
                        if (!shaded && !snow) {   /* the target's wireframe, from where the player stands */
                            const world_actor *ta2 = &g_actors[tgt];
                            float ddx = (float)g_pl.origin[0] - (float)ta2->mech.origin[0], ddz = (float)g_pl.origin[2] - (float)ta2->mech.origin[2];
                            float brg = atan2f(ddx, ddz) * 180.0f / 3.14159265f;
                            static const float wblue[4] = {0.0f, 48.0f / 255.0f, 215.0f / 255.0f, 1.0f};
                            /* the posed parts are already in the world (heading applied): only the viewing direction turns */
                            {   /* the viewer's camera (wire_camera): level at the mech's centre, 3 R back along the bearing */
                                float sr0 = g_wire_r[0], sr1 = g_wire_r[1], sf0 = g_wire_f[0], sf1 = g_wire_f[1];
                                float R = g_sim.radius ? g_sim.radius[tgt] : 545.0f, ch = g_sim.centre_h ? g_sim.centre_h[tgt] : 550.0f;
                                (void)brg; (void)wblue;
                                wire_camera((float)ta2->mech.origin[0], (tgt < g_sim.actor_count ? g_sim.units[tgt].y : (float)ta2->mech.origin[1]) + ch, (float)ta2->mech.origin[2], R);
                                g_wire_unit = g_sim.armed[tgt] ? &g_sim.units[tgt] : NULL;
                                draw_target_wire(&ta2->mech, 0, ta2->mech.part_count, head, bx, by, bw, bh, HP(7));   /* colour 7 (0x28810's first argument) */
                                g_wire_unit = NULL; g_wire_persp = 0;
                                g_wire_r[0] = sr0; g_wire_r[1] = sr1; g_wire_f[0] = sf0; g_wire_f[1] = sf1;
                            }
                        }
                        if (ds && !snow) {
                            float ow = hud_px_x(g_hud, (float)ds->lvl[0].w), oh = hud_px_y(g_hud, (float)ds->lvl[0].h);
                            draw_outline((const hud_sprite *)ds->lvl, (const int (*)[4])ds->regions, &g_sim.units[tgt], bx + (bw - ow) * 0.5f, by + (bh - oh) * 0.5f);
                        }
                        {   /* the text line (0x10020e40): the inspection result for 0x16a ticks, else the name (unit +0xe8,
                             * "Mech" when empty), in the side's colour (0x1002119f: alliance 0 colour 0xe, 1 0xa, 2 6) */
                            const char *it = inspect_text(tgt, -1);
                            hud_text(g_hud, nx, ty, it ? it : g_actors[tgt].name[0] ? g_actors[tgt].name : "Mech",
                                     g_actors[tgt].alliance == 2 ? HP(6) : g_actors[tgt].friendly ? HP(0xe) : HP(0xa));
                        }
                        range_text(info, sizeof info, tdist);
                        hud_text(g_hud, rgx, rgy, info, green);
                    } else if (g_tgt_thing >= 0 && g_tgt_thing < g_sim.bld_count && (g_sim.bld[g_tgt_thing].hp <= 0 ||
                               (g_world && g_sim.bld[g_tgt_thing].intact >= 0 && g_sim.bld[g_tgt_thing].intact < g_mech.part_count && g_mech.parts[g_sim.bld[g_tgt_thing].intact].hidden))) {
                        g_tgt_thing = -1;   /* destroyed: the target is dropped (engine 0x10046a90 clears DAT_1025a664) */
                    } else if (g_tgt_thing >= 0 && g_tgt_thing < g_sim.bld_count) {   /* a structure: its name, then (inspected)
                                                                                         * what it holds, and the range */
                        const struct msim_building *gb = &g_sim.bld[g_tgt_thing];
                        float cx = (gb->mn[0] + gb->mx[0]) * 0.5f - (float)g_pl.origin[0], cz = (gb->mn[2] + gb->mx[2]) * 0.5f - (float)g_pl.origin[2];
                        if (!snow && g_world && gb->intact >= 0 && gb->intact < g_mech.part_count) {   /* the structure's wireframe */
                            static const float wblue[4] = {0.0f, 48.0f / 255.0f, 215.0f / 255.0f, 1.0f};
                            const mech3d_part *bp = &g_mech.parts[gb->intact];
                            float brg = atan2f(-cx, -cz) * 180.0f / 3.14159265f;
                            mech3d one;
                            memset(&one, 0, sizeof one);
                            one.parts = (mech3d_part *)bp; one.part_count = 1;
                            {   /* world placed; the camera on the structure's centre, its radius the box's half diagonal
                                 * (the engine's bounding sphere +0x34..+0x40, 0x10029890 - the port keeps a box) */
                                mech3d_part tmp = *bp;
                                float sr0 = g_wire_r[0], sr1 = g_wire_r[1], sf0 = g_wire_f[0], sf1 = g_wire_f[1];
                                float hx = (gb->mx[0] - gb->mn[0]) * 0.5f, hy = (gb->mx[1] - gb->mn[1]) * 0.5f, hz = (gb->mx[2] - gb->mn[2]) * 0.5f;
                                one.parts = &tmp;
                                (void)brg; (void)wblue;
                                wire_camera((gb->mn[0] + gb->mx[0]) * 0.5f, (gb->mn[1] + gb->mx[1]) * 0.5f, (gb->mn[2] + gb->mx[2]) * 0.5f, sqrtf(hx * hx + hy * hy + hz * hz));
                                draw_target_wire(&one, 0, 1, head, bx, by, bw, bh, HP(7));
                                g_wire_persp = 0;
                                g_wire_r[0] = sr0; g_wire_r[1] = sr1; g_wire_f[0] = sf0; g_wire_f[1] = sf1;
                            }
                        }
                        {   /* one text line (0x10020e40): the inspection result for 0x16a ticks, else the name ("Installation"
                             * when empty); its colour by the thing's alliance (0x10021167) - not kept by the port: red */
                            const char *it = inspect_text(-1, g_tgt_thing);
                            hud_text(g_hud, nx, ty, it ? it : gb->name[0] ? gb->name : "Installation", red);
                        }
                        range_text(info, sizeof info, sqrtf(cx * cx + cz * cz));
                        hud_text(g_hud, rgx, rgy, info, green);
                    } else {
                        const char *label = NULL;
                        /* only a nav point the player selected (NEXT / PREV_NAVPOINT): the viewer draws the player's target
                         * record +0xdc, black when it is none (0x10021300) - DOSBox YELLSCN1 / TNJ1SCN1: the box stays black
                         * from the start (60 s) with no nav selected; the port had shown the current objective's nav */
                        int nv = g_nav_sel >= 0 ? current_nav(&label) : -1;
                        if (nv >= 0) {
                            if (g_hs_vtgt_nav.tex && !snow) hud_draw(g_hud, &g_hs_vtgt_nav, (float)(tvw.x0 + (tvw.x1 - tvw.x0) / 2) * tvw.ux, (float)(tvw.y0 + (tvw.y1 - tvw.y0) / 2) * tvw.uy, NULL);   /* at ((x1 - x0) / 2, (y1 - y0) / 2) of the window (0x100213d1, C division): DOSBox (114, 622) at 1024 x 768 */
                            hud_text(g_hud, nx, ty, label, HP(2));   /* DOSBox YELLSCN1 "Nav Epsilon": 190,146,0 = index 2 */
                            /* the range as for any target (the player's +0xc4, formatted by range_text) in the text
                             * routine's colour 0xe (DOS 0x30180 / 3Dfx 0x1002118f reset it after the name): DOSBox YELLSCN1
                             * "1.08k" in green for Nav Epsilon at 1075 m (the port had " 1075m" in the name's yellow) */
                            range_text(info, sizeof info, sqrtf((g_sim.navs[nv].x - (float)g_pl.origin[0]) * (g_sim.navs[nv].x - (float)g_pl.origin[0]) +
                                                                (g_sim.navs[nv].z - (float)g_pl.origin[2]) * (g_sim.navs[nv].z - (float)g_pl.origin[2])));
                            hud_text(g_hud, rgx, rgy, info, green);
                        }
                    }
                }
                HUD_SIDE(0);
            }
            if (!hud_dead) {   /* the instruments (dark while not up) */
            /* bottom centre: "Heat" (0x1001c350 / DOS 0x310b0), "dH/dT" (0x1001c4a0 / DOS 0x311f0) and, for a
             * jump-capable mech, "Jets" (0x1001c750 / DOS 0x31480), in instrument windows 19 / 20 / 21 (DOS table
             * 0x968e0: (110,175)-(169,189), (170,175)-(209,189), (210,175)-(249,189); 3Dfx 20-22). Each bar (set-up
             * 0x1001c250 / DOS 0x30fc0) starts at the window's (0, 0.2) and is (0.915, 0.333) of it (heat; dH/dT and jets
             * 0.872 wide; DOS 0x968a8.., 3Dfx 0x1024c878..), rounded, one row less tall; its label at the window's
             * (0, 0.65) (0x96ac0 entries 19-21). DOSBox YELLSCN1: heat x 353-526, dH/dT 545-654, rows 685-701, labels at
             * y 709 (1024 x 768); 220-329 / 341-409, rows 428-437, labels y 443 (640 x 480) - the port had 640-unit
             * bars (1024 x 768: x 351-527, rows 685-699, labels 3 px high) */
            {
                static float prev_heat = -1, rate = 0;
                static int32_t prev_t;
                float dt = prev_t ? (float)(g_sim.now - prev_t) / 1000.0f : 0;
                const hud_win hw = hud_window(110, 175, 169, 189, 0, 0x3333), dw = hud_window(170, 175, 209, 189, 0, 0x3333);
                const hud_win jw = hud_window(210, 175, 249, 189, 0, 0x3333);
                const int hlen = fx16_round((long long)(hw.x1 - hw.x0) * 0xea4e), dlen = fx16_round((long long)(dw.x1 - dw.x0) * 0xdf2e);
                const int jlen = fx16_round((long long)(jw.x1 - jw.x0) * 0xdf2e);
                const int bh = fx16_round((long long)(hw.y1 - hw.y0) * 0x5555) - 1, by = hw.ry - hw.y0;
                const int ly = fx16_round((long long)(hw.y1 - hw.y0) * 0xa666);   /* the labels' row (0.65) */
                if (prev_heat >= 0 && dt > 0) rate += ((pu->heat - prev_heat) / dt - rate) * (dt > 0.25f ? 1.0f : dt * 4.0f);
                prev_heat = pu->heat;
                prev_t = g_sim.now;
                {
                    /* heat: p = heat x length / 100 (16.16, >> 16). p >= length: all red (0xb); p < 1: all blue (7); under
                     * half the length: yellow (3) 0..p, blue p + 1..length - p + 1, yellow from there p on; from half on the
                     * same with e = length - p and red between (the painter's spans are inclusive, so a part-filled bar is
                     * one column longer: DOS 0x310b0 / 3Dfx 0x1001c437). p is smoothed, shift 1 a frame (see g_heat_s):
                     * the DOS look in 16.16 (x = 16.16 heat x length / 100, p = s >> 16: DOS 0x310f0..0x3110e), the 3D
                     * looks in whole pixels (x = trunc(heat) x length / 100, p = s: 3Dfx 0x1001c36a..0x1001c3bc) - so a
                     * rising bar stops a step short (x - s = 1 >> 1 = 0) */
                    int p, n = hud_filter_steps(&g_heat_acc, &g_heat_seen);
                    const int32_t hx = g_tex_kind == 3 ? (int32_t)((long long)(int32_t)((double)pu->heat * 65536.0) * hlen / 100)
                                                       : (int32_t)((long long)(int32_t)pu->heat * hlen / 100);
                    g_heat_x = hx;
                    while (n-- > 0) g_heat_s += (hx - g_heat_s) >> 1;
                    p = g_heat_p = g_tex_kind == 3 ? g_heat_s >> 16 : g_heat_s;
                    if (p >= hlen) px_band_h(&hw, 0, by, hlen, bh, 0xb);
                    else if (p < 1) px_band_h(&hw, 0, by, hlen, bh, 7);
                    else {
                        int e = p < (hlen >> 1) ? p : hlen - p, mid = p < (hlen >> 1) ? 7 : 0xb, mw = hlen - 2 * e;
                        px_band_h(&hw, 0, by, e, bh, 3);
                        px_band_h(&hw, e + 1, by, mw, bh, mid);
                        px_band_h(&hw, e + 1 + mw, by, e, bh, 3);
                    }
                }
                {   /* engine 0x1001c4a0: each frame x = trunc(heat added - frame ticks x sinking) << 10 (the frame's net
                     * heat change, whole units), smoothed s += (x - s) >> 4 (0x1003ba60); s < 1 blue, under 0x300 yellow
                     * s / 0x300 of the bar over blue, past it red (s - 0x300) / 0x300 over yellow; the second part from
                     * the first's end (0x1001c596). The original's frames: 30 a second ASSUMED (the 3Dfx loop is
                     * uncapped) - sampled here every 1/30 s */
                    static int32_t sm, acc_t;
                    static float h0 = -1;
                    int32_t s2;
                    int f = 0, c1 = 7, c2 = 7;
                    float dnet = 0;
                    if (h0 < 0) { h0 = pu->heat; acc_t = g_sim.now; ((combat_unit *)pu)->dhdt_net = 0; }
                    if (g_sim.now - acc_t >= 33) {   /* (DOS: the net since the last sample, shared by the 1/30 s steps) */
                        dnet = pu->dhdt_net / (float)((g_sim.now - acc_t) / 33);
                        ((combat_unit *)pu)->dhdt_net = 0;
                    }
                    while (g_sim.now - acc_t >= 33) {
                        float dh = pu->heat - h0;
                        int32_t x = (int32_t)dh * 1024;   /* ftol truncates toward zero */
                        /* the DOS look (MW2.EXE 0x311f0): x = (+0x94 - +0x9c x ticks) >> 6 - the frame's heat gain less
                         * its dissipation in 16.16, floored to 1/1024 units and not held at 0 (the 3D editions truncate to
                         * whole units first): a cool mech's s settles below 0 (-sink x ticks x 1024) and a small steady
                         * gain shows; the same filter (0x49cf0 = 0x1003ba60, shift 4) and bar */
                        if (g_tex_kind == 3) x = (int32_t)(((int64_t)floor((double)dnet * 65536.0)) >> 6);
                        sm += (x - sm) >> 4;
                        h0 = pu->heat; acc_t += 33;
                        if (g_sim.now - acc_t > 1000) acc_t = g_sim.now;
                    }
                    s2 = g_dhdt_s = sm;
                    if (s2 < 1) s2 = 0;
                    else if (s2 < 0x300) { c1 = 3; c2 = 7; }
                    else { c1 = 0xb; c2 = 3; s2 -= 0x300; }
                    if (s2 > 0) { f = (int)((long long)dlen * s2 / 0x300); if (f < 0) f = 0; if (f > dlen) f = dlen; }
                    if (f > 0) px_band_h(&dw, 0, by, f, bh, c1);
                    if (f < dlen) px_band_h(&dw, f, by, dlen - f, bh, c2);
                    (void)rate;
                }
                /* engine 0x100185e0: the heat bar's label - flag 4 (shutdown sequence) "Shutdown..." in red (0xb), with flag 8
                 * (overridden) "Overridden" in red, else "Heat" in green (0xe) */
                if (pu->shutdown_pending || pu->shutdown) hud_text(g_hud, (float)hw.x0 * hw.ux, (float)(hw.y0 + ly) * hw.uy, pu->override ? "Overridden" : "Shutdown...", red);
                else hud_text(g_hud, (float)hw.x0 * hw.ux, (float)(hw.y0 + ly) * hw.uy, "Heat", green);
                hud_text(g_hud, (float)dw.x0 * dw.ux, (float)(dw.y0 + ly) * dw.uy, "dH/dT", green);
                if (pu->jets > 0) {
                    /* engine 0x1001c750 / DOS 0x31480: drawn while fuel (+0xc0) >= 0; the whole-unit fuel smoothed s +=
                     * (fuel - s) >> 1 a frame (3Dfx 0x1024c848 / DOS 0x9687c, see g_heat_s); fill = s x (length + 1) /
                     * 0x712 (DOS look: / 0x71c, 0x314b9) capped at the length in green bands (0xf), the rest from its end
                     * red bands (0xb) */
                    int jp, n = hud_filter_steps(&g_jet_acc, &g_jet_seen);
                    const int32_t fuel = (int32_t)pu->jet_fuel;
                    while (n-- > 0) g_jet_s += (fuel - g_jet_s) >> 1;
                    jp = (int)((long long)(jlen + 1) * g_jet_s / (g_tex_kind == 3 ? 0x71c : 0x712));
                    if (jp > jlen) jp = jlen;
                    g_jet_p = jp;
                    px_band_h(&jw, 0, by, jp, bh, 0xf);
                    px_band_h(&jw, jp, by, jlen - jp, bh, 0xb);
                    hud_text(g_hud, (float)jw.x0 * jw.ux, (float)(jw.y0 + ly) * jw.uy, "Jets", green);
                }
            }
            }
            if (getenv("MW2_HUD_RECTS")) {   /* TEST hook: the HUD elements' rectangles in window pixels (x0 y0 x1 y1), once a
                                              * second: side -1 left-hand, 1 right-hand, 0 centred (hud_side) */
                static double last = -1;
                if (g_sim.now / 1000.0 >= last + 1.0 || g_sim.now / 1000.0 < last) {
                    static const struct { const char *nm; int side, rect; float x0, y0, x1, y1; } E[] = {
                        {"radar", -1, -1, 89.6f - 75.8f, 86.4f - 75.8f, 89.6f + 75.8f, 86.4f + 75.8f}, {"alt", -1, -1, 2, 178, 70, 267},
                        {"viewer", -1, 13, 0, 0, 0, 0}, {"autopilot", -1, -1, 110, 456, 110, 456},
                        {"compass", 0, -1, 230, 2, 411, 84}, {"reticle", 0, -1, 320, 240, 320, 240}, {"heat", 0, -1, 219.5f, 428.1f, 329.5f, 428.1f},
                        {"dhdt", 0, -1, 340.5f, 428.1f, 408.5f, 428.1f}, {"jets", 0, -1, 422, 428.1f, 491, 428.1f},
                        {"weapons", 1, 3, 0, 0, 0, 0}, {"damage", 1, 2, 0, 0, 0, 0}, {"vport", 1, -1, 516.9f, 349.4f, 619.4f, 428.8f},
                        {"throttle", 1, -1, 625.6f, 338.75f, 635.6f, 470.0f}, {"kph", 1, -1, 551, 453, 551, 453}};
                    int e2;
                    last = g_sim.now / 1000.0;
                    printf("HUDRECTS t=%.1f window %dx%d layout %s\n", last, w, h, g_hud_wide ? "wide" : "original");
                    for (e2 = 0; e2 < (int)(sizeof E / sizeof E[0]); e2++) {
                        float x0 = E[e2].x0, y0 = E[e2].y0, x1 = E[e2].x1, y1 = E[e2].y1;
                        if (E[e2].rect >= 0) {
                            const int *rr = g_layout[E[e2].rect];
                            if (E[e2].rect >= g_layout_n) continue;
                            x0 = LX(rr[0]); y0 = LY(rr[1]); x1 = LX(rr[0] + rr[2]); y1 = LY(rr[1] + rr[3]);
                        }
                        HUD_SIDE(E[e2].side);
                        printf("HUDRECT %s %d %.0f %.0f %.0f %.0f\n", E[e2].nm, E[e2].side, hud_screen_x(g_hud, x0), hud_screen_y(g_hud, y0), hud_screen_x(g_hud, x1), hud_screen_y(g_hud, y1));
                        HUD_SIDE(0);
                    }
                }
            }
            /* the MISSION OBJECTIVES display: on demand (F12), not part of the end sequence */
            if (g_show_objectives) {   /* DOS footage: blue / green text over the view, no panel */
                /* engine 0x10017b80: the title, then 0x100176d0 once per objective priority - 1 Primary, 2 Secondary,
                 * 4 Tertiary, 0 (default, labelled Tertiary), 8 Return - each listing the player's table nodes whose
                 * runtime "shown" flag (node +0x74, set at load from the 'V' byte and flipped by toggle nodes,
                 * 0x1000a1d0 kind 0x40000) is set, so an objective toggled on mid-mission (the dustoff) appears at once.
                 * Rows: label colour 6, text 0xe (a text of 32+ characters on two lines: its first 32, then the rest),
                 * status right-aligned on the first line: "In progress" 0xe, "Successful" 7, "Failed" 0xb. Spacing:
                 * title + h + h/2, h/2 before the 2, 4 and 8 groups (none before 0), h per row (2h for two lines), the
                 * time line h + h/2 below. DOSBox YELLSCN1 F12 (dustoff made visible in a PRJ copy): Primary x2,
                 * Secondary, Tertiary, Return, all "In progress" in green */
                static const float oblue[4] = {0.15f, 0.25f, 0.85f, 1.0f};
                static const int ORDER[5] = {1, 2, 4, 0, 8};
                const mtbl_table *t0 = &g_sim.logic.tables[0];
                const float lh = hud_text_height(g_hud), half = floorf(lh * 0.5f);
                const float tx = 60.0f + hud_text_width(g_hud, "Secondary: ");   /* text column (0x100176a0) */
                float y = 165.0f;
                int q2, pass;
                hud_text(g_hud, 60.0f, y, "MISSION OBJECTIVES", oblue);
                hud_line(g_hud, 60.0f, y + lh + 1.0f, 60.0f + hud_text_width(g_hud, "MISSION OBJECTIVES"), y + lh + 1.0f, 1.0f, oblue);
                y += lh;
                for (pass = 0; pass < 5; pass++) {
                    if (ORDER[pass] != 0) y += half;
                    for (q2 = 0; q2 < t0->node_count; q2++) {
                        const mtbl_node *nd = &t0->nodes[q2];
                        int stv = g_sim.logic.state[0][q2];
                        const char *st = stv == MTBL_OK ? "Successful" : stv == MTBL_FAILED ? "Failed" : "In progress";
                        const float *sc = stv == MTBL_OK ? oblue : stv == MTBL_FAILED ? red : green;
                        if (!g_sim.logic.shown[0][q2] || nd->priority != ORDER[pass]) continue;
                        hud_text(g_hud, 60.0f, y, nd->priority == 1 ? "Primary:" : nd->priority == 2 ? "Secondary:" : nd->priority == 8 ? "Return:" : "Tertiary:", oblue);
                        if (strlen(nd->text) < 32) hud_text(g_hud, tx, y, nd->text, green);
                        else {
                            char l1[33];
                            memcpy(l1, nd->text, 32); l1[32] = '\0';
                            hud_text(g_hud, tx, y, l1, green);
                            hud_text(g_hud, tx, y + lh, nd->text + 32, green);
                        }
                        hud_text(g_hud, 600.0f - hud_text_width(g_hud, st), y, st, sc);
                        y += strlen(nd->text) < 32 ? lh : 2.0f * lh;
                    }
                }
                y += lh + half - 12.0f;   /* the time line h + h/2 below the next row (drawn at y + 12 below) */
                if ((int32_t)t0->time_limit > 0) {
                    char tr[48];
                    double left = (double)(int32_t)t0->time_limit - g_sim.now / 1000.0;
                    if (left < 0) left = 0;
                    snprintf(tr, sizeof tr, "Time Remaining: %02d:%02d:%05.2f", (int)(left / 3600), (int)(left / 60) % 60, fmod(left, 60.0));
                    hud_text(g_hud, 60.0f, y + 12.0f, tr, oblue);
                }
                if (g_sim.ending) {
                    const char *res = msim_outcome_text(&g_sim) ? msim_outcome_text(&g_sim) : "Mission over";
                    hud_text(g_hud, 60.0f, y + 34.0f, res, g_sim.outcome == 2 ? green : red);
                }
            }
            if (g_cc_page >= 0 && g_sim_ok) cc_draw();   /* the COMMAND COMPUTER / SYSTEMS STATUS panel */
            mm_draw();   /* the MAIN MENU over the HUD */
            if (g_paused && g_hs_paused.tex)   /* 0x10036370: PAUSED centred in the rect (0, 0.2)-(1, 0.4), native size */
                hud_draw(g_hud, &g_hs_paused, 320.0f - hud_px_x(g_hud, (float)g_hs_paused.w) * 0.5f,
                         144.0f - hud_px_y(g_hud, (float)g_hs_paused.h) * 0.5f, NULL);
            /* message lines (engine 0x10003010 / 0x10002e40): line 0 an MSGBAR across the top (DOS capture: rows 0-48
             * at 1024x768, the text 17-27 px down), line 1 the same across the bottom; each drawn only while active */
            if (g_msg_t > 0) {
                int k, any = 0;
                for (k = 0; k < 2; k++) {
                    if (g_msg_line[k].until <= g_sim.now) continue;
                    any = 1;
                    { float fk = hud_flash_level(g_hud); hud_set_flash(g_hud, 1, 0, 0, 0); msg_bar(k, w, h); hud_set_flash(g_hud, 1, 0, 0, fk); }   /* the bar: the palette's grey ramp (0xf0-0xff), which the red flash leaves alone */
                    msg_text(k, w, h, green);
                }
                if (!any) g_msg_t = 0;
            }
            (void)l; (void)ppd;
#undef LX
#undef LY
            hud_end(g_hud);
        } else if (g_hud && g_world && ((g_paused && g_hs_paused.tex) || (MSG_ONLY_VIEW && g_msg_t > 0) || (g_cc_page >= 0 && g_sim_ok))) {   /* HUD hidden / external view: PAUSED all the same */
            const float *green2 = HP(0xe);
            HUD_BEGIN(g_hud, w, h);
            if (g_cc_page >= 0 && g_sim_ok) cc_draw();   /* the menus are not part of the HUD (SYSTEMS STATUS: HUD Off) */
            if (g_paused && g_hs_paused.tex)
                hud_draw(g_hud, &g_hs_paused, 320.0f - hud_px_x(g_hud, (float)g_hs_paused.w) * 0.5f,
                         144.0f - hud_px_y(g_hud, (float)g_hs_paused.h) * 0.5f, NULL);
            if (MSG_ONLY_VIEW) {   /* ejected / destroyed: the message lines stay (DOSBox: "Ejecting", "Press CTRL-Q to exit...") */
                int k;
                for (k = 0; k < 2; k++) {
                    if (g_msg_line[k].until <= g_sim.now) continue;
                    { float fk = hud_flash_level(g_hud); hud_set_flash(g_hud, 1, 0, 0, 0); msg_bar(k, w, h); hud_set_flash(g_hud, 1, 0, 0, fk); }   /* the bar: the palette's grey ramp (0xf0-0xff), which the red flash leaves alone */
                    msg_text(k, w, h, green2);
                }
            }
            hud_end(g_hud);
        }
        if (g_hud) hud_set_flash(g_hud, 0, 0, 0, 0);
        if (g_render_w && g_world && g_lr_fbo[0]) { lowres_present(g_out_w, g_out_h); w = g_out_w; h = g_out_h; }
        if (g_sim_ok && g_world && g_hud && !getenv("MW2_NO_STARTUP") && (g_tex_kind == 3 || c3d_ti)) {
            /* the palette fade from black at the mission start (engine 0x10007900 -> 0x100079d0 -> 0x1002b090 ->
             * 0x1002ad50; DOS 0x15070 -> 0x3eae0 -> 0x3e7d0): the palette starts all black (slot 0x10, 0x1002b0c0); the
             * time-of-day code skips its first two frames, on the third it fades to the day's palette over 0x16a ticks
             * (DOS 0x16c) - counted in frames: steps = 0x16a / the last frame's ticks (DAT_1024ab4c) + 0.5, one step per
             * frame (0x1002acf0 at the frame's end), the whole screen (HUD, cockpit) with it. At 30 fps 2.0 s; DOSBox's slow
             * first frames give fewer steps (YELLSCN1 9 steps / 1.0 s, TNJ1SCN1 8 / 0.5 s).
             * DOS look only: in the 3D editions the black start palette goes to the driver's set-palette entries
             * (0x1002b0c0 -> +0x1c / +0x18 = 0x10005b30 / 0x10005b20, empty in the Glide driver) without touching the
             * vertex colour (DAT_1024e494) or the flat colours / HUD table (0x1002ac40), and the frame-3 fade targets the
             * time of day's slot (4 / 0 / 8: DAT_1024e494 off) - so the picture is there from the first frame; the HUD
             * and flat colours are the start slot's (hud_mission_palette). (Titanium textures keep the fade: not traced) */
            static int frame, steps;
            static int32_t prev_now;
            float level;
            frame++;
            if (frame == 3) {
                float ticks = (float)(g_sim.now - prev_now) * 0.182f;
                steps = ticks <= 0 ? 0x14 : ticks < 1.0f ? 10 : (int)(362.0f / ticks + 0.5f);
                if (steps < 1) steps = 1;
            }
            prev_now = g_sim.now;
            level = frame <= 3 ? 0.0f : (float)(frame - 3) / (float)steps;
            g_fade_level = level > 1.0f ? 1.0f : level;
            if (level < 1.0f) {
                int fw, fh;
                float blk[4] = {0, 0, 0, 0};
                blk[3] = 1.0f - level;
                SDL_GL_GetDrawableSize(win, &fw, &fh);
                glViewport(0, 0, fw, fh);
                hud_begin(g_hud, fw, fh);
                hud_rect(g_hud, hud_left_x(g_hud) - 10.0f, -10.0f, 660.0f - 2.0f * hud_left_x(g_hud), 500.0f, blk);
                hud_end(g_hud);
            }
        }
        test_frame(w, h);   /* TEST hook (tests/sequences) */
        if (g_auto_on && (g_world || getenv("MW2_TEST_VIEWER")) && (g_auto_elapsed += g_auto_dt) >= g_auto_secs) {
            test_dump("final");   /* TEST hook */
            unsigned char *px = malloc((size_t)w * (size_t)h * 3);
            FILE *of = fopen(g_auto_out, "wb");
            if (px && of) {
                int y;
                glPixelStorei(GL_PACK_ALIGNMENT, 1);   /* tight rows: a width not a multiple of 4 overran px */
                glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
                fprintf(of, "P6\n%d %d\n255\n", w, h);
                for (y = h - 1; y >= 0; y--) fwrite(px + (size_t)y * (size_t)w * 3, 1, (size_t)w * 3, of);
            }
            if (of) fclose(of);
            free(px);
            if (getenv("MW2_DEBUG_FIRE")) printf("fire: hits %d misses %d ending %d shots %d shutdown %d attacks %d mask %x w0 state %d ammo %d\n", g_sim.hits, g_sim.misses, g_sim.ending, g_sim.shot_count, g_sim.player_unit.shutdown, g_sim.player_attacks, g_sim.player_fire_mask, g_sim.player_unit.weapons[0].state, g_sim.player_unit.weapons[0].ammo);
            printf("autopilot: %.1f s, height %.1f m, at %d,%d heading %.0f, %.0f km/h, armour %d, %s\n", (double)g_auto_elapsed,
                   (double)(g_sim.player_unit.y / 100.0f),
                   g_pl.origin[0], g_pl.origin[2], (double)g_pl.heading, (double)(g_pl_speed * 0.036f),
                   combat_health(&g_sim.player_unit), g_sim.player_unit.destroyed ? "destroyed" : "alive");
            running = 0;
        }
        perf_mark(2);
        SDL_GL_SwapWindow(win);
        g_frame_no++;
        perf_mark(3);
        perf_report(w, h);
        if (g_world && g_sim_ok && g_pl_ok && g_pilot)
            snprintf(title, sizeof title, "MW2 - %s  %3.0f km/h  twist %+3.0f  armour %d  heat %.0f%s  target %s%s", mission,
                     (double)(g_pl_speed * 0.036f), (double)g_pl.twist, combat_health(&g_sim.player_unit),
                     (double)g_sim.player_unit.heat,
                     g_sim.player_unit.shutdown ? " SHUTDOWN" :
                     g_sim.player_unit.override ? " SHUTDOWN OVERRIDDEN" :
                     g_sim.player_unit.shutdown_pending ? " SHUTDOWN SEQUENCE INITIATED (O = override)" :
                     g_sim.player_unit.heat > 65.0f ? " HEAT LEVEL CRITICAL" : "",
                     g_pl_target >= 0 ? g_actors[g_pl_target].name : "-",
                     g_sim.player_unit.destroyed ? "  DESTROYED" : "");
        else if (g_world && g_sim_ok)
            snprintf(title, sizeof title, "MW2 - %s  %s  player %d armour+structure, heat %.0f%s  (P run, F fire)", mission,
                     g_patrol ? "running" : "paused", combat_health(&g_sim.player_unit), (double)g_sim.player_unit.heat,
                     g_sim.player_unit.destroyed ? "  DESTROYED" : "");
        else
        snprintf(title, sizeof title, "MW2 viewer - %s  mission %s camo %d clan %d  %dx%d  fog %.5f  %s %s", rec, mission, camo, clan, w, h,
                 v.fog_density, v.bilinear ? "bilinear" : "point", msaa ? "MSAA" : "");
        SDL_SetWindowTitle(win, g_world ? "MechWarrior 2" : title);
    }
    if (g_world) write_results();   /* left early: the shell still gets a result */
    music_stop();
    if (g_have_mech) { mech3d_free(&g_mech); c3d_free_scene(&g_scene); }
    if (g_actors) { if (g_sim_ok) msim_free(&g_sim); world3d_free_actors(g_actors, g_actor_count); }
    if (g_pl_ok) mech3d_free(&g_pl);
    sfx_destroy(g_sfx);
    glr_destroy(r);
    syskeys_update(0);   /* the system's shortcuts back (atexit too, for the other ways out) */
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (g_fx_ta && g_fx_ta != ta) prj_close(g_fx_ta);   /* the mission's first texture archive, after a Video mode change */
    titanium_open(0);
    prj_close(ma); prj_close(ta);
    return exit_code;   /* 42: Flee to Windows (the shell leaves too) */
}

/* weapon sounds: once per volley (engine 0x100437a0); the player's at full volume, others by distance */
static void on_shot(void *u, int weapon, int owner, int first, float x, float z)
{
    int snd = combat_weapon_sound(weapon);
    (void)u;
    if (getenv("MW2_SFX_TRACE")) fprintf(stderr, "shot w=%d owner=%d first=%d snd=%d sfx=%p\n", weapon, owner, first, snd, (void *)g_sfx);
    if (!first || snd <= 0 || !g_sfx) return;
    if (owner < 0) {
        /* the player's own (0x100437a0 -> 0x10043b52, DOS 0x52080: the volley's first projectile): in the cockpit view
         * 0x1001bc30 (DOS 0x287f0) -> 0x10032ba0 not positional, volume 0x32, pan by the weapon's location (slot +0x28):
         * 2 / 5 (CT, LA) 0x4f, 4 / 6 (RA, RL) 0x2f, else 0x40; outside it at the mech (0x10032bf0, not halved: level
         * 100 at ~0 m). DOSBox YELLSCN1 Mad Dog, cockpit, SB16: LA L 0.397 R 0.299, RA L 0.290 R 0.402, RA MPL L 0.297
         * R 0.395, LRMs (RT / LT) 0.40 / 0.40 */
        if (VIEW_IN_COCKPIT) {
            int lc = g_sim.shot_loc;
            sfx_play(g_sfx, snd, SFX_VOL(0x32), sfx_pan(lc == 2 || lc == 5 ? 0x4f : lc == 4 || lc == 6 ? 0x2f : 0x40));
        } else { float p3[3]; snd_player_pos(p3); snd_at(snd, p3, 1, snd_cockpit()); }
    }
    else if (owner < g_sim.actor_count) {
        /* 0x10043b59: the camera minus the shooter's position (+0x50; pan mirrored), halved in the cockpit (edx
         * DAT_1024e678, 0x10043ae3) */
        float p3[3] = {(float)g_sim.actors[owner].mech.origin[0], g_sim.units[owner].y, (float)g_sim.actors[owner].mech.origin[2]};
        (void)x; (void)z;
        snd_at(snd, p3, 1, snd_cockpit());
    }
}
