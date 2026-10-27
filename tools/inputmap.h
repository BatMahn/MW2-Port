/*
 * inputmap.h - the original's two input files, as MECH2 reads them:
 *
 *   GAMEKEY.MAP  "COMMAND key" lines (MW2.EXE 0x484e0; '#' starts a comment anywhere): discrete commands. A key is
 *                one character ("t", "`", "\", ";", "/" ...) or a name - F1-F12, Esc, Tab, Enter, BSP, Insert, Pause,
 *                Plus - joined by '+' with CTRL / ALT / SHIFT in any order and case. One command per key: the first
 *                line naming a key wins, later ones are ignored; a command may have several keys (ALL_PT_MENU: b and
 *                CTRL+F1) and is unbound by deleting its lines. Modifiers must match exactly (t = NEXT_TARGET, CTRL+t =
 *                RESET_TARGETTING). An unknown command or a broken line ends the reading (the lines before it stay).
 *   INPUT.MAP    "control {" blocks of conditions "+ device key" (held) / "- device key" (not held), all of which
 *                must hold (up to 8); a control listed in several blocks is on when any block is (the Cockpit Controls
 *                screen writes this file; KEYBOARD.MAP, JOYSTICK.MAP, ... are templates in the same syntax). Key names
 *                are KEYBOARD.DLL's (IM_KEYNAMES): the plain cursor-pad names are the keypad (LeftArrow = keypad 4,
 *                Home = keypad 7, Insert = keypad 0, Delete = keypad .), "Grey" ones the separate cursor block and the
 *                keypad's grey operators (GreySlash, GreyStar, GreyPlus, GreyMinus); letters, One..Zero, LeftShift ...
 * Both: any case, CR LF or LF line ends, tabs or spaces; a UTF-8 byte order mark is skipped (port).
 * Stick and mouse lines are evaluated too (im_held: buttons and hat directions; im_axis: the axes - see g_im_dev).
 */
#ifndef MW2_INPUTMAP_H
#define MW2_INPUTMAP_H

#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define IM_MAX_RULES 512
#define IM_MAX_BINDS 256

typedef struct { int sign, device, code; } im_cond;   /* device 0 keyboard (code: scancode, or -1/-2/-3 Ctrl/Shift/Alt),
                                                       * 1 a stick (joystick / fltstck / msjstick / tmaster), 2 mouse, 3 other;
                                                       * stick / mouse code: 100 + axis (0 Left/Right, 1 Up/Down, 2 Rudder /
                                                       * Twist, 3 Throttle), 200 + hat direction (0 up 1 right 2 down 3 left),
                                                       * else the button number */
/* the live state of the non-keyboard devices (set by the front end each frame) */
/* the mouse as the GIDDI driver keeps it (MOUSE.DLL): INT 33h reset, limits 0..(1024 - 1) x 8 on both axes (fn 7 / 8,
 * 0x10bc1 / 0x10cc7), 1 mickey per 8 pixels on both (fn 0Fh CX = DX = 1, 0x10dcd), the pointer centred (fn 4,
 * 0x10870); its event handler (fn 0Ch) leaves the position in 0x4f2 / 0x4f4, read >> 3 (0x107b0) - a 1024 x 1024 box
 * moved 1 unit a mickey and stopped at its edges; an axis reads ((2 pos - 1024) << 16) / 1024 (0x10350). Kept here as
 * the offset from the centre, -512..511; one SDL relative-motion unit is taken as one mickey (port) */
typedef struct { SDL_Joystick *joy; Uint32 mouse_buttons; int mouse_x, mouse_y; } im_devstate;
static im_devstate g_im_dev;
#define IM_MAX_COND 8   /* conditions in a block: the sim takes 8 (0x47ed0, "chord too big" past them) */
typedef struct { char name[40]; int ncond; im_cond c[IM_MAX_COND]; } im_rule;
typedef struct { char action[40]; SDL_Keycode key; int ctrl, alt, shift; } im_bind;
#define IM_MAX_DRV 16   /* device names kept in the order the conditions name them (the sim loads at most 5, see im_load_std) */
typedef struct {
    im_rule rules[IM_MAX_RULES];
    int     nrules;
    im_bind binds[IM_MAX_BINDS];
    int     nbinds;
    int     have_gamekey, have_input;
    char    drv[IM_MAX_DRV][16];   /* the drivers the conditions name, first mention first (MW2.EXE 0x476a0) */
    int     ndrv;
} inputmap;

/* INPUT.MAP keyboard key names: GIDDI\KEYBOARD.DLL's list, in its order - entry i is DOS scan code i + 1 (the sim looks a
 * name up in the driver's list, any case, MW2.EXE 0x47950; a name starting with a digit is the entry number itself,
 * atoi). Plain cursor names are the keypad, "Grey" ones the cursor block and the keypad's operators. 0 = no PC key
 * (SDL has none); -1 / -2 / -3: either Control / Shift / Alt */
static const struct { const char *name; int sc; } IM_KEYNAMES[] = {
    {"Esc", SDL_SCANCODE_ESCAPE}, {"One", SDL_SCANCODE_1}, {"Two", SDL_SCANCODE_2}, {"Three", SDL_SCANCODE_3},
    {"Four", SDL_SCANCODE_4}, {"Five", SDL_SCANCODE_5}, {"Six", SDL_SCANCODE_6}, {"Seven", SDL_SCANCODE_7},
    {"Eight", SDL_SCANCODE_8}, {"Nine", SDL_SCANCODE_9}, {"Zero", SDL_SCANCODE_0}, {"Minus", SDL_SCANCODE_MINUS},
    {"Equal", SDL_SCANCODE_EQUALS}, {"Backspace", SDL_SCANCODE_BACKSPACE}, {"Tab", SDL_SCANCODE_TAB}, {"Q", SDL_SCANCODE_Q},
    {"W", SDL_SCANCODE_W}, {"E", SDL_SCANCODE_E}, {"R", SDL_SCANCODE_R}, {"T", SDL_SCANCODE_T}, {"Y", SDL_SCANCODE_Y},
    {"U", SDL_SCANCODE_U}, {"I", SDL_SCANCODE_I}, {"O", SDL_SCANCODE_O}, {"P", SDL_SCANCODE_P},
    {"LeftBracket", SDL_SCANCODE_LEFTBRACKET}, {"RightBracket", SDL_SCANCODE_RIGHTBRACKET}, {"Enter", SDL_SCANCODE_RETURN},
    {"LeftControl", SDL_SCANCODE_LCTRL}, {"A", SDL_SCANCODE_A}, {"S", SDL_SCANCODE_S}, {"D", SDL_SCANCODE_D},
    {"F", SDL_SCANCODE_F}, {"G", SDL_SCANCODE_G}, {"H", SDL_SCANCODE_H}, {"J", SDL_SCANCODE_J}, {"K", SDL_SCANCODE_K},
    {"L", SDL_SCANCODE_L}, {"Semicolon", SDL_SCANCODE_SEMICOLON}, {"Quote", SDL_SCANCODE_APOSTROPHE},
    {"BackQuote", SDL_SCANCODE_GRAVE}, {"LeftShift", SDL_SCANCODE_LSHIFT}, {"BackSlash", SDL_SCANCODE_BACKSLASH},
    {"Z", SDL_SCANCODE_Z}, {"X", SDL_SCANCODE_X}, {"C", SDL_SCANCODE_C}, {"V", SDL_SCANCODE_V}, {"B", SDL_SCANCODE_B},
    {"N", SDL_SCANCODE_N}, {"M", SDL_SCANCODE_M}, {"Comma", SDL_SCANCODE_COMMA}, {"Period", SDL_SCANCODE_PERIOD},
    {"Slash", SDL_SCANCODE_SLASH}, {"RightShift", SDL_SCANCODE_RSHIFT}, {"GreyStar", SDL_SCANCODE_KP_MULTIPLY},
    {"LeftAlt", SDL_SCANCODE_LALT}, {"Space", SDL_SCANCODE_SPACE}, {"CAPSLock", SDL_SCANCODE_CAPSLOCK},
    {"F1", SDL_SCANCODE_F1}, {"F2", SDL_SCANCODE_F2}, {"F3", SDL_SCANCODE_F3}, {"F4", SDL_SCANCODE_F4}, {"F5", SDL_SCANCODE_F5},
    {"F6", SDL_SCANCODE_F6}, {"F7", SDL_SCANCODE_F7}, {"F8", SDL_SCANCODE_F8}, {"F9", SDL_SCANCODE_F9}, {"F10", SDL_SCANCODE_F10},
    {"NUMLock", SDL_SCANCODE_NUMLOCKCLEAR}, {"ScrollLock", SDL_SCANCODE_SCROLLLOCK}, {"Home", SDL_SCANCODE_KP_7},
    {"UpArrow", SDL_SCANCODE_KP_8}, {"PageUp", SDL_SCANCODE_KP_9}, {"GreyMinus", SDL_SCANCODE_KP_MINUS},
    {"LeftArrow", SDL_SCANCODE_KP_4}, {"Keypad5", SDL_SCANCODE_KP_5}, {"RightArrow", SDL_SCANCODE_KP_6},
    {"GreyPlus", SDL_SCANCODE_KP_PLUS}, {"End", SDL_SCANCODE_KP_1}, {"DownArrow", SDL_SCANCODE_KP_2},
    {"PageDown", SDL_SCANCODE_KP_3}, {"Insert", SDL_SCANCODE_KP_0}, {"Delete", SDL_SCANCODE_KP_PERIOD},
    {"SYSREQ", SDL_SCANCODE_SYSREQ}, {"KeypadEnter", SDL_SCANCODE_KP_ENTER}, {"LeftBackSlash", SDL_SCANCODE_NONUSBACKSLASH},
    {"F11", SDL_SCANCODE_F11}, {"F12", SDL_SCANCODE_F12}, {"GreySlash", SDL_SCANCODE_KP_DIVIDE}, {"PA1", 0},
    {"F13", SDL_SCANCODE_F13}, {"F14", SDL_SCANCODE_F14}, {"F15", SDL_SCANCODE_F15}, {"GreyHome", SDL_SCANCODE_HOME},
    {"GreyUpArrow", SDL_SCANCODE_UP}, {"GreyPageUp", SDL_SCANCODE_PAGEUP}, {"GreyLeftArrow", SDL_SCANCODE_LEFT},
    {"GreyRightArrow", SDL_SCANCODE_RIGHT}, {"GreyEnd", SDL_SCANCODE_END}, {"GreyDownArrow", SDL_SCANCODE_DOWN},
    {"GreyPageDown", SDL_SCANCODE_PAGEDOWN}, {"GreyInsert", SDL_SCANCODE_INSERT}, {"GreyDelete", SDL_SCANCODE_DELETE},
    {"F21", SDL_SCANCODE_F21}, {"F22", SDL_SCANCODE_F22}, {"F23", SDL_SCANCODE_F23}, {"F24", SDL_SCANCODE_F24},
    {"UNNAMED_1", 0}, {"EraseEOF", 0}, {"CopyPlay", 0}, {"RightCtrl", SDL_SCANCODE_RCTRL}, {"RightAlt", SDL_SCANCODE_RALT},
    {"CRSel", SDL_SCANCODE_CRSEL}, {"EXSel", SDL_SCANCODE_EXSEL}, {"UNAMED_2", 0}, {"Clear", SDL_SCANCODE_CLEAR},
    {"Shift", -2}, {"Control", -1}, {"Alt", -3},
};

/* an INPUT.MAP keyboard key name -> SDL scancode (or -1 / -2 / -3 for either Control / Shift / Alt); 0 none */
static int im_scancode(const char *n)
{
    static const struct { const char *name; int sc; } ALIAS[] = {   /* port: names the driver lacks, kept from earlier */
        {"Escape", SDL_SCANCODE_ESCAPE}, {"Tilde", SDL_SCANCODE_GRAVE}};
    size_t i, nk = sizeof IM_KEYNAMES / sizeof IM_KEYNAMES[0];
    if (isdigit((unsigned char)n[0])) {   /* 0x47950: ctype digit -> the entry number (atoi) */
        int e = atoi(n);
        return e >= 0 && (size_t)e < nk ? IM_KEYNAMES[e].sc : 0;
    }
    for (i = 0; i < nk; i++) if (strcasecmp(IM_KEYNAMES[i].name, n) == 0) return IM_KEYNAMES[i].sc;
    for (i = 0; i < sizeof ALIAS / sizeof ALIAS[0]; i++) if (strcasecmp(ALIAS[i].name, n) == 0) return ALIAS[i].sc;
    return 0;   /* unknown: the sim warns and takes entry 0 (Esc); the port never holds it */
}

/* GAMEKEY.MAP's commands, the sim's table 0x9dca4 (153 names, compared in any case); a line naming anything else ends
 * the file's reading */
static const char *const IM_GAMEKEY_ACTIONS[] = {
    "RESET_INPUTS", "MFD_CYCLE", "TOGGLE_HTAL", "TOGGLE_REAR_VIEW", "TOGGLE_DOWN_VIEW", "TOGGLE_WEAPON_DISPLAY",
    "TOGGLE_DAMAGE_DISPLAY", "TOGGLE_TARGET_DISPLAY", "COCKPIT_VIEW", "FREEEYE_VIEW", "TRACK_VIEW", "TRACK_NEXT", "TRACK_PREV",
    "ORDINANCE_VIEW", "JUMP_DEBUGGER", "REQUEST_ORDINANCE", "FEET_TO_TORSO", "INSPECT_TARGET", "TOGGLE_HUD", "JETTISON_AMMO",
    "COCKPIT_ZOOM_IN", "COCKPIT_ZOOM_OUT", "COCKPIT_RESET_ZOOM", "DECREASE_THROTTLE", "INCREASE_THROTTLE", "THROTTLE_STOP",
    "THROTTLE_2", "THROTTLE_3", "THROTTLE_4", "THROTTLE_5", "THROTTLE_6", "THROTTLE_7", "THROTTLE_8", "THROTTLE_9",
    "THROTTLE_FULL", "NEXT_GAMETHING", "PREV_GAMETHING", "RESET_GAMETHING", "RESET_TARGETTING", "NEXT_NAVPOINT",
    "PREV_NAVPOINT", "RESET_NAVPOINT", "NEXT_GAMEPIECE", "PREV_GAMEPIECE", "RESET_GAMEPIECE", "NEXT_RADAR_MODE",
    "RADAR_ZOOM_IN", "RADAR_ZOOM_OUT", "RADAR_RESET_ZOOM", "RADAR_MAP_TOGGLE", "MAIN_MENU", "USER_MENU", "ALL_PT_MENU",
    "PT_2_MENU", "PT_3_MENU", "HELP_MENU", "EJECT", "TOGGLE_AUTOEJECT", "STARTUP_MECH", "SHUTDOWN_MECH", "REVERSE_DIRECTION",
    "OVERRIDE_SHUTDOWN", "INFRARED", "ENHANCED_VISION", "DISPLAY_OBJECTIVES", "TOGGLE_AUTOPILOT", "TOGGLE_GROUP_FIRE",
    "NEXT_TARGET", "PREV_TARGET", "RESET_TARGET", "TARGET_NEAREST_ENEMY", "TARGET_FRIENDLY", "TARGET_AT_RETICLE",
    "TARGET_LAST_SHOT", "NEXT_OBJECTIVE", "PUNCH_IN_AUTO_HDG", "TOGGLE_MASC", "SELF_DESTRUCT", "PAUSE_GAME", "EXIT_SIM",
    "ABOUT_BANNER", "DUMP_GIF", "DEBUG_CRITICAL_HITS_1", "DEBUG_CRITICAL_HITS_2", "DEBUG_CRITICAL_HITS_3",
    "DEBUG_CRITICAL_HITS_4", "DEBUG_CRITICAL_HITS_5", "DEBUG_CRITICAL_HITS_6", "DEBUG_CRITICAL_HITS_7",
    "DEBUG_CRITICAL_HITS_8", "DEBUG_CRITICAL_HITS_9", "DEBUG_CRITICAL_HITS_0", "SELECT_HEAD", "SELECT_RIGHT_TORSO",
    "SELECT_CENTER_TORSO", "SELECT_LEFT_TORSO", "SELECT_RIGHT_ARM", "SELECT_LEFT_ARM", "SELECT_RIGHT_LEG", "SELECT_LEFT_LEG",
    "AI_0_MONO_OBJECTIVES", "AI_1_MONO_OBJECTIVES", "AI_2_MONO_OBJECTIVES", "AI_3_MONO_OBJECTIVES", "AI_4_MONO_OBJECTIVES",
    "AI_5_MONO_OBJECTIVES", "AI_6_MONO_OBJECTIVES", "AI_7_MONO_OBJECTIVES", "AI_8_MONO_OBJECTIVES", "AI_9_MONO_OBJECTIVES",
    "OBJECTIVES_OFF", "NEXT_MONO_OBJECTIVE", "PREV_MONO_OBJECTIVE", "FAIL_MONO_OBJECTIVE", "SUCCEED_MONO_OBJECTIVE",
    "TOGGLE_GP_TEXT_MAPS", "TOGGLE_GT_TEXT_MAPS", "TOGGLE_GS_TEXT_MAPS", "TOGGLE_TERRAIN_TEXT_MAPS", "GO_TURBO_CONTROL",
    "TOGGLE_MEM_INFO", "TOGGLE_CACHE_INFO", "COLLISION_WIREFRAME", "TOGGLE_FRAMERATE_MAIN", "TOGGLE_SCENE_INFO_MAIN",
    "TOGGLE_EYE_POSITION_MAIN", "TOGGLE_VULNERABILITY", "TOGGLE_AMMO_LIMIT", "TOGGLE_SPLASH_DAMAGE",
    "TOGGLE_COLLISION_DAMAGE", "TOGGLE_HEAT_TRACKING", "TOGGLE_LOD_QUALITY", "DUMP_CACHE_ONCE", "DUMP_CACHE_REPEAT",
    "TOGGLE_RADAR_DAMAGE", "TOGGLE_FRAMERATE", "CRACK_MODE", "TOGGLE_EYE_POSITION", "TOGGLE_SCENE_INFO", "TOGGLE_WIREFRAME",
    "TOGGLE_PALETTE", "ADD_WEAPON_TO_GROUP_1", "ADD_WEAPON_TO_GROUP_2", "ADD_WEAPON_TO_GROUP_3", "NEXT_WEAPON_GROUP",
    "FIRE_WEAPON_GROUP", "CREATE_NAVPOINT_TEST", "DELETE_NAVPOINT_TEST", "TOGGLE_SAVE_FRAMES", "PLAY_CURRENT_MIDI",
    "PLAY_NEXT_MIDI", "HACK_JETS", "KILL_TARGETTED_MECH"};

/* INPUT.MAP's controls, the sim's table 0x9d1c4 (87 names, any case); a block naming anything else ends the reading */
static const char *const IM_INPUT_CONTROLS[] = {
    "torso_tilt", "torso_tilt_plus", "torso_tilt_minus", "torso_tilt_reset", "torso_pan", "torso_pan_plus", "torso_pan_minus",
    "torso_pan_set", "torso_pan_reset", "throttle", "legs_pan_delta", "jumpjet_enabled", "jumpjet_fire_left",
    "jumpjet_fire_right", "jumpjet_fire_forward", "jumpjet_fire_backward", "throttle_plus", "throttle_minus", "throttle_set",
    "weapon_fire", "weapon_cycle", "weapon_fire_group", "weapon_fire_group_1", "weapon_fire_group_2", "weapon_fire_group_3",
    "weapon_cycle_group", "toggle_group_fire", "legs_pan_minus", "legs_pan_plus", "advance_nav", "previous_nav", "reset_nav",
    "advance_target", "previous_target", "reset_target", "target_reticle", "target_friendly", "nearest_enemy",
    "target_last_shot", "inspect_target", "advance_gamepiece", "previous_gamepiece", "reset_gamepiece", "advance_gamething",
    "previous_gamething", "reset_gamething", "self_destruct", "autopilot", "pilot_tilt", "pilot_tilt_plus", "pilot_tilt_minus",
    "pilot_tilt_reset", "pilot_pan", "pilot_pan_plus", "pilot_pan_minus", "pilot_pan_reset", "glance_left", "glance_right",
    "glance_up", "glance_down", "eyepoint_tilt", "eyepoint_tilt_plus", "eyepoint_tilt_minus", "eyepoint_tilt_reset",
    "eyepoint_pan_delta", "eyepoint_pan_plus", "eyepoint_pan_minus", "eyepoint_pan_reset", "eyepoint_slide_delta",
    "eyepoint_slide_plus", "eyepoint_slide_minus", "track_distance_delta", "track_distance_plus", "track_distance_minus",
    "track_height_delta", "track_height_plus", "track_height_minus", "zoom_factor", "zoom_factor_plus", "zoom_factor_minus",
    "zoom_factor_reset", "menu_item", "menu_item_reset", "menu_value", "menu_value_reset", "menu_enter", "menu_abort"};

static int im_find(const char *const *list, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++) if (strcasecmp(list[i], name) == 0) return (int)i;
    return -1;
}

/* a GAMEKEY.MAP key token (MW2.EXE 0x484e0: the names table 0x9e7a4, any case; any other token is its first character,
 * lower case 0x75dee - so "Space" reads as s) -> SDL keycode; *mods |= 1 Ctrl, 2 Alt, 4 Shift */
static SDL_Keycode im_keytoken(const char *t, int *mods)
{
    static const struct { const char *name; SDL_Keycode k; } N[] = {
        {"TAB", SDLK_TAB}, {"INSERT", SDLK_INSERT}, {"ESC", SDLK_ESCAPE}, {"BSP", SDLK_BACKSPACE}, {"ENTER", SDLK_RETURN},
        {"PAUSE", SDLK_PAUSE}, {"PLUS", '+'}};
    size_t i;
    if (strcasecmp(t, "CTRL") == 0) { *mods |= 1; return SDLK_UNKNOWN; }
    if (strcasecmp(t, "ALT") == 0) { *mods |= 2; return SDLK_UNKNOWN; }
    if (strcasecmp(t, "SHIFT") == 0) { *mods |= 4; return SDLK_UNKNOWN; }
    if ((t[0] == 'F' || t[0] == 'f') && t[1] && strspn(t + 1, "0123456789") == strlen(t + 1) && atoi(t + 1) >= 1 && atoi(t + 1) <= 12)
        return SDLK_F1 + atoi(t + 1) - 1;
    for (i = 0; i < sizeof N / sizeof N[0]; i++) if (strcasecmp(N[i].name, t) == 0) return N[i].k;
    return (SDL_Keycode)tolower((unsigned char)t[0]);
}

/* the US layout's scancode for a GAMEKEY.MAP key (the original reads scan codes) */
static SDL_Scancode im_key_scancode(SDL_Keycode k)
{
    static const struct { SDL_Keycode k; SDL_Scancode s; } P[] = {
        {'`', SDL_SCANCODE_GRAVE}, {'\\', SDL_SCANCODE_BACKSLASH}, {'\'', SDL_SCANCODE_APOSTROPHE}, {';', SDL_SCANCODE_SEMICOLON},
        {'/', SDL_SCANCODE_SLASH}, {',', SDL_SCANCODE_COMMA}, {'.', SDL_SCANCODE_PERIOD}, {'-', SDL_SCANCODE_MINUS},
        {'=', SDL_SCANCODE_EQUALS}, {'[', SDL_SCANCODE_LEFTBRACKET}, {']', SDL_SCANCODE_RIGHTBRACKET}, {' ', SDL_SCANCODE_SPACE},
        {SDLK_ESCAPE, SDL_SCANCODE_ESCAPE}, {SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE}, {SDLK_PAUSE, SDL_SCANCODE_PAUSE},
        {SDLK_TAB, SDL_SCANCODE_TAB}, {SDLK_INSERT, SDL_SCANCODE_INSERT}, {SDLK_RETURN, SDL_SCANCODE_RETURN}, {'+', SDL_SCANCODE_KP_PLUS},
    };
    size_t i;
    if (k >= 'a' && k <= 'z') return (SDL_Scancode)(SDL_SCANCODE_A + (k - 'a'));
    if (k >= '1' && k <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (k - '1'));
    if (k == '0') return SDL_SCANCODE_0;
    if (k >= SDLK_F1 && k <= SDLK_F12) return (SDL_Scancode)(SDL_SCANCODE_F1 + (k - SDLK_F1));
    for (i = 0; i < sizeof P / sizeof P[0]; i++) if (P[i].k == k) return P[i].s;
    return SDL_SCANCODE_UNKNOWN;
}

/* a line as the sim's reader takes it (0x47a00): cut at the first '#' or newline; *lineno counts every line; a leading
 * UTF-8 byte order mark (a modern editor's) is dropped (port); returns 0 for a line with nothing but blanks */
static int im_line(char *line, int *lineno)
{
    char *p;
    if (++*lineno == 1 && (unsigned char)line[0] == 0xef && (unsigned char)line[1] == 0xbb && (unsigned char)line[2] == 0xbf)
        memmove(line, line + 3, strlen(line + 3) + 1);
    if ((p = strpbrk(line, "#\n"))) *p = 0;
    for (p = line; *p; p++) if (!isspace((unsigned char)*p)) return 1;
    return 0;
}

/* GAMEKEY.MAP (MW2.EXE 0x484e0): each line "COMMAND key" ("%s %s"); the key is tokens joined by '+' in any order (CTRL,
 * ALT, SHIFT and one key). One command per key: a key already taken keeps its first line ("remapping key" warning, the
 * line ignored); a command may have several keys. Reading stops - the lines before it kept - at a line without two
 * words, a key with no token, or an unknown command */
static void im_parse_gamekey(inputmap *m, FILE *f)
{
    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        char act[64], key[64], *t;
        im_bind nb;
        int i, ai, mods = 0, dup = 0;
        if (!im_line(line, &lineno)) continue;
        if (sscanf(line, "%63s %63s", act, key) != 2) { fprintf(stderr, "GAMEKEY.MAP line %d: read error - the rest is ignored\n", lineno); break; }
        memset(&nb, 0, sizeof nb);
        nb.key = SDLK_UNKNOWN;
        for (t = strtok(key, "+"), i = 0; t; t = strtok(NULL, "+"), i++) { SDL_Keycode k = im_keytoken(t, &mods); if (k != SDLK_UNKNOWN) nb.key = k; }
        if (!i) { fprintf(stderr, "GAMEKEY.MAP line %d: bad sequence - the rest is ignored\n", lineno); break; }
        nb.ctrl = mods & 1; nb.alt = (mods >> 1) & 1; nb.shift = (mods >> 2) & 1;
        for (i = 0; i < m->nbinds; i++)
            if (m->binds[i].key == nb.key && m->binds[i].ctrl == nb.ctrl && m->binds[i].alt == nb.alt && m->binds[i].shift == nb.shift) dup = 1;
        if (dup) { fprintf(stderr, "GAMEKEY.MAP line %d: key '%s' already taken - line ignored\n", lineno, act); continue; }
        ai = im_find(IM_GAMEKEY_ACTIONS, sizeof IM_GAMEKEY_ACTIONS / sizeof IM_GAMEKEY_ACTIONS[0], act);
        if (ai < 0) { fprintf(stderr, "GAMEKEY.MAP line %d: no gamekey %s - the rest is ignored\n", lineno, act); break; }
        snprintf(nb.action, sizeof nb.action, "%s", IM_GAMEKEY_ACTIONS[ai]);
        if (nb.key != SDLK_UNKNOWN && m->nbinds < IM_MAX_BINDS) m->binds[m->nbinds++] = nb;
    }
    m->have_gamekey = 1;
}

/* INPUT.MAP (MW2.EXE 0x48090): "control {" (the name in any case; an unknown one ends the reading), then up to 8
 * conditions "+|- device key" (more: "chord too big", the reading ends), "}" closes the block */
static void im_parse_input(inputmap *m, FILE *f)
{
    char line[256];
    im_rule *r = NULL;
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line, a[64], b[64], c[64];
        if (!im_line(line, &lineno)) continue;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '}') { if (r) { m->nrules++; r = NULL; } continue; }
        if (!r && sscanf(p, "%63s %63s", a, b) >= 1 && strchr(p, '{')) {
            char *br = strchr(a, '{');
            int q;
            if (br) *br = 0;
            if (im_find(IM_INPUT_CONTROLS, sizeof IM_INPUT_CONTROLS / sizeof IM_INPUT_CONTROLS[0], a) < 0) {
                fprintf(stderr, "INPUT.MAP line %d: unknown control %s - the rest is ignored\n", lineno, a);
                break;
            }
            if (m->nrules >= IM_MAX_RULES) break;
            r = &m->rules[m->nrules];
            memset(r, 0, sizeof *r);
            for (q = 0; a[q] && q < 39; q++) r->name[q] = (char)tolower((unsigned char)a[q]);
            continue;
        }
        if (r && sscanf(p, "%63s %63s %63s", a, b, c) == 3) {
            im_cond *k;
            if (r->ncond >= IM_MAX_COND) { fprintf(stderr, "INPUT.MAP line %d: chord too big - the rest is ignored\n", lineno); r = NULL; break; }
            k = &r->c[r->ncond++];
            k->sign = a[0] == '-' ? -1 : 1;
            {   /* the driver: looked up / loaded as the condition names it (0x47ed0 -> 0x476a0) */
                int q;
                for (q = 0; q < m->ndrv && strcasecmp(m->drv[q], b) != 0; q++) ;
                if (q == m->ndrv && m->ndrv < IM_MAX_DRV) snprintf(m->drv[m->ndrv++], sizeof m->drv[0], "%.15s", b);
            }
            k->device = strcasecmp(b, "keyboard") == 0 ? 0 : strcasecmp(b, "mouse") == 0 ? 2 :
                        (strcasecmp(b, "joystick") == 0 || strcasecmp(b, "fltstck") == 0 || strcasecmp(b, "msjstick") == 0 ||
                         strcasecmp(b, "tmaster") == 0) ? 1 : 3;
            if (k->device == 0) k->code = im_scancode(c);
            else {
                static const struct { const char *n; int code; } IN[] = {
                    {"Left/Right", 100}, {"Up/Down", 101}, {"Rudder", 102}, {"Twist", 102}, {"Throttle", 103},
                    {"HatUp", 200}, {"HatRight", 201}, {"HatDown", 202}, {"HatLeft", 203},
                    {"Button1", 0}, {"Trigger", 0}, {"LeftBtn", 0}, {"Button2", 1}, {"TopBtn", 1}, {"RearBtn", 1},
                    {"MiddleBtn", 2}, {"BottomBtn", 3}, {"RightBtn", 3}};
                size_t q;
                k->code = -100;
                for (q = 0; q < sizeof IN / sizeof IN[0]; q++) if (strcasecmp(IN[q].n, c) == 0) k->code = IN[q].code;
                if (k->device == 2) {   /* the mouse: LeftBtn 1, MiddleBtn 2, RightBtn 3 (SDL numbering) */
                    if (strcasecmp(c, "LeftBtn") == 0) k->code = SDL_BUTTON_LEFT;
                    else if (strcasecmp(c, "MiddleBtn") == 0) k->code = SDL_BUTTON_MIDDLE;
                    else if (strcasecmp(c, "RightBtn") == 0) k->code = SDL_BUTTON_RIGHT;
                }
            }
        }
    }
    m->have_input = 1;
}

/* both files (full paths, already resolved by the caller - any case of the names, the install folder before the disc's);
 * returns 0 if both load */
static int im_load(inputmap *m, const char *gamekey, const char *input)
{
    FILE *f;
    memset(m, 0, sizeof *m);
    if (gamekey && (f = fopen(gamekey, "rb"))) { im_parse_gamekey(m, f); fclose(f); }
    if (input && (f = fopen(input, "rb"))) { im_parse_input(m, f); fclose(f); }
    return m->have_gamekey && m->have_input ? 0 : -1;
}

/* The drivers' own map files (MW2.EXE 0x483b0, right after input.map is read): for each driver loaded so far - in the
 * order the conditions first named it, a driver counting only when giddi\<name>.dll loads, at most 5 (0x476a0: "4 <
 * count" refuses a sixth) - "giddi\<name>.std" (0x92e88 "giddi" + "\" + name + 0x93168 ".std") is read the way
 * input.map is (0x48090: its blocks join the others; an unknown control ends that file's reading); a missing one is
 * skipped. A driver a .std names is loaded then and its own .std read in turn (the loop runs to the growing count).
 * The shipped ones (FLTSTCK, JOYSTICK, MOUSE, MSJSTICK, TMASTER; no KEYBOARD.STD) bind the menu controls to the
 * device: menu_item = Up/Down, menu_value = Left/Right, menu_enter / menu_abort = two buttons (see im_menu_key).
 * open(rel) opens a file by its DOS path ("GIDDI\\JOYSTICK.STD", any case) or returns NULL. Returns the files read. */
static int im_load_std(inputmap *m, FILE *(*open)(const char *rel))
{
    int i, loaded = 0, nread = 0, have = m->have_input;
    for (i = 0; i < m->ndrv && loaded < 5; i++) {
        char rel[64];
        FILE *f;
        snprintf(rel, sizeof rel, "GIDDI\\%s.DLL", m->drv[i]);
        if (!(f = open(rel))) continue;   /* the driver does not load: not in the table */
        fclose(f);
        loaded++;
        snprintf(rel, sizeof rel, "GIDDI\\%s.STD", m->drv[i]);
        if ((f = open(rel))) { im_parse_input(m, f); fclose(f); nread++; }
    }
    m->have_input = have;
    return nread;
}

/* any INPUT.MAP condition on device d (2 = the mouse) */
static int im_uses(const inputmap *m, int d)
{
    int i, k;
    for (i = 0; i < m->nrules; i++) for (k = 0; k < m->rules[i].ncond; k++) if (m->rules[i].c[k].device == d) return 1;
    return 0;
}

/* the key event (sym, mod) is bound to action: the key's one command (GAMEKEY.MAP keeps a key's first line). The
 * keypad's + is the PLUS key */
static int im_action(const inputmap *m, const char *action, SDL_Keycode sym, Uint16 mod)
{
    int i, ctrl = (mod & KMOD_CTRL) != 0, alt = (mod & KMOD_ALT) != 0, shift = (mod & KMOD_SHIFT) != 0;
    if (sym == SDLK_KP_PLUS) sym = '+';
    for (i = 0; i < m->nbinds; i++) {
        const im_bind *b = &m->binds[i];
        if (b->key == sym && b->ctrl == ctrl && b->alt == alt && b->shift == shift) return strcmp(b->action, action) == 0;
    }
    return 0;
}

/* an action's key is held now (with its modifiers): for actions the sim treats as held, e.g. FIRE_WEAPON_GROUP */
static int im_action_held(const inputmap *m, const char *action, const Uint8 *ks)
{
    int i, ctrl = ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL], alt = ks[SDL_SCANCODE_LALT] || ks[SDL_SCANCODE_RALT];
    int shift = ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
    for (i = 0; i < m->nbinds; i++) {
        const im_bind *b = &m->binds[i];
        SDL_Scancode sc;
        if (strcmp(b->action, action) != 0) continue;
        sc = im_key_scancode(b->key);
        if (sc != SDL_SCANCODE_UNKNOWN && ks[sc] && b->ctrl == ctrl && b->alt == alt && b->shift == shift) return 1;
    }
    return 0;
}

/* an axis condition's value -1..1 (sign applied); 0 if not an axis */
/* a stick axis as the GIDDI joystick driver reports it (JOYSTICK.DLL, read 0x10500 -> 0x10660, calibration 0x10eb0):
 * the deflection from the calibrated centre within a dead zone of span / 16 (axes 1 and 2: 12.5 % of the half travel;
 * the third axis span / 10, 20 %; the fourth raw) - or twice the centre's jitter if more - reads 0; beyond it the output
 * grows linearly to full (0x10000) span / 16 short of the end (87.5 % of the half travel), its magnitude rounded up to
 * the next 1/32 (>> 11, + 1, << 11), and each read is smoothed: (2 x the last + the new) / 3 (at the 30 fps reference) */
static float im_stick_shape(int ax, float v)
{
    static float sm[8];
    static Uint32 last[8];
    float dz = ax == 2 ? 0.2f : 0.125f, a = v < 0 ? -v : v, y;
    Uint32 now = SDL_GetTicks();
    int k = ax & 7;
    if (ax == 3) {   /* "Throttle" (FLTSTCK.DLL / MSJSTICK.DLL, the only drivers naming it): calibrated from its two ends
                      * (0x10540 / MSJ 0x10560): the centre between them, the ends pulled in by a fifth of the half travel,
                      * so full (0x10000) at 80 % of the half travel; no dead zone (+0x40 = 0); the magnitude rounded up to
                      * the next 1/32 (0x10a00); stored as read - no (2 last + new) / 3 smoothing (0x109ca) */
        y = a <= 0.0f ? 0.0f : a / 0.8f;
        if (y > 0) { y = (float)((int)(y * 32.0f) + 1) / 32.0f; if (y > 1) y = 1; }
        return v < 0 ? -y : y;
    }
    if (ax > 3) y = v;   /* further axes: no calibration (stored raw) */
    else {
        y = a <= dz ? 0.0f : (a - dz) / (1.0f - dz - 0.125f);
        if (y > 0) { y = (float)((int)(y * 32.0f) + 1) / 32.0f; if (y > 1) y = 1; }
        if (v < 0) y = -y;
    }
    {   /* (2 x last + new) / 3 for each 1/30 s frame passed */
        static Uint32 acc[8];
        int n;
        if (!last[k] || now - last[k] > 1000) { sm[k] = y; acc[k] = 0; last[k] = now; return y; }
        acc[k] += (now - last[k]) * 30u; last[k] = now;
        for (n = 0; acc[k] >= 1000u && n < 30; n++) { acc[k] -= 1000u; sm[k] = (2.0f * sm[k] + y) / 3.0f; }
    }
    return sm[k];
}

static float im_cond_axis(const im_cond *c)
{
    float v = 0;
    if (c->code < 100 || c->code >= 200) return 0;
    if (c->device == 1 && g_im_dev.joy) {
        int ax = c->code - 100;
        if (ax < SDL_JoystickNumAxes(g_im_dev.joy)) v = (float)SDL_JoystickGetAxis(g_im_dev.joy, ax) / 32767.0f;
        v = im_stick_shape(ax, v);
    } else if (c->device == 2) v = c->code == 100 ? (float)g_im_dev.mouse_x / 512.0f : c->code == 101 ? (float)g_im_dev.mouse_y / 512.0f : 0;   /* MOUSE.DLL 0x10350 */
    return c->sign < 0 ? -v : v;
}

static int im_cond_held(const im_cond *c, const Uint8 *ks)
{
    int on;
    if (c->device != 0) {
        if (c->code >= 100 && c->code < 200) return 1;   /* an axis: the block's other conditions decide */
        if (c->device == 1 && g_im_dev.joy) {
            if (c->code >= 200) { Uint8 h = SDL_JoystickNumHats(g_im_dev.joy) > 0 ? SDL_JoystickGetHat(g_im_dev.joy, 0) : 0;
                                  static const Uint8 HB[4] = {SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN, SDL_HAT_LEFT}; on = (h & HB[c->code - 200]) != 0; }
            else on = c->code >= 0 && c->code < SDL_JoystickNumButtons(g_im_dev.joy) && SDL_JoystickGetButton(g_im_dev.joy, c->code);
        } else if (c->device == 2) on = c->code > 0 && (g_im_dev.mouse_buttons & SDL_BUTTON(c->code)) != 0;
        else on = 0;
        return c->sign > 0 ? on : !on;
    }
    if (c->code == -1) on = ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL];
    else if (c->code == -2) on = ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
    else if (c->code == -3) on = ks[SDL_SCANCODE_LALT] || ks[SDL_SCANCODE_RALT];
    else on = c->code > 0 && ks[c->code] != 0;   /* an unknown key (0): never held */
    return c->sign > 0 ? on : !on;
}

/* the control's axis value now: the sum over its blocks whose other conditions hold (an analog control in INPUT.MAP:
 * throttle, legs_pan_delta, torso_pan, torso_tilt, ...) on device 1 (stick) or 2 (mouse); *present = it has such a block
 * (2: and one whose other conditions hold now - the engine reads an axis only then, 0x100396e0) */
static float im_axis(const inputmap *m, const char *control, const Uint8 *ks, int device, int *present)
{
    int i, k;
    float v = 0;
    if (present) *present = 0;
    for (i = 0; i < m->nrules; i++) {
        const im_rule *r = &m->rules[i];
        int ok = 1, axis = -1;
        if (strcmp(r->name, control) != 0) continue;
        for (k = 0; k < r->ncond; k++) {
            if (r->c[k].device != 0 && r->c[k].code >= 100 && r->c[k].code < 200) { if (r->c[k].device == device) axis = k; }
            else if (!im_cond_held(&r->c[k], ks)) ok = 0;
        }
        if (axis < 0) continue;
        if (present) *present = 1;
        if (ok) { v += im_cond_axis(&r->c[axis]); if (present) *present = 2; }   /* 2: a block's other conditions hold */
    }
    return v;
}

/* the control is on now (any of its blocks holds) */
static int im_held(const inputmap *m, const char *control, const Uint8 *ks)
{
    int i, k;
    for (i = 0; i < m->nrules; i++) {
        const im_rule *r = &m->rules[i];
        int ok = r->ncond > 0;
        if (strcmp(r->name, control) != 0) continue;
        for (k = 0; k < r->ncond && ok; k++) {
            if (r->c[k].device != 0 && r->c[k].code >= 100 && r->c[k].code < 200) ok = 0;   /* axis blocks: im_axis */
            else if (!im_cond_held(&r->c[k], ks)) ok = 0;
        }
        if (ok) return 1;
    }
    return 0;
}

/* The menu controls (the menu's key reader with the open menu's flag bit 0 set - the MAIN MENU, flags 0x11: DOS MW2.EXE
 * 0x17cd0, MW2.DLL 0x10065a70), when no key was typed: menu_enter gives Enter, else menu_abort Esc (both type-2
 * controls: on the press only, 0x48970 keeps the last state); else, once the menu's timer passed 91 ticks (0x5b),
 * menu_item past an eighth of its travel (-0x2000 / 0x2000 of 0x10000) gives Up / Down (0x96 / 0x97, the DLL's 0xc6 /
 * 0xc7: the previous / next item); else, past 45 ticks (0x2d), menu_value Left / Space (0x99 / 0x20: the value down /
 * up). ticks: the menu timer (the 182 Hz real-time count, 0x61eb3 / 0x1003db83) since its last reset; *prev_enter /
 * *prev_abort: the buttons' last state (updated here; call it every frame, the menu open or not, open = 0 when closed).
 * The caller resets the timer and recentres the menu axes (menu_item_reset / menu_value_reset) on a key. The axes:
 * the sum of the stick and mouse blocks whose other conditions hold. (The 3Dfx DLL compares its float control value,
 * -1..1, with -8192.0 / 8192.0 - the DOS integer limits - so there the axes never move the menu; the port takes DOS's.) */
static SDL_Keycode im_menu_key(const inputmap *m, const Uint8 *ks, int open, int ticks, int *prev_enter, int *prev_abort)
{
    int en = im_held(m, "menu_enter", ks), ab = im_held(m, "menu_abort", ks);
    int en_press = en && !*prev_enter, ab_press = ab && !*prev_abort;
    float v;
    *prev_enter = en; *prev_abort = ab;
    if (!open) return 0;
    if (en_press) return SDLK_RETURN;
    if (ab_press) return SDLK_ESCAPE;
    if (ticks > 0x5b) {
        v = im_axis(m, "menu_item", ks, 1, NULL) + im_axis(m, "menu_item", ks, 2, NULL);
        if (v < -0.125f) return SDLK_UP;
        if (v > 0.125f) return SDLK_DOWN;
    }
    if (ticks > 0x2d) {
        v = im_axis(m, "menu_value", ks, 1, NULL) + im_axis(m, "menu_value", ks, 2, NULL);
        if (v < -0.125f) return SDLK_LEFT;
        if (v > 0.125f) return SDLK_SPACE;
    }
    return 0;
}
/* menu_item_reset / menu_value_reset (set by the menu after each key, or a block of their own): the control's input
 * recentred by its driver (0x48970 -> the driver's entry +0x10: MOUSE.DLL 0x10260 puts the pointer back in the middle of
 * the axis; JOYSTICK.DLL 0x102d0 does nothing - a stick reads where it is) */
static void im_menu_recentre(const inputmap *m, const char *control)
{
    int i, k;
    for (i = 0; i < m->nrules; i++) {
        if (strcmp(m->rules[i].name, control) != 0) continue;
        for (k = 0; k < m->rules[i].ncond; k++) {
            const im_cond *c = &m->rules[i].c[k];
            if (c->device == 2 && c->code == 100) g_im_dev.mouse_x = 0;
            if (c->device == 2 && c->code == 101) g_im_dev.mouse_y = 0;
        }
    }
}

#endif
