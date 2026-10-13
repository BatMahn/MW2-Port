/* skygnd.c - see skygnd.h. */
#include "skygnd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void skygnd_defaults(skygnd *s)
{
    s->tile_sky = 4.0f;
    s->tile_ground = 20000.0f;
    s->anim_rate = 0.0001f;   /* engine default scroll is tile_sky*anim = 0.0004 */
    s->fog = 0;
    s->has_fog = 0;
}

int skygnd_lookup(const char *path, const char *mission, skygnd *out)
{
    char line[256];
    FILE *f = path ? fopen(path, "r") : NULL;
    int found = -1;
    if (!f) return -1;
    while (found != 0 && fgets(line, sizeof line, f)) {
        char *p = strstr(line, "mission="), *q;
        if (!p || strncasecmp(p + 8, mission, 4) != 0) continue;
        if ((q = strstr(line, "tile_sky="))) out->tile_sky = (float)atof(q + 9);
        if ((q = strstr(line, "tile_ground="))) out->tile_ground = (float)atof(q + 12);
        if ((q = strstr(line, "anim_rate="))) out->anim_rate = (float)atof(q + 10);
        if ((q = strstr(line, "fog="))) {
            out->fog = (float)atof(q + 4);
            if (out->fog > 0.7f) out->fog *= 1.0f / 4096.0f; /* as the PowerVR engine does */
            out->has_fog = 1;
        }
        found = 0;
    }
    fclose(f);
    return found;
}

int skygnd_ti_lookup(const char *path, const char *mission, skygnd_ti *o)
{
    char line[1024];
    FILE *f = path ? fopen(path, "r") : NULL;
    memset(o, 0, sizeof *o);
    o->tile_ground = 5000.0f / 128.0f;   /* the engine's initial values (0x1009e850 = 39.0625, 0x1009e854 = 320000) */
    o->ground_max = 320000.0f;
    if (!f) return -1;
    while (!o->found && fgets(line, sizeof line, f)) {
        char m[8] = "", *h = strchr(line, '#'), cw[2] = {0, 0};
        int ii[3] = {0, 0, 0}, a[3] = {0, 0, 0}, wa[3] = {0, 0, 0}, wb[3] = {0, 0, 0}, fg[3] = {0, 0, 0}, sk[3] = {0, 0, 0};
        float tg = 0, gd = 0, gm = 0, gc[3] = {0, 0, 0}, wf[3] = {0, 0, 0}, wbf[3] = {0, 0, 0}, dn[3] = {0, 0, 0}, an[2] = {0, 0};
        int n;
        if (h) *h = 0;   /* (0x10053bf2: strchr '#') */
        n = sscanf(line, "mission=%4s globalintensity=%d tile_ground=%f grounddelta=%f ground_max=%f shadowtable=%d "
                         "gclip=%f,%f,%f amb=%d,%d,%d wa=%d,%d,%d,%c,%f,%f,%f wb=%d,%d,%d,%c,%f,%f,%f fog=%d,%d,%d "
                         "density=%f,%f,%f sky=%d,%d,%d skyAnim=%f,%f",
                   m, &ii[0], &tg, &gd, &gm, &ii[1], &gc[0], &gc[1], &gc[2], &a[0], &a[1], &a[2],
                   &wa[0], &wa[1], &wa[2], &cw[0], &wf[0], &wf[1], &wf[2], &wb[0], &wb[1], &wb[2], &cw[1], &wbf[0], &wbf[1], &wbf[2],
                   &fg[0], &fg[1], &fg[2], &dn[0], &dn[1], &dn[2], &sk[0], &sk[1], &sk[2], &an[0], &an[1]);
        if (n < 1 || strncasecmp(m, mission, 4) != 0) continue;
        o->found = 1;
        o->intensity = ii[0];
        if (n >= 3) o->tile_ground = tg / 128.0f;
        if (n >= 5) o->ground_max = gm * 2.0f;
        memcpy(o->amb, a, sizeof a);
        memcpy(o->wa, wa, sizeof wa); memcpy(o->wb, wb, sizeof wb);
        memcpy(o->wa_vec, wf, sizeof wf); memcpy(o->wb_vec, wbf, sizeof wbf);
        o->wa_type = cw[0] == 'P' ? 2 : 1;   /* (0x10053e46: T - 'P', neg, sbb, + 2) */
        o->wb_type = cw[1] == 'P' ? 2 : 1;
        o->shadowtable = ii[1]; o->grounddelta = gd; memcpy(o->gclip, gc, sizeof gc);
        memcpy(o->fog, fg, sizeof fg);
        o->dens_y = dn[0]; o->dens_m = dn[1]; o->dens_z = dn[2] * 10.0f;
        memcpy(o->sky, sk, sizeof sk);
        o->anim[0] = an[0] / 181.0f; o->anim[1] = an[1] / 181.0f;
        (void)gd;
    }
    fclose(f);
    return o->found ? 0 : -1;
}
