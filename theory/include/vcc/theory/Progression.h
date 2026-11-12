// Progression catalog and the Chord Explorer's renderer (Templates
// Supplement §9.1-§9.3, acceptance criteria 12-13).
//
// A Progression is Roman numerals with durations in bars. Rendering it
// resolves the numerals in a key, voices the chords, and lays notes out at
// exact ticks from a start bar using the meter map. It never touches a
// project: auditioning is render + play; Send to Track is render + one
// undoable insert (vcc::project::Project::insert_notes).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vcc/core/Json.h"
#include "vcc/core/Midi.h"
#include "vcc/core/Time.h"
#include "vcc/theory/Chord.h"
#include "vcc/theory/Voicing.h"

namespace vcc::theory {

struct ProgressionStep {
    std::string numeral;
    double bars = 1.0;  ///< May be fractional (0.5 = half a bar).
};

struct Progression {
    std::string id;
    std::string name;
    std::string family;     ///< "pop", "jazz", "cinematic"...
    std::string tonality;   ///< "major", "minor", "modal".
    Mode default_mode = Mode::Major;
    int bars = 4;
    std::vector<ProgressionStep> steps;
    std::vector<int> allowed_bar_lengths;
    /// True for forms such as the 12-bar blues that must not be truncated
    /// or stretched to a length that is not a whole number of choruses.
    bool fixed_length = false;
    std::string cadence;
    std::vector<std::string> tags;
    std::string notes;

    /// Resolves every numeral in `key`. Fails on the first bad numeral.
    std::optional<std::vector<Chord>> chords(const Key& key, std::string* error = nullptr) const;
    /// "I–V–vi–IV".
    std::string roman_text() const;
};

class ProgressionCatalog {
public:
    /// Loads content/progressions.json. Every numeral is parsed at load time,
    /// so a typo in shipped content fails the content test, not the user.
    bool load(const json::Value& doc, std::string* error = nullptr);
    bool load_file(const std::string& path, std::string* error = nullptr);

    const std::vector<Progression>& all() const noexcept { return items_; }
    const Progression* find(const std::string& id) const noexcept;
    /// Case-insensitive match against id, name, family and tags.
    std::vector<const Progression*> search(const std::string& query) const;

private:
    std::vector<Progression> items_;
};

enum class ChordRhythm {
    Block,      ///< One sustained chord per change.
    BeatPulse,  ///< Re-struck on every felt beat.
    Arpeggio,   ///< Broken upwards in eighth notes.
    Strum,      ///< Block chord with a short low-to-high roll.
};

const char* to_string(ChordRhythm rhythm) noexcept;
std::optional<ChordRhythm> chord_rhythm_from_string(const std::string& text) noexcept;

/// How a pattern shorter than the requested length fills it.
enum class ExpandMode {
    Stretch,  ///< Each chord held proportionally longer (4 -> 8 doubles every chord).
    Repeat,   ///< The pattern played again.
};

struct ProgressionRenderOptions {
    Key key;
    int start_bar = 1;
    int length_bars = 4;  ///< Usually 4 or 8; 12-bar forms need a multiple of 12.
    ExpandMode expand = ExpandMode::Stretch;
    VoicingOptions voicing;
    std::vector<std::optional<int>> locked_inversions;  ///< Per *pattern* step.
    ChordRhythm rhythm = ChordRhythm::Block;
    int velocity = 80;
    double legato = 0.95;  ///< Fraction of each slot a note sounds.
    int channel = 0;
};

struct RealizedChord {
    std::string numeral;
    Chord chord;
    Voicing voicing;
    Ticks start = 0;
    Ticks duration = 0;
};

struct ProgressionRender {
    std::vector<RealizedChord> chords;
    std::vector<NoteEvent> notes;
    Ticks start = 0;
    Ticks end = 0;  ///< Exactly start + span(length_bars).
};

/// Renders. Returns nullopt with a reason when the request is impossible -
/// for example 4 bars of a 12-bar blues, which spec §9.2 says must never be
/// truncated silently.
std::optional<ProgressionRender> render_progression(const Progression& progression, const MeterMap& meter,
                                                    const ProgressionRenderOptions& options,
                                                    std::string* error = nullptr);

}  // namespace vcc::theory
