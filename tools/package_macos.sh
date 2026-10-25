#!/bin/bash
# package_macos.sh [destination folder]: XIVLegacy.app - the launcher (crates/launcher, the same program
# as on Windows) with the game host (build/xivlegacy-host, as Contents/Helpers/xivlegacy-host) and SDL3
# beside it, signed with the keychain's Developer ID if there is one, and installed into the destination
# (default /Applications). Build the host first: python3 tools/build.py host. For a release, build it
# against SDL's own framework (XIVLEGACY_SDL3_FRAMEWORK, see tools/build.py) so it runs on macOS 13+,
# package into a folder (tools/package_macos.sh dist) and notarize (tools/notarize_macos.sh).
#
# The signature: hardened runtime and a secure timestamp on everything, as notarization wants, and one
# entitlement, on the host only: library validation off, because the game module it loads is compiled
# on the player's machine, so no team signed it.
#
# The host holds none of the game's code: on first Join the launcher makes the game module from the
# player's own ffxivgame.exe (runtime/xiv_game.h), so the app can be published.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
dest=${1:-/Applications}
[ -x "$root/build/xivlegacy-host" ] || { echo "no build/xivlegacy-host: run python3 tools/build.py host first" >&2; exit 1; }
version=$(sed -n 's/^version = "\(.*\)"/\1/p' "$root/Cargo.toml" | head -1)
(cd "$root" && cargo build --release -p xivlegacy-launcher)
work=$(mktemp -d -t xivlegacy-app)
app="$work/XIVLegacy.app"
sdl=$(otool -L "$root/build/xivlegacy-host" | awk '/SDL3/ {print $1}')

mkdir -p "$app/Contents/MacOS" "$app/Contents/Helpers" "$app/Contents/Frameworks" "$app/Contents/Resources"
cp "$root/target/release/XIVLegacy" "$app/Contents/MacOS/XIVLegacy"
cp "$root/build/xivlegacy-host" "$app/Contents/Helpers/xivlegacy-host"
case "$sdl" in
@rpath/SDL3.framework/*)
  # SDL's framework (XIVLEGACY_SDL3_FRAMEWORK): the host already looks in ../Frameworks
  ditto "${XIVLEGACY_SDL3_FRAMEWORK:?the host links SDL3.framework: set XIVLEGACY_SDL3_FRAMEWORK}/SDL3.framework" \
    "$app/Contents/Frameworks/SDL3.framework"
  sdl_bin="$app/Contents/Frameworks/SDL3.framework" ;;
*)
  # Homebrew's dylib (local builds)
  cp "$sdl" "$app/Contents/Frameworks/libSDL3.0.dylib"
  chmod u+w "$app/Contents/Frameworks/libSDL3.0.dylib"
  install_name_tool -id "@rpath/libSDL3.0.dylib" "$app/Contents/Frameworks/libSDL3.0.dylib"
  install_name_tool -change "$sdl" "@executable_path/../Frameworks/libSDL3.0.dylib" "$app/Contents/Helpers/xivlegacy-host"
  sdl_bin="$app/Contents/Frameworks/libSDL3.0.dylib" ;;
esac

cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>XIVLegacy</string>
	<key>CFBundleDisplayName</key><string>XIVLegacy</string>
	<key>CFBundleIdentifier</key><string>com.xivlegacy.launcher</string>
	<key>CFBundleExecutable</key><string>XIVLegacy</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>$version</string>
	<key>CFBundleVersion</key><string>$version</string>
	<key>LSMinimumSystemVersion</key><string>13.0</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>
	<key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

id=${MACOS_SIGN_IDENTITY:-$(security find-identity -v -p codesigning | sed -n 's/.*"\(Developer ID Application: [^"]*\)".*/\1/p' | head -1)}
if [ -n "$id" ]; then
  echo "signing as: $id"
  ent=$(mktemp -t xivlegacy-host-entitlements).plist
  cat > "$ent" <<'ENT'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>com.apple.security.cs.disable-library-validation</key><true/>
</dict>
</plist>
ENT
  sign() { codesign --force --timestamp --options runtime --sign "$id" "$@"; }
  sign "$sdl_bin"
  sign --entitlements "$ent" "$app/Contents/Helpers/xivlegacy-host"
  sign "$app"
  rm -f "$ent"
else
  echo "no Developer ID in the keychain: ad-hoc signing (the keychain will ask again after each install)"
  codesign --force --deep --sign - "$app"
fi
codesign --verify --deep --strict "$app"

rm -rf "$dest/XIVLegacy.app"
ditto "$app" "$dest/XIVLegacy.app"
rm -rf "$work"
echo "installed: $dest/XIVLegacy.app ($version)"
