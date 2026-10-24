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
#include "core/checked.h"
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
#include <fcntl.h>
#include <unistd.h>

/* =========================================================================
 * Internal types
 * ========================================================================= */

/* A single directive from the config file. Key/value are heap strings owned
 * by the enclosing block: inline char[64]/char[1024] members would make
 * ConfigDirective 1088 B and — with the 64-slot inline array below — every
 * ConfigBlock a ~70 KB calloc, one per Host/Match line in ~/.ssh/config. */
typedef struct ConfigDirective {
    char *key;
    char *value;
} ConfigDirective;

/* Hard cap on directives kept per block: anything past this is silently
 * dropped. */
#define MAX_BLOCK_DIRECTIVES 64

/* A Host/Match block with its directives */
typedef struct ConfigBlock {
    /* Host patterns (space-separated), e.g. "*.example.com bastion" */
    char patterns[512];
    bool is_match;           /* true = Match directive, false = Host */
    char match_criteria[512]; /* for Match: "host *.example.com" etc. */

    ConfigDirective *directives;  /* grown geometrically, cap <= MAX_BLOCK_DIRECTIVES */
    i32 directive_count;
    i32 directive_cap;

    /* The block whose Include read this one in (NULL: written at the top
     * level). A block applies only when ITS header matches and the whole chain
     * above it does: an Include inside `Host production` carries `production`
     * with everything in the included file, so a nested Host or Match can no
     * longer match a host the includer would not. */
    const struct ConfigBlock *outer;

    struct ConfigBlock *next;
} ConfigBlock;

/* Parser state */
typedef struct {
    ConfigBlock *blocks;     /* linked list of parsed blocks */
    ConfigBlock *tail;
    i32 include_depth;       /* prevent infinite recursion */
    i32 include_files;       /* files read through Include in this parse */
    bool depth_reported;     /* the depth limit was hit and said so */
} ConfigParser;

#define MAX_INCLUDE_DEPTH 16
#define MAX_INCLUDE_FILES 256

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

#define SSH_CONFIG_MAX_FILE_BYTES (4u * 1024u * 1024u)
#define SSH_CONFIG_MAX_LINES      100000u

/* Simple glob/wildcard pattern matching for Host patterns. Supports * (any
 * chars) and ? (single char), case-insensitive.
 *
 * Iterative greedy matcher, O(len(pattern) * len(text)) worst case: on a
 * mismatch it backs up to the most recent '*' and lets it swallow one more
 * character. A recursive matcher that tries every split for every star
 * would explore O(n^k) paths on the connecting thread for a "Host ****…*x"
 * line — hundreds of stars, then a letter the hostname does not end in. */
static bool pattern_match(const char *pattern, const char *text) {
    const char *p = pattern, *t = text;
    const char *star = NULL, *star_t = NULL;
    while (*t) {
        if (*p == '*') {
            star = p++;
            star_t = t;
        } else if (*p == '?' ||
                   tolower((unsigned char)*p) == tolower((unsigned char)*t)) {
            p++;
            t++;
        } else if (star) {
            p = star + 1;
            t = ++star_t;
        } else {
            return false;
        }
    }
    while (*p == '*') p++;
    return *p == '\0';
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

static void block_free(ConfigBlock *b) {
    if (!b) return;
    for (i32 i = 0; i < b->directive_count; i++) {
        free(b->directives[i].key);
        free(b->directives[i].value);
    }
    free(b->directives);
    free(b);
}

static void block_add_directive(ConfigBlock *b, const char *key, const char *value) {
    if (b->directive_count >= MAX_BLOCK_DIRECTIVES) return;

    if (b->directive_count >= b->directive_cap) {
        i32 ncap = (b->directive_cap > 0) ? b->directive_cap * 2 : 4;
        if (ncap > MAX_BLOCK_DIRECTIVES) ncap = MAX_BLOCK_DIRECTIVES;
        /* ncap <= 64 and elem is 2 pointers, so the product cannot overflow. */
        ConfigDirective *nd = realloc(b->directives,
                                      (usize)ncap * sizeof(ConfigDirective));
        if (!nd) return;  /* OOM: drop the directive, same as hitting the cap */
        memset(nd + b->directive_cap, 0,
               (usize)(ncap - b->directive_cap) * sizeof(ConfigDirective));
        b->directives = nd;
        b->directive_cap = ncap;
    }

    /* key/value arrive already truncated to 63/1023 chars by the caller, so
     * duplicating them verbatim keeps those length limits. */
    char *k = strdup(key);
    char *v = strdup(value);
    if (!k || !v) { free(k); free(v); return; }
    b->directives[b->directive_count].key = k;
    b->directives[b->directive_count].value = v;
    b->directive_count++;
}

static void parser_free_blocks(ConfigParser *parser) {
    ConfigBlock *b = parser->blocks;
    while (b) {
        ConfigBlock *next = b->next;
        block_free(b);
        b = next;
    }
    parser->blocks = NULL;
    parser->tail = NULL;
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
static bool parse_config_file(ConfigParser *parser, const char *path,
                              const ConfigBlock *scope);

/* A block with the same condition as `src` (header and outer chain) and no
 * directives: where lines that belong to `src`'s scope go once something else
 * has been appended after it. */
static ConfigBlock *block_clone_scope(const ConfigBlock *src) {
    ConfigBlock *b = block_create();
    if (!b) return NULL;
    b->is_match = src->is_match;
    memcpy(b->patterns, src->patterns, sizeof b->patterns);
    memcpy(b->match_criteria, src->match_criteria, sizeof b->match_criteria);
    b->outer = src->outer;
    return b;
}

/* `scope` is the block the Include line sits in (NULL at the top level). */
static void parse_include(ConfigParser *parser, const char *value,
                          const ConfigBlock *scope) {
    if (parser->include_depth >= MAX_INCLUDE_DEPTH) {
        /* ssh(1) refuses to run at this point ("Too many recursive
         * configuration includes"). Liu stops descending and connects with
         * what it has read — but it says so, once per parse, because what it
         * has read is then not what ssh would use. */
        if (!parser->depth_reported) {
            parser->depth_reported = true;
            fprintf(stderr, "liu: ssh config: Include nested deeper than %d levels at '%s' — "
                            "ignored (ssh itself rejects such a config)\n",
                    MAX_INCLUDE_DEPTH, value ? value : "");
        }
        return;
    }

    char expanded[1024];
    expand_tilde(value, expanded, sizeof(expanded));

    /* If relative path, resolve relative to ~/.ssh/ */
    char full_path[1024];
    if (expanded[0] != '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", net_ssh_dir(), expanded);
    } else {
        snprintf(full_path, sizeof(full_path), "%s", expanded);
    }

    /* Use glob to expand wildcards. SORTED, as ssh does: directory order is
     * not a precedence anyone chose, and first-value-wins makes order the
     * whole semantics of a conf.d. */
    glob_t gl = {0};
    int rc = glob(full_path, GLOB_TILDE, NULL, &gl);
    if (rc == 0) {
        for (usize i = 0; i < gl.gl_pathc; i++) {
            /* Depth alone bounds nothing: a file that includes itself k times
             * is k^16 parses on the UI thread. A config has tens of files. */
            if (parser->include_files >= MAX_INCLUDE_FILES) break;
            parser->include_files++;
            parser->include_depth++;
            parse_config_file(parser, gl.gl_pathv[i], scope);
            parser->include_depth--;
        }
        globfree(&gl);
    }
}

static bool parse_config_file(ConfigParser *parser, const char *path,
                              const ConfigBlock *scope) {
    /* A config file is a regular file of modest size; "Include /dev/zero"
     * or a FIFO never reaches EOF and would park the parser. */
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        (u64)st.st_size > SSH_CONFIG_MAX_FILE_BYTES) {
        close(fd);
        return false;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return false; }

    /* In an included file `scope` is the block that included it: the file's
     * bare lines belong to THAT block's condition, and any Host/Match it
     * contains is conditioned on it. */
    ConfigBlock *current = NULL;
    char line[2048];
    u32 lines = 0;

    while (fgets(line, sizeof(line), f)) {
        if (++lines > SSH_CONFIG_MAX_LINES) break;
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
            current->outer = scope;
            snprintf(current->patterns, sizeof(current->patterns), "%s", value);
            parser_add_block(parser, current);
            continue;
        }

        if (strcasecmp(key, "Match") == 0) {
            current = block_create();
            if (!current) continue;
            current->is_match = true;
            current->outer = scope;
            snprintf(current->match_criteria, sizeof(current->match_criteria), "%s", value);
            parser_add_block(parser, current);
            continue;
        }

        if (strcasecmp(key, "Include") == 0) {
            /* The block this line is in: the includer's current block, or —
             * before any Host/Match of this file — the scope this whole file
             * was included under. */
            const ConfigBlock *here = current ? current : scope;
            ConfigBlock *tail_before = parser->tail;
            parse_include(parser, value, here);
            /* What the include appended sits AFTER `current` in the list, and
             * resolution is first-value-wins in list order. Lines of this file
             * that follow the Include must come after the included ones, as
             * they do in the file, so they continue in a fresh block with the
             * same condition appended at the end. */
            if (here && parser->tail != tail_before) {
                ConfigBlock *cont = block_clone_scope(here);
                if (cont) {
                    parser_add_block(parser, cont);
                    current = cont;
                }
            }
            continue;
        }

        /* Regular directive — add to current block */
        if (!current) {
            if (scope) {
                /* Bare lines at the top of an included file: the includer's
                 * own condition, not a global block. */
                current = block_clone_scope(scope);
                if (!current) continue;
            } else {
                /* Global directives before any Host block → a "Host *" block */
                current = block_create();
                if (!current) continue;
                current->is_match = false;
                snprintf(current->patterns, sizeof(current->patterns), "*");
            }
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
        /* ONE word of patterns (comma-separated, as ssh writes them). A second
         * criterion after it — `exec "…"`, `user …`, `localuser …` — is a
         * condition Liu cannot evaluate, and taking the rest of the line as
         * more host patterns would make such a block apply to every connection
         * to that host whether its condition held or not. */
        char *pattern = strtok_r(NULL, " \t", &saveptr);
        if (!pattern) return false;
        char *more = strtok_r(NULL, " \t", &saveptr);
        if (more) return false;
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
    if (!value || !fwd) return false;
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
        if (!checked_parse_i32(colon + 1, &fwd->bind_port)) return false;
    } else {
        if (!checked_parse_i32(local_part, &fwd->bind_port)) return false;
    }

    /* Parse remote part: "host:port" */
    colon = strrchr(remote_part, ':');
    if (!colon) return false;
    *colon = '\0';
    snprintf(fwd->dest_host, sizeof(fwd->dest_host), "%s", remote_part);
    if (!checked_parse_i32(colon + 1, &fwd->dest_port)) return false;

    return (fwd->bind_port > 0 && fwd->bind_port <= 65535 &&
            fwd->dest_port > 0 && fwd->dest_port <= 65535);
}

/* "LocalForward <local socket> <remote socket>": a Unix socket here forwarded
 * to one on the server. The local path may start with ~/ (this machine's
 * home); the remote one must be absolute — it is the server's, and nothing
 * here knows the server's home. parse_forward_spec refuses this shape, so it
 * is parsed here. */
static bool parse_stream_forward_spec(const char *value, SSHStreamForwardSpec *fwd) {
    if (!value || !fwd) return false;
    memset(fwd, 0, sizeof(*fwd));
    char local_part[512] = {0};
    char remote_part[512] = {0};
    if (sscanf(value, "%511s %511s", local_part, remote_part) < 2) return false;
    if (remote_part[0] != '/') return false;
    if (local_part[0] != '/' && !(local_part[0] == '~' && local_part[1] == '/')) return false;
    char expanded[512];
    expand_tilde(local_part, expanded, sizeof(expanded));
    int n = snprintf(fwd->local_socket, sizeof(fwd->local_socket), "%s", expanded);
    int m = snprintf(fwd->remote_socket, sizeof(fwd->remote_socket), "%s", remote_part);
    return n > 0 && (usize)n < sizeof(fwd->local_socket) &&
           m > 0 && (usize)m < sizeof(fwd->remote_socket);
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
                    i32 port;
                    if (checked_parse_i32(bracket_end + 2, &port) &&
                        port > 0 && port <= 65535) hop->port = port;
                }
            }
        } else {
            char *colon = strrchr(host_start, ':');
            if (colon) {
                *colon = '\0';
                snprintf(hop->hostname, sizeof(hop->hostname), "%s", host_start);
                i32 port;
                if (checked_parse_i32(colon + 1, &port) &&
                    port > 0 && port <= 65535) hop->port = port;
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

    #define SET_ONCE_NONNEG_INT(flag, field) do { \
        if (!(cfg->set_flags & (flag))) { \
            i32 parsed_; \
            if (checked_parse_i32(value, &parsed_) && parsed_ >= 0) { \
                cfg->field = parsed_; \
                cfg->set_flags |= (flag); \
            } \
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
        if (!(cfg->set_flags & SSH_CFG_SET_PORT)) {
            i32 port;
            if (checked_parse_i32(value, &port) && port > 0 && port <= 65535) {
                cfg->port = port;
                cfg->set_flags |= SSH_CFG_SET_PORT;
            }
        }
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
    } else if (strcasecmp(key, "ControlMaster") == 0) {
        if (!(cfg->set_flags & SSH_CFG_SET_CONTROL_MASTER)) {
            cfg->control_master = strcasecmp(value, "yes") == 0 ||
                                  strcasecmp(value, "auto") == 0 ||
                                  strcasecmp(value, "autoask") == 0;
            cfg->set_flags |= SSH_CFG_SET_CONTROL_MASTER;
        }
    } else if (strcasecmp(key, "ControlPath") == 0) {
        if (!(cfg->set_flags & SSH_CFG_SET_CONTROL_PATH)) {
            cfg->control_path_none = strcasecmp(value, "none") == 0;
            cfg->set_flags |= SSH_CFG_SET_CONTROL_PATH;
        }
    } else if (strcasecmp(key, "ServerAliveInterval") == 0) {
        SET_ONCE_NONNEG_INT(SSH_CFG_SET_ALIVE_INTERVAL, server_alive_interval);
    } else if (strcasecmp(key, "ServerAliveCountMax") == 0) {
        SET_ONCE_NONNEG_INT(SSH_CFG_SET_ALIVE_COUNT, server_alive_count_max);
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
        SSHStreamForwardSpec sfwd;
        if (parse_stream_forward_spec(value, &sfwd)) {
            if (cfg->stream_forward_count < SSH_CONFIG_MAX_STREAM_FORWARDS)
                cfg->stream_forwards[cfg->stream_forward_count++] = sfwd;
        } else if (cfg->local_forward_count < SSH_CONFIG_MAX_FORWARDS) {
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
    #undef SET_ONCE_NONNEG_INT
    #undef SET_ONCE_BOOL
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/* Cap ProxyJump recursion so a self-referential or cyclic ProxyJump chain
 * (host A -> ProxyJump A) can't recurse until the stack overflows. */
#define SSH_CONFIG_MAX_PROXY_DEPTH 10

/* Resolve against an already-parsed block list. The list is parsed once by
 * ssh_config_resolve_from() and reused for every ProxyJump hop, so a chain of
 * N hops does not re-read and re-allocate the whole config N+1 times. */
static SSHResolvedConfig *ssh_config_resolve_blocks(const char *hostname,
                                                    const ConfigParser *parser,
                                                    i32 depth) {
    if (!hostname || !hostname[0]) return NULL;

    /* Create resolved config with defaults */
    SSHResolvedConfig *cfg = calloc(1, sizeof(SSHResolvedConfig));
    if (!cfg) return NULL;

    snprintf(cfg->match_host, sizeof(cfg->match_host), "%s", hostname);
    cfg->server_alive_count_max = 3;  /* OpenSSH default */

    /* Walk blocks, apply matching ones */
    for (const ConfigBlock *block = parser->blocks; block; block = block->next) {
        bool applies = true;

        /* The block itself, then every block that included it. */
        for (const ConfigBlock *b = block; b && applies; b = b->outer) {
            applies = b->is_match ? match_applies(b, hostname)
                                  : host_matches_patterns(hostname, b->patterns);
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
        SSHResolvedConfig *hop_cfg = ssh_config_resolve_blocks(hop->hostname, parser, depth + 1);
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

    return cfg;
}

SSHResolvedConfig *ssh_config_resolve_from(const char *hostname, const char *config_path) {
    if (!hostname || !hostname[0]) return NULL;

    /* Determine config path */
    char path[1024];
    if (config_path) {
        snprintf(path, sizeof(path), "%s", config_path);
    } else {
        snprintf(path, sizeof(path), "%s/.ssh/config", net_home_dir());
    }

    /* Parse the config file once; every ProxyJump hop reuses these blocks. */
    ConfigParser parser = {0};
    parse_config_file(&parser, path, NULL);

    SSHResolvedConfig *cfg = ssh_config_resolve_blocks(hostname, &parser, 0);

    parser_free_blocks(&parser);
    return cfg;
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
                /* If a key was found, prefer pubkey auth */
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

    if (force || out->stream_forward_count == 0) {
        out->stream_forward_count = 0;
        for (i32 i = 0; i < resolved->stream_forward_count; i++) {
            StreamLocalForward *dst = ssh_config_push_stream_forward(out);
            if (!dst) break;
            const SSHStreamForwardSpec *src = &resolved->stream_forwards[i];
            snprintf(dst->local_socket, sizeof(dst->local_socket), "%s", src->local_socket);
            snprintf(dst->remote_socket, sizeof(dst->remote_socket), "%s", src->remote_socket);
            dst->is_local_to_remote = true;
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

    /* ForwardAgent: only an explicit "yes" in the config turns it on. */
    if (resolved->set_flags & SSH_CFG_SET_FORWARD_AGENT)
        out->forward_agent = resolved->forward_agent;
    /* ControlMaster: only an explicit yes / auto / autoask turns sharing on,
     * and ControlPath none turns it off again. */
    if (resolved->set_flags & SSH_CFG_SET_CONTROL_MASTER)
        out->control_master = resolved->control_master && !resolved->control_path_none;
    /* IdentitiesOnly: offer the key the user named, not every identity the
     * agent holds ahead of it. */
    if (resolved->identity_only) out->identities_only = true;

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
