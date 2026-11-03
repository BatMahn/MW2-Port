/* glshot - the world renderer headless to an image (no window: the GPU's off-screen picture, read back)
 *   glshot MODELS.PRJ TEXTURES.PRJ ati|3dfx RECORD MISSION CAMO CLAN out.ppm WIDTH HEIGHT
 *          [yaw] [pitch] [fog_density|-1 = mission's PowerVR fog] [bilinear 0/1] [msaa samples]
 * Sky/ground/fog settings: MW2_SKYGND=3dfx skygnd.par, MW2_SKYGND_FOG=PowerVR skygnd.par
 * RECORD "@" = the mission's world (all its xxxxAREn records); then optional
 *   [target_x target_z distance] after msaa (default: centre of the structures).
 *   MISSION: 4-letter code (CYAN...), CAMO: 0-7, CLAN: insignia index (0 Wolf, 1 Jade Falcon...)
 * Headless machines: SDL_VIDEODRIVER=offscreen (the GPU device needs SDL's video subsystem for Vulkan).
 */
#include "sdlcompat.h"   /* SDL3 (SDL2 in the legacy build: SDL=2) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "common3d.h"
#include "glr.h"
#include "rb.h"

static world_actor *g_actors;
static int g_actor_count;

int main(int argc, char **argv)
{
    char err[256];
    prj_archive *ma, *ta;
    mech3d m;
    c3d_scene sc;
    glr *r;
    glr_view v;
    int w, h, samples, y, rc = 0;
    unsigned char *px;
    FILE *f;

    if (argc < 11) { fprintf(stderr, "usage: %s MODELS.PRJ TEXTURES.PRJ ati|3dfx RECORD MISSION CAMO CLAN out.ppm W H [yaw pitch fog bilinear msaa]\n", argv[0]); return 2; }
    w = atoi(argv[9]); h = atoi(argv[10]);
    samples = argc > 15 ? atoi(argv[15]) : 4;
    if (!(ma = prj_open(argv[1], err, sizeof err)) || !(ta = prj_open(argv[2], err, sizeof err))) { fprintf(stderr, "%s\n", err); return 1; }
    if (argv[4][0] == '@') {
        char rec[16];
        memset(&m, 0, sizeof m);
        snprintf(rec, sizeof rec, "%.4sSCN1", argv[5]);
        printf("world: %d records\n", world3d_mission(ma, rec, 0, &m));
        {   /* spawned pieces as actors on the second layer; MW2_WALK_SECONDS walks the mechs forward */
            float secs = getenv("MW2_WALK_SECONDS") ? (float)atof(getenv("MW2_WALK_SECONDS")) : 0;
            int na = 0, k;
            world3d_actors(ma, rec, &g_actors, &na);
            g_actor_count = na;
            for (k = 0; k < na; k++) {
                world_actor *ac = &g_actors[k];
                int first = 0, cnt = 12;
                float h = ac->mech.heading * 3.14159265f / 180.0f;
                if (!ac->have_anim || secs <= 0) continue;
                anim_sequence(&ac->anim, 0, &first, &cnt);
                ac->t = fmodf(secs * ac->keys_per_s, (float)cnt);
                ac->mech.origin[0] += (int32_t)lrintf(sinf(h) * ac->speed * secs);
                ac->mech.origin[2] += (int32_t)lrintf(cosf(h) * ac->speed * secs);
                mech3d_pose(&ac->mech, &ac->anim, (float)first + ac->t);
                printf("  %-20s speed %5.0f u/s (%4.1f km/h)  %4.1f keys/s  moved %6.0f units\n", ac->name, ac->speed,
                       ac->speed * 0.036f, ac->keys_per_s, ac->speed * secs);
            }
            printf("spawns placed: %d\n", na);
        }
        {   /* MW2_EXTRA_MECH="RECORD x z" places one more mech (for test shots) */
            const char *em = getenv("MW2_EXTRA_MECH");
            char nm[32];
            int ex, ez;
            mech3d piece;
            if (em && sscanf(em, "%31s %d %d", nm, &ex, &ez) == 3 && mech3d_load(ma, nm, 0, &piece) == 0) {
                mech3d_part *g = realloc(m.parts, (size_t)(m.part_count + piece.part_count) * sizeof *g);
                if (g) {
                    int q;
                    m.parts = g;
                    for (q = 0; q < piece.part_count; q++) {
                        m.parts[m.part_count] = piece.parts[q];
                        m.parts[m.part_count].pos[0] += ex;
                        m.parts[m.part_count].pos[2] += ez;
                        m.part_count++;
                    }
                    free(piece.parts);
                    printf("extra mech %s at %d,%d\n", nm, ex, ez);
                } else mech3d_free(&piece);
            }
        }
        if (m.part_count == 0) { fprintf(stderr, "no world objects for %s\n", argv[5]); return 1; }
    } else if (mech3d_load(ma, argv[4], getenv("MW2_SET") ? atoi(getenv("MW2_SET")) : 0, &m) != 0) { fprintf(stderr, "cannot load %s\n", argv[4]); return 1; }
    else if (getenv("MW2_ANIM_KEY")) {   /* pose at an animation time (keys), e.g. 4.5 */
        anim_set an;
        if (m.anim_id && anim_load_id(ma, m.anim_id, &an) == 0) {
            mech3d_pose(&m, &an, (float)atof(getenv("MW2_ANIM_KEY")));
            printf("posed with anim %d (%d tracks, %d bindings) at key %s\n", m.anim_id, an.track_count, m.bind_count, getenv("MW2_ANIM_KEY"));
        }
    }
    c3d_load_scene(ma, ta, strcmp(argv[3], "ati") == 0, argv[5], &sc);
    if (!SDL_Init(SDL_INIT_VIDEO) || rb_init(NULL, RB_GPU_AUTO, samples, err, sizeof err) != 0) { fprintf(stderr, "no GPU device: %s\n", err[0] ? err : SDL_GetError()); return 1; }
    rb_frame(w, h);   /* the picture: w x h, multisampled with samples > 1 */
    rb_target(0);
    if (!(r = glr_create())) return 1;
    glr_set_mech(r, &m, (const uint8_t (*)[3])sc.palette, sc.slot, atoi(argv[6]), atoi(argv[7]));
    if (g_actors) {
        mech3d all;
        if (world3d_compose(g_actors, g_actor_count, &all) == 0) {
            glr_set_actors(r, &all, (const uint8_t (*)[3])sc.palette, sc.slot, atoi(argv[6]), atoi(argv[7]));
            free(all.parts);
        }
    }

    glr_default_view(&v);
    c3d_environment(r, &sc, argv[5], &v, argc > 13 ? (float)atof(argv[13]) : -1.0f);
    if (argc > 11) v.yaw = (float)atof(argv[11]);
    if (argc > 12) v.pitch = (float)atof(argv[12]);
    if (argv[4][0] == '@') {
        double cx = 0, cz = 0;
        int k;
        for (k = 0; k < m.part_count; k++) { cx += m.parts[k].pos[0]; cz += m.parts[k].pos[2]; }
        v.use_target = 1;
        v.target[0] = argc > 16 ? (float)atof(argv[16]) : (float)(cx / m.part_count);
        v.target[1] = 0;
        v.target[2] = argc > 17 ? (float)atof(argv[17]) : (float)(cz / m.part_count);
        v.distance_abs = argc > 18 ? (float)atof(argv[18]) : 60000.0f;
        if (argc > 19) {   /* first-person: eye at target, height argv[19], look along yaw/pitch */
            v.free_cam = 1;
            v.eye[0] = v.target[0]; v.eye[1] = (float)atof(argv[19]); v.eye[2] = v.target[2];
            v.look_yaw = v.yaw; v.look_pitch = v.pitch;
        }
        printf("world: %d objects (%d nodes), target %.0f,%.0f\n", m.part_count, m.node_count, v.target[0], v.target[2]);
    }
    if (argc > 14) v.bilinear = atoi(argv[14]);
    glr_draw(r, &v, w, h);

    px = malloc((size_t)w * (size_t)h * 3);
    if (!px) return 1;
    rb_read_rgb(0, 0, w, h, px);   /* (resolved; GL's row order, the bottom row first) */
    f = fopen(argv[8], "wb");
    if (!f) return 1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = h - 1; y >= 0; y--) {            /* GL rows are bottom-up */
        int x;
        for (x = 0; x < w; x++) fwrite(px + ((size_t)y * (size_t)w + (size_t)x) * 3, 1, 3, f);
    }
    fclose(f);
    printf("%s in %s (%d textures) camo %s clan %s %dx%d msaa %d fog %.6f -> %s\n", argv[4], argv[5], sc.textures_loaded,
           argv[6], argv[7], w, h, samples, v.fog_density, argv[8]);
    free(px); glr_destroy(r); rb_quit(); SDL_Quit(); c3d_free_scene(&sc); mech3d_free(&m);
    if (g_actors) world3d_free_actors(g_actors, g_actor_count);
    prj_close(ma); prj_close(ta);
    return rc;
}
