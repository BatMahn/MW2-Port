/* SDL_opengl.h - OpenGL 1.2 for the SDL subset builds: on classic Mac OS (and with MW2_GL_DYN anywhere) the functions
 * are pointers loaded at start (../gl_dyn.h); otherwise the system's headers (link checks) */
#if defined(MW2_CLASSIC_MAC) || defined(MW2_GL_DYN)
#  include "../gl_dyn.h"
#elif defined(__APPLE__)
#  include <OpenGL/gl.h>
#  include <OpenGL/glext.h>
#else
#  include <GL/gl.h>
#  include <GL/glext.h>
#endif
