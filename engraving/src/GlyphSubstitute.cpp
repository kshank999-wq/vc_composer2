#include "vcc/engraving/GlyphSubstitute.h"

namespace vcc::engraving {

GlyphSubstitute substitute_for(Glyph glyph) noexcept
{
    switch (glyph) {
        // Noteheads. Open for a half and longer, which is the one notehead
        // distinction a reader needs at a glance.
        case Glyph::NoteheadBlack:
            return GlyphSubstitute{SubstituteKind::FilledEllipse, "", 0.0};
        case Glyph::NoteheadHalf:
        case Glyph::NoteheadWhole:
        case Glyph::NoteheadDoubleWhole:
            return GlyphSubstitute{SubstituteKind::OpenEllipse, "", 0.0};

        case Glyph::AugmentationDot:
            return GlyphSubstitute{SubstituteKind::Dot, "", 0.0};

        // Rests, sized roughly as the real glyphs are: a whole rest is a
        // flat block, a quarter rest is tall.
        case Glyph::RestDoubleWhole:
        case Glyph::RestWhole:
        case Glyph::RestHalf:
            return GlyphSubstitute{SubstituteKind::Box, "", 0.5};
        case Glyph::RestQuarter:
            return GlyphSubstitute{SubstituteKind::Box, "", 1.6};
        case Glyph::Rest8th:
            return GlyphSubstitute{SubstituteKind::Box, "", 1.2};
        case Glyph::Rest16th:
            return GlyphSubstitute{SubstituteKind::Box, "", 1.4};
        case Glyph::Rest32nd:
            return GlyphSubstitute{SubstituteKind::Box, "", 1.6};
        case Glyph::Rest64th:
            return GlyphSubstitute{SubstituteKind::Box, "", 1.8};

        // Flags, pointing the way they hang.
        case Glyph::Flag8thUp:
        case Glyph::Flag16thUp:
        case Glyph::Flag32ndUp:
        case Glyph::Flag64thUp:
            return GlyphSubstitute{SubstituteKind::Triangle, "", -1.2};
        case Glyph::Flag8thDown:
        case Glyph::Flag16thDown:
        case Glyph::Flag32ndDown:
        case Glyph::Flag64thDown:
            return GlyphSubstitute{SubstituteKind::Triangle, "", 1.2};

        // Clefs, by the letter each one is named for.
        case Glyph::GClef:
        case Glyph::GClef8va:
        case Glyph::GClef8vb:
            return GlyphSubstitute{SubstituteKind::Letters, "G", 0.0};
        case Glyph::FClef:
        case Glyph::FClef8vb:
            return GlyphSubstitute{SubstituteKind::Letters, "F", 0.0};
        case Glyph::CClef:
            return GlyphSubstitute{SubstituteKind::Letters, "C", 0.0};
        case Glyph::UnpitchedPercussionClef:
            return GlyphSubstitute{SubstituteKind::Letters, "P", 0.0};

        case Glyph::AccidentalSharp:
            return GlyphSubstitute{SubstituteKind::Letters, "#", 0.0};
        case Glyph::AccidentalFlat:
            return GlyphSubstitute{SubstituteKind::Letters, "b", 0.0};
        case Glyph::AccidentalNatural:
            return GlyphSubstitute{SubstituteKind::Letters, "n", 0.0};
        case Glyph::AccidentalDoubleSharp:
            return GlyphSubstitute{SubstituteKind::Letters, "x", 0.0};
        case Glyph::AccidentalDoubleFlat:
            return GlyphSubstitute{SubstituteKind::Letters, "bb", 0.0};

        case Glyph::TimeSig0: return GlyphSubstitute{SubstituteKind::Letters, "0", 0.0};
        case Glyph::TimeSig1: return GlyphSubstitute{SubstituteKind::Letters, "1", 0.0};
        case Glyph::TimeSig2: return GlyphSubstitute{SubstituteKind::Letters, "2", 0.0};
        case Glyph::TimeSig3: return GlyphSubstitute{SubstituteKind::Letters, "3", 0.0};
        case Glyph::TimeSig4: return GlyphSubstitute{SubstituteKind::Letters, "4", 0.0};
        case Glyph::TimeSig5: return GlyphSubstitute{SubstituteKind::Letters, "5", 0.0};
        case Glyph::TimeSig6: return GlyphSubstitute{SubstituteKind::Letters, "6", 0.0};
        case Glyph::TimeSig7: return GlyphSubstitute{SubstituteKind::Letters, "7", 0.0};
        case Glyph::TimeSig8: return GlyphSubstitute{SubstituteKind::Letters, "8", 0.0};
        case Glyph::TimeSig9: return GlyphSubstitute{SubstituteKind::Letters, "9", 0.0};

        case Glyph::DynamicPiano: return GlyphSubstitute{SubstituteKind::Letters, "p", 0.0};
        case Glyph::DynamicMezzo: return GlyphSubstitute{SubstituteKind::Letters, "m", 0.0};
        case Glyph::DynamicForte: return GlyphSubstitute{SubstituteKind::Letters, "f", 0.0};

        // Articulations, by the mark they are written as.
        case Glyph::ArticAccentAbove:
        case Glyph::ArticAccentBelow:
            return GlyphSubstitute{SubstituteKind::Letters, ">", 0.0};
        case Glyph::ArticStaccatoAbove:
        case Glyph::ArticStaccatoBelow:
            return GlyphSubstitute{SubstituteKind::Letters, ".", 0.0};
        case Glyph::ArticTenutoAbove:
        case Glyph::ArticTenutoBelow:
            return GlyphSubstitute{SubstituteKind::Letters, "-", 0.0};
        case Glyph::ArticMarcatoAbove:
        case Glyph::ArticMarcatoBelow:
            return GlyphSubstitute{SubstituteKind::Letters, "^", 0.0};
        case Glyph::ArticStaccatissimoAbove:
        case Glyph::ArticStaccatissimoBelow:
            return GlyphSubstitute{SubstituteKind::Letters, "'", 0.0};

        case Glyph::Fermata:        return GlyphSubstitute{SubstituteKind::Letters, "U", 0.0};
        case Glyph::Tremolo1:       return GlyphSubstitute{SubstituteKind::Letters, "z", 0.0};
        case Glyph::StringsUpBow:   return GlyphSubstitute{SubstituteKind::Letters, "V", 0.0};
        case Glyph::StringsDownBow: return GlyphSubstitute{SubstituteKind::Letters, "n", 0.0};
        case Glyph::OrnamentTrill:  return GlyphSubstitute{SubstituteKind::Letters, "tr", 0.0};

        case Glyph::None:
            break;
    }
    return GlyphSubstitute{};
}

}  // namespace vcc::engraving
