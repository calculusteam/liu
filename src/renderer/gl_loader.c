/*
 * Liu — OpenGL 2.0+/3.x function loader (definitions). See gl_loader.h.
 * No-op translation unit on macOS (uses <OpenGL/gl3.h> prototypes instead).
 */
#if !defined(__APPLE__) && !defined(PLATFORM_MACOS)

#include <GL/gl.h>             /* GL base types + 1.1 prototypes */
#include "renderer/gl_loader.h" /* note: LIU_GL_REDIRECT NOT defined here */
#include <stdio.h>

/* Define one pointer per function (initially NULL). */
#define X(ret, name, params) LiuPFN_##name liu_##name = NULL;
LIU_GL_FUNCS(X)
LIU_GL_FUNCS_OPT(X)
#undef X

bool gl_loader_init(void *(*getproc)(const char *)) {
    if (!getproc) return false;
    bool ok = true;

    /* `glXGetProcAddressARB`/`wglGetProcAddress` may also return non-NULL for
     * functions that aren't actually available; that edge is rare for core
     * 3.3 entry points and a NULL check covers the common "driver too old"
     * case. Each miss is logged so a broken target is obvious in CI. */
    #define X(ret, name, params)                                            \
        liu_##name = (LiuPFN_##name)getproc(#name);                         \
        if (!liu_##name) {                                                  \
            fprintf(stderr, "[liu gl] missing OpenGL function: %s\n", #name); \
            ok = false;                                                     \
        }
    LIU_GL_FUNCS(X)
    #undef X

    /* Optional tier: log the miss so a stripped driver is still visible in CI,
     * but leave `ok` alone — the caller destroys the window on false, and no
     * diagnostic is worth that. Callers must NULL-check these. */
    #define X(ret, name, params)                                              \
        liu_##name = (LiuPFN_##name)getproc(#name);                           \
        if (!liu_##name)                                                      \
            fprintf(stderr, "[liu gl] optional OpenGL function absent: %s\n", #name);
    LIU_GL_FUNCS_OPT(X)
    #undef X

    return ok;
}

#endif /* !__APPLE__ */
