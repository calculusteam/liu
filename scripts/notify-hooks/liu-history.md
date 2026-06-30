# liu-history

Read agent chat history (Claude Code, Codex CLI, GitHub Copilot CLI) from
disk. No network calls. No writes — strictly read-only on `~/.claude`,
`~/.codex`, `~/.copilot`.

## Install

```bash
cmake --build build --target liu-history -j
sudo cmake --install build   # puts liu-history in /usr/local/bin
```

## Commands

```bash
# Top 20 most-recently-modified sessions across all tools
liu-history list
liu-history list --tool=claude --limit=5

# Full transcript of one session (matches by UUID/basename/path)
liu-history show 1b477590-d552-424e-87d4-9c7551fe6e41
liu-history show ./my-session.jsonl --format=markdown
liu-history show <id> --format=json | jq '.[] | select(.role=="assistant")'

# Filter which roles get printed
liu-history show <id> --roles=user,assistant             # skip tool noise
liu-history show <id> --roles=tool                       # just tool_use + tool_result

# Session metadata + event count (parses the file once)
liu-history info <id>

# Debug: raw list of every discovered session file
liu-history scan
```

## Environment overrides

If your agent CLIs store sessions elsewhere, point `liu-history` at them:

- `CLAUDE_CONFIG_DIR` → scans `$CLAUDE_CONFIG_DIR/projects`
- `CODEX_HOME`        → scans `$CODEX_HOME/sessions`
- `COPILOT_HOME`      → scans `$COPILOT_HOME/session-state`

## Formats

- `text` (default) — plain stdout with role-emoji prefix per event
- `markdown` — heading per event, fenced code blocks for tool I/O
- `json` — array of ChatEvent objects, each with
  `{tool, role, tool_name?, timestamp_ms?, session_id, text}`

## Notes

- Tool results from the `Bash` / `Read` tools can contain raw escape sequences.
  When stdout is a TTY, liu-history strips C0 control bytes from tool_use /
  tool_result content so a session file cannot corrupt your terminal.
- Session files open with `O_NOFOLLOW` — a symlink under `~/.claude/projects/`
  will not be followed.
- Copilot CLI format varies across versions; v1 ships a stub parser that
  shows the first 200 chars of each record until we have a stable corpus to
  reverse-engineer.

## Performance

A 3.8 MB Claude session (~900 events) parses in **~27 ms** on M1. Arena-backed
parse, one allocation reset per line.
