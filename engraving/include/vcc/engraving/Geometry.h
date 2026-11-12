// Copyright (c) VC Composer. All rights reserved.
//
// Geometry and the staff coordinate system (spec 02 sections 4.3, 4.4, 5.1).
//
// Every measurement is in **staff spaces** - the distance between two
// adjacent staff lines. That is the unit notation is conventionally
// described in, and the unit a font's metrics are expressed in, so using
// anything else means converting twice and rounding twice.
//
// y increases downward, matching every screen coordinate system, so
// converting a staff position to y is a subtraction rather than a flip.
#pragma once

#include <algorithm>
#include <cstdint>

namespace vcc::engraving {

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct Size {
    double width = 0.0;
    double height = 0.0;
};

struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    double right() const noexcept { return x + width; }
    double bottom() const noexcept { return y + height; }

    bool contains(const Point& point) const noexcept
    {
        return point.x >= x && point.x <= right() && point.y >= y && point.y <= bottom();
    }

    bool intersects(const Rect& other) const noexcept
    {
        return x < other.right() && other.x < right() && y < other.bottom()
            && other.y < bottom();
    }

    /// The smallest rectangle containing both. An empty rectangle is
    /// absorbed rather than dragging the result to the origin.
    Rect united_with(const Rect& other) const noexcept
    {
        if (width <= 0.0 && height <= 0.0)
            return other;
        if (other.width <= 0.0 && other.height <= 0.0)
            return *this;

        const double left = std::min(x, other.x);
        const double top = std::min(y, other.y);
        const double far_right = std::max(right(), other.right());
        const double far_bottom = std::max(bottom(), other.bottom());
        return Rect{left, top, far_right - left, far_bottom - top};
    }

    Rect expanded(double amount) const noexcept
    {
        return Rect{x - amount, y - amount, width + 2.0 * amount, height + 2.0 * amount};
    }
};

/// Vertical geometry of one five-line staff.
///
/// All values in staff spaces except `top_y`, which is also in staff spaces
/// but measured from the top of the layout so several staves can be stacked.
struct StaffMetrics {
    /// y of the staff's top line.
    double top_y = 0.0;

    /// Line thickness. The conventional value is one eighth of a space.
    double line_thickness = 0.125;

    /// Stem thickness, conventionally 3/25 of a space.
    double stem_thickness = 0.12;

    /// Conventional stem length, in spaces, for a note inside the staff.
    double stem_length = 3.5;

    /// Beam thickness and the gap between beams.
    double beam_thickness = 0.5;
    double beam_gap = 0.25;

    /// Ledger line length either side of the notehead.
    double ledger_extension = 0.4;

    /// A five-line staff is four spaces tall.
    double height() const noexcept { return 4.0; }

    double bottom_y() const noexcept { return top_y + height(); }

    /// y of one of the five staff lines, 0 being the top line.
    double line_y(int line_from_top) const noexcept
    {
        return top_y + static_cast<double>(line_from_top);
    }

    /// y for a staff position as the score model reports it: 0 is the bottom
    /// line, 1 the space above it, 8 the top line. Each position is half a
    /// space, and y decreases as the position rises.
    double y_for_staff_position(int position) const noexcept
    {
        return bottom_y() - static_cast<double>(position) * 0.5;
    }

    /// The inverse, rounded to the nearest position. Used by hit testing to
    /// turn a click into a pitch.
    int staff_position_for_y(double y) const noexcept
    {
        const double exact = (bottom_y() - y) * 2.0;
        return static_cast<int>(exact >= 0.0 ? exact + 0.5 : exact - 0.5);
    }

    /// True when a position needs ledger lines: outside the five lines.
    bool needs_ledger_lines(int position) const noexcept
    {
        return position < 0 || position > 8;
    }

    /// The staff positions at which ledger lines are drawn for a notehead at
    /// `position` - the even positions between the staff and the note.
    /// Writes at most `capacity` values and returns how many.
    std::size_t ledger_line_positions(int position, int* out, std::size_t capacity) const noexcept
    {
        std::size_t count = 0;
        if (position < 0) {
            // Below the staff: -2, -4, -6 ... down to the note.
            for (int line = -2; line >= position && count < capacity; line -= 2)
                out[count++] = line;
        } else if (position > 8) {
            for (int line = 10; line <= position && count < capacity; line += 2)
                out[count++] = line;
        }
        return count;
    }
};

/// Converts staff spaces to pixels.
///
/// One number for the vertical scale, which is what "zoom" means for a
/// staff. Horizontal scale is governed by the timeline instead, because
/// spec 4.3 requires horizontal position to follow the DAW timeline rather
/// than engraving spacing.
struct ViewScale {
    /// Pixels per staff space. 8 is a comfortable on-screen staff; smaller
    /// gets cramped, larger is for a zoomed view.
    double pixels_per_staff_space = 8.0;

    double to_pixels(double staff_spaces) const noexcept
    {
        return staff_spaces * pixels_per_staff_space;
    }

    double to_staff_spaces(double pixels) const noexcept
    {
        return pixels_per_staff_space > 0.0 ? pixels / pixels_per_staff_space : 0.0;
    }

    bool is_valid() const noexcept
    {
        return pixels_per_staff_space >= 2.0 && pixels_per_staff_space <= 80.0;
    }
};

}  // namespace vcc::engraving
