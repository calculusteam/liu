#!/usr/bin/env python3
"""Ed25519-sign the Liu update feed (appcast.json), in place.

The artifact signatures vouch for the BYTES of a download; they say nothing about which release
a client is told to fetch. An unsigned feed therefore let whoever controlled it offer an older,
genuinely signed (and vulnerable) build, or simply stop offering anything. This closes that: the
feed ends with one more member,

    ,"feed_signature":"<base64 of the 64-byte Ed25519 signature>"}

and the signature covers EVERY byte of the file before that marker, preceded by the context
string "liu-update-feed-v1\n". The context is what keeps a signature the same key made over a zip or
a manifest.json from ever passing as a feed. The same key that signs the artifacts signs the feed
(src/core/update_pubkey.h is the only trust anchor). The client
(src/update/updater.c, updater_feed_verify) splits at the LAST marker, requires nothing but the
closing brace after the signature, and checks the bytes in front of it.

Two members are added to the signed text:

  generated  epoch seconds. ALWAYS greater than the one in the feed being merged into, whatever
             the CI clock says, because a client remembers the newest one it has accepted and
             refuses an older feed — a clock that stepped back must not lock every client out.
  expires    only with --expires-days. Left out by default: the feed lives at
             releases/latest/download/ and is rewritten only by a release, so an expiry would
             make every client report "expired" after that many days without one. Set it only
             when something re-signs the feed on a schedule.

Layout is kept the way gen_feed.py writes it (indent=2, one key per line, ASCII only) because
scripts/install.sh reads the feed with awk. The signature member goes on the last line.

Usage:
  scripts/sign_feed.py --feed build/package/appcast.json --key "$RUNNER_TEMP/ed25519.key"
"""
import math
from verify_release import check_feed_times, check_feed_signature, FEED_TIME_MAX
from pathlib import Path
import argparse
import base64
import json
import os
import subprocess
import sys
import tempfile
import time

MARKER = ',"feed_signature":"'
# The release key also signs update ZIPs and manifest.json, so the feed signature includes a
# context prefix. Keep FEED_SIG_CONTEXT in src/update/updater.c identical.
CONTEXT = b"liu-update-feed-v1\n"
DROP = ("feed_signature", "generated", "expires")


def fail(msg):
    sys.exit(f"error: {msg}")


def openssl_sign(openssl, key, data):
    with tempfile.TemporaryDirectory() as td:
        msg = os.path.join(td, "msg")
        with open(msg, "wb") as f:
            f.write(data)
        r = subprocess.run([openssl, "pkeyutl", "-sign", "-inkey", key, "-rawin", "-in", msg],
                           capture_output=True)
        if r.returncode or len(r.stdout) != 64:
            fail(f"openssl could not sign the feed: {r.stderr.decode(errors='replace').strip()}")
        return r.stdout


def openssl_verify_own(openssl, key, data, sig):
    """Check what was just signed against the key's own public half — it catches a broken
    openssl or a key that is not an Ed25519 key before anything is published. That this key IS
    the one compiled into the app is scripts/check_pubkey.py's job and verify_release.py's."""
    with tempfile.TemporaryDirectory() as td:
        pub, msg, sg = (os.path.join(td, n) for n in ("pub.pem", "msg", "sig"))
        r = subprocess.run([openssl, "pkey", "-in", key, "-pubout", "-out", pub], capture_output=True)
        if r.returncode:
            fail(f"cannot derive the public key: {r.stderr.decode(errors='replace').strip()}")
        open(msg, "wb").write(data)
        open(sg, "wb").write(sig)
        r = subprocess.run([openssl, "pkeyutl", "-verify", "-pubin", "-inkey", pub, "-rawin",
                            "-sigfile", sg, "-in", msg], capture_output=True)
        if r.returncode:
            fail("the signature just produced does not verify against the key's own public half")


def main():
    ap = argparse.ArgumentParser(description="Sign the Liu update feed in place.")
    ap.add_argument("--feed", required=True, help="path to appcast.json (rewritten in place)")
    ap.add_argument("--key", required=True, help="Ed25519 private key (PEM)")
    ap.add_argument("--openssl", default="openssl")
    ap.add_argument("--expires-days", type=float, default=0,
                    help="add an 'expires' this many days after 'generated' (default: none)")
    ap.add_argument("--generated", type=int, default=0,
                    help="override the generation time (tests only; still forced above the prior one)")
    args = ap.parse_args()

    if not os.path.isfile(args.feed): fail(f"feed not found: {args.feed}")
    if not os.path.isfile(args.key): fail(f"key not found: {args.key}")
    try:
        feed = json.load(open(args.feed))
    except Exception as e:
        fail(f"{args.feed} is not valid JSON: {e}")
    if not isinstance(feed, dict) or not isinstance(feed.get("versions"), list):
        fail(f"{args.feed} is not a feed (an object with a versions array)")

    if "feed_signature" in feed:
        # Direct re-signing is also a prior-feed consumer. Validate the exact
        # original bytes before carrying its replay counter into a new signature.
        pub = subprocess.run([args.openssl, "pkey", "-in", args.key, "-pubout"],
                             capture_output=True, text=True)
        if pub.returncode:
            fail("cannot derive the signing key's public half")
        feed = check_feed_signature(args.openssl, pub.stdout, Path(args.feed))

    prior = feed.get("generated", 0)
    if "generated" in feed:
        check_feed_times(feed)
    if args.generated < 0 or args.generated >= FEED_TIME_MAX:
        fail("--generated is outside the client's generation-time bounds")
    if not math.isfinite(args.expires_days) or not 0 <= args.expires_days < FEED_TIME_MAX / 86400:
        fail("--expires-days must be finite, nonnegative and within client time bounds")
    for k in DROP:
        feed.pop(k, None)

    generated = max(args.generated or int(time.time()), prior + 1)
    feed["generated"] = generated
    if args.expires_days > 0:
        feed["expires"] = generated + int(args.expires_days * 86400)

    check_feed_times(feed)

    text = json.dumps(feed, indent=2)            # ensure_ascii stays on: the signed bytes are ASCII
    if not text.endswith("\n}"):
        fail("unexpected serialisation")
    prefix = text[:-2]                           # everything up to, not including, "\n}"
    data = prefix.encode("ascii")
    if MARKER.encode() in data:
        fail("the feed text already contains the signature marker")

    message = CONTEXT + data
    sig = openssl_sign(args.openssl, args.key, message)
    openssl_verify_own(args.openssl, args.key, message, sig)
    out = prefix + MARKER + base64.b64encode(sig).decode() + '"}\n'

    tmp = args.feed + ".tmp"
    with open(tmp, "w", newline="\n") as f:
        f.write(out)
    os.replace(tmp, args.feed)
    exp = f", expires={feed['expires']}" if "expires" in feed else ""
    print(f"signed {args.feed}: generated={generated}{exp}, {len(feed['versions'])} version(s)")


if __name__ == "__main__":
    main()
