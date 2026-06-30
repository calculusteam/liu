# liu-notify hook scripts

These scripts bridge CLI coding-tool hook systems into `liu-notify`, which
speaks a status update when something interesting happens (a task finishes,
a notification fires, a command errors out).

## Install

```bash
# build liu-notify
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target liu-notify -j

# install binary + symlink
sudo cmake --install build

# install hook scripts
sudo install -m 0755 scripts/notify-hooks/claude-hook.sh /usr/local/bin/
sudo install -m 0755 scripts/notify-hooks/wrap.sh        /usr/local/bin/
```

## Optional — auto-start the daemon at login

### macOS (launchd)

```bash
install -m 0644 scripts/notify-hooks/com.liu.notify.plist \
    ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.liu.notify.plist
# verify:
liu-notify status   # → running
```

To uninstall: `launchctl unload ~/Library/LaunchAgents/com.liu.notify.plist`.

### Linux (systemd user)

```bash
mkdir -p ~/.config/systemd/user
install -m 0644 scripts/notify-hooks/liu-notify.service \
    ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now liu-notify.service
```

Without auto-start, the daemon is automatically spawned the first time any
CLI or hook calls `liu-notify send` (double-fork, detached).

## Configuration

Optional config file at `~/.config/liu/notify.conf` (or `$XDG_CONFIG_HOME/liu/notify.conf`):

```ini
# Global switch
enabled              = true

# Speech
voice                = en-US       # BCP-47 tag or AVSpeech/espeak voice id
rate                 = 1.0         # 0.5..2.0

# Show a macOS banner / Linux notify-send toast alongside TTS
desktop_notification = false

# Queue tuning (0 = built-in defaults: 10/s refill, 20 burst, 5s dedup)
rate_limit_per_sec   = 0
rate_burst           = 0
dedup_window_sec     = 0

# Per-tool enable/disable
tool.claude.enabled  = true
tool.copilot.enabled = true
tool.codex.enabled   = true
tool.custom.enabled  = true
```

Any missing key keeps its default; the file itself is entirely optional.

## Claude Code

Edit `~/.claude/settings.json`:

```json
{
  "hooks": {
    "Stop": [
      { "matcher": "*", "hooks": [
        { "type": "command", "command": "/usr/local/bin/claude-hook.sh" }
      ]}
    ],
    "Notification": [
      { "matcher": "*", "hooks": [
        { "type": "command", "command": "/usr/local/bin/claude-hook.sh" }
      ]}
    ]
  }
}
```

The first time Claude Code fires a hook, `liu-notify` will spawn its daemon
automatically (double-fork, detached, `$TMPDIR/liu-notify.sock` on macOS or
`$XDG_RUNTIME_DIR/liu-notify.sock` on Linux).

## GitHub Copilot CLI, OpenAI Codex, or any other CLI

Use the generic `wrap.sh`:

```bash
# ~/.zshrc (or ~/.bashrc)
alias copilot='/usr/local/bin/wrap.sh copilot -- gh copilot'
alias codex='/usr/local/bin/wrap.sh codex -- codex'       # adjust to your binary name
```

On completion you'll hear "copilot: complete - 12s içinde bitti" (or "error — exit 1 (3s)").

## Manual probes

```bash
# check daemon status
liu-notify status

# fire a test message
liu-notify send --tool=custom --event=notify --title="test" --body="merhaba"

# stop the daemon
liu-notify stop
```

## Uninstall

```bash
liu-notify stop
sudo rm /usr/local/bin/liu-notify /usr/local/bin/liu-notifyd \
        /usr/local/bin/claude-hook.sh /usr/local/bin/wrap.sh
```

## Privacy & security notes

- The AF_UNIX socket lives under the per-UID temp dir and is `chmod 0600`.
- The daemon verifies peer UID on every connection (`getpeereid` / `SO_PEERCRED`)
  and rejects cross-user traffic.
- Titles/bodies are capped at 256 / 3072 bytes and validated as UTF-8; the
  daemon never spawns a shell — TTS is delivered via `AVSpeechSynthesizer`
  (macOS).
- Rate limit: 10 utterances/sec sustained, burst 20. Duplicate (tool,event,
  title,body) within 5 s are dropped.
