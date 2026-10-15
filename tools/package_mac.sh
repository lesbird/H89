#!/bin/bash
# Builds a universal (Apple silicon + Intel) H89.app, signs it, notarizes it
# when credentials are available, and wraps it in dist/H89-<version>-macOS.dmg.
#
# Signing identity (H89_SIGN_IDENTITY):
#   "Developer ID Application: Your Name (TEAMID)" for a release; without
#   it the app is signed ad hoc and runs only after right-click › Open (or
#   System Settings › Privacy & Security › Open Anyway).
# Notarization, one of:
#   H89_NOTARY_PROFILE  a keychain profile made once with
#                       xcrun notarytool store-credentials <profile> --apple-id ... --team-id ...
#   APPLE_ID, APPLE_TEAM_ID, APPLE_APP_PASSWORD (an app-specific password)
# Notarization needs a Developer ID signature; it is skipped for ad hoc.
#
# BUILD_DIR (default build-release) and CMAKE (default cmake) can be set.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR=${BUILD_DIR:-build-release}
CMAKE=${CMAKE:-cmake}
IDENTITY=${H89_SIGN_IDENTITY:--}

"$CMAKE" -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
    -DH89_BUILD_TESTS=OFF
"$CMAKE" --build "$BUILD_DIR" --config Release --target H89 --parallel

APP=$(find "$BUILD_DIR" -name H89.app -type d -maxdepth 2 | head -1)
[ -n "$APP" ] || { echo "H89.app not found in $BUILD_DIR" >&2; exit 1; }
VERSION=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$APP/Contents/Info.plist")
lipo -info "$APP/Contents/MacOS/H89"

sign() {
    if [ "$IDENTITY" = "-" ]; then
        codesign --force --sign - "$1"
    else
        codesign --force --options runtime --timestamp --sign "$IDENTITY" "$1"
    fi
}

notarize() {
    [ "$IDENTITY" != "-" ] || return 0
    local args
    if [ -n "${H89_NOTARY_PROFILE:-}" ]; then
        args=(--keychain-profile "$H89_NOTARY_PROFILE")
    elif [ -n "${APPLE_ID:-}" ] && [ -n "${APPLE_TEAM_ID:-}" ] && [ -n "${APPLE_APP_PASSWORD:-}" ]; then
        args=(--apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_PASSWORD")
    else
        echo "No notarization credentials: skipping notarization of $1"
        return 0
    fi
    xcrun notarytool submit "$1" "${args[@]}" --wait
    xcrun stapler staple "$2"
}

echo "Signing $APP as ${IDENTITY/#-/ad hoc}"
sign "$APP"
codesign --verify --strict --verbose=2 "$APP"

# Notarize the app itself first, so its ticket is stapled inside the .dmg
# and it opens offline after being copied out.
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
ditto -c -k --keepParent "$APP" "$WORK/H89.zip"
notarize "$WORK/H89.zip" "$APP"

mkdir -p dist "$WORK/dmg"
ditto "$APP" "$WORK/dmg/H89.app"
ln -s /Applications "$WORK/dmg/Applications"
DMG="dist/H89-$VERSION-macOS.dmg"
rm -f "$DMG"
hdiutil create -volname "H89 $VERSION" -srcfolder "$WORK/dmg" -ov -format UDZO "$DMG" >/dev/null
[ "$IDENTITY" = "-" ] || sign "$DMG"
notarize "$DMG" "$DMG"

if [ "$IDENTITY" != "-" ]; then
    spctl --assess --type open --context context:primary-signature --verbose "$DMG" || true
fi
echo "Wrote $DMG"
