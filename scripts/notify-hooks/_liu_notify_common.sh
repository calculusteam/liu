# Shared helpers for liu-notify hook scripts. Sourced, not executed.

# Sets $LIU_NOTIFY to the first liu-notify binary found, or empty.
# An explicit $LIU_NOTIFY env (e.g. pinning a specific dev build, or stubbing
# the daemon out for tests) wins over PATH so callers can override.
liu_notify_locate() {
    if [[ -n "${LIU_NOTIFY:-}" && -x "${LIU_NOTIFY}" ]]; then
        return 0
    fi
    if command -v liu-notify >/dev/null 2>&1; then
        LIU_NOTIFY="liu-notify"
        return 0
    fi
    for cand in /usr/local/bin/liu-notify /opt/homebrew/bin/liu-notify "$HOME/.local/bin/liu-notify"; do
        if [[ -x "$cand" ]]; then
            LIU_NOTIFY="$cand"
            return 0
        fi
    done
    LIU_NOTIFY=""
    return 1
}
