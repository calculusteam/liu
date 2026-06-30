/*
 * Liu - ~/.ssh/config parser
 * Parses OpenSSH-format config files with Host patterns, Match directives,
 * Include, ProxyJump chains, and token substitution.
 */
#ifndef SSH_CONFIG_H
#define SSH_CONFIG_H

#include "core/types.h"

/* Maximum number of hops in a ProxyJump chain */
#define SSH_CONFIG_MAX_PROXY_HOPS 8

/* Maximum number of identity files per host */
#define SSH_CONFIG_MAX_IDENTITY_FILES 8

/* Maximum number of local/remote forwards */
#define SSH_CONFIG_MAX_FORWARDS 8

/* =========================================================================
 * Port forward specification
 * ========================================================================= */

typedef struct {
    char bind_address[128];   /* local bind address (or empty for localhost) */
    i32  bind_port;
    char dest_host[256];      /* remote destination host */
    i32  dest_port;
} SSHForwardSpec;

/* =========================================================================
 * Proxy hop — one jump in a ProxyJump chain
 * ========================================================================= */

typedef struct {
    char hostname[256];
    i32  port;                /* 0 = default (22) */
    char username[128];       /* empty = inherit from resolved config */
    char identity_file[512];  /* empty = use default */
} SSHProxyHop;

/* =========================================================================
 * Resolved SSH config — all directives merged for a target hostname
 * ========================================================================= */

typedef struct {
    /* Matching host alias (the Host pattern that matched) */
    char match_host[256];

    /* Connection */
    char hostname[256];           /* resolved HostName (may differ from alias) */
    i32  port;                    /* resolved Port, 0 = use default 22 */
    char user[128];               /* resolved User */

    /* Authentication */
    char identity_files[SSH_CONFIG_MAX_IDENTITY_FILES][512];
    i32  identity_file_count;
    bool identity_only;           /* IdentitiesOnly yes */

    /* Proxy */
    char proxy_command[1024];     /* ProxyCommand (raw, tokens expanded) */
    SSHProxyHop proxy_hops[SSH_CONFIG_MAX_PROXY_HOPS];
    i32  proxy_hop_count;         /* 0 = direct connection */

    /* Agent forwarding */
    bool forward_agent;           /* ForwardAgent yes/no */
    bool add_keys_to_agent;       /* AddKeysToAgent yes/no */

    /* Keep-alive */
    i32  server_alive_interval;   /* ServerAliveInterval (seconds, 0 = off) */
    i32  server_alive_count_max;  /* ServerAliveCountMax */

    /* Forwarding */
    SSHForwardSpec local_forwards[SSH_CONFIG_MAX_FORWARDS];
    i32  local_forward_count;
    SSHForwardSpec remote_forwards[SSH_CONFIG_MAX_FORWARDS];
    i32  remote_forward_count;

    /* Misc */
    char log_level[32];           /* LogLevel */
    bool compression;             /* Compression yes/no */
    bool request_tty;             /* RequestTTY — true = yes/force */

    /* Flags to track which directives were explicitly set (first-match wins) */
    u32  set_flags;
} SSHResolvedConfig;

/* Bit flags for set_flags — tracks which directives have been set */
#define SSH_CFG_SET_HOSTNAME        (1u << 0)
#define SSH_CFG_SET_PORT            (1u << 1)
#define SSH_CFG_SET_USER            (1u << 2)
#define SSH_CFG_SET_PROXY_COMMAND   (1u << 3)
#define SSH_CFG_SET_PROXY_JUMP      (1u << 4)
#define SSH_CFG_SET_FORWARD_AGENT   (1u << 5)
#define SSH_CFG_SET_ALIVE_INTERVAL  (1u << 6)
#define SSH_CFG_SET_ALIVE_COUNT     (1u << 7)
#define SSH_CFG_SET_COMPRESSION     (1u << 8)
#define SSH_CFG_SET_REQUEST_TTY     (1u << 9)
#define SSH_CFG_SET_LOG_LEVEL       (1u << 10)
#define SSH_CFG_SET_IDENTITY_ONLY   (1u << 11)
#define SSH_CFG_SET_ADD_KEYS_AGENT  (1u << 12)

/* =========================================================================
 * API
 * ========================================================================= */

/*
 * Resolve SSH config for a given hostname.
 * Parses ~/.ssh/config (and any Include'd files), applies Host pattern
 * matching with first-match-wins semantics, expands tokens (%h, %p, %r),
 * and resolves ProxyJump chains recursively.
 *
 * Returns a heap-allocated SSHResolvedConfig, or NULL on failure.
 * Caller must free with ssh_config_free().
 */
SSHResolvedConfig *ssh_config_resolve(const char *hostname);

/*
 * Resolve SSH config from a specific config file path.
 * If config_path is NULL, uses ~/.ssh/config.
 */
SSHResolvedConfig *ssh_config_resolve_from(const char *hostname, const char *config_path);

/*
 * Free a resolved config returned by ssh_config_resolve().
 */
void ssh_config_free(SSHResolvedConfig *cfg);

/*
 * Apply resolved SSH config to an SSHConfig struct (used by ssh_session).
 * Fills in hostname, port, username, key_path, and proxy chain info.
 * Does not overwrite fields that are already set in `out` (non-empty string
 * or non-zero port) unless `force` is true.
 */
struct SSHConfig_tag;  /* forward decl — avoids circular include */
void ssh_config_apply(const SSHResolvedConfig *resolved, struct SSHConfig_tag *out, bool force);

#endif /* SSH_CONFIG_H */
