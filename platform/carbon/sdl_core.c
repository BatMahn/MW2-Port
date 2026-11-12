/*
 * sdl_core.c - the SDL3 subset the port uses (SDL3/SDL.h here), for systems SDL3 doesn't run on: classic Mac OS /
 * Carbon (sdl_plat_carbon.c). The system-specific part is behind sdl_plat.h; this file is the rest - the event queue,
 * the keyboard (SDL scancodes and SDL3's default US keycodes), properties and hints, the audio streams and their mix,
 * the shell's 2D renderer (on OpenGL 1.2), surfaces, display modes.
 *
 * Audio: the game's stream callbacks (the effects mixer, the CD music) run on the MAIN thread - SDL_PumpEvents /
 * SDL_PollEvent / the presents call core_audio_pump, which mixes every stream into a ring of 16-bit frames; the
 * system's audio callback (on Mac OS 9 at interrupt time, where nothing may allocate or touch a file) only copies out of
 * that ring. So SDL_LockAudioStream has nothing to lock against and the game's own locks see no other thread.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include "sdl_plat.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

static char g_err[256];
static int g_inited;
static void set_error(const char *m) { snprintf(g_err, sizeof g_err, "%s", m); }
const char *SDL_GetError(void) { return g_err; }

bool SDL_Init(SDL_InitFlags flags)
{
    (void)flags;
    if (g_inited) return true;
    if (plat_init(g_err, (int)sizeof g_err) != 0) return false;
    g_inited = 1;
    return true;
}
bool SDL_InitSubSystem(SDL_InitFlags flags) { return SDL_Init(flags); }
void SDL_Quit(void) { if (g_inited) { plat_audio_close(); plat_quit(); } g_inited = 0; }
const char *SDL_GetCurrentVideoDriver(void) { return plat_name(); }
const char *SDL_GetCurrentAudioDriver(void) { return "soundmanager"; }
void SDL_Delay(Uint32 ms) { plat_delay_ms(ms); }
Uint64 SDL_GetTicks(void) { return plat_ticks_us() / 1000u; }
Uint64 SDL_GetPerformanceCounter(void) { return plat_ticks_us(); }
Uint64 SDL_GetPerformanceFrequency(void) { return 1000000u; }
const char *SDL_GetBasePath(void) { return plat_base_path(); }
bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags, const char *title, const char *message, SDL_Window *window)
{
    (void)window;
    plat_message_box((flags & 0x10) != 0, title ? title : "", message ? message : "");
    return true;
}

/* ------------------------------------------------------------------------------------------------ hints, properties */
#define MAXH 32
static char g_hk[MAXH][64], g_hv[MAXH][64];
static int g_nh;
const char *SDL_GetHint(const char *name)
{
    int k;
    for (k = 0; k < g_nh; k++) if (strcmp(g_hk[k], name) == 0) return g_hv[k];
    return getenv(name);
}
bool SDL_SetHint(const char *name, const char *value)
{
    int k;
    for (k = 0; k < g_nh && strcmp(g_hk[k], name) != 0; k++) {}
    if (k == g_nh) { if (g_nh == MAXH) return false; g_nh++; }
    snprintf(g_hk[k], sizeof g_hk[k], "%s", name);
    snprintf(g_hv[k], sizeof g_hv[k], "%s", value ? value : "");
    return true;
}

/* window-creation properties: title, x, y, w, h, flags (all the game sets) */
typedef struct { int used; char title[128]; Sint64 w, h, flags; } props;
static props g_props[8];
SDL_PropertiesID SDL_CreateProperties(void)
{
    int k;
    for (k = 0; k < 8; k++) if (!g_props[k].used) { memset(&g_props[k], 0, sizeof g_props[k]); g_props[k].used = 1; return (SDL_PropertiesID)(k + 1); }
    return 0;
}
void SDL_DestroyProperties(SDL_PropertiesID p) { if (p >= 1 && p <= 8) g_props[p - 1].used = 0; }
static props *P(SDL_PropertiesID p) { return p >= 1 && p <= 8 && g_props[p - 1].used ? &g_props[p - 1] : NULL; }
bool SDL_SetStringProperty(SDL_PropertiesID p, const char *name, const char *value)
{
    props *q = P(p);
    if (q && strcmp(name, SDL_PROP_WINDOW_CREATE_TITLE_STRING) == 0) snprintf(q->title, sizeof q->title, "%s", value ? value : "");
    return q != NULL;
}
bool SDL_SetNumberProperty(SDL_PropertiesID p, const char *name, Sint64 value)
{
    props *q = P(p);
    if (!q) return false;
    if (strcmp(name, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER) == 0) q->w = value;
    else if (strcmp(name, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER) == 0) q->h = value;
    else if (strcmp(name, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER) == 0) q->flags = value;
    return true;
}
bool SDL_SetBooleanProperty(SDL_PropertiesID p, const char *name, bool value) { (void)name; (void)value; return P(p) != NULL; }

/* ------------------------------------------------------------------------------------------------------- windows */
struct SDL_Window { plat_window *pw; SDL_WindowFlags flags; int fw, fh; int relative, text; };
static SDL_Window *g_focus;

SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags)
{
    SDL_Window *win;
    if (!g_inited && !SDL_Init(0)) return NULL;
    win = calloc(1, sizeof *win);
    if (!win) return NULL;
    win->flags = flags;
    win->pw = plat_window_create(title ? title : "", w, h, (flags & SDL_WINDOW_FULLSCREEN) != 0, (flags & SDL_WINDOW_HIDDEN) != 0);
    if (!win->pw) { free(win); set_error("can't create the window"); return NULL; }
    g_focus = win;
    return win;
}
SDL_Window *SDL_CreateWindowWithProperties(SDL_PropertiesID p)
{
    props *q = P(p);
    return q ? SDL_CreateWindow(q->title, (int)q->w, (int)q->h, (SDL_WindowFlags)q->flags) : NULL;
}
void SDL_DestroyWindow(SDL_Window *w) { if (!w) return; plat_window_destroy(w->pw); if (g_focus == w) g_focus = NULL; free(w); }
bool SDL_SetWindowTitle(SDL_Window *w, const char *t) { if (w) plat_window_title(w->pw, t); return w != NULL; }
bool SDL_GetWindowSizeInPixels(SDL_Window *w, int *pw, int *ph) { int a = 0, b = 0; if (w) plat_window_size(w->pw, &a, &b); if (pw) *pw = a; if (ph) *ph = b; return w != NULL; }
bool SDL_SetWindowSize(SDL_Window *w, int pw, int ph) { return w && plat_window_set_size(w->pw, pw, ph) == 0; }
bool SDL_ShowWindow(SDL_Window *w) { if (w) { plat_window_show(w->pw, 1); w->flags &= ~(SDL_WindowFlags)SDL_WINDOW_HIDDEN; } return w != NULL; }
bool SDL_HideWindow(SDL_Window *w) { if (w) { plat_window_show(w->pw, 0); w->flags |= SDL_WINDOW_HIDDEN; } return w != NULL; }
bool SDL_RaiseWindow(SDL_Window *w) { if (w) { plat_window_show(w->pw, 1); g_focus = w; } return w != NULL; }
bool SDL_SyncWindow(SDL_Window *w) { (void)w; return true; }
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *w)
{
    SDL_WindowFlags f;
    if (!w) return 0;
    f = w->flags;
    if (plat_window_focused(w->pw)) f |= SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS;
    return f;
}
bool SDL_SetWindowFullscreenMode(SDL_Window *w, const SDL_DisplayMode *m) { if (w && m) { w->fw = m->w; w->fh = m->h; } return w != NULL; }
bool SDL_SetWindowFullscreen(SDL_Window *w, bool on)
{
    if (!w) return false;
    if (plat_window_fullscreen(w->pw, on, w->fw, w->fh) != 0) return false;
    if (on) w->flags |= SDL_WINDOW_FULLSCREEN; else w->flags &= ~(SDL_WindowFlags)SDL_WINDOW_FULLSCREEN;
    return true;
}
bool SDL_SetWindowRelativeMouseMode(SDL_Window *w, bool on) { if (w) { w->relative = on; plat_relative_mouse(w->pw, on); } return w != NULL; }
bool SDL_StartTextInput(SDL_Window *w) { if (w) w->text = 1; return w != NULL; }
bool SDL_StopTextInput(SDL_Window *w) { if (w) w->text = 0; return w != NULL; }
bool SDL_TextInputActive(SDL_Window *w) { return w && w->text; }

/* displays: one */
static SDL_DisplayMode g_modes[32], *g_modep[33], g_desk;
SDL_DisplayID SDL_GetPrimaryDisplay(void) { return 1; }
SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *w) { (void)w; return 1; }
const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID id)
{
    (void)id;
    memset(&g_desk, 0, sizeof g_desk);
    plat_desktop_mode(&g_desk.w, &g_desk.h);
    g_desk.displayID = 1; g_desk.pixel_density = 1; g_desk.refresh_rate = 60;
    return &g_desk;
}
SDL_DisplayMode **SDL_GetFullscreenDisplayModes(SDL_DisplayID id, int *count)
{
    int wh[32][2], n, k;
    SDL_DisplayMode **out;
    (void)id;
    n = plat_display_modes(wh, 32);
    for (k = 0; k < n; k++) { memset(&g_modes[k], 0, sizeof g_modes[k]); g_modes[k].displayID = 1; g_modes[k].w = wh[k][0]; g_modes[k].h = wh[k][1]; g_modes[k].pixel_density = 1; g_modes[k].refresh_rate = 60; }
    out = malloc(sizeof *out * (size_t)(n + 1));   /* (the caller SDL_free()s it, as SDL's) */
    if (!out) return NULL;
    for (k = 0; k < n; k++) out[k] = &g_modes[k];
    out[n] = NULL;
    if (count) *count = n;
    (void)g_modep;
    return out;
}
bool SDL_GetClosestFullscreenDisplayMode(SDL_DisplayID id, int w, int h, float rr, bool hd, SDL_DisplayMode *closest)
{
    int n = 0, k, best = -1;
    long bd = 0;
    SDL_DisplayMode **m = SDL_GetFullscreenDisplayModes(id, &n);
    (void)rr; (void)hd;
    for (k = 0; k < n; k++) {   /* the smallest mode at least as large, else the largest */
        long d = (long)m[k]->w * m[k]->h;
        if (m[k]->w >= w && m[k]->h >= h && (best < 0 || d < bd)) { best = k; bd = d; }
    }
    if (best < 0 && n > 0) best = 0;
    if (best >= 0 && closest) *closest = *m[best];
    free(m);
    return best >= 0;
}

/* ----------------------------------------------------------------------------------------------------- OpenGL */
static int g_gl_swap = 1;
static void gl_load(void);
bool SDL_GL_SetAttribute(SDL_GLAttr attr, int value) { (void)attr; (void)value; return true; }
/* the GL functions found once a context exists (gl_dyn.h) */
static void gl_load(void)
{
#if defined(MW2_CLASSIC_MAC) || defined(MW2_GL_DYN)
    static int done;
    if (!done && gl_dyn_load(plat_gl_proc) != 0) plat_message_box(1, "OpenGL", "OpenGL 1.2 functions are missing (is OpenGL installed?)");
    done = 1;
#endif
}
SDL_GLContext SDL_GL_CreateContext(SDL_Window *w) { void *c = w ? plat_gl_create(w->pw) : NULL; if (c) gl_load(); return c; }
bool SDL_GL_MakeCurrent(SDL_Window *w, SDL_GLContext c) { return w && plat_gl_make_current(w->pw, c) == 0; }
bool SDL_GL_DestroyContext(SDL_GLContext c) { plat_gl_destroy(c); return true; }
SDL_FunctionPointer SDL_GL_GetProcAddress(const char *p) { return (SDL_FunctionPointer)plat_gl_proc(p); }
bool SDL_GL_SetSwapInterval(int n) { g_gl_swap = n; plat_gl_swap_interval(n); return true; }
void core_audio_pump(void);
bool SDL_GL_SwapWindow(SDL_Window *w) { if (!w) return false; plat_gl_swap(w->pw); core_audio_pump(); return true; }

/* ------------------------------------------------------------------------------------------------------ events */
#define QN 256
static SDL_Event g_q[QN];
static int g_qh, g_qt;
static bool g_keys[SDL_SCANCODE_COUNT];
static SDL_Keymod g_mod;
static float g_mx, g_my;
static uint32_t g_mbuttons;
static char g_text[8][16];
static int g_textn;

static void push(const SDL_Event *e)
{
    int n = (g_qt + 1) % QN;
    if (n == g_qh) return;   /* full: dropped */
    g_q[g_qt] = *e; g_qt = n;
}
bool SDL_PushEvent(SDL_Event *e) { push(e); return true; }
void SDL_PumpEvents(void) { plat_pump(); core_audio_pump(); }
bool SDL_PollEvent(SDL_Event *e)
{
    if (g_qh == g_qt) SDL_PumpEvents();
    if (g_qh == g_qt) return false;
    if (e) *e = g_q[g_qh];
    g_qh = (g_qh + 1) % QN;
    return true;
}
const bool *SDL_GetKeyboardState(int *n) { if (n) *n = SDL_SCANCODE_COUNT; return g_keys; }
SDL_Keymod SDL_GetModState(void) { return g_mod; }
SDL_MouseButtonFlags SDL_GetMouseState(float *x, float *y)
{
    if (x) *x = g_mx;
    if (y) *y = g_my;
    return g_mbuttons;
}

/* SDL3's default (US) keycode for a scancode, unshifted (SDL_keymap.c SDL_GetDefaultKeyFromScancode) */
static SDL_Keycode default_key(int sc)
{
    static const SDL_Keycode sym[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\r', 0x1b, '\b', '\t', ' ', '-', '=', '[', ']', '\\', '#', ';', '\'', '`', ',', '.', '/'};
    if (sc < SDL_SCANCODE_A) return 0;
    if (sc < SDL_SCANCODE_1) return (SDL_Keycode)('a' + sc - SDL_SCANCODE_A);
    if (sc < SDL_SCANCODE_CAPSLOCK) return sym[sc - SDL_SCANCODE_1];
    if (sc == SDL_SCANCODE_DELETE) return 0x7f;
    return (SDL_Keycode)sc | 0x40000000u;
}
SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key, SDL_Keymod *mod)
{
    int sc;
    if (mod) *mod = 0;
    for (sc = 0; sc < SDL_SCANCODE_COUNT; sc++) if (default_key(sc) == key && key) return sc;
    return 0;
}

/* Mac virtual key codes -> SDL scancodes (SDL's src/events/scancodes_darwin.h) */
#include "scancodes_mac.h"

static SDL_Keymod mod_of(int sc)
{
    switch (sc) {
    case SDL_SCANCODE_LSHIFT: return SDL_KMOD_LSHIFT; case SDL_SCANCODE_RSHIFT: return SDL_KMOD_RSHIFT;
    case SDL_SCANCODE_LCTRL: return SDL_KMOD_LCTRL; case SDL_SCANCODE_RCTRL: return SDL_KMOD_RCTRL;
    case SDL_SCANCODE_LALT: return SDL_KMOD_LALT; case SDL_SCANCODE_RALT: return SDL_KMOD_RALT;
    case SDL_SCANCODE_LGUI: return SDL_KMOD_LGUI; case SDL_SCANCODE_RGUI: return SDL_KMOD_RGUI;
    default: return 0;
    }
}
void core_key(int code, int is_scancode, int down)
{
    SDL_Event e;
    int sc = is_scancode ? code : (code >= 0 && code < (int)(sizeof mac_scancode / sizeof mac_scancode[0]) ? mac_scancode[code] : 0);
    if (sc <= 0 || sc >= SDL_SCANCODE_COUNT) return;
    if (!g_keys[sc] && !down) return;
    memset(&e, 0, sizeof e);
    e.key.repeat = g_keys[sc] && down;
    g_keys[sc] = down != 0;
    if (down) g_mod |= mod_of(sc); else g_mod &= (SDL_Keymod)~mod_of(sc);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.timestamp = plat_ticks_us() * 1000u;
    e.key.windowID = 1;
    e.key.scancode = sc;
    e.key.key = default_key(sc);
    e.key.mod = g_mod;
    e.key.down = down != 0;
    push(&e);
}
void core_text(const char *utf8)
{
    SDL_Event e;
    if (!g_focus || !g_focus->text || !utf8 || !*utf8) return;
    snprintf(g_text[g_textn], sizeof g_text[0], "%s", utf8);   /* (valid until 8 more text events) */
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.windowID = 1;
    e.text.text = g_text[g_textn];
    g_textn = (g_textn + 1) & 7;
    push(&e);
}
void core_mouse_move(float x, float y, float dx, float dy)
{
    SDL_Event e;
    g_mx = x; g_my = y;
    if (dx == 0 && dy == 0) return;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = 1; e.motion.state = g_mbuttons;
    e.motion.x = x; e.motion.y = y; e.motion.xrel = dx; e.motion.yrel = dy;
    push(&e);
}
void core_mouse_button(int button, int down, float x, float y)
{
    SDL_Event e;
    if (down) g_mbuttons |= SDL_BUTTON_MASK(button); else g_mbuttons &= ~SDL_BUTTON_MASK(button);
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = 1; e.button.button = (Uint8)button; e.button.down = down != 0; e.button.clicks = 1;
    e.button.x = x; e.button.y = y;
    push(&e);
}
void core_mouse_wheel(float dy)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.windowID = 1; e.wheel.y = dy; e.wheel.mouse_x = g_mx; e.wheel.mouse_y = g_my; e.wheel.integer_y = (Sint32)dy;
    push(&e);
}
void core_quit_request(void) { SDL_Event e; memset(&e, 0, sizeof e); e.type = SDL_EVENT_QUIT; push(&e); }

/* joysticks: none yet (InputSprocket is a later step) */
SDL_JoystickID *SDL_GetJoysticks(int *count) { SDL_JoystickID *l = malloc(sizeof *l); if (l) *l = 0; if (count) *count = 0; return l; }
SDL_Joystick *SDL_OpenJoystick(SDL_JoystickID id) { (void)id; set_error("no joysticks"); return NULL; }
void SDL_CloseJoystick(SDL_Joystick *j) { (void)j; }
Sint16 SDL_GetJoystickAxis(SDL_Joystick *j, int a) { (void)j; (void)a; return 0; }
bool SDL_GetJoystickButton(SDL_Joystick *j, int b) { (void)j; (void)b; return false; }
Uint8 SDL_GetJoystickHat(SDL_Joystick *j, int h) { (void)j; (void)h; return 0; }
int SDL_GetNumJoystickAxes(SDL_Joystick *j) { (void)j; return 0; }
int SDL_GetNumJoystickButtons(SDL_Joystick *j) { (void)j; return 0; }
int SDL_GetNumJoystickHats(SDL_Joystick *j) { (void)j; return 0; }

/* ------------------------------------------------------------------------------------------------------- audio */
/* each stream: float frames (its spec: the game opens 44.1 kHz stereo float), resampled to the device's 44.1 kHz by
 * nearest-sample stepping if a stream's rate differs (none does today) */
#define DEV_RATE 44100
#define RING (8192)                     /* device frames buffered: ~186 ms */
struct SDL_AudioStream { SDL_AudioSpec spec; SDL_AudioStreamCallback cb; void *ud; int running; float *q; int qn, qcap; };
static SDL_AudioStream *g_streams[4];
static int16_t g_ring[RING * 2];
static volatile unsigned g_rd, g_wr;    /* frames read (the system's callback) / written (the pump); RING divides 2^32 */
static int g_audio_open;

static void fill_cb(int16_t *out, int frames)
{
    int k;
    for (k = 0; k < frames; k++) {
        if (g_rd == g_wr) { out[k * 2] = out[k * 2 + 1] = 0; continue; }   /* underrun: silence */
        out[k * 2] = g_ring[(g_rd % RING) * 2]; out[k * 2 + 1] = g_ring[(g_rd % RING) * 2 + 1];
        g_rd++;
    }
}

SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID dev, const SDL_AudioSpec *spec, SDL_AudioStreamCallback cb, void *ud)
{
    SDL_AudioStream *s;
    int k;
    (void)dev;
    if (!spec || spec->format != SDL_AUDIO_F32 || spec->channels != 2) { set_error("unsupported audio format"); return NULL; }
    for (k = 0; k < 4 && g_streams[k]; k++) {}
    if (k == 4) { set_error("too many audio streams"); return NULL; }
    if (!g_audio_open) {
        const char *h = SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
        if (plat_audio_open(DEV_RATE, h && atoi(h) > 0 ? atoi(h) : 1024, fill_cb) != 0) { set_error("no audio device"); return NULL; }
        g_audio_open = 1;
    }
    s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->spec = *spec; s->cb = cb; s->ud = ud;
    g_streams[k] = s;
    return s;
}
void SDL_DestroyAudioStream(SDL_AudioStream *s)
{
    int k, any = 0;
    if (!s) return;
    for (k = 0; k < 4; k++) { if (g_streams[k] == s) g_streams[k] = NULL; any |= g_streams[k] != NULL; }
    free(s->q); free(s);
    if (!any && g_audio_open) { plat_audio_close(); g_audio_open = 0; }
}
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *s) { if (s) s->running = 1; return s != NULL; }
bool SDL_LockAudioStream(SDL_AudioStream *s) { (void)s; return true; }
bool SDL_UnlockAudioStream(SDL_AudioStream *s) { (void)s; return true; }
bool SDL_PutAudioStreamData(SDL_AudioStream *s, const void *buf, int len)
{
    int n = len / (int)sizeof(float);
    if (!s || n <= 0) return s != NULL;
    if (s->qn + n > s->qcap) {
        int c = s->qcap ? s->qcap : 4096;
        float *q;
        while (c < s->qn + n) c *= 2;
        q = realloc(s->q, (size_t)c * sizeof(float));
        if (!q) return false;
        s->q = q; s->qcap = c;
    }
    memcpy(s->q + s->qn, buf, (size_t)n * sizeof(float));
    s->qn += n;
    return true;
}
/* the ring topped up from every running stream (main thread): each stream's callback asked for what it lacks */
void core_audio_pump(void)
{
    int space, frames, k, f;
    static float mixbuf[1024 * 2];
    if (!g_audio_open) return;
    for (;;) {
        space = RING - (int)(g_wr - g_rd) - 1;
        if (space < 256) break;
        frames = space > 1024 ? 1024 : space;
        memset(mixbuf, 0, sizeof mixbuf);
        for (k = 0; k < 4; k++) {
            SDL_AudioStream *s = g_streams[k];
            int need;
            if (!s || !s->running) continue;
            need = frames * 2 - s->qn;
            if (need > 0 && s->cb) s->cb(s->ud, s, need * (int)sizeof(float), need * (int)sizeof(float));
            for (f = 0; f < frames * 2 && f < s->qn; f++) mixbuf[f] += s->q[f];
            f = frames * 2 < s->qn ? frames * 2 : s->qn;
            memmove(s->q, s->q + f, (size_t)(s->qn - f) * sizeof(float));
            s->qn -= f;
        }
        for (f = 0; f < frames; f++) {
            float l = mixbuf[f * 2] * 32767.0f, r = mixbuf[f * 2 + 1] * 32767.0f;
            g_ring[((g_wr + (unsigned)f) % RING) * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            g_ring[((g_wr + (unsigned)f) % RING) * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        g_wr += (unsigned)frames;
    }
}

/* ---------------------------------------------------------------------------------- the 2D renderer (OpenGL 1.2) */
/* The shell's use: textures (ABGR8888 - bytes R, G, B, A as the game writes them -, RGB24) drawn as rectangles in the
 * logical resolution (letterboxed / integer-scaled into the window), clear, present, read back. */
struct SDL_Texture { SDL_Renderer *rd; GLuint id; int w, h, W, H, fmt, linear, blend; void *lockbuf; };
struct SDL_Renderer { SDL_Window *win; void *ctx; int lw, lh, mode; int vp[4]; float sx, sy; int clip_on; SDL_Rect clip; };

static int p2(int v) { int p = 1; while (p < v) p <<= 1; return p; }

SDL_Renderer *SDL_CreateRenderer(SDL_Window *win, const char *name)
{
    SDL_Renderer *r;
    (void)name;
    if (!win) return NULL;
    r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->win = win;
    r->ctx = plat_gl_create(win->pw);
    if (!r->ctx) { free(r); set_error("no OpenGL context for the renderer"); return NULL; }
    plat_gl_make_current(win->pw, r->ctx);
    gl_load();
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_LIGHTING);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ALIGNMENT, 1);
    return r;
}
void SDL_DestroyRenderer(SDL_Renderer *r) { if (!r) return; plat_gl_destroy(r->ctx); free(r); }
const char *SDL_GetRendererName(SDL_Renderer *r) { (void)r; return "opengl"; }
bool SDL_SetRenderVSync(SDL_Renderer *r, int v) { (void)r; plat_gl_swap_interval(v); return true; }
bool SDL_GetRenderOutputSize(SDL_Renderer *r, int *w, int *h) { return r && SDL_GetWindowSizeInPixels(r->win, w, h); }
bool SDL_SetRenderLogicalPresentation(SDL_Renderer *r, int w, int h, SDL_RendererLogicalPresentation mode) { if (r) { r->lw = w; r->lh = h; r->mode = mode; } return r != NULL; }
bool SDL_GetRenderLogicalPresentation(SDL_Renderer *r, int *w, int *h, SDL_RendererLogicalPresentation *mode)
{
    if (!r) return false;
    if (w) *w = r->lw;
    if (h) *h = r->lh;
    if (mode) *mode = r->mode;
    return true;
}
/* the logical area in the window (SDL3's SDL_RenderLogicalPresentation rules) */
static void layout(SDL_Renderer *r)
{
    int ow = 0, oh = 0;
    SDL_GetWindowSizeInPixels(r->win, &ow, &oh);
    if (r->mode == SDL_LOGICAL_PRESENTATION_DISABLED || r->lw <= 0 || r->lh <= 0) { r->vp[0] = 0; r->vp[1] = 0; r->vp[2] = ow; r->vp[3] = oh; r->sx = r->sy = 1; return; }
    if (r->mode == SDL_LOGICAL_PRESENTATION_STRETCH) { r->sx = (float)ow / (float)r->lw; r->sy = (float)oh / (float)r->lh; }
    else {
        float s = fminf((float)ow / (float)r->lw, (float)oh / (float)r->lh);
        if (r->mode == SDL_LOGICAL_PRESENTATION_INTEGER_SCALE) { s = floorf(s); if (s < 1) s = 1; }
        r->sx = r->sy = s;
    }
    r->vp[2] = (int)floorf((float)r->lw * r->sx + 0.5f); r->vp[3] = (int)floorf((float)r->lh * r->sy + 0.5f);
    r->vp[0] = (ow - r->vp[2]) / 2; r->vp[1] = (oh - r->vp[3]) / 2;
}
bool SDL_RenderCoordinatesFromWindow(SDL_Renderer *r, float wx, float wy, float *x, float *y)
{
    if (!r) return false;
    layout(r);
    if (x) *x = (wx - (float)r->vp[0]) / r->sx;
    if (y) *y = (wy - (float)r->vp[1]) / r->sy;
    return true;
}
bool SDL_ConvertEventToRenderCoordinates(SDL_Renderer *r, SDL_Event *e)
{
    if (!r || !e) return false;
    if (e->type == SDL_EVENT_MOUSE_MOTION) {
        SDL_RenderCoordinatesFromWindow(r, e->motion.x, e->motion.y, &e->motion.x, &e->motion.y);
        e->motion.xrel /= r->sx; e->motion.yrel /= r->sy;
    } else if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP)
        SDL_RenderCoordinatesFromWindow(r, e->button.x, e->button.y, &e->button.x, &e->button.y);
    else if (e->type == SDL_EVENT_MOUSE_WHEEL)
        SDL_RenderCoordinatesFromWindow(r, e->wheel.mouse_x, e->wheel.mouse_y, &e->wheel.mouse_x, &e->wheel.mouse_y);
    return true;
}

SDL_Texture *SDL_CreateTexture(SDL_Renderer *r, SDL_PixelFormat fmt, SDL_TextureAccess access, int w, int h)
{
    SDL_Texture *t;
    (void)access;
    if (!r || w <= 0 || h <= 0) return NULL;
    t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->rd = r; t->w = w; t->h = h; t->W = p2(w); t->H = p2(h); t->fmt = (int)fmt;
    t->linear = 1;                                  /* SDL3's default scale mode */
    t->blend = fmt == SDL_PIXELFORMAT_ABGR8888;     /* SDL3: blended when the format has alpha */
    plat_gl_make_current(r->win->pw, r->ctx);
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->W, t->H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}
void SDL_DestroyTexture(SDL_Texture *t) { if (!t) return; glDeleteTextures(1, &t->id); free(t->lockbuf); free(t); }

/* a streaming texture's pixels: a CPU copy, given to GL on unlock (the whole texture; rect is not used by the game) */
bool SDL_LockTexture(SDL_Texture *t, const SDL_Rect *rect, void **pixels, int *pitch)
{
    (void)rect;
    if (!t || !pixels || !pitch) return false;
    if (!t->lockbuf && !(t->lockbuf = calloc((size_t)t->w * (size_t)t->h, 4))) return false;
    *pixels = t->lockbuf; *pitch = t->w * 4;
    return true;
}
void SDL_UnlockTexture(SDL_Texture *t) { if (t && t->lockbuf) SDL_UpdateTexture(t, NULL, t->lockbuf, t->w * 4); }
bool SDL_GetTextureSize(SDL_Texture *t, float *w, float *h) { if (!t) return false; if (w) *w = (float)t->w; if (h) *h = (float)t->h; return true; }

/* the clip rectangle (logical coordinates), as GL's scissor box in the window's pixels when drawing */
bool SDL_SetRenderClipRect(SDL_Renderer *r, const SDL_Rect *rect)
{
    if (!r) return false;
    r->clip_on = rect != NULL;
    if (rect) r->clip = *rect;
    return true;
}

/* threads: one CPU - a "thread" runs to its end when made; waiting gives its result */
struct SDL_Thread { int status; };
SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data)
{
    SDL_Thread *t = malloc(sizeof *t);
    (void)name;
    if (!t) return NULL;
    t->status = fn ? fn(data) : 0;
    return t;
}
void SDL_WaitThread(SDL_Thread *t, int *status) { if (!t) return; if (status) *status = t->status; free(t); }
int SDL_GetNumLogicalCPUCores(void) { return 1; }
bool SDL_SetTextureBlendMode(SDL_Texture *t, SDL_BlendMode m) { if (t) t->blend = m == SDL_BLENDMODE_BLEND; return t != NULL; }
bool SDL_SetTextureScaleMode(SDL_Texture *t, SDL_ScaleMode m) { if (t) t->linear = m == SDL_SCALEMODE_LINEAR; return t != NULL; }
bool SDL_UpdateTexture(SDL_Texture *t, const SDL_Rect *rect, const void *pixels, int pitch)
{
    int x = 0, y = 0, w, h, k, bpp;
    const uint8_t *src = pixels;
    uint8_t *buf;
    if (!t || !pixels) return false;
    w = rect ? rect->w : t->w; h = rect ? rect->h : t->h;
    if (rect) { x = rect->x; y = rect->y; }
    bpp = t->fmt == SDL_PIXELFORMAT_RGB24 ? 3 : 4;
    /* rows with the last texel repeated one past the right and bottom edges (a padded texture's bilinear edge) */
    buf = malloc((size_t)(w + 1) * (size_t)(h + 1) * 4);
    if (!buf) return false;
    for (k = 0; k < h + 1; k++) {
        const uint8_t *s = src + (size_t)(k < h ? k : h - 1) * (size_t)pitch;
        uint8_t *d = buf + (size_t)k * (size_t)(w + 1) * 4;
        int i;
        for (i = 0; i < w + 1; i++) {
            const uint8_t *p = s + (size_t)(i < w ? i : w - 1) * (size_t)bpp;
            d[i * 4] = p[0]; d[i * 4 + 1] = p[1]; d[i * 4 + 2] = p[2]; d[i * 4 + 3] = bpp == 4 ? p[3] : 255;
        }
    }
    plat_gl_make_current(t->rd->win->pw, t->rd->ctx);
    glBindTexture(GL_TEXTURE_2D, t->id);
    {
        int ew = x + w + 1 <= t->W ? w + 1 : w, eh = y + h + 1 <= t->H ? h + 1 : h;
        glPixelStorei(GL_UNPACK_ROW_LENGTH, w + 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, ew, eh, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    free(buf);
    return true;
}
static void begin2d(SDL_Renderer *r)
{
    int ow = 0, oh = 0;
    plat_gl_make_current(r->win->pw, r->ctx);
    layout(r);
    SDL_GetWindowSizeInPixels(r->win, &ow, &oh);
    /* GL's viewport is bottom-up: the logical area's rows top first */
    glViewport(r->vp[0], oh - r->vp[1] - r->vp[3], r->vp[2], r->vp[3]);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, r->lw > 0 && r->mode ? r->lw : ow, r->lh > 0 && r->mode ? r->lh : oh, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    if (r->clip_on) {   /* logical -> the viewport's pixels, GL's rows bottom-up */
        float kx = r->lw > 0 && r->mode ? (float)r->vp[2] / (float)r->lw : 1.0f, ky = r->lh > 0 && r->mode ? (float)r->vp[3] / (float)r->lh : 1.0f;
        int x = r->vp[0] + (int)(r->clip.x * kx + 0.5f), y = r->vp[1] + (int)(r->clip.y * ky + 0.5f);
        int w = (int)(r->clip.w * kx + 0.5f), h = (int)(r->clip.h * ky + 0.5f);
        glEnable(GL_SCISSOR_TEST);
        glScissor(x, oh - y - h, w < 0 ? 0 : w, h < 0 ? 0 : h);
    } else glDisable(GL_SCISSOR_TEST);
}
bool SDL_RenderClear(SDL_Renderer *r)
{
    if (!r) return false;
    plat_gl_make_current(r->win->pw, r->ctx);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    return true;
}
bool SDL_RenderTexture(SDL_Renderer *r, SDL_Texture *t, const SDL_FRect *src, const SDL_FRect *dst)
{
    float s0, t0, s1, t1, x0, y0, x1, y1;
    if (!r || !t) return false;
    begin2d(r);
    s0 = src ? src->x : 0; t0 = src ? src->y : 0; s1 = src ? src->x + src->w : (float)t->w; t1 = src ? src->y + src->h : (float)t->h;
    s0 /= (float)t->W; s1 /= (float)t->W; t0 /= (float)t->H; t1 /= (float)t->H;
    if (dst) { x0 = dst->x; y0 = dst->y; x1 = dst->x + dst->w; y1 = dst->y + dst->h; }
    else { x0 = 0; y0 = 0; x1 = (float)(r->mode ? r->lw : r->vp[2]); y1 = (float)(r->mode ? r->lh : r->vp[3]); }
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, t->linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, t->linear ? GL_LINEAR : GL_NEAREST);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    if (t->blend) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); } else glDisable(GL_BLEND);
    glBegin(GL_QUADS);
    glTexCoord2f(s0, t0); glVertex2f(x0, y0);
    glTexCoord2f(s1, t0); glVertex2f(x1, y0);
    glTexCoord2f(s1, t1); glVertex2f(x1, y1);
    glTexCoord2f(s0, t1); glVertex2f(x0, y1);
    glEnd();
    glDisable(GL_TEXTURE_2D); glDisable(GL_BLEND);
    return true;
}
bool SDL_RenderPresent(SDL_Renderer *r) { if (!r) return false; plat_gl_swap(r->win->pw); core_audio_pump(); return true; }
SDL_Surface *SDL_RenderReadPixels(SDL_Renderer *r, const SDL_Rect *rect)
{
    int ow = 0, oh = 0, k;
    SDL_Surface *s;
    uint8_t *tmp;
    (void)rect;
    if (!r) return NULL;
    SDL_GetWindowSizeInPixels(r->win, &ow, &oh);
    s = calloc(1, sizeof *s);
    tmp = malloc((size_t)ow * (size_t)oh * 4);
    if (!s || !tmp) { free(s); free(tmp); return NULL; }
    s->pixels = malloc((size_t)ow * (size_t)oh * 4);
    if (!s->pixels) { free(s); free(tmp); return NULL; }
    plat_gl_make_current(r->win->pw, r->ctx);
    glReadPixels(0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    for (k = 0; k < oh; k++) memcpy((uint8_t *)s->pixels + (size_t)k * (size_t)ow * 4, tmp + (size_t)(oh - 1 - k) * (size_t)ow * 4, (size_t)ow * 4);
    free(tmp);
    s->w = ow; s->h = oh; s->pitch = ow * 4; s->format = SDL_PIXELFORMAT_ABGR8888;
    return s;
}
SDL_Surface *SDL_ConvertSurface(SDL_Surface *src, SDL_PixelFormat fmt)
{
    SDL_Surface *s;
    int k;
    if (!src || fmt != SDL_PIXELFORMAT_RGB24) return NULL;
    s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->pixels = malloc((size_t)src->w * (size_t)src->h * 3);
    if (!s->pixels) { free(s); return NULL; }
    s->w = src->w; s->h = src->h; s->pitch = src->w * 3; s->format = fmt;
    for (k = 0; k < src->w * src->h; k++) memcpy((uint8_t *)s->pixels + k * 3, (uint8_t *)src->pixels + k * 4, 3);
    return s;
}
void SDL_DestroySurface(SDL_Surface *s) { if (s) { free(s->pixels); free(s); } }
