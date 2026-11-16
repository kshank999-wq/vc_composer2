// Handwritten notation (Add-On spec §6.2): Pencil strokes on a staff become
// notes, rests, accidentals and rhythm - shown as a preview the composer can
// correct before anything is written to the score.
//
// Recognition is geometric and deterministic: each stroke is classified by
// its shape (a filled or hollow notehead, a stem, a flag, a beam, a dot, a
// rest, a sharp, flat or natural, a barline), then strokes are grouped into
// symbols (a head and its stem and flags; an accidental to a head's left,
// dots to its right; heads on one stem as a chord). Pitch comes from a
// head's staff position, rhythm from the values in writing order. Doubts
// are reported, never guessed away: a head between a line and a space
// offers both readings; a filled head without a stem, an accidental that
// cannot be told, and a bar written too full are flagged on the symbol.
//
// Coordinates are the engraving's (staff spaces, y down), so ink drawn over
// the score is recognized against the staves it was written on. The release
// scope is the spec's recommendation: notes, rests, accidentals and rhythm
// first; articulations and slurs later.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vcc::mobile {

struct InkPoint {
    double x = 0, y = 0;  ///< Staff spaces, in the track's layout.
    double t = 0;         ///< Seconds; optional.
    double force = 0;     ///< 0..1; optional - recognition never needs it.
};
using Stroke = std::vector<InkPoint>;

/// What one stroke looked like (exposed for tests and the debug overlay).
enum class StrokeShape : int {
    Unknown = 0,
    FilledHead,
    HollowHead,
    Stem,       ///< Or a barline: a long vertical line.
    Flag,
    Beam,
    Dot,
    WholeRest,  ///< A short horizontal block (whole or half by its place on the staff).
    QuarterRest,
    EighthRest,
    Flat,
    AccidentalLine,  ///< One line of a sharp or natural.
};
const char* to_string(StrokeShape shape) noexcept;
StrokeShape classify(const Stroke& stroke) noexcept;

struct Recognized {
    enum class Kind : int { Note = 0, Rest = 1 };
    Kind kind = Kind::Note;
    int value = 3;    ///< notation::NoteValue: 1 whole .. 6 thirty-second.
    int dots = 0;
    /// -2..2 written, or kNoAccidental: as the key spells it.
    static constexpr int kNoAccidental = 99;
    int alteration = kNoAccidental;
    int staff = 0;
    /// For each head of a chord (one for a single note): staff position,
    /// 0 = bottom line.
    std::vector<int> positions;
    std::int64_t tick = 0;
    /// Where the preview draws it (staff spaces): the first head's centre.
    double x = 0, y = 0;
    /// Something the composer should look at before accepting, and why:
    /// "between a line and a space", "no stem: read as a quarter", "the
    /// bar is too full", "accidental unclear". Empty when it is clean.
    bool doubtful = false;
    std::string why;
    /// For a head between a line and a space: the other reading.
    int alternative_position = 0;
    bool has_alternative = false;
    std::vector<std::size_t> strokes;  ///< Which strokes made it.
};

struct Recognition {
    std::vector<Recognized> items;
    /// Strokes no symbol used: shown in red, never silently dropped.
    std::vector<std::size_t> unused_strokes;
    std::string summary;  ///< "4 notes, 1 rest - 1 to check"
    int doubtful_count() const noexcept;
};

}  // namespace vcc::mobile
