/*
 * The --version and --help options print to stdout and exit before the app
 * opens a window or restores session state.
 */
#ifndef CORE_CLI_ARGS_H
#define CORE_CLI_ARGS_H

#include "core/types.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    LIU_CLI_LAUNCH,    /* not a question: start the app */
    LIU_CLI_VERSION,
    LIU_CLI_HELP,
} LiuCliAsk;

/* Only the FIRST argument is read, and only these exact spellings. */
static inline LiuCliAsk liu_cli_ask(int argc, char **argv) {
    if (argc < 2 || !argv || !argv[1]) return LIU_CLI_LAUNCH;
    if (strcmp(argv[1], "--version") == 0) return LIU_CLI_VERSION;
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) return LIU_CLI_HELP;
    return LIU_CLI_LAUNCH;
}

/* The answer, as text for stdout. Returns the length written, 0 when `ask` is
 * not a question or the text does not fit (nothing is cut in half). */
static inline i32 liu_cli_answer(LiuCliAsk ask, const char *version, const char *build,
                                 const char *arch, char *out, usize cap) {
    int n = 0;
    if (cap) out[0] = '\0';
    if (ask == LIU_CLI_VERSION) {
        n = snprintf(out, cap, "Liu %s (%s) %s\n", version, build, arch);
    } else if (ask == LIU_CLI_HELP) {
        n = snprintf(out, cap,
            "Liu %s - a terminal for AI coding agents\n"
            "\n"
            "Usage: Liu [--version | --help]\n"
            "\n"
            "Liu is a windowed app: started with no arguments it opens a window.\n"
            "Settings are in the app and in ~/.config/Liu/config.json.\n"
            "\n"
            "Beside this binary:\n"
            "  liu-history   list, read and continue agent sessions (liu-history --help)\n"
            "  liu-notify    the notification helper agents' hooks call (liu-notify --help)\n",
            version);
    }
    if (n <= 0 || (usize)n >= cap) { if (cap) out[0] = '\0'; return 0; }
    return (i32)n;
}

#endif /* CORE_CLI_ARGS_H */
