#!/usr/bin/env bash
# Makes "VC Composer.app" self-contained and, when a Developer ID is set
# up, signs, notarizes and staples it, then wraps it in a .dmg.
#   packaging/macos/bundle.sh <app> <ffmpeg-prefix> <out.dmg>
# Signing (all optional; without them the .dmg is unsigned, for testing):
#   MACOS_SIGNING_IDENTITY  "Developer ID Application: Name (TEAMID)" in the keychain
#   APPLE_ID, APPLE_TEAM_ID, APPLE_APP_PASSWORD  for notarytool
set -euo pipefail
APP="$1"
FFMPEG="$2"
DMG="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
MACOS="$APP/Contents/MacOS"
FRAMEWORKS="$APP/Contents/Frameworks"
mkdir -p "$FRAMEWORKS" "$APP/Contents/Resources/licenses"

# FFmpeg's dylibs (and the ones they load) into Frameworks; every executable
# looks there.
# Their install names are @rpath/lib<name>.<major>.dylib: copy those names,
# following the symlinks to the real files.
for lib in "$FFMPEG"/lib/lib*.dylib; do
    name="$(basename "$lib")"
    [[ "$name" =~ ^lib[a-z]+\.[0-9]+\.dylib$ ]] || continue
    cp -L "$lib" "$FRAMEWORKS/$name"
    chmod u+w "$FRAMEWORKS/$name"
done
for exe in "$MACOS"/*; do
    [ -f "$exe" ] && [ -x "$exe" ] || continue
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$exe" 2>/dev/null || true
done
cp "$FFMPEG"/share/licenses/* "$APP/Contents/Resources/licenses/" 2>/dev/null || true

# Nothing may still point outside the bundle (Homebrew, the build prefix).
bad=0
for f in "$MACOS"/* "$FRAMEWORKS"/*.dylib; do
    if otool -L "$f" | tail -n +2 | grep -E "/opt/homebrew|/usr/local|$FFMPEG" ; then
        echo "error: $f links outside the bundle"; bad=1
    fi
done
[ "$bad" = 0 ]

if [ -n "${MACOS_SIGNING_IDENTITY:-}" ]; then
    # Forgive how the name was pasted: stray spaces or line ends, a leading "CN=".
    MACOS_SIGNING_IDENTITY="$(printf '%s' "$MACOS_SIGNING_IDENTITY" | tr -d '\r\n' | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e 's/^CN=//')"
    identities="$(security find-identity -v -p codesigning)"
    if ! printf '%s' "$identities" | grep -qF "\"$MACOS_SIGNING_IDENTITY\""; then
        # Not that exact name. With exactly one Developer ID identity here, sign with it.
        devid="$(printf '%s\n' "$identities" | grep '"Developer ID Application:' || true)"
        if [ "$(printf '%s' "$devid" | grep -c . || true)" = 1 ]; then
            echo "warning: MACOS_SIGNING_IDENTITY does not match the certificate's name exactly;"
            echo "         signing with the one Developer ID Application identity in the keychain."
            MACOS_SIGNING_IDENTITY="$(printf '%s' "$devid" | awk '{print $2}')"
        else
            echo "error: no signing identity named as MACOS_SIGNING_IDENTITY. Identities here:"
            printf '%s\n' "$identities"
            exit 1
        fi
    fi
    sign() { codesign --force --timestamp --options runtime --entitlements "$HERE/entitlements.plist" \
                      --sign "$MACOS_SIGNING_IDENTITY" "$@"; }
    for f in "$FRAMEWORKS"/*.dylib; do sign "$f"; done
    for f in "$MACOS"/*; do [ "$(basename "$f")" = "VC Composer" ] || sign "$f"; done
    sign "$APP"
    codesign --verify --deep --strict --verbose=2 "$APP"
else
    echo "No MACOS_SIGNING_IDENTITY: the app is ad-hoc signed (right-click > Open on first launch)."
    codesign --force --deep --sign - "$APP"
fi

rm -f "$DMG"
STAGE="$(mktemp -d)"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "VC Composer" -srcfolder "$STAGE" -ov -format UDZO "$DMG"

if [ -n "${MACOS_SIGNING_IDENTITY:-}" ] && [ -n "${APPLE_ID:-}" ] && [ -n "${APPLE_APP_PASSWORD:-}" ]; then
    codesign --force --timestamp --sign "$MACOS_SIGNING_IDENTITY" "$DMG"
    auth=(--apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_PASSWORD")
    result="$(xcrun notarytool submit "$DMG" "${auth[@]}" --wait --output-format json)"
    echo "$result"
    id="$(printf '%s' "$result" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("id",""))')"
    status="$(printf '%s' "$result" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("status",""))')"
    if [ "$status" != "Accepted" ]; then
        echo "error: Apple's notarization says: $status. Its log:"
        [ -n "$id" ] && xcrun notarytool log "$id" "${auth[@]}" || true
        exit 1
    fi
    xcrun stapler staple "$DMG"
fi
