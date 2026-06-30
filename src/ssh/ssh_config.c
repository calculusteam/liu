/*
 * Liu - ~/.ssh/config parser implementation
 *
 * Parses OpenSSH config files with:
 * - Host patterns (wildcards *, ?)
 * - Match host directives
 * - Include directive (relative/absolute paths, glob)
 * - Key directives: HostName, User, Port, IdentityFile, ProxyJump,
 *   ProxyCommand, ForwardAgent, ServerAliveInterval/CountMax,
 *   LocalForward, RemoteForward, AddKeysToAgent
 * - First-match-wins per-directive (OpenSSH semantics)
 * - Tilde expansion (~/) for paths
 * - Token substitution: %h, %p, %r in HostName, ProxyCommand, etc.
 */
#include "ssh/ssh_config.h"
#include "ssh/ssh_session.h"
#include "core/types.h"
#include "core/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strcasecmp */
#include <ctype.h>
#ifndef PLATFORM_WIN32
#include <glob.h>
#endif
#include <sys/stat.h>

/* =========================================================================
 * Internal types
 * ========================================================================= */

/* A single directive from the config file */
typedef struct ConfigDirective {
    char key[64];
    char value[1024];
} ConfigDirective;

/* A Host/Match block with its directives */
typedef struct ConfigBlock {
    /* Host patterns (space-separated), e.g. "*.example.com bastion" */
    char patterns[512];
    bool is_match;           /* true = Match directive, false = Host */
    char match_criteria[512]; /* for Match: "host *.example.com" etc. */

    ConfigDirective directives[64];
    i32 directive_count;

    struct ConfigBlock *next;
} ConfigBlock;

/* Parser state */
typedef struct {
    ConfigBlock *blocks;     /* linked list of parsed blocks */
    ConfigBlock *tail;
    i32 include_depth;       /* prevent infinite recursion */
} ConfigParser;

#define MAX_INCLUDE_DEPTH 16

/* =========================================================================
 * String helpers
 * ========================================================================= */

static void str_trim_inplace(char *s) {
    /* Trim leading whitespace */
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    /* Trim trailing whitespace */
    usize len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

/* Expand ~ to home directory in a path */
static void expand_tilde(const char *input, char *output, usize output_size) {
    if (input[0] == '~' && (input[1] == '/' || input[1] == '\0')) {
        const char *home = net_home_dir();
        snprintf(output, output_size, "%s%s", home, input + 1);
    } else {
        snprintf(output, output_size, "%s", input);
    }
}

/* Token substitution: %h=hostname, %p=port, %r=user */
static void expand_tokens(const char *input, char *output, usize output_size,
                          const char *hostname, i32 port, const char *user) {
    if (output_size == 0) return;
    usize out_i = 0;
    for (usize i = 0; input[i] && out_i < output_size - 1; i++) {
        if (input[i] == '%' && input[i + 1]) {
            const char *sub = NULL;
            char portbuf[16];
            switch (input[i + 1]) {
            case 'h': sub = hostname ? hostname : ""; break;
            case 'p':
                snprintf(portbuf, sizeof portbuf, "%d", (port > 0) ? port : 22);
                sub = portbuf;
                break;
            case 'r': sub = user ? user : ""; break;
            case '%': output[out_i++] = '%'; i++; continue;
            default: break;
            }
            if (sub) {
                /* snprintf returns the would-have-written length; clamping to
                 * what actually fit keeps out_i <= output_size-1 so the final
                 * NUL (and output_size - out_i below) never go out of bounds. */
                usize avail = output_size - out_i;        /* >= 1 here */
                int w = snprintf(output + out_i, avail, "%s", sub);
                out_i += (w < 0) ? 0 : ((usize)w < avail ? (usize)w : avail - 1);
                i++;
                continue;
            }
        }
        output[out_i++] = input[i];
    }
    output[out_i] = '\0';
}

/* Simple glob/wildcard pattern matching for Host patterns.
 * Supports * (any chars) and ? (single char). */
static bool pattern_match(const char *pattern, const char *text) {
    while (*pattern && *text) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return true;  /* trailing * matches all */
            /* Try matching rest from every position in text */
            for (const char *t = text; *t; t++) {
                if (pattern_match(pattern, t)) return true;
            }
            return pattern_match(pattern, text + strlen(text));
        } else if (*pattern == '?') {
            pattern++;
            text++;
        } else {
            if (tolower((unsigned char)*pattern) != tolower((unsigned char)*text))
                return false;
            pattern++;
            text++;
        }
    }
    /* Consume trailing *'s */
    while (*pattern == '*') pattern++;
    return (*pattern == '\0' && *text == '\0');
}

/* Check if a hostname matches a space-separated list of patterns.
 * A pattern prefixed with ! is a negation. */
static bool host_matches_patterns(const char *hostname, const char *patterns) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", patterns);

    bool matched = false;
    bool negated = false;
    char *saveptr = NULL;
    char *token = strtok_r(buf, " \t", &saveptr);

    while (token) {
        bool neg = false;
        const char *pat = token;
        if (pat[0] == '!') {
            neg = true;
            pat++;
        }
        if (pattern_match(pat, hostname)) {
            if (neg) {
                negated = true;
            } else {
                matched = true;
            }
        }
        token = strtok_r(NULL, " \t", &saveptr);
    }

    return matched && !negated;
}

/* =========================================================================
 * Parsing
 * ========================================================================= */

static ConfigBlock *block_create(void) {
    ConfigBlock *b = calloc(1, sizeof(ConfigBlock));
    return b;
}

static void block_add_directive(ConfigBlock *b, const char *key, const char *value) {
    if (b->directive_count >= 64) return;
    ConfigDirective *d = &b->directives[b->directive_count++];
    snprintf(d->key, sizeof(d->key), "%s", key);
    snprintf(d->value, sizeof(d->value), "%s", value);
}

static void parser_add_block(ConfigParser *parser, ConfigBlock *block) {
    if (!parser->blocks) {
        parser->blocks = block;
        parser->tail = block;
    } else {
        parser->tail->next = block;
        parser->tail = block;
    }
}

/* Forward declaration */
static bool parse_config_file(ConfigParser *parser, const char *path);

static void parse_include(ConfigParser *parser, const char *value) {
    if (parser->include_depth >= MAX_INCLUDE_DEPTH) return;

    char expanded[1024];
    expand_tilde(value, expanded, sizeof(expanded));

    /* If relative path, resolve relative to ~/.ssh/ */
    char full_path[1024];
    if (expanded[0] != '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", net_ssh_dir(), expanded);
    } else {
        snprintf(full_path, sizeof(full_path), "%s", expanded);
    }

    /* Use glob to expand wildcards */
    glob_t gl = {0};
    int rc = glob(full_path, GLOB_NOSORT | GLOB_TILDE, NULL, &gl);
    if (rc == 0) {
        for (usize i = 0; i < gl.gl_pathc; i++) {
            parser->include_depth++;
            parse_config_file(parser, gl.gl_pathv[i]);
            parser->include_depth--;
        }
        globfree(&gl);
    }
}

static bool parse_config_file(ConfigParser *parser, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    ConfigBlock *current = NULL;
    char line[2048];

    while (fgets(line, sizeof(line), f)) {
        /* Trim and skip comments/empty */
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        /* Remove trailing newline/whitespace */
        char *end = p + strlen(p) - 1;
        while (end > p && isspace((unsigned char)*end)) *end-- = '\0';

        /* Parse "Key Value" or "Key=Value" */
        char key[64] = {0};
        char value[1024] = {0};

        /* Handle "Key=Value" format */
        char *eq = strchr(p, '=');
        char *sp = p;
        while (*sp && !isspace((unsigned char)*sp) && *sp != '=') sp++;

        usize key_len = (usize)(sp - p);
        if (key_len >= sizeof(key)) key_len = sizeof(key) - 1;
        memcpy(key, p, key_len);
        key[key_len] = '\0';

        /* Get value: skip separator (whitespace or =) */
        char *val_start = sp;
        if (eq && eq == sp) {
            val_start = eq + 1;
        } else {
            while (*val_start && isspace((unsigned char)*val_start)) val_start++;
        }
        /* Skip leading whitespace in value */
        while (*val_start && isspace((unsigned char)*val_start)) val_start++;
        snprintf(value, sizeof(value), "%s", val_start);
        str_trim_inplace(value);

        /* Handle top-level directives */
        if (strcasecmp(key, "Host") == 0) {
            current = block_create();
            if (!current) continue;
            current->is_match = false;
            snprintf(current->patterns, sizeof(current->patterns), "%s", value);
            parser_add_block(parser, current);
            continue;
        }

        if (strcasecmp(key, "Match") == 0) {
            current = block_create();
            if (!current) continue;
            current->is_match = true;
            snprintf(current->match_criteria, sizeof(current->match_criteria), "%s", value);
            parser_add_block(parser, current);
            continue;
        }

        if (strcasecmp(key, "Include") == 0) {
            parse_include(parser, value);
            continue;
        }

        /* Regular directive — add to current block (or global if no block yet) */
        if (!current) {
            /* Global directives before any Host block → create a "Host *" block */
            current = block_create();
            if (!current) continue;
            current->is_match = false;
            snprintf(current->patterns, sizeof(current->patterns), "*");
            parser_add_block(parser, current);
        }
        block_add_directive(current, key, value);
    }

    fclose(f);
    return true;
}

/* =========================================================================
 * Match evaluation
 * ========================================================================= */

/* Check if a Match block applies to the given hostname */
static bool match_applies(const ConfigBlock *block, const char *hostname) {
    if (!block->is_match) return false;

    /* Parse "Match host <pattern>" or "Match all" */
    char criteria[512];
    snprintf(criteria, sizeof(criteria), "%s", block->match_criteria);
    str_trim_inplace(criteria);

    if (strcasecmp(criteria, "all") == 0) return true;

    /* "host <patterns>" */
    char *saveptr = NULL;
    char *token = strtok_r(criteria, " \t", &saveptr);
    if (!token) return false;

    if (strcasecmp(token, "host") == 0) {
        char *pattern = strtok_r(NULL, "", &saveptr);
        if (!pattern) return false;
        str_trim_inplace(pattern);
        return host_matches_patterns(hostname, pattern);
    }

    /* Unsupported Match criteria — skip */
    return false;
}

/* =========================================================================
 * Forward spec parsing: "bind_addr:bind_port dest_host:dest_port"
 * or "bind_port dest_host:dest_port"
 * ========================================================================= */

static bool parse_forward_spec(const char *value, SSHForwardSpec *fwd) {
    memset(fwd, 0, sizeof(*fwd));

    /* Try "bind_addr:bind_port host:port" */
    char local_part[256] = {0};
    char remote_part[256] = {0};

    if (sscanf(value, "%255s %255s", local_part, remote_part) < 2) return false;

    /* Parse local part: either "port" or "addr:port" */
    char *colon = strrchr(local_part, ':');
    if (colon) {
        *colon = '\0';
        snprintf(fwd->bind_address, sizeof(fwd->bind_address), "%s", local_part);
        fwd->bind_port = atoi(colon + 1);
    } else {
        fwd->bind_port = atoi(local_part);
    }

    /* Parse remote part: "host:port" */
    colon = strrchr(remote_part, ':');
    if (!colon) return false;
    *colon = '\0';
    snprintf(fwd->dest_host, sizeof(fwd->dest_host), "%s", remote_part);
    fwd->dest_port = atoi(colon + 1);

    return (fwd->bind_port > 0 && fwd->dest_port > 0);
}

/* =========================================================================
 * ProxyJump parsing: "user1@host1:port1,user2@host2:port2,..."
 * ========================================================================= */

static i32 parse_proxy_jump(const char *value, SSHProxyHop *hops, i32 max_hops) {
    if (!value || !value[0]) return 0;
    if (strcasecmp(value, "none") == 0) return 0;

    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", value);

    i32 count = 0;
    char *saveptr = NULL;
    char *token = strtok_r(buf, ",", &saveptr);

    while (token && count < max_hops) {
        str_trim_inplace(token);
        if (!token[0]) { token = strtok_r(NULL, ",", &saveptr); continue; }

        SSHProxyHop *hop = &hops[count];
        memset(hop, 0, sizeof(*hop));
        hop->port = 22;

        /* Parse user@host:port */
        char *at = strchr(token, '@');
        char *host_start = token;

        if (at) {
            *at = '\0';
            snprintf(hop->username, sizeof(hop->username), "%s", token);
            host_start = at + 1;
        }

        /* Check for [host]:port (IPv6) or host:port */
        if (host_start[0] == '[') {
            char *bracket_end = strchr(host_start, ']');
            if (bracket_end) {
                *bracket_end = '\0';
                snprintf(hop->hostname, sizeof(hop->hostname), "%s", host_start + 1);
                if (bracket_end[1] == ':') {
                    hop->port = atoi(bracket_end + 2);
                }
            }
        } else {
            char *colon = strrchr(host_start, ':');
            if (colon) {
                *colon = '\0';
                snprintf(hop->hostname, sizeof(hop->hostname), "%s", host_start);
                hop->port = atoi(colon + 1);
                if (hop->port <= 0) hop->port = 22;
            } else {
                snprintf(hop->hostname, sizeof(hop->hostname), "%s", host_start);
            }
        }

        if (hop->hostname[0]) count++;
        token = strtok_r(NULL, ",", &saveptr);
    }

    return count;
}

/* =========================================================================
 * Resolution: merge matching blocks into SSHResolvedConfig
 * ========================================================================= */

static void apply_directive(SSHResolvedConfig *cfg, const char *key,
                            const char *value, const char *orig_hostname) {
    /* Helper macro: set a field only if not already set (first match wins) */
    #define SET_ONCE_STR(flag, field) do { \
        if (!(cfg->set_flags & (flag))) { \
            snprintf(cfg->field, sizeof(cfg->field), "%s", value); \
            cfg->set_flags |= (flag); \
        } \
    } while (0)

    #define SET_ONCE_INT(flag, field) do { \
        if (!(cfg->set_flags & (flag))) { \
            cfg->field = atoi(value); \
            cfg->set_flags |= (flag); \
        } \
    } while (0)

    #define SET_ONCE_BOOL(flag, field) do { \
        if (!(cfg->set_flags & (flag))) { \
            cfg->field = (strcasecmp(value, "yes") == 0); \
            cfg->set_flags |= (flag); \
        } \
    } while (0)

    if (strcasecmp(key, "HostName") == 0 || strcasecmp(key, "Hostname") == 0) {
        if (!(cfg->set_flags & SSH_CFG_SET_HOSTNAME)) {
            /* Expand tokens in HostName */
            expand_tokens(value, cfg->hostname, sizeof(cfg->hostname),
                          orig_hostname, cfg->port, cfg->user);
            cfg->set_flags |= SSH_CFG_SET_HOSTNAME;
        }
    } else if (strcasecmp(key, "Port") == 0) {
        SET_ONCE_INT(SSH_CFG_SET_PORT, port);
    } else if (strcasecmp(key, "User") == 0) {
        SET_ONCE_STR(SSH_CFG_SET_USER, user);
    } else if (strcasecmp(key, "IdentityFile") == 0) {
        if (cfg->identity_file_count < SSH_CONFIG_MAX_IDENTITY_FILES) {
            char expanded[512];
            expand_tilde(value, expanded, sizeof(expanded));
            /* Expand tokens */
            char final[512];
            expand_tokens(expanded, final, sizeof(final),
                          orig_hostname, cfg->port, cfg->user);
            snprintf(cfg->identity_files[cfg->identity_file_count],
                     sizeof(cfg->identity_files[0]), "%s", final);
            cfg->identity_file_count++;
        }
    } else if (strcasecmp(key, "IdentitiesOnly") == 0) {
        SET_ONCE_BOOL(SSH_CFG_SET_IDENTITY_ONLY, identity_only);
    } else if (strcasecmp(key, "ProxyJump") == 0) {
        if (!(cfg->set_flags & SSH_CFG_SET_PROXY_JUMP)) {
            cfg->proxy_hop_count = parse_proxy_jump(value, cfg->proxy_hops,
                                                     SSH_CONFIG_MAX_PROXY_HOPS);
            cfg->set_flags |= SSH_CFG_SET_PROXY_JUMP;
        }
    } else if (strcasecmp(key, "ProxyCommand") == 0) {
        if (!(cfg->set_flags & SSH_CFG_SET_PROXY_COMMAND)) {
            expand_tokens(value, cfg->proxy_command, sizeof(cfg->proxy_command),
                          cfg->hostname[0] ? cfg->hostname : orig_hostname,
                          cfg->port > 0 ? cfg->port : 22,
                          cfg->user);
            cfg->set_flags |= SSH_CFG_SET_PROXY_COMMAND;
        }
    } else if (strcasecmp(key, "ForwardAgent") == 0) {
        SET_ONCE_BOOL(SSH_CFG_SET_FORWARD_AGENT, forward_agent);
    } else if (strcasecmp(key, "AddKeysToAgent") == 0) {
        SET_ONCE_BOOL(SSH_CFG_SET_ADD_KEYS_AGENT, add_keys_to_agent);
    } else if (strcasecmp(key, "ServerAliveInterval") == 0) {
        SET_ONCE_INT(SSH_CFG_SET_ALIVE_INTERVAL, server_alive_interval);
    } else if (strcasecmp(key, "ServerAliveCountMax") == 0) {
        SET_ONCE_INT(SSH_CFG_SET_ALIVE_COUNT, server_alive_count_max);
    } else if (strcasecmp(key, "Compression") == 0) {
        SET_ONCE_BOOL(SSH_CFG_SET_COMPRESSION, compression);
    } else if (strcasecmp(key, "RequestTTY") == 0) {
        if (!(cfg->set_flags & SSH_CFG_SET_REQUEST_TTY)) {
            cfg->request_tty = (strcasecmp(value, "yes") == 0 ||
                                strcasecmp(value, "force") == 0);
            cfg->set_flags |= SSH_CFG_SET_REQUEST_TTY;
        }
    } else if (strcasecmp(key, "LogLevel") == 0) {
        SET_ONCE_STR(SSH_CFG_SET_LOG_LEVEL, log_level);
    } else if (strcasecmp(key, "LocalForward") == 0) {
        if (cfg->local_forward_count < SSH_CONFIG_MAX_FORWARDS) {
            SSHForwardSpec fwd;
            if (parse_forward_spec(value, &fwd)) {
                cfg->local_forwards[cfg->local_forward_count++] = fwd;
            }
        }
    } else if (strcasecmp(key, "RemoteForward") == 0) {
        if (cfg->remote_forward_count < SSH_CONFIG_MAX_FORWARDS) {
            SSHForwardSpec fwd;
            if (parse_forward_spec(value, &fwd)) {
                cfg->remote_forwards[cfg->remote_forward_count++] = fwd;
            }
        }
    }

    #undef SET_ONCE_STR
    #undef SET_ONCE_INT
    #undef SET_ONCE_BOOL
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/* Cap ProxyJump recursion so a self-referential or cyclic ProxyJump chain
 * (host A -> ProxyJump A) can't recurse until the stack overflows. */
#define SSH_CONFIG_MAX_PROXY_DEPTH 10

static SSHResolvedConfig *ssh_config_resolve_depth(const char *hostname,
                                                   const char *config_path,
                                                   i32 depth) {
    if (!hostname || !hostname[0]) return NULL;

    /* Determine config path */
    char path[1024];
    if (config_path) {
        snprintf(path, sizeof(path), "%s", config_path);
    } else {
        snprintf(path, sizeof(path), "%s/.ssh/config", net_home_dir());
    }

    /* Parse the config file */
    ConfigParser parser = {0};
    parse_config_file(&parser, path);

    /* Create resolved config with defaults */
    SSHResolvedConfig *cfg = calloc(1, sizeof(SSHResolvedConfig));
    if (!cfg) {
        /* Free blocks */
        ConfigBlock *b = parser.blocks;
        while (b) { ConfigBlock *next = b->next; free(b); b = next; }
        return NULL;
    }

    snprintf(cfg->match_host, sizeof(cfg->match_host), "%s", hostname);
    cfg->server_alive_count_max = 3;  /* OpenSSH default */

    /* Walk blocks, apply matching ones */
    for (ConfigBlock *block = parser.blocks; block; block = block->next) {
        bool applies = false;

        if (block->is_match) {
            applies = match_applies(block, hostname);
        } else {
            applies = host_matches_patterns(hostname, block->patterns);
        }

        if (!applies) continue;

        /* Apply all directives from this block */
        for (i32 i = 0; i < block->directive_count; i++) {
            apply_directive(cfg, block->directives[i].key,
                           block->directives[i].value, hostname);
        }
    }

    /* Apply defaults for unset fields */
    if (!cfg->hostname[0]) {
        snprintf(cfg->hostname, sizeof(cfg->hostname), "%s", hostname);
    }
    if (cfg->port <= 0) {
        cfg->port = 22;
    }
    if (!cfg->user[0]) {
        /* Default to current system user */
        const char *user = getenv("USER");
        if (!user) user = getenv("LOGNAME");
        if (user) snprintf(cfg->user, sizeof(cfg->user), "%s", user);
    }

    /* If no identity files specified, add defaults */
    if (cfg->identity_file_count == 0) {
        const char *default_keys[] = {
            "~/.ssh/id_ed25519",
            "~/.ssh/id_rsa",
            "~/.ssh/id_ecdsa",
        };
        for (usize i = 0; i < ARRAY_LEN(default_keys); i++) {
            char expanded[512];
            expand_tilde(default_keys[i], expanded, sizeof(expanded));
            struct stat st;
            if (stat(expanded, &st) == 0) {
                snprintf(cfg->identity_files[cfg->identity_file_count],
                         sizeof(cfg->identity_files[0]), "%s", expanded);
                cfg->identity_file_count++;
            }
        }
    }

    /* Recursively resolve ProxyJump hops — each hop may itself have config.
     * Bounded by depth so a cyclic ProxyJump can't recurse forever; at the
     * limit the hop keeps its raw (unresolved) config. */
    for (i32 i = 0; depth < SSH_CONFIG_MAX_PROXY_DEPTH && i < cfg->proxy_hop_count; i++) {
        SSHProxyHop *hop = &cfg->proxy_hops[i];
        SSHResolvedConfig *hop_cfg = ssh_config_resolve_depth(hop->hostname, config_path, depth + 1);
        if (hop_cfg) {
            /* Apply resolved hostname (it may differ from the alias) */
            snprintf(hop->hostname, sizeof(hop->hostname), "%s", hop_cfg->hostname);
            if (hop->port <= 0 || hop->port == 22) {
                hop->port = hop_cfg->port;
            }
            if (!hop->username[0] && hop_cfg->user[0]) {
                snprintf(hop->username, sizeof(hop->username), "%s", hop_cfg->user);
            }
            if (!hop->identity_file[0] && hop_cfg->identity_file_count > 0) {
                snprintf(hop->identity_file, sizeof(hop->identity_file),
                         "%s", hop_cfg->identity_files[0]);
            }
            ssh_config_free(hop_cfg);
        }
    }

    /* Free parser blocks */
    ConfigBlock *b = parser.blocks;
    while (b) {
        ConfigBlock *next = b->next;
        free(b);
        b = next;
    }

    return cfg;
}

SSHResolvedConfig *ssh_config_resolve_from(const char *hostname, const char *config_path) {
    return ssh_config_resolve_depth(hostname, config_path, 0);
}

SSHResolvedConfig *ssh_config_resolve(const char *hostname) {
    return ssh_config_resolve_from(hostname, NULL);
}

void ssh_config_free(SSHResolvedConfig *cfg) {
    free(cfg);
}

void ssh_config_apply(const SSHResolvedConfig *resolved, SSHConfig *out, bool force) {
    if (!resolved || !out) return;

    /* Hostname */
    if (force || !out->hostname[0]) {
        snprintf(out->hostname, sizeof(out->hostname), "%s", resolved->hostname);
    }

    /* Port */
    if (force || out->port <= 0) {
        out->port = resolved->port > 0 ? resolved->port : 22;
    }

    /* Username */
    if (force || !out->username[0]) {
        snprintf(out->username, sizeof(out->username), "%s", resolved->user);
    }

    /* Identity file → key_path (use first available) */
    if (force || !out->key_path[0]) {
        for (i32 i = 0; i < resolved->identity_file_count; i++) {
            struct stat st;
            if (stat(resolved->identity_files[i], &st) == 0) {
                snprintf(out->key_path, sizeof(out->key_path),
                         "%s", resolved->identity_files[i]);
                /* If we found a key, prefer pubkey auth */
                if (out->auth_method == AUTH_PASSWORD && !out->password[0]) {
                    out->auth_method = AUTH_PUBLICKEY;
                }
                break;
            }
        }
    }

    /* ProxyJump chain — dynamic arrays in `out` are NULL by default; push
     * each hop via the canonical helper which grows geometrically. */
    if (force || out->proxy_chain_len == 0) {
        out->proxy_chain_len = 0;  /* reset; helper appends from current count */
        for (i32 i = 0; i < resolved->proxy_hop_count && i < MAX_PROXY_HOPS; i++) {
            ProxyHop *dst = ssh_config_push_proxy_hop(out);
            if (!dst) break;
            const SSHProxyHop *src = &resolved->proxy_hops[i];
            snprintf(dst->hostname, sizeof(dst->hostname), "%s", src->hostname);
            dst->port = src->port > 0 ? src->port : 22;
            snprintf(dst->username, sizeof(dst->username), "%s", src->username);
            if (src->identity_file[0]) {
                snprintf(dst->key_path, sizeof(dst->key_path), "%s", src->identity_file);
                dst->auth_method = AUTH_PUBLICKEY;
            } else {
                dst->auth_method = AUTH_AGENT;  /* default for proxy hops */
            }
        }
    }

    if (force || out->local_forward_count == 0) {
        out->local_forward_count = 0;
        for (i32 i = 0; i < resolved->local_forward_count && i < MAX_PORT_FORWARDS; i++) {
            PortForwardSpec *dst = ssh_config_push_local_forward(out);
            if (!dst) break;
            const SSHForwardSpec *src = &resolved->local_forwards[i];
            snprintf(dst->bind_host, sizeof(dst->bind_host), "%s", src->bind_address);
            dst->bind_port = src->bind_port;
            snprintf(dst->dest_host, sizeof(dst->dest_host), "%s", src->dest_host);
            dst->dest_port = src->dest_port;
        }
    }

    if (force || out->remote_forward_count == 0) {
        out->remote_forward_count = 0;
        for (i32 i = 0; i < resolved->remote_forward_count && i < MAX_PORT_FORWARDS; i++) {
            PortForwardSpec *dst = ssh_config_push_remote_forward(out);
            if (!dst) break;
            const SSHForwardSpec *src = &resolved->remote_forwards[i];
            snprintf(dst->bind_host, sizeof(dst->bind_host), "%s", src->bind_address);
            dst->bind_port = src->bind_port;
            snprintf(dst->dest_host, sizeof(dst->dest_host), "%s", src->dest_host);
            dst->dest_port = src->dest_port;
        }
    }

    /* Legacy jump_host compat: if proxy chain has exactly one hop and
     * legacy jump_host is empty, fill it in for backward compatibility */
    if (!out->jump_host[0] && out->proxy_chain_len == 1) {
        snprintf(out->jump_host, sizeof(out->jump_host),
                 "%s", out->proxy_chain[0].hostname);
        out->jump_port = out->proxy_chain[0].port;
        snprintf(out->jump_username, sizeof(out->jump_username),
                 "%s", out->proxy_chain[0].username);
    }
}
