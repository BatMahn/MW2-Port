/* qtmidi.h - the classic Mac build's "built-in" menu music: QuickTime's General MIDI synthesizer (qtmidi.c) */
#ifndef MW2_QTMIDI_H
#define MW2_QTMIDI_H
int  qtmidi_open(void);                                   /* 0 if QuickTime's music is there */
void qtmidi_msg(void *user, int status, int a, int b);    /* a gus_sink's midi */
void qtmidi_close(void);
#endif
