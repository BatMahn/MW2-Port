/*
 * cockpitcfg.h - the shell's Cockpit Controls data (MW2SHELL FUN_00021020 and its GIDDI drivers):
 *
 *   GIDDI\*.CPC (3872 bytes): char name[64]; 16 x {i32 device index, char short_name[12]} (-1 = empty: fltstck,
 *   joystick, keyboard, mouse, msjstick, tmaster, vio1, vio2); then 4 levels (Primary .. Quaternary Controls) x 37
 *   control slots x {i32 type (0 axis, 1 button, 2 two buttons), i32 device (-1 none), u32 flags (bit 0 Ctrl, 1 Alt,
 *   2 Shift, 31 reverse), i32 0, i32 a, i32 b}. DEFAULT.CPC / KEYBOARD.CPC / JOYSTICK.CPC ... are the defaults,
 *   CONFIG00-04.CPC the saved customs (giddi\config%02d.cpc).
 *   The 31 controls (shell table 0x7f48f labels, 0x7f620 map names): throttle, legs_pan_delta, torso_pan, torso_tilt,
 *   pilot_pan, pilot_tilt, zoom_factor (the 7 directional ones), torso_tilt_reset, glance_left .. advance_nav.
 *   An input is the driver's index: keyboard = DOS scan code - 1 (names from GIDDI\KEYBOARD.DLL), others per driver.
 *
 *   INPUT.MAP, as the shell writes it (checked byte for byte against the installed file generated from KEYBOARD.CPC):
 *   "# mw2shell CockPit Config generated map file", then for each level and control: a two-button control writes
 *   <base>_minus / _plus blocks (a then b; reverse: a is _plus), the modifier on both keys (on a only when a == b); an
 *   axis writes "<base>" with "+ dev name" ("-" when reversed); a button writes "<base>". Aliases written beside:
 *   pilot_pan -> eyepoint_pan, pilot_tilt -> track_height, zoom_factor -> track_distance (opposite sense),
 *   torso_tilt_reset -> pilot_tilt_reset, torso_pan_reset, pilot_pan_reset; glance_left / right -> eyepoint_pan_minus /
 *   plus; jumpjet_fire_* -> jumpjet_enabled. A key used elsewhere with a modifier gets "- keyboard <modifier>" where it is
 *   used without. With the keyboard selected, GreyDelete / GreyPageDown turn the legs with the jets. Last: the counts.
 */
#ifndef MW2_COCKPITCFG_H
#define MW2_COCKPITCFG_H

#include <stdint.h>
#include <stddef.h>

#define CC_LEVELS   4
#define CC_SLOTS    37
#define CC_CONTROLS 31
#define CC_DEVICES  8

typedef struct { int32_t type, device; uint32_t flags; int32_t pad, a, b; } cc_binding;
typedef struct {
    char       name[64];
    int32_t    dev_index[16];
    char       dev_name[16][12];
    cc_binding b[CC_LEVELS][CC_SLOTS];
} cc_config;

typedef struct { char map[24], label[40]; } cc_input;
typedef struct {
    char     shortname[12], title[48];
    int      present;                 /* the driver file exists */
    cc_input axis[8];  int naxis;
    cc_input button[128]; int nbutton;
} cc_device;

extern const char *const CC_LABEL[CC_CONTROLS];      /* "Throttle ", "Chassis ", ... (0x7f48f) */
extern const char *const CC_MAPNAME[CC_CONTROLS];    /* "throttle", "legs_pan_delta", ... (0x7f620) */
extern const char *const CC_DEVNAME[CC_DEVICES];     /* fltstck .. vio2 */

int  cc_load(cc_config *c, const char *path);
int  cc_save(const cc_config *c, const char *path);
/* the drivers' inputs (GIDDI\<name>.DLL); dir = the GIDDI directory */
void cc_devices(cc_device dev[CC_DEVICES], const char *dir);
/* Merge the defaults of the selected devices (GIDDI\<name>.CPC) into one config (ACCEPT on the device page): each
 * device's bindings fill the first free levels of each control. */
int  cc_merge_defaults(cc_config *out, const char *dir, const int selected[CC_DEVICES]);
/* INPUT.MAP text for the selected devices; returns the length written (0 if it does not fit) */
size_t cc_generate(const cc_config *c, const cc_device dev[CC_DEVICES], const int selected[CC_DEVICES], char *out, size_t cap);
/* the text the editor shows for a binding: "key Equal/Minus", "joystick Left/Right", "----" */
void cc_describe(const cc_binding *b, const cc_device dev[CC_DEVICES], char *out, size_t cap);

#endif
