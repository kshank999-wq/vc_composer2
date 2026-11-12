// Copyright (c) VC Composer. All rights reserved.
//
// Loading a SMuFL music font and its metrics (spec 02 sections 5.1, 5.3).
//
// The reference font, Bravura, is embedded in every build (SIL Open Font
// License, which permits exactly that; resources/fonts/Bravura), so there is
// always a music font. A composer can still point at another SMuFL font on
// disk, and everything here still works with no font at all - which is why
// the renderer can draw without one.
//
// Metrics come from the font's own metadata, not from the built-in defaults,
// because advance widths differ between fonts and a layout that assumes one
// font's metrics is subtly wrong in another (see Smufl.h).
#pragma once

#include <cstddef>
#include <juce_gui_basics/juce_gui_basics.h>

#include "vcc/engraving/Smufl.h"

namespace vcc::ui {

/// What a load attempt achieved, and why it did not achieve more.
///
/// Not a bool: a font can load while its metadata does not, which is worth
/// telling the user about because the engraving will then be subtly
/// mis-spaced rather than broken.
struct MusicFontLoad {
    bool typeface_loaded = false;
    bool advances_loaded = false;
    bool bounding_boxes_loaded = false;
    bool codepoints_loaded = false;

    std::size_t glyphs_overridden = 0;

    /// Human-readable summary for the UI. Never empty.
    juce::String message;

    bool usable() const noexcept { return typeface_loaded; }
};

/// A loaded music font.
class MusicFont {
public:
    /// Loads a font file and applies whatever metadata is alongside it.
    ///
    /// `metadata_file` is the font's SMuFL metadata (`bravura_metadata.json`
    /// and its equivalents) and `glyph_names_file` is SMuFL's
    /// `glyphnames.json`. Either may be absent: the built-in defaults then
    /// stand in for that part, and the result says so.
    MusicFontLoad load(const juce::File& font_file, const juce::File& metadata_file,
                       const juce::File& glyph_names_file, engraving::GlyphTable& table);

    /// Loads a font from bytes already in memory, with its SMuFL metadata as
    /// JSON text. What the embedded font goes through: the same metadata
    /// application as `load`, without a file system in between.
    MusicFontLoad load_from_memory(const void* font_bytes, std::size_t font_size,
                                   const juce::String& metadata_json,
                                   engraving::GlyphTable& table);

    /// Looks for a font and its metadata in a directory, by the naming SMuFL
    /// fonts conventionally use. Returns what `load` returned, or a failure
    /// naming what was missing.
    MusicFontLoad load_from_directory(const juce::File& directory,
                                      engraving::GlyphTable& table);

    /// Uses a font already installed on the machine, by family name.
    ///
    /// The common case on a composer's machine and the one a file path
    /// cannot serve: Dorico, MuseScore and Sibelius all install their music
    /// font as an ordinary system font, and a composer who has one of those
    /// has the font without knowing where its file is.
    ///
    /// No metadata comes with it - a system font is a typeface and nothing
    /// else - so the built-in metrics stand in, and the result says so.
    /// That is the trade this exists to make: correct glyphs at slightly
    /// wrong advances, against a full metadata load that needs a path.
    MusicFontLoad load_installed(const juce::String& family_name,
                                 engraving::GlyphTable& table);

    /// Families this project looks for, in order of preference.
    ///
    /// Bravura is SMuFL's reference font and the one the built-in metrics
    /// were measured from, so it comes first; the rest are the fonts the
    /// common notation applications install. A name here is a name to try,
    /// not a claim that it is present.
    static juce::StringArray known_font_families();

    /// The first of `known_font_families` that is installed, or empty.
    static juce::String first_installed_font_family();

    void unload();

    bool is_loaded() const noexcept { return typeface_ != nullptr; }
    const juce::String& name() const noexcept { return name_; }

    /// The font at the size one staff space implies.
    ///
    /// A SMuFL font is designed on an em square equal to the staff height -
    /// four staff spaces - so the point size is four times the space size.
    /// This is the only conversion between staff spaces and font size, and
    /// getting it wrong scales every glyph in the score.
    juce::Font font_for_staff_space(double pixels_per_staff_space) const;

private:
    /// Applies whatever metadata was given and writes the summary. `result`
    /// arrives with `typeface_loaded` set; the rest is filled in here.
    void apply_metadata(const juce::var& metadata, const juce::var& glyph_names,
                        engraving::GlyphTable& table, MusicFontLoad& result) const;

    juce::Typeface::Ptr typeface_;
    juce::String name_;
};

/// Applies a SMuFL font metadata document's `glyphAdvanceWidths` to a table.
/// Returns how many of this engine's glyphs it covered.
///
/// A font's metadata describes thousands of glyphs; the ones this engine does
/// not use are skipped rather than reported as errors.
std::size_t apply_smufl_advance_widths(const juce::var& metadata,
                                       engraving::GlyphTable& table);

/// Applies `glyphBBoxes`. SMuFL states bounding boxes with y pointing up,
/// and this engine has y pointing down, so the box is flipped on the way in -
/// missing that inversion puts every glyph's bounds on the wrong side of its
/// origin, and hit testing with it.
std::size_t apply_smufl_bounding_boxes(const juce::var& metadata,
                                       engraving::GlyphTable& table);

/// Applies SMuFL's `glyphnames.json`, whose codepoints are written "U+E0A4".
std::size_t apply_smufl_codepoints(const juce::var& glyph_names,
                                   engraving::GlyphTable& table);

/// Parses a SMuFL codepoint string such as "U+E0A4". Returns 0 when the text
/// is not one, rather than guessing - a zero codepoint is what the layout
/// treats as a missing glyph.
char32_t parse_smufl_codepoint(const juce::String& text) noexcept;

}  // namespace vcc::ui
