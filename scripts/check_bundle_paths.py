#!/usr/bin/env python3
"""Check packaged binaries for prohibited filesystem paths.

    check_bundle_paths.py --forbid "$ROOT_DIR" --forbid "$HOME" Liu.app/Contents/MacOS/*

Paths embedded in a binary can make runtime behavior depend on files outside
the application bundle. Check each --forbid prefix as bytes, both as written
and after resolving symlinks.

Exit 0 when no file contains a forbidden prefix; exit 1 otherwise.
"""
import argparse
import os
import sys


def printable_run(data, at, limit=160):
    """The printable text around offset `at` — the string the path sits in."""
    start = at
    while start > 0 and 0x20 <= data[start - 1] < 0x7F:
        start -= 1
    end = at
    while end < len(data) and 0x20 <= data[end] < 0x7F:
        end += 1
    return data[start:end][:limit].decode("ascii", "replace")


def find_paths(data, prefixes):
    """Distinct strings in `data` that contain one of `prefixes` (bytes)."""
    found = []
    for prefix in prefixes:
        at = data.find(prefix)
        while at != -1:
            text = printable_run(data, at)
            if text not in found:
                found.append(text)
            at = data.find(prefix, at + len(prefix))
    return found


def forbidden_prefixes(paths):
    out = []
    for p in paths:
        for q in (p, os.path.realpath(p)):
            q = q.rstrip("/")
            # "/" or "" would match everything; a two-character root is a typo.
            if len(q) < 4 or not q.startswith("/"):
                raise argparse.ArgumentTypeError(f"not a usable path prefix: {p!r}")
            b = (q + "/").encode()
            if b not in out:
                out.append(b)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--forbid", action="append", required=True, metavar="DIR",
                    help="a directory of the build machine no shipped file may name (repeatable)")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args(argv)
    try:
        prefixes = forbidden_prefixes(args.forbid)
    except argparse.ArgumentTypeError as e:
        ap.error(str(e))

    ok = True
    for path in args.files:
        if os.path.isdir(path):
            continue
        try:
            with open(path, "rb") as f:
                hits = find_paths(f.read(), prefixes)
        except OSError as e:
            print(f"{path}: {e}", file=sys.stderr)
            ok = False
            continue
        for text in hits[:8]:
            print(f"{path}: names a build-machine path: {text}", file=sys.stderr)
        if len(hits) > 8:
            print(f"{path}: … and {len(hits) - 8} more", file=sys.stderr)
        ok = ok and not hits
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
