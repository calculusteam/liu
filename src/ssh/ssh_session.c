/*
 * Liu - SSH + local PTY session implementation
 */
#include "ssh/ssh_session.h"
#include "core/config.h"       /* liu_executable_dir() */
#include "core/memory.h"       /* secure_zero() */
#include "vault/crypto.h"      /* crypto_random() — CSPRNG for X11 cookie */
#include "platform/platform.h" /* platform_unwatch_socket — cancel dispatch source before close */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <strings.h>
#include <time.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <pwd.h>
#if defined(PLATFORM_MACOS)
    #include <util.h>
    #include <libproc.h>
    #include <sys/sysctl.h>   /* KERN_PROCARGS2 — resolve node-script CLIs */
#else
    #include <pty.h>
#endif

#ifdef PLATFORM_MACOS
static bool session_proc_name_is_agent(const char *name) {
    if (!name || !name[0]) return false;
    return strcmp(name, "claude") == 0 ||
           strcmp(name, "codex") == 0 ||
           strcmp(name, "gpt") == 0 ||
           strcmp(name, "chatgpt") == 0 ||
           strcmp(name, "openai") == 0 ||
           strcmp(name, "copilot") == 0 ||
           strcmp(name, "opencode") == 0 ||
           strcmp(name, "cursor-agent") == 0 ||
           strcmp(name, "cursor") == 0 ||
           strcmp(name, "amp") == 0 ||
           strcmp(name, "cline") == 0 ||
           strcmp(name, "roo") == 0 ||
           strcmp(name, "kilo") == 0 ||
           strcmp(name, "kiro") == 0 ||
           strcmp(name, "crush") == 0 ||
           strcmp(name, "droid") == 0 ||
           strcmp(name, "antigravity") == 0 ||
           strcmp(name, "kimi") == 0 ||
           strcmp(name, "qwen") == 0 ||
           strcmp(name, "aider") == 0 ||
           strcmp(name, "amazon-q") == 0 ||
           strcmp(name, "amazonq") == 0 ||
           strcmp(name, "continue") == 0 ||
           strcmp(name, "windsurf") == 0 ||
           strcmp(name, "zed") == 0 ||
           strcmp(name, "commandcode") == 0 ||
           strcmp(name, "command-code") == 0 ||
           strcmp(name, "cmd") == 0 ||
           strcmp(name, "grok") == 0 ||
           strcmp(name, "xai") == 0;
}

static bool session_proc_name_is_wrapper(const char *name) {
    if (!name || !name[0]) return true;
    return strcmp(name, "sh") == 0 ||
           strcmp(name, "bash") == 0 ||
           strcmp(name, "zsh") == 0 ||
           strcmp(name, "fish") == 0 ||
           strcmp(name, "node") == 0 ||
           strcmp(name, "npm") == 0 ||
           strcmp(name, "npx") == 0 ||
           strcmp(name, "bun") == 0 ||
           strcmp(name, "env") == 0;
}

/* A node/bun/python wrapper hides the real CLI behind the interpreter's
 * process name ("node"). Read the process arguments (KERN_PROCARGS2) and
 * return the basename of the first non-flag argv entry that names a known
 * agent — that's the script path, e.g. ".../commandcode" → "commandcode".
 * This is what lets script-based CLIs (codex, commandcode, grok …)
 * be recognized the same way native binaries (claude, opencode) already are. */
static bool session_proc_argv_agent(pid_t pid, char *out, usize out_cap) {
    int argmax = 0;
    size_t am_sz = sizeof argmax;
    int mib_am[2] = { CTL_KERN, KERN_ARGMAX };
    if (sysctl(mib_am, 2, &argmax, &am_sz, NULL, 0) != 0 || argmax <= 0)
        argmax = 256 * 1024;
    char *buf = (char *)malloc((size_t)argmax);
    if (!buf) return false;

    size_t bsz = (size_t)argmax;
    int mib[3] = { CTL_KERN, KERN_PROCARGS2, (int)pid };
    if (sysctl(mib, 3, buf, &bsz, NULL, 0) != 0 || bsz < sizeof(int)) {
        free(buf);
        return false;
    }

    int argc = 0;
    memcpy(&argc, buf, sizeof argc);
    char *p   = buf + sizeof(int);
    char *end = buf + bsz;

    /* Layout: argc | exec_path\0 | \0-padding | argv[0]\0 argv[1]\0 … */
    while (p < end && *p != '\0') p++;   /* skip exec_path        */
    while (p < end && *p == '\0') p++;   /* skip padding to argv0 */

    bool found = false;
    for (int ai = 0; ai < argc && p < end; ai++) {
        char *arg = p;
        while (p < end && *p != '\0') p++;
        if (p < end) p++;                /* step past the NUL */
        if (ai == 0) continue;           /* argv[0] = interpreter */
        const char *bn = strrchr(arg, '/');
        bn = bn ? bn + 1 : arg;
        if (bn[0] == '-' || bn[0] == '\0') continue;   /* a flag */
        if (session_proc_name_is_agent(bn)) {
            snprintf(out, out_cap, "%s", bn);
            found = true;
            break;
        }
    }
    free(buf);
    return found;
}

/* A CLI compiled with Bun/PyInstaller re-execs into a versioned binary whose
 * accounting name is a bare version string — Claude Code installs at
 * ~/.local/share/claude/versions/<ver> and runs with comm "2.1.150", so it is
 * neither a known agent name nor an interpreter whose argv we can scan. As a
 * last resort, inspect the executable path itself: if any path component names
 * a known agent (e.g. ".../claude/versions/2.1.150" → "claude"), use it. The
 * >=5-char guard keeps short, ambiguous ids (amp, zed, cmd, gpt) from matching
 * an incidental directory name. */
static bool session_proc_path_agent(pid_t pid, char *out, usize out_cap) {
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, path, sizeof path) <= 0) return false;
    char *save = NULL;
    for (char *tok = strtok_r(path, "/", &save); tok;
         tok = strtok_r(NULL, "/", &save)) {
        if (strlen(tok) >= 5 && session_proc_name_is_agent(tok)) {
            snprintf(out, out_cap, "%s", tok);
            return true;
        }
    }
    return false;
}

/* A CLI shipped as a single self-contained native binary often runs under a
 * versioned process name: grok installs to ~/.grok and runs as
 * "grok-0.2.22-macos-aarch64" (comm truncated to "grok-0.2.22-mac"). Match the
 * leading token — up to the first '-' or '.' — against the known-agent set.
 * The delimiter must be present (a bare exact name is already handled by
 * session_proc_name_is_agent), and the leading token must itself be an exact
 * agent id, so dashed ids like "amazon-q" / "command-code" can't partial-match
 * (their head "amazon" / "command" is not a known agent). */
static bool session_proc_name_versioned_agent(const char *name,
                                              char *out, usize out_cap) {
    if (!name || !name[0]) return false;
    char head[32];
    usize n = 0;
    while (name[n] && name[n] != '-' && name[n] != '.' && n + 1 < sizeof head) {
        head[n] = name[n];
        n++;
    }
    head[n] = '\0';
    if (name[n] != '-' && name[n] != '.') return false;   /* no version suffix */
    if (!session_proc_name_is_agent(head)) return false;
    snprintf(out, out_cap, "%s", head);
    return true;
}

static bool session_find_interesting_pgrp_process(pid_t pgid,
                                                  const char *leader,
                                                  char *out, usize out_cap) {
    if (!out || out_cap == 0) return false;
    out[0] = '\0';
    if (pgid <= 0) return false;

    i32 bytes = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (bytes <= 0) return false;
    pid_t *pids = (pid_t *)malloc((usize)bytes);
    if (!pids) return false;

    i32 got = proc_listpids(PROC_ALL_PIDS, 0, pids, bytes);
    i32 count = got > 0 ? got / (i32)sizeof(pid_t) : 0;
    char fallback[256] = {0};

    for (i32 i = 0; i < count; i++) {
        if (pids[i] <= 0) continue;
        struct proc_bsdinfo bi;
        i32 n = proc_pidinfo(pids[i], PROC_PIDTBSDINFO, 0, &bi, sizeof bi);
        if (n != (i32)sizeof bi || (pid_t)bi.pbi_pgid != pgid || !bi.pbi_name[0]) continue;

        if (session_proc_name_is_agent(bi.pbi_name)) {
            snprintf(out, out_cap, "%s", bi.pbi_name);
            free(pids);
            return true;
        }
        /* Versioned native binary (e.g. grok's "grok-0.2.22-macos-aarch64").
         * Recover the leading agent id before falling through to wrapper/argv. */
        if (session_proc_name_versioned_agent(bi.pbi_name, out, out_cap)) {
            free(pids);
            return true;
        }
        if (session_proc_name_is_wrapper(bi.pbi_name)) {
            /* node/bun/python interpreter → inspect its argv for the script. */
            if (session_proc_argv_agent(pids[i], out, out_cap)) {
                free(pids);
                return true;
            }
        } else {
            /* Opaque name (e.g. a Bun-compiled CLI re-exec'd as its bare
             * version string, like Claude Code's "2.1.150") → recover the
             * agent from the executable path before treating it as fallback. */
            if (session_proc_path_agent(pids[i], out, out_cap)) {
                free(pids);
                return true;
            }
            if (!fallback[0])
                snprintf(fallback, sizeof fallback, "%s", bi.pbi_name);
        }
    }

    free(pids);
    if (fallback[0]) {
        snprintf(out, out_cap, "%s", fallback);
        return true;
    }
    if (leader && leader[0]) {
        snprintf(out, out_cap, "%s", leader);
        return true;
    }
    return false;
}
#endif

#include <libssh2.h>
#include <libssh2_sftp.h>

#ifndef PLATFORM_WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netdb.h>
#include <poll.h>
#endif

#include "core/net.h"
#include "ssh/known_hosts.h"
#include "ssh/mosh.h"
#include "ssh/telnet.h"
#include "ssh/serial.h"
#include "ssh/port_forward.h"

#define SESSION_WRITE_BUFFER_LIMIT ((usize)(4u * 1024u * 1024u))
#define SESSION_WRITE_FLUSH_LIMIT  ((usize)(256u * 1024u))

/* =========================================================================
 * X11 forwarding relay
 * ========================================================================= */

#define MAX_X11_CHANNELS 16

typedef struct {
    LIBSSH2_CHANNEL *channel;
    int              local_fd;    /* connected to local X11 display */
    bool             active;
} X11Relay;

/* Pending X11 channels received via callback but not yet connected locally */
static LIBSSH2_CHANNEL *g_pending_x11[MAX_X11_CHANNELS];
static i32 g_pending_x11_count = 0;

/* X11 callback invoked by libssh2 when the server opens an X11 channel */
static void x11_open_callback(LIBSSH2_SESSION *session, LIBSSH2_CHANNEL *channel,
                                const char *shost, int sport, void **abstract) {
    (void)session; (void)shost; (void)sport; (void)abstract;
    if (g_pending_x11_count < MAX_X11_CHANNELS) {
        g_pending_x11[g_pending_x11_count++] = channel;
    } else {
        /* Too many X11 channels -- reject */
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
    }
}

/* =========================================================================
 * Session struct
 * ========================================================================= */

/* Proxy hop runtime state for multi-hop connections */
typedef struct {
    int              sock;
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *tunnel;   /* direct-tcpip channel to next hop */
} ProxyHopState;

/* Channel relay for proxying data between socketpair and SSH channel */
typedef struct {
    int              local_fd;   /* one end of socketpair */
    LIBSSH2_CHANNEL *channel;   /* direct-tcpip channel */
    LIBSSH2_SESSION *session;   /* owning session (for blocking mode) */
    volatile bool    running;
    pthread_t        thread;
} ChannelRelay;

/* Remote forward listener state */
typedef struct {
    LIBSSH2_LISTENER *listener;
    char bind_host[256];
    i32  bind_port;
    char dest_host[256];
    i32  dest_port;
    bool active;
} RemoteForwardListener;

/* Remote forward accepted connection */
typedef struct {
    LIBSSH2_CHANNEL *channel;
    int              dest_sock;
    pthread_t        thread;
    volatile bool    running;
} RemoteForwardConn;

#define MAX_REMOTE_LISTENERS  MAX_PORT_FORWARDS
#define MAX_REMOTE_FWD_CONNS  64

struct Session {
    SessionType   type;
    SessionStatus status;
    char          error_msg[256];
    i32           cols, rows;

    /* Local PTY */
    int           pty_master;
    pid_t         child_pid;

    /* Exit-code capture for managed commands (Sites / dev-server manager).
     * session_is_alive() reaps the child with waitpid(WNOHANG) but otherwise
     * discards the status; we stash it here so session_exited() can report the
     * real exit code without a second (racy) waitpid. */
    int           last_exit_status;
    bool          exited;

    /* session_fg_process() TTL cache. That call runs on every typed character
     * (autosuggest) and on macOS scans every pid on the system, so cache the
     * result per session and re-scan only when the fg process group changes or
     * the short TTL lapses. */
    char          fg_cache[256];
    f64           fg_cache_at;     /* monotonic seconds of last scan (0 = never) */
    pid_t         fg_cache_pgid;   /* fg pgid the cache was computed for */

    /* Sleep/suspend (SIGSTOP/SIGCONT on child_pid) */
    bool          suspended;

    /* SSH */
    int             sock;
    LIBSSH2_SESSION *ssh_session;
    LIBSSH2_CHANNEL *ssh_channel;
    LIBSSH2_SFTP    *sftp_session;
    SSHConfig       *config;   /* NULL for local sessions */
    u8              *write_buf;
    usize            write_len;
    usize            write_cap;

    /* ControlMaster: session reuse */
    Session        *shared_session;   /* if non-NULL, we borrow this session's SSH conn */
    bool            is_shared_owner;  /* true = we own ssh_session/sock, false = borrowed */

    /* StreamLocal forwarding */
    int             stream_fwd_fds[4];  /* listening Unix socket fds (-1 if unused) */
    i32             stream_fwd_count;
    bool            stream_fwd_active;

    /* connect worker — runs TCP + libssh2 handshake + auth off the
     * main thread so the UI never freezes during connect.  After publishing
     * SESSION_CONNECTED the worker exits and libssh2 ownership passes to
     * the main thread, which does all subsequent non-blocking I/O. */
    pthread_t       connect_thread;
    bool            connect_thread_active;
    atomic_bool     connect_cancel;
    pthread_mutex_t status_lock;

    /* Background SFTP worker owns libssh2 exclusively for the duration of
     * a multi-GB transfer. scope_begin flips the session to blocking mode;
     * if the main thread concurrently calls libssh2_channel_read/_write on
     * the same LIBSSH2_SESSION it now blocks instead of returning EAGAIN,
     * freezing the whole event loop. The main thread reads this flag via
     * session_io_is_suspended() and skips its poll for that session until
     * the worker clears it. */
    atomic_bool     io_suspended;

    /* Passphrase prompt: worker blocks on cond until UI supplies passphrase */
    bool            passphrase_pending;
    char            passphrase_prompt_path[512];
    pthread_cond_t  passphrase_cond;
    bool            passphrase_supplied;  /* UI set this before signal */
    bool            passphrase_cancelled; /* UI cancelled the prompt */

    /* Host key dialog — worker blocks on cond, UI wakes it after user decision */
    pthread_mutex_t hostkey_lock;
    pthread_cond_t  hostkey_cond;
    bool            hostkey_pending;   /* worker set: waiting for UI response */
    bool            hostkey_accepted;  /* UI set: user accepted the new key */
    bool            hostkey_is_change; /* true = key changed, false = new key */
    char            hostkey_old_fp[128];
    char            hostkey_new_fp[128];

    /* Keyboard-interactive (KBI / 2FA) dialog. Prompts/responses are heap-
     * allocated on first KBI challenge — local PTY sessions and SSH sessions
     * that never face 2FA never pay the 4 KB. */
    pthread_mutex_t kbi_lock;
    pthread_cond_t  kbi_cond;
    bool            kbi_ready;         /* UI set: responses are filled in */
    char            kbi_name[256];
    char            kbi_instruction[512];
    i32             kbi_num_prompts;
    char          (*kbi_prompts)[256];     /* heap [KBI_MAX_PROMPTS][256] */
    bool            kbi_echo[KBI_MAX_PROMPTS];
    char          (*kbi_responses)[256];   /* heap [KBI_MAX_PROMPTS][256] */

    /* ProxyJump multi-hop state */
    ProxyHopState   proxy_hops[MAX_PROXY_HOPS];
    i32             proxy_hop_count;
    ChannelRelay   *proxy_relays[MAX_PROXY_HOPS];

    /* Keepalive */
    f64             last_keepalive_time;
    i32             keepalive_missed;
    i32             keepalive_interval;
    i32             keepalive_count_max;

    /* Remote port forwarding. Both arrays are lazy — typical sessions use
     * zero remote forwards, and even forwarders use only a handful. We
     * allocate the worst-case slot pool on first use rather than baking
     * 8.5 KB + 2 KB into every Session. */
    RemoteForwardListener *remote_listeners;   /* heap [MAX_REMOTE_LISTENERS] when non-NULL */
    i32                    remote_listener_count;
    RemoteForwardConn     *remote_fwd_conns;   /* heap [MAX_REMOTE_FWD_CONNS] when non-NULL */
    i32                    remote_fwd_conn_count;

    /* X11 forwarding (lazy — local sessions and SSH targets without X11
     * never alloc the relay slots). */
    X11Relay       *x11_relays;            /* heap [MAX_X11_CHANNELS] when non-NULL */
    i32             x11_relay_count;

    /* Mosh (SESSION_MOSH only) */
    MoshSession    *mosh;

    /* Telnet / Serial (their SESSION_* types only). Single-threaded like
     * LOCAL: no worker, so no status_lock involvement. */
    TelnetSession  *telnet;
    SerialSession  *serial;

    /* SFTP auto-init: populated by the connect worker before publishing
     * SESSION_CONNECTED. Readers on the main thread see these values
     * after an acquire-ordered read of s->status, so no extra lock. The
     * remote home path is heap-allocated when sftp_realpath returns; local
     * sessions and SSH targets without SFTP support stay at NULL. */
    char           *initial_cwd;
    bool            sftp_init_tried;
};

/* =========================================================================
 * Session pool for ControlMaster (connection multiplexing)
 *
 * When control_master is enabled, we reuse an existing LIBSSH2_SESSION to
 * the same host:port+user rather than opening a new TCP connection.  The
 * pool is a simple array scanned linearly — the number of distinct SSH
 * destinations is tiny in practice.
 * ========================================================================= */

#define SESSION_POOL_MAX 64

static struct {
    Session *sessions[SESSION_POOL_MAX];
    i32      count;
    pthread_mutex_t lock;
    bool     initialized;
} g_session_pool = {0};

static void session_pool_init_once(void) {
    if (!g_session_pool.initialized) {
        pthread_mutex_init(&g_session_pool.lock, NULL);
        g_session_pool.initialized = true;
    }
}

/* Find a connected session matching host:port + user.  Returns NULL if none.
 * Caller must hold g_session_pool.lock. */
static Session *session_pool_find_locked(const char *hostname, i32 port,
                                          const char *username) {
    for (i32 i = 0; i < g_session_pool.count; i++) {
        Session *s = g_session_pool.sessions[i];
        if (!s || !s->config) continue;
        if (s->status != SESSION_CONNECTED) continue;
        if (s->config->port != port) continue;
        if (strcmp(s->config->hostname, hostname) != 0) continue;
        if (strcmp(s->config->username, username) != 0) continue;
        return s;
    }
    return NULL;
}

/* =========================================================================
 * Helpers
 * ========================================================================= */

static void set_error(Session *s, const char *msg) {
    /* SSH/Mosh sessions may have a worker thread writing status while
     * main reads it. Serialize. Local sessions don't init the lock. */
    if (s->type == SESSION_SSH || s->type == SESSION_MOSH) {
        pthread_mutex_lock(&s->status_lock);
        s->status = SESSION_ERROR;
        snprintf(s->error_msg, sizeof(s->error_msg), "%s", msg);
        pthread_mutex_unlock(&s->status_lock);
    } else {
        s->status = SESSION_ERROR;
        snprintf(s->error_msg, sizeof(s->error_msg), "%s", msg);
    }
}

/* tcp_connect and set_nonblocking are in core/net.c */

static LIBSSH2_USERAUTH_KBDINT_RESPONSE_FUNC(ssh_kbdint_callback) {
    Session *s = NULL;
    if (abstract && *abstract) {
        s = (Session *)*abstract;
    }
    if (!s) return;

    const SSHConfig *config = s->config;

    /* Fast path: single password-like prompt with a password in config —
     * auto-fill without blocking on UI. */
    if (num_prompts == 1 && config && config->password[0]) {
        /* Check if the prompt looks like a password prompt */
        bool looks_like_password = false;
        if (prompts[0].length == 0) {
            looks_like_password = true;
        } else {
            const char *p = (const char *)prompts[0].text;
            usize plen = prompts[0].length;
            /* Case-insensitive check for "password" in prompt text */
            for (usize j = 0; j + 8 <= plen; j++) {
                if (strncasecmp(p + j, "password", 8) == 0) {
                    looks_like_password = true;
                    break;
                }
            }
        }
        if (looks_like_password) {
            const char *reply = config->password;
            usize reply_len = strlen(reply);
            responses[0].text = malloc(reply_len + 1);
            if (responses[0].text) {
                memcpy(responses[0].text, reply, reply_len + 1);
                responses[0].length = (unsigned int)reply_len;
            } else {
                responses[0].length = 0;
            }
            return;
        }
    }

    /* Multi-prompt / OTP / 2FA path: store prompts in Session, set
     * SESSION_KBI_PENDING, and block until the UI fills in responses. */
    pthread_mutex_lock(&s->kbi_lock);

    /* Copy name and instruction for UI display */
    if (name && name_len > 0) {
        usize n = ((usize)name_len < sizeof(s->kbi_name) - 1) ? (usize)name_len : sizeof(s->kbi_name) - 1;
        memcpy(s->kbi_name, name, n);
        s->kbi_name[n] = '\0';
    } else {
        s->kbi_name[0] = '\0';
    }

    if (instruction && instruction_len > 0) {
        usize n = ((usize)instruction_len < sizeof(s->kbi_instruction) - 1) ? (usize)instruction_len : sizeof(s->kbi_instruction) - 1;
        memcpy(s->kbi_instruction, instruction, n);
        s->kbi_instruction[n] = '\0';
    } else {
        s->kbi_instruction[0] = '\0';
    }

    i32 count = (num_prompts > KBI_MAX_PROMPTS) ? KBI_MAX_PROMPTS : num_prompts;
    s->kbi_num_prompts = count;

    /* Lazy-alloc prompt + response pools the first time this session faces
     * a KBI challenge. Local sessions and pubkey-only SSH targets never
     * trigger this path. */
    if (!s->kbi_prompts) {
        s->kbi_prompts = calloc(KBI_MAX_PROMPTS, sizeof(*s->kbi_prompts));
    }
    if (!s->kbi_responses) {
        s->kbi_responses = calloc(KBI_MAX_PROMPTS, sizeof(*s->kbi_responses));
    }
    if (!s->kbi_prompts || !s->kbi_responses) {
        /* Out of memory: skip KBI, treat as auth failure downstream. The
         * caller will see kbi_num_prompts=0 plus empty responses[]. */
        s->kbi_num_prompts = 0;
        pthread_mutex_unlock(&s->kbi_lock);
        for (int i = 0; i < num_prompts; i++) responses[i].length = 0;
        return;
    }

    for (i32 i = 0; i < count; i++) {
        usize plen = prompts[i].length;
        if (plen >= sizeof(s->kbi_prompts[i]))
            plen = sizeof(s->kbi_prompts[i]) - 1;
        memcpy(s->kbi_prompts[i], prompts[i].text, plen);
        s->kbi_prompts[i][plen] = '\0';
        s->kbi_echo[i] = prompts[i].echo != 0;
        s->kbi_responses[i][0] = '\0';
    }

    s->kbi_ready = false;

    /* Publish SESSION_KBI_PENDING so UI knows to show the dialog */
    pthread_mutex_lock(&s->status_lock);
    s->status = SESSION_KBI_PENDING;
    pthread_mutex_unlock(&s->status_lock);

    /* Block until UI thread calls session_kbi_submit() */
    while (!s->kbi_ready) {
        pthread_cond_wait(&s->kbi_cond, &s->kbi_lock);
    }

    /* Restore status to authenticating */
    pthread_mutex_lock(&s->status_lock);
    s->status = SESSION_AUTHENTICATING;
    pthread_mutex_unlock(&s->status_lock);

    /* Fill libssh2 responses from what UI provided */
    for (int i = 0; i < num_prompts; i++) {
        const char *reply = "";
        if (i < count) reply = s->kbi_responses[i];
        usize reply_len = strlen(reply);
        responses[i].text = malloc(reply_len + 1);
        if (responses[i].text) {
            memcpy(responses[i].text, reply, reply_len + 1);
            responses[i].length = (unsigned int)reply_len;
        } else {
            responses[i].length = 0;
        }
    }

    pthread_mutex_unlock(&s->kbi_lock);
}

__attribute__((unused)) static bool ssh_prompt_trust_hostkey(const char *hostname, i32 port, const char *fingerprint) {
    if (!isatty(STDIN_FILENO)) {
        return false;
    }

    fprintf(stderr,
            "\nTrust new SSH host key for %s:%d?\nFingerprint: %s\nType 'yes' to trust: ",
            hostname, port, fingerprint ? fingerprint : "(unknown)");
    fflush(stderr);

    char answer[32] = {0};
    if (!fgets(answer, sizeof(answer), stdin)) {
        return false;
    }
    return strcmp(answer, "yes\n") == 0 || strcmp(answer, "yes") == 0;
}

static int ssh_verify_hostkey(Session *s) {
    HostKeyStatus hk = known_hosts_verify(s->ssh_session, s->config->hostname, s->config->port);
    if (hk == HOST_KEY_OK) return 0;
    if (hk == HOST_KEY_REVOKED) {
        set_error(s, "SSH host key is REVOKED - connection refused");
        return -1;
    }
    if (hk == HOST_KEY_ERROR) {
        set_error(s, "SSH host key verification failed");
        return -1;
    }

    char fp[128] = {0};
    known_hosts_fingerprint_sha256(s->ssh_session, fp, sizeof(fp));

    if (hk == HOST_KEY_CHANGED) {
        /* Host key has changed — present dialog to user.
         * The worker thread blocks on hostkey_cond until the UI responds. */
        pthread_mutex_lock(&s->hostkey_lock);
        s->hostkey_is_change = true;
        snprintf(s->hostkey_new_fp, sizeof(s->hostkey_new_fp), "%s", fp);
        s->hostkey_old_fp[0] = '\0'; /* We don't easily have the old fp */
        s->hostkey_pending = true;
        s->hostkey_accepted = false;

        /* Wait for UI thread to signal a decision */
        while (s->hostkey_pending) {
            pthread_cond_wait(&s->hostkey_cond, &s->hostkey_lock);
        }
        bool accepted = s->hostkey_accepted;
        pthread_mutex_unlock(&s->hostkey_lock);

        if (!accepted) {
            set_error(s, "SSH host key mismatch - rejected by user");
            return -1;
        }
        /* User accepted: remove old key and add new one */
        known_hosts_remove(s->ssh_session, s->config->hostname, s->config->port);
        if (!known_hosts_add(s->ssh_session, s->config->hostname, s->config->port)) {
            set_error(s, "Failed to persist new SSH host key");
            return -1;
        }
        return 0;
    }

    /* HOST_KEY_NEW — ask user to trust via GUI dialog */
    pthread_mutex_lock(&s->hostkey_lock);
    s->hostkey_is_change = false;
    snprintf(s->hostkey_new_fp, sizeof(s->hostkey_new_fp), "%s", fp);
    s->hostkey_old_fp[0] = '\0';
    s->hostkey_pending = true;
    s->hostkey_accepted = false;

    while (s->hostkey_pending) {
        pthread_cond_wait(&s->hostkey_cond, &s->hostkey_lock);
    }
    bool accepted = s->hostkey_accepted;
    pthread_mutex_unlock(&s->hostkey_lock);

    if (!accepted) {
        set_error(s, "Untrusted new SSH host key");
        return -1;
    }
    if (!known_hosts_add(s->ssh_session, s->config->hostname, s->config->port)) {
        set_error(s, "Failed to persist SSH host key");
        return -1;
    }
    return 0;
}

/* =========================================================================
 * Local PTY session
 * ========================================================================= */

#ifndef PLATFORM_WIN32
/* Shared child-side setup for local PTY spawns. Runs in the forked child
 * before exec: terminal identity env, caller-supplied env vars, PATH prepend,
 * and cwd selection (with the Finder-launch HOME safety net). Factored out so
 * both the interactive-shell spawn
 * (session_create_local_with_env) and the command spawn
 * (session_create_command) stay byte-for-byte identical here. */
static void local_child_setup(const LocalEnvVar *env, i32 env_count,
                              const char *cwd) {
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    setenv("TERM_PROGRAM", "Liu", 1);
    setenv("FORCE_COLOR", "3", 1);
    setenv("CLICOLOR", "1", 1);
    setenv("CLICOLOR_FORCE", "1", 1);
    /* npm/chalk respect these; ensure nothing upstream disabled color */
    unsetenv("NO_COLOR");
    setenv("npm_config_color", "always", 1);

    /* Smart Vault globals — applied AFTER the hardcoded defaults so
     * a user-defined TERM / PATH can override. setenv(..., 1)
     * overrides. */
    for (i32 i = 0; i < env_count && env; i++) {
        if (env[i].name[0])
            setenv(env[i].name, env[i].value, 1);
    }

    /* Prepend liu's own directory to PATH so sibling helpers — liu-history,
     * liu-notify, agenthistory — are callable by name from the local shell
     * without a full install. */
    const char *liu_dir = liu_executable_dir();
    if (liu_dir && liu_dir[0]) {
        const char *old_path = getenv("PATH");
        char new_path[4096];
        snprintf(new_path, sizeof(new_path), "%s:%s",
                 liu_dir,
                 old_path && *old_path ? old_path
                                       : "/usr/local/bin:/usr/bin:/bin");
        setenv("PATH", new_path, 1);
    }

    /* If the caller passed an explicit starting cwd (split inheriting the
     * active pane's directory, "open new shell here" workflows…) honor it
     * and keep $PWD consistent so the shell doesn't paper over it. PWD
     * must be the canonical absolute path the kernel sees as cwd —
     * passing the raw input string would leave PWD with relative
     * segments or symlinked prefixes (e.g. /var → /private/var on
     * macOS), which then disagrees with `pwd` / getcwd() and breaks
     * direnv, oh-my-zsh prompt hooks, some VCS plugins. A stale path
     * falls through to the Finder-launch safety net below. */
    bool chdir_done = false;
    if (cwd && cwd[0] && chdir(cwd) == 0) {
        char *resolved = realpath(".", NULL);
        setenv("PWD", resolved ? resolved : cwd, 1);
        free(resolved);
        chdir_done = true;
    }
    /* When liu is launched as a .app bundle (Finder/Dock/`open -a`), the
     * parent's cwd is "/" — that's a useless place to drop the user. If
     * the inherited cwd is root, fall back to $HOME. CLI-launched runs
     * keep whatever working dir the user chose. */
    if (!chdir_done) {
        char cur_cwd[1024];
        if (getcwd(cur_cwd, sizeof cur_cwd) && strcmp(cur_cwd, "/") == 0) {
            const char *home = getenv("HOME");
            if (!home || !*home) {
                struct passwd *pw = getpwuid(getuid());
                home = pw ? pw->pw_dir : NULL;
            }
            if (home && *home) (void)chdir(home);
        }
    }
}

/* Resolve the user's login shell: $SHELL, then the passwd entry, then sh. */
static const char *local_login_shell(void) {
    const char *shell = getenv("SHELL");
    if (shell && *shell) return shell;   /* environ strings survive getpwuid() */
    /* getpwuid() returns a pointer into a shared static passwd struct; the
     * getpwuid() call inside local_child_setup() would overwrite pw_shell
     * before execvp() runs. Snapshot it into a child-local static buffer so the
     * returned pointer stays valid through child setup. */
    static char pw_shell_buf[256];
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_shell && pw->pw_shell[0]) {
        snprintf(pw_shell_buf, sizeof pw_shell_buf, "%s", pw->pw_shell);
        return pw_shell_buf;
    }
    return "/bin/sh";
}
#endif /* !PLATFORM_WIN32 */

Session *session_create_local_with_env(i32 cols, i32 rows,
                                       const LocalEnvVar *env, i32 env_count,
                                       const char *cwd) {
    Session *s = calloc(1, sizeof(Session));
    if (!s) return NULL;
    s->type = SESSION_LOCAL;
    s->cols = cols;
    s->rows = rows;
    s->pty_master = -1;
    s->sock = -1;

    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)cols,
    };

    pid_t pid = forkpty(&s->pty_master, NULL, NULL, &ws);
    if (pid < 0) {
        set_error(s, "forkpty failed");
        return s;
    }

    if (pid == 0) {
        /* Child process — exec an interactive login shell. The parent passes
         * `env` via a copied pointer; after fork the child sees its own copy
         * of the parent's address space, so the same pointer is valid here. */
        const char *shell = local_login_shell();
        local_child_setup(env, env_count, cwd);
        char *argv[] = { (char *)shell, "-l", NULL };
        execvp(shell, argv);
        _exit(127);
    }

    /* Parent */
    s->child_pid = pid;
    net_set_nonblocking(s->pty_master);
    s->status = SESSION_CONNECTED;
    return s;
}

Session *session_create_command(i32 cols, i32 rows,
                                const LocalEnvVar *env, i32 env_count,
                                const char *cwd, const char *command) {
    Session *s = calloc(1, sizeof(Session));
    if (!s) return NULL;
    s->type = SESSION_LOCAL;
    s->cols = cols;
    s->rows = rows;
    s->pty_master = -1;
    s->sock = -1;

    if (!command || !command[0]) {
        set_error(s, "no command");
        return s;
    }
    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)cols,
    };

    pid_t pid = forkpty(&s->pty_master, NULL, NULL, &ws);
    if (pid < 0) {
        set_error(s, "forkpty failed");
        return s;
    }

    if (pid == 0) {
        /* Child — run `command` through the user's LOGIN shell:
         *     shell -l -c "<command>"
         * -l loads the user's profile so PATH has node/npm even when liu was
         * Finder/Dock-launched with a minimal GUI PATH. The command is passed
         * VERBATIM (NOT prefixed with `exec`) so normal shell syntax works —
         * env-prefixed (`PORT=3000 npm run dev`) and chained
         * (`npm run build && npm run preview`) commands behave correctly. A
         * leading `exec` would mis-parse the env assignment as argv[0] and would
         * drop everything after the first `&&`. forkpty() already called
         * setsid(), so the child shell leads its own process group (pgid == pid);
         * the manager killpg(child_pid, …) to tree-kill the shell + node + its
         * esbuild/vite workers together, and `sh -c` propagates the dev server's
         * exit code. */
        const char *shell = local_login_shell();
        local_child_setup(env, env_count, cwd);
        char *argv[] = { (char *)shell, "-l", "-c", (char *)command, NULL };
        execvp(shell, argv);
        _exit(127);
    }

    /* Parent */
    s->child_pid = pid;
    net_set_nonblocking(s->pty_master);
    s->status = SESSION_CONNECTED;
    return s;
}

Session *session_create_local(i32 cols, i32 rows) {
    return session_create_local_with_env(cols, rows, NULL, 0, NULL);
}

/* =========================================================================
 * SSH session
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * ssh_authenticate — extracted auth logic, used for both proxy hops and
 * the final destination.  `session` must be blocking.
 * Returns 0 on success, -1 on failure (sets s->error_msg).
 * ------------------------------------------------------------------------- */
static int ssh_authenticate(Session *s, LIBSSH2_SESSION *session,
                            const char *username, const char *password,
                            const char *key_path, const char *key_passphrase,
                            AuthMethod method) {
    int auth_rc = -1;
    switch (method) {
    case AUTH_PASSWORD:
        auth_rc = libssh2_userauth_password(session, username, password);
        if (auth_rc != 0 && password[0]) {
            auth_rc = libssh2_userauth_keyboard_interactive(
                session, username, &ssh_kbdint_callback);
        }
        break;
    case AUTH_PUBLICKEY: {
        char pub_path[520];
        snprintf(pub_path, sizeof(pub_path), "%s.pub", key_path);
        const char *pp = (key_passphrase && key_passphrase[0]) ? key_passphrase : NULL;
        auth_rc = libssh2_userauth_publickey_fromfile(
            session, username, pub_path, key_path, pp);
        break;
    }
    case AUTH_AGENT: {
        LIBSSH2_AGENT *agent = libssh2_agent_init(session);
        if (agent && libssh2_agent_connect(agent) == 0) {
            libssh2_agent_list_identities(agent);
            struct libssh2_agent_publickey *identity = NULL;
            while (libssh2_agent_get_identity(agent, &identity, identity) == 0) {
                if (libssh2_agent_userauth(agent, username, identity) == 0) {
                    auth_rc = 0;
                    break;
                }
            }
            libssh2_agent_disconnect(agent);
        }
        if (agent) libssh2_agent_free(agent);
        break;
    }
    case AUTH_GSSAPI:
        /* Handled by the gssapi_auth path in ssh_do_connect; shouldn't reach here */
        break;
    }
    if (auth_rc != 0) {
        char *errmsg;
        libssh2_session_last_error(session, &errmsg, NULL, 0);
        set_error(s, errmsg);
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Channel relay thread — bidirectionally copies data between a local
 * socketpair fd and a libssh2 direct-tcpip channel.  Used for ProxyJump
 * so the next hop's libssh2 can treat the socketpair fd as a "raw TCP"
 * socket.
 * ------------------------------------------------------------------------- */
static void *channel_relay_thread(void *arg) {
    ChannelRelay *r = (ChannelRelay *)arg;
    u8 buf[16384];
    fd_set fds;
    struct timeval tv;

    libssh2_session_set_blocking(r->session, 0);

    while (r->running) {
        bool did_work = false;

        /* channel -> local_fd */
        for (;;) {
            ssize_t n = libssh2_channel_read(r->channel, (char *)buf, sizeof(buf));
            if (n > 0) {
                ssize_t written = 0;
                while (written < n) {
                    ssize_t w = write(r->local_fd, buf + written, (usize)(n - written));
                    if (w <= 0) break;
                    written += w;
                }
                did_work = true;
            } else {
                if (n == 0 || (n != LIBSSH2_ERROR_EAGAIN)) {
                    if (n == 0 || libssh2_channel_eof(r->channel)) {
                        r->running = false;
                    }
                }
                break;
            }
        }

        /* local_fd -> channel */
        FD_ZERO(&fds);
        FD_SET(r->local_fd, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (select(r->local_fd + 1, &fds, NULL, NULL, &tv) > 0) {
            ssize_t n = read(r->local_fd, buf, sizeof(buf));
            if (n > 0) {
                ssize_t written = 0;
                /* Temporarily set blocking for reliable write */
                libssh2_session_set_blocking(r->session, 1);
                while (written < n) {
                    ssize_t w = libssh2_channel_write(r->channel, (const char *)buf + written,
                                                       (usize)(n - written));
                    if (w < 0) break;
                    written += w;
                }
                libssh2_session_set_blocking(r->session, 0);
                did_work = true;
            } else if (n == 0) {
                r->running = false;
            }
        }

        if (!did_work && r->running) {
            usleep(1000); /* 1ms idle sleep to avoid busy-wait */
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Proxy hop cleanup helper
 * ------------------------------------------------------------------------- */
static void proxy_hops_cleanup(Session *s) {
    for (i32 i = 0; i < s->proxy_hop_count; i++) {
        /* Stop relay thread */
        if (s->proxy_relays[i]) {
            s->proxy_relays[i]->running = false;
            pthread_join(s->proxy_relays[i]->thread, NULL);
            close(s->proxy_relays[i]->local_fd);
            free(s->proxy_relays[i]);
            s->proxy_relays[i] = NULL;
        }
        ProxyHopState *h = &s->proxy_hops[i];
        if (h->tunnel) {
            libssh2_channel_free(h->tunnel);
            h->tunnel = NULL;
        }
        if (h->session) {
            libssh2_session_disconnect(h->session, "proxy cleanup");
            libssh2_session_free(h->session);
            h->session = NULL;
        }
        if (h->sock >= 0) {
            close(h->sock);
            h->sock = -1;
        }
    }
    s->proxy_hop_count = 0;
}

/* -------------------------------------------------------------------------
 * Remote forwarding cleanup helper
 * ------------------------------------------------------------------------- */
static void remote_forward_cleanup_all(Session *s) {
    /* Stop all accepted connection relay threads */
    if (s->remote_fwd_conns) {
        for (i32 i = 0; i < s->remote_fwd_conn_count; i++) {
            RemoteForwardConn *c = &s->remote_fwd_conns[i];
            if (c->running) {
                c->running = false;
                if (c->dest_sock >= 0) {
                    shutdown(c->dest_sock, SHUT_RDWR);
                    close(c->dest_sock);
                    c->dest_sock = -1;
                }
                pthread_join(c->thread, NULL);
            }
            if (c->channel) {
                libssh2_channel_free(c->channel);
                c->channel = NULL;
            }
        }
    }
    s->remote_fwd_conn_count = 0;

    /* Cancel listeners */
    if (s->remote_listeners) {
        for (i32 i = 0; i < s->remote_listener_count; i++) {
            RemoteForwardListener *l = &s->remote_listeners[i];
            if (l->listener) {
                libssh2_channel_forward_cancel(l->listener);
                l->listener = NULL;
            }
            l->active = false;
        }
    }
    s->remote_listener_count = 0;
}

/* Body of the blocking connect path — shared by initial connect and
 * session_reconnect().  Must run on a thread that has exclusive access to
 * `s->ssh_session` / `s->ssh_channel` / `s->sock`.  Returns 0 on success
 * or sets s->status to SESSION_ERROR and returns -1. */
static int ssh_do_connect(Session *s) {
    const SSHConfig *config = s->config;

    #define CANCELLED() atomic_load_explicit(&s->connect_cancel, memory_order_acquire)
    #define FAIL(msg) do { set_error(s, msg); return -1; } while (0)

    if (CANCELLED()) FAIL("cancelled");

    /* ==================================================================
     * ProxyJump / multi-hop chain
     * ==================================================================
     * If config->proxy_chain_len > 0, we build a chain:
     *   TCP -> hop0 -> direct-tcpip -> hop1 -> ... -> final destination
     * Each hop gets its own libssh2 session.  Between hops we use a
     * socketpair + relay thread so the next hop's libssh2 can do a normal
     * handshake over what looks like a plain fd.
     * ================================================================== */

    int final_sock = -1;  /* fd the final session will use */

    if (config->proxy_chain_len > 0) {
        int prev_sock = -1; (void)prev_sock;
        LIBSSH2_SESSION *prev_session = NULL;

        for (i32 hop = 0; hop < config->proxy_chain_len; hop++) {
            if (CANCELLED()) FAIL("cancelled during proxy chain");

            const ProxyHop *ph = &config->proxy_chain[hop];
            ProxyHopState *hs = &s->proxy_hops[hop];
            memset(hs, 0, sizeof(*hs));
            hs->sock = -1;

            if (hop == 0) {
                /* First hop: plain TCP */
                hs->sock = net_tcp_connect(ph->hostname, ph->port);
                if (hs->sock < 0) FAIL("TCP connection to proxy hop failed");
            } else {
                /* Subsequent hops: open direct-tcpip on previous session,
                 * create socketpair, spawn relay thread */
                libssh2_session_set_blocking(prev_session, 1);
                LIBSSH2_CHANNEL *tunnel = libssh2_channel_direct_tcpip(
                    prev_session, ph->hostname, ph->port);
                if (!tunnel) FAIL("Failed to open tunnel to next proxy hop");

                s->proxy_hops[hop - 1].tunnel = tunnel;

                int sv[2];
                if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
                    FAIL("socketpair failed for proxy relay");

                ChannelRelay *relay = calloc(1, sizeof(ChannelRelay));
                if (!relay) { close(sv[0]); close(sv[1]); FAIL("OOM"); }
                relay->local_fd = sv[0];
                relay->channel = tunnel;
                relay->session = prev_session;
                relay->running = true;
                s->proxy_relays[hop - 1] = relay;

                if (pthread_create(&relay->thread, NULL, channel_relay_thread, relay) != 0) {
                    close(sv[0]); close(sv[1]); free(relay);
                    s->proxy_relays[hop - 1] = NULL;
                    FAIL("Failed to spawn proxy relay thread");
                }

                hs->sock = sv[1]; /* the "remote" end looks like a TCP socket */
            }

            /* Count this hop as soon as its socket exists — BEFORE the handshake
             * — so a session_init / handshake / auth failure leaves the in-flight
             * session + socket covered by proxy_hops_cleanup (run by the teardown
             * and reconnect paths). Those paths pthread_join this worker first
             * (ssh_session.c:1964), so there is no concurrent free. Without this,
             * every failed proxy hop leaked a libssh2 session + a socket fd, and
             * a flaky-proxy auto-reconnect loop exhausted the fd table. */
            s->proxy_hop_count = hop + 1;

            /* SSH handshake on this hop */
            hs->session = libssh2_session_init();
            if (!hs->session) FAIL("libssh2_session_init failed for proxy hop");
            *libssh2_session_abstract(hs->session) = s;
            libssh2_session_set_blocking(hs->session, 1);

            if (libssh2_session_handshake(hs->session, hs->sock) != 0) {
                char *errmsg;
                libssh2_session_last_error(hs->session, &errmsg, NULL, 0);
                set_error(s, errmsg);
                return -1;
            }

            /* Authenticate proxy hop */
            if (ssh_authenticate(s, hs->session, ph->username, "",
                                 ph->key_path, "", ph->auth_method) != 0) {
                return -1;
            }

            prev_sock = hs->sock;
            prev_session = hs->session;
            /* proxy_hop_count already bumped above, before the handshake. */
        }

        /* Open direct-tcpip from last proxy hop to the final destination */
        libssh2_session_set_blocking(prev_session, 1);
        LIBSSH2_CHANNEL *final_tunnel = libssh2_channel_direct_tcpip(
            prev_session, config->hostname, config->port);
        if (!final_tunnel) FAIL("Failed to open tunnel to final destination");

        s->proxy_hops[s->proxy_hop_count - 1].tunnel = final_tunnel;

        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
            FAIL("socketpair failed for final tunnel relay");

        ChannelRelay *relay = calloc(1, sizeof(ChannelRelay));
        if (!relay) { close(sv[0]); close(sv[1]); FAIL("OOM"); }
        relay->local_fd = sv[0];
        relay->channel = final_tunnel;
        relay->session = prev_session;
        relay->running = true;
        s->proxy_relays[s->proxy_hop_count - 1] = relay;

        if (pthread_create(&relay->thread, NULL, channel_relay_thread, relay) != 0) {
            close(sv[0]); close(sv[1]); free(relay);
            s->proxy_relays[s->proxy_hop_count - 1] = NULL;
            FAIL("Failed to spawn final tunnel relay thread");
        }

        final_sock = sv[1];
    } else {
        /* No proxy — plain TCP to destination */
        final_sock = net_tcp_connect(config->hostname, config->port);
        if (final_sock < 0) FAIL("TCP connection failed");
    }

    if (CANCELLED()) {
        close(final_sock);
        FAIL("cancelled");
    }

    s->sock = final_sock;

    /* ==================================================================
     * SSH handshake + auth on the final destination
     * ================================================================== */

    s->ssh_session = libssh2_session_init();
    if (!s->ssh_session) {
        close(s->sock); s->sock = -1;
        FAIL("libssh2_session_init failed");
    }
    *libssh2_session_abstract(s->ssh_session) = s;
    libssh2_session_set_blocking(s->ssh_session, 1);

    if (libssh2_session_handshake(s->ssh_session, s->sock) != 0) {
        char *errmsg;
        libssh2_session_last_error(s->ssh_session, &errmsg, NULL, 0);
        set_error(s, errmsg);
        return -1;
    }
    if (ssh_verify_hostkey(s) != 0) {
        return -1;
    }
    if (CANCELLED()) FAIL("cancelled");

    pthread_mutex_lock(&s->status_lock);
    s->status = SESSION_AUTHENTICATING;
    pthread_mutex_unlock(&s->status_lock);

    /* Check which auth methods the server supports */
    char *auth_list = libssh2_userauth_list(s->ssh_session, config->username,
                                             (unsigned int)strlen(config->username));

    int auth_rc = -1;

    /* GSSAPI: when gssapi_auth flag is set OR auth_method is AUTH_GSSAPI,
     * try methods in order: agent -> keyboard-interactive (GSSAPI fallback)
     * -> publickey -> password.  Many Kerberos-enabled servers accept
     * keyboard-interactive or even empty-password auth when a valid ticket
     * exists in the user's credential cache. */
    if (config->auth_method == AUTH_GSSAPI || config->gssapi_auth) {
        bool server_has_kbdint = auth_list && strstr(auth_list, "keyboard-interactive");
        bool server_has_passwd = auth_list && strstr(auth_list, "password");
        bool server_has_pubkey = auth_list && strstr(auth_list, "publickey");

        /* 1) Try SSH agent first (tickets may be forwarded via agent) */
        if (auth_rc != 0) {
            LIBSSH2_AGENT *agent = libssh2_agent_init(s->ssh_session);
            if (agent && libssh2_agent_connect(agent) == 0) {
                libssh2_agent_list_identities(agent);
                struct libssh2_agent_publickey *identity = NULL;
                while (libssh2_agent_get_identity(agent, &identity, identity) == 0) {
                    if (libssh2_agent_userauth(agent, config->username, identity) == 0) {
                        auth_rc = 0;
                        break;
                    }
                }
                libssh2_agent_disconnect(agent);
            }
            if (agent) libssh2_agent_free(agent);
        }

        /* 2) keyboard-interactive — Kerberos servers commonly use this */
        if (auth_rc != 0 && server_has_kbdint) {
            auth_rc = libssh2_userauth_keyboard_interactive(
                s->ssh_session, config->username, &ssh_kbdint_callback);
        }

        /* 3) Try password with empty string (some GSSAPI setups accept this) */
        if (auth_rc != 0 && server_has_passwd) {
            auth_rc = libssh2_userauth_password(s->ssh_session, config->username, "");
        }

        /* 4) Public key if available — prefer in-memory PEM from the
         *    vault when present; fall back to on-disk key file. */
        if (auth_rc != 0 && server_has_pubkey && config->key_pem) {
            const char *pp = config->key_passphrase[0] ? config->key_passphrase : NULL;
            auth_rc = libssh2_userauth_publickey_frommemory(
                s->ssh_session,
                config->username, strlen(config->username),
                NULL, 0,  /* derive public key from private */
                (const char *)config->key_pem, config->key_pem_len,
                pp);
        }
        if (auth_rc != 0 && server_has_pubkey && config->key_path[0]) {
            char pub_path[520];
            snprintf(pub_path, sizeof(pub_path), "%s.pub", config->key_path);
            const char *pp = config->key_passphrase[0] ? config->key_passphrase : NULL;
            auth_rc = libssh2_userauth_publickey_fromfile(
                s->ssh_session, config->username, pub_path, config->key_path, pp);
        }

        /* 5) Password as last resort */
        if (auth_rc != 0 && server_has_passwd && config->password[0]) {
            auth_rc = libssh2_userauth_password(s->ssh_session, config->username,
                                                 config->password);
        }
    } else {
        /* Standard auth paths */
        switch (config->auth_method) {
        case AUTH_PASSWORD:
            /* Attempt password auth if a password is set; then always fall
             * back to keyboard-interactive on failure. The fallback is
             * unconditional so that a Recent-row reconnect with an empty
             * password field still gets a KBI prompt callback fired, which
             * the UI layer maps to the kbi_dialog overlay. */
            if (config->password[0]) {
                auth_rc = libssh2_userauth_password(s->ssh_session,
                                                    config->username,
                                                    config->password);
            } else {
                auth_rc = -1;  /* force KBI path */
            }
            if (auth_rc != 0) {
                auth_rc = libssh2_userauth_keyboard_interactive(
                    s->ssh_session, config->username, &ssh_kbdint_callback);
            }
            break;
        case AUTH_PUBLICKEY: {
            char pub_path[520];
            snprintf(pub_path, sizeof(pub_path), "%s.pub", config->key_path);
            const char *passphrase = config->key_passphrase[0] ? config->key_passphrase : NULL;
            /* Prefer the in-memory PEM when a Smart Vault-stored key is
             * attached: avoids touching the filesystem with decrypted
             * material. Falls back to the on-disk path when no vault key
             * is available. */
            if (config->key_pem) {
                auth_rc = libssh2_userauth_publickey_frommemory(
                    s->ssh_session,
                    config->username, strlen(config->username),
                    NULL, 0,
                    (const char *)config->key_pem, config->key_pem_len,
                    passphrase);
            } else {
                auth_rc = libssh2_userauth_publickey_fromfile(
                    s->ssh_session, config->username, pub_path, config->key_path, passphrase);
            }

            /* Detect passphrase-needed error and prompt the UI */
            if (auth_rc != 0 && !passphrase) {
                char *errmsg = NULL;
                libssh2_session_last_error(s->ssh_session, &errmsg, NULL, 0);
                bool needs_passphrase = false;
                if (errmsg) {
                    needs_passphrase = (strstr(errmsg, "passphrase") != NULL ||
                                        strstr(errmsg, "decrypt") != NULL ||
                                        strstr(errmsg, "Unable to extract") != NULL ||
                                        strstr(errmsg, "Failed to parse") != NULL);
                }
                if (needs_passphrase) {
                    /* Signal the UI to show a passphrase prompt */
                    pthread_mutex_lock(&s->status_lock);
                    s->passphrase_pending = true;
                    s->passphrase_supplied = false;
                    s->passphrase_cancelled = false;
                    snprintf(s->passphrase_prompt_path, sizeof(s->passphrase_prompt_path),
                             "%s", config->key_path);
                    pthread_mutex_unlock(&s->status_lock);

                    /* Block until UI supplies passphrase or cancels */
                    pthread_mutex_lock(&s->status_lock);
                    while (!s->passphrase_supplied && !s->passphrase_cancelled &&
                           !CANCELLED()) {
                        pthread_cond_wait(&s->passphrase_cond, &s->status_lock);
                    }
                    s->passphrase_pending = false;
                    bool cancelled = s->passphrase_cancelled || CANCELLED();
                    pthread_mutex_unlock(&s->status_lock);

                    if (cancelled) FAIL("Authentication cancelled");

                    /* Retry with the supplied passphrase */
                    const char *pp = s->config->key_passphrase[0]
                                     ? s->config->key_passphrase : NULL;
                    if (config->key_pem) {
                        auth_rc = libssh2_userauth_publickey_frommemory(
                            s->ssh_session,
                            config->username, strlen(config->username),
                            NULL, 0,
                            (const char *)config->key_pem,
                            config->key_pem_len, pp);
                    } else {
                        auth_rc = libssh2_userauth_publickey_fromfile(
                            s->ssh_session, config->username, pub_path,
                            config->key_path, pp);
                    }
                }
            }
            break;
        }
        case AUTH_AGENT: {
            LIBSSH2_AGENT *agent = libssh2_agent_init(s->ssh_session);
            if (agent && libssh2_agent_connect(agent) == 0) {
                libssh2_agent_list_identities(agent);
                struct libssh2_agent_publickey *identity = NULL;
                while (libssh2_agent_get_identity(agent, &identity, identity) == 0) {
                    if (libssh2_agent_userauth(agent, config->username, identity) == 0) {
                        auth_rc = 0;
                        break;
                    }
                }
                libssh2_agent_disconnect(agent);
            }
            if (agent) libssh2_agent_free(agent);

            /* If agent auth failed, show a helpful error about ssh-add */
            if (auth_rc != 0 && config->key_path[0]) {
                char agent_msg[512];
                snprintf(agent_msg, sizeof(agent_msg),
                         "Agent auth failed. Try: ssh-add %s", config->key_path);
                set_error(s, agent_msg);
                return -1;
            }
            break;
        }
        case AUTH_GSSAPI:
            /* Handled by the gssapi_auth path above; shouldn't reach here */
            break;
        }
    }
    if (auth_rc != 0) {
        char *errmsg;
        libssh2_session_last_error(s->ssh_session, &errmsg, NULL, 0);
        set_error(s, errmsg);
        return -1;
    }
    if (CANCELLED()) FAIL("cancelled");

    /* ==================================================================
     * Open channel, PTY, shell
     * ================================================================== */

    s->ssh_channel = libssh2_channel_open_session(s->ssh_session);
    if (!s->ssh_channel) FAIL("Failed to open SSH channel");

    if (libssh2_channel_request_pty_ex(s->ssh_channel, "xterm-256color", 14,
                                        NULL, 0, s->cols, s->rows, 0, 0) != 0)
        FAIL("PTY request failed");

    /* Agent forwarding — non-fatal on failure */
    if (config->local_forward_count > 0) {
        /* Agent forwarding is requested via a flag; here we use a simple
         * heuristic: if the first local forward has bind_port == 0, treat
         * it as agent forwarding.  But the real flag comes from config. */
    }
    /* Request agent forwarding if the auth method is agent-based.
     * This enables the remote host to use the local SSH agent. */
    if (config->auth_method == AUTH_AGENT) {
        /* libssh2_channel_request_auth_agent is available in newer libssh2.
         * Non-fatal if it fails or is not available. */
#ifdef LIBSSH2_CHANNEL_FORWARD_AGENT
        libssh2_channel_request_auth_agent(s->ssh_channel);
#endif
    }

    /* Request X11 forwarding if configured (after PTY, before shell) */
    if (config->forward_x11) {
        /* Register the X11 open callback. libssh2_session_callback_set2 +
         * libssh2_cb_generic landed in libssh2 1.11.1; older libssh2 (e.g.
         * Ubuntu 24.04's 1.11.0) only has the void*-typed callback_set. */
#if defined(LIBSSH2_VERSION_NUM) && LIBSSH2_VERSION_NUM >= 0x010b01
        libssh2_session_callback_set2(s->ssh_session, LIBSSH2_CALLBACK_X11,
                                       (libssh2_cb_generic *)x11_open_callback);
#else
        libssh2_session_callback_set(s->ssh_session, LIBSSH2_CALLBACK_X11,
                                     (void *)x11_open_callback);
#endif

        /* Generate a random MIT-MAGIC-COOKIE-1 auth cookie (32 hex chars) from
         * a CSPRNG. rand() is unseeded and predictable here, which would let a
         * local attacker forge the cookie and connect to the forwarded X11
         * channel. */
        u8 cookie_rnd[16];
        if (!crypto_random(cookie_rnd, sizeof(cookie_rnd))) {
            fprintf(stderr, "X11 forwarding skipped: secure RNG unavailable\n");
        } else {
            char cookie[33];
            static const char hexd[] = "0123456789abcdef";
            for (int ci = 0; ci < 16; ci++) {
                cookie[ci * 2]     = hexd[(cookie_rnd[ci] >> 4) & 0xf];
                cookie[ci * 2 + 1] = hexd[cookie_rnd[ci] & 0xf];
            }
            cookie[32] = '\0';

            int x11_rc = libssh2_channel_x11_req_ex(s->ssh_channel,
                                                      0,      /* single_connection */
                                                      "MIT-MAGIC-COOKIE-1",
                                                      cookie,
                                                      0);     /* screen_number */
            if (x11_rc != 0) {
                /* X11 forwarding is best-effort -- log but don't fail the session */
                fprintf(stderr, "X11 forwarding request failed (rc=%d)\n", x11_rc);
            }
        }
    }

    /* Smart Vault — inject environment variables before starting the
     * shell. The server must have `AcceptEnv` configured for the var
     * name (or `AcceptEnv *`) or libssh2_channel_setenv returns an
     * error. We treat that as non-fatal: missing env is a misconfig,
     * not a reason to refuse the session. */
    for (i32 ei = 0; config->env && ei < config->env_count; ei++) {
        const SSHEnvVar *ev = &config->env[ei];
        if (!ev->name[0]) continue;
        int erc = libssh2_channel_setenv(s->ssh_channel, ev->name, ev->value);
        if (erc != 0) {
            fprintf(stderr,
                    "vault env: server rejected %s (rc=%d); "
                    "check AcceptEnv in sshd_config\n",
                    ev->name, erc);
        }
    }

    if (libssh2_channel_shell(s->ssh_channel) != 0)
        FAIL("Shell request failed");

    /* ==================================================================
     * Keepalive configuration
     * ================================================================== */
    if (s->keepalive_interval > 0) {
        libssh2_keepalive_config(s->ssh_session, 1, (unsigned int)s->keepalive_interval);
    }
    s->last_keepalive_time = 0;
    s->keepalive_missed = 0;

    /* ==================================================================
     * Remote port forwarding setup from config
     * ================================================================== */
    for (i32 i = 0; i < config->remote_forward_count; i++) {
        const PortForwardSpec *spec = &config->remote_forwards[i];
        int bound_port = 0;
        LIBSSH2_LISTENER *listener = libssh2_channel_forward_listen_ex(
            s->ssh_session,
            spec->bind_host[0] ? spec->bind_host : NULL,
            spec->bind_port, &bound_port, 1);
        if (listener && s->remote_listener_count < MAX_REMOTE_LISTENERS) {
            if (!s->remote_listeners) {
                s->remote_listeners = calloc(MAX_REMOTE_LISTENERS,
                                             sizeof(*s->remote_listeners));
                if (!s->remote_listeners) continue;
            }
            RemoteForwardListener *l = &s->remote_listeners[s->remote_listener_count++];
            l->listener = listener;
            snprintf(l->bind_host, sizeof(l->bind_host), "%s",
                     spec->bind_host[0] ? spec->bind_host : "0.0.0.0");
            l->bind_port = bound_port;
            snprintf(l->dest_host, sizeof(l->dest_host), "%s", spec->dest_host);
            l->dest_port = spec->dest_port;
            l->active = true;
        }
    }

    /* Pre-init SFTP + resolve remote home while still in blocking mode.
     * Doing this here costs one SFTP handshake + one realpath round-trip
     * on the worker thread, which is already hidden behind the "connecting"
     * UI state. In return, the main thread never has to flip libssh2 to
     * blocking mode to get an SFTP handle — removing a stall that the old
     * lazy session_get_sftp used to cause the first time the sidebar was
     * opened on an SSH tab. SFTP being unavailable is non-fatal: the shell
     * session works regardless. */
    if (!CANCELLED()) {
        s->sftp_init_tried = true;
        LIBSSH2_SFTP *sftp = libssh2_sftp_init(s->ssh_session);
        if (sftp) {
            s->sftp_session = sftp;
            char rp[1024];
            int n = libssh2_sftp_realpath(sftp, ".", rp, sizeof(rp) - 1);
            if (n > 0) {
                rp[n] = '\0';
                free(s->initial_cwd);
                s->initial_cwd = malloc((usize)n + 1);
                if (s->initial_cwd) memcpy(s->initial_cwd, rp, (usize)n + 1);
            }
        }
    }

    /* Switch to non-blocking — main thread owns libssh2 from here on. */
    libssh2_session_set_blocking(s->ssh_session, 0);

    pthread_mutex_lock(&s->status_lock);
    s->status = SESSION_CONNECTED;
    pthread_mutex_unlock(&s->status_lock);
    return 0;

    #undef CANCELLED
    #undef FAIL
}

static void *ssh_connect_worker(void *arg) {
    Session *s = (Session *)arg;
    ssh_do_connect(s);
    return NULL;
}

/* Open a new channel + PTY + shell on an existing LIBSSH2_SESSION.
 * Used by ControlMaster session reuse. Runs on a worker thread. */
static int ssh_do_reuse_connect(Session *s) {
    #define FAIL_REUSE(msg) do { set_error(s, msg); return -1; } while (0)

    if (!s->shared_session || !s->shared_session->ssh_session)
        FAIL_REUSE("Shared session is not available");

    /* Borrow the libssh2 session from the owner.  The owner's session must
     * be in blocking mode for channel-open to succeed. */
    LIBSSH2_SESSION *ssh = s->shared_session->ssh_session;
    libssh2_session_set_blocking(ssh, 1);

    s->ssh_session = ssh;
    s->sock = s->shared_session->sock;

    s->ssh_channel = libssh2_channel_open_session(ssh);
    if (!s->ssh_channel) {
        libssh2_session_set_blocking(ssh, 0);
        s->ssh_session = NULL;
        s->sock = -1;
        FAIL_REUSE("Failed to open channel on shared session");
    }

    if (libssh2_channel_request_pty_ex(s->ssh_channel, "xterm-256color", 14,
                                        NULL, 0, s->cols, s->rows, 0, 0) != 0) {
        libssh2_channel_free(s->ssh_channel);
        s->ssh_channel = NULL;
        libssh2_session_set_blocking(ssh, 0);
        s->ssh_session = NULL;
        s->sock = -1;
        FAIL_REUSE("PTY request failed on shared session");
    }

    if (libssh2_channel_shell(s->ssh_channel) != 0) {
        libssh2_channel_free(s->ssh_channel);
        s->ssh_channel = NULL;
        libssh2_session_set_blocking(ssh, 0);
        s->ssh_session = NULL;
        s->sock = -1;
        FAIL_REUSE("Shell request failed on shared session");
    }

    /* Pre-init SFTP on the shared libssh2 session (same treatment as the
     * non-shared path — see ssh_do_connect). libssh2 allows multiple SFTP
     * sub-channels per session, so this reused tab gets its own handle
     * that session_destroy can shut down independently. */
    s->sftp_init_tried = true;
    LIBSSH2_SFTP *sftp = libssh2_sftp_init(ssh);
    if (sftp) {
        s->sftp_session = sftp;
        char rp[1024];
        int n = libssh2_sftp_realpath(sftp, ".", rp, sizeof(rp) - 1);
        if (n > 0) {
            rp[n] = '\0';
            free(s->initial_cwd);
            s->initial_cwd = malloc((usize)n + 1);
            if (s->initial_cwd) memcpy(s->initial_cwd, rp, (usize)n + 1);
        }
    }

    libssh2_session_set_blocking(ssh, 0);

    pthread_mutex_lock(&s->status_lock);
    s->status = SESSION_CONNECTED;
    pthread_mutex_unlock(&s->status_lock);
    return 0;

    #undef FAIL_REUSE
}

static void *ssh_reuse_connect_worker(void *arg) {
    Session *s = (Session *)arg;
    ssh_do_reuse_connect(s);
    return NULL;
}

Session *session_create_ssh(const SSHConfig *config, i32 cols, i32 rows) {
    session_pool_init_once();
    Session *s = calloc(1, sizeof(Session));
    if (!s) return NULL;
    s->type = SESSION_SSH;
    s->cols = cols;
    s->rows = rows;
    s->pty_master = -1;
    s->sock = -1;
    s->is_shared_owner = true; /* default: we own our own connection */
    for (i32 i = 0; i < 4; i++) s->stream_fwd_fds[i] = -1;
    s->config = calloc(1, sizeof(SSHConfig));
    if (!s->config) { free(s); return NULL; }
    if (!ssh_config_clone(s->config, config)) {
        free(s->config); free(s); return NULL;
    }
    s->status = SESSION_CONNECTING;
    pthread_mutex_init(&s->status_lock, NULL);
    pthread_cond_init(&s->passphrase_cond, NULL);
    pthread_mutex_init(&s->hostkey_lock, NULL);
    pthread_cond_init(&s->hostkey_cond, NULL);
    s->hostkey_pending = false;
    s->hostkey_accepted = false;
    pthread_mutex_init(&s->kbi_lock, NULL);
    pthread_cond_init(&s->kbi_cond, NULL);
    s->kbi_ready = false;
    s->kbi_num_prompts = 0;
    s->proxy_hop_count = 0;
    s->keepalive_interval = 30;   /* default: 30 seconds */
    s->keepalive_count_max = 3;   /* disconnect after 3 missed */
    s->keepalive_missed = 0;
    s->last_keepalive_time = 0;
    s->remote_listener_count = 0;
    s->remote_fwd_conn_count = 0;
    atomic_init(&s->connect_cancel, false);

    /* ControlMaster: try to reuse an existing connected session to the same
     * host:port+user instead of opening a new TCP connection. */
    if (config->control_master) {
        pthread_mutex_lock(&g_session_pool.lock);
        Session *owner = session_pool_find_locked(config->hostname, config->port,
                                                   config->username);
        pthread_mutex_unlock(&g_session_pool.lock);

        if (owner) {
            s->shared_session = owner;
            s->is_shared_owner = false;

            if (pthread_create(&s->connect_thread, NULL,
                               ssh_reuse_connect_worker, s) != 0) {
                set_error(s, "Failed to spawn SSH reuse thread");
                return s;
            }
            s->connect_thread_active = true;
            return s;
        }
        /* No existing session — fall through to normal connect */
    }

    /* spawn the blocking connect on a dedicated thread so the UI
     * stays responsive during TCP connect, handshake, and authentication. */
    if (pthread_create(&s->connect_thread, NULL, ssh_connect_worker, s) != 0) {
        set_error(s, "Failed to spawn SSH connect thread");
        return s;
    }
    s->connect_thread_active = true;
    return s;
}

/* =========================================================================
 * Mosh session (wraps mosh-client via local PTY)
 * ========================================================================= */

Session *session_create_mosh(const SSHConfig *config, i32 cols, i32 rows) {
    if (!mosh_available()) return NULL;

    Session *s = calloc(1, sizeof(Session));
    if (!s) return NULL;
    s->type = SESSION_MOSH;
    s->cols = cols;
    s->rows = rows;
    s->pty_master = -1;
    s->sock = -1;
    s->config = calloc(1, sizeof(SSHConfig));
    if (!s->config) { free(s); return NULL; }
    if (!ssh_config_clone(s->config, config)) {
        free(s->config); free(s); return NULL;
    }
    pthread_mutex_init(&s->status_lock, NULL);
    atomic_init(&s->connect_cancel, false);

    /* mosh-client is spawned as a child process with a local PTY.
     * The mosh binary handles the SSH bootstrap (key exchange via
     * mosh-server) and the UDP session internally. */
    s->mosh = mosh_create(config->hostname, config->username,
                           config->port, cols, rows);
    if (!s->mosh) {
        set_error(s, "Failed to start mosh-client");
        return s;
    }

    s->status = SESSION_CONNECTED;
    return s;
}

/* =========================================================================
 * Telnet / Serial sessions (single-threaded socket / device fd)
 * ========================================================================= */

Session *session_create_telnet(const char *host, i32 port, i32 cols, i32 rows) {
    if (!host || !host[0]) return NULL;
    Session *s = calloc(1, sizeof(Session));
    if (!s) return NULL;
    s->type = SESSION_TELNET;
    s->cols = cols;
    s->rows = rows;
    s->pty_master = -1;
    s->sock = -1;
    s->telnet = telnet_create(host, port > 0 ? port : 23, cols, rows);
    if (!s->telnet) {
        s->status = SESSION_ERROR;
        snprintf(s->error_msg, sizeof(s->error_msg),
                 "Telnet: could not connect to %s:%d", host, port > 0 ? port : 23);
        return s;
    }
    s->status = SESSION_CONNECTED;
    return s;
}

Session *session_create_serial(const SerialConfig *cfg, i32 cols, i32 rows) {
    if (!cfg || !cfg->port[0]) return NULL;
    Session *s = calloc(1, sizeof(Session));
    if (!s) return NULL;
    s->type = SESSION_SERIAL;
    s->cols = cols;
    s->rows = rows;
    s->pty_master = -1;
    s->sock = -1;
    s->serial = serial_create(cfg);
    if (!s->serial) {
        s->status = SESSION_ERROR;
        snprintf(s->error_msg, sizeof(s->error_msg),
                 "Serial: could not open %s @ %u", cfg->port, cfg->baud_rate);
        return s;
    }
    s->status = SESSION_CONNECTED;
    return s;
}

/* =========================================================================
 * Destroy
 * ========================================================================= */

void session_destroy(Session *s) {
    if (!s) return;

    if (s->type == SESSION_LOCAL) {
        if (s->pty_master >= 0) {
            platform_unwatch_socket(s->pty_master);
            close(s->pty_master);
        }
        if (s->child_pid > 0) {
            kill(s->child_pid, SIGHUP);
            waitpid(s->child_pid, NULL, WNOHANG);
        }
    } else if (s->type == SESSION_MOSH) {
        mosh_destroy(s->mosh);
        s->mosh = NULL;
        pthread_mutex_destroy(&s->status_lock);
    } else if (s->type == SESSION_TELNET) {
        telnet_destroy(s->telnet);
        s->telnet = NULL;
    } else if (s->type == SESSION_SERIAL) {
        serial_destroy(s->serial);
        s->serial = NULL;
    } else {
        /* Unregister from session pool before teardown */
        session_pool_unregister(s);

        /* Stop StreamLocal forwarding threads */
        session_stop_stream_forwards(s);

        /* Close X11 forwarding relays */
        if (s->x11_relays) {
            for (i32 i = 0; i < s->x11_relay_count; i++) {
                if (s->x11_relays[i].active) {
                    if (s->x11_relays[i].channel) {
                        libssh2_channel_close(s->x11_relays[i].channel);
                        libssh2_channel_free(s->x11_relays[i].channel);
                    }
                    if (s->x11_relays[i].local_fd >= 0) close(s->x11_relays[i].local_fd);
                }
            }
        }
        /* shut down the connect worker before tearing libssh2 down.
         * Setting cancel alone can't abort a blocking tcp_connect/handshake,
         * so we also close the socket to force libssh2 out of any in-flight
         * read/write; the worker then drops through to error cleanup fast. */
        if (s->connect_thread_active) {
            atomic_store_explicit(&s->connect_cancel, true, memory_order_release);
            /* If worker is blocked on hostkey dialog, wake it with rejection */
            pthread_mutex_lock(&s->hostkey_lock);
            if (s->hostkey_pending) {
                s->hostkey_accepted = false;
                s->hostkey_pending = false;
                pthread_cond_signal(&s->hostkey_cond);
            }
            pthread_mutex_unlock(&s->hostkey_lock);
            /* If worker is blocked on KBI dialog, wake it */
            pthread_mutex_lock(&s->kbi_lock);
            s->kbi_ready = true;
            pthread_cond_signal(&s->kbi_cond);
            pthread_mutex_unlock(&s->kbi_lock);
            /* If worker is blocked on passphrase dialog, wake it */
            pthread_mutex_lock(&s->status_lock);
            if (s->passphrase_pending) {
                s->passphrase_cancelled = true;
                pthread_cond_signal(&s->passphrase_cond);
            }
            pthread_mutex_unlock(&s->status_lock);
            if (s->is_shared_owner && s->sock >= 0) {
                shutdown(s->sock, SHUT_RDWR); /* unblock libssh2 reads */
            }
            pthread_join(s->connect_thread, NULL);
            s->connect_thread_active = false;
        }
        /* Remote forwarding cleanup */
        remote_forward_cleanup_all(s);
        if (s->is_shared_owner && s->ssh_session) {
            forward_cleanup(s->ssh_session);
        }
        if (s->sftp_session) libssh2_sftp_shutdown(s->sftp_session);
        if (s->ssh_channel) {
            libssh2_channel_send_eof(s->ssh_channel);
            libssh2_channel_close(s->ssh_channel);
            libssh2_channel_free(s->ssh_channel);
        }
        /* Only tear down the SSH session and socket if we own them.
         * Borrowed (shared) sessions leave the owner's connection intact. */
        if (s->is_shared_owner) {
            if (s->ssh_session) {
                libssh2_session_disconnect(s->ssh_session, "Client disconnect");
                libssh2_session_free(s->ssh_session);
            }
            if (s->sock >= 0) {
                /* Cancel any dispatch_source_t watching this fd before the
                 * close(), per Apple's libdispatch rules (the source's fd
                 * must remain valid until cancellation completes). */
                platform_unwatch_socket(s->sock);
                close(s->sock);
            }
        }
        /* Proxy chain cleanup */
        proxy_hops_cleanup(s);
        pthread_cond_destroy(&s->passphrase_cond);
        pthread_mutex_destroy(&s->status_lock);
        pthread_mutex_destroy(&s->hostkey_lock);
        pthread_cond_destroy(&s->hostkey_cond);
        pthread_mutex_destroy(&s->kbi_lock);
        pthread_cond_destroy(&s->kbi_cond);
    }
    /* KBI prompt/response heap pools — responses can hold 2FA tokens / OTPs;
     * scrub before free. Prompts are public text (no scrub needed). */
    if (s->kbi_responses) {
        secure_zero(s->kbi_responses,
                    (usize)KBI_MAX_PROMPTS * sizeof(*s->kbi_responses));
        free(s->kbi_responses);
        s->kbi_responses = NULL;
    }
    free(s->kbi_prompts);
    s->kbi_prompts = NULL;
    free(s->initial_cwd);
    s->initial_cwd = NULL;
    free(s->remote_listeners);
    s->remote_listeners = NULL;
    free(s->remote_fwd_conns);
    s->remote_fwd_conns = NULL;
    free(s->x11_relays);
    s->x11_relays = NULL;
    /* Scrub all credentials + dynamic arrays + key material via the canonical
     * dispose path (secure_zero on every secret), then release the heap shell. */
    if (s->config) {
        ssh_config_dispose(s->config);
        free(s->config);
    }
    free(s->write_buf);
    free(s);
}

/* =========================================================================
 * I/O
 * ========================================================================= */

static bool session_queue_write(Session *s, const u8 *data, usize len) {
    if (!s || !data || len == 0) return true;
    if (len > SIZE_MAX - s->write_len) return false;
    usize need = s->write_len + len;
    if (need > SESSION_WRITE_BUFFER_LIMIT) return false;
    if (need > s->write_cap) {
        usize new_cap = s->write_cap ? s->write_cap : 4096;
        while (new_cap < need) {
            if (new_cap > SIZE_MAX / 2) { new_cap = need; break; }
            new_cap *= 2;
        }
        u8 *new_buf = realloc(s->write_buf, new_cap);
        if (!new_buf) return false;
        s->write_buf = new_buf;
        s->write_cap = new_cap;
    }
    memcpy(s->write_buf + s->write_len, data, len);
    s->write_len += len;
    return true;
}

static i32 session_try_write_now(Session *s, const u8 *data, usize len) {
    if (!s || !data || len == 0) return 0;
    if (s->type == SESSION_LOCAL) {
        ssize_t n = write(s->pty_master, data, len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
            return -1;
        }
        return (i32)n;
    }
    if (s->type == SESSION_SSH) {
        ssize_t n = libssh2_channel_write(s->ssh_channel, (const char *)data, len);
        if (n == LIBSSH2_ERROR_EAGAIN) return 0;
        if (n < 0) return -1;
        return (i32)n;
    }
    if (s->type == SESSION_TELNET)
        return s->telnet ? telnet_write(s->telnet, data, (i32)len) : -1;
    if (s->type == SESSION_SERIAL)
        return s->serial ? serial_write(s->serial, data, (i32)len) : -1;
    return s->mosh ? mosh_write(s->mosh, data, (i32)len) : -1;
}

static void session_flush_pending_write(Session *s) {
    if (!s || s->write_len == 0 || session_status(s) != SESSION_CONNECTED) return;
    usize flush_limit = s->write_len < SESSION_WRITE_FLUSH_LIMIT
                      ? s->write_len : SESSION_WRITE_FLUSH_LIMIT;
    usize total = 0;
    while (total < flush_limit) {
        i32 n = session_try_write_now(s, s->write_buf + total, flush_limit - total);
        if (n <= 0) break;
        total += (usize)n;
    }
    if (total > 0) {
        s->write_len -= total;
        if (s->write_len > 0) memmove(s->write_buf, s->write_buf + total, s->write_len);
    }
}

/* Temporary diagnostic: when $LIU_PTY_LOG names a file, append a hex-escaped
 * trace of bytes exchanged with the PTY. "IN" = child→Liu, "OUT" = Liu→child.
 * Used to debug runaway TUI output (e.g. opencode redrawing nonstop). A no-op
 * when the env var is unset, so it costs nothing in normal use. */
static void pty_log_bytes(const char *dir, const u8 *data, i32 len) {
    static FILE *f = NULL;
    static int inited = 0;
    if (!inited) {
        inited = 1;
        const char *p = getenv("LIU_PTY_LOG");
        if (p && *p) f = fopen(p, "a");
    }
    if (!f || !data || len <= 0) return;
    fprintf(f, "[%s %d] ", dir, len);
    for (i32 i = 0; i < len; i++) {
        u8 c = data[i];
        if (c == 0x1b)                  fputs("\\e", f);
        else if (c >= 0x20 && c < 0x7f) fputc((int)c, f);
        else                            fprintf(f, "\\x%02x", c);
    }
    fputc('\n', f);
    fflush(f);
}

i32 session_read(Session *s, u8 *buf, i32 buf_size) {
    /* go through locked accessor so we acquire the worker's
     * release fence before reading s->ssh_channel / ssh_session. */
    if (session_status(s) != SESSION_CONNECTED) return -1;
    session_flush_pending_write(s);

    if (s->type == SESSION_LOCAL) {
        ssize_t n = read(s->pty_master, buf, (usize)buf_size);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (n > 0) pty_log_bytes("IN", buf, (i32)n);
        return (i32)n;
    } else if (s->type == SESSION_MOSH) {
        return s->mosh ? mosh_read(s->mosh, buf, buf_size) : -1;
    } else if (s->type == SESSION_TELNET) {
        return s->telnet ? telnet_read(s->telnet, buf, buf_size) : -1;
    } else if (s->type == SESSION_SERIAL) {
        return s->serial ? serial_read(s->serial, buf, buf_size) : -1;
    } else {
        ssize_t n = libssh2_channel_read(s->ssh_channel, (char *)buf, (usize)buf_size);
        if (n == LIBSSH2_ERROR_EAGAIN) return 0;
        if (n < 0) return -1;
        return (i32)n;
    }
}

i32 session_write(Session *s, const u8 *data, i32 len) {
    if (session_status(s) != SESSION_CONNECTED) return -1;
    if (!data || len <= 0) return 0;
    pty_log_bytes("OUT", data, len);

    if (s->type == SESSION_MOSH) {
        return s->mosh ? mosh_write(s->mosh, data, len) : -1;
    }
    if (s->type == SESSION_TELNET) {
        return s->telnet ? telnet_write(s->telnet, data, len) : -1;
    }
    if (s->type == SESSION_SERIAL) {
        return s->serial ? serial_write(s->serial, data, len) : -1;
    }

    session_flush_pending_write(s);
    if (s->write_len > 0) {
        return session_queue_write(s, data, (usize)len) ? len : -1;
    }

    usize total = 0;
    while (total < (usize)len) {
        i32 n = session_try_write_now(s, data + total, (usize)len - total);
        if (n < 0) return total > 0 ? (i32)total : -1;
        if (n == 0) {
            if (!session_queue_write(s, data + total, (usize)len - total)) return -1;
            return len;
        }
        total += (usize)n;
    }
    return (i32)total;
}

usize session_pending_write_bytes(const Session *s) {
    if (!s) return 0;
    return s->write_len;
}

void session_resize(Session *s, i32 cols, i32 rows) {
    s->cols = cols;
    s->rows = rows;

    if (s->type == SESSION_LOCAL) {
        struct winsize ws = {
            .ws_row = (unsigned short)rows,
            .ws_col = (unsigned short)cols,
        };
        ioctl(s->pty_master, TIOCSWINSZ, &ws);
    } else if (s->type == SESSION_MOSH) {
        if (s->mosh) mosh_resize(s->mosh, cols, rows);
    } else if (s->type == SESSION_TELNET) {
        if (s->telnet) telnet_resize(s->telnet, cols, rows);
    } else if (s->type == SESSION_SERIAL) {
        /* fixed-geometry device; nothing to negotiate */
    } else {
        /* wait until worker has published the channel. */
        if (session_status(s) != SESSION_CONNECTED) return;
        if (s->ssh_channel)
            libssh2_channel_request_pty_size(s->ssh_channel, cols, rows);
    }
}

/* =========================================================================
 * Status
 * ========================================================================= */

SessionStatus session_status(const Session *s) {
    /* SSH session's status is written by a worker during connect.
     * Lock ensures a consistent view when polled from main thread.  Local
     * sessions skip the lock since they have no worker. */
    if (s->type == SESSION_SSH || s->type == SESSION_MOSH) {
        Session *sm = (Session *)s;
        pthread_mutex_lock(&sm->status_lock);
        SessionStatus st = s->status;
        pthread_mutex_unlock(&sm->status_lock);
        return st;
    }
    return s->status;
}
SessionType   session_type(const Session *s)   { return s->type; }
const char   *session_error(const Session *s)  { return s->error_msg; }
int           session_sock(const Session *s)   { return s ? s->sock : -1; }

int session_io_fd(const Session *s) {
    if (!s) return -1;
    if (s->type == SESSION_SSH) return s->sock;
    if (s->type == SESSION_TELNET) return s->telnet ? telnet_fd(s->telnet) : -1;
    if (s->type == SESSION_SERIAL) return s->serial ? serial_fd(s->serial) : -1;
    /* LOCAL and MOSH both use a PTY master fd for the read path —
     * mosh_create wraps mosh-client in a local PTY just like a bare shell. */
    return s->pty_master;
}

bool session_is_shared_owner(const Session *s) {
    if (!s) return false;
    if (s->type != SESSION_SSH) return true;
    return s->is_shared_owner;
}

bool session_is_alive(Session *s) {
    if (s->status != SESSION_CONNECTED) return false;
    if (s->type == SESSION_LOCAL) {
        int status;
        pid_t r = waitpid(s->child_pid, &status, WNOHANG);
        if (r == 0) return true; /* still running */
        if (r == s->child_pid) {
            /* Stash the wait status so session_exited() can report the real
             * exit code; this is the only place the child is reaped (no
             * SIGCHLD handler exists by design), so a second waitpid would
             * race to ECHILD. */
            s->last_exit_status = status;
            s->exited = true;
            s->child_pid = -1;
            s->status = SESSION_DISCONNECTED;
        }
        return false;
    } else if (s->type == SESSION_MOSH) {
        return s->mosh ? mosh_is_alive(s->mosh) : false;
    } else if (s->type == SESSION_TELNET) {
        return s->telnet ? telnet_is_alive(s->telnet) : false;
    } else if (s->type == SESSION_SERIAL) {
        return s->serial ? serial_is_alive(s->serial) : false;
    } else {
        return !libssh2_channel_eof(s->ssh_channel);
    }
}

i32 session_child_pid(Session *s) {
    if (!s || s->type != SESSION_LOCAL) return -1;
    return (i32)s->child_pid;
}

bool session_exited(const Session *s, int *code_out) {
    if (!s || !s->exited) return false;
    if (code_out) {
        int st = s->last_exit_status;
        if (WIFEXITED(st))        *code_out = WEXITSTATUS(st);
        else if (WIFSIGNALED(st)) *code_out = -WTERMSIG(st);
        else                      *code_out = -1;
    }
    return true;
}

bool session_suspend(Session *s) {
    if (!s) return false;
    if (s->suspended) return true;

    /* SSH: no OS signal to send -- the remote shell keeps running server-side.
     * We just flag the session as suspended so the poll loop drains channel
     * reads (so libssh2 window stays open and keepalives keep flowing). */
    if (s->type == SESSION_SSH) {
        if (s->status != SESSION_CONNECTED) return false;
        s->suspended = true;
        return true;
    }

    pid_t target = -1;
    if (s->type == SESSION_LOCAL && s->child_pid > 0) {
        target = s->child_pid;
    } else if (s->type == SESSION_MOSH && s->mosh) {
        pid_t p = mosh_child_pid(s->mosh);
        if (p > 0) target = p;
    }
    if (target <= 0) return false;
    /* Signal the full process group so the shell's children (editors, tools)
     * also stop, not just the shell itself. */
    pid_t pgid = getpgid(target);
    if (pgid > 0) {
        if (killpg(pgid, SIGSTOP) == 0) { s->suspended = true; return true; }
    }
    if (kill(target, SIGSTOP) == 0) { s->suspended = true; return true; }
    return false;
}

bool session_resume(Session *s) {
    if (!s) return false;
    if (!s->suspended) return true;

    if (s->type == SESSION_SSH) {
        s->suspended = false;
        return true;
    }

    pid_t target = -1;
    if (s->type == SESSION_LOCAL && s->child_pid > 0) {
        target = s->child_pid;
    } else if (s->type == SESSION_MOSH && s->mosh) {
        pid_t p = mosh_child_pid(s->mosh);
        if (p > 0) target = p;
    }
    if (target <= 0) return false;
    pid_t pgid = getpgid(target);
    bool ok = false;
    if (pgid > 0 && killpg(pgid, SIGCONT) == 0) ok = true;
    if (!ok && kill(target, SIGCONT) == 0) ok = true;
    if (ok) s->suspended = false;
    return ok;
}

bool session_send_tstp(Session *s) {
    if (!s || s->type != SESSION_LOCAL || s->status != SESSION_CONNECTED)
        return false;

    if (s->pty_master < 0) return false;
    pid_t pgid = tcgetpgrp(s->pty_master);
    if (pgid <= 0) return false;
    return killpg(pgid, SIGTSTP) == 0;
}

bool session_is_suspended(const Session *s) {
    return s ? s->suspended : false;
}

bool session_fg_is_shell(Session *s) {
    if (!s || s->type != SESSION_LOCAL || s->status != SESSION_CONNECTED) return true;
    if (s->pty_master < 0 || s->child_pid <= 0) return true;
    pid_t fg = tcgetpgrp(s->pty_master);
    if (fg <= 0) return true;
    pid_t shell_pgid = getpgid(s->child_pid);
    if (shell_pgid <= 0) return true;
    return fg == shell_pgid;
}

bool session_kill_fg(Session *s, int sig) {
    if (!s || s->type != SESSION_LOCAL || s->status != SESSION_CONNECTED) return false;
    if (s->pty_master < 0 || s->child_pid <= 0) return false;
    pid_t fg = tcgetpgrp(s->pty_master);
    if (fg <= 0) return false;
    pid_t shell_pgid = getpgid(s->child_pid);
    if (shell_pgid > 0 && fg == shell_pgid) return false;
    return killpg(fg, sig) == 0;
}

/* Drop the foreground-process TTL cache so the next session_fg_process()
 * scans live. Used at close time, where a 0.6 s stale window could miss a
 * just-launched agent (or linger on one that just exited). */
void session_invalidate_fg_cache(Session *s) {
    if (s) s->fg_cache_at = 0.0;
}

const char *session_fg_process(Session *s) {
    static char empty[1] = {0};
    if (!s || s->type != SESSION_LOCAL || s->status != SESSION_CONNECTED) return empty;

    pid_t fg = tcgetpgrp(s->pty_master);   /* cheap ioctl */

    /* TTL cache: at a shell prompt this fires on every keystroke (autosuggest);
     * the macOS lookup below scans every pid on the system. Re-scan only when
     * the fg process group changes (a command started/exited) or the short TTL
     * lapses, so steady typing costs one ioctl instead of a full pid sweep. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    f64 now = (f64)ts.tv_sec + (f64)ts.tv_nsec / 1e9;
    if (s->fg_cache_at > 0.0 && s->fg_cache_pgid == fg && now - s->fg_cache_at < 0.6) {
        return s->fg_cache;
    }

    s->fg_cache[0] = '\0';
#ifdef PLATFORM_MACOS
    if (fg > 0) {
        /* Fast path: the foreground pgrp leader's pid IS the pgid, so one
         * proc_pidinfo identifies it without session_find_interesting_pgrp_
         * process()'s proc_listpids(PROC_ALL_PIDS) sweep over every process on
         * the system — a recurring main-thread stall (re-armed each time the
         * 0.6s TTL lapses while frames render). A native agent (grok, a bun-
         * compiled claude, …) is the leader and gets resolved here directly.
         * Only a plain shell/wrapper leader — which may hide the real agent in
         * a child (node/npm-launched CLIs) — still needs the group sweep. */
        struct proc_bsdinfo bi;
        if (proc_pidinfo(fg, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) == (int)sizeof bi
            && bi.pbi_name[0]) {
            char picked[64] = {0};
            if (session_proc_name_is_agent(bi.pbi_name)) {
                snprintf(s->fg_cache, sizeof s->fg_cache, "%s", bi.pbi_name);
            } else if (session_proc_name_versioned_agent(bi.pbi_name,
                                                         picked, sizeof picked)) {
                snprintf(s->fg_cache, sizeof s->fg_cache, "%s", picked);
            } else if (!session_proc_name_is_wrapper(bi.pbi_name) &&
                       session_proc_path_agent(fg, picked, sizeof picked)) {
                snprintf(s->fg_cache, sizeof s->fg_cache, "%s", picked);
            }
        }
        if (s->fg_cache[0] == '\0') {
            /* Leader is a shell/wrapper (or unidentified) — scan the group for a
             * child agent, keeping the leader-name fallback so shell tabs still
             * get a title. */
            char leader[256] = {0};
            proc_name(fg, leader, sizeof leader);
            (void)session_find_interesting_pgrp_process(fg, leader,
                                                        s->fg_cache, sizeof s->fg_cache);
        }
    }
#elif defined(PLATFORM_LINUX)
    if (fg > 0) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/comm", fg);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fgets(s->fg_cache, sizeof s->fg_cache, f)) {
                char *nl = strchr(s->fg_cache, '\n');
                if (nl) *nl = '\0';
            }
            fclose(f);
        }
    }
#endif
    s->fg_cache_at   = now;
    s->fg_cache_pgid = fg;
    return s->fg_cache;
}

const char *session_local_cwd(Session *s) {
    static char buf[1024];
    buf[0] = '\0';
    if (!s || s->type != SESSION_LOCAL || s->status != SESSION_CONNECTED) return buf;

    /* Prefer the PTY foreground process group leader — that's the active
     * shell/editor/repl in this tab. Fall back to the shell child PID if the
     * pgrp lookup fails (e.g., PTY just spawned, no fg pgrp set yet). */
    pid_t target = tcgetpgrp(s->pty_master);
    if (target <= 0) target = s->child_pid;
    if (target <= 0) return buf;

#ifdef PLATFORM_MACOS
    struct proc_vnodepathinfo vpi;
    int n = proc_pidinfo(target, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof vpi);
    if (n == (int)sizeof vpi && vpi.pvi_cdir.vip_path[0]) {
        snprintf(buf, sizeof buf, "%s", vpi.pvi_cdir.vip_path);
    }
#elif defined(PLATFORM_LINUX)
    char link[64];
    snprintf(link, sizeof link, "/proc/%d/cwd", (int)target);
    ssize_t n = readlink(link, buf, sizeof buf - 1);
    if (n > 0) buf[n] = '\0';
    else       buf[0] = '\0';
#else
    (void)target;
#endif
    return buf;
}

void *session_get_sftp(Session *s) {
    if (!s || s->type != SESSION_SSH) return NULL;
    if (session_status(s) != SESSION_CONNECTED) return NULL;
    return s->sftp_session;
}

void *session_sftp_handle(const Session *s) {
    if (!s || s->type != SESSION_SSH) return NULL;
    if (session_status(s) != SESSION_CONNECTED) return NULL;
    return s->sftp_session;
}

const char *session_initial_cwd(const Session *s) {
    if (!s || s->type != SESSION_SSH) return "";
    if (session_status(s) != SESSION_CONNECTED) return "";
    return s->initial_cwd ? s->initial_cwd : "";
}

/* The LIBSSH2_SESSION * we should flip to blocking mode for an SFTP call
 * on this Session: its own if owner, or the shared owner's. Returns NULL
 * if the session isn't usable for SFTP right now. */
static LIBSSH2_SESSION *session_libssh2_for_sftp(Session *s) {
    if (!s || s->type != SESSION_SSH) return NULL;
    if (session_status(s) != SESSION_CONNECTED) return NULL;
    if (!s->sftp_session) return NULL;
    if (!s->is_shared_owner && s->shared_session)
        return s->shared_session->ssh_session;
    return s->ssh_session;
}

void session_sftp_scope_begin(Session *s) {
    LIBSSH2_SESSION *ssh = session_libssh2_for_sftp(s);
    if (ssh) libssh2_session_set_blocking(ssh, 1);
}

void session_sftp_scope_end(Session *s) {
    LIBSSH2_SESSION *ssh = session_libssh2_for_sftp(s);
    if (ssh) libssh2_session_set_blocking(ssh, 0);
}

bool session_io_is_suspended(const Session *s) {
    return s ? atomic_load(&((Session *)s)->io_suspended) : false;
}

void session_io_set_suspended(Session *s, bool v) {
    if (s) atomic_store(&s->io_suspended, v);
}

const SSHConfig *session_get_config(const Session *s) {
    if (!s || (s->type != SESSION_SSH && s->type != SESSION_MOSH)) return NULL;
    return s->config;
}

bool session_reconnect(Session *s) {
    if (!s || s->type != SESSION_SSH || !s->config) return false;

    /* make sure any prior connect worker is fully done before we
     * touch libssh2 state, otherwise the worker and this thread would race. */
    if (s->connect_thread_active) {
        atomic_store_explicit(&s->connect_cancel, true, memory_order_release);
        if (s->is_shared_owner && s->sock >= 0) shutdown(s->sock, SHUT_RDWR);
        pthread_join(s->connect_thread, NULL);
        s->connect_thread_active = false;
    }

    /* Cleanup old connection */
    session_stop_stream_forwards(s);
    remote_forward_cleanup_all(s);
    if (s->is_shared_owner && s->ssh_session) {
        forward_cleanup(s->ssh_session);
    }
    if (s->sftp_session) { libssh2_sftp_shutdown(s->sftp_session); s->sftp_session = NULL; }
    /* initial_cwd is a heap pointer that is only allocated once SFTP+realpath
     * succeed; it is NULL on SFTP-less hosts. Writing initial_cwd[0] here
     * dereferenced NULL on auto-reconnect. free()+NULL also drops the stale
     * cwd so the fresh connection re-resolves it. */
    free(s->initial_cwd);
    s->initial_cwd = NULL;
    s->sftp_init_tried = false;
    if (s->ssh_channel) {
        libssh2_channel_send_eof(s->ssh_channel);
        libssh2_channel_close(s->ssh_channel);
        libssh2_channel_free(s->ssh_channel);
        s->ssh_channel = NULL;
    }
    if (s->is_shared_owner) {
        if (s->ssh_session) {
            libssh2_session_disconnect(s->ssh_session, "reconnect");
            libssh2_session_free(s->ssh_session);
            s->ssh_session = NULL;
        }
        if (s->sock >= 0) {
            /* Unwatch before close — otherwise the UI's status-edge
             * detector (which fires next tick with s->sock==-1) has no
             * way to cancel the dispatch source, leaving a leaked
             * source watching a recycled fd. */
            platform_unwatch_socket(s->sock);
            close(s->sock);
            s->sock = -1;
        }
    } else {
        s->ssh_session = NULL;
        s->sock = -1;
    }
    s->shared_session = NULL;
    s->is_shared_owner = true; /* reconnect always creates its own connection */
    proxy_hops_cleanup(s);

    /* Reset cancel flag for new attempt, set status, and spawn worker.
     * same async model as session_create_ssh — the caller returns
     * immediately with status == SESSION_CONNECTING, UI polls for transition. */
    atomic_store_explicit(&s->connect_cancel, false, memory_order_release);
    pthread_mutex_lock(&s->status_lock);
    s->status = SESSION_CONNECTING;
    s->error_msg[0] = '\0';
    pthread_mutex_unlock(&s->status_lock);

    /* Reset hostkey dialog state for new connection attempt */
    pthread_mutex_lock(&s->hostkey_lock);
    s->hostkey_pending = false;
    s->hostkey_accepted = false;
    pthread_mutex_unlock(&s->hostkey_lock);

    /* Reset KBI dialog state */
    pthread_mutex_lock(&s->kbi_lock);
    s->kbi_ready = false;
    s->kbi_num_prompts = 0;
    pthread_mutex_unlock(&s->kbi_lock);

    /* Reset keepalive counters */
    s->last_keepalive_time = 0;
    s->keepalive_missed = 0;

    if (pthread_create(&s->connect_thread, NULL, ssh_connect_worker, s) != 0) {
        set_error(s, "Failed to spawn SSH reconnect thread");
        return false;
    }
    s->connect_thread_active = true;
    return true;
}

/* =========================================================================
 * Host key dialog accessors (called from main/UI thread)
 * ========================================================================= */

bool session_hostkey_pending(Session *s) {
    if (!s || s->type != SESSION_SSH) return false;
    pthread_mutex_lock(&s->hostkey_lock);
    bool pending = s->hostkey_pending;
    pthread_mutex_unlock(&s->hostkey_lock);
    return pending;
}

void session_hostkey_get_info(Session *s, bool *is_change,
                              char *hostname, usize hostname_sz,
                              i32 *port,
                              char *old_fp, usize old_fp_sz,
                              char *new_fp, usize new_fp_sz) {
    if (!s) return;
    pthread_mutex_lock(&s->hostkey_lock);
    if (is_change)  *is_change = s->hostkey_is_change;
    if (hostname && s->config)
        snprintf(hostname, hostname_sz, "%s", s->config->hostname);
    if (port && s->config) *port = s->config->port;
    if (old_fp) snprintf(old_fp, old_fp_sz, "%s", s->hostkey_old_fp);
    if (new_fp) snprintf(new_fp, new_fp_sz, "%s", s->hostkey_new_fp);
    pthread_mutex_unlock(&s->hostkey_lock);
}

void session_hostkey_respond(Session *s, bool accept) {
    if (!s || s->type != SESSION_SSH) return;
    pthread_mutex_lock(&s->hostkey_lock);
    s->hostkey_accepted = accept;
    s->hostkey_pending = false;
    pthread_cond_signal(&s->hostkey_cond);
    pthread_mutex_unlock(&s->hostkey_lock);
}

/* =========================================================================
 * Keyboard-interactive (KBI / 2FA) accessors — called from UI thread
 * ========================================================================= */

bool session_kbi_pending(Session *s) {
    if (!s || s->type != SESSION_SSH) return false;
    pthread_mutex_lock(&s->status_lock);
    bool pending = (s->status == SESSION_KBI_PENDING);
    pthread_mutex_unlock(&s->status_lock);
    return pending;
}

i32 session_kbi_num_prompts(Session *s) {
    if (!s) return 0;
    pthread_mutex_lock(&s->kbi_lock);
    i32 n = s->kbi_num_prompts;
    pthread_mutex_unlock(&s->kbi_lock);
    return n;
}

const char *session_kbi_name(Session *s) {
    if (!s) return "";
    /* Caller must use result before next call; kbi_name is stable while
     * SESSION_KBI_PENDING (worker is blocked). */
    return s->kbi_name;
}

const char *session_kbi_instruction(Session *s) {
    if (!s) return "";
    return s->kbi_instruction;
}

const char *session_kbi_prompt(Session *s, i32 index) {
    if (!s || !s->kbi_prompts || index < 0 || index >= s->kbi_num_prompts) return "";
    return s->kbi_prompts[index];
}

bool session_kbi_echo(Session *s, i32 index) {
    if (!s || index < 0 || index >= s->kbi_num_prompts) return true;
    return s->kbi_echo[index];
}

void session_kbi_submit(Session *s, const char **responses, i32 count) {
    if (!s) return;
    pthread_mutex_lock(&s->kbi_lock);
    if (s->kbi_responses) {
        i32 n = (count < s->kbi_num_prompts) ? count : s->kbi_num_prompts;
        for (i32 i = 0; i < n; i++) {
            if (responses[i]) {
                snprintf(s->kbi_responses[i], sizeof(s->kbi_responses[i]), "%s", responses[i]);
            } else {
                s->kbi_responses[i][0] = '\0';
            }
        }
    }
    s->kbi_ready = true;
    pthread_cond_signal(&s->kbi_cond);
    pthread_mutex_unlock(&s->kbi_lock);
}

/* =========================================================================
 * Port forwarding implementations
 * ========================================================================= */

bool session_local_forward_start(Session *s, i32 local_port,
                                  const char *remote_host, i32 remote_port) {
    if (!s || s->type != SESSION_SSH || s->status != SESSION_CONNECTED)
        return false;
    if (!s->ssh_session) return false;
    return forward_add_local(s->ssh_session, local_port, remote_host, remote_port) >= 0;
}

bool session_dynamic_forward_start(Session *s, i32 local_port) {
    if (!s || s->type != SESSION_SSH || s->status != SESSION_CONNECTED)
        return false;
    if (!s->ssh_session) return false;
    return forward_add_socks5(s->ssh_session, local_port) >= 0;
}

void session_local_forward_poll(Session *s) {
    if (!s || s->type != SESSION_SSH || s->status != SESSION_CONNECTED)
        return;
    if (!s->ssh_session) return;
    forward_poll(s->ssh_session);
}

i32 session_local_forward_count(Session *s) {
    if (!s || s->type != SESSION_SSH || s->status != SESSION_CONNECTED)
        return 0;
    if (!s->ssh_session) return 0;
    return forward_count(s->ssh_session);
}

bool session_local_forward_get(Session *s, i32 index, PortForwardInfo *info) {
    if (!info) return false;
    if (!s || s->type != SESSION_SSH || s->status != SESSION_CONNECTED)
        return false;
    if (!s->ssh_session) return false;

    ForwardRule rule;
    if (!forward_get(s->ssh_session, index, &rule)) return false;

    memset(info, 0, sizeof(*info));
    snprintf(info->bind_host, sizeof(info->bind_host), "%s", "127.0.0.1");
    info->bind_port = rule.local_port;
    snprintf(info->remote_host, sizeof(info->remote_host), "%s", rule.remote_host);
    info->remote_port = rule.remote_port;
    info->local_port = rule.local_port;
    info->active = rule.active;
    info->type = rule.type;
    return true;
}

void session_local_forward_remove(Session *s, i32 index) {
    if (!s || s->type != SESSION_SSH || !s->ssh_session) return;
    forward_remove(s->ssh_session, index);
}

/* =========================================================================
 * Remote port forwarding
 * ========================================================================= */

static void *remote_fwd_relay_thread(void *arg) {
    RemoteForwardConn *c = (RemoteForwardConn *)arg;
    u8 buf[16384];
    while (c->running) {
        bool did_work = false;

        /* channel -> dest_sock */
        ssize_t n = libssh2_channel_read(c->channel, (char *)buf, sizeof(buf));
        if (n > 0) {
            ssize_t written = 0;
            while (written < n) {
                ssize_t w = write(c->dest_sock, buf + written, (usize)(n - written));
                if (w <= 0) { c->running = false; break; }
                written += w;
            }
            did_work = true;
        } else if (n == 0 || (n < 0 && n != LIBSSH2_ERROR_EAGAIN)) {
            c->running = false;
            break;
        }

        /* dest_sock -> channel */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(c->dest_sock, &fds);
        struct timeval tv = {0, 0};
        if (select(c->dest_sock + 1, &fds, NULL, NULL, &tv) > 0) {
            n = read(c->dest_sock, buf, sizeof(buf));
            if (n > 0) {
                ssize_t written = 0;
                while (written < n) {
                    ssize_t w = libssh2_channel_write(c->channel,
                        (const char *)buf + written, (usize)(n - written));
                    if (w == LIBSSH2_ERROR_EAGAIN) { usleep(1000); continue; }
                    if (w < 0) { c->running = false; break; }
                    written += w;
                }
                did_work = true;
            } else if (n == 0) {
                c->running = false;
            }
        }

        if (!did_work && c->running) usleep(1000);
    }
    return NULL;
}

void session_remote_forward_poll(Session *s) {
    if (!s || s->type != SESSION_SSH || s->status != SESSION_CONNECTED) return;
    if (s->remote_listener_count == 0 || !s->remote_listeners) return;

    /* Check each listener for accepted connections */
    for (i32 i = 0; i < s->remote_listener_count; i++) {
        RemoteForwardListener *l = &s->remote_listeners[i];
        if (!l->active || !l->listener) continue;

        LIBSSH2_CHANNEL *ch = libssh2_channel_forward_accept(l->listener);
        if (!ch) continue;

        /* Accepted a new remote forward connection — connect to local dest */
        int dest_sock = net_tcp_connect(l->dest_host, l->dest_port);
        if (dest_sock < 0) {
            libssh2_channel_free(ch);
            continue;
        }

        if (s->remote_fwd_conn_count >= MAX_REMOTE_FWD_CONNS) {
            close(dest_sock);
            libssh2_channel_free(ch);
            continue;
        }
        if (!s->remote_fwd_conns) {
            s->remote_fwd_conns = calloc(MAX_REMOTE_FWD_CONNS,
                                         sizeof(*s->remote_fwd_conns));
            if (!s->remote_fwd_conns) {
                close(dest_sock);
                libssh2_channel_free(ch);
                continue;
            }
        }

        RemoteForwardConn *c = &s->remote_fwd_conns[s->remote_fwd_conn_count];
        c->channel = ch;
        c->dest_sock = dest_sock;
        c->running = true;

        net_set_nonblocking(dest_sock);

        if (pthread_create(&c->thread, NULL, remote_fwd_relay_thread, c) != 0) {
            close(dest_sock);
            libssh2_channel_free(ch);
            continue;
        }
        s->remote_fwd_conn_count++;
    }

    /* Clean up finished connections */
    for (i32 i = 0; i < s->remote_fwd_conn_count; ) {
        RemoteForwardConn *c = &s->remote_fwd_conns[i];
        if (!c->running) {
            pthread_join(c->thread, NULL);
            if (c->dest_sock >= 0) close(c->dest_sock);
            if (c->channel) libssh2_channel_free(c->channel);
            /* Swap with last */
            s->remote_fwd_conns[i] = s->remote_fwd_conns[s->remote_fwd_conn_count - 1];
            s->remote_fwd_conn_count--;
        } else {
            i++;
        }
    }
}

/* =========================================================================
 * Keepalive
 * ========================================================================= */

void session_keepalive_check(Session *s, f64 now_sec) {
    if (!s || s->type != SESSION_SSH || s->status != SESSION_CONNECTED) return;
    if (!s->ssh_session) return;

    i32 ival = s->keepalive_interval;
    if (ival <= 0) return;
    if (now_sec <= 0) return;
    if (s->last_keepalive_time > 0 &&
        now_sec - s->last_keepalive_time < (f64)ival) {
        return;
    }

    /* libssh2_keepalive_send returns seconds until next keepalive should
     * be sent.  If it returns 0 or an error, we might have connectivity
     * issues. */
    int seconds_to_next = 0;
    int rc = libssh2_keepalive_send(s->ssh_session, &seconds_to_next);
    s->last_keepalive_time = now_sec;
    if (rc != 0) {
        s->keepalive_missed++;
        if (s->keepalive_missed >= s->keepalive_count_max) {
            set_error(s, "SSH keepalive timeout — connection lost");
        }
    } else {
        s->keepalive_missed = 0;
    }
}

/* =========================================================================
 * ssh_config_default
 * ========================================================================= */

SSHConfig ssh_config_default(void) {
    SSHConfig c;
    memset(&c, 0, sizeof(c));
    c.port = 22;
    c.auth_method = AUTH_AGENT;
    return c;
}

/* ----- Dynamic array helpers --------------------------------------------- */

static bool grow_array(void **base, i32 *cap, i32 want, usize elem) {
    if (want <= *cap) return true;
    i32 ncap = *cap > 0 ? *cap : 2;
    while (ncap < want) ncap *= 2;
    void *p = realloc(*base, (usize)ncap * elem);
    if (!p) return false;
    /* Zero the freshly grown tail so callers reading uninitialised slots
     * (e.g. before they've filled the new entry) see a stable value. */
    u8 *bytes = (u8 *)p;
    memset(bytes + (usize)*cap * elem, 0, (usize)(ncap - *cap) * elem);
    *base = p;
    *cap = ncap;
    return true;
}

/* The struct uses both `_count` and `_len` for sizes and singular vs plural
 * field names (e.g. `local_forwards` array but `local_forward_count`), so each
 * helper expands the explicit field/cap/len triple instead of a macro. */
StreamLocalForward *ssh_config_push_stream_forward(SSHConfig *cfg) {
    if (!cfg) return NULL;
    if (!grow_array((void **)&cfg->stream_forwards, &cfg->stream_forward_cap,
                    cfg->stream_forward_count + 1, sizeof(StreamLocalForward))) return NULL;
    StreamLocalForward *slot = &cfg->stream_forwards[cfg->stream_forward_count++];
    memset(slot, 0, sizeof *slot);
    return slot;
}

ProxyHop *ssh_config_push_proxy_hop(SSHConfig *cfg) {
    if (!cfg) return NULL;
    if (!grow_array((void **)&cfg->proxy_chain, &cfg->proxy_chain_cap,
                    cfg->proxy_chain_len + 1, sizeof(ProxyHop))) return NULL;
    ProxyHop *slot = &cfg->proxy_chain[cfg->proxy_chain_len++];
    memset(slot, 0, sizeof *slot);
    return slot;
}

PortForwardSpec *ssh_config_push_local_forward(SSHConfig *cfg) {
    if (!cfg) return NULL;
    if (!grow_array((void **)&cfg->local_forwards, &cfg->local_forward_cap,
                    cfg->local_forward_count + 1, sizeof(PortForwardSpec))) return NULL;
    PortForwardSpec *slot = &cfg->local_forwards[cfg->local_forward_count++];
    memset(slot, 0, sizeof *slot);
    return slot;
}

PortForwardSpec *ssh_config_push_remote_forward(SSHConfig *cfg) {
    if (!cfg) return NULL;
    if (!grow_array((void **)&cfg->remote_forwards, &cfg->remote_forward_cap,
                    cfg->remote_forward_count + 1, sizeof(PortForwardSpec))) return NULL;
    PortForwardSpec *slot = &cfg->remote_forwards[cfg->remote_forward_count++];
    memset(slot, 0, sizeof *slot);
    return slot;
}

SSHEnvVar *ssh_config_push_env(SSHConfig *cfg) {
    if (!cfg) return NULL;
    if (!grow_array((void **)&cfg->env, &cfg->env_cap,
                    cfg->env_count + 1, sizeof(SSHEnvVar))) return NULL;
    SSHEnvVar *slot = &cfg->env[cfg->env_count++];
    memset(slot, 0, sizeof *slot);
    return slot;
}

bool ssh_config_clone(SSHConfig *dst, const SSHConfig *src) {
    if (!dst) return false;
    /* Free any existing arrays in dst before overwriting. */
    ssh_config_dispose(dst);
    if (!src) return true;

    /* Scalar copy: covers fixed-size strings + counts + pointer fields we'll
     * overwrite below. Scratch the dynamic-pointer fields immediately so any
     * early return doesn't leave dst aliasing src. */
    *dst = *src;
    dst->stream_forwards = NULL; dst->stream_forward_cap = 0;
    dst->proxy_chain     = NULL; dst->proxy_chain_cap = 0;
    dst->local_forwards  = NULL; dst->local_forward_cap = 0;
    dst->remote_forwards = NULL; dst->remote_forward_cap = 0;
    dst->env             = NULL; dst->env_cap = 0;
    dst->key_pem         = NULL; dst->key_pem_len = 0;

    if (src->stream_forward_count > 0) {
        usize n = (usize)src->stream_forward_count;
        dst->stream_forwards = malloc(n * sizeof(*dst->stream_forwards));
        if (!dst->stream_forwards) goto fail;
        memcpy(dst->stream_forwards, src->stream_forwards, n * sizeof(*dst->stream_forwards));
        dst->stream_forward_cap = src->stream_forward_count;
    }
    if (src->proxy_chain_len > 0) {
        usize n = (usize)src->proxy_chain_len;
        dst->proxy_chain = malloc(n * sizeof(*dst->proxy_chain));
        if (!dst->proxy_chain) goto fail;
        memcpy(dst->proxy_chain, src->proxy_chain, n * sizeof(*dst->proxy_chain));
        dst->proxy_chain_cap = src->proxy_chain_len;
    }
    if (src->local_forward_count > 0) {
        usize n = (usize)src->local_forward_count;
        dst->local_forwards = malloc(n * sizeof(*dst->local_forwards));
        if (!dst->local_forwards) goto fail;
        memcpy(dst->local_forwards, src->local_forwards, n * sizeof(*dst->local_forwards));
        dst->local_forward_cap = src->local_forward_count;
    }
    if (src->remote_forward_count > 0) {
        usize n = (usize)src->remote_forward_count;
        dst->remote_forwards = malloc(n * sizeof(*dst->remote_forwards));
        if (!dst->remote_forwards) goto fail;
        memcpy(dst->remote_forwards, src->remote_forwards, n * sizeof(*dst->remote_forwards));
        dst->remote_forward_cap = src->remote_forward_count;
    }
    if (src->env_count > 0) {
        usize n = (usize)src->env_count;
        dst->env = malloc(n * sizeof(*dst->env));
        if (!dst->env) goto fail;
        memcpy(dst->env, src->env, n * sizeof(*dst->env));
        dst->env_cap = src->env_count;
    }
    if (src->key_pem && src->key_pem_len > 0) {
        dst->key_pem = malloc(src->key_pem_len);
        if (!dst->key_pem) goto fail;
        memcpy(dst->key_pem, src->key_pem, src->key_pem_len);
        dst->key_pem_len = src->key_pem_len;
    }
    return true;

fail:
    ssh_config_dispose(dst);
    return false;
}

void ssh_config_dispose(SSHConfig *cfg) {
    if (!cfg) return;
    /* Scrub credentials before releasing memory. */
    secure_zero(cfg->password,       sizeof cfg->password);
    secure_zero(cfg->key_passphrase, sizeof cfg->key_passphrase);
    secure_zero(cfg->jump_password,  sizeof cfg->jump_password);
    if (cfg->key_pem) {
        secure_zero(cfg->key_pem, cfg->key_pem_len);
        free(cfg->key_pem);
    }
    if (cfg->env) {
        for (i32 i = 0; i < cfg->env_count; i++) {
            secure_zero(cfg->env[i].value, sizeof cfg->env[i].value);
        }
        free(cfg->env);
    }
    free(cfg->stream_forwards);
    free(cfg->proxy_chain);
    free(cfg->local_forwards);
    free(cfg->remote_forwards);
    memset(cfg, 0, sizeof *cfg);
}

/* =========================================================================
 * X11 forwarding relay
 * ========================================================================= */

/* Connect to the local X11 display. Parses $DISPLAY to determine whether
 * to use a Unix socket (/tmp/.X11-unix/X<n>) or TCP (127.0.0.1:6000+n). */
static int x11_connect_display(void) {
    const char *display = getenv("DISPLAY");
    if (!display || !display[0]) return -1;

    int screen = 0;

    /* Unix socket path: $DISPLAY = :N or :N.S */
    if (display[0] == ':') {
        screen = atoi(display + 1);
        char sock_path[128];
        snprintf(sock_path, sizeof(sock_path), "/tmp/.X11-unix/X%d", screen);

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            /* Fall through to TCP */
        } else {
            net_set_nonblocking(fd);
            return fd;
        }
    }

    /* TCP: hostname:N or localhost:N -- connect to 127.0.0.1:6000+N */
    {
        const char *colon = strrchr(display, ':');
        if (colon) screen = atoi(colon + 1);
        int port = 6000 + screen;
        int fd = net_tcp_connect("127.0.0.1", port);
        if (fd >= 0) net_set_nonblocking(fd);
        return fd;
    }
}

void session_x11_poll(Session *s) {
    if (!s || s->type != SESSION_SSH || s->status != SESSION_CONNECTED) return;
    if (!s->ssh_session || !s->config || !s->config->forward_x11) return;

    /* Accept any pending X11 channels delivered via the callback */
    while (g_pending_x11_count > 0 && s->x11_relay_count < MAX_X11_CHANNELS) {
        LIBSSH2_CHANNEL *x11ch = g_pending_x11[--g_pending_x11_count];

        int local_fd = x11_connect_display();
        if (local_fd < 0) {
            libssh2_channel_close(x11ch);
            libssh2_channel_free(x11ch);
            continue;
        }

        if (!s->x11_relays) {
            s->x11_relays = calloc(MAX_X11_CHANNELS, sizeof(*s->x11_relays));
            if (!s->x11_relays) {
                libssh2_channel_close(x11ch);
                libssh2_channel_free(x11ch);
                close(local_fd);
                continue;
            }
        }
        X11Relay *relay = &s->x11_relays[s->x11_relay_count++];
        relay->channel = x11ch;
        relay->local_fd = local_fd;
        relay->active = true;
    }

    /* Relay data bidirectionally for all active X11 channels */
    if (!s->x11_relays) return;
    for (i32 i = 0; i < s->x11_relay_count; i++) {
        X11Relay *xr = &s->x11_relays[i];
        if (!xr->active) continue;

        char buf[8192];

        /* Remote X11 channel -> local display */
        ssize_t n = libssh2_channel_read(xr->channel, buf, sizeof(buf));
        if (n > 0) {
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t w = write(xr->local_fd, buf + sent, (size_t)(n - sent));
                if (w <= 0) break;
                sent += w;
            }
        } else if (n == 0 || (n < 0 && n != LIBSSH2_ERROR_EAGAIN)) {
            libssh2_channel_close(xr->channel);
            libssh2_channel_free(xr->channel);
            close(xr->local_fd);
            xr->active = false;
            continue;
        }

        /* Local display -> remote X11 channel */
        n = read(xr->local_fd, buf, sizeof(buf));
        if (n > 0) {
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t w = libssh2_channel_write(xr->channel, buf + sent, (size_t)(n - sent));
                /* session_x11_poll runs on the main UI thread every frame;
                 * sleeping here stalls keystroke echo. See identical note in
                 * port_forward.c/poll_connections. */
                if (w == LIBSSH2_ERROR_EAGAIN) continue;
                if (w < 0) break;
                sent += w;
            }
        } else if (n == 0) {
            libssh2_channel_close(xr->channel);
            libssh2_channel_free(xr->channel);
            close(xr->local_fd);
            xr->active = false;
        }
    }
}

/* =========================================================================
 * Session pool (ControlMaster)
 * ========================================================================= */

void session_pool_register(Session *s) {
    if (!s || s->type != SESSION_SSH) return;
    session_pool_init_once();

    pthread_mutex_lock(&g_session_pool.lock);
    /* Avoid duplicates */
    for (i32 i = 0; i < g_session_pool.count; i++) {
        if (g_session_pool.sessions[i] == s) {
            pthread_mutex_unlock(&g_session_pool.lock);
            return;
        }
    }
    if (g_session_pool.count < SESSION_POOL_MAX) {
        g_session_pool.sessions[g_session_pool.count++] = s;
    }
    pthread_mutex_unlock(&g_session_pool.lock);
}

void session_pool_unregister(Session *s) {
    if (!s || !g_session_pool.initialized) return;

    pthread_mutex_lock(&g_session_pool.lock);
    for (i32 i = 0; i < g_session_pool.count; i++) {
        if (g_session_pool.sessions[i] == s) {
            /* Swap with last and shrink */
            g_session_pool.sessions[i] = g_session_pool.sessions[--g_session_pool.count];
            break;
        }
    }
    pthread_mutex_unlock(&g_session_pool.lock);
}

/* =========================================================================
 * StreamLocal (Unix socket forwarding)
 * ========================================================================= */

#ifndef PLATFORM_WIN32

/* Context for a single StreamLocal relay thread */
typedef struct {
    int             listen_fd;      /* Unix domain listening socket */
    LIBSSH2_SESSION *ssh_session;
    char            remote_socket[512];
    bool            is_local_to_remote;
    volatile bool   *stop_flag;
} StreamLocalCtx;

/* Create a listening Unix domain socket at the given path.
 * Unlinks any existing socket file first. Returns fd or -1. */
static int create_unix_listener(const char *path) {
    struct sockaddr_un addr;
    if (strlen(path) >= sizeof(addr.sun_path)) return -1;

    /* Remove stale socket file */
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 4) < 0) {
        close(fd);
        unlink(path);
        return -1;
    }

    /* Non-blocking for the accept loop */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return fd;
}

/* Connect to a local Unix domain socket.  Returns fd or -1.
 * Used by remote->local StreamLocal forwarding (future). */
__attribute__((unused))
static int connect_unix_socket(const char *path) {
    struct sockaddr_un addr;
    if (strlen(path) >= sizeof(addr.sun_path)) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Bidirectional relay: local Unix socket fd <-> SSH streamlocal channel */
static void streamlocal_relay(int client_fd, LIBSSH2_CHANNEL *ch) {
    char buf[8192];

    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
    libssh2_channel_set_blocking(ch, 0);

    while (!libssh2_channel_eof(ch)) {
        bool did_work = false;

        /* Local -> SSH */
        ssize_t n = read(client_fd, buf, sizeof(buf));
        if (n > 0) {
            libssh2_channel_set_blocking(ch, 1);
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t w = libssh2_channel_write(ch, buf + sent, (size_t)(n - sent));
                if (w < 0) goto done;
                sent += w;
            }
            libssh2_channel_set_blocking(ch, 0);
            did_work = true;
        } else if (n == 0) {
            break;
        }

        /* SSH -> Local */
        ssize_t r = libssh2_channel_read(ch, buf, sizeof(buf));
        if (r > 0) {
            ssize_t sent = 0;
            while (sent < r) {
                ssize_t w = write(client_fd, buf + sent, (size_t)(r - sent));
                if (w <= 0) goto done;
                sent += w;
            }
            did_work = true;
        } else if (r == 0) {
            break;
        }

        if (!did_work) usleep(1000);
    }

done:
    libssh2_channel_close(ch);
    libssh2_channel_free(ch);
    close(client_fd);
}

/* Relay args for detached streamlocal relay threads */
typedef struct {
    int             client_fd;
    LIBSSH2_CHANNEL *channel;
} StreamLocalRelayArgs;

static void *streamlocal_relay_runner(void *arg) {
    StreamLocalRelayArgs *ra = (StreamLocalRelayArgs *)arg;
    streamlocal_relay(ra->client_fd, ra->channel);
    free(ra);
    return NULL;
}

/* Thread: listens on a local Unix socket, relays connections to a remote
 * Unix socket via libssh2_channel_direct_streamlocal_ex(). */
static void *streamlocal_listener_thread(void *arg) {
    StreamLocalCtx *ctx = (StreamLocalCtx *)arg;
    int listen_fd = ctx->listen_fd;

    while (!*ctx->stop_flag) {
        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 500); /* 500ms timeout for stop check */
        if (pr <= 0) continue;

        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) continue;

        if (ctx->is_local_to_remote) {
            /* Open a direct-streamlocal channel to the remote Unix socket */
            libssh2_session_set_blocking(ctx->ssh_session, 1);
            LIBSSH2_CHANNEL *ch = libssh2_channel_direct_streamlocal_ex(
                ctx->ssh_session, ctx->remote_socket, NULL, 0);
            libssh2_session_set_blocking(ctx->ssh_session, 0);

            if (!ch) {
                close(client_fd);
                continue;
            }

            /* Spawn a detached relay thread for this connection */
            StreamLocalRelayArgs *ra = malloc(sizeof(StreamLocalRelayArgs));
            if (!ra) {
                libssh2_channel_free(ch);
                close(client_fd);
                continue;
            }
            ra->client_fd = client_fd;
            ra->channel = ch;

            pthread_t tid;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&tid, &attr, streamlocal_relay_runner, ra) != 0) {
                free(ra);
                libssh2_channel_free(ch);
                close(client_fd);
            }
            pthread_attr_destroy(&attr);
        } else {
            /* remote->local: connect to local Unix socket and relay */
            /* This path is handled differently — see below */
            close(client_fd);
        }
    }

    /* Do NOT close(listen_fd) here. session_stop_stream_forwards is the single
     * owner of this fd: it sets *stop_flag=false AND close()s the fd to unblock
     * poll(). This loop only exits once stop_flag is false — i.e. only after the
     * stopper already closed the fd — so closing it again here was a double-close
     * that, under fd reuse, could silently close an unrelated descriptor. The fd
     * never leaks: the only exit path runs after the stopper's close(). */
    free(ctx);
    return NULL;
}

#endif /* !PLATFORM_WIN32 */

void session_start_stream_forwards(Session *s) {
    if (!s || s->type != SESSION_SSH || !s->config) return;
    if (s->status != SESSION_CONNECTED) return;
    if (s->config->stream_forward_count <= 0) return;
    if (s->stream_fwd_active) return;

    s->stream_fwd_active = true;

    for (i32 i = 0; i < s->config->stream_forward_count && i < 4; i++) {
        const StreamLocalForward *fwd = &s->config->stream_forwards[i];
        if (!fwd->local_socket[0] || !fwd->remote_socket[0]) continue;

        if (fwd->is_local_to_remote) {
            /* Listen on local Unix socket, forward to remote */
            int fd = create_unix_listener(fwd->local_socket);
            if (fd < 0) continue;
            s->stream_fwd_fds[i] = fd;

            StreamLocalCtx *ctx = malloc(sizeof(StreamLocalCtx));
            if (!ctx) { close(fd); s->stream_fwd_fds[i] = -1; continue; }
            ctx->listen_fd = fd;
            ctx->ssh_session = s->ssh_session;
            snprintf(ctx->remote_socket, sizeof(ctx->remote_socket), "%s",
                     fwd->remote_socket);
            ctx->is_local_to_remote = true;
            ctx->stop_flag = &s->stream_fwd_active; /* volatile bool */

            pthread_t tid;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&tid, &attr, streamlocal_listener_thread, ctx) != 0) {
                free(ctx);
                close(fd);
                s->stream_fwd_fds[i] = -1;
            }
            pthread_attr_destroy(&attr);
        }
        /* Remote->local forwarding requires libssh2_channel_forward_listen
         * on the remote side, which is more complex and currently not
         * implemented in this initial version. */
    }
}

void session_stop_stream_forwards(Session *s) {
    if (!s) return;
    s->stream_fwd_active = false;

#ifndef PLATFORM_WIN32
    for (i32 i = 0; i < 4; i++) {
        if (s->stream_fwd_fds[i] >= 0) {
            /* Close the listening socket — this unblocks the poll() in the
             * listener thread, which will then notice stop_flag and exit. */
            close(s->stream_fwd_fds[i]);
            s->stream_fwd_fds[i] = -1;
        }
    }
    s->stream_fwd_count = 0;
#endif
}

/* =========================================================================
 * Passphrase prompt API
 * ========================================================================= */

bool session_needs_passphrase(const Session *s) {
    if (!s || s->type != SESSION_SSH) return false;
    Session *sm = (Session *)s;
    pthread_mutex_lock(&sm->status_lock);
    bool pending = sm->passphrase_pending;
    pthread_mutex_unlock(&sm->status_lock);
    return pending;
}

const char *session_passphrase_key_path(const Session *s) {
    if (!s || s->type != SESSION_SSH) return "";
    return s->passphrase_prompt_path;
}

void session_supply_passphrase(Session *s, const char *passphrase) {
    if (!s || s->type != SESSION_SSH || !s->config) return;
    pthread_mutex_lock(&s->status_lock);
    secure_zero(s->config->key_passphrase, sizeof(s->config->key_passphrase));
    snprintf(s->config->key_passphrase, sizeof(s->config->key_passphrase),
             "%s", passphrase ? passphrase : "");
    s->passphrase_supplied = true;
    pthread_cond_signal(&s->passphrase_cond);
    pthread_mutex_unlock(&s->status_lock);
}

void session_cancel_passphrase(Session *s) {
    if (!s || s->type != SESSION_SSH) return;
    pthread_mutex_lock(&s->status_lock);
    s->passphrase_cancelled = true;
    pthread_cond_signal(&s->passphrase_cond);
    pthread_mutex_unlock(&s->status_lock);
}
