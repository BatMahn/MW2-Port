/*
 * carbon_extra.h - what the Carbon build uses that Retro68's open-source Multiversal headers don't declare. Retro68's
 * CarbonLib import library exports every one of these (CarbonLib carries CoreFoundation and QuickTime too), so only
 * the declarations are needed - no Apple Universal Interfaces. With Apple's headers instead (they don't define
 * M68K_INLINE) this file adds nothing.
 * Values and layouts are Apple's (Universal Interfaces 3.4).
 */
#ifndef MW2_CARBON_EXTRA_H
#define MW2_CARBON_EXTRA_H

#ifdef M68K_INLINE   /* Multiversal */
#include <stdint.h>

/* --- Toolbox */
typedef struct { uint32_t hi, lo; } UnsignedWide;
pascal void Microseconds(UnsignedWide *microTickCount);
pascal void HideMenuBar(void);
pascal void ShowMenuBar(void);
pascal void GetGlobalMouse(Point *globalMouse);
enum { kWindowContentRgn = 33 };
pascal OSStatus GetWindowBounds(WindowRef window, uint16_t regionCode, Rect *globalBounds);
enum { kAlertStopAlert = 0, kAlertNoteAlert = 1, kAlertCautionAlert = 2, kAlertPlainAlert = 3 };
pascal OSErr StandardAlert(int16_t alertType, ConstStr255Param error, ConstStr255Param explanation, const void *param, int16_t *outItemHit);
typedef unsigned char KeyMap[16];
enum { plainDBoxProc = 2, controlKey = ControlKey, fsRtDirID = 2, kMiddleC = 60, initStereo = 0x00C0 };
enum { kPowerPCCFragArch = 'pwpc' };
typedef ConnectionID CFragConnectionID;
typedef SymClass CFragSymbolClass;
typedef SndCallbackUPP SndCallBackUPP;
pascal SndCallBackUPP NewSndCallBackUPP(SndCallbackProcPtr userRoutine);
pascal void DisposeSndCallBackUPP(SndCallBackUPP userUPP);
pascal ComponentInstance OpenDefaultComponent(OSType componentType, OSType componentSubType);
pascal OSErr CloseComponent(ComponentInstance aComponentInstance);

/* Navigation Services' folder dialog */
pascal OSErr NavGetDefaultDialogOptions(NavDialogOptions *dialogOptions);
pascal OSErr NavChooseFolder(AEDesc *defaultLocation, NavReplyRecord *reply, NavDialogOptions *dialogOptions,
                             NavEventUPP eventProc, NavObjectFilterUPP filterProc, void *callBackUD);

/* --- CoreFoundation (C calls) */
typedef const void *CFAllocatorRef;
typedef const struct __CFString *CFStringRef;
typedef const struct __CFURL *CFURLRef;
typedef struct __CFBundle *CFBundleRef;
typedef uint32_t CFStringEncoding;
enum { kCFStringEncodingMacRoman = 0, kCFURLPOSIXPathStyle = 0 };
CFStringRef CFStringCreateWithCString(CFAllocatorRef alloc, const char *cStr, CFStringEncoding encoding);
CFURLRef CFURLCreateWithFileSystemPath(CFAllocatorRef allocator, CFStringRef filePath, long pathStyle, Boolean isDirectory);
CFBundleRef CFBundleCreate(CFAllocatorRef allocator, CFURLRef bundleURL);
Boolean CFBundleLoadExecutable(CFBundleRef bundle);
void *CFBundleGetFunctionPointerForName(CFBundleRef bundle, CFStringRef functionName);
void CFRelease(const void *cf);

/* --- QuickTime Music (QuickTimeMusic.h; C calls) */
typedef ComponentInstance NoteAllocator;
typedef struct OpaqueNoteChannel *NoteChannel;
typedef struct ToneDescription {
    OSType synthesizerType;
    Str31 synthesizerName;
    Str31 instrumentName;
    long instrumentNumber;
    long gmNumber;
} ToneDescription;
typedef struct NoteRequestInfo {
    uint8_t flags;
    uint8_t midiChannelAssignment;
    int16_t polyphony;
    Fixed typicalPolyphony;
} NoteRequestInfo;
typedef struct NoteRequest { NoteRequestInfo info; ToneDescription tone; } NoteRequest;
enum { kNoteAllocatorComponentType = 'nota', kFirstDrumkit = 16384 };
enum { kControllerModulationWheel = 1, kControllerVolume = 7, kControllerPan = 10, kControllerExpression = 11,
       kControllerPitchBend = 32, kControllerSustain = 64 };
long NAStuffToneDescription(NoteAllocator na, long gmNumber, ToneDescription *td);
long NANewNoteChannel(NoteAllocator na, NoteRequest *noteRequest, NoteChannel *outChannel);
long NADisposeNoteChannel(NoteAllocator na, NoteChannel noteChannel);
long NAPlayNote(NoteAllocator na, NoteChannel noteChannel, long pitch, long velocity);
long NASetController(NoteAllocator na, NoteChannel noteChannel, long controllerNumber, long controllerValue);
long NASetInstrumentNumber(NoteAllocator na, NoteChannel noteChannel, long instrumentNumber);

#endif   /* M68K_INLINE */
#endif
