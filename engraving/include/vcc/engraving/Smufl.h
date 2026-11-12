// Copyright (c) VC Composer. All rights reserved.
//
// SMuFL glyph identification (spec 02 sections 5.1, 5.3).
//
// Glyphs are referred to by their canonical SMuFL name, never by a
// hard-coded codepoint in layout code. SMuFL maps musical symbols into
// Unicode's Private Use Area and is the format-independent standard; its
// reference font, Bravura, is under the SIL Open Font License.
//
// The codepoints and advance widths below are DEFAULTS. SMuFL fonts ship a
// metadata file mapping names to codepoints along with per-glyph advance
// widths and anchor points, and `GlyphTable` takes those as overrides.
// That is not a nicety: advance widths differ between fonts, so a layout
// that assumes one font's metrics is subtly wrong in another, and taking
// them from the font is the only way to be correct.
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace vcc::engraving {

/// The glyphs this engine uses.
///
/// Deliberately a small set: everything spec 5.3 asks for and nothing
/// speculative. Each maps to a canonical SMuFL name.
enum class Glyph : std::uint16_t {
    None = 0,

    // --- Clefs --------------------------------------------------------------
    GClef,
    FClef,
    CClef,
    GClef8vb,
    GClef8va,
    FClef8vb,
    UnpitchedPercussionClef,

    // --- Noteheads ----------------------------------------------------------
    NoteheadDoubleWhole,
    NoteheadWhole,
    NoteheadHalf,
    NoteheadBlack,

    // --- Stems and flags ----------------------------------------------------
    Flag8thUp,
    Flag8thDown,
    Flag16thUp,
    Flag16thDown,
    Flag32ndUp,
    Flag32ndDown,
    Flag64thUp,
    Flag64thDown,

    // --- Rests --------------------------------------------------------------
    RestDoubleWhole,
    RestWhole,
    RestHalf,
    RestQuarter,
    Rest8th,
    Rest16th,
    Rest32nd,
    Rest64th,

    // --- Accidentals --------------------------------------------------------
    AccidentalFlat,
    AccidentalNatural,
    AccidentalSharp,
    AccidentalDoubleSharp,
    AccidentalDoubleFlat,

    // --- Augmentation -------------------------------------------------------
    AugmentationDot,

    // --- Time signature digits ----------------------------------------------
    TimeSig0,
    TimeSig1,
    TimeSig2,
    TimeSig3,
    TimeSig4,
    TimeSig5,
    TimeSig6,
    TimeSig7,
    TimeSig8,
    TimeSig9,

    // --- Dynamics -----------------------------------------------------------
    DynamicPiano,
    DynamicMezzo,
    DynamicForte,

    // --- Articulations ------------------------------------------------------
    ArticAccentAbove,
    ArticAccentBelow,
    ArticStaccatoAbove,
    ArticStaccatoBelow,
    ArticTenutoAbove,
    ArticTenutoBelow,
    ArticMarcatoAbove,
    ArticMarcatoBelow,
    ArticStaccatissimoAbove,
    ArticStaccatissimoBelow,

    // --- Ornaments and other ------------------------------------------------
    Fermata,
    Tremolo1,
    StringsUpBow,
    StringsDownBow,
    OrnamentTrill,
};

/// Canonical SMuFL name, as the specification and every font's metadata use
/// it. This is the identifier that matters; the codepoint is a lookup.
const char* smufl_name(Glyph glyph) noexcept;

/// The glyph for a time-signature digit 0-9.
Glyph time_signature_digit(int digit) noexcept;

/// The glyph a canonical SMuFL name refers to.
///
/// Returns false when the name is not one this engine uses, which is the
/// common case when walking a font's metadata: a SMuFL font describes
/// thousands of glyphs and this engine uses a few dozen.
bool glyph_from_smufl_name(const std::string& name, Glyph& out) noexcept;

/// Per-glyph metrics, in staff spaces.
struct GlyphMetrics {
    /// Codepoint in the font. 0 means unknown, which the layout treats as a
    /// missing glyph rather than drawing something arbitrary.
    char32_t codepoint = 0;

    /// Horizontal advance, in staff spaces. Used for spacing accidentals,
    /// stacking chords and placing dots.
    double advance_width = 1.0;

    /// Bounding box, in staff spaces, relative to the glyph origin. y is
    /// positive downward, as everywhere else here.
    double bbox_left = 0.0;
    double bbox_top = -0.5;
    double bbox_right = 1.0;
    double bbox_bottom = 0.5;

    double width() const noexcept { return bbox_right - bbox_left; }
    double height() const noexcept { return bbox_bottom - bbox_top; }
};

/// Resolves glyphs to codepoints and metrics.
///
/// Starts from built-in defaults and accepts per-glyph overrides taken from
/// a font's SMuFL metadata, which is the only way to be metrically correct
/// in a font other than the one the defaults were measured from.
class GlyphTable {
public:
    /// Built-in defaults.
    GlyphTable();

    const GlyphMetrics& metrics(Glyph glyph) const noexcept;

    /// Overrides one glyph, as a font's metadata would.
    void set_metrics(Glyph glyph, const GlyphMetrics& metrics);

    /// Overrides by SMuFL name, which is the form metadata arrives in.
    /// Returns false when the name is not one this engine uses - not an
    /// error, since a font's metadata describes thousands of glyphs.
    bool set_metrics_by_name(const std::string& smufl_name, const GlyphMetrics& metrics);

    /// True when every glyph has a usable codepoint. A false result names
    /// the first missing glyph in `first_missing`, so a font that cannot
    /// render this engine's output is reported rather than silently
    /// producing gaps.
    bool is_complete(Glyph* first_missing = nullptr) const noexcept;

    /// True when metrics came from a font rather than from the defaults.
    bool has_font_metrics() const noexcept { return overridden_ > 0; }
    std::size_t overridden_count() const noexcept { return overridden_; }

private:
    std::map<Glyph, GlyphMetrics> metrics_;
    std::size_t overridden_ = 0;
};

}  // namespace vcc::engraving
