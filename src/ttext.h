/* TrueType text for the port's own additions (stb_truetype): a string rendered to an 8-bit coverage image.
 * The font: assets/fonts/LiberationSans-Bold.ttf (next to the executable, the working directory, or the system's). */
#ifndef TTEXT_H
#define TTEXT_H
#include <stdint.h>
int  ttext_init(const char *base_dir);   /* 0 if a font was found */
int  ttext_available(void);
/* Render: cap height in pixels, horizontal scale (1 = natural). Returns a malloc'd w x h coverage image (0..255),
 * *baseline_y = the row of the baseline. NULL if no font. */
uint8_t *ttext_render(const char *s, float cap_px, float hscale, int *w, int *h, int *baseline_y);
/* the same with `track` extra pixels after every letter (letter spacing) */
uint8_t *ttext_render_track(const char *s, float cap_px, float hscale, float track, int *w, int *h, int *baseline_y);
/* the advance width in pixels of s at that cap height and horizontal scale */
float ttext_width(const char *s, float cap_px, float hscale);
#endif
