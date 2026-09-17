#!/bin/bash
# Builds build/MagicMouseFix.app (universal) and build/MagicMouseFix-<version>.zip.
#
#   SIGN_IDENTITY="Developer ID Application: Name (TEAMID)" scripts/build.sh
#
# Without SIGN_IDENTITY the app is ad-hoc signed: fine for your own Mac, but
# macOS will then forget the Input Monitoring permission on every rebuild.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${VERSION:-1.0.0}"
SIGN_IDENTITY="${SIGN_IDENTITY:--}"
# Works with just the Command Line Tools; a full Xcode is not required.
TOOLS="$(xcode-select -p 2>/dev/null || true)"
if [ ! -x "$TOOLS/usr/bin/clang" ] || ! "$TOOLS/usr/bin/clang" --version >/dev/null 2>&1; then
    TOOLS=/Library/Developer/CommandLineTools
fi
CLANG="$TOOLS/usr/bin/clang"
SWIFTC="$TOOLS/usr/bin/swiftc"
SDK="${SDKROOT:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}"
[ -d "$SDK" ] || SDK="$(xcrun --show-sdk-path)"

APP=build/MagicMouseFix.app
rm -rf build && mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

echo "==> compiling (arm64 + x86_64)"
"$CLANG" -O2 -Wall -isysroot "$SDK" -arch arm64 -arch x86_64 -mmacosx-version-min=13.0 \
    -o "$APP/Contents/MacOS/MagicMouseFix" src/MagicMouseFix.c \
    -framework IOKit -framework CoreFoundation

echo "==> icon"
if [ ! -f assets/icon-1024.png ]; then
    "$SWIFTC" -sdk "$SDK" -O -o build/make-icon scripts/make-icon.swift 2>/dev/null
    build/make-icon assets/icon-1024.png
fi
ICONSET=build/AppIcon.iconset
mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
    sips -z $s $s assets/icon-1024.png --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
    d=$((s * 2))
    sips -z $d $d assets/icon-1024.png --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"
rm -rf "$ICONSET" build/make-icon

sed "s/__VERSION__/$VERSION/g" assets/Info.plist > "$APP/Contents/Info.plist"

echo "==> signing with: $SIGN_IDENTITY"
if [ "$SIGN_IDENTITY" = "-" ]; then
    codesign --force --sign - "$APP"
else
    codesign --force --options runtime --timestamp --sign "$SIGN_IDENTITY" "$APP"
fi
codesign --verify --strict --verbose=2 "$APP"

ZIP="build/MagicMouseFix-$VERSION.zip"
ditto -c -k --keepParent "$APP" "$ZIP"
echo "==> built $APP"
echo "==> packaged $ZIP"
