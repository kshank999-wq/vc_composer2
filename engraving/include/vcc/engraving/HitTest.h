// Copyright (c) VC Composer. All rights reserved.
//
// Turning a point into something musical (spec 02 sections 5.2, 4.4).
//
// Spec 5.2 requires "mouse-based entry: click on a staff to place a note of
// the currently selected duration", which needs two things a drawing layer
// cannot supply: what existing item is under the pointer, and what pitch and
// tick a bare click on the staff means.
//
// Both are answered from a LayoutResult, so they agree with what was drawn
// by construction rather than by two code paths happening to match.
#pragma once

#include <cstddef>
#include <cstdint>

#include "vcc/engraving/Geometry.h"
#include "vcc/engraving/LayoutEngine.h"
#include "vcc/engraving/TimelineMap.h"
#include "vcc/notation/Score.h"

namespace vcc::engraving {

/// What the pointer is over.
struct HitResult {
    bool hit = false;

    /// The score event, when the item belongs to one.
    notation::EventId id = notation::kInvalidEventId;
    ItemRole role = ItemRole::StaffLine;

    /// Index into `LayoutResult::notes` when a note or rest was hit, or
    /// SIZE_MAX otherwise. Saves the caller a second search for the
    /// geometry it is about to need.
    std::size_t note_index = static_cast<std::size_t>(-1);

    bool is_note() const noexcept { return hit && note_index != static_cast<std::size_t>(-1); }
};

/// The item under a point, preferring noteheads and rests over structure.
///
/// A click near a notehead means the note, even when it also lands on a
/// staff line or a stem - which it usually does. `tolerance` widens every
/// item's bounds, because a pointer is not a mathematical point.
HitResult hit_test(const LayoutResult& layout, const Point& point, double tolerance = 0.25);

/// Where a click on empty staff would insert a note (spec 5.2).
struct InsertionPoint {
    bool valid = false;

    std::size_t staff_index = 0;

    /// Tick, quantized to the grid the caller asked for.
    notation::Ticks tick = 0;

    /// Staff position under the pointer, and the pitch it spells in the
    /// staff's clef and key.
    int staff_position = 0;
    notation::Pitch pitch{};
};

/// Resolves a point to a staff, a quantized tick and a pitch.
///
/// `quantize_ticks` is the entry grid - a sixteenth note, say. 0 means no
/// quantization, which is what a tuplet-aware entry mode wants.
///
/// The pitch is spelled from the key signature in force at that tick, so
/// clicking the top line in E flat major gives F, not E sharp. Accidentals
/// are the composer's to add afterwards.
InsertionPoint insertion_point_at(const LayoutResult& layout, const notation::Score& score,
                                  const TimelineMap& timeline, const Point& point,
                                  notation::Ticks quantize_ticks = 0,
                                  double staff_tolerance = 3.0);

}  // namespace vcc::engraving
