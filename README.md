<p align="center">
  <img src="assets/liu-bunny.svg" width="128" height="128" alt="Liu Terminal">
</p>

<h1 align="center">Liu Terminal</h1>

<p align="center">
  <strong>One terminal for every AI agent.</strong><br>
  Liu sits between you and a variety of coding agents and makes ten parallel threads feel like one place.<br>
  Written in <strong>C</strong> — no Electron, JavaScript, or frameworks.
</p>

<p align="center">
  <a href="https://liu.software">Download</a>
  ·
  <a href="https://github.com/calculusteam/liu/releases">Releases</a>
  ·
  <a href="https://liu.software/docs">Docs</a>
  ·
  <a href="https://github.com/calculusteam/liu">GitHub</a>
</p>

---

## Features

### Agent History

Press **⌘K**, type *agent history*, and browse every installed AI CLI — Claude Code, Codex, Copilot, OpenCode, Qwen, Cline, Antigravity, Grok, and more. Resume a session natively or scroll the transcript without launching the agent.

### AI Create Theme

Describe a look; a local agent emits JSON; Liu applies it live. Saved into `~/.config/Liu/themes/` alongside the built-ins.

### Markdown Reader & Editor

The file browser opens READMEs, designs, and agent notes — styled Markdown preview or raw edit in-window. Images, code blocks, and links render in read mode.

### Graph View

A knowledge graph of every Markdown file in a folder — nodes are files, edges are `[[wikilinks]]` and embeds. Force-directed layout; hover to light up neighbours, drag to pin, click to open.

### Built-in notification system

`liu-notify` speaks only when an agent truly needs you — a voice alert, a desktop banner, or a custom sound. A standalone background service that keeps running even when Liu is closed.

### Token Saving

Hit **Ctrl+⌘** twice in your prompt — a local Metal-accelerated LLM (or chosen agent) translates non-English to English *before* the request leaves your machine. Context window shrinks; API bills drop.

### Metal & OpenGL renderer

Metal on macOS, OpenGL 3.3 core on Linux and elsewhere. Multi-pass pipeline — batched backgrounds, one instanced draw per glyph batch, on-demand atlasing.

### Built-in SSH

Full SSH client behind one Session: live file upload and download, remote file browser, port forwarding (`-L` / `-D`), password / key / agent auth, known-hosts TOFU, Mosh reconnection, and recent-session history.

### Command Palette

A single **⌘K** fuzzy-searches every command, setting, host, snippet, theme, and Agent History entry — no menu hunting.

### Splits + Tabs

Up to 32 tabs, each with up to 8 panes in any H/V split tree. Pane drag-and-drop. **⌘⇧B** broadcasts input to every active pane simultaneously.

### File Browser

Sidebar local + SFTP browser with one-click open. Drag-to-Finder transfers files in the background. 32 MiB LRU image cache.

### Tab Sleep

Tabs idle for N minutes auto-suspend — CPU and RAM drop to near zero. Click to instantly resume exactly where you left off, scrollback intact.

---

## Supported platforms

| Platform | Architecture | Format |
| :--- | :--- | :--- |
| **macOS** | Apple Silicon (arm64) | `.dmg` |
| **macOS** | Intel (x86_64) | `.dmg` |

macOS is the primary and only supported target.

**Package managers:** Homebrew — see [liu.software](https://liu.software) for install commands.

## Our Goal

- [ ] **Linux support** — ARM64 and x86_64 builds (`.tar.gz`), apt (PPA), dnf (COPR), and AUR packages. Linux is planned but not currently supported.

---

## Build from source

**Dependencies**

- macOS — `brew install libssh2`
- SQLite3, stb_truetype, stb_image, and cJSON are vendored under `third_party/`

```bash
git clone https://github.com/calculusteam/liu.git && cd liu

./build.sh              # interactive build TUI (macOS)
./build.sh --yes        # non-interactive recommended build
./build.sh --package    # build + release artifact

# Or CMake directly:
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
./build/Liu
```

A single build also produces `liu-notify` (notification daemon) and `liu-history` (agent-history CLI).

```bash
tic -x Liu.terminfo     # install custom terminfo entry
```

---

<p align="center">
  <sub>
    MIT · v0.1.0 beta
  </sub>
</p>