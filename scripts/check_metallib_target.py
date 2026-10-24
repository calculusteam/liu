#!/usr/bin/env python3
"""Refuse a Metal library built for a newer macOS than the app declares.

    check_metallib_target.py --max-os 11.0 Liu.app/Contents/MacOS/*.metallib

A .metallib records the macOS it was compiled for twice: in the container
header (from format 2.6 on) and in the target triple of every function's
bitcode ("air64_v23-apple-macosx11.0.0"). The Metal compiler takes that
target from the SDK unless it is told otherwise, so the same sources give
macOS 14.0 on one Xcode and macOS 27.0 on another, and a library for a newer
system than the one it is opened on is not loaded. This reads both places and
takes the higher; a file it cannot find a target in fails too.

Exit 0 when every library fits, 1 otherwise.
"""
import argparse
import re
import struct
import sys

TRIPLE = re.compile(rb"air64(?:_v\d+)?-apple-macosx(\d+)\.(\d+)")
HEADER_HAS_OS = 0x81        # the header's "target OS" byte when the version follows


def parse_version(text):
    parts = text.split(".")
    if not 1 <= len(parts) <= 3 or not all(p.isdigit() for p in parts):
        raise argparse.ArgumentTypeError(f"not a macOS version: {text!r}")
    return (int(parts[0]), int(parts[1]) if len(parts) > 1 else 0)


def library_target(data):
    """(major, minor) the library needs, its container format, or raises ValueError."""
    if len(data) < 16 or data[:4] != b"MTLB":
        raise ValueError("not a Metal library (no MTLB header)")
    _platform, fmt_major, fmt_minor, _kind, target_os, os_major, os_minor = \
        struct.unpack_from("<HHHBBHH", data, 4)
    found = [(int(a), int(b)) for a, b in TRIPLE.findall(data)]
    if target_os == HEADER_HAS_OS:
        found.append((os_major, os_minor))
    if not found:
        raise ValueError("no macOS target found in it")
    return max(found), (fmt_major, fmt_minor)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--max-os", required=True, type=parse_version,
                    help="the newest macOS a library may ask for (the app's minimum)")
    ap.add_argument("libraries", nargs="+")
    args = ap.parse_args()

    ok = True
    for path in args.libraries:
        try:
            with open(path, "rb") as f:
                target, fmt = library_target(f.read())
        except (OSError, ValueError) as e:
            print(f"{path}: {e}", file=sys.stderr)
            ok = False
            continue
        fits = target <= args.max_os
        print(f"{path}: macOS {target[0]}.{target[1]}, container {fmt[0]}.{fmt[1]}"
              f"{'' if fits else f' — newer than {args.max_os[0]}.{args.max_os[1]}'}",
              file=sys.stdout if fits else sys.stderr)
        ok = ok and fits
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
