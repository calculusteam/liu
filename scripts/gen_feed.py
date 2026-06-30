#!/usr/bin/env python3
"""Generate / update the Liu auto-update feed (appcast.json).

Merges a new version entry into the feed: computes sha256 + size for each
artifact, reads its Ed25519 .sig (base64, produced by sign_ed25519.sh), builds
the per-arch artifacts array, prepends/replaces the version in versions[], and
sets "latest". Idempotent — re-running for the same version replaces its entry.

Example:
  scripts/gen_feed.py \
    --feed appcast.json \
    --version 0.2.0 \
    --notes-url https://github.com/calculusteam/liu/releases/tag/v0.2.0 \
    --min-os 11.0 \
    --base-url https://github.com/calculusteam/liu/releases/download/v0.2.0 \
    --artifact arm64=build/package/Liu-0.2.0-arm64.zip

The signature file is expected next to each artifact as <artifact>.sig.
"""
import argparse
import base64
import datetime
import hashlib
import json
import os
import sys


def artifact_meta(path):
    if not os.path.isfile(path):
        sys.exit(f"error: artifact not found: {path}")
    sig_path = path + ".sig"
    if not os.path.isfile(sig_path):
        sys.exit(f"error: signature not found: {sig_path} (run sign_ed25519.sh first)")
    data = open(path, "rb").read()
    sig_b64 = open(sig_path).read().strip()
    # validate the sig is real base64 of a 64-byte Ed25519 signature
    try:
        raw = base64.b64decode(sig_b64, validate=True)
    except Exception as e:
        sys.exit(f"error: {sig_path} is not valid base64: {e}")
    if len(raw) != 64:
        sys.exit(f"error: {sig_path} decodes to {len(raw)} bytes, expected 64")
    return {
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "ed25519": sig_b64,
    }


def main():
    ap = argparse.ArgumentParser(description="Update the Liu update feed.")
    ap.add_argument("--feed", required=True, help="path to appcast.json (created if absent)")
    ap.add_argument("--version", required=True)
    ap.add_argument("--build", default="", help="git short hash (optional)")
    ap.add_argument("--notes-url", required=True)
    ap.add_argument("--min-os", default="11.0")
    ap.add_argument("--pubdate", default="", help="ISO-8601; defaults to now (UTC)")
    ap.add_argument("--base-url", required=True,
                    help="URL prefix the artifact filenames are appended to")
    ap.add_argument("--channel", default="stable")
    ap.add_argument("--artifact", action="append", default=[], metavar="ARCH=PATH",
                    help="repeatable, e.g. arm64=build/package/Liu-0.2.0-arm64.zip")
    args = ap.parse_args()

    if not args.artifact:
        sys.exit("error: at least one --artifact ARCH=PATH is required")

    pubdate = args.pubdate or datetime.datetime.now(
        datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    artifacts = []
    for spec in args.artifact:
        if "=" not in spec:
            sys.exit(f"error: --artifact must be ARCH=PATH, got: {spec}")
        arch, path = spec.split("=", 1)
        meta = artifact_meta(path)
        artifacts.append({
            "arch": arch,
            "url": args.base_url.rstrip("/") + "/" + os.path.basename(path),
            **meta,
        })

    entry = {
        "version": args.version,
        "build": args.build,
        "pubDate": pubdate,
        "minimumSystemVersion": args.min_os,
        "notesURL": args.notes_url,
        "yanked": False,
        "artifacts": artifacts,
    }

    if os.path.isfile(args.feed):
        feed = json.load(open(args.feed))
    else:
        feed = {"schema": 1, "channel": args.channel, "latest": args.version, "versions": []}

    feed["schema"] = 1
    feed["channel"] = args.channel
    # replace an existing same-version entry, else prepend
    feed["versions"] = [v for v in feed.get("versions", []) if v.get("version") != args.version]
    feed["versions"].insert(0, entry)
    feed["latest"] = args.version

    with open(args.feed, "w") as f:
        json.dump(feed, f, indent=2)
        f.write("\n")
    print(f"wrote {args.feed}: latest={args.version}, {len(artifacts)} artifact(s)")


if __name__ == "__main__":
    main()
