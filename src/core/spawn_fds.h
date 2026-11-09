#ifndef CORE_SPAWN_FDS_H
#define CORE_SPAWN_FDS_H

/*
 * A spawned child gets the descriptors its file actions place on stdin,
 * stdout and stderr, and nothing else of the parent's.
 *
 * macOS has a spawn flag for it (POSIX_SPAWN_CLOEXEC_DEFAULT). glibc has a
 * file action from 2.34 (posix_spawn_file_actions_addclosefrom_np), which the
 * Linux builds' floor has. Without either, every descriptor the parent opened
 * without O_CLOEXEC is the child's too — the write end of a pipe meant to give
 * the child its stdin among them, and a child that holds its own stdin's
 * write end never reads to its end.
 *
 * Called after the actions that place the three.
 */

#include <spawn.h>
#include <unistd.h>

static inline void spawn_fds_only_stdio(posix_spawn_file_actions_t *fa, short *flags) {
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    (void)fa;
    *flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    (void)flags;
    posix_spawn_file_actions_addclosefrom_np(fa, STDERR_FILENO + 1);
#else
    (void)fa;
    (void)flags;
#endif
}

#endif
