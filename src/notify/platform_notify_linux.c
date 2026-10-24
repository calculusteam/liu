/*
 * liu-notify - Linux TTS + desktop backend.
 *
 *   TTS           : execvp espeak-ng / espeak / spd-say with argv (no shell).
 *   desktop banner: execvp notify-send with argv if present (also no shell).
 *
 * fork + execvp, so the payload bytes never pass through a shell
 * interpreter. argv elements are NOT parsed as /bin/sh input.
 */
#include "notify/platform_notify.h"
#include "notify/markup_escape.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
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
 * Reaping the children this file starts — and ONLY those.
 *
 * The banner (notify-send) and sound (paplay/aplay) children are
 * fire-and-forget: nobody waits for them, so each one has to be collected by
 * someone or it stays a zombie. Setting SIGCHLD to SIG_IGN + SA_NOCLDWAIT
 * would make the kernel discard EVERY child's exit status — for the whole
 * process. In the standalone daemon that process is liu-notify. In the app it
 * is the GUI: tts_init() runs on the in-process server's thread, and from that
 * moment every waitpid() anywhere in Liu would fail with ECHILD — git's exit
 * status for the status bar, the updater's curl, a session's exit code.
 *
 * So the disposition is left alone and each child gets what the macOS file
 * gives its own (reap_spawned_child there): one short-lived detached thread
 * parked in waitpid() on that pid. No table to scan, no polling, no fork() in
 * a threaded process, and nothing another part of the app can observe.
 */
static void *reap_child_main(void *arg) {
    pid_t pid = (pid_t)(intptr_t)arg;
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    /* ECHILD here means someone else collected it (a waitpid(-1) elsewhere);
     * either way there is no zombie left. */
    return NULL;
}

static void reap_spawned_child(pid_t pid) {
    if (pid <= 0) return;
    bool started = false;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) == 0) {
        /* The thread only sits in waitpid(): the smallest stack will do. */
        size_t stack = 64u * 1024u;
        if (stack < (size_t)PTHREAD_STACK_MIN) stack = (size_t)PTHREAD_STACK_MIN;
        (void)pthread_attr_setstacksize(&attr, stack);
        pthread_t th;
        if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0)
            started = pthread_create(&th, &attr, reap_child_main,
                                     (void *)(intptr_t)pid) == 0;
        pthread_attr_destroy(&attr);
    }
    /* No thread to be had: take the child if it has already gone, and leave
     * one zombie behind until the process exits if it has not. That beats
     * blocking the caller — which may be the app's main thread. */
    if (!started) (void)waitpid(pid, NULL, WNOHANG);
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
    select_engine();
    if (!g_engine) {
        fprintf(stderr, "liu-notify: no TTS engine found "
                        "(install espeak-ng, espeak, or speech-dispatcher)\n");
        return false;
    }
    return true;
}

void tts_shutdown(void) { tts_cancel(); }

/* The speech child is the one child that IS waited for by pid (tts_busy polls
 * it, tts_cancel collects it), so it needs no reaper thread — and must not
 * have one, or the two would race for its status. */
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

    /* If a previous utterance is still going, let it finish — spd-say queues
     * internally, but espeak does not. For simplicity the new one is skipped.
     */
    reap_tts(WNOHANG);
    if (g_tts_pid > 0) return;

    if (!isfinite(rate) || rate == 0.0f) rate = 1.0f;
    if (rate < 0.5f) rate = 0.5f;
    if (rate > 2.0f) rate = 2.0f;
    char rate_buf[16];
    snprintf(rate_buf, sizeof rate_buf, "%d",
             g_engine->kind == ENG_ESPEAK ? (int)(175.0f * rate)
                                          : (int)(100.0f * (rate - 1.0f)));

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
    /* The daemon may render the body as markup (<a href>, <img>), and this text is an agent's
     * own summary: escape it so it can only ever be text (notify/markup_escape.h). Four times
     * the source covers the worst expansion (& -> &amp;). */
    char safe_title[4 * 96 + 1], safe_body[4 * 1024 + 1];
    (void)notify_escape_markup(title, safe_title, sizeof safe_title);
    (void)notify_escape_markup(body, safe_body, sizeof safe_body);
    /* "--" ends option parsing: a title or body beginning with '-' is text,
     * not a notify-send flag. */
    char *argv[] = { "notify-send", "-a", "liu-notify", "--",
                     safe_title, safe_body, NULL };
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) return;
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    if (posix_spawnp(&pid, "notify-send", &fa, NULL, argv, environ) == 0) {
        reap_spawned_child(pid);
    }
    posix_spawn_file_actions_destroy(&fa);
}

/* Called from two threads in the app (the notify server's loop and the main
 * thread's done-panel sound), like its macOS twin. It keeps no state of its
 * own — every call spawns one independent player — so there is nothing here to
 * lock; reap_spawned_child is the only shared mechanism and it has no state
 * either. */
bool notify_play_sound(const char *path) {
    if (!path || !*path) return false;
    /* Best-effort: spawn paplay or aplay; silent on missing binary. */
    char *argv[] = { "paplay", "--", (char *)path, NULL };
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) return false;
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int rc = posix_spawnp(&pid, "paplay", &fa, NULL, argv, environ);
    if (rc != 0) {
        char *alt[] = { "aplay", "--", (char *)path, NULL };
        rc = posix_spawnp(&pid, "aplay", &fa, NULL, alt, environ);
    }
    posix_spawn_file_actions_destroy(&fa);
    if (rc == 0) { reap_spawned_child(pid); return true; }
    return false;
}
bool notify_sound_busy(void)   { return false; }
void notify_sound_cancel(void) {}
/* notify_target_active() is platform-independent — notify/focus_state.c
 * answers it from the frontmost flag the app publishes. */
