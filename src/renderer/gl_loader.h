/*
 * Liu — dependency-free OpenGL 2.0+/3.x function loader.
 *
 * The OpenGL renderer (renderer.c) calls modern GL functions (shaders, VAOs,
 * instanced draws, uniforms, …). On macOS those come prototyped from
 * <OpenGL/gl3.h>, so this file is a no-op there. On Linux/Windows <GL/gl.h>
 * exposes only the GL 1.1 functions; everything past that must be resolved at
 * runtime via the platform's GetProcAddress. This header declares typed
 * pointers for exactly the calls the renderer makes and (when LIU_GL_REDIRECT
 * is defined) #defines the gl* names onto them, so renderer.c stays unchanged.
 * The GL 1.1 calls (glClear/glViewport/glBindTexture/glTexImage2D/…) keep
 * linking from the system libGL/opengl32.
 *
 * IMPORTANT: include this AFTER <GL/gl.h> — the typedefs use the GL base types.
 * Define LIU_GL_REDIRECT before including to activate the gl*→pointer mapping
 * (renderer.c does this; the loader's own .c does not, so it can define the
 * pointers cleanly).
 */
#ifndef LIU_GL_LOADER_H
#define LIU_GL_LOADER_H

#if !defined(__APPLE__) && !defined(PLATFORM_MACOS)

#include <stddef.h>   /* ptrdiff_t — ABI shape of GLsizeiptr/GLintptr params */
#include <stdbool.h>

/* Windows' <GL/gl.h> stops at GL 1.1: the 1.5+ typedefs (GLsizeiptr, GLchar)
 * and enums (GL_ARRAY_BUFFER, GL_STREAM_DRAW, …) that renderer.c uses live in
 * <GL/glext.h>, which MinGW-w64 ships. Mesa's gl.h pulls glext.h by default
 * (and defines GL_VERSION_1_5), so this only fires where it's needed. */
#ifndef GL_VERSION_1_5
#include <GL/glext.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Single source of truth: (return type, GL name, parameter type list).
 * GLsizeiptr/GLintptr-shaped params use ptrdiff_t and GLchar uses char (both
 * ABI-identical), so this depends only on GL 1.1 base types from <GL/gl.h>.
 */
#define LIU_GL_FUNCS(X) \
    X(GLuint, glCreateShader,            (GLenum)) \
    X(void,   glShaderSource,            (GLuint, GLsizei, const char *const *, const GLint *)) \
    X(void,   glCompileShader,           (GLuint)) \
    X(void,   glGetShaderiv,             (GLuint, GLenum, GLint *)) \
    X(void,   glGetShaderInfoLog,        (GLuint, GLsizei, GLsizei *, char *)) \
    X(GLuint, glCreateProgram,           (void)) \
    X(void,   glAttachShader,            (GLuint, GLuint)) \
    X(void,   glDetachShader,            (GLuint, GLuint)) \
    X(void,   glLinkProgram,             (GLuint)) \
    X(void,   glGetProgramiv,            (GLuint, GLenum, GLint *)) \
    X(void,   glGetProgramInfoLog,       (GLuint, GLsizei, GLsizei *, char *)) \
    X(void,   glUseProgram,              (GLuint)) \
    X(void,   glDeleteShader,            (GLuint)) \
    X(void,   glDeleteProgram,           (GLuint)) \
    X(void,   glGenBuffers,              (GLsizei, GLuint *)) \
    X(void,   glBindBuffer,              (GLenum, GLuint)) \
    X(void,   glBufferData,              (GLenum, ptrdiff_t, const void *, GLenum)) \
    X(void,   glBufferSubData,           (GLenum, ptrdiff_t, ptrdiff_t, const void *)) \
    X(void,   glDeleteBuffers,           (GLsizei, const GLuint *)) \
    X(void,   glGenVertexArrays,         (GLsizei, GLuint *)) \
    X(void,   glBindVertexArray,         (GLuint)) \
    X(void,   glDeleteVertexArrays,      (GLsizei, const GLuint *)) \
    X(void,   glEnableVertexAttribArray, (GLuint)) \
    X(void,   glVertexAttribPointer,     (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void,   glVertexAttribDivisor,     (GLuint, GLuint)) \
    X(void,   glDrawArraysInstanced,     (GLenum, GLint, GLsizei, GLsizei)) \
    X(GLint,  glGetUniformLocation,      (GLuint, const char *)) \
    X(void,   glUniform1i,               (GLint, GLint)) \
    X(void,   glUniform1f,               (GLint, GLfloat)) \
    X(void,   glUniform2f,               (GLint, GLfloat, GLfloat)) \
    X(void,   glUniform4f,               (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void,   glUniformMatrix4fv,        (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void,   glActiveTexture,           (GLenum)) \
    X(void,   glGenerateMipmap,          (GLenum)) \
    X(void,   glBlendFuncSeparate,       (GLenum, GLenum, GLenum, GLenum)) \
    X(void,   glBlendEquationSeparate,   (GLenum, GLenum)) \
    X(void,   glGenSamplers,             (GLsizei, GLuint *)) \
    X(void,   glDeleteSamplers,          (GLsizei, const GLuint *)) \
    X(void,   glBindSampler,             (GLuint, GLuint)) \
    X(void,   glSamplerParameteri,       (GLuint, GLenum, GLint)) \
    /* ARB_sync (core in 3.2). GLsync is a pointer and GLuint64 is 64-bit
     * unsigned, both spelled here in base types so this list keeps depending
     * only on GL 1.1 — the same rule the GLsizeiptr entries follow. */ \
    X(void *, glFenceSync,               (GLenum, GLbitfield)) \
    X(GLenum, glClientWaitSync,          (void *, GLbitfield, unsigned long long)) \
    X(void,   glDeleteSync,              (void *))

/* OPTIONAL entry points: resolved the same way, but a miss is not fatal.
 *
 * The required list above is required in the strongest sense — platform_linux.c
 * tears the window down and returns NULL when gl_loader_init fails, so anything
 * listed there is a hard "this driver cannot run Liu". A diagnostic must never
 * earn that verdict, which is what this second tier is for: the caller checks
 * the pointer before use and does without when it is NULL.
 *
 * Timer queries are GL 3.3 core (ARB_timer_query) and so should be present
 * wherever the required list is, but "should be" is not a reason to make the
 * app refuse to start over the frame probe's gpu-ms column. The GLuint64
 * out-param is spelled `unsigned long long *`, ABI-identical, which keeps this
 * header on GL 1.1 base types. */
#define LIU_GL_FUNCS_OPT(X) \
    X(void,   glGenQueries,              (GLsizei, GLuint *)) \
    X(void,   glDeleteQueries,           (GLsizei, const GLuint *)) \
    X(void,   glBeginQuery,              (GLenum, GLuint)) \
    X(void,   glEndQuery,                (GLenum)) \
    X(void,   glGetQueryObjectiv,        (GLuint, GLenum, GLint *)) \
    X(void,   glGetQueryObjectui64v,     (GLuint, GLenum, unsigned long long *))

#define X(ret, name, params) typedef ret (*LiuPFN_##name) params;
LIU_GL_FUNCS(X)
LIU_GL_FUNCS_OPT(X)
#undef X

#define X(ret, name, params) extern LiuPFN_##name liu_##name;
LIU_GL_FUNCS(X)
LIU_GL_FUNCS_OPT(X)
#undef X

/* Resolve every pointer via `getproc` (e.g. glXGetProcAddressARB on Linux).
 * Call once after the GL context is current. Returns false (and logs the
 * missing name) if any REQUIRED function is absent; a missing optional one
 * leaves its pointer NULL and returns true. */
bool gl_loader_init(void *(*getproc)(const char *));

#ifdef LIU_GL_REDIRECT
#define glCreateShader            liu_glCreateShader
#define glShaderSource            liu_glShaderSource
#define glCompileShader           liu_glCompileShader
#define glGetShaderiv             liu_glGetShaderiv
#define glGetShaderInfoLog        liu_glGetShaderInfoLog
#define glCreateProgram           liu_glCreateProgram
#define glAttachShader            liu_glAttachShader
#define glDetachShader            liu_glDetachShader
#define glLinkProgram             liu_glLinkProgram
#define glGetProgramiv            liu_glGetProgramiv
#define glGetProgramInfoLog       liu_glGetProgramInfoLog
#define glUseProgram              liu_glUseProgram
#define glDeleteShader            liu_glDeleteShader
#define glDeleteProgram           liu_glDeleteProgram
#define glGenBuffers              liu_glGenBuffers
#define glBindBuffer              liu_glBindBuffer
#define glBufferData              liu_glBufferData
#define glBufferSubData           liu_glBufferSubData
#define glDeleteBuffers           liu_glDeleteBuffers
#define glGenVertexArrays         liu_glGenVertexArrays
#define glBindVertexArray         liu_glBindVertexArray
#define glDeleteVertexArrays      liu_glDeleteVertexArrays
#define glEnableVertexAttribArray liu_glEnableVertexAttribArray
#define glVertexAttribPointer     liu_glVertexAttribPointer
#define glVertexAttribDivisor     liu_glVertexAttribDivisor
#define glDrawArraysInstanced     liu_glDrawArraysInstanced
#define glGetUniformLocation      liu_glGetUniformLocation
#define glUniform1i               liu_glUniform1i
#define glUniform1f               liu_glUniform1f
#define glUniform2f               liu_glUniform2f
#define glUniform4f               liu_glUniform4f
#define glUniformMatrix4fv        liu_glUniformMatrix4fv
#define glActiveTexture           liu_glActiveTexture
#define glGenerateMipmap          liu_glGenerateMipmap
#define glBlendFuncSeparate       liu_glBlendFuncSeparate
#define glBlendEquationSeparate   liu_glBlendEquationSeparate
#define glGenSamplers             liu_glGenSamplers
#define glDeleteSamplers          liu_glDeleteSamplers
#define glBindSampler             liu_glBindSampler
#define glSamplerParameteri       liu_glSamplerParameteri
#define glFenceSync               liu_glFenceSync
#define glClientWaitSync          liu_glClientWaitSync
#define glDeleteSync              liu_glDeleteSync
#define glGenQueries              liu_glGenQueries
#define glDeleteQueries           liu_glDeleteQueries
#define glBeginQuery              liu_glBeginQuery
#define glEndQuery                liu_glEndQuery
#define glGetQueryObjectiv        liu_glGetQueryObjectiv
#define glGetQueryObjectui64v     liu_glGetQueryObjectui64v
#endif /* LIU_GL_REDIRECT */

#ifdef __cplusplus
}
#endif

#endif /* !__APPLE__ */
#endif /* LIU_GL_LOADER_H */
