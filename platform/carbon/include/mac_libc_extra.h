/* mac_libc_extra.h - force-included in miniaudio.c on the classic Mac build: the POSIX call it may reference that
 * Retro68's C library lacks (posix_mac.c implements it) */
#ifndef MW2_MAC_LIBC_EXTRA_H
#define MW2_MAC_LIBC_EXTRA_H
#include <stddef.h>
#include <time.h>
int nanosleep(const struct timespec *rqtp, struct timespec *rmtp);
/* in Retro68's libc but not shown by its headers (as in posix_mac.h) */
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
#endif
