// Copyright (c) VC Composer. All rights reserved.
//
// Pitch, clefs and key signatures (spec 02 sections 5.1, 5.3).
//
// Pitch is stored diatonically - step, octave, alteration - rather than as a
// MIDI note number, because notation has to preserve enharmonic spelling.
// F# and Gb are the same key on a piano and the same MIDI note, but they are
// different notes on a staff, they sit on different lines, and in a score
// they mean different things. A model that stored only the MIDI number could
// not render back what the composer wrote (spec 5.3: "accidentals and
// enharmonic spelling").
#pragma once

#include <cstdint>
#include <string>

namespace vcc::notation {

/// Diatonic step within an octave, C through B.
enum class Step : std::uint8_t { C = 0, D = 1, E = 2, F = 3, G = 4, A = 5, B = 6 };

const char* to_string(Step step) noexcept;

/// Reads a step back from what `to_string` wrote.
///
/// Defined as the writer's inverse - it compares against `to_string` rather
/// than keeping a second table - so the two cannot drift apart when the
/// enum grows. Returns false for anything it does not write, because a
/// name this build does not know is a fact to report, not one to default.
bool step_from_string(const std::string& text, Step& out) noexcept;

/// Semitone offset of each diatonic step above C.
int semitones_above_c(Step step) noexcept;

/// A written pitch.
struct Pitch {
    Step step = Step::C;

    /// Scientific pitch notation: middle C is C4, so octave 4 contains
    /// MIDI note 60.
    std::int8_t octave = 4;

    /// Chromatic alteration in semitones: -2 double flat, -1 flat, 0
    /// natural, +1 sharp, +2 double sharp.
    std::int8_t alteration = 0;

    bool is_valid() const noexcept;
};

bool operator==(const Pitch& a, const Pitch& b) noexcept;
bool operator!=(const Pitch& a, const Pitch& b) noexcept;

/// Orders by sounding pitch, then by written step, so a container of pitches
/// sorts the way a chord reads bottom to top.
bool operator<(const Pitch& a, const Pitch& b) noexcept;

/// MIDI note number, 0-127 in range. Middle C (C4) is 60.
///
/// Returns -1 when the pitch would fall outside the MIDI range, which is a
/// real possibility for a double-flatted C0 or a piccolo's written extremes
/// after transposition.
int midi_note_number(const Pitch& pitch) noexcept;

/// Diatonic index: octave * 7 + step. Used for staff placement and for
/// interval arithmetic that must ignore accidentals.
int diatonic_index(const Pitch& pitch) noexcept;

/// The number of semitones between two written pitches.
int semitone_interval(const Pitch& from, const Pitch& to) noexcept;

/// Key signature, as a count of sharps or flats.
///
/// `fifths` follows the convention used by MusicXML and most notation
/// software: 0 is C major/A minor, positive counts sharps (1 = G major),
/// negative counts flats (-1 = F major). The mode is carried separately
/// because it changes the tonic, not the accidentals.
struct KeySignature {
    std::int8_t fifths = 0;
    bool is_minor = false;

    bool is_valid() const noexcept { return fifths >= -7 && fifths <= 7; }

    /// True when this step carries a sharp or flat in the key signature.
    bool alters(Step step) const noexcept;

    /// The alteration this key signature applies to the given step:
    /// +1 in a sharp key, -1 in a flat key, 0 when unaltered.
    int alteration_for(Step step) const noexcept;
};

bool operator==(const KeySignature& a, const KeySignature& b) noexcept;
bool operator!=(const KeySignature& a, const KeySignature& b) noexcept;

/// Human-readable key name, for example "D major" or "Bb minor". Writes into
/// `out` (needs 24 bytes) and returns it.
const char* key_name(const KeySignature& key, char* out, std::size_t capacity) noexcept;

/// Spells a MIDI note as a written pitch appropriate to a key signature.
///
/// Used when MIDI is read back into notation. A key with flats spells the
/// black keys as flats, a key with sharps as sharps - which is what a
/// composer expects, and is the closest a mechanical process gets to correct
/// spelling without harmonic analysis.
Pitch spell_midi_note(int midi_note, const KeySignature& key) noexcept;

/// Clefs (spec 5.1: treble, bass, alto, tenor and others).
enum class Clef : std::uint8_t {
    Treble = 0,
    Bass,
    Alto,          ///< C clef on the middle line, for viola.
    Tenor,         ///< C clef on the fourth line, for trombone and cello.
    TrebleOctaveDown,  ///< Vocal tenor and guitar: sounds an octave below.
    TrebleOctaveUp,
    BassOctaveDown,
    Percussion,    ///< Unpitched; staff position carries no pitch meaning.
};

const char* to_string(Clef clef) noexcept;

/// Reads a clef back from what `to_string` wrote. The writer's inverse, as
/// `step_from_string` is.
bool clef_from_string(const std::string& text, Clef& out) noexcept;

/// The diatonic index that sits on the bottom line of the staff in this
/// clef. Staff position for any pitch follows from this.
int bottom_line_diatonic_index(Clef clef) noexcept;

/// Staff position of a pitch, in half-steps of staff space: 0 is the bottom
/// line, 1 the space above it, 2 the second line, and so on. Negative values
/// are below the staff.
///
/// Meaningless for `Clef::Percussion`, which returns 0.
int staff_position(const Pitch& pitch, Clef clef) noexcept;

}  // namespace vcc::notation
