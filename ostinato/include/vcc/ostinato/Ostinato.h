// Ostinato Creator engine (Templates Supplement §10).
//
// Generation is a pure function of (options, seeds): the same inputs give the
// same notes on every platform and every run (test T07). For that reason it
// uses its own PRNG and its own integer/real draws - std::uniform_*
// distributions are allowed to differ between standard libraries, which
// would make a saved variant sound different on a collaborator's Windows
// machine than on the composer's Mac.
//
// The engine knows nothing about tracks, transports or audio. Audition plays
// the returned notes; Send to Track inserts them through the project's single
// undoable transaction.
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "vcc/core/Json.h"
#include "vcc/core/Midi.h"
#include "vcc/core/Time.h"
#include "vcc/theory/Chord.h"
#include "vcc/theory/Pitch.h"

namespace vcc::ostinato {

/// SplitMix64-seeded xoshiro256**. Portable and fast; not cryptographic.
class Rng {
public:
    explicit Rng(std::uint64_t seed) noexcept;
    std::uint64_t next() noexcept;
    /// Uniform integer in [lo, hi] (inclusive), unbiased.
    int range(int lo, int hi) noexcept;
    /// Uniform real in [0, 1).
    double unit() noexcept;
    bool chance(double p) noexcept { return unit() < p; }

private:
    std::uint64_t s_[4];
};

/// Stable 64-bit mix of a seed and a label, used to derive independent
/// per-aspect streams ("rhythm", "pitch", ...) from one variant seed.
std::uint64_t derive_seed(std::uint64_t seed, const std::string& label) noexcept;

enum class Grid { Whole, Half, Quarter, Eighth, EighthTriplet, Sixteenth, ThirtySecond };
Ticks grid_ticks(Grid grid) noexcept;
const char* to_string(Grid grid) noexcept;
std::optional<Grid> grid_from_string(const std::string& text) noexcept;

/// Which pitches the pattern may use (Supplement §10.1 chord-follow options).
enum class PitchMode {
    RootOnly,
    ChordTones,
    ScaleTones,
    PassingTones,   ///< Chord tones on strong steps, scale tones between.
    Pedal,          ///< The key's tonic (or the chord root if following).
    ChromaticTension,
    ArpeggioUp,
    ArpeggioUpDown,
    Alberti,
    Octave,         ///< Root and its octave.
    Drum,           ///< Unpitched: drum-map notes, no pitched-key logic.
};
const char* to_string(PitchMode mode) noexcept;
std::optional<PitchMode> pitch_mode_from_string(const std::string& text) noexcept;

enum class Contour { Static, Up, Down, UpDown, RandomWalk };
const char* to_string(Contour c) noexcept;
std::optional<Contour> contour_from_string(const std::string& text) noexcept;

enum class Register { Low, Mid, High };

enum class AccentPattern { None, Downbeats, Offbeats, Backbeat, Groups332, Syncopated };
std::optional<AccentPattern> accent_pattern_from_string(const std::string& text) noexcept;

/// A chord sounding over a span, used for chord-follow.
struct HarmonySpan {
    Ticks start = 0;
    Ticks end = 0;
    theory::Chord chord;
};

struct OstinatoOptions {
    // Context
    theory::Key key{theory::SpelledPitch{0, 0}, theory::Mode::Minor};
    /// Optional explicit pitch collection (e.g. a maqam in cents). When set it
    /// replaces the key's scale for ScaleTones/PassingTones.
    std::optional<theory::PitchCollection> custom_scale;
    std::vector<HarmonySpan> harmony;  ///< Empty = follow the key only.
    MeterMap meter;
    int start_bar = 1;
    int length_bars = 16;

    // Instrument
    int range_low = 48;   ///< Playable range, sounding MIDI.
    int range_high = 84;
    Register reg = Register::Mid;
    std::vector<int> drum_notes = {36, 38, 42, 46};  ///< kick, snare, closed hat, open hat.

    // Pattern
    Grid grid = Grid::Sixteenth;
    PitchMode pitch_mode = PitchMode::ChordTones;
    Contour contour = Contour::UpDown;
    double density = 0.9;          ///< Probability a grid step sounds (cell level).
    double rest_probability = 0.0; ///< Extra rests, applied per cell step.
    AccentPattern accents = AccentPattern::Downbeats;
    double swing = 0.0;            ///< 0-0.5 of a grid step, applied to odd steps.
    double cell_beats = 2.0;       ///< Repeating cell length in written beats.
    double note_length = 0.5;      ///< Fraction of a step each note sounds.
    int interval_limit = 7;        ///< Max leap in semitones for random walks.
    int velocity_low = 60;
    int velocity_high = 100;
    std::string articulation;
    int channel = 0;
    bool percussion = false;

    // Development (Supplement §10.1 "evolving 4-bar segments across 16 bars")
    bool evolve = false;         ///< Mutate one aspect every 4 bars.
    bool call_response = false;  ///< Alternate bars answer with an inverted contour.

    /// Applies a preset's fields over these options.
    void apply_preset(const json::Value& preset);
};

/// Aspects that can be re-rolled independently (one parameter at a time).
enum class Aspect { Rhythm, Pitch, Accents, Register };

/// Locks keep chosen cell steps exactly as they are when a variant is varied
/// (Supplement §10.1 "mutate/lock chosen notes, beats, contours").
struct Locks {
    std::set<int> steps;        ///< Cell step indices whose rhythm+pitch are frozen.
    bool contour = false;       ///< Keep the pitch-index sequence entirely.
    bool rhythm = false;        ///< Keep the on/off pattern entirely.
};

struct Variant {
    std::string id;     ///< "A", "B", "C" or a derived label; persistent with the seed.
    std::uint64_t seed = 0;
    std::uint64_t rhythm_seed = 0;
    std::uint64_t pitch_seed = 0;
    std::uint64_t accent_seed = 0;
    std::uint64_t register_seed = 0;
    Locks locks;
    /// Seeds of the variant this one was varied from; locked steps are taken
    /// from it. Empty for a freshly generated variant.
    std::vector<std::uint64_t> parent_seeds;
    int parent_mutation = 0;
    /// Extra perturbation applied when a seed alone would duplicate a sibling
    /// (e.g. a fully dense, static pulse). Saved, so recall is exact.
    int mutation = 0;
    std::vector<NoteEvent> notes;
    Ticks start = 0;
    Ticks end = 0;

    json::Value to_json() const;  ///< Seeds and locks only; notes are re-derivable.
    static std::optional<Variant> from_json(const json::Value& v);
};

class Generator {
public:
    explicit Generator(OstinatoOptions options);

    /// One variant from a seed.
    Variant generate(std::uint64_t seed, const std::string& id = "A") const;
    /// `count` distinct variants (A, B, C, ...) from consecutive derived seeds.
    std::vector<Variant> generate_variants(std::uint64_t seed, int count = 3) const;
    /// Re-rolls one aspect of an existing variant, honouring its locks.
    Variant vary(const Variant& base, Aspect aspect, const std::string& new_id) const;
    /// Regenerates a variant's notes from its stored seeds (after load).
    Variant regenerate(const Variant& stored) const;

    const OstinatoOptions& options() const noexcept { return opt_; }

private:
    Variant build(Variant v) const;
    OstinatoOptions opt_;
};

/// Loads content/ostinato_presets.json into id -> preset JSON.
class PresetCatalog {
public:
    bool load_file(const std::string& path, std::string* error = nullptr);
    bool load(const json::Value& doc, std::string* error = nullptr);
    const json::Value* find(const std::string& id) const noexcept;
    std::vector<std::string> ids() const;

private:
    std::vector<std::pair<std::string, json::Value>> presets_;
};

}  // namespace vcc::ostinato
