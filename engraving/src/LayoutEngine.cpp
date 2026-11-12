#include "vcc/engraving/LayoutEngine.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <string>

namespace vcc::engraving {
namespace {

/// Tuplet numbers: their size, how far a bracket sits from the notes and
/// from the staff, and how far its hooks turn in toward the notes.
constexpr double kTupletTextSize = 1.3;
constexpr double kTupletClearance = 0.75;
constexpr double kTupletStaffClearance = 1.0;
constexpr double kTupletHookLength = 0.75;

/// Estimated width of one character of a tuplet number, in staff spaces,
/// for centring it. Layout has no text metrics - see DisplayList - so this
/// is the same guess the display list's own bounds make.
constexpr double kTupletCharacterWidth = 0.65;

/// Minimum stem length for a beamed note. Shorter than this and the beam
/// visually merges with the notehead.
constexpr double kMinimumBeamedStemLength = 2.5;

/// Steepest a beam may slope, in staff spaces, and as a fraction of its
/// horizontal span. Both limits are conventional: an unlimited slope over a
/// wide beam group is unreadable.
constexpr double kMaximumBeamSlope = 2.0;
constexpr double kMaximumBeamSlopeRatio = 0.25;

/// Horizontal gap between an accidental column and the notehead it belongs
/// to, and between two accidentals in the same column.
constexpr double kAccidentalGap = 0.2;

/// Grace notes: cue size, the gap between the last and the chord it
/// ornaments, and the gap between one grace chord and the next.
constexpr double kGraceScale = 0.6;
constexpr double kGraceGap = 0.45;
constexpr double kGraceSpacing = 0.3;
constexpr double kAccidentalColumnGap = 0.16;

/// Two accidentals closer than this vertically cannot share a column.
constexpr int kAccidentalColumnClearance = 6;  // In half staff positions.

/// The staff position a percussion staff draws everything on: the middle
/// line. Staff position carries no pitch meaning there (see Pitch.h).
constexpr int kPercussionPosition = 4;

/// Sharps and flats in the order a key signature writes them.
constexpr notation::Step kSharpOrder[7] = {
    notation::Step::F, notation::Step::C, notation::Step::G, notation::Step::D,
    notation::Step::A, notation::Step::E, notation::Step::B,
};
constexpr notation::Step kFlatOrder[7] = {
    notation::Step::B, notation::Step::E, notation::Step::A, notation::Step::D,
    notation::Step::G, notation::Step::C, notation::Step::F,
};

/// The letterforms a dynamic is composed from. "mf" is a mezzo followed by a
/// forte, which is how music fonts expect dynamics to be assembled - the
/// combined glyphs exist but are not in every font.
std::size_t dynamic_glyphs(notation::DynamicLevel level, Glyph* out, std::size_t capacity)
{
    const Glyph p = Glyph::DynamicPiano;
    const Glyph m = Glyph::DynamicMezzo;
    const Glyph f = Glyph::DynamicForte;

    Glyph sequence[3]{};
    std::size_t count = 0;
    switch (level) {
        case notation::DynamicLevel::Pianississimo:
            sequence[0] = p; sequence[1] = p; sequence[2] = p; count = 3; break;
        case notation::DynamicLevel::Pianissimo:
            sequence[0] = p; sequence[1] = p; count = 2; break;
        case notation::DynamicLevel::Piano:
            sequence[0] = p; count = 1; break;
        case notation::DynamicLevel::MezzoPiano:
            sequence[0] = m; sequence[1] = p; count = 2; break;
        case notation::DynamicLevel::MezzoForte:
            sequence[0] = m; sequence[1] = f; count = 2; break;
        case notation::DynamicLevel::Forte:
            sequence[0] = f; count = 1; break;
        case notation::DynamicLevel::Fortissimo:
            sequence[0] = f; sequence[1] = f; count = 2; break;
        case notation::DynamicLevel::Fortississimo:
            sequence[0] = f; sequence[1] = f; sequence[2] = f; count = 3; break;
    }

    const std::size_t written = std::min(count, capacity);
    for (std::size_t index = 0; index < written; ++index)
        out[index] = sequence[index];
    return written;
}

/// Ticks in one beat of a time signature: a note of the denominator's value,
/// which is what a musician counts and what beams group by.
notation::Ticks beat_ticks_of(const music::TimeSignature& time_signature) noexcept
{
    if (time_signature.denominator == 0)
        return notation::kTicksPerQuarter;
    return notation::kTicksPerWhole / static_cast<notation::Ticks>(time_signature.denominator);
}

/// A cubic Bezier arching `height` away from the straight line between two
/// points. Negative arches upward, which is the direction a slur above the
/// staff takes.
CurveItem arch(const Point& from, const Point& to, double height, ItemRole role,
               notation::EventId source_id)
{
    CurveItem curve;
    curve.start = from;
    curve.end = to;
    const double dx = to.x - from.x;

    // The crown is level: both control points sit `height` beyond the end
    // that is further out on the arch's side, so a slur from a high note
    // to a low one arches over the drop rather than leaning down it. On
    // the arch's side the curve is then at least as far out as a tilted
    // arch would be, which is what the clearance under it was computed
    // for.
    const double outer = height < 0.0 ? std::min(from.y, to.y) : std::max(from.y, to.y);
    curve.control_one = Point{from.x + dx / 3.0, outer + height};
    curve.control_two = Point{from.x + dx * 2.0 / 3.0, outer + height};

    // An engraver's curve: thin at the ends, full at the middle.
    curve.tapered = true;
    curve.thickness = role == ItemRole::Tie ? 0.2 : 0.22;
    curve.end_thickness = 0.05;

    curve.role = role;
    curve.source_id = source_id;
    return curve;
}

}  // namespace

// --- Free functions ---------------------------------------------------------

std::string tuplet_label(const notation::Tuplet& ratio)
{
    // The number alone when the ratio is the one a reader assumes: more
    // notes in the time of the largest power of two below the count (3:2,
    // 5:4, 6:4, 7:4), or the compound-time duplet and quadruplet (2:3,
    // 4:6). Anything else has to say what it is.
    bool assumed = false;
    if (ratio.actual > ratio.normal) {
        std::uint16_t largest_power = 1;
        while (largest_power * 2 < ratio.actual)
            largest_power = static_cast<std::uint16_t>(largest_power * 2);
        assumed = ratio.normal == largest_power;
    } else {
        assumed = (ratio.actual == 2 && ratio.normal == 3)
               || (ratio.actual == 4 && ratio.normal == 6);
    }

    if (assumed)
        return std::to_string(ratio.actual);
    return std::to_string(ratio.actual) + ":" + std::to_string(ratio.normal);
}

bool tuplet_span_is_complete(const notation::Tuplet& ratio, notation::Ticks sounding_ticks,
                             notation::Ticks shortest_written) noexcept
{
    if (ratio.normal == 0 || sounding_ticks <= 0 || shortest_written <= 0)
        return false;
    if (sounding_ticks % ratio.normal != 0)
        return false;

    // The value the tuplet stands in for: `normal` of them fill the span.
    // It has to be a plain written value - a breve down to a 64th - and no
    // shorter than the shortest note written under the number, or three
    // sextuplet sixteenths would close as a whole 6:4 at half its length.
    const notation::Ticks value = sounding_ticks / ratio.normal;
    constexpr notation::Ticks kBreve = notation::kTicksPerQuarter * 8;
    if (value < shortest_written || kBreve % value != 0)
        return false;
    const notation::Ticks count = kBreve / value;
    return (count & (count - 1)) == 0;
}

int beam_count_for(const notation::Duration& duration) noexcept
{
    switch (duration.value) {
        case notation::NoteValue::Eighth:       return 1;
        case notation::NoteValue::Sixteenth:    return 2;
        case notation::NoteValue::ThirtySecond: return 3;
        case notation::NoteValue::SixtyFourth:  return 4;
        case notation::NoteValue::Breve:
        case notation::NoteValue::Whole:
        case notation::NoteValue::Half:
        case notation::NoteValue::Quarter:
            break;
    }
    return 0;
}

bool stem_up_for_position(int staff_position) noexcept
{
    // The middle line is position 4. A note below it takes an up stem; at or
    // above it, a down stem.
    return staff_position < 4;
}

bool stem_up_for_chord(const std::vector<int>& staff_positions) noexcept
{
    if (staff_positions.empty())
        return true;

    int lowest = staff_positions.front();
    int highest = staff_positions.front();
    for (const int position : staff_positions) {
        lowest = std::min(lowest, position);
        highest = std::max(highest, position);
    }

    // Whichever extreme is further from the middle line decides, which is
    // the conventional rule. A tie goes to a down stem - the same answer the
    // single-note rule gives a note on the middle line, which this used to
    // contradict: the first picture of real notation had a middle-line half
    // note with its stem up, alone among its neighbours.
    const int below = 4 - lowest;
    const int above = highest - 4;
    return below > above;
}

bool stem_up_for_voice(std::uint8_t voice) noexcept
{
    return voice % 2 == 0;
}

std::vector<std::int32_t> measures_with_several_voices(
    const notation::Score& score, const std::vector<notation::ScoreNote>& notes)
{
    struct Seen {
        std::uint8_t voice = 0;
        bool several = false;
    };
    std::map<std::int32_t, Seen> by_measure;

    for (const notation::ScoreNote& note : notes) {
        const std::int32_t number = score.measure_at(note.start_tick).number;
        const auto found = by_measure.find(number);
        if (found == by_measure.end()) {
            by_measure[number] = Seen{note.voice, false};
        } else if (found->second.voice != note.voice) {
            found->second.several = true;
        }
    }

    std::vector<std::int32_t> result;
    for (const auto& entry : by_measure) {
        if (entry.second.several)
            result.push_back(entry.first);
    }
    return result;
}

std::size_t key_signature_positions(const notation::KeySignature& key, notation::Clef clef,
                                    int* out, std::size_t capacity) noexcept
{
    if (out == nullptr || capacity == 0 || key.fifths == 0)
        return 0;
    if (clef == notation::Clef::Percussion)
        return 0;  // A percussion staff carries no key signature.

    const bool sharps = key.fifths > 0;
    const int wanted = sharps ? key.fifths : -key.fifths;
    const notation::Step* order = sharps ? kSharpOrder : kFlatOrder;

    // A key signature is not a set of independently placed accidentals: each
    // one steps from the one before by a fifth up or a fourth down (a fourth
    // up or a fifth down for flats), and the direction is chosen to keep the
    // group inside a band around the staff. Placing each accidental at its
    // own "nearest conventional height" instead gets the traditional pattern
    // wrong - it puts the seventh flat an octave above where it is engraved.
    //
    // The band is centred on the clef, one position higher for sharps than
    // for flats, which is where convention puts them.
    int centre = 6;
    switch (clef) {
        case notation::Clef::Treble:
        case notation::Clef::TrebleOctaveDown:
        case notation::Clef::TrebleOctaveUp:
            centre = sharps ? 6 : 4;
            break;
        case notation::Clef::Bass:
        case notation::Clef::BassOctaveDown:
            centre = sharps ? 4 : 2;
            break;
        case notation::Clef::Alto:
        case notation::Clef::Tenor:
            // The C clefs sit between the two, and the tenor clef's
            // traditional signature is lower still than this puts it.
            centre = sharps ? 5 : 3;
            break;
        case notation::Clef::Percussion:
            break;
    }
    const int band_top = centre + 3;

    // The first accidental goes at whichever octave of its step lands
    // closest to the centre of the band.
    notation::Pitch pitch;
    pitch.step = order[0];
    int position = 0;
    bool have_position = false;
    for (int octave = 0; octave <= 9; ++octave) {
        pitch.octave = static_cast<std::int8_t>(octave);
        const int candidate = notation::staff_position(pitch, clef);
        if (!have_position || std::abs(candidate - centre) < std::abs(position - centre)
            || (std::abs(candidate - centre) == std::abs(position - centre)
                && candidate > position)) {
            position = candidate;
            have_position = true;
        }
    }
    if (!have_position)
        return 0;

    const int up = sharps ? 4 : 3;    // A fifth up, or a fourth up.
    const int down = sharps ? 3 : 4;  // A fourth down, or a fifth down.

    std::size_t written = 0;
    for (int index = 0; index < wanted && written < capacity; ++index) {
        if (index > 0)
            position = (position + up <= band_top) ? position + up : position - down;
        out[written++] = position;
    }
    return written;
}

// --- LayoutOptions / LayoutResult ------------------------------------------

bool LayoutOptions::is_valid() const noexcept
{
    return staff_gap >= 0.0 && header_width >= 0.0 && ruler_height >= 0.0
        && margin_above >= 0.0 && margin_below >= 0.0 && cursor_tick >= 0;
}

void LayoutResult::clear()
{
    display_list.clear();
    notes.clear();
    staff_top_y.clear();
    extent = Rect{};
    drawn_bounds = Rect{};
    crowded_note_count = 0;
}

double LayoutEngine::BeamGroup::y_at(double x) const noexcept
{
    const double dx = end.x - start.x;
    if (std::abs(dx) < 1e-9)
        return start.y;
    const double t = (x - start.x) / dx;
    return start.y + t * (end.y - start.y);
}

// --- Construction -----------------------------------------------------------

LayoutEngine::LayoutEngine() = default;

LayoutEngine::LayoutEngine(GlyphTable glyphs, LayoutOptions options)
    : glyphs_(std::move(glyphs)), options_(options)
{
}

// --- Glyph selection --------------------------------------------------------

double LayoutEngine::advance_of(Glyph glyph) const noexcept
{
    return glyphs_.metrics(glyph).advance_width;
}

Glyph LayoutEngine::notehead_for(const notation::Duration& duration) const noexcept
{
    switch (duration.value) {
        case notation::NoteValue::Breve: return Glyph::NoteheadDoubleWhole;
        case notation::NoteValue::Whole: return Glyph::NoteheadWhole;
        case notation::NoteValue::Half:  return Glyph::NoteheadHalf;
        default: break;
    }
    return Glyph::NoteheadBlack;
}

Glyph LayoutEngine::rest_for(const notation::Duration& duration) const noexcept
{
    switch (duration.value) {
        case notation::NoteValue::Breve:        return Glyph::RestDoubleWhole;
        case notation::NoteValue::Whole:        return Glyph::RestWhole;
        case notation::NoteValue::Half:         return Glyph::RestHalf;
        case notation::NoteValue::Quarter:      return Glyph::RestQuarter;
        case notation::NoteValue::Eighth:       return Glyph::Rest8th;
        case notation::NoteValue::Sixteenth:    return Glyph::Rest16th;
        case notation::NoteValue::ThirtySecond: return Glyph::Rest32nd;
        case notation::NoteValue::SixtyFourth:  return Glyph::Rest64th;
    }
    return Glyph::RestQuarter;
}

Glyph LayoutEngine::flag_for(const notation::Duration& duration, bool stem_up) const noexcept
{
    switch (duration.value) {
        case notation::NoteValue::Eighth:
            return stem_up ? Glyph::Flag8thUp : Glyph::Flag8thDown;
        case notation::NoteValue::Sixteenth:
            return stem_up ? Glyph::Flag16thUp : Glyph::Flag16thDown;
        case notation::NoteValue::ThirtySecond:
            return stem_up ? Glyph::Flag32ndUp : Glyph::Flag32ndDown;
        case notation::NoteValue::SixtyFourth:
            return stem_up ? Glyph::Flag64thUp : Glyph::Flag64thDown;
        default: break;
    }
    return Glyph::None;
}

Glyph LayoutEngine::accidental_for(int alteration) const noexcept
{
    switch (alteration) {
        case -2: return Glyph::AccidentalDoubleFlat;
        case -1: return Glyph::AccidentalFlat;
        case 0:  return Glyph::AccidentalNatural;
        case 1:  return Glyph::AccidentalSharp;
        case 2:  return Glyph::AccidentalDoubleSharp;
        default: break;
    }
    // Beyond a double alteration there is no standard glyph, and inventing
    // one would draw something the composer did not write.
    return Glyph::None;
}

Glyph LayoutEngine::clef_glyph_for(notation::Clef clef) const noexcept
{
    switch (clef) {
        case notation::Clef::Treble:           return Glyph::GClef;
        case notation::Clef::TrebleOctaveDown: return Glyph::GClef8vb;
        case notation::Clef::TrebleOctaveUp:   return Glyph::GClef8va;
        case notation::Clef::Bass:             return Glyph::FClef;
        case notation::Clef::BassOctaveDown:   return Glyph::FClef8vb;
        case notation::Clef::Alto:
        case notation::Clef::Tenor:            return Glyph::CClef;
        case notation::Clef::Percussion:       return Glyph::UnpitchedPercussionClef;
    }
    return Glyph::GClef;
}

int LayoutEngine::clef_staff_position(notation::Clef clef) const noexcept
{
    // The line the clef names: G4 on the second line for a treble clef, F3
    // on the fourth for a bass clef, middle C on the clef's own line for a
    // C clef.
    switch (clef) {
        case notation::Clef::Treble:
        case notation::Clef::TrebleOctaveDown:
        case notation::Clef::TrebleOctaveUp:   return 2;
        case notation::Clef::Bass:
        case notation::Clef::BassOctaveDown:   return 6;
        case notation::Clef::Alto:             return 4;
        case notation::Clef::Tenor:            return 6;
        case notation::Clef::Percussion:       return 4;
    }
    return 2;
}

// --- Emission ---------------------------------------------------------------

void LayoutEngine::emit_glyph(Glyph glyph, const Point& origin, ItemRole role,
                              notation::EventId source_id, LayoutResult& result,
                              double scale) const
{
    if (glyph == Glyph::None)
        return;

    const GlyphMetrics& metrics = glyphs_.metrics(glyph);
    if (metrics.codepoint == 0) {
        // A font without this glyph is reported rather than drawn as a
        // blank: silent gaps in an engraving are worse than a loud failure.
        auto& missing = result.display_list.missing_glyphs;
        if (std::find(missing.begin(), missing.end(), glyph) == missing.end())
            missing.push_back(glyph);
        return;
    }

    GlyphItem item;
    item.glyph = glyph;
    item.origin = origin;
    item.role = role;
    item.source_id = source_id;
    item.scale = scale;
    item.bounds = Rect{origin.x + metrics.bbox_left * scale, origin.y + metrics.bbox_top * scale,
                       metrics.width() * scale, metrics.height() * scale};
    result.display_list.add(item);
}

double LayoutEngine::stem_x_for(const Chord& chord, const StaffMetrics& metrics) const noexcept
{
    if (chord.notes.empty())
        return chord.x;

    const double head_width = advance_of(notehead_for(chord.notes.front()->duration));
    // The stem runs up the right edge of the notehead column, or down its
    // left edge, inset by half its own thickness so it sits flush.
    return chord.stem_up ? chord.x + head_width - metrics.stem_thickness * 0.5
                         : chord.x + metrics.stem_thickness * 0.5;
}

// --- Structure --------------------------------------------------------------

void LayoutEngine::layout_staff_lines(const StaffContext& staff, double from_x, double to_x,
                                      LayoutResult& result) const
{
    for (int line = 0; line <= 4; ++line) {
        LineItem item;
        item.from = Point{from_x, staff.metrics.line_y(line)};
        item.to = Point{to_x, staff.metrics.line_y(line)};
        item.thickness = staff.metrics.line_thickness;
        item.role = ItemRole::StaffLine;
        result.display_list.add(item);
    }
}

double LayoutEngine::layout_header(const notation::Score& score, const StaffContext* staff,
                                   notation::Ticks at_tick, double left_x,
                                   LayoutResult* result) const
{
    const bool drawing = staff != nullptr && result != nullptr;
    const notation::Clef clef = drawing ? staff->clef : score.staff(0).clef;

    double x = left_x + 0.3;

    // Clef.
    const Glyph clef_glyph = clef_glyph_for(clef);
    if (drawing) {
        const double y = staff->metrics.y_for_staff_position(clef_staff_position(clef));
        emit_glyph(clef_glyph, Point{x, y}, ItemRole::Clef, notation::kInvalidEventId, *result);
    }
    x += advance_of(clef_glyph) + 0.4;

    // Key signature.
    const notation::KeySignature key = score.key_signature_at(at_tick);
    int positions[7]{};
    const std::size_t accidental_count =
        key_signature_positions(key, clef, positions, std::size(positions));
    const Glyph accidental = key.fifths >= 0 ? Glyph::AccidentalSharp : Glyph::AccidentalFlat;
    for (std::size_t index = 0; index < accidental_count; ++index) {
        if (drawing) {
            const double y = staff->metrics.y_for_staff_position(positions[index]);
            emit_glyph(accidental, Point{x, y}, ItemRole::KeySignature,
                       notation::kInvalidEventId, *result);
        }
        x += advance_of(accidental) + 0.08;
    }
    if (accidental_count > 0)
        x += 0.3;

    // Time signature: numerator over denominator, each centred on the other.
    if (!options_.header_time_signature)
        return x - left_x;
    const music::TimeSignature time_signature = score.time_signature_at(at_tick);
    const std::string numerator = std::to_string(time_signature.numerator);
    const std::string denominator = std::to_string(time_signature.denominator);

    const auto digits_width = [this](const std::string& text) {
        double width = 0.0;
        for (const char character : text)
            width += advance_of(time_signature_digit(character - '0'));
        return width;
    };

    const double numerator_width = digits_width(numerator);
    const double denominator_width = digits_width(denominator);
    const double group_width = std::max(numerator_width, denominator_width);

    if (drawing) {
        const auto emit_digits = [&](const std::string& text, double width, int position) {
            double digit_x = x + (group_width - width) * 0.5;
            const double y = staff->metrics.y_for_staff_position(position);
            for (const char character : text) {
                const Glyph glyph = time_signature_digit(character - '0');
                emit_glyph(glyph, Point{digit_x, y}, ItemRole::TimeSignature,
                           notation::kInvalidEventId, *result);
                digit_x += advance_of(glyph);
            }
        };
        // Positions 6 and 2 centre each digit in the upper and lower half of
        // the staff, which is where a time signature is engraved.
        emit_digits(numerator, numerator_width, 6);
        emit_digits(denominator, denominator_width, 2);
    }
    x += group_width + 0.5;

    return x - left_x;
}

void LayoutEngine::layout_barlines(const notation::Score& score, const TimelineMap& timeline,
                                   const std::vector<StaffContext>& staves,
                                   notation::Ticks from_tick, notation::Ticks to_tick,
                                   const GraceEdges& grace_edges, LayoutResult& result) const
{
    if (staves.empty() || to_tick <= from_tick)
        return;

    const double top = staves.front().metrics.top_y;
    const double bottom = staves.back().metrics.bottom_y();

    // Each measure start in view, as `barline_positions` walks them - with
    // the tick kept, because a barline at a tick with grace notes before
    // it moves left of them: they belong after it, with the note they
    // ornament, and the timeline leaves no room between.
    notation::Measure measure = score.measure_at(from_tick);
    for (int guard = 0; guard < 100000; ++guard) {
        if (measure.start_tick >= to_tick || measure.length_ticks <= 0)
            break;
        if (measure.start_tick < from_tick) {
            measure = score.measure_at(measure.end_tick());
            continue;
        }
        double x = timeline.x_for_tick(measure.start_tick);
        const auto edge = grace_edges.find(measure.start_tick);
        if (edge != grace_edges.end())
            x = std::min(x, edge->second - kGraceGap);
        measure = score.measure_at(measure.end_tick());

        LineItem item;
        item.from = Point{x, top};
        item.to = Point{x, bottom};
        item.thickness = staff_prototype_.line_thickness * 1.6;
        item.role = ItemRole::Barline;
        result.display_list.add(item);
    }
}

void LayoutEngine::layout_ruler(const notation::Score& score, const TimelineMap& timeline,
                                notation::Ticks from_tick, notation::Ticks to_tick,
                                LayoutResult& result) const
{
    const double height = options_.ruler_height;
    if (height <= 0.0)
        return;

    for (const RulerMark& mark : build_ruler(score, timeline, from_tick, to_tick)) {
        // A barline mark is full height, a beat half, a subdivision a
        // quarter, so the bar structure reads at a glance.
        double top = height * 0.5;
        if (mark.is_barline())
            top = 0.0;
        else if (mark.is_subdivision)
            top = height * 0.75;

        LineItem item;
        item.from = Point{mark.x, top};
        item.to = Point{mark.x, height};
        item.thickness = staff_prototype_.line_thickness;
        item.role = ItemRole::RulerMark;
        result.display_list.add(item);

        if (!mark.label.empty()) {
            TextItem label;
            label.text = mark.label;
            label.origin = Point{mark.x + 0.25, height - 0.3};
            label.size = 1.2;
            label.role = ItemRole::RulerLabel;
            result.display_list.add(label);
        }
    }
}

// --- Chords -----------------------------------------------------------------

std::vector<LayoutEngine::Chord> LayoutEngine::build_chords(
    const notation::Score& score, const StaffContext& staff,
    const std::vector<StaffContext>& staves, const TimelineMap& timeline,
    const std::vector<notation::ScoreNote>& notes, notation::Ticks from_tick,
    const std::vector<std::int32_t>& shared_measures,
    GraceEdges& grace_edges) const
{
    std::vector<Chord> chords;

    // Accidentals in force, keyed by diatonic index so that a C sharp does
    // not silence the accidental on the C an octave above it. Cleared at
    // every barline, which is the rule notation follows.
    std::map<int, int> in_force;
    std::int32_t current_measure = -1;

    std::size_t index = 0;
    while (index < notes.size()) {
        const notation::Ticks tick = notes[index].start_tick;

        const std::int32_t measure_number = score.measure_at(tick).number;
        if (measure_number != current_measure) {
            in_force.clear();
            current_measure = measure_number;
        }
        const bool shared = std::binary_search(shared_measures.begin(), shared_measures.end(),
                                               measure_number);

        // Every note at this tick, split by voice: notes of different voices
        // sharing a tick are separate chords with separate stems. A voice's
        // grace notes are chords of their own too, one per order, drawn
        // before the chord they ornament.
        std::map<std::pair<std::uint8_t, int>, Chord> by_voice;
        const std::size_t first_at_tick = chords.size();
        const notation::KeySignature key = score.key_signature_at(tick);

        while (index < notes.size() && notes[index].start_tick == tick) {
            const notation::ScoreNote& note = notes[index];
            ++index;
            if (note.is_rest)
                continue;

            Chord& chord = by_voice[{note.voice, note.grace ? note.grace_order + 1 : 0}];
            chord.start_tick = tick;
            chord.voice = note.voice;
            chord.grace = note.grace;
            chord.grace_order = note.grace_order;
            chord.grace_slashed = note.grace_slashed;
            chord.end_tick = std::max(chord.end_tick, note.end_tick());
            chord.notes.push_back(&note);

            // Drawn on the staff above or below where the note crosses to
            // one and there is one: its positions are on that staff, by
            // that staff's clef. The chord takes its first note's word for
            // it, since a chord split across the staves is not drawn.
            chord.home_staff = staff.index;
            if (chord.drawn_on == nullptr) {
                chord.drawn_on = &staff;
                const int shown = static_cast<int>(staff.index) + note.staff_shift;
                if (note.staff_shift != 0 && shown >= 0
                    && static_cast<std::size_t>(shown) < staves.size())
                    chord.drawn_on = &staves[static_cast<std::size_t>(shown)];
            }
            const StaffContext& drawn = *chord.drawn_on;

            const int position = drawn.clef == notation::Clef::Percussion
                                     ? kPercussionPosition
                                     : notation::staff_position(note.pitch, drawn.clef);
            chord.positions.push_back(position);

            // An accidental is written when the note's alteration differs
            // from what is already in force - from the key signature, or
            // from an earlier accidental in the same measure.
            bool needs = false;
            if (drawn.clef != notation::Clef::Percussion) {
                const int diatonic = notation::diatonic_index(note.pitch);
                const auto found = in_force.find(diatonic);
                const int expected =
                    found != in_force.end() ? found->second : key.alteration_for(note.pitch.step);
                if (note.pitch.alteration != expected) {
                    needs = true;
                    in_force[diatonic] = note.pitch.alteration;
                }
            }
            chord.needs_accidental.push_back(needs);
        }

        for (auto& entry : by_voice) {
            Chord& chord = entry.second;
            if (chord.notes.empty())
                continue;

            // Bottom to top, which is the order the offset and accidental
            // rules are stated in.
            std::vector<std::size_t> order(chord.notes.size());
            for (std::size_t k = 0; k < order.size(); ++k)
                order[k] = k;
            std::stable_sort(order.begin(), order.end(),
                             [&chord](std::size_t a, std::size_t b) {
                                 return chord.positions[a] < chord.positions[b];
                             });

            Chord sorted;
            sorted.start_tick = chord.start_tick;
            sorted.end_tick = chord.end_tick;
            sorted.voice = chord.voice;
            sorted.grace = chord.grace;
            sorted.grace_order = chord.grace_order;
            sorted.grace_slashed = chord.grace_slashed;
            sorted.home_staff = chord.home_staff;
            sorted.drawn_on = chord.drawn_on;
            for (const std::size_t k : order) {
                sorted.notes.push_back(chord.notes[k]);
                sorted.positions.push_back(chord.positions[k]);
                sorted.needs_accidental.push_back(chord.needs_accidental[k]);
            }

            // A second cannot be engraved as two heads in one column, so the
            // upper note of each second moves right. Marking only alternate
            // members keeps a cluster of thirds from cascading.
            sorted.offset.assign(sorted.notes.size(), false);
            for (std::size_t k = 1; k < sorted.positions.size(); ++k) {
                if (sorted.positions[k] - sorted.positions[k - 1] == 1 && !sorted.offset[k - 1])
                    sorted.offset[k] = true;
            }

            // Where another voice shares the measure the stem says which
            // voice this is; alone, it goes where the pitches put it.
            // A grace chord's stem goes up whatever the pitches and the
            // voice, and it is never beamed.
            sorted.stem_up = sorted.grace ? true
                           : shared       ? stem_up_for_voice(sorted.voice)
                                          : stem_up_for_chord(sorted.positions);
            sorted.in_shared_measure = shared;
            sorted.shared_head.assign(sorted.notes.size(), false);
            sorted.beam_count = sorted.grace ? 0 : beam_count_for(sorted.notes.front()->duration);
            sorted.x = timeline.x_for_tick(sorted.start_tick);
            sorted.stem_x = stem_x_for(sorted, staff.metrics);

            // Notes before the visible range were walked for their effect on
            // the accidental state, which is why they are dropped here and
            // not earlier.
            if (sorted.start_tick >= from_tick)
                chords.push_back(std::move(sorted));
        }

        // The grace chords of each voice go to the left of the chord they
        // ornament, last one nearest it, clear of that chord's accidentals
        // and of each other's.
        for (std::size_t which = first_at_tick; which < chords.size(); ++which) {
            Chord& principal = chords[which];
            if (principal.grace)
                continue;
            double right_edge = principal.x - kGraceGap;
            double widest = 0.0;
            for (std::size_t k = 0; k < principal.notes.size(); ++k) {
                if (principal.needs_accidental[k])
                    widest = std::max(widest, advance_of(accidental_for(principal.notes[k]->pitch.alteration)));
            }
            if (widest > 0.0)
                right_edge -= widest + kAccidentalGap + kAccidentalColumnGap;
            place_grace_chords(chords, first_at_tick, principal.voice, right_edge);
        }
        // Grace notes with nothing after them yet - the composer is still
        // typing - hang before the tick itself.
        for (std::uint8_t voice = 0; voice < 4; ++voice) {
            bool has_principal = false;
            bool has_grace = false;
            for (std::size_t which = first_at_tick; which < chords.size(); ++which) {
                if (chords[which].voice != voice)
                    continue;
                (chords[which].grace ? has_grace : has_principal) = true;
            }
            if (has_grace && !has_principal)
                place_grace_chords(chords, first_at_tick, voice, timeline.x_for_tick(tick) - kGraceGap);
        }
        // How far left they reach, accidentals included, for the barline
        // that may stand at this tick.
        for (std::size_t which = first_at_tick; which < chords.size(); ++which) {
            const Chord& chord = chords[which];
            if (!chord.grace)
                continue;
            double widest = 0.0;
            for (std::size_t k = 0; k < chord.notes.size(); ++k) {
                if (chord.needs_accidental[k])
                    widest = std::max(widest, advance_of(accidental_for(chord.notes[k]->pitch.alteration)));
            }
            const double left = chord.x - (widest > 0.0 ? (widest + kAccidentalGap) * kGraceScale : 0.0);
            const auto found = grace_edges.find(tick);
            if (found == grace_edges.end())
                grace_edges[tick] = left;
            else
                found->second = std::min(found->second, left);
        }

        // Two voices meeting at a unison or a second cannot share a column:
        // the heads would print on top of each other, or as one chord. The
        // down-stem voice moves a notehead to the right, so its down stem
        // stands beside the up stem rather than through the other head.
        //
        // Voices that cross - the down-stem voice wholly above the up-stem
        // one - cannot share a column either: each stem would run through
        // the other voice's head. There the sides swap, and it is the
        // up-stem voice that moves right: its stem is then on the far side
        // of the column from the other's head, and the down stem on the
        // far side from this one's. A chord moves once, whatever else it
        // meets.
        //
        // A unison of one value is the exception: two voices on one pitch
        // for one length share a single head, the up stem rising from its
        // right and the down stem falling from its left, which is how a
        // reader tells "both voices play this" from two notes. Only where
        // every note of the down-stem chord is such a unison, and only for
        // notes with stems - two whole notes on one head read as one.
        for (std::size_t upper = first_at_tick; upper < chords.size(); ++upper) {
            if (!chords[upper].stem_up || chords[upper].grace)
                continue;
            for (std::size_t lower = first_at_tick; lower < chords.size(); ++lower) {
                Chord& below = chords[lower];
                if (below.stem_up || below.grace
                    || below.shares_heads_with != static_cast<std::size_t>(-1))
                    continue;
                const Chord& above = chords[upper];

                bool all_unisons = true;
                for (std::size_t k = 0; k < below.notes.size() && all_unisons; ++k) {
                    const notation::ScoreNote& note = *below.notes[k];
                    const bool stemmed = static_cast<int>(note.duration.value)
                                      >= static_cast<int>(notation::NoteValue::Half);
                    bool matched = false;
                    for (const notation::ScoreNote* other : above.notes) {
                        if (other->pitch == note.pitch && other->duration == note.duration) {
                            matched = true;
                            break;
                        }
                    }
                    all_unisons = stemmed && matched;
                }
                if (!all_unisons)
                    continue;

                below.shared_head.assign(below.notes.size(), true);
                below.shares_heads_with = upper;
            }
        }

        std::vector<bool> moved(chords.size() - first_at_tick, false);
        const auto move_right = [&](std::size_t which) {
            if (moved[which - first_at_tick])
                return;
            Chord& chord = chords[which];
            chord.x += advance_of(notehead_for(chord.notes.front()->duration));
            chord.stem_x = stem_x_for(chord, staff.metrics);
            moved[which - first_at_tick] = true;

            // A chord drawn on this one's heads goes where they go.
            for (std::size_t other = first_at_tick; other < chords.size(); ++other) {
                if (chords[other].shares_heads_with != which || moved[other - first_at_tick])
                    continue;
                chords[other].x = chord.x;
                chords[other].stem_x = stem_x_for(chords[other], staff.metrics);
                moved[other - first_at_tick] = true;
            }
        };

        for (std::size_t upper = first_at_tick; upper < chords.size(); ++upper) {
            if (!chords[upper].stem_up || chords[upper].grace)
                continue;
            const auto upper_range = std::minmax_element(chords[upper].positions.begin(),
                                                         chords[upper].positions.end());
            const int upper_lowest = *upper_range.first;
            const int upper_highest = *upper_range.second;

            for (std::size_t lower = first_at_tick; lower < chords.size(); ++lower) {
                if (chords[lower].stem_up || chords[lower].grace
                    || chords[lower].shares_heads_with == upper)
                    continue;
                const auto lower_range = std::minmax_element(chords[lower].positions.begin(),
                                                             chords[lower].positions.end());
                const int lower_lowest = *lower_range.first;
                const int lower_highest = *lower_range.second;

                if (lower_lowest > upper_highest)
                    move_right(upper);  // Crossed.
                else if (lower_highest >= upper_lowest - 1)
                    move_right(lower);  // Touching, or interleaved.
            }
        }

        // The dots of every chord at this tick go in one column, clear of
        // every voice's heads: a dot after the up-stem voice's head would
        // print through the head the down-stem voice moved beside it.
        double right_edge = 0.0;
        for (std::size_t which = first_at_tick; which < chords.size(); ++which) {
            const Chord& chord = chords[which];
            if (chord.grace)
                continue;
            const double head_width = advance_of(notehead_for(chord.notes.front()->duration));
            const bool any_offset =
                std::find(chord.offset.begin(), chord.offset.end(), true) != chord.offset.end();
            right_edge = std::max(right_edge, chord.x + head_width + (any_offset ? head_width : 0.0));
        }
        for (std::size_t which = first_at_tick; which < chords.size(); ++which)
            chords[which].dot_x = right_edge + 0.25;

        // Where each note's dots go. A dot sits in a space, so a note on a
        // line has its dot raised to the space above - or lowered to the
        // space below in the down-stem voice of a shared measure, so the
        // two voices' dots on one line do not print on each other. Two
        // dotted notes in one space do the same: the upper voice's dot
        // stays and the lower voice's drops a space.
        for (std::size_t which = first_at_tick; which < chords.size(); ++which) {
            Chord& chord = chords[which];
            chord.dot_positions.clear();
            if (chord.grace)
                continue;
            for (const int position : chord.positions) {
                const bool on_line = position % 2 == 0;
                const bool downward = chord.in_shared_measure && !chord.stem_up;
                chord.dot_positions.push_back(on_line ? (downward ? position - 1 : position + 1)
                                                      : position);
            }
        }
        for (std::size_t lower = first_at_tick; lower < chords.size(); ++lower) {
            Chord& below = chords[lower];
            if (below.stem_up || below.grace || !below.in_shared_measure
                || below.notes.front()->duration.dots == 0)
                continue;
            for (std::size_t k = 0; k < below.dot_positions.size(); ++k) {
                bool taken = false;
                for (std::size_t upper = first_at_tick; upper < chords.size() && !taken; ++upper) {
                    const Chord& above = chords[upper];
                    if (!above.stem_up || above.grace || above.notes.front()->duration.dots == 0)
                        continue;
                    taken = std::find(above.dot_positions.begin(), above.dot_positions.end(),
                                      below.dot_positions[k])
                            != above.dot_positions.end();
                }
                if (taken)
                    below.dot_positions[k] -= 2;
            }
        }
    }

    return chords;
}

std::vector<LayoutEngine::BeamGroup> LayoutEngine::build_beam_groups(
    const notation::Score& score, const StaffContext& staff, std::vector<Chord>& chords) const
{
    std::vector<BeamGroup> groups;

    // Chords are in tick order, every voice together, so a group's members
    // are not a contiguous run: the chord after a voice-one eighth is the
    // voice-two chord at the same tick. Each voice is walked past the
    // others, and a chord already beamed is not the start of another group.
    std::vector<bool> beamed(chords.size(), false);

    for (std::size_t start = 0; start < chords.size(); ++start) {
        if (beamed[start] || chords[start].beam_count == 0)
            continue;

        // A beam group runs while the notes stay inside one beat of one
        // measure and follow each other without a gap. A rest or a hole in
        // the voice ends the beam, which is what the gap test detects.
        const notation::Measure measure = score.measure_at(chords[start].start_tick);
        const notation::Ticks beat = beat_ticks_of(measure.time_signature);
        const notation::Ticks beat_index =
            beat > 0 ? (chords[start].start_tick - measure.start_tick) / beat : 0;

        std::vector<std::size_t> members{start};
        for (std::size_t next = start + 1; next < chords.size(); ++next) {
            const Chord& candidate = chords[next];
            // Another voice's chord is neither a member nor a break, and
            // nor is a grace chord: it takes no time between the eighths.
            if (candidate.voice != chords[start].voice || candidate.grace)
                continue;
            const Chord& previous = chords[members.back()];
            if (candidate.beam_count == 0)
                break;
            if (candidate.start_tick != previous.end_tick)
                break;
            const notation::Measure candidate_measure = score.measure_at(candidate.start_tick);
            if (candidate_measure.number != measure.number)
                break;
            const notation::Ticks candidate_beat =
                beat > 0 ? (candidate.start_tick - candidate_measure.start_tick) / beat : 0;
            if (candidate_beat != beat_index)
                break;
            members.push_back(next);
        }

        if (members.size() < 2) {
            // A single beamable note keeps its flag.
            continue;
        }

        BeamGroup group;
        group.chords = members;
        for (const std::size_t index : members)
            beamed[index] = true;

        // Members drawn on both staves: the beam runs flat through the gap
        // between them, and each stem reaches it from its own side - the
        // upper staff's notes stem down to it, the lower staff's up.
        const StaffContext* first_drawn = chords[members.front()].drawn_on;
        for (const std::size_t index : members) {
            if (chords[index].drawn_on != nullptr && chords[index].drawn_on != first_drawn)
                group.cross_staff = true;
        }
        if (group.cross_staff) {
            const StaffContext* upper = first_drawn;
            const StaffContext* lower = first_drawn;
            for (const std::size_t index : members) {
                const StaffContext* drawn = chords[index].drawn_on;
                if (drawn->index < upper->index)
                    upper = drawn;
                if (drawn->index > lower->index)
                    lower = drawn;
            }
            const double y = (upper->metrics.bottom_y() + lower->metrics.top_y) * 0.5;
            group.stem_up = true;
            for (const std::size_t index : members) {
                Chord& chord = chords[index];
                chord.stem_up = chord.drawn_on->index != upper->index;
                chord.beamed = true;
                chord.stem_x = stem_x_for(chord, staff.metrics);
                chord.stem_end_y = y;
                chord.has_stem_end_override = true;
            }
            group.start = Point{chords[members.front()].stem_x, y};
            group.end = Point{chords[members.back()].stem_x, y};
            groups.push_back(std::move(group));
            continue;
        }

        // One stem direction for the whole group, decided by majority; a tie
        // goes up, matching the single-note rule.
        std::size_t up_votes = 0;
        for (const std::size_t index : group.chords) {
            if (chords[index].stem_up)
                ++up_votes;
        }
        group.stem_up = up_votes * 2 >= group.chords.size();

        for (const std::size_t index : group.chords) {
            chords[index].stem_up = group.stem_up;
            chords[index].beamed = true;
            chords[index].stem_x = stem_x_for(chords[index], staff.metrics);
        }

        // The beam line: through the ideal stem ends of the first and last
        // notes, with the slope limited, then shifted so no stem in the
        // group is shorter than the minimum.
        const auto extreme_y = [&](const Chord& chord) {
            const auto bounds = std::minmax_element(chord.positions.begin(),
                                                    chord.positions.end());
            const int position = group.stem_up ? *bounds.second : *bounds.first;
            return staff.metrics.y_for_staff_position(position);
        };
        const double direction = group.stem_up ? -1.0 : 1.0;

        const Chord& first = chords[group.chords.front()];
        const Chord& last = chords[group.chords.back()];

        const double first_ideal = extreme_y(first) + direction * staff.metrics.stem_length;
        const double last_ideal = extreme_y(last) + direction * staff.metrics.stem_length;

        const double dx = last.stem_x - first.stem_x;
        double dy = last_ideal - first_ideal;
        const double slope_limit =
            std::min(kMaximumBeamSlope, std::abs(dx) * kMaximumBeamSlopeRatio);
        dy = std::max(-slope_limit, std::min(slope_limit, dy));

        group.start = Point{first.stem_x, first_ideal};
        group.end = Point{last.stem_x, first_ideal + dy};

        double shift = 0.0;
        for (const std::size_t index : group.chords) {
            const Chord& chord = chords[index];
            const double line_y = group.y_at(chord.stem_x);
            const double required =
                extreme_y(chord) + direction * kMinimumBeamedStemLength;
            if (group.stem_up) {
                shift = std::min(shift, required - line_y);
            } else {
                shift = std::max(shift, required - line_y);
            }
        }
        group.start.y += shift;
        group.end.y += shift;

        for (const std::size_t index : group.chords) {
            chords[index].stem_end_y = group.y_at(chords[index].stem_x);
            chords[index].has_stem_end_override = true;
        }

        groups.push_back(std::move(group));
    }

    return groups;
}

void LayoutEngine::place_grace_chords(std::vector<Chord>& chords, std::size_t first_at_tick,
                                      std::uint8_t voice, double right_edge) const
{
    // Highest order nearest the principal, walking leftwards.
    for (int order = 255; order >= 0; --order) {
        for (std::size_t which = first_at_tick; which < chords.size(); ++which) {
            Chord& chord = chords[which];
            if (!chord.grace || chord.voice != voice || chord.grace_order != order)
                continue;
            const double head_width =
                advance_of(notehead_for(chord.notes.front()->duration)) * kGraceScale;
            chord.x = right_edge - head_width;
            double widest = 0.0;
            for (std::size_t k = 0; k < chord.notes.size(); ++k) {
                if (chord.needs_accidental[k])
                    widest = std::max(widest, advance_of(accidental_for(chord.notes[k]->pitch.alteration)));
            }
            right_edge = chord.x - kGraceSpacing
                       - (widest > 0.0 ? (widest + kAccidentalGap) * kGraceScale : 0.0);
        }
    }
}

void LayoutEngine::layout_grace_chord(const StaffContext& staff, Chord& chord,
                                      LayoutResult& result) const
{
    const notation::Duration& duration = chord.notes.front()->duration;
    const Glyph head = notehead_for(duration);
    const double head_width = advance_of(head) * kGraceScale;

    int lowest = chord.positions.front();
    int highest = chord.positions.front();
    for (const int position : chord.positions) {
        lowest = std::min(lowest, position);
        highest = std::max(highest, position);
    }

    // A short up stem, as grace notes are written, from the lowest head
    // past the highest.
    chord.stem_up = true;
    chord.stem_x = chord.x + head_width - staff.metrics.stem_thickness * 0.5;
    const double near_y = staff.metrics.y_for_staff_position(lowest);
    const double stem_end_y =
        staff.metrics.y_for_staff_position(highest) - staff.metrics.stem_length * 0.75;
    chord.stem_end_y = stem_end_y;

    for (std::size_t index = 0; index < chord.notes.size(); ++index) {
        const notation::ScoreNote& note = *chord.notes[index];
        const int position = chord.positions[index];
        const double x = chord.x + (chord.offset[index] ? head_width : 0.0);
        const double y = staff.metrics.y_for_staff_position(position);

        emit_glyph(head, Point{x, y}, ItemRole::Notehead, note.id, result, kGraceScale);

        if (staff.metrics.needs_ledger_lines(position)) {
            int lines[16]{};
            const std::size_t count =
                staff.metrics.ledger_line_positions(position, lines, std::size(lines));
            for (std::size_t line = 0; line < count; ++line) {
                LineItem item;
                const double line_y = staff.metrics.y_for_staff_position(lines[line]);
                const double extension = staff.metrics.ledger_extension * kGraceScale;
                item.from = Point{x - extension, line_y};
                item.to = Point{x + head_width + extension, line_y};
                item.thickness = staff.metrics.line_thickness * 1.3;
                item.role = ItemRole::LedgerLine;
                item.source_id = note.id;
                result.display_list.add(item);
            }
        }

        if (chord.needs_accidental[index]) {
            const Glyph glyph = accidental_for(note.pitch.alteration);
            const double accidental_x =
                chord.x - (kAccidentalGap + advance_of(glyph)) * kGraceScale;
            emit_glyph(glyph, Point{accidental_x, y}, ItemRole::Accidental, note.id, result,
                       kGraceScale);
        }

        LaidOutNote laid;
        laid.id = note.id;
        laid.staff_index = chord.home_staff;
        laid.start_tick = note.start_tick;
        laid.end_tick = note.end_tick();
        laid.centre = Point{x + head_width * 0.5, y};
        laid.notehead_bounds = Rect{x, y - 0.5 * kGraceScale, head_width, kGraceScale};
        laid.staff_position = position;
        laid.is_rest = false;
        laid.stem_up = true;
        laid.voice = note.voice;
        laid.has_stem = true;
        laid.stem_x = chord.stem_x;
        laid.stem_end_y = stem_end_y;
        laid.is_offset = chord.offset[index];
        laid.is_grace = true;
        result.notes.push_back(laid);
    }

    LineItem stem;
    stem.from = Point{chord.stem_x, near_y};
    stem.to = Point{chord.stem_x, stem_end_y};
    stem.thickness = staff.metrics.stem_thickness;
    stem.role = ItemRole::Stem;
    stem.source_id = chord.notes.front()->id;
    result.display_list.add(stem);

    if (beam_count_for(duration) > 0) {
        const Glyph flag = flag_for(duration, true);
        emit_glyph(flag, Point{chord.stem_x - staff.metrics.stem_thickness * 0.5, stem_end_y},
                   ItemRole::Flag, chord.notes.front()->id, result, kGraceScale);
    }

    // The acciaccatura's slash, through the stem below the flag, rising to
    // the right.
    if (chord.grace_slashed) {
        LineItem slash;
        const double centre_y = stem_end_y + (near_y - stem_end_y) * 0.4;
        slash.from = Point{chord.stem_x - 0.7, centre_y + 0.45};
        slash.to = Point{chord.stem_x + 0.7, centre_y - 0.45};
        slash.thickness = staff.metrics.stem_thickness;
        slash.role = ItemRole::GraceSlash;
        slash.source_id = chord.notes.front()->id;
        result.display_list.add(slash);
    }
}

void LayoutEngine::layout_chord(const StaffContext& staff, const TimelineMap& timeline,
                                Chord& chord, LayoutResult& result) const
{
    if (chord.notes.empty())
        return;
    if (chord.grace) {
        layout_grace_chord(staff, chord, result);
        return;
    }

    const notation::Duration& duration = chord.notes.front()->duration;
    const Glyph head = notehead_for(duration);
    const double head_width = advance_of(head);
    const double dot_advance = advance_of(Glyph::AugmentationDot);

    if (timeline.is_crowded_at(duration.ticks(), head_width))
        result.crowded_note_count += chord.notes.size();

    int lowest = chord.positions.front();
    int highest = chord.positions.front();
    for (const int position : chord.positions) {
        lowest = std::min(lowest, position);
        highest = std::max(highest, position);
    }

    // --- Where the stem ends, decided first --------------------------------
    // The noteheads record it, so a slur can be fitted to the stem it
    // starts from without searching the display list for the line.
    const bool has_stem = static_cast<int>(duration.value)
                       >= static_cast<int>(notation::NoteValue::Half);
    double stem_end_y = 0.0;
    if (has_stem) {
        const double far_y =
            staff.metrics.y_for_staff_position(chord.stem_up ? highest : lowest);
        const double middle_y = staff.metrics.line_y(2);

        stem_end_y = chord.stem_end_y;
        if (!chord.has_stem_end_override) {
            // An unbeamed stem is the conventional length, but always long
            // enough to reach the middle line from outside the staff.
            stem_end_y = chord.stem_up ? std::min(far_y - staff.metrics.stem_length, middle_y)
                                       : std::max(far_y + staff.metrics.stem_length, middle_y);
            chord.stem_end_y = stem_end_y;
        }
    }

    // --- Noteheads, ledger lines and dots ---------------------------------
    for (std::size_t index = 0; index < chord.notes.size(); ++index) {
        const notation::ScoreNote& note = *chord.notes[index];
        const int position = chord.positions[index];
        const bool offset = chord.offset[index];

        const double x = chord.x + (offset ? head_width : 0.0);
        const double y = staff.metrics.y_for_staff_position(position);

        // A head another voice draws is not drawn twice: this chord
        // contributes its stem to it, and its own place in the result.
        const bool shared = index < chord.shared_head.size() && chord.shared_head[index];

        if (!shared)
            emit_glyph(head, Point{x, y}, ItemRole::Notehead, note.id, result);

        if (!shared && staff.metrics.needs_ledger_lines(position)) {
            int lines[16]{};
            const std::size_t count =
                staff.metrics.ledger_line_positions(position, lines, std::size(lines));
            for (std::size_t line = 0; line < count; ++line) {
                LineItem item;
                const double line_y = staff.metrics.y_for_staff_position(lines[line]);
                item.from = Point{x - staff.metrics.ledger_extension, line_y};
                item.to = Point{x + head_width + staff.metrics.ledger_extension, line_y};
                item.thickness = staff.metrics.line_thickness * 1.3;
                item.role = ItemRole::LedgerLine;
                item.source_id = note.id;
                result.display_list.add(item);
            }
        }

        // Augmentation dots sit in a space - which space, and in which
        // column, build_chords decided with the other voices in view.
        const int dot_position = index < chord.dot_positions.size()
                                     ? chord.dot_positions[index]
                                     : ((position % 2 == 0) ? position + 1 : position);
        double dot_x = chord.dot_x > 0.0 ? chord.dot_x : x + head_width + 0.25;
        for (std::uint8_t dot = 0; dot < duration.dots && !shared; ++dot) {
            emit_glyph(Glyph::AugmentationDot,
                       Point{dot_x, staff.metrics.y_for_staff_position(dot_position)},
                       ItemRole::AugmentationDot, note.id, result);
            dot_x += dot_advance + 0.15;
        }

        LaidOutNote laid;
        laid.id = note.id;
        laid.staff_index = chord.home_staff;
        laid.start_tick = note.start_tick;
        laid.end_tick = note.end_tick();
        laid.centre = Point{x + head_width * 0.5, y};
        laid.notehead_bounds = Rect{x, y - 0.5, head_width, 1.0};
        laid.staff_position = position;
        laid.is_rest = false;
        laid.stem_up = chord.stem_up;
        laid.voice = note.voice;
        laid.has_stem = has_stem;
        laid.stem_x = chord.stem_x;
        laid.stem_end_y = stem_end_y;
        laid.is_offset = offset;
        result.notes.push_back(laid);

        // Where another voice shares the measure the marks go on the
        // voice's outer side - the stem side, beyond the stem - so the
        // upper voice's marks are above it and the lower voice's below,
        // rather than both between the voices.
        if (options_.draw_articulations && note.articulations != 0) {
            layout_articulations(staff, note, laid.centre, chord.stem_up,
                                 chord.in_shared_measure && has_stem, stem_end_y, result);
        }
    }

    // --- Accidentals -------------------------------------------------------
    // Placed top note first and packed into columns to the left of the
    // chord: two accidentals too close vertically cannot share a column.
    double widest = 0.0;
    for (std::size_t index = 0; index < chord.notes.size(); ++index) {
        if (chord.needs_accidental[index])
            widest = std::max(widest, advance_of(accidental_for(chord.notes[index]->pitch.alteration)));
    }

    if (widest > 0.0) {
        const double column_width = widest + kAccidentalColumnGap;
        std::vector<std::pair<std::size_t, int>> columns;  // index, column.

        for (std::size_t reverse = chord.notes.size(); reverse > 0; --reverse) {
            const std::size_t index = reverse - 1;
            if (!chord.needs_accidental[index])
                continue;

            int column = 0;
            bool placed = false;
            while (!placed) {
                placed = true;
                for (const auto& existing : columns) {
                    if (existing.second != column)
                        continue;
                    const int gap =
                        std::abs(chord.positions[existing.first] - chord.positions[index]);
                    if (gap < kAccidentalColumnClearance) {
                        placed = false;
                        break;
                    }
                }
                if (!placed)
                    ++column;
            }
            columns.emplace_back(index, column);

            const Glyph glyph = accidental_for(chord.notes[index]->pitch.alteration);
            const double x = chord.x - kAccidentalGap
                           - static_cast<double>(column + 1) * column_width;
            const double y = staff.metrics.y_for_staff_position(chord.positions[index]);
            emit_glyph(glyph, Point{x, y}, ItemRole::Accidental, chord.notes[index]->id, result);
        }
    }

    // --- Stem and flag -----------------------------------------------------
    if (!has_stem)
        return;

    const double near_y =
        staff.metrics.y_for_staff_position(chord.stem_up ? lowest : highest);
    const double end_y = stem_end_y;

    LineItem stem;
    stem.from = Point{chord.stem_x, near_y};
    stem.to = Point{chord.stem_x, end_y};
    stem.thickness = staff.metrics.stem_thickness;
    stem.role = ItemRole::Stem;
    stem.source_id = chord.notes.front()->id;
    result.display_list.add(stem);

    if (!chord.beamed && chord.beam_count > 0) {
        const Glyph flag = flag_for(duration, chord.stem_up);
        // The flag hangs from the free end of the stem, on the stem's side
        // of the notehead column.
        // A SMuFL flag is drawn from the stem's own edge, which is the same
        // point whichever way the stem points.
        const double flag_x = chord.stem_x - staff.metrics.stem_thickness * 0.5;
        emit_glyph(flag, Point{flag_x, end_y}, ItemRole::Flag, chord.notes.front()->id, result);
    }
}

void LayoutEngine::layout_rest(const StaffContext& staff, const TimelineMap& timeline,
                               const notation::ScoreNote& rest, bool shared,
                               const std::vector<notation::ScoreNote>& notes,
                               LayoutResult& result) const
{
    const Glyph glyph = rest_for(rest.duration);
    const double x = timeline.x_for_tick(rest.start_tick);

    if (timeline.is_crowded_at(rest.duration.ticks(), advance_of(Glyph::NoteheadBlack)))
        ++result.crowded_note_count;

    // A whole rest hangs from the second line down; everything shorter is
    // centred on the middle line. Where another voice shares the measure
    // the rest moves a space toward its voice's stem side - up for the
    // first voice, down for the second - so the voices' rests do not print
    // on top of each other.
    const bool hangs = rest.duration.value == notation::NoteValue::Whole
                    || rest.duration.value == notation::NoteValue::Breve;
    int position = hangs ? 6 : 4;
    if (shared) {
        const bool up = stem_up_for_voice(rest.voice);
        position += up ? 2 : -2;

        // A space is enough to clear the other voice's rests, not its
        // notes: a held note on the middle line prints straight through a
        // rest dropped one space. So the rest keeps its own half-height
        // and a notehead's clear of whatever another voice sounds through
        // it, on the side it moved to.
        constexpr int kClearance = 4;
        for (const notation::ScoreNote& other : notes) {
            if (other.is_rest || other.voice == rest.voice)
                continue;
            if (other.start_tick >= rest.end_tick() || other.end_tick() <= rest.start_tick)
                continue;
            if (staff.clef == notation::Clef::Percussion)
                continue;
            const int note_position = notation::staff_position(other.pitch, staff.clef);
            if (up)
                position = std::max(position, note_position + kClearance);
            else
                position = std::min(position, note_position - kClearance);
        }
    }
    const double y = staff.metrics.y_for_staff_position(position);
    emit_glyph(glyph, Point{x, y}, ItemRole::Rest, rest.id, result);

    // The dot sits in the space above a centred rest, and in the space a
    // hanging rest hangs into.
    const int dot_position = hangs ? position - 1 : position + 1;
    const double width = advance_of(glyph);
    double dot_x = x + width + 0.25;
    for (std::uint8_t dot = 0; dot < rest.duration.dots; ++dot) {
        emit_glyph(Glyph::AugmentationDot,
                   Point{dot_x, staff.metrics.y_for_staff_position(dot_position)},
                   ItemRole::AugmentationDot, rest.id, result);
        dot_x += advance_of(Glyph::AugmentationDot) + 0.15;
    }

    LaidOutNote laid;
    laid.id = rest.id;
    laid.staff_index = staff.index;
    laid.start_tick = rest.start_tick;
    laid.end_tick = rest.end_tick();
    laid.centre = Point{x + width * 0.5, y};
    laid.notehead_bounds = Rect{x, y - 0.75, width, 1.5};
    laid.staff_position = position;
    laid.is_rest = true;
    laid.voice = rest.voice;
    result.notes.push_back(laid);
}

void LayoutEngine::layout_beams(const StaffContext& staff, const std::vector<Chord>& chords,
                                const std::vector<BeamGroup>& groups,
                                LayoutResult& result) const
{
    const double thickness = staff.metrics.beam_thickness;
    const double step = thickness + staff.metrics.beam_gap;

    for (const BeamGroup& group : groups) {
        if (group.chords.size() < 2)
            continue;

        // Beams stack inward from the stem ends: downward under an up stem,
        // upward over a down stem.
        const double direction = group.stem_up ? 1.0 : -1.0;

        int deepest = 0;
        for (const std::size_t index : group.chords)
            deepest = std::max(deepest, chords[index].beam_count);

        const notation::EventId source = chords[group.chords.front()].notes.empty()
                                             ? notation::kInvalidEventId
                                             : chords[group.chords.front()].notes.front()->id;

        const auto emit_span = [&](double from_x, double to_x, int level) {
            const double offset = static_cast<double>(level - 1) * step * direction;
            const double from_edge = group.y_at(from_x) + offset;
            const double to_edge = group.y_at(to_x) + offset;

            QuadItem quad;
            if (group.stem_up) {
                quad.corners[0] = Point{from_x, from_edge};
                quad.corners[1] = Point{to_x, to_edge};
                quad.corners[2] = Point{to_x, to_edge + thickness};
                quad.corners[3] = Point{from_x, from_edge + thickness};
            } else {
                quad.corners[0] = Point{from_x, from_edge - thickness};
                quad.corners[1] = Point{to_x, to_edge - thickness};
                quad.corners[2] = Point{to_x, to_edge};
                quad.corners[3] = Point{from_x, from_edge};
            }
            quad.role = ItemRole::Beam;
            quad.source_id = source;
            result.display_list.add(quad);
        };

        // The stems are drawn to the beam line, so the primary beam has to
        // cover the full thickness of the stem ends at both ends of the
        // group.
        const double half_stem = staff.metrics.stem_thickness * 0.5;
        const double left = chords[group.chords.front()].stem_x - half_stem;
        const double right = chords[group.chords.back()].stem_x + half_stem;

        emit_span(left, right, 1);

        for (int level = 2; level <= deepest; ++level) {
            std::size_t position = 0;
            while (position < group.chords.size()) {
                if (chords[group.chords[position]].beam_count < level) {
                    ++position;
                    continue;
                }

                std::size_t run_end = position;
                while (run_end + 1 < group.chords.size()
                       && chords[group.chords[run_end + 1]].beam_count >= level)
                    ++run_end;

                if (run_end > position) {
                    emit_span(chords[group.chords[position]].stem_x - half_stem,
                              chords[group.chords[run_end]].stem_x + half_stem, level);
                } else {
                    // A lone note needing this beam takes a stub, pointing
                    // back toward the group unless it is the first note.
                    constexpr double kStubLength = 1.0;
                    const double x = chords[group.chords[position]].stem_x;
                    if (position == 0)
                        emit_span(x - half_stem, x + kStubLength, level);
                    else
                        emit_span(x - kStubLength, x + half_stem, level);
                }

                position = run_end + 1;
            }
        }
    }
}

// --- Tuplets ----------------------------------------------------------------

std::vector<LayoutEngine::TupletGroup> LayoutEngine::build_tuplet_groups(
    const notation::Score& score, const TimelineMap& timeline,
    const std::vector<notation::ScoreNote>& notes, notation::Ticks from_tick,
    const std::vector<Chord>& chords, std::vector<TupletEvent>& events) const
{
    events.clear();

    for (std::size_t index = 0; index < chords.size(); ++index) {
        const Chord& chord = chords[index];
        // A grace chord takes no time, so it is neither one of a tuplet's
        // events nor a gap between two of them.
        if (chord.notes.empty() || chord.grace)
            continue;
        TupletEvent event;
        event.start_tick = chord.start_tick;
        event.end_tick = chord.end_tick;
        event.voice = chord.voice;
        event.ratio = chord.notes.front()->duration.tuplet;
        event.outer = chord.notes.front()->duration.outer;
        event.id = chord.notes.front()->id;
        event.chord = index;
        event.x = chord.x;
        event.width = advance_of(notehead_for(chord.notes.front()->duration));
        events.push_back(event);
    }

    // Rests are laid out on their own and are not chords, but a rest inside
    // a triplet is still one of its three.
    for (const notation::ScoreNote& note : notes) {
        if (!note.is_rest || note.start_tick < from_tick)
            continue;
        TupletEvent event;
        event.start_tick = note.start_tick;
        event.end_tick = note.end_tick();
        event.voice = note.voice;
        event.ratio = note.duration.tuplet;
        event.outer = note.duration.outer;
        event.id = note.id;
        event.x = timeline.x_for_tick(note.start_tick);
        event.width = advance_of(rest_for(note.duration));
        events.push_back(event);
    }

    std::stable_sort(events.begin(), events.end(),
                     [](const TupletEvent& a, const TupletEvent& b) {
                         if (a.voice != b.voice)
                             return a.voice < b.voice;
                         return a.start_tick < b.start_tick;
                     });

    // What was written under the numbers: the duration with every ratio
    // taken back off, which is exact because the ratios were applied
    // exactly.
    const auto written_ticks = [](const TupletEvent& event) {
        const notation::Ticks sounding = event.end_tick - event.start_tick;
        return sounding * event.ratio.actual * event.outer.actual
               / (event.ratio.normal * event.outer.normal);
    };

    // The ratio an event is read by at each level: at the outer level a
    // nested tuplet is one of its outer tuplet's events, at the inner level
    // only a nested tuplet is anything at all.
    const auto ratio_at = [](const TupletEvent& event, int level) {
        if (level == 0)
            return event.outer.is_tuplet() ? event.outer : event.ratio;
        return event.outer.is_tuplet() ? event.ratio : notation::Tuplet{};
    };
    // An inner group's span, in its outer tuplet's time: that is where
    // three triplet sixteenths add up to a written eighth.
    const auto span_at = [](const TupletEvent& event, int level) {
        const notation::Ticks sounding = event.end_tick - event.start_tick;
        return level == 0 ? sounding : sounding * event.outer.actual / event.outer.normal;
    };

    std::vector<TupletGroup> groups;

    for (int level = 0; level <= 1; ++level) {
        std::size_t start = 0;
        while (start < events.size()) {
            const notation::Tuplet first_ratio = ratio_at(events[start], level);
            if (!first_ratio.is_tuplet() || !first_ratio.is_valid()) {
                ++start;
                continue;
            }

            TupletGroup group;
            group.ratio = first_ratio;
            group.level = level;
            const notation::Measure measure = score.measure_at(events[start].start_tick);

            notation::Ticks sounding = 0;
            notation::Ticks shortest = 0;
            std::size_t end = start;
            while (end < events.size()) {
                const TupletEvent& event = events[end];
                if (end > start) {
                    const TupletEvent& previous = events[end - 1];
                    if (event.voice != previous.voice || ratio_at(event, level) != group.ratio
                        || event.start_tick != previous.end_tick
                        || score.measure_at(event.start_tick).number != measure.number) {
                        break;
                    }
                }

                group.events.push_back(end);
                sounding += span_at(event, level);
                const notation::Ticks written = written_ticks(event);
                shortest = shortest == 0 ? written : std::min(shortest, written);
                ++end;

                if (tuplet_span_is_complete(group.ratio, sounding, shortest)) {
                    group.complete = true;
                    break;
                }
            }

            groups.push_back(std::move(group));
            start = end;
        }
    }

    return groups;
}

void LayoutEngine::layout_tuplets(const StaffContext& staff, const std::vector<Chord>& chords,
                                  const std::vector<BeamGroup>& beams,
                                  const std::vector<TupletEvent>& events,
                                  const std::vector<TupletGroup>& groups,
                                  LayoutResult& result) const
{
    const double middle_y = staff.metrics.line_y(2);

    // What the inner groups drew, so an outer group can clear it: the far
    // edge of each number or bracket, on its side, over its span.
    struct InnerExtent {
        double left = 0.0;
        double right = 0.0;
        double edge_y = 0.0;
        bool above = true;
    };
    std::vector<InnerExtent> inner_extents;

    // Inner groups first, nearest the notes; then the outer ones beyond.
    std::vector<const TupletGroup*> ordered;
    for (const TupletGroup& group : groups) {
        if (group.level > 0)
            ordered.push_back(&group);
    }
    for (const TupletGroup& group : groups) {
        if (group.level == 0)
            ordered.push_back(&group);
    }

    for (const TupletGroup* group_pointer : ordered) {
        const TupletGroup& group = *group_pointer;
        if (group.events.empty())
            continue;

        const TupletEvent& first = events[group.events.front()];
        const TupletEvent& last = events[group.events.back()];
        const std::string label = tuplet_label(group.ratio);
        const double label_width = kTupletCharacterWidth * kTupletTextSize
                                 * static_cast<double>(label.size());

        // An outer group with a nested tuplet inside it: always bracketed,
        // since the inner number is in the way of "the beam is the
        // bracket", and the whole figure is what the reader has to see.
        bool holds_nested = false;
        for (const std::size_t index : group.events)
            holds_nested = holds_nested || events[index].outer.is_tuplet();
        const bool outer_of_nested = group.level == 0 && holds_nested;

        // A group one beam already holds together takes the number alone,
        // against the beam: the beam is the bracket.
        const BeamGroup* holding_beam = nullptr;
        if (first.chord != TupletEvent::kNoChord && !outer_of_nested) {
            for (const BeamGroup& beam : beams) {
                if (std::find(beam.chords.begin(), beam.chords.end(), first.chord)
                    == beam.chords.end()) {
                    continue;
                }
                bool holds_all = true;
                for (const std::size_t index : group.events) {
                    const TupletEvent& event = events[index];
                    if (event.chord == TupletEvent::kNoChord
                        || std::find(beam.chords.begin(), beam.chords.end(), event.chord)
                               == beam.chords.end()) {
                        holds_all = false;
                        break;
                    }
                }
                if (holds_all)
                    holding_beam = &beam;
                break;
            }
        }

        const double left = first.x;
        const double right = last.x + last.width;
        const double mid = (left + right) * 0.5;

        TextItem number;
        number.text = label;
        number.size = kTupletTextSize;
        number.italic = true;
        number.role = ItemRole::TupletBracket;
        number.source_id = first.id;

        if (holding_beam != nullptr) {
            // Just beyond the beam at the group's middle, and outside the
            // staff even when the beam is inside it. Beams stack inward from
            // the stem ends, so the beam line itself is the outer edge.
            const double beam_y = holding_beam->y_at(mid);
            const double baseline =
                holding_beam->stem_up
                    ? std::min(beam_y - kTupletClearance,
                               staff.metrics.top_y - kTupletStaffClearance)
                    : std::max(beam_y + kTupletClearance,
                               staff.metrics.bottom_y() + kTupletStaffClearance)
                          + kTupletTextSize * 0.75;
            number.origin = Point{mid - label_width * 0.5, baseline};
            result.display_list.add(number);
            if (group.level > 0) {
                inner_extents.push_back(InnerExtent{
                    left, right,
                    holding_beam->stem_up ? baseline - kTupletTextSize * 0.75 : baseline,
                    holding_beam->stem_up});
            }
            continue;
        }

        // The bracket goes on the stem side, decided by the chords; a group
        // of rests, or of stemless notes, goes above.
        std::size_t up_votes = 0;
        std::size_t voters = 0;
        for (const std::size_t index : group.events) {
            const TupletEvent& event = events[index];
            if (event.chord == TupletEvent::kNoChord)
                continue;
            ++voters;
            if (chords[event.chord].stem_up)
                ++up_votes;
        }
        const bool above = voters == 0 || up_votes * 2 >= voters;

        // Clear of everything in the group on that side: stem ends, the far
        // notehead of a stemless chord, and the rests, which reach about a
        // space and a half either side of the middle line - and, for an
        // outer group, whatever an inner group drew on that side within
        // its span.
        double extreme = above ? middle_y - 1.5 : middle_y + 1.5;
        if (group.level == 0) {
            for (const InnerExtent& inner : inner_extents) {
                if (inner.above != above || inner.right < first.x || inner.left > last.x + last.width)
                    continue;
                extreme = above ? std::min(extreme, inner.edge_y) : std::max(extreme, inner.edge_y);
            }
        }
        for (const std::size_t index : group.events) {
            const TupletEvent& event = events[index];
            if (event.chord == TupletEvent::kNoChord)
                continue;
            const Chord& chord = chords[event.chord];
            const bool has_stem = static_cast<int>(chord.notes.front()->duration.value)
                               >= static_cast<int>(notation::NoteValue::Half);
            const auto bounds = std::minmax_element(chord.positions.begin(),
                                                    chord.positions.end());
            const double far_head =
                staff.metrics.y_for_staff_position(above ? *bounds.second : *bounds.first)
                + (above ? -0.5 : 0.5);
            const double reach = has_stem && chord.stem_up == above ? chord.stem_end_y : far_head;
            extreme = above ? std::min(extreme, reach) : std::max(extreme, reach);
        }

        const double bracket_y =
            above ? std::min(staff.metrics.top_y - kTupletStaffClearance,
                             extreme - kTupletClearance)
                  : std::max(staff.metrics.bottom_y() + kTupletStaffClearance,
                             extreme + kTupletClearance);
        const double hook_direction = above ? 1.0 : -1.0;  // Toward the notes.

        const auto emit_line = [&](Point from, Point to) {
            LineItem line;
            line.from = from;
            line.to = to;
            line.thickness = staff.metrics.stem_thickness;
            line.role = ItemRole::TupletBracket;
            line.source_id = first.id;
            result.display_list.add(line);
        };

        emit_line(Point{left, bracket_y}, Point{left, bracket_y + hook_direction * kTupletHookLength});
        emit_line(Point{right, bracket_y},
                  Point{right, bracket_y + hook_direction * kTupletHookLength});

        // The number sits in a gap in the bracket, centred on the line. A
        // bracket too short for the gap is drawn whole and the number goes
        // just beyond it.
        const double gap = label_width + 0.6;
        if (right - left > gap + 1.0) {
            emit_line(Point{left, bracket_y}, Point{mid - gap * 0.5, bracket_y});
            emit_line(Point{mid + gap * 0.5, bracket_y}, Point{right, bracket_y});
            number.origin = Point{mid - label_width * 0.5, bracket_y + kTupletTextSize * 0.35};
        } else {
            emit_line(Point{left, bracket_y}, Point{right, bracket_y});
            const double baseline = above ? bracket_y - 0.3
                                          : bracket_y + 0.3 + kTupletTextSize * 0.75;
            number.origin = Point{mid - label_width * 0.5, baseline};
        }
        result.display_list.add(number);

        if (group.level > 0) {
            // The far edge of what was drawn: the number's top or bottom,
            // which reaches past the bracket line.
            const double edge = above ? std::min(bracket_y, number.origin.y - kTupletTextSize * 0.75)
                                      : std::max(bracket_y, number.origin.y);
            inner_extents.push_back(InnerExtent{left, right, edge, above});
        }
    }
}

// --- Ties, slurs and markings ----------------------------------------------

void LayoutEngine::layout_ties(const notation::Score& score, const StaffContext& staff,
                               const TimelineMap& timeline, const std::vector<Chord>& chords,
                               LayoutResult& result) const
{
    const std::vector<notation::ScoreNote>& all = score.notes(staff.index);

    for (const Chord& chord : chords) {
        const double head_width = advance_of(notehead_for(chord.notes.front()->duration));

        for (std::size_t index = 0; index < chord.notes.size(); ++index) {
            const notation::ScoreNote& note = *chord.notes[index];
            if (!note.tied_to_next)
                continue;

            // The partner is the next note of the same pitch and voice that
            // starts where this one ends. When there is none the tie is
            // dangling - drawn short rather than dropped, because a tie the
            // composer wrote and cannot see is worse than a visible stub.
            const notation::ScoreNote* partner = nullptr;
            for (const notation::ScoreNote& candidate : all) {
                if (candidate.start_tick < note.end_tick())
                    continue;
                if (candidate.start_tick > note.end_tick())
                    break;
                if (candidate.voice == note.voice && candidate.pitch == note.pitch
                    && !candidate.is_rest) {
                    partner = &candidate;
                    break;
                }
            }

            const double from_x = chord.x + (chord.offset[index] ? head_width : 0.0);
            const double y = staff.metrics.y_for_staff_position(chord.positions[index]);

            const double to_x = partner != nullptr
                                    ? timeline.x_for_tick(partner->start_tick)
                                    : from_x + head_width + 1.0;

            // A tie arches away from the stem so the two do not cross -
            // except where another voice shares the measure, when it
            // arches toward the voice's outer side, which is the stem
            // side: a tie between the voices would cross the other voice.
            const double side = (chord.stem_up != chord.in_shared_measure) ? 1.0 : -1.0;
            const Point start{from_x + head_width + 0.1, y + side * 0.45};
            const Point end{std::max(to_x - 0.1, start.x + 0.4), y + side * 0.45};

            result.display_list.add(arch(start, end, side * 0.6, ItemRole::Tie, note.id));
        }
    }
}

void LayoutEngine::layout_slurs(const notation::Score& score, const StaffContext& staff,
                                const TimelineMap& timeline, notation::Ticks from_tick,
                                notation::Ticks to_tick,
                                const std::vector<std::int32_t>& shared_measures,
                                LayoutResult& result) const
{
    // Everything this staff has drawn, as boxes, for the slur to clear.
    // What counts as this staff's is anything between the staff's bottom
    // line and the gap above it: the staff above ends a gap away, and a
    // note of its that reaches this far down is as much in the way.
    struct Box {
        double left, right, top, bottom;
    };
    std::vector<Box> boxes;
    for (const GlyphItem& item : result.display_list.glyphs)
        boxes.push_back({item.bounds.x, item.bounds.right(), item.bounds.y, item.bounds.bottom()});
    for (const LineItem& item : result.display_list.lines) {
        if (item.role != ItemRole::Stem && item.role != ItemRole::LedgerLine)
            continue;
        boxes.push_back({std::min(item.from.x, item.to.x), std::max(item.from.x, item.to.x),
                         std::min(item.from.y, item.to.y), std::max(item.from.y, item.to.y)});
    }
    for (const QuadItem& item : result.display_list.quads) {
        Box box{item.corners[0].x, item.corners[0].x, item.corners[0].y, item.corners[0].y};
        for (const Point& corner : item.corners) {
            box.left = std::min(box.left, corner.x);
            box.right = std::max(box.right, corner.x);
            box.top = std::min(box.top, corner.y);
            box.bottom = std::max(box.bottom, corner.y);
        }
        boxes.push_back(box);
    }
    const double reach_above = staff.metrics.top_y - options_.staff_gap * 0.75;
    const double reach_below = staff.metrics.bottom_y() + options_.staff_gap * 0.75;

    constexpr double kClearance = 0.6;
    constexpr double kFromHead = 0.6;
    constexpr double kFromStem = 0.4;

    for (const notation::Slur& slur : score.slurs()) {
        if (slur.staff != staff.index)
            continue;
        if (slur.end_tick <= from_tick || slur.start_tick >= to_tick)
            continue;

        // The notes it joins: the first and last under it, in its voice.
        // A chord counts once, by whichever of its heads is nearest the
        // slur's side, decided below.
        const LaidOutNote* first = nullptr;
        const LaidOutNote* last = nullptr;
        bool any_up = false;
        bool any_down = false;
        for (const LaidOutNote& note : result.notes) {
            if (note.staff_index != staff.index || note.is_rest || note.voice != slur.voice)
                continue;
            if (note.start_tick < slur.start_tick || note.start_tick >= slur.end_tick)
                continue;
            if (first == nullptr || note.start_tick < first->start_tick)
                first = &note;
            if (last == nullptr || note.start_tick > last->start_tick)
                last = &note;
            if (note.has_stem) {
                any_up = any_up || note.stem_up;
                any_down = any_down || !note.stem_up;
            }
        }

        if (first == nullptr || last == nullptr || first->start_tick == last->start_tick) {
            // Nothing, or one note, to fit to: one flat arch above the
            // staff, clear of what lies under its span, as every slur
            // used to be.
            const double x1 = timeline.x_for_tick(slur.start_tick);
            const double x2 = timeline.x_for_tick(slur.end_tick);
            if (x2 - x1 < 0.4)
                continue;
            double y = staff.metrics.top_y - 1.5;
            for (const Box& box : boxes) {
                if (box.right < x1 || box.left > x2)
                    continue;
                if (box.bottom < reach_above || box.top > staff.metrics.bottom_y())
                    continue;
                y = std::min(y, box.top - kClearance);
            }
            result.display_list.add(arch(Point{x1, y}, Point{x2, y}, -1.2, ItemRole::Slur,
                                         notation::kInvalidEventId));
            continue;
        }

        // Which side. Away from the stems when they agree, above when they
        // do not - and where the voice chose the stems, on the voice's
        // outer side: the upper voice's slurs go above its up stems, the
        // lower voice's below its down stems, so the two do not meet in the
        // middle of the staff.
        bool above = !any_up || any_down;
        if (std::binary_search(shared_measures.begin(), shared_measures.end(),
                               score.measure_at(first->start_tick).number)) {
            above = stem_up_for_voice(slur.voice);
        }

        // Each end: just outside the notehead on the slur's side, or just
        // past the stem's end when the stem points toward the slur. A
        // chord's end goes by the head nearest the slur, which is the one
        // whose stem it shares.
        const auto end_of = [&](const LaidOutNote& note) {
            // The extreme head of this chord on the slur's side: the same
            // chord's notes share a start tick, voice and stem.
            const LaidOutNote* extreme = &note;
            for (const LaidOutNote& other : result.notes) {
                if (other.staff_index != note.staff_index || other.voice != note.voice
                    || other.start_tick != note.start_tick || other.is_rest)
                    continue;
                if (above ? other.notehead_bounds.y < extreme->notehead_bounds.y
                          : other.notehead_bounds.bottom() > extreme->notehead_bounds.bottom())
                    extreme = &other;
            }
            const bool stem_toward = extreme->has_stem && extreme->stem_up == above;
            Point at = stem_toward
                           ? Point{extreme->stem_x, above ? extreme->stem_end_y - kFromStem
                                                          : extreme->stem_end_y + kFromStem}
                           : Point{extreme->centre.x,
                                   above ? extreme->notehead_bounds.y - kFromHead
                                         : extreme->notehead_bounds.bottom() + kFromHead};

            // Past this note's own articulations on the slur's side: the
            // first picture of a fitted slur had it starting on top of the
            // accent over its first note.
            for (const GlyphItem& item : result.display_list.glyphs) {
                if (item.role != ItemRole::Articulation || item.source_id != extreme->id)
                    continue;
                if (above)
                    at.y = std::min(at.y, item.bounds.y - kFromStem);
                else
                    at.y = std::max(at.y, item.bounds.bottom() + kFromStem);
            }
            return at;
        };
        const Point start = end_of(*first);
        const Point end = end_of(*last);
        if (end.x - start.x < 0.4)
            continue;

        // How high it arches: a little for a short slur, more for a long
        // one, and at least enough to clear whatever lies between its ends
        // - a stem or a head further out than the ends are. The arch's
        // bump at any point is 3t(1-t) of its height, so the height each
        // thing needs follows from where under the slur it sits.
        const double width = end.x - start.x;
        double height = std::min(2.0, 0.6 + width * 0.12);
        for (const Box& box : boxes) {
            if (box.right <= start.x + 0.5 || box.left >= end.x - 0.5)
                continue;
            if (above ? box.bottom < reach_above : box.top > reach_below)
                continue;
            const double centre = std::max(start.x + 0.5, std::min(end.x - 0.5,
                                                                   (box.left + box.right) * 0.5));
            const double t = std::max(0.1, std::min(0.9, (centre - start.x) / width));
            const double baseline = start.y + (end.y - start.y) * t;
            const double needed = above ? baseline - (box.top - kClearance)
                                        : (box.bottom + kClearance) - baseline;
            if (needed > 0.0)
                height = std::max(height, needed / (3.0 * t * (1.0 - t)));
        }

        result.display_list.add(arch(start, end, above ? -height : height, ItemRole::Slur,
                                     notation::kInvalidEventId));
    }
}

double LayoutEngine::marking_baseline(const StaffContext& staff, bool below,
                                      const LayoutResult& result) const
{
    constexpr double kClearance = 2.2;

    double extreme = below ? staff.metrics.bottom_y() : staff.metrics.top_y;

    // What was drawn in this staff's band - the staff and half the gap to
    // its neighbours - by where it was drawn rather than which staff it
    // belongs to: a note crossed to the staff below is the staff below's
    // for this purpose, and a staccato under the bass staff is not what a
    // marking between the staves has to clear.
    const bool first = staff.index == 0;
    const bool last = staff.index + 1 >= result.staff_top_y.size();
    const double band_top = first ? -1e9 : staff.metrics.top_y - options_.staff_gap * 0.5;
    const double band_bottom = last ? 1e9 : staff.metrics.bottom_y() + options_.staff_gap * 0.5;
    const auto in_band = [&](double y) { return y >= band_top && y <= band_bottom; };

    for (const LaidOutNote& note : result.notes) {
        if (!in_band(note.centre.y))
            continue;
        extreme = below ? std::max(extreme, note.notehead_bounds.bottom())
                        : std::min(extreme, note.notehead_bounds.y);
    }

    // Articulations too: a staccato dot under a low note is exactly what a
    // dynamic would otherwise land on.
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role != ItemRole::Articulation)
            continue;
        if (below && in_band(item.bounds.y) && item.bounds.bottom() > staff.metrics.top_y)
            extreme = std::max(extreme, item.bounds.bottom());
        if (!below && in_band(item.bounds.bottom()) && item.bounds.y < staff.metrics.bottom_y())
            extreme = std::min(extreme, item.bounds.y);
    }

    return below ? extreme + kClearance : extreme - kClearance;
}

namespace {

/// How much further out each extra row of markings sits.
constexpr double kMarkingRowGap = 2.0;

bool measure_is_shared(const notation::Score& score,
                       const std::vector<std::int32_t>& shared_measures, notation::Ticks tick)
{
    return std::binary_search(shared_measures.begin(), shared_measures.end(),
                              score.measure_at(tick).number);
}

}  // namespace

bool LayoutEngine::marking_goes_below(const notation::Score& score, const MarkingStaff& staff,
                                      std::uint8_t staff_index, std::uint8_t voice,
                                      notation::Ticks from_tick, notation::Ticks to_tick) const
{
    if (!measure_is_shared(score, staff.shared_measures, from_tick))
        return staff.default_below;

    // A voice's outer side, as its stems, slurs and marks take.
    if (!stem_up_for_voice(voice))
        return true;
    if (voice != 0)
        return false;

    // Voice 0 governs every voice that has no marking of its own (the
    // MIDI reads it that way), so it keeps the staff's default - unless a
    // down-stem voice has its own marking here on this staff, which makes
    // this one the upper voice's: then each goes on its voice's side
    // rather than both on one.
    for (const notation::DynamicMarking& other : score.dynamics()) {
        if (stem_up_for_voice(other.voice) || other.staff != staff_index)
            continue;
        if (other.tick >= from_tick && other.tick <= to_tick)
            return false;
    }
    for (const notation::Hairpin& other : score.hairpins()) {
        if (stem_up_for_voice(other.voice) || other.staff != staff_index)
            continue;
        if (other.start_tick <= to_tick && other.end_tick > from_tick)
            return false;
    }
    return staff.default_below;
}

namespace {

/// The staff a marking is drawn against: its own, or the last there is
/// for a marking from a file with more staves than this score.
std::uint8_t marking_staff_index(std::uint8_t marked, std::size_t staff_count)
{
    const std::size_t last = staff_count > 0 ? staff_count - 1 : 0;
    return static_cast<std::uint8_t>(std::min<std::size_t>(marked, last));
}

}  // namespace

void LayoutEngine::layout_dynamics(const notation::Score& score, const TimelineMap& timeline,
                                   notation::Ticks from_tick, notation::Ticks to_tick,
                                   const std::vector<MarkingStaff>& staves,
                                   LayoutResult& result) const
{
    if (staves.empty())
        return;

    for (const notation::DynamicMarking& marking : score.dynamics()) {
        if (marking.tick < from_tick || marking.tick >= to_tick)
            continue;

        const std::uint8_t on = marking_staff_index(marking.staff, staves.size());
        const MarkingStaff& staff = staves[on];
        const bool below =
            marking_goes_below(score, staff, on, marking.voice, marking.tick, marking.tick);

        // Another voice's dynamic at this tick on this staff and side: the
        // lower-numbered voice keeps the baseline and this one goes a row
        // out, rather than the two printing over each other.
        int row = 0;
        std::vector<std::uint8_t> counted;
        for (const notation::DynamicMarking& other : score.dynamics()) {
            if (other.tick != marking.tick || other.voice >= marking.voice
                || marking_staff_index(other.staff, staves.size()) != on)
                continue;
            if (std::find(counted.begin(), counted.end(), other.voice) != counted.end())
                continue;
            if (marking_goes_below(score, staff, on, other.voice, other.tick, other.tick) != below)
                continue;
            counted.push_back(other.voice);
            ++row;
        }

        const double y = below ? staff.baselines.below + 0.4 + row * kMarkingRowGap
                               : staff.baselines.above - 0.4 - row * kMarkingRowGap;
        const double x = timeline.x_for_tick(marking.tick);

        if (!marking.text.empty()) {
            // An extensible text dynamic renders as what was written; the
            // level behind it is a performance concern, not a display one.
            TextItem item;
            item.text = marking.text;
            item.origin = Point{x, y};
            item.size = 1.6;
            item.italic = true;
            item.role = ItemRole::Dynamic;
            result.display_list.add(item);
            continue;
        }

        Glyph letters[3]{};
        const std::size_t count = dynamic_glyphs(marking.level, letters, std::size(letters));
        double letter_x = x;
        for (std::size_t index = 0; index < count; ++index) {
            emit_glyph(letters[index], Point{letter_x, y}, ItemRole::Dynamic,
                       notation::kInvalidEventId, result);
            letter_x += advance_of(letters[index]);
        }
    }
}

void LayoutEngine::layout_texts(const notation::Score& score, const TimelineMap& timeline,
                                const StaffContext& staff, notation::Ticks from_tick,
                                notation::Ticks to_tick, LayoutResult& result) const
{
    // Above the top staff, as a rehearsal letter or a cue point is, and
    // never inside a voice: the model's text carries a voice for the MIDI
    // it drives, not for where it is drawn.
    constexpr double kSize = 1.5;
    constexpr double kAboveStaff = 1.5;
    constexpr double kAboveInk = 0.8;

    for (const notation::TextMarking& marking : score.texts()) {
        if (marking.tick < from_tick || marking.tick >= to_tick || marking.text.empty())
            continue;

        const double x = timeline.x_for_tick(marking.tick);
        const double width = static_cast<double>(marking.text.size()) * kSize * 0.5;

        // Clear of whatever this staff's notes put under it: heads above
        // the staff, and stems rising past them.
        double y = staff.metrics.top_y - kAboveStaff;
        for (const LaidOutNote& note : result.notes) {
            if (note.staff_index != staff.index || note.is_rest)
                continue;
            const double left = note.notehead_bounds.x;
            const double right = left + note.notehead_bounds.width;
            if (right < x || left > x + width)
                continue;
            double top = note.notehead_bounds.y;
            if (note.has_stem && note.stem_up)
                top = std::min(top, note.stem_end_y);
            y = std::min(y, top - kAboveInk);
        }

        // And of the dynamics and hairpins a voice put above the staff: the
        // marking stays nearest the music, and the text goes over it.
        for (const GlyphItem& glyph : result.display_list.glyphs) {
            if (glyph.role != ItemRole::Dynamic || glyph.bounds.y > staff.metrics.top_y)
                continue;
            if (glyph.bounds.x + glyph.bounds.width < x || glyph.bounds.x > x + width)
                continue;
            y = std::min(y, glyph.bounds.y - kAboveInk);
        }
        for (const LineItem& line : result.display_list.lines) {
            if (line.role != ItemRole::Hairpin)
                continue;
            const double top = std::min(line.from.y, line.to.y);
            if (top > staff.metrics.top_y)
                continue;
            if (std::max(line.from.x, line.to.x) < x || std::min(line.from.x, line.to.x) > x + width)
                continue;
            y = std::min(y, top - kAboveInk);
        }

        TextItem item;
        item.text = marking.text;
        item.origin = Point{x, y};
        item.size = kSize;
        item.italic = true;
        item.role = ItemRole::Text;
        result.display_list.add(item);
    }
}

double LayoutEngine::dynamic_width_at(const notation::Score& score, notation::Ticks tick,
                                      std::uint8_t voice, std::uint8_t staff) const
{
    for (const notation::DynamicMarking& marking : score.dynamics()) {
        if (marking.tick != tick || marking.voice != voice || marking.staff != staff)
            continue;
        if (!marking.text.empty())
            return kTupletCharacterWidth * 1.6 * static_cast<double>(marking.text.size());
        Glyph letters[3]{};
        const std::size_t count = dynamic_glyphs(marking.level, letters, std::size(letters));
        double width = 0.0;
        for (std::size_t index = 0; index < count; ++index)
            width += advance_of(letters[index]);
        return width;
    }
    return 0.0;
}

void LayoutEngine::layout_hairpins(const notation::Score& score, const TimelineMap& timeline,
                                   notation::Ticks from_tick, notation::Ticks to_tick,
                                   const std::vector<MarkingStaff>& staves,
                                   LayoutResult& result) const
{
    constexpr double kAperture = 0.55;
    if (staves.empty())
        return;

    for (const notation::Hairpin& hairpin : score.hairpins()) {
        if (hairpin.end_tick <= from_tick || hairpin.start_tick >= to_tick)
            continue;

        const std::uint8_t on = marking_staff_index(hairpin.staff, staves.size());
        const MarkingStaff& staff = staves[on];
        const bool below = marking_goes_below(score, staff, on, hairpin.voice,
                                              hairpin.start_tick, hairpin.end_tick);

        // A lower-numbered voice's hairpin overlapping this one on this
        // staff and side keeps the baseline; this one goes a row out.
        int row = 0;
        std::vector<std::uint8_t> counted;
        for (const notation::Hairpin& other : score.hairpins()) {
            if (other.voice >= hairpin.voice || other.start_tick >= hairpin.end_tick
                || other.end_tick <= hairpin.start_tick
                || marking_staff_index(other.staff, staves.size()) != on)
                continue;
            if (std::find(counted.begin(), counted.end(), other.voice) != counted.end())
                continue;
            if (marking_goes_below(score, staff, on, other.voice, other.start_tick, other.end_tick)
                != below)
                continue;
            counted.push_back(other.voice);
            ++row;
        }
        const double baseline = below ? staff.baselines.below + row * kMarkingRowGap
                                      : staff.baselines.above - row * kMarkingRowGap;

        // Clipped to the view rather than skipped, so a hairpin that starts
        // off-screen still shows where it is going.
        const notation::Ticks start = std::max(hairpin.start_tick, from_tick);
        const notation::Ticks end = std::min(hairpin.end_tick, to_tick);

        // A dynamic written at the hairpin's start or end shares its tick
        // and its baseline, so the hairpin gives way: it begins after a
        // marking at its start and stops short of one at its end. The first
        // picture of real notation had "dim." underlined by the diminuendo
        // that began on it.
        constexpr double kGap = 0.4;
        double x1 = timeline.x_for_tick(start);
        double x2 = timeline.x_for_tick(end);
        if (start == hairpin.start_tick) {
            const double width =
                dynamic_width_at(score, hairpin.start_tick, hairpin.voice, hairpin.staff);
            if (width > 0.0)
                x1 += width + kGap;
        }
        if (end == hairpin.end_tick
            && dynamic_width_at(score, hairpin.end_tick, hairpin.voice, hairpin.staff) > 0.0)
            x2 -= kGap;
        if (x2 - x1 < 0.3)
            continue;

        const bool opening = hairpin.kind == notation::HairpinKind::Crescendo;
        const double apex_x = opening ? x1 : x2;
        const double mouth_x = opening ? x2 : x1;

        for (const double side : {-1.0, 1.0}) {
            LineItem item;
            item.from = Point{apex_x, baseline};
            item.to = Point{mouth_x, baseline + side * kAperture};
            item.thickness = staff_prototype_.line_thickness;
            item.role = ItemRole::Hairpin;
            result.display_list.add(item);
        }
    }
}

void LayoutEngine::layout_articulations(const StaffContext& staff,
                                        const notation::ScoreNote& note,
                                        const Point& notehead_centre, bool stem_up,
                                        bool on_stem_side, double stem_end_y,
                                        LayoutResult& result) const
{
    // An articulation goes on the side of the notehead away from the stem -
    // or, where two voices share a staff, on the voice's outer side, which
    // is the stem side, beyond the stem's free end.
    const bool below = stem_up != on_stem_side;
    const double direction = below ? 1.0 : -1.0;

    // Start clear of the notehead - or of the stem end - and clear of the
    // staff when that is inside it, so the mark is not sitting on a line.
    // Closer to the staff than to a notehead: a mark a full space outside
    // the staff read as belonging to nothing in particular.
    constexpr double kFromNotehead = 1.1;
    constexpr double kFromStem = 0.6;
    constexpr double kFromStaff = 0.75;
    const double anchor_y = on_stem_side ? stem_end_y : notehead_centre.y;
    double distance = on_stem_side ? kFromStem : kFromNotehead;
    if (below)
        distance = std::max(distance, staff.metrics.y_for_staff_position(0) + kFromStaff - anchor_y);
    else
        distance = std::max(distance, anchor_y - (staff.metrics.y_for_staff_position(8) - kFromStaff));
    // Measured from the anchor from here on.
    const Point anchor{notehead_centre.x, anchor_y};

    struct Entry {
        notation::Articulation articulation;
        Glyph above;
        Glyph below;
    };

    // Only the articulations with a glyph in the table are drawn. The rest
    // (pizzicato, harmonic, mute and the jazz inflections) are engraved as
    // text or as instrument-specific symbols this engine does not carry yet;
    // they still drive MIDI through the expression map.
    static const Entry kEntries[] = {
        {notation::Articulation::Accent, Glyph::ArticAccentAbove, Glyph::ArticAccentBelow},
        {notation::Articulation::Tenuto, Glyph::ArticTenutoAbove, Glyph::ArticTenutoBelow},
        {notation::Articulation::Staccato, Glyph::ArticStaccatoAbove, Glyph::ArticStaccatoBelow},
        {notation::Articulation::Staccatissimo, Glyph::ArticStaccatissimoAbove,
         Glyph::ArticStaccatissimoBelow},
        {notation::Articulation::Spiccato, Glyph::ArticStaccatissimoAbove,
         Glyph::ArticStaccatissimoBelow},
    };

    for (const Entry& entry : kEntries) {
        if (!notation::has_articulation(note.articulations, entry.articulation))
            continue;
        const Glyph glyph = below ? entry.below : entry.above;
        emit_glyph(glyph, Point{anchor.x - advance_of(glyph) * 0.5, anchor.y + direction * distance},
                   ItemRole::Articulation, note.id, result);
        distance += 0.9;
    }

    // Portato is a tenuto and a staccato together.
    if (notation::has_articulation(note.articulations, notation::Articulation::Portato)) {
        for (const Glyph glyph : {below ? Glyph::ArticTenutoBelow : Glyph::ArticTenutoAbove,
                                  below ? Glyph::ArticStaccatoBelow : Glyph::ArticStaccatoAbove}) {
            emit_glyph(glyph, Point{anchor.x - advance_of(glyph) * 0.5,
                                    anchor.y + direction * distance},
                       ItemRole::Articulation, note.id, result);
            distance += 0.9;
        }
    }

    // The marcato goes over the note by convention, whichever way the stem
    // points - the first picture of real notation had one hanging under a
    // bass note where nobody would look for it - except in the lower voice
    // of a shared measure, where "over the note" is inside the other voice
    // and the font's below form is what a reader expects.
    if (notation::has_articulation(note.articulations, notation::Articulation::Marcato)
        && on_stem_side && below) {
        emit_glyph(Glyph::ArticMarcatoBelow,
                   Point{anchor.x - advance_of(Glyph::ArticMarcatoBelow) * 0.5,
                         anchor.y + distance},
                   ItemRole::Articulation, note.id, result);
        distance += 1.2;
    }

    // These always sit above the note, whichever way the stem points.
    const double above_clearance =
        std::max(below ? 1.1 : distance,
                 notehead_centre.y - staff.metrics.y_for_staff_position(8) + 1.1);
    double above_y = (below ? notehead_centre.y : anchor.y) - above_clearance;
    const auto emit_above = [&](Glyph glyph) {
        emit_glyph(glyph, Point{notehead_centre.x - advance_of(glyph) * 0.5, above_y},
                   ItemRole::Articulation, note.id, result);
        above_y -= 1.2;
    };

    if (notation::has_articulation(note.articulations, notation::Articulation::Marcato)
        && !(on_stem_side && below))
        emit_above(Glyph::ArticMarcatoAbove);
    if (notation::has_articulation(note.articulations, notation::Articulation::Fermata))
        emit_above(Glyph::Fermata);
    if (notation::has_articulation(note.articulations, notation::Articulation::Trill))
        emit_above(Glyph::OrnamentTrill);
    if (notation::has_articulation(note.articulations, notation::Articulation::UpBow))
        emit_above(Glyph::StringsUpBow);
    if (notation::has_articulation(note.articulations, notation::Articulation::DownBow))
        emit_above(Glyph::StringsDownBow);
    if (notation::has_articulation(note.articulations, notation::Articulation::Tremolo))
        emit_above(Glyph::Tremolo1);
}

// --- Layout -----------------------------------------------------------------

LayoutResult LayoutEngine::layout(const notation::Score& score, const TimelineMap& timeline,
                                  notation::Ticks from_tick, notation::Ticks to_tick) const
{
    LayoutResult result;
    if (to_tick <= from_tick || score.staff_count() == 0)
        return result;

    const double ruler_height = options_.draw_ruler ? options_.ruler_height : 0.0;
    const double content_top = ruler_height + options_.margin_above;

    std::vector<StaffContext> staves;
    staves.reserve(score.staff_count());
    for (std::size_t index = 0; index < score.staff_count(); ++index) {
        StaffContext context;
        context.index = index;
        context.clef = score.staff(index).clef;
        context.metrics = staff_prototype_;
        context.metrics.top_y = content_top
                              + static_cast<double>(index)
                                    * (staff_prototype_.height() + options_.staff_gap);
        result.staff_top_y.push_back(context.metrics.top_y);
        staves.push_back(context);
    }

    // The header is the only horizontal measurement layout owns; everything
    // to the right of it comes from the timeline.
    double header_width = 0.0;
    if (options_.draw_header) {
        header_width = std::max(options_.header_width,
                                layout_header(score, nullptr, from_tick, 0.0, nullptr));
    }
    const double music_x = timeline.x_for_tick(from_tick);
    const double left_x = music_x - header_width;
    const double right_x = timeline.x_for_tick(to_tick);

    if (options_.draw_ruler)
        layout_ruler(score, timeline, from_tick, to_tick, result);

    for (const StaffContext& staff : staves) {
        layout_staff_lines(staff, left_x, right_x, result);
        if (options_.draw_header)
            layout_header(score, &staff, from_tick, left_x, &result);
    }

    // The barlines are drawn after the staves' chords, which say where the
    // grace notes at a measure start reach to.
    GraceEdges grace_edges;

    // Accidental state has to start from the beginning of the measure the
    // view starts in, so the query reaches back that far even though nothing
    // before `from_tick` is drawn.
    const notation::Ticks query_from =
        std::min(from_tick, score.measure_at(from_tick).start_tick);

    for (const StaffContext& staff : staves) {
        const std::vector<notation::ScoreNote> visible =
            score.notes_in_range(staff.index, query_from, to_tick);

        // Which measures hold more than one voice is decided over whole
        // measures, including what lies past the right edge of the view:
        // otherwise a stem would flip as the other voice's first note
        // scrolled into sight.
        const notation::Ticks census_to =
            std::max(to_tick, score.measure_at(std::max(to_tick - 1, from_tick)).end_tick());
        const std::vector<std::int32_t> shared_measures = measures_with_several_voices(
            score, score.notes_in_range(staff.index, query_from, census_to));
        const auto is_shared = [&](notation::Ticks tick) {
            return std::binary_search(shared_measures.begin(), shared_measures.end(),
                                      score.measure_at(tick).number);
        };

        for (const notation::ScoreNote& note : visible) {
            if (note.is_rest && note.start_tick >= from_tick)
                layout_rest(staff, timeline, note, is_shared(note.start_tick), visible, result);
        }

        std::vector<Chord> chords =
            build_chords(score, staff, staves, timeline, visible, from_tick, shared_measures,
                         grace_edges);

        std::vector<BeamGroup> groups;
        if (options_.draw_beams)
            groups = build_beam_groups(score, staff, chords);

        for (Chord& chord : chords)
            layout_chord(chord.drawn_on != nullptr ? *chord.drawn_on : staff, timeline, chord,
                         result);

        if (options_.draw_beams)
            layout_beams(staff, chords, groups, result);
        if (options_.draw_tuplets) {
            // After the chords, whose stem ends the brackets clear, and
            // after the beams, since a group one beam holds needs no bracket.
            std::vector<TupletEvent> events;
            const std::vector<TupletGroup> tuplets =
                build_tuplet_groups(score, timeline, visible, from_tick, chords, events);
            layout_tuplets(staff, chords, groups, events, tuplets, result);
        }
        if (options_.draw_ties)
            layout_ties(score, staff, timeline, chords, result);
        if (options_.draw_slurs)
            layout_slurs(score, staff, timeline, from_tick, to_tick, shared_measures, result);
    }

    if (options_.draw_barlines) {
        layout_barlines(score, timeline, staves, from_tick, to_tick, grace_edges, result);

        // A grand staff is joined at the left by a system barline.
        if (staves.size() > 1) {
            LineItem item;
            item.from = Point{left_x, staves.front().metrics.top_y};
            item.to = Point{left_x, staves.back().metrics.bottom_y()};
            item.thickness = staff_prototype_.line_thickness * 1.6;
            item.role = ItemRole::Barline;
            result.display_list.add(item);
        }
    }

    // Dynamics and hairpins are drawn against the staff they are written
    // on. On one staff they go where the placement option says; on a grand
    // staff they go between the staves, as a piano part has them - below
    // every staff but the last and above that one - and a voice can still
    // take its marking to its outer side (`marking_goes_below`). Which
    // measures two voices share is read off each marking's own staff.
    const notation::Ticks marking_census_to =
        std::max(to_tick, score.measure_at(std::max(to_tick - 1, from_tick)).end_tick());
    std::vector<MarkingStaff> marking_staves;
    marking_staves.reserve(staves.size());
    for (const StaffContext& staff : staves) {
        MarkingStaff marking_staff;
        marking_staff.baselines.above = marking_baseline(staff, false, result);
        marking_staff.baselines.below = marking_baseline(staff, true, result);
        marking_staff.shared_measures = measures_with_several_voices(
            score, score.notes_in_range(staff.index, query_from, marking_census_to));
        marking_staff.default_below =
            staves.size() > 1 ? staff.index + 1 < staves.size()
                              : options_.dynamics_placement == Placement::Below;
        marking_staves.push_back(std::move(marking_staff));
    }
    layout_dynamics(score, timeline, from_tick, to_tick, marking_staves, result);
    layout_hairpins(score, timeline, from_tick, to_tick, marking_staves, result);

    // After the markings, which a text clears when they share the space
    // above the top staff: a dynamic goes nearest the staff, and a
    // rehearsal letter or a cue point above it.
    if (options_.draw_texts)
        layout_texts(score, timeline, staves.front(), from_tick, to_tick, result);

    // Only when it is in view. A cursor drawn at its tick regardless would
    // sit far outside the page after scrolling, and anything that reads the
    // drawn bounds would then be describing a page the size of the score.
    if (options_.draw_cursor && options_.cursor_tick >= from_tick
        && options_.cursor_tick < to_tick) {
        const double x = timeline.x_for_tick(options_.cursor_tick);
        double top = staves.front().metrics.top_y - 1.0;
        double bottom = staves.back().metrics.bottom_y() + 1.0;
        if (options_.cursor_staff < staves.size()) {
            top = staves[options_.cursor_staff].metrics.top_y - 1.0;
            bottom = staves[options_.cursor_staff].metrics.bottom_y() + 1.0;
        }

        LineItem item;
        item.from = Point{x, top};
        item.to = Point{x, bottom};
        item.thickness = staff_prototype_.line_thickness * 2.0;
        item.role = ItemRole::Cursor;
        result.display_list.add(item);
    }

    result.display_list.is_crowded = result.crowded_note_count > 0;

    // The page is what the view range and the options imply, and nothing
    // else - a renderer positions itself from it. What was drawn can reach
    // outside it, and that is reported separately so a renderer sizing an
    // image clips nothing.
    result.extent = Rect{left_x, 0.0, right_x - left_x,
                         staves.back().metrics.bottom_y() + options_.margin_below};
    result.drawn_bounds = result.extent.united_with(result.display_list.bounds);
    return result;
}

}  // namespace vcc::engraving
