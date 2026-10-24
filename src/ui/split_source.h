/*
 * Select the connection type and configuration for a new split pane.
 *
 * Splitting an SSH or mosh pane opens a separate connection to the same host.
 * Other session types open a local shell. The sibling uses the source
 * connection's credentials and host settings, but does not inherit port
 * forwards or Unix-socket forwards because those resources belong to the
 * original connection and cannot be bound twice.
 *
 * When the host configuration enables ControlMaster, the SSH library may
 * share the underlying connection. This helper only transforms configuration
 * data and does not require a live session.
 */
#ifndef UI_SPLIT_SOURCE_H
#define UI_SPLIT_SOURCE_H

#include "core/types.h"
#include "ssh/ssh_session.h"
#include "ssh/ssh_env_bootstrap.h"
#include <stdio.h>
#include <stdlib.h>

typedef enum {
    SPLIT_SRC_LOCAL = 0,   /* a local shell, in the source pane's folder */
    SPLIT_SRC_SSH,         /* another SSH connection to the same host */
    SPLIT_SRC_MOSH,        /* another mosh session to the same host */
} SplitSourceKind;

/* `has_config`: the source pane's session carries a connection config (only
 * SSH and mosh do — session_get_config() is NULL for the rest). */
static inline SplitSourceKind split_source_kind(SessionType type, bool has_config) {
    if (!has_config) return SPLIT_SRC_LOCAL;
    if (type == SESSION_SSH)  return SPLIT_SRC_SSH;
    if (type == SESSION_MOSH) return SPLIT_SRC_MOSH;
    return SPLIT_SRC_LOCAL;
}

/* Turn `cfg` — a deep copy of the source pane's config — into the sibling's:
 * no port forward, remote forward or Unix-socket forward (they stay with the
 * connection that owns them), and the shell starts in `start_dir` when that is
 * a folder the remote shell reported and one ssh_start_dir_ok() accepts, in
 * the account's home otherwise. Everything else — host, user, credentials,
 * jump hosts, X11 and agent forwarding, environment — is kept. */
static inline void split_sibling_config(SSHConfig *cfg, const char *start_dir) {
    if (!cfg) return;
    free(cfg->local_forwards);
    cfg->local_forwards = NULL;
    cfg->local_forward_count = 0;
    cfg->local_forward_cap = 0;
    free(cfg->remote_forwards);
    cfg->remote_forwards = NULL;
    cfg->remote_forward_count = 0;
    cfg->remote_forward_cap = 0;
    free(cfg->stream_forwards);
    cfg->stream_forwards = NULL;
    cfg->stream_forward_count = 0;
    cfg->stream_forward_cap = 0;

    cfg->start_dir[0] = '\0';
    if (ssh_start_dir_ok(start_dir))
        snprintf(cfg->start_dir, sizeof cfg->start_dir, "%s", start_dir);
}

/* Turn `cfg` — a deep copy of a host's config, or one the SSH form built —
 * into the SFTP tab's: a file-transfer connection (SSHConfig.sftp_only) with
 * nothing a shell would ask for. No forwards of any kind (they belong to a
 * terminal's connection, and a second bind breaks the first), no X11 or agent
 * forwarding, no shell folder, and never a shared connection. Host, user,
 * credentials and jump hosts are kept. */
static inline void sftp_only_config(SSHConfig *cfg) {
    if (!cfg) return;
    split_sibling_config(cfg, NULL);
    cfg->sftp_only      = true;
    cfg->control_master = false;
    cfg->forward_x11    = false;
    cfg->forward_agent  = false;
}

#endif /* UI_SPLIT_SOURCE_H */
