// Articulation maps (Main Spec §8 "Articulation → playback", §11): how a
// track's written articulations and dynamics reach its instrument -
// keyswitches, controllers, program changes, channel switching, velocity
// layers, and the "natural" switch back to sustain. A map is a track's own
// (saved in the project) or a built-in by name; every switch is sent before
// each note, so playback started anywhere chases the right articulation.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vcc/core/Json.h"
#include "vcc/notation/ExpressionMap.h"
#include "vcc/project/Project.h"

namespace vcc::project {

/// The built-in maps: ("default", "Default"), ("orchestral_cc", ...).
std::vector<std::pair<std::string, std::string>> builtin_expression_maps();

json::Value expression_map_to_json(const notation::ExpressionMap& map);
/// Nullopt when the document is not a valid map.
std::optional<notation::ExpressionMap> expression_map_from_json(const json::Value& doc);

/// What a track plays through: its own map, else the built-in it names.
notation::ExpressionMap expression_map_for(const Track& track);
/// Changes whenever what plays changes (the render cache keys on it).
std::string expression_map_key(const Track& track);

/// Which of a map's sounds a note with these articulations plays: the
/// first sound-selecting rule's articulation name ("staccato"), or
/// "natural" when none applies (Memory Management spec §4: expanded
/// articulation tracks split on exactly what playback selects).
std::string articulation_group(const notation::ExpressionMap& map, notation::ArticulationMask articulations);
/// Which of a part's articulation tracks (`groups`) plays a note, so that
/// every note plays exactly once: the map's choice when it has a track;
/// else the first written articulation that has one (tracks that each load
/// their own patch split on what is written); else the natural track (or,
/// with none, the first).
std::string articulation_route(const notation::ExpressionMap& map, notation::ArticulationMask articulations,
                               const std::vector<std::string>& groups);
/// A part's articulation tracks, ready to insert after it: each a copy of
/// the part (instrument, map, routing, mix) named "<part> - <Sound>",
/// playing its MIDI, with an empty version that `sync_scores` fills.
std::vector<Track> make_articulation_tracks(const Track& part, const std::vector<std::string>& groups, IdAllocator& ids);
/// "Staccato", "Natural".
std::string articulation_group_name(const std::string& group);

/// Mapping health (Main Spec §11 "mapping health indicator").
struct MapHealth {
    /// Articulations written in the track's score that the map does nothing
    /// for: they are shown but not heard.
    std::vector<std::string> unheard;
    /// Techniques the track asks for (its template intents) that are
    /// notated articulations with no rule.
    std::vector<std::string> unmapped_intents;
    /// The map switches sounds by articulation but has no natural switch:
    /// plain notes after an articulated one keep its sound.
    bool missing_natural = false;
    bool ok() const noexcept { return unheard.empty() && unmapped_intents.empty() && !missing_natural; }
    /// "Healthy", or "staccato, pizzicato are not heard; no natural switch".
    std::string summary() const;
};
MapHealth map_health(const Track& track);

}  // namespace vcc::project
