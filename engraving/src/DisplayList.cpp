#include "vcc/engraving/DisplayList.h"

#include <algorithm>

namespace vcc::engraving {
namespace {

Rect rect_from_line(const LineItem& line)
{
    const double left = std::min(line.from.x, line.to.x);
    const double top = std::min(line.from.y, line.to.y);
    const double width = std::abs(line.to.x - line.from.x);
    const double height = std::abs(line.to.y - line.from.y);

    // A perfectly horizontal or vertical line has zero extent in one axis;
    // give it the line's thickness so its bounds are hit-testable.
    return Rect{left, top - line.thickness * 0.5, std::max(width, line.thickness),
                std::max(height, line.thickness)};
}

Rect rect_from_quad(const QuadItem& quad)
{
    double left = quad.corners[0].x;
    double right = quad.corners[0].x;
    double top = quad.corners[0].y;
    double bottom = quad.corners[0].y;

    for (int index = 1; index < 4; ++index) {
        left = std::min(left, quad.corners[index].x);
        right = std::max(right, quad.corners[index].x);
        top = std::min(top, quad.corners[index].y);
        bottom = std::max(bottom, quad.corners[index].y);
    }
    return Rect{left, top, right - left, bottom - top};
}

Rect rect_from_curve(const CurveItem& curve)
{
    // The control points bound a Bezier, so their bounding box contains the
    // curve. Looser than the true extent, which is the right trade here:
    // computing the exact hull costs more than it saves for hit testing.
    double left = std::min({curve.start.x, curve.control_one.x, curve.control_two.x, curve.end.x});
    double right = std::max({curve.start.x, curve.control_one.x, curve.control_two.x, curve.end.x});
    double top = std::min({curve.start.y, curve.control_one.y, curve.control_two.y, curve.end.y});
    double bottom =
        std::max({curve.start.y, curve.control_one.y, curve.control_two.y, curve.end.y});

    return Rect{left, top - curve.thickness, right - left,
                (bottom - top) + 2.0 * curve.thickness};
}

Rect rect_from_text(const TextItem& text)
{
    // Without a text measuring context, estimate from the character count.
    // Half the point size per character is a conventional approximation for
    // a proportional face, and the origin is the text baseline.
    const double width = static_cast<double>(text.text.size()) * text.size * 0.5;
    return Rect{text.origin.x, text.origin.y - text.size * 0.8, width, text.size};
}

}  // namespace

const char* to_string(ItemRole role) noexcept
{
    switch (role) {
        case ItemRole::StaffLine:     return "staff-line";
        case ItemRole::Barline:       return "barline";
        case ItemRole::Clef:          return "clef";
        case ItemRole::KeySignature:  return "key-signature";
        case ItemRole::TimeSignature: return "time-signature";
        case ItemRole::Notehead:      return "notehead";
        case ItemRole::Stem:          return "stem";
        case ItemRole::Flag:          return "flag";
        case ItemRole::Beam:          return "beam";
        case ItemRole::LedgerLine:    return "ledger-line";
        case ItemRole::Accidental:    return "accidental";
        case ItemRole::AugmentationDot: return "augmentation-dot";
        case ItemRole::Rest:          return "rest";
        case ItemRole::Tie:           return "tie";
        case ItemRole::Slur:          return "slur";
        case ItemRole::Hairpin:       return "hairpin";
        case ItemRole::Dynamic:       return "dynamic";
        case ItemRole::Articulation:  return "articulation";
        case ItemRole::TupletBracket: return "tuplet-bracket";
        case ItemRole::RulerMark:     return "ruler-mark";
        case ItemRole::RulerLabel:    return "ruler-label";
        case ItemRole::Cursor:        return "cursor";
        case ItemRole::GraceSlash:    return "grace-slash";
        case ItemRole::Text:          break;
    }
    return "text";
}

void DisplayList::clear()
{
    glyphs.clear();
    lines.clear();
    quads.clear();
    curves.clear();
    texts.clear();
    missing_glyphs.clear();
    bounds = Rect{};
    is_crowded = false;
}

void DisplayList::add(const GlyphItem& item)
{
    glyphs.push_back(item);
    bounds = bounds.united_with(item.bounds);
}

void DisplayList::add(const LineItem& item)
{
    lines.push_back(item);
    bounds = bounds.united_with(rect_from_line(item));
}

void DisplayList::add(const QuadItem& item)
{
    quads.push_back(item);
    bounds = bounds.united_with(rect_from_quad(item));
}

void DisplayList::add(const CurveItem& item)
{
    curves.push_back(item);
    bounds = bounds.united_with(rect_from_curve(item));
}

void DisplayList::add(const TextItem& item)
{
    texts.push_back(item);
    bounds = bounds.united_with(rect_from_text(item));
}

std::vector<Rect> DisplayList::bounds_for_event(notation::EventId id) const
{
    std::vector<Rect> result;
    if (id == notation::kInvalidEventId)
        return result;

    for (const GlyphItem& item : glyphs) {
        if (item.source_id == id)
            result.push_back(item.bounds);
    }
    for (const LineItem& item : lines) {
        if (item.source_id == id)
            result.push_back(rect_from_line(item));
    }
    for (const QuadItem& item : quads) {
        if (item.source_id == id)
            result.push_back(rect_from_quad(item));
    }
    for (const CurveItem& item : curves) {
        if (item.source_id == id)
            result.push_back(rect_from_curve(item));
    }
    for (const TextItem& item : texts) {
        if (item.source_id == id)
            result.push_back(rect_from_text(item));
    }
    return result;
}

}  // namespace vcc::engraving
