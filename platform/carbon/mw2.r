/* mw2.r - the application's SIZE: 160 MB preferred, 128 MB minimum. Measured game heap at a mission's peak: 106 MB for the
 * 3Dfx / ATi / S3 looks (decoded textures kept after upload: 68 MB, the GL renderer's prepared copies 21 MB), 66 MB DOS,
 * 38 MB PowerVR; the rest is room for Apple's OpenGL, QuickTime and MP3 decoding. */
#include "Processes.r"
resource 'SIZE' (-1) {
    reserved, acceptSuspendResumeEvents, reserved, canBackground, doesActivateOnFGSwitch, backgroundAndForeground,
    dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible, isHighLevelEventAware, onlyLocalHLEvents,
    notStationeryAware, dontUseTextEditServices, reserved, reserved, reserved,
    160 * 1024 * 1024, 128 * 1024 * 1024
};
