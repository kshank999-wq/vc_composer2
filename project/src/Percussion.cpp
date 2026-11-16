#include "vcc/project/Percussion.h"

namespace vcc::project {

const PercussionSound* PercussionMap::by_written(int written) const noexcept
{
    for (const auto& s : sounds)
        if (s.written == written) return &s;
    return nullptr;
}

const PercussionSound* PercussionMap::by_midi(int midi) const noexcept
{
    for (const auto& s : sounds)
        if (s.midi == midi) return &s;
    return nullptr;
}

const std::vector<PercussionMap>& builtin_percussion_maps()
{
    // Positions as drum-set parts are commonly written (one line per
    // limb's reach): kick in the bottom space, snare on the third space,
    // toms around it, cymbals above the staff with x noteheads.
    static const std::vector<PercussionMap> maps = {
        {"gm_kit",
         "Drum kit (General MIDI)",
         {
             {36, "Kick", 65, 0},                // F4
             {38, "Snare", 72, 0},               // C5
             {50, "High tom", 76, 0},            // E5
             {47, "Mid tom", 74, 0},             // D5
             {45, "Low tom", 69, 0},             // A4
             {43, "Floor tom", 67, 0},           // G4
             {42, "Hi-hat (closed)", 79, 1},     // G5, x
             {46, "Hi-hat (open)", 83, 2},       // B5, circled x
             {44, "Hi-hat (foot)", 62, 1},       // D4, x
             {51, "Ride", 77, 1},                // F5, x
             {49, "Crash", 81, 1},               // A5, x
             {56, "Cowbell", 64, 3},             // E4, triangle
         }},
    };
    return maps;
}

const PercussionMap* percussion_map_named(const std::string& id) noexcept
{
    for (const auto& m : builtin_percussion_maps())
        if (m.id == id) return &m;
    return nullptr;
}

std::string default_percussion_map(const std::string& instrument_id)
{
    for (const char* kit : {"rhythm.drum_kit", "rhythm.drum_kit_brushes", "rhythm.breakbeat_kit", "rhythm.sample_drums",
                            "synth.drum_machine"})
        if (instrument_id == kit) return "gm_kit";
    return {};
}

}  // namespace vcc::project
