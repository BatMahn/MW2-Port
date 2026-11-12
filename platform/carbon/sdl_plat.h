/*
 * sdl_plat.h - what the SDL subset (sdl_core.c) needs from the system: sdl_plat_carbon.c on classic Mac OS / Carbon
 * (WaitNextEvent, GetKeys, AGL, the Sound Manager), sdl_plat_egl.c on Linux for the tests (EGL pbuffer, no input,
 * silent audio). Everything here is called from the main thread except plat_audio's fill callback.
 */
#ifndef MW2_SDL_PLAT_H
#define MW2_SDL_PLAT_H

#include <stdint.h>

typedef struct plat_window plat_window;

int  plat_init(char *err, int errn);
void plat_quit(void);
uint64_t plat_ticks_us(void);           /* a monotonic microsecond clock */
void plat_delay_ms(uint32_t ms);
const char *plat_name(void);            /* "carbon", "egl" */

/* windows: size in pixels; fullscreen at w x h (the closest mode the display has) */
plat_window *plat_window_create(const char *title, int w, int h, int fullscreen, int hidden);
void plat_window_destroy(plat_window *w);
void plat_window_title(plat_window *w, const char *title);
void plat_window_size(plat_window *w, int *pw, int *ph);
int  plat_window_set_size(plat_window *w, int pw, int ph);
int  plat_window_fullscreen(plat_window *w, int on, int fw, int fh);
void plat_window_show(plat_window *w, int show);
int  plat_window_focused(plat_window *w);

/* OpenGL (1.2): a context on the window's drawable */
void *plat_gl_create(plat_window *w);
int  plat_gl_make_current(plat_window *w, void *ctx);
void plat_gl_destroy(void *ctx);
void plat_gl_swap(plat_window *w);
void *plat_gl_proc(const char *name);
void plat_gl_swap_interval(int n);

/* input: plat_pump reports through these (sdl_core.c) */
void core_key(int mac_vkey_or_scancode, int is_scancode, int down);
void core_text(const char *utf8);
void core_mouse_move(float x, float y, float dx, float dy);
void core_mouse_button(int button, int down, float x, float y);
void core_mouse_wheel(float dy);
void core_quit_request(void);
void plat_pump(void);
void plat_relative_mouse(plat_window *w, int on);
void plat_mouse_state(float *x, float *y, uint32_t *buttons);

/* audio: 16-bit signed stereo, native endian, at `rate`; fill(frames) is called by the system (interrupt time on
 * Mac OS 9: it may only copy) for `frames` frames into out */
typedef void (*plat_audio_fill)(int16_t *out, int frames);
int  plat_audio_open(int rate, int frames_per_buffer, plat_audio_fill fill);
void plat_audio_close(void);

void plat_message_box(int error, const char *title, const char *msg);
const char *plat_base_path(void);       /* the application's folder, '/'-separated, ending in '/' */

/* display modes (largest first) and the desktop's */
int  plat_display_modes(int (*wh)[2], int max);
void plat_desktop_mode(int *w, int *h);

#endif
