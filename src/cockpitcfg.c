/* cockpitcfg.c - see cockpitcfg.h */
#include "cockpitcfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const CC_LABEL[CC_CONTROLS] = {
    "Throttle ", "Chassis ", "Turret Turn", "Turret Tilt", "Eye Point ", "Eye Point ", "Eye Point ", "Recenter Torso",
    "Glance Left", "Glance Right", "Glance Up", "Glance Down", "Jump Jets On", "Jump Jets Front", "Jump Jets Back",
    "Jump Jets Left", "Jump Jets Right", "Fire Weapon", "Cycle Weapon", "Cycle Group", "Fire Group 1", "Fire Group 2",
    "Fire Group 3", "Group Toggle", "Target Next", "Target Prev", "Target Reticle", "Target Friendly", "Nearest Enemy",
    "Inspect Target", "Next Nav Point"};
const char *const CC_MAPNAME[CC_CONTROLS] = {
    "throttle", "legs_pan_delta", "torso_pan", "torso_tilt", "pilot_pan", "pilot_tilt", "zoom_factor", "torso_tilt_reset",
    "glance_left", "glance_right", "glance_up", "glance_down", "jumpjet_enabled", "jumpjet_fire_forward",
    "jumpjet_fire_backward", "jumpjet_fire_left", "jumpjet_fire_right", "weapon_fire", "weapon_cycle", "weapon_cycle_group",
    "weapon_fire_group_1", "weapon_fire_group_2", "weapon_fire_group_3", "toggle_group_fire", "advance_target",
    "previous_target", "target_reticle", "target_friendly", "nearest_enemy", "inspect_target", "advance_nav"};
const char *const CC_DEVNAME[CC_DEVICES] = {"fltstck", "joystick", "keyboard", "mouse", "msjstick", "tmaster", "vio1", "vio2"};

static int32_t rd32(const uint8_t *p) { return (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

int cc_load(cc_config *c, const char *path)
{
    uint8_t d[3872];
    FILE *f = fopen(path, "rb");
    int i, l, s;
    if (!f) return -1;
    if (fread(d, 1, sizeof d, f) != sizeof d) { fclose(f); return -1; }
    fclose(f);
    memset(c, 0, sizeof *c);
    memcpy(c->name, d, 63);
    for (i = 0; i < 16; i++) { c->dev_index[i] = rd32(d + 64 + i * 16); memcpy(c->dev_name[i], d + 68 + i * 16, 11); }
    for (l = 0; l < CC_LEVELS; l++)
        for (s = 0; s < CC_SLOTS; s++) {
            const uint8_t *p = d + 320 + (l * CC_SLOTS + s) * 24;
            cc_binding *b = &c->b[l][s];
            b->type = rd32(p); b->device = rd32(p + 4); b->flags = (uint32_t)rd32(p + 8); b->pad = rd32(p + 12);
            b->a = rd32(p + 16); b->b = rd32(p + 20);
        }
    return 0;
}

int cc_save(const cc_config *c, const char *path)
{
    uint8_t d[3872];
    FILE *f;
    int i, l, s;
    memset(d, 0, sizeof d);
    memcpy(d, c->name, 63);
    for (i = 0; i < 16; i++) { wr32(d + 64 + i * 16, (uint32_t)c->dev_index[i]); memcpy(d + 68 + i * 16, c->dev_name[i], 11); }
    for (l = 0; l < CC_LEVELS; l++)
        for (s = 0; s < CC_SLOTS; s++) {
            uint8_t *p = d + 320 + (l * CC_SLOTS + s) * 24;
            const cc_binding *b = &c->b[l][s];
            wr32(p, (uint32_t)b->type); wr32(p + 4, (uint32_t)b->device); wr32(p + 8, b->flags); wr32(p + 12, (uint32_t)b->pad);
            wr32(p + 16, (uint32_t)b->a); wr32(p + 20, (uint32_t)b->b);
        }
    if (!(f = fopen(path, "wb"))) return -1;
    if (fwrite(d, 1, sizeof d, f) != sizeof d) { fclose(f); return -1; }
    return fclose(f);
}

/* the drivers' inputs: the keyboard's from KEYBOARD.DLL (its list of map names, then the shown names, by scan code);
 * the others as their drivers list them (axes, then buttons) */
static void add(cc_input *v, int *n, int max, const char *map, const char *label)
{
    if (*n >= max) return;
    snprintf(v[*n].map, sizeof v[*n].map, "%s", map);
    snprintf(v[*n].label, sizeof v[*n].label, "%s", label);
    (*n)++;
}
static void keyboard_from_dll(cc_device *d, const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b = NULL;
    long n = 0, i, start = -1;
    char maps[128][24];
    int nm = 0, nl = 0;
    if (!f) return;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0 && (b = malloc((size_t)n + 1)) && fread(b, 1, (size_t)n, f) == (size_t)n) {
        b[n] = 0;
        for (i = 0; i + 10 < n; i++) if (memcmp(b + i, "Keyboard\0", 9) == 0) { start = i + 9; break; }
        if (start >= 0) {
            long p = start;
            int phase = 0;
            while (p < n && nl < 128) {
                const char *s = (const char *)b + p;
                size_t len = strlen(s);
                if (len == 0) { p++; if (phase == 0 && nm == 0) continue; if (phase == 1) break; continue; }
                if (phase == 0 && strcmp(s, "Escape Key") == 0) phase = 1;
                if (phase == 0) { if (nm < 128) snprintf(maps[nm++], 24, "%s", s[0] == '*' ? s + 1 : s); }
                else { if (nl < nm) add(d->button, &d->nbutton, 128, maps[nl], s); nl++; if (nl >= nm) break; }
                p += (long)len + 1;
            }
            d->present = d->nbutton > 0;
        }
    }
    free(b);
    fclose(f);
}

void cc_devices(cc_device dev[CC_DEVICES], const char *dir)
{
    static const char *HAT[4][2] = {{"HatUp", "Hat Up"}, {"HatRight", "Hat Right"}, {"HatDown", "Hat Down"}, {"HatLeft", "Hat Left"}};
    char p[1100];
    int k, h;
    memset(dev, 0, sizeof(cc_device) * CC_DEVICES);
    for (k = 0; k < CC_DEVICES; k++) {
        FILE *f;
        snprintf(dev[k].shortname, sizeof dev[k].shortname, "%s", CC_DEVNAME[k]);
        snprintf(p, sizeof p, "%s/%s.DLL", dir, CC_DEVNAME[k]);
        for (h = (int)strlen(dir) + 1; p[h]; h++) if (p[h] >= 'a' && p[h] <= 'z') p[h] = (char)(p[h] - 32);
        if ((f = fopen(p, "rb"))) { dev[k].present = 1; fclose(f); }
        if (k == 2) { keyboard_from_dll(&dev[k], p); snprintf(dev[k].title, sizeof dev[k].title, "Keyboard"); continue; }
        if (k == 0 || k == 1 || k == 4 || k == 5) {
            add(dev[k].axis, &dev[k].naxis, 8, "Up/Down", "Joystick Up/Down Movement");
            add(dev[k].axis, &dev[k].naxis, 8, "Left/Right", "Joystick Left/Right Movement");
        }
        switch (k) {
        case 0:
            snprintf(dev[k].title, sizeof dev[k].title, "Flightstick PRO");
            add(dev[k].axis, &dev[k].naxis, 8, "Rudder", "Rudder Pedals"); add(dev[k].axis, &dev[k].naxis, 8, "Throttle", "Throttle Dial");
            add(dev[k].button, &dev[k].nbutton, 128, "Trigger", "Trigger"); add(dev[k].button, &dev[k].nbutton, 128, "LeftBtn", "Left Thumb Switch");
            add(dev[k].button, &dev[k].nbutton, 128, "RightBtn", "Right Thumb Switch"); add(dev[k].button, &dev[k].nbutton, 128, "MiddleBtn", "Middle Thumb Switch");
            break;
        case 1:
            snprintf(dev[k].title, sizeof dev[k].title, "Two Button Joystick");
            add(dev[k].button, &dev[k].nbutton, 128, "Button1", "Button 1"); add(dev[k].button, &dev[k].nbutton, 128, "Button2", "Button 2");
            break;
        case 3:
            snprintf(dev[k].title, sizeof dev[k].title, "Mouse");
            add(dev[k].axis, &dev[k].naxis, 8, "Up/Down", "Mouse Up/Down Movement"); add(dev[k].axis, &dev[k].naxis, 8, "Left/Right", "Mouse Left/Right Movement");
            add(dev[k].button, &dev[k].nbutton, 128, "LeftBtn", "Left button"); add(dev[k].button, &dev[k].nbutton, 128, "MiddleBtn", "Middle button");
            add(dev[k].button, &dev[k].nbutton, 128, "RightBtn", "Right button");
            break;
        case 4:
            snprintf(dev[k].title, sizeof dev[k].title, "MS SideWinder (CH Pro Mode)");
            add(dev[k].axis, &dev[k].naxis, 8, "Twist", "Joystick Twist"); add(dev[k].axis, &dev[k].naxis, 8, "Throttle", "Throttle Slider");
            add(dev[k].button, &dev[k].nbutton, 128, "Trigger", "Trigger"); add(dev[k].button, &dev[k].nbutton, 128, "RearBtn", "Rear Switch");
            add(dev[k].button, &dev[k].nbutton, 128, "TopBtn", "Top Thumb Switch"); add(dev[k].button, &dev[k].nbutton, 128, "BottomBtn", "Bottom Thumb Switch");
            break;
        case 5:
            snprintf(dev[k].title, sizeof dev[k].title, "ThrustMaster Mark II");
            add(dev[k].axis, &dev[k].naxis, 8, "Rudder", "Rudder Pedals");
            add(dev[k].button, &dev[k].nbutton, 128, "Trigger", "Trigger"); add(dev[k].button, &dev[k].nbutton, 128, "TopBtn", "Top Switch");
            add(dev[k].button, &dev[k].nbutton, 128, "MiddleBtn", "Middle Switch"); add(dev[k].button, &dev[k].nbutton, 128, "BottomBtn", "Bottom Switch");
            break;
        default:
            snprintf(dev[k].title, sizeof dev[k].title, "Virtual i/O i-glasses! on COM%d:", k - 5);
            add(dev[k].axis, &dev[k].naxis, 8, "Tilt", "Head Tilt (look Up/Down)"); add(dev[k].axis, &dev[k].naxis, 8, "Pan", "Head Pan (look Left/Right)");
            break;
        }
        if (k == 0 || k == 4 || k == 5) for (h = 0; h < 4; h++) add(dev[k].button, &dev[k].nbutton, 128, HAT[h][0], HAT[h][1]);
    }
}

/* the Cockpit Controls' "Default" (MW2SHELL FUN_00020c10): the bindings cleared (FUN_00020730: device -1, flags / a / b
 * 0, type 0 for the seven analog slots, 1 for the rest), named "Default Config"; then for each selected device in the
 * device list's order its GIDDI\<device>.CPC, of which only the device's own bindings are taken: a binding at level l
 * goes to the first free level from l on, unless an identical binding (all six fields) is met first - then it is
 * dropped; with no free level left it is dropped too */
int cc_merge_defaults(cc_config *out, const char *dir, const int selected[CC_DEVICES])
{
    int k, l, s, ok = 0;
    char p[1100];
    cc_config d;
    if (cc_load(out, (snprintf(p, sizeof p, "%s/DEFAULT.CPC", dir), p)) != 0) memset(out, 0, sizeof *out);   /* the device table */
    for (l = 0; l < CC_LEVELS; l++)
        for (s = 0; s < CC_SLOTS; s++) {
            cc_binding *b = &out->b[l][s];
            b->device = -1; b->a = b->b = 0; b->flags = 0; b->pad = 0; b->type = s > 6 ? 1 : 0;
        }
    snprintf(out->name, sizeof out->name, "Default Config");
    for (k = 0; k < CC_DEVICES; k++) {
        if (!selected[k]) continue;
        snprintf(p, sizeof p, "%s/%s.CPC", dir, CC_DEVNAME[k]);
        { int h; for (h = (int)strlen(dir) + 1; p[h]; h++) if (p[h] >= 'a' && p[h] <= 'z') p[h] = (char)(p[h] - 32); }
        if (cc_load(&d, p) != 0) continue;
        ok = 1;
        for (l = 0; l < CC_LEVELS; l++)
            for (s = 0; s < CC_SLOTS; s++) {
                const cc_binding *nb = &d.b[l][s];
                int l2;
                if (nb->device != k) continue;
                for (l2 = l; l2 < CC_LEVELS; l2++) {
                    cc_binding *t = &out->b[l2][s];
                    if (t->device == nb->device && t->type == nb->type && t->flags == nb->flags && t->a == nb->a && t->b == nb->b &&
                        t->pad == nb->pad) break;                    /* already there */
                    if (t->device < 0) { *t = *nb; break; }
                }
            }
    }
    return ok ? 0 : -1;
}

static const char *in_map(const cc_device dev[CC_DEVICES], int device, int type, int idx)
{
    const cc_device *d;
    if (device < 0 || device >= CC_DEVICES) return NULL;
    d = &dev[device];
    if (type == 0) return idx >= 0 && idx < d->naxis ? d->axis[idx].map : NULL;
    return idx >= 0 && idx < d->nbutton ? d->button[idx].map : NULL;
}
static const char *MODNAME[3] = {"Control", "Alt", "Shift"};

void cc_describe(const cc_binding *b, const cc_device dev[CC_DEVICES], char *out, size_t cap)
{
    const char *x = in_map(dev, b->device, b->type, b->a), *y = in_map(dev, b->device, b->type, b->b);
    const char *dn = b->device == 2 ? "key" : b->device >= 0 && b->device < CC_DEVICES ? CC_DEVNAME[b->device] : "";
    if (b->device < 0 || !x) { snprintf(out, cap, "----"); return; }
    if (b->type == 2) snprintf(out, cap, "%s %s/%s", dn, x, y ? y : "?");
    else snprintf(out, cap, "%s %s", dn, x);
}

/* text building */
typedef struct { char *o; size_t n, cap; int over, analog, discrete; } sb;
static void put(sb *s, const char *t) { size_t l = strlen(t); if (s->n + l + 1 > s->cap) { s->over = 1; return; } memcpy(s->o + s->n, t, l + 1); s->n += l; }
typedef struct { const char *name; int sign; const char *dev; const char *in; int mods, neg; } cc_out;

/* the modifiers a binding must NOT have held (FUN_000201a0's first passes, bits 4-6 of its flags): a binding with
 * modifiers m marks them on every other binding of its kind on the same input number - the axes (type 0) among the
 * axes, the keys / buttons (types 1, 2) among those, a two-input control's second input (b) too - the input number
 * compared whatever the device (as the shell does); a two-input control whose inputs are one key gives the second its
 * first's modifiers as NOT modifiers. pos_b: the second input's own modifiers */
static void neg_mods(const cc_config *c, int neg_a[CC_LEVELS][CC_SLOTS], int neg_b[CC_LEVELS][CC_SLOTS], int pos_b[CC_LEVELS][CC_SLOTS])
{
    int i, j;
    const int n = CC_LEVELS * CC_SLOTS;
    for (i = 0; i < n; i++) {
        const cc_binding *k = &c->b[i / CC_SLOTS][i % CC_SLOTS];
        int m = (int)(k->flags & 7);
        neg_a[i / CC_SLOTS][i % CC_SLOTS] = 0;
        pos_b[i / CC_SLOTS][i % CC_SLOTS] = k->type == 2 && k->a != k->b ? m : 0;
        neg_b[i / CC_SLOTS][i % CC_SLOTS] = k->type == 2 && k->a == k->b ? m : 0;
    }
    for (i = 0; i < n; i++) {
        const cc_binding *k = &c->b[i / CC_SLOTS][i % CC_SLOTS];
        int m = (int)(k->flags & 7), mb = pos_b[i / CC_SLOTS][i % CC_SLOTS];
        if (k->device < 0 || !m) continue;
        for (j = 0; j < n; j++) {
            const cc_binding *o = &c->b[j / CC_SLOTS][j % CC_SLOTS];
            int *na = &neg_a[j / CC_SLOTS][j % CC_SLOTS], *nb = &neg_b[j / CC_SLOTS][j % CC_SLOTS];
            if (j == i || o->device < 0) continue;
            if (k->type == 0) { if (o->type == 0 && o->a == k->a) *na |= m; continue; }
            if (k->type > 2 || o->type == 0) continue;
            if (o->a == k->a) *na |= m;
            if (o->type == 2 && o->b == k->a) *nb |= m;
            if (o->type == 2 && o->a == k->b) *na |= mb;
            if (o->type == 2 && o->b == k->b) *nb |= mb;
        }
    }
}

size_t cc_generate(const cc_config *c, const cc_device dev[CC_DEVICES], const int selected[CC_DEVICES], char *out, size_t cap)
{
    static cc_out o[1024];
    static int neg_a[CC_LEVELS][CC_SLOTS], neg_b[CC_LEVELS][CC_SLOTS], pos_b[CC_LEVELS][CC_SLOTS];
    int no = 0, l, s, i, m, ng = 0;
    sb b = {out, 0, cap, 0, 0, 0};
#define EMIT(nm, sg, dv, inp, md) do { if (no < 1024) { o[no].name = (nm); o[no].sign = (sg); o[no].dev = (dv); o[no].in = (inp); o[no].mods = (md); o[no].neg = ng; no++; } } while (0)
    neg_mods(c, neg_a, neg_b, pos_b);
    for (l = 0; l < CC_LEVELS; l++)
        for (s = 0; s < CC_CONTROLS; s++) {
            const cc_binding *k = &c->b[l][s];
            const char *dn, *x, *y;
            int mods = (int)(k->flags & 7), rev = (k->flags & 0x80000000u) != 0;
            static char names[CC_LEVELS][CC_CONTROLS][4][40];
            if (k->device < 0 || k->device >= CC_DEVICES || !selected[k->device]) continue;
            dn = CC_DEVNAME[k->device];
            x = in_map(dev, k->device, k->type, k->a);
            y = in_map(dev, k->device, k->type, k->b);
            if (!x) continue;
            if (k->type == 2 && s <= 6 && y) {
                /* the base (legs_pan_delta -> legs_pan) and its aliases */
                static const char *ALIAS[7] = {NULL, NULL, NULL, NULL, "eyepoint_pan", "track_height", "track_distance"};
                const char *base = CC_MAPNAME[s];
                char bn[32];
                int amod = mods, bmod = pos_b[l][s], q;
                snprintf(bn, sizeof bn, "%.*s", strncmp(base + strlen(base) - 6, "_delta", 6) == 0 ? (int)strlen(base) - 6 : (int)strlen(base), base);
                for (q = 0; q < 2; q++) {
                    const char *nm = q == 0 ? bn : ALIAS[s];
                    int r = q == 1 && s == 6 ? !rev : rev;   /* track_distance runs the other way to zoom_factor */
                    if (!nm) continue;
                    snprintf(names[l][s][q * 2], 40, "%s_%s", nm, r ? "plus" : "minus");
                    snprintf(names[l][s][q * 2 + 1], 40, "%s_%s", nm, r ? "minus" : "plus");
                    ng = neg_a[l][s]; EMIT(names[l][s][q * 2], 1, dn, x, amod);
                    ng = neg_b[l][s]; EMIT(names[l][s][q * 2 + 1], 1, dn, y, bmod);
                }
            } else if (k->type == 0) {
                ng = neg_a[l][s]; EMIT(CC_MAPNAME[s], rev ? -1 : 1, dn, x, mods);
            } else {
                ng = neg_a[l][s]; EMIT(CC_MAPNAME[s], 1, dn, x, mods);
                if (s == 7) { EMIT("pilot_tilt_reset", 1, dn, x, mods); EMIT("torso_pan_reset", 1, dn, x, mods); EMIT("pilot_pan_reset", 1, dn, x, mods); }
                if (s == 8) EMIT("eyepoint_pan_minus", 1, dn, x, mods);
                if (s == 9) EMIT("eyepoint_pan_plus", 1, dn, x, mods);
                if (s >= 13 && s <= 16) EMIT("jumpjet_enabled", 1, dn, x, mods);
            }
        }
    ng = 0;
    {   /* the jump-turn keys (FUN_000201a0): after the bindings, whatever is selected - the keyboard's slot (DAT_0007a958,
         * always present), inputs 0x66 / 100: GreyDelete legs_pan_minus + jumpjet_enabled, GreyPageDown legs_pan_plus +
         * jumpjet_enabled */
        EMIT("legs_pan_minus", 1, "keyboard", "GreyDelete", 0); EMIT("jumpjet_enabled", 1, "keyboard", "GreyDelete", 0);
        EMIT("legs_pan_plus", 1, "keyboard", "GreyPageDown", 0); EMIT("jumpjet_enabled", 1, "keyboard", "GreyPageDown", 0);
    }
    put(&b, "# mw2shell CockPit Config generated map file\r\n");
    for (i = 0; i < no; i++) {
        char line[128];
        snprintf(line, sizeof line, "%s {\r\n\t%c %s\t%s\r\n", o[i].name, o[i].sign < 0 ? '-' : '+', o[i].dev, o[i].in);
        put(&b, line);
        for (m = 0; m < 3; m++) {
            if (o[i].mods & (1 << m)) { snprintf(line, sizeof line, "\t+ keyboard\t%s\r\n", MODNAME[m]); put(&b, line); }
            else if (o[i].neg & (1 << m)) { snprintf(line, sizeof line, "\t- keyboard\t%s\r\n", MODNAME[m]); put(&b, line); }
        }
        put(&b, "}\r\n");
        if (strstr(o[i].name, "_delta") || (strcmp(o[i].dev, "keyboard") != 0 && strstr(o[i].in, "/"))) b.analog++; else b.discrete++;
    }
    {
        char t[96];
        snprintf(t, sizeof t, "# analog count = %d\r\n# discrete count = %d\r\n", b.analog, b.discrete);
        put(&b, t);
    }
    return b.over ? 0 : b.n;
#undef EMIT
}
