#!/bin/bash
# package_macos.sh [destination folder]: XIVLegacy.app - the launcher (crates/launcher, the same program
# as on Windows) with the game host (build/xivlegacy-host, as Contents/Helpers/xivlegacy-host) and SDL3
# beside it, signed with the keychain's Developer ID if there is one, and installed into the destination
# (default /Applications). Build the host first: python3 tools/build.py host.
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
sdl=$(otool -L "$root/build/xivlegacy-host" | awk '/libSDL3/ {print $1}')

mkdir -p "$app/Contents/MacOS" "$app/Contents/Helpers" "$app/Contents/Frameworks" "$app/Contents/Resources"
cp "$root/target/release/XIVLegacy" "$app/Contents/MacOS/XIVLegacy"
cp "$root/build/xivlegacy-host" "$app/Contents/Helpers/xivlegacy-host"
cp "$sdl" "$app/Contents/Frameworks/libSDL3.0.dylib"
chmod u+w "$app/Contents/Frameworks/libSDL3.0.dylib"
install_name_tool -id "@rpath/libSDL3.0.dylib" "$app/Contents/Frameworks/libSDL3.0.dylib"
install_name_tool -change "$sdl" "@executable_path/../Frameworks/libSDL3.0.dylib" "$app/Contents/Helpers/xivlegacy-host"

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
  codesign --force --timestamp --sign "$id" "$app/Contents/Frameworks/libSDL3.0.dylib"
  codesign --force --timestamp --sign "$id" "$app/Contents/Helpers/xivlegacy-host"
  codesign --force --timestamp --sign "$id" "$app"
else
  echo "no Developer ID in the keychain: ad-hoc signing (the keychain will ask again after each install)"
  codesign --force --deep --sign - "$app"
fi
codesign --verify --deep --strict "$app"

rm -rf "$dest/XIVLegacy.app"
ditto "$app" "$dest/XIVLegacy.app"
rm -rf "$work"
echo "installed: $dest/XIVLegacy.app ($version)"
