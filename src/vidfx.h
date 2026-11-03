/* vidfx.h - optional CPU movie presentation filters for the shell's full-screen cinematics (mw2port.cfg videos=).
 * A PORT PRESENTATION OPTION, not original behaviour: the original shows its movies in plain pixels (ORIGINAL here).
 * Adapted from the Mercenaries fork of this port (src/vidfx.c / vidfx.h there, adapted from the supplied GBL port).
 * These are port presentation choices, not reconstructed MechWarrior 2 rendering
 * rules. Input is palette-indexed; output is RGB24 at the requested pixel size.
 * ORIGINAL uses solid source-pixel blocks; SMOOTH uses Mitchell-Netravali taps;
 * SCANLINES uses narrow source-line beams; CRT adds bloom and an aperture grille;
 * CRT_CURVED adds the GBL filter's two-pass curved-screen approximation.
 */
#ifndef MW2_VIDFX_H
#define MW2_VIDFX_H
#include <stdint.h>
enum { VIDFX_ORIGINAL, VIDFX_SMOOTH, VIDFX_SCANLINES, VIDFX_CRT, VIDFX_CRT_CURVED, VIDFX_COUNT };
typedef struct vidfx vidfx;
vidfx *vidfx_new(void);
void vidfx_free(vidfx *v);
/* Return 0 on success, -1 for invalid dimensions/pitch or allocation failure.
 * Output must be at least the source size; the presenter can downscale later.
 * On error the output is unspecified and must not be presented as a good frame.
 * Source dimensions are bounded to 4096; output dimensions to 8192.
 */
int vidfx_render(vidfx *v, int mode, const uint8_t *pix, int sw, int sh,
                 const uint8_t (*pal)[3], uint8_t *out, int ow, int oh, int pitch);
/* The same in two steps, so the output rows can be drawn on several threads: vidfx_prepare does the frame's source-sized
 * work (0, or -1 as vidfx_render), then vidfx_rows draws output rows y0 .. y1-1 (0 <= y0) into out (row y at out + (y - y0)
 * pitch: out is row y0's start); calls for separate bands may run at the same time. Same pixels as vidfx_render. */
int vidfx_prepare(vidfx *v, int mode, const uint8_t *pix, int sw, int sh, const uint8_t (*pal)[3], int ow, int oh);
int vidfx_rows(vidfx *v, uint8_t *out, int pitch, int y0, int y1);
#endif
