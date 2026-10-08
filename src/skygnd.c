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
