// Copyright (c) VC Composer. All rights reserved.
//
// Finding a music font at run time (spec 02 sections 5.1, 5.3).
//
// Four places are tried, in this order, because they answer four different
// situations:
//
//   1. A directory the composer named. Explicit beats clever, and it is the
//      only answer for a font installed somewhere unusual - and the way to
//      use a font other than the embedded one.
//   2. The embedded Bravura, with its metadata, built into every window this
//      project draws (resources/fonts/Bravura). This is what a composer sees
//      unless they asked for something else: the same font on every machine,
//      with nothing to install and nothing to find.
//   3. The conventional SMuFL directories for this platform, and
//   4. a font already installed as an ordinary system font.
//
// The last two are reached only if the embedded font fails to load, which
// it has no ordinary way of doing. They are kept because they are the right
// fallbacks if it ever did, and because the composer's-directory search
// is built out of the same pieces. Before the font was embedded they were
// the only way to a real font, and the case they served - a composer with
// Dorico's or MuseScore's font installed and nothing else - is now served
// better by the embedded one, which brings its metadata with it.
//
// The platform directories are this project's reading of SMuFL's
// recommendation and have **not** been checked against a machine with a
// font installed in them; see `docs/aax/10-notation-view-design.md`. They
// are a list rather than code for that reason: correcting one is an edit to
// data.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "vcc/ui/MusicFont.h"

namespace vcc::ui {

/// Where a music font was found, if it was.
enum class MusicFontSource : std::uint8_t {
    None = 0,
    /// A directory the composer named.
    NamedDirectory,
    /// The Bravura built into this binary, with its metadata.
    Embedded,
    /// One of the platform's conventional SMuFL directories.
    ConventionalDirectory,
    /// A font already installed on the system, without metadata.
    InstalledFont,
};

const char* to_string(MusicFontSource source) noexcept;

/// What the search did.
struct MusicFontSearchResult {
    MusicFontSource source = MusicFontSource::None;
    MusicFontLoad load{};

    /// The directory or family name it came from, for the diagnostics panel.
    /// Never a full path when the source is an installed font.
    juce::String from;

    /// Every place that was looked in and what was there, in order. The
    /// point of a search that can fail is being able to say where it looked.
    juce::StringArray attempts;

    bool found() const noexcept { return load.usable(); }
};

/// The conventional SMuFL font directories for this platform, in order.
///
/// Empty on a platform this project has no reading for, which is a better
/// answer than a guessed path.
juce::Array<juce::File> conventional_music_font_directories();

/// Looks for a music font and loads the first one found.
///
/// `named_directory` is the composer's own, and may be empty. `table`
/// receives whatever metrics the font brought with it. With the embedded
/// font in every build this always finds one unless that font cannot be
/// loaded, so `found()` being false is news.
MusicFontSearchResult find_music_font(MusicFont& font, engraving::GlyphTable& table,
                                      const juce::File& named_directory = {});

/// The directory named by `VCC_MUSIC_FONT_DIR`, or nothing.
///
/// An environment variable because it is the one setting that has to work
/// before any settings file is read - including inside a plug-in, which has
/// no settings file of its own and is started by the host.
juce::File music_font_directory_from_environment();

}  // namespace vcc::ui
