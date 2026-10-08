/* inputmap_defaults.h - the original's bindings, used only when the game directory lacks GAMEKEY.MAP / INPUT.MAP:
 * GAMEKEY.MAP's commands and the keyboard set of INPUT.MAP as the shipped game has them (same syntax). */
#ifndef MW2_INPUTMAP_DEFAULTS_H
#define MW2_INPUTMAP_DEFAULTS_H
static const char IM_DEFAULT_GAMEKEY[] =
    "MAIN_MENU Esc\nUSER_MENU u\nALL_PT_MENU b\nALL_PT_MENU CTRL+F1\nPT_2_MENU CTRL+F2\nPT_3_MENU CTRL+F3\nMFD_CYCLE F1\n"
    "NEXT_RADAR_MODE F2\nRADAR_MAP_TOGGLE F3\nTOGGLE_TARGET_DISPLAY F4\nTOGGLE_DAMAGE_DISPLAY F5\nTOGGLE_HTAL F6\n"
    "TOGGLE_REAR_VIEW F7\nTOGGLE_DOWN_VIEW F8\nTOGGLE_WEAPON_DISPLAY F9\nORDINANCE_VIEW F10\nTOGGLE_HUD F11\n"
    "DISPLAY_OBJECTIVES F12\nCOCKPIT_VIEW c\nCOCKPIT_RESET_ZOOM CTRL+z\nRESET_INPUTS /\nFEET_TO_TORSO m\n"
    "THROTTLE_STOP 1\nTHROTTLE_2 2\nTHROTTLE_3 3\nTHROTTLE_4 4\nTHROTTLE_5 5\nTHROTTLE_6 6\nTHROTTLE_7 7\nTHROTTLE_8 8\n"
    "THROTTLE_9 9\nTHROTTLE_FULL 0\nREVERSE_DIRECTION `\nREVERSE_DIRECTION BSP\nNEXT_TARGET t\nPREV_TARGET r\n"
    "RESET_TARGETTING CTRL+t\nTARGET_NEAREST_ENEMY e\nTARGET_FRIENDLY f\nTARGET_AT_RETICLE q\nINSPECT_TARGET i\n"
    "NEXT_NAVPOINT n\nPREV_NAVPOINT SHIFT+n\nRADAR_ZOOM_IN x\nRADAR_ZOOM_OUT SHIFT+x\nEJECT CTRL+ALT+e\nTOGGLE_AUTOEJECT CTRL+e\n"
    "STARTUP_MECH CTRL+s\nSHUTDOWN_MECH s\nOVERRIDE_SHUTDOWN o\nTOGGLE_AUTOPILOT a\nTOGGLE_MASC v\nINFRARED l\n"
    "ENHANCED_VISION w\nSELF_DESTRUCT Ctrl+Alt+x\nADD_WEAPON_TO_GROUP_1 SHIFT+1\nADD_WEAPON_TO_GROUP_2 SHIFT+2\n"
    "ADD_WEAPON_TO_GROUP_3 SHIFT+3\nTOGGLE_GROUP_FIRE \\\nNEXT_WEAPON_GROUP '\nFIRE_WEAPON_GROUP ;\nJETTISON_AMMO k\n"
    "CRACK_MODE CTRL+ALT+f\nDUMP_GIF Ctrl+p\nPAUSE_GAME Alt+p\nPAUSE_GAME PAUSE\nEXIT_SIM Ctrl+Q\n";
#define IM_K(c, k) c " {\n\t+ keyboard\t" k "\n}\n"
#define IM_K2(c, k, s, m) c " {\n\t+ keyboard\t" k "\n\t" s " keyboard\t" m "\n}\n"
static const char IM_DEFAULT_INPUT[] =
    IM_K("throttle_plus", "Equal") IM_K("throttle_minus", "Minus") IM_K("legs_pan_minus", "LeftArrow")
    IM_K("legs_pan_plus", "RightArrow") IM_K("torso_pan_minus", "Comma") IM_K("torso_pan_plus", "Period")
    IM_K("torso_tilt_plus", "UpArrow") IM_K("torso_tilt_minus", "DownArrow")
    IM_K2("pilot_pan_minus", "GreyLeftArrow", "+", "Control") IM_K2("pilot_pan_plus", "GreyRightArrow", "+", "Control")
    IM_K2("eyepoint_pan_minus", "GreyLeftArrow", "+", "Control") IM_K2("eyepoint_pan_plus", "GreyRightArrow", "+", "Control")
    IM_K2("pilot_tilt_minus", "GreyDownArrow", "+", "Control") IM_K2("pilot_tilt_plus", "GreyUpArrow", "+", "Control")
    IM_K2("track_height_minus", "GreyDownArrow", "+", "Control") IM_K2("track_height_plus", "GreyUpArrow", "+", "Control")
    IM_K2("zoom_factor_minus", "Z", "+", "Shift") IM_K2("zoom_factor_plus", "Z", "-", "Shift")
    IM_K2("track_distance_plus", "Z", "+", "Shift") IM_K2("track_distance_minus", "Z", "-", "Shift")
    IM_K("torso_tilt_reset", "Keypad5") IM_K("pilot_tilt_reset", "Keypad5") IM_K("torso_pan_reset", "Keypad5")
    IM_K("pilot_pan_reset", "Keypad5") IM_K("glance_left", "Home") IM_K("eyepoint_pan_minus", "Home") IM_K("glance_right", "PageUp") IM_K("eyepoint_pan_plus", "PageUp")
    IM_K("jumpjet_enabled", "J") IM_K("jumpjet_fire_forward", "GreyHome") IM_K("jumpjet_enabled", "GreyHome")
    IM_K("jumpjet_fire_backward", "GreyEnd") IM_K("jumpjet_enabled", "GreyEnd") IM_K("jumpjet_fire_left", "GreyInsert")
    IM_K("jumpjet_enabled", "GreyInsert") IM_K("jumpjet_fire_right", "GreyPageUp") IM_K("jumpjet_enabled", "GreyPageUp")
    IM_K("weapon_fire", "Space") IM_K("weapon_cycle", "Enter") IM_K("weapon_fire_group_1", "NUMLock")
    IM_K("weapon_fire_group_2", "GreySlash") IM_K("weapon_fire_group_3", "GreyStar") IM_K("throttle_plus", "GreyPlus")
    IM_K("throttle_minus", "GreyMinus") IM_K2("legs_pan_minus", "GreyLeftArrow", "-", "Control")
    IM_K2("legs_pan_plus", "GreyRightArrow", "-", "Control") IM_K("torso_pan_minus", "End") IM_K("torso_pan_plus", "PageDown")
    IM_K2("torso_tilt_plus", "GreyUpArrow", "-", "Control") IM_K2("torso_tilt_minus", "GreyDownArrow", "-", "Control")
    IM_K("weapon_fire", "KeypadEnter") IM_K("weapon_cycle", "Delete") IM_K("legs_pan_minus", "GreyDelete")
    IM_K("jumpjet_enabled", "GreyDelete") IM_K("legs_pan_plus", "GreyPageDown") IM_K("jumpjet_enabled", "GreyPageDown");
#endif
