#ifndef MW2_DEVELOPMENT_H
#define MW2_DEVELOPMENT_H

#include <stdlib.h>

/* The published binaries ignore development-only environment switches. */
#ifdef MW2_RELEASE
enum { MW2_DEVELOPMENT_LOGS = 0 };
#else
enum { MW2_DEVELOPMENT_LOGS = 1 };
#endif

static inline char *mw2_development_env(const char *name)
{
#ifdef MW2_RELEASE
    (void)name;
    return NULL;
#else
    return getenv(name);
#endif
}

#endif
