#!/bin/bash
# notarize_macos.sh <signed .app> <out.zip>: Apple's notary service checks the app, its ticket is
# stapled to it, and the stapled app is zipped for players. Credentials, never on the command line:
#   - a notarytool keychain profile: NOTARY_PROFILE (default "xivlegacy"), made once with
#       xcrun notarytool store-credentials xivlegacy --apple-id <id> --team-id <team>
#     (it asks for an app-specific password); or
#   - an App Store Connect API key (CI): APPLE_API_KEY_PATH (the .p8), APPLE_API_KEY_ID, APPLE_API_ISSUER.
set -euo pipefail
app=$1 out=$2
tmp=$(mktemp -d)
# no extended attributes in the zips: ditto stores them as "._" AppleDouble entries, which
# Finder's Archive Utility turns back into attributes but other unzippers write out as files,
# and stray "._" files inside the app break its code seal ("unsealed contents")
zip_app() { ditto -c -k --norsrc --noextattr --noqtn --keepParent "$1" "$2"; }
zip_app "$app" "$tmp/submit.zip"
if [ -n "${APPLE_API_KEY_PATH:-}" ]; then
  auth=(--key "$APPLE_API_KEY_PATH" --key-id "$APPLE_API_KEY_ID" --issuer "$APPLE_API_ISSUER")
else
  auth=(--keychain-profile "${NOTARY_PROFILE:-xivlegacy}")
fi
xcrun notarytool submit "$tmp/submit.zip" "${auth[@]}" --wait --timeout 60m | tee "$tmp/result.txt"
grep -q "status: Accepted" "$tmp/result.txt" || {
  id=$(sed -n 's/^ *id: //p' "$tmp/result.txt" | head -1)
  [ -n "$id" ] && xcrun notarytool log "$id" "${auth[@]}" || true
  echo "notarization was not accepted" >&2; exit 1
}
xcrun stapler staple "$app"
spctl --assess --type execute --verbose=2 "$app"
mkdir -p "$(dirname "$out")"
zip_app "$app" "$out"
rm -rf "$tmp"
echo "notarized: $out"
