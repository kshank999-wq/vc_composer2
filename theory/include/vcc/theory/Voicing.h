// Chord voicing and voice leading (Templates Supplement §9.1, §9.3).
//
// A voicing is a bass note plus upper voices. The bass follows the chord
// (root, inversion, or slash/pedal bass) unless free inversions are allowed;
// the upper structure is stacked in the chosen style and, when voice leading
// is on, arranged to move as little as possible from the previous chord.
// "Deterministic algorithm minimizes total semitone movement within
// instrument range and avoids unwanted crossings, with user-controlled
// inversions/voicing locks."
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vcc/theory/Chord.h"

namespace vcc::theory {

enum class VoicingStyle {
    Closed,  ///< Upper voices within an octave, directly above the bass.
    Open,    ///< Alternate upper voices raised an octave.
    Drop2,   ///< Second voice from the top dropped an octave.
    Drop3,   ///< Third voice from the top dropped an octave.
    Spread,  ///< Bass an octave or more below a closed upper structure.
};

const char* to_string(VoicingStyle style) noexcept;
std::optional<VoicingStyle> voicing_style_from_string(const std::string& text) noexcept;

struct VoicingOptions {
    VoicingStyle style = VoicingStyle::Closed;
    /// MIDI note the upper structure is centred on (60 = middle C).
    int center = 60;
    /// Instrument range: no voice leaves it if the chord can fit at all.
    int low = 36;
    int high = 84;
    /// Minimise movement from the previous chord.
    bool voice_lead = true;
    /// Let the voice-leader choose inversions too (changes the bass line).
    bool free_inversions = false;
    /// Double the bass an octave below.
    bool double_bass = false;
};

struct Voicing {
    std::vector<int> notes;  ///< Ascending MIDI notes; notes.front() is the bass.
    int bass() const noexcept { return notes.empty() ? -1 : notes.front(); }
};

/// Voices one chord without reference to a previous one.
Voicing voice_chord(const Chord& chord, const VoicingOptions& options);

/// Voices a sequence. `locked_inversions[i]`, when set, fixes chord i's
/// inversion regardless of free_inversions (a voicing lock).
std::vector<Voicing> voice_progression(const std::vector<Chord>& chords, const VoicingOptions& options,
                                       const std::vector<std::optional<int>>& locked_inversions = {});

/// A keyboard player's voicing of a sequence: the root (or a slash chord's
/// bass) in the bass, each moving to the nearest root within
/// [options.low, options.center - 5]; above it the other tones closed and
/// voice-led through their inversions around options.center, never down
/// into the bass. The upper voices move as little as they can while the
/// bass line stays on the roots.
std::vector<Voicing> voice_root_bass(const std::vector<Chord>& chords, const VoicingOptions& options);

/// One note of a chord voiced for entry: its spelling and its MIDI note.
struct VoicedTone {
    SpelledPitch pitch;
    int midi = 60;
    /// Scientific octave of the spelled note (B#3 is MIDI 60, Cb5 is 71).
    int octave() const noexcept;
};

/// Chord Mode (Add-On spec §3): the chord as written notes. The bass - the
/// root, the inverted tone or the slash bass - sits in `bass_octave` (C4 is
/// middle C); the other tones stack above it in `style`: Closed stacks each
/// tone just above the last; Open lifts the second voice an octave; Drop2
/// drops the second voice from the top of the upper structure an octave;
/// Spread puts the bass an octave under a closed upper structure. Every
/// tone is spelled as the chord spells it. Ascending.
std::vector<VoicedTone> voice_for_entry(const Chord& chord, int bass_octave, VoicingStyle style);

/// Total semitone movement between two voicings, matching voices in order
/// (and nearest-note for unequal sizes).
int voice_movement(const Voicing& from, const Voicing& to);

}  // namespace vcc::theory
