#include "vcc/engraving/TimelineMap.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace vcc::engraving {

void TimelineMap::set_staff_spaces_per_quarter(double value) noexcept
{
    // Clamped rather than rejected: a zoom control should saturate at its
    // limits, not refuse to move.
    staff_spaces_per_quarter_ = std::max(kMinimumZoom, std::min(kMaximumZoom, value));
}

double TimelineMap::x_for_tick(notation::Ticks tick) const noexcept
{
    const double quarters = static_cast<double>(tick - origin_tick_)
                          / static_cast<double>(notation::kTicksPerQuarter);
    return quarters * staff_spaces_per_quarter_;
}

notation::Ticks TimelineMap::tick_for_x(double x) const noexcept
{
    if (!(staff_spaces_per_quarter_ > 0.0))
        return origin_tick_;
    const double quarters = x / staff_spaces_per_quarter_;
    return origin_tick_
         + static_cast<notation::Ticks>(std::llround(quarters * notation::kTicksPerQuarter));
}

double TimelineMap::width_for_ticks(notation::Ticks ticks) const noexcept
{
    const double quarters = static_cast<double>(ticks)
                          / static_cast<double>(notation::kTicksPerQuarter);
    return quarters * staff_spaces_per_quarter_;
}

notation::Ticks TimelineMap::ticks_for_width(double width) const noexcept
{
    if (!(staff_spaces_per_quarter_ > 0.0))
        return 0;
    const double quarters = width / staff_spaces_per_quarter_;
    return static_cast<notation::Ticks>(std::llround(quarters * notation::kTicksPerQuarter));
}

bool TimelineMap::is_crowded_at(notation::Ticks duration_ticks, double minimum_spaces) const noexcept
{
    if (duration_ticks <= 0)
        return false;
    return width_for_ticks(duration_ticks) < minimum_spaces;
}

double TimelineMap::zoom_to_fit(notation::Ticks duration_ticks, double spaces) const noexcept
{
    if (duration_ticks <= 0 || !(spaces > 0.0))
        return staff_spaces_per_quarter_;

    const double quarters = static_cast<double>(duration_ticks)
                          / static_cast<double>(notation::kTicksPerQuarter);
    const double wanted = spaces / quarters;
    return std::max(kMinimumZoom, std::min(kMaximumZoom, wanted));
}

bool TimelineMap::is_valid() const noexcept
{
    return staff_spaces_per_quarter_ >= kMinimumZoom
        && staff_spaces_per_quarter_ <= kMaximumZoom && origin_tick_ >= 0;
}

std::vector<RulerMark> build_ruler(const notation::Score& score, const TimelineMap& timeline,
                                   notation::Ticks from_tick, notation::Ticks to_tick)
{
    std::vector<RulerMark> marks;
    if (to_tick <= from_tick)
        return marks;

    // Detail follows zoom. Beats are only drawn when one is wide enough to
    // read; subdivisions only when there is a lot of room. Otherwise the
    // ruler becomes a grey smear that conveys nothing.
    constexpr double kMinimumSpacesForBeats = 2.0;
    constexpr double kMinimumSpacesForSubdivisions = 8.0;

    // Walk measure by measure so meter changes are honoured, and start from
    // the measure containing the first visible tick rather than from bar 1.
    notation::Measure measure = score.measure_at(from_tick);

    // A hard iteration bound: a pathological meter must not spin here.
    constexpr int kMaximumMeasures = 100000;

    for (int guard = 0; guard < kMaximumMeasures; ++guard) {
        if (measure.start_tick >= to_tick)
            break;
        if (measure.length_ticks <= 0)
            break;

        // The barline.
        if (measure.start_tick >= from_tick) {
            RulerMark barline;
            barline.tick = measure.start_tick;
            barline.x = timeline.x_for_tick(measure.start_tick);
            barline.measure = measure.number;
            barline.beat = 1;
            barline.label = std::to_string(measure.number);
            marks.push_back(barline);
        }

        // Beats within the measure. One beat is a note of the denominator's
        // value, which is what a musician counts.
        const notation::Ticks beat_ticks =
            notation::kTicksPerWhole / static_cast<notation::Ticks>(measure.time_signature.denominator);

        if (beat_ticks > 0 && timeline.width_for_ticks(beat_ticks) >= kMinimumSpacesForBeats) {
            const int beats = measure.time_signature.numerator;
            for (int beat = 1; beat < beats; ++beat) {
                const notation::Ticks tick =
                    measure.start_tick + static_cast<notation::Ticks>(beat) * beat_ticks;
                if (tick < from_tick || tick >= to_tick)
                    continue;

                RulerMark mark;
                mark.tick = tick;
                mark.x = timeline.x_for_tick(tick);
                mark.measure = measure.number;
                mark.beat = beat + 1;
                marks.push_back(mark);
            }

            // Subdivisions, at a close zoom only.
            if (timeline.width_for_ticks(beat_ticks) >= kMinimumSpacesForSubdivisions) {
                const notation::Ticks half_beat = beat_ticks / 2;
                if (half_beat > 0) {
                    for (int beat = 0; beat < beats; ++beat) {
                        const notation::Ticks tick = measure.start_tick
                                                   + static_cast<notation::Ticks>(beat) * beat_ticks
                                                   + half_beat;
                        if (tick < from_tick || tick >= to_tick)
                            continue;

                        RulerMark mark;
                        mark.tick = tick;
                        mark.x = timeline.x_for_tick(tick);
                        mark.measure = measure.number;
                        mark.beat = beat + 1;
                        mark.is_subdivision = true;
                        marks.push_back(mark);
                    }
                }
            }
        }

        measure = score.measure_at(measure.end_tick());
    }

    std::sort(marks.begin(), marks.end(),
              [](const RulerMark& a, const RulerMark& b) { return a.tick < b.tick; });
    return marks;
}

std::vector<double> barline_positions(const notation::Score& score, const TimelineMap& timeline,
                                      notation::Ticks from_tick, notation::Ticks to_tick)
{
    std::vector<double> positions;
    if (to_tick <= from_tick)
        return positions;

    notation::Measure measure = score.measure_at(from_tick);
    constexpr int kMaximumMeasures = 100000;

    for (int guard = 0; guard < kMaximumMeasures; ++guard) {
        if (measure.start_tick >= to_tick)
            break;
        if (measure.length_ticks <= 0)
            break;
        if (measure.start_tick >= from_tick)
            positions.push_back(timeline.x_for_tick(measure.start_tick));
        measure = score.measure_at(measure.end_tick());
    }

    return positions;
}

}  // namespace vcc::engraving
