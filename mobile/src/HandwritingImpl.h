// Inside the recognizer: stroke measurements and the context a recognition
// runs against (the staves and the timeline of the track's layout).
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "vcc/mobile/Handwriting.h"

namespace vcc::mobile::detail {

struct Features {
    double left = 0, right = 0, top = 0, bottom = 0;
    double width = 0, height = 0, cx = 0, cy = 0;
    double length = 0;        ///< Path length.
    double straightness = 1;  ///< End-to-end distance / path length.
    double closure = 0;       ///< End-to-end distance.
    double density = 0;       ///< Path length per side of the bounding box.
    int x_reversals = 0, y_reversals = 0;
};
Features measure(const Stroke& s) noexcept;

struct InkContext {
    std::vector<double> staff_top;
    /// The tick at a layout x (where writing starts), on the entry grid.
    std::function<std::int64_t(double)> tick_for_x;
    /// The end of the bar a tick is in.
    std::function<std::int64_t(std::int64_t)> bar_end_of;
    /// A value (notation::NoteValue) with dots, in the project's ticks.
    std::function<std::int64_t(int, int)> duration_ticks;
};

Recognition recognize(const std::vector<Stroke>& strokes, const InkContext& context);

}  // namespace vcc::mobile::detail
