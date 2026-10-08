/*
 * inputmap.h - the original's two input files, as MECH2 reads them:
 *
 *   GAMEKEY.MAP  "ACTION   key" lines (# comments): discrete commands; a key is a name ("t", "F7", "Esc", "BSP",
 *                "PAUSE", "`", "\", "'", ";", "/") with optional CTRL+ / ALT+ / SHIFT+ prefixes (any case). An
 *                action may be listed more than once (ALL_PT_MENU: b and CTRL+F1). The modifiers must match exactly
 *                (t = NEXT_TARGET, CTRL+t = RESET_TARGETTING).
 *   INPUT.MAP    "control {" blocks of conditions "+ device key" (held) / "- device key" (not held), all of which
 *                must hold; a control listed in several blocks is on when any block is (the Cockpit Controls screen
 *                writes this file from the device templates KEYBOARD.MAP, JOYSTICK.MAP, ...). Key names are the DOS
 *                keyboard's: the plain cursor-pad names are the keypad (LeftArrow = keypad 4, Home = keypad 7,
 *                Insert = keypad 0, Delete = keypad .), "Grey" ones the separate cursor block and the keypad's grey
 *                operators (GreySlash, GreyStar, GreyPlus, GreyMinus).
 * Only keyboard conditions are evaluated so far; joystick / mouse lines make a block never match (NOT YET).
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
typedef struct { SDL_Joystick *joy; Uint32 mouse_buttons; float mouse_dx, mouse_dy; } im_devstate;
static im_devstate g_im_dev;
typedef struct { char name[40]; int ncond; im_cond c[6]; } im_rule;
typedef struct { char action[40]; SDL_Keycode key; int ctrl, alt, shift; } im_bind;
typedef struct {
    im_rule rules[IM_MAX_RULES];
    int     nrules;
    im_bind binds[IM_MAX_BINDS];
    int     nbinds;
    int     have_gamekey, have_input;
} inputmap;

static int im_scancode(const char *n)
{
    static const struct { const char *name; int sc; } T[] = {
        {"Equal", SDL_SCANCODE_EQUALS}, {"Minus", SDL_SCANCODE_MINUS}, {"Comma", SDL_SCANCODE_COMMA}, {"Period", SDL_SCANCODE_PERIOD},
        {"Space", SDL_SCANCODE_SPACE}, {"Enter", SDL_SCANCODE_RETURN}, {"Tab", SDL_SCANCODE_TAB}, {"Escape", SDL_SCANCODE_ESCAPE},
        {"Backspace", SDL_SCANCODE_BACKSPACE}, {"Slash", SDL_SCANCODE_SLASH}, {"Semicolon", SDL_SCANCODE_SEMICOLON},
        {"Quote", SDL_SCANCODE_APOSTROPHE}, {"BackSlash", SDL_SCANCODE_BACKSLASH}, {"Tilde", SDL_SCANCODE_GRAVE},
        {"LeftBracket", SDL_SCANCODE_LEFTBRACKET}, {"RightBracket", SDL_SCANCODE_RIGHTBRACKET},
        /* the keypad, by its cursor names (numlock off) */
        {"LeftArrow", SDL_SCANCODE_KP_4}, {"RightArrow", SDL_SCANCODE_KP_6}, {"UpArrow", SDL_SCANCODE_KP_8}, {"DownArrow", SDL_SCANCODE_KP_2},
        {"Home", SDL_SCANCODE_KP_7}, {"End", SDL_SCANCODE_KP_1}, {"PageUp", SDL_SCANCODE_KP_9}, {"PageDown", SDL_SCANCODE_KP_3},
        {"Insert", SDL_SCANCODE_KP_0}, {"Delete", SDL_SCANCODE_KP_PERIOD}, {"Keypad5", SDL_SCANCODE_KP_5},
        {"KeypadEnter", SDL_SCANCODE_KP_ENTER}, {"NUMLock", SDL_SCANCODE_NUMLOCKCLEAR},
        {"GreySlash", SDL_SCANCODE_KP_DIVIDE}, {"GreyStar", SDL_SCANCODE_KP_MULTIPLY}, {"GreyPlus", SDL_SCANCODE_KP_PLUS},
        {"GreyMinus", SDL_SCANCODE_KP_MINUS},
        /* the grey cursor block */
        {"GreyLeftArrow", SDL_SCANCODE_LEFT}, {"GreyRightArrow", SDL_SCANCODE_RIGHT}, {"GreyUpArrow", SDL_SCANCODE_UP},
        {"GreyDownArrow", SDL_SCANCODE_DOWN}, {"GreyHome", SDL_SCANCODE_HOME}, {"GreyEnd", SDL_SCANCODE_END},
        {"GreyPageUp", SDL_SCANCODE_PAGEUP}, {"GreyPageDown", SDL_SCANCODE_PAGEDOWN}, {"GreyInsert", SDL_SCANCODE_INSERT},
        {"GreyDelete", SDL_SCANCODE_DELETE},
        {"Control", -1}, {"Shift", -2}, {"Alt", -3},
    };
    size_t i;
    for (i = 0; i < sizeof T / sizeof T[0]; i++) if (strcasecmp(T[i].name, n) == 0) return T[i].sc;
    if (strlen(n) == 1 && isalpha((unsigned char)n[0])) return SDL_SCANCODE_A + (tolower((unsigned char)n[0]) - 'a');
    if (strlen(n) == 1 && n[0] >= '1' && n[0] <= '9') return SDL_SCANCODE_1 + (n[0] - '1');
    if (strcmp(n, "0") == 0) return SDL_SCANCODE_0;
    if ((n[0] == 'F' || n[0] == 'f') && atoi(n + 1) >= 1 && atoi(n + 1) <= 12) return SDL_SCANCODE_F1 + atoi(n + 1) - 1;
    return SDL_SCANCODE_UNKNOWN;
}

static SDL_Keycode im_keycode(const char *n)
{
    if (strcasecmp(n, "Esc") == 0) return SDLK_ESCAPE;
    if (strcasecmp(n, "BSP") == 0) return SDLK_BACKSPACE;
    if (strcasecmp(n, "PAUSE") == 0) return SDLK_PAUSE;
    if ((n[0] == 'F' || n[0] == 'f') && n[1] && atoi(n + 1) >= 1 && atoi(n + 1) <= 12) return SDLK_F1 + atoi(n + 1) - 1;
    if (strlen(n) == 1) return (SDL_Keycode)tolower((unsigned char)n[0]);
    return SDLK_UNKNOWN;
}

/* the US layout's scancode for a GAMEKEY.MAP key (the original reads scan codes) */
static SDL_Scancode im_key_scancode(SDL_Keycode k)
{
    static const struct { SDL_Keycode k; SDL_Scancode s; } P[] = {
        {'`', SDL_SCANCODE_GRAVE}, {'\\', SDL_SCANCODE_BACKSLASH}, {'\'', SDL_SCANCODE_APOSTROPHE}, {';', SDL_SCANCODE_SEMICOLON},
        {'/', SDL_SCANCODE_SLASH}, {',', SDL_SCANCODE_COMMA}, {'.', SDL_SCANCODE_PERIOD}, {'-', SDL_SCANCODE_MINUS},
        {'=', SDL_SCANCODE_EQUALS}, {'[', SDL_SCANCODE_LEFTBRACKET}, {']', SDL_SCANCODE_RIGHTBRACKET}, {' ', SDL_SCANCODE_SPACE},
        {SDLK_ESCAPE, SDL_SCANCODE_ESCAPE}, {SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE}, {SDLK_PAUSE, SDL_SCANCODE_PAUSE},
    };
    size_t i;
    if (k >= 'a' && k <= 'z') return (SDL_Scancode)(SDL_SCANCODE_A + (k - 'a'));
    if (k >= '1' && k <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (k - '1'));
    if (k == '0') return SDL_SCANCODE_0;
    if (k >= SDLK_F1 && k <= SDLK_F12) return (SDL_Scancode)(SDL_SCANCODE_F1 + (k - SDLK_F1));
    for (i = 0; i < sizeof P / sizeof P[0]; i++) if (P[i].k == k) return P[i].s;
    return SDL_SCANCODE_UNKNOWN;
}

static void im_parse_gamekey(inputmap *m, FILE *f)
{
    char line[256];
    while (fgets(line, sizeof line, f) && m->nbinds < IM_MAX_BINDS) {
        char act[64], key[64], *p;
        im_bind *b;
        if (line[0] == '#' || sscanf(line, "%63s %63s", act, key) != 2) continue;
        b = &m->binds[m->nbinds];
        memset(b, 0, sizeof *b);
        snprintf(b->action, sizeof b->action, "%.39s", act);
        p = key;
        for (;;) {
            if (strncasecmp(p, "CTRL+", 5) == 0) { b->ctrl = 1; p += 5; }
            else if (strncasecmp(p, "ALT+", 4) == 0) { b->alt = 1; p += 4; }
            else if (strncasecmp(p, "SHIFT+", 6) == 0) { b->shift = 1; p += 6; }
            else break;
        }
        b->key = im_keycode(p);
        if (b->key != SDLK_UNKNOWN) m->nbinds++;
    }
    m->have_gamekey = 1;
}

static void im_parse_input(inputmap *m, FILE *f)
{
    char line[256];
    im_rule *r = NULL;
    while (fgets(line, sizeof line, f)) {
        char *p = line, a[64], b[64], c[64];
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\r' || *p == '\n' || !*p) continue;
        if (*p == '}') { if (r) { m->nrules++; r = NULL; } continue; }
        if (!r && sscanf(p, "%63s %63s", a, b) >= 1 && strchr(p, '{') && m->nrules < IM_MAX_RULES) {
            char *br = strchr(a, '{');
            if (br) *br = 0;
            r = &m->rules[m->nrules];
            memset(r, 0, sizeof *r);
            snprintf(r->name, sizeof r->name, "%.39s", a);
            continue;
        }
        if (r && sscanf(p, "%63s %63s %63s", a, b, c) == 3 && r->ncond < 6) {
            im_cond *k = &r->c[r->ncond++];
            k->sign = a[0] == '-' ? -1 : 1;
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

/* both files from the game directory (dir/NAME, any case of the name tried by the caller); returns 0 if both load */
static int im_load(inputmap *m, const char *gamekey, const char *input)
{
    FILE *f;
    memset(m, 0, sizeof *m);
    if (gamekey && (f = fopen(gamekey, "rb"))) { im_parse_gamekey(m, f); fclose(f); }
    if (input && (f = fopen(input, "rb"))) { im_parse_input(m, f); fclose(f); }
    return m->have_gamekey && m->have_input ? 0 : -1;
}

/* the key event (sym, mod) is bound to action */
/* any INPUT.MAP condition on device d (2 = the mouse) */
static int im_uses(const inputmap *m, int d)
{
    int i, k;
    for (i = 0; i < m->nrules; i++) for (k = 0; k < m->rules[i].ncond; k++) if (m->rules[i].c[k].device == d) return 1;
    return 0;
}

static int im_action(const inputmap *m, const char *action, SDL_Keycode sym, Uint16 mod)
{
    int i, ctrl = (mod & KMOD_CTRL) != 0, alt = (mod & KMOD_ALT) != 0, shift = (mod & KMOD_SHIFT) != 0;
    for (i = 0; i < m->nbinds; i++) {
        const im_bind *b = &m->binds[i];
        if (b->key != sym || strcmp(b->action, action) != 0) continue;
        if (b->ctrl == ctrl && b->alt == alt && b->shift == shift) return 1;
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
static float im_cond_axis(const im_cond *c)
{
    float v = 0;
    if (c->code < 100 || c->code >= 200) return 0;
    if (c->device == 1 && g_im_dev.joy) {
        int ax = c->code - 100;
        if (ax < SDL_JoystickNumAxes(g_im_dev.joy)) v = (float)SDL_JoystickGetAxis(g_im_dev.joy, ax) / 32767.0f;
        if (v > -0.08f && v < 0.08f) v = 0;   /* centre dead zone (ASSUMED; the drivers calibrate a centre) */
    } else if (c->device == 2) v = c->code == 100 ? g_im_dev.mouse_dx : c->code == 101 ? g_im_dev.mouse_dy : 0;
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
    else if (c->code <= 0) return 0;
    else on = ks[c->code] != 0;
    return c->sign > 0 ? on : !on;
}

/* the control's axis value now: the sum over its blocks whose other conditions hold (an analog control in INPUT.MAP:
 * throttle, legs_pan_delta, torso_pan, torso_tilt, ...) on device 1 (stick) or 2 (mouse); *present = it has such a block */
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
        if (ok) v += im_cond_axis(&r->c[axis]);
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

#endif
