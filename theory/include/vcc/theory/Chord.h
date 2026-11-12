// Chords, chord symbols and Roman numerals.
//
// Templates Supplement §9.1 chord selection: triads (major, minor,
// diminished, augmented, sus2/sus4), sixths, sevenths (maj7, min7, dom7,
// half-diminished, dim7, min-major7), ninths, elevenths, thirteenths, add9 and
// altered dominants, with omit/add tones.
//
// A chord tone is a *spelled interval* (generic degree plus semitones), so
// every tone gets the right letter: in E minor, iiø7 is F#-A-C-E, never
// F#-A-B#-E. Display names and voicing both come from the same tone list.
//
// Roman numeral rules (documented because conventions differ):
//   * A numeral with no accidental takes its root from the key's own scale:
//     VI in C minor is Ab, VI in C major is A.
//   * A written accidental alters the *major-scale* degree, the jazz/pop
//     convention: bVII is Bb in C whatever the mode, bII is Db, #iv is F#.
//   * Case gives the third: upper = major, lower = minor. o = diminished,
//     ø = half-diminished, + = augmented.
//   * Uppercase with 7 is a dominant seventh (V7, I7 in a blues); maj7 makes
//     it a major seventh; lowercase with maj7 is minor-major (imaj7).
//   * Figures: 6 = first inversion, 64 = second inversion; 65/43/42 (or 2) are
//     seventh-chord inversions. An added sixth is written add6.
//   * @n puts scale degree n in the bass as a pedal: bVI@1 is Ab/C in C minor.
//   * V/V is an applied chord: V of the key whose tonic is this key's V.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vcc/theory/Pitch.h"

namespace vcc::theory {

/// A spelled interval above the root. degree is generic (1 = root,
/// 3 = third, 9 = ninth); semitones is exact (9 with 1 semitone over the
/// octave = b9 -> 13 semitones).
struct ChordInterval {
    int degree = 1;
    int semitones = 0;
};

struct ChordQuality {
    std::string id;       ///< "maj7", "m7b5", "7alt" ...
    std::string symbol;   ///< Suffix as shown in a chord name: "maj7", "m7♭5".
    std::vector<ChordInterval> tones;

    bool is_seventh_or_more() const noexcept;
};

/// The quality table, in the order the Chord Explorer shows it.
const std::vector<ChordQuality>& chord_qualities();
const ChordQuality* find_quality(const std::string& id) noexcept;

struct Chord {
    SpelledPitch root;
    ChordQuality quality;
    /// 0 = root position, 1 = third in the bass, 2 = fifth, 3 = seventh.
    int inversion = 0;
    /// Explicit bass note (slash chord / pedal). Overrides inversion.
    std::optional<SpelledPitch> bass;
    /// Tones removed (by generic degree, e.g. 5) and added.
    std::vector<int> omit_degrees;
    std::vector<ChordInterval> added;

    /// Chord tones in stacking order (root first), spelled.
    std::vector<SpelledPitch> tones() const;
    /// The tones with their intervals, after omit/add.
    std::vector<ChordInterval> intervals() const;
    /// The note that sounds lowest: explicit bass, or the inverted tone.
    SpelledPitch bass_note() const;
    /// "G", "Em", "F#m7b5", "B7alt", "C/E", "Ab/C". ASCII accidentals.
    std::string name() const;
    /// Same with Unicode accidentals and ø/° where conventional.
    std::string display_name() const;
};

/// Parses a chord symbol: "Em7", "F#m7b5", "Bbmaj9", "C/E", "G7alt", "Dsus4".
std::optional<Chord> parse_chord_symbol(const std::string& text);

struct RomanNumeral {
    std::string text;    ///< As written: "bVII", "iiø7", "V7/V".
    int degree = 1;      ///< 1-7.
    int accidental = 0;  ///< Written accidental applied to the major-scale degree.
    bool has_accidental = false;
    bool upper = true;
    std::string quality_id;  ///< Resolved quality.
    int inversion = 0;
    std::optional<int> pedal_degree;          ///< From @n.
    std::optional<std::string> applied_to;    ///< "V" in V7/V.
};

/// Parses Roman numeral text. Returns nullopt with a reason in `error`.
std::optional<RomanNumeral> parse_roman(const std::string& text, std::string* error = nullptr);

/// Builds the chord a numeral names in a key.
std::optional<Chord> chord_from_roman(const std::string& numeral, const Key& key,
                                      std::string* error = nullptr);

/// The diatonic chords of a key as triads (size 3), sevenths (4) or ninths
/// (5) - the Chord Explorer's chord matrix row. Heptatonic modes only.
std::vector<Chord> diatonic_chords(const Key& key, int size);

/// Roman numeral label for a diatonic chord built from scale degree
/// `degree` with `quality` ("ii7", "V7", "viiø7").
std::string roman_label(int degree, const ChordQuality& quality);

}  // namespace vcc::theory
