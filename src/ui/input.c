/*
 * Liu - SSH config import + known_hosts
 * Parses ~/.ssh/config into VaultHost records.
 */
#include "core/types.h"
#include "core/string_utils.h"
#include "ssh/ssh_session.h"
#include "vault/vault.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* Simple UUID-like ID generator */
static void gen_id(char *buf, usize size) {
    static u32 counter = 0;
    snprintf(buf, size, "%08x-%04x-%04x",
             (u32)time(NULL), (u32)(counter++ & 0xFFFF), (u32)(rand() & 0xFFFF));
}

/* Parse a single Host block from ssh config */
typedef struct {
    char host_alias[128];
    char hostname[256];
    char user[128];
    i32  port;
    char identity_file[512];
    char proxy_jump[256];
    bool forward_agent;
    bool forward_x11;
    bool gssapi_auth;
    bool control_master;
    char control_path[512];
    StreamLocalForward stream_forwards[4];
    i32  stream_forward_count;
} SSHConfigEntry;

/* Import ~/.ssh/config into vault */
i32 import_ssh_config(Vault *v, const char *config_path) {
    if (!config_path) {
        const char *home = getenv("HOME");
        if (!home) return -1;
        static char default_path[512];
        snprintf(default_path, sizeof(default_path), "%s/.ssh/config", home);
        config_path = default_path;
    }

    FILE *f = fopen(config_path, "r");
    if (!f) return -1;

    SSHConfigEntry current = {0};
    current.port = 22;
    bool in_host = false;
    i32 imported = 0;
    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        /* Trim leading whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        /* Skip comments and empty lines */
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        /* Remove trailing newline */
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';

        /* Parse key value */
        char key[64] = {0}, value[512] = {0};
        if (sscanf(p, "%63s %511[^\n]", key, value) < 2) continue;

        if (strcasecmp(key, "Host") == 0) {
            /* Save previous entry */
            if (in_host && current.hostname[0] && current.host_alias[0] != '*') {
                VaultHost h = {0};
                gen_id(h.id, sizeof(h.id));
                snprintf(h.label, sizeof(h.label), "%s", current.host_alias);
                snprintf(h.hostname, sizeof(h.hostname), "%s",
                         current.hostname[0] ? current.hostname : current.host_alias);
                h.port = current.port;
                h.protocol = PROTO_SSH;
                snprintf(h.username, sizeof(h.username), "%s", current.user);
                h.auth_method = current.gssapi_auth ? VAUTH_GSSAPI
                              : current.identity_file[0] ? VAUTH_PUBLICKEY
                              : VAUTH_PASSWORD;
                h.created_at = (i64)time(NULL);
                h.updated_at = h.created_at;
                if (vault_host_insert(v, &h)) imported++;
            }
            /* Start new entry */
            memset(&current, 0, sizeof(current));
            current.port = 22;
            snprintf(current.host_alias, sizeof(current.host_alias), "%s", value);
            in_host = true;
        } else if (strcasecmp(key, "HostName") == 0 || strcasecmp(key, "Hostname") == 0) {
            snprintf(current.hostname, sizeof(current.hostname), "%s", value);
        } else if (strcasecmp(key, "User") == 0) {
            snprintf(current.user, sizeof(current.user), "%s", value);
        } else if (strcasecmp(key, "Port") == 0) {
            current.port = atoi(value);
        } else if (strcasecmp(key, "IdentityFile") == 0) {
            snprintf(current.identity_file, sizeof(current.identity_file), "%s", value);
        } else if (strcasecmp(key, "ProxyJump") == 0) {
            snprintf(current.proxy_jump, sizeof(current.proxy_jump), "%s", value);
        } else if (strcasecmp(key, "GSSAPIAuthentication") == 0) {
            current.gssapi_auth = (strcasecmp(value, "yes") == 0);
        } else if (strcasecmp(key, "ControlMaster") == 0) {
            current.control_master = (strcasecmp(value, "auto") == 0 ||
                                      strcasecmp(value, "yes") == 0);
        } else if (strcasecmp(key, "ControlPath") == 0) {
            snprintf(current.control_path, sizeof(current.control_path), "%s", value);
        } else if (strcasecmp(key, "LocalForward") == 0 ||
                   strcasecmp(key, "RemoteForward") == 0) {
            /* Parse Unix socket forwarding: <local_path> <remote_path>
             * Only handles paths starting with / (Unix sockets).
             * TCP port forwards are handled elsewhere. */
            bool is_local = (strcasecmp(key, "LocalForward") == 0);
            char arg1[512] = {0}, arg2[512] = {0};
            if (sscanf(value, "%511s %511s", arg1, arg2) == 2) {
                if (arg1[0] == '/' && arg2[0] == '/' &&
                    current.stream_forward_count < 4) {
                    StreamLocalForward *fwd =
                        &current.stream_forwards[current.stream_forward_count++];
                    snprintf(fwd->local_socket, sizeof(fwd->local_socket),
                             "%s", arg1);
                    snprintf(fwd->remote_socket, sizeof(fwd->remote_socket),
                             "%s", arg2);
                    fwd->is_local_to_remote = is_local;
                }
            }
        } else if (strcasecmp(key, "ForwardAgent") == 0) {
            current.forward_agent = (strcasecmp(value, "yes") == 0);
        } else if (strcasecmp(key, "ForwardX11") == 0) {
            current.forward_x11 = (strcasecmp(value, "yes") == 0);
        }
    }

    /* Save last entry */
    if (in_host && current.host_alias[0] && current.host_alias[0] != '*') {
        VaultHost h = {0};
        gen_id(h.id, sizeof(h.id));
        snprintf(h.label, sizeof(h.label), "%s", current.host_alias);
        snprintf(h.hostname, sizeof(h.hostname), "%s",
                 current.hostname[0] ? current.hostname : current.host_alias);
        h.port = current.port;
        h.protocol = PROTO_SSH;
        snprintf(h.username, sizeof(h.username), "%s", current.user);
        h.auth_method = current.gssapi_auth ? VAUTH_GSSAPI
                      : current.identity_file[0] ? VAUTH_PUBLICKEY
                      : VAUTH_PASSWORD;
        h.created_at = (i64)time(NULL);
        h.updated_at = h.created_at;
        if (vault_host_insert(v, &h)) imported++;
    }

    fclose(f);
    return imported;
}
