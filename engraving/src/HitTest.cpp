#include "vcc/engraving/HitTest.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vcc::engraving {
namespace {

/// How much a role is preferred when several items overlap a point. A click
/// almost always lands on a staff line as well as on the note the user
/// meant, so the note has to win.
int role_priority(ItemRole role) noexcept
{
    switch (role) {
        case ItemRole::Notehead:
        case ItemRole::Rest:            return 100;
        case ItemRole::Accidental:
        case ItemRole::AugmentationDot: return 90;
        case ItemRole::Articulation:
        case ItemRole::Dynamic:         return 80;
        case ItemRole::Flag:
        case ItemRole::Beam:            return 70;
        case ItemRole::Stem:
        case ItemRole::GraceSlash:      return 60;
        case ItemRole::Tie:
        case ItemRole::Slur:
        case ItemRole::Hairpin:         return 50;
        case ItemRole::Clef:
        case ItemRole::KeySignature:
        case ItemRole::TimeSignature:   return 40;
        case ItemRole::Cursor:          return 30;
        case ItemRole::RulerMark:
        case ItemRole::RulerLabel:      return 20;
        case ItemRole::Barline:
        case ItemRole::TupletBracket:
        case ItemRole::Text:            return 10;
        case ItemRole::LedgerLine:
        case ItemRole::StaffLine:       break;
    }
    return 0;
}

Rect bounds_of(const LineItem& line, double tolerance)
{
    const double left = std::min(line.from.x, line.to.x);
    const double top = std::min(line.from.y, line.to.y);
    const double width = std::abs(line.to.x - line.from.x);
    const double height = std::abs(line.to.y - line.from.y);
    return Rect{left, top, std::max(width, line.thickness), std::max(height, line.thickness)}
        .expanded(tolerance);
}

}  // namespace

HitResult hit_test(const LayoutResult& layout, const Point& point, double tolerance)
{
    HitResult best;
    int best_priority = -1;
    double best_distance = std::numeric_limits<double>::max();

    const auto consider = [&](const Rect& bounds, ItemRole role, notation::EventId id) {
        if (!bounds.contains(point))
            return;

        const int priority = role_priority(role);
        // Among items of equal standing, the one whose centre is nearest
        // wins: two adjacent noteheads should resolve to the closer.
        const double centre_x = bounds.x + bounds.width * 0.5;
        const double centre_y = bounds.y + bounds.height * 0.5;
        const double distance = std::hypot(point.x - centre_x, point.y - centre_y);

        if (priority < best_priority)
            return;
        if (priority == best_priority && distance >= best_distance)
            return;

        best.hit = true;
        best.id = id;
        best.role = role;
        best.note_index = static_cast<std::size_t>(-1);
        best_priority = priority;
        best_distance = distance;
    };

    for (const GlyphItem& item : layout.display_list.glyphs)
        consider(item.bounds.expanded(tolerance), item.role, item.source_id);
    for (const LineItem& item : layout.display_list.lines)
        consider(bounds_of(item, tolerance), item.role, item.source_id);
    for (const QuadItem& item : layout.display_list.quads) {
        Rect bounds{item.corners[0].x, item.corners[0].y, 0.0, 0.0};
        for (const Point& corner : item.corners)
            bounds = bounds.united_with(Rect{corner.x, corner.y, 0.0001, 0.0001});
        consider(bounds.expanded(tolerance), item.role, item.source_id);
    }

    if (!best.hit)
        return best;

    if (best.id != notation::kInvalidEventId) {
        for (std::size_t index = 0; index < layout.notes.size(); ++index) {
            if (layout.notes[index].id == best.id) {
                best.note_index = index;
                break;
            }
        }
    }
    return best;
}

InsertionPoint insertion_point_at(const LayoutResult& layout, const notation::Score& score,
                                  const TimelineMap& timeline, const Point& point,
                                  notation::Ticks quantize_ticks, double staff_tolerance)
{
    InsertionPoint insertion;
    if (layout.staff_top_y.empty())
        return insertion;

    // The nearest staff, but only within the tolerance: a click far above
    // the top staff is not an insertion, it is a miss. Without that, every
    // click in the margin would enter a note.
    std::size_t nearest = 0;
    double nearest_distance = std::numeric_limits<double>::max();
    for (std::size_t index = 0; index < layout.staff_top_y.size(); ++index) {
        const double top = layout.staff_top_y[index];
        const double bottom = top + 4.0;
        double distance = 0.0;
        if (point.y < top)
            distance = top - point.y;
        else if (point.y > bottom)
            distance = point.y - bottom;
        if (distance < nearest_distance) {
            nearest_distance = distance;
            nearest = index;
        }
    }
    if (nearest_distance > staff_tolerance)
        return insertion;
    if (nearest >= score.staff_count())
        return insertion;

    StaffMetrics metrics;
    metrics.top_y = layout.staff_top_y[nearest];

    insertion.staff_index = nearest;
    insertion.staff_position = metrics.staff_position_for_y(point.y);

    notation::Ticks tick = timeline.tick_for_x(point.x);
    if (tick < 0)
        tick = 0;
    if (quantize_ticks > 0) {
        // Rounded to the nearest grid line rather than truncated: clicking
        // just past a beat means that beat, not a note a grid step late.
        const notation::Ticks half = quantize_ticks / 2;
        tick = ((tick + half) / quantize_ticks) * quantize_ticks;
    }
    insertion.tick = tick;

    const notation::Clef clef = score.staff(nearest).clef;
    if (clef == notation::Clef::Percussion) {
        // Staff position carries no pitch meaning on a percussion staff, so
        // there is nothing honest to spell. The tick and staff are still
        // useful, and the pitch is left at its default.
        insertion.valid = true;
        return insertion;
    }

    // The diatonic degree the position names, then the key signature's
    // alteration for that degree - which is what a musician expects from
    // clicking a line in a key with accidentals.
    const int diatonic = notation::bottom_line_diatonic_index(clef) + insertion.staff_position;
    if (diatonic < 0)
        return insertion;

    notation::Pitch pitch;
    pitch.octave = static_cast<std::int8_t>(diatonic / 7);
    pitch.step = static_cast<notation::Step>(diatonic % 7);

    const notation::KeySignature key = score.key_signature_at(tick);
    pitch.alteration = static_cast<std::int8_t>(key.alteration_for(pitch.step));

    insertion.pitch = pitch;
    insertion.valid = pitch.is_valid();
    return insertion;
}

}  // namespace vcc::engraving
