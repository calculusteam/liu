#!/usr/bin/env python3
"""Generate / update the Liu auto-update feed (appcast.json).

Merges a new version entry into the feed: computes sha256 + size for each
artifact, reads its Ed25519 .sig (base64, produced by sign_ed25519.sh), builds
the per-arch artifacts array, prepends/replaces the version in versions[], and
sets "latest". Re-running for the same version replaces its entry — against a
SIGNED feed: the existing feed is authenticated before it is merged, and this
script's own output is unsigned until sign_feed.py has run on it.

For a same-version platform re-cut, --keep-other-artifacts carries forward the
authenticated artifacts for (os, arch) pairs not supplied by this invocation.
Their metadata and signatures are kept verbatim, not reconstructed from names.
The new artifacts' signatures are also verified against --pubkey-header.
When any are kept, Linux is stably ordered before macOS for the site's resolver.
The default remains full replacement, as used by the multi-platform CI release.

A feed whose history holds a live version ABOVE the one being released is not
merged without being told how (--drop-newer / --keep-newer): a client on the
new release would be offered the old build as its update.

Example:
  scripts/gen_feed.py \
    --feed appcast.json \
    --version 0.1.0 \
    --notes-url https://github.com/calculusteam/liu/releases/tag/v0.1.0 \
    --min-os 11.0 \
    --base-url https://github.com/calculusteam/liu/releases/download/v0.1.0 \
    --artifact macos:arm64=build/package/Liu-0.1.0-arm64.zip \
    --artifact linux:x86_64=build/package/liu-0.1.0-linux-x86_64.tar.gz

The signature file is expected next to each artifact as <artifact>.sig.

Each artifact carries BOTH "os" and "arch", because neither identifies a build
on its own: "x86_64" names a macOS .zip and a Linux tarball equally well, and a
client matching on arch alone downloads the wrong one and tries to install it.
The OS: prefix is optional and defaults to macos — feeds and release scripts
written before Liu shipped for Linux are all-macOS, which is the same rule
updater.c applies to an artifact with no "os" key at all.
"""
import argparse
import base64
import datetime
import hashlib
import json
import os
import re
import sys
import tempfile
from pathlib import Path
from verify_release import (check_feed_artifact, check_feed_signature, feed_artifact_pair,
                            public_key_pem, release_entry, verify_signature)


def artifact_meta(path, openssl="openssl", pem=None):
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
    if pem is not None:
        # Verify the same snapshot whose hash and size are recorded below.
        with tempfile.TemporaryDirectory() as td:
            body = Path(td) / "artifact.signed"
            body.write_bytes(data)
            verify_signature(openssl, pem, body, sig_b64, path)
    return {
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "ed25519": sig_b64,
    }


def release_numbers(version):
    """X.Y.Z as three integers, or None when the text does not start that way.

    Only the numbers: a prerelease of the SAME numbers is not "a newer line",
    and that is the one question the guard below asks.
    """
    m = re.match(r"(\d+)\.(\d+)\.(\d+)(?:[-+].*)?$", str(version or ""))
    return tuple(int(g) for g in m.groups()) if m else None


def newer_live_entries(feed, version):
    """Non-yanked entries of `feed` whose version is ABOVE the one being released.

    updater.c offers a client that is already on "latest" the highest remaining
    entry newer than itself (updater_pick_version). So an old feed merged under
    a LOWER version (a restarted version line) makes every client on the new
    release an offer of the old build. A yanked entry is skipped by the
    client and is left alone here. An entry whose version cannot be read counts
    as newer: what cannot be compared must not ride along unseen.
    """
    mine = release_numbers(version)
    found = []
    for entry in feed.get("versions", []):
        if not isinstance(entry, dict) or entry.get("yanked"):
            continue
        theirs = release_numbers(entry.get("version"))
        if theirs is None or theirs > mine:
            found.append(str(entry.get("version")))
    return found


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
    ap.add_argument("--pubkey-header", default=str(Path(__file__).resolve().parent.parent / "src/core/update_pubkey.h"))
    ap.add_argument("--openssl", default="openssl")
    ap.add_argument("--keep-other-artifacts", action="store_true",
                    help="for a same-version re-cut, preserve authenticated artifacts for "
                         "(os, arch) pairs not rebuilt by this call; Linux stays before macOS")
    newer = ap.add_mutually_exclusive_group()
    newer.add_argument("--drop-newer", action="store_true",
                       help="leave out of the new feed every entry of the old one that is newer "
                            "than --version (the version line was restarted)")
    newer.add_argument("--keep-newer", action="store_true",
                       help="keep such entries on purpose (a staged rollout); clients already on "
                            "--version WILL be offered them")
    ap.add_argument("--artifact", action="append", default=[], metavar="[OS:]ARCH=PATH",
                    help="repeatable, e.g. macos:arm64=build/package/Liu-0.1.0-arm64.zip; "
                         "the OS: prefix defaults to macos")
    args = ap.parse_args()

    if not args.artifact:
        sys.exit("error: at least one --artifact ARCH=PATH is required")
    if release_numbers(args.version) is None:
        sys.exit(f"error: --version must be X.Y.Z (optionally -prerelease), got: {args.version}")
    pem = public_key_pem(Path(args.pubkey_header)) if args.keep_other_artifacts else None

    pubdate = args.pubdate or datetime.datetime.now(
        datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    KNOWN_OS = ("macos", "linux")
    artifacts = []
    seen = set()
    for spec in args.artifact:
        if "=" not in spec:
            sys.exit(f"error: --artifact must be [OS:]ARCH=PATH, got: {spec}")
        token, path = spec.split("=", 1)
        if ":" in token:
            art_os, arch = token.split(":", 1)
        else:
            art_os, arch = "macos", token
        if art_os not in KNOWN_OS:
            sys.exit(f"error: unknown artifact os {art_os!r} in {spec} "
                     f"(expected one of {', '.join(KNOWN_OS)})")
        if not arch:
            sys.exit(f"error: --artifact is missing the arch in: {spec}")
        # (os, arch) is the identity a client matches on, so two artifacts
        # sharing one is not a warning — it means whichever happens to come
        # first in the array wins and the other is unreachable.
        if (art_os, arch) in seen:
            sys.exit(f"error: duplicate artifact for {art_os}/{arch}")
        seen.add((art_os, arch))
        meta = artifact_meta(path, args.openssl, pem)
        item = {
            "os": art_os,
            "arch": arch,
            "url": args.base_url.rstrip("/") + "/" + os.path.basename(path),
            **meta,
        }
        if args.keep_other_artifacts:
            check_feed_artifact(item, "new artifact")
        artifacts.append(item)

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
        # Authenticate the original bytes before removing the old signature.
        # Only a trusted history may influence the next signed replay counter.
        feed = check_feed_signature(args.openssl,
                                    pem or public_key_pem(Path(args.pubkey_header)),
                                    Path(args.feed))
    else:
        feed = {"schema": 1, "channel": args.channel, "latest": args.version, "versions": []}

    prior = release_entry(feed, args.version)
    kept = []
    if args.keep_other_artifacts and prior is not None:
        old_artifacts = prior.get("artifacts")
        if not isinstance(old_artifacts, list):
            sys.exit(f"error: existing {args.version} entry has no artifacts array")
        old_pairs = set()
        for item in old_artifacts:
            pair = feed_artifact_pair(item)
            if pair in old_pairs:
                sys.exit(f"error: existing {args.version} entry has duplicate artifact for {pair[0]}/{pair[1]}")
            old_pairs.add(pair)
            if pair not in seen:
                # A signature authenticates origin, not installability. Do not
                # re-sign malformed metadata; a rebuilt pair can repair its own.
                check_feed_artifact(item, f"existing {args.version} artifact")
                kept.append(item)
        if kept:
            artifacts = sorted(kept + artifacts, key=lambda a: a.get("os", "macos") != "linux")
            entry["artifacts"] = artifacts

    # Before anything is changed: does the history about to be carried forward
    # outrank the release it is being merged under?
    ahead = newer_live_entries(feed, args.version)
    if ahead and args.drop_newer:
        feed["versions"] = [v for v in feed.get("versions", [])
                            if not (isinstance(v, dict) and str(v.get("version")) in ahead)]
        print(f"dropped from the merged history (newer than {args.version}): {', '.join(ahead)}")
    elif ahead and not args.keep_newer:
        sys.exit(f"error: {args.feed} carries version(s) newer than {args.version} that are not "
                 f"yanked: {', '.join(ahead)}.\n"
                 f"A client running {args.version} would be offered them as an update. If the "
                 f"version line was restarted, re-run with --drop-newer; to keep them on purpose "
                 f"(a staged rollout), --keep-newer. Nothing was written.")

    feed["schema"] = 1
    feed["channel"] = args.channel
    # The live feed this was merged from carries the PREVIOUS release's signature, which covers
    # text that is about to change. scripts/sign_feed.py signs the finished file; until it runs
    # the feed is deliberately unsigned (verify_release.py refuses it). "generated" stays: the
    # signer reads it to keep the next one strictly greater.
    feed.pop("feed_signature", None)
    # replace an existing same-version entry, else prepend. Preserve a prior
    # yanked=true on re-runs — regenerating a release must never silently un-yank
    # a build that was pulled for being broken or insecure.
    if prior and prior.get("yanked"):
        entry["yanked"] = True
    feed["versions"] = [v for v in feed.get("versions", []) if v.get("version") != args.version]
    feed["versions"].insert(0, entry)
    # Never promote a yanked build to "latest"; keep the previous non-yanked one.
    if not entry.get("yanked"):
        feed["latest"] = args.version
    elif not feed.get("latest"):
        feed["latest"] = args.version

    with open(args.feed, "w") as f:
        json.dump(feed, f, indent=2)
        f.write("\n")
    if kept:
        print(f"kept from the existing {args.version} entry: " +
              ", ".join(f"{a.get('os', 'macos')}/{a['arch']}" for a in kept))
    print(f"wrote {args.feed}: latest={args.version}, {len(artifacts)} artifact(s)")


if __name__ == "__main__":
    main()
