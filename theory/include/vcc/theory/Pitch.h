// Spelled pitch, keys, modes and tuning.
//
// Templates Supplement §9.2/§9.3: "represent progressions as scale
// degrees/functions plus explicit alterations and quality, not hard-coded
// C-major note lists... use pitch-class math and a spelling layer
// separately." A SpelledPitch is that spelling layer: a letter and an
// alteration, so F# and Gb are different objects that sound the same.
// Pitch-class arithmetic (0-11) happens underneath.
//
// Major/minor labels are not universal (§9.2). Modes cover the Western
// diatonic family plus a few symmetric and pentatonic collections; anything
// else is a PitchCollection in cents with an explicit TuningSystem, so raga,
// maqam, makam or gamelan material is described honestly rather than squeezed
// into twelve equal semitones.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vcc::theory {

/// Letter 0-6 = C D E F G A B; alteration in semitones (-2 = double flat).
struct SpelledPitch {
    int letter = 0;
    int alteration = 0;

    /// Pitch class 0-11.
    int pitch_class() const noexcept;
    /// "C", "F#", "Bb", "Cx" (double sharp), "Ebb".
    std::string name() const;
    /// Same, with Unicode accidentals: "F♯", "B♭", "C𝄪".
    std::string display_name() const;
    /// Accepts "C", "c#", "Bb", "B♭", "F##", "Fx", "Ebb".
    static std::optional<SpelledPitch> parse(const std::string& text);

    /// Transposes by a spelled interval: `steps` letters and `semitones`
    /// semitones. A major third up from D is (2, 4) -> F#.
    SpelledPitch transposed(int steps, int semitones) const noexcept;
};

bool operator==(const SpelledPitch& a, const SpelledPitch& b) noexcept;
bool operator!=(const SpelledPitch& a, const SpelledPitch& b) noexcept;

/// Semitones of the natural letter above C.
int letter_pitch_class(int letter) noexcept;

enum class Mode : std::uint8_t {
    Major,
    Minor,  ///< Natural minor; alias of Aeolian with a "minor" key signature.
    HarmonicMinor,
    MelodicMinor,
    Dorian,
    Phrygian,
    Lydian,
    Mixolydian,
    Aeolian,
    Locrian,
    PhrygianDominant,
    DoubleHarmonic,
    WholeTone,
    Octatonic,  ///< Half-whole.
    MajorPentatonic,
    MinorPentatonic,
    Blues,
};

const char* to_string(Mode mode) noexcept;  ///< Content-file name: "harmonic_minor".
const char* display_name(Mode mode) noexcept;  ///< "Harmonic minor".
std::optional<Mode> mode_from_string(const std::string& text) noexcept;

/// Semitones above the tonic for each scale step.
const std::vector<int>& mode_intervals(Mode mode) noexcept;
/// Seven-note modes can be spelled one letter per degree and carry Roman
/// numerals; the others are pitch collections only.
bool is_heptatonic(Mode mode) noexcept;
/// For a non-heptatonic mode, the seven-note mode used to read Roman numerals
/// (minor pentatonic and blues read as Aeolian, major pentatonic as Major...).
Mode parent_heptatonic(Mode mode) noexcept;
/// True when the third above the tonic is minor.
bool is_minor_family(Mode mode) noexcept;

struct Key {
    SpelledPitch tonic;
    Mode mode = Mode::Major;

    /// The scale spelled one letter per degree (heptatonic modes), or by
    /// nearest sensible spelling for the others.
    std::vector<SpelledPitch> scale() const;
    /// 1-based scale degree in this key's parent heptatonic scale.
    SpelledPitch degree(int degree) const;
    /// Degree of the tonic's major scale - the reference for written
    /// accidentals in Roman numerals (bVII is a lowered major-scale 7th).
    SpelledPitch major_degree(int degree) const;
    /// Key-signature sharps (+) or flats (-) for notation, -7..7.
    /// Major-like modes count from their major parent, minor-like from the
    /// relative major of their Aeolian parent.
    int fifths() const noexcept;
    /// "G major", "E minor", "D dorian".
    std::string name() const;
    /// Accepts "G major", "E minor", "D dorian", "F# harmonic_minor",
    /// "Bb", "Em" (bare tonic = major, trailing m = minor).
    static std::optional<Key> parse(const std::string& text);
    /// True when the pitch class belongs to the key's scale.
    bool contains(int pitch_class) const;
};

/// A 12-note tuning: cents deviation for each pitch class from 12-TET,
/// relative to a reference A4 frequency.
struct TuningSystem {
    std::string name = "12-TET";
    double reference_a4_hz = 440.0;
    std::array<double, 12> cents_offset{};

    static TuningSystem equal_temperament();
    /// Historical well-temperaments and meantone, as offered by the Early
    /// Baroque templates (Supplement §6.1).
    static TuningSystem quarter_comma_meantone();
    static TuningSystem werckmeister_iii();
    double frequency(int midi_note) const noexcept;
};

/// A scale or mode given in cents above the tonic, for collections that 12
/// equal semitones do not represent (maqam, makam, raga, pelog, slendro...).
/// The caller is responsible for its provenance; nothing here asserts that a
/// particular collection is "the" tuning of a tradition.
struct PitchCollection {
    std::string name;
    std::vector<double> cents;  ///< Ascending, first is 0; octave implied at 1200.
    std::string notes;

    /// Nearest MIDI note and the pitch bend (in cents) needed to reach a
    /// step. Step may exceed size() to climb octaves; tonic_midi is step 0.
    struct Realized {
        int midi_note = 60;
        double bend_cents = 0.0;
    };
    Realized realize(int tonic_midi, int step) const;

    static PitchCollection from_mode(Mode mode);
};

}  // namespace vcc::theory
