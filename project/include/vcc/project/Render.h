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
#include "vcc/notation/MidiRenderer.h"
#include "vcc/project/Project.h"

namespace vcc::project {

struct RenderedTrack {
    std::vector<NoteEvent> notes;        ///< Sounding notes: score + regions.
    /// Parallel to `notes`: the score note each came from (the first note of
    /// a tie chain), or 0 for a note of a MIDI region.
    std::vector<notation::EventId> note_ids;
    std::vector<NoteEvent> keyswitches;  ///< Player instructions, not music.
    std::vector<ControllerEvent> controllers;
    std::vector<std::string> warnings;   ///< Out-of-range notes, unmapped articulations.
};

/// The built-in maps by name ("default", "orchestral_cc"); default otherwise.
notation::ExpressionMap expression_map_named(const std::string& name);

/// Renders a track's active version. With `performance`, the version's
/// note overrides are applied (what playback and export use); without, the
/// notes are as written (what the piano roll shows as the written position).
RenderedTrack render_track(const Track& track, const notation::ExpressionMap& map, bool performance = true);
RenderedTrack render_track(const Track& track, bool performance = true);

/// The notation render of a track's active version, before performance
/// and regions: events in canonical order (by tick, then send order, then
/// source note), so two renders of the same score compare equal event for
/// event. With `to > from` only the notes and markings in that range.
notation::RenderResult render_score_events(const Track& track, const notation::ExpressionMap& map, Ticks from = 0,
                                           Ticks to = 0);
/// The track's MIDI from a score render: notes paired, overrides applied
/// (with `performance`), regions added. `render_track` is
/// `finish_render(render_score_events(...))`; RenderCache supplies the
/// score render from its cache instead.
RenderedTrack finish_render(const Track& track, const notation::ExpressionMap& map, const notation::RenderResult& r,
                            bool performance);
/// Sorts rendered events into the canonical order.
void canonical_order(std::vector<notation::TimedMidiEvent>& events);

/// Applies one override to a rendered note (clamped: never before zero,
/// never shorter than a tick, velocity 1-127).
NoteEvent apply_override(NoteEvent note, const NoteOverride& o) noexcept;

/// Deliverable MIDI for a project: one track per exported instrument track,
/// keyswitches included only when `include_keyswitches` (they are for the
/// composer's own players, not for a GM file sent to someone else).
std::string export_project_midi(const Project& project, bool include_keyswitches = false);

}  // namespace vcc::project
