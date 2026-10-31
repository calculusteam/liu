<p align="center">
  <img src="assets/liu-bunny.png" width="92" height="128" alt="Liu Terminal">
</p>

<h1 align="center">Liu Terminal</h1>

<p align="center">
  <strong>One terminal for every AI agent.</strong><br>
  Liu sits between you and a variety of coding agents and makes ten parallel threads feel like one place.<br>
  Written in <strong>C</strong>: native on macOS, GPU-accelerated.
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

<p align="center">
  <strong>Languages:</strong>
  <a href="README.md">English</a>
  ·
  <a href="README.tr.md">Türkçe</a>
  ·
  <a href="README.zh.md">简体中文</a>
  ·
  <a href="README.ko.md">한국어</a>
  ·
  <a href="README.es.md">Español</a>
  ·
  <a href="README.de.md">Deutsch</a>
  ·
  <a href="README.it.md">Italiano</a>
  ·
  <a href="README.fr.md">Français</a>
  ·
  <a href="README.ja.md">日本語</a>
  ·
  <a href="README.ar.md">العربية</a>
</p>

---

## Features

### Agent History

Press Cmd+K to open the command palette, then type "agent history" to browse every installed AI CLI: Claude Code, Codex, Copilot, OpenCode, Qwen, Cline, Antigravity. Resume natively or scroll the transcript without launching the agent.

### Agent View

Liu tracks the agent in every split pane, not just in every tab. A tab names itself after the agent's own chat, carries one logo per agent pane, and sweeps an activity bar while output is being produced. Rest the pointer on it and a panel lists each pane: agent, chat topic, and a pill reading `working 1m 12s` or `idle 40s`. When an agent finishes a stretch of work, a card drops in under that tab and the completion sound plays.

### Agents Start Agents

An agent in a pane can hand work to another through Liu: `liu-agent start codex "port the tests"`. Liu asks you first, then runs it as a small job tab joined to the right of the asking tab: its logo over a moving haze that shows its state. Rest the pointer on it for its newest lines; click it for its chat, where you can send the next turn, stop it, or continue its session in a pane. Agents started together work as a team and are told when two of them touch the same file.

### Subagent Tabs

When an agent starts subagents of its own (Claude Code's Agent tool, Codex, OpenCode, Copilot CLI, Cursor, Qwen Code, Droid, Crush and more), each becomes a job tab beside the agent's tab, read live from the agent's own records. Five subagents, five tabs, each haze a picture of its own. Rest on one for its newest lines, click it for its whole log; each reads done or failed when it ends, and they clear at your next prompt.

### Voice Prompt

Press **⌘⇧M** and talk. Whisper large-v3-turbo runs on this Mac (no network, no account, nothing uploaded) and the words are typed into whichever pane has focus, so it works in Claude Code, in a plain shell, in a REPL. A live waveform shows what the microphone is actually hearing; when you stop, the trace freezes and a scan runs across it while the audio is transcribed. Pick the microphone, pick from all 100 languages Whisper knows, or point it at a hosted speech API instead.

### Sketch

Press **⌘K** and pick *Sketch*: a drawing sheet opens over the terminal; pen, line, arrow, box and eraser, 14 colours and a stroke-width slider. **P** **L** **A** **R** **E** switch tools, **⌘Z** undoes. **Esc** or **Return** turns the drawing into a PNG and puts it on the clipboard, ready to paste into an agent that reads images; show a layout instead of describing it. **⌥+Esc** closes without copying.

### AI Create Theme

Describe a look, a local agent emits JSON, Liu applies it live. Saved into `~/.config/Liu/themes/`. The agent gets Liu's own theming guide, so it fills all 25 colour slots. Only agents found on your PATH are offered, and the result joins the 19 built-in themes in Settings → Appearance.

### Markdown Reader & Editor

File browser opens READMEs, designs and agent notes: styled Markdown preview or raw edit in-window. Images, code and links render in read mode. Fenced code blocks carry a language label and a Copy button, and `![[note]]` embeds another note inline.

### Code Editor

Press **⌘K** and pick *Code Editor*: the tab becomes a small IDE; explorer rail on the left, the file on the right, a file-type icon on every row and tab. One table-driven highlighter covers 50+ languages, and the script inside a Vue, Svelte or Astro component is lexed by the same code as a .js file. **⌘F** finds, **⌘⇧F** replaces, **⌘S** saves; the status bar reads Ln/Col, language, line endings and encoding.

### Graph View

A knowledge graph of every Markdown file in a folder: nodes are files, edges are `[[wikilinks]]` and embeds. A force-directed layout settles them; hover a note to light up its neighbours, drag to pin, click to open.

### Notes Vault

Every note lives in one Vault. **⌘K** → Create Note lets a local agent draft Markdown straight into it, `[[wikilinks]]` connect them and feed the Graph, and Open Note / Search Notes jump anywhere. Terminal agents inherit `$LIU_VAULT` and write there too: **⌘⇧A** feeds any note back as an `@source`. One knowledge base, shared by you and your agents.

### Built-in notification system

`liu-notify` speaks only when an agent truly needs you, and only then: a voice alert, a desktop banner, or a custom sound. It runs inside Liu: the hooks go into your agents at launch and are removed when Liu quits, so agents you run elsewhere are not affected.

### Token Saving

Hit **Ctrl+⌘** twice in your prompt: the local Metal-accelerated LLM (or chosen agent) translates non-English to English *before* the request leaves your machine. Context window shrinks, API bills drop dramatically.

### Built-in SSH

Full SSH client behind one Session: live file upload and download, remote file browser, per-session menu, port forwarding (-L / -D), password / key / agent auth, known-hosts TOFU, Mosh reconnection, and recent-session history.

### Command Palette

A single **⌘K** fuzzy-searches every command, setting, host, snippet, theme and Agent History entry: no menu hunting, no muscle memory required. It changes shape too: `Open Note` is a note quick-switcher, `Search Notes` full-text-searches your Vault, and `Go to Heading` jumps through the open document's outline.

### Splits + Tabs

Up to 32 tabs, each with up to 8 panes in any H/V split tree. Pane drag-and-drop. **⌘⇧I** broadcasts input to every active pane simultaneously. Drag a tab out to make a window, or onto another window's tab strip to join it. Quit and relaunch, and Liu reopens the tabs you had.

### Resume Where You Left Off

Liu saves your tabs while it runs, so a quit, a crash or a restart brings back the same windows, tabs, splits and folders, each pane showing its earlier screen above a dim `Restored session` line. An agent that was running is started again on its own session by the shell (Claude Code, Codex, OpenCode and every agent with a resume flag), nothing typed into it.

### File Browser

Sidebar local + SFTP browser with one-click open. NSFilePromise drag-to-Finder transfers files in the background. 32 MiB LRU image cache. The browser opens at the focused pane's working directory. Drop a Finder file on a pane and its path is typed into the prompt for you, or uploaded over SFTP when that pane is an SSH session.

### Tab Sleep

Tabs idle for N minutes auto-suspend: CPU and RAM drop to near zero. Click to instantly resume exactly where you left off, scrollback intact. Only a tab sitting at its prompt sleeps. A build, a dev server or an agent mid-turn keeps it awake, and the scrollback is saved to disk while it sleeps.

### Group Tabs

Cluster related terminals into named, colour-coded groups right in the tab bar, Chrome-style. Create one from a tab's right-click menu or the command palette, and drag tabs in and out. Collapse a group to hide its tabs and put them to sleep, freeing their PTY/SSH connections. Recolour from an 8-colour palette, rename inline, ungroup, or close a whole group at once. Named, coloured groups survive restarts: up to 8 at a time.

### Site Manager

Register local project folders as sites, start and stop their dev servers from one panel, and watch live process logs: Local URL, HMR, and status at a glance. Open the browser, restart, or remove a site without leaving the terminal.

### Liu Browser

Flip on the element picker in Liu's built-in browser and click anything on a live page: a button, a card, a misaligned row. Liu grabs its CSS selector, tag, text, size and full HTML and drops it inline into your agent's prompt as a context chip. No screenshots, no copy-paste: point at the exact node and Claude, Codex or any CLI sees precisely what you see. Ideal for "fix this", "restyle that" or "why is this off-center?".

---

## Supported platforms

| Platform | Architecture | Format |
| :--- | :--- | :--- |
| **macOS** | Apple Silicon (arm64) | `.dmg` |
| **macOS** | Intel (x86_64) | `.dmg` |

macOS is the primary and only supported target.

## Our Goal

- [ ] **Linux support**: ARM64 and x86_64 builds (`.tar.gz`), apt (PPA), dnf (COPR), and AUR packages. Linux is planned but not currently supported.

---

## Build from source

**Dependencies**

- macOS: `brew install libssh2`
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

---

<p align="center">
  <sub>
    Apache-2.0
  </sub>
</p>
