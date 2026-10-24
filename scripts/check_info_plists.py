#!/usr/bin/env python3
"""Compare user-facing Info.plist entries across Liu's packaging configurations.

    scripts/package_macos_dmg.sh   release bundle Contents/Info.plist
    scripts/run_dev_macos.sh       development bundle
    CMakeLists.txt                 plist embedded in the bare binary

The structural keys differ because a bare binary has no icon file. Privacy
usage descriptions and Services menu entries must agree across all three
configurations. The script exits 1 when an entry differs.

Usage: check_info_plists.py [repo root]
"""
import re
import sys
from pathlib import Path

SOURCES = ("scripts/package_macos_dmg.sh", "scripts/run_dev_macos.sh", "CMakeLists.txt")
# Fail if any required entry is missing from the extracted plist values.
REQUIRED = ("NSMicrophoneUsageDescription", "NSDocumentsFolderUsageDescription",
            "NSLocalNetworkUsageDescription", "services", "web-content-http")


def texts(source: str) -> dict:
    found = dict(re.findall(r"<key>(NS\w+UsageDescription)</key>\s*<string>(.*?)</string>", source, re.S))
    services = re.findall(r"<key>(NSMessage|NSPortName|default)</key>\s*<string>(.*?)</string>", source, re.S)
    if services:
        found["services"] = services
    # App Transport Security permits a web view to load plain HTTP without an
    # exception only from an IP address, a single-label host, or .local. Add
    # this key so development servers on private network hostnames can load in
    # every plist configuration.
    if re.search(r"<key>NSAllowsArbitraryLoadsInWebContent</key>\s*<true/>", source):
        found["web-content-http"] = "true"
    return found


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    per_file = {name: texts((root / name).read_text(encoding="utf-8")) for name in SOURCES}
    bad = 0
    for name, found in per_file.items():
        for key in REQUIRED:
            if key not in found:
                print(f"check_info_plists: {name} has no {key}", file=sys.stderr)
                bad += 1
    for key in sorted(set().union(*per_file.values())):
        values = [per_file[name].get(key) for name in SOURCES]
        if any(value != values[0] for value in values):
            bad += 1
            print(f"check_info_plists: {key} differs:", file=sys.stderr)
            for name, value in zip(SOURCES, values):
                print(f"    {name}: {value!r}", file=sys.stderr)
    if bad:
        return 1
    count = len(set().union(*per_file.values()))
    print(f"check_info_plists: {count} user-visible entries agree across {len(SOURCES)} sources")
    return 0


if __name__ == "__main__":
    sys.exit(main())
