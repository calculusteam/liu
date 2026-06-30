# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Project

**liu** — a native terminal built around AI-assisted coding workflows, written in C11 + hand-written SIMD assembly (ARM64 NEON on Apple Silicon, x86-64 SSE2 elsewhere). No web technologies, no frameworks. Prioritizes extreme speed and minimality.

## Build

```bash
# Configure (from repo root)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build -j$(sysctl -n hw.ncpu)

# Run
./build/liu

# Release build (LTO, -O3; portable baseline — add -DLIU_NATIVE_ARCH=ON for -march=native)
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(sysctl -n hw.ncpu)

# Interactive build TUI + per-OS release packaging
./build.sh        # macOS/Linux; --yes / --package

# Disable ASM (fallback to C implementations)
cmake -B build -DUSE_ASM=OFF

# Disable Metal, use OpenGL (macOS)
cmake -B build -DUSE_METAL=OFF
```

**Dependencies** (macOS): `brew install libssh2`. Linux: `apt install libssh2-1-dev`. SQLite3 and stb_truetype are vendored in `third_party/`.

**Terminfo**: `tic -x liu.terminfo` — install the custom terminfo entry for correct terminal capabilities.

## Architecture

The app binary is called `liu`. The codebase is pure C11 with optional hand-written assembly for hot paths.

### Layer structure (bottom-up)

1. **`src/core/`** — Foundation types, arena/pool allocators, UTF-8, dynamic arrays, config, keybindings, networking
   - `types.h` defines project-wide typedefs: `u8/u16/u32/u64`, `i8-i64`, `f32/f64`, `usize/isize`, plus `KB()/MB()/GB()` macros
   - `memory.h` — Arena (bump) and Pool (fixed-block) allocators; SIMD-optimized `fast_memcpy/memset/memzero`
   - `config.h` — JSON-based config (`~/.config/Liu/config.json`), 13 built-in themes

2. **`src/platform/`** — OS abstraction: window, GPU context, input events, clipboard, cursor, native tab bar
   - `platform.h` is the single interface; implementations: `platform_macos.m` (Cocoa), `platform_linux.c` (X11)
   - Event-driven: `platform_poll_events()` / `platform_next_event()` with typed `PlatformEvent` union
   - GPU accessors: `platform_get_gpu_device/layer/queue()` expose Metal objects on macOS

3. **`src/renderer/`** — Multi-pass terminal renderer
   - Two backends: **Metal** (default on macOS, `renderer_metal.m`) and **OpenGL 3.3 core** (`renderer.c`)
   - Pass 0: clear, Pass 1: cell backgrounds (rect batch), Pass 2: glyph instances (single draw call), Pass 3: decorations
   - `font.c` — stb_truetype font atlas with on-demand Unicode glyph rasterization; uses Metal textures when `USE_METAL=1`
   - Metal shaders: `src/swift/LiuShaders.metal` (compiled to `.metallib` at build time)
   - OpenGL shaders: `src/renderer/shaders/` (text.vert/frag, rect.vert/frag)

4. **`src/terminal/`** — VT100/xterm terminal emulator
   - `terminal.c` — cell grid, scrollback ring buffer, cursor, modes
   - `vt_parser.c` — VT sequence state machine (CSI, OSC, DCS, etc.)
   - `buffer.c`, `mouse.c`, `search.c`, `selection.c` — scrollback, mouse reporting, in-terminal search, smart selection

5. **`src/ssh/`** — Session management via libssh2
   - `ssh_session.c` — Local PTY + remote SSH sessions (password/key/agent auth)
   - `sftp.c`, `port_forward.c`, `telnet.c`, `mosh.c`, `serial.c` — protocol modules
   - `keygen.c`, `known_hosts.c` — SSH key generation and host verification

6. **`src/vault/`** — Encrypted storage (SQLite + AES-256) for hosts, keys, snippets, groups

7. **`src/ui/`** — UI system: tabs (max 32), split panes, sidebar (hosts/SFTP/snippets), command palette, settings overlay, file browser with editor
   - `ui.c` — `AppState` is the top-level struct that owns everything
   - `hittest.c` — hit testing for UI regions
   - `layout.h` — single source of truth for all UI dimension constants (in points, multiply by `dpi_scale`)

8. **`src/asm/`** — Hand-written SIMD: ARM64 NEON (`*_arm64.S`, Apple Silicon) and x86-64 SSE2 (`*_x86_64.S`, System V ABI via the C preprocessor in `x86_64_abi.h`). CMake compiles whichever matches the target arch; non-Apple ARM falls back to portable C.
   - `memops` — fast memory operations; `utf8` — UTF-8 validation; `terminal` — cell operations; `render` — batch helpers (ARM64 only; dead code on both)

9. **`src/swift/`** — Metal GPU layer
   - `LiuShaders.metal` — Metal shader pipelines (instanced glyphs + batched rects)
   - `LiuBridge.h` — C-to-Swift bridging header
   - `LiuRenderer.swift` — Swift rendering bridge (potential Swift UI layer)

### Main loop (`src/main.c`)

`main()` → `libssh2_init()` → `platform_init()` → window create → `app_init()` → Metal GPU setup (if `USE_METAL`) → create initial local PTY tab → event loop (`platform_poll_events` → process events → `app_poll_sessions` → `app_render` → swap buffers) → cleanup.

### Color encoding

Foreground/background colors use a shared `u32` encoding: values 0-255 are ANSI palette indices; bit 24 set (`0x01RRGGBB`) indicates 24-bit truecolor. Check with `IS_TRUECOLOR(c)`.

## Conventions

- All source uses the `u8/u16/u32/u64/i8-i64/f32/f64/usize/isize` typedefs from `core/types.h` — never use raw `uint32_t` etc.
- Memory: prefer arena/pool allocators (`Arena`, `Pool` from `memory.h`) over raw `malloc`.
- No C++ or Objective-C++ — macOS platform code is Objective-C (`.m`), Metal shaders are `.metal`.
- Keep dependencies minimal: only libssh2, SQLite3 (amalgamation), stb_truetype.
- Renderer code must handle both Metal and OpenGL paths — guard Metal-specific code with `#ifdef USE_METAL`.
