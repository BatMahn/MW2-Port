/* The Cockpit Controls writer against the installed INPUT.MAP: the keyboard's defaults (GIDDI\KEYBOARD.CPC) with only the
 * keyboard selected must give the shipped file byte for byte; a CPC load / save round trip must be exact. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cockpitcfg.h"
int main(int argc, char **argv)
{
    cc_device dev[CC_DEVICES];
    cc_config cfg, back;
    int sel[CC_DEVICES] = {0}, fails = 0;
    static char out[65536], ref[65536];
    char p[1100];
    size_t n, m;
    FILE *f;
    const char *dir = argc > 1 ? argv[1] : ".";
    snprintf(p, sizeof p, "%s/GIDDI", dir);
    cc_devices(dev, p);
    sel[2] = 1;
    if (cc_merge_defaults(&cfg, p, sel) != 0) { printf("cockpitcfg: no GIDDI defaults in %s\n", p); return 1; }
    n = cc_generate(&cfg, dev, sel, out, sizeof out);
    snprintf(p, sizeof p, "%s/INPUT.MAP", dir);
    if (!(f = fopen(p, "rb"))) { printf("cockpitcfg: no %s\n", p); return 1; }
    m = fread(ref, 1, sizeof ref, f);
    fclose(f);
    if (n != m || memcmp(out, ref, n) != 0) { printf("FAIL generated INPUT.MAP differs from the installed one (%zu vs %zu bytes)\n", n, m); fails++; }
    {   /* the shell's Default (FUN_00020c10) with the joystick and the mouse: each device's own bindings, level for level
         * into the first free level from there - torso_tilt (slot 3) from both: the joystick's at level 0, the mouse's
         * at level 1; the jump-turn keys written whatever is selected (FUN_000201a0) */
        int s2[CC_DEVICES] = {0};
        s2[1] = 1; s2[3] = 1;
        snprintf(p, sizeof p, "%s/GIDDI", dir);
        if (cc_merge_defaults(&cfg, p, s2) != 0) { printf("FAIL merge joystick + mouse\n"); fails++; }
        else {
            if (cfg.b[0][3].device != 1 || cfg.b[1][3].device != 3 || cfg.b[0][2].device != 3 || cfg.b[0][1].device != 1 || cfg.b[2][3].device != -1) {
                printf("FAIL merge levels: tilt %d / %d / %d, pan %d, legs %d\n", cfg.b[0][3].device, cfg.b[1][3].device, cfg.b[2][3].device, cfg.b[0][2].device, cfg.b[0][1].device); fails++;
            }
            size_t n2 = cc_generate(&cfg, dev, s2, out, sizeof out);
            if (!n2 || !strstr(out, "legs_pan_minus {\r\n\t+ keyboard\tGreyDelete")) { printf("FAIL jump-turn keys without the keyboard\n"); fails++; }
        }
    }
    snprintf(p, sizeof p, "%s/GIDDI/DEFAULT.CPC", dir);
    if (cc_load(&cfg, p) != 0 || cc_save(&cfg, "/tmp/cc_rt.cpc") != 0 || cc_load(&back, "/tmp/cc_rt.cpc") != 0 || memcmp(&cfg, &back, sizeof cfg) != 0) { printf("FAIL CPC round trip\n"); fails++; }
    printf("cockpitcfg: %d keyboard inputs, INPUT.MAP %zu bytes, %s\n", dev[2].nbutton, n, fails ? "FAILED" : "identical to the installed file");
    return fails != 0;
}
