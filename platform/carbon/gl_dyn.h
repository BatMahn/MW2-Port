/*
* gl_dyn.h - OpenGL 1.2 (+ GL_ARB_multitexture) for the classic Mac OS / Carbon build: the types, the constants the
 * renderer uses and one pointer per function, filled by name at start (gl_dyn_load) - from OpenGLLibrary on Mac OS 9
 * (CFM FindSymbol), from OpenGL.framework on Mac OS X - so the build needs no OpenGL SDK or import library.
 * GENERATED from the functions src/rb_gl12*.c and platform/carbon/sdl_core.c call (prototypes from Mesa's gl.h). */
#ifndef MW2_GL_DYN_H
#define MW2_GL_DYN_H

typedef unsigned int GLenum; typedef unsigned char GLboolean; typedef unsigned int GLbitfield; typedef void GLvoid;
typedef signed char GLbyte; typedef short GLshort; typedef int GLint; typedef unsigned char GLubyte; typedef unsigned short GLushort;
typedef unsigned int GLuint; typedef int GLsizei; typedef float GLfloat; typedef float GLclampf; typedef double GLdouble; typedef double GLclampd;
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_TEXTURE0_ARB 0x84C0
#define GL_TEXTURE1_ARB 0x84C1
#define GL_MAX_TEXTURE_UNITS_ARB 0x84E2
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_UNPACK_ROW_LENGTH 0x0CF2

#define GL_ALPHA_TEST                    0x0BC0
#define GL_ALWAYS                        0x0207
#define GL_BLEND                         0x0BE2
#define GL_COLOR_ARRAY                   0x8076
#define GL_COLOR_BUFFER_BIT              0x00004000
#define GL_CULL_FACE                     0x0B44
#define GL_DECAL                         0x2101
#define GL_DEPTH_BUFFER_BIT              0x00000100
#define GL_DEPTH_TEST                    0x0B71
#define GL_DITHER                        0x0BD0
#define GL_DST_COLOR                     0x0306
#define GL_EQUAL                         0x0202
#define GL_EXTENSIONS                    0x1F03
#define GL_FLOAT                         0x1406
#define GL_GEQUAL                        0x0206
#define GL_GREATER                       0x0204
#define GL_LEQUAL                        0x0203
#define GL_LESS                          0x0201
#define GL_LIGHTING                      0x0B50
#define GL_LINEAR                        0x2601
#define GL_LINES                         0x0001
#define GL_MAX_TEXTURE_SIZE              0x0D33
#define GL_MODELVIEW                     0x1700
#define GL_MODULATE                      0x2100
#define GL_NEAREST                       0x2600
#define GL_NICEST                        0x1102
#define GL_ONE                           1
#define GL_ONE_MINUS_SRC_ALPHA           0x0303
#define GL_PACK_ALIGNMENT                0x0D05
#define GL_PERSPECTIVE_CORRECTION_HINT   0x0C50
#define GL_POLYGON_OFFSET_FILL           0x8037
#define GL_POLYGON_STIPPLE               0x0B42
#define GL_PROJECTION                    0x1701
#define GL_QUADS                         0x0007
#define GL_RENDERER                      0x1F01
#define GL_REPEAT                        0x2901
#define GL_REPLACE                       0x1E01
#define GL_RGB                           0x1907
#define GL_RGBA                          0x1908
#define GL_SCISSOR_TEST                  0x0C11
#define GL_SMOOTH                        0x1D01
#define GL_SRC_ALPHA                     0x0302
#define GL_TEXTURE                       0x1702
#define GL_TEXTURE_1D                    0x0DE0
#define GL_TEXTURE_2D                    0x0DE1
#define GL_TEXTURE_COORD_ARRAY           0x8078
#define GL_TEXTURE_ENV                   0x2300
#define GL_TEXTURE_ENV_MODE              0x2200
#define GL_TEXTURE_ENV_COLOR             0x2201
#define GL_RED_BITS                      0x0D52
#define GL_GREEN_BITS                    0x0D53
#define GL_BLUE_BITS                     0x0D54
#define GL_DEPTH_BITS                    0x0D56
#define GL_FOG                           0x0B60
#define GL_LUMINANCE                     0x1909
#define GL_LUMINANCE8                    0x8040
#define GL_RGBA8                         0x8058
#define GL_TEXTURE_MAG_FILTER            0x2800
#define GL_TEXTURE_MIN_FILTER            0x2801
#define GL_TEXTURE_WRAP_S                0x2802
#define GL_TEXTURE_WRAP_T                0x2803
#define GL_TRIANGLES                     0x0004
#define GL_UNPACK_ALIGNMENT              0x0CF5
#define GL_UNSIGNED_BYTE                 0x1401
#define GL_VENDOR                        0x1F00
#define GL_VERSION                       0x1F02
#define GL_VERTEX_ARRAY                  0x8074
#define GL_ZERO                          0

extern void (*p_glActiveTextureARB)(GLenum texture);
#define glActiveTextureARB p_glActiveTextureARB
extern void (*p_glAlphaFunc)(GLenum func, GLclampf ref);
#define glAlphaFunc p_glAlphaFunc
extern void (*p_glBegin)(GLenum mode);
#define glBegin p_glBegin
extern void (*p_glBindTexture)(GLenum target, GLuint texture);
#define glBindTexture p_glBindTexture
extern void (*p_glBlendFunc)(GLenum sfactor, GLenum dfactor);
#define glBlendFunc p_glBlendFunc
extern void (*p_glClear)(GLbitfield mask);
#define glClear p_glClear
extern void (*p_glClearColor)(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
#define glClearColor p_glClearColor
extern void (*p_glClearDepth)(GLclampd depth);
#define glClearDepth p_glClearDepth
extern void (*p_glClientActiveTextureARB)(GLenum texture);
#define glClientActiveTextureARB p_glClientActiveTextureARB
extern void (*p_glColor3fv)(const GLfloat *v);
#define glColor3fv p_glColor3fv
extern void (*p_glColorMask)(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
#define glColorMask p_glColorMask
extern void (*p_glColorPointer)(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
#define glColorPointer p_glColorPointer
extern void (*p_glCopyTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
#define glCopyTexSubImage2D p_glCopyTexSubImage2D
extern void (*p_glDeleteTextures)(GLsizei n, const GLuint *textures);
#define glDeleteTextures p_glDeleteTextures
extern void (*p_glDepthFunc)(GLenum func);
#define glDepthFunc p_glDepthFunc
extern void (*p_glDepthMask)(GLboolean flag);
#define glDepthMask p_glDepthMask
extern void (*p_glDepthRange)(GLclampd near_val, GLclampd far_val);
#define glDepthRange p_glDepthRange
extern void (*p_glDisable)(GLenum cap);
#define glDisable p_glDisable
extern void (*p_glDisableClientState)(GLenum cap);
#define glDisableClientState p_glDisableClientState
extern void (*p_glDrawArrays)(GLenum mode, GLint first, GLsizei count);
#define glDrawArrays p_glDrawArrays
extern void (*p_glEnable)(GLenum cap);
#define glEnable p_glEnable
extern void (*p_glEnableClientState)(GLenum cap);
#define glEnableClientState p_glEnableClientState
extern void (*p_glEnd)(void);
#define glEnd p_glEnd
extern void (*p_glFinish)(void);
#define glFinish p_glFinish
extern void (*p_glGenTextures)(GLsizei n, GLuint *textures);
#define glGenTextures p_glGenTextures
extern void (*p_glGetIntegerv)(GLenum pname, GLint *params);
#define glGetIntegerv p_glGetIntegerv
extern const GLubyte * (*p_glGetString)(GLenum name);
#define glGetString p_glGetString
extern void (*p_glHint)(GLenum target, GLenum mode);
#define glHint p_glHint
extern void (*p_glLoadIdentity)(void);
#define glLoadIdentity p_glLoadIdentity
extern void (*p_glMatrixMode)(GLenum mode);
#define glMatrixMode p_glMatrixMode
extern void (*p_glOrtho)(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_val, GLdouble far_val);
#define glOrtho p_glOrtho
extern void (*p_glPixelStorei)(GLenum pname, GLint param);
#define glPixelStorei p_glPixelStorei
extern void (*p_glPolygonOffset)(GLfloat factor, GLfloat units);
#define glPolygonOffset p_glPolygonOffset
extern void (*p_glPolygonStipple)(const GLubyte *mask);
#define glPolygonStipple p_glPolygonStipple
extern void (*p_glReadPixels)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels);
#define glReadPixels p_glReadPixels
extern void (*p_glScalef)(GLfloat x, GLfloat y, GLfloat z);
#define glScalef p_glScalef
extern void (*p_glScissor)(GLint x, GLint y, GLsizei width, GLsizei height);
#define glScissor p_glScissor
extern void (*p_glShadeModel)(GLenum mode);
#define glShadeModel p_glShadeModel
extern void (*p_glTexCoord2f)(GLfloat s, GLfloat t);
#define glTexCoord2f p_glTexCoord2f
extern void (*p_glTexCoordPointer)(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
#define glTexCoordPointer p_glTexCoordPointer
extern void (*p_glTexEnvi)(GLenum target, GLenum pname, GLint param);
#define glTexEnvi p_glTexEnvi
extern void (*p_glTexEnvfv)(GLenum target, GLenum pname, const GLfloat *params);
#define glTexEnvfv p_glTexEnvfv
extern void (*p_glLineWidth)(GLfloat width);
#define glLineWidth p_glLineWidth
extern void (*p_glTexImage1D)(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLint border, GLenum format, GLenum type, const GLvoid *pixels);
#define glTexImage1D p_glTexImage1D
extern void (*p_glTexImage2D)(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels);
#define glTexImage2D p_glTexImage2D
extern void (*p_glTexParameteri)(GLenum target, GLenum pname, GLint param);
#define glTexParameteri p_glTexParameteri
extern void (*p_glTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels);
#define glTexSubImage2D p_glTexSubImage2D
extern void (*p_glVertex2f)(GLfloat x, GLfloat y);
#define glVertex2f p_glVertex2f
extern void (*p_glVertex4fv)(const GLfloat *v);
#define glVertex4fv p_glVertex4fv
extern void (*p_glVertexPointer)(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
#define glVertexPointer p_glVertexPointer
extern void (*p_glViewport)(GLint x, GLint y, GLsizei width, GLsizei height);
#define glViewport p_glViewport

/* fill the pointers: get(name) returns the function or NULL; 0 if every OpenGL 1.2 core function was found */
int gl_dyn_load(void *(*get)(const char *name));

#endif
