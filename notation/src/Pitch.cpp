#include "vcc/notation/Pitch.h"

#include <cstddef>
#include <cstdio>

namespace vcc::notation {
namespace {

/// Semitones above C for each diatonic step: C D E F G A B.
constexpr int kStepSemitones[7] = {0, 2, 4, 5, 7, 9, 11};

/// Order in which sharps are added to a key signature: F C G D A E B.
constexpr Step kSharpOrder[7] = {Step::F, Step::C, Step::G, Step::D,
                                 Step::A, Step::E, Step::B};

/// Order in which flats are added: B E A D G C F - the sharp order reversed.
constexpr Step kFlatOrder[7] = {Step::B, Step::E, Step::A, Step::D,
                                Step::G, Step::C, Step::F};

}  // namespace

const char* to_string(Step step) noexcept
{
    switch (step) {
        case Step::C: return "C";
        case Step::D: return "D";
        case Step::E: return "E";
        case Step::F: return "F";
        case Step::G: return "G";
        case Step::A: return "A";
        case Step::B: break;
    }
    return "B";
}

bool step_from_string(const std::string& text, Step& out) noexcept
{
    for (int index = 0; index <= 6; ++index) {
        const auto step = static_cast<Step>(index);
        if (text == to_string(step)) {
            out = step;
            return true;
        }
    }
    return false;
}

int semitones_above_c(Step step) noexcept
{
    const auto index = static_cast<int>(step);
    return (index >= 0 && index < 7) ? kStepSemitones[index] : 0;
}

bool Pitch::is_valid() const noexcept
{
    if (static_cast<int>(step) > 6)
        return false;
    if (alteration < -2 || alteration > 2)
        return false;
    // Wider than any real instrument, but wide enough not to reject a
    // transposed extreme before transposition has been applied.
    return octave >= -1 && octave <= 10;
}

bool operator==(const Pitch& a, const Pitch& b) noexcept
{
    return a.step == b.step && a.octave == b.octave && a.alteration == b.alteration;
}

bool operator!=(const Pitch& a, const Pitch& b) noexcept
{
    return !(a == b);
}

bool operator<(const Pitch& a, const Pitch& b) noexcept
{
    const int sounding_a = midi_note_number(a);
    const int sounding_b = midi_note_number(b);
    if (sounding_a != sounding_b)
        return sounding_a < sounding_b;
    // Same sounding pitch, different spelling: order by staff position so
    // the result is stable rather than arbitrary.
    return diatonic_index(a) < diatonic_index(b);
}

int midi_note_number(const Pitch& pitch) noexcept
{
    // MIDI note 0 is C-1, so C4 = (4 + 1) * 12 = 60.
    const int note = (pitch.octave + 1) * 12 + semitones_above_c(pitch.step) + pitch.alteration;
    return (note >= 0 && note <= 127) ? note : -1;
}

int diatonic_index(const Pitch& pitch) noexcept
{
    return pitch.octave * 7 + static_cast<int>(pitch.step);
}

int semitone_interval(const Pitch& from, const Pitch& to) noexcept
{
    // Computed without the MIDI range clamp, so intervals stay correct for
    // pitches outside MIDI's range.
    const int absolute_from =
        (from.octave + 1) * 12 + semitones_above_c(from.step) + from.alteration;
    const int absolute_to = (to.octave + 1) * 12 + semitones_above_c(to.step) + to.alteration;
    return absolute_to - absolute_from;
}

bool KeySignature::alters(Step step) const noexcept
{
    return alteration_for(step) != 0;
}

int KeySignature::alteration_for(Step step) const noexcept
{
    if (fifths > 0) {
        for (int i = 0; i < fifths && i < 7; ++i) {
            if (kSharpOrder[i] == step)
                return 1;
        }
    } else if (fifths < 0) {
        const int count = -fifths;
        for (int i = 0; i < count && i < 7; ++i) {
            if (kFlatOrder[i] == step)
                return -1;
        }
    }
    return 0;
}

bool operator==(const KeySignature& a, const KeySignature& b) noexcept
{
    return a.fifths == b.fifths && a.is_minor == b.is_minor;
}

bool operator!=(const KeySignature& a, const KeySignature& b) noexcept
{
    return !(a == b);
}

const char* key_name(const KeySignature& key, char* out, std::size_t capacity) noexcept
{
    if (out == nullptr || capacity == 0)
        return "";

    // Tonic names indexed by fifths, -7 through +7.
    static const char* const kMajor[15] = {"Cb", "Gb", "Db", "Ab", "Eb", "Bb", "F",  "C",
                                           "G",  "D",  "A",  "E",  "B",  "F#", "C#"};
    static const char* const kMinor[15] = {"Ab", "Eb", "Bb", "F",  "C",  "G",  "D",  "A",
                                           "E",  "B",  "F#", "C#", "G#", "D#", "A#"};

    if (!key.is_valid()) {
        std::snprintf(out, capacity, "invalid key");
        return out;
    }

    const int index = key.fifths + 7;
    std::snprintf(out, capacity, "%s %s", key.is_minor ? kMinor[index] : kMajor[index],
                  key.is_minor ? "minor" : "major");
    return out;
}

Pitch spell_midi_note(int midi_note, const KeySignature& key) noexcept
{
    Pitch pitch;
    if (midi_note < 0 || midi_note > 127)
        return pitch;

    const int octave = midi_note / 12 - 1;
    const int semitone = midi_note % 12;

    // Natural steps first: if the pitch class is a white key, it needs no
    // accidental regardless of the key signature.
    for (int index = 0; index < 7; ++index) {
        if (kStepSemitones[index] == semitone) {
            pitch.step = static_cast<Step>(index);
            pitch.octave = static_cast<std::int8_t>(octave);
            pitch.alteration = 0;
            return pitch;
        }
    }

    // A black key. Spell it to match the key signature: flats in flat keys,
    // sharps otherwise. Without harmonic analysis this is the closest a
    // mechanical spelling gets to what a composer would write.
    const bool prefer_flats = key.fifths < 0;

    if (prefer_flats) {
        // The step above, flattened.
        for (int index = 0; index < 7; ++index) {
            if (kStepSemitones[index] == semitone + 1) {
                pitch.step = static_cast<Step>(index);
                pitch.octave = static_cast<std::int8_t>(octave);
                pitch.alteration = -1;
                return pitch;
            }
        }
    }

    // The step below, sharpened.
    for (int index = 0; index < 7; ++index) {
        if (kStepSemitones[index] == semitone - 1) {
            pitch.step = static_cast<Step>(index);
            pitch.octave = static_cast<std::int8_t>(octave);
            pitch.alteration = 1;
            return pitch;
        }
    }

    // Only reachable for semitone 0 with prefer_flats, which the natural
    // loop above already handled; kept so every path returns something sane.
    pitch.step = Step::C;
    pitch.octave = static_cast<std::int8_t>(octave);
    pitch.alteration = 0;
    return pitch;
}

const char* to_string(Clef clef) noexcept
{
    switch (clef) {
        case Clef::Treble:           return "treble";
        case Clef::Bass:             return "bass";
        case Clef::Alto:             return "alto";
        case Clef::Tenor:            return "tenor";
        case Clef::TrebleOctaveDown: return "treble 8vb";
        case Clef::TrebleOctaveUp:   return "treble 8va";
        case Clef::BassOctaveDown:   return "bass 8vb";
        case Clef::Percussion:       break;
    }
    return "percussion";
}

bool clef_from_string(const std::string& text, Clef& out) noexcept
{
    for (int index = 0; index <= static_cast<int>(Clef::Percussion); ++index) {
        const auto clef = static_cast<Clef>(index);
        if (text == to_string(clef)) {
            out = clef;
            return true;
        }
    }
    return false;
}

int bottom_line_diatonic_index(Clef clef) noexcept
{
    // Treble: bottom line is E4, diatonic index 4*7 + E(2) = 30.
    // Bass:   bottom line is G2, 2*7 + G(4) = 18.
    // Alto:   bottom line is F3, 3*7 + F(3) = 24.
    // Tenor:  bottom line is D3, 3*7 + D(1) = 22.
    switch (clef) {
        case Clef::Treble:           return 30;
        case Clef::Bass:             return 18;
        case Clef::Alto:             return 24;
        case Clef::Tenor:            return 22;
        // The octave clefs are written like their base clef; the octave
        // shift is a sounding transposition, applied elsewhere, so that what
        // the composer sees on the staff is unchanged.
        case Clef::TrebleOctaveDown: return 30;
        case Clef::TrebleOctaveUp:   return 30;
        case Clef::BassOctaveDown:   return 18;
        case Clef::Percussion:       break;
    }
    return 30;
}

int staff_position(const Pitch& pitch, Clef clef) noexcept
{
    if (clef == Clef::Percussion)
        return 0;
    return diatonic_index(pitch) - bottom_line_diatonic_index(clef);
}

}  // namespace vcc::notation
