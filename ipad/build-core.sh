#!/usr/bin/env bash
# Builds the shared C++ core (vcc_mobile and everything it uses: session,
# project, notation, engraving, editing...) for iPadOS devices and the
# simulator, and packs it as ipad/build/VCCCore.xcframework for the app.
# Run on a Mac with Xcode. The sources are the desktop's own: nothing here
# is iPad-specific but the toolchain flags.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
out="$here/build"
min_os="${IPAD_MIN_OS:-17.0}"
rm -rf "$out/VCCCore.xcframework" "$out/headers"
mkdir -p "$out/headers"

build() {  # name sysroot archs
    local dir="$out/$1"
    cmake -S "$root" -B "$dir" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$2" -DCMAKE_OSX_ARCHITECTURES="$3" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$min_os" \
        -DVCC_BUILD_TESTS=OFF -DVCC_BUILD_TOOLS=OFF -DVCC_BUILD_DESKTOP=OFF >/dev/null
    cmake --build "$dir" --target vcc_mobile --parallel "$(sysctl -n hw.ncpu)"
    # One static library holding every core library the facade links.
    local libs
    libs=$(find "$dir/lib" -name 'libvcc_*.a' | sort)
    libtool -static -o "$dir/libVCCCore.a" $libs 2>/dev/null
    echo "$1: $(echo "$libs" | wc -l | tr -d ' ') libraries -> $dir/libVCCCore.a"
}

build iphoneos iphoneos arm64
build iphonesimulator iphonesimulator "arm64;x86_64"

cp -R "$root/mobile/include/" "$out/headers/"
xcodebuild -create-xcframework \
    -library "$out/iphoneos/libVCCCore.a" -headers "$out/headers" \
    -library "$out/iphonesimulator/libVCCCore.a" -headers "$out/headers" \
    -output "$out/VCCCore.xcframework"
echo "Built $out/VCCCore.xcframework"
