/*
 * qtmidi.c - the classic Mac build's "built-in" menu music: the shell's General MIDI song set (the one the Gravis
 * path plays - DATABASE + 32, GM programs) sent to QuickTime's own synthesizer (the QuickTime Music Architecture's
 * note allocator: Roland GS-derived instruments, mixed by QuickTime on its own sound channel). gusmid.c's sequencer
 * (gus_song_open_sink) times the messages; it runs on the main thread (sdl_core.c's audio pump), so the note allocator
 * is never called at interrupt time.
 *
 * UNTESTED: written against QuickTimeMusic.h (Universal Interfaces 3.4); docs/CARBON.md lists what to check.
 */
#include <string.h>

#include <Carbon.h>
#include "carbon_extra.h"   /* QuickTime Music's declarations with Multiversal */
#ifndef M68K_INLINE
#include <QuickTimeComponents.h>
#include <QuickTimeMusic.h>
#endif

#include "qtmidi.h"

static NoteAllocator g_na;
static NoteChannel g_ch[16];
static int g_prog[16], g_brange[16], g_rpn[16][2];
static unsigned char g_on[16][128];   /* sounding notes (all-notes-off and close) */

int qtmidi_open(void)
{
    if (g_na) return 0;
    if (EnterMovies() != noErr) return -1;
    g_na = OpenDefaultComponent(kNoteAllocatorComponentType, 0);
    if (!g_na) return -1;
    memset(g_ch, 0, sizeof g_ch);
    { int c; for (c = 0; c < 16; c++) { g_prog[c] = 0; g_brange[c] = 2; g_rpn[c][0] = g_rpn[c][1] = 127; } }
    return 0;
}

static NoteChannel chan(int ch)
{
    if (!g_ch[ch]) {
        NoteRequest nr;
        memset(&nr, 0, sizeof nr);
        nr.info.polyphony = 16;
        nr.info.typicalPolyphony = 0x00010000;   /* (Fixed 1.0) */
        /* GM programs are QuickTime instruments 1-128; channel 10 the GM drum kit */
        if (NAStuffToneDescription(g_na, ch == 9 ? kFirstDrumkit + 1 : g_prog[ch] + 1, &nr.tone) != noErr) return NULL;
        if (NANewNoteChannel(g_na, &nr, &g_ch[ch]) != noErr) g_ch[ch] = NULL;
    }
    return g_ch[ch];
}

static void notes_off(int ch)
{
    int n;
    for (n = 0; n < 128; n++) while (g_on[ch][n]) { g_on[ch][n]--; if (g_ch[ch]) NAPlayNote(g_na, g_ch[ch], n, 0); }
}

void qtmidi_msg(void *user, int status, int a, int b)
{
    int hi = status & 0xf0, ch = status & 15;
    NoteChannel c;
    (void)user;
    if (!g_na) return;
    switch (hi) {
    case 0x90:
        if (b > 0) { if ((c = chan(ch)) != NULL) { NAPlayNote(g_na, c, a, b); if (g_on[ch][a] < 255) g_on[ch][a]++; } break; }
        /* (velocity 0: note-off) */
    case 0x80:
        if (g_ch[ch] && g_on[ch][a]) { g_on[ch][a]--; NAPlayNote(g_na, g_ch[ch], a, 0); }
        break;
    case 0xc0:
        g_prog[ch] = a;
        if (ch != 9 && g_ch[ch]) NASetInstrumentNumber(g_na, g_ch[ch], a + 1);
        break;
    case 0xe0:
        if ((c = chan(ch)) != NULL) {   /* QuickTime's bend: semitones, 8.8 fixed */
            long v = (long)((a | b << 7) - 8192);
            NASetController(g_na, c, kControllerPitchBend, (long)(v * g_brange[ch] * 256 / 8192));
        }
        break;
    case 0xb0:
        if ((c = chan(ch)) == NULL) break;
        switch (a) {   /* QuickTime's controllers take 8.8 fixed values */
        case 1: NASetController(g_na, c, kControllerModulationWheel, b << 8); break;
        case 7: NASetController(g_na, c, kControllerVolume, b << 8); break;
        case 10: NASetController(g_na, c, kControllerPan, (b << 8) / 127 + 256); break;   /* (256 left .. 512 right) */
        case 11: NASetController(g_na, c, kControllerExpression, b << 8); break;
        case 64: NASetController(g_na, c, kControllerSustain, b >= 64 ? 1 << 8 : 0); break;
        case 100: g_rpn[ch][1] = b; break;
        case 101: g_rpn[ch][0] = b; break;
        case 6: if (!g_rpn[ch][0] && !g_rpn[ch][1]) g_brange[ch] = b > 24 ? 24 : b; break;   /* RPN 0: bend range */
        case 121: NASetController(g_na, c, kControllerPitchBend, 0); NASetController(g_na, c, kControllerSustain, 0); break;
        case 120: case 123: notes_off(ch); break;
        }
        break;
    }
}

void qtmidi_close(void)
{
    int ch;
    if (!g_na) return;
    for (ch = 0; ch < 16; ch++) { notes_off(ch); if (g_ch[ch]) NADisposeNoteChannel(g_na, g_ch[ch]); g_ch[ch] = NULL; }
    CloseComponent(g_na);
    g_na = NULL;
}
