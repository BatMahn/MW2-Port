/*
 * SDL.h - the subset of SDL3's API the port uses, for the classic Mac OS / Carbon build (platform/carbon/sdl_carbon.c
 * implements it on Carbon, AGL and the Sound Manager). Prototypes and constant values are SDL 3.4's, taken from its
 * headers (the game's code compiles unchanged against either); only what the game calls is here.
 */
#ifndef MW2_CARBON_SDL_H
#define MW2_CARBON_SDL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDLCALL
#define SDL_DECLSPEC
#define SDL_UINT64_C(c) c##ULL
#define SDL_MAIN_HANDLED 1

typedef int8_t Sint8; typedef uint8_t Uint8; typedef int16_t Sint16; typedef uint16_t Uint16;
typedef int32_t Sint32; typedef uint32_t Uint32; typedef int64_t Sint64; typedef uint64_t Uint64;

#define SDL_zero(x) memset(&(x), 0, sizeof(x))
#define SDL_free free
#define SDL_BUTTON_MASK(X) (1u << ((X) - 1))

typedef Uint32 SDL_InitFlags, SDL_PropertiesID, SDL_DisplayID, SDL_WindowID, SDL_JoystickID, SDL_KeyboardID, SDL_MouseID, SDL_AudioDeviceID;
typedef Uint64 SDL_WindowFlags;
typedef Uint32 SDL_Keycode;
typedef Uint16 SDL_Keymod;
typedef Uint32 SDL_MouseButtonFlags;
typedef Uint32 SDL_PixelFormat;
typedef int SDL_Scancode;
typedef int SDL_ScaleMode, SDL_TextureAccess, SDL_RendererLogicalPresentation, SDL_GLAttr, SDL_MessageBoxFlags, SDL_MouseWheelDirection, SDL_AudioFormat;
typedef Uint32 SDL_BlendMode, SDL_EventType;
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef struct SDL_Joystick SDL_Joystick;
typedef struct SDL_AudioStream SDL_AudioStream;
typedef void *SDL_GLContext;
typedef void (*SDL_FunctionPointer)(void);

typedef struct SDL_Rect { int x, y, w, h; } SDL_Rect;
typedef struct SDL_FRect { float x, y, w, h; } SDL_FRect;
typedef struct SDL_DisplayMode { SDL_DisplayID displayID; SDL_PixelFormat format; int w, h; float pixel_density, refresh_rate; int refresh_rate_numerator, refresh_rate_denominator; void *internal; } SDL_DisplayMode;
typedef struct SDL_Surface { Uint32 flags; SDL_PixelFormat format; int w, h, pitch; void *pixels; int refcount; void *reserved; } SDL_Surface;
typedef struct SDL_AudioSpec { SDL_AudioFormat format; int channels, freq; } SDL_AudioSpec;
typedef void (SDLCALL *SDL_AudioStreamCallback)(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount);

typedef struct SDL_CommonEvent { Uint32 type, reserved; Uint64 timestamp; } SDL_CommonEvent;
typedef struct SDL_KeyboardEvent { SDL_EventType type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; SDL_KeyboardID which; SDL_Scancode scancode; SDL_Keycode key; SDL_Keymod mod; Uint16 raw; bool down, repeat; } SDL_KeyboardEvent;
typedef struct SDL_TextInputEvent { SDL_EventType type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; const char *text; } SDL_TextInputEvent;
typedef struct SDL_MouseMotionEvent { SDL_EventType type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; SDL_MouseID which; SDL_MouseButtonFlags state; float x, y, xrel, yrel; } SDL_MouseMotionEvent;
typedef struct SDL_MouseButtonEvent { SDL_EventType type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; SDL_MouseID which; Uint8 button; bool down; Uint8 clicks, padding; float x, y; } SDL_MouseButtonEvent;
typedef struct SDL_MouseWheelEvent { SDL_EventType type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; SDL_MouseID which; float x, y; SDL_MouseWheelDirection direction; float mouse_x, mouse_y; Sint32 integer_x, integer_y; } SDL_MouseWheelEvent;
typedef struct SDL_QuitEvent { SDL_EventType type; Uint32 reserved; Uint64 timestamp; } SDL_QuitEvent;
typedef union SDL_Event {
    Uint32 type;
    SDL_CommonEvent common;
    SDL_KeyboardEvent key;
    SDL_TextInputEvent text;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    SDL_QuitEvent quit;
    Uint8 padding[128];
} SDL_Event;

/* events */
#define SDL_EVENT_QUIT 0x100
#define SDL_EVENT_KEY_DOWN 0x300
#define SDL_EVENT_KEY_UP 0x301
#define SDL_EVENT_TEXT_EDITING 0x302
#define SDL_EVENT_TEXT_INPUT 0x303
#define SDL_EVENT_MOUSE_MOTION 0x400
#define SDL_EVENT_MOUSE_BUTTON_DOWN 0x401
#define SDL_EVENT_MOUSE_BUTTON_UP 0x402
#define SDL_EVENT_MOUSE_WHEEL 0x403
/* render / texture / GL enums (SDL 3.4's order) */
#define SDL_SCALEMODE_NEAREST 0
#define SDL_SCALEMODE_LINEAR 1
#define SDL_TEXTUREACCESS_STATIC 0
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_LOGICAL_PRESENTATION_DISABLED 0
#define SDL_LOGICAL_PRESENTATION_STRETCH 1
#define SDL_LOGICAL_PRESENTATION_LETTERBOX 2
#define SDL_LOGICAL_PRESENTATION_OVERSCAN 3
#define SDL_LOGICAL_PRESENTATION_INTEGER_SCALE 4
#define SDL_GL_RED_SIZE 0
#define SDL_GL_GREEN_SIZE 1
#define SDL_GL_BLUE_SIZE 2
#define SDL_GL_DOUBLEBUFFER 5
#define SDL_GL_DEPTH_SIZE 6
#define SDL_GL_CONTEXT_MAJOR_VERSION 17
#define SDL_GL_CONTEXT_MINOR_VERSION 18
/* audio: native-endian float (PowerPC: big) */
#define SDL_AUDIO_F32 0x8120
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ((SDL_AudioDeviceID) 0xFFFFFFFFu)
#define SDL_BLENDMODE_BLEND 0x00000001u
#define SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES "SDL_AUDIO_DEVICE_SAMPLE_FRAMES"
#define SDL_HINT_RENDER_DRIVER "SDL_RENDER_DRIVER"
#define SDL_INIT_AUDIO 0x00000010u
#define SDL_INIT_JOYSTICK 0x00000200u
#define SDL_INIT_VIDEO 0x00000020u
#define SDL_MESSAGEBOX_ERROR 0x00000010u
#define SDL_MESSAGEBOX_WARNING 0x00000020u
#define SDL_PIXELFORMAT_ABGR8888 (0x16762004u)
#define SDL_PIXELFORMAT_RGB24 (0x17101803u)
#define SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER "SDL.window.create.flags"
#define SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER "SDL.window.create.height"
#define SDL_PROP_WINDOW_CREATE_TITLE_STRING "SDL.window.create.title"
#define SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER "SDL.window.create.width"
#define SDL_PROP_WINDOW_CREATE_X_NUMBER "SDL.window.create.x"
#define SDL_PROP_WINDOW_CREATE_Y_NUMBER "SDL.window.create.y"
#define SDL_WINDOW_FULLSCREEN SDL_UINT64_C(0x0000000000000001)
#define SDL_WINDOW_HIDDEN SDL_UINT64_C(0x0000000000000008)
#define SDL_WINDOW_HIGH_PIXEL_DENSITY SDL_UINT64_C(0x0000000000002000)
#define SDL_WINDOW_INPUT_FOCUS SDL_UINT64_C(0x0000000000000200)
#define SDL_WINDOW_MOUSE_FOCUS SDL_UINT64_C(0x0000000000000400)
#define SDL_WINDOW_OPENGL SDL_UINT64_C(0x0000000000000002)
#define SDL_WINDOW_RESIZABLE SDL_UINT64_C(0x0000000000000020)
#define SDL_SCANCODE_COUNT 512
#define SDLK_0 0x00000030u
#define SDLK_9 0x00000039u
#define SDLK_APOSTROPHE 0x00000027u
#define SDLK_B 0x00000062u
#define SDLK_BACKSLASH 0x0000005cu
#define SDLK_BACKSPACE 0x00000008u
#define SDLK_C 0x00000063u
#define SDLK_CAPSLOCK 0x40000039u
#define SDLK_COMMA 0x0000002cu
#define SDLK_DELETE 0x0000007fu
#define SDLK_DOWN 0x40000051u
#define SDLK_END 0x4000004du
#define SDLK_EQUALS 0x0000003du
#define SDLK_ESCAPE 0x0000001bu
#define SDLK_F 0x00000066u
#define SDLK_F1 0x4000003au
#define SDLK_F12 0x40000045u
#define SDLK_G 0x00000067u
#define SDLK_GRAVE 0x00000060u
#define SDLK_HOME 0x4000004au
#define SDLK_I 0x00000069u
#define SDLK_INSERT 0x40000049u
#define SDLK_KP_0 0x40000062u
#define SDLK_KP_1 0x40000059u
#define SDLK_KP_2 0x4000005au
#define SDLK_KP_4 0x4000005cu
#define SDLK_KP_5 0x4000005du
#define SDLK_KP_6 0x4000005eu
#define SDLK_KP_7 0x4000005fu
#define SDLK_KP_8 0x40000060u
#define SDLK_KP_9 0x40000061u
#define SDLK_KP_DIVIDE 0x40000054u
#define SDLK_KP_ENTER 0x40000058u
#define SDLK_KP_MULTIPLY 0x40000055u
#define SDLK_KP_PLUS 0x40000057u
#define SDLK_LALT 0x400000e2u
#define SDLK_LCTRL 0x400000e0u
#define SDLK_LEFT 0x40000050u
#define SDLK_LEFTBRACKET 0x0000005bu
#define SDLK_LGUI 0x400000e3u
#define SDLK_LSHIFT 0x400000e1u
#define SDLK_M 0x0000006du
#define SDLK_MINUS 0x0000002du
#define SDLK_MODE 0x40000101u
#define SDLK_N 0x0000006eu
#define SDLK_NUMLOCKCLEAR 0x40000053u
#define SDLK_P 0x00000070u
#define SDLK_PAGEDOWN 0x4000004eu
#define SDLK_PAGEUP 0x4000004bu
#define SDLK_PAUSE 0x40000048u
#define SDLK_PERIOD 0x0000002eu
#define SDLK_RALT 0x400000e6u
#define SDLK_RCTRL 0x400000e4u
#define SDLK_RETURN 0x0000000du
#define SDLK_RGUI 0x400000e7u
#define SDLK_RIGHT 0x4000004fu
#define SDLK_RIGHTBRACKET 0x0000005du
#define SDLK_RSHIFT 0x400000e5u
#define SDLK_SCROLLLOCK 0x40000047u
#define SDLK_SEMICOLON 0x0000003bu
#define SDLK_SLASH 0x0000002fu
#define SDLK_SPACE 0x00000020u
#define SDLK_TAB 0x00000009u
#define SDLK_UNKNOWN 0x00000000u
#define SDLK_UP 0x40000052u
#define SDLK_X 0x00000078u
#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3
#define SDL_KMOD_ALT (SDL_KMOD_LALT | SDL_KMOD_RALT)
#define SDL_KMOD_CTRL (SDL_KMOD_LCTRL | SDL_KMOD_RCTRL)
#define SDL_KMOD_GUI (SDL_KMOD_LGUI | SDL_KMOD_RGUI)
#define SDL_KMOD_LALT 0x0100u
#define SDL_KMOD_LCTRL 0x0040u
#define SDL_KMOD_LSHIFT 0x0001u
#define SDL_KMOD_SHIFT (SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT)
#define SDL_SCANCODE_0 39
#define SDL_SCANCODE_1 30
#define SDL_SCANCODE_2 31
#define SDL_SCANCODE_3 32
#define SDL_SCANCODE_4 33
#define SDL_SCANCODE_5 34
#define SDL_SCANCODE_6 35
#define SDL_SCANCODE_7 36
#define SDL_SCANCODE_8 37
#define SDL_SCANCODE_9 38
#define SDL_SCANCODE_A 4
#define SDL_SCANCODE_APOSTROPHE 52
#define SDL_SCANCODE_B 5
#define SDL_SCANCODE_BACKSLASH 49
#define SDL_SCANCODE_BACKSPACE 42
#define SDL_SCANCODE_C 6
#define SDL_SCANCODE_CAPSLOCK 57
#define SDL_SCANCODE_CLEAR 156
#define SDL_SCANCODE_COMMA 54
#define SDL_SCANCODE_CRSEL 163
#define SDL_SCANCODE_D 7
#define SDL_SCANCODE_DELETE 76
#define SDL_SCANCODE_DOWN 81
#define SDL_SCANCODE_E 8
#define SDL_SCANCODE_END 77
#define SDL_SCANCODE_EQUALS 46
#define SDL_SCANCODE_ESCAPE 41
#define SDL_SCANCODE_EXSEL 164
#define SDL_SCANCODE_F 9
#define SDL_SCANCODE_F1 58
#define SDL_SCANCODE_F10 67
#define SDL_SCANCODE_F11 68
#define SDL_SCANCODE_F12 69
#define SDL_SCANCODE_F13 104
#define SDL_SCANCODE_F14 105
#define SDL_SCANCODE_F15 106
#define SDL_SCANCODE_F2 59
#define SDL_SCANCODE_F21 112
#define SDL_SCANCODE_F22 113
#define SDL_SCANCODE_F23 114
#define SDL_SCANCODE_F24 115
#define SDL_SCANCODE_F3 60
#define SDL_SCANCODE_F4 61
#define SDL_SCANCODE_F5 62
#define SDL_SCANCODE_F6 63
#define SDL_SCANCODE_F7 64
#define SDL_SCANCODE_F8 65
#define SDL_SCANCODE_F9 66
#define SDL_SCANCODE_G 10
#define SDL_SCANCODE_GRAVE 53
#define SDL_SCANCODE_H 11
#define SDL_SCANCODE_HOME 74
#define SDL_SCANCODE_I 12
#define SDL_SCANCODE_INSERT 73
#define SDL_SCANCODE_J 13
#define SDL_SCANCODE_K 14
#define SDL_SCANCODE_KP_0 98
#define SDL_SCANCODE_KP_1 89
#define SDL_SCANCODE_KP_2 90
#define SDL_SCANCODE_KP_3 91
#define SDL_SCANCODE_KP_4 92
#define SDL_SCANCODE_KP_5 93
#define SDL_SCANCODE_KP_6 94
#define SDL_SCANCODE_KP_7 95
#define SDL_SCANCODE_KP_8 96
#define SDL_SCANCODE_KP_9 97
#define SDL_SCANCODE_KP_DIVIDE 84
#define SDL_SCANCODE_KP_ENTER 88
#define SDL_SCANCODE_KP_MINUS 86
#define SDL_SCANCODE_KP_MULTIPLY 85
#define SDL_SCANCODE_KP_PERIOD 99
#define SDL_SCANCODE_KP_PLUS 87
#define SDL_SCANCODE_L 15
#define SDL_SCANCODE_LALT 226
#define SDL_SCANCODE_LCTRL 224
#define SDL_SCANCODE_LEFT 80
#define SDL_SCANCODE_LEFTBRACKET 47
#define SDL_SCANCODE_LSHIFT 225
#define SDL_SCANCODE_M 16
#define SDL_SCANCODE_MINUS 45
#define SDL_SCANCODE_N 17
#define SDL_SCANCODE_NONUSBACKSLASH 100
#define SDL_SCANCODE_NUMLOCKCLEAR 83
#define SDL_SCANCODE_O 18
#define SDL_SCANCODE_P 19
#define SDL_SCANCODE_PAGEDOWN 78
#define SDL_SCANCODE_PAGEUP 75
#define SDL_SCANCODE_PAUSE 72
#define SDL_SCANCODE_PERIOD 55
#define SDL_SCANCODE_Q 20
#define SDL_SCANCODE_R 21
#define SDL_SCANCODE_RALT 230
#define SDL_SCANCODE_RCTRL 228
#define SDL_SCANCODE_RETURN 40
#define SDL_SCANCODE_RIGHT 79
#define SDL_SCANCODE_RIGHTBRACKET 48
#define SDL_SCANCODE_RSHIFT 229
#define SDL_SCANCODE_S 22
#define SDL_SCANCODE_SCROLLLOCK 71
#define SDL_SCANCODE_SEMICOLON 51
#define SDL_SCANCODE_SLASH 56
#define SDL_SCANCODE_SPACE 44
#define SDL_SCANCODE_SYSREQ 154
#define SDL_SCANCODE_T 23
#define SDL_SCANCODE_TAB 43
#define SDL_SCANCODE_U 24
#define SDL_SCANCODE_UNKNOWN 0
#define SDL_SCANCODE_UP 82
#define SDL_SCANCODE_V 25
#define SDL_SCANCODE_W 26
#define SDL_SCANCODE_X 27
#define SDL_SCANCODE_Y 28
#define SDL_SCANCODE_Z 29

#define SDL_KMOD_RSHIFT 0x0002u
#define SDL_KMOD_RCTRL 0x0080u
#define SDL_KMOD_RALT 0x0200u
#define SDL_KMOD_LGUI 0x0400u
#define SDL_KMOD_RGUI 0x0800u
#define SDL_HAT_CENTERED 0x00u
#define SDL_HAT_UP 0x01u
#define SDL_HAT_RIGHT 0x02u
#define SDL_HAT_DOWN 0x04u
#define SDL_HAT_LEFT 0x08u
#define SDL_WINDOWPOS_CENTERED_MASK 0x2FFF0000u
#define SDL_WINDOWPOS_CENTERED_DISPLAY(X) (SDL_WINDOWPOS_CENTERED_MASK|(X))
#define SDL_WINDOWPOS_CENTERED SDL_WINDOWPOS_CENTERED_DISPLAY(0)

/* the rest of SDL 3.4's scancodes */
#define SDL_SCANCODE_NONUSHASH 50
#define SDL_SCANCODE_PRINTSCREEN 70
#define SDL_SCANCODE_APPLICATION 101
#define SDL_SCANCODE_POWER 102
#define SDL_SCANCODE_KP_EQUALS 103
#define SDL_SCANCODE_F16 107
#define SDL_SCANCODE_F17 108
#define SDL_SCANCODE_F18 109
#define SDL_SCANCODE_F19 110
#define SDL_SCANCODE_F20 111
#define SDL_SCANCODE_EXECUTE 116
#define SDL_SCANCODE_HELP 117
#define SDL_SCANCODE_MENU 118
#define SDL_SCANCODE_SELECT 119
#define SDL_SCANCODE_STOP 120
#define SDL_SCANCODE_AGAIN 121
#define SDL_SCANCODE_UNDO 122
#define SDL_SCANCODE_CUT 123
#define SDL_SCANCODE_COPY 124
#define SDL_SCANCODE_PASTE 125
#define SDL_SCANCODE_FIND 126
#define SDL_SCANCODE_MUTE 127
#define SDL_SCANCODE_VOLUMEUP 128
#define SDL_SCANCODE_VOLUMEDOWN 129
#define SDL_SCANCODE_LOCKINGCAPSLOCK 130
#define SDL_SCANCODE_LOCKINGNUMLOCK 131
#define SDL_SCANCODE_LOCKINGSCROLLLOCK 132
#define SDL_SCANCODE_KP_COMMA 133
#define SDL_SCANCODE_KP_EQUALSAS400 134
#define SDL_SCANCODE_INTERNATIONAL1 135
#define SDL_SCANCODE_INTERNATIONAL2 136
#define SDL_SCANCODE_INTERNATIONAL3 137
#define SDL_SCANCODE_INTERNATIONAL4 138
#define SDL_SCANCODE_INTERNATIONAL5 139
#define SDL_SCANCODE_INTERNATIONAL6 140
#define SDL_SCANCODE_INTERNATIONAL7 141
#define SDL_SCANCODE_INTERNATIONAL8 142
#define SDL_SCANCODE_INTERNATIONAL9 143
#define SDL_SCANCODE_LANG1 144
#define SDL_SCANCODE_LANG2 145
#define SDL_SCANCODE_LANG3 146
#define SDL_SCANCODE_LANG4 147
#define SDL_SCANCODE_LANG5 148
#define SDL_SCANCODE_LANG6 149
#define SDL_SCANCODE_LANG7 150
#define SDL_SCANCODE_LANG8 151
#define SDL_SCANCODE_LANG9 152
#define SDL_SCANCODE_ALTERASE 153
#define SDL_SCANCODE_CANCEL 155
#define SDL_SCANCODE_PRIOR 157
#define SDL_SCANCODE_RETURN2 158
#define SDL_SCANCODE_SEPARATOR 159
#define SDL_SCANCODE_OUT 160
#define SDL_SCANCODE_OPER 161
#define SDL_SCANCODE_CLEARAGAIN 162
#define SDL_SCANCODE_KP_00 176
#define SDL_SCANCODE_KP_000 177
#define SDL_SCANCODE_THOUSANDSSEPARATOR 178
#define SDL_SCANCODE_DECIMALSEPARATOR 179
#define SDL_SCANCODE_CURRENCYUNIT 180
#define SDL_SCANCODE_CURRENCYSUBUNIT 181
#define SDL_SCANCODE_KP_LEFTPAREN 182
#define SDL_SCANCODE_KP_RIGHTPAREN 183
#define SDL_SCANCODE_KP_LEFTBRACE 184
#define SDL_SCANCODE_KP_RIGHTBRACE 185
#define SDL_SCANCODE_KP_TAB 186
#define SDL_SCANCODE_KP_BACKSPACE 187
#define SDL_SCANCODE_KP_A 188
#define SDL_SCANCODE_KP_B 189
#define SDL_SCANCODE_KP_C 190
#define SDL_SCANCODE_KP_D 191
#define SDL_SCANCODE_KP_E 192
#define SDL_SCANCODE_KP_F 193
#define SDL_SCANCODE_KP_XOR 194
#define SDL_SCANCODE_KP_POWER 195
#define SDL_SCANCODE_KP_PERCENT 196
#define SDL_SCANCODE_KP_LESS 197
#define SDL_SCANCODE_KP_GREATER 198
#define SDL_SCANCODE_KP_AMPERSAND 199
#define SDL_SCANCODE_KP_DBLAMPERSAND 200
#define SDL_SCANCODE_KP_VERTICALBAR 201
#define SDL_SCANCODE_KP_DBLVERTICALBAR 202
#define SDL_SCANCODE_KP_COLON 203
#define SDL_SCANCODE_KP_HASH 204
#define SDL_SCANCODE_KP_SPACE 205
#define SDL_SCANCODE_KP_AT 206
#define SDL_SCANCODE_KP_EXCLAM 207
#define SDL_SCANCODE_KP_MEMSTORE 208
#define SDL_SCANCODE_KP_MEMRECALL 209
#define SDL_SCANCODE_KP_MEMCLEAR 210
#define SDL_SCANCODE_KP_MEMADD 211
#define SDL_SCANCODE_KP_MEMSUBTRACT 212
#define SDL_SCANCODE_KP_MEMMULTIPLY 213
#define SDL_SCANCODE_KP_MEMDIVIDE 214
#define SDL_SCANCODE_KP_PLUSMINUS 215
#define SDL_SCANCODE_KP_CLEAR 216
#define SDL_SCANCODE_KP_CLEARENTRY 217
#define SDL_SCANCODE_KP_BINARY 218
#define SDL_SCANCODE_KP_OCTAL 219
#define SDL_SCANCODE_KP_DECIMAL 220
#define SDL_SCANCODE_KP_HEXADECIMAL 221
#define SDL_SCANCODE_LGUI 227
#define SDL_SCANCODE_RGUI 231
#define SDL_SCANCODE_MODE 257
#define SDL_SCANCODE_SLEEP 258
#define SDL_SCANCODE_WAKE 259
#define SDL_SCANCODE_CHANNEL_INCREMENT 260
#define SDL_SCANCODE_CHANNEL_DECREMENT 261
#define SDL_SCANCODE_MEDIA_PLAY 262
#define SDL_SCANCODE_MEDIA_PAUSE 263
#define SDL_SCANCODE_MEDIA_RECORD 264
#define SDL_SCANCODE_MEDIA_FAST_FORWARD 265
#define SDL_SCANCODE_MEDIA_REWIND 266
#define SDL_SCANCODE_MEDIA_NEXT_TRACK 267
#define SDL_SCANCODE_MEDIA_PREVIOUS_TRACK 268
#define SDL_SCANCODE_MEDIA_STOP 269
#define SDL_SCANCODE_MEDIA_EJECT 270
#define SDL_SCANCODE_MEDIA_PLAY_PAUSE 271
#define SDL_SCANCODE_MEDIA_SELECT 272
#define SDL_SCANCODE_AC_NEW 273
#define SDL_SCANCODE_AC_OPEN 274
#define SDL_SCANCODE_AC_CLOSE 275
#define SDL_SCANCODE_AC_EXIT 276
#define SDL_SCANCODE_AC_SAVE 277
#define SDL_SCANCODE_AC_PRINT 278
#define SDL_SCANCODE_AC_PROPERTIES 279
#define SDL_SCANCODE_AC_SEARCH 280
#define SDL_SCANCODE_AC_HOME 281
#define SDL_SCANCODE_AC_BACK 282
#define SDL_SCANCODE_AC_FORWARD 283
#define SDL_SCANCODE_AC_STOP 284
#define SDL_SCANCODE_AC_REFRESH 285
#define SDL_SCANCODE_AC_BOOKMARKS 286
#define SDL_SCANCODE_SOFTLEFT 287
#define SDL_SCANCODE_SOFTRIGHT 288
#define SDL_SCANCODE_CALL 289
#define SDL_SCANCODE_ENDCALL 290
#define SDL_SCANCODE_RESERVED 400

/* functions (SDL 3.4 prototypes) */
void SDL_CloseJoystick(SDL_Joystick *joystick);
bool SDL_ConvertEventToRenderCoordinates(SDL_Renderer *renderer, SDL_Event *event);
SDL_Surface * SDL_ConvertSurface(SDL_Surface *surface, SDL_PixelFormat format);
SDL_PropertiesID SDL_CreateProperties(void);
SDL_Renderer * SDL_CreateRenderer(SDL_Window *window, const char *name);
SDL_Texture * SDL_CreateTexture(SDL_Renderer *renderer, SDL_PixelFormat format, SDL_TextureAccess access, int w, int h);
SDL_Window * SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags);
SDL_Window * SDL_CreateWindowWithProperties(SDL_PropertiesID props);
void SDL_Delay(Uint32 ms);
void SDL_DestroyAudioStream(SDL_AudioStream *stream);
void SDL_DestroyProperties(SDL_PropertiesID props);
void SDL_DestroyRenderer(SDL_Renderer *renderer);
void SDL_DestroySurface(SDL_Surface *surface);
void SDL_DestroyTexture(SDL_Texture *texture);
void SDL_DestroyWindow(SDL_Window *window);
SDL_GLContext SDL_GL_CreateContext(SDL_Window *window);
bool SDL_GL_DestroyContext(SDL_GLContext context);
SDL_FunctionPointer SDL_GL_GetProcAddress(const char *proc);
bool SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context);
bool SDL_GL_SetAttribute(SDL_GLAttr attr, int value);
bool SDL_GL_SetSwapInterval(int interval);
bool SDL_GL_SwapWindow(SDL_Window *window);
const char * SDL_GetBasePath(void);
bool SDL_GetClosestFullscreenDisplayMode(SDL_DisplayID displayID, int w, int h, float refresh_rate, bool include_high_density_modes, SDL_DisplayMode *closest);
const char * SDL_GetCurrentAudioDriver(void);
const char * SDL_GetCurrentVideoDriver(void);
const SDL_DisplayMode * SDL_GetDesktopDisplayMode(SDL_DisplayID displayID);
SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *window);
const char * SDL_GetError(void);
SDL_DisplayMode ** SDL_GetFullscreenDisplayModes(SDL_DisplayID displayID, int *count);
const char * SDL_GetHint(const char *name);
Sint16 SDL_GetJoystickAxis(SDL_Joystick *joystick, int axis);
bool SDL_GetJoystickButton(SDL_Joystick *joystick, int button);
Uint8 SDL_GetJoystickHat(SDL_Joystick *joystick, int hat);
SDL_JoystickID * SDL_GetJoysticks(int *count);
const bool * SDL_GetKeyboardState(int *numkeys);
SDL_Keymod SDL_GetModState(void);
SDL_MouseButtonFlags SDL_GetMouseState(float *x, float *y);
int SDL_GetNumJoystickAxes(SDL_Joystick *joystick);
int SDL_GetNumJoystickButtons(SDL_Joystick *joystick);
int SDL_GetNumJoystickHats(SDL_Joystick *joystick);
Uint64 SDL_GetPerformanceCounter(void);
Uint64 SDL_GetPerformanceFrequency(void);
SDL_DisplayID SDL_GetPrimaryDisplay(void);
bool SDL_GetRenderLogicalPresentation(SDL_Renderer *renderer, int *w, int *h, SDL_RendererLogicalPresentation *mode);
bool SDL_GetRenderOutputSize(SDL_Renderer *renderer, int *w, int *h);
const char * SDL_GetRendererName(SDL_Renderer *renderer);
SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key, SDL_Keymod *modstate);
Uint64 SDL_GetTicks(void);
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window);
bool SDL_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h);
bool SDL_HideWindow(SDL_Window *window);
bool SDL_Init(SDL_InitFlags flags);
bool SDL_InitSubSystem(SDL_InitFlags flags);
bool SDL_LockAudioStream(SDL_AudioStream *stream);
SDL_AudioStream * SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec, SDL_AudioStreamCallback callback, void *userdata);
SDL_Joystick * SDL_OpenJoystick(SDL_JoystickID instance_id);
bool SDL_PollEvent(SDL_Event *event);
void SDL_PumpEvents(void);
bool SDL_PushEvent(SDL_Event *event);
bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *buf, int len);
void SDL_Quit(void);
bool SDL_RaiseWindow(SDL_Window *window);
bool SDL_RenderClear(SDL_Renderer *renderer);
bool SDL_RenderCoordinatesFromWindow(SDL_Renderer *renderer, float window_x, float window_y, float *x, float *y);
bool SDL_RenderPresent(SDL_Renderer *renderer);
SDL_Surface * SDL_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect);
bool SDL_RenderTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_FRect *srcrect, const SDL_FRect *dstrect);
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream);
bool SDL_SetBooleanProperty(SDL_PropertiesID props, const char *name, bool value);
bool SDL_SetHint(const char *name, const char *value);
bool SDL_SetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 value);
bool SDL_SetRenderLogicalPresentation(SDL_Renderer *renderer, int w, int h, SDL_RendererLogicalPresentation mode);
bool SDL_SetRenderVSync(SDL_Renderer *renderer, int vsync);
bool SDL_SetStringProperty(SDL_PropertiesID props, const char *name, const char *value);
bool SDL_SetTextureBlendMode(SDL_Texture *texture, SDL_BlendMode blendMode);
bool SDL_SetTextureScaleMode(SDL_Texture *texture, SDL_ScaleMode scaleMode);
bool SDL_SetWindowFullscreen(SDL_Window *window, bool fullscreen);
bool SDL_SetWindowFullscreenMode(SDL_Window *window, const SDL_DisplayMode *mode);
bool SDL_SetWindowRelativeMouseMode(SDL_Window *window, bool enabled);
bool SDL_SetWindowSize(SDL_Window *window, int w, int h);
bool SDL_SetWindowTitle(SDL_Window *window, const char *title);
bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags, const char *title, const char *message, SDL_Window *window);
bool SDL_ShowWindow(SDL_Window *window);
bool SDL_StartTextInput(SDL_Window *window);
bool SDL_StopTextInput(SDL_Window *window);
bool SDL_SyncWindow(SDL_Window *window);
bool SDL_TextInputActive(SDL_Window *window);
bool SDL_UnlockAudioStream(SDL_AudioStream *stream);
bool SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch);
bool SDL_LockTexture(SDL_Texture *texture, const SDL_Rect *rect, void **pixels, int *pitch);
void SDL_UnlockTexture(SDL_Texture *texture);
bool SDL_GetTextureSize(SDL_Texture *texture, float *w, float *h);
bool SDL_SetRenderClipRect(SDL_Renderer *renderer, const SDL_Rect *rect);

/* threads: the classic Mac build runs a "thread" to its end at once (one CPU; SDL_GetNumLogicalCPUCores says 1) */
typedef struct SDL_Thread SDL_Thread;
typedef int (SDLCALL *SDL_ThreadFunction)(void *data);
SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data);
void SDL_WaitThread(SDL_Thread *thread, int *status);
int SDL_GetNumLogicalCPUCores(void);
#define SDL_snprintf snprintf   /* (<stdio.h>) */


#ifdef __cplusplus
}
#endif
#endif
