/*
 * liu-notify - Linux TTS + desktop backend.
 *
 *   TTS           : execvp espeak-ng / espeak / spd-say with argv (no shell).
 *   desktop banner: execvp notify-send with argv if present (also no shell).
 *
 * We fork + execvp so the payload bytes never pass through a shell
 * interpreter. argv elements are NOT parsed as /bin/sh input.
 */
#include "notify/platform_notify.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* Ordered preference for speech engines. First one found in $PATH wins. */
typedef struct { const char *name; enum { ENG_ESPEAK, ENG_SPD } kind; } SpeechEngine;
static const SpeechEngine k_engines[] = {
    { "espeak-ng", ENG_ESPEAK },
    { "espeak",    ENG_ESPEAK },
    { "spd-say",   ENG_SPD    },
};

static const SpeechEngine *g_engine = NULL;
static volatile pid_t      g_tts_pid = 0;

/*
 * Auto-reap spawned children. The banner (notify-send) and sound (paplay/
 * aplay) paths fire-and-forget — their single WNOHANG waitpid almost always
 * returns 0 (child still alive), so without this every notification would
 * leak a zombie. Setting SIGCHLD to SIG_IGN tells the kernel to reap exited
 * children automatically. The TTS path's specific-pid waitpid then returns
 * -1/ECHILD, which reap_tts() already treats as "finished" (clears the pid).
 */
static void install_child_reaper(void) {
    static bool done = false;
    if (done) return;
    done = true;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sa.sa_flags   = SA_NOCLDWAIT;
    sigaction(SIGCHLD, &sa, NULL);
}

static bool in_path(const char *prog) {
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/bin:/bin:/usr/local/bin";
    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t n = colon ? (size_t)(colon - p) : strlen(p);
        char buf[512];
        if (n > 0 && n + strlen(prog) + 2 < sizeof buf) {
            memcpy(buf, p, n);
            buf[n] = '/';
            snprintf(buf + n + 1, sizeof buf - n - 1, "%s", prog);
            struct stat st;
            if (stat(buf, &st) == 0 && (st.st_mode & S_IXUSR)) return true;
        }
        if (!colon) break;
        p = colon + 1;
    }
    return false;
}

static void select_engine(void) {
    if (g_engine) return;
    for (size_t i = 0; i < sizeof k_engines / sizeof k_engines[0]; i++) {
        if (in_path(k_engines[i].name)) { g_engine = &k_engines[i]; return; }
    }
}

bool tts_init(void) {
    install_child_reaper();
    select_engine();
    if (!g_engine) {
        fprintf(stderr, "liu-notify: no TTS engine found "
                        "(install espeak-ng, espeak, or speech-dispatcher)\n");
        return false;
    }
    return true;
}

void tts_shutdown(void) { tts_cancel(); }

static void reap_tts(int opts) {
    if (g_tts_pid > 0) {
        int status;
        pid_t r = waitpid(g_tts_pid, &status, opts);
        if (r == g_tts_pid || r < 0) g_tts_pid = 0;
    }
}

void tts_speak(const char *text, const char *voice, f32 rate) {
    if (!text || !*text) return;
    select_engine();
    if (!g_engine) return;

    /* If a previous utterance is still going, let it finish — spd-say
     * queues internally, but espeak does not. For simplicity we just skip. */
    reap_tts(WNOHANG);
    if (g_tts_pid > 0) return;

    char rate_buf[16];
    snprintf(rate_buf, sizeof rate_buf, "%d",
             g_engine->kind == ENG_ESPEAK ? (int)(175.0f * (rate ? rate : 1.0f))
                                          : (int)(100.0f * ((rate ? rate : 1.0f) - 1.0f)));

    char *argv[12];
    int argc = 0;
    argv[argc++] = (char *)g_engine->name;
    if (g_engine->kind == ENG_ESPEAK) {
        argv[argc++] = "-s"; argv[argc++] = rate_buf;
        if (voice && *voice) { argv[argc++] = "-v"; argv[argc++] = (char *)voice; }
        argv[argc++] = "--"; argv[argc++] = (char *)text;
    } else { /* spd-say */
        argv[argc++] = "-r"; argv[argc++] = rate_buf;
        if (voice && *voice) { argv[argc++] = "-l"; argv[argc++] = (char *)voice; }
        argv[argc++] = "--"; argv[argc++] = (char *)text;
    }
    argv[argc] = NULL;

    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) return;
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);

    pid_t pid = 0;
    if (posix_spawnp(&pid, g_engine->name, &fa, NULL, argv, environ) == 0) {
        g_tts_pid = pid;
    }
    posix_spawn_file_actions_destroy(&fa);
}

bool tts_busy(void) {
    reap_tts(WNOHANG);
    return g_tts_pid > 0;
}

void tts_cancel(void) {
    if (g_tts_pid > 0) {
        kill(g_tts_pid, SIGTERM);
        reap_tts(0);
    }
}

void platform_notify_desktop(const char *title, const char *body) {
    if (!title || !body) return;
    if (!in_path("notify-send")) return;
    install_child_reaper();
    char *argv[] = { "notify-send", "-a", "liu-notify",
                     (char *)title, (char *)body, NULL };
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) return;
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    if (posix_spawnp(&pid, "notify-send", &fa, NULL, argv, environ) == 0) {
        (void)waitpid(pid, NULL, WNOHANG);
    }
    posix_spawn_file_actions_destroy(&fa);
}

bool notify_play_sound(const char *path) {
    if (!path || !*path) return false;
    install_child_reaper();
    /* Best-effort: spawn paplay or aplay; silent on missing binary. */
    char *argv[] = { "paplay", (char *)path, NULL };
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) return false;
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int rc = posix_spawnp(&pid, "paplay", &fa, NULL, argv, environ);
    if (rc != 0) {
        char *alt[] = { "aplay", (char *)path, NULL };
        rc = posix_spawnp(&pid, "aplay", &fa, NULL, alt, environ);
    }
    posix_spawn_file_actions_destroy(&fa);
    if (rc == 0) { (void)waitpid(pid, NULL, WNOHANG); return true; }
    return false;
}
bool notify_sound_busy(void)   { return false; }
void notify_sound_cancel(void) {}
bool notify_target_active(void) { return false; }

