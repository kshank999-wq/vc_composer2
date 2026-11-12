// The staff coordinate system and the timeline map (spec 02 sections 4.3, 4.4).
//
// These are the two pieces of geometry everything else is measured against,
// and the timeline map is where the notation view's central decision lives:
// horizontal position is a linear function of musical time, so that the
// strip stays aligned with the Pro Tools timeline.
#include "vcc/engraving/Geometry.h"

#include <cstddef>
#include "vcc/engraving/TimelineMap.h"

#include "vcc_test.h"

using namespace vcc;
using namespace vcc::engraving;

/// A quarter note in ticks, so the arithmetic reads as rhythm.
constexpr notation::Ticks kQuarter = notation::kTicksPerQuarter;

VCC_TEST(a_staff_is_four_spaces_tall_and_y_grows_downward)
{
    StaffMetrics metrics;
    metrics.top_y = 10.0;

    VCC_CHECK_NEAR(metrics.height(), 4.0, 1e-9);
    VCC_CHECK_NEAR(metrics.bottom_y(), 14.0, 1e-9);
    VCC_CHECK_NEAR(metrics.line_y(0), 10.0, 1e-9);
    VCC_CHECK_NEAR(metrics.line_y(4), 14.0, 1e-9);

    // Position 0 is the bottom line, 8 the top, and each step is half a
    // space. y decreases as the position rises.
    VCC_CHECK_NEAR(metrics.y_for_staff_position(0), 14.0, 1e-9);
    VCC_CHECK_NEAR(metrics.y_for_staff_position(4), 12.0, 1e-9);
    VCC_CHECK_NEAR(metrics.y_for_staff_position(8), 10.0, 1e-9);
}

VCC_TEST(a_point_maps_back_to_the_nearest_staff_position)
{
    StaffMetrics metrics;
    metrics.top_y = 0.0;

    for (int position = -6; position <= 14; ++position) {
        const double y = metrics.y_for_staff_position(position);
        VCC_CHECK_EQ(metrics.staff_position_for_y(y), position);
    }

    // A click between two positions rounds to the nearer, which is what
    // mouse entry needs.
    VCC_CHECK_EQ(metrics.staff_position_for_y(metrics.y_for_staff_position(4) - 0.2), 4);
    VCC_CHECK_EQ(metrics.staff_position_for_y(metrics.y_for_staff_position(4) - 0.3), 5);
}

VCC_TEST(ledger_lines_are_drawn_only_outside_the_staff)
{
    StaffMetrics metrics;

    VCC_CHECK(!metrics.needs_ledger_lines(0));
    VCC_CHECK(!metrics.needs_ledger_lines(8));
    VCC_CHECK(metrics.needs_ledger_lines(-1));
    VCC_CHECK(metrics.needs_ledger_lines(9));

    int lines[8]{};
    // Middle C in treble sits one ledger line below the staff.
    VCC_CHECK_EQ(metrics.ledger_line_positions(-2, lines, 8), std::size_t{1});
    VCC_CHECK_EQ(lines[0], -2);

    // Two lines above, and only on the lines - not in the spaces.
    VCC_CHECK_EQ(metrics.ledger_line_positions(12, lines, 8), std::size_t{2});
    VCC_CHECK_EQ(lines[0], 10);
    VCC_CHECK_EQ(lines[1], 12);

    // A note in a space beyond the staff still needs the line below it.
    VCC_CHECK_EQ(metrics.ledger_line_positions(11, lines, 8), std::size_t{1});
    VCC_CHECK_EQ(lines[0], 10);

    VCC_CHECK_EQ(metrics.ledger_line_positions(4, lines, 8), std::size_t{0});
}

VCC_TEST(x_is_a_linear_function_of_musical_time)
{
    TimelineMap timeline(6.0);

    VCC_CHECK_NEAR(timeline.x_for_tick(0), 0.0, 1e-9);
    VCC_CHECK_NEAR(timeline.x_for_tick(notation::kTicksPerQuarter), 6.0, 1e-9);
    VCC_CHECK_NEAR(timeline.x_for_tick(4 * notation::kTicksPerQuarter), 24.0, 1e-9);

    // Which is the whole point: equal musical durations occupy equal width,
    // so a note's x corresponds to its position on the host timeline.
    const double first = timeline.x_for_tick((kQuarter / 2)) - timeline.x_for_tick(0);
    const double second = timeline.x_for_tick(5 * kQuarter + kQuarter / 2) - timeline.x_for_tick(5 * kQuarter);
    VCC_CHECK_NEAR(first, second, 1e-9);
}

VCC_TEST(the_view_origin_scrolls_the_strip)
{
    TimelineMap timeline(6.0);
    timeline.set_origin_tick((4 * kQuarter));

    VCC_CHECK_NEAR(timeline.x_for_tick((4 * kQuarter)), 0.0, 1e-9);
    VCC_CHECK_NEAR(timeline.x_for_tick((4 * kQuarter) + kQuarter), 6.0, 1e-9);
    VCC_CHECK_EQ(timeline.tick_for_x(6.0), notation::Ticks{(5 * kQuarter)});
}

VCC_TEST(tick_for_x_inverts_x_for_tick)
{
    TimelineMap timeline(7.5);
    for (notation::Ticks tick = 0; tick < 20000; tick += 137)
        VCC_CHECK_EQ(timeline.tick_for_x(timeline.x_for_tick(tick)), tick);
}

VCC_TEST(zoom_is_clamped_rather_than_refused)
{
    TimelineMap timeline;

    timeline.set_staff_spaces_per_quarter(1000.0);
    VCC_CHECK_NEAR(timeline.staff_spaces_per_quarter(), TimelineMap::kMaximumZoom, 1e-9);

    timeline.set_staff_spaces_per_quarter(-5.0);
    VCC_CHECK_NEAR(timeline.staff_spaces_per_quarter(), TimelineMap::kMinimumZoom, 1e-9);
    VCC_CHECK(timeline.is_valid());
}

VCC_TEST(crowding_is_a_property_of_zoom_and_duration)
{
    TimelineMap timeline(6.0);

    // A quarter gets 6 spaces, a sixteenth 1.5, a 64th 0.375.
    VCC_CHECK(!timeline.is_crowded_at(notation::quarter().ticks()));
    VCC_CHECK(!timeline.is_crowded_at(notation::sixteenth().ticks()));
    VCC_CHECK(timeline.is_crowded_at(notation::Duration{notation::NoteValue::SixtyFourth}.ticks()));

    // Zoom out and even a quarter crowds.
    timeline.set_staff_spaces_per_quarter(1.0);
    VCC_CHECK(timeline.is_crowded_at(notation::quarter().ticks()));

    // A zero or negative duration is not crowded; it is nothing.
    VCC_CHECK(!timeline.is_crowded_at(0));
}

VCC_TEST(zoom_to_fit_answers_the_zoom_a_duration_needs)
{
    TimelineMap timeline(6.0);

    const notation::Ticks sixteenth = notation::sixteenth().ticks();
    const double wanted = timeline.zoom_to_fit(sixteenth, 2.0);

    TimelineMap zoomed(wanted);
    VCC_CHECK_NEAR(zoomed.width_for_ticks(sixteenth), 2.0, 1e-9);
    VCC_CHECK(!zoomed.is_crowded_at(sixteenth));

    // Clamped to the usable range rather than returning something absurd.
    VCC_CHECK(timeline.zoom_to_fit(1, 40.0) <= TimelineMap::kMaximumZoom);
}

// --- The ruler --------------------------------------------------------------

namespace {

std::size_t count_barlines(const std::vector<RulerMark>& marks)
{
    std::size_t count = 0;
    for (const RulerMark& mark : marks) {
        if (mark.is_barline())
            ++count;
    }
    return count;
}

}  // namespace

VCC_TEST(the_ruler_marks_bars_and_beats)
{
    notation::Score score;
    TimelineMap timeline(6.0);

    // Two bars of 4/4 at a zoom where a beat has room to be drawn but a
    // subdivision does not.
    const std::vector<RulerMark> marks = build_ruler(score, timeline, 0, (8 * kQuarter));

    VCC_CHECK_EQ(count_barlines(marks), std::size_t{2});
    VCC_CHECK_EQ(marks.size(), std::size_t{8});  // 2 barlines + 3 beats each.

    VCC_CHECK_EQ(marks.front().measure, 1);
    VCC_CHECK_EQ(marks.front().label, std::string("1"));
    VCC_CHECK_NEAR(marks.front().x, 0.0, 1e-9);

    // Only barlines carry a label: a ruler numbered at every beat is
    // unreadable.
    for (const RulerMark& mark : marks) {
        if (!mark.is_barline())
            VCC_CHECK(mark.label.empty());
    }

    // Marks are ordered by tick.
    for (std::size_t index = 1; index < marks.size(); ++index)
        VCC_CHECK(marks[index - 1].tick <= marks[index].tick);
}

VCC_TEST(ruler_detail_follows_zoom)
{
    notation::Score score;

    // Zoomed out: barlines only.
    const std::vector<RulerMark> coarse = build_ruler(score, TimelineMap(1.0), 0, (8 * kQuarter));
    VCC_CHECK_EQ(coarse.size(), std::size_t{2});
    VCC_CHECK_EQ(count_barlines(coarse), std::size_t{2});

    // Zoomed in: barline, three beats and four half-beat subdivisions.
    const std::vector<RulerMark> fine = build_ruler(score, TimelineMap(12.0), 0, (4 * kQuarter));
    VCC_CHECK_EQ(fine.size(), std::size_t{8});

    std::size_t subdivisions = 0;
    for (const RulerMark& mark : fine) {
        if (mark.is_subdivision)
            ++subdivisions;
    }
    VCC_CHECK_EQ(subdivisions, std::size_t{4});
}

VCC_TEST(the_ruler_honours_a_meter_change)
{
    notation::Score score;
    VCC_CHECK(score.set_time_signature((4 * kQuarter), vcc::music::TimeSignature{3, 4}));

    // Bar 1 in 4/4 (four quarters), bar 2 in 3/4 (three).
    const std::vector<RulerMark> marks = build_ruler(score, TimelineMap(6.0), 0, 7 * kQuarter);

    VCC_CHECK_EQ(count_barlines(marks), std::size_t{2});
    // 4 marks in the first bar, 3 in the second.
    VCC_CHECK_EQ(marks.size(), std::size_t{7});

    const std::vector<double> barlines = barline_positions(score, TimelineMap(6.0), 0, 7 * kQuarter);
    VCC_CHECK_EQ(barlines.size(), std::size_t{2});
    VCC_CHECK_NEAR(barlines[0], 0.0, 1e-9);
    VCC_CHECK_NEAR(barlines[1], 24.0, 1e-9);  // Four quarters at 6 spaces a quarter.
}

VCC_TEST(an_empty_or_inverted_range_produces_nothing)
{
    notation::Score score;
    VCC_CHECK(build_ruler(score, TimelineMap(6.0), 1000, 1000).empty());
    VCC_CHECK(build_ruler(score, TimelineMap(6.0), 2000, 1000).empty());
    VCC_CHECK(barline_positions(score, TimelineMap(6.0), 2000, 1000).empty());
}

VCC_TEST_MAIN("engraving/Timeline")
