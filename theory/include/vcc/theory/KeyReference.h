// The key reference behind the Circle of Fifths window (Add-On Features spec
// §9): one service for circle order, key signatures, scales, scale degrees,
// diatonic chords with Roman numerals and the keys around a key, so the
// desktop window (and a later iPad one) never keep their own tables.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vcc/theory/Chord.h"
#include "vcc/theory/Pitch.h"

namespace vcc::theory {

/// One position of the circle, clockwise from C at the top.
struct CirclePosition {
    int fifths = 0;                          ///< Of the major key: 0 C, 1 G ... 6 F#, -5 Db ... -1 F.
    SpelledPitch major;                      ///< The outer key's tonic.
    std::optional<SpelledPitch> major_enharmonic;  ///< Gb for F#, C# for Db, Cb for B.
    SpelledPitch minor;                      ///< The inner (relative minor) key's tonic.
    std::optional<SpelledPitch> minor_enharmonic;  ///< Eb for D#, A# for Bb, Ab for G#.
};

/// The twelve positions, clockwise from C.
const std::vector<CirclePosition>& circle_of_fifths();
/// The position a key belongs to (its key signature's), 0-11.
int circle_index(const Key& key) noexcept;

/// How the minor scale is shown. Major keys ignore it.
enum class MinorForm { Natural, Harmonic, MelodicAscending, MelodicDescending };

struct ScaleDegree {
    int degree = 1;             ///< 1-7
    SpelledPitch note;
    std::string function;       ///< "Tonic", "Supertonic", ... "Leading tone" / "Subtonic"
    bool altered = false;       ///< Differs from the key signature (harmonic/melodic minor's 6th and 7th).
};

struct DiatonicChord {
    int degree = 1;
    std::string roman;          ///< "ii", "V7", "vii°", "iiø7", "III+"
    Chord chord;
    bool uses_altered = false;  ///< Built on a raised degree: outside the natural minor.
};

/// A chord borrowed from outside the key shown (modal mixture): built on a
/// degree of a related scale, never labelled diatonic (spec §9.3).
struct BorrowedChord {
    int degree = 1;
    std::string roman;          ///< Relative to the home key: "bVI", "iv", "bII", "I".
    Chord chord;
    std::string source;         ///< "C minor", "C major", "C Phrygian".
    std::string name;           ///< "Neapolitan", "Picardy third", or empty.
};

struct KeyReference {
    Key key;                    ///< Major, or Minor with the form below.
    MinorForm form = MinorForm::Natural;
    int fifths = 0;             ///< Key signature: + sharps, - flats.
    std::vector<SpelledPitch> signature;   ///< The signature's accidentals in order (F# C# ... or Bb Eb ...).
    std::vector<ScaleDegree> scale;        ///< Seven degrees, one letter each.
    std::vector<DiatonicChord> triads;     ///< I..vii°
    std::vector<DiatonicChord> sevenths;   ///< Imaj7..viiø7
    /// Common borrowed chords, triads and sevenths: a major key's from its
    /// parallel minor, a minor key's from its parallel major (those not
    /// already in the minor form shown), and the Neapolitan (bII, from
    /// Phrygian) for both. In degree order.
    std::vector<BorrowedChord> borrowed_triads;
    std::vector<BorrowedChord> borrowed_sevenths;
    Key relative, parallel, dominant, subdominant;
    std::string altered_note;   ///< "Raised 7th (G#)" when the form alters degrees; empty otherwise.
};

/// The reference for a major or minor key (other modes read as the major or
/// natural minor of their key signature's family).
KeyReference key_reference(const Key& key, MinorForm form = MinorForm::Natural);

/// The major key, or its relative minor, with this key signature (-7..7).
Key key_for_signature(int fifths, bool minor);

/// "1 sharp", "3 flats", "no sharps or flats".
std::string signature_text(int fifths);

}  // namespace vcc::theory
