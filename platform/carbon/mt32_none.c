/* mt32_none.c - the classic Mac build has no MT-32 emulator (its menu music: QuickTime's synth or Gravis patches);
 * the shell's MT-32 calls find none */
#include <stddef.h>
#include "mt32mid.h"
mt32_synth *mt32_open(const char *rom_dir, int rate) { (void)rom_dir; (void)rate; return NULL; }
void mt32_close(mt32_synth *s) { (void)s; }
mt32_song *mt32_song_open(mt32_synth *syn, const uint8_t *xmi, size_t len) { (void)syn; (void)xmi; (void)len; return NULL; }
void mt32_song_close(mt32_song *s) { (void)s; }
int mt32_song_render(mt32_song *s, float *out, int frames) { (void)s; (void)out; (void)frames; return 0; }
