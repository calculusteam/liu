/*
 * Environment hooks are available only in LIU_DEV_HOOKS builds. Other builds
 * return NULL without reading hook variables; documented runtime
 * configuration variables remain available in all builds.
 */
#ifndef CORE_DEV_HOOKS_H
#define CORE_DEV_HOOKS_H

#include <stdlib.h>

#ifdef LIU_DEV_HOOKS
#define LIU_DEV_HOOKS_ON 1
/* `name` is a string literal at every call site but two (the intro's flag
 * helpers), so this is a macro: with the hooks off the literal is gone. */
#define liu_dev_env(name) getenv(name)
#else
#define LIU_DEV_HOOKS_ON 0
#define liu_dev_env(name) ((char *)0)
#endif

#endif /* CORE_DEV_HOOKS_H */
