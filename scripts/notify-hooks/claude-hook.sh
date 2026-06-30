#!/usr/bin/env bash
# claude-hook.sh — Claude Code hook bridge → liu-notify
#
# Claude Code pipes a JSON payload on stdin and calls whatever command is
# configured under ~/.claude/settings.json `hooks:`. This script adapts the
# payload into a liu-notify frame.
#
# Install example (~/.claude/settings.json):
#   {
#     "hooks": {
#       "Stop":         [{"matcher":"*","hooks":[{"type":"command","command":"/usr/local/bin/claude-hook.sh"}]}],
#       "Notification": [{"matcher":"*","hooks":[{"type":"command","command":"/usr/local/bin/claude-hook.sh"}]}]
#     }
#   }

set -euo pipefail
IFS=$'\n\t'
umask 077

# Read entire stdin (Claude sends a single JSON object)
payload="$(cat -)"

if ! command -v jq >/dev/null 2>&1; then
    echo "claude-hook.sh: jq not found in PATH" >&2
    exit 0   # fail-open: don't block Claude
fi

event="$(jq -r '.hook_event_name // "unknown"' <<<"$payload")"

title="Claude"
body="Bildirim"

case "$event" in
    Stop)
        body="Görev tamamlandı"
        ;;
    Notification)
        body="$(jq -r '.message // "Bildirim"' <<<"$payload")"
        ;;
    PostToolUse)
        tool="$(jq -r '.tool_name // ""' <<<"$payload")"
        body="${tool:-araç} çalıştırıldı"
        ;;
    PreToolUse|UserPromptSubmit|PreCompact|SessionStart|SubagentStop)
        # Quieter events: skip by default. Uncomment the next line to enable.
        exit 0
        ;;
    *)
        exit 0
        ;;
esac

# Defence-in-depth truncation (the daemon also enforces these caps).
title="${title:0:256}"
body="${body:0:3072}"

# shellcheck source=./_liu_notify_common.sh
. "$(dirname -- "$0")/_liu_notify_common.sh"
if ! liu_notify_locate; then
    echo "claude-hook.sh: liu-notify not found" >&2
    exit 0
fi

exec "$LIU_NOTIFY" send \
    --tool=claude \
    --event="$event" \
    --title="$title" \
    --body="$body"
