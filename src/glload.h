/* OpenGL 2.0+ entry points on Windows. opengl32.dll exports only OpenGL 1.1; everything newer has to be looked up
 * from the driver after a context exists (SDL_GL_GetProcAddress). This header stands in for <GL/gl.h> + <GL/glext.h>
 * on Windows: each function the port uses beyond 1.1 becomes a pointer of the same name, filled in by mw2gl_load().
 * Other platforms link these directly and never include this file. To use a new GL function on Windows, add it to
 * MW2GL_FUNCS. */
#ifndef MW2_GLLOAD_H
#define MW2_GLLOAD_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
/* windef.h's 16-bit-era macros collide with ordinary identifiers in the port (near, far, IN, OUT ...) */
#undef near
#undef far
#undef IN
#undef OUT
#undef OPTIONAL
#undef min
#undef max

#define MW2GL_FUNCS \
    X(glActiveTexture, PFNGLACTIVETEXTUREPROC) \
    X(glAttachShader, PFNGLATTACHSHADERPROC) \
    X(glBindBuffer, PFNGLBINDBUFFERPROC) \
    X(glBindFramebuffer, PFNGLBINDFRAMEBUFFERPROC) \
    X(glBindRenderbuffer, PFNGLBINDRENDERBUFFERPROC) \
    X(glBindVertexArray, PFNGLBINDVERTEXARRAYPROC) \
    X(glBlitFramebuffer, PFNGLBLITFRAMEBUFFERPROC) \
    X(glBufferData, PFNGLBUFFERDATAPROC) \
    X(glBufferSubData, PFNGLBUFFERSUBDATAPROC) \
    X(glMapBufferRange, PFNGLMAPBUFFERRANGEPROC) \
    X(glUnmapBuffer, PFNGLUNMAPBUFFERPROC) \
    X(glCheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC) \
    X(glCompileShader, PFNGLCOMPILESHADERPROC) \
    X(glCreateProgram, PFNGLCREATEPROGRAMPROC) \
    X(glCreateShader, PFNGLCREATESHADERPROC) \
    X(glDeleteBuffers, PFNGLDELETEBUFFERSPROC) \
    X(glDeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSPROC) \
    X(glDeleteProgram, PFNGLDELETEPROGRAMPROC) \
    X(glDeleteRenderbuffers, PFNGLDELETERENDERBUFFERSPROC) \
    X(glDeleteShader, PFNGLDELETESHADERPROC) \
    X(glDeleteVertexArrays, PFNGLDELETEVERTEXARRAYSPROC) \
    X(glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC) \
    X(glFramebufferRenderbuffer, PFNGLFRAMEBUFFERRENDERBUFFERPROC) \
    X(glFramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC) \
    X(glGenBuffers, PFNGLGENBUFFERSPROC) \
    X(glGenFramebuffers, PFNGLGENFRAMEBUFFERSPROC) \
    X(glGenRenderbuffers, PFNGLGENRENDERBUFFERSPROC) \
    X(glGenVertexArrays, PFNGLGENVERTEXARRAYSPROC) \
    X(glGenerateMipmap, PFNGLGENERATEMIPMAPPROC) \
    X(glGetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC) \
    X(glGetProgramiv, PFNGLGETPROGRAMIVPROC) \
    X(glGetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC) \
    X(glGetShaderiv, PFNGLGETSHADERIVPROC) \
    X(glGetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC) \
    X(glLinkProgram, PFNGLLINKPROGRAMPROC) \
    X(glRenderbufferStorage, PFNGLRENDERBUFFERSTORAGEPROC) \
    X(glShaderSource, PFNGLSHADERSOURCEPROC) \
    X(glTexBuffer, PFNGLTEXBUFFERPROC) \
    X(glUniform1f, PFNGLUNIFORM1FPROC) \
    X(glUniform1i, PFNGLUNIFORM1IPROC) \
    X(glUniform1iv, PFNGLUNIFORM1IVPROC) \
    X(glUniform2f, PFNGLUNIFORM2FPROC) \
    X(glUniform2i, PFNGLUNIFORM2IPROC) \
    X(glUniform3f, PFNGLUNIFORM3FPROC) \
    X(glUniform3fv, PFNGLUNIFORM3FVPROC) \
    X(glUniform4f, PFNGLUNIFORM4FPROC) \
    X(glUniform4fv, PFNGLUNIFORM4FVPROC) \
    X(glUniformMatrix4fv, PFNGLUNIFORMMATRIX4FVPROC) \
    X(glUseProgram, PFNGLUSEPROGRAMPROC) \
    X(glVertexAttrib4f, PFNGLVERTEXATTRIB4FPROC) \
    X(glVertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC) \

#define X(name, type) extern type mw2gl_##name;
MW2GL_FUNCS
#undef X
#define X(name, type) /* */
/* call sites keep the standard names */
#define glActiveTexture mw2gl_glActiveTexture
#define glAttachShader mw2gl_glAttachShader
#define glBindBuffer mw2gl_glBindBuffer
#define glBindFramebuffer mw2gl_glBindFramebuffer
#define glBindRenderbuffer mw2gl_glBindRenderbuffer
#define glBindVertexArray mw2gl_glBindVertexArray
#define glBlitFramebuffer mw2gl_glBlitFramebuffer
#define glBufferData mw2gl_glBufferData
#define glBufferSubData mw2gl_glBufferSubData
#define glMapBufferRange mw2gl_glMapBufferRange
#define glUnmapBuffer mw2gl_glUnmapBuffer
#define glCheckFramebufferStatus mw2gl_glCheckFramebufferStatus
#define glCompileShader mw2gl_glCompileShader
#define glCreateProgram mw2gl_glCreateProgram
#define glCreateShader mw2gl_glCreateShader
#define glDeleteBuffers mw2gl_glDeleteBuffers
#define glDeleteFramebuffers mw2gl_glDeleteFramebuffers
#define glDeleteProgram mw2gl_glDeleteProgram
#define glDeleteRenderbuffers mw2gl_glDeleteRenderbuffers
#define glDeleteShader mw2gl_glDeleteShader
#define glDeleteVertexArrays mw2gl_glDeleteVertexArrays
#define glEnableVertexAttribArray mw2gl_glEnableVertexAttribArray
#define glFramebufferRenderbuffer mw2gl_glFramebufferRenderbuffer
#define glFramebufferTexture2D mw2gl_glFramebufferTexture2D
#define glGenBuffers mw2gl_glGenBuffers
#define glGenFramebuffers mw2gl_glGenFramebuffers
#define glGenRenderbuffers mw2gl_glGenRenderbuffers
#define glGenVertexArrays mw2gl_glGenVertexArrays
#define glGenerateMipmap mw2gl_glGenerateMipmap
#define glGetProgramInfoLog mw2gl_glGetProgramInfoLog
#define glGetProgramiv mw2gl_glGetProgramiv
#define glGetShaderInfoLog mw2gl_glGetShaderInfoLog
#define glGetShaderiv mw2gl_glGetShaderiv
#define glGetUniformLocation mw2gl_glGetUniformLocation
#define glLinkProgram mw2gl_glLinkProgram
#define glRenderbufferStorage mw2gl_glRenderbufferStorage
#define glShaderSource mw2gl_glShaderSource
#define glTexBuffer mw2gl_glTexBuffer
#define glUniform1f mw2gl_glUniform1f
#define glUniform1i mw2gl_glUniform1i
#define glUniform1iv mw2gl_glUniform1iv
#define glUniform2f mw2gl_glUniform2f
#define glUniform2i mw2gl_glUniform2i
#define glUniform3f mw2gl_glUniform3f
#define glUniform3fv mw2gl_glUniform3fv
#define glUniform4f mw2gl_glUniform4f
#define glUniform4fv mw2gl_glUniform4fv
#define glUniformMatrix4fv mw2gl_glUniformMatrix4fv
#define glUseProgram mw2gl_glUseProgram
#define glVertexAttrib4f mw2gl_glVertexAttrib4f
#define glVertexAttribPointer mw2gl_glVertexAttribPointer
#undef X

/* after SDL_GL_CreateContext: 0 if every entry point was found, else the number missing (each one logged) */
int mw2gl_load(void);
#endif
