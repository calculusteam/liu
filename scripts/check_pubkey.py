#!/usr/bin/env python3
"""Assert the committed update public key matches a private key.

usage: check_pubkey.py <ed25519-private-key.pem> <src/core/update_pubkey.h>

Exits 0 if the 32-byte Ed25519 public key derived from the private key equals
the LIU_UPDATE_PUBKEY array in the header; non-zero otherwise. Run in CI so a
build can never ship a binary whose embedded key can't verify its own channel.
"""
import re
import subprocess
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: check_pubkey.py <privkey.pem> <update_pubkey.h>")
    priv, hdr = sys.argv[1], sys.argv[2]
    der = subprocess.check_output(["openssl", "pkey", "-in", priv, "-pubout", "-outform", "DER"])
    pub = der[-32:]  # raw key is the trailing 32 bytes of the SPKI DER
    text = open(hdr).read()
    m = re.search(r"LIU_UPDATE_PUBKEY\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}", text)
    if not m:
        sys.exit("error: LIU_UPDATE_PUBKEY array not found in " + hdr)
    arr = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))
    if arr == pub:
        print("pubkey OK (header matches private key)")
        return
    sys.exit("MISMATCH: header public key does not match the private key — "
             "regenerate src/core/update_pubkey.h")


if __name__ == "__main__":
    main()
