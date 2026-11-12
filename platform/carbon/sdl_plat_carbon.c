/*
 * sdl_plat_carbon.c - the SDL subset's system layer (sdl_plat.h) on Carbon: Mac OS 9 with CarbonLib, and Mac OS X
 * 10.1-10.4 on PowerPC, one CFM binary for both.
 *
 *  - windows: NewCWindow; fullscreen through AGL (aglSetFullScreen): the display switched to 640x480, 800x600 or
 *    1024x768 when the window's GL context is made, restored when it goes (no switching mid-mission yet)
 *  - OpenGL: AGL on the window's port; OpenGL and AGL loaded by name at start (Mac OS 9: the OpenGLLibrary / AGL code
 *    fragments through CFM; Mac OS X: OpenGL.framework / AGL.framework through CFBundle, their Mach-O entry points
 *    wrapped in CFM transition vectors), so the build links neither
 *  - input: WaitNextEvent for clicks, text and quit; GetKeys diffed every pump for key down / up by Mac virtual key code
 *    (as SDL's Cocoa backend maps them); GetGlobalMouse for the position and motion. Relative mouse: Mac OS X warps the
 *    cursor back (CGWarpMouseCursorPosition) and reads CGGetLastMouseDelta; Mac OS 9 has no warp in Carbon - motion
 *    stops at the screen's edge (OPEN: InputSprocket)
 *  - audio: a Sound Manager channel, 16-bit stereo 44.1 kHz, two buffers re-queued from the callBackCmd callback - which
 *    on Mac OS 9 runs at interrupt time, so it only copies (sdl_core.c's ring)
 *
 * UNTESTED as written: it compiles with Retro68 against Apple's Universal Interfaces 3.4 (TARGET_API_MAC_CARBON);
 * see docs/CARBON.md for the build and what is still to check on hardware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Carbon.h>
#include "carbon_extra.h"   /* (what Multiversal lacks) */

#include "sdl_plat.h"

/* ------------------------------------------------------------------------------------------------ the system */
static int g_osx;                       /* running on Mac OS X (CarbonLib's Carbon: Mac OS 9) */

/* ---------------------------------------------------------------------------------------------- settings file */
/* Mac OS 9 has no environment variables, so the game's switches (MW2_VRAM_MB, MW2_MUSIC, the MW2_GL12_* and test
 * switches - docs/CARBON.md) come from a text file "mw2.env" next to the application: one KEY=VALUE a line, '#'
 * comments, an optional "export " and quotes (the same lines as the game folder's mw2.env, whose $PWD lines are skipped).
 * Read before main(), so every getenv() in the game sees them. */
FILE *mw2_fopen(const char *path, const char *mode);
int setenv(const char *name, const char *value, int overwrite);
__attribute__((constructor)) static void load_settings(void)
{
    char path[1100], line[512];
    FILE *f;
    snprintf(path, sizeof path, "%smw2.env", plat_base_path());
    if (!(f = mw2_fopen(path, "r"))) return;
    while (fgets(line, sizeof line, f)) {
        char *k = line, *v, *e;
        while (*k == ' ' || *k == '\t') k++;
        if (strncmp(k, "export ", 7) == 0) k += 7;
        if (*k == '#' || !(v = strchr(k, '='))) continue;
        *v++ = 0;
        for (e = v; *e && *e != '\r' && *e != '\n' && !(*e == '#' && e > v && (e[-1] == ' ' || e[-1] == '\t')); e++) {}
        while (e > v && (e[-1] == ' ' || e[-1] == '\t')) e--;
        *e = 0;
        if (*v == '"' && e > v + 1 && e[-1] == '"') { v++; e[-1] = 0; }
        if (*k && !strstr(v, "$")) setenv(k, v, 0);
    }
    fclose(f);
}

int plat_init(char *err, int errn)
{
    SInt32 v = 0;
    (void)err; (void)errn;
    InitCursor();
    if (Gestalt(gestaltSystemVersion, &v) == noErr && v >= 0x1000) g_osx = 1;
    return 0;
}
void plat_quit(void) { ShowMenuBar(); InitCursor(); }
const char *plat_name(void) { return g_osx ? "carbon (mac os x)" : "carbon (mac os 9)"; }

uint64_t plat_ticks_us(void)
{
    UnsignedWide u;
    Microseconds(&u);
    return ((uint64_t)u.hi << 32) | u.lo;
}
void plat_delay_ms(uint32_t ms)
{
    uint64_t end = plat_ticks_us() + (uint64_t)ms * 1000u;
    EventRecord ev;
    while (plat_ticks_us() < end) {
        if (ms >= 17) EventAvail(0, &ev);   /* (lets the system breathe on Mac OS 9's cooperative scheduler) */
    }
}

/* ------------------------------------------------------------------------------------------- loading by name */
/* Mac OS X: a CFBundle's function wrapped in a transition vector (code, TOC) so CFM code can call it */
static void *tvector(void *machfn)
{
    void **tv;
    if (!machfn) return NULL;
    tv = (void **)NewPtr(2 * sizeof(void *));
    if (!tv) return NULL;
    tv[0] = machfn; tv[1] = NULL;
    return tv;
}
static CFBundleRef bundle(const char *path)
{
    CFStringRef s = CFStringCreateWithCString(NULL, path, kCFStringEncodingMacRoman);
    CFURLRef u = CFURLCreateWithFileSystemPath(NULL, s, kCFURLPOSIXPathStyle, true);
    CFBundleRef b = CFBundleCreate(NULL, u);
    CFRelease(u); CFRelease(s);
    if (b && !CFBundleLoadExecutable(b)) { CFRelease(b); b = NULL; }
    return b;
}
static void *bundle_fn(CFBundleRef b, const char *name)
{
    CFStringRef s;
    void *f;
    if (!b) return NULL;
    s = CFStringCreateWithCString(NULL, name, kCFStringEncodingMacRoman);
    f = CFBundleGetFunctionPointerForName(b, s);
    CFRelease(s);
    return tvector(f);
}
/* Mac OS 9: a symbol of one of the system's code fragments */
static void *fragment_fn(const char *lib, const char *name)
{
    Str63 l;
    Str255 n;
    CFragConnectionID c;
    Ptr main_addr, sym = NULL;
    Str255 errname;
    CFragSymbolClass cls;
    l[0] = (unsigned char)strlen(lib); memcpy(l + 1, lib, l[0]);
    if (GetSharedLibrary(l, kPowerPCCFragArch, kReferenceCFrag, &c, &main_addr, errname) != noErr) return NULL;
    n[0] = (unsigned char)strlen(name); memcpy(n + 1, name, n[0]);
    if (FindSymbol(c, n, &sym, &cls) != noErr) return NULL;
    return sym;
}
static void *gl_fn(const char *name)
{
    static CFBundleRef gl, agl;
    static const char *const libs[] = {"OpenGLLibrary", "AGL", "OpenGLUtility", NULL};
    int k;
    void *f;
    if (g_osx) {
        if (!gl) gl = bundle("/System/Library/Frameworks/OpenGL.framework");
        if (!agl) agl = bundle("/System/Library/Frameworks/AGL.framework");
        f = bundle_fn(name[0] == 'a' ? agl : gl, name);
        return f ? f : bundle_fn(name[0] == 'a' ? gl : agl, name);
    }
    for (k = 0; libs[k]; k++) if ((f = fragment_fn(libs[k], name)) != NULL) return f;
    return NULL;
}

/* ----------------------------------------------------------------------------------------------------- AGL */
typedef struct __AGLPixelFormatRec *AGLPixelFormat;
typedef struct __AGLContextRec *AGLContext;
enum { AGL_NONE = 0, AGL_RGBA = 4, AGL_DOUBLEBUFFER = 5, AGL_RED_SIZE = 8, AGL_GREEN_SIZE = 9, AGL_BLUE_SIZE = 10,
       AGL_DEPTH_SIZE = 12, AGL_PIXEL_SIZE = 50, AGL_FULLSCREEN = 54, AGL_NO_RECOVERY = 72, AGL_ACCELERATED = 73,
       AGL_VIDEO_MEMORY = 120, AGL_SWAP_INTERVAL = 222 };   /* agl.h */
static AGLPixelFormat (*p_aglChoosePixelFormat)(const void *gdevs, int ndev, const long *attribs);
typedef struct __AGLRendererInfoRec *AGLRendererInfo;
static AGLRendererInfo (*p_aglQueryRendererInfo)(const void *gdevs, long ndev);
static AGLRendererInfo (*p_aglNextRendererInfo)(AGLRendererInfo rend);
static unsigned char (*p_aglDescribeRenderer)(AGLRendererInfo rend, long prop, long *value);
static void (*p_aglDestroyRendererInfo)(AGLRendererInfo rend);
static AGLContext (*p_aglCreateContext)(AGLPixelFormat pix, AGLContext share);
static unsigned char (*p_aglSetDrawable)(AGLContext ctx, CGrafPtr draw);
static unsigned char (*p_aglSetCurrentContext)(AGLContext ctx);
static void (*p_aglSwapBuffers)(AGLContext ctx);
static unsigned char (*p_aglSetInteger)(AGLContext ctx, unsigned long pname, const long *params);
static unsigned char (*p_aglUpdateContext)(AGLContext ctx);
static unsigned char (*p_aglDestroyContext)(AGLContext ctx);
static void (*p_aglDestroyPixelFormat)(AGLPixelFormat pix);
static unsigned char (*p_aglSetFullScreen)(AGLContext ctx, long width, long height, long freq, long device);
static int agl_load(void)
{
    static int done = -1;
    if (done >= 0) return done;
    *(void **)&p_aglChoosePixelFormat = gl_fn("aglChoosePixelFormat");
    *(void **)&p_aglQueryRendererInfo = gl_fn("aglQueryRendererInfo");
    *(void **)&p_aglNextRendererInfo = gl_fn("aglNextRendererInfo");
    *(void **)&p_aglDescribeRenderer = gl_fn("aglDescribeRenderer");
    *(void **)&p_aglDestroyRendererInfo = gl_fn("aglDestroyRendererInfo");
    *(void **)&p_aglCreateContext = gl_fn("aglCreateContext");
    *(void **)&p_aglSetDrawable = gl_fn("aglSetDrawable");
    *(void **)&p_aglSetCurrentContext = gl_fn("aglSetCurrentContext");
    *(void **)&p_aglSwapBuffers = gl_fn("aglSwapBuffers");
    *(void **)&p_aglSetInteger = gl_fn("aglSetInteger");
    *(void **)&p_aglUpdateContext = gl_fn("aglUpdateContext");
    *(void **)&p_aglDestroyContext = gl_fn("aglDestroyContext");
    *(void **)&p_aglDestroyPixelFormat = gl_fn("aglDestroyPixelFormat");
    *(void **)&p_aglSetFullScreen = gl_fn("aglSetFullScreen");
    done = p_aglChoosePixelFormat && p_aglCreateContext && p_aglSetDrawable && p_aglSetCurrentContext && p_aglSwapBuffers;
    return done;
}

/* ------------------------------------------------------------------------------------------------------ windows */
/* fullscreen: AGL's own (aglSetFullScreen) - the display switched to the window's size when its context is made, and
 * back when the context goes; the window itself stays as the event target */
struct plat_window { WindowPtr w; int width, height, fullscreen, fs_active; Rect saved; AGLContext ctx; };
static plat_window *g_win;

static Rect screen_rect(void)
{
    BitMap bm;
    GetQDGlobalsScreenBits(&bm);
    return bm.bounds;
}
static void pstr(Str255 out, const char *s) { size_t n = strlen(s); if (n > 255) n = 255; out[0] = (unsigned char)n; memcpy(out + 1, s, n); }

plat_window *plat_window_create(const char *title, int w, int h, int fullscreen, int hidden)
{
    plat_window *pw = calloc(1, sizeof *pw);
    Rect s = screen_rect(), r;
    Str255 t;
    if (!pw) return NULL;
    pstr(t, title);
    if (fullscreen) { r.left = s.left + 40; r.top = s.top + 60; r.right = (short)(r.left + 64); r.bottom = (short)(r.top + 48); }
    else {
        int sw = s.right - s.left, sh = s.bottom - s.top;
        if (w > sw) w = sw;
        if (h > sh - 40) h = sh - 40;
        r.left = (short)(s.left + (sw - w) / 2); r.top = (short)(s.top + 40 + (sh - 40 - h) / 2);
        r.right = (short)(r.left + w); r.bottom = (short)(r.top + h);
    }
    pw->w = NewCWindow(NULL, &r, t, !hidden && !fullscreen, fullscreen ? plainDBoxProc : noGrowDocProc, (WindowPtr)-1L, !fullscreen, 0);
    if (!pw->w) { free(pw); return NULL; }
    pw->width = r.right - r.left; pw->height = r.bottom - r.top; pw->fullscreen = fullscreen;
    if (fullscreen) { pw->width = w; pw->height = h; }
    SetPortWindowPort(pw->w);
    g_win = pw;
    return pw;
}
void plat_window_destroy(plat_window *w)
{
    if (!w) return;
    if (w->ctx && w->fs_active && p_aglSetDrawable) p_aglSetDrawable(w->ctx, NULL);   /* (the display's mode back) */
    DisposeWindow(w->w);
    if (g_win == w) g_win = NULL;
    free(w);
}
void plat_window_title(plat_window *w, const char *title) { Str255 t; pstr(t, title); SetWTitle(w->w, t); }
void plat_window_size(plat_window *w, int *pw, int *ph) { *pw = w->width; *ph = w->height; }
int plat_window_set_size(plat_window *w, int pw, int ph)
{
    if (w->fullscreen) return 0;
    SizeWindow(w->w, (short)pw, (short)ph, true);
    w->width = pw; w->height = ph;
    if (w->ctx && p_aglUpdateContext) p_aglUpdateContext(w->ctx);
    return 0;
}
int plat_window_fullscreen(plat_window *w, int on, int fw, int fh)
{
    /* before the window's GL context exists: the mode is recorded and set when the context is made (how the shell and
     * the missions open fullscreen). Afterwards a change would need a new context (the game's GL state and textures
     * live in this one): declined for now */
    if (fw <= 0 || fh <= 0) { fw = w->width; fh = w->height; }
    if (w->ctx) return (on == w->fullscreen && (!on || (fw == w->width && fh == w->height))) ? 0 : -1;
    w->fullscreen = on;
    if (on) { w->width = fw; w->height = fh; }
    return 0;
}
void plat_window_show(plat_window *w, int show) { if (show) { ShowWindow(w->w); SelectWindow(w->w); } else HideWindow(w->w); }
int plat_window_focused(plat_window *w) { return FrontWindow() == w->w; }

/* ----------------------------------------------------------------------------------------------------- OpenGL */
void *plat_gl_create(plat_window *w)
{
    static const long attr16[] = {AGL_RGBA, AGL_DOUBLEBUFFER, AGL_DEPTH_SIZE, 16, AGL_ACCELERATED, AGL_NO_RECOVERY, AGL_NONE};
    static const long attr24[] = {AGL_RGBA, AGL_DOUBLEBUFFER, AGL_DEPTH_SIZE, 24, AGL_ACCELERATED, AGL_NO_RECOVERY, AGL_NONE};
    /* fullscreen: a 16-bit display first (half the frame buffers' video memory - the art is from 6-bit-a-channel VGA
     * palettes), the depth buffer at 24 bits where the card has it */
    static const long fs16c24[] = {AGL_RGBA, AGL_DOUBLEBUFFER, AGL_FULLSCREEN, AGL_PIXEL_SIZE, 16, AGL_DEPTH_SIZE, 24, AGL_ACCELERATED, AGL_NO_RECOVERY, AGL_NONE};
    static const long fs16c16[] = {AGL_RGBA, AGL_DOUBLEBUFFER, AGL_FULLSCREEN, AGL_PIXEL_SIZE, 16, AGL_DEPTH_SIZE, 16, AGL_ACCELERATED, AGL_NO_RECOVERY, AGL_NONE};
    static const long fs24[] = {AGL_RGBA, AGL_DOUBLEBUFFER, AGL_FULLSCREEN, AGL_DEPTH_SIZE, 24, AGL_ACCELERATED, AGL_NO_RECOVERY, AGL_NONE};
    static const long fs16[] = {AGL_RGBA, AGL_DOUBLEBUFFER, AGL_FULLSCREEN, AGL_DEPTH_SIZE, 16, AGL_ACCELERATED, AGL_NO_RECOVERY, AGL_NONE};
    AGLPixelFormat pf;
    AGLContext c;
    if (!agl_load()) return NULL;
    if (w->fullscreen && p_aglSetFullScreen) {   /* the display switched to the window's size (640x480, 800x600, 1024x768) */
        GDHandle dev = GetMainDevice();
        {   /* only a mode the list offers: the smallest that holds the window, else the largest */
            int m[3][2], n = plat_display_modes(m, 3), k, pick = 0;
            for (k = n - 1; k >= 0; k--) if (m[k][0] >= w->width && m[k][1] >= w->height) { pick = k; break; }
            if (k < 0) pick = 0;
            if (n > 0 && (m[pick][0] != w->width || m[pick][1] != w->height)) { w->width = m[pick][0]; w->height = m[pick][1]; }
        }
        pf = p_aglChoosePixelFormat(&dev, 1, fs16c24);
        if (!pf) pf = p_aglChoosePixelFormat(&dev, 1, fs16c16);
        if (!pf) pf = p_aglChoosePixelFormat(&dev, 1, fs24);
        if (!pf) pf = p_aglChoosePixelFormat(&dev, 1, fs16);
        if (pf) {
            c = p_aglCreateContext(pf, NULL);
            if (p_aglDestroyPixelFormat) p_aglDestroyPixelFormat(pf);
            if (c && p_aglSetFullScreen(c, w->width, w->height, 0, 0) && p_aglSetCurrentContext(c)) { w->ctx = c; w->fs_active = 1; return c; }
            if (c && p_aglDestroyContext) p_aglDestroyContext(c);
        }
        /* no such mode: a window at that size instead */
        w->fullscreen = 0;
        SizeWindow(w->w, (short)w->width, (short)w->height, true);
        ShowWindow(w->w);
    }
    /* a 24-bit depth buffer where the card has one (Radeon), else 16 (Rage 128 / Rage Pro) */
    pf = p_aglChoosePixelFormat(NULL, 0, attr24);
    if (!pf) pf = p_aglChoosePixelFormat(NULL, 0, attr16);
    if (!pf) return NULL;
    c = p_aglCreateContext(pf, NULL);
    if (p_aglDestroyPixelFormat) p_aglDestroyPixelFormat(pf);
    if (!c) return NULL;
    if (!p_aglSetDrawable(c, GetWindowPort(w->w)) || !p_aglSetCurrentContext(c)) { if (p_aglDestroyContext) p_aglDestroyContext(c); return NULL; }
    w->ctx = c;
    return c;
}
int plat_gl_make_current(plat_window *w, void *ctx)
{
    static AGLContext cur;
    if (cur == (AGLContext)ctx) return 0;
    (void)w;
    cur = (AGLContext)ctx;
    return p_aglSetCurrentContext(cur) ? 0 : -1;
}
void plat_gl_destroy(void *ctx) { if (ctx && p_aglDestroyContext) { p_aglSetCurrentContext(NULL); p_aglDestroyContext((AGLContext)ctx); } }
void plat_gl_swap(plat_window *w) { if (w->ctx) p_aglSwapBuffers(w->ctx); }
void *plat_gl_proc(const char *name) { return gl_fn(name); }
void plat_gl_swap_interval(int n) { long v = n; if (g_win && g_win->ctx && p_aglSetInteger) p_aglSetInteger(g_win->ctx, AGL_SWAP_INTERVAL, &v); }

/* ------------------------------------------------------------------------------------------------------- input */
static KeyMap g_km;
static Point g_last;
static int g_rel, g_btn;
static void (*p_CGWarpMouseCursorPosition)(float x, float y);   /* (CGPoint passed as two floats in registers) */
static void (*p_CGGetLastMouseDelta)(int32_t *dx, int32_t *dy);
static int (*p_CGAssociateMouseAndMouseCursorPosition)(int connected);

static void local(Point *p)
{
    GrafPtr old;
    if (!g_win || g_win->fs_active) return;   /* (fullscreen: the display is the window) */
    GetPort(&old);
    SetPortWindowPort(g_win->w);
    GlobalToLocal(p);
    SetPort(old);
}
static void key_diff(void)
{
    KeyMap now;
    int k;
    GetKeys(now);
    for (k = 0; k < 128; k++) {
        int a = (((const unsigned char *)g_km)[k >> 3] >> (k & 7)) & 1, b = (((const unsigned char *)now)[k >> 3] >> (k & 7)) & 1;
        if (a != b) core_key(k, 0, b);
    }
    memcpy(g_km, now, sizeof now);
}
static pascal OSErr quit_handler(const AppleEvent *e, AppleEvent *r, SInt32 refcon) { (void)e; (void)r; (void)refcon; core_quit_request(); return noErr; }

void plat_pump(void)
{
    EventRecord ev;
    Point gp, lp;
    static int ae;
    if (!ae) { AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, NewAEEventHandlerUPP((AEEventHandlerProcPtr)quit_handler), 0, false); ae = 1; }
    while (WaitNextEvent(everyEvent, &ev, 0, NULL)) {
        WindowPtr hit;
        short part;
        switch (ev.what) {
        case mouseDown:
            part = FindWindow(ev.where, &hit);
            if (part == inDrag && g_win && !g_win->fullscreen) { Rect s = screen_rect(); DragWindow(hit, ev.where, &s); }
            else if (part == inGoAway) { if (TrackGoAway(hit, ev.where)) core_quit_request(); }
            else if (part == inContent) {
                lp = ev.where; local(&lp);
                g_btn = (ev.modifiers & controlKey) ? 3 : 1;   /* control-click: the right button (one-button mice) */
                if (hit != FrontWindow()) SelectWindow(hit);
                core_mouse_button(g_btn, 1, (float)lp.h, (float)lp.v);
            }
            break;
        case mouseUp:
            lp = ev.where; local(&lp);
            if (g_btn) core_mouse_button(g_btn, 0, (float)lp.h, (float)lp.v);
            g_btn = 0;
            break;
        case keyDown: case autoKey:
            if ((ev.modifiers & cmdKey) && (ev.message & charCodeMask) == 'q') { core_quit_request(); break; }
            {
                char c = (char)(ev.message & charCodeMask), s[2] = {0, 0};
                if ((unsigned char)c >= 32 && (unsigned char)c < 127) { s[0] = c; core_text(s); }   /* (ASCII; MacRoman's upper half not yet) */
            }
            break;
        case updateEvt:
            BeginUpdate((WindowPtr)ev.message); EndUpdate((WindowPtr)ev.message);
            break;
        case kHighLevelEvent:
            AEProcessAppleEvent(&ev);
            break;
        default: break;
        }
    }
    key_diff();
    GetGlobalMouse(&gp);
    if (g_rel && g_osx && p_CGGetLastMouseDelta) {
        int32_t dx = 0, dy = 0;
        p_CGGetLastMouseDelta(&dx, &dy);
        lp = gp; local(&lp);
        core_mouse_move((float)lp.h, (float)lp.v, (float)dx, (float)dy);
    } else {
        lp = gp; local(&lp);
        core_mouse_move((float)lp.h, (float)lp.v, (float)(gp.h - g_last.h), (float)(gp.v - g_last.v));
    }
    g_last = gp;
}
void plat_relative_mouse(plat_window *w, int on)
{
    (void)w;
    g_rel = on;
    if (on) HideCursor(); else ShowCursor();
    if (g_osx) {
        static CFBundleRef as;
        if (!as) as = bundle("/System/Library/Frameworks/ApplicationServices.framework");
        if (!p_CGGetLastMouseDelta) {
            *(void **)&p_CGGetLastMouseDelta = bundle_fn(as, "CGGetLastMouseDelta");
            *(void **)&p_CGAssociateMouseAndMouseCursorPosition = bundle_fn(as, "CGAssociateMouseAndMouseCursorPosition");
            *(void **)&p_CGWarpMouseCursorPosition = bundle_fn(as, "CGWarpMouseCursorPosition");
        }
        if (p_CGAssociateMouseAndMouseCursorPosition) p_CGAssociateMouseAndMouseCursorPosition(!on);
    }
}
void plat_mouse_state(float *x, float *y, uint32_t *b) { Point p; GetGlobalMouse(&p); local(&p); *x = p.h; *y = p.v; *b = Button() ? 1u : 0u; }

/* ------------------------------------------------------------------------------------------------------- audio */
#define NBUF 2
static SndChannelPtr g_chan;
static SndCallBackUPP g_cbupp;
static plat_audio_fill g_fill;
static int g_frames, g_rate;
static int16_t *g_buf[NBUF];
static ExtSoundHeader g_hdr[NBUF];

static void queue(int i)
{
    SndCommand c;
    g_fill(g_buf[i], g_frames);   /* (interrupt time on Mac OS 9: a copy out of sdl_core.c's ring) */
    c.cmd = bufferCmd; c.param1 = 0; c.param2 = (long)&g_hdr[i];
    SndDoCommand(g_chan, &c, true);
    c.cmd = callBackCmd; c.param1 = (short)i; c.param2 = 0;
    SndDoCommand(g_chan, &c, true);
}
static pascal void snd_cb(SndChannelPtr ch, SndCommand *c) { (void)ch; if (g_fill) queue(c->param1); }

int plat_audio_open(int rate, int frames, plat_audio_fill fill)
{
    int i;
    g_fill = fill; g_frames = frames; g_rate = rate;
    g_cbupp = NewSndCallBackUPP(snd_cb);
    if (SndNewChannel(&g_chan, sampledSynth, initStereo, g_cbupp) != noErr) { g_chan = NULL; return -1; }
    for (i = 0; i < NBUF; i++) {
        g_buf[i] = (int16_t *)NewPtrClear((Size)frames * 4);
        if (!g_buf[i]) return -1;
        memset(&g_hdr[i], 0, sizeof g_hdr[i]);
        g_hdr[i].samplePtr = (Ptr)g_buf[i];
        g_hdr[i].numChannels = 2;
        g_hdr[i].sampleRate = (__typeof__(g_hdr[i].sampleRate))((unsigned long)rate << 16);   /* (Fixed / UnsignedFixed) */
        g_hdr[i].encode = extSH;
        g_hdr[i].baseFrequency = kMiddleC;
        g_hdr[i].numFrames = (unsigned long)frames;
        g_hdr[i].sampleSize = 16;
    }
    for (i = 0; i < NBUF; i++) queue(i);
    return 0;
}
void plat_audio_close(void)
{
    int i;
    if (!g_chan) return;
    g_fill = NULL;
    SndDisposeChannel(g_chan, true);
    g_chan = NULL;
    for (i = 0; i < NBUF; i++) { if (g_buf[i]) DisposePtr((Ptr)g_buf[i]); g_buf[i] = NULL; }
    if (g_cbupp) DisposeSndCallBackUPP(g_cbupp);
    g_cbupp = NULL;
}

/* ------------------------------------------------------------------------------------------------- the rest */
void plat_message_box(int error, const char *title, const char *msg)
{
    Str255 t, m;
    SInt16 item;
    pstr(t, title); pstr(m, msg);
    StandardAlert(error ? kAlertStopAlert : kAlertCautionAlert, t, m, NULL, &item);
}

/* a directory as a '/' path ("/Volume/Folder/"), walking up the catalog from its ID */
static void dir_path(short vref, SInt32 dir, char *path, size_t n)
{
    CInfoPBRec pb;
    Str255 name;
    char tmp[1024];
    path[0] = 0;
    for (;;) {
        memset(&pb, 0, sizeof pb);
        name[0] = 0;
        pb.dirInfo.ioNamePtr = name;
        pb.dirInfo.ioVRefNum = vref;
        pb.dirInfo.ioFDirIndex = -1;
        pb.dirInfo.ioDrDirID = dir;
        if (PBGetCatInfoSync(&pb) != noErr) break;
        snprintf(tmp, sizeof tmp, "%.*s/%s", (int)name[0], (const char *)name + 1, path);
        snprintf(path, n, "%s", tmp);
        if (dir == fsRtDirID) break;
        dir = pb.dirInfo.ioDrParID;
    }
    snprintf(tmp, sizeof tmp, "/%s", path);
    snprintf(path, n, "%s", tmp);
}

/* the application's folder */
const char *plat_base_path(void)
{
    static char path[1024];
    ProcessSerialNumber psn = {0, kCurrentProcess};
    ProcessInfoRec info;
    FSSpec app;
    if (path[0]) return path;
    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info;
    info.processAppSpec = &app;
    if (GetProcessInformation(&psn, &info) != noErr) { strcpy(path, "./"); return path; }
    dir_path(app.vRefNum, app.parID, path, sizeof path);
    return path;
}

/* the game folder when none is found (mw2shell.c's pick_folder): Navigation Services' Choose a Folder dialog.
 * 1 a folder ('/' path, no trailing '/'), 0 cancelled, -1 not available */
int mw2_pick_folder(const char *title, char *out, size_t n)
{
    NavDialogOptions opt;
    NavReplyRecord reply;
    AEKeyword kw;
    DescType type;
    SInt32 size;
    FSSpec fs;
    char dir[1024];
    OSErr e;
    if (NavGetDefaultDialogOptions(&opt) != noErr) return -1;
    pstr(opt.message, title);
    memset(&reply, 0, sizeof reply);
    e = NavChooseFolder(NULL, &reply, &opt, NULL, NULL, NULL);
    if (e == userCanceledErr || (e == noErr && !reply.validRecord)) { if (e == noErr) NavDisposeReply(&reply); return 0; }
    if (e != noErr) return -1;
    e = AEGetNthPtr(&reply.selection, 1, typeFSS, &kw, &type, &fs, sizeof fs, &size);
    NavDisposeReply(&reply);
    if (e != noErr) return -1;
    dir_path(fs.vRefNum, fs.parID, dir, sizeof dir);   /* the chosen folder's parent, then its name */
    snprintf(out, n, "%s%.*s", dir, (int)fs.name[0], (const char *)fs.name + 1);
    return 1;
}
/* the system's Preferences folder as a '/' path ending in '/' (System Folder:Preferences on Mac OS 9, ~/Library/
 * Preferences on OS X) - where portcfg.c keeps the port's settings; "" if it can't be found */
const char *mw2_mac_prefs_dir(void)
{
    static char path[1024];
    short vref;
    SInt32 dir;
    if (path[0]) return path;
    if (FindFolder((short)-32768 /* kOnSystemDisk */, kPreferencesFolderType, 0 /* kDontCreateFolder */, &vref, &dir) != noErr) return "";
    dir_path(vref, dir, path, sizeof path);
    return path;
}

/* Option held down (at launch: choose the game folder again) */
int mw2_mac_option_down(void)
{
    KeyMap km;
    GetKeys((void *)km);
    return (((const unsigned char *)km)[58 >> 3] >> (58 & 7)) & 1;   /* virtual key 0x3A: Option */
}

/* the main display's accelerated renderer's video memory in MB (0: unknown) - glview.c caps the resolution by it */
int mw2_mac_vram_mb(void)
{
    static int mb = -1;
    GDHandle dev;
    AGLRendererInfo first, r;
    if (mb >= 0) return mb;
    mb = 0;
    if (getenv("MW2_VRAM_MB")) return mb = atoi(getenv("MW2_VRAM_MB"));   /* (tests, or a card that misreports) */
    if (!agl_load() || !p_aglQueryRendererInfo || !p_aglDescribeRenderer) return mb;
    dev = GetMainDevice();
    first = p_aglQueryRendererInfo(&dev, 1);
    for (r = first; r; r = p_aglNextRendererInfo ? p_aglNextRendererInfo(r) : NULL) {
        long acc = 0, vram = 0;
        if (p_aglDescribeRenderer(r, AGL_ACCELERATED, &acc) && acc && p_aglDescribeRenderer(r, AGL_VIDEO_MEMORY, &vram) && vram / (1024 * 1024) > mb)
            mb = (int)(vram / (1024 * 1024));
    }
    if (first && p_aglDestroyRendererInfo) p_aglDestroyRendererInfo(first);
    return mb;
}

int plat_display_modes(int (*wh)[2], int max)
{
    /* the three the G3-era displays all take (largest first, as SDL lists them); 1024 x 768 only with 16 MB of video
     * memory (glview.c caps a mission the same way) */
    static const int m[3][2] = {{1024, 768}, {800, 600}, {640, 480}};
    int k, n = 0, first = mw2_mac_vram_mb() >= 16 ? 0 : 1;
    for (k = first; k < 3 && n < max; k++, n++) { wh[n][0] = m[k][0]; wh[n][1] = m[k][1]; }
    return n;
}
void plat_desktop_mode(int *w, int *h) { Rect s = screen_rect(); *w = s.right - s.left; *h = s.bottom - s.top; }
