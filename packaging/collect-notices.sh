#!/usr/bin/env bash
# Writes THIRD-PARTY-NOTICES.txt (and the licences it refers to) into a
# folder that ships with the app.   packaging/collect-notices.sh <dir> [ffmpeg-licence-dir]
set -euo pipefail
OUT="$1"
FFLIC="${2:-}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$OUT"
cp "$ROOT/packaging/licenses/EULA.txt" "$OUT/"
N="$OUT/THIRD-PARTY-NOTICES.txt"
{
  echo "VC Composer - third-party software"
  echo "===================================="
  echo
  echo "JUCE - used under a commercial JUCE licence held by VC Software."
  echo
  echo "Steinberg VST3 SDK - VST is a trademark of Steinberg Media Technologies GmbH."
  echo "The VST3 SDK is used under its licence as distributed with JUCE."
  echo
  echo "FFmpeg (libavformat, libavcodec, libavutil, libswscale) - LGPL 2.1 or later."
  echo "These libraries are separate files beside the program (Windows) or in"
  echo "Contents/Frameworks (macOS) and may be replaced with compatible builds."
  echo "Source: https://ffmpeg.org/download.html - the exact version and build"
  echo "settings are in FFmpeg-SOURCE.txt where present; on request we provide"
  echo "the corresponding source for three years: https://vc-software.com/support"
  echo
  echo "TinySoundFont - Copyright (C) 2017-2025 Bernhard Schelling, based on SFZero,"
  echo "Copyright (C) 2012 Steve Folta. MIT licence (desktop/third_party/tsf/tsf.h)."
  echo "Changed: samples are stored 16-bit."
  echo
  echo "stb_vorbis - Sean Barrett. Public domain (or MIT)."
  echo
  echo "Built-in sounds (downloaded on first launch, with their own CREDITS.txt):"
  echo "Salamander Grand Piano by Alexander Holm, CC-BY 3.0 (reduced edition);"
  echo "MuseScore General by S. Christian Collins, MIT."
  echo
  echo "ASIO is a trademark and software of Steinberg Media Technologies GmbH."
  echo
  for font in Bravura BarlowCondensed Inter ShareTechMono; do
    f="$ROOT/resources/fonts/$font/OFL.txt"
    [ -f "$f" ] || continue
    echo "----------------------------------------------------------------------"
    echo "Font: $font - SIL Open Font License 1.1"
    echo "----------------------------------------------------------------------"
    cat "$f"
    echo
  done
} > "$N"
if [ -n "$FFLIC" ] && [ -d "$FFLIC" ]; then cp "$FFLIC"/* "$OUT/"; fi
echo "notices in $OUT"
