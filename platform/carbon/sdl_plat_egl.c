/*
 * sdl_plat_egl.c - a test platform for the SDL subset on Linux: windows are EGL pbuffers (Mesa), no input, the audio
 * device consumes nothing. It lets the Carbon build's SDL layer (sdl_core.c) and the OpenGL 1.2 renderer run headless
 * here, so their pictures can be compared with the real SDL3 build's. Not a way to play.
 */
#include <EGL/egl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sdl_plat.h"

struct plat_window { EGLSurface surf; int w, h; };
static EGLDisplay g_dpy;
static EGLConfig g_cfg;
static int g_audio;
static plat_audio_fill g_fill;
static FILE *g_wav;
static uint64_t g_t0, g_done;

int plat_init(char *err, int errn)
{
    EGLint n = 0;
    static const EGLint attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,
                                  EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!g_dpy || !eglInitialize(g_dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_API) || !eglChooseConfig(g_dpy, attr, &g_cfg, 1, &n) || n < 1) {
        snprintf(err, (size_t)errn, "EGL: no display / config");
        return -1;
    }
    return 0;
}
void plat_quit(void) { if (g_dpy) eglTerminate(g_dpy); g_dpy = NULL; }
uint64_t plat_ticks_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u; }
void plat_delay_ms(uint32_t ms) { usleep(ms * 1000u); }
const char *plat_name(void) { return "egl"; }

static EGLSurface mk(int w, int h) { EGLint a[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE}; return eglCreatePbufferSurface(g_dpy, g_cfg, a); }
plat_window *plat_window_create(const char *title, int w, int h, int fullscreen, int hidden)
{
    plat_window *pw = calloc(1, sizeof *pw);
    (void)title; (void)fullscreen; (void)hidden;
    if (!pw) return NULL;
    pw->w = w > 0 ? w : 640; pw->h = h > 0 ? h : 480;
    pw->surf = mk(pw->w, pw->h);
    if (pw->surf == EGL_NO_SURFACE) { free(pw); return NULL; }
    return pw;
}
void plat_window_destroy(plat_window *w) { if (w) { eglDestroySurface(g_dpy, w->surf); free(w); } }
void plat_window_title(plat_window *w, const char *t) { (void)w; (void)t; }
void plat_window_size(plat_window *w, int *pw, int *ph) { *pw = w->w; *ph = w->h; }
int plat_window_set_size(plat_window *w, int pw, int ph)
{
    EGLContext c = eglGetCurrentContext();
    if (pw == w->w && ph == w->h) return 0;
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(g_dpy, w->surf);
    w->w = pw; w->h = ph; w->surf = mk(pw, ph);
    if (c != EGL_NO_CONTEXT) eglMakeCurrent(g_dpy, w->surf, w->surf, c);
    return 0;
}
int plat_window_fullscreen(plat_window *w, int on, int fw, int fh) { if (on && fw > 0) return plat_window_set_size(w, fw, fh); return 0; }
void plat_window_show(plat_window *w, int show) { (void)w; (void)show; }
int plat_window_focused(plat_window *w) { (void)w; return 1; }

void *plat_gl_create(plat_window *w)
{
    EGLContext c = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, NULL);
    if (c == EGL_NO_CONTEXT) return NULL;
    eglMakeCurrent(g_dpy, w->surf, w->surf, c);
    return c;
}
int plat_gl_make_current(plat_window *w, void *ctx) { return eglMakeCurrent(g_dpy, w->surf, w->surf, (EGLContext)ctx) ? 0 : -1; }
void plat_gl_destroy(void *ctx) { if (ctx) { eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); eglDestroyContext(g_dpy, (EGLContext)ctx); } }
void plat_gl_swap(plat_window *w) { eglSwapBuffers(g_dpy, w->surf); }
void *plat_gl_proc(const char *name) { return (void *)eglGetProcAddress(name); }
void plat_gl_swap_interval(int n) { eglSwapInterval(g_dpy, n); }

/* tests: MW2_EGL_WAV=file - the device consumed in real time, written as raw 16-bit stereo 44.1 kHz */
void plat_pump(void)
{
    int16_t buf[4096 * 2];
    uint64_t want;
    if (!g_audio || !g_fill) return;
    want = (plat_ticks_us() - g_t0) * 44100u / 1000000u;
    while (g_done < want) {
        int n = want - g_done > 4096 ? 4096 : (int)(want - g_done);
        g_fill(buf, n);
        if (g_wav) fwrite(buf, 4, (size_t)n, g_wav);
        g_done += (uint64_t)n;
    }
}
void plat_relative_mouse(plat_window *w, int on) { (void)w; (void)on; }
void plat_mouse_state(float *x, float *y, uint32_t *b) { *x = *y = 0; *b = 0; }

int plat_audio_open(int rate, int frames, plat_audio_fill fill) { (void)rate; (void)frames; g_fill = fill; g_audio = 1; g_t0 = plat_ticks_us(); g_done = 0; if (getenv("MW2_EGL_WAV")) g_wav = fopen(getenv("MW2_EGL_WAV"), "wb"); return 0; }
void plat_audio_close(void) { g_audio = 0; if (g_wav) fclose(g_wav); g_wav = NULL; }

void plat_message_box(int error, const char *title, const char *msg) { fprintf(stderr, "%s: %s: %s\n", error ? "error" : "warning", title, msg); }
const char *plat_base_path(void)
{
    static char p[1024];
    ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
    if (n <= 0) return "./";
    p[n] = 0;
    { char *s = strrchr(p, '/'); if (s) s[1] = 0; }
    return p;
}
int plat_display_modes(int (*wh)[2], int max)
{
    static const int m[][2] = {{1024, 768}, {800, 600}, {640, 480}};
    int k;
    for (k = 0; k < 3 && k < max; k++) { wh[k][0] = m[k][0]; wh[k][1] = m[k][1]; }
    return k;
}
void plat_desktop_mode(int *w, int *h) { *w = 1024; *h = 768; }

/* the stand-in has no QuickTime: the Mac build's built-in synth (qtmidi.c) absent */
int qtmidi_open(void) { return -1; }
void qtmidi_msg(void *user, int status, int a, int b) { (void)user; (void)status; (void)a; (void)b; }
void qtmidi_close(void) {}
/* no folder dialog on the stand-in (the Carbon build: Navigation Services) */
int mw2_pick_folder(const char *title, char *out, size_t n) { (void)title; (void)out; (void)n; return -1; }
/* the stand-in's "video memory" (the Carbon build asks AGL): MW2_VRAM_MB, else 16 */
int mw2_mac_vram_mb(void) { return getenv("MW2_VRAM_MB") ? atoi(getenv("MW2_VRAM_MB")) : 16; }
/* the stand-in's Preferences folder and Option key (the Carbon build: FindFolder, GetKeys) */
const char *mw2_mac_prefs_dir(void) { return getenv("MW2_PREFS_DIR") ? getenv("MW2_PREFS_DIR") : "./"; }
int mw2_mac_option_down(void) { return getenv("MW2_OPTION_KEY") != NULL; }
