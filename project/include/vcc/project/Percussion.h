// Percussion maps (Main Spec §7 "percussion maps"): on a percussion staff a
// note's place says which drum, not which pitch. A map gives each sound its
// staff position (as a written pitch on the percussion clef), its notehead
// (x for cymbals and hi-hats) and the MIDI note that plays it. A drum-kit
// track carries one: entry, display and playback all go through it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vcc::project {

struct PercussionSound {
    int midi = 36;          ///< What plays (General MIDI drum map numbering).
    std::string name;       ///< "Kick", "Hi-hat (closed)".
    int written = 65;       ///< Staff position, as the MIDI number of the written pitch.
    std::uint8_t notehead = 0;  ///< As ScoreNote::notehead: 0 normal, 1 x, 2 circled x, 3 triangle, 4 diamond.
};

struct PercussionMap {
    std::string id;
    std::string name;
    std::vector<PercussionSound> sounds;
    const PercussionSound* by_written(int written) const noexcept;
    const PercussionSound* by_midi(int midi) const noexcept;
};

/// "gm_kit": a drum kit in the usual drum-set notation, General MIDI notes.
const std::vector<PercussionMap>& builtin_percussion_maps();
const PercussionMap* percussion_map_named(const std::string& id) noexcept;
/// The map an instrument gets when its track is made (drum kits), or "".
std::string default_percussion_map(const std::string& instrument_id);

}  // namespace vcc::project
