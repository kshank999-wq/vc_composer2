// Track rendering: what a track sounds like, as MIDI (Main Spec §8).
//
// The score is rendered through the track's expression map by the ported
// notation engine (dynamics, hairpins, articulations, keyswitches,
// transposition), then the version's MIDI regions are added. The result is
// what playback schedules and what deliverable MIDI export writes.
//
// Keyswitches are kept apart from the notes: they are instructions to a
// sample player, never pitched music (Supplement §8), so a piano roll can
// hide them and a GM placeholder can skip them.
#pragma once

#include <string>
#include <vector>

#include "vcc/core/Midi.h"
#include "vcc/notation/ExpressionMap.h"
#include "vcc/project/Project.h"

namespace vcc::project {

struct RenderedTrack {
    std::vector<NoteEvent> notes;        ///< Sounding notes: score + regions.
    std::vector<NoteEvent> keyswitches;  ///< Player instructions, not music.
    std::vector<ControllerEvent> controllers;
    std::vector<std::string> warnings;   ///< Out-of-range notes, unmapped articulations.
};

/// The built-in maps by name ("default", "orchestral_cc"); default otherwise.
notation::ExpressionMap expression_map_named(const std::string& name);

/// Renders a track's active version.
RenderedTrack render_track(const Track& track, const notation::ExpressionMap& map);
RenderedTrack render_track(const Track& track);

/// Deliverable MIDI for a project: one track per exported instrument track,
/// keyswitches included only when `include_keyswitches` (they are for the
/// composer's own players, not for a GM file sent to someone else).
std::string export_project_midi(const Project& project, bool include_keyswitches = false);

}  // namespace vcc::project
