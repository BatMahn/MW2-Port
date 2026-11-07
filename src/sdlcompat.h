/*
 * sdlcompat.h - the SDL the port is built against. Normally SDL3: this header is then just <SDL3/SDL.h>, and nothing
 * below applies. The legacy build (make ... SDL=2, which defines MW2_SDL2) uses SDL2 - SDL3 does not run on Windows XP,
 * SDL2 does - and this header maps the SDL3 calls, names and event fields the port uses onto SDL2 (2.0.18 or newer),
 * mechanically: renamed calls as macros, changed signatures as small wrappers (mw2c_*), SDL_Event as a union with
 * SDL3's field names filled from SDL2's events, window creation properties as a small table, the audio device stream
 * as SDL2's callback device, joysticks by index. Only what the port uses is mapped.
 */
#ifndef MW2_SDLCOMPAT_H
#define MW2_SDLCOMPAT_H

#ifndef MW2_SDL2
#include <SDL3/SDL.h>
#else

#include <SDL.h>
#include <stdbool.h>
#include <string.h>

#if !SDL_VERSION_ATLEAST(2, 0, 18)
#error "the SDL=2 build needs SDL2 2.0.18 or newer"
#endif

#ifdef __GNUC__
#define MW2C_UNUSED __attribute__((unused))
#else
#define MW2C_UNUSED
#endif
#define MW2C static inline MW2C_UNUSED

/* ---- init, time, misc */
MW2C bool mw2c_Init(Uint32 f) { return SDL_Init(f) == 0; }
MW2C bool mw2c_InitSubSystem(Uint32 f) { return SDL_InitSubSystem(f) == 0; }
#define SDL_Init(f) mw2c_Init(f)
#define SDL_InitSubSystem(f) mw2c_InitSubSystem(f)
#define SDL_GetTicks SDL_GetTicks64
#define SDL_GetNumLogicalCPUCores SDL_GetCPUCount
MW2C const char *mw2c_GetBasePath(void)
{
    static char *bp;   /* SDL3: SDL's own string; SDL2: the caller frees it - kept here */
    if (!bp) bp = (SDL_GetBasePath)();
    return bp;
}
#define SDL_GetBasePath() mw2c_GetBasePath()
#ifndef SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES
#define SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES "SDL_AUDIO_DEVICE_SAMPLE_FRAMES"
#endif
#define SDL_GL_DestroyContext SDL_GL_DeleteContext

/* ---- keys and the mouse */
#define SDL_KMOD_SHIFT KMOD_SHIFT
#define SDL_KMOD_LSHIFT KMOD_LSHIFT
#define SDL_KMOD_CTRL KMOD_CTRL
#define SDL_KMOD_LCTRL KMOD_LCTRL
#define SDL_KMOD_ALT KMOD_ALT
#define SDL_KMOD_LALT KMOD_LALT
#define SDL_KMOD_GUI KMOD_GUI
#define SDLK_A SDLK_a
#define SDLK_B SDLK_b
#define SDLK_C SDLK_c
#define SDLK_D SDLK_d
#define SDLK_E SDLK_e
#define SDLK_F SDLK_f
#define SDLK_G SDLK_g
#define SDLK_H SDLK_h
#define SDLK_I SDLK_i
#define SDLK_J SDLK_j
#define SDLK_K SDLK_k
#define SDLK_L SDLK_l
#define SDLK_M SDLK_m
#define SDLK_N SDLK_n
#define SDLK_O SDLK_o
#define SDLK_P SDLK_p
#define SDLK_Q SDLK_q
#define SDLK_R SDLK_r
#define SDLK_S SDLK_s
#define SDLK_T SDLK_t
#define SDLK_U SDLK_u
#define SDLK_V SDLK_v
#define SDLK_W SDLK_w
#define SDLK_X SDLK_x
#define SDLK_Y SDLK_y
#define SDLK_Z SDLK_z
#define SDLK_APOSTROPHE SDLK_QUOTE
#define SDLK_GRAVE SDLK_BACKQUOTE
#define SDL_GetScancodeFromKey(k, modstate) (SDL_GetScancodeFromKey)(k)
#define SDL_BUTTON_MASK(b) SDL_BUTTON(b)
typedef Uint32 SDL_MouseButtonFlags;
MW2C Uint32 mw2c_GetMouseState(float *x, float *y)
{
    int ix = 0, iy = 0;
    Uint32 b = (SDL_GetMouseState)(&ix, &iy);
    if (x) *x = (float)ix;
    if (y) *y = (float)iy;
    return b;
}
#define SDL_GetMouseState(x, y) mw2c_GetMouseState(x, y)
#define SDL_GetKeyboardState(n) ((const bool *)(SDL_GetKeyboardState)(n))   /* (Uint8 0 / 1: bool's size and values) */
#define SDL_SCANCODE_COUNT SDL_NUM_SCANCODES
#define SDL_StartTextInput(w) (SDL_StartTextInput)()
#define SDL_StopTextInput(w) (SDL_StopTextInput)()
#define SDL_TextInputActive(w) ((SDL_IsTextInputActive)() == SDL_TRUE)
MW2C bool SDL_SetWindowRelativeMouseMode(SDL_Window *w, bool on) { (void)w; return SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE) == 0; }
#define SDL_SetWindowKeyboardGrab(w, on) SDL_SetWindowKeyboardGrab(w, (on) ? SDL_TRUE : SDL_FALSE)

/* ---- events: SDL3's field names (type; key.key / scancode / mod / down; button.button / x / y; motion.x / y / xrel /
 * yrel; wheel.y; text.text), filled from SDL2's; events pushed by the port go through a queue of their own */
#define SDL_EVENT_QUIT SDL_QUIT
#define SDL_EVENT_KEY_DOWN SDL_KEYDOWN
#define SDL_EVENT_KEY_UP SDL_KEYUP
#define SDL_EVENT_TEXT_INPUT SDL_TEXTINPUT
#define SDL_EVENT_MOUSE_MOTION SDL_MOUSEMOTION
#define SDL_EVENT_MOUSE_BUTTON_DOWN SDL_MOUSEBUTTONDOWN
#define SDL_EVENT_MOUSE_BUTTON_UP SDL_MOUSEBUTTONUP
#define SDL_EVENT_MOUSE_WHEEL SDL_MOUSEWHEEL
typedef SDL_Event mw2c_Event2;   /* SDL2's own */
typedef union {
    Uint32 type;
    struct { Uint32 type; SDL_Keycode key; SDL_Scancode scancode; Uint16 mod; bool down, repeat; } key;
    struct { Uint32 type; Uint8 button; bool down; Uint8 clicks; float x, y; } button;
    struct { Uint32 type; Uint32 state; float x, y, xrel, yrel; } motion;
    struct { Uint32 type; float x, y; } wheel;
    struct { Uint32 type; const char *text; } text;
    Uint8 padding[64];
} mw2c_Event;
#define SDL_Event mw2c_Event
static mw2c_Event mw2c_pushed[64];
static int mw2c_npushed MW2C_UNUSED;
MW2C bool mw2c_PushEvent(const mw2c_Event *e)
{
    if (mw2c_npushed >= 64) return false;
    mw2c_pushed[mw2c_npushed++] = *e;
    return true;
}
MW2C bool mw2c_PollEvent(mw2c_Event *e)
{
    static char text[8][32];
    static int ti;
    {
        mw2c_Event2 s;
        if (mw2c_npushed > 0) {   /* the port's own first (in the order pushed) */
            *e = mw2c_pushed[0];
            memmove(mw2c_pushed, mw2c_pushed + 1, (size_t)(--mw2c_npushed) * sizeof *mw2c_pushed);
            return true;
        }
        if (!(SDL_PollEvent)(&s)) return false;
        memset(e, 0, sizeof *e);
        e->type = s.type;
        switch (s.type) {
        case SDL_KEYDOWN: case SDL_KEYUP:
            e->key.key = s.key.keysym.sym; e->key.scancode = s.key.keysym.scancode; e->key.mod = s.key.keysym.mod;
            e->key.down = s.key.state == SDL_PRESSED; e->key.repeat = s.key.repeat != 0;
            break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
            e->button.button = s.button.button; e->button.down = s.button.state == SDL_PRESSED; e->button.clicks = s.button.clicks;
            e->button.x = (float)s.button.x; e->button.y = (float)s.button.y;
            break;
        case SDL_MOUSEMOTION:
            e->motion.state = s.motion.state; e->motion.x = (float)s.motion.x; e->motion.y = (float)s.motion.y;
            e->motion.xrel = (float)s.motion.xrel; e->motion.yrel = (float)s.motion.yrel;
            break;
        case SDL_MOUSEWHEEL:
            e->wheel.x = s.wheel.preciseX; e->wheel.y = s.wheel.preciseY;
            if (s.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) { e->wheel.x = -e->wheel.x; e->wheel.y = -e->wheel.y; }
            break;
        case SDL_TEXTINPUT:
            ti = (ti + 1) & 7;
            memcpy(text[ti], s.text.text, sizeof text[ti]);
            text[ti][31] = 0;
            e->text.text = text[ti];
            break;
        default: break;
        }
    }
    return true;
}
#define SDL_PollEvent(e) mw2c_PollEvent(e)
#define SDL_PushEvent(e) mw2c_PushEvent(e)
MW2C bool SDL_ConvertEventToRenderCoordinates(SDL_Renderer *r, mw2c_Event *e) { (void)r; (void)e; return true; }   /* (SDL2's renderer did it already) */

/* ---- windows and display modes */
typedef Uint32 SDL_PropertiesID;
#define SDL_PROP_WINDOW_CREATE_TITLE_STRING "title"
#define SDL_PROP_WINDOW_CREATE_X_NUMBER "x"
#define SDL_PROP_WINDOW_CREATE_Y_NUMBER "y"
#define SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER "width"
#define SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER "height"
#define SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER "flags"
typedef struct { char title[128]; Sint64 x, y, w, h, flags; } mw2c_props;
static mw2c_props mw2c_prop;   /* (one set at a time: the port makes a window's and destroys it) */
MW2C SDL_PropertiesID SDL_CreateProperties(void)
{
    memset(&mw2c_prop, 0, sizeof mw2c_prop);
    mw2c_prop.x = mw2c_prop.y = SDL_WINDOWPOS_UNDEFINED;
    return 1;
}
MW2C bool SDL_SetStringProperty(SDL_PropertiesID p, const char *n, const char *v)
{
    (void)p;
    if (!strcmp(n, "title")) SDL_strlcpy(mw2c_prop.title, v ? v : "", sizeof mw2c_prop.title);
    return true;
}
MW2C bool SDL_SetNumberProperty(SDL_PropertiesID p, const char *n, Sint64 v)
{
    (void)p;
    if (!strcmp(n, "x")) mw2c_prop.x = v; else if (!strcmp(n, "y")) mw2c_prop.y = v;
    else if (!strcmp(n, "width")) mw2c_prop.w = v; else if (!strcmp(n, "height")) mw2c_prop.h = v;
    else if (!strcmp(n, "flags")) mw2c_prop.flags = v;
    return true;
}
MW2C bool SDL_SetBooleanProperty(SDL_PropertiesID p, const char *n, bool v) { (void)p; (void)n; (void)v; return true; }
MW2C void SDL_DestroyProperties(SDL_PropertiesID p) { (void)p; }
#define SDL_WINDOW_HIGH_PIXEL_DENSITY SDL_WINDOW_ALLOW_HIGHDPI
/* SDL3's SDL_WINDOW_FULLSCREEN with no mode set is the desktop's (SDL2: FULLSCREEN_DESKTOP); a mode set on the window
 * (SDL_SetWindowFullscreenMode) makes it exclusive */
static int mw2c_mode_set;
MW2C Uint32 mw2c_flags(Sint64 f)
{
    Uint32 o = (Uint32)f;
    if (o & SDL_WINDOW_FULLSCREEN) o = (o & ~(Uint32)SDL_WINDOW_FULLSCREEN) | SDL_WINDOW_FULLSCREEN_DESKTOP;
    return o;
}
MW2C SDL_Window *SDL_CreateWindowWithProperties(SDL_PropertiesID p)
{
    (void)p;
    return (SDL_CreateWindow)(mw2c_prop.title, (int)mw2c_prop.x, (int)mw2c_prop.y, (int)mw2c_prop.w, (int)mw2c_prop.h, mw2c_flags(mw2c_prop.flags));
}
#define SDL_CreateWindow(t, w, h, f) (SDL_CreateWindow)(t, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, mw2c_flags((Sint64)(f)))
MW2C bool mw2c_SetWindowFullscreen(SDL_Window *w, bool on)
{
    return (SDL_SetWindowFullscreen)(w, on ? (mw2c_mode_set ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_FULLSCREEN_DESKTOP) : 0) == 0;
}
#define SDL_SetWindowFullscreen(w, on) mw2c_SetWindowFullscreen(w, on)
MW2C bool SDL_SetWindowFullscreenMode(SDL_Window *w, const SDL_DisplayMode *m)
{
    mw2c_mode_set = m != NULL;
    return !m || SDL_SetWindowDisplayMode(w, m) == 0;
}
MW2C bool SDL_SyncWindow(SDL_Window *w) { (void)w; return true; }
MW2C bool mw2c_GetWindowSizeInPixels(SDL_Window *w, int *x, int *y)
{
#if SDL_VERSION_ATLEAST(2, 26, 0)
    (SDL_GetWindowSizeInPixels)(w, x, y);
#else
    if (SDL_GetWindowFlags(w) & SDL_WINDOW_OPENGL) SDL_GL_GetDrawableSize(w, x, y); else SDL_GetWindowSize(w, x, y);
#endif
    return true;
}
#define SDL_GetWindowSizeInPixels(w, x, y) mw2c_GetWindowSizeInPixels(w, x, y)
typedef int SDL_DisplayID;
#define SDL_GetPrimaryDisplay() 0
#define SDL_GetDisplayForWindow(w) SDL_GetWindowDisplayIndex(w)
MW2C const SDL_DisplayMode *mw2c_GetDesktopDisplayMode(int d)
{
    static SDL_DisplayMode m;
    return (SDL_GetDesktopDisplayMode)(d, &m) == 0 ? &m : NULL;
}
#define SDL_GetDesktopDisplayMode(d) mw2c_GetDesktopDisplayMode(d)
MW2C SDL_DisplayMode **SDL_GetFullscreenDisplayModes(int d, int *count)
{
    int n = SDL_GetNumDisplayModes(d), i, k = 0;
    SDL_DisplayMode **list;
    if (n < 0) n = 0;
    list = (SDL_DisplayMode **)SDL_malloc((size_t)(n + 1) * (sizeof *list + sizeof **list));
    if (!list) { if (count) *count = 0; return NULL; }
    for (i = 0; i < n; i++) {
        SDL_DisplayMode *m = (SDL_DisplayMode *)(list + n + 1) + k;
        if (SDL_GetDisplayMode(d, i, m) == 0) list[k++] = m;
    }
    list[k] = NULL;
    if (count) *count = k;
    return list;
}
MW2C bool SDL_GetClosestFullscreenDisplayMode(int d, int w, int h, float refresh, bool hd, SDL_DisplayMode *out)
{
    SDL_DisplayMode want;
    (void)hd;
    memset(&want, 0, sizeof want);
    want.w = w; want.h = h; want.refresh_rate = (int)refresh;
    return SDL_GetClosestDisplayMode(d, &want, out) != NULL;
}

/* ---- the 2D renderer (the shell) */
typedef enum { SDL_LOGICAL_PRESENTATION_DISABLED, SDL_LOGICAL_PRESENTATION_STRETCH, SDL_LOGICAL_PRESENTATION_LETTERBOX,
               SDL_LOGICAL_PRESENTATION_OVERSCAN, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE } SDL_RendererLogicalPresentation;
#define SDL_SCALEMODE_NEAREST SDL_ScaleModeNearest
#define SDL_SCALEMODE_LINEAR SDL_ScaleModeLinear
MW2C SDL_Renderer *mw2c_CreateRenderer(SDL_Window *w)
{
    /* SDL3 takes the hint's list (e.g. "opengl,software"); SDL2 one name: each in turn */
    const char *h = SDL_GetHint(SDL_HINT_RENDER_DRIVER);
    char list[128], *p, *q;
    SDL_Renderer *r = NULL;
    if (!h || !h[0]) return (SDL_CreateRenderer)(w, -1, 0);
    SDL_strlcpy(list, h, sizeof list);
    for (p = list; p && *p && !r; p = q) {
        q = SDL_strchr(p, ',');
        if (q) *q++ = 0;
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, p);
        r = (SDL_CreateRenderer)(w, -1, !SDL_strcasecmp(p, "software") ? SDL_RENDERER_SOFTWARE : 0);
    }
    return r;
}
#define SDL_CreateRenderer(w, name) mw2c_CreateRenderer(w)
MW2C const char *SDL_GetRendererName(SDL_Renderer *r)
{
    static SDL_RendererInfo i;
    return SDL_GetRendererInfo(r, &i) == 0 ? i.name : "?";
}
MW2C bool SDL_SetRenderVSync(SDL_Renderer *r, int v) { return SDL_RenderSetVSync(r, v) == 0; }
MW2C bool SDL_SetRenderLogicalPresentation(SDL_Renderer *r, int w, int h, SDL_RendererLogicalPresentation m)
{
    if (m == SDL_LOGICAL_PRESENTATION_DISABLED) { w = 0; h = 0; }
    SDL_RenderSetLogicalSize(r, w, h);
    SDL_RenderSetIntegerScale(r, m == SDL_LOGICAL_PRESENTATION_INTEGER_SCALE ? SDL_TRUE : SDL_FALSE);
    return true;
}
MW2C bool SDL_GetRenderLogicalPresentation(SDL_Renderer *r, int *w, int *h, SDL_RendererLogicalPresentation *m)
{
    int lw = 0, lh = 0;
    SDL_RenderGetLogicalSize(r, &lw, &lh);
    if (w) *w = lw;
    if (h) *h = lh;
    if (m) *m = !lw ? SDL_LOGICAL_PRESENTATION_DISABLED : SDL_RenderGetIntegerScale(r) ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE : SDL_LOGICAL_PRESENTATION_LETTERBOX;
    return true;
}
MW2C bool SDL_GetRenderOutputSize(SDL_Renderer *r, int *w, int *h) { return SDL_GetRendererOutputSize(r, w, h) == 0; }
MW2C bool SDL_SetRenderClipRect(SDL_Renderer *r, const SDL_Rect *rc) { return SDL_RenderSetClipRect(r, rc) == 0; }   /* logical coordinates, as SDL3 */
MW2C bool SDL_RenderCoordinatesFromWindow(SDL_Renderer *r, float wx, float wy, float *x, float *y) { SDL_RenderWindowToLogical(r, (int)wx, (int)wy, x, y); return true; }
MW2C bool mw2c_RenderTexture(SDL_Renderer *r, SDL_Texture *t, const SDL_FRect *src, const SDL_FRect *dst)
{
    SDL_Rect s;
    if (src) { s.x = (int)src->x; s.y = (int)src->y; s.w = (int)src->w; s.h = (int)src->h; }
    return SDL_RenderCopyF(r, t, src ? &s : NULL, dst) == 0;
}
#define SDL_RenderTexture(r, t, s, d) mw2c_RenderTexture(r, t, s, d)
MW2C bool mw2c_UpdateTexture(SDL_Texture *t, const SDL_Rect *r, const void *p, int n) { return (SDL_UpdateTexture)(t, r, p, n) == 0; }
MW2C bool mw2c_LockTexture(SDL_Texture *t, const SDL_Rect *r, void **p, int *n) { return (SDL_LockTexture)(t, r, p, n) == 0; }
#define SDL_UpdateTexture(t, r, p, n) mw2c_UpdateTexture(t, r, p, n)
#define SDL_LockTexture(t, r, p, n) mw2c_LockTexture(t, r, p, n)
MW2C bool SDL_GetTextureSize(SDL_Texture *t, float *w, float *h)
{
    int iw = 0, ih = 0;
    bool ok = SDL_QueryTexture(t, NULL, NULL, &iw, &ih) == 0;
    if (w) *w = (float)iw;
    if (h) *h = (float)ih;
    return ok;
}
MW2C SDL_Surface *mw2c_RenderReadPixels(SDL_Renderer *r, const SDL_Rect *rect)
{
    int w = 0, h = 0;
    SDL_Surface *s;
    if (rect) { w = rect->w; h = rect->h; } else SDL_GetRendererOutputSize(r, &w, &h);
    s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (s && (SDL_RenderReadPixels)(r, rect, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) != 0) { SDL_FreeSurface(s); s = NULL; }
    return s;
}
#define SDL_RenderReadPixels(r, rect) mw2c_RenderReadPixels(r, rect)
#define SDL_ConvertSurface(s, f) SDL_ConvertSurfaceFormat(s, f, 0)
#define SDL_DestroySurface SDL_FreeSurface

/* ---- audio: SDL3's device stream (the callback asks for bytes, SDL_PutAudioStreamData answers) as SDL2's callback
 * device; the stream's lock is the device's (SDL2 holds it through the callback, as SDL3 holds the stream's) */
#define SDL_AUDIO_F32 AUDIO_F32SYS
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK 0
typedef struct mw2c_AudioStream mw2c_AudioStream;
typedef void (SDLCALL *mw2c_AudioStreamCallback)(void *user, mw2c_AudioStream *s, int additional, int total);
struct mw2c_AudioStream { SDL_AudioDeviceID dev; mw2c_AudioStreamCallback cb; void *user; Uint8 *out; int len, pos; };
#define SDL_AudioStream mw2c_AudioStream
static void SDLCALL mw2c_audio_cb(void *u, Uint8 *buf, int len)
{
    mw2c_AudioStream *s = (mw2c_AudioStream *)u;
    s->out = buf; s->len = len; s->pos = 0;
    s->cb(s->user, s, len, len);
    if (s->pos < len) memset(buf + s->pos, 0, (size_t)(len - s->pos));
    s->out = NULL;
}
MW2C mw2c_AudioStream *SDL_OpenAudioDeviceStream(int dev, const SDL_AudioSpec *spec, mw2c_AudioStreamCallback cb, void *user)
{
    SDL_AudioSpec want, have;
    const char *fr = SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
    mw2c_AudioStream *s = (mw2c_AudioStream *)SDL_calloc(1, sizeof *s);
    (void)dev;
    if (!s) return NULL;
    memset(&want, 0, sizeof want);
    want.freq = spec->freq; want.format = spec->format; want.channels = spec->channels;
    want.samples = (Uint16)(fr && SDL_atoi(fr) > 0 ? SDL_atoi(fr) : 1024);
    want.callback = mw2c_audio_cb; want.userdata = s;
    s->cb = cb; s->user = user;
    s->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);   /* (no changes allowed: SDL2 converts to the device's) */
    if (!s->dev) { SDL_free(s); return NULL; }
    return s;
}
MW2C bool SDL_PutAudioStreamData(mw2c_AudioStream *s, const void *data, int n)
{
    int k = s->out && n > s->len - s->pos ? s->len - s->pos : n;
    if (!s->out || k <= 0) return false;
    memcpy(s->out + s->pos, data, (size_t)k);
    s->pos += k;
    return true;
}
MW2C bool SDL_ResumeAudioStreamDevice(mw2c_AudioStream *s) { SDL_PauseAudioDevice(s->dev, 0); return true; }
MW2C bool SDL_LockAudioStream(mw2c_AudioStream *s) { if (s) SDL_LockAudioDevice(s->dev); return true; }
MW2C bool SDL_UnlockAudioStream(mw2c_AudioStream *s) { if (s) SDL_UnlockAudioDevice(s->dev); return true; }
MW2C void SDL_DestroyAudioStream(mw2c_AudioStream *s) { if (s) { SDL_CloseAudioDevice(s->dev); SDL_free(s); } }

/* ---- joysticks: SDL3 opens by instance id; here the "ids" are SDL2's device indices */
MW2C SDL_JoystickID *SDL_GetJoysticks(int *count)
{
    int n = SDL_NumJoysticks(), i;
    SDL_JoystickID *ids;
    if (n < 0) n = 0;
    ids = (SDL_JoystickID *)SDL_malloc((size_t)(n + 1) * sizeof *ids);
    if (ids) { for (i = 0; i < n; i++) ids[i] = i; ids[n] = 0; }
    if (count) *count = ids ? n : 0;
    return ids;
}
#define SDL_OpenJoystick(id) SDL_JoystickOpen((int)(id))
#define SDL_CloseJoystick SDL_JoystickClose
#define SDL_GetNumJoystickAxes SDL_JoystickNumAxes
#define SDL_GetNumJoystickButtons SDL_JoystickNumButtons
#define SDL_GetNumJoystickHats SDL_JoystickNumHats
#define SDL_GetJoystickAxis SDL_JoystickGetAxis
#define SDL_GetJoystickButton(j, b) (SDL_JoystickGetButton(j, b) != 0)
#define SDL_GetJoystickHat SDL_JoystickGetHat

#endif /* MW2_SDL2 */
#endif
