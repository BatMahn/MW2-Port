/* OpenGL entry point loader for Windows - see glload.h. Built into mw2 only on Windows. */
#ifdef _WIN32
#include <stdio.h>
#include <SDL.h>
#include "glload.h"

#undef X
#define X(name, type) type mw2gl_##name;
MW2GL_FUNCS
#undef X

int mw2gl_load(void)
{
    int missing = 0;
#define X(name, type) \
    if (!(mw2gl_##name = (type)SDL_GL_GetProcAddress(#name))) { fprintf(stderr, "OpenGL: %s not available\n", #name); missing++; }
    MW2GL_FUNCS
#undef X
    return missing;
}
#else
typedef int mw2_glload_unused;   /* ISO C: a translation unit must not be empty */
#endif
