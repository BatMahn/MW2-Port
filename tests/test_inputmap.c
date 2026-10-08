#include <stdlib.h>
#include "inputmap.h"
#include "inputmap_defaults.h"
static Uint8 ks[SDL_NUM_SCANCODES];
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
 printf("inputmap: %d bindings, %d control rules, %s\n", m.nbinds, m.nrules, fails ? "FAILED" : "all checks passed"); return fails != 0;}
