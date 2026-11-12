// Copyright (c) VC Composer. All rights reserved.
//
// The renderer-agnostic output of layout (spec 02 sections 4.3, 5.1, 5.3).
//
// `LayoutEngine` produces one of these and never draws. Same seam, and same
// reason, as `UiState` in the plug-in core: the geometry is then testable
// without a graphics context, a window, or a font installed, and one layout
// can be drawn by more than one back end - the JUCE plug-in window, and SVG
// for verification and export.
//
// Coordinates are in staff spaces throughout. A renderer multiplies by its
// own scale; nothing here knows about pixels.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vcc/engraving/Geometry.h"
#include "vcc/engraving/Smufl.h"
#include "vcc/notation/Score.h"

namespace vcc::engraving {

/// What an item belongs to, so a renderer can style by role and hit testing
/// can filter by what the user could have meant to click.
enum class ItemRole : std::uint8_t {
    StaffLine = 0,
    Barline,
    Clef,
    KeySignature,
    TimeSignature,
    Notehead,
    Stem,
    Flag,
    Beam,
    LedgerLine,
    Accidental,
    AugmentationDot,
    Rest,
    Tie,
    Slur,
    Hairpin,
    Dynamic,
    Articulation,
    TupletBracket,
    RulerMark,
    RulerLabel,
    Cursor,
    Text,

    /// The acciaccatura's stroke through a grace note's stem.
    GraceSlash,
};

const char* to_string(ItemRole role) noexcept;

/// A glyph from the music font, positioned at its origin.
struct GlyphItem {
    Glyph glyph = Glyph::None;
    Point origin{};
    ItemRole role = ItemRole::Notehead;

    /// The score event this came from, or `kInvalidEventId` for structural
    /// items. This is what lets a click resolve to a note, and what lets a
    /// selection highlight the right glyphs.
    notation::EventId source_id = notation::kInvalidEventId;

    /// Bounding box, filled in by layout from the glyph metrics. Hit
    /// testing uses it, so it is not optional.
    Rect bounds{};

    /// Scale relative to the normal staff size: 0.6 for a cue-sized glyph.
    double scale = 1.0;
};

/// A straight line: staff lines, barlines, stems, ledger lines, beams drawn
/// as thin quadrilaterals when horizontal.
struct LineItem {
    Point from{};
    Point to{};
    double thickness = 0.125;
    ItemRole role = ItemRole::StaffLine;
    notation::EventId source_id = notation::kInvalidEventId;
};

/// A filled quadrilateral, for beams, which are sloped and so cannot be
/// expressed as a stroked line of constant thickness without distortion at
/// the ends.
struct QuadItem {
    Point corners[4]{};
    ItemRole role = ItemRole::Beam;
    notation::EventId source_id = notation::kInvalidEventId;
};

/// A cubic Bezier, for ties and slurs.
struct CurveItem {
    Point start{};
    Point control_one{};
    Point control_two{};
    Point end{};

    /// Thickness at the middle. A tapered curve thins to `end_thickness`
    /// at its ends, as an engraver's slur or tie does; an untapered one is
    /// a stroke of `thickness` throughout.
    double thickness = 0.14;
    bool tapered = false;
    double end_thickness = 0.05;

    ItemRole role = ItemRole::Tie;
    notation::EventId source_id = notation::kInvalidEventId;
};

/// Text not drawn from the music font: bar numbers, tempo marks, playing
/// techniques. Dynamics use music-font glyphs instead, since "mf" in a text
/// font does not look like a dynamic.
struct TextItem {
    std::string text;
    Point origin{};
    /// Size in staff spaces, so text scales with the staff.
    double size = 1.6;
    bool italic = false;
    bool bold = false;
    ItemRole role = ItemRole::Text;
    notation::EventId source_id = notation::kInvalidEventId;
};

/// Everything to draw, plus what layout learned while producing it.
struct DisplayList {
    std::vector<GlyphItem> glyphs;
    std::vector<LineItem> lines;
    std::vector<QuadItem> quads;
    std::vector<CurveItem> curves;
    std::vector<TextItem> texts;

    /// Union of every item's bounds, in staff spaces.
    Rect bounds{};

    /// Glyphs whose codepoint was unknown and so were not emitted. Reported
    /// rather than drawn as a blank, so a font missing what this engine
    /// needs is visible instead of producing silent gaps.
    std::vector<Glyph> missing_glyphs;

    /// True when the shortest note in view is narrower than a notehead, so
    /// the UI can suggest zooming rather than showing overlapping glyphs
    /// (see TimelineMap).
    bool is_crowded = false;

    std::size_t item_count() const noexcept
    {
        return glyphs.size() + lines.size() + quads.size() + curves.size() + texts.size();
    }

    bool empty() const noexcept { return item_count() == 0; }

    void clear();

    /// Adds an item and grows `bounds`.
    void add(const GlyphItem& item);
    void add(const LineItem& item);
    void add(const QuadItem& item);
    void add(const CurveItem& item);
    void add(const TextItem& item);

    /// Every item belonging to a score event, for selection highlighting.
    std::vector<Rect> bounds_for_event(notation::EventId id) const;
};

}  // namespace vcc::engraving
