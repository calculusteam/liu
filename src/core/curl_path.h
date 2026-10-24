/*
 * Liu — which `curl` a request that carries a secret runs.
 *
 * Translate (API), Voice (API) and the model catalog hand `curl -K -` a config
 * on stdin with the user's API key in it. Liu's PATH is the user's login-shell
 * PATH (the GUI inherits launchd's stripped one, so agent_ensure_process_path
 * repairs it — ~/.local/bin, Homebrew, ...), which makes it a list of
 * directories that tools and package managers write to; the first `curl` found
 * there would receive the Authorization header. So these requests run the
 * system's curl when it exists, as the updater does, and search the PATH only
 * as the fallback for a system without it.
 *
 * posix_spawnp with a path that contains a '/' does no PATH search, so callers
 * keep their argv[0] of "curl" and pass this as the file.
 */
#ifndef CORE_CURL_PATH_H
#define CORE_CURL_PATH_H

#include <unistd.h>

static inline const char *liu_curl_exe(void) {
    return access("/usr/bin/curl", X_OK) == 0 ? "/usr/bin/curl" : "curl";
}

#endif /* CORE_CURL_PATH_H */
