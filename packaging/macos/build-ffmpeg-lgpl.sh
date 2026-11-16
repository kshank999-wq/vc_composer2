#!/usr/bin/env bash
# Builds the LGPL FFmpeg that macOS releases bundle (docs/01 licence gate:
# LGPL, dynamically linked - never Homebrew's GPL build). Shared libraries
# with @rpath install names, no programs, no network, no external libraries
# but Apple's own (VideoToolbox, AudioToolbox, zlib).
#   packaging/macos/build-ffmpeg-lgpl.sh <prefix>
set -euo pipefail
VERSION="${FFMPEG_VERSION:-7.1.1}"
PREFIX="$1"
WORK="$(mktemp -d)"
cd "$WORK"
curl -fsSL "https://ffmpeg.org/releases/ffmpeg-${VERSION}.tar.xz" -o ffmpeg.tar.xz
tar xf ffmpeg.tar.xz
cd "ffmpeg-${VERSION}"
./configure --prefix="$PREFIX" --install-name-dir='@rpath' \
    --enable-shared --disable-static --enable-pic \
    --disable-gpl --disable-nonfree --disable-version3 \
    --disable-programs --disable-doc --disable-network --disable-debug \
    --disable-autodetect --enable-videotoolbox --enable-audiotoolbox --enable-zlib \
    --extra-cflags="-mmacosx-version-min=11.0" --extra-ldflags="-mmacosx-version-min=11.0"
make -j"$(sysctl -n hw.ncpu)"
make install
# The licence and where its source came from travel with the libraries.
mkdir -p "$PREFIX/share/licenses"
cp COPYING.LGPLv2.1 "$PREFIX/share/licenses/FFmpeg-LGPL-2.1.txt"
printf 'FFmpeg %s, LGPL 2.1, built unmodified from\nhttps://ffmpeg.org/releases/ffmpeg-%s.tar.xz\nwith: ./configure %s\n' \
    "$VERSION" "$VERSION" "$(grep -m1 'FFMPEG_CONFIGURATION' config.h | sed 's/.*FFMPEG_CONFIGURATION "\(.*\)"/\1/')" \
    > "$PREFIX/share/licenses/FFmpeg-SOURCE.txt"
