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
    snprintf(p, sizeof p, "%s/GIDDI/DEFAULT.CPC", dir);
    if (cc_load(&cfg, p) != 0 || cc_save(&cfg, "/tmp/cc_rt.cpc") != 0 || cc_load(&back, "/tmp/cc_rt.cpc") != 0 || memcmp(&cfg, &back, sizeof cfg) != 0) { printf("FAIL CPC round trip\n"); fails++; }
    printf("cockpitcfg: %d keyboard inputs, INPUT.MAP %zu bytes, %s\n", dev[2].nbutton, n, fails ? "FAILED" : "identical to the installed file");
    return fails != 0;
}
