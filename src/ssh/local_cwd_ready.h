/*
 * Liu — is a freshly forked local PTY child ready to be asked where it is?
 *
 * `session_local_cwd()` answers "what directory is this tab in" by probing the
 * child's cwd through the OS (proc_pidinfo on macOS, /proc/<pid>/cwd on
 * Linux), because a stock zsh emits no OSC 7. The file browser in the sidebar
 * FOLLOWS that answer.
 *
 * The trap is that `child_pid` is valid from the moment `forkpty()` returns,
 * while the child does its own `chdir()` afterwards and `execve()`s after
 * that. In between, the child is still a copy of Liu, so the probe reports the
 * cwd the child INHERITED — and for an app launched from Finder or the Dock
 * that cwd is "/", which would send the sidebar to the disk root.
 *
 * The gate is EXEC and not a comparison against Liu's own cwd: the child's
 * chdir comes before its exec, so waiting for the exec cannot let a stale
 * answer through — while "the child's cwd equals Liu's" is a legitimate state
 * (an instance launched from a project directory has a shell sitting in
 * exactly that directory) and rejecting it would stop the browser following at
 * all.
 *
 * Only Liu's own fork of the shell races. A job the SHELL starts inherits the
 * shell's cwd, which is already the right answer, and never chdirs — so the
 * foreground-process probe needs no gate of its own.
 *
 * "Cannot tell" answers TRUE. The failure directions are not symmetric: a
 * wrong "not ready" would freeze the file browser on a stale folder for the
 * life of the tab, while a wrong "ready" costs at most the couple of
 * milliseconds of wrongness this file exists to remove.
 */
#ifndef SSH_LOCAL_CWD_READY_H
#define SSH_LOCAL_CWD_READY_H

#include "core/types.h"

#include <stdio.h>    /* snprintf */
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
  #include <libproc.h>
  #include <sys/proc_info.h>
#endif

/* Has `pid` replaced Liu's image with something of its own yet?
 *
 * Asked of the forked pid, and latched by the caller — exec happens once and
 * cannot be undone, so this never has to be re-asked once it is true. */
static inline bool liu_child_has_execd(pid_t pid) {
    if (pid <= 0) return false;

#if defined(__APPLE__)
    /* Our own name, resolved once. Same single-threaded contract as
     * session_local_cwd's shared return buffer. */
    static char self_comm[MAXCOMLEN + 1];
    static bool self_known = false;
    struct proc_bsdshortinfo bsi;
    if (!self_known) {
        if (proc_pidinfo(getpid(), PROC_PIDT_SHORTBSDINFO, 0, &bsi, sizeof bsi)
            != (int)sizeof bsi)
            return true;                      /* cannot tell — see above */
        snprintf(self_comm, sizeof self_comm, "%s", bsi.pbsi_comm);
        self_known = true;
    }
    if (proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &bsi, sizeof bsi)
        != (int)sizeof bsi)
        return true;
    /* pbsi_comm is a fixed 16-byte field, not necessarily NUL-terminated. */
    return strncmp(bsi.pbsi_comm, self_comm, sizeof bsi.pbsi_comm) != 0;

#elif defined(__linux__)
    /* /proc/<pid>/exe follows the CURRENT image, so it differs from ours the
     * moment the child execs — and a pre-exec child's link is ours verbatim. */
    char link[64], theirs[4096], ours[4096];
    ssize_t n, m;
    snprintf(link, sizeof link, "/proc/%d/exe", (int)pid);
    n = readlink(link, theirs, sizeof theirs - 1);
    if (n <= 0) return true;                  /* cannot tell */
    theirs[n] = '\0';
    m = readlink("/proc/self/exe", ours, sizeof ours - 1);
    if (m <= 0) return true;
    ours[m] = '\0';
    return strcmp(theirs, ours) != 0;

#else
    (void)pid;
    return true;
#endif
}

#endif /* SSH_LOCAL_CWD_READY_H */
