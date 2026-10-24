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
    bool  forward_agent;     /* Request agent forwarding (ForwardAgent yes).
                              * OpenSSH's default is NO, and so is ours: the
                              * remote root can use every key in the agent. */
    bool  identities_only;   /* IdentitiesOnly yes: the named key and nothing
                              * the agent happens to hold */
    bool  agent_before_key;  /* AUTH_PUBLICKEY from a key FILE: offer the
                              * identities the agent holds first, as ssh does.
                              * A protected key that is loaded there then needs
                              * no passphrase from us. Set by the SSH form's
                              * key login; never for a key from the vault. */

    /* GSSAPI/Kerberos */
    bool  gssapi_auth;              /* try GSSAPI auth methods */

    /* ControlMaster (connection multiplexing) */
    bool  control_master;           /* reuse existing SSH connections */
    char  control_path[512];        /* socket path pattern (unused for internal reuse) */

    /* StreamLocal (Unix socket forwarding). Dynamic: most SSH configs use
     * none, so the 4 KB per-config inline cost is not paid. */
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
     * Dynamic: typical SSH configs inject none, so the 9 KB inline cost is
     * skipped. Names live in the same heap allocation as values for cache
     * locality during the libssh2_channel_setenv loop after auth. */
    SSHEnvVar *env;          /* heap [env_cap] when non-NULL */
    i32        env_count;
    i32        env_cap;

    /* The folder the remote shell starts in ("" = the account's home). Set
     * when a pane is split and its shell had reported where it was (OSC 7);
     * ssh_start_shell honours it only through ssh_start_dir_ok(). */
    char       start_dir[512];

    /* A file-transfer connection (the SFTP tab): authenticate, open SFTP, and
     * open NO shell channel — no PTY, shell, environment, X11, agent or
     * remote forward is requested. An account whose server allows SFTP only
     * (`ForceCommand internal-sftp`, `PermitTTY no`) refuses the PTY a shell
     * needs, so asking for one would fail the connect. Without SFTP the
     * connect fails. Never shared with another session (ControlMaster): a
     * terminal that borrowed it would still want a shell. A reconnect keeps
     * it, because it lives in the config the session keeps. */
    bool       sftp_only;
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

/* What every local pane's environment carries besides the terminal's own
 * announcement. Process-wide, set by the app from its config before the first
 * shell is started and again when the config changes; a shell already running
 * keeps what it was given. Main thread.
 *
 *   shell integration: the folder core/shell_integration.h wrote ("" or NULL
 *     = none). A zsh is then started with ZDOTDIR pointing into it.
 *   term: TERM for the pane. "xterm-liu" needs `terminfo_dir` (the folder
 *     that holds the compiled entry, exported as TERMINFO); without one, or
 *     with any other name, panes get "xterm-256color". */
void session_local_set_shell_integration(const char *dir);
void session_local_set_term(const char *term, const char *terminfo_dir);
/* The locale a pane's programs start in (ssh/local_lang.h): `asked` is the
 * config's "lang" ("" = Liu decides, "system", or a locale's name) and
 * `system_locale` the system's own as language_COUNTRY ("" when not known).
 * session_local_lang() is what LANG will be set to — "" when the environment
 * Liu was started in already names a locale and is left alone. */
void session_local_set_lang(const char *asked, const char *system_locale);
const char *session_local_lang(void);

/* The config's "ssh_legacy_algorithms": also offer a server the key exchanges,
 * host-key signatures, ciphers and MACs OpenSSH stopped offering
 * (ssh/ssh_algs.h). Off by default; applies to the next connect. */
void session_ssh_set_legacy_algorithms(bool allow);

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

/* Begin the existing local-child shutdown grace period before handing an
 * exclusively owned Session to deferred destruction. Irreversible: no more
 * I/O or liveness polling may follow; session_destroy must release the owner.
 * Other session types, shared references and nonstandard SIGCHLD policies
 * keep their ordinary teardown. Repeating an accepted request is harmless. */
bool session_prepare_destroy(Session *s);

/* Hold a Session across an asynchronous operation. The owning UI reference is
 * released by session_destroy(); each successful retain must be paired with
 * session_release(). Final teardown happens after the last reference. */
bool session_retain(Session *s);
void session_release(Session *s);
/* Immutable identity assigned at construction, never inferred from its
 * allocation address. Compare only after finding a live Session owner. */
u64 session_instance_id(const Session *s);
/* The key a local pane's shell puts on its OSC 133 marks ("" when there is
 * none). The terminal believes a mark for the command history only when it
 * carries this (terminal.h, shell_mark_key). */
const char *session_shell_mark_key(const Session *s);

/* Suspend the backing child process (local PTY or mosh-client) via SIGSTOP.
 * Session struct, PTY fd, and child process are kept alive so the process
 * state (shell env, cwd, running programs) survives sleep/wake.
 * Returns true if the session was suspended, false if not supported or no-op. */
bool session_suspend(Session *s);

/* Stop a local producer and deliver every byte already queued in its PTY to
 * `consume` before returning. This is the sleep/snapshot handoff: success
 * means no later PTY read can belong to the pre-suspend Terminal state.
 * Failure resumes the session and leaves the caller's Terminal live. */
typedef void (*SessionOutputConsumeFn)(void *ctx, const u8 *data, usize len);
bool session_suspend_and_drain(Session *s, SessionOutputConsumeFn consume,
                               void *ctx);

/* Resume a previously suspended session via SIGCONT. Returns true on success. */
bool session_resume(Session *s);

/* Send job-control suspend (SIGTSTP) to the local PTY foreground process
 * group. This mirrors what the terminal line discipline does for Ctrl+Z,
 * without marking the whole tab/session as suspended. */
bool session_send_tstp(Session *s);

/* Returns true if the session is currently suspended. */
bool session_is_suspended(const Session *s);

/* Bounded wait for a just-suspended LOCAL child to actually REACH the stopped
 * state. killpg() returns when the signal has been posted, not when the target
 * has stopped: on a multicore machine the child can still complete a write()
 * that was already in the kernel. Anything that suspends a session in order to
 * declare "nothing more will be produced" — tab sleep — has to close that
 * window, or a barrier taken straight after the signal can observe an empty
 * queue microseconds before the last write lands.
 *
 * The observation is a read-only process-state query (kinfo_proc.p_stat on
 * macOS, /proc/<pid>/stat on Linux), NOT waitpid(): session_is_alive() is the
 * only reaper in this tree by design, and consuming a wait status here would
 * race it to ECHILD and eat the exit code Sites reports.
 *
 * Returns true once the child is observed stopped — or exited, which cannot
 * produce another byte either — and immediately for a session with no local
 * child of ours to observe: a MOSH session's stoppable pid belongs to
 * mosh_client, and SSH/telnet/serial have none at all. A query that FAILS is
 * treated as "not yet" rather than as success, so an unobservable child ends in
 * a timeout and the caller declines to sleep. False only on timeout.
 *
 * What it proves is bounded. It observes the SHELL — s->child_pid, the group
 * leader. session_suspend() signals the whole group, and the other members are
 * not observed; under the sleep policy the shell is the foreground process
 * group, so at sleep time there are normally no others. A job the shell left
 * running in its OWN group is neither signalled by session_suspend() nor
 * covered here. */
bool session_wait_suspended(Session *s, i32 timeout_ms);
/* Post the same guarded SIGSTOP as session_suspend, retaining the Session
 * for rollback/resume, but never waitpid() for its acknowledgement. Call
 * session_wait_suspended(...,0) on later turns before requesting a barrier. */
bool session_suspend_begin(Session *s);

/* ---- producer quiescence -----------------------------------------------
 *
 * session_read_pending() == 0 is not proof that a producer has finished
 * handing over. It is 0 for a gather thread blocked inside read() about to
 * return 8 KB, for one that has copied bytes into the ring but not yet
 * published them, and for one parked in poll() over a kernel tty queue that
 * still holds a screenful — all of them the normal state of a quiet pane.
 *
 * begin() takes a barrier generation; wait() reports whether it has been met,
 * blocking up to timeout_ms and returning as soon as it is. A timeout of 0 is
 * a pure poll. The caller MUST keep draining with session_read() between
 * waits: a full ring parks the gather thread where it cannot reach the empty
 * read that satisfies the barrier.
 *
 * Sessions with no gather thread (LIU_PTY_THREAD=0, SSH, telnet, serial) are
 * already quiescent by construction — session_read() goes straight to the
 * kernel, so a return of 0 IS the empty queue — and both calls are no-ops that
 * report success. MOSH is the one gap: its bytes come off a network protocol,
 * so a drain that comes back empty proves only that nothing has arrived yet.
 * See src/ui/sleep_flush.h for what that means for sleep. */
u32  session_quiesce_begin(Session *s);
bool session_quiesce_wait(Session *s, u32 token, i32 timeout_ms);

/* True when the PTY foreground process group is the shell itself (no
 * agent/editor/REPL is in front). Local-only. Used by agent-history
 * resume to decide whether to kill a foreground child before pasting
 * a new shell command. */
bool session_fg_is_shell(Session *s);

typedef enum {
    SESSION_FG_PROBE_PENDING = 0,
    SESSION_FG_PROBE_SHELL,
    SESSION_FG_PROBE_CHILD,
    SESSION_FG_PROBE_UNAVAILABLE,
} SessionFgProbeResult;

/* Pending resume must request AFTER signaling the old foreground child, and
 * paste only after a fresh SHELL result. A CHILD result requires a new probe
 * to observe the later return to the shell. Local sessions without a reader
 * report UNAVAILABLE instead of making a tty ioctl on the UI thread. */
u32 session_fg_probe_begin(Session *s);
SessionFgProbeResult session_fg_probe_poll(Session *s, u32 token);

/* Conservative local-tab sleep gate. True only when the shell is the sole
 * live job-control process group attached to its PTY. A foreground-pgid check
 * alone misses `server &`: the shell is foreground again while the server's
 * separate background group remains able to fill and wedge on the PTY. */
bool session_local_sleep_is_safe(Session *s);

#ifdef LIU_AGENT_ARGV_PROBE
/* Read process arguments through KERN_PROCARGS2 on macOS and return
 * timing and allocation counters. */
bool session_argv_probe_for_bench(i32 pid, char *out, usize out_cap);
void session_argv_probe_stats(u64 *probe_count, u64 *probe_ns,
                              u64 *probe_max_ns, u64 *argmax_count,
                              u64 *procargs_count, u64 *malloc_bytes);
#endif

/* Send `sig` to the PTY foreground process group ONLY if that pgrp is
 * not the shell's own pgrp. Returns true when a signal was actually
 * delivered (i.e., an agent/REPL was running). Local-only. */
bool session_kill_fg(Session *s, int sig);

/* =========================================================================
 * I/O
 * ========================================================================= */

/* Read available output from session. Returns bytes read, 0 if nothing, -1 on error. */
i32 session_read(Session *s, u8 *buf, i32 buf_size);

/* Bytes already lifted off the PTY by this session's reader thread and not yet
 * handed out by session_read(). Zero for every session that has no reader
 * thread: SSH, mosh, telnet, serial, and local when the gather thread is off.
 * A drain loop that stopped on its byte budget uses this to decide whether it
 * left a backlog behind (see core/pty_reader.h's backlog latch). */
i32 session_read_pending(const Session *s);
/* The descriptor the main loop should hold a read watch on, or -1: the session
 * is CONNECTED, owns its fd, is not read by its own thread, and its fd has not
 * already reported the end. Every place that registers a watch asks this —
 * registering from the raw status re-armed the watch on a dead telnet / mosh /
 * serial fd on every poll (their status stays CONNECTED after the peer goes). */
int session_watch_fd(const Session *s);
/* True when a pty_reader thread owns the fd, so session_read copies from a
 * ring instead of reading the kernel. */
bool session_has_reader(const Session *s);
/* After the main thread has parsed everything it took from this session: the
 * parser is at rest (`ok`) and a cursor report would say `row`, `col` (1-based,
 * terminal_reply_position). Lets the pty reader answer a chunk of questions by
 * itself (core/pty_reader.h); nothing is published while input is still owed
 * to the program. Local sessions with a reader only; a no-op otherwise. */
void session_reply_publish(Session *s, bool ok, i32 row, i32 col);
/* How many chunks of questions the pty reader answered by itself. */
u32  session_fast_replies(const Session *s);

/* Write input to session (keyboard data). Returns bytes written or -1. */
i32 session_write(Session *s, const u8 *data, i32 len);

/* A large input — a paste — fed to the session in bounded pieces.
 *
 * session_write copies its bytes into the session's write queue, which is
 * capped at SESSION_WRITE_BUFFER_LIMIT (4 MB) so a slow receiver cannot make
 * the process hold an unbounded backlog; past the cap it REFUSES, and the
 * caller of a paste has nowhere to put the refusal, so part of a large paste
 * into a program that is not reading would be lost silently. A source is one
 * copy of the whole text, reference-counted so a broadcast paste to N panes
 * holds it once, and each session pulls from it into its queue as the queue
 * drains. Order is preserved: anything written to the session while a source
 * is pending goes BEHIND it, so a bracketed-paste terminator or a keystroke
 * typed during the paste lands where it was sent. The queue itself stays
 * small while a source drains (SESSION_WRITE_PULL_LOW), which also makes the
 * peak memory of a paste one copy rather than two.
 *
 * `create` copies `data` (refcount 1); `release` drops the caller's reference
 * — the session holds its own from `session_write_source` until the last byte
 * is queued, or the session is destroyed or its transport reset. */
typedef struct SessionInputSource SessionInputSource;
SessionInputSource *session_input_source_create(const u8 *data, usize len);
void                session_input_source_release(SessionInputSource *src);
/* Queue src[off, off+len) behind everything already queued. Returns false
 * (nothing queued, no reference taken) when the session is not connected,
 * the range is out of bounds, or the session's own bookkeeping cannot grow. */
bool session_write_source(Session *s, SessionInputSource *src, usize off, usize len);
/* UI-thread serial for invalidating asynchronous edits after any input. */
u64 session_input_serial(const Session *s);
/* Queue a complete local editor operation before sending any bytes. Existing
 * queued input stays ahead of it. False leaves the PTY and queue untouched
 * (disconnected/invalid input/allocation failure).
 * True means accepted for delivery, not acknowledged by the child process. */
bool session_write_local_batch(Session *s, const u8 *data, i32 len);

/* Bytes waiting in the non-blocking write queue. Used by the main loop to
 * keep polling aggressively while SSH input is back-pressured. */
usize session_pending_write_bytes(const Session *s);
/* The part of that the MAIN LOOP still has to push (its own queue); the rest
 * is with the pty's reader thread, which needs no help. Use this one to decide
 * whether to keep ticking for a write. */
usize session_queued_write_bytes(const Session *s);

/* Resize PTY */
void session_resize(Session *s, i32 cols, i32 rows);
/* The pane's cell in pixels, sent with every window size from then on. */
void session_set_cell_pixels(Session *s, i32 w, i32 h);

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

/* Worker scope for nonblocking SFTP. The operation lock stays held across
 * EAGAIN while io_lock is released for the shell. Between completed calls,
 * sftp_io_breathe hands both locks to waiting work. Nested scopes on the same
 * connection are counted; nesting across different connections is unsupported.
 * Use begin/end in pairs and retain the Session for the worker's lifetime. */
void session_sftp_scope_begin(Session *s);
void session_sftp_scope_end(Session *s);

/* Counted reconnect/lifetime gate for active SFTP work. It does not stop shell
 * polling: session_read uses a nonblocking trylock while SFTP waits for I/O. */
bool session_io_is_suspended(const Session *s);
void session_io_set_suspended(Session *s, bool v);

/* Check if session is still alive */
bool session_is_alive(Session *s);
/* A file-transfer connection (SSHConfig.sftp_only): SFTP and no shell. It has
 * nothing to read and refuses input (session_write answers -1, unharmed). */
bool session_is_sftp_only(const Session *s);

/* Get foreground process name (local PTY only). Returns empty string on error. */
const char *session_fg_process(Session *s);
/* Keystroke-safe lookup: never scans processes or argv. Empty when the cached
 * answer is expired or no longer belongs to the current foreground group. */
const char *session_fg_process_cached(Session *s);

/* Foreground process-group leader pid on the tab's PTY (local PTY only).
 * -1 when not applicable. One tcgetpgrp ioctl — cheap. */
i32 session_fg_pid(Session *s);

/* Is the pane's program reading a password — the tty has echo off and is in
 * line mode (getpass, sudo, ssh, `read -s`)? Local PTYs only; false for a
 * remote session, whose tty is not on this machine. Never blocks: the reader
 * thread samples it. */
bool session_password_prompt(Session *s);

/* Process-group id of the local shell this session started, i.e. what
 * session_fg_pid() reads back while nothing is running in the foreground.
 * -1 when not applicable (remote sessions, or no child). One getpgid — cheap.
 * Compare the two to answer "is a job running in this pane?"; the comparison
 * itself lives in ui/sleep_policy.h so it can be tested without a pty. */
i32 session_shell_pgid(Session *s);

/* How many LIVE processes share this pane's controlling terminal but sit
 * OUTSIDE the shell's own process group — the jobs a shell has left running
 * behind its prompt. 0 for a clean prompt and for every non-local session,
 * and -1 when the query itself failed.
 *
 * This is the second half of the tab-sleep predicate and it is not derivable
 * from the first: session_suspend() signals the SHELL's process group, an
 * interactive shell puts each job in its own group, so a job left running
 * behind the prompt is NOT stopped by a sleep — it keeps writing into a pty
 * nobody drains and wedges in write(). See ui/sleep_policy.h.
 *
 * Costs one proc_pidinfo plus one kernel-filtered sysctl on macOS, and one
 * /proc walk on Linux. Main thread; the callers are the 30-second idle-sleep
 * scan and the explicit "Sleep This Tab Now", never the render or poll path. */
i32 session_background_jobs(Session *s);

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

/* How long the local child lived, in seconds, once it has exited and been
 * reaped; -1 before that. */
f64 session_lived_sec(const Session *s);
/* The pane has said its shell is gone and is waiting for Enter to close
 * (ui/shell_exit.h: a shell that died as it started is not closed unseen). */
bool session_exit_held(const Session *s);
void session_set_exit_held(Session *s, bool held);

/* Get the current working directory of the local PTY's foreground process
 * group (falls back to the shell child if no fg pgrp is set). Used as a
 * fallback for agent-history's cwd filter when the shell hasn't emitted
 * OSC 7. Returns an empty string if unavailable; pointer is into a static
 * buffer overwritten on each call. */
const char *session_local_cwd(Session *s);

/* Auto-reconnect for SSH sessions */
bool session_reconnect(Session *s);
/* The same, with the attempt held back `delay_ms` first. The wait is spent on
 * the connect worker (cancellable), so the caller needs no timer: the status
 * is SESSION_CONNECTING from now and the loop is woken when it ends. */
bool session_reconnect_after(Session *s, i32 delay_ms);

/* What an SSH pane's connection just did, for whoever draws the pane to say
 * (ssh/link_policy.h has the rules; values match its LinkSay). Call once per
 * session poll, on the main thread: it also ACTS — a remote shell that exited
 * is marked ended, a lost link gets its next attempt started.
 *   retry_in_ms  with SESSION_LINK_LOST: how long until that attempt begins
 *   attempt      with SESSION_LINK_LOST: which automatic attempt it is (1-based) */
typedef enum {
    SESSION_LINK_NOTHING = 0,
    SESSION_LINK_CONNECTING,
    SESSION_LINK_UP,
    SESSION_LINK_RECONNECTED,
    SESSION_LINK_ENDED,
    SESSION_LINK_LOST,
    SESSION_LINK_FAILED,
    SESSION_LINK_GAVE_UP,
} SessionLinkEvent;
SessionLinkEvent session_link_step(Session *s, i32 *retry_in_ms, i32 *attempt);
/* Why the link last went down (session_error() is cleared by the reconnect). */
const char *session_link_reason(const Session *s);
/* The pane is ended or down and nothing is in flight: Enter connects again. */
bool session_link_wants_enter(const Session *s);
bool session_link_retry_now(Session *s);

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
/* "user@host" of whoever sent the prompt (a jump host or the destination). */
const char *session_kbi_peer(Session *s);
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
/* Returns true when the relay stopped on its per-call budget with work left,
 * so the loop should poll again soon (SSH latency S05). */
bool session_local_forward_poll(Session *s);
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

/* Report cumulative full PROC_ALL_PIDS sweep count and elapsed time. This
 * is available on macOS and is a no-op elsewhere. */
void session_proc_scan_stats(u64 *out_sweeps, f64 *out_seconds);
