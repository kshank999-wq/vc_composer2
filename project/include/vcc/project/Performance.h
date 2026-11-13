// Performance overrides (Main Spec §8): how written notes are played,
// keyed by stable score note id and kept apart from the score.
//
// The score says what the music is; this layer says how a particular
// performance departs from it: a note pushed a little early, a phrase end
// held longer, an accent leaned on harder. Overrides are offsets from what
// the score renders, so a later score edit still reaches the performed note,
// and clearing an override returns the note exactly to the written one.
// Printed parts and MusicXML never see them; playback and MIDI export do.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vcc/project/Project.h"

namespace vcc::project {

/// Bounds an override so it stays a performance of the note rather than a
/// rewrite of it: within two quarters early or late, no shorter than the
/// note, velocity and CC within ±127.
NoteOverride clamp_override(const NoteOverride& o) noexcept;

/// A seeded humanize (Supplement §10.1: the same seed gives the same
/// result). Each note's offsets come from the seed and its own id only, so
/// humanizing a note gives it the same feel whatever else is selected.
struct HumanizeSettings {
    Ticks timing = 30;     ///< Up to this much early or late.
    Ticks length = 0;      ///< Up to this much shorter or longer.
    int velocity = 5;      ///< Up to this much softer or louder.
    std::uint64_t seed = 1;
};
/// The named presets the app offers: "subtle", "natural", "loose".
HumanizeSettings humanize_preset(const std::string& name);
/// `current` with its timing, length and velocity replaced by humanized
/// offsets (a zero amount keeps that component); the CC offset is kept.
NoteOverride humanized(const NoteOverride& current, notation::EventId id, const HumanizeSettings& s) noexcept;

/// The score notes of a track's active version that sound as notes of
/// their own: not rests, not the continuation of a tie chain. These are the
/// ids an override can be attached to.
std::vector<notation::EventId> performable_notes(const Track& track);

/// Drops empty overrides and overrides of notes that no longer exist.
/// Returns how many were dropped.
std::size_t prune_performance(TrackVersion& version);

}  // namespace vcc::project
