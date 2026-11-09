#!/usr/bin/env bash
# Find Liu.app bundles whose signing identity and bundle identifier can
# interfere with the installed app's privacy grants.
#
# macOS associates TCC grants with a bundle identifier and a code requirement.
# Developer ID signatures provide a stable requirement; ad-hoc signatures
# identify a specific code hash. An ad-hoc bundle that shares the release
# identifier can replace the TCC row used by the installed app, causing its
# permission requests to fail even when System Settings shows the grant enabled.
# A Developer ID requirement names the signing team, so a release-id bundle
# signed by another team than the installed app's does the same once it is run
# and granted, and the installed app's own grants stop matching after its
# signing team changes.
#
# Reset an affected ScreenCapture grant with:
#     tccutil reset ScreenCapture com.github.calculusteam.Liu
# Then grant it again from a Developer ID-signed build.
#
# Usage: scripts/check_macos_signing.sh Exit: 0 when clean, 1 when a bundle can
# interfere with the installed app's grants.
set -uo pipefail

RELEASE_ID="com.github.calculusteam.Liu"
bad=0
soft=0
other=0
found=0

# The team the installed app is signed by; the grants follow it.
installed_team="$(codesign -dv /Applications/Liu.app 2>&1 \
                    | sed -n 's/^TeamIdentifier=//p')"

# mdfind covers indexed volumes; the explicit paths cover the ones it misses
# (a freshly written build dir, a volume with indexing off).
paths=$( { mdfind -name "Liu.app" 2>/dev/null
           ls -d /Applications/Liu.app "$HOME/Applications/Liu.app" 2>/dev/null
         } | sort -u )

printf "%-14s %-12s %-34s %s\n" "SIGNATURE" "RUNTIME" "BUNDLE ID" "PATH"
printf '%.0s-' {1..110}; echo

while IFS= read -r app; do
    [[ -z "$app" ]] && continue
    [[ -d "$app" ]] || continue
    # Simulator bundles are a different product; skip them.
    case "$app" in *CoreSimulator*) continue;; esac
    found=$((found + 1))

    info="$(codesign -dv --verbose=2 "$app" 2>&1)"
    bid="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' \
              "$app/Contents/Info.plist" 2>/dev/null || echo '?')"
    flags="$(printf '%s' "$info" | sed -nE 's/.*flags=0x[0-9a-f]*\(([^)]*)\).*/\1/p' | head -1)"
    team="$(printf '%s' "$info" | sed -n 's/^TeamIdentifier=//p')"

    if printf '%s' "$info" | grep -q '^Authority=Developer ID'; then
        sig="DeveloperID"
    elif printf '%s' "$info" | grep -q 'flags=.*adhoc'; then
        sig="ad-hoc"
    elif printf '%s' "$info" | grep -q 'code object is not signed'; then
        sig="UNSIGNED"
    else
        sig="other"
    fi

    mark=""
    if [[ "$bid" == "$RELEASE_ID" && "$sig" != "DeveloperID" ]]; then
        mark="   <-- poisons the bundle id"
        bad=$((bad + 1))
    elif [[ "$bid" == "$RELEASE_ID" && -n "$installed_team" \
            && "$team" != "$installed_team" ]]; then
        mark="   <-- signed by team $team, not $installed_team"
        other=$((other + 1))
    elif [[ "$bid" == "$RELEASE_ID" && "${flags:-none}" != *runtime* ]]; then
        # A Developer ID-signed bundle with the release identifier but
        # without the hardened runtime can load injected libraries with
        # the installed app's permissions. Development builds use the .dev
        # identifier.
        mark="   <-- release id without the hardened runtime"
        soft=$((soft + 1))
    fi
    printf "%-14s %-12s %-34s %s%s\n" "$sig" "${flags:-none}" "$bid" "$app" "$mark"
done <<< "$paths"

echo
if [[ $found -eq 0 ]]; then
    echo "No Liu.app found."
    exit 0
fi

if [[ $soft -gt 0 ]]; then
    cat <<MSG
$soft bundle(s) carry the id "$RELEASE_ID" with a Developer ID signature but
without the hardened runtime. Code injected into one of them holds the
installed Liu.app's permissions. Delete them, or build them again: a dev build
now signs as "$RELEASE_ID.dev" (CMakeLists.txt, LIU_DEV_HOOKS).

MSG
fi

if [[ $other -gt 0 ]]; then
    cat <<MSG
$other bundle(s) carry the id "$RELEASE_ID" signed by another team than
/Applications/Liu.app ($installed_team). A grant given while one of them runs
names that team, and the installed app's requests then fail to match. A grant
given before the installed app's team changed fails the same way. Delete those
bundles or build them again under the .dev id, then clear the rows that fail
and grant once more:
  tccutil reset ScreenCapture $RELEASE_ID

To find the rows that fail (the service is at the end of each line):
  /usr/bin/log show --last 1h --predicate 'process == "tccd"' \\
    | grep "Failed to match existing code requirement for subject $RELEASE_ID"

MSG
fi

if [[ $bad -gt 0 ]]; then
    cat <<MSG
$bad bundle(s) carry the id "$RELEASE_ID" without a Developer ID signature.
The moment one of them is run, the installed Liu.app's TCC rows stop matching:
System Settings shows the switch ON, and macOS asks for the permission again at
every launch.

  1) Delete those bundles, or sign them again with the Developer ID:
       codesign --force --deep --sign "Developer ID Application: ..." <path>
  2) Clear the damaged rows and grant once more:
       tccutil reset ScreenCapture $RELEASE_ID
       tccutil reset Microphone    $RELEASE_ID

To verify (after granting, this line must NO LONGER appear):
  log show --last 5m --predicate 'subsystem == "com.apple.TCC"' \\
    | grep "Failed to match existing code requirement"
MSG
    exit 1
fi

if [[ $soft -gt 0 || $other -gt 0 ]]; then
    exit 1
fi
echo "Clean: every bundle carrying '$RELEASE_ID' is signed with the installed app's Developer ID team and hardened."
exit 0
