#!/usr/bin/env python3
"""Generate the Liu release manifest (manifest.json).

Unlike the auto-update feed (appcast.json), which only lists the signed UPDATE
artifacts the in-app updater / install.sh consume (the macOS .zip and the Linux
.tar.gz), this manifest lists EVERY published file — including the macOS .dmg —
with its sha256 + size + final URL. It is a signed record for verifying a
downloaded artifact by hand; it has no automated consumer, and the updater path
never reads it.

For a platform re-cut, --merge-from authenticates a prior manifest's exact bytes
against its adjacent .sig before keeping any same-version entries. Rebuilt
(kind, arch) pairs replace their old slots; other entries stay verbatim. A signed
manifest of another version is not merged. --feed authenticates the finished,
SIGNED appcast and requires this manifest to cover every update artifact of
--version by platform, URL, checksum and size. The output still needs signing.

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
from pathlib import Path
from verify_release import (check_feed_signature, check_manifest_feed, check_manifest_signature,
                            manifest_artifact_pair, manifest_artifacts, public_key_pem, release_entry)


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
    ap.add_argument("--merge-from", help="signed prior manifest to merge for the same version; "
                    "its detached signature must be at PATH.sig")
    ap.add_argument("--feed", help="signed appcast.json whose --version artifacts must all be covered")
    ap.add_argument("--pubkey-header", default=str(Path(__file__).resolve().parent.parent / "src/core/update_pubkey.h"))
    ap.add_argument("--openssl", default="openssl")
    ap.add_argument("--artifact", action="append", default=[],
                    metavar="KIND:ARCH=PATH",
                    help="repeatable, e.g. dmg:arm64=build/package/Liu-0.1.0-arm64.dmg")
    args = ap.parse_args()

    if not args.artifact:
        sys.exit("error: at least one --artifact KIND:ARCH=PATH is required")

    artifacts = []
    replacements = {}
    for spec in args.artifact:
        if "=" not in spec or ":" not in spec.split("=", 1)[0]:
            sys.exit(f"error: --artifact must be KIND:ARCH=PATH, got: {spec}")
        kindarch, path = spec.split("=", 1)
        kind, arch = kindarch.split(":", 1)
        if not kind or not arch:
            sys.exit(f"error: --artifact must have a nonempty KIND:ARCH, got: {spec}")
        if (kind, arch) in replacements:
            sys.exit(f"error: duplicate manifest artifact for {kind}/{arch}")
        size, sha = file_meta(path)
        item = {
            "kind": kind,
            "arch": arch,
            "filename": os.path.basename(path),
            "url": args.base_url.rstrip("/") + "/" + os.path.basename(path),
            "sha256": sha,
            "size": size,
        }
        artifacts.append(item)
        replacements[(kind, arch)] = item

    pem = public_key_pem(Path(args.pubkey_header)) if args.merge_from or args.feed else None
    if args.merge_from:
        prior = check_manifest_signature(args.openssl, pem, Path(args.merge_from))
        if prior["version"] != args.version:
            print(f"prior manifest is for {prior['version']}, not {args.version}: nothing to merge")
        else:
            merged, seen = [], set()
            for item in prior["artifacts"]:
                pair = manifest_artifact_pair(item)
                if pair in seen:
                    sys.exit(f"error: duplicate prior manifest artifact for {pair[0]}/{pair[1]}")
                seen.add(pair)
                merged.append(replacements.get(pair, item))
            merged.extend(item for pair, item in replacements.items() if pair not in seen)
            artifacts = merged

    manifest = {"version": args.version, "artifacts": artifacts}
    manifest_artifacts(manifest)
    if args.feed:
        feed = check_feed_signature(args.openssl, pem, Path(args.feed))
        entry = release_entry(feed, args.version)
        if entry is None:
            sys.exit(f"error: appcast.json has no release entry for {args.version}")
        check_manifest_feed(manifest, entry)
    with open(args.out, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"wrote {args.out}: version={args.version}, {len(artifacts)} artifact(s)")


if __name__ == "__main__":
    main()
