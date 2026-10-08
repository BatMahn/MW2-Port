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
#define HUD_BEGIN(hd, w, h) (g_render_w == 320 && g_lr_fbo[0] ? hud_begin_stretched(hd, w, h) : hud_begin(hd, w, h))
static const char *HN(const char *base) { static char b[8][16]; static int k; k = (k + 1) & 7; snprintf(b[k], 16, "%s%s", base, g_hud_sfx); return b[k]; }
static int        g_cockpit_frame;   /* the cockpit frame (the eye's hull piece): off until its rule is solved (ASSUMPTIONS
                                      * "Cockpit frames"); MW2_COCKPIT_FRAME=1 shows the current attempt */
/* the key ramp's acceleration, full scale per second squared: constant acceleration as in the engine
 * (0x10039ee0): 3 << shift per tick, the shift per control set with the bindings (not traced; shifts 5 / 3 fit the DOS
 * leg taps and zoom taps) */
#define KEY_ACCEL 48.5f   /* 3 << 5 per tick^2 at 182 ticks/s on a 16.16 value: 48.5 /s^2 (DOS leg taps fit 7-9 / 21 deg) */
#define ZOOM_ACCEL 12.1f  /* 3 << 3: the zoom (DOS: fitted 11-16) */
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
static int        g_im_ok, g_im_mouse;   /* g_im_mouse: INPUT.MAP uses the mouse (else the port's mouse look) */
static float      g_eye_pan, g_eye_tilt, g_zoom = 1.0f;   /* pilot_pan / pilot_tilt / zoom_factor (the cockpit view) */
#define A(name) (g_im_ok && im_action(&g_im, name, k, e.key.keysym.mod))
#define H(name) (g_im_ok && im_held(&g_im, name, ks))

static mech3d    g_mech;          /* the viewed mech, kept for posing */
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
static const char *const RADAR_LABEL[4] = {"R: 250.0m", "R: 500.0m", "R: 1.0km", "R: 2.0km"};
static const float RADAR_CM[4] = {25000.0f, 50000.0f, 100000.0f, 200000.0f};
static int       g_tgt_sel = -1, g_nav_sel = -1, g_radar_i = 2, g_hud_off, g_dmg_off, g_tgtdisp_off, g_tgtdisp = 1;   /* target viewer (0x10021300): 1 wireframe, 2 shaded, 0 off */
static int       g_group_fire, g_cur_group, g_htal;   /* F6: TOGGLE_HTAL */
static int       g_pit_first = -1;   /* this frame: index of the first cockpit-piece part in the actor layer */
static mech3d    g_frame_layer;     /* this frame's actor layer, kept for the viewport pass */
static int       g_sat_i;   /* satellite map range 1000 m / 2^i full width (descriptor 4: 100000 .. 6250 cm) */
static int       g_radar_big, g_satmap;   /* F2 NEXT_RADAR_MODE (small / large centred), F3 RADAR_MAP_TOGGLE */
static unsigned char g_nav_seen[256];   /* nav points reached (engine nav flag 0x20: RRNP on the radar, autopilot skips them) */
static int       g_radar_mode = 1;        /* engine 0x10003fb0: 1 small -> 2 large -> 0 off -> 1 */
static hud_sprite g_hs_rgp[3], g_hs_rtgp[3], g_hs_rtgt[3], g_hs_rtof[3], g_hs_ruser, g_hs_rnp[2], g_hs_rtnp[2];   /* radar sprites, side F / E / N */
static int       g_autopilot;   /* a: TOGGLE_AUTOPILOT (engine 0x10015c00) */
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
static int       g_pl_ok, g_pilot = 1, g_cockpit = 1, g_pl_target = -1;   /* missions start in the cockpit, as the original */
static int       g_frame_mech;
static int       g_win_w = 1280, g_win_h = 720;   /* bumped whenever the player's mech is (re)loaded */
static float     g_pl_stride = 100, g_pl_walk = 1500, g_throttle, g_pl_speed, g_pl_t, g_pitch;
static int       g_feet_to_torso;
static hud      *g_hud;
static hud_sprite g_hs_rcl[6], g_hs_rcl_dos[6], g_hs_reticle_dos, g_hs_tgt_dos[3];   /* the DOS (base set) reticle and brackets */   /* RCLINOP RCLLOCK RCLNOLK RCLPLOC RCLTGT RCLGLOC */
static hud_sprite g_hs_compass, g_hs_reticle, g_hs_damage, g_hs_dmg_lvl[4];
static hud_sprite g_hs_cmpmk[4], g_hs_altmk[2];   /* CMPMKR1-4, ALTMKR1-2 */
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
static int       g_last_display_hits, g_last_display_hits5, g_last_pending, g_last_override;
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
    m->heading = full->heading; m->twist = full->twist;
    mech3d_pose(m, (const anim_set *)full->pose_anim, full->pose_t);
    if (full->part_count > 0) { int q; for (q = 0; q < m->part_count; q++) m->parts[q].tex_offset = full->parts[0].tex_offset; }   /* the star's camo / insignia */
    return m;
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
        /* ATi: one format; 3dfx/S3: effect frames carry transparency (ARGB4444) */
        if (tex_decode(r.data, r.size, g_fx_ati == 1 ? TEX_ATI : g_fx_ati == 2 ? TEX_PVR : g_fx_ati == 3 ? TEX_DOS : g_fx_ati == 4 ? TEX_MGA : TEX_3DFX_4444, t) != 0) t->rgba = NULL;
        else if (g_fx_ati == 3) {   /* DOS: the effect frames in the mission's palette (index 0xff transparent) */
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

/* an effect starts (msim on_effect): pool, then its sound at its position (engine table 0x1025b168) */
static void on_effect(void *u, int type, const float pos[3])
{
    int snd;
    (void)u;
    if (!g_fx_ok) return;
    if (fx_spawn(&g_fxw, &g_fxd, type, pos, (int32_t)((int64_t)g_sim.now * 182 / 1000)) < 0) { g_sim.fx_refused = 1; return; }   /* nor its chunks */
    if (getenv("MW2_FX_TRACE")) fprintf(stderr, "fx %.1fs type %x at %.0f,%.0f,%.0f\n", g_sim.now / 1000.0, type, pos[0], pos[1], pos[2]);
    snd = fx_sound(type);
    if (snd > 0 && g_sfx) sfx_play_at(g_sfx, snd, pos[0] - (float)g_pl.origin[0], pos[2] - (float)g_pl.origin[2], g_pl.heading + g_pl.twist);
}
static void on_sound(void *user, int snd, const float pos[3])
{
    if (snd == -1) {   /* the collision clang (KICKCHNK) */
        static int kid = -2;
        if (kid == -2) { int st = g_ma ? prj_find_type(g_ma, "SNDS") : -1; kid = st >= 0 ? prj_find_id(g_ma, st, "MECBLDC1") : -1; }
        snd = kid;
        if (snd <= 0) return;
    }
    if (!pos) { if (g_sfx) sfx_play(g_sfx, snd, 0.5f, 0.0f); return; }   /* not positional: 0x10032b60 volume 0x32 (ENEMYFIR 0x6f) */
    float dx = pos[0] - (float)g_pl.origin[0], dz = pos[2] - (float)g_pl.origin[2];
    (void)user;
    if (g_sfx && dx * dx + dz * dz < 50000.0f * 50000.0f) sfx_play_at(g_sfx, snd, dx, dz, g_pl.heading + g_pl.twist);
}
/* object sound tasks (TSK type 4: 0x1000ec90 -> 0x10032cc0, every run): a looping sample at the node, at most 8 at once
 * (sample channels 8-15, the first to ask keeps one); ended beyond the task's range or 500 m (0x10032f60 0); volume
 * 0x10032f60 (100 - 100 (d / 500 m)^2, d the horizontal distance in whole metres) x EFFECTS >> 2 - a quarter of a
 * one-shot's level; pan 0x10032c10 as sfx_play_at. A name missing from SNDTABLE (VULTURES, JNG, MECDSLXX) plays
 * nothing (0x10025eb0 -1). */
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
    world3d_sound s[64];
    int n, i, k;
    if (owner != (const void *)g_sim.paths || !g_sfx) {   /* a new mission (or none): the loops end */
        for (k = 0; k < 8; k++) if (ch[k].pcm) { if (g_sfx) sfx_stop_pcm(g_sfx, ch[k].pcm); free(ch[k].pcm); ch[k].pcm = NULL; }
        memset(bad, 0, sizeof bad);
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
        float dx = s[i].pos[0] - (float)g_pl.origin[0], dz = s[i].pos[2] - (float)g_pl.origin[2], d = sqrtf(dx * dx + dz * dz), rel, pan;
        int m = (int)(d / 100.0f), vol = m >= 500 ? 0 : 100 - m * m * 100 / 250000, c = -1;
        for (k = 0; k < 8; k++) if (ch[k].pcm && ch[k].task == s[i].task) c = k;
        if (d > (float)s[i].range || vol <= 0) {
            if (c >= 0) { sfx_stop_pcm(g_sfx, ch[c].pcm); free(ch[c].pcm); ch[c].pcm = NULL; }
            continue;
        }
        if (c < 0) {
            int st = prj_find_type(g_ma, "SNDS"), id, len, rate = 11025;
            unsigned char *pcm = NULL;
            if (s[i].task < 4096 && bad[s[i].task]) continue;
            for (k = 0; k < 8 && ch[k].pcm; k++) {}
            if (k == 8) continue;   /* every channel taken: retried at its next run */
            id = st >= 0 ? prj_find_id(g_ma, st, s[i].name) : -1;
            if (id <= 0 || (len = obj_snd_pcm(id, &pcm, &rate)) <= 0) { free(pcm); if (s[i].task < 4096) bad[s[i].task] = 1; continue; }
            c = k; ch[c].task = s[i].task; ch[c].pcm = pcm;
            sfx_play_pcm(g_sfx, pcm, len, rate, 0.0f, 1);
        }
        rel = atan2f(dx, dz) * 180.0f / 3.14159265f - (g_pl.heading + g_pl.twist);
        pan = sinf(rel * 3.14159265f / 180.0f);
        if (pan > 0.766f) pan = 0.766f;
        if (pan < -0.766f) pan = -0.766f;
        sfx_pcm_level(g_sfx, ch[c].pcm, (float)vol / 100.0f * 0.8f * 0.25f, pan);
    }
}
/* engine 0x10037880: the display toggles (MFD cycle, HTAL, rear / down view, weapon / damage / target display, HUD)
 * click with sound 0xdc */
static int ui_click(void) { if (g_sfx) sfx_play(g_sfx, 0xdc, 0.4f, 0.0f); return 1; }
static float g_fade_level = 1.0f;   /* the opening palette fade: 0 black .. 1 done (see the frame end) */
static int   g_voice_off;   /* the VOICE slider at 0: the outcome is shown as text instead (0x10031440) */
static int   g_radio_qn;    /* radio lines waiting (the msim end sequence waits for none, DAT_1024f6bc) */
static int   g_masc;   /* MASC engaged (engine DAT_1024c554) */
/* mission radio messages (mtbl node start / finish SNDS names): on the voice channel, one at a time - a message that
 * arrives while another plays waits for it */
static void on_radio(void *user, const char *name)
{
    static char queue[8][12];
    static int qn, qpri[8], flushed;
    (void)user;
    g_radio_qn = qn + (g_sfx && sfx_voice_busy(g_sfx));
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
static int   g_dos_lasers;
static int   g_tex_kind;   /* 0 3Dfx / S3, 1 ATi, 2 PowerVR, 3 DOS, 4 Matrox Mystique */
static int   g_edition;    /* PORTCFG_ED_* from MW2_EDITION (the shell's RENDERER choice) */   /* MW2_DOS_LASERS=1: the DOS edition's green balls instead of the 3D editions' bolts */   /* LASER1-3 model lengths (z extent), cm */
static mech3d_part g_proj[9];   /* projectile models: LASER1-3, MISSILE1, BULLET, PPCSHOT1, MISSILE2; debris CHUNKER1-2 */
static int       g_proj_ok;
static void load_projectiles(prj_archive *a)
{
    static const char *names[9] = {"LASER1", "LASER2", "LASER3", "MISSILE1", "BULLET", "PPCSHOT1", "MISSILE2", "CHUNKER1", "CHUNKER2"};
    int i;
    if (g_proj_ok) return;
    for (i = 0; i < 9; i++) {
        prj_record r;
        memset(&g_proj[i], 0, sizeof g_proj[i]);
        snprintf(g_proj[i].model_name, sizeof g_proj[i].model_name, "%s", names[i]);
        g_proj[i].node = -1;
        if (prj_read_named(a, "POLY", names[i], &r) == PRJ_OK) {
            wtb_parse(r.data, r.size, &g_proj[i].model);
            prj_record_free(&r);
            if (i < 3 && g_proj[i].model.object_count) {
                const wtb_object *o = &g_proj[i].model.objects[0];
                int k2;
                float lo = 1e9f, hi = -1e9f;
                for (k2 = 0; k2 < o->vert_count; k2++) { if (o->verts[k2].z < lo) lo = (float)o->verts[k2].z; if (o->verts[k2].z > hi) hi = (float)o->verts[k2].z; }
                g_proj_len[i] = hi - lo;
            }
        }
    }
    g_proj_ok = 1;
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
static int g_inspect_until;   /* sim ms: the target viewer shows the inspection (0x16a ticks, engine 0x10021xxx) */
static void inspect_now(void)
{
    int r = msim_inspect(&g_sim, g_tgt_sel, g_tgt_sel < 0 ? g_tgt_thing : -1);
    if (r == 1) { hud_message_v("Inspection successful", 68); g_inspect_until = g_sim.now + 362 * 1000 / 182; }
    else if (r == 2) hud_message_v("Target is beyond inspection radius", 69);
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
    if (g_tgt_thing >= g_sim.bld_count) g_tgt_thing = -1;
    else if (g_tgt_thing >= 0 && (g_sim.bld[g_tgt_thing].hp <= 0 ||
             (g_world && g_sim.bld[g_tgt_thing].intact >= 0 && g_sim.bld[g_tgt_thing].intact < g_mech.part_count && g_mech.parts[g_sim.bld[g_tgt_thing].intact].hidden)))
        g_tgt_thing = -1;
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
        if (!g_sim.planet.hostile) { pu->ejected = 1; pu->intact_out = 1; if (g_sfx) sfx_play(g_sfx, 0xc5, 0.5f, 0.0f); }   /* state 5 */
        else hud_message_v("Atmosphere hostile : ejection aborted", 63);   /* message 0x20; then an ordinary death */
    }
    pu->destroyed = 1;   /* 0x10016280 ("Ejecting" for state 5: player_upkeep); the mission's end follows (msim) */
}
/* External camera modes (engine DAT_1024e66c, 0x1002dc90): 0 the cockpit (the port's chase camera, g_cockpit 0, stands in
 * for the engine's orbit mode 1), 3 the ordnance camera (0x1002e210), 4 the ejection camera (0x1002e7d0) */
static int      g_camode, g_cam_prev, g_cam_last;   /* the mode; the one ordnance returns to (DAT_1024e664); last frame's (DAT_1024e660) */
static float    g_cam_zoom = 1.0f;                  /* DAT_1024e65c: the external modes' zoom (1.0 at start, 2.0 from mode 3 on) */
static unsigned g_follow;                           /* DAT_1025a6a0: the projectile the weapon view / ordnance camera follow (shot id) */
static float    g_cam_eye[3], g_cam_yaw, g_cam_pitch;   /* last frame's camera (the ejection / death cameras start from it) */
static float    g_ej_vy;                            /* the ejection camera: climb speed (cm per tick, DAT_1024e694) */
static int32_t  g_ej_t0, g_cam_now;                 /* its start (sim ms, DAT_1024e69c); the sim time last seen */
static int      g_ej_ending;
/* the camera in the cockpit (mode 0, not the chase camera): cockpit model, own muzzle flashes, halved sounds */
#define VIEW_IN_COCKPIT (g_cockpit && g_camode == 0)
/* the HUD and its windows: not once ejected (DOSBox: the HUD goes; the ordnance camera keeps it) */
#define HUD_VIEW (g_cockpit && g_camode != 4 && g_camode != 1 && !g_sim.player_unit.intact_out)
/* the HUD's message lines alone (DOSBox: ejected - "Ejecting"; destroyed - "Mission failed", "Press any key to exit...") */
#define MSG_ONLY_VIEW (g_camode == 4 || g_camode == 1 || g_sim.player_unit.intact_out)
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
 * DAT_10160cf0: (255, 8, 8)), so the world turns red at once and stays so until the fade back begins; the palette-drawn
 * 2D (the HUD) fades both ways. DOS fades everything through the palette. A flash during a running fade starts from the
 * colours reached (DAT_1007c6b8 / DAT_0014fe0c; ASSUMED: then back to the normal palette - the engine's "palette before"
 * is -1 there). */
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
static void red_flash(int dur, int32_t now)
{
    int dos = g_tex_kind == 3;
    float half = (float)(dur >> 1), frames = g_frame_ticks > 0 ? half / g_frame_ticks : 20.0f;
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
    if (last_now < 0 || now < last_now) { g_flash_t0 = -1; g_flash_n = 0; dead_done = 0; }   /* a new mission */
    if (now > last_now && last_now >= 0) g_frame_ticks = (float)(now - last_now) * 0.182f;
    last_now = now;
    if (pu->red_flash) {   /* 0x1001e6c0 -> 0x1001bda0 (DOS 0x32fc0 -> 0x28930) */
        int n = 3 * ++g_flash_n, dur;
        pu->red_flash = 0;
        if (g_tex_kind == 3) dur = n < 16 ? (int)(((int64_t)((n << 16) / 15) * 0x16c + 0x8000) >> 16) : 0x16c;
        else dur = n < 16 ? (int)((float)n * 0.06666667f * 362.0f) : 0x16a;
        red_flash(dur, now);
    }
    /* the death's fade after a flash asked for in the same frame (ASSUMED order: the killing hit's short flash would
     * otherwise cut the death's second-long fade to its own length) */
    if (pu->destroyed && !pu->intact_out && !dead_done) {   /* state 4: 0x1001bd80 (DOS 0x28910) */
        dead_done = 1;
        red_flash(g_tex_kind == 3 ? 0x16c : 0x16a, now);
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
        static float ang;
        float tgt[3], d, a, want_yaw, want_pitch, dx, dy, dz, gy;
        if (g_sim.player_eye_ok) memcpy(tgt, g_sim.player_eye, sizeof tgt);
        else { tgt[0] = (float)g_pl.origin[0]; tgt[1] = g_sim.player_unit.y + 950.0f; tgt[2] = (float)g_pl.origin[2]; }
        d = 3.0f * (g_sim.player_radius > 0 ? g_sim.player_radius : 545.0f);
        if (g_cam_last != 1) {   /* entering mode 1: the servos start from the camera as it stands */
            servo_set(&g_srv_x, g_cam_eye[0] - tgt[0], now, 0.5f);
            servo_set(&g_srv_y, g_cam_eye[1] - tgt[1], now, 0.7f);
            servo_set(&g_srv_z, g_cam_eye[2] - tgt[2], now, 0.5f);
            servo_set(&g_srv_yaw, g_cam_yaw, now, 0.2f);
            servo_set(&g_srv_pitch, g_cam_pitch, now, 0.2f);
            g_cam_zoom = 1.0f;
        }
        ang = fmodf(ang + 0.25f * dt, 360.0f);
        /* the view turns toward the target from last frame's camera (0x1001d970) */
        dx = tgt[0] - g_cam_eye[0]; dy = tgt[1] - g_cam_eye[1]; dz = tgt[2] - g_cam_eye[2];
        want_yaw = atan2f(dx, dz) * 180.0f / 3.14159265f;
        want_pitch = atan2f(dy, sqrtf(dx * dx + dz * dz)) * 180.0f / 3.14159265f;
        v->look_yaw = servo_angle(&g_srv_yaw, want_yaw, now);
        v->look_pitch = servo_angle(&g_srv_pitch, want_pitch, now);
        a = (g_pl.heading + g_pl.twist - ang) * 3.14159265f / 180.0f;
        v->free_cam = 1;
        v->eye[0] = tgt[0] + servo(&g_srv_x, -sinf(a) * d, now);
        v->eye[2] = tgt[2] + servo(&g_srv_z, -cosf(a) * d, now);
        gy = msim_ground(&g_sim, v->eye[0], v->eye[2], g_cam_eye[1] + 10000.0f);
        gy += gy > 1e-7f ? 500.0f : 200.0f;
        v->eye[1] = tgt[1] + servo(&g_srv_y, tgt[1] + 0.25f * d < gy ? gy - tgt[1] : 0.25f * d, now);
    }
    if (g_camode == 1 || g_camode == 3 || g_camode == 4)
        v->vfov = 2.0f * atanf(tanf(73.74f * 3.14159265f / 360.0f) / g_cam_zoom) * 180.0f / 3.14159265f;
    g_cam_last = g_camode;
    memcpy(g_cam_eye, v->eye, sizeof g_cam_eye);
    g_cam_yaw = v->look_yaw; g_cam_pitch = v->look_pitch;
}
static int g_voice_q[8], g_voice_qn;   /* voiced messages waiting for the voice channel */
static int g_voice_next;   /* a suffix line (engaged 80 / disengaged 84) to say once the voice channel is free */
/* a voiced message (the voice queue's text, 0x10031440: 0x712 ticks at priority 0x32); an unvoiced key / toggle
 * acknowledgement: 0x16a ticks (2 s) at 0x32 */
static void hud_message_v(const char *m, int voice)
{   /* DOSBox (SB16 on / off, the S key): "Shutting down" is spoken without text when voices play, shown when silent -
     * engine 0x10031440 posts a voiced line's text only when it cannot be played (MW2_VOICE_TEXT=1: always show it) */
    if (getenv("MW2_STATE_DUMP")) printf("MSGV now=%d voice=%d %s\n", (int)g_sim.now, voice, m);   /* TEST hook (tests/sequences) */
    if (voice > 0 && sfx_voice_audible(g_sfx, voice)) {
        /* one voice at a time (the engine's voice queue, 0x10031870): DOSBox YELLSCN1 - "Shutting down" (BET11_1) said
         * at s waited for the 16 s opener to end */
        if (sfx_voice_busy(g_sfx) || g_voice_qn) { if (g_voice_qn < 8) g_voice_q[g_voice_qn++] = voice; }
        else sfx_play_voice(g_sfx, voice, 0.9f);
        if (!getenv("MW2_VOICE_TEXT")) return;
    }
    hud_message_ex(m, voice > 0 ? 0x712 : 0x16a, 0x32);
}
/* message table 0x1024f140 {voice, text, 0} + suffix table 0x1024edb8 {voice, "engaged" / "disengaged"} (engine
 * 0x10031870(message, suffix): the two lines queued) */
static void hud_message_v2(const char *m, int voice, int suffix) { hud_message_v(m, voice); g_voice_next = suffix; }   /* the VOICE slider */

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

/* ---- the COMMAND COMPUTER (3D edition menu table 0x10260950..0x10261600; keys USER_MENU u, ALL_PT_MENU b / Ctrl+F1,
 * PT_2_MENU Ctrl+F2, PT_3_MENU Ctrl+F3; navigation 0x10065df0: a digit picks an item, Esc goes back). Pages: -1 closed,
 * 0 the root, 1 Change Formation, 2 + p the commands for point p (0 = all). The panel's look (MBAR / MENDL / MENDR
 * sprites) is not reproduced yet: plain boxed text, ASSUMED. */
static int g_cc_page = -1;
static void hud_message_v2(const char *m, int voice, int suffix);
static void cc_message(void *u, const char *t, int voice)
{
    (void)u;
    if (voice < 0) hud_message_ex(t, -voice, 100);   /* a timed system line (the end sequence's CTRL-Q prompt) */
    else if (voice >> 16) hud_message_v2(t, voice & 0xffff, voice >> 16);
    else hud_message_v(t, voice);
}
static const int CC_ORDER[5] = {MSIM_CMD_ATTACK, MSIM_CMD_DEFEND, MSIM_CMD_JOIN, MSIM_CMD_ENGAGE, MSIM_CMD_SHUTDOWN};
static const char *const CC_ORDER_TEXT[5] = {"Attack My Target", "Defend My Target", "Join Formation", "Engage at Will", "Shutdown"};
static const char *const CC_LAST_TEXT[8] = {"Change Formation", "Engage at Will", "Attack", "Join Formation", "Defend",
                                            "Disengage", "Shutdown", "No Cmd"};   /* 0x10260998 */
static void on_shot(void *u, int weapon, int owner, int first, float x, float z);
static int       g_dmg_regions[15][4];
/* damage outlines of other mechs (the target viewer shows the target's): xxxxDMG6 remapped per level */
typedef struct { char skel[17]; hud_sprite lvl[4]; int regions[15][4]; int ok; } dmg_set;
static dmg_set    g_tdmg[16];
static hud_sprite g_hs_vtgt_nav;   /* VTGT_NP6: the nav point emblem in the target viewer */
static hud_sprite g_hs_navmk, g_hs_edge[4], g_hs_edge_nav[4];   /* TGTNP6 ("NAV" marker); CRTLFT6 / CRTRGHT6 / CRTUP6 / CRTDOWN6 (red; nav: green) */
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
static void hud_bar(float x, float y, float w, const float base[4], const float light[4], const float dark[4])
{
    const float u = 9.375f / 15.0f;   /* one 1024-mode pixel in 640x480 units */
    if (w <= 0) return;
    hud_rect(g_hud, x, y, w, 4 * u, base);
    hud_rect(g_hud, x, y + 4 * u, w, 4 * u, light);
    hud_rect(g_hud, x, y + 8 * u, w, 4 * u, base);
    hud_rect(g_hud, x, y + 12 * u, w, 3 * u, dark);
}

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
static int current_nav(const char **label)
{
    const mtbl_table *t0 = &g_sim.logic.tables[0];
    int q2, nv;
    if (g_nav_sel >= 0 && g_nav_sel < g_sim.nav_count) {   /* NEXT_NAVPOINT / PREV_NAVPOINT */
        if (label) *label = nav_label(g_nav_sel);
        return g_nav_sel;
    }
    for (q2 = 0; q2 < t0->node_count; q2++) {
        const mtbl_node *nd = &t0->nodes[q2];
        if (nd->kind != MTBL_K_REACH || !g_sim.logic.shown[0][q2] || g_sim.logic.state[0][q2] == MTBL_OK) continue;
        for (nv = 0; nv < g_sim.nav_count; nv++)
            if (strcasecmp(g_sim.navs[nv].name, nd->target) == 0) { if (label) *label = nav_label(nv); return nv; }
        /* its target isn't a nav point (an area): look further down the table */
    }
    for (nv = 0; nv < g_sim.nav_count; nv++)   /* no open REACH objective: the mission's first selectable nav point */
        if (g_sim.navs[nv].selectable) { if (label) *label = nav_label(nv); return nv; }
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
    if (strncmp(skel, "WARHAWK", 7) == 0) snprintf(dmg, sizeof dmg, "WARKDMG%s", g_hud_sfx);
    else snprintf(dmg, sizeof dmg, "%.4sDMG%s", skel, g_hud_sfx);
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
    for (q = 0; q < 15; q++) {
        int loc = REG_LOC[q] - 1, lf = 0, lr = 0, lv, pick;
        const int *rg = regions[q];
        if (rg[2] <= 0 || rg[3] <= 0) continue;
        if (u->armor_max[loc] > 0) lf = 15 - 15 * (int)u->armor[loc] / u->armor_max[loc];
        if (u->rear_max[loc] > 0) lr = 15 - 15 * (int)u->rear[loc] / u->rear_max[loc];
        lv = lf > lr ? lf : lr;
        pick = u->loc_gone[loc] ? 3 : lv <= 0 ? 0 : lv < 12 ? 1 : 2;
        hud_draw_region(g_hud, &lvl[pick], dx, dy, (float)rg[0] * 2.0f * g_hud_s, (float)rg[1] * 2.0f * g_hud_s, (float)rg[2] * 2.0f * g_hud_s, (float)rg[3] * 2.0f * g_hud_s);
    }
}
static int       g_layout[24][4], g_layout_n;

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
static char  g_auto_out[256];

static void sim_log(void *u, const char *line)
{
    (void)u;
    if (strstr(line, "player hits") && strstr(line, "- DESTROYED")) hud_message_v("Enemy mech destroyed", 70);
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
/* the target viewer's range (engine 0x10021150): whole metres "%3ldm" up to 1000, then "%2.2fk" km */
static void range_text(char *out, size_t n, float cm)
{
    long m = (long)cm / 100;
    if (m < 1001) snprintf(out, n, "%3ldm", m);
    else snprintf(out, n, "%2.2fk", (double)((float)m * 0.001f));
}

/* the target viewer's wireframe (DOS footage: the targeted mech or structure drawn as a blue line model in the box,
 * turning as you move round it): the model's polygon edges, seen from the player's bearing, a little from above,
 * scaled to fit the box. view_yaw: the player's bearing from the target, relative to the target's heading (degrees) */
static float g_wire_r[2] = {1, 0}, g_wire_f[2] = {0, 1};   /* the camera's right / forward on the ground (x, z), set per frame */
/* the target viewer's mech: its blown-off locations left out, as the view draws it (the engine renders the object
 * itself, 0x10021300, so lost limbs are gone there too) */
static const combat_unit *g_wire_unit;
/* a location's colour in the target view, as the damage outline (0x1001b620: levels 6 / 3 / 11 / 0 - here the HUD
 * blue, yellow, red, black): armour lost front or rear in fifteenths, 0 intact, 1-11 yellow, 12+ red; the centre torso
 * of a destroyed unit black. Returns 0 blue, 1 yellow, 2 red, 3 black; tu NULL or gq outside 1-8: blue */
static int loc_level(const combat_unit *tu, int gq)
{
    int loc, lf = 0, lr = 0, lv;
    if (!tu || gq < 1 || gq > 8) return 0;
    loc = gq - 1;
    if (tu->armor_max[loc] > 0) lf = 15 - 15 * (int)tu->armor[loc] / tu->armor_max[loc];
    if (tu->rear_max[loc] > 0) lr = 15 - 15 * (int)tu->rear[loc] / tu->rear_max[loc];
    lv = lf > lr ? lf : lr;
    if (tu->destroyed && loc == 2) return 3;
    return lv >= 12 ? 2 : lv > 0 ? 1 : 0;
}
static const unsigned LOC_RGB[4] = {0x0030D7u, 0xFFCE00u, 0xFF1818u, 0x000000u};
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

/* ---- the in-game MAIN MENU (GAMEKEY.MAP MAIN_MENU, Esc). The 3D editions' menu system (MW2.DLL 0x10065xxx): menu 4,
 * table 0x10264980 - MAIN MENU: 1 Abort Mission, 2 Graphics, 3 Audio Ctrl, 4 Combat Variables, 5 Flee to Windows,
 * 0 Accept (Esc to cancel); flags 0x11 (the controls off while open, 0x1003a3c0); while it is open the game is paused
 * (WinMain 0x10009770: "pause timer TRUE", the effect samples end, voice and music pause - 0x10031f30).
 * Pages (the item records {type, text, draw, -, submenu}: type 0 a submenu, 1 an item, 2 Accept, 3 a label):
 *   ABORT MISSION  0x102646d0: "Confirmation requested", 1 Accept -> 0x10064280: action 0x3b (EXIT_SIM)
 *   FLEE TO WINDOWS 0x10264828: "Confirm your cowardice", 1 Accept -> 0x100642e0: leave the game
 *   GRAPHICS 0x10265570: Textured Sky, Textured Ground (Off / On; off = flat in the texture's average colour,
 *            0x10027c80 / 0x1002d330), Resolution 800x600 (No / Yes; its commit is empty) - the port: Video mode
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
enum { MP_MAIN, MP_ABORT, MP_GRAPHICS, MP_AUDIO, MP_COMBAT, MP_FLEE, MP_COUNT };
enum { MV_SKY, MV_GROUND, MV_VIDEO, MV_MUSIC, MV_EFFECTS, MV_VOICE, MV_OBJTEX, MV_TERTEX, MV_DETAIL, MV_DENSITY, MV_CHUNKS, MV_COUNT };
typedef struct { int type; const char *text; int arg; } mm_item;
typedef struct { const char *title; int n; mm_item it[6]; } mm_page;
static const mm_page MM_PAGE[MP_COUNT] = {
    {"MAIN MENU", 6, {{MI_SUB, "Abort Mission", MP_ABORT}, {MI_SUB, "Graphics", MP_GRAPHICS}, {MI_SUB, "Audio Ctrl", MP_AUDIO},
                      {MI_SUB, "Combat Variables", MP_COMBAT}, {MI_SUB, "Flee to Windows", MP_FLEE}, {MI_ACCEPT, "Accept (Esc to cancel)", 0}}},
    {"ABORT MISSION", 2, {{MI_LABEL, "Confirmation requested", 0}, {MI_CONFIRM, "Accept (Esc to cancel)", 0}}},
    {"GRAPHICS", 4, {{MI_VAL, "Textured Sky", MV_SKY}, {MI_VAL, "Textured Ground", MV_GROUND}, {MI_VAL, "Video mode", MV_VIDEO},
                     {MI_ACCEPT, "Accept (Esc to cancel)", 0}}},
    {"SET AUDIO VOLUME", 4, {{MI_SLIDER, "Music", MV_MUSIC}, {MI_SLIDER, "Sound Effects", MV_EFFECTS}, {MI_SLIDER, "Voice", MV_VOICE},
                             {MI_ACCEPT, "Accept (Esc to cancel)", 0}}},
    {"COMBAT VARIABLES", 6, {{MI_VAL, "Object textmaps", MV_OBJTEX}, {MI_VAL, "Terrain textmaps", MV_TERTEX}, {MI_VAL, "Display detail", MV_DETAIL},
                             {MI_VAL, "Object density", MV_DENSITY}, {MI_VAL, "Explosion chunks", MV_CHUNKS}, {MI_ACCEPT, "Accept (Esc to cancel)", 0}}},
    {"FLEE TO WINDOWS", 2, {{MI_LABEL, "Confirm your cowardice", 0}, {MI_CONFIRM, "Accept (Esc to cancel)", 1}}},
};
static const char *const MM_OFFON[2] = {"Off", "On"}, *const MM_LOWHIGH[2] = {"Low", "High"};
static const char *const MM_ED_TEXT[PORTCFG_ED_COUNT] = {"Enhanced", "DOS", "3Dfx", "ATi", "S3", "PowerVR", "Matrox"};
static int  g_mm_stack[8], g_mm_depth;          /* open pages; empty = closed */
static int  g_mm_sel;                           /* the chosen item of the top page */
static int  g_mm_v[MV_COUNT], g_mm_old[MV_COUNT];   /* the pages' values (sliders 0..0x10000), as opened */
static int  g_mm_eds[PORTCFG_ED_COUNT], g_mm_ned = -1;   /* the editions the Video mode offers */
static int  g_mm_vol[3] = {-1, -1, -1};         /* music, effects, voice in force (16.16; -1 = not read yet) */
static int  g_mm_ed_req = -1, g_mm_world_dirty, g_mm_quit;
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
static int mm_value_count(int mv) { return mv == MV_VIDEO ? (g_mm_ned > 0 ? g_mm_ned : 1) : 2; }
static const char *mm_value_text(int mv)
{
    if (mv == MV_VIDEO) return MM_ED_TEXT[g_mm_ned > 0 ? g_mm_eds[g_mm_v[mv]] : g_edition];
    return (mv == MV_DETAIL || mv == MV_DENSITY ? MM_LOWHIGH : MM_OFFON)[g_mm_v[mv] ? 1 : 0];
}
/* a page's values as they stand (0x10064d90 state 1: each item's getter) */
static void mm_load_page(int pg)
{
    int k;
    if (pg == MP_GRAPHICS) {
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
    if (pg == MP_GRAPHICS) {
        if (g_view) { g_view->flat_sky = !g_mm_v[MV_SKY]; g_view->flat_ground = !g_mm_v[MV_GROUND]; }
        if (g_mm_ned > 0 && g_mm_eds[g_mm_v[MV_VIDEO]] != g_edition) g_mm_ed_req = g_mm_eds[g_mm_v[MV_VIDEO]];
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
        if (++n == d) return k;
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
        g_mm_stack[g_mm_depth++] = it->arg; g_mm_sel = 0;
        while (g_mm_sel < MM_PAGE[it->arg].n && MM_PAGE[it->arg].it[g_mm_sel].type == MI_LABEL) g_mm_sel++;
        mm_load_page(it->arg);
        if (g_sfx) sfx_play(g_sfx, 0xe1, 1.0f, 0.0f);
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
            if (p->it[g_mm_sel].type != MI_LABEL) break;
        }
    } else if (k == SDLK_RIGHT || k == SDLK_SPACE || k == SDLK_LEFT) mm_step(&p->it[g_mm_sel], k == SDLK_LEFT ? -1 : 1);
    if (g_mm_depth && g_mm_sel != prev && g_mm_stack[g_mm_depth - 1] == pg && g_sfx) sfx_play(g_sfx, 0xdb, 1.0f, 0.0f);
}
static void mm_draw(void)
{
    static hud_sprite frame, endl, bar, endr, knob;
    static int loaded;
    static const float grey[4] = {170 / 255.0f, 170 / 255.0f, 170 / 255.0f, 1}, red[4] = {211 / 255.0f, 0, 0, 1},
                       gold[4] = {121 / 255.0f, 97 / 255.0f, 0, 1};
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
        const float *c = k == g_mm_sel ? red : grey;
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
    if (g_have_mech) { mech3d_free(&g_mech); c3d_free_scene(&g_scene); g_have_mech = 0; }
    if (g_actors) { if (g_sim_ok) msim_free(&g_sim); g_sim_ok = 0; world3d_free_actors(g_actors, g_actor_count); g_actors = NULL; g_actor_count = 0; }
    g_have_anim = 0;
    if (g_world) {
        char scene[16];
        memset(&m, 0, sizeof m);
        snprintf(scene, sizeof scene, "%.4sSCN1", mission);
        if (world3d_mission(ma, scene, 1, &m) <= 0 || m.part_count == 0) { fprintf(stderr, "no world for %s\n", mission); return -1; }
        world3d_actors(ma, scene, &g_actors, &g_actor_count);   /* posable, drawn on the actor layer */
        load_view_far(ma, scene);
        g_exit_fade_t0 = -1;
        g_sim_ok = msim_init(&g_sim, ma, scene, g_actors, g_actor_count) == 0;
        if (g_sim_ok && !g_drop_tex) music_start(g_sim.music_track);   /* with the drop screen: when it fades (main loop) */
        g_sim.log = sim_log;
        g_sim.sd_at_once = g_edition == PORTCFG_ED_DOS;   /* an AI self-destruct: DOS at once, 3D editions 0x16a ticks on */
        g_sim.on_message = cc_message;                 /* lancemate acknowledgements on the message bar, with voice */
        if (getenv("MW2_CC_PAGE")) g_cc_page = atoi(getenv("MW2_CC_PAGE"));   /* tests: open a COMMAND COMPUTER page */
        g_sim.on_shot = on_shot;
        g_sim.on_effect = on_effect;
        g_sim.on_sound = on_sound;
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
            if (getenv("MW2_START")) {   /* debug: start the player at "x,z" */
                float sx0 = 0, sz0 = 0;
                if (sscanf(getenv("MW2_START"), "%f,%f", &sx0, &sz0) == 2) { g_sim.player[0] = sx0; g_sim.player[2] = sz0; }
                if (getenv("MW2_START_HEADING")) g_sim.player_heading = (float)atof(getenv("MW2_START_HEADING"));   /* debug */
            }
            g_pl.origin[0] = (int32_t)g_sim.player[0]; g_pl.origin[2] = (int32_t)g_sim.player[2];
            g_pl.heading = g_sim.player_heading;
            g_throttle = 0; g_pl_speed = 0; g_thr_srv = 0; g_spd_srv = 0; g_drive_slow = 0; g_pl_t = 0; g_pitch = 0; g_pl.twist = 0; g_masc = 0;
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
                 * <name>K (1024x768), the font too (BASE6X7, BASE6X76, BASE6X7K); the set follows the picture's lines -
                 * render=320x200 the plain set drawn 1:1 into the 320x200 frame, 640x480 and native below 768 lines the 6
                 * set, 1024x768 and native from 768 lines the K set (the 6 and K art are the same size apart from the
                 * full-width bars) */
                char dmg[16];
                if (!g_hud) g_hud = hud_create(ma);
                g_hud_s = g_render_w == 320 ? 0.5f : 1.0f;
                g_hud_sfx = g_render_w == 320 ? "" : g_render_w == 640 ? "6" : g_render_w == 1024 ? "K" : g_win_lines >= 768 ? "K" : "6";
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
                    /* damage outline: first 4 letters + DMG (Warhawk: WARKDMG) */
                    if (strncmp(g_sim.player_skel, "WARHAWK", 7) == 0) snprintf(dmg, sizeof dmg, "%s", HN("WARKDMG"));
                    else snprintf(dmg, sizeof dmg, "%.4sDMG%s", g_sim.player_skel, g_hud_sfx);
                    if (hud_sprite_load(g_hud, ma, dmg, 0, &g_hs_damage) != 0) { snprintf(dmg, sizeof dmg, "%s", HN("TIMBDMG")); hud_sprite_load(g_hud, ma, dmg, 0, &g_hs_damage); }
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
                            hud_sprite_free(&g_hs_edge[q]); hud_sprite_load(g_hud, ma, en[q], 0, &g_hs_edge[q]);
                            /* nav points: the chevron in the HUD green (DOS footage; colour 10 -> 14) */
                            hud_sprite_free(&g_hs_edge_nav[q]); hud_sprite_load_remap(g_hud, ma, en[q], 0, 10, 14, &g_hs_edge_nav[q]);
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
            {   /* torso twist limit from the mech's MGEO record (inferred field; see ASSUMPTIONS.md) */
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

/* the edition's look on the view: lasers, texture filtering, the PowerVR veil (at start, and the in-game Video mode) */
static void edition_look(glr_view *v)
{
    v->tex_min = v->tex_mag = v->env_min = v->env_mag = 0; v->mga_mip = 0;
    /* the 3D editions' laser bolts by default (user choice); the DOS edition's green balls with it, or MW2_DOS_LASERS=1 */
    g_dos_lasers = getenv("MW2_DOS_LASERS") != NULL || g_edition == PORTCFG_ED_DOS;
    /* each edition's texture filtering (traced in its renderer DLL):
     *   3Dfx  guTexAllocateMemory(min = mag = bilinear, GR_MIPMAP_NEAREST_DITHER): linear, nearest mip
     *   ATi   "Filter Textures" on (the default): C3D_ETFILT_MINPNT_MAG2BY2 - point minification, bilinear magnification,
     *         no mips (off: point)
     *   S3    "Filter Textures" on (default): TEX4TPP - bilinear without mips; the sky and ground with mips (M4TPP)
     *   PVR   bilinear by the driver (no filter call in the game); mips taken as nearest
     *   DOS   texel indices, point sampled (glr forces it) */
    if (g_edition == PORTCFG_ED_3DFX || g_edition == PORTCFG_ED_PVR) { v->tex_min = GL_LINEAR_MIPMAP_NEAREST; v->tex_mag = GL_LINEAR; }
    else if (g_edition == PORTCFG_ED_ATI) { v->tex_min = GL_NEAREST; v->tex_mag = GL_LINEAR; }
    else if (g_edition == PORTCFG_ED_S3) { v->tex_min = GL_LINEAR; v->tex_mag = GL_LINEAR; v->env_min = GL_LINEAR_MIPMAP_NEAREST; v->env_mag = GL_LINEAR; }
    else if (g_edition == PORTCFG_ED_MGA) {
        /* Matrox Mystique (MW2.DLL, Msi95): no filtering at all ("Filter Textures" is a dead option, 0x1005fca0 = ret);
         * the ground alone has mips - 2x2 box levels picked per triangle by view depth (2500 / 5000 / 15000 cm, from
         * MYSTIQUE.PAR: thresholds x MIPmaxZ), the sky one level */
        v->tex_min = GL_NEAREST; v->tex_mag = GL_NEAREST; v->env_min = GL_NEAREST_MIPMAP_NEAREST; v->env_mag = GL_NEAREST; v->mga_mip = 1;
    }
    /* the PowerVR fog's veil over the sky: with the PowerVR edition and the enhanced mix (which takes its fog) */
    v->pvr_veil = g_edition == PORTCFG_ED_PVR || g_edition == PORTCFG_ED_ENHANCED;
}

/* The in-game Video mode (the GRAPHICS menu; a port addition in place of the 3Dfx edition's "Resolution 800x600", whose
 * commit 0x10066520 -> 0x100667e0 does nothing): another 3D edition's look mid-mission - its texture archive, sky file,
 * fog and filtering. The mission keeps the archive it was started with for models and data (an edition's archive
 * differs in record ids, and the simulation lives on them); textures are found by name. The DOS look has its own
 * palette-indexed archive and stays a shell choice. The choice is saved as the next mission's edition. */
static int switch_look(glr *r, glr_view *v, prj_archive *ma, prj_archive **ta, int *ati, const char *mission, int ed)
{
    static char mo[PORTCFG_PATH], tx[PORTCFG_PATH], kind[PORTCFG_PATH], sky[PORTCFG_PATH], fog[PORTCFG_PATH];
    char err[256], *bp;
    portcfg pc, raw;
    prj_archive *nt;
    int k;
    if (ed == PORTCFG_ED_DOS || g_tex_kind == 3 || ed == g_edition) return -1;
    bp = SDL_GetBasePath(); portcfg_load(NULL, &pc); portcfg_resolve(&pc, bp, NULL); SDL_free(bp);
    if (portcfg_edition(&pc, ed, mo, tx, kind, sky, fog) != 0) return -1;
    if (!(nt = prj_open(tx, err, sizeof err))) { fprintf(stderr, "video mode: %s\n", err); return -1; }
    k = strcmp(kind, "ati") == 0 ? 1 : strcmp(kind, "pvr") == 0 ? 2 : strcmp(kind, "mga") == 0 ? 4 : 0;
    if (sky[0]) setenv("MW2_SKYGND", sky, 1); else unsetenv("MW2_SKYGND");
    if (fog[0]) setenv("MW2_SKYGND_FOG", fog, 1); else unsetenv("MW2_SKYGND_FOG");
    setenv("MW2_EDITION", PORTCFG_ED_NAME[ed], 1);
    if (*ta != g_fx_ta) prj_close(*ta);   /* the effect frames stay with the mission's first texture archive (CEL ids) */
    *ta = nt; *ati = k; g_tex_kind = k; g_edition = ed;
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
    if (g_lite_level > 0) v->ambient = (float)g_lite_level * 3.0f / 256.0f;
    portcfg_load(NULL, &raw);   /* the file's own values (unresolved), the edition changed: the next mission's */
    raw.edition = ed;
    portcfg_save(NULL, &raw);
    printf("video mode: %s (%s)\n", PORTCFG_ED_NAME[ed], tx);
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
/* the static in a window whose top-left is (x, y) in 640x480 units: SNOWCLR frame (ticks since its first draw / 18) % 10 */
static void draw_snow(float x, float y)
{
    int f;
    if (g_snow_t0 < 0) g_snow_t0 = g_sim.now;
    f = (int)(((int64_t)(g_sim.now - g_snow_t0) * 182 / 1000) / 18) % 10;
    if (f < 0) f = 0;
    if (g_hs_snow[f].tex) hud_draw(g_hud, &g_hs_snow[f], x, y, NULL);
}

/* ---- TEST ONLY (tests/sequences): held keys, a machine-readable state dump, timed screenshots, player hits ----
 * MW2_HOLD="t0-t1:key,..."  keys held between autopilot times t0 and t1 (names as MW2_KEYS, + shift+/ctrl+/alt+)
 * MW2_STATE_DUMP="1,2.5,..." or "/0.5"  print "STATE ..." lines at those autopilot times (or every 0.5 s), and at exit;
 *                                        hud messages print as "MSG ..." lines
 * MW2_SHOT="t:file.ppm,..."  write the frame at those times
 * MW2_TEST_AHIT="t:actor:loc:weapon:count,..."  the same for an actor (stops once it is destroyed)
 * MW2_TEST_PHIT="t:loc:weapon:count,..."  the player takes `count` hits of weapon `weapon` on location loc (1-8) at t
 * MW2_TEST_HEAT="t:value,..."  set the player's heat at t
 * MW2_TEST_RNG="t:value"  set the simulation's random state at t (deterministic criticals)
 * MW2_TEST_IMMOBILE="t:actor"  the actor stands still from t (its unit immobile)
 * MW2_TEST_DISARM="t:actor[:level]"  the actor's weapons all spent at t (out of weapons: jump / charge and self-destruct /
 *                                        flee); level: its pilot level too, and no jump jets
 * MW2_TEST_AIM="k,dist,dy"   stand dist cm south of actor k (north if negative) with the reticle held on its centre + dy
 * MW2_AIM_TRACE=1            print AIMHIT lines: each player impact against the reticle line it was fired along
 * MW2_AIM_BODY=1             the old aim ray from the body point (comparison only)
 * MW2_TEST_DISPLAY="t:instrument:level,..."  set a cockpit instrument's display-damage level at t (0 radar, 2 viewport
 *                                        window, 13 target viewer); the dump adds lvl= (0/2/13), snow= / snow_n= counts
 * MW2_TEST_KILL_ALL="t:0"  destroy every armed enemy (not friendly, not neutral) at t
 * MW2_TEST_THING="t:bld,..." target structure bld at t;  MW2_TEST_BLD_KILL="t:bld,..." destroy structure bld at t
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
        {"kp5", SDLK_KP_5}, {"slash", SDLK_SLASH}, {"pause", SDLK_PAUSE}};
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
static void test_immobile(const char *r) { int a = atoi(r); if (a >= 0 && a < g_actor_count && g_sim.armed[a]) { g_sim.units[a].immobile = 1; if (g_sim.minds) g_sim.minds[a].speed = 0; } }
static void test_display(const char *r) { int i = -1, l = 0; if (sscanf(r, "%d:%d", &i, &l) == 2 && i >= 0 && i < 26) g_inst_lvl[i] = (short)l; }
static void test_rng(const char *r) { g_sim.rng = (unsigned)strtoul(r, NULL, 0); }
static void test_kill_all(const char *r) { int i; (void)r; for (i = 0; i < g_actor_count; i++) if (g_sim.armed[i] && !g_actors[i].friendly && g_actors[i].alliance != 2) g_sim.units[i].destroyed = 1; }
static void test_heat(const char *r) { g_sim.player_unit.heat = g_sim.player_unit.heat_mark = (float)atof(r); }
static void test_thing(const char *r) { g_tgt_sel = -1; g_tgt_thing = atoi(r); }
static void test_bld_kill(const char *r) { int b = atoi(r); if (b >= 0 && b < g_sim.bld_count) msim_damage_building(&g_sim, b, 1e6f); }
static int test_drop_actor = -1;   /* MW2_TEST_DROP's actor (its state is dumped as a_*) */
/* MW2_TEST_DROP="t:actor:dz:height:vy:hspeed"  actor put dz cm ahead (+z) of the player, its feet `height` cm above the
 * player's, falling at vy cm/tick (negative down), carrying hspeed cm/s toward the player */
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
    g_actors[a].mech.origin[1] = (int32_t)g_sim.units[a].y;
    g_sim.units[a].vy = vy;
    if (g_sim.air_speed) g_sim.air_speed[a] = hs;
    if (g_sim.minds) g_sim.minds[a].speed = hs;
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
    printf(" pkey=%.3f", (double)g_pl_t);
    if (getenv("MW2_KILL_ACTOR")) {
        int ka = atoi(getenv("MW2_KILL_ACTOR"));
        if (ka >= 0 && ka < g_actor_count)
            printf(" k_key=%.3f k_dead=%d k_gone=0x%02x", (double)g_actors[ka].t, g_sim.units[ka].destroyed,
                   (g_sim.units[ka].loc_gone[0] != 0) | (g_sim.units[ka].loc_gone[2] != 0) << 2);
    }
    printf(" parmor=");   /* armour + structure per location 1-8 */
    for (i = 0; i < 8; i++) printf("%s%.1f", i ? "," : "", (double)(pu->armor[i] + pu->internal[i]));
    if (test_drop_actor >= 0) {   /* MW2_TEST_DROP's actor */
        const combat_unit *au = &g_sim.units[test_drop_actor];
        int ag = 0;
        for (i = 0; i < 8; i++) ag |= (au->loc_gone[i] != 0) << i;
        printf(" a_y=%.0f a_vy=%.2f a_ground=%.0f a_gone=0x%02x a_health=%d a_x=%d a_z=%d a_dead=%d a_armor=", (double)au->y, (double)au->vy, (double)au->ground, ag, combat_health(au),
               g_actors[test_drop_actor].mech.origin[0], g_actors[test_drop_actor].mech.origin[2], au->destroyed);
        for (i = 0; i < 8; i++) printf("%s%.1f", i ? "," : "", (double)(au->armor[i] + au->internal[i]));
    }
    printf("\n");
    fflush(stdout);
}
static void test_frame(int w, int h)
{
    static int d_phit, d_heat, d_thing, d_bld, d_shot, dump_idx;
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
            printf("BLD %d hp=%d type=%d x=%.0f z=%.0f %s/%s\n", b, g_sim.bld[b].hp, g_sim.bld[b].type, (double)((g_sim.bld[b].mn[0] + g_sim.bld[b].mx[0]) * 0.5f),
                   (double)((g_sim.bld[b].mn[2] + g_sim.bld[b].mx[2]) * 0.5f), g_sim.bld[b].name, g_sim.bld[b].sub);
        listed = 1;
    }
    test_timed("MW2_TEST_PHIT", &d_phit, test_phit);
    { static int d_ahit; test_timed("MW2_TEST_AHIT", &d_ahit, test_ahit); }
    test_timed("MW2_TEST_HEAT", &d_heat, test_heat);
    { static int d_ka; test_timed("MW2_TEST_KILL_ALL", &d_ka, test_kill_all); }
    { static int d_rng; test_timed("MW2_TEST_RNG", &d_rng, test_rng); }
    { static int d_disp; test_timed("MW2_TEST_DISPLAY", &d_disp, test_display); }
    { static int d_imm; test_timed("MW2_TEST_IMMOBILE", &d_imm, test_immobile); }
    { static int d_dis; test_timed("MW2_TEST_DISARM", &d_dis, test_disarm); }
    test_timed("MW2_TEST_THING", &d_thing, test_thing);
    test_timed("MW2_TEST_BLD_KILL", &d_bld, test_bld_kill);
    { static int d_drop; test_timed("MW2_TEST_DROP", &d_drop, test_drop); }
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
    }
    if (!win || !(ctx = SDL_GL_CreateContext(win))) { fprintf(stderr, "OpenGL 3.3: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);
    { int dw0, dh0; SDL_GL_GetDrawableSize(win, &dw0, &dh0); g_win_lines = dh0; }   /* the HUD set for native rendering */
    glr_default_view(&v);
    g_view = &v;
    if (rec[0] == '@') { g_world = 1; rec = MECHS[0]; SDL_SetRelativeMouseMode(SDL_TRUE); }
    if (getenv("MW2_AUTOPILOT")) {
        char out[256] = "autopilot.ppm";
        int fire = 0, ck = 0;
        int jt = 0;
        if (sscanf(getenv("MW2_AUTOPILOT"), "%f,%f,%f,%d,%255[^,],%d,%d", &g_auto_secs, &g_auto_throttle, &g_auto_twist, &fire, out, &ck, &jt) >= 4) {
            g_auto_on = 1; g_auto_fire = fire; g_auto_cockpit = ck; g_auto_jet = jt;
            snprintf(g_auto_out, sizeof g_auto_out, "%s", out);
        }
    }
    {   /* the original's controls: GAMEKEY.MAP and INPUT.MAP from the game directory (the shell's Cockpit Controls writes
         * INPUT.MAP); without them, the files as the game installs them (tools/inputmap_defaults.h) */
        const char *dir = getenv("MW2_INSTALL_DIR") ? getenv("MW2_INSTALL_DIR") : ".";
        char gk[1100], in[1100];
        snprintf(gk, sizeof gk, "%s/GAMEKEY.MAP", dir);
        snprintf(in, sizeof in, "%s/INPUT.MAP", dir);
        im_load(&g_im, gk, in);
        if (!g_im.have_gamekey) { FILE *f = fmemopen((void *)IM_DEFAULT_GAMEKEY, strlen(IM_DEFAULT_GAMEKEY), "r"); if (f) { im_parse_gamekey(&g_im, f); fclose(f); } }
        if (!g_im.have_input) { FILE *f = fmemopen((void *)IM_DEFAULT_INPUT, strlen(IM_DEFAULT_INPUT), "r"); if (f) { im_parse_input(&g_im, f); fclose(f); } }
        g_im_ok = g_im.nbinds > 0 && g_im.nrules > 0;
        g_im_mouse = g_im_ok && im_uses(&g_im, 2);
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
    {   /* the edition (the shell's Combat Variables RENDERER, mw2port.cfg edition=): which look the simulation takes */
        const char *e = getenv("MW2_EDITION");
        int k;
        g_edition = g_tex_kind == 3 ? PORTCFG_ED_DOS : PORTCFG_ED_ENHANCED;
        for (k = 0; e && k < PORTCFG_ED_COUNT; k++) if (strcmp(e, PORTCFG_ED_NAME[k]) == 0) g_edition = k;
        g_sim.sd_at_once = g_edition == PORTCFG_ED_DOS;   /* (the mission is loaded before the edition is known) */
    }
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
        if (g_drop_tex) {   /* hold the drop screen until 2 s after launch (user choice; was 5 s); the mission waits. Then
                             * as the original: the mission's CD track starts and the drop screen fades to black over 0.86 s
                             * before the first mission frame (DOSBox YELLSCN1 / TNJ1SCN1: the music from the start of the
                             * fade, linear in brightness; the fade's code not traced - ASSUMED linear) */
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
        if (g_voice_qn && g_sfx && !sfx_voice_busy(g_sfx)) { sfx_play_voice(g_sfx, g_voice_q[0], 0.9f); memmove(g_voice_q, g_voice_q + 1, sizeof g_voice_q[0] * 7); g_voice_qn--; }
        if (g_voice_next && g_sfx && !g_voice_qn && !sfx_voice_busy(g_sfx)) { sfx_play_voice(g_sfx, g_voice_next, 0.9f); g_voice_next = 0; }
        if (g_sim_ok && g_world && g_sfx) {   /* missile lock tones: the tracking tone while a guided weapon is acquiring
                                               * (on target, 0x8000), the lock beep when it locks (0x80) - MISLTRCK /
                                               * MECXLOCK by their names */
            static int prev_lock;
            static int t_trk = -1, t_lock = -1;
            int lk = g_sim.player_lock;
            /* engine 0x1001e6c0: the lock tone (0xfe MECXLOCK) once on locking, and once as acquiring starts 0xcf
             * (MECBYHLX) - each latched until its flag drops */
            if (t_trk < 0) { int st = g_ma ? prj_find_type(g_ma, "SNDS") : -1; t_trk = st >= 0 ? prj_find_id(g_ma, st, "MECBYHLX") : 0; t_lock = st >= 0 ? prj_find_id(g_ma, st, "MECXLOCK") : 0; }
            if ((lk & 0x80) && !(prev_lock & 0x80) && t_lock > 0) sfx_play(g_sfx, t_lock, 0.8f, 0.0f);
            else if ((lk & 0x8000) && !(prev_lock & 0x8000) && !(lk & 0x80) && t_trk > 0) sfx_play(g_sfx, t_trk, 0.6f, 0.0f);
            prev_lock = lk;
        }
        if (g_sim_ok && g_world) on_radio(NULL, NULL);   /* the radio queue */
        {   /* the mission start (DOSBox YELLSCN1 / TNJ1SCN1 audio + frames, docs/reference/startup_dos_vs_port.png):
             * frame 1 - the player's state 0 -> 1 (engine 0x1001ab71): MECTURX2 0xf7 at the mech (0x10032bf0, positional);
             * frame 2 - every other unit the mech controller drives whose alliance is not 1 (0x10019040: friendly 0 and
             * neutral 2 - TNJ1's instructor) its own MECTURX2 at its position (enemies, alliance 1, sound NONPSTRT
             * only once the player is up - none at the start); DOSBox: the second MECTURX2 one frame (0.19 s) after the
             * first. The opener line (the start node's radio) plays as soon as it is queued (on_radio). The palette fade
             * from black (frames 1-3 black): see g_fade_*. */
            static int stage = 0;
            if (stage == 0 && g_sim_ok && g_world) {
                if (g_sfx) sfx_play(g_sfx, 0xf7, 0.8f, 0.0f);
                stage = 1;
            } else if (stage == 1) {
                int a;
                for (a = 0; g_sfx && a < g_actor_count; a++) {
                    const world_actor *ac = &g_actors[a];
                    int uc = ac->unit_class;
                    if (ac->alliance == 1 && !ac->friendly) continue;
                    if (g_sim.powered_down && g_sim.powered_down[a]) continue;   /* a resting group (0x10014520 states 10 / 11, set by its node at the start): no start-up */
                    if (!(uc == 1 || uc == 2 || uc == 4 || uc == 5 || uc == 8)) continue;
                    if (getenv("MW2_SFX_LOG")) fprintf(stderr, "SFXLOG start-up %s class %d alliance %d friendly %d\n", ac->name, uc, ac->alliance, ac->friendly);   /* TEST ONLY */
                    {
                        float p3[3] = {(float)ac->mech.origin[0], 0.0f, (float)ac->mech.origin[2]};
                        on_sound(NULL, 0xf7, p3);
                    }
                }
                stage = 2;
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
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if ((g_mm_depth || g_paused) && (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEWHEEL)) continue;   /* controls off (0x1003a3c0) */
            else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) dragging = 1;
            else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) dragging = 0;
            else if (e.type == SDL_MOUSEMOTION && g_world && g_pilot && g_pl_ok && g_im_mouse) {
                g_im_dev.mouse_dx += (float)e.motion.xrel;    /* the mouse as INPUT.MAP configures it */
                g_im_dev.mouse_dy += (float)e.motion.yrel;
            }
            else if (e.type == SDL_MOUSEMOTION && g_world && g_pilot && g_pl_ok) {
                /* port: with no mouse in INPUT.MAP, mouse look - torso twist, limited per mech (MGEO), and view pitch */
                g_pl.twist += (float)e.motion.xrel * 0.2f;
                if (g_twist_limit < 360.0f) {
                    if (g_pl.twist > g_twist_limit) g_pl.twist = g_twist_limit;
                    if (g_pl.twist < -g_twist_limit) g_pl.twist = -g_twist_limit;
                }
                g_pitch -= (float)e.motion.yrel * 0.15f;
                if (g_pitch > 30) g_pitch = 30;
                if (g_pitch < -30) g_pitch = -30;
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
                    int item = k == SDLK_ESCAPE ? -1 : (int)(k - SDLK_0);
                    if (item <= 0) g_cc_page = g_cc_page == 0 ? -1 : 0;                        /* Esc / 0: back */
                    else if (g_cc_page == 0) {
                        if (item == 1) g_cc_page = 2;                                             /* Command All */
                        else if (item == 2) g_cc_page = 1;                                        /* Change Formation */
                        else if (item <= 6 && msim_point_actor(&g_sim, item - 2) >= 0) g_cc_page = 2 + item - 2;
                    } else if (g_cc_page == 1) {
                        if (item <= 6 && msim_set_formation(&g_sim, item - 1) == 0) g_cc_page = -1;
                    } else if (item <= 5) {
                        msim_command(&g_sim, g_cc_page - 2, CC_ORDER[item - 1], g_tgt_sel);   /* the menu closes (0x100659a0) */
                        g_cc_page = -1;
                    }
                    continue;
                }
                if (g_world && g_sim_ok && (A("USER_MENU") || A("ALL_PT_MENU") || A("PT_2_MENU") || A("PT_3_MENU"))) {
                    int pt = A("PT_2_MENU") ? 1 : A("PT_3_MENU") ? 2 : 0;
                    if (A("USER_MENU")) g_cc_page = g_cc_page == 0 ? -1 : 0;
                    else if (pt == 0) g_cc_page = g_cc_page == 2 ? -1 : 2;
                    else if (msim_point_actor(&g_sim, pt) >= 0) g_cc_page = 2 + pt;
                    else hud_message_v("NO STAR MATES", 0);
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
                    int q, n2 = g_actor_count, best = -1, nxt = A("NEXT_TARGET");
                    float bd = 1e30f;
                    g_tgt_thing = -1;
                    if (A("RESET_TARGETTING")) g_tgt_sel = -1;
                    else if (A("TARGET_AT_RETICLE")) {   /* a mech under the reticle, else a structure there */
                        float dd2;
                        g_tgt_sel = g_pl_target;
                        if (g_tgt_sel < 0) g_tgt_thing = msim_aim_thing(&g_sim, g_pl.heading + g_pl.twist, 5.0f, &dd2);
                    }
                    else if (A("TARGET_NEAREST_ENEMY") || A("TARGET_FRIENDLY")) {
                        int fr = A("TARGET_FRIENDLY");
                        for (q = 0; q < n2; q++) {
                            float ddx, ddz, dd;
                            if (!g_sim.armed[q] || g_sim.units[q].destroyed || g_actors[q].friendly != fr) continue;
                            ddx = (float)g_actors[q].mech.origin[0] - (float)g_pl.origin[0]; ddz = (float)g_actors[q].mech.origin[2] - (float)g_pl.origin[2];
                            dd = ddx * ddx + ddz * ddz;
                            if (dd < bd) { bd = dd; best = q; }
                        }
                        g_tgt_sel = best;
                    } else {                                                                             /* NEXT / PREV_TARGET */
                        int step = nxt ? 1 : n2 - 1, i;
                        for (i = 1; i <= n2; i++) {
                            q = ((g_tgt_sel < 0 ? (nxt ? -1 : 0) : g_tgt_sel) + step * i + n2 * 4) % n2;
                            if (g_sim.armed[q] && !g_sim.units[q].destroyed && !g_actors[q].friendly) { g_tgt_sel = q; break; }
                        }
                    }
                }
                else if (g_world && g_sim_ok && (A("NEXT_NAVPOINT") || A("PREV_NAVPOINT")) && g_sim.nav_count > 0) {
                    int cur = g_nav_sel >= 0 ? g_nav_sel : current_nav(NULL);
                    g_nav_sel = nav_step(cur, A("PREV_NAVPOINT") ? -1 : 1);
                    g_tgt_sel = -1; g_tgt_thing = -1;   /* the nav point shows in the target display (a selected target had hidden it) */
                }
                else if (g_world && g_satmap && (A("RADAR_ZOOM_IN") || A("RADAR_ZOOM_OUT")))   /* 0x10004040 on mode 4: halve / double, wrapping */
                    g_sat_i = (g_sat_i + (A("RADAR_ZOOM_OUT") ? 4 : 1)) % 5;
                else if (g_world && (A("RADAR_ZOOM_IN") || A("RADAR_ZOOM_OUT")))   /* DOS: 1.0km -x-> 500m -> 250m -> 2.0km -> 1.0km, X the other way */
                    g_radar_i = (g_radar_i + (A("RADAR_ZOOM_OUT") ? 1 : 3)) % 4;
                else if (g_world && (A("TOGGLE_HUD") && ui_click())) g_hud_off = !g_hud_off;
                else if (g_world && (A("TOGGLE_DAMAGE_DISPLAY") && ui_click())) g_dmg_off = !g_dmg_off;
                else if (g_world && (A("TOGGLE_HTAL") && ui_click())) g_htal = !g_htal;
                else if (g_world && A("NEXT_RADAR_MODE")) { g_radar_mode = (g_radar_mode + 1) % 3; g_radar_big = g_radar_mode == 2; }
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
                    else { pu2->shutdown = 1; pu2->manual_down = 1; g_throttle = 0; hud_message_v("Shutting down", 14); if (g_sfx) sfx_play(g_sfx, 0xf2, 0.8f, 0.0f); }   /* message 0xd {14, "Shutting down"}; MECSHTD1 0xf2 at the mech (0x1001b060, positional; DOSBox: at s, not on a heat shutdown 0x10016054) */
                }
                else if (g_world && g_sim_ok && A("TOGGLE_AUTOPILOT") && g_pilot) {
                    g_autopilot = !g_autopilot && g_sim.nav_count > 0;
                    hud_message_v2(g_autopilot ? "Autopilot engaged." : "Autopilot disengaged.", 65, g_autopilot ? 80 : 84);
                }
                else if (g_world && (A("INFRARED") || A("ENHANCED_VISION")) && g_pilot) {
                    int want = A("INFRARED") ? 1 : 2;
                    if (want == 1 && g_inst_lvl[INST_RADAR] > 0 && g_vision != 1) want = 0;   /* refused with the radar (instrument 0) damaged (0x10007a50) */
                    int was = g_vision;
                    if (want) g_vision = g_vision == want ? 0 : want;
                    if (g_vision == 1 && was != 1 && g_sfx) sfx_play(g_sfx, 0xb2, 0.8f, 0.0f);   /* 0x10007a99: IREDMODE on engaging (0x50) */
                    if (g_vision == 1) hud_message_v2("Light amplification engaged.", 77, 80);
                    else if (g_vision == 2) hud_message_v2("Image enhancement engaged.", 76, 80);
                }
                else if (g_world && A("RADAR_MAP_TOGGLE")) {
                    g_satmap = !g_satmap;
                    if (g_satmap) hud_message_v("Satellite link established.", 67);
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
                        if (g_edition == PORTCFG_ED_DOS) player_eject(0);         /* DOS: at once */
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
                    /* action 1: the reset flags of the view (pilot pan / tilt - the ones COCKPIT_VIEW sets too) and the
                     * torso (twist / tilt) - as Keypad 5 (ASSUMED: which flag is which is not traced) */
                    g_eye_pan = g_eye_tilt = 0;
                    g_pitch = 0;
                    if (g_pl_ok) g_pl.twist = 0;
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
                    /* engine 0x1001adf0: not fitted -> the message; else MASC on (sound 0xca) / off (0xcb) */
                    if (!combat_has_masc(&g_sim.player_unit)) hud_message_v("Not equipped with MASC.", 0);
                    else { g_masc = !g_masc; if (g_sfx) sfx_play(g_sfx, g_masc ? 0xca : 0xcb, 0.7f, 0.0f); hud_message_v2(g_masc ? "MASC engaged." : "MASC disengaged.", 61, g_masc ? 80 : 84); }
                }
                else if (g_world && g_sim_ok && A("JETTISON_AMMO")) {
                    /* engine 0x10044220: the selected weapon's ammunition, if it has any: sound 0xb3, the message */
                    if (combat_jettison(&g_sim.player_unit, g_sel_weapon)) {
                        if (g_sfx) sfx_play(g_sfx, 0xb3, 0.8f, 0.0f);
                        hud_message_v("Ammo for current weapon jettisoned.", 0);
                    }
                }   /* engine: zoom_factor_reset -> 1 (DOS Ctrl+Z) */
                else if (g_world && A("FEET_TO_TORSO")) g_feet_to_torso = 1;
                else if (g_world && g_sim_ok && A("INSPECT_TARGET")) inspect_now();
                else if (g_world && A("TOGGLE_GROUP_FIRE")) {   /* \ : chain fire <-> group fire (messages 6 / 7, voices 18 / 19) */
                    g_group_fire = !g_group_fire;
                    hud_message_v(g_group_fire ? "Group fire" : "Chain fire", g_group_fire ? 19 : 18);
                }
                else if (g_world && A("NEXT_WEAPON_GROUP")) g_cur_group = (g_cur_group + 1) % 3;
                else if (g_world && A("COCKPIT_VIEW") && !(g_sim_ok && g_sim.player_unit.destroyed)) {   /* action 9: not once dead (+0x14 & 2) */
                    if (g_camode) { g_camode = 0; g_cockpit = 1; }   /* any other mode: back to the cockpit (0x1002df20(0)) */
                    else {
                        g_cockpit = !g_cockpit;
                        if (!g_cockpit) { hud_message_v("External camera engaged.", 66); g_cam_zoom = 1.0f; }   /* message table 0x1024f144 entry 17; mode 1 zoom 1.0 */
                    }
                }
                else if (g_world && k == SDLK_g && !(e.key.keysym.mod & (KMOD_CTRL | KMOD_ALT))) { g_pilot = !g_pilot; SDL_SetRelativeMouseMode(g_pilot ? SDL_TRUE : SDL_FALSE); }   /* port: free camera */
                else if (g_world && (A("THROTTLE_STOP") || A("THROTTLE_2") || A("THROTTLE_3") || A("THROTTLE_4") || A("THROTTLE_5") ||
                                     A("THROTTLE_6") || A("THROTTLE_7") || A("THROTTLE_8") || A("THROTTLE_9") || A("THROTTLE_FULL"))) {
                    static const char *const TH[10] = {"THROTTLE_STOP", "THROTTLE_2", "THROTTLE_3", "THROTTLE_4", "THROTTLE_5",
                                                       "THROTTLE_6", "THROTTLE_7", "THROTTLE_8", "THROTTLE_9", "THROTTLE_FULL"};
                    int q;
                    for (q = 0; q < 10; q++) if (A(TH[q])) g_throttle = msim_throttle_preset(q);   /* action 0x1a + q: DOS 0x4684c q x 113 / 0x400 of full (the 3Dfx DLL q / 576 = q/9); DOS bar: THROTTLE_6 = 5/9 */
                }
                else if (g_world && A("REVERSE_DIRECTION")) g_throttle = -g_throttle;   /* ASSUMED: flips the throttle's sign */
                else if (g_world && A("OVERRIDE_SHUTDOWN") && g_sim_ok) combat_override(&g_sim.player_unit);
                else if ((k == SDLK_f && !g_world) || (k == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                    Uint32 fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                    SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                } else if (k == SDLK_TAB) { mi = (mi + 1) % COUNT(MECHS); rec = MECHS[mi]; reload = 1; }
                else if (k == SDLK_n && !g_world) { ni = (ni + 1) % COUNT(MISSIONS); mission = MISSIONS[ni]; reload = 1; }
                else if (k == SDLK_c && !g_world) { camo = (camo + 1) % 8; reload = 1; }
                else if (k == SDLK_i && !g_world) { clan = (clan + 1) % 6; reload = 1; }
                else if (k == SDLK_b && !g_world) v.bilinear = !v.bilinear;
                else if (k == SDLK_SPACE && !g_world) g_walk = !g_walk;
                else if (k == SDLK_p && !g_world) g_patrol = !g_patrol;
                else if (k == SDLK_m && !g_world) { msaa = !msaa; if (msaa) glEnable(GL_MULTISAMPLE); else glDisable(GL_MULTISAMPLE); }
                else if (k == SDLK_RIGHTBRACKET) v.fog_density = v.fog_density <= 0 ? 0.0001f : v.fog_density * 1.25f;
                else if (k == SDLK_LEFTBRACKET) v.fog_density = v.fog_density < 0.00011f ? 0 : v.fog_density / 1.25f;
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
        if (g_world && g_actors) {
            /* actors: animate (walking only while P is on), move along their heading */
            static Uint32 aprev;
            Uint32 now = SDL_GetTicks();
            float dt = aprev ? (float)(now - aprev) / 1000.0f : 0;
            if (g_auto_on) dt = 1.0f / 30.0f;
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
                if (g_auto_throttle >= -1.5f) g_throttle = g_auto_throttle;   /* TEST hook: throttle -9 = the keys drive it */
                g_cockpit = g_auto_cockpit;
                if (g_auto_twist > -900.0f && !(g_sim_ok && (g_sim.player_unit.shutdown || (g_sim.now < g_sim.player_online_at && !getenv("MW2_NO_STARTUP"))))) g_pl.twist = g_auto_twist;   /* twist -999 = free */
            }
            g_sim.no_player_muzzle = VIEW_IN_COCKPIT;
            if (g_pl_ok && g_pilot && !g_sim.player_unit.destroyed) {
                /* Engine (3dfx MW2.DLL 0x1001a180 / 0x10041410): full throttle = walk MP x 105.49/64
                 * cm per 1/182 s tick = walk MP x 10.8 km/h; reverse at half; leg turn rate scales
                 * with cos(min(80, speed in km/h)) deg; FEET_TO_TORSO swings the legs at 55 deg/s.
                 * ASSUMED still: throttle ramp 0.6/s on W/S; base turn 90 deg/s inferred. Speed response measured in DOS. */
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
                float heading_before = g_pl.heading, twist_before = g_pl.twist, pitch_before = g_pitch;
                float top = g_pl_walk * (g_sim_ok ? g_sim.player_unit.speed_gain : 1.0f) * (g_masc ? 1.5f : 1.0f), h, kmh, turn;
                if (g_masc && g_sim_ok) {
                    /* engine 0x1001a180: MASC raises the target speed by half; while it runs and the mech moves forward,
                     * every 0xb5 ticks rand() % 60 == 12 is a malfunction (not for an invulnerable player, only while up -
                     * 0x1001a98f) - "MASC malfunction.", sound 0xc9, MASC off, and the heat (+0x98) rises by a quarter */
                    static float masc_t;
                    masc_t += dt * 182.0f;
                    if (g_pl_speed > 1.0f && masc_t >= 181.0f) {
                        masc_t = 0;
                        if (rand() % 60 == 12 && !g_sim.player_unit.invulnerable && !g_sim.player_unit.shutdown) {
                            g_masc = 0; if (g_sfx) sfx_play(g_sfx, 0xc9, 0.8f, 0.0f); hud_message_v("MASC malfunction.", 0);
                            if (!g_sim.player_unit.no_heat) {   /* straight into the heat, not the frame's gain */
                                float add = g_sim.player_unit.heat * 0.25f;
                                g_sim.player_unit.heat += add; g_sim.player_unit.heat_mark += add;
                            }
                        }
                    }
                }
                int first = 0, cnt = 12;
                /* the original KEYBOARD.MAP: throttle_plus / minus on = - and keypad + -; legs pan on the arrows;
                 * torso tilt on Up / Down; torso pan on , . and End / PgDn (rates of the held keys ASSUMED) */
                {   /* DOSBox (holding =): the throttle bar from 0 reaches 35 % at 0.5 s and full before 1 s - an
                     * accelerating ramp, throttle = a t^2 / 2 with a = 2.76 / s^2; it starts again from rest on release */
                    static float tv;
                    int up = H("throttle_plus"), dn = H("throttle_minus");
                    if (up != dn) {
                        if ((up && tv < 0) || (dn && tv > 0)) tv = 0;
                        tv += (up ? 2.76f : -2.76f) * dt;
                        g_throttle += tv * dt;
                    } else tv = 0;
                }
                /* keys reversed (user request, flight style: up arrow tilts down); the mouse is unchanged */
                if (H("torso_tilt_plus")) g_pitch -= 30.0f * dt;
                if (H("torso_tilt_minus")) g_pitch += 30.0f * dt;
                if (H("torso_tilt_reset")) g_pitch = 0;
                if (H("torso_pan_minus")) g_pl.twist -= 45.0f * dt;
                if (H("torso_pan_plus")) g_pl.twist += 45.0f * dt;
                if (H("torso_pan_reset")) g_pl.twist = 0;
                {   /* the torso servo (engine 0x1001a180 end): sound 0x13c is TORSLOOP, a 0.29 s loop - held while the torso
                     * turns (twist more than 2 degrees from where it is driven), stopped when it stops; restarting it every
                     * 0.29 s buzzed */
                    static float prev_twist, still;
                    static unsigned char *loop_pcm;
                    static int loop_len, playing;
                    static float prev_heading;
                    static int primed;
                    float dh = g_pl.heading - prev_heading, rate;
                    if (!primed) { prev_heading = g_pl.heading; prev_twist = g_pl.twist; dh = 0; primed = 1; }   /* the first frame: no turn (the start heading had played TORSLOOP at t = 0) */
                    while (dh > 180.0f) dh -= 360.0f;
                    while (dh < -180.0f) dh += 360.0f;
                    /* the torso twisting, or the legs turning the whole mech (ASSUMED: the same gyro loop) */
                    rate = dt > 0 ? (fabsf(g_pl.twist - prev_twist) + fabsf(dh)) / dt : 0;
                    prev_twist = g_pl.twist; prev_heading = g_pl.heading;
                    if (!loop_pcm && g_ma) loop_len = sfx_decode(g_ma, 0x13c, &loop_pcm);
                    still = rate > 8.0f ? 0 : still + dt;
                    if (g_sfx && loop_pcm && loop_len > 0) {
                        if (rate > 8.0f && !playing) { sfx_play_pcm(g_sfx, loop_pcm, loop_len, 11025, 0.5f, 1); playing = 1; }
                        else if (playing && still > 0.12f) { sfx_stop_pcm(g_sfx, loop_pcm); playing = 0; }
                    }
                }
                {   /* analog controls (INPUT.MAP axis blocks): a stick sets the position, the mouse moves it (scales ASSUMED) */
                    int pr = 0;
                    float a;
                    a = g_im_ok ? im_axis(&g_im, "throttle", ks, 1, &pr) : 0;
                    if (pr && g_im_dev.joy) g_throttle = -a;                                      /* stick forward = ahead */
                    a = g_im_ok ? im_axis(&g_im, "torso_pan", ks, 1, &pr) : 0;
                    if (pr && g_im_dev.joy) g_pl.twist = a * (g_twist_limit < 360.0f ? g_twist_limit : 90.0f);
                    a = g_im_ok ? im_axis(&g_im, "torso_tilt", ks, 1, &pr) : 0;
                    if (pr && g_im_dev.joy) g_pitch = a * 30.0f;
                    g_pl.twist += (g_im_ok ? im_axis(&g_im, "torso_pan", ks, 2, &pr) : 0) * 0.2f;
                    g_pitch += (g_im_ok ? im_axis(&g_im, "torso_tilt", ks, 2, &pr) : 0) * 0.15f;
                    a = g_im_ok ? im_axis(&g_im, "pilot_pan", ks, 1, &pr) : 0;
                    if (pr && g_im_dev.joy) g_eye_pan = a * 90.0f;
                    a = g_im_ok ? im_axis(&g_im, "pilot_tilt", ks, 1, &pr) : 0;
                    if (pr && g_im_dev.joy) g_eye_tilt = a * 45.0f;
                    g_im_dev.mouse_dx = g_im_dev.mouse_dy = 0;
                }
                /* the pilot's view: pan / tilt keys, glances (held: 90 degrees aside, ASSUMED), recentre, zoom */
                if (getenv("MW2_EYE_PAN")) g_eye_pan = (float)atof(getenv("MW2_EYE_PAN"));   /* tests */
                /* the engine's key-driven controls (0x10039ee0): while a _plus / _minus key is held the control's velocity
                 * grows at a constant acceleration (3 << shift per tick, velocity capped at half scale per tick) and the
                 * value (-1..1) moves by it; the velocity drops to 0 on release. pilot_pan spans -70..70, pilot_tilt
                 * -60..60 (control table 0x10256540) and keeps its value after release (DOS capture); acceleration
                 * fitted to the DOS leg taps (KEY_ACCEL) */
                {
                    static float ppv, ptv;
                    int pm = H("pilot_pan_minus"), pp = H("pilot_pan_plus"), tm = H("pilot_tilt_minus"), tp = H("pilot_tilt_plus");
                    float pv = g_eye_pan / 70.0f, tv = g_eye_tilt / 60.0f;
                    if (pp || pm) { ppv += (pp ? KEY_ACCEL : -KEY_ACCEL) * dt; pv += ppv * dt; } else ppv = 0;
                    if (tp || tm) { ptv += (tp ? KEY_ACCEL : -KEY_ACCEL) * dt; tv += ptv * dt; } else ptv = 0;
                    pv = pv > 1 ? 1 : pv < -1 ? -1 : pv;
                    tv = tv > 1 ? 1 : tv < -1 ? -1 : tv;
                    g_eye_pan = pv * 70.0f; g_eye_tilt = tv * 60.0f;
                }
                if (H("pilot_pan_reset") || H("pilot_tilt_reset")) g_eye_pan = g_eye_tilt = 0;
                {   /* zoom_factor (control table: 1..4, camera +0x18 -> the projection's zoom): the same key ramp at
                     * shift 3 (12.1 /s^2 on the -1..1 control value; DOS: a 0.12 s tap of Z 1.00 -> 1.17, then 1.39;
                     * 1.5 s -> 4; Shift+Z 0.25 s 3.97 -> 3.47, measured from the radar's view cone); COCKPIT_RESET_ZOOM
                     * (Ctrl+Z) -> 1 */
                    static float zvel;
                    int zp = H("zoom_factor_plus"), zm = H("zoom_factor_minus");
                    float zv = (g_zoom - 1.0f) / 1.5f - 1.0f;
                    if (zp != zm) { zvel += (zp ? ZOOM_ACCEL : -ZOOM_ACCEL) * dt; zv += zvel * dt; } else zvel = 0;
                    zv = zv > 1 ? 1 : zv < -1 ? -1 : zv;
                    g_zoom = 1.0f + (zv + 1.0f) * 1.5f;
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
                /* engine turn = input/1024 x 90 x cos(min(80, km/h)); full key deflection taken as 1024 (inferred from the AI's 819.2 cap) */
                turn = 90.0f * cosf((kmh < 80.0f ? kmh : 80.0f) * 3.14159265f / 180.0f);
                if (g_autopilot && (H("legs_pan_minus") || H("legs_pan_plus"))) g_autopilot = 0;   /* manual leg turn cancels it */
                if (g_autopilot) {   /* engine 0x10015c00: steer to the selected nav (else the next not visited); the pilot's
                                      * throttle stays; turn = (bearing error x 18.2044, at most 819.2) / 1024 of full - 0.8 of
                                      * full beyond 45 degrees; on arrival MECNAVPT (0xe7), the nav marked visited and the next
                                      * one taken; past the last, autopilot off and the throttle to 0 */
                    static int ap_nav = -1;
                    const unsigned char *visited = g_nav_seen;   /* set by the nav reach test (0xe7 there) */
                    int nv = g_nav_sel >= 0 && g_nav_sel < g_sim.nav_count ? g_nav_sel : ap_nav;
                    if (nv >= 0 && nv < g_sim.nav_count && nv < 256 && visited[nv]) {   /* arrived: the next one, off past the last */
                        int k3;
                        if (g_nav_sel == nv) g_nav_sel = -1;
                        for (k3 = nv + 1; k3 < g_sim.nav_count && k3 < 256 && (visited[k3] || !g_sim.navs[k3].selectable); k3++) {}
                        if (k3 >= g_sim.nav_count || k3 >= 256) { g_autopilot = 0; g_throttle = 0; ap_nav = -1; nv = -2; hud_message_v("Autopilot disengaged.", 0); }
                        else nv = k3;
                    }
                    if (nv == -1 || nv >= g_sim.nav_count) {
                        for (nv = 0; nv < g_sim.nav_count && nv < 256 && (visited[nv] || !g_sim.navs[nv].selectable); nv++) {}
                        if (nv >= g_sim.nav_count || nv >= 256) nv = -1;
                    }
                    ap_nav = nv >= 0 ? nv : -1;
                    if (getenv("MW2_TEST_NAVAUTO")) { static int once; if (!once++) fprintf(stderr, "nav autopilot: nav %d\n", nv); }
                    if (nv == -1) { g_autopilot = 0; g_throttle = 0; }
                    else if (nv >= 0) {
                        float dx2 = g_sim.navs[nv].x - (float)g_pl.origin[0], dz2 = g_sim.navs[nv].z - (float)g_pl.origin[2];
                        float want = atan2f(dx2, dz2) * 180.0f / 3.14159265f, dh = want - g_pl.heading, c;
                        while (dh > 180) dh -= 360;
                        while (dh < -180) dh += 360;
                        c = dh * 18.2044f;
                        if (c > 819.2f) c = 819.2f;
                        if (c < -819.2f) c = -819.2f;
                        g_pl.heading += turn * (c / 1024.0f) * dt;
                    }
                }
                {   /* keyboard turning builds up (the engine's key ramp, see KEY_ACCEL) and stops on release */
                    static float lv, lx;
                    int lm = H("legs_pan_minus"), lp = H("legs_pan_plus");
                    if (lm != lp) {
                        if ((lp && lv < 0) || (lm && lv > 0)) lv = 0;
                        lv += (lp ? KEY_ACCEL : -KEY_ACCEL) * dt;
                        lx += lv * dt;
                        lx = lx > 1 ? 1 : lx < -1 ? -1 : lx;
                    } else lv = lx = 0;
                    g_pl.heading += turn * lx * dt;
                }
                { int pr = 0; float a = g_im_ok ? im_axis(&g_im, "legs_pan_delta", ks, 1, &pr) : 0; if (pr) g_pl.heading += a * turn * dt; }
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
                    /* inputs discarded; the torso servos run on toward the cleared demands - twist and pitch back to 0, each
                     * value += dt x (0 - value) / T with the player's T = 0.2 s (0x1003b7e0, see ai.h) */
                    float k = dt / 0.2f;
                    if (k > 1.0f) k = 1.0f;
                    g_pl.heading = heading_before; g_throttle = 0; g_throttle_eff = 0;
                    g_pl.twist = twist_before - twist_before * k;
                    g_pitch = pitch_before - pitch_before * k;
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
                msim_drive_step(&g_thr_srv, &g_spd_srv, &g_pl_speed, g_throttle_eff, 1.0f, top,
                                !(g_sim_ok && (g_sim.player_unit.shutdown || g_sim.player_unit.immobile)), g_drive_slow, dt * 182.0f);
                h = g_pl.heading * 3.14159265f / 180.0f;
                {   /* locomotion on the terrain (engine 0x10019310): steep or out-of-window steps refused */
                    float nx = (float)g_pl.origin[0] + sinf(h) * g_pl_speed * dt, nz = (float)g_pl.origin[2] + cosf(h) * g_pl_speed * dt;
                    {   /* mech-mech contact in 3D, as the AI's (msim.c; engine 0x1000ba20 spheres, 0x1000c160 / 0x1000c1f0):
                         * landing on top of a mech (normal y above 0.7071, feet within 10 m of the terrain) sets the player
                         * down beside it, no damage; any other contact damages the player by the normal (head from above x
                         * the tonnage ratio, legs from below, else arm / torso) at the engine's relative speed on every
                         * contact tick - the player only: the other mech is damaged only by its own movement's contacts
                         * (0x10019d6a -> 0x1000c160 takes the mover alone) - and clangs once per contact (latch 0x1007c338); airborne the player bounces off
                         * along the normal at half that speed, walking it bounces back 60 cm at -30 % speed */
                        static int contact = -2;
                        float ticks = dt * 182.0f, air = g_sim.player_unit.y > g_sim.player_unit.ground + 1.0f, vy = air ? g_sim.player_unit.vy : 0.0f;
                        float ch = g_sim.player_centre_h, x0 = (float)g_pl.origin[0], z0 = (float)g_pl.origin[2];
                        float p0[3] = {x0, g_sim.player_unit.y + ch, z0}, p1[3] = {nx, g_sim.player_unit.y + ch + vy * ticks, nz};
                        float n[3], c[3], rr = 0, ov[3], sv[3] = {sinf(h) * g_pl_speed / 182.0f, vy, cosf(h) * g_pl_speed / 182.0f};
                        int hit = g_sim_ok ? msim_unit_contact(&g_sim, -1, p0, p1, n, c, &rr, ov) : -2, pushout = 0;
                        g_sim.player_vel[0] = sv[0]; g_sim.player_vel[1] = sv[1]; g_sim.player_vel[2] = sv[2];
                        /* set down inside the sphere of the mech it landed on (contact < -2): the move ends inside it, whatever
                         * its direction - the engine's push-out (0x1000ba20) */
                        if (hit == -2 && g_sim_ok && contact < -2 && msim_unit_inside(&g_sim, -1, -contact - 3, p1, n, c, &rr, ov)) { hit = -contact - 3; pushout = 1; }
                        if (hit >= 0) {
                            /* the engine's speeds (0x1000ba20 copies the other's z velocity over its own): damage at
                             * |(dvx, dvy)|, bounce at half |(dvx, dvy, the other's vz)|, at least 1 cm/tick */
                            float dvx = sv[0] - ov[0], dvy = sv[1] - ov[1], sp = sqrtf(dvx * dvx + dvy * dvy), bs = 0.5f * sqrtf(dvx * dvx + dvy * dvy + ov[2] * ov[2]);
                            float py = c[1] + n[1] * rr - ch, hl = sqrtf(n[0] * n[0] + n[2] * n[2]);
                            float gnd = msim_ground(&g_sim, c[0] + n[0] * rr, c[2] + n[2] * rr, py);
                            if (n[1] > 0.70710677f && py - gnd < 1000.0f && py - gnd > -10000.0f) {   /* landed on it (0x10019c4e) */
                                /* the engine: projected onto the sphere (0x1000ba20), the feet taken as 1 cm under the terrain
                                 * there (local_5c = -1): stood on the terrain at that point, inside the sphere, vy 0, the
                                 * velocity n x max(1, dv / 2); the next frame's move pushes it out (above) */
                                float gx = c[0] + n[0] * rr, gz = c[2] + n[2] * rr;
                                (void)hl;
                                if (bs < 1.0f) bs = 1.0f;
                                g_pl.origin[0] = (int32_t)lrintf(gx); g_pl.origin[2] = (int32_t)lrintf(gz);
                                g_sim.player_unit.ground = gnd;
                                g_sim.player_unit.y = g_sim.player_unit.ground; g_sim.player_unit.vy = 0;
                                g_pl_speed = (n[0] * sinf(h) + n[2] * cosf(h)) * bs * 182.0f;   /* along the heading */
                                contact = -hit - 3;   /* landed on it: pushed out next frame */
                            } else {
                                if (g_sim.collision_damage)   /* every contact tick, the player only */
                                    combat_collision_unit(&g_sim.player_unit, sp, n, g_pl.heading + g_pl.twist, g_sim.units[hit].tons, &g_sim.rng);
                                if (contact != hit) {
                                    if (g_sfx && sp > 0.5f) {
                                        static int kid = -2;
                                        if (kid == -2) { int st = g_ma ? prj_find_type(g_ma, "SNDS") : -1; kid = st >= 0 ? prj_find_id(g_ma, st, "MECBLDC1") : -1; }
                                        if (kid > 0) sfx_play(g_sfx, kid, sp > 23.02f ? 1.0f : sp / 23.02f, 0.0f);   /* 0x10019d10: full above 23 cm/tick */
                                    }
                                    contact = hit;
                                }
                                if (air || pushout) {   /* bounced off / pushed out along the normal */
                                    float gx = c[0] + n[0] * (rr + 1.0f), gz = c[2] + n[2] * (rr + 1.0f);
                                    if (bs < 1.0f) bs = 1.0f;
                                    g_pl.origin[0] = (int32_t)lrintf(gx); g_pl.origin[2] = (int32_t)lrintf(gz);
                                    g_sim.player_unit.y = c[1] + n[1] * (rr + 1.0f) - ch;
                                    g_sim.player_unit.ground = msim_ground(&g_sim, gx, gz, g_sim.player_unit.y);
                                    if (g_sim.player_unit.y < g_sim.player_unit.ground) g_sim.player_unit.y = g_sim.player_unit.ground;
                                    g_sim.player_unit.vy = g_sim.player_unit.y <= g_sim.player_unit.ground + 1.0f ? 0.0f : n[1] * bs;   /* on the ground: vy 0 */
                                    g_pl_speed = (n[0] * sinf(h) + n[2] * cosf(h)) * bs * 182.0f;
                                } else {
                                    float bx = x0 - c[0], bz = z0 - c[2], bl = sqrtf(bx * bx + bz * bz);
                                    if (bl > 1.0f) { g_pl.origin[0] += (int32_t)lrintf(bx / bl * 60.0f); g_pl.origin[2] += (int32_t)lrintf(bz / bl * 60.0f); }   /* bounced back */
                                    g_pl_speed *= -0.3f;
                                }
                            }
                            nx = (float)g_pl.origin[0]; nz = (float)g_pl.origin[2];
                        } else contact = -2;
                    }
                    if (g_sim.player_unit.y > g_sim.player_unit.ground + 1.0f || msim_can_step_hr(&g_sim, (float)g_pl.origin[0], (float)g_pl.origin[2], nx, nz, g_sim.player_unit.y, g_sim.player_centre_h, g_sim.player_radius)) {
                        g_pl.origin[0] = (int32_t)lrintf(nx);
                        g_pl.origin[2] = (int32_t)lrintf(nz);
                        g_sim.player_unit.blocked = 0;
                    } else {   /* walked into steep terrain: collision damage at speed (engine 0x1000c3c0 / 0x1000c1f0) */
                        float wn[3];
                        msim_block_normal(wn);   /* the refusing surface's normal (the engine's hit normal), as the AI's - the
                                                  * ground's normal at the target had sent an object's face hit to the legs */
                        if (getenv("MW2_WALK_TRACE")) { static int32_t wtl = -1000; if (g_sim.now - wtl >= 500) { wtl = g_sim.now; wt_block((float)g_pl.origin[0], (float)g_pl.origin[2], nx, nz); } }   /* TEST ONLY */
                        if (!g_sim.player_unit.blocked && g_sfx && fabsf(g_pl_speed) / 182.0f > 3.07f) {
                            /* engine 0x10019cf8: the impact sound - 0xc8 MECBLDC1 against an object, 0xe5 MECMTNHD against
                             * the terrain; volume speed / 23 cm/tick, full above */
                            float sp3 = fabsf(g_pl_speed) / 182.0f;
                            int wall = msim_wall_at(&g_sim, nx, nz, NULL);
                            sfx_play(g_sfx, wall ? 0xc8 : 0xe5, sp3 > 23.02f ? 1.0f : sp3 / 23.02f, 0.0f);
                        }
                        /* every contact frame at the attempted speed (0x10019d84 -> 0x1000c3c0); the impact itself cuts
                         * the move back and reflects the velocity at a quarter (0x1000b5e0, msim_world_impact) */
                        combat_collision(&g_sim.player_unit, fabsf(g_pl_speed) / 182.0f, wn[1],
                                         atan2f(-wn[0], -wn[2]) * 57.29578f - (g_pl.heading + g_pl.twist), &g_sim.rng);
                        {
                            float bx = nx, bz = nz;
                            msim_world_impact(&g_sim, (float)g_pl.origin[0], (float)g_pl.origin[2], &bx, &bz, g_sim.player_unit.y, g_sim.player_centre_h,
                                              g_sim.player_radius, wn, g_pl.heading, &g_pl_speed);
                            g_pl.origin[0] = (int32_t)lrintf(bx); g_pl.origin[2] = (int32_t)lrintf(bz);
                        }
                    }
                }
                {   /* gait by throttle (engine 0x1000b3a0): < 25% seq 0, 25-75% seq 1, >= 75% seq 2 (run).
                     * Key timing (engine 0x1000dec0 / 0x1000e600): every key lasts the TSK duration
                     * (Timber Wolf 70 ticks) x 1.5 / 1 / 0.75 for gait 1 / 2 / 3; not tied to ground speed. */
                    static const float MOD[3] = {1.5f, 1.0f, 0.75f};
                    float frac = fabsf(g_thr_srv);   /* the current throttle c[0x13] (0x1000b3a0) */
                    /* the three sequences are walk, run and REVERSE (their strides: Nova +101 / +211 / -105 cm) - the
                     * fastest throttles had picked the backwards walk. Forward: walk below half speed, run above
                     * (threshold ASSUMED); backing up: sequence 2 */
                    /* engine 0x1000b3a0: speed over the stop level, as a fraction of full: gait 1 under 1/4, 2 under
                     * 3/4, else 3; the sequence is the walk below gait 3, the run at gait 3, the reverse walk backing up */
                    int gait = frac < 0.25f ? 0 : frac < 0.75f ? 1 : 2;
                    int seq = g_pl_speed < -1.0f ? 2 : gait == 2 ? 1 : 0;
                    static int last_seq = -1;
                    int r0 = g_pl.anim_rate > 0 ? g_pl.anim_rate : 70;
                    float dur = (float)(gait == 0 ? r0 + (r0 >> 1) : gait == 1 ? r0 : r0 - (r0 >> 2));   /* key time by gait, integer (0x1000e600) */
                    (void)MOD;
                    if (anim_sequence(&g_pl_anim, seq, &first, &cnt) != 0) { seq = 0; anim_sequence(&g_pl_anim, 0, &first, &cnt); }
                    if (seq != last_seq) { g_pl_t = 0; last_seq = seq; }
                    if (fabsf(g_pl_speed) > 1.0f) {
                        /* footsteps (engine 0x1000b3a0 / 0x1000e600): entering a key whose flags carry 0x800 (a foot
                         * coming down) raises the mech's footstep flag, and the controller plays its footstep sound -
                         * 0x103 for every mech but the player, whose comes from its record (ASSUMED the same here) -
                         * at the mech; the gait (1 walk, 2 trot, 3 run by speed) sets the key time, so the steps
                         * follow the speed */
                        int k0 = (int)g_pl_t, k1;
                        {   /* the walk cycle loops from the key before its first footfall: wrapping to the sequence's
                             * first key replayed the lead-in from standing - walk, walk, pause */
                            /* the stride loops left - right - left: from the first footfall over two footfalls' worth of
                             * keys (the keys after them return to standing and were the "stop"); the lead-in plays once.
                             * Step rate: footfalls 1 + 1.4 x (speed / top) a second, at least the engine's key time */
                            int ls, le;
                            anim_loop_range(&g_pl_anim, first, cnt, &ls, &le);
                            if (getenv("MW2_STEP_TRACE")) { static int lq = -1; if (lq != seq) { lq = seq; fprintf(stderr, "seq %d first %d count %d loop %d..%d\n", seq, first, cnt, ls, le); } }
                            g_pl_t += dt * 182.0f / dur;   /* the engine's key time only: no coupling to ground speed */
                            if (g_pl_t >= (float)le) g_pl_t = (float)ls + fmodf(g_pl_t - (float)le, (float)(le - ls > 0 ? le - ls : 1));
                        }
                        k1 = (int)g_pl_t;
                        if (first + k1 < g_pl_anim.key_count && (g_pl_anim.key_flags[first + k1] & 0x10)) g_drive_slow = 1;   /* 0x1001a180: T 90.5 from now on */
                        if (k1 != k0 && first + k1 < g_pl_anim.key_count && (g_pl_anim.key_flags[first + k1] & 0x800) && g_sfx && !g_sim.player_unit.destroyed)
                        {   /* the player's own footstep: the mech record's ASND chunk {u16 sound, name} (Mad Dog, Nova, Timber
                             * Wolf MECFSMED 0xd6; Jenner, Kit Fox MECLIGHT 0xe0) - engine 0x1000b3a0 plays object +0x90, set
                             * from it (+0x44) for the player; other mechs NONFOOT 0x103 */
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
                            sfx_play(g_sfx, fs, VIEW_IN_COCKPIT ? 0.5f : 1.0f, 0.0f);   /* 0x10032ad0: halved in the cockpit view (DAT_1024e678) */
                            if (getenv("MW2_STEP_TRACE")) fprintf(stderr, "footstep key %d sound %d\n", first + k1, fs);
                        }
                    }
                    {   /* gait sounds (engine 0x1000b4e0, the player only): the samples name the changes - STOP2WLK 302,
                         * WLK2STOP 335, WALK2RUN 328, RUN2WLK 282, STOP2REV 301, REV2STOP 281 - played when the gait
                         * changes between stopped, walking (or trotting), running and reversing */
                        static int prev_cls = 0;
                        int cls = fabsf(g_pl_speed) < 1.0f ? 0 : g_pl_speed < 0 ? 3 : seq == 1 ? 2 : 1, snd2 = -1;
                        if (cls != prev_cls) {
                            if (prev_cls == 0) snd2 = cls == 3 ? 301 : 302;
                            else if (cls == 0) snd2 = prev_cls == 3 ? 281 : 335;
                            else if (prev_cls == 1 && cls == 2) snd2 = 328;
                            else if (prev_cls == 2 && cls == 1) snd2 = 282;
                        }
                        if (snd2 > 0 && g_sfx && !g_sim.player_unit.destroyed) sfx_play(g_sfx, snd2, 0.7f, 0.0f);
                        prev_cls = cls;
                    }
                }
                mech3d_pose(&g_pl, &g_pl_anim, (float)first + g_pl_t);
                if (g_plpit_ok) {
                    memcpy(g_plpit.origin, g_pl.origin, sizeof g_pl.origin);
                    g_plpit.heading = g_pl.heading; g_plpit.twist = g_pl.twist;
                    mech3d_pose(&g_plpit, getenv("MW2_PIT_NOANIM") ? NULL : &g_pl_anim, (float)first + g_pl_t);
                }
                if (g_sim_ok) {   /* the mounts as drawn (walk animation, torso twist) for the shots and jet flames */
                    memcpy(g_sim.pl_shown.mount_pos, g_pl.mount_pos, sizeof g_pl.mount_pos);
                    memcpy(g_sim.pl_shown.mount_from, g_pl.mount_from, sizeof g_pl.mount_from);
                    g_sim.pl_shown.mount_ok = g_pl.mount_ok;
                    g_sim.pl_shown_ok = 1;
                }
            }
            if (g_sim_ok && g_pl_ok && g_pilot) {
                float d;
                /* jump jets (hold J): engine physics in combat_jets */
                g_sim.player_unit.ground = msim_ground(&g_sim, (float)g_pl.origin[0], (float)g_pl.origin[2], g_sim.player_unit.y);
                {
                    const Uint8 *ks = test_ks();   /* TEST hook: + MW2_HOLD */
                    int jet = (H("jumpjet_enabled") || (g_auto_on && g_auto_jet)) && !(g_sim_ok && ((g_sim.now < g_sim.player_online_at && !getenv("MW2_NO_STARTUP")) || g_sim.player_unit.shutdown));
                    {   /* directional jets (INPUT.MAP jumpjet_fire_left / right / forward / backward, each also enabling
                         * the jets; engine physics 0x10019310): while jetting and the legs are not turning, the FIRST held
                         * direction in the order left, right, forward, back pushes at the jets' thrust (+0xec) along it -
                         * forward (sin h, cos h), left (-cos h, sin h), right (cos h, -sin h); no lift meanwhile. On the
                         * ground the drift eases toward the walking velocity at 1/45 per tick (0x102473b4) and snaps to
                         * zero under 0.0625 cm/tick - it does not stop dead on landing. Drag only while thrusting directionally (below). */
                        static float jvx, jvz;
                        int l2 = H("jumpjet_fire_left"), r2 = !l2 && H("jumpjet_fire_right"), f2 = !l2 && !r2 && H("jumpjet_fire_forward");
                        int b2 = !l2 && !r2 && !f2 && H("jumpjet_fire_backward");
                        int legs_turning = H("legs_pan_minus") || H("legs_pan_plus");
                        float hh = g_pl.heading * 3.14159265f / 180.0f, dv = 0, dx = 0, dz = 0;
                        if (legs_turning) l2 = r2 = f2 = b2 = 0;
                        if (f2) { dx += sinf(hh); dz += cosf(hh); }
                        if (b2) { dx -= sinf(hh); dz -= cosf(hh); }
                        if (r2) { dx += cosf(hh); dz -= sinf(hh); }
                        if (l2) { dx -= cosf(hh); dz += sinf(hh); }
                        combat_jets_forward(&g_sim.player_unit, (f2 || b2 || l2 || r2) && jet, dt, &dv);
                        jvx += dx * dv * 182.0f; jvz += dz * dv * 182.0f;                      /* cm/tick -> cm/s */
                        if (dv > 0) {   /* the drag inside the directional branch (0x100194fa..0x10019618), cm/tick: T thrust,
                                         * L = 2.5 x top ground speed (+0x88 x 0.0390625), S speed; terminal speed = L */
                            float ticks = dt * 182.0f, T = dv / (ticks > 1e-6f ? ticks : 1e-6f), L = 2.5f * g_pl_walk / 182.0f;
                            float vx = jvx / 182.0f, vz = jvz / 182.0f, S = sqrtf(vx * vx + vz * vz);
                            if (S > 1e-7f && L > 1e-7f && T > 1e-7f) {
                                float kq = T * (1.0f - 1.0f / (3.0f * L)) / (L * L - 0.75f * L), B = (T - kq * L * L) / L, f = kq * S + B;
                                jvx -= vx * f * ticks * 182.0f; jvz -= vz * f * ticks * 182.0f;
                            }
                            if (fabsf(jvx) < 0.0625f * 182.0f) jvx = 0;
                            if (fabsf(jvz) < 0.0625f * 182.0f) jvz = 0;
                        }
                        if (g_sim.player_unit.y <= g_sim.player_unit.ground + 1.0f && !jet) {
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
                    }
                    combat_jets(&g_sim.player_unit, jet, dt, &g_sim.rng);
                    if (jet && g_sim.player_unit.jet_fuel > 0) msim_jet_flames_at(&g_sim, &g_pl, (float)g_pl.origin[0], g_sim.player_unit.y, (float)g_pl.origin[2], g_pl.heading);
                    {   /* the player's damage alarms (engine 0x100167b0: a critical hit to a component or a location lost
                         * sounds 0xd0) */
                        static int prev_slots = -1, prev_gone = -1;
                        int lq, sq, nslots = 0, ngone = 0;
                        for (lq = 0; lq < 8; lq++) { ngone += g_sim.player_unit.loc_gone[lq] != 0; for (sq = 0; sq < MEK_SLOTS; sq++) nslots += g_sim.player_unit.slots[lq][sq] != 0; }
                        if (g_sfx && prev_slots >= 0 && !g_sim.player_unit.destroyed && (nslots < prev_slots || ngone > prev_gone)) sfx_play(g_sfx, 0xd0, 0.6f, 0.0f);
                        prev_slots = nslots; prev_gone = ngone;
                    }
                    /* touching down on the terrain from a fall or a jump (engine 0x10019310 at 0x10019e28: faster than 5.384
                     * cm/tick -> 0x1001c0a0): MECMTNSF 0xe6 up to 16.154 cm/tick, MECMTNHD 0xe5 faster (0xe5 + (vy >= -16.154)),
                     * at the mech (0x10032bf0, positional), and for the player the camera jolt 0x1001c120(0, -vy, 0); a
                     * destroyed mech lands as a wreck (0x1000c4d0) without either. The port had sounded 0xf0 (that is the
                     * object-landing branch, 0x10019c1d) */
                    if (g_sim.player_unit.landed_vy < -5.384f && !g_sim.player_unit.destroyed) {
                        if (g_sfx) sfx_play(g_sfx, g_sim.player_unit.landed_vy >= -16.154f ? 0xe6 : 0xe5, 0.8f, 0.0f);
                        jolt_start(0.0f, -g_sim.player_unit.landed_vy, 0.0f, g_sim.now);
                    }
                    {   /* reaching a nav point (engine 0x10009b80: the player inside an area's radius the first time sets its
                         * visited flag and beeps 0xe7); the mission's nav points, their radius (60 m at least) */
                        unsigned char *seen = g_nav_seen;
                        static const msim *seen_for;
                        int nv;
                        if (seen_for != &g_sim) { memset(g_nav_seen, 0, sizeof g_nav_seen); seen_for = &g_sim;
                            if (getenv("MW2_TEST_NAVSEEN")) { const char *q = getenv("MW2_TEST_NAVSEEN"); while (*q) { int k = atoi(q); if (k >= 0 && k < 256) g_nav_seen[k] = 1; while (*q && *q != ',') q++; if (*q) q++; } }   /* TEST ONLY: navs already visited */
                        }
                        for (nv = 0; nv < g_sim.nav_count && nv < 256; nv++) {
                            const msim_nav *np = &g_sim.navs[nv];
                            float r2 = np->radius, ddx = np->x - (float)g_pl.origin[0], ddz = np->z - (float)g_pl.origin[2];
                            if (r2 < 6000.0f) r2 = 6000.0f;
                            if (seen[nv] || !np->shown) continue;   /* 0x10009b80: only shown navs (NAVP +16): not the AI's goal / leave areas (YELLGOU2) */
                            if (ddx * ddx + ddz * ddz < r2 * r2) { seen[nv] = 1; if (g_sfx) sfx_play(g_sfx, 0xe7, 0.6f, 0.0f); }
                        }
                    }
                    {   /* jet sounds (engine 0x1001bf90, every tick the jets burn): sound 0xdf at the jets, held while
                         * they fire; the player's fuel falling through 0x1c4 ticks (a quarter left) sounds the warning
                         * tone 0xde */
                        static float prev_fuel = -1;
                        float fuel = g_sim.player_unit.jet_fuel;
                        if (jet && fuel > 0 && g_sfx && !sfx_playing(g_sfx, 0xdf)) sfx_play(g_sfx, 0xdf, 0.7f, 0.0f);
                        if (prev_fuel >= 452.0f && fuel < 452.0f && g_sfx) sfx_play(g_sfx, 0xde, 0.5f, 0.0f);   /* 0x10032b60: id 0xde, volume 0x40 */
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
                                        if (g_sfx) sfx_play(g_sfx, 0xfd, 0.4f, 0.0f);   /* engine 0x1001adf0: the selector click */
                                        break;
                                case 1: g_cur_group = (g_cur_group + 1) % 3; break;
                                case 2: g_group_fire = !g_group_fire; hud_message_v(g_group_fire ? "Group fire" : "Chain fire", g_group_fire ? 19 : 18); break;
                                case 3: case 4: {
                                    int i2, qq, step = q == 3 ? 1 : n2 - 1;
                                    for (i2 = 1; i2 <= n2; i2++) {
                                        qq = ((g_tgt_sel < 0 ? (q == 3 ? -1 : 0) : g_tgt_sel) + step * i2 + n2 * 4) % n2;
                                        if (g_sim.armed[qq] && !g_sim.units[qq].destroyed && !g_actors[qq].friendly) { g_tgt_sel = qq; break; }
                                    }
                                    break; }
                                case 5: {   /* TARGET_AT_RETICLE (q): whatever is closest to the reticle within a few degrees -
                                             * a mech, else a structure; nothing there clears the target (back to the nav point) */
                                    float dd2, dd3;
                                    g_tgt_thing = -1;
                                    g_tgt_sel = msim_aim_target(&g_sim, g_pl.heading + g_pl.twist, 5.0f, &dd3);
                                    if (g_tgt_sel < 0) g_tgt_thing = msim_aim_thing(&g_sim, g_pl.heading + g_pl.twist, 5.0f, &dd2);
                                } break;
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
                                case 9: if (g_sim.nav_count > 0) g_nav_sel = nav_step(g_nav_sel >= 0 ? g_nav_sel : current_nav(NULL), 1); break;
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
                                sfx_play(g_sfx, 0x149, 0.5f, 0.0f);
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
                {   /* the aim ray's origin: the eye the cockpit camera rides (repr 4's EYEO, else the full model's) */
                    float ep[3];
                    g_sim.player_eye_ok = (g_plpit_ok && !getenv("MW2_EYE_FULL") && mech3d_head_point(&g_plpit, ep) == 0) || mech3d_head_point(&g_pl, ep) == 0;
                    if (g_sim.player_eye_ok) memcpy(g_sim.player_eye, ep, sizeof ep);
                }
                g_sim.player_lock_target = g_tgt_sel;   /* the lock works on the selected target (T / E / Q) */
                g_sim.radio_busy = g_radio_qn > 0;
                if (!g_mm_depth && !g_paused) msim_step(&g_sim, sim_ms(dt));
                if (!g_mm_depth && !g_paused) player_upkeep();   /* self-destruct countdown, automatic ejection */
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
                    {   /* glance_left / right / up / down held: 90 degrees aside / 45 up / down (ASSUMED angles) */
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
                        mech3d *lm = actor_lod(k, src, g_actors[k].skel, dead ? -1.0f : sqrtf(ddx * ddx + ddy * ddy + ddz * ddz));
                        if (lm && lm->part_count > src->part_count) {
                            mech3d_part *grown = realloc(all.parts, (size_t)(total + lm->part_count) * sizeof *all.parts);
                            if (grown) { all.parts = grown; total += lm->part_count; } else lm = NULL;
                        }
                        if (lm) src = lm;
                        for (q = 0; q < src->part_count; q++) {   /* blown-off locations aren't drawn */
                            int gq = src->parts[q].group;
                            if (mech3d_fly_loc(src, q, fly)) continue;
                            all.parts[all.part_count] = src->parts[q];
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
                    for (q = 0; q < g_pl.part_count; q++) {
                        if (mech3d_fly_loc(&g_pl, q, fly)) continue;
                        all.parts[all.part_count++] = g_pl.parts[q];
                    }
                }
                for (k = 0; all.parts && g_proj_ok && k < g_sim.shot_count; k++) {
                    /* each shot: its weapon's model at its position, pointing along its velocity */
                    const msim_shot *sh = &g_sim.shots[k];
                    int ps = proj_slot_w(sh->weapon);
                    mech3d_part pp;
                    if (ps < 0) continue;
                    if (g_camode == 3 && g_follow && sh->id == g_follow) continue;   /* the ordnance camera rides it (DOSBox: not in view) */
                    if (ps <= 2 && g_dos_lasers) continue;   /* MW2_DOS_LASERS=1: the DOS glowing balls (cards, below) */
                    pp = g_proj[ps];
                    float hv = sqrtf(sh->vx * sh->vx + sh->vz * sh->vz), yw = atan2f(sh->vx, sh->vz), pt = atan2f(sh->vy, hv);
                    float cy = cosf(yw), sy = sinf(yw), cp = cosf(pt), sp = sinf(pt);
                    if (pp.model.object_count < 1) continue;
                    pp.pos[0] = (int32_t)sh->x; pp.pos[1] = (int32_t)sh->y; pp.pos[2] = (int32_t)sh->z;
                    pp.has_rot = 1;            /* R = Ryaw * Rpitch (model +z forward; a climbing shot's nose up - the
                                                * pitch had been applied the wrong way round) */
                    pp.rot[0] = cy;  pp.rot[1] = -sy * sp; pp.rot[2] = sy * cp;
                    pp.rot[3] = 0;   pp.rot[4] = cp;       pp.rot[5] = sp;
                    pp.rot[6] = -sy; pp.rot[7] = -cy * sp; pp.rot[8] = cy * cp;
                    if (ps <= 2) {
                        /* the 3D editions' laser bolt (engine 0x10043cc0 / 0x10044ac0): the LASER1-3 mesh with its MW2_SHT1
                         * OBJ scale (3, 3, 12) - long thin bolts, 43 / 30 / 19 m - set along the velocity at launch, no glow;
                         * the model lies behind its origin (nose at z 0), and the engine puts that origin one model length
                         * ahead of the muzzle, so the bolt's tail leaves the muzzle: drawn here one scaled length ahead of
                         * the shot point (the shot itself, and its hits, still start at the muzzle) */
                        int q;
                        float L = (g_proj_len[ps] > 0 ? g_proj_len[ps] : 300.0f) * 12.0f, vl = sqrtf(sh->vx * sh->vx + sh->vy * sh->vy + sh->vz * sh->vz);
                        for (q = 0; q < 3; q++) { pp.rot[q * 3 + 0] *= 3.0f; pp.rot[q * 3 + 1] *= 3.0f; pp.rot[q * 3 + 2] *= 12.0f; }
                        if (vl > 0) {
                            pp.pos[0] = (int32_t)(sh->x + sh->vx / vl * L);
                            pp.pos[1] = (int32_t)(sh->y + sh->vy / vl * L);
                            pp.pos[2] = (int32_t)(sh->z + sh->vz / vl * L);
                        }
                    }
                    pp.group = 0;
                    all.parts[all.part_count++] = pp;
                }
                for (k = 0; all.parts && g_proj_ok && k < g_sim.debris_count; k++) {   /* debris: tumbling chunks */
                    const msim_debris *d = &g_sim.debris[k];
                    mech3d_part pp;
                    if (d->kind == 3) { if (!g_sim.world || d->part >= g_sim.world->part_count) continue; pp = g_sim.world->parts[d->part]; pp.hidden = 0; pp.moving = 0; }
                    else pp = d->kind == 2 ? (d->actor < 0 ? g_pl.parts[d->part] : g_actors[d->actor].mech.parts[d->part]) : g_proj[7 + d->kind];
                    float a = d->ang[0] * 0.0174533f, b = d->ang[1] * 0.0174533f, c = d->ang[2] * 0.0174533f;
                    float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b), cc = cosf(c), sc = sinf(c);
                    if (pp.model.object_count < 1) continue;
                    pp.pos[0] = (int32_t)d->p[0]; pp.pos[1] = (int32_t)d->p[1]; pp.pos[2] = (int32_t)d->p[2];
                    pp.has_rot = 1;            /* R = Ry(b) Rx(a) Rz(c) */
                    pp.rot[0] = cb * cc + sb * sa * sc; pp.rot[1] = -cb * sc + sb * sa * cc; pp.rot[2] = sb * ca;
                    pp.rot[3] = ca * sc;                pp.rot[4] = ca * cc;                 pp.rot[5] = -sa;
                    pp.rot[6] = -sb * cc + cb * sa * sc; pp.rot[7] = sb * sc + cb * sa * cc; pp.rot[8] = cb * ca;
                    if (d->kind < 2) { int q; for (q = 0; q < 9; q++) pp.rot[q] *= 2.0f; }   /* the debris objects' OBJ scale 2 (MW2_SHT1) */
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
            if (last && !g_mm_depth && !g_paused) v.sky_scroll += sg.tile_sky * sg.anim_rate * (float)(now - last) * 0.03f;   /* held while paused */
            last = now;
        }
        SDL_GL_GetDrawableSize(win, &w, &h);   /* real pixels, incl. HiDPI */
        g_out_w = w; g_out_h = h;
        g_win_lines = h;
        if (g_render_w && g_world) {
            /* the original resolutions: the frame is drawn at 640 x 480 (1024 x 768 for that choice) - the scale the HUD
             * art and layout are made for - then, for 320 x 200, point-sampled down to 320 x 200; the result is shown
             * scaled up in whole blocks to fill 4:3 (non-square pixels at 320 x 200, as a VGA monitor showed them) */
            int dw = g_render_w == 320 ? 640 : g_render_w, dh = g_render_w == 320 ? 480 : g_render_h;
            if (lowres_target(dw, dh) == 0) { w = dw; h = dh; glBindFramebuffer(GL_FRAMEBUFFER, g_lr_fbo[0]); }
        }
        music_poll();
        if (g_mm_ed_req >= 0) { switch_look(r, &v, ma, &ta, &ati, mission, g_mm_ed_req); g_mm_ed_req = -1; }   /* Video mode */
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
        if (g_world && g_fx_ok && getenv("MW2_FX_TEST")) {   /* test: spawn an effect 30 m ahead of the player at 1 s */
            static int done;
            if (!done && g_sim.now >= 1000) {
                float h = (g_pl.heading + g_pl.twist) * 3.14159265f / 180.0f;
                float p[3] = {(float)g_pl.origin[0] + sinf(h) * 3000.0f, g_sim.player_unit.y + 950.0f, (float)g_pl.origin[2] + cosf(h) * 3000.0f};
                on_effect(NULL, (int)strtol(getenv("MW2_FX_TEST"), NULL, 16), p);
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
                spr[n].half_w = g_fxd.half_w[e->type];
                spr[n].half_h = g_fxd.half_h[e->type];
                spr[n].tex = t;
                n++;
            }
            /* the PPC and gauss projectiles: cards (type 0x3000 -> 0x1002a8e0): texture slot (w & 0xff0) >> 4 of bank 0 -
             * 0x30b0 -> 0x0b a9ppcs01, 0x30d0 -> 0x0d a9gaus01, single frames; a square |CA| wide: PPCSHOT1 (centre to axis
             * 100 cm x scale 2) 4 m, GAS0_0 2 m */
            for (k = 0; g_sim_ok && k < g_sim.shot_count && n < GLR_MAX_SPRITES; k++) {
                const msim_shot *sh = &g_sim.shots[k];
                int kind = combat_weapon_kind(sh->weapon), slot, f;
                const fx_anim *an;
                texture *t;
                if ((kind != 6 && kind != 7) || !combat_weapon_has_object(sh->weapon)) continue;
                slot = kind == 6 ? 0x0b : 0x0d;
                an = &g_fxd.tex[slot];
                if (an->frames <= 0) continue;
                f = an->rate > 0 ? (int)((now / an->rate) % an->frames) : 0;
                if (!(t = fx_texture(an->cel[f]))) continue;
                spr[n].pos[0] = sh->x; spr[n].pos[1] = sh->y; spr[n].pos[2] = sh->z;
                spr[n].half_w = spr[n].half_h = kind == 6 ? 200.0f : 100.0f;
                spr[n].tex = t;
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
                        n++;
                    }
                }
            }
            glr_set_sprites(r, spr, n);
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
                    dx = e->pos[0] - v.eye[0]; dy = e->pos[1] - v.eye[1]; dz = e->pos[2] - v.eye[2];
                    d2 = dx * dx + dy * dy + dz * dz;
                    if (li >= 0) {
                        float ex = g_fxw.e[li].pos[0] - v.eye[0], ey = g_fxw.e[li].pos[1] - v.eye[1], ez = g_fxw.e[li].pos[2] - v.eye[2];
                        if (d2 >= ex * ex + ey * ey + ez * ez) continue;
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
            }
        }
        /* cockpit: 90 degrees horizontal at 4:3 = 73.74 vertical (DOS, measured against the same scene); chase: 40 */
        if (g_world) v.vfov = g_cockpit ? 2.0f * atanf(tanf(73.74f * 3.14159265f / 360.0f) / g_zoom) * 180.0f / 3.14159265f : 40.0f;   /* zoom_factor narrows the cockpit view */
        {   /* 0x1002df60 (each frame from 0x1002dc90): the camera's zoom (+0x18) changed - VIEWZOOM 0x147 (0x50); the
             * cockpit zoom factor only here (the engine's other view zooms, 0x1002e210's 2.0, not tracked) */
            static float last_zoom = 1.0f;
            if (g_world && g_cockpit && g_zoom != last_zoom && g_sfx) sfx_play(g_sfx, 0x147, 0.8f, 0.0f);
            last_zoom = g_zoom;
        }
        if (g_world && g_sim_ok && g_pl_ok && g_pilot) camera_modes(&v);   /* ordnance (3) / ejection (4) camera */
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
        if (getenv("MW2_TEST_TARGET")) { static int set; if (!set++) g_tgt_sel = atoi(getenv("MW2_TEST_TARGET")); }   /* tests */
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
        if (getenv("MW2_RADARMODE")) { g_radar_big = atoi(getenv("MW2_RADARMODE")) == 1; g_satmap = atoi(getenv("MW2_RADARMODE")) == 2; g_radar_mode = g_radar_big ? 2 : 1; }   /* tests */
        glr_draw(r, &v, w, h);
        if (g_sim_ok) {   /* the HUD's power state (see pow_update): 3 shut down, 1 starting, else 2 */
            const combat_unit *pp = &g_sim.player_unit;
            int st = pp->destroyed ? 4 : pp->shutdown ? 3 : (g_sim.now < g_sim.player_online_at && !getenv("MW2_NO_STARTUP")) ? 1 : 2;
            static int prev_st = -1;
            pow_update(st == 4 ? 2 : st, g_sim.now);
            /* engine 0x1001e6c0 (end): the player's controller state changing to 2 (operational - the start-up done, also
             * after a restart) sounds 0xce MECBSYRX (0x10032b60(0xce, 0x64, 0x2f, 5, 0x32)). DOSBox: YELLSCN1 / TNJ1SCN1,
             * at the frame the full HUD appears; relative to MECTURX2 0.4x as loud, right of centre (L 61 : R 95) */
            if (prev_st == 1 && st == 2 && g_sfx) sfx_play(g_sfx, 0xce, 0.38f, 0.36f);
            /* ... and to 4 (destroyed) - or 0, the restart request, which the mech's own update turns into 1 before the
             * HUD sees it (DOSBox: no MECTRDXX on Ctrl+s) - 0xf6 MECTRDXX (0x10032b60(0xf6, pan 0x64, volume 0x40):
             * the arguments read as pan 0..127 / volume 0..127, as MECBSYRX's 0x64 / 0x2f match DOSBox's L 61 : R 95 and
             * 0.4x level) */
            if (prev_st >= 0 && prev_st != 4 && st == 4 && g_sfx) sfx_play(g_sfx, 0xf6, 0.40f, 0.36f);
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
            int px = (int)(x0 + 516.9f * sx / 640.0f), pw = (int)(102.5f * sx / 640.0f);
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
            int px = (int)(x0 + ((float)rr[0] * 2.0f) * sx / 640.0f), pw = (int)(((float)rr[2] * 2.0f) * sx / 640.0f);
            int ph = (int)(((float)rr[3] * 2.4f) * (float)h / 480.0f), py = h - (int)(((float)rr[1] * 2.4f) * (float)h / 480.0f) - ph;
            {   /* powering up / down: the window about its centre (0x10021730 / 0x100217f0 draw it there) */
                int npw = (int)((float)pw * td_wf), nph = (int)((float)ph * td_hf);
                px += (pw - npw) / 2; py += (ph - nph) / 2; pw = npw > 1 ? npw : 1; ph = nph > 1 ? nph : 1;
            }
            if (ll < 1.0f) { lx = 0; lz = 1; ll = 1; }
            vv.free_cam = 1;
            vv.eye[0] = (float)ta->mech.origin[0] - lx / ll * 3.0f * R; vv.eye[2] = (float)ta->mech.origin[2] - lz / ll * 3.0f * R;
            vv.eye[1] = (float)ta->mech.origin[1] + ch;
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
            } else if (in_ph) {   /* 3D editions: the textured world in the fog colour (255, 8, 8) while fading in */
                static const float mul[3] = {1.0f, 8.0f / 255.0f, 8.0f / 255.0f}, add[3] = {0, 0, 0};
                HUD_BEGIN(g_hud, w, h); hud_tint(g_hud, w, h, mul, add); hud_end(g_hud);
            }
            hud_set_flash(g_hud, 1.0f, 0.0f, 0.0f, k);
        }
        if (g_hud && g_world && g_pl_ok && g_pilot && !g_hud_off && (HUD_VIEW || (getenv("MW2_HUD") && g_camode != 4))) {   /* F11: TOGGLE_HUD */
            /* The cockpit HUD, laid out as the original (PowerVR edition screenshots): layout values are
             * 320x200 units mapped x2 across and x2.4 down (480 / 200), as the engine scales them (0x10005db0). */
#define LX(x) ((float)(x) * 2.0f)
#define LY(y) ((float)(y) * 2.4f)
            static const float green[4] = {0.3f, 1.0f, 0.35f, 1.0f}, red[4] = {1.0f, 0.22f, 0.15f, 1.0f}, white[4] = {1.0f, 1.0f, 1.0f, 1.0f},
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
            if (g_vision == 1 && g_cockpit && g_tex_kind != 3) {   /* light amplification, 3D editions: (16,32,8)/255 + in x (0.5, 1.0, 0.6),
                                                                     * fitted to DOS (the DOS edition fades its palette, dos_time_of_day) */
                static const float mul[3] = {0.5f, 1.0f, 0.6f}, add[3] = {16.0f / 255.0f, 32.0f / 255.0f, 8.0f / 255.0f};
                hud_tint(g_hud, w, h, mul, add);
            }

            /* events -> messages + Betty (message table 0x1024f144) */
            /* 0x10015f50: "Heat level critical" once per latch (65-80, net gain), "Shutting down" (message 0xd) when the
             * sequence shuts the mech down */
            if (pu->heat_warning) { hud_message_v("Heat level critical", 64); g_sim.player_unit.heat_warning = 0; }
            if (pu->auto_shutdown) { hud_message_v("Shutting down", 14); g_sim.player_unit.auto_shutdown = 0; }
            /* 0x10016175: with message 3 the overheat alarm MECOVHW1 0xe9 (volume 0x50; latch 0x1000, cleared below 65
             * with no sequence pending - the same edge) */
            if (pu->shutdown_pending && !g_last_pending) { hud_message_v("Shutdown sequence initiated", 15); if (g_sfx) sfx_play(g_sfx, 0xe9, 0.8f, 0.0f); }   /* message 3 (DOS) */
            /* 0x1001e4fe: OVERRIDE while a sequence is pending (flag 4, not 8) and not shut down: MECBSOXX 0xcd (0x32) */
            if (pu->override && !g_last_override) { hud_message_v("Shutdown sequence overridden", 78); if (g_sfx) sfx_play(g_sfx, 0xcd, 0.5f, 0.0f); }
            /* 0x1001732e: a location's armour used up for the first time: MECRDWA1 0xec (0x50) */
            if (pu->breach_sound) { if (g_sfx) sfx_play(g_sfx, 0xec, 0.8f, 0.0f); g_sim.player_unit.breach_sound = 0; }
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
                        if (g_sfx) sfx_play(g_sfx, 0x100, 0.5f, 0.0f);
                        break;
                    }
                }
            }
            if (pu->ammo_blown) { hud_message_v("Internal ammo explosion detected", 48); g_sim.player_unit.ammo_blown = 0; }
            g_last_pending = pu->shutdown_pending; g_last_override = pu->override;
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
                    float cxu = 321.0f, cyu = 35.0f, ux = hud_px_x(g_hud, 1.0f), uy = hud_px_y(g_hud, 1.0f);
                    float hx;
                    int tx_ok = 0, nav_t = 0;
                    float tbx = 0, tbz = 0, tby = 0;
                    ppd = (float)g_hs_compass.w / 360.0f;
                    hx = cxu + ux * (float)(int)(lhead * ppd);                       /* the hotspot's screen x */
                    centre = (230.0f - hx) / ux - (float)g_hs_compass.left;          /* texture u at the window's left */
                    hud_draw_window(g_hud, &g_hs_compass, centre, 181.0f / ux, 320.5f, cyu + uy * (float)g_hs_compass.top, NULL);
                    if (g_hs_cmpmk[2].tex) hud_draw(g_hud, &g_hs_cmpmk[2], cxu, cyu, NULL);
                    if (fabsf(twist) >= 1e-7f) {
                        static const float b0[4] = {0, 210 / 255.0f, 0, 1}, b1[4] = {0, 1, 85 / 255.0f, 1}, b2[4] = {0, 125 / 255.0f, 0, 1};
                        float wv = ux * (float)(int)fabsf(twist), x0 = twist > 0 ? cxu : cxu - wv, y0 = cyu - 5.0f * uy;
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
                        const hud_sprite *ar = nav_t ? g_hs_edge_nav : g_hs_edge;   /* CRTLFT, CRTRGHT, CRTUP, CRTDOWN */
                        while (rel > 180.0f) rel -= 360.0f;
                        while (rel < -180.0f) rel += 360.0f;
                        elev = atan2f(tby - pu->y, dd > 1 ? dd : 1) * 180.0f / 3.14159265f - g_pitch;
                        if (fabsf(rel) < 1e-7f) {
                            if (g_hs_cmpmk[1].tex) hud_draw(g_hud, &g_hs_cmpmk[1], cxu, cyu, NULL);
                            if (g_hs_cmpmk[3].tex) hud_draw(g_hud, &g_hs_cmpmk[3], cxu, cyu, NULL);
                        } else {
                            /* DOSBox (YELL, E on the Falcon 43 degrees right): CMPMKR1 right of the centre, CRTRGHT beside the
                             * tape - the marker sits on the target's own side, although the tape's numbers rise leftward */
                            mx = cxu + ux * (float)(int)(rel * ppd);
                            if (getenv("MW2_DEBUG_CMP")) fprintf(stderr, "cmp rel %.1f mx %.1f tex %u\n", rel, mx, g_hs_cmpmk[0].tex);
                            if (mx >= 230.0f && mx <= 411.0f && g_hs_cmpmk[0].tex) hud_draw(g_hud, &g_hs_cmpmk[0], mx, cyu, NULL);
                        }
                        if (elev > -3.0f && ar[2].tex) hud_draw(g_hud, &ar[2], cxu, cyu - 11.0f * uy, NULL);
                        if (elev < 3.0f && ar[3].tex) hud_draw(g_hud, &ar[3], cxu, cyu + 25.0f * uy, NULL);
                        /* the side the marker lies on (screen right = positive rel) */
                        if (rel > -3.0f && ar[1].tex) { hud_draw(g_hud, &ar[1], 411.0f + 5.0f * ux, cyu + 5.0f * uy, NULL); if (rel > 90.0f) hud_draw(g_hud, &ar[1], 411.0f + 6.0f * ux, cyu + 5.0f * uy, NULL); }
                        if (rel < 3.0f && ar[0].tex) { hud_draw(g_hud, &ar[0], 230.0f - 6.0f * ux, cyu + 5.0f * uy, NULL); if (rel < -90.0f) hud_draw(g_hud, &ar[0], 230.0f - 7.0f * ux, cyu + 5.0f * uy, NULL); }
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
                    }
                    {
                        const hud_sprite *R = dos ? g_hs_rcl_dos : g_hs_rcl;
                        /* RCLINOP (the yellow ring) is "inoperative": the weapon destroyed or out of ammunition - not
                         * recycling (DOS / Win95 footage: the normal reticle between shots) */
                        int inop = cw->ammo == 0 || (cw->location >= 0 && cw->location < 8 && g_sim.player_unit.loc_gone[cw->location]);
                        if (inop) rs = &R[0];
                        else if (msim_weapon_guided(wid)) rs = (g_sim.player_lock & 0x80) ? &R[5] : (g_sim.player_lock & 0x8000) ? &R[3] : &R[2];
                        else if (kind == 3) rs = on ? &R[1] : &R[2];
                        else if (on) rs = &R[4];
                        else if (dos) rs = &g_hs_reticle_dos;
                    }
                } else if (dos) rs = &g_hs_reticle_dos;
                if (dos) hud_draw_f(g_hud, rs, 320.0f, 240.0f, -1.0f);
                else hud_draw(g_hud, rs, 320.0f, 240.0f, NULL);
            }
            /* target bracket (engine dispatcher 0x10022830): a mech (0x10022cc0) gets four corner pieces TGTGP1-4 (+E enemy,
             * +N neutral) at (x -+ h, y -+ h), h = R x F / depth - R its contact radius (MGEO int[6]), F the view's focal
             * length, the point its centre (MGEO int[0] above the feet), no minimum or cap; a nav point TGTNP. Off
             * screen (behind: |depth|): the ray from the screen centre through the point clipped to the view, TGTOFFF /
             * E / N (an X) there - TGTOFFF for a nav point. */
            {
                float wp[3];
                int have = 0, isnav = 0, side = 1;
                if (tgt >= 0) {
                    wp[0] = (float)g_actors[tgt].mech.origin[0];
                    wp[1] = (float)g_actors[tgt].mech.origin[1] + (g_sim.centre_h ? g_sim.centre_h[tgt] : 550.0f);
                    wp[2] = (float)g_actors[tgt].mech.origin[2];
                    side = g_actors[tgt].friendly ? 0 : g_actors[tgt].alliance == 2 ? 2 : 1;
                    have = 1;
                } else {
                    int nv = current_nav(NULL);
                    if (nv >= 0) {
                        wp[0] = g_sim.navs[nv].x; wp[2] = g_sim.navs[nv].z;
                        wp[1] = msim_ground(&g_sim, wp[0], wp[2], 1e7f);
                        have = isnav = 1; side = 0;
                    }
                }
                if (have) {
                    float tx = wp[0] - v.eye[0], ty = wp[1] - v.eye[1], tz = wp[2] - v.eye[2];
                    float zf = tx * fx + ty * fy + tz * fz, xr = tx * rx + tz * rz, yu = tx * ux + ty * uy + tz * uz;
                    float sx = (float)h * 4.0f / 3.0f, kx = 320.0f * (float)w / sx;   /* 640-unit x per unit of nx */
                    float az = fabsf(zf) > 1.0f ? fabsf(zf) : 1.0f;
                    float nx = xr / az / (th * aspect), ny = yu / az / th;
                    float vx = 320.0f + nx * kx, vy = 240.0f - ny * 240.0f;
                    if (zf > 1.0f && vx > 320.0f - kx && vx < 320.0f + kx && vy > 0 && vy < 480) {   /* the whole view, not just its 4:3 middle */
                        if (isnav) hud_draw(g_hud, &g_hs_navmk, vx, vy, NULL);
                        else {
                            float R = g_sim.radius ? g_sim.radius[tgt] : 545.0f, hb = R / zf / (th * aspect) * kx;
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
                        hud_draw(g_hud, &g_hs_off[isnav ? 0 : side], 320.0f + tt * dx, 240.0f + tt * dy, NULL);
                    }
                }
            }
            }
            /* radar, top left (CPIT rect 0): yellow circle, view cone, own marker above centre, blips; 1 km (assumed) */
            if (g_layout_n > 0 && (g_radar_mode != 0 || g_satmap) && (!hud_dead || g_satmap || rad_on)) {
                /* the window from the mode descriptor 0x1024a760 (screen fractions; CPIT rect 0 is overwritten by it,
                 * 0x10003250): mode 1 (0.02, 0.02)-(0.26, 0.34), mode 2 (0.13, 0)-(0.87, 1.0); circle radius w / 2 - 1 */
                float cx = 89.6f, cy = 86.4f, rad = 75.8f;
                if (g_radar_big) { cx = 320.0f; cy = 240.0f; rad = 235.8f; }
                if (g_satmap) { cx = 320.0f; cy = 240.0f; rad = 420.0f; }       /* mode 4: full screen, north up */
                else if (hud_dead) rad *= rad_wf;   /* powering up / down: the window (and its circle) about the centre */
                /* DOS footage: own marker at the centre; the view cone is +-45 degrees about the torso's
                 * facing (it turns with the twist; the radar is aligned with the legs) */
                float px = cx, py = cy, cone = atanf(1.0f / (g_zoom > 1 ? g_zoom : 1.0f)), tw = (g_pl.twist + (g_satmap ? g_pl.heading : 0.0f)) * 3.14159265f / 180.0f;   /* +-45 deg, narrowed by the zoom (DOS); north-up map: heading + twist */
                if (g_satmap) {   /* "Range: " (half the width) at (0.01, 0.01), "Bearing: %3.1lf" at (0.01, 0.05) */
                    char lb[32];
                    float hr = 500.0f / (float)(1 << g_sat_i), bh = fmodf(g_pl.heading + g_pl.twist + 720.0f, 360.0f);
                    if (hr >= 1000.0f) snprintf(lb, sizeof lb, "Range: %3.1lfkm", (double)hr / 1000.0); else snprintf(lb, sizeof lb, "Range: %3.1lfm", (double)hr);
                    hud_text(g_hud, 6.4f, 4.8f, lb, green);
                    snprintf(lb, sizeof lb, "Bearing: %3.1lf", (double)bh);
                    hud_text(g_hud, 6.4f, 24.0f, lb, green);
                }
                if (!g_satmap) hud_circle(g_hud, cx, cy, rad, 1.5f, yellow);
                if (!g_satmap && !hud_dead) hud_text(g_hud, g_radar_big ? 120.8f : 13.0f, g_radar_big ? 62.5f : 15.0f, RADAR_LABEL[g_radar_i], green);   /* DOS: range label (not while powering up / down: footage) */
                {   /* the view cone's sides end at the circle (DOS / PowerVR footage) */
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
                {   /* blips (0x10003d40): mechs RGPF / E / N; the selected target RTGTGP (a plus), RTGTOF at the edge when
                     * beyond the radar; nav points RNP (RRNP once reached), the targeted one RTGTNP / RRTGTNP */
                    float ly = g_satmap ? 0.0f : g_pl.heading * 3.14159265f / 180.0f;
                    float rcm = g_satmap ? 50000.0f / (float)(1 << g_sat_i) : RADAR_CM[g_radar_i], rr2 = g_satmap ? 320.0f : rad;
                    int nvt = tgt < 0 ? current_nav(NULL) : -1;
                    for (q = -1 - g_sim.nav_count; q < g_actor_count; q++) {
                        float wx, wz, ddx, ddz, ax, ay, bx, by, dd2;
                        int sd = 1, istgt, nvq = -1;
                        const hud_sprite *sp;
                        if (q >= 0) {
                            if (!g_sim.armed[q] || g_sim.units[q].destroyed) continue;
                            wx = (float)g_actors[q].mech.origin[0]; wz = (float)g_actors[q].mech.origin[2];
                            sd = g_actors[q].friendly ? 0 : g_actors[q].alliance == 2 ? 2 : 1;
                            istgt = q == tgt;
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
                        if (dd2 > rad * rad) {
                            if (!istgt || nvq >= 0) continue;
                            {   float l = sqrtf(dd2); bx = cx + (bx - cx) * rad / l; by = cy + (by - cy) * rad / l; }
                            sp = &g_hs_rtof[sd];
                        } else if (nvq >= 0) {
                            int rch = nvq < 256 && g_nav_seen[nvq];
                            sp = istgt ? &g_hs_rtnp[rch] : &g_hs_rnp[rch];
                        } else sp = istgt ? &g_hs_rtgp[sd] : &g_hs_rgp[sd];
                        if (sp->tex) hud_draw(g_hud, sp, bx, by, NULL);
                        else if (nvq < 0) hud_rect(g_hud, bx - 1.5f, by - 1.5f, 3.0f, 3.0f, blipc);
                    }
                }
            }
            /* altitude tape (engine 0x100222e0; window 23 = (1,74)-(35,111) in 320x200 -> 640 units x 2-70, y 178-267;
             * reference point (0.7, 0.5) = local (48, 45) -> (50, 223)). It shows the ABSOLUTE elevation of the feet
             * (object y - controller +0xcc): ALTTAPE by its hotspot at local (6, 45 + (h - 201 m) x 4 px), clipped to the
             * window; ALTTOP at the caret line above 201 m; ALTMKR1 the caret at (48, 45); ALTMKR2 the ground under the
             * mech at (54, 45 + (h - ground) x 4); the target's height: CRTLFT at (54, y), or CRTUP / CRTDOWN at (60, 0) /
             * (60, 89) when it lies above / below the window */
            {
            if (!hud_dead && g_hs_alt.tex) {
                float ux = hud_px_x(g_hud, 1.0f), uy = hud_px_y(g_hud, 1.0f), alt = pu->y / 100.0f, gnd = pu->ground / 100.0f;
                float hy = 178.0f + 45.0f * uy + uy * (float)(int)((alt - 201.0f) * 4.0f);   /* the tape's hotspot */
                float v0 = (178.0f - hy) / uy - (float)g_hs_alt.top;
                hud_draw_window_v(g_hud, &g_hs_alt, v0, 89.0f / uy, 2.0f + 6.0f * ux + ux * (float)g_hs_alt.left, 178.0f);
                if (alt > 201.0f) hud_draw(g_hud, &g_hs_alttop, 2.0f + 6.0f * ux, 178.0f + 45.0f * uy, NULL);
                if (g_hs_altmk[0].tex) hud_draw(g_hud, &g_hs_altmk[0], 2.0f + 48.0f * ux, 178.0f + 45.0f * uy, NULL);
                {
                    float gy = 45.0f + (float)(int)((alt - gnd) * 4.0f);
                    if (g_hs_altmk[1].tex && gy >= 0 && gy <= 89.0f) hud_draw(g_hud, &g_hs_altmk[1], 2.0f + 54.0f * ux, 178.0f + gy * uy, NULL);
                }
                {
                    int nv = tgt < 0 ? current_nav(NULL) : -1;
                    if (tgt >= 0 || nv >= 0) {
                        float ta = tgt >= 0 ? g_sim.units[tgt].y / 100.0f : (float)g_sim.navs[nv].y / 100.0f;
                        float ty = 45.0f + (float)(int)((alt - ta) * 4.0f);
                        const hud_sprite *ar = tgt >= 0 ? g_hs_edge : g_hs_edge_nav;
                        if (ty < 0) { if (ar[2].tex) hud_draw(g_hud, &ar[2], 2.0f + 60.0f * ux, 178.0f, NULL); }
                        else if (ty > 89.0f) { if (ar[3].tex) hud_draw(g_hud, &ar[3], 2.0f + 60.0f * ux, 178.0f + 89.0f * uy, NULL); }
                        else if (ar[0].tex) hud_draw(g_hud, &ar[0], 2.0f + 54.0f * ux, 178.0f + ty * uy, NULL);
                    }
                }
            }
            /* weapon list, top right (CPIT rects 3-12): green ready, white ready but target out of range,
             * red recycling, grey empty / destroyed; box around the selected weapon */
            {
                int wi;
                for (wi = 0; wi < pu->weapon_count && wi < 10 && g_layout_n > 12 && (!hud_dead || g_pow == 1); wi++) {
                    const combat_weapon *cw = &pu->weapons[wi];
                    const int win = weapon_window(pu, wi), *rr = g_layout[win];
                    char line[24];
                    const float *col;
                    float tw;
                    if (hud_dead) {   /* start-up (LAB_1001e9a0): the name alone, green, once the mission reaches its window's
                                       * time (0x1024cce8); a destroyed slot not at all */
                        static const int APPEAR[10] = {543, 573, 603, 633, 663, 814, 784, 754, 724, 693};
                        static const float sgreen[4] = {0, 210 / 255.0f, 0, 1};
                        if (cw->state >= 0 && (float)g_sim.now * 0.182f >= (float)APPEAR[win - 3])
                            hud_text(g_hud, LX(rr[0]), LY(rr[1]), sim_weapon_short[cw->weapon], sgreen);
                        continue;
                    }
                    /* engine 0x1001e820, by the slot's state: destroyed / out of ammunition (-1) dark red (index 8);
                     * recycling red (0xb); a burst being fired green (0xe); ready: by its fire group - 1 green (0xe),
                     * 2 white (0xfe), 3 yellow (3) */
                    static const float wdark[4] = {60 / 255.0f, 0, 0, 1}, wred[4] = {1, 24 / 255.0f, 24 / 255.0f, 1},
                                       wgreen[4] = {0, 210 / 255.0f, 0, 1}, wwhite[4] = {238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 1},
                                       wyellow[4] = {1, 206 / 255.0f, 0, 1};
                    (void)grey; (void)white;
                    if (cw->ammo == 0 || pu->loc_gone[cw->location]) col = wdark;
                    else if (cw->state == CW_FIRING) col = wgreen;
                    else if (cw->state != CW_READY) col = wred;
                    else if (g_groups[1] & (1u << wi)) col = wwhite;
                    else if (g_groups[2] & (1u << wi)) col = wyellow;
                    else col = wgreen;
                    if (cw->ammo >= 0) snprintf(line, sizeof line, "%s %d", sim_weapon_short[cw->weapon], cw->ammo);
                    else snprintf(line, sizeof line, "%s", sim_weapon_short[cw->weapon]);
                    tw = hud_text(g_hud, LX(rr[0]), LY(rr[1]), line, col);
                    if (wi == g_sel_weapon) {
                        float bx0 = LX(rr[0]) - 4.0f, by0 = LY(rr[1]) - 3.0f, bx1 = LX(rr[0]) + tw + 4.0f, by1 = LY(rr[1]) + hud_px_y(g_hud, 14.0f * g_hud_s) + 3.0f;
                        hud_line(g_hud, bx0, by0, bx1, by0, 1.5f, col); hud_line(g_hud, bx1, by0, bx1, by1, 1.5f, col);
                        hud_line(g_hud, bx1, by1, bx0, by1, 1.5f, col); hud_line(g_hud, bx0, by1, bx0, by0, 1.5f, col);
                    }
                }
            }
            }
            /* damage outline, bottom right (CPIT rect 2), per location (engine 0x1001b620); F5 hides it */
            if (g_vport) {   /* the viewport box: blue frame, label sprite at the top centre (DOS) */
                static const float vblue[4] = {0.1f, 0.1f, 0.9f, 1};
                float x0 = 516.9f, y0 = 349.4f, x1 = 619.4f, y1 = 428.8f, vwf = 1, vhf = 1;
                if (pow_window(1, &vwf, &vhf)) {   /* powering up / down: window 2 about its centre (0x10011aa0 / 0x10011b70) */
                    float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f, hw = (x1 - x0) * 0.5f * vwf, hh = (y1 - y0) * 0.5f * vhf;
                    int full = vwf >= 1.0f && vhf >= 1.0f;
                    x0 = mx - hw; x1 = mx + hw; y0 = my - hh; y1 = my + hh;
                    if (g_vport == 3 && !g_wcam_live) hud_rect(g_hud, x0, y0, x1 - x0, y1 - y0, black);   /* weapon view, nothing in flight: black (DOS) */
                    if (full && g_snow_show[0]) draw_snow(x0, y0);   /* display damage: SNOWCLR instead of the view (0x10011a40) */
                    hud_line(g_hud, x0, y0, x1, y0, 1.0f, vblue); hud_line(g_hud, x1, y0, x1, y1, 1.0f, vblue);
                    hud_line(g_hud, x1, y1, x0, y1, 1.0f, vblue); hud_line(g_hud, x0, y1, x0, y0, 1.0f, vblue);
                    if (!(full && g_snow_show[0]) && g_hs_vp[g_vport - 1].tex) {
                        /* the label (0x10011a70 -> 0x10023120: x = the window's left edge + half the instrument's full width
                         * (+0x44 / 2), y = its top + 2): while the window opens / closes it rides the moving left and top
                         * edges, clipped to the box */
                        if (!full) hud_clip(g_hud, x0, y0, x1 - x0, y1 - y0);
                        hud_draw(g_hud, &g_hs_vp[g_vport - 1], x0 + (619.4f - 516.9f) * 0.5f, y0 + 6.0f, NULL);
                        if (!full) hud_clip_off(g_hud);
                    }
                }
            } else if (g_htal && !g_dmg_off && !hud_dead) {
                /* H T A L (DOS, F6; engine 0x1001b890): armour bars hanging from a line at y 375.6, 1.125 units per
                 * armour point (1.8 px at 1024x768), torsos as half-width front / rear pairs, blue letters (index 6).
                 * The armour left is green (0xf bands); the armour lost, below it on the same bar up to the maximum,
                 * yellow (3) while more than a quarter is left, else red (0xb); a destroyed location all grey (0xf3) */
                static const struct { int loc, rear; float x, w; } B[11] = {
                    {0, 0, 531.3f, 8.1f}, {1, 0, 543.1f, 3.75f}, {1, 1, 546.9f, 3.75f}, {2, 0, 553.1f, 3.75f}, {2, 1, 556.9f, 3.75f},
                    {3, 0, 563.1f, 3.75f}, {3, 1, 566.9f, 3.75f}, {4, 0, 575.0f, 8.1f}, {5, 0, 585.0f, 8.1f}, {6, 0, 597.5f, 8.1f}, {7, 0, 607.5f, 8.1f}};
                static const float hb[4] = {0, 210 / 255.0f, 0, 1}, hl[4] = {0, 1.0f, 85 / 255.0f, 1}, hd[4] = {0, 125 / 255.0f, 0, 1};
                static const float yb[4] = {190 / 255.0f, 145 / 255.0f, 0, 1}, yl[4] = {1, 206 / 255.0f, 0, 1}, ydk[4] = {121 / 255.0f, 97 / 255.0f, 0, 1};
                static const float rb[4] = {210 / 255.0f, 0, 0, 1}, rl[4] = {1, 24 / 255.0f, 24 / 255.0f, 1}, rd[4] = {125 / 255.0f, 0, 0, 1};
                static const float gb2[4] = {32 / 255.0f, 32 / 255.0f, 32 / 255.0f, 1}, gl2[4] = {52 / 255.0f, 52 / 255.0f, 52 / 255.0f, 1}, gd2[4] = {16 / 255.0f, 16 / 255.0f, 16 / 255.0f, 1};
                static const float hblue[4] = {0, 0, 190 / 255.0f, 1};
                int b2;
                hud_text(g_hud, 528.8f, 357.5f, "H", hblue); hud_text(g_hud, 549.4f, 357.5f, "T", hblue);
                hud_text(g_hud, 577.5f, 357.5f, "A", hblue); hud_text(g_hud, 601.3f, 357.5f, "L", hblue);
                for (b2 = 0; b2 < 11; b2++) {
                    int loc = B[b2].loc, cur = (int)(B[b2].rear ? pu->rear[loc] : pu->armor[loc]), mx = B[b2].rear ? pu->rear_max[loc] : pu->armor_max[loc];
                    float len = (float)(cur > 0 ? cur : 0) * 1.125f, full = (float)(mx > 0 ? mx : 0) * 1.125f, w3 = B[b2].w / 3.0f;
                    const float *mb = cur * 4 > mx ? yb : rb, *ml = cur * 4 > mx ? yl : rl, *md = cur * 4 > mx ? ydk : rd;
                    if (pu->loc_gone[loc]) { len = 0; mb = gb2; ml = gl2; md = gd2; }
                    if (len > 0) {
                        hud_rect(g_hud, B[b2].x, 375.6f, w3, len, hb);
                        hud_rect(g_hud, B[b2].x + w3, 375.6f, w3, len, hl);
                        hud_rect(g_hud, B[b2].x + 2 * w3, 375.6f, w3, len, hd);
                    }
                    if (full > len) {
                        hud_rect(g_hud, B[b2].x, 375.6f + len, w3, full - len, mb);
                        hud_rect(g_hud, B[b2].x + w3, 375.6f + len, w3, full - len, ml);
                        hud_rect(g_hud, B[b2].x + 2 * w3, 375.6f + len, w3, full - len, md);
                    }
                }
            } else if (g_layout_n > 2 && !g_dmg_off && !hud_dead) {
                static const int REG_LOC[15] = {1, 3, 3, 2, 2, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8};
                float dx = LX(g_layout[2][0]), dy = LY(g_layout[2][1]);
                for (q = 0; q < 15; q++) {
                    int loc = REG_LOC[q] - 1, lf = 0, lr = 0, lv, pick;
                    const int *rg = g_dmg_regions[q];
                    if (rg[2] <= 0 || rg[3] <= 0) continue;
                    if (pu->armor_max[loc] > 0) lf = 15 - 15 * (int)pu->armor[loc] / pu->armor_max[loc];
                    if (pu->rear_max[loc] > 0) lr = 15 - 15 * (int)pu->rear[loc] / pu->rear_max[loc];
                    lv = lf > lr ? lf : lr;
                    pick = pu->loc_gone[loc] ? 3 : lv <= 0 ? 0 : lv < 12 ? 1 : 2;
                    hud_draw_region(g_hud, &g_hs_dmg_lvl[pick], dx, dy, (float)rg[0] * 2.0f * g_hud_s, (float)rg[1] * 2.0f * g_hud_s, (float)rg[2] * 2.0f * g_hud_s, (float)rg[3] * 2.0f * g_hud_s);
                }
            }
            if (!hud_dead) {   /* the instruments (dark while shut down) */
            /* tall bar, right edge of CPIT rect 1 (engine 0x1001c5b0, geometry 0x1001c250): the THROTTLE SETTING, its
             * shown value easing halfway to the setting on every draw (cur += (target - cur) >> 1; the DOS ramp 57 / 67 /
             * 74 / 76 px fits halving every 0.085 s). Zero line at bottom - H/2 - 2 (34 % up); forward fills up in green
             * bands (0xf), full throttle = the whole span; reverse fills down in blue bands (7), full reverse = the lower
             * span; the fill is h + 1 rows, so zero throttle leaves a single line; frame red (0xa) */
            if (g_layout_n > 1) {
                /* DOS: frame columns 1001-1017, rows 542-752 at 1024x768 */
                static float shown;
                static int32_t shown_t;
                float bar_dt = shown_t ? (float)(SDL_GetTicks() - (Uint32)shown_t) / 1000.0f : 0;
                float bx = 625.6f, by = 338.75f, bh = 131.25f, u = 9.375f / 15.0f;
                float zy = by + bh * (1.0f - 0.34f), th2 = g_throttle, hgt;
                static const float fg0[4] = {0, 210 / 255.0f, 0, 1}, fg1[4] = {0, 1.0f, 85 / 255.0f, 1}, fg2[4] = {0, 125 / 255.0f, 0, 1};
                static const float fb0[4] = {0, 0, 190 / 255.0f, 1}, fb1[4] = {60 / 255.0f, 60 / 255.0f, 1, 1}, fb2[4] = {0, 0, 125 / 255.0f, 1};
                const float *c0, *c1, *c2;
                if (th2 > 1) th2 = 1;
                if (th2 < -1) th2 = -1;
                shown_t = (int32_t)SDL_GetTicks();
                if (bar_dt > 1) bar_dt = 1;
                shown += (th2 - shown) * (1.0f - powf(0.5f, 12.0f * bar_dt));
                if (shown >= 0) { hgt = (zy - by) * shown; c0 = fg0; c1 = fg1; c2 = fg2; }
                else { hgt = (by + bh - zy) * -shown; c0 = fb0; c1 = fb1; c2 = fb2; }
                {   /* four bands across the bar: c-1, c, c-1, c-2 */
                    float y0 = shown >= 0 ? zy - hgt : zy, hh = hgt + u, w4 = 8.0f / 4.0f;
                    hud_rect(g_hud, bx + 1.0f, y0, w4, hh, c0); hud_rect(g_hud, bx + 1.0f + w4, y0, w4, hh, c1);
                    hud_rect(g_hud, bx + 1.0f + 2 * w4, y0, w4, hh, c0); hud_rect(g_hud, bx + 1.0f + 3 * w4, y0, w4, hh, c2);
                }
                {
                    static const float fr[4] = {210 / 255.0f, 0, 0, 1};
                    hud_line(g_hud, bx, by, bx + 10.0f, by, 1.2f, fr); hud_line(g_hud, bx + 10.0f, by, bx + 10.0f, by + bh, 1.2f, fr);
                    hud_line(g_hud, bx + 10.0f, by + bh, bx, by + bh, 1.2f, fr); hud_line(g_hud, bx, by + bh, bx, by, 1.2f, fr);
                }
                {   /* DOS footage: "NN kph" under it, right-aligned left of the bar */
                    char kph[16];
                    /* engine 0x10018470: |v| in cm/tick x 6.516 x 1.5, truncated; minus when reversing */
                    {   /* DOS 0x32085 (3Dfx 0x10018470 the same 1.5): |v| estimated as (4 max + mid + min) >> 2 of the velocity's
                         * |x|, |y|, |z| (16.16 cm/tick), / 10002 (= 1 km/h) truncated, x 1.5 truncated (0x5c3c2: frndint with
                         * the control word's rounding set to chop) - the port had rounded the last step (91.5 -> 92) */
                        float hh = g_pl.heading * 3.14159265f / 180.0f, kv = g_pl_speed / 182.0f * 65536.0f;
                        int32_t ax = (int32_t)fabsf(sinf(hh) * kv), az = (int32_t)fabsf(cosf(hh) * kv), ay = (int32_t)(fabsf(g_sim.player_unit.vy) * 65536.0f), t3;
                        if (ax < az) { t3 = ax; ax = az; az = t3; }
                        if (ax < ay) { t3 = ax; ax = ay; ay = t3; }
                        snprintf(kph, sizeof kph, "%d kph", (int)((double)(((4 * ax + az + ay) >> 2) / 10002) * 1.5) * (g_pl_speed < 0 ? -1 : 1));
                    }
                    if (getenv("MW2_KPH_TRACE")) fprintf(stderr, "kph %.2fs %s (%.1f cm/s, heading %.2f)\n", g_sim.now / 1000.0, kph, g_pl_speed, g_pl.heading);   /* tests */
                    hud_text(g_hud, 551.0f, 453.0f, kph, green);   /* DOS / footage: left edge x 551 */
                }
            }
            }
            /* target viewer, bottom left (CPIT rect 13); F4 hides it. Powering up / down: the window about its centre
             * (0x10021730 / 0x100217f0, see pow_update) - the box alone while it moves */
            float tv_wf = 1, tv_hf = 1;
            if (g_layout_n > 13 && !g_tgtdisp_off && pow_window(1, &tv_wf, &tv_hf)) {
                const int *rr = g_layout[13];
                float bx = LX(rr[0]), by = LY(rr[1]), bw = LX(rr[2]), bh = LY(rr[3]);
                int shaded = (g_tgtdisp == 2 || (g_tgtdisp == 1 && g_tex_kind != 3)) && tgt >= 0 && g_cockpit && g_pow == 2;   /* drawn in 3D before the HUD */
                int full = tv_wf >= 1.0f && tv_hf >= 1.0f;
                if (!full) { bx += bw * (1.0f - tv_wf) * 0.5f; by += bh * (1.0f - tv_hf) * 0.5f; bw *= tv_wf; bh *= tv_hf; }
                const int snow = full && g_snow_show[1];   /* display damage: SNOWCLR instead of the contents (0x10021710) */
                if (!shaded) hud_rect(g_hud, bx, by, bw, bh, black);
                if (snow) draw_snow(bx, by);
                if (g_pow != 2 && tgt < 0 && g_nav_sel >= 0 && current_nav(NULL) >= 0 && g_hs_vtgt_nav.tex && !snow) {
                    /* starting up / shut down (0x10021730 / 0x100217f0 -> 0x10021300 with the window set to the moving
                     * rectangle): a selected nav point's emblem (0x109 / 0x106 at half the rectangle's width and height,
                     * 0x10023120) - centred, clipped to the box as it opens or closes; a mech or building target: black */
                    hud_clip(g_hud, bx, by, bw, bh);
                    hud_draw(g_hud, &g_hs_vtgt_nav, bx + bw * 0.5f, by + bh * 0.5f, NULL);
                    hud_clip_off(g_hud);
                }
                hud_line(g_hud, bx, by, bx + bw, by, 1.2f, red); hud_line(g_hud, bx + bw, by, bx + bw, by + bh, 1.2f, red);
                hud_line(g_hud, bx + bw, by + bh, bx, by + bh, 1.2f, red); hud_line(g_hud, bx, by + bh, bx, by, 1.2f, red);
                if (full && g_pow == 2) {   /* (not while starting up / shut down: the box alone - DOSBox YELLSCN1 / TNJ1SCN1 start-up)
                     * DOS footage: a mech target shows its damage outline, name below in red, range in green;
                     * otherwise the current nav point: VTGT_NP6 emblem, name and range below in yellow */
                    char info[48];
                    float ty = by + bh + 6.0f, lh2 = hud_px_y(g_hud, g_hud_sfx[0] ? 18.0f : 9.0f);   /* line spacing: 18 px; the 320x200 set 9 (ASSUMED) */
                    if (tgt >= 0) {
                        dmg_set *ds = NULL;
                        if (getenv("MW2_TARGET_OUTLINE") && g_sim.armed[tgt]) ds = target_outline(g_ma ? g_ma : NULL, g_actors[tgt].skel);   /* the damage outline instead (tests) */
                        if (!shaded && !snow) {   /* the target's wireframe, from where the player stands */
                            const world_actor *ta2 = &g_actors[tgt];
                            float ddx = (float)g_pl.origin[0] - (float)ta2->mech.origin[0], ddz = (float)g_pl.origin[2] - (float)ta2->mech.origin[2];
                            float brg = atan2f(ddx, ddz) * 180.0f / 3.14159265f;
                            static const float wblue[4] = {0.0f, 48.0f / 255.0f, 215.0f / 255.0f, 1.0f};
                            /* the posed parts are already in the world (heading applied): only the viewing direction turns */
                            {   /* seen along the line of sight from the eye to the target (a mech off to one side is seen
                                 * turned by that angle, as on screen), not along the camera's axis */
                                float lx = (float)ta2->mech.origin[0] - v.eye[0], lz = (float)ta2->mech.origin[2] - v.eye[2], ll = sqrtf(lx * lx + lz * lz);
                                float sr0 = g_wire_r[0], sr1 = g_wire_r[1], sf0 = g_wire_f[0], sf1 = g_wire_f[1];
                                if (ll > 1.0f) { g_wire_f[0] = lx / ll; g_wire_f[1] = lz / ll; g_wire_r[0] = g_wire_f[1]; g_wire_r[1] = -g_wire_f[0]; }
                                (void)brg;
                                g_wire_unit = g_sim.armed[tgt] ? &g_sim.units[tgt] : NULL;
                                draw_target_wire(&ta2->mech, 0, ta2->mech.part_count, head, bx, by, bw, bh, wblue);
                                g_wire_unit = NULL;
                                g_wire_r[0] = sr0; g_wire_r[1] = sr1; g_wire_f[0] = sf0; g_wire_f[1] = sf1;
                            }
                        }
                        if (ds && !snow) {
                            float ow = hud_px_x(g_hud, (float)ds->lvl[0].w), oh = hud_px_y(g_hud, (float)ds->lvl[0].h);
                            draw_outline((const hud_sprite *)ds->lvl, (const int (*)[4])ds->regions, &g_sim.units[tgt], bx + (bw - ow) * 0.5f, by + (bh - oh) * 0.5f);
                        }
                        {   /* the name in the side's colour: enemy red, neutral blue (DOS footage: "Instructor" in blue), friendly green */
                            static const float nblue[4] = {0.0f, 48.0f / 255.0f, 215.0f / 255.0f, 1.0f};
                            hud_text(g_hud, bx, ty, g_actors[tgt].name[0] ? g_actors[tgt].name : g_actors[tgt].skel,
                                     g_actors[tgt].alliance == 2 ? nblue : g_actors[tgt].friendly ? green : red);
                        }
                        range_text(info, sizeof info, tdist);
                        hud_text(g_hud, bx, ty + lh2, info, green);
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
                            {   /* centred on itself: the part's own placement dropped */
                                mech3d_part tmp = *bp;
                                tmp.pos[0] = tmp.pos[1] = tmp.pos[2] = 0;
                                one.parts = &tmp;
                                (void)brg;
                                tmp.pos[0] = bp->pos[0]; tmp.pos[2] = bp->pos[2];   /* world placed, like the view; framing re-centres it */
                                draw_target_wire(&one, 0, 1, head, bx, by, bw, bh, wblue);
                            }
                        }
                        hud_text(g_hud, bx, ty, gb->name, red);
                        if (gb->inspected && gb->sub[0]) { hud_text(g_hud, bx, ty + lh2, gb->sub, green); ty += lh2; }
                        range_text(info, sizeof info, sqrtf(cx * cx + cz * cz));
                        hud_text(g_hud, bx, ty + lh2, info, green);
                    } else {
                        const char *label = NULL;
                        /* only a nav point the player selected (NEXT / PREV_NAVPOINT): the viewer draws the player's target
                         * record +0xdc, black when it is none (0x10021300) - DOSBox YELLSCN1 / TNJ1SCN1: the box stays black
                         * from the start (60 s) with no nav selected; the port had shown the current objective's nav */
                        int nv = g_nav_sel >= 0 ? current_nav(&label) : -1;
                        if (nv >= 0) {
                            if (g_hs_vtgt_nav.tex && !snow) hud_draw(g_hud, &g_hs_vtgt_nav, bx + bw * 0.5f, by + bh * 0.5f, NULL);
                            hud_text(g_hud, bx, ty, label, yellow);
                            snprintf(info, sizeof info, " %.0fm", (double)(sqrtf((g_sim.navs[nv].x - (float)g_pl.origin[0]) * (g_sim.navs[nv].x - (float)g_pl.origin[0]) +
                                                                                    (g_sim.navs[nv].z - (float)g_pl.origin[2]) * (g_sim.navs[nv].z - (float)g_pl.origin[2])) / 100.0f));
                            hud_text(g_hud, bx, ty + lh2, info, yellow);
                        }
                    }
                }
            }
            if (!hud_dead) {   /* the instruments (dark while not up) */
            /* bottom centre (DOS footage): "Heat" - yellow fills from both ends toward the middle;
             * "dH/dT" - the heating rate (engine 0x1001c4a0, below) */
            {
                static float prev_heat = -1, rate = 0;
                static int32_t prev_t;
                float dt = prev_t ? (float)(g_sim.now - prev_t) / 1000.0f : 0;
                float ht = pu->heat / 100.0f, rt, by = 428.1f;   /* DOS rows 685-699 at 1024x768 */
                if (prev_heat >= 0 && dt > 0) rate += ((pu->heat - prev_heat) / dt - rate) * (dt > 0.25f ? 1.0f : dt * 4.0f);
                prev_heat = pu->heat;
                prev_t = g_sim.now;
                if (ht > 1) ht = 1;
                if (ht < 0) ht = 0;
                rt = rate / 10.0f;
                if (rt > 1) rt = 1;
                if (rt < 0) rt = 0;
                {   /* DOS colours: blue 0,0,199 / 0,48,215 / 0,0,125; green 0,211,0 / 0,255,85 / 0,125,0; yellow ASSUMED */
                    static const float bb[4] = {0, 0, 0.78f, 1}, bl[4] = {0, 0.19f, 0.84f, 1}, bd[4] = {0, 0, 0.49f, 1};
                    /* COCKPIT palette bands (c-1, c, c-1, c-2): yellow 3 = 2 / 3 / 1, red 0xb = 0xa / 0xb / 9 */
                    static const float yb[4] = {190 / 255.0f, 145 / 255.0f, 0, 1}, yl[4] = {1, 206 / 255.0f, 0, 1}, yd[4] = {121 / 255.0f, 97 / 255.0f, 0, 1};
                    /* engine 0x1001c350: p = heat x width / 100. p <= 0: all blue (colour 7); p >= width: all red (0xb);
                     * under half: yellow (3) p wide at each end, blue between; from half on: yellow (width - p) at each
                     * end, red between - the yellow ends meet at 50 %, then the red grows out of the middle (DOSBox:
                     * matches the sustained-fire captures) */
                    static const float rb[4] = {210 / 255.0f, 0, 0, 1}, rl[4] = {1, 24 / 255.0f, 24 / 255.0f, 1}, rd[4] = {125 / 255.0f, 0, 0, 1};
                    const float W = 110.0f;
                    float p = ht * W;
                    if (p <= 0) hud_bar(219.5f, by, W, bb, bl, bd);
                    else if (p >= W) hud_bar(219.5f, by, W, rb, rl, rd);
                    else {
                        float e = p < W * 0.5f ? p : W - p;
                        int red = p >= W * 0.5f;
                        hud_bar(219.5f, by, W, red ? rb : bb, red ? rl : bl, red ? rd : bd);
                        hud_bar(219.5f, by, e, yb, yl, yd);
                        hud_bar(219.5f + W - e, by, e, yb, yl, yd);
                    }
                    {   /* engine 0x1001c4a0: each frame x = trunc(heat added - frame ticks x sinking) << 10 (the frame's net
                         * heat change, whole units), smoothed s += (x - s) >> 4 (0x1003ba60); s < 1 blue, under 0x300 yellow
                         * s / 0x300 of the bar over blue, past it red (s - 0x300) / 0x300 over yellow. The original's frames:
                         * 30 a second ASSUMED (the 3Dfx loop is uncapped) - sampled here every 1/30 s */
                        static int32_t sm, acc_t;
                        static float h0 = -1;
                        float r2;
                        if (h0 < 0) { h0 = pu->heat; acc_t = g_sim.now; }
                        while (g_sim.now - acc_t >= 33) {
                            float dh = pu->heat - h0;
                            int32_t x = (int32_t)dh * 1024;   /* ftol truncates toward zero */
                            sm += (x - sm) >> 4;
                            h0 = pu->heat; acc_t += 33;
                            if (g_sim.now - acc_t > 1000) acc_t = g_sim.now;
                        }
                        r2 = sm < 1 ? 0.0f : (float)sm / 768.0f;
                        static const float rb2[4] = {210 / 255.0f, 0, 0, 1}, rl2[4] = {1, 24 / 255.0f, 24 / 255.0f, 1}, rd2[4] = {125 / 255.0f, 0, 0, 1};
                        if (r2 > 2) r2 = 2;
                        if (r2 <= 1.0f) { hud_bar(340.5f, by, 68.0f, bb, bl, bd); if (r2 > 0) hud_bar(340.5f, by, 68.0f * r2, yb, yl, yd); }
                        else { hud_bar(340.5f, by, 68.0f, yb, yl, yd); hud_bar(340.5f, by, 68.0f * (r2 - 1.0f), rb2, rl2, rd2); }
                        (void)rt;
                    }
                }
                /* engine 0x100185e0: the heat bar's label - flag 4 (shutdown sequence) "Shutdown..." in red (0xb), with flag 8
                 * (overridden) "Overridden" in red, else "Heat" in green (0xe) */
                if (pu->shutdown_pending || pu->shutdown) hud_text(g_hud, 220.0f, 441.0f, pu->override ? "Overridden" : "Shutdown...", red);
                else hud_text(g_hud, 220.0f, 441.0f, "Heat", green);
                hud_text(g_hud, 341.0f, 441.0f, "dH/dT", green);
                if (pu->jets > 0) {   /* DOS: jump-capable mechs get a third bar, "Jets" (x 422-491) */
                    /* engine 0x1001c750: drawn while fuel (+0xc0) >= 0; shown value eases halfway each draw; fill =
                     * fuel x (W + 1) / 0x712 capped at W in green bands (0xf), the empty part red bands (0xb) */
                    static float jshown = -1;
                    float jf = pu->jet_fuel * (69.0f + 1.0f) / 1810.0f;
                    static const float rb3[4] = {210 / 255.0f, 0, 0, 1}, rl3[4] = {1, 24 / 255.0f, 24 / 255.0f, 1}, rd3[4] = {125 / 255.0f, 0, 0, 1};
                    static const float gb[4] = {0, 210 / 255.0f, 0, 1}, gl[4] = {0, 1.0f, 85 / 255.0f, 1}, gd[4] = {0, 125 / 255.0f, 0, 1};
                    if (jf < 0) jf = 0;
                    if (jf > 69.0f) jf = 69.0f;
                    jshown = jshown < 0 ? jf : jshown + (jf - jshown) * 0.5f;
                    hud_bar(422.0f, by, 69.0f, rb3, rl3, rd3);
                    hud_bar(422.0f, by, jshown, gb, gl, gd);
                    hud_text(g_hud, 422.0f, 441.0f, "Jets", green);
                }
            }
            }
            /* the MISSION OBJECTIVES display: on demand (F12), not part of the end sequence */
            if (g_show_objectives) {   /* DOS footage: blue / green text over the view, no panel */
                static const float oblue[4] = {0.15f, 0.25f, 0.85f, 1.0f};
                const mtbl_table *t0 = &g_sim.logic.tables[0];
                float y = 193.0f;
                int q2, pass;
                hud_text(g_hud, 60.0f, 165.0f, "MISSION OBJECTIVES", oblue);
                hud_line(g_hud, 60.0f, 181.0f, 60.0f + hud_text_width(g_hud, "MISSION OBJECTIVES"), 181.0f, 1.0f, oblue);
                for (pass = 0; pass < 3; pass++)   /* primary, secondary, tertiary (the node's priority 1 / 2 / 0) */
                    for (q2 = 0; q2 < t0->node_count; q2++) {
                        const mtbl_node *nd = &t0->nodes[q2];
                        const char *st;
                        static const int ORDER[3] = {1, 2, 0};
                        if (!g_sim.logic.shown[0][q2] || nd->priority != ORDER[pass]) continue;
                        st = g_sim.logic.state[0][q2] == MTBL_OK ? "Successful" : g_sim.logic.state[0][q2] == MTBL_FAILED ? "Failed" : "";
                        hud_text(g_hud, 60.0f, y, nd->priority == 1 ? "Primary:" : nd->priority == 2 ? "Secondary:" : "Tertiary:", oblue);
                        hud_text(g_hud, 130.0f, y, nd->text, green);
                        hud_text(g_hud, 600.0f - hud_text_width(g_hud, st), y, st, oblue);
                        y += 29.0f;
                    }
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
            if (g_cc_page >= 0 && g_sim_ok) {   /* the COMMAND COMPUTER panel */
                char lines[10][64];
                int nl = 0, q;
                const float bg[4] = {0.0f, 0.0f, 0.0f, 0.6f};
                float py = 120.0f;
                if (g_cc_page == 0) {
                    snprintf(lines[nl++], 64, "COMMAND COMPUTER");
                    snprintf(lines[nl++], 64, "1 Command All     %s", CC_LAST_TEXT[g_sim.last_cmd[0]]);
                    snprintf(lines[nl++], 64, "2 Change Formation");
                    if (msim_point_actor(&g_sim, 1) < 0 && msim_point_status(&g_sim, 1) == 0) snprintf(lines[nl++], 64, "NO STAR MATES");
                    for (q = 1; q <= 4; q++) {
                        int st = msim_point_status(&g_sim, q);
                        if (st == 0) snprintf(lines[nl++], 64, "%d Command Point %d  NOT AVAILABLE", q + 2, q + 1);
                        else snprintf(lines[nl++], 64, "%d Command Point %d  %s", q + 2, q + 1, MSIM_STATUS_TEXT[st]);
                    }
                } else if (g_cc_page == 1) {
                    snprintf(lines[nl++], 64, "CHANGE FORMATION");
                    snprintf(lines[nl++], 64, "Current Form: %s", MSIM_FORMATION_TEXT[g_sim.star_formation >= 0 ? g_sim.star_formation : 6]);
                    for (q = 0; q < 6; q++) snprintf(lines[nl++], 64, "%d %s", q + 1, MSIM_FORMATION_TEXT[q]);
                } else {
                    int pt = g_cc_page - 2;
                    if (pt == 0) snprintf(lines[nl++], 64, "COMMAND ALL");
                    else {
                        snprintf(lines[nl++], 64, "COMMAND POINT %d", pt + 1);
                        snprintf(lines[nl++], 64, "Status: %s", MSIM_STATUS_TEXT[msim_point_status(&g_sim, pt)]);
                    }
                    for (q = 0; q < 5; q++) snprintf(lines[nl++], 64, "%d %s", q + 1, CC_ORDER_TEXT[q]);
                }
                /* the 3Dfx menu record (0x102653f0, menus 1 / 7 / 8): no background or frame (flags 0x0a), no sprites;
                 * rect (0.22, 0.30)-(1.0, 0.60) of the screen = x 141-640, y 144-288; title and items in palette 14 (the
                 * HUD green), a line under the title (0x10006520). Row spacing ASSUMED 14 units */
                (void)bg; (void)yellow;
                py = 144.0f + 6.0f;
                for (q = 0; q < nl; q++) hud_text(g_hud, 141.0f + 0.02f * 499.0f, py + (float)q * 14.0f, lines[q], green);
                hud_line(g_hud, 141.0f + 0.02f * 499.0f, py + 11.0f, 141.0f + 0.02f * 499.0f + 200.0f, py + 11.0f, 1.0f, green);
            }
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
                    { float fk = hud_flash_level(g_hud); hud_set_flash(g_hud, 1, 0, 0, 0); hud_draw_stretch_row(g_hud, &g_hs_msgbar, 0.0f, k); hud_set_flash(g_hud, 1, 0, 0, fk); }   /* the bar: the palette's grey ramp (0xf0-0xff), which the red flash leaves alone */
                    hud_text(g_hud, hud_left_x(g_hud) + 7.0f, k ? 480.0f - 480.0f * 28.0f / 768.0f : 480.0f * 21.0f / 768.0f, g_msg_line[k].text, green);
                }
                if (!any) g_msg_t = 0;
            }
            (void)l; (void)ppd;
#undef LX
#undef LY
            hud_end(g_hud);
        } else if (g_hud && g_world && ((g_paused && g_hs_paused.tex) || (MSG_ONLY_VIEW && g_msg_t > 0))) {   /* HUD hidden / external view: PAUSED all the same */
            static const float green2[4] = {0.3f, 1.0f, 0.35f, 1.0f};
            HUD_BEGIN(g_hud, w, h);
            if (g_paused && g_hs_paused.tex)
                hud_draw(g_hud, &g_hs_paused, 320.0f - hud_px_x(g_hud, (float)g_hs_paused.w) * 0.5f,
                         144.0f - hud_px_y(g_hud, (float)g_hs_paused.h) * 0.5f, NULL);
            if (MSG_ONLY_VIEW) {   /* ejected / destroyed: the message lines stay (DOSBox: "Ejecting", "Press CTRL-Q to exit...") */
                int k;
                for (k = 0; k < 2; k++) {
                    if (g_msg_line[k].until <= g_sim.now) continue;
                    { float fk = hud_flash_level(g_hud); hud_set_flash(g_hud, 1, 0, 0, 0); hud_draw_stretch_row(g_hud, &g_hs_msgbar, 0.0f, k); hud_set_flash(g_hud, 1, 0, 0, fk); }   /* the bar: the palette's grey ramp (0xf0-0xff), which the red flash leaves alone */
                    hud_text(g_hud, hud_left_x(g_hud) + 7.0f, k ? 480.0f - 480.0f * 28.0f / 768.0f : 480.0f * 21.0f / 768.0f, g_msg_line[k].text, green2);
                }
            }
            hud_end(g_hud);
        }
        if (g_hud) hud_set_flash(g_hud, 0, 0, 0, 0);
        if (g_render_w && g_world && g_lr_fbo[0]) { lowres_present(g_out_w, g_out_h); w = g_out_w; h = g_out_h; }
        if (g_sim_ok && g_world && g_hud && !getenv("MW2_NO_STARTUP")) {
            /* the palette fade from black at the mission start (engine 0x10007900 -> 0x100079d0 -> 0x1002b090 ->
             * 0x1002ad50; DOS 0x15070 -> 0x3eae0 -> 0x3e7d0): the palette starts all black (slot 0x10, 0x1002b0c0); the
             * time-of-day code skips its first two frames, on the third it fades to the day's palette over 0x16a ticks
             * (DOS 0x16c) - counted in frames: steps = 0x16a / the last frame's ticks (DAT_1024ab4c) + 0.5, one step per
             * frame (0x1002acf0 at the frame's end), the whole screen (HUD, cockpit) with it. At 30 fps 2.0 s; DOSBox's slow
             * first frames give fewer steps (YELLSCN1 9 steps / 1.0 s, TNJ1SCN1 8 / 0.5 s) */
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
        if (g_auto_on && g_world && (g_auto_elapsed += 1.0f / 30.0f) >= g_auto_secs) {
            test_dump("final");   /* TEST hook */
            unsigned char *px = malloc((size_t)w * (size_t)h * 3);
            FILE *of = fopen(g_auto_out, "wb");
            if (px && of) {
                int y;
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
        SDL_GL_SwapWindow(win);
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
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (g_fx_ta && g_fx_ta != ta) prj_close(g_fx_ta);   /* the mission's first texture archive, after a Video mode change */
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
    if (owner < 0) sfx_play(g_sfx, snd, 0.8f, 0.0f);
    else sfx_play_at(g_sfx, snd, x - (float)g_pl.origin[0], z - (float)g_pl.origin[2], g_pl.heading + g_pl.twist);
}
