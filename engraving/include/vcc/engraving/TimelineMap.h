// Copyright (c) VC Composer. All rights reserved.
//
// Musical time to horizontal position (spec 02 sections 4.3, 4.4).
//
// This is the decision that shapes the whole notation view. Spec 4.3
// requires the notation strip to carry "a horizontal time ruler aligned to
// the Pro Tools timeline" whose "measures, bars and musical positions should
// remain synchronized with the corresponding Pro Tools track", and spec 4.4
// requires that "horizontal position should track the relevant Pro Tools
// timeline position".
//
// So x is a linear function of musical time, not of engraving spacing.
// Classical notation spaces notes by proportion and justifies each system,
// which would place a note at an x that does not correspond to its position
// on the DAW timeline - exactly what spec 4.3 forbids. Zoom is a change to
// one number here.
//
// The consequence is that dense passages can collide at low zoom. That is
// correct for a timeline view, the same way a piano-roll gets dense, and the
// answer is to zoom rather than to re-space. `is_crowded_at` reports it so
// the UI can hint.
#pragma once

#include <cstdint>
#include <vector>

#include "vcc/notation/MusicalTime.h"
#include "vcc/notation/Score.h"

namespace vcc::engraving {

/// Maps ticks to x, in staff spaces.
class TimelineMap {
public:
    TimelineMap() = default;

    /// `staff_spaces_per_quarter` is the horizontal zoom: how much room one
    /// quarter note occupies. Around 6 is comfortable for reading; 2 is a
    /// dense overview, 24 a close edit view.
    explicit TimelineMap(double staff_spaces_per_quarter)
        : staff_spaces_per_quarter_(staff_spaces_per_quarter)
    {
    }

    double staff_spaces_per_quarter() const noexcept { return staff_spaces_per_quarter_; }

    /// Sets the horizontal zoom. Clamped to a usable range: below the
    /// minimum a notehead is wider than a whole bar, above it a single bar
    /// fills a monitor.
    void set_staff_spaces_per_quarter(double value) noexcept;

    /// The tick the view starts at, so the strip can scroll with the host
    /// timeline (spec 4.4).
    notation::Ticks origin_tick() const noexcept { return origin_tick_; }
    void set_origin_tick(notation::Ticks tick) noexcept { origin_tick_ = tick; }

    /// x in staff spaces for a tick, relative to the view origin.
    double x_for_tick(notation::Ticks tick) const noexcept;

    /// The inverse, for hit testing and for placing the edit cursor.
    notation::Ticks tick_for_x(double x) const noexcept;

    /// Width in staff spaces of a span of ticks.
    double width_for_ticks(notation::Ticks ticks) const noexcept;

    /// The tick span a width covers.
    notation::Ticks ticks_for_width(double width) const noexcept;

    /// True when a duration is narrower than `minimum_spaces` at this zoom,
    /// so notes of that value will visually crowd. Lets the UI suggest
    /// zooming rather than silently overlapping glyphs.
    bool is_crowded_at(notation::Ticks duration_ticks,
                       double minimum_spaces = 1.4) const noexcept;

    /// The zoom at which a duration would occupy exactly `spaces`. Used to
    /// implement "zoom to fit the shortest note".
    double zoom_to_fit(notation::Ticks duration_ticks, double spaces) const noexcept;

    bool is_valid() const noexcept;

    /// Smallest and largest usable horizontal zoom.
    /// The horizontal zoom a map has before anybody changes it, so a view
    /// that wants to say "the default size" has a name for it rather than a
    /// number that can drift from this one.
    static constexpr double kDefaultSpacesPerQuarter = 6.0;

    static constexpr double kMinimumZoom = 0.5;
    static constexpr double kMaximumZoom = 96.0;

private:
    double staff_spaces_per_quarter_ = kDefaultSpacesPerQuarter;
    notation::Ticks origin_tick_ = 0;
};

/// One tick mark on the bar/beat ruler (spec 4.3).
struct RulerMark {
    notation::Ticks tick = 0;
    double x = 0.0;

    std::int32_t measure = 1;
    /// 1-based beat within the measure. 1 means this is a barline.
    int beat = 1;

    bool is_barline() const noexcept { return beat == 1; }

    /// Subdivision below the beat, for a close zoom. Empty label; drawn as a
    /// shorter tick.
    bool is_subdivision = false;

    /// Label for a barline, such as "5". Empty for beats and subdivisions,
    /// because a ruler labelled at every beat is unreadable.
    std::string label;
};

/// Builds the ruler for a visible range.
///
/// Detail adapts to zoom: barlines always, beats when there is room, and
/// subdivisions when there is a lot. A ruler that drew every beat at a low
/// zoom would be a grey smear.
std::vector<RulerMark> build_ruler(const notation::Score& score, const TimelineMap& timeline,
                                   notation::Ticks from_tick, notation::Ticks to_tick);

/// Barline positions in a range, which the staff layout draws through every
/// staff of a system.
std::vector<double> barline_positions(const notation::Score& score,
                                      const TimelineMap& timeline,
                                      notation::Ticks from_tick, notation::Ticks to_tick);

}  // namespace vcc::engraving
