#!/usr/bin/env python3
"""Fail-closed verifier for the signed bytes about to be published."""
import argparse, base64, hashlib, json, re, subprocess, sys, tempfile
from pathlib import Path
from urllib.parse import urlsplit

def fail(message):
    raise SystemExit(f"error: {message}")

def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def public_key_pem(header):
    values = re.findall(r"0x([0-9a-fA-F]{2})", header.read_text())
    if len(values) != 32: fail(f"expected 32 public-key bytes in {header}")
    der = bytes.fromhex("302a300506032b6570032100") + bytes.fromhex("".join(values))
    body = base64.b64encode(der).decode()
    return "-----BEGIN PUBLIC KEY-----\n" + "\n".join(body[i:i+64] for i in range(0, len(body), 64)) + "\n-----END PUBLIC KEY-----\n"

def verify_signature(openssl, pem, data, signature, what):
    try:
        raw = base64.b64decode(signature.strip(), validate=True)
    except Exception as exc:
        fail(f"{what} signature is not base64: {exc}")
    if len(raw) != 64: fail(f"{what} signature is {len(raw)} bytes, not 64")
    with tempfile.TemporaryDirectory() as td:
        td = Path(td); key = td / "pub.pem"; sig = td / "sig.bin"
        key.write_text(pem); sig.write_bytes(raw)
        result = subprocess.run([openssl, "pkeyutl", "-verify", "-pubin", "-inkey", str(key), "-rawin", "-sigfile", str(sig), "-in", str(data)], capture_output=True, text=True)
        if result.returncode: fail(f"Ed25519 verification failed for {what}: {result.stderr.strip()}")

FEED_TIME_MAX = 4_000_000_000_000  # updater.c uses the same exclusive bound

def check_feed_times(feed):
    for name in ("generated", "expires"):
        if name == "expires" and name not in feed: continue
        value = feed.get(name)
        if not isinstance(value, int) or isinstance(value, bool) or not 0 < value < FEED_TIME_MAX:
            fail(f"appcast.json has invalid {name!r} — clients require an integer between 0 and {FEED_TIME_MAX}")

def feed_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result: fail(f"appcast.json has duplicate member {key!r}")
        result[key] = value
    return result

FEED_MARKER = ',"feed_signature":"'
FEED_CONTEXT = b"liu-update-feed-v1\n"   # keep in lockstep with updater.c FEED_SIG_CONTEXT and sign_feed.py

def check_feed_signature(openssl, pem, feed_path):
    """The feed's own signature, split the way the client splits it (updater_feed_verify): the
    LAST marker, nothing but the closing brace after the signature, the signed bytes in front."""
    raw = feed_path.read_bytes()
    i = raw.rfind(FEED_MARKER.encode())
    if i < 0: fail("appcast.json is not signed (no feed_signature) — run scripts/sign_feed.py")
    rest = raw[i + len(FEED_MARKER):]
    q = rest.find(b'"')
    if q < 0 or q > 128 or rest[q + 1:q + 2] != b"}" or rest[q + 2:].strip(b" \t\r\n"):
        fail("appcast.json: the signature member is not the last thing in the file")
    with tempfile.TemporaryDirectory() as td:
        body = Path(td) / "feed.signed"
        body.write_bytes(FEED_CONTEXT + raw[:i])
        verify_signature(openssl, pem, body, rest[:q].decode("ascii", "replace"), "appcast.json")
    try:
        feed = json.loads(raw.decode(), object_pairs_hook=feed_object)
    except (ValueError, UnicodeError) as exc:
        fail(f"appcast.json is not valid JSON: {exc}")
    if not isinstance(feed, dict) or not isinstance(feed.get("versions"), list):
        fail("appcast.json is not a feed object with a versions array")
    check_feed_times(feed)
    return feed

def check_manifest_signature(openssl, pem, path):
    """Authenticate the exact prior manifest bytes, not metadata re-read after verification."""
    if not path.is_file(): fail(f"manifest not found: {path}")
    sig_path = Path(str(path) + ".sig")
    if not sig_path.is_file(): fail(f"no signature for manifest: {sig_path}")
    raw = path.read_bytes()
    with tempfile.TemporaryDirectory() as td:
        body = Path(td) / "manifest.signed"
        body.write_bytes(raw)
        verify_signature(openssl, pem, body, sig_path.read_text(), "manifest.json")
    def members(pairs):
        result = {}
        for key, value in pairs:
            if key in result: fail(f"manifest.json has duplicate member {key!r}")
            result[key] = value
        return result
    try:
        manifest = json.loads(raw.decode(), object_pairs_hook=members)
    except (ValueError, UnicodeError) as exc:
        fail(f"manifest.json is not valid JSON: {exc}")
    if (not isinstance(manifest, dict) or not isinstance(manifest.get("artifacts"), list) or
            not isinstance(manifest.get("version"), str) or not manifest["version"]):
        fail("manifest.json is not a manifest object with a version and artifacts array")
    return manifest

def release_entry(feed, version):
    entries = feed.get("versions", [])
    if any(not isinstance(v, dict) for v in entries):
        fail("appcast.json has a non-object version entry")
    matches = [v for v in entries if v.get("version") == version]
    if len(matches) > 1: fail(f"appcast.json has duplicate entries for version {version}")
    return matches[0] if matches else None

def feed_artifact_pair(item):
    if not isinstance(item, dict): fail("feed artifact is not an object")
    os_name, arch = item.get("os", "macos"), item.get("arch")
    if os_name not in ("macos", "linux") or not isinstance(arch, str) or not arch:
        fail(f"feed artifact has invalid (os, arch): {os_name!r}/{arch!r}")
    return os_name, arch

def manifest_artifact_pair(item):
    if not isinstance(item, dict): fail("manifest artifact is not an object")
    kind, arch = item.get("kind"), item.get("arch")
    if not isinstance(kind, str) or not kind or not isinstance(arch, str) or not arch:
        fail(f"manifest artifact has invalid (kind, arch): {kind!r}/{arch!r}")
    return kind, arch

def artifact_metadata(item, label):
    size = item.get("size")
    if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
        fail(f"{label}: invalid size (expected a positive integer)")
    digest = item.get("sha256")
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
        fail(f"{label}: invalid sha256 (expected 64 hex characters)")
    url = item.get("url")
    try:
        parts = urlsplit(url) if isinstance(url, str) else None
        if (parts is None or not url.startswith("https://") or not parts.hostname or
                parts.username is not None or parts.password is not None or parts.query or parts.fragment or
                "\\" in url or any(c.isspace() or ord(c) < 32 for c in url)):
            fail(f"{label}: invalid URL (expected an absolute HTTPS artifact URL)")
        parts.port  # Validate the port syntax as well as the host.
    except ValueError as exc:
        fail(f"{label}: invalid URL: {exc}")
    filename = parts.path.rsplit("/", 1)[-1]
    if not filename or filename in (".", "..") or "\\" in filename:
        fail(f"{label}: invalid artifact filename in URL")
    return filename

def check_feed_artifact(item, label):
    os_name, arch = feed_artifact_pair(item)
    label = f"{label} {os_name}/{arch}"
    filename = artifact_metadata(item, label)
    want = ".tar.gz" if os_name == "linux" else ".zip"
    if not filename.endswith(want): fail(f"{label}: artifact is not a {want}: {item['url']}")
    signature = item.get("ed25519")
    if not isinstance(signature, str): fail(f"{label}: missing or invalid ed25519 signature")
    try:
        raw = base64.b64decode(signature, validate=True)
    except ValueError as exc:
        fail(f"{label}: ed25519 signature is not base64: {exc}")
    if len(raw) != 64: fail(f"{label}: ed25519 signature is {len(raw)} bytes, not 64")
    return filename

def manifest_artifacts(manifest):
    """Validate identities and metadata before a carried entry can be re-signed."""
    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, list): fail("manifest.json has no artifacts array")
    by_pair, filenames = {}, set()
    for item in artifacts:
        pair = manifest_artifact_pair(item)
        label = f"manifest {pair[0]}/{pair[1]}"
        if pair in by_pair: fail(f"duplicate manifest artifact for {pair[0]}/{pair[1]}")
        filename = artifact_metadata(item, label)
        if item.get("filename") != filename: fail(f"{label}: filename does not match URL")
        if filename in filenames: fail(f"manifest has duplicate filename {filename!r}")
        by_pair[pair] = item
        filenames.add(filename)
    return by_pair

def check_manifest_feed(manifest, entry):
    """Require coverage by platform/kind AND URL/hash/size, using authenticated inputs."""
    by_pair = manifest_artifacts(manifest)
    artifacts = entry.get("artifacts")
    if not isinstance(artifacts, list): fail("feed release has no artifacts array")
    seen = set()
    for item in artifacts:
        pair = feed_artifact_pair(item)
        if pair in seen: fail(f"feed has two artifacts for the same (os, arch): {pair[0]}/{pair[1]}")
        seen.add(pair)
        filename = check_feed_artifact(item, "feed")
        os_name, arch = pair
        manifest_pair = ("tarball", "linux-" + arch) if os_name == "linux" else ("zip", arch)
        other = by_pair.get(manifest_pair)
        if (other is None or other["filename"] != filename or other["url"] != item["url"] or
                other["sha256"] != item["sha256"] or other["size"] != item["size"]):
            fail(f"feed/manifest disagree for {os_name}/{arch} ({filename}): missing artifact or URL/sha256/size mismatch")

def check_artifact(root, item, base_url, openssl, pem, label):
    filename = artifact_metadata(item, label)
    path = root / filename
    if not filename or not path.is_file(): fail(f"{label}: missing artifact {filename!r}")
    if item.get("url") != f"{base_url}/{filename}": fail(f"{label}: unexpected URL for {filename}")
    if item.get("size") != path.stat().st_size: fail(f"{label}: size mismatch for {filename}")
    if item.get("sha256") != sha256(path): fail(f"{label}: SHA-256 mismatch for {filename}")
    if "ed25519" in item: verify_signature(openssl, pem, path, item["ed25519"], filename)
    return filename, item["sha256"], item["size"]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--feed", help="verify an original feed before merging it")
    ap.add_argument("--package-dir"); ap.add_argument("--version"); ap.add_argument("--base-url")
    ap.add_argument("--pubkey-header", default="src/core/update_pubkey.h"); ap.add_argument("--openssl", default="openssl")
    args = ap.parse_args()
    pem = public_key_pem(Path(args.pubkey_header))
    if args.feed:
        check_feed_signature(args.openssl, pem, Path(args.feed))
        return
    if not args.package_dir or not args.version or not args.base_url:
        ap.error("--package-dir, --version and --base-url are required unless --feed is used")
    root = Path(args.package_dir)
    feed_path, manifest_path, manifest_sig = root / "appcast.json", root / "manifest.json", root / "manifest.json.sig"
    for path in (feed_path, manifest_path, manifest_sig):
        if not path.is_file(): fail(f"missing {path}")
    feed = check_feed_signature(args.openssl, pem, feed_path)
    manifest = check_manifest_signature(args.openssl, pem, manifest_path)
    if feed.get("latest") != args.version or manifest.get("version") != args.version: fail("feed/manifest version does not match release version")
    entry = release_entry(feed, args.version)
    if not entry or entry.get("yanked"): fail("release is missing from feed or yanked")
    if not isinstance(entry.get("artifacts"), list): fail("feed release has no artifacts array")
    for item in entry["artifacts"]: check_feed_artifact(item, "feed")
    manifest_artifacts(manifest)
    for item in entry["artifacts"]: check_artifact(root, item, args.base_url, args.openssl, pem, "feed")
    # (os, arch) pairs, not arches: a set of arches alone cannot tell "two macOS
    # builds" from "one macOS and one Linux", so it would pass a release missing
    # an entire platform. An artifact with no "os" is macOS, the same default
    # updater.c and install.sh apply.
    seen_pairs = {(a.get("os", "macos"), a.get("arch")) for a in entry.get("artifacts", [])}
    required_pairs = {("macos", "arm64"), ("macos", "x86_64"), ("linux", "arm64"), ("linux", "x86_64")}
    missing = required_pairs - seen_pairs
    if missing: fail("feed is missing " + ", ".join(f"{o}/{c}" for o, c in sorted(missing)))
    if len(seen_pairs) != len(entry.get("artifacts", [])): fail("feed has two artifacts for the same (os, arch)")
    manifest_items = [check_artifact(root, a, args.base_url, args.openssl, pem, "manifest") for a in manifest.get("artifacts", [])]
    # Every file the release page carries: a DMG and an update zip per macOS arch; the
    # updater's tarball, a .deb, an .rpm and an AppImage per Linux arch.
    required_kinds = ({(k, a) for k in ("dmg", "zip") for a in ("arm64", "x86_64")} |
                      {(k, a) for k in ("tarball", "deb", "rpm", "appimage") for a in ("linux-arm64", "linux-x86_64")})
    kinds = [(a.get("kind"), a.get("arch")) for a in manifest.get("artifacts", [])]
    if set(kinds) != required_kinds or len(kinds) != len(required_kinds):
        fail("manifest must list, per arch, the macOS DMG and zip and the Linux tarball, .deb, .rpm and AppImage "
             f"(missing: {sorted(required_kinds - set(kinds))}, unexpected: {sorted(set(kinds) - required_kinds)})")
    check_manifest_feed(manifest, entry)
    print(f"release bundle verified: {args.version} ({len(manifest_items)} artifacts)")

if __name__ == "__main__": main()
