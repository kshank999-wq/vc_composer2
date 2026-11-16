// Retrospective MIDI record (Main Spec §9 "retrospective record where
// feasible"): what was played on a MIDI keyboard while not recording is kept
// by the engine, and Transport > Capture MIDI turns the last performance into
// a region on the selected track, as if it had been recorded.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "vcc/core/Time.h"
#include "vcc/project/Project.h"

namespace vcc::session {

struct LiveMidi {
    std::uint8_t status = 0, data1 = 0, data2 = 0;
    double wall_seconds = 0.0;  ///< Any monotonic clock.
    Ticks tick = -1;            ///< Where the transport was, or -1 when it was stopped.
};

/// The last performance - the events after the last pause of more than
/// `gap_seconds` - as a region. Played along with the transport (events
/// carry ticks), the notes land where they were played; played while
/// stopped, they start at `at` and keep their wall-clock timing at the
/// tempo there. Notes still held at the end close at the last event. Pitch
/// bend, aftertouch and controllers come too. Nullopt when there is no note.
std::optional<project::MidiRegion> capture_performance(const std::vector<LiveMidi>& events, const TempoMap& tempo,
                                                       Ticks at, double gap_seconds = 3.0);

}  // namespace vcc::session
