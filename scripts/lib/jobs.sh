# liu_jobs resolves a positive parallel-build job count. It honors a valid
# JOBS override, probes available platform utilities in order, validates each
# result, and uses a fixed fallback if none succeeds.
#
# Usage: . "$(dirname "$0")/lib/jobs.sh"; make -j"$(liu_jobs)"

# A positive decimal integer, and nothing else. Rejects "", "0", "-4", "8 ",
# "abc", and a probe that printed a warning before its number.
liu__jobs_valid() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;   # empty, or contains anything but a digit
    esac
    [ "$1" -gt 0 ] 2>/dev/null     # and "0" is not a job count
}

liu_jobs() {
    # An explicit JOBS wins, but only if it is sane; a typo must not become
    # unbounded parallelism either.
    if liu__jobs_valid "${JOBS:-}"; then printf '%s\n' "$JOBS"; return 0; fi

    liu__n=""

    # 1. macOS / BSD.
    if command -v sysctl >/dev/null 2>&1; then
        liu__n="$(sysctl -n hw.ncpu 2>/dev/null | tr -d '[:space:]')"
        liu__jobs_valid "$liu__n" && { printf '%s\n' "$liu__n"; return 0; }
    fi

    # 2. POSIX, and present on both macOS and glibc/musl Linux.
    if command -v getconf >/dev/null 2>&1; then
        liu__n="$(getconf _NPROCESSORS_ONLN 2>/dev/null | tr -d '[:space:]')"
        liu__jobs_valid "$liu__n" && { printf '%s\n' "$liu__n"; return 0; }
    fi

    # 3. GNU coreutils. Only when it EXISTS — calling it blind is the empty
    #    `-j` failure described at the top of this file.
    if command -v nproc >/dev/null 2>&1; then
        liu__n="$(nproc 2>/dev/null | tr -d '[:space:]')"
        liu__jobs_valid "$liu__n" && { printf '%s\n' "$liu__n"; return 0; }
    fi

    # 4. Small, positive, and deliberately conservative: this is the branch
    #    taken on a machine that reports no CPU count, and over-subscribing
    #    one of those is worse than building slowly.
    printf '4\n'
}
