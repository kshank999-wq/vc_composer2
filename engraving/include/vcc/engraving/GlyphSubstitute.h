// Copyright (c) VC Composer. All rights reserved.
//
// Drawing a glyph without a music font (spec 02 sections 5.1, 5.3).
//
// Every back end faces the same problem: the music font may not be there. A
// build machine has none installed, a user may not have pointed the
// application at one yet, and a font that is present may still be missing a
// glyph. Drawing nothing in that case produces a page that looks empty for
// no stated reason, and the geometry - which is the part with the bugs in it
// - becomes invisible.
//
// So each glyph has a primitive stand-in: an ellipse for a notehead, a
// letter for a clef. The stand-ins live here rather than in a renderer
// because both the SVG and the on-screen back ends need them, and two
// independent sets would drift.
#pragma once

#include <cstdint>

#include "vcc/engraving/Smufl.h"

namespace vcc::engraving {

enum class SubstituteKind : std::uint8_t {
    /// Nothing sensible to draw.
    None = 0,

    /// A notehead: an ellipse spanning the glyph's advance width, centred on
    /// the origin, filled or open.
    FilledEllipse,
    OpenEllipse,

    /// A small filled circle, for an augmentation dot.
    Dot,

    /// An open rectangle, for a rest. `height` gives its height in staff
    /// spaces; the width is 0.7 of the advance.
    Box,

    /// A triangle hanging from the origin, for a flag. `height` is signed:
    /// negative points up, positive down.
    Triangle,

    /// ASCII text in the interface font, baselined a little below the glyph
    /// origin so it reads where the music glyph would have sat.
    Letters,
};

/// How to draw a glyph when the music font cannot.
struct GlyphSubstitute {
    SubstituteKind kind = SubstituteKind::None;

    /// For `Letters`: a short ASCII stand-in, never empty for that kind.
    const char* text = "";

    /// For `Box` and `Triangle`: extent in staff spaces, signed for a
    /// triangle.
    double height = 0.0;
};

/// The stand-in for a glyph.
GlyphSubstitute substitute_for(Glyph glyph) noexcept;

}  // namespace vcc::engraving
