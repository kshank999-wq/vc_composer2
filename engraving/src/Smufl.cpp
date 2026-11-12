#include "vcc/engraving/Smufl.h"

namespace vcc::engraving {
namespace {

struct GlyphEntry {
    Glyph glyph;
    const char* name;
    char32_t codepoint;
    double advance;
};

/// Default glyph table.
///
/// Codepoints are the SMuFL Private Use Area assignments; advance widths are
/// conventional approximations in staff spaces, not measurements from any
/// particular font. Both should be replaced from the font's metadata -
/// `GlyphTable::has_font_metrics()` reports whether that has happened, and
/// the layout is metrically correct only when it has.
const GlyphEntry kDefaults[] = {
    // Clefs.
    {Glyph::GClef,                   "gClef",                     0xE050, 2.6},
    {Glyph::FClef,                   "fClef",                     0xE062, 2.8},
    {Glyph::CClef,                   "cClef",                     0xE05C, 2.8},
    {Glyph::GClef8vb,                "gClef8vb",                  0xE052, 2.6},
    {Glyph::GClef8va,                "gClef8va",                  0xE053, 2.6},
    {Glyph::FClef8vb,                "fClef8vb",                  0xE064, 2.8},
    {Glyph::UnpitchedPercussionClef, "unpitchedPercussionClef1",  0xE069, 2.0},

    // Noteheads. A black notehead is about 1.18 spaces wide in Bravura, and
    // the whole layout's horizontal rhythm follows from this number.
    {Glyph::NoteheadDoubleWhole, "noteheadDoubleWhole", 0xE0A0, 2.2},
    {Glyph::NoteheadWhole,       "noteheadWhole",       0xE0A2, 1.7},
    {Glyph::NoteheadHalf,        "noteheadHalf",        0xE0A3, 1.18},
    {Glyph::NoteheadBlack,       "noteheadBlack",       0xE0A4, 1.18},

    // Flags.
    {Glyph::Flag8thUp,    "flag8thUp",    0xE240, 0.9},
    {Glyph::Flag8thDown,  "flag8thDown",  0xE241, 0.9},
    {Glyph::Flag16thUp,   "flag16thUp",   0xE242, 0.9},
    {Glyph::Flag16thDown, "flag16thDown", 0xE243, 0.9},
    {Glyph::Flag32ndUp,   "flag32ndUp",   0xE244, 0.9},
    {Glyph::Flag32ndDown, "flag32ndDown", 0xE245, 0.9},
    {Glyph::Flag64thUp,   "flag64thUp",   0xE246, 0.9},
    {Glyph::Flag64thDown, "flag64thDown", 0xE247, 0.9},

    // Rests.
    {Glyph::RestDoubleWhole, "restDoubleWhole", 0xE4E2, 1.6},
    {Glyph::RestWhole,       "restWhole",       0xE4E3, 1.6},
    {Glyph::RestHalf,        "restHalf",        0xE4E4, 1.6},
    {Glyph::RestQuarter,     "restQuarter",     0xE4E5, 1.1},
    {Glyph::Rest8th,         "rest8th",         0xE4E6, 1.0},
    {Glyph::Rest16th,        "rest16th",        0xE4E7, 1.2},
    {Glyph::Rest32nd,        "rest32nd",        0xE4E8, 1.4},
    {Glyph::Rest64th,        "rest64th",        0xE4E9, 1.6},

    // Accidentals.
    {Glyph::AccidentalFlat,        "accidentalFlat",              0xE260, 0.8},
    {Glyph::AccidentalNatural,     "accidentalNatural",           0xE261, 0.7},
    {Glyph::AccidentalSharp,       "accidentalSharp",             0xE262, 1.0},
    {Glyph::AccidentalDoubleSharp, "accidentalDoubleSharp",       0xE263, 1.0},
    {Glyph::AccidentalDoubleFlat,  "accidentalDoubleFlat",        0xE264, 1.4},

    {Glyph::AugmentationDot, "augmentationDot", 0xE1E7, 0.4},

    // Time signature digits, consecutive from 0.
    {Glyph::TimeSig0, "timeSig0", 0xE080, 1.3},
    {Glyph::TimeSig1, "timeSig1", 0xE081, 1.3},
    {Glyph::TimeSig2, "timeSig2", 0xE082, 1.3},
    {Glyph::TimeSig3, "timeSig3", 0xE083, 1.3},
    {Glyph::TimeSig4, "timeSig4", 0xE084, 1.3},
    {Glyph::TimeSig5, "timeSig5", 0xE085, 1.3},
    {Glyph::TimeSig6, "timeSig6", 0xE086, 1.3},
    {Glyph::TimeSig7, "timeSig7", 0xE087, 1.3},
    {Glyph::TimeSig8, "timeSig8", 0xE088, 1.3},
    {Glyph::TimeSig9, "timeSig9", 0xE089, 1.3},

    // Dynamics are composed from these three letterforms: "mf" is
    // DynamicMezzo followed by DynamicForte.
    {Glyph::DynamicPiano, "dynamicPiano", 0xE520, 1.2},
    {Glyph::DynamicMezzo, "dynamicMezzo", 0xE521, 1.8},
    {Glyph::DynamicForte, "dynamicForte", 0xE522, 1.1},

    // Articulations, above and below forms.
    {Glyph::ArticAccentAbove,         "articAccentAbove",         0xE4A0, 1.2},
    {Glyph::ArticAccentBelow,         "articAccentBelow",         0xE4A1, 1.2},
    {Glyph::ArticStaccatoAbove,       "articStaccatoAbove",       0xE4A2, 0.4},
    {Glyph::ArticStaccatoBelow,       "articStaccatoBelow",       0xE4A3, 0.4},
    {Glyph::ArticTenutoAbove,         "articTenutoAbove",         0xE4A4, 1.0},
    {Glyph::ArticTenutoBelow,         "articTenutoBelow",         0xE4A5, 1.0},
    {Glyph::ArticMarcatoAbove,        "articMarcatoAbove",        0xE4AC, 1.0},
    {Glyph::ArticMarcatoBelow,        "articMarcatoBelow",        0xE4AD, 1.0},
    {Glyph::ArticStaccatissimoAbove,  "articStaccatissimoAbove",  0xE4A6, 0.5},
    {Glyph::ArticStaccatissimoBelow,  "articStaccatissimoBelow",  0xE4A7, 0.5},

    {Glyph::Fermata,         "fermataAbove",   0xE4C0, 1.6},
    {Glyph::Tremolo1,        "tremolo1",       0xE220, 1.0},
    {Glyph::StringsUpBow,    "stringsUpBow",   0xE612, 1.0},
    {Glyph::StringsDownBow,  "stringsDownBow", 0xE610, 1.2},
    {Glyph::OrnamentTrill,   "ornamentTrill",  0xE566, 1.6},
};

}  // namespace

const char* smufl_name(Glyph glyph) noexcept
{
    for (const GlyphEntry& entry : kDefaults) {
        if (entry.glyph == glyph)
            return entry.name;
    }
    return "";
}

Glyph time_signature_digit(int digit) noexcept
{
    if (digit < 0 || digit > 9)
        return Glyph::None;
    return static_cast<Glyph>(static_cast<std::uint16_t>(Glyph::TimeSig0)
                              + static_cast<std::uint16_t>(digit));
}

bool glyph_from_smufl_name(const std::string& name, Glyph& out) noexcept
{
    for (const GlyphEntry& entry : kDefaults) {
        if (name == entry.name) {
            out = entry.glyph;
            return true;
        }
    }
    return false;
}

GlyphTable::GlyphTable()
{
    for (const GlyphEntry& entry : kDefaults) {
        GlyphMetrics metrics;
        metrics.codepoint = entry.codepoint;
        metrics.advance_width = entry.advance;
        // A conventional bounding box centred vertically on the origin. Real
        // boxes come from the font; these are enough for coarse layout and
        // for hit testing to have something to work with.
        metrics.bbox_left = 0.0;
        metrics.bbox_right = entry.advance;
        metrics.bbox_top = -0.5;
        metrics.bbox_bottom = 0.5;
        metrics_[entry.glyph] = metrics;
    }
}

const GlyphMetrics& GlyphTable::metrics(Glyph glyph) const noexcept
{
    static const GlyphMetrics fallback{};
    const auto found = metrics_.find(glyph);
    return found == metrics_.end() ? fallback : found->second;
}

void GlyphTable::set_metrics(Glyph glyph, const GlyphMetrics& metrics)
{
    metrics_[glyph] = metrics;
    ++overridden_;
}

bool GlyphTable::set_metrics_by_name(const std::string& name, const GlyphMetrics& metrics)
{
    Glyph glyph = Glyph::None;
    if (!glyph_from_smufl_name(name, glyph))
        return false;  // Not one of ours: the common case, not a failure.
    set_metrics(glyph, metrics);
    return true;
}

bool GlyphTable::is_complete(Glyph* first_missing) const noexcept
{
    for (const GlyphEntry& entry : kDefaults) {
        const auto found = metrics_.find(entry.glyph);
        if (found == metrics_.end() || found->second.codepoint == 0) {
            if (first_missing != nullptr)
                *first_missing = entry.glyph;
            return false;
        }
    }
    return true;
}

}  // namespace vcc::engraving
