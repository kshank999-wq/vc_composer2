// Copyright (c) VC Composer. All rights reserved.
//
// The music font built into every window this project draws (spec 02
// sections 5.1, 5.3).
//
// Bravura, SMuFL's reference font, and its metadata, compiled in as byte
// arrays (resources/fonts/Bravura, SIL Open Font License). It is what a
// composer sees on a machine where nothing was installed - which is every
// machine the plug-in is first tried on - and it is the font the engine's
// built-in metrics were measured from, so the two agree.
//
// Behind a function rather than exposed as arrays, so that the only place
// that knows the generated header's names is this file's implementation.
#pragma once

#include "vcc/ui/MusicFont.h"

#include <cstddef>

namespace vcc::ui {

/// Loads the embedded font and applies its metadata to `table`.
MusicFontLoad load_embedded_music_font(MusicFont& font, engraving::GlyphTable& table);

/// What the embedded font is called, for the diagnostics panel: "Bravura".
const char* embedded_music_font_name() noexcept;

/// The embedded font's size in bytes, for a test that wants to know the
/// arrays are the files and not a stub.
std::size_t embedded_music_font_size() noexcept;

/// The font file itself, for embedding in a printed page.
std::string embedded_music_font_bytes();

}  // namespace vcc::ui
