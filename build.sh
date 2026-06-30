#!/usr/bin/env bash
# liu — build entry point (macOS / Linux). Thin wrapper so `./build.sh` works
# from the repo root; the real TUI lives in scripts/build.sh (see --help).
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/scripts/build.sh" "$@"
