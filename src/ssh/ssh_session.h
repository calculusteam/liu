/*
 * Liu - SSH session manager (libssh2) + local PTY
 * Manages both local shell and remote SSH connections.
 */
#ifndef SSH_SESSION_H
#define SSH_SESSION_H

#include "core/types.h"

typedef enum {
    SESSION_LOCAL,       /* Local PTY (bash/zsh) */
    SESSION_SSH,         /* Remote SSH */
    SESSION_MOSH,        /* Mosh (UDP, wraps mosh-client) */
    SESSION_TELNET,      /* Raw telnet (RFC 854; server does in-band login) */
    SESSION_SERIAL,      /* RS-232 serial device */
} SessionType;

typedef enum {
    SESSION_DISCONNECTED = 0,
    SESSION_CONNECTING,
    SESSION_AUTHENTICATING,
    SESSION_CONNECTED,
    SESSION_ERROR,
    SESSION_KBI_PENDING,
} SessionStatus;

typedef enum {
    AUTH_PASSWORD,
    AUTH_PUBLICKEY,
    AUTH_AGENT,
    AUTH_GSSAPI,
} AuthMethod;

/* Unix socket / StreamLocal forwarding rule */
typedef struct {
    char local_socket[512];    /* local Unix socket path */
    char remote_socket[512];   /* remote Unix socket path */
    bool is_local_to_remote;   /* true: local->remote, false: remote->local */
} StreamLocalForward;

#define MAX_PROXY_HOPS 8
#define MAX_PORT_FORWARDS 16

typedef struct {
    char hostname[256];
    i32  port;
    char username[128];
    char key_path[512];
    AuthMethod auth_method;
} ProxyHop;

typedef struct {
    char bind_host[256];
    i32  bind_port;
    char dest_host[256];
    i32  dest_port;
} PortForwardSpec;

/* Per-env-var slot used in the dynamic env arrays below. Allocated lazily so
 * a typical SSH config that injects no env vars allocates zero bytes for
 * environment material. */
typedef struct {
    char name[64];
    char value[512];
} SSHEnvVar;

typedef struct SSHConfig_tag {
    char  hostname[256];
    i32   port;
    char  username[128];
    char  password[256];
    char  key_path[512];
    char  key_passphrase[256];
    char  jump_host[256];
    i32   jump_port;
    char  jump_username[128];
    char  jump_password[256];
    AuthMethod auth_method;
    bool  forward_x11;       /* Request X11 forwarding */

    /* GSSAPI/Kerberos */
    bool  gssapi_auth;              /* try GSSAPI auth methods */

    /* ControlMaster (connection multiplexing) */
    bool  control_master;           /* reuse existing SSH connections */
    char  control_path[512];        /* socket path pattern (unused for internal reuse) */

    /* StreamLocal (Unix socket forwarding). Dynamic — most SSH configs use
     * zero of these, so we don't pay the 4 KB per-config inline cost. */
    StreamLocalForward *stream_forwards;
    i32   stream_forward_count;
    i32   stream_forward_cap;

    /* Proxy chain for multi-hop connections (dynamic). */
    ProxyHop *proxy_chain;
    i32       proxy_chain_len;
    i32       proxy_chain_cap;

    /* Port forwarding config (dynamic). */
    PortForwardSpec *local_forwards;
    i32  local_forward_count;
    i32  local_forward_cap;
    PortForwardSpec *remote_forwards;
    i32  remote_forward_count;
    i32  remote_forward_cap;

    /* Smart Vault — secret references. When non-empty, the UI layer
     * reveals the secret from the vault just-in-time and populates the
     * plaintext buffers above (password / key_passphrase / a separate
     * decrypted PEM used by the in-memory auth path in M7). The worker
     * thread never touches the vault — it only reads the populated
     * buffers, so the thread stays DEK-unaware. */
    char password_secret_id[48];
    char passphrase_secret_id[48];
    char private_key_secret_id[48];

    /* In-memory private key material — populated from the vault when
     * private_key_secret_id is set. `key_pem_len` is the length in
     * bytes; `key_pem` is malloc'd by the reveal helper and freed (+
     * secure_zero'd) when the session is destroyed. */
    u8   *key_pem;
    usize key_pem_len;

    /* Environment variables to inject into the remote shell channel.
     * Dynamic — typical SSH configs inject zero, so we skip the 9 KB inline
     * cost. Names live in the same heap allocation as values for cache
     * locality during the libssh2_channel_setenv loop after auth. */
    SSHEnvVar *env;          /* heap [env_cap] when non-NULL */
    i32        env_count;
    i32        env_cap;
} SSHConfig;

/* Create a default (zeroed) SSHConfig — all dynamic arrays start NULL. */
SSHConfig ssh_config_default(void);

/* Deep-copy: clones every dynamic array so dst and src are fully independent.
 * The destination's existing dynamic arrays are freed (with key_pem secured)
 * before being replaced. Returns true on success; on alloc failure dst is
 * left zeroed and false is returned. */
bool ssh_config_clone(SSHConfig *dst, const SSHConfig *src);

/* Free all dynamic resources owned by `cfg` (arrays + key_pem) and zero the
 * struct. Sensitive fields are secure_zero'd. Safe to call on a zeroed cfg. */
void ssh_config_dispose(SSHConfig *cfg);

/* Append helpers — grow the corresponding dynamic array geometrically and
 * return the new slot's pointer. Caller fills it. NULL on alloc failure. */
StreamLocalForward *ssh_config_push_stream_forward(SSHConfig *cfg);
ProxyHop           *ssh_config_push_proxy_hop(SSHConfig *cfg);
PortForwardSpec    *ssh_config_push_local_forward(SSHConfig *cfg);
PortForwardSpec    *ssh_config_push_remote_forward(SSHConfig *cfg);
SSHEnvVar          *ssh_config_push_env(SSHConfig *cfg);

typedef struct Session Session;

/* Vault env-var shape — mirrors VaultEnvVar but keeps the SSH layer from
 * having to include vault.h. */
typedef struct {
    char name[64];
    char value[1024];
} LocalEnvVar;

/* =========================================================================
 * Session lifecycle
 * ========================================================================= */

/* Create a local terminal session (spawns default shell) */
Session *session_create_local(i32 cols, i32 rows);

/* Variant that injects additional environment variables into the child
 * process before exec. Used by Smart Vault to plumb globally-scoped
 * env vars into local shells. `names`/`values` may be NULL when
 * `count` == 0. The caller retains ownership and should zero the
 * values after this returns. `cwd` (NULL or "" = no preference) makes the
 * child chdir() to that directory before exec — used by split-pane creation
 * to inherit the active pane's current directory; falls back to the usual
 * Finder-launch HOME safety net if the chdir fails or no cwd was passed. */
Session *session_create_local_with_env(i32 cols, i32 rows,
                                       const LocalEnvVar *env, i32 env_count,
                                       const char *cwd);

/* Create a local PTY that runs `command` (instead of an interactive shell)
 * via the user's login shell: `shell -l -c "exec <command>"`. The login shell
 * gives the child the user's full PATH (so node/npm resolve even under a
 * Finder/Dock launch) and `exec` makes signals + the exit code map straight to
 * the command. Because forkpty() calls setsid(), the child leads its own
 * process group, so the caller can killpg(session_child_pid(s), sig) to
 * tree-kill the command and any workers it spawned. `env`/`env_count`/`cwd`
 * behave exactly as in session_create_local_with_env. Used by the Sites /
 * dev-server manager. */
Session *session_create_command(i32 cols, i32 rows,
                                const LocalEnvVar *env, i32 env_count,
                                const char *cwd, const char *command);

/* Create an SSH session */
Session *session_create_ssh(const SSHConfig *config, i32 cols, i32 rows);

/* Create a mosh session (wraps mosh-client via local PTY) */
Session *session_create_mosh(const SSHConfig *config, i32 cols, i32 rows);

#include "ssh/serial.h"   /* SerialConfig for session_create_serial */

/* Telnet: plain socket session; the server does its own in-band login. */
Session *session_create_telnet(const char *host, i32 port, i32 cols, i32 rows);

/* Serial: RS-232 device session (cfg->port = device path + cfg->baud_rate). */
Session *session_create_serial(const SerialConfig *cfg, i32 cols, i32 rows);

/* Destroy session and free resources */
void session_destroy(Session *s);

/* Suspend the backing child process (local PTY or mosh-client) via SIGSTOP.
 * Session struct, PTY fd, and child process are kept alive so the process
 * state (shell env, cwd, running programs) survives sleep/wake.
 * Returns true if the session was suspended, false if not supported or no-op. */
bool session_suspend(Session *s);

/* Resume a previously suspended session via SIGCONT. Returns true on success. */
bool session_resume(Session *s);

/* Send job-control suspend (SIGTSTP) to the local PTY foreground process
 * group. This mirrors what the terminal line discipline does for Ctrl+Z,
 * without marking the whole tab/session as suspended. */
bool session_send_tstp(Session *s);

/* Returns true if the session is currently suspended. */
bool session_is_suspended(const Session *s);

/* True when the PTY foreground process group is the shell itself (no
 * agent/editor/REPL is in front). Local-only. Used by agent-history
 * resume to decide whether to kill a foreground child before pasting
 * a new shell command. */
bool session_fg_is_shell(Session *s);

/* Send `sig` to the PTY foreground process group ONLY if that pgrp is
 * not the shell's own pgrp. Returns true when a signal was actually
 * delivered (i.e., an agent/REPL was running). Local-only. */
bool session_kill_fg(Session *s, int sig);

/* =========================================================================
 * I/O
 * ========================================================================= */

/* Read available output from session. Returns bytes read, 0 if nothing, -1 on error. */
i32 session_read(Session *s, u8 *buf, i32 buf_size);

/* Write input to session (keyboard data). Returns bytes written or -1. */
i32 session_write(Session *s, const u8 *data, i32 len);

/* Bytes waiting in the non-blocking write queue. Used by the main loop to
 * keep polling aggressively while SSH input is back-pressured. */
usize session_pending_write_bytes(const Session *s);

/* Resize PTY */
void session_resize(Session *s, i32 cols, i32 rows);

/* =========================================================================
 * Status
 * ========================================================================= */

SessionStatus session_status(const Session *s);
SessionType   session_type(const Session *s);
const char   *session_error(const Session *s);

/* Underlying TCP socket fd for SSH sessions (or -1 if not applicable / not
 * connected yet). Used by the main loop to wake on socket read-readiness
 * via platform_watch_socket, eliminating poll-cycle latency for typed
 * input echo. */
int           session_sock(const Session *s);

/* Generic read-path fd used by the main loop to register for read-ready
 * wake-ups. Returns the socket for SSH sessions and the PTY master fd for
 * local/mosh sessions. -1 if not ready yet. Both the SSH socket path and
 * local-PTY path benefit from wake-up — e.g. running `ssh remote` inside
 * a local terminal keeps the tty in raw mode, so every keystroke's echo
 * arrives at the PTY master and needs the same polling-delay elimination
 * that the SSH path got. */
int           session_io_fd(const Session *s);

/* True when this session exclusively owns its SSH socket / PTY master.
 * ControlMaster-style borrowed sessions alias the owner's fd and return
 * false here — the UI uses this to avoid cancelling the owner's watch
 * / closing the owner's fd when a borrowed tab goes away. Always true
 * for non-SSH sessions. */
bool          session_is_shared_owner(const Session *s);

/* =========================================================================
 * SSH-specific
 * ========================================================================= */

/* Get the SFTP sub-channel for file operations (only for SSH sessions).
 * The handle is initialized by the connect worker before SESSION_CONNECTED
 * is published, so this call is a plain read — no blocking I/O. Returns
 * NULL if the server does not support SFTP or the session is not connected. */
void *session_get_sftp(Session *s);

/* Alias for session_get_sftp with clearer naming — never triggers I/O. */
void *session_sftp_handle(const Session *s);

/* Remote home directory, resolved by the connect worker via
 * libssh2_sftp_realpath("."). Returns "" when SFTP is unavailable or
 * realpath failed — never NULL. */
const char *session_initial_cwd(const Session *s);

/* Flip the underlying LIBSSH2_SESSION to blocking mode for the duration
 * of a main-thread SFTP call (directory listing, etc). Mirrors the
 * blocking/non-blocking state around the call. Use in pairs. No-op on
 * non-SSH sessions or when the handle is NULL. */
void session_sftp_scope_begin(Session *s);
void session_sftp_scope_end(Session *s);

/* Main-thread pause hook for long-running SFTP workers.
 * Worker threads that call libssh2_sftp_* on `s` must flip this to true
 * while they hold the session in blocking mode (via scope_begin). The
 * main loop's app_poll_sessions checks session_io_is_suspended() and
 * skips that session's channel_read until the worker clears the flag;
 * otherwise the main thread blocks inside libssh2 and the UI freezes
 * for the duration of the transfer. */
bool session_io_is_suspended(const Session *s);
void session_io_set_suspended(Session *s, bool v);

/* Check if session is still alive */
bool session_is_alive(Session *s);

/* Get foreground process name (local PTY only). Returns empty string on error. */
const char *session_fg_process(Session *s);

/* Drop the foreground-process TTL cache so the next session_fg_process()
 * scans live (used at close time to avoid a stale-window miss). */
void session_invalidate_fg_cache(Session *s);

/* Get child PID (local PTY only) */
i32 session_child_pid(Session *s);

/* True once the local child has exited and been reaped by session_is_alive().
 * When it returns true, *code_out (if non-NULL) is the exit code, or the
 * negated signal number if the child was killed by a signal. Lets the Sites
 * manager distinguish a clean exit from a crash without a second waitpid. */
bool session_exited(const Session *s, int *code_out);

/* Get the current working directory of the local PTY's foreground process
 * group (falls back to the shell child if no fg pgrp is set). Used as a
 * fallback for agent-history's cwd filter when the shell hasn't emitted
 * OSC 7. Returns an empty string if unavailable; pointer is into a static
 * buffer overwritten on each call. */
const char *session_local_cwd(Session *s);

/* Auto-reconnect for SSH sessions */
bool session_reconnect(Session *s);

/* Get SSH config (for reconnect) */
const SSHConfig *session_get_config(const Session *s);

/* Poll X11 forwarded connections -- call from main loop for SSH sessions */
void session_x11_poll(Session *s);

/* =========================================================================
 * Keyboard-interactive (2FA) dialog
 * ========================================================================= */

#define KBI_MAX_PROMPTS 8

bool session_kbi_pending(Session *s);
i32  session_kbi_num_prompts(Session *s);
const char *session_kbi_name(Session *s);
const char *session_kbi_instruction(Session *s);
const char *session_kbi_prompt(Session *s, i32 index);
bool session_kbi_echo(Session *s, i32 index);
void session_kbi_submit(Session *s, const char **responses, i32 count);

/* =========================================================================
 * Port forwarding
 * ========================================================================= */

typedef struct {
    char  bind_host[256];
    i32   bind_port;
    char  remote_host[256];
    i32   remote_port;
    i32   local_port;
    bool  active;
    i32   type;  /* 0=local, 1=dynamic */
} PortForwardInfo;

typedef PortForwardInfo LocalForwardInfo;

bool session_local_forward_start(Session *s, i32 local_port,
                                  const char *remote_host, i32 remote_port);
bool session_dynamic_forward_start(Session *s, i32 local_port);
void session_local_forward_poll(Session *s);
i32  session_local_forward_count(Session *s);
bool session_local_forward_get(Session *s, i32 index, PortForwardInfo *info);
void session_local_forward_remove(Session *s, i32 index);
void session_remote_forward_poll(Session *s);
void session_keepalive_check(Session *s, f64 now_sec);

/* =========================================================================
 * Host key dialog (GUI ← worker thread sync)
 * ========================================================================= */

/* Check if a session is waiting for user to accept/reject a host key */
bool session_hostkey_pending(Session *s);

/* Get info about the pending host key dialog */
void session_hostkey_get_info(Session *s, bool *is_change,
                              char *hostname, usize hostname_sz,
                              i32 *port,
                              char *old_fp, usize old_fp_sz,
                              char *new_fp, usize new_fp_sz);

/* Respond to a pending host key dialog (accept=true or reject=false) */
void session_hostkey_respond(Session *s, bool accept);

/* =========================================================================
 * Connection multiplexing (ControlMaster)
 * ========================================================================= */

/* Register a connected session for potential reuse by later connections.
 * Must be called from the main thread after SESSION_CONNECTED. */
void session_pool_register(Session *s);

/* Unregister a session from the reuse pool (called by session_destroy). */
void session_pool_unregister(Session *s);

/* =========================================================================
 * StreamLocal (Unix socket forwarding)
 * ========================================================================= */

/* Start Unix socket forwarding for the given session's config. */
void session_start_stream_forwards(Session *s);

/* Stop all Unix socket forwards for a session. */
void session_stop_stream_forwards(Session *s);

/* =========================================================================
 * Passphrase prompt (worker↔UI sync)
 * ========================================================================= */

/* Check if session is waiting for a passphrase from the UI */
bool session_needs_passphrase(const Session *s);

/* Get the key path that needs a passphrase */
const char *session_passphrase_key_path(const Session *s);

/* Supply the passphrase from the UI and wake the connect worker to retry */
void session_supply_passphrase(Session *s, const char *passphrase);

/* Cancel the passphrase prompt (user pressed Cancel/Escape) */
void session_cancel_passphrase(Session *s);

#endif /* SSH_SESSION_H */
