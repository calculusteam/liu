# Release manifest (machine-written)

`manifest.json` + `manifest.json.sig` are written by `scripts/release.sh` at
every publish and committed here afterwards — do not hand-edit them.

The pair is a signed checksum record of every published release asset: the
detached Ed25519 signature is verified against the embedded public key before
any checksum here is trusted. It has no automated consumer: the in-app updater
and `install.sh` both read `appcast.json`, never this file. Keeping the pair in
the repo (instead of on the release page) keeps the release assets down to what
users actually download.

The pair is absent until the first release of the 0.1.0 line is published.
