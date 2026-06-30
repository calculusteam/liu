#!/usr/bin/env python3
"""Generate the Liu release manifest (manifest.json).

Unlike the auto-update feed (appcast.json), which only lists the signed UPDATE
artifacts the in-app updater / install.sh consume (the macOS .zip and the Linux
.tar.gz), this manifest lists EVERY published file — including the macOS .dmg —
with its sha256 + size + final URL. The store packaging files (Homebrew cask,
AUR, deb/rpm) read their checksums from here via scripts/update-manifests.sh, so
a SHA is never hand-copied.

Example:
  scripts/gen_manifest.py \
    --out build/package/manifest.json \
    --version 0.1.0 \
    --base-url https://liu.software/downloads/v0.1.0 \
    --artifact dmg:arm64=build/package/Liu-0.1.0-arm64.dmg \
    --artifact dmg:x86_64=build/package/Liu-0.1.0-x86_64.dmg \
    --artifact tarball:linux-x86_64=build/package/liu-0.1.0-linux-x86_64.tar.gz \
    --artifact zip:arm64=build/package/Liu-0.1.0-arm64.zip
"""
import argparse
import hashlib
import json
import os
import sys


def file_meta(path):
    if not os.path.isfile(path):
        sys.exit(f"error: artifact not found: {path}")
    data = open(path, "rb").read()
    return len(data), hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description="Write the Liu release manifest.")
    ap.add_argument("--out", required=True, help="path to manifest.json")
    ap.add_argument("--version", required=True)
    ap.add_argument("--base-url", required=True,
                    help="URL prefix the artifact filenames are appended to")
    ap.add_argument("--artifact", action="append", default=[],
                    metavar="KIND:ARCH=PATH",
                    help="repeatable, e.g. dmg:arm64=build/package/Liu-0.1.0-arm64.dmg")
    args = ap.parse_args()

    if not args.artifact:
        sys.exit("error: at least one --artifact KIND:ARCH=PATH is required")

    artifacts = []
    for spec in args.artifact:
        if "=" not in spec or ":" not in spec.split("=", 1)[0]:
            sys.exit(f"error: --artifact must be KIND:ARCH=PATH, got: {spec}")
        kindarch, path = spec.split("=", 1)
        kind, arch = kindarch.split(":", 1)
        size, sha = file_meta(path)
        artifacts.append({
            "kind": kind,
            "arch": arch,
            "filename": os.path.basename(path),
            "url": args.base_url.rstrip("/") + "/" + os.path.basename(path),
            "sha256": sha,
            "size": size,
        })

    manifest = {"version": args.version, "artifacts": artifacts}
    with open(args.out, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"wrote {args.out}: version={args.version}, {len(artifacts)} artifact(s)")


if __name__ == "__main__":
    main()
