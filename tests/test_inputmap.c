#include <math.h>
#include <stdlib.h>
#include "inputmap.h"
#include "inputmap_defaults.h"
#include "datapath.h"
#include <sys/stat.h>
#include <unistd.h>
static Uint8 ks[SDL_NUM_SCANCODES];
static void parse_str(inputmap *m, const char *gk, const char *in)
{
    FILE *f;
    memset(m, 0, sizeof *m);
    if (gk) { f = fmemopen((void *)gk, strlen(gk), "r"); im_parse_gamekey(m, f); fclose(f); }
    if (in) { f = fmemopen((void *)in, strlen(in), "r"); im_parse_input(m, f); fclose(f); }
}
static void put(const char *path, const char *text) { FILE *f = fopen(path, "wb"); fputs(text, f); fclose(f); }
#define CHK(x) do { if (!(x)) { printf("FAIL %s\n", #x); fails++; } } while (0)
static int fails;
int main(int argc, char **argv){ inputmap m; char a[1100], b[1100]; snprintf(a, sizeof a, "%s/GAMEKEY.MAP", argc > 1 ? argv[1] : "."); snprintf(b, sizeof b, "%s/INPUT.MAP", argc > 1 ? argv[1] : "."); im_load(&m, a, b); printf("binds %d rules %d\n",m.nbinds,m.nrules);
 CHK(im_action(&m,"NEXT_TARGET",SDLK_t,0)); CHK(!im_action(&m,"NEXT_TARGET",SDLK_t,KMOD_LCTRL)); CHK(im_action(&m,"RESET_TARGETTING",SDLK_t,KMOD_LCTRL));
 CHK(im_action(&m,"ALL_PT_MENU",SDLK_F1,KMOD_RCTRL)); CHK(im_action(&m,"PREV_NAVPOINT",SDLK_n,KMOD_LSHIFT)); CHK(im_action(&m,"EXIT_SIM",SDLK_q,KMOD_LCTRL));
 CHK(im_action(&m,"REVERSE_DIRECTION",SDLK_BACKSPACE,0)); CHK(im_action(&m,"TOGGLE_GROUP_FIRE",SDLK_BACKSLASH,0)); CHK(im_action(&m,"SELF_DESTRUCT",SDLK_x,KMOD_LCTRL|KMOD_LALT));
 ks[SDL_SCANCODE_LEFT]=1; CHK(im_held(&m,"legs_pan_minus",ks)); ks[SDL_SCANCODE_LCTRL]=1; CHK(!im_held(&m,"legs_pan_minus",ks)); CHK(im_held(&m,"pilot_pan_minus",ks)); memset(ks,0,sizeof ks);
 ks[SDL_SCANCODE_KP_4]=1; CHK(im_held(&m,"legs_pan_minus",ks)); memset(ks,0,sizeof ks);
 ks[SDL_SCANCODE_KP_1]=1; CHK(im_held(&m,"torso_pan_minus",ks)); memset(ks,0,sizeof ks);
 ks[SDL_SCANCODE_DELETE]=1; CHK(im_held(&m,"legs_pan_minus",ks) && im_held(&m,"jumpjet_enabled",ks)); memset(ks,0,sizeof ks);
 ks[SDL_SCANCODE_Z]=1; CHK(im_held(&m,"zoom_factor_plus",ks) && !im_held(&m,"zoom_factor_minus",ks)); ks[SDL_SCANCODE_RSHIFT]=1; CHK(im_held(&m,"zoom_factor_minus",ks)); memset(ks,0,sizeof ks);
 ks[SDL_SCANCODE_SEMICOLON]=1; CHK(im_action_held(&m,"FIRE_WEAPON_GROUP",ks));
 { FILE *f = fmemopen((void *)IM_DEFAULT_GAMEKEY, strlen(IM_DEFAULT_GAMEKEY), "r"); inputmap d; memset(&d, 0, sizeof d); im_parse_gamekey(&d, f); fclose(f); f = fmemopen((void *)IM_DEFAULT_INPUT, strlen(IM_DEFAULT_INPUT), "r"); im_parse_input(&d, f); fclose(f); CHK(d.nbinds == m.nbinds); CHK(d.nrules == m.nrules); }
 /* the throttle axis (FLTSTCK / MSJSTICK 0x10a00): v / 0.8 rounded up to the next 1/32, no dead zone, full at 80 %; the
  * mouse (MOUSE.DLL 0x10350): the box offset / 512 */
 CHK(im_stick_shape(3, 0.0f) == 0.0f); CHK(fabsf(im_stick_shape(3, 0.4f) - 17.0f / 32.0f) < 1e-6f); CHK(fabsf(im_stick_shape(3, -0.4f) + 17.0f / 32.0f) < 1e-6f);
 CHK(im_stick_shape(3, 0.8f) == 1.0f); CHK(im_stick_shape(3, 1.0f) == 1.0f); CHK(fabsf(im_stick_shape(3, 0.01f) - 1.0f / 32.0f) < 1e-6f);
 { im_cond mc = {-1, 2, 101}; g_im_dev.mouse_y = 256; CHK(fabsf(im_cond_axis(&mc) + 0.5f) < 1e-6f); mc.sign = 1; mc.code = 100; g_im_dev.mouse_x = -512; CHK(im_cond_axis(&mc) == -1.0f); g_im_dev.mouse_x = g_im_dev.mouse_y = 0; }
 {   /* the original's GAMEKEY.MAP reading (MW2.EXE 0x484e0): a modern editor's file - byte order mark, CR LF, tabs, comments
      * after the words, lower-case commands and keys, modifiers in any order; one command per key (the first line wins);
      * a command with two keys; Space reads as s; an unknown command ends the reading */
   inputmap g;
   parse_str(&g, "\xef\xbb\xbfmain_menu\tesc\r\nNext_Target  T   # comment\r\nPREV_TARGET t\r\nRESET_TARGETTING t+Ctrl\r\n"
                 "\r\n   \t\r\nTOGGLE_HUD F11\r\nTOGGLE_HUD Tab\r\nFIRE_WEAPON_GROUP Space\r\nEXIT_SIM ctrl+q#x\r\nPAUSE_GAME Plus\r\n"
                 "TOGGLE_MASC Insert\r\nMFD_CYCLE Enter\r\nNOT_A_COMMAND k\r\nJETTISON_AMMO j\r\n", NULL);
   CHK(im_action(&g, "MAIN_MENU", SDLK_ESCAPE, 0)); CHK(im_action(&g, "NEXT_TARGET", SDLK_t, 0)); CHK(!im_action(&g, "PREV_TARGET", SDLK_t, 0));
   CHK(im_action(&g, "RESET_TARGETTING", SDLK_t, KMOD_LCTRL)); CHK(im_action(&g, "TOGGLE_HUD", SDLK_F11, 0)); CHK(im_action(&g, "TOGGLE_HUD", SDLK_TAB, 0));
   CHK(im_action(&g, "FIRE_WEAPON_GROUP", SDLK_s, 0)); CHK(im_action(&g, "EXIT_SIM", SDLK_q, KMOD_RCTRL)); CHK(im_action(&g, "PAUSE_GAME", SDLK_KP_PLUS, 0));
   CHK(im_action(&g, "TOGGLE_MASC", SDLK_INSERT, 0)); CHK(im_action(&g, "MFD_CYCLE", SDLK_RETURN, 0));
   CHK(!im_action(&g, "JETTISON_AMMO", SDLK_j, 0));   /* after the unknown command: not read */
   CHK(g.nbinds == 10);
 }
 {   /* INPUT.MAP: KEYBOARD.DLL's names (LeftControl, One, a number = the list entry), any case of the control, CR LF and
      * a byte order mark, comments, up to 8 conditions; an unknown control ends the reading */
   inputmap g;
   parse_str(&g, NULL, "\xef\xbb\xbfWEAPON_FIRE {\r\n\t+ keyboard\tOne\r\n\t- keyboard\tLeftControl  # Just because!\r\n}\r\n"
                       "throttle_plus {\n + keyboard 16\n}\nlegs_pan_minus {\n+ keyboard a\n+ keyboard LeftShift\n- keyboard RightShift\n"
                       "- keyboard Control\n- keyboard Alt\n- keyboard CAPSLock\n- keyboard BackQuote\n- keyboard Esc\n}\n"
                       "bogus_control {\n+ keyboard x\n}\nlegs_pan_plus {\n+ keyboard d\n}\n");
   memset(ks, 0, sizeof ks); ks[SDL_SCANCODE_1] = 1; CHK(im_held(&g, "weapon_fire", ks)); ks[SDL_SCANCODE_LCTRL] = 1; CHK(!im_held(&g, "weapon_fire", ks));
   memset(ks, 0, sizeof ks); ks[SDL_SCANCODE_W] = 1; CHK(im_held(&g, "throttle_plus", ks));
   memset(ks, 0, sizeof ks); ks[SDL_SCANCODE_A] = 1; ks[SDL_SCANCODE_LSHIFT] = 1; CHK(im_held(&g, "legs_pan_minus", ks)); CHK(g.rules[2].ncond == 8);
   ks[SDL_SCANCODE_RSHIFT] = 1; CHK(!im_held(&g, "legs_pan_minus", ks));
   CHK(g.nrules == 3);   /* bogus_control and what follows: not read */
   memset(ks, 0, sizeof ks);
 }
 {   /* a WASD set: GAMEKEY.MAP's w / a / s moved off (a key does only what its line says), INPUT.MAP drives with W A S D */
   inputmap g;
   const Uint8 *k2 = ks;
   parse_str(&g, "TOGGLE_AUTOPILOT p\nSHUTDOWN_MECH ctrl+d\nSTARTUP_MECH CTRL+s\nENHANCED_VISION v\nTOGGLE_MASC ctrl+v\n",
             "throttle_plus {\n+ keyboard W\n}\nthrottle_minus {\n+ keyboard S\n}\nlegs_pan_minus {\n+ keyboard A\n}\nlegs_pan_plus {\n+ keyboard D\n}\n"
             "torso_pan {\n+ mouse Left/Right\n}\nweapon_fire {\n+ mouse LeftBtn\n}\n");
   CHK(!im_action(&g, "TOGGLE_AUTOPILOT", SDLK_a, 0)); CHK(!im_action(&g, "SHUTDOWN_MECH", SDLK_s, 0)); CHK(!im_action(&g, "ENHANCED_VISION", SDLK_w, 0));
   CHK(im_action(&g, "SHUTDOWN_MECH", SDLK_d, KMOD_LCTRL)); CHK(im_action(&g, "ENHANCED_VISION", SDLK_v, 0));
   ks[SDL_SCANCODE_W] = 1; CHK(im_held(&g, "throttle_plus", k2) && !im_held(&g, "throttle_minus", k2)); memset(ks, 0, sizeof ks);
   ks[SDL_SCANCODE_D] = 1; CHK(im_held(&g, "legs_pan_plus", k2)); memset(ks, 0, sizeof ks);
   CHK(im_uses(&g, 2));
 }
 {   /* the files as a player leaves them: lower-case names on a case-sensitive disk, read-only - found and read through the
      * game's file search (datapath: the install folder first, then the disc's MECH2 folder) */
   char d[256], cd[300], sub[320], p1[400], p2[400], o1[2048], o2[2048];
   inputmap g;
   snprintf(d, sizeof d, "/tmp/test_inputmap_%d", (int)getpid());
   snprintf(cd, sizeof cd, "%s/cd", d); snprintf(sub, sizeof sub, "%s/MECH2", cd);
   mkdir(d, 0755); mkdir(cd, 0755); mkdir(sub, 0755);
   snprintf(p1, sizeof p1, "%s/gamekey.map", d); put(p1, "TOGGLE_AUTOPILOT p\n"); chmod(p1, 0444);
   snprintf(p2, sizeof p2, "%s/Input.Map", sub); put(p2, "throttle_plus {\n+ keyboard W\n}\n"); chmod(p2, 0444);
   dp_clear_roots(); dp_add_root(d); dp_add_root(sub); dp_add_root(cd);
   CHK(dp_resolve_in(d, "GAMEKEY.MAP", o1, sizeof o1) == 0 && strcmp(o1, p1) == 0);
   CHK(dp_resolve_in(d, "INPUT.MAP", o2, sizeof o2) != 0 && dp_resolve("INPUT.MAP", o2, sizeof o2) == 0 && strcmp(o2, p2) == 0);
   im_load(&g, o1, o2);
   CHK(g.have_gamekey && g.have_input && g.nbinds == 1 && g.nrules == 1 && im_action(&g, "TOGGLE_AUTOPILOT", SDLK_p, 0));
   ks[SDL_SCANCODE_W] = 1; CHK(im_held(&g, "throttle_plus", ks)); memset(ks, 0, sizeof ks);
   chmod(p1, 0644); chmod(p2, 0644); remove(p1); remove(p2); rmdir(sub); rmdir(cd); rmdir(d);
 }
 printf("inputmap: %d bindings, %d control rules, %s\n", m.nbinds, m.nrules, fails ? "FAILED" : "all checks passed"); return fails != 0;}
