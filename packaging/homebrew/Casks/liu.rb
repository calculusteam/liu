# Liu — Homebrew cask (GUI .app, so a cask, not a formula).
#
# Lives in the self-hosted tap calculusteam/homebrew-liu:
#   brew install --cask calculusteam/liu/liu
#
# A self-hosted tap (not homebrew/cask) is required because Liu is currently
# AD-HOC signed (no Apple Developer ID). homebrew/cask rejects unsigned apps and
# forbids stripping quarantine; a private tap allows the postflight below, which
# clears the quarantine xattr so Gatekeeper doesn't block the first launch.
#
# version + sha256 are rewritten on every release by scripts/update-manifests.sh
# from liu.software/manifest.json — do not hand-edit them.
cask "liu" do
  version "0.1.0"

  on_arm do
    sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    url "https://liu.software/downloads/v#{version}/Liu-#{version}-arm64.dmg"
  end
  on_intel do
    sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    url "https://liu.software/downloads/v#{version}/Liu-#{version}-x86_64.dmg"
  end

  name "Liu"
  desc "macOS-native terminal built for AI-assisted coding workflows"
  homepage "https://liu.software"

  app "Liu.app"

  # Ad-hoc signature: Homebrew quarantines downloads, and Gatekeeper blocks an
  # ad-hoc app the first time. Strip the quarantine xattr so `brew install`
  # yields a launchable app. Remove this once a Developer ID / notarization
  # lands (then the cask can move toward homebrew/cask).
  postflight do
    system_command "/usr/bin/xattr",
                   args: ["-dr", "com.apple.quarantine", "#{appdir}/Liu.app"],
                   sudo: false
  end

  zap trash: [
    "~/.config/Liu",
    "~/.config/liu",
    "~/Library/Application Support/Liu",
    "~/Library/Saved Application State/com.github.calculusteam.Liu.savedState",
  ]
end
