#!/usr/bin/env bash
# wrap.sh <tool> -- <command...>
# Run a command, notify via liu-notify on completion/error.
#
# Example: alias copilot='/usr/local/bin/wrap.sh copilot -- gh copilot'

set -euo pipefail
IFS=$'\n\t'
umask 077

tool="${1:?usage: wrap.sh <tool> -- <cmd> [args...]}"
shift

if [[ "${1:-}" != "--" ]]; then
    echo "wrap.sh: missing -- separator" >&2
    exit 64
fi
shift

if [[ $# -eq 0 ]]; then
    echo "wrap.sh: no command given" >&2
    exit 64
fi

name="$(basename -- "${1:-$tool}")"
start="$(date +%s)"
status=0
"$@" || status=$?
dur=$(( $(date +%s) - start ))

# shellcheck source=./_liu_notify_common.sh
. "$(dirname -- "$0")/_liu_notify_common.sh"
if liu_notify_locate; then
    if [[ $status -eq 0 ]]; then
        "$LIU_NOTIFY" send --tool="$tool" --event=complete \
            --title="$name" --body="${dur}s içinde bitti" >/dev/null 2>&1 || true
    else
        "$LIU_NOTIFY" send --tool="$tool" --event=error \
            --title="$name" --body="exit $status (${dur}s)" >/dev/null 2>&1 || true
    fi
fi

exit $status
