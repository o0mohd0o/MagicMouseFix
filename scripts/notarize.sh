#!/bin/bash
# Notarizes and staples build/MagicMouseFix.app so Gatekeeper opens it without
# warnings on other people's Macs. Needs a Developer ID-signed build and a
# stored notarytool profile. Create the profile once with:
#
#   xcrun notarytool store-credentials magicmousefix \
#       --apple-id you@example.com --team-id TEAMID --password <app-specific-password>
#
# (App-specific passwords: https://account.apple.com > Sign-In and Security.)
set -euo pipefail
cd "$(dirname "$0")/.."

PROFILE="${NOTARY_PROFILE:-magicmousefix}"
VERSION="${VERSION:-1.0.0}"
APP=build/MagicMouseFix.app
ZIP="build/MagicMouseFix-$VERSION.zip"
[ -d "$APP" ] || { echo "run scripts/build.sh first"; exit 1; }

xcrun notarytool submit "$ZIP" --keychain-profile "$PROFILE" --wait
xcrun stapler staple "$APP"
# Re-zip so the download carries the stapled ticket.
rm -f "$ZIP"
ditto -c -k --keepParent "$APP" "$ZIP"
spctl --assess --type execute --verbose=2 "$APP"
echo "==> notarized and stapled: $ZIP"
