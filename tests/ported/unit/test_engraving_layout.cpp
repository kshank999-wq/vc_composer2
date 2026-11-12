// Score to display list (spec 02 sections 5.1, 5.2, 5.3).
//
// The layout engine emits geometry rather than drawing, which is what makes
// these assertions possible at all: every one of them checks a real
// engraving rule against real numbers, on a machine with no window and no
// music font installed.
#include "vcc/engraving/LayoutEngine.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "vcc_test.h"

using namespace vcc;
using namespace vcc::engraving;

namespace {

notation::ScoreNote note_at(notation::Ticks tick, const notation::Duration& duration,
                            const notation::Pitch& pitch)
{
    notation::ScoreNote note;
    note.start_tick = tick;
    note.duration = duration;
    note.pitch = pitch;
    return note;
}

notation::ScoreNote rest_at(notation::Ticks tick, const notation::Duration& duration)
{
    notation::ScoreNote note;
    note.start_tick = tick;
    note.duration = duration;
    note.is_rest = true;
    return note;
}

std::size_t count_glyphs(const DisplayList& list, ItemRole role)
{
    std::size_t count = 0;
    for (const GlyphItem& item : list.glyphs) {
        if (item.role == role)
            ++count;
    }
    return count;
}

std::size_t count_lines(const DisplayList& list, ItemRole role)
{
    std::size_t count = 0;
    for (const LineItem& item : list.lines) {
        if (item.role == role)
            ++count;
    }
    return count;
}

std::size_t count_curves(const DisplayList& list, ItemRole role)
{
    std::size_t count = 0;
    for (const CurveItem& item : list.curves) {
        if (item.role == role)
            ++count;
    }
    return count;
}

std::vector<const TextItem*> texts_with(const DisplayList& list, ItemRole role)
{
    std::vector<const TextItem*> found;
    for (const TextItem& item : list.texts) {
        if (item.role == role)
            found.push_back(&item);
    }
    return found;
}

std::vector<const LineItem*> lines_with(const DisplayList& list, ItemRole role)
{
    std::vector<const LineItem*> found;
    for (const LineItem& item : list.lines) {
        if (item.role == role)
            found.push_back(&item);
    }
    return found;
}

const GlyphItem* find_glyph(const DisplayList& list, ItemRole role)
{
    for (const GlyphItem& item : list.glyphs) {
        if (item.role == role)
            return &item;
    }
    return nullptr;
}

const LaidOutNote* find_note(const LayoutResult& result, notation::EventId id)
{
    for (const LaidOutNote& note : result.notes) {
        if (note.id == id)
            return &note;
    }
    return nullptr;
}

StaffMetrics metrics_for(const LayoutResult& result, std::size_t staff_index)
{
    StaffMetrics metrics;
    metrics.top_y = result.staff_top_y.at(staff_index);
    return metrics;
}

constexpr notation::Ticks kQuarter = notation::kTicksPerQuarter;
constexpr notation::Ticks kBar = 4 * kQuarter;

}  // namespace

// --- Key signatures ---------------------------------------------------------

VCC_TEST(key_signature_accidentals_follow_the_conventional_pattern)
{
    int positions[7]{};

    // The pattern every engraver knows: in treble, sharps run F on the top
    // line, C in the third space, G above the staff, D on the fourth line,
    // A in the second space, E in the fourth space, B on the middle line.
    const std::size_t sharps =
        key_signature_positions(notation::KeySignature{7, false}, notation::Clef::Treble,
                                positions, 7);
    VCC_CHECK_EQ(sharps, std::size_t{7});
    const int expected_sharps[7] = {8, 5, 9, 6, 3, 7, 4};
    for (std::size_t index = 0; index < 7; ++index)
        VCC_CHECK_EQ(positions[index], expected_sharps[index]);

    // And flats run B, E, A, D, G, C, F - the seventh flat low in the
    // staff, not an octave above it.
    const std::size_t flats =
        key_signature_positions(notation::KeySignature{-7, false}, notation::Clef::Treble,
                                positions, 7);
    VCC_CHECK_EQ(flats, std::size_t{7});
    const int expected_flats[7] = {4, 7, 3, 6, 2, 5, 1};
    for (std::size_t index = 0; index < 7; ++index)
        VCC_CHECK_EQ(positions[index], expected_flats[index]);
}

VCC_TEST(key_signatures_sit_two_positions_lower_in_bass_clef)
{
    int treble[7]{};
    int bass[7]{};
    key_signature_positions(notation::KeySignature{7, false}, notation::Clef::Treble, treble, 7);
    key_signature_positions(notation::KeySignature{7, false}, notation::Clef::Bass, bass, 7);

    for (std::size_t index = 0; index < 7; ++index)
        VCC_CHECK_EQ(bass[index], treble[index] - 2);
}

VCC_TEST(c_major_and_a_percussion_staff_have_no_key_signature)
{
    int positions[7]{};
    VCC_CHECK_EQ(key_signature_positions(notation::KeySignature{0, false},
                                         notation::Clef::Treble, positions, 7),
                 std::size_t{0});
    VCC_CHECK_EQ(key_signature_positions(notation::KeySignature{3, false},
                                         notation::Clef::Percussion, positions, 7),
                 std::size_t{0});
}

// --- Placement --------------------------------------------------------------

VCC_TEST(notes_land_on_the_lines_their_pitch_names)
{
    notation::Score score;
    const notation::Step scale[8] = {notation::Step::C, notation::Step::D, notation::Step::E,
                                     notation::Step::F, notation::Step::G, notation::Step::A,
                                     notation::Step::B, notation::Step::C};
    for (int index = 0; index < 8; ++index) {
        notation::Pitch pitch;
        pitch.step = scale[index];
        pitch.octave = index == 7 ? 5 : 4;
        VCC_CHECK(score.insert_note(
            0, note_at(static_cast<notation::Ticks>(index) * kQuarter, notation::quarter(), pitch)));
    }

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, 8 * kQuarter);

    VCC_CHECK_EQ(result.notes.size(), std::size_t{8});

    // Middle C is one ledger line below the treble staff; the C above it
    // sits in the third space.
    for (std::size_t index = 0; index < result.notes.size(); ++index)
        VCC_CHECK_EQ(result.notes[index].staff_position, static_cast<int>(index) - 2);

    // And x follows the timeline, not engraving spacing.
    const StaffMetrics metrics = metrics_for(result, 0);
    for (const LaidOutNote& note : result.notes) {
        VCC_CHECK_NEAR(note.centre.y, metrics.y_for_staff_position(note.staff_position), 1e-9);
        VCC_CHECK_NEAR(note.centre.x - 1.18 * 0.5,
                       TimelineMap(6.0).x_for_tick(note.start_tick), 1e-9);
    }
}

VCC_TEST(notes_outside_the_staff_get_ledger_lines)
{
    notation::Score score;
    // Middle C: one ledger line. G4, inside the staff: none.
    const notation::EventId middle_c =
        score.insert_note(0, note_at(0, notation::quarter(), notation::Pitch{notation::Step::C, 4, 0}));
    score.insert_note(0, note_at(kQuarter, notation::quarter(), notation::Pitch{notation::Step::G, 4, 0}));
    // A5, two ledger lines above.
    score.insert_note(0, note_at((2 * kQuarter), notation::quarter(), notation::Pitch{notation::Step::A, 5, 0}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    std::size_t for_middle_c = 0;
    for (const LineItem& line : result.display_list.lines) {
        if (line.role == ItemRole::LedgerLine && line.source_id == middle_c)
            ++for_middle_c;
    }
    VCC_CHECK_EQ(for_middle_c, std::size_t{1});

    // A5 is position 10: one line above the staff.
    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::LedgerLine), std::size_t{2});
}

VCC_TEST(a_note_on_the_middle_line_takes_a_down_stem_when_laid_out)
{
    // Through the chord rule, which every laid-out note goes through, not
    // only the single-note helper: the two disagreed on the middle line.
    notation::Score score;
    score.insert_note(0, note_at(0, notation::half(), notation::Pitch{notation::Step::B, 4, 0}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);
    VCC_CHECK_EQ(result.notes.front().staff_position, 4);
    VCC_CHECK(!result.notes.front().stem_up);
}

VCC_TEST(stem_direction_follows_the_middle_line)
{
    VCC_CHECK(stem_up_for_position(0));
    VCC_CHECK(stem_up_for_position(3));
    VCC_CHECK(!stem_up_for_position(4));  // The middle line takes a down stem.
    VCC_CHECK(!stem_up_for_position(8));

    notation::Score score;
    const notation::EventId low =
        score.insert_note(0, note_at(0, notation::quarter(), notation::Pitch{notation::Step::G, 4, 0}));
    const notation::EventId high =
        score.insert_note(0, note_at(kQuarter, notation::quarter(), notation::Pitch{notation::Step::C, 5, 0}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, kBar);

    VCC_CHECK(find_note(result, low)->stem_up);
    VCC_CHECK(!find_note(result, high)->stem_up);
    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::Stem), std::size_t{2});
}

VCC_TEST(a_stem_reaches_the_middle_line_from_outside_the_staff)
{
    notation::Score score;
    // Two ledger lines below the staff: a conventional 3.5-space stem would
    // stop short of the staff, which is not how it is engraved.
    score.insert_note(0, note_at(0, notation::quarter(), notation::Pitch{notation::Step::A, 3, 0}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, kBar);

    const StaffMetrics metrics = metrics_for(result, 0);
    bool checked = false;
    for (const LineItem& line : result.display_list.lines) {
        if (line.role != ItemRole::Stem)
            continue;
        // The stem's free end is at or above the middle line.
        VCC_CHECK(std::min(line.from.y, line.to.y) <= metrics.line_y(2) + 1e-9);
        checked = true;
    }
    VCC_CHECK(checked);
}

VCC_TEST(whole_notes_have_no_stem_and_no_flag)
{
    notation::Score score;
    score.insert_note(0, note_at(0, notation::whole(), notation::Pitch{notation::Step::G, 4, 0}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, kBar);

    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::Stem), std::size_t{0});
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Flag), std::size_t{0});
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Notehead), std::size_t{1});
}

// --- Chords -----------------------------------------------------------------

VCC_TEST(a_chord_stacks_in_one_column_and_takes_one_stem)
{
    notation::Score score;
    const std::vector<notation::EventId> ids = score.insert_chord(
        0, 0, notation::quarter(),
        {notation::Pitch{notation::Step::C, 4, 0}, notation::Pitch{notation::Step::E, 4, 0},
         notation::Pitch{notation::Step::G, 4, 0}});
    VCC_CHECK_EQ(ids.size(), std::size_t{3});

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, kBar);

    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Notehead), std::size_t{3});
    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::Stem), std::size_t{1});

    // No seconds in a C major triad, so nothing is offset and every head
    // shares one x.
    const double x = find_note(result, ids[0])->centre.x;
    for (const notation::EventId id : ids) {
        VCC_CHECK(!find_note(result, id)->is_offset);
        VCC_CHECK_NEAR(find_note(result, id)->centre.x, x, 1e-9);
    }

    // The chord spans positions -2 to 2, so it is furthest below the middle
    // line and takes an up stem.
    VCC_CHECK(find_note(result, ids[0])->stem_up);
}

VCC_TEST(a_second_inside_a_chord_offsets_the_upper_note)
{
    notation::Score score;
    const std::vector<notation::EventId> ids = score.insert_chord(
        0, 0, notation::quarter(),
        {notation::Pitch{notation::Step::C, 4, 0}, notation::Pitch{notation::Step::D, 4, 0}});

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, kBar);

    const LaidOutNote* lower = find_note(result, ids[0]);
    const LaidOutNote* upper = find_note(result, ids[1]);
    VCC_CHECK(!lower->is_offset);
    VCC_CHECK(upper->is_offset);

    // Offset by a notehead width, so the two heads touch rather than
    // overlap.
    VCC_CHECK(upper->centre.x > lower->centre.x);
    VCC_CHECK_NEAR(upper->centre.x - lower->centre.x, 1.18, 1e-9);
}

VCC_TEST(stem_direction_for_a_chord_follows_its_furthest_note)
{
    VCC_CHECK(stem_up_for_chord({-2, 0, 2}));   // Centre of gravity below.
    VCC_CHECK(!stem_up_for_chord({5, 7, 9}));   // Above.
    // Equally far: down, which is the convention (Gould, Behind Bars), and
    // the answer the single-note rule gives a note on the middle line. Up
    // was the tie-break for a while, and the first picture of real notation
    // showed a middle-line half note with its stem the wrong way.
    VCC_CHECK(!stem_up_for_chord({0, 8}));
    VCC_CHECK(!stem_up_for_chord({4}));
    VCC_CHECK(stem_up_for_chord({}));
}

// --- Accidentals ------------------------------------------------------------

VCC_TEST(an_accidental_is_written_once_per_measure_and_not_repeated)
{
    notation::Score score;
    const notation::Pitch f_sharp{notation::Step::F, 4, 1};
    score.insert_note(0, note_at(0, notation::quarter(), f_sharp));
    score.insert_note(0, note_at(kQuarter, notation::quarter(), f_sharp));
    // Same pitch in the next bar: the accidental is written again.
    score.insert_note(0, note_at(kBar, notation::quarter(), f_sharp));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, 2 * kBar);

    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Accidental), std::size_t{2});
}

VCC_TEST(the_key_signature_decides_which_accidentals_are_written)
{
    notation::Score score;
    VCC_CHECK(score.set_key_signature(0, notation::KeySignature{1, false}));  // G major.

    // F sharp is in the key: no accidental. F natural is not: one natural.
    score.insert_note(0, note_at(0, notation::quarter(), notation::Pitch{notation::Step::F, 4, 1}));
    score.insert_note(0, note_at(kQuarter, notation::quarter(), notation::Pitch{notation::Step::G, 4, 0}));

    LayoutEngine engine;
    LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, kBar);
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Accidental), std::size_t{0});

    notation::Score with_natural;
    VCC_CHECK(with_natural.set_key_signature(0, notation::KeySignature{1, false}));
    with_natural.insert_note(
        0, note_at(0, notation::quarter(), notation::Pitch{notation::Step::F, 4, 0}));

    result = engine.layout(with_natural, TimelineMap(6.0), 0, kBar);
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Accidental), std::size_t{1});
    VCC_CHECK_EQ(find_glyph(result.display_list, ItemRole::Accidental)->glyph,
                 Glyph::AccidentalNatural);
}

VCC_TEST(accidental_state_carries_in_from_earlier_in_the_measure)
{
    // A view that starts mid-bar still has to know what accidentals are
    // already in force, or it writes one the reader can already see.
    notation::Score score;
    const notation::Pitch f_sharp{notation::Step::F, 4, 1};
    score.insert_note(0, note_at(0, notation::quarter(), f_sharp));
    score.insert_note(0, note_at((2 * kQuarter), notation::quarter(), f_sharp));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), (2 * kQuarter), kBar);

    VCC_CHECK_EQ(result.notes.size(), std::size_t{1});
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Accidental), std::size_t{0});
}

VCC_TEST(accidentals_in_a_chord_are_stacked_into_columns)
{
    notation::Score score;
    // Three adjacent sharps cannot share one column.
    score.insert_chord(0, 0, notation::quarter(),
                       {notation::Pitch{notation::Step::C, 4, 1},
                        notation::Pitch{notation::Step::D, 4, 1},
                        notation::Pitch{notation::Step::E, 4, 1}});

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, kBar);

    std::vector<double> xs;
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role == ItemRole::Accidental)
            xs.push_back(item.origin.x);
    }
    VCC_CHECK_EQ(xs.size(), std::size_t{3});

    std::sort(xs.begin(), xs.end());
    // Two distinct columns at least, and every accidental left of the chord.
    VCC_CHECK(xs.front() < xs.back() - 0.5);
    for (const double x : xs)
        VCC_CHECK(x < find_note(result, result.notes.front().id)->centre.x);
}

// --- Beams, flags and dots --------------------------------------------------

VCC_TEST(beam_counts_follow_the_written_value)
{
    VCC_CHECK_EQ(beam_count_for(notation::quarter()), 0);
    VCC_CHECK_EQ(beam_count_for(notation::eighth()), 1);
    VCC_CHECK_EQ(beam_count_for(notation::sixteenth()), 2);
    VCC_CHECK_EQ(beam_count_for(notation::Duration{notation::NoteValue::ThirtySecond}), 3);
    VCC_CHECK_EQ(beam_count_for(notation::Duration{notation::NoteValue::SixtyFourth}), 4);
    VCC_CHECK_EQ(beam_count_for(notation::whole()), 0);
}

VCC_TEST(eighths_beam_by_beat_and_take_no_flags)
{
    notation::Score score;
    for (int index = 0; index < 4; ++index) {
        score.insert_note(0, note_at(static_cast<notation::Ticks>(index) * (kQuarter / 2),
                                     notation::eighth(),
                                     notation::Pitch{notation::Step::G, 4, 0}));
    }

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    // Two beats of two eighths: two beams, not one across the pair of
    // beats, and no flags.
    VCC_CHECK_EQ(result.display_list.quads.size(), std::size_t{2});
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Flag), std::size_t{0});
    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::Stem), std::size_t{4});
}

VCC_TEST(a_lone_eighth_keeps_its_flag)
{
    notation::Score score;
    score.insert_note(0, note_at(0, notation::eighth(), notation::Pitch{notation::Step::G, 4, 0}));
    score.insert_note(0, rest_at((kQuarter / 2), notation::eighth()));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    VCC_CHECK(result.display_list.quads.empty());
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Flag), std::size_t{1});
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Rest), std::size_t{1});
}

VCC_TEST(a_rest_inside_a_beat_breaks_the_beam)
{
    notation::Score score;
    const notation::Pitch pitch{notation::Step::G, 4, 0};
    score.insert_note(0, note_at(0, notation::sixteenth(), pitch));
    score.insert_note(0, note_at((kQuarter / 4), notation::sixteenth(), pitch));
    score.insert_note(0, rest_at((kQuarter / 2), notation::sixteenth()));
    score.insert_note(0, note_at((3 * kQuarter / 4), notation::sixteenth(), pitch));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(12.0), 0, kBar);

    // The first two beam together (primary and secondary beam: two quads);
    // the one after the rest is on its own and keeps a flag.
    VCC_CHECK_EQ(result.display_list.quads.size(), std::size_t{2});
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Flag), std::size_t{1});
}

VCC_TEST(a_beam_slopes_but_not_steeply)
{
    notation::Score score;
    // A wide leap: an unlimited beam slope would be unreadable.
    score.insert_note(0, note_at(0, notation::eighth(), notation::Pitch{notation::Step::C, 4, 0}));
    score.insert_note(0, note_at((kQuarter / 2), notation::eighth(), notation::Pitch{notation::Step::C, 6, 0}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    VCC_CHECK_EQ(result.display_list.quads.size(), std::size_t{1});
    const QuadItem& beam = result.display_list.quads.front();
    const double rise = std::abs(beam.corners[1].y - beam.corners[0].y);
    VCC_CHECK(rise <= 2.0 + 1e-9);
    // And it does slope, rather than being forced flat.
    VCC_CHECK(rise > 0.0);
}

// --- Tuplets ----------------------------------------------------------------

VCC_TEST(tuplet_labels_say_only_what_a_reader_would_not_assume)
{
    // The number alone where the ratio is the conventional one for it.
    VCC_CHECK(tuplet_label(notation::Tuplet{3, 2}) == "3");
    VCC_CHECK(tuplet_label(notation::Tuplet{5, 4}) == "5");
    VCC_CHECK(tuplet_label(notation::Tuplet{6, 4}) == "6");
    VCC_CHECK(tuplet_label(notation::Tuplet{7, 4}) == "7");
    VCC_CHECK(tuplet_label(notation::Tuplet{9, 8}) == "9");
    VCC_CHECK(tuplet_label(notation::Tuplet{2, 3}) == "2");
    VCC_CHECK(tuplet_label(notation::Tuplet{4, 6}) == "4");

    // And the ratio where it is not.
    VCC_CHECK(tuplet_label(notation::Tuplet{5, 3}) == "5:3");
    VCC_CHECK(tuplet_label(notation::Tuplet{7, 8}) == "7:8");
    VCC_CHECK(tuplet_label(notation::Tuplet{3, 4}) == "3:4");
    VCC_CHECK(tuplet_label(notation::Tuplet{4, 3}) == "4:3");
}

VCC_TEST(a_tuplet_span_is_whole_when_it_fills_its_normal_values)
{
    const notation::Tuplet triplet{3, 2};
    // Three triplet eighths: 960 sounding, an eighth written. Two are not.
    VCC_CHECK(tuplet_span_is_complete(triplet, kQuarter, (kQuarter / 2)));
    VCC_CHECK(!tuplet_span_is_complete(triplet, (2 * kQuarter / 3), (kQuarter / 2)));
    VCC_CHECK(!tuplet_span_is_complete(triplet, (kQuarter / 3), (kQuarter / 2)));
    // Three triplet quarters fill a half.
    VCC_CHECK(tuplet_span_is_complete(triplet, (2 * kQuarter), kQuarter));
    // A quarter and an eighth under one 3: a quarter's worth, whole.
    VCC_CHECK(tuplet_span_is_complete(triplet, kQuarter, (kQuarter / 2)));

    // Three sextuplet sixteenths sound as two sixteenths too, which is not
    // a whole 6:4 - the shortest written value says so.
    const notation::Tuplet sextuplet{6, 4};
    VCC_CHECK(!tuplet_span_is_complete(sextuplet, (kQuarter / 2), (kQuarter / 4)));
    VCC_CHECK(tuplet_span_is_complete(sextuplet, kQuarter, (kQuarter / 4)));

    // A quintuplet of sixteenths closes at five and nowhere earlier.
    const notation::Tuplet quintuplet{5, 4};
    for (int count = 1; count < 5; ++count)
        VCC_CHECK(!tuplet_span_is_complete(quintuplet, 192 * count, (kQuarter / 4)));
    VCC_CHECK(tuplet_span_is_complete(quintuplet, kQuarter, (kQuarter / 4)));

    // A duplet in compound time: two in the time of three eighths.
    const notation::Tuplet duplet{2, 3};
    VCC_CHECK(!tuplet_span_is_complete(duplet, (3 * kQuarter / 4), (kQuarter / 2)));
    VCC_CHECK(tuplet_span_is_complete(duplet, (3 * kQuarter / 2), (kQuarter / 2)));

    // Nonsense is never whole.
    VCC_CHECK(!tuplet_span_is_complete(triplet, 0, (kQuarter / 2)));
    VCC_CHECK(!tuplet_span_is_complete(notation::Tuplet{3, 0}, kQuarter, (kQuarter / 2)));
}

VCC_TEST(a_beamed_triplet_takes_a_number_and_no_bracket)
{
    // The beam is the bracket: three triplet eighths under one beam get the
    // 3 alone, beyond the beam, and no lines.
    notation::Score score;
    const notation::Pitch pitch{notation::Step::G, 4, 0};
    const notation::Duration eighth = notation::triplet(notation::NoteValue::Eighth);
    for (int index = 0; index < 3; ++index)
        score.insert_note(0, note_at(static_cast<notation::Ticks>(index) * (kQuarter / 3), eighth, pitch));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(12.0), 0, kBar);

    VCC_CHECK_EQ(result.display_list.quads.size(), std::size_t{1});
    const std::vector<const TextItem*> numbers =
        texts_with(result.display_list, ItemRole::TupletBracket);
    VCC_CHECK_EQ(numbers.size(), std::size_t{1});
    VCC_CHECK(numbers[0]->text == "3");
    VCC_CHECK(numbers[0]->italic);
    VCC_CHECK(lines_with(result.display_list, ItemRole::TupletBracket).empty());

    // G4 stems up, so the beam is above the notes and the number is above
    // the beam, centred on the group.
    const QuadItem& beam = result.display_list.quads.front();
    double beam_top = beam.corners[0].y;
    for (const Point& corner : beam.corners)
        beam_top = std::min(beam_top, corner.y);
    VCC_CHECK(numbers[0]->origin.y < beam_top);
    const double first_x = result.notes.front().notehead_bounds.x;
    const double last_right = result.notes.back().notehead_bounds.x
                            + result.notes.back().notehead_bounds.width;
    VCC_CHECK(numbers[0]->origin.x > first_x);
    VCC_CHECK(numbers[0]->origin.x < last_right);

    // High notes beam below, inside the staff; the number still clears the
    // staff rather than sitting on its lines.
    notation::Score high;
    for (int index = 0; index < 3; ++index) {
        high.insert_note(0, note_at(static_cast<notation::Ticks>(index) * (kQuarter / 3), eighth,
                                    notation::Pitch{notation::Step::A, 5, 0}));
    }
    const LayoutResult below = engine.layout(high, TimelineMap(12.0), 0, kBar);
    const StaffMetrics metrics = metrics_for(below, 0);
    const std::vector<const TextItem*> under =
        texts_with(below.display_list, ItemRole::TupletBracket);
    VCC_CHECK_EQ(under.size(), std::size_t{1});
    VCC_CHECK(lines_with(below.display_list, ItemRole::TupletBracket).empty());
    // The baseline is the text's bottom; its top is about three quarters of
    // its size above that, and has to be below the staff.
    VCC_CHECK(under[0]->origin.y - under[0]->size * 0.75 >= metrics.bottom_y() + 1.0 - 1e-9);
}

VCC_TEST(an_unbeamed_tuplet_is_bracketed_on_the_stem_side)
{
    // Three triplet quarters: nothing beams them, so a bracket holds them -
    // two hooks and a line broken for the number - on the stem side, clear
    // of the staff.
    const notation::Duration quarter = notation::triplet(notation::NoteValue::Quarter);

    notation::Score low;
    for (int index = 0; index < 3; ++index) {
        low.insert_note(0, note_at(static_cast<notation::Ticks>(index) * (2 * kQuarter / 3), quarter,
                                   notation::Pitch{notation::Step::E, 4, 0}));
    }

    LayoutEngine engine;
    const LayoutResult above = engine.layout(low, TimelineMap(8.0), 0, kBar);
    const StaffMetrics metrics = metrics_for(above, 0);

    const std::vector<const LineItem*> bracket =
        lines_with(above.display_list, ItemRole::TupletBracket);
    VCC_CHECK_EQ(bracket.size(), std::size_t{4});
    const std::vector<const TextItem*> numbers =
        texts_with(above.display_list, ItemRole::TupletBracket);
    VCC_CHECK_EQ(numbers.size(), std::size_t{1});
    VCC_CHECK(numbers[0]->text == "3");

    // E4 stems up: the bracket is above the staff, and above every stem end.
    double stem_top = metrics.bottom_y();
    for (const LineItem& line : above.display_list.lines) {
        if (line.role == ItemRole::Stem)
            stem_top = std::min(stem_top, std::min(line.from.y, line.to.y));
    }
    double bracket_y = metrics.bottom_y();
    double left = 1e9;
    double right = -1e9;
    for (const LineItem* line : bracket) {
        bracket_y = std::min(bracket_y, std::min(line->from.y, line->to.y));
        left = std::min(left, std::min(line->from.x, line->to.x));
        right = std::max(right, std::max(line->from.x, line->to.x));
    }
    VCC_CHECK(bracket_y <= metrics.top_y - 1.0);
    VCC_CHECK(bracket_y < stem_top);

    // From the first notehead to the right edge of the last, with the hooks
    // starting on the bracket's line and turned down toward the notes, and
    // the number in the middle.
    VCC_CHECK_NEAR(left, above.notes.front().notehead_bounds.x, 1e-9);
    VCC_CHECK_NEAR(right,
                   above.notes.back().notehead_bounds.x + above.notes.back().notehead_bounds.width,
                   1e-9);
    double line_y = 0.0;
    std::size_t horizontals = 0;
    for (const LineItem* line : bracket) {
        if (std::abs(line->from.y - line->to.y) < 1e-9) {
            ++horizontals;
            line_y = line->from.y;
        }
    }
    VCC_CHECK_EQ(horizontals, std::size_t{2});
    std::size_t hooks = 0;
    for (const LineItem* line : bracket) {
        if (std::abs(line->from.x - line->to.x) < 1e-9) {
            ++hooks;
            VCC_CHECK_NEAR(std::min(line->from.y, line->to.y), line_y, 1e-9);
            VCC_CHECK(std::max(line->from.y, line->to.y) > line_y + 0.5);
        }
    }
    VCC_CHECK_EQ(hooks, std::size_t{2});
    VCC_CHECK(numbers[0]->origin.x > left && numbers[0]->origin.x < right);

    // High notes stem down: the same group goes below.
    notation::Score high;
    for (int index = 0; index < 3; ++index) {
        high.insert_note(0, note_at(static_cast<notation::Ticks>(index) * (2 * kQuarter / 3), quarter,
                                    notation::Pitch{notation::Step::C, 6, 0}));
    }
    const LayoutResult below = engine.layout(high, TimelineMap(8.0), 0, kBar);
    double below_y = -1e9;
    for (const LineItem* line : lines_with(below.display_list, ItemRole::TupletBracket))
        below_y = std::max(below_y, std::max(line->from.y, line->to.y));
    double stem_bottom = metrics.top_y;
    for (const LineItem& line : below.display_list.lines) {
        if (line.role == ItemRole::Stem)
            stem_bottom = std::max(stem_bottom, std::max(line.from.y, line.to.y));
    }
    VCC_CHECK(below_y >= metrics.bottom_y() + 1.0);
    VCC_CHECK(below_y > stem_bottom);
    for (const LineItem* line : lines_with(below.display_list, ItemRole::TupletBracket)) {
        if (std::abs(line->from.x - line->to.x) < 1e-9) {
            VCC_CHECK_NEAR(std::max(line->from.y, line->to.y), below_y, 1e-9);
            VCC_CHECK(std::min(line->from.y, line->to.y) < below_y - 0.5);
        }
    }
}

VCC_TEST(a_tuplet_bracket_clears_stems_that_reach_past_the_staff)
{
    // A chord of C6 over G4 stems down from the G4, so the stems end two
    // and a half spaces below the staff - further than the staff clearance
    // alone would put the bracket. It has to clear the stems, not just the
    // noteheads.
    notation::Score score;
    const notation::Duration quarter = notation::triplet(notation::NoteValue::Quarter);
    for (int index = 0; index < 3; ++index) {
        score.insert_chord(0, static_cast<notation::Ticks>(index) * (2 * kQuarter / 3), quarter,
                           {notation::Pitch{notation::Step::G, 4, 0},
                            notation::Pitch{notation::Step::C, 6, 0}});
    }

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);
    const StaffMetrics metrics = metrics_for(result, 0);

    double stem_bottom = metrics.top_y;
    for (const LineItem& line : result.display_list.lines) {
        if (line.role == ItemRole::Stem)
            stem_bottom = std::max(stem_bottom, std::max(line.from.y, line.to.y));
    }
    VCC_CHECK(stem_bottom > metrics.bottom_y() + 2.0);

    double line_y = -1e9;
    for (const LineItem* line : lines_with(result.display_list, ItemRole::TupletBracket)) {
        if (std::abs(line->from.y - line->to.y) < 1e-9)
            line_y = std::max(line_y, line->from.y);
    }
    VCC_CHECK(line_y > stem_bottom + 0.5);
}

VCC_TEST(a_rest_inside_a_tuplet_is_still_one_of_its_three)
{
    // Note, rest, note under one 3. The rest breaks the beam, so this one
    // takes a bracket - spanning all three, rest included.
    notation::Score score;
    const notation::Duration eighth = notation::triplet(notation::NoteValue::Eighth);
    score.insert_note(0, note_at(0, eighth, notation::Pitch{notation::Step::G, 4, 0}));
    score.insert_note(0, rest_at((kQuarter / 3), eighth));
    score.insert_note(0, note_at((2 * kQuarter / 3), eighth, notation::Pitch{notation::Step::G, 4, 0}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(12.0), 0, kBar);

    const std::vector<const TextItem*> numbers =
        texts_with(result.display_list, ItemRole::TupletBracket);
    VCC_CHECK_EQ(numbers.size(), std::size_t{1});
    VCC_CHECK(numbers[0]->text == "3");

    const std::vector<const LineItem*> bracket =
        lines_with(result.display_list, ItemRole::TupletBracket);
    VCC_CHECK_EQ(bracket.size(), std::size_t{4});
    double left = 1e9;
    double right = -1e9;
    for (const LineItem* line : bracket) {
        left = std::min(left, std::min(line->from.x, line->to.x));
        right = std::max(right, std::max(line->from.x, line->to.x));
    }
    VCC_CHECK(left < TimelineMap(12.0).x_for_tick((kQuarter / 3)));
    VCC_CHECK(right > TimelineMap(12.0).x_for_tick((2 * kQuarter / 3)));
}

VCC_TEST(a_tuplet_closes_at_its_own_span_and_the_next_begins)
{
    // Six triplet quarters are two triplets, each a half note long, not
    // one bracket over the bar.
    notation::Score score;
    const notation::Duration quarter = notation::triplet(notation::NoteValue::Quarter);
    for (int index = 0; index < 6; ++index) {
        score.insert_note(0, note_at(static_cast<notation::Ticks>(index) * (2 * kQuarter / 3), quarter,
                                     notation::Pitch{notation::Step::E, 4, 0}));
    }

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);
    VCC_CHECK_EQ(texts_with(result.display_list, ItemRole::TupletBracket).size(),
                 std::size_t{2});
    VCC_CHECK_EQ(lines_with(result.display_list, ItemRole::TupletBracket).size(),
                 std::size_t{8});

    // Six sextuplet sixteenths are one 6, not two 3s: the shortest written
    // value keeps the group open past the point where three would have
    // added up to a whole triplet.
    notation::Score sextuplet;
    const notation::Duration sixteenth{notation::NoteValue::Sixteenth, 0, notation::Tuplet{6, 4}};
    for (int index = 0; index < 6; ++index) {
        sextuplet.insert_note(0, note_at(static_cast<notation::Ticks>(index) * (kQuarter / 6), sixteenth,
                                         notation::Pitch{notation::Step::G, 4, 0}));
    }
    const LayoutResult six = engine.layout(sextuplet, TimelineMap(16.0), 0, kBar);
    const std::vector<const TextItem*> numbers =
        texts_with(six.display_list, ItemRole::TupletBracket);
    VCC_CHECK_EQ(numbers.size(), std::size_t{1});
    VCC_CHECK(numbers[0]->text == "6");
}

VCC_TEST(an_incomplete_tuplet_still_shows_its_number)
{
    // A lone triplet eighth followed by plain notes is a mistake the composer
    // has to be able to see, so it is labelled rather than hidden.
    notation::Score score;
    score.insert_note(0, note_at(0, notation::triplet(notation::NoteValue::Eighth),
                                 notation::Pitch{notation::Step::G, 4, 0}));
    score.insert_note(0, note_at((kQuarter / 3), notation::quarter(), notation::Pitch{notation::Step::G, 4, 0}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(12.0), 0, kBar);
    const std::vector<const TextItem*> numbers =
        texts_with(result.display_list, ItemRole::TupletBracket);
    VCC_CHECK_EQ(numbers.size(), std::size_t{1});
    VCC_CHECK(numbers[0]->text == "3");

    // Two triplet eighths followed by a quintuplet are a 3 and a 5, not one
    // group that swallowed the change of ratio.
    notation::Score mixed;
    for (int index = 0; index < 2; ++index) {
        mixed.insert_note(0, note_at(static_cast<notation::Ticks>(index) * (kQuarter / 3),
                                     notation::triplet(notation::NoteValue::Eighth),
                                     notation::Pitch{notation::Step::G, 4, 0}));
    }
    const notation::Duration fifth{notation::NoteValue::Sixteenth, 0, notation::Tuplet{5, 4}};
    for (int index = 0; index < 5; ++index) {
        mixed.insert_note(0, note_at((2 * kQuarter / 3) + static_cast<notation::Ticks>(index) * (kQuarter / 5), fifth,
                                     notation::Pitch{notation::Step::G, 4, 0}));
    }
    const LayoutResult two = engine.layout(mixed, TimelineMap(16.0), 0, kBar);
    const std::vector<const TextItem*> labels =
        texts_with(two.display_list, ItemRole::TupletBracket);
    VCC_CHECK_EQ(labels.size(), std::size_t{2});
    VCC_CHECK(labels[0]->text == "3");
    VCC_CHECK(labels[1]->text == "5");

    // Ordinary notes get nothing.
    notation::Score plain;
    for (int index = 0; index < 4; ++index) {
        plain.insert_note(0, note_at(static_cast<notation::Ticks>(index) * (kQuarter / 2),
                                     notation::eighth(), notation::Pitch{notation::Step::G, 4, 0}));
    }
    const LayoutResult none = engine.layout(plain, TimelineMap(12.0), 0, kBar);
    VCC_CHECK(texts_with(none.display_list, ItemRole::TupletBracket).empty());
    VCC_CHECK(lines_with(none.display_list, ItemRole::TupletBracket).empty());
}

VCC_TEST(a_tuplet_inside_a_tuplet_takes_a_second_bracket_further_out)
{
    // A triplet of eighths whose middle eighth is itself a triplet of
    // sixteenths: the sixteenths take their own 3 nearest the notes, and
    // the whole figure takes a bracketed 3 beyond it, since the inner
    // number is what the outer bracket has to clear.
    notation::Score score;
    const notation::Pitch pitch{notation::Step::G, 4, 0};
    const notation::Duration eighth = notation::triplet(notation::NoteValue::Eighth);
    notation::Duration sixteenth = notation::triplet(notation::NoteValue::Sixteenth);
    sixteenth.outer = notation::Tuplet{3, 2};
    VCC_CHECK_EQ(sixteenth.ticks(), kQuarter / 9);

    score.insert_note(0, note_at(0, eighth, pitch));
    for (int index = 0; index < 3; ++index) {
        score.insert_note(0, note_at(kQuarter / 3 + static_cast<notation::Ticks>(index) * (kQuarter / 9),
                                     sixteenth, pitch));
    }
    score.insert_note(0, note_at(2 * kQuarter / 3, eighth, pitch));

    LayoutEngine engine;
    const TimelineMap timeline(24.0);
    const LayoutResult result = engine.layout(score, timeline, 0, kBar);

    const std::vector<const TextItem*> numbers =
        texts_with(result.display_list, ItemRole::TupletBracket);
    VCC_CHECK_EQ(numbers.size(), std::size_t{2});
    if (numbers.size() != 2)
        return;
    VCC_CHECK(numbers[0]->text == "3" && numbers[1]->text == "3");

    // The inner number is centred on the sixteenths, the outer on the
    // whole figure - between the first and last heads of each, as a
    // bracket runs.
    const double inner_centre =
        (timeline.x_for_tick(kQuarter / 3) + timeline.x_for_tick(kQuarter / 3 + 2 * kQuarter / 9)) * 0.5;
    const double outer_centre = (timeline.x_for_tick(0) + timeline.x_for_tick(2 * kQuarter / 3)) * 0.5;
    const TextItem* inner = numbers[0];
    const TextItem* outer = numbers[1];
    if (std::abs(inner->origin.x - inner_centre) > std::abs(outer->origin.x - inner_centre))
        std::swap(inner, outer);
    // The origin is the number's left edge; a "3" is under a space wide.
    VCC_CHECK(std::abs(inner->origin.x - inner_centre) < 1.5);
    VCC_CHECK(std::abs(outer->origin.x - outer_centre) < 1.5);

    // G4 stems up, so both go above, and the outer is further up than the
    // inner, with its bracket between the two.
    const StaffMetrics metrics = metrics_for(result, 0);
    VCC_CHECK(inner->origin.y < metrics.top_y);
    VCC_CHECK(outer->origin.y < inner->origin.y - 1.0);
    const std::vector<const LineItem*> lines =
        lines_with(result.display_list, ItemRole::TupletBracket);
    VCC_CHECK(!lines.empty());
    double bracket_left = 1e9;
    double bracket_right = -1e9;
    double bracket_y = 1e9;
    for (const LineItem* line : lines) {
        bracket_left = std::min(bracket_left, std::min(line->from.x, line->to.x));
        bracket_right = std::max(bracket_right, std::max(line->from.x, line->to.x));
        bracket_y = std::min(bracket_y, std::min(line->from.y, line->to.y));
    }
    VCC_CHECK(bracket_left < timeline.x_for_tick(0) + 1.0);
    VCC_CHECK(bracket_right > timeline.x_for_tick(2 * kQuarter / 3));
    VCC_CHECK(bracket_y < inner->origin.y);

    // And a plain triplet beside it is still a plain triplet.
    for (int index = 0; index < 3; ++index) {
        score.insert_note(0, note_at(kQuarter + static_cast<notation::Ticks>(index) * (kQuarter / 3),
                                     eighth, pitch));
    }
    const LayoutResult more = engine.layout(score, timeline, 0, kBar);
    VCC_CHECK_EQ(texts_with(more.display_list, ItemRole::TupletBracket).size(), std::size_t{3});

    // An inner triplet closes as a whole in its outer tuplet's time: six
    // nested sixteenths after the first eighth are two inner 3s under one
    // outer, not one 6 or one run of six.
    notation::Score twice;
    twice.insert_note(0, note_at(0, eighth, pitch));
    for (int index = 0; index < 6; ++index) {
        twice.insert_note(0, note_at(kQuarter / 3 + static_cast<notation::Ticks>(index) * (kQuarter / 9),
                                     sixteenth, pitch));
    }
    const LayoutResult pair = engine.layout(twice, timeline, 0, kBar);
    const std::vector<const TextItem*> three =
        texts_with(pair.display_list, ItemRole::TupletBracket);
    VCC_CHECK_EQ(three.size(), std::size_t{3});
    for (const TextItem* number : three)
        VCC_CHECK(number->text == "3");
}

VCC_TEST(a_tuplet_does_not_cross_a_barline)
{
    // A triplet quarter at the end of one bar and two at the start of the
    // next are two groups, both incomplete, rather than one that spans the
    // barline.
    notation::Score score;
    const notation::Duration quarter = notation::triplet(notation::NoteValue::Quarter);
    score.insert_note(0, note_at(kBar - (2 * kQuarter / 3), quarter, notation::Pitch{notation::Step::E, 4, 0}));
    score.insert_note(0, note_at(kBar, quarter, notation::Pitch{notation::Step::E, 4, 0}));
    score.insert_note(0, note_at(kBar + (2 * kQuarter / 3), quarter, notation::Pitch{notation::Step::E, 4, 0}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, 2 * kBar);
    VCC_CHECK_EQ(texts_with(result.display_list, ItemRole::TupletBracket).size(),
                 std::size_t{2});
}

VCC_TEST(tuplets_can_be_turned_off)
{
    notation::Score score;
    const notation::Duration quarter = notation::triplet(notation::NoteValue::Quarter);
    for (int index = 0; index < 3; ++index) {
        score.insert_note(0, note_at(static_cast<notation::Ticks>(index) * (2 * kQuarter / 3), quarter,
                                     notation::Pitch{notation::Step::E, 4, 0}));
    }

    LayoutOptions options;
    options.draw_tuplets = false;
    LayoutEngine engine(GlyphTable{}, options);
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);
    VCC_CHECK(texts_with(result.display_list, ItemRole::TupletBracket).empty());
    VCC_CHECK(lines_with(result.display_list, ItemRole::TupletBracket).empty());
    // The notes themselves are unaffected.
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Notehead), std::size_t{3});
}

VCC_TEST(dotted_notes_get_their_dots_in_a_space)
{
    notation::Score score;
    // E4 sits on the bottom line, so its dot is raised to the space above.
    score.insert_note(0, note_at(0, notation::quarter(2), notation::Pitch{notation::Step::E, 4, 0}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::AugmentationDot), std::size_t{2});

    const StaffMetrics metrics = metrics_for(result, 0);
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role == ItemRole::AugmentationDot)
            VCC_CHECK_NEAR(item.origin.y, metrics.y_for_staff_position(1), 1e-9);
    }
}

VCC_TEST(rests_are_placed_by_value)
{
    notation::Score score;
    score.insert_note(0, rest_at(0, notation::whole()));
    score.insert_note(0, rest_at(kBar, notation::quarter()));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, 2 * kBar);

    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Rest), std::size_t{2});
    VCC_CHECK_EQ(result.notes.size(), std::size_t{2});
    VCC_CHECK(result.notes[0].is_rest);

    const StaffMetrics metrics = metrics_for(result, 0);
    // A whole rest hangs from the second line down; a quarter rest is
    // centred on the middle line.
    VCC_CHECK_NEAR(result.notes[0].centre.y, metrics.y_for_staff_position(6), 1e-9);
    VCC_CHECK_NEAR(result.notes[1].centre.y, metrics.y_for_staff_position(4), 1e-9);
}

// --- Ties, slurs and markings ----------------------------------------------

VCC_TEST(a_tie_is_drawn_between_the_notes_it_joins)
{
    notation::Score score;
    const notation::Pitch pitch{notation::Step::G, 4, 0};
    notation::ScoreNote first = note_at(0, notation::quarter(), pitch);
    first.tied_to_next = true;
    score.insert_note(0, first);
    score.insert_note(0, note_at(kQuarter, notation::quarter(), pitch));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    VCC_CHECK_EQ(count_curves(result.display_list, ItemRole::Tie), std::size_t{1});

    const CurveItem& tie = result.display_list.curves.front();
    // It runs from the first notehead to the second, and arches away from
    // the up stem - downward, which in these coordinates is a larger y.
    VCC_CHECK(tie.end.x > tie.start.x);
    VCC_CHECK(tie.control_one.y > tie.start.y);
}

VCC_TEST(a_slur_spans_the_range_it_covers)
{
    notation::Score score;
    score.add_slur(notation::Slur{0, (2 * kQuarter), 0});
    score.insert_note(0, note_at(0, notation::quarter(), notation::Pitch{notation::Step::G, 4, 0}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    VCC_CHECK_EQ(count_curves(result.display_list, ItemRole::Slur), std::size_t{1});

    const TimelineMap timeline(8.0);
    for (const CurveItem& curve : result.display_list.curves) {
        if (curve.role != ItemRole::Slur)
            continue;
        VCC_CHECK_NEAR(curve.start.x, timeline.x_for_tick(0), 1e-9);
        VCC_CHECK_NEAR(curve.end.x, timeline.x_for_tick((2 * kQuarter)), 1e-9);
        // Above the staff, and arching upward.
        VCC_CHECK(curve.control_one.y < curve.start.y);
    }
}

VCC_TEST(dynamics_are_composed_from_letterforms_below_the_staff)
{
    notation::Score score;
    score.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::MezzoForte, 0, ""});
    score.add_dynamic(notation::DynamicMarking{kQuarter, notation::DynamicLevel::Pianississimo, 0, ""});

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    // "mf" is two letterforms, "ppp" three.
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Dynamic), std::size_t{5});

    const StaffMetrics metrics = metrics_for(result, 0);
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role == ItemRole::Dynamic)
            VCC_CHECK(item.origin.y > metrics.bottom_y());
    }
}

VCC_TEST(a_text_dynamic_renders_as_what_was_written)
{
    notation::Score score;
    score.add_dynamic(
        notation::DynamicMarking{0, notation::DynamicLevel::Piano, 0, "sotto voce"});

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Dynamic), std::size_t{0});

    // The bar numbers on the ruler are text too, so the marking is found by
    // role rather than by being the only text on the page.
    const TextItem* written = nullptr;
    for (const TextItem& text : result.display_list.texts) {
        if (text.role == ItemRole::Dynamic)
            written = &text;
    }
    VCC_CHECK(written != nullptr);
    VCC_CHECK_EQ(written->text, std::string("sotto voce"));
    VCC_CHECK(written->italic);
}

VCC_TEST(a_hairpin_opens_toward_its_louder_end)
{
    notation::Score score;
    notation::Hairpin crescendo;
    crescendo.start_tick = 0;
    crescendo.end_tick = (2 * kQuarter);
    crescendo.kind = notation::HairpinKind::Crescendo;
    score.add_hairpin(crescendo);

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    // Two lines meeting at the quiet end.
    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::Hairpin), std::size_t{2});

    std::vector<const LineItem*> hairpin;
    for (const LineItem& line : result.display_list.lines) {
        if (line.role == ItemRole::Hairpin)
            hairpin.push_back(&line);
    }
    // Both start at the same point, and separate to the right.
    VCC_CHECK_NEAR(hairpin[0]->from.x, hairpin[1]->from.x, 1e-9);
    VCC_CHECK_NEAR(hairpin[0]->from.y, hairpin[1]->from.y, 1e-9);
    VCC_CHECK(std::abs(hairpin[0]->to.y - hairpin[1]->to.y) > 0.5);
}

VCC_TEST(a_hairpin_that_starts_off_screen_is_clipped_not_dropped)
{
    notation::Score score;
    notation::Hairpin hairpin;
    hairpin.start_tick = 0;
    hairpin.end_tick = (8 * kQuarter);
    score.add_hairpin(hairpin);

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), (2 * kQuarter), kBar);

    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::Hairpin), std::size_t{2});
}

VCC_TEST(a_slur_is_drawn_on_its_own_staff_and_no_other)
{
    // A grand staff used to print every slur twice, once over each staff,
    // because a slur carried no staff. It does now.
    notation::Score score;
    score.make_grand_staff();
    score.insert_note(0, note_at(0, notation::quarter(), notation::Pitch{notation::Step::G, 4, 0}));
    score.insert_note(1, note_at(0, notation::quarter(), notation::Pitch{notation::Step::D, 3, 0}));
    score.add_slur(notation::Slur{0, (2 * kQuarter), 0, 1});

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    VCC_CHECK_EQ(count_curves(result.display_list, ItemRole::Slur), std::size_t{1});
    const StaffMetrics lower = metrics_for(result, 1);
    const StaffMetrics upper = metrics_for(result, 0);
    for (const CurveItem& curve : result.display_list.curves) {
        if (curve.role != ItemRole::Slur)
            continue;
        // Above the lower staff: below the upper one's bottom line.
        VCC_CHECK(curve.start.y > upper.bottom_y());
        VCC_CHECK(curve.start.y < lower.top_y);
    }
}

VCC_TEST(a_slur_clears_what_lies_under_it)
{
    // Not fitted to the stems, but not through them either: a slur over a
    // note on ledger lines above the staff sits above that note, where a
    // fixed height put it through the notehead - and through the accent on
    // the first note of the slur, in the first picture of real notation.
    notation::Score score;
    score.insert_note(0, note_at(0, notation::quarter(), notation::Pitch{notation::Step::E, 6, 0}));
    score.insert_note(0, note_at(kQuarter, notation::quarter(), notation::Pitch{notation::Step::E, 6, 0}));
    score.add_slur(notation::Slur{0, (2 * kQuarter), 0, 0});

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);
    const StaffMetrics metrics = metrics_for(result, 0);

    double head_top = metrics.bottom_y();
    for (const LaidOutNote& note : result.notes)
        head_top = std::min(head_top, note.notehead_bounds.y);
    VCC_CHECK(head_top < metrics.top_y - 1.5);  // The note is above the default height.

    const CurveItem* slur = nullptr;
    for (const CurveItem& curve : result.display_list.curves) {
        if (curve.role == ItemRole::Slur)
            slur = &curve;
    }
    VCC_CHECK(slur != nullptr);
    if (slur != nullptr)
        VCC_CHECK(slur->start.y < head_top - 0.5);

    // And a slur over nothing tall keeps the default height, so this is a
    // clearance and not a general lift - even with a tall note elsewhere on
    // the staff, outside the span. Only what lies *under* the slur counts.
    notation::Score low;
    low.insert_note(0, note_at(0, notation::quarter(), notation::Pitch{notation::Step::G, 4, 0}));
    low.insert_note(0, note_at((3 * kQuarter), notation::quarter(), notation::Pitch{notation::Step::E, 6, 0}));
    low.add_slur(notation::Slur{0, (2 * kQuarter), 0, 0});
    const LayoutResult plain = engine.layout(low, TimelineMap(8.0), 0, kBar);
    std::size_t slurs = 0;
    for (const CurveItem& curve : plain.display_list.curves) {
        if (curve.role != ItemRole::Slur)
            continue;
        ++slurs;
        VCC_CHECK_NEAR(curve.start.y, metrics.top_y - 1.5, 1e-9);
    }
    VCC_CHECK_EQ(slurs, std::size_t{1});
}

namespace {

const CurveItem* only_slur(const LayoutResult& result, std::size_t which = 0)
{
    std::size_t seen = 0;
    for (const CurveItem& curve : result.display_list.curves) {
        if (curve.role != ItemRole::Slur)
            continue;
        if (seen == which)
            return &curve;
        ++seen;
    }
    return nullptr;
}

/// The arch's midpoint, from the cubic: where it clears things, or does not.
double midpoint_y(const CurveItem& curve)
{
    return (curve.start.y + 3.0 * curve.control_one.y + 3.0 * curve.control_two.y + curve.end.y)
           / 8.0;
}

}  // namespace

VCC_TEST(a_slur_sits_away_from_the_stems_and_joins_the_notes_it_covers)
{
    // Low notes, stems up: the slur goes under them, from the first head to
    // the last, not from the slur's first tick to its last.
    notation::Score low;
    const notation::EventId first_low =
        low.insert_note(0, note_at(0, notation::quarter(), {notation::Step::C, 4, 0}));
    low.insert_note(0, note_at(kQuarter, notation::quarter(), {notation::Step::D, 4, 0}));
    const notation::EventId last_low =
        low.insert_note(0, note_at((2 * kQuarter), notation::quarter(), {notation::Step::E, 4, 0}));
    low.add_slur(notation::Slur{0, (3 * kQuarter), 0, 0});

    LayoutEngine engine;
    const TimelineMap timeline(8.0);
    const LayoutResult under = engine.layout(low, timeline, 0, kBar);
    const CurveItem* below = only_slur(under);
    VCC_CHECK(below != nullptr);
    if (below != nullptr) {
        const LaidOutNote& first = *find_note(under, first_low);
        const LaidOutNote& last = *find_note(under, last_low);
        VCC_CHECK(first.stem_up);
        VCC_CHECK(below->start.y > first.notehead_bounds.bottom());
        VCC_CHECK(below->end.y > last.notehead_bounds.bottom());
        VCC_CHECK_NEAR(below->start.x, first.centre.x, 1e-9);
        VCC_CHECK_NEAR(below->end.x, last.centre.x, 1e-9);
        VCC_CHECK(below->end.x < timeline.x_for_tick((3 * kQuarter)));
        // Arching downward, away from the notes.
        VCC_CHECK(below->control_one.y > below->start.y);
    }

    // High notes, stems down: over them, from head to head.
    notation::Score high;
    const notation::EventId first_high =
        high.insert_note(0, note_at(0, notation::quarter(), {notation::Step::B, 5, 0}));
    high.insert_note(0, note_at(kQuarter, notation::quarter(), {notation::Step::A, 5, 0}));
    high.add_slur(notation::Slur{0, (2 * kQuarter), 0, 0});
    const LayoutResult over = engine.layout(high, timeline, 0, kBar);
    const CurveItem* above = only_slur(over);
    VCC_CHECK(above != nullptr);
    if (above != nullptr) {
        const LaidOutNote& first = *find_note(over, first_high);
        VCC_CHECK(!first.stem_up);
        VCC_CHECK(above->start.y < first.notehead_bounds.y);
        VCC_CHECK_NEAR(above->start.x, first.centre.x, 1e-9);
        VCC_CHECK(above->control_one.y < above->start.y);
    }
}

VCC_TEST(a_slur_over_disagreeing_stems_goes_above_and_leaves_from_the_stem)
{
    notation::Score score;
    const notation::EventId low =
        score.insert_note(0, note_at(0, notation::quarter(), {notation::Step::C, 4, 0}));
    const notation::EventId high =
        score.insert_note(0, note_at(kQuarter, notation::quarter(), {notation::Step::B, 5, 0}));
    score.add_slur(notation::Slur{0, (2 * kQuarter), 0, 0});

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);
    const CurveItem* slur = only_slur(result);
    VCC_CHECK(slur != nullptr);
    if (slur == nullptr)
        return;

    // Above, since the stems do not agree. The first note's stem points up
    // toward it, so the slur leaves from the stem's end; the last note's
    // stem points away, so it arrives above the head.
    const LaidOutNote& first = *find_note(result, low);
    const LaidOutNote& last = *find_note(result, high);
    VCC_CHECK(first.stem_up && !last.stem_up);
    VCC_CHECK_NEAR(slur->start.x, first.stem_x, 1e-9);
    VCC_CHECK(slur->start.y < first.stem_end_y);
    VCC_CHECK(slur->start.y > first.stem_end_y - 1.0);
    VCC_CHECK_NEAR(slur->end.x, last.centre.x, 1e-9);
    VCC_CHECK(slur->end.y < last.notehead_bounds.y);
    VCC_CHECK(slur->control_one.y < slur->start.y);
}

VCC_TEST(a_slur_in_a_shared_measure_takes_its_voices_outer_side)
{
    // Two voices on one staff, each with a slur. By the stems alone both
    // would go on the head side and meet in the middle of the staff; by
    // voice the upper's goes above its up stems and the lower's below its
    // down stems.
    notation::Score score;
    for (int index = 0; index < 2; ++index) {
        notation::ScoreNote upper = note_at(index * kQuarter, notation::quarter(),
                                            {notation::Step::G, 4, 0});
        score.insert_note(0, upper);
        notation::ScoreNote lower = note_at(index * kQuarter, notation::quarter(),
                                            {notation::Step::C, 4, 0});
        lower.voice = 1;
        score.insert_note(0, lower);
    }
    // The lower voice carries on a beat after the upper one stops.
    notation::ScoreNote onward = note_at((2 * kQuarter), notation::quarter(), {notation::Step::C, 4, 0});
    onward.voice = 1;
    score.insert_note(0, onward);
    score.add_slur(notation::Slur{0, (3 * kQuarter), 0, 0});
    score.add_slur(notation::Slur{0, (3 * kQuarter), 1, 0});

    LayoutEngine engine;
    const TimelineMap timeline(8.0);
    const LayoutResult result = engine.layout(score, timeline, 0, kBar);
    const StaffMetrics metrics = metrics_for(result, 0);

    const CurveItem* upper = only_slur(result, 0);
    const CurveItem* lower = only_slur(result, 1);
    VCC_CHECK(upper != nullptr && lower != nullptr);
    if (upper == nullptr || lower == nullptr)
        return;

    // The upper voice's slur above the staff, from stem end to stem end.
    VCC_CHECK(upper->start.y < metrics.top_y);
    VCC_CHECK(upper->control_one.y < upper->start.y);
    // The lower voice's below the staff, from stem end to stem end.
    VCC_CHECK(lower->start.y > metrics.bottom_y());
    VCC_CHECK(lower->control_one.y > lower->start.y);

    // Each fitted to its own voice's notes: the upper voice's slur ends at
    // the upper voice's last note, not at the lower voice's note after it.
    VCC_CHECK(upper->end.x < timeline.x_for_tick((2 * kQuarter)));
    VCC_CHECK(lower->end.x > timeline.x_for_tick((2 * kQuarter)));
}

VCC_TEST(a_slur_over_chords_leaves_from_the_head_nearest_it)
{
    notation::Score score;
    const std::vector<notation::EventId> first = score.insert_chord(
        0, 0, notation::quarter(), {{notation::Step::C, 5, 0}, {notation::Step::B, 5, 0}});
    score.insert_chord(0, kQuarter, notation::quarter(),
                       {{notation::Step::C, 5, 0}, {notation::Step::B, 5, 0}});
    score.add_slur(notation::Slur{0, (2 * kQuarter), 0, 0});

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);
    const CurveItem* slur = only_slur(result);
    VCC_CHECK(slur != nullptr);
    if (slur == nullptr)
        return;

    // Stems down, so the slur goes above - and above the *top* head of
    // the chord, not the bottom one it would have found first.
    const LaidOutNote& bottom = *find_note(result, first[0]);
    const LaidOutNote& top = *find_note(result, first[1]);
    VCC_CHECK(!top.stem_up);
    VCC_CHECK(top.notehead_bounds.y < bottom.notehead_bounds.y);
    VCC_CHECK(slur->start.y < top.notehead_bounds.y);
}

VCC_TEST(a_slur_rises_to_clear_a_note_further_out_than_its_ends)
{
    notation::Score score;
    score.insert_note(0, note_at(0, notation::quarter(), {notation::Step::B, 5, 0}));
    const notation::EventId peak =
        score.insert_note(0, note_at(kQuarter, notation::quarter(), {notation::Step::A, 6, 0}));
    score.insert_note(0, note_at((2 * kQuarter), notation::quarter(), {notation::Step::B, 5, 0}));
    score.add_slur(notation::Slur{0, (3 * kQuarter), 0, 0});

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);
    const CurveItem* slur = only_slur(result);
    VCC_CHECK(slur != nullptr);
    if (slur == nullptr)
        return;

    // The ends sit over the B5s; the middle has to get over the A6 between
    // them, so the arch is taller than the ends alone would make it.
    const double peak_top = find_note(result, peak)->notehead_bounds.y;
    VCC_CHECK(slur->start.y > peak_top);
    VCC_CHECK(midpoint_y(*slur) < peak_top - 0.5);

    // And a short slur arches less than a long one: height follows length
    // rather than being one number.
    notation::Score brief;
    brief.insert_note(0, note_at(0, notation::eighth(), {notation::Step::B, 5, 0}));
    brief.insert_note(0, note_at((kQuarter / 2), notation::eighth(), {notation::Step::B, 5, 0}));
    brief.add_slur(notation::Slur{0, kQuarter, 0, 0});
    notation::Score lengthy;
    lengthy.insert_note(0, note_at(0, notation::whole(), {notation::Step::B, 5, 0}));
    lengthy.insert_note(0, note_at(kBar, notation::whole(), {notation::Step::B, 5, 0}));
    lengthy.add_slur(notation::Slur{0, 2 * kBar, 0, 0});
    const LayoutResult short_result = engine.layout(brief, TimelineMap(8.0), 0, kBar);
    const CurveItem* short_slur = only_slur(short_result);
    const LayoutResult long_result = engine.layout(lengthy, TimelineMap(8.0), 0, 2 * kBar);
    const CurveItem* long_slur = only_slur(long_result);
    VCC_CHECK(short_slur != nullptr && long_slur != nullptr);
    if (short_slur != nullptr && long_slur != nullptr) {
        VCC_CHECK(std::abs(short_slur->control_one.y - short_slur->start.y)
                  < std::abs(long_slur->control_one.y - long_slur->start.y));
    }
}

VCC_TEST(a_slur_starts_past_the_articulation_on_its_first_note)
{
    // High notes, stems down, slur above: the accent over the first note
    // sits where the slur would start, and the slur starts above it.
    notation::Score score;
    notation::ScoreNote accented = note_at(0, notation::quarter(), {notation::Step::B, 5, 0});
    accented.articulations = notation::to_mask(notation::Articulation::Accent);
    const notation::EventId first = score.insert_note(0, accented);
    score.insert_note(0, note_at(kQuarter, notation::quarter(), {notation::Step::B, 5, 0}));
    score.add_slur(notation::Slur{0, (2 * kQuarter), 0, 0});

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);
    const CurveItem* slur = only_slur(result);
    VCC_CHECK(slur != nullptr);

    double accent_top = 0.0;
    bool found = false;
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role == ItemRole::Articulation && item.source_id == first) {
            accent_top = item.bounds.y;
            found = true;
        }
    }
    VCC_CHECK(found);
    if (slur != nullptr && found) {
        VCC_CHECK(slur->start.y < accent_top);
        // The other end, with nothing over its note, stays at the head.
        VCC_CHECK(slur->end.y > slur->start.y);
    }
}

VCC_TEST(a_slur_over_rests_alone_keeps_the_flat_arch)
{
    notation::Score score;
    score.insert_note(0, rest_at(0, notation::quarter()));
    score.insert_note(0, rest_at(kQuarter, notation::quarter()));
    score.add_slur(notation::Slur{0, (2 * kQuarter), 0, 0});

    LayoutEngine engine;
    const TimelineMap timeline(8.0);
    const LayoutResult result = engine.layout(score, timeline, 0, kBar);
    const StaffMetrics metrics = metrics_for(result, 0);
    const CurveItem* slur = only_slur(result);
    VCC_CHECK(slur != nullptr);
    if (slur != nullptr) {
        VCC_CHECK_NEAR(slur->start.x, timeline.x_for_tick(0), 1e-9);
        VCC_CHECK_NEAR(slur->end.x, timeline.x_for_tick((2 * kQuarter)), 1e-9);
        VCC_CHECK_NEAR(slur->start.y, metrics.top_y - 1.5, 1e-9);
    }
}

VCC_TEST(a_hairpin_gives_way_to_the_dynamics_it_meets)
{
    // A dynamic at the hairpin's start shares its tick and its baseline, so
    // the hairpin begins after it; one at its end is left a gap. The first
    // picture of real notation had "dim." underlined by its own diminuendo.
    notation::Score score;
    score.insert_note(0, note_at(0, notation::quarter(), notation::Pitch{notation::Step::G, 4, 0}));
    score.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::MezzoForte, 0, "dim."});
    score.add_dynamic(notation::DynamicMarking{(2 * kQuarter), notation::DynamicLevel::Piano, 0, ""});
    notation::Hairpin hairpin;
    hairpin.start_tick = 0;
    hairpin.end_tick = (2 * kQuarter);
    hairpin.kind = notation::HairpinKind::Diminuendo;
    score.add_hairpin(hairpin);

    LayoutEngine engine;
    const TimelineMap timeline(8.0);
    const LayoutResult result = engine.layout(score, timeline, 0, kBar);

    double left = 1e9;
    double right = -1e9;
    for (const LineItem& line : result.display_list.lines) {
        if (line.role != ItemRole::Hairpin)
            continue;
        left = std::min(left, std::min(line.from.x, line.to.x));
        right = std::max(right, std::max(line.from.x, line.to.x));
    }
    VCC_CHECK(left > timeline.x_for_tick(0) + 2.0);        // After four characters of text.
    VCC_CHECK(right < timeline.x_for_tick((2 * kQuarter)) - 0.3);    // Short of the p.

    // With no dynamic at either end the hairpin takes its whole range.
    notation::Score bare;
    bare.insert_note(0, note_at(0, notation::quarter(), notation::Pitch{notation::Step::G, 4, 0}));
    bare.add_hairpin(hairpin);
    const LayoutResult whole = engine.layout(bare, timeline, 0, kBar);
    for (const LineItem& line : whole.display_list.lines) {
        if (line.role != ItemRole::Hairpin)
            continue;
        VCC_CHECK_NEAR(std::min(line.from.x, line.to.x), timeline.x_for_tick(0), 1e-9);
        VCC_CHECK_NEAR(std::max(line.from.x, line.to.x), timeline.x_for_tick((2 * kQuarter)), 1e-9);
    }
}

namespace {

/// The dynamic glyphs whose left edge is at `x`, split by which side of
/// the staff they sit on.
struct DynamicsAt {
    std::vector<const GlyphItem*> above;
    std::vector<const GlyphItem*> below;
};

DynamicsAt dynamics_at(const LayoutResult& result, double x, const StaffMetrics& metrics)
{
    DynamicsAt found;
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role != ItemRole::Dynamic || std::abs(item.origin.x - x) > 1e-6)
            continue;
        if (item.origin.y < metrics.top_y)
            found.above.push_back(&item);
        else if (item.origin.y > metrics.bottom_y())
            found.below.push_back(&item);
    }
    return found;
}

/// A bar of quarters in `voice` on staff 0, from `tick`.
void fill_a_bar(notation::Score& score, notation::Ticks tick, std::uint8_t voice,
                notation::Step step)
{
    for (int beat = 0; beat < 4; ++beat) {
        notation::ScoreNote note =
            note_at(tick + beat * kQuarter, notation::quarter(), notation::Pitch{step, 4, 0});
        note.voice = voice;
        score.insert_note(0, note);
    }
}

}  // namespace

VCC_TEST(dynamics_in_a_shared_measure_go_on_their_voices_sides)
{
    // Bar 1 has two voices, bar 2 one. The lower voice's dynamics go below,
    // its outer side; voice 0's go where the placement puts them, except
    // where the lower voice has its own at the same tick, which makes voice
    // 0's the upper voice's: then it goes above, and the two do not
    // overprint. Outside the shared bar the placement decides for every
    // voice, as before.
    notation::Score score;
    fill_a_bar(score, 0, 0, notation::Step::G);
    fill_a_bar(score, 0, 1, notation::Step::E);
    fill_a_bar(score, kBar, 0, notation::Step::G);
    score.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::MezzoForte, 0, ""});
    score.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::Piano, 1, ""});
    score.add_dynamic(notation::DynamicMarking{(2 * kQuarter), notation::DynamicLevel::Piano, 0, ""});
    score.add_dynamic(notation::DynamicMarking{kBar + kQuarter, notation::DynamicLevel::Forte, 1, ""});

    const TimelineMap timeline(8.0);
    {
        LayoutEngine engine;
        const LayoutResult result = engine.layout(score, timeline, 0, 2 * kBar);
        const StaffMetrics metrics = metrics_for(result, 0);

        const DynamicsAt at_start = dynamics_at(result, timeline.x_for_tick(0), metrics);
        VCC_CHECK_EQ(at_start.above.size(), std::size_t{1});  // The m of voice 0's mf.
        VCC_CHECK_EQ(at_start.below.size(), std::size_t{1});  // Voice 1's p.

        const DynamicsAt alone = dynamics_at(result, timeline.x_for_tick((2 * kQuarter)), metrics);
        VCC_CHECK_EQ(alone.above.size(), std::size_t{0});
        VCC_CHECK_EQ(alone.below.size(), std::size_t{1});

        // Voice 1's p at the start sits on the same row as voice 0's lone
        // p: voice 0's mf is on the other side, so it pushes nothing out.
        if (at_start.below.size() == 1 && alone.below.size() == 1)
            VCC_CHECK_NEAR(at_start.below[0]->origin.y, alone.below[0]->origin.y, 1e-9);

        const DynamicsAt unshared = dynamics_at(result, timeline.x_for_tick(kBar + kQuarter), metrics);
        VCC_CHECK_EQ(unshared.above.size(), std::size_t{0});
        VCC_CHECK_EQ(unshared.below.size(), std::size_t{1});
    }

    // With the placement above: voice 1's in the shared bar still goes
    // below - its outer side is not a matter of placement - while voice 0's
    // alone, and voice 1's in the unshared bar, follow the placement.
    LayoutOptions options;
    options.dynamics_placement = Placement::Above;
    LayoutEngine engine(GlyphTable{}, options);
    const LayoutResult result = engine.layout(score, timeline, 0, 2 * kBar);
    const StaffMetrics metrics = metrics_for(result, 0);

    const DynamicsAt at_start = dynamics_at(result, timeline.x_for_tick(0), metrics);
    VCC_CHECK_EQ(at_start.above.size(), std::size_t{1});
    VCC_CHECK_EQ(at_start.below.size(), std::size_t{1});
    const DynamicsAt alone = dynamics_at(result, timeline.x_for_tick((2 * kQuarter)), metrics);
    VCC_CHECK_EQ(alone.above.size(), std::size_t{1});
    VCC_CHECK_EQ(alone.below.size(), std::size_t{0});
    const DynamicsAt unshared = dynamics_at(result, timeline.x_for_tick(kBar + kQuarter), metrics);
    VCC_CHECK_EQ(unshared.above.size(), std::size_t{1});
    VCC_CHECK_EQ(unshared.below.size(), std::size_t{0});
}

VCC_TEST(an_up_stem_voice_of_its_own_puts_its_dynamics_above)
{
    // Voice 2 stems up like voice 0 but is a voice of its own, so in a
    // shared bar its dynamics go above - while voice 0's at the same tick
    // stays below, since no down-stem voice has a marking there to make
    // voice 0's the upper voice's.
    notation::Score score;
    fill_a_bar(score, 0, 0, notation::Step::G);
    fill_a_bar(score, 0, 1, notation::Step::E);
    fill_a_bar(score, 0, 2, notation::Step::B);
    score.add_dynamic(notation::DynamicMarking{(2 * kQuarter), notation::DynamicLevel::Forte, 2, ""});
    score.add_dynamic(notation::DynamicMarking{(2 * kQuarter), notation::DynamicLevel::Piano, 0, ""});

    LayoutEngine engine;
    const TimelineMap timeline(8.0);
    const LayoutResult result = engine.layout(score, timeline, 0, kBar);
    const StaffMetrics metrics = metrics_for(result, 0);

    const DynamicsAt at = dynamics_at(result, timeline.x_for_tick((2 * kQuarter)), metrics);
    VCC_CHECK_EQ(at.above.size(), std::size_t{1});
    VCC_CHECK_EQ(at.below.size(), std::size_t{1});
    if (at.above.size() == 1)
        VCC_CHECK(at.above[0]->glyph == Glyph::DynamicForte);
    if (at.below.size() == 1)
        VCC_CHECK(at.below[0]->glyph == Glyph::DynamicPiano);
}

VCC_TEST(dynamics_of_two_voices_on_one_side_stack_rather_than_overprint)
{
    // Voices 1 and 3 both stem down, so both their dynamics go below in a
    // shared bar; the higher-numbered voice's goes a row further out. In
    // the unshared bar voice 0's and voice 1's both go below, and stack the
    // same way.
    notation::Score score;
    fill_a_bar(score, 0, 0, notation::Step::G);
    fill_a_bar(score, 0, 1, notation::Step::E);
    fill_a_bar(score, 0, 3, notation::Step::C);
    fill_a_bar(score, kBar, 0, notation::Step::G);
    score.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::Piano, 1, ""});
    score.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::Forte, 3, ""});
    score.add_dynamic(notation::DynamicMarking{kBar, notation::DynamicLevel::Piano, 0, ""});
    score.add_dynamic(notation::DynamicMarking{kBar, notation::DynamicLevel::Forte, 1, ""});

    LayoutEngine engine;
    const TimelineMap timeline(8.0);
    const LayoutResult result = engine.layout(score, timeline, 0, 2 * kBar);
    const StaffMetrics metrics = metrics_for(result, 0);

    const DynamicsAt shared = dynamics_at(result, timeline.x_for_tick(0), metrics);
    VCC_CHECK_EQ(shared.above.size(), std::size_t{0});
    VCC_CHECK_EQ(shared.below.size(), std::size_t{2});
    if (shared.below.size() == 2) {
        VCC_CHECK(std::abs(shared.below[0]->origin.y - shared.below[1]->origin.y) >= 1.9);
        // The p (voice 1) nearer the staff than the f (voice 3).
        const GlyphItem* p = shared.below[0]->glyph == Glyph::DynamicPiano ? shared.below[0]
                                                                            : shared.below[1];
        const GlyphItem* f = p == shared.below[0] ? shared.below[1] : shared.below[0];
        VCC_CHECK(p->glyph == Glyph::DynamicPiano && f->glyph == Glyph::DynamicForte);
        VCC_CHECK(p->origin.y < f->origin.y);
    }

    const DynamicsAt unshared = dynamics_at(result, timeline.x_for_tick(kBar), metrics);
    VCC_CHECK_EQ(unshared.above.size(), std::size_t{0});
    VCC_CHECK_EQ(unshared.below.size(), std::size_t{2});
    if (unshared.below.size() == 2)
        VCC_CHECK(std::abs(unshared.below[0]->origin.y - unshared.below[1]->origin.y) >= 1.9);
}

VCC_TEST(a_hairpin_follows_its_voice_and_gives_way_only_to_its_own_dynamic)
{
    // In a shared bar the lower voice's hairpin goes below, after its own
    // p; voice 0's, meeting it, goes above - and starts at its tick, since
    // the p it does not give way to is another voice's. A third voice's
    // hairpin below goes a row further out than the lower voice's.
    notation::Score score;
    fill_a_bar(score, 0, 0, notation::Step::G);
    fill_a_bar(score, 0, 1, notation::Step::E);
    fill_a_bar(score, 0, 3, notation::Step::C);
    score.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::Piano, 1, ""});

    notation::Hairpin lower;
    lower.start_tick = 0;
    lower.end_tick = kBar;
    lower.kind = notation::HairpinKind::Crescendo;
    lower.voice = 1;
    score.add_hairpin(lower);
    notation::Hairpin upper = lower;
    upper.voice = 0;
    score.add_hairpin(upper);
    notation::Hairpin third = lower;
    third.voice = 3;
    third.end_tick = (2 * kQuarter);
    score.add_hairpin(third);

    LayoutEngine engine;
    const TimelineMap timeline(8.0);
    const LayoutResult result = engine.layout(score, timeline, 0, kBar);
    const StaffMetrics metrics = metrics_for(result, 0);

    // Each hairpin is two lines meeting at its quiet end, which is its
    // start here: the apex y is the hairpin's baseline.
    std::vector<const LineItem*> above;
    std::vector<const LineItem*> below;
    for (const LineItem* line : lines_with(result.display_list, ItemRole::Hairpin))
        (line->from.y < metrics.top_y ? above : below).push_back(line);
    VCC_CHECK_EQ(above.size(), std::size_t{2});
    VCC_CHECK_EQ(below.size(), std::size_t{4});

    for (const LineItem* line : above)
        VCC_CHECK_NEAR(line->from.x, timeline.x_for_tick(0), 1e-9);

    double nearest = 1e9;
    double furthest = -1e9;
    for (const LineItem* line : below) {
        nearest = std::min(nearest, line->from.y);
        furthest = std::max(furthest, line->from.y);
    }
    VCC_CHECK(furthest - nearest >= 1.9);
    for (const LineItem* line : below) {
        if (std::abs(line->from.y - nearest) < 1e-9)
            VCC_CHECK(line->from.x > timeline.x_for_tick(0) + 0.5);  // Voice 1's, after its p.
        else
            VCC_CHECK_NEAR(line->from.x, timeline.x_for_tick(0), 1e-9);  // Voice 3's.
    }
}

VCC_TEST(a_text_goes_over_a_dynamic_that_a_voice_put_above_the_staff)
{
    // The upper voice's mf goes above the staff; a cue point at the same
    // tick goes over it rather than through it, and a hairpin above is
    // cleared the same way.
    notation::Score score;
    fill_a_bar(score, 0, 0, notation::Step::G);
    fill_a_bar(score, 0, 1, notation::Step::E);
    score.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::MezzoForte, 0, ""});
    score.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::Piano, 1, ""});
    score.add_text(notation::TextMarking{0, 0, "Hit"});
    notation::Hairpin lower;
    lower.start_tick = (2 * kQuarter);
    lower.end_tick = kBar;
    lower.kind = notation::HairpinKind::Crescendo;
    lower.voice = 1;
    score.add_hairpin(lower);
    notation::Hairpin upper = lower;
    upper.voice = 0;
    score.add_hairpin(upper);
    score.add_text(notation::TextMarking{(2 * kQuarter), 0, "Hit"});

    LayoutEngine engine;
    const TimelineMap timeline(8.0);
    const LayoutResult result = engine.layout(score, timeline, 0, kBar);
    const StaffMetrics metrics = metrics_for(result, 0);

    const std::vector<const TextItem*> texts = texts_with(result.display_list, ItemRole::Text);
    VCC_CHECK_EQ(texts.size(), std::size_t{2});
    if (texts.size() != 2)
        return;

    const DynamicsAt at_start = dynamics_at(result, timeline.x_for_tick(0), metrics);
    VCC_CHECK_EQ(at_start.above.size(), std::size_t{1});
    for (const GlyphItem* glyph : at_start.above)
        VCC_CHECK(texts[0]->origin.y < glyph->bounds.y);

    double hairpin_top = 1e9;
    for (const LineItem* line : lines_with(result.display_list, ItemRole::Hairpin)) {
        if (line->from.y < metrics.top_y)
            hairpin_top = std::min(hairpin_top, std::min(line->from.y, line->to.y));
    }
    VCC_CHECK(hairpin_top < metrics.top_y);
    VCC_CHECK(texts[1]->origin.y < hairpin_top);
}

VCC_TEST(a_slur_from_a_high_note_to_a_low_one_keeps_a_level_crown_and_tapers)
{
    // A slur over a drop of a twelfth, between whole notes so no stem
    // moves an end: both control points at one height, beyond the higher
    // end, so the slur arches over the drop rather than leaning down it -
    // a crown beyond the lower end would sit below the higher note. And
    // an engraver's curve, thin at the ends and full in the middle, which
    // the renderers draw as a filled lens.
    notation::Score score;
    score.insert_note(0, note_at(0, notation::whole(), notation::Pitch{notation::Step::A, 5, 0}));
    score.insert_note(0, note_at(kBar, notation::whole(), notation::Pitch{notation::Step::C, 4, 0}));
    notation::Slur slur;
    slur.start_tick = 0;
    slur.end_tick = 2 * kBar;
    score.add_slur(slur);

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, 2 * kBar);
    const CurveItem* drawn = nullptr;
    for (const CurveItem& curve : result.display_list.curves) {
        if (curve.role == ItemRole::Slur)
            drawn = &curve;
    }
    VCC_CHECK(drawn != nullptr);
    if (drawn == nullptr)
        return;

    VCC_CHECK(drawn->end.y > drawn->start.y + 4.0);  // It is a drop.
    VCC_CHECK_NEAR(drawn->control_one.y, drawn->control_two.y, 1e-9);
    VCC_CHECK(drawn->control_one.y < std::min(drawn->start.y, drawn->end.y));
    VCC_CHECK(drawn->tapered);
    VCC_CHECK(drawn->end_thickness < drawn->thickness);

    // A tie is tapered too, and still arches between its two ends.
    notation::Score tied;
    notation::ScoreNote first =
        note_at(0, notation::quarter(), notation::Pitch{notation::Step::C, 5, 0});
    first.tied_to_next = true;
    tied.insert_note(0, first);
    tied.insert_note(0, note_at(kQuarter, notation::quarter(), notation::Pitch{notation::Step::C, 5, 0}));
    const LayoutResult with_tie = engine.layout(tied, TimelineMap(8.0), 0, kBar);
    std::size_t ties = 0;
    for (const CurveItem& curve : with_tie.display_list.curves) {
        if (curve.role != ItemRole::Tie)
            continue;
        ++ties;
        VCC_CHECK(curve.tapered);
    }
    VCC_CHECK_EQ(ties, std::size_t{1});
}

VCC_TEST(a_marcato_sits_above_the_note_whichever_way_the_stem_points)
{
    // The strong accent goes over the note by convention. G4 stems up, so
    // the other articulations go below - and the marcato still goes above.
    notation::Score score;
    notation::ScoreNote note =
        note_at(0, notation::quarter(), notation::Pitch{notation::Step::G, 4, 0});
    note.articulations = static_cast<notation::ArticulationMask>(notation::Articulation::Marcato)
                       | static_cast<notation::ArticulationMask>(notation::Articulation::Staccato);
    score.insert_note(0, note);

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);
    const StaffMetrics metrics = metrics_for(result, 0);
    VCC_CHECK(result.notes.front().stem_up);

    bool marcato_above = false;
    bool staccato_below = false;
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role != ItemRole::Articulation)
            continue;
        if (item.glyph == Glyph::ArticMarcatoAbove)
            marcato_above = item.origin.y < metrics.top_y;
        if (item.glyph == Glyph::ArticStaccatoBelow)
            staccato_below = item.origin.y > metrics.bottom_y();
    }
    VCC_CHECK(marcato_above);
    VCC_CHECK(staccato_below);
}

VCC_TEST(an_articulation_on_a_note_inside_the_staff_sits_just_outside_it)
{
    // Three quarters of a space beyond the staff line, rather than a whole
    // space and more: a mark further out read as belonging to nothing.
    notation::Score score;
    notation::ScoreNote note =
        note_at(0, notation::quarter(), notation::Pitch{notation::Step::D, 5, 0});  // Stem down.
    note.articulations = static_cast<notation::ArticulationMask>(notation::Articulation::Accent);
    score.insert_note(0, note);

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);
    const StaffMetrics metrics = metrics_for(result, 0);
    VCC_CHECK(!result.notes.front().stem_up);

    const GlyphItem* accent = find_glyph(result.display_list, ItemRole::Articulation);
    VCC_CHECK(accent != nullptr);
    if (accent != nullptr)
        VCC_CHECK_NEAR(accent->origin.y, metrics.top_y - 0.75, 1e-9);
}

VCC_TEST(a_marking_is_drawn_once_however_many_staves_there_are)
{
    // Dynamics and hairpins belong to the score, not to a staff. Laying out
    // a grand staff used to print each one twice, which is what this pins.
    notation::Score score;
    score.make_grand_staff();
    score.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::Forte, 0, ""});
    notation::Hairpin hairpin;
    hairpin.start_tick = 0;
    hairpin.end_tick = (2 * kQuarter);
    score.add_hairpin(hairpin);

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, kBar);

    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Dynamic), std::size_t{1});
    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::Hairpin), std::size_t{2});

    // Between the staves, as a piano part has them: below the upper staff
    // and above the lower, whatever the placement option says.
    const StaffMetrics upper = metrics_for(result, 0);
    const StaffMetrics lower = metrics_for(result, 1);
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role == ItemRole::Dynamic) {
            VCC_CHECK(item.origin.y > upper.bottom_y());
            VCC_CHECK(item.origin.y < lower.top_y);
        }
    }
    for (const LineItem* line : lines_with(result.display_list, ItemRole::Hairpin)) {
        VCC_CHECK(line->from.y > upper.bottom_y());
        VCC_CHECK(line->from.y < lower.top_y);
    }
}

VCC_TEST(markings_clear_low_notes_and_articulations)
{
    // A dynamic at a fixed offset below the staff lands on top of a note
    // that is already down there, or on its staccato dot.
    notation::Score plain;
    plain.insert_note(0, note_at(0, notation::quarter(),
                                 notation::Pitch{notation::Step::B, 4, 0}));
    plain.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::Forte, 0, ""});

    notation::Score low;
    notation::ScoreNote deep = note_at(0, notation::quarter(),
                                       notation::Pitch{notation::Step::C, 3, 0});
    deep.articulations = notation::to_mask(notation::Articulation::Staccato);
    low.insert_note(0, deep);
    low.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::Forte, 0, ""});

    LayoutEngine engine;
    const LayoutResult high_result = engine.layout(plain, TimelineMap(6.0), 0, kBar);
    const LayoutResult low_result = engine.layout(low, TimelineMap(6.0), 0, kBar);

    const GlyphItem* high = find_glyph(high_result.display_list, ItemRole::Dynamic);
    const GlyphItem* deep_dynamic = find_glyph(low_result.display_list, ItemRole::Dynamic);
    VCC_CHECK(high != nullptr);
    VCC_CHECK(deep_dynamic != nullptr);

    // The low note pushes the dynamic further down, clear of the note and
    // of the staccato dot under it.
    VCC_CHECK(deep_dynamic->origin.y > high->origin.y + 1.0);
    for (const GlyphItem& item : low_result.display_list.glyphs) {
        if (item.role == ItemRole::Articulation)
            VCC_CHECK(deep_dynamic->bounds.y > item.bounds.bottom());
    }
    for (const LaidOutNote& note : low_result.notes)
        VCC_CHECK(deep_dynamic->bounds.y > note.notehead_bounds.bottom());
}

VCC_TEST(markings_move_above_the_top_staff_when_asked)
{
    // On one staff the placement option decides.
    notation::Score score;
    score.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::Forte, 0, ""});

    LayoutOptions options;
    options.dynamics_placement = Placement::Above;
    LayoutEngine engine(GlyphTable{}, options);
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, kBar);

    const StaffMetrics upper = metrics_for(result, 0);
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Dynamic), std::size_t{1});
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role == ItemRole::Dynamic)
            VCC_CHECK(item.origin.y < upper.top_y);
    }

    // On a grand staff it does not: the markings go between the staves.
    notation::Score grand;
    grand.make_grand_staff();
    grand.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::Forte, 0, ""});
    const LayoutResult between = engine.layout(grand, TimelineMap(6.0), 0, kBar);
    const StaffMetrics top = metrics_for(between, 0);
    const StaffMetrics bottom = metrics_for(between, 1);
    for (const GlyphItem& item : between.display_list.glyphs) {
        if (item.role == ItemRole::Dynamic) {
            VCC_CHECK(item.origin.y > top.bottom_y());
            VCC_CHECK(item.origin.y < bottom.top_y);
        }
    }
}

VCC_TEST(each_staff_of_a_grand_staff_draws_its_own_markings_between_the_staves)
{
    // A marking on the lower staff goes above it - between the staves too,
    // from the other side - and clears the lower staff's own high notes;
    // one on the upper staff goes below the upper staff and clears a note
    // hanging under it, not a note on the staff below. A marking from a
    // file with a staff this score lacks is drawn against the last staff.
    notation::Score score;
    score.make_grand_staff();
    score.insert_note(0, note_at(0, notation::quarter(), notation::Pitch{notation::Step::A, 3, 0}));
    score.insert_note(1, note_at((2 * kQuarter), notation::quarter(), notation::Pitch{notation::Step::G, 4, 0}));
    notation::DynamicMarking upper{0, notation::DynamicLevel::Forte, 0, ""};
    upper.staff = 0;
    score.add_dynamic(upper);
    notation::DynamicMarking lower{(2 * kQuarter), notation::DynamicLevel::Piano, 0, ""};
    lower.staff = 1;
    score.add_dynamic(lower);
    notation::DynamicMarking stray{(3 * kQuarter), notation::DynamicLevel::Fortissimo, 0, ""};
    stray.staff = 7;
    score.add_dynamic(stray);

    LayoutEngine engine;
    const TimelineMap timeline(6.0);
    const LayoutResult result = engine.layout(score, timeline, 0, kBar);
    const StaffMetrics top = metrics_for(result, 0);
    const StaffMetrics bottom = metrics_for(result, 1);

    const GlyphItem* f = nullptr;
    const GlyphItem* p = nullptr;
    const GlyphItem* ff = nullptr;
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role != ItemRole::Dynamic)
            continue;
        if (std::abs(item.origin.x - timeline.x_for_tick(0)) < 1e-9)
            f = &item;
        else if (std::abs(item.origin.x - timeline.x_for_tick((2 * kQuarter))) < 1e-9)
            p = &item;
        else if (std::abs(item.origin.x - timeline.x_for_tick((3 * kQuarter))) < 1e-9)
            ff = &item;
    }
    VCC_CHECK(f != nullptr && p != nullptr && ff != nullptr);
    if (f == nullptr || p == nullptr || ff == nullptr)
        return;

    // The f: below the upper staff, under the A3 hanging there.
    const LaidOutNote& low_note = result.notes[0];
    VCC_CHECK(f->origin.y > top.bottom_y());
    VCC_CHECK(f->bounds.y > low_note.notehead_bounds.bottom());
    VCC_CHECK(f->origin.y < bottom.top_y);

    // The p: above the lower staff, over the G4 standing there. Each
    // marking sits close under or over its own staff's note - the f is
    // not pushed by the G4, nor the p by the A3 - so at different ticks
    // the two may well cross in height, as they do here.
    const LaidOutNote& high_note = result.notes[1];
    VCC_CHECK(p->origin.y < bottom.top_y);
    VCC_CHECK(p->bounds.bottom() < high_note.notehead_bounds.y);
    VCC_CHECK(p->origin.y > top.bottom_y());
    VCC_CHECK(f->bounds.y < low_note.notehead_bounds.bottom() + 4.0);
    VCC_CHECK(p->bounds.bottom() > high_note.notehead_bounds.y - 4.0);

    // The stray: against the last staff, so above it like the p.
    VCC_CHECK(ff->origin.y < bottom.top_y);
    VCC_CHECK(ff->origin.y > top.bottom_y());
}


namespace {

notation::ScoreNote grace_at(notation::Ticks tick, const notation::Pitch& pitch,
                             std::uint8_t order, bool slashed = true)
{
    notation::ScoreNote note = note_at(tick, notation::eighth(), pitch);
    note.grace = true;
    note.grace_order = order;
    note.grace_slashed = slashed;
    return note;
}

}  // namespace

VCC_TEST(grace_notes_are_drawn_small_before_the_note_they_ornament)
{
    // Two grace notes before an F-sharp on beat 2, the second with an
    // accidental of its own: cue-sized heads left of the principal and
    // its sharp, in order, each with a short up stem, a flag and a slash.
    notation::Score score;
    const notation::EventId first =
        score.insert_note(0, grace_at(kQuarter, notation::Pitch{notation::Step::D, 5, 0}, 0));
    const notation::EventId second =
        score.insert_note(0, grace_at(kQuarter, notation::Pitch{notation::Step::E, 5, -1}, 1));
    const notation::EventId principal = score.insert_note(
        0, note_at(kQuarter, notation::quarter(), notation::Pitch{notation::Step::F, 5, 1}));

    LayoutEngine engine;
    const TimelineMap timeline(8.0);
    const LayoutResult result = engine.layout(score, timeline, 0, kBar);

    const LaidOutNote* first_laid = find_note(result, first);
    const LaidOutNote* second_laid = find_note(result, second);
    const LaidOutNote* principal_laid = find_note(result, principal);
    VCC_CHECK(first_laid != nullptr && second_laid != nullptr && principal_laid != nullptr);
    if (first_laid == nullptr || second_laid == nullptr || principal_laid == nullptr)
        return;

    VCC_CHECK(first_laid->is_grace && second_laid->is_grace && !principal_laid->is_grace);
    VCC_CHECK_EQ(first_laid->end_tick, notation::Ticks{kQuarter});
    VCC_CHECK(!principal_laid->is_grace);

    // Order on the page: first, then second, then the principal's sharp,
    // then the principal; the principal where its tick puts it.
    VCC_CHECK_NEAR(principal_laid->notehead_bounds.x, timeline.x_for_tick(kQuarter), 1e-9);
    VCC_CHECK(first_laid->notehead_bounds.x + first_laid->notehead_bounds.width
              < second_laid->notehead_bounds.x);
    const GlyphItem* principal_sharp = nullptr;
    const GlyphItem* grace_flat = nullptr;
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role != ItemRole::Accidental)
            continue;
        if (item.source_id == principal)
            principal_sharp = &item;
        if (item.source_id == second)
            grace_flat = &item;
    }
    VCC_CHECK(principal_sharp != nullptr && grace_flat != nullptr);
    if (principal_sharp != nullptr) {
        VCC_CHECK(second_laid->notehead_bounds.x + second_laid->notehead_bounds.width
                  < principal_sharp->bounds.x);
        VCC_CHECK_NEAR(principal_sharp->scale, 1.0, 1e-9);
    }
    if (grace_flat != nullptr) {
        VCC_CHECK_NEAR(grace_flat->scale, 0.6, 1e-9);
        VCC_CHECK(grace_flat->bounds.x < second_laid->notehead_bounds.x);
        VCC_CHECK(grace_flat->bounds.x
                  > first_laid->notehead_bounds.x + first_laid->notehead_bounds.width);
    }

    // Small heads, small flags; the principal full size.
    std::size_t small_heads = 0;
    std::size_t small_flags = 0;
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role == ItemRole::Notehead && item.scale < 1.0)
            ++small_heads;
        if (item.role == ItemRole::Flag && item.scale < 1.0)
            ++small_flags;
        if (item.source_id == principal)
            VCC_CHECK_NEAR(item.scale, 1.0, 1e-9);
    }
    VCC_CHECK_EQ(small_heads, std::size_t{2});
    VCC_CHECK_EQ(small_flags, std::size_t{2});
    VCC_CHECK(first_laid->notehead_bounds.width < principal_laid->notehead_bounds.width);

    // Stems up, shorter than the principal's, and a slash through each.
    VCC_CHECK(first_laid->stem_up && second_laid->stem_up);
    VCC_CHECK(first_laid->has_stem);
    VCC_CHECK(first_laid->centre.y - first_laid->stem_end_y
              < std::abs(principal_laid->centre.y - principal_laid->stem_end_y));
    std::size_t slashes = 0;
    for (const LineItem* line : lines_with(result.display_list, ItemRole::GraceSlash)) {
        ++slashes;
        VCC_CHECK(line->source_id == first || line->source_id == second);
        VCC_CHECK(line->from.y > line->to.y);  // Rising to the right.
        VCC_CHECK(line->from.x < line->to.x);
    }
    VCC_CHECK_EQ(slashes, std::size_t{2});
    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::Beam), std::size_t{0});
}

VCC_TEST(a_grace_chord_is_one_small_chord_with_one_stem)
{
    // Two grace notes of one order before a quarter: two small heads in
    // one column, one stem, one flag, one slash.
    notation::Score score;
    notation::ScoreNote lower = grace_at(kQuarter, notation::Pitch{notation::Step::C, 5, 0}, 0);
    lower.in_chord = true;
    notation::ScoreNote upper = grace_at(kQuarter, notation::Pitch{notation::Step::E, 5, 0}, 0);
    upper.in_chord = true;
    const notation::EventId low = score.insert_note(0, lower);
    const notation::EventId high = score.insert_note(0, upper);
    score.insert_note(0, note_at(kQuarter, notation::quarter(), notation::Pitch{notation::Step::G, 5, 0}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);
    const LaidOutNote* a = find_note(result, low);
    const LaidOutNote* b = find_note(result, high);
    VCC_CHECK(a != nullptr && b != nullptr);
    if (a == nullptr || b == nullptr)
        return;
    VCC_CHECK(a->is_grace && b->is_grace);
    VCC_CHECK_NEAR(a->notehead_bounds.x, b->notehead_bounds.x, 1e-9);
    VCC_CHECK_NEAR(a->stem_x, b->stem_x, 1e-9);
    VCC_CHECK_NEAR(a->stem_end_y, b->stem_end_y, 1e-9);
    std::size_t small_flags = 0;
    for (const GlyphItem& item : result.display_list.glyphs)
        small_flags += (item.role == ItemRole::Flag && item.scale < 1.0) ? 1 : 0;
    VCC_CHECK_EQ(small_flags, std::size_t{1});
    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::GraceSlash), std::size_t{1});
}

VCC_TEST(an_unslashed_grace_note_has_no_slash_and_grace_notes_do_not_break_a_beam)
{
    // An appoggiatura is written without the slash. And a grace note
    // between two beamed eighths is neither in the beam nor a break in it.
    // Three sixteenths in one beat, the grace note before the second: in
    // chord order the grace chord sits between the second and the third.
    notation::Score score;
    score.insert_note(0, note_at(0, notation::sixteenth(), notation::Pitch{notation::Step::G, 4, 0}));
    score.insert_note(0, grace_at((kQuarter / 4), notation::Pitch{notation::Step::B, 4, 0}, 0, false));
    score.insert_note(0, note_at((kQuarter / 4), notation::sixteenth(), notation::Pitch{notation::Step::A, 4, 0}));
    score.insert_note(0, note_at((kQuarter / 2), notation::sixteenth(), notation::Pitch{notation::Step::B, 4, 0}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::GraceSlash), std::size_t{0});
    // The three sixteenths still beam as one group - the grace chord is
    // neither a member nor a break - so the only flag on the page is the
    // grace note's.
    VCC_CHECK(result.display_list.quads.size() >= 1);
    std::size_t small_flags = 0;
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role == ItemRole::Flag && item.scale < 1.0)
            ++small_flags;
    }
    VCC_CHECK_EQ(small_flags, std::size_t{1});
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Flag), std::size_t{1});

    // A grace note with nothing after it yet - the composer is still
    // typing - hangs before its tick.
    notation::Score alone;
    const notation::EventId id =
        alone.insert_note(0, grace_at(kQuarter, notation::Pitch{notation::Step::C, 5, 0}, 0));
    const TimelineMap timeline(8.0);
    const LayoutResult hanging = engine.layout(alone, timeline, 0, kBar);
    const LaidOutNote* laid = find_note(hanging, id);
    VCC_CHECK(laid != nullptr);
    if (laid != nullptr)
        VCC_CHECK(laid->notehead_bounds.x + laid->notehead_bounds.width < timeline.x_for_tick(kQuarter));
}

VCC_TEST(a_barline_moves_left_of_the_grace_notes_at_its_tick)
{
    // Grace notes belong after the barline, with the note they ornament,
    // and the timeline puts that note at the barline: so the barline
    // stands left of them. A barline with no grace notes stays at its
    // tick.
    notation::Score score;
    score.insert_note(0, note_at(0, notation::whole(), notation::Pitch{notation::Step::C, 5, 0}));
    const notation::EventId grace =
        score.insert_note(0, grace_at(kBar, notation::Pitch{notation::Step::B, 4, 0}, 0));
    score.insert_note(0, note_at(kBar, notation::quarter(), notation::Pitch{notation::Step::C, 5, 0}));

    LayoutEngine engine;
    const TimelineMap timeline(8.0);
    const LayoutResult result = engine.layout(score, timeline, 0, 3 * kBar);

    const LaidOutNote* laid = find_note(result, grace);
    VCC_CHECK(laid != nullptr);
    const std::vector<const LineItem*> barlines = lines_with(result.display_list, ItemRole::Barline);
    VCC_CHECK_EQ(barlines.size(), std::size_t{3});
    if (laid == nullptr || barlines.size() != 3)
        return;
    // The barline at kBar has moved left of the grace note, by less than
    // a few spaces; the ones at 0 and 2 bars stand at their ticks.
    bool at_start = false;
    bool at_two_bars = false;
    double moved = -1e9;
    for (const LineItem* line : barlines) {
        const double x = line->from.x;
        if (std::abs(x - timeline.x_for_tick(0)) < 1e-9)
            at_start = true;
        else if (std::abs(x - timeline.x_for_tick(2 * kBar)) < 1e-9)
            at_two_bars = true;
        else
            moved = x;
    }
    VCC_CHECK(at_start && at_two_bars);
    VCC_CHECK(moved < laid->notehead_bounds.x);
    VCC_CHECK(moved < timeline.x_for_tick(kBar));
    VCC_CHECK(moved > timeline.x_for_tick(kBar) - 4.0);
}

VCC_TEST(a_grace_note_inside_a_triplet_leaves_the_triplet_whole)
{
    // The triplet's three eighths are still one group under one number
    // with a grace note before the second: a grace chord is neither one
    // of the tuplet's events nor a gap between two of them.
    notation::Score score;
    const notation::Pitch pitch{notation::Step::G, 4, 0};
    const notation::Duration eighth = notation::triplet(notation::NoteValue::Eighth);
    for (int index = 0; index < 3; ++index)
        score.insert_note(0, note_at(static_cast<notation::Ticks>(index) * (kQuarter / 3), eighth, pitch));
    score.insert_note(0, grace_at((kQuarter / 3), notation::Pitch{notation::Step::A, 4, 0}, 0));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(12.0), 0, kBar);

    const std::vector<const TextItem*> numbers =
        texts_with(result.display_list, ItemRole::TupletBracket);
    VCC_CHECK_EQ(numbers.size(), std::size_t{1});
    VCC_CHECK_EQ(result.display_list.quads.size(), std::size_t{1});
}

VCC_TEST(a_note_crossed_to_the_staff_below_is_drawn_there_and_reports_its_own_staff)
{
    // A C4 on the treble staff of a grand staff, crossed to the bass
    // staff: drawn by the bass clef's positions - a C4 sits just above
    // the bass staff - below the treble staff's bottom line, and still
    // the treble staff's note to the view.
    notation::Score score;
    score.make_grand_staff();
    notation::ScoreNote note =
        note_at(0, notation::quarter(), notation::Pitch{notation::Step::C, 4, 0});
    note.staff_shift = 1;
    const notation::EventId id = score.insert_note(0, note);
    notation::ScoreNote plain =
        note_at(kQuarter, notation::quarter(), notation::Pitch{notation::Step::C, 4, 0});
    const notation::EventId plain_id = score.insert_note(0, plain);

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);
    const StaffMetrics treble = metrics_for(result, 0);
    const StaffMetrics bass = metrics_for(result, 1);

    const LaidOutNote* crossed = find_note(result, id);
    const LaidOutNote* home = find_note(result, plain_id);
    VCC_CHECK(crossed != nullptr && home != nullptr);
    if (crossed == nullptr || home == nullptr)
        return;
    VCC_CHECK_EQ(crossed->staff_index, std::size_t{0});
    VCC_CHECK(crossed->centre.y > treble.bottom_y());
    VCC_CHECK(crossed->centre.y < bass.top_y);  // Just above the bass staff.
    VCC_CHECK_EQ(crossed->staff_position,
                 notation::staff_position(notation::Pitch{notation::Step::C, 4, 0},
                                          notation::Clef::Bass));
    VCC_CHECK(crossed->staff_position
              != notation::staff_position(notation::Pitch{notation::Step::C, 4, 0},
                                          notation::Clef::Treble));
    // A C4 in treble clef hangs one ledger line below the staff; the
    // crossed one is further down still, by the bass staff.
    VCC_CHECK(home->centre.y > treble.bottom_y());
    VCC_CHECK(home->centre.y < crossed->centre.y - 2.0);

    // Crossing where there is no staff draws the note where it belongs.
    notation::Score single;
    const notation::EventId lone = single.insert_note(0, note);
    const LayoutResult alone = engine.layout(single, TimelineMap(8.0), 0, kBar);
    const LaidOutNote* laid = find_note(alone, lone);
    VCC_CHECK(laid != nullptr);
    if (laid != nullptr)
        VCC_CHECK_NEAR(laid->centre.y, home->centre.y, 1e-9);
}

VCC_TEST(a_beam_across_the_staves_runs_between_them_with_stems_from_both_sides)
{
    // Four beamed sixteenths in one beat on the treble staff, the first
    // two crossed to the bass staff: flat beams in the gap between the
    // staves, the bass-drawn notes stemming up to them and the
    // treble-drawn notes down.
    notation::Score score;
    score.make_grand_staff();
    std::vector<notation::EventId> ids;
    for (int index = 0; index < 4; ++index) {
        notation::ScoreNote note = note_at(static_cast<notation::Ticks>(index) * (kQuarter / 4),
                                           notation::sixteenth(),
                                           notation::Pitch{index < 2 ? notation::Step::A : notation::Step::C,
                                                           static_cast<std::int8_t>(index < 2 ? 3 : 5), 0});
        note.staff_shift = index < 2 ? 1 : 0;
        ids.push_back(score.insert_note(0, note));
    }

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);
    const StaffMetrics treble = metrics_for(result, 0);
    const StaffMetrics bass = metrics_for(result, 1);

    // Two beams - sixteenths - both flat, both in the gap.
    VCC_CHECK_EQ(result.display_list.quads.size(), std::size_t{2});
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Flag), std::size_t{0});
    if (result.display_list.quads.empty())
        return;
    double top = 1e9;
    double bottom = -1e9;
    for (const QuadItem& beam : result.display_list.quads) {
        for (const Point& corner : beam.corners) {
            top = std::min(top, corner.y);
            bottom = std::max(bottom, corner.y);
        }
        VCC_CHECK_NEAR(beam.corners[0].y, beam.corners[1].y, 1e-9);  // Flat.
    }
    VCC_CHECK(top > treble.bottom_y());
    VCC_CHECK(bottom < bass.top_y);

    for (std::size_t index = 0; index < ids.size(); ++index) {
        const LaidOutNote* laid = find_note(result, ids[index]);
        VCC_CHECK(laid != nullptr);
        if (laid == nullptr)
            continue;
        VCC_CHECK_EQ(laid->staff_index, std::size_t{0});
        if (index < 2) {
            // Drawn by the bass staff - an A3 sits on its top line - and
            // stemming up to the beam above it.
            VCC_CHECK(laid->centre.y >= bass.top_y - 1.0 - 1e-9);
            VCC_CHECK(laid->centre.y > bottom);
            VCC_CHECK(laid->stem_up);
        } else {
            // Drawn in the treble staff, stemming down to the beam below.
            VCC_CHECK(laid->centre.y < treble.bottom_y());
            VCC_CHECK(laid->centre.y < top);
            VCC_CHECK(!laid->stem_up);
        }
        VCC_CHECK(laid->stem_end_y >= top - 1e-9 && laid->stem_end_y <= bottom + 1e-9);
    }
}

VCC_TEST(articulations_go_on_the_side_away_from_the_stem)
{
    notation::Score score;
    notation::ScoreNote note = note_at(0, notation::quarter(),
                                      notation::Pitch{notation::Step::G, 4, 0});
    note.articulations = notation::to_mask(notation::Articulation::Staccato)
                       | notation::to_mask(notation::Articulation::Accent);
    score.insert_note(0, note);

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Articulation), std::size_t{2});

    // G4 takes an up stem, so the marks go below the notehead and use the
    // "below" glyph forms.
    const LaidOutNote& laid = result.notes.front();
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role != ItemRole::Articulation)
            continue;
        VCC_CHECK(item.origin.y > laid.centre.y);
        VCC_CHECK(item.glyph == Glyph::ArticStaccatoBelow
                  || item.glyph == Glyph::ArticAccentBelow);
    }
}

// --- Structure --------------------------------------------------------------

VCC_TEST(a_grand_staff_stacks_and_is_joined_at_the_left)
{
    notation::Score score;
    score.make_grand_staff();

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, kBar);

    VCC_CHECK_EQ(result.staff_top_y.size(), std::size_t{2});
    VCC_CHECK_NEAR(result.staff_top_y[1] - result.staff_top_y[0],
                   4.0 + engine.options().staff_gap, 1e-9);

    // Five lines per staff.
    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::StaffLine), std::size_t{10});

    // A treble clef above, a bass clef below.
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Clef), std::size_t{2});
    bool have_treble = false;
    bool have_bass = false;
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role != ItemRole::Clef)
            continue;
        have_treble = have_treble || item.glyph == Glyph::GClef;
        have_bass = have_bass || item.glyph == Glyph::FClef;
    }
    VCC_CHECK(have_treble);
    VCC_CHECK(have_bass);

    // The barline at bar 1 plus the system barline joining the staves.
    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::Barline), std::size_t{2});
}

VCC_TEST(the_header_widens_for_a_key_signature_that_needs_it)
{
    notation::Score plain;
    notation::Score seven_flats;
    VCC_CHECK(seven_flats.set_key_signature(0, notation::KeySignature{-7, false}));

    LayoutEngine engine;
    const LayoutResult narrow = engine.layout(plain, TimelineMap(6.0), 0, kBar);
    const LayoutResult wide = engine.layout(seven_flats, TimelineMap(6.0), 0, kBar);

    // With nothing to draw, the configured header width is enough.
    VCC_CHECK_NEAR(narrow.extent.x, -engine.options().header_width, 1e-9);

    // Seven flats need more, and the header takes it rather than letting the
    // signature collide with the first note.
    VCC_CHECK(wide.extent.x < narrow.extent.x - 2.0);
    VCC_CHECK_EQ(count_glyphs(wide.display_list, ItemRole::KeySignature), std::size_t{7});

    // The music still starts at the tick the timeline puts it at.
    VCC_CHECK_NEAR(wide.extent.right(), narrow.extent.right(), 1e-9);
}

VCC_TEST(the_time_signature_is_drawn_as_stacked_digits)
{
    notation::Score score;
    VCC_CHECK(score.set_time_signature(0, vcc::music::TimeSignature{12, 8}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, (6 * kQuarter));

    // "12" over "8": three digits.
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::TimeSignature), std::size_t{3});

    const StaffMetrics metrics = metrics_for(result, 0);
    std::size_t upper = 0;
    std::size_t lower = 0;
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role != ItemRole::TimeSignature)
            continue;
        if (item.origin.y < metrics.line_y(2))
            ++upper;
        else
            ++lower;
    }
    VCC_CHECK_EQ(upper, std::size_t{2});
    VCC_CHECK_EQ(lower, std::size_t{1});
}

VCC_TEST(only_the_visible_range_is_laid_out)
{
    notation::Score score;
    for (int bar = 0; bar < 100; ++bar) {
        score.insert_note(0, note_at(static_cast<notation::Ticks>(bar) * kBar,
                                     notation::quarter(),
                                     notation::Pitch{notation::Step::G, 4, 0}));
    }

    LayoutEngine engine;
    const LayoutResult two_bars = engine.layout(score, TimelineMap(6.0), 0, 2 * kBar);
    const LayoutResult whole = engine.layout(score, TimelineMap(6.0), 0, 100 * kBar);

    VCC_CHECK_EQ(two_bars.notes.size(), std::size_t{2});
    VCC_CHECK_EQ(whole.notes.size(), std::size_t{100});

    // A scrolled view lays out only what it shows, which is what keeps a
    // long score affordable.
    const LayoutResult scrolled = engine.layout(score, TimelineMap(6.0), 50 * kBar, 52 * kBar);
    VCC_CHECK_EQ(scrolled.notes.size(), std::size_t{2});
    VCC_CHECK_EQ(scrolled.notes.front().start_tick, 50 * kBar);
}

VCC_TEST(an_empty_range_or_empty_score_lays_out_nothing)
{
    notation::Score score;
    LayoutEngine engine;

    VCC_CHECK(engine.layout(score, TimelineMap(6.0), 100, 100).display_list.empty());
    VCC_CHECK(engine.layout(score, TimelineMap(6.0), 200, 100).display_list.empty());

    // An empty score still lays out its staff.
    const LayoutResult empty = engine.layout(score, TimelineMap(6.0), 0, kBar);
    VCC_CHECK(empty.notes.empty());
    VCC_CHECK_EQ(count_lines(empty.display_list, ItemRole::StaffLine), std::size_t{5});
}

VCC_TEST(crowding_is_reported_rather_than_re_spaced)
{
    notation::Score score;
    for (int index = 0; index < 16; ++index) {
        score.insert_note(0, note_at(static_cast<notation::Ticks>(index) * (kQuarter / 4),
                                     notation::sixteenth(),
                                     notation::Pitch{notation::Step::G, 4, 0}));
    }

    LayoutEngine engine;

    // At a comfortable zoom nothing crowds.
    const LayoutResult roomy = engine.layout(score, TimelineMap(24.0), 0, kBar);
    VCC_CHECK_EQ(roomy.crowded_note_count, std::size_t{0});
    VCC_CHECK(!roomy.display_list.is_crowded);

    // Zoomed out, every note is narrower than its own notehead - and the
    // positions still come straight from the timeline, because re-spacing
    // would break alignment with the host.
    const LayoutResult dense = engine.layout(score, TimelineMap(1.0), 0, kBar);
    VCC_CHECK_EQ(dense.crowded_note_count, std::size_t{16});
    VCC_CHECK(dense.display_list.is_crowded);

    const TimelineMap timeline(1.0);
    for (const LaidOutNote& note : dense.notes)
        VCC_CHECK_NEAR(note.centre.x - 1.18 * 0.5, timeline.x_for_tick(note.start_tick), 1e-9);
}

VCC_TEST(the_cursor_is_drawn_where_it_is_asked_for)
{
    notation::Score score;
    LayoutOptions options;
    options.draw_cursor = true;
    options.cursor_tick = (2 * kQuarter);

    LayoutEngine engine(GlyphTable{}, options);
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, kBar);

    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::Cursor), std::size_t{1});
    for (const LineItem& line : result.display_list.lines) {
        if (line.role == ItemRole::Cursor)
            VCC_CHECK_NEAR(line.from.x, TimelineMap(6.0).x_for_tick((2 * kQuarter)), 1e-9);
    }
}

VCC_TEST(a_font_missing_a_glyph_is_reported_rather_than_drawn_blank)
{
    GlyphTable table;
    GlyphMetrics absent;
    absent.codepoint = 0;  // As a font lacking the glyph would report it.
    table.set_metrics(Glyph::NoteheadBlack, absent);

    Glyph first_missing = Glyph::None;
    VCC_CHECK(!table.is_complete(&first_missing));
    VCC_CHECK_EQ(first_missing, Glyph::NoteheadBlack);

    notation::Score score;
    score.insert_note(0, note_at(0, notation::quarter(), notation::Pitch{notation::Step::G, 4, 0}));

    LayoutEngine engine(std::move(table), LayoutOptions{});
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, kBar);

    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Notehead), std::size_t{0});
    VCC_CHECK_EQ(result.display_list.missing_glyphs.size(), std::size_t{1});
    VCC_CHECK_EQ(result.display_list.missing_glyphs.front(), Glyph::NoteheadBlack);

    // The note is still laid out: its stem is drawn, and it can still be
    // selected and edited.
    VCC_CHECK_EQ(result.notes.size(), std::size_t{1});
    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::Stem), std::size_t{1});
}

VCC_TEST(every_item_belonging_to_a_note_can_be_found_by_its_id)
{
    notation::Score score;
    notation::ScoreNote note = note_at(0, notation::quarter(2),
                                       notation::Pitch{notation::Step::C, 4, 1});
    note.articulations = notation::to_mask(notation::Articulation::Staccato);
    const notation::EventId id = score.insert_note(0, note);

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    // Notehead, accidental, two dots, ledger line, stem and articulation -
    // which is what makes selection highlighting possible.
    const std::vector<Rect> bounds = result.display_list.bounds_for_event(id);
    VCC_CHECK(bounds.size() >= 7);
}

VCC_TEST(options_that_turn_layers_off_are_honoured)
{
    notation::Score score;
    score.insert_note(0, note_at(0, notation::eighth(), notation::Pitch{notation::Step::G, 4, 0}));
    score.insert_note(0, note_at((kQuarter / 2), notation::eighth(), notation::Pitch{notation::Step::G, 4, 0}));
    score.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::Forte, 0, ""});

    LayoutOptions bare;
    bare.draw_header = false;
    bare.draw_barlines = false;
    bare.draw_ruler = false;
    bare.draw_beams = false;
    VCC_CHECK(bare.is_valid());

    LayoutEngine engine(GlyphTable{}, bare);
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Clef), std::size_t{0});
    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::Barline), std::size_t{0});
    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::RulerMark), std::size_t{0});
    VCC_CHECK(result.display_list.quads.empty());

    // Beams off means flags instead, not notes without either.
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Flag), std::size_t{2});

    // And the staff starts at the music rather than reserving a header.
    VCC_CHECK_NEAR(result.extent.x, 0.0, 1e-9);
}

VCC_TEST(a_transposing_staff_displays_what_is_written)
{
    // Spec 5.1: transposition applies when rendering to MIDI, never when
    // displaying. A clarinet part shows the written note.
    notation::Score score;
    score.staff(0).transposition_semitones = -2;

    const notation::EventId id =
        score.insert_note(0, note_at(0, notation::quarter(),
                                     notation::Pitch{notation::Step::C, 5, 0}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, kBar);

    // C5 in treble is the third space, whatever the instrument sounds.
    VCC_CHECK_EQ(find_note(result, id)->staff_position, 5);
}

VCC_TEST(the_extent_covers_the_margins_a_reader_needs)
{
    notation::Score score;
    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, kBar);

    const LayoutOptions& options = engine.options();
    // The page starts at the top: at 0, or a hair above it, since the
    // topmost ruler mark's stroke straddles y = 0.
    VCC_CHECK(result.extent.y <= 0.0);
    VCC_CHECK(result.extent.y > -0.5);
    VCC_CHECK_NEAR(result.staff_top_y.front(), options.ruler_height + options.margin_above, 1e-9);
    VCC_CHECK_NEAR(result.extent.bottom(),
                   result.staff_top_y.front() + 4.0 + options.margin_below, 1e-9);
    VCC_CHECK_NEAR(result.extent.right(), TimelineMap(6.0).x_for_tick(kBar), 1e-9);
}

VCC_TEST(the_drawn_bounds_contain_whatever_fell_outside_the_page)
{
    // A renderer sizing an image uses the drawn bounds, so an item beyond
    // the margins widens those rather than being quietly clipped.
    notation::Score score;
    score.insert_note(0, note_at(0, notation::quarter(),
                                 notation::Pitch{notation::Step::C, 1, 0}));
    score.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::Forte, 0, ""});

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, kBar);

    const LayoutOptions& options = engine.options();
    const double margin_bottom = result.staff_top_y.front() + 4.0 + options.margin_below;
    VCC_CHECK(result.drawn_bounds.bottom() > margin_bottom);

    for (const GlyphItem& item : result.display_list.glyphs)
        VCC_CHECK(result.drawn_bounds.bottom() >= item.bounds.bottom() - 1e-9);
    for (const LineItem& item : result.display_list.lines) {
        VCC_CHECK(result.drawn_bounds.bottom() >= std::max(item.from.y, item.to.y) - 1e-9);
        VCC_CHECK(result.drawn_bounds.x <= std::min(item.from.x, item.to.x) + 1e-9);
    }

    // And the page itself is exactly what the options imply, whatever was
    // drawn - the number a renderer positions from must not move.
    VCC_CHECK_NEAR(result.extent.bottom(), margin_bottom, 1e-9);
    VCC_CHECK_NEAR(result.extent.x, -options.header_width, 1e-9);
}

VCC_TEST(the_page_does_not_move_when_something_is_drawn_off_it)
{
    // The bug this pins: the edit cursor is drawn at its own tick, so after
    // scrolling it sat far outside the view. With the page taking its left
    // edge from whatever was drawn, that pushed the entire score sideways.
    notation::Score score;
    for (int bar = 0; bar < 8; ++bar) {
        score.insert_note(0, note_at(static_cast<notation::Ticks>(bar) * kBar,
                                     notation::quarter(),
                                     notation::Pitch{notation::Step::G, 4, 0}));
    }

    LayoutOptions options;
    options.draw_cursor = true;
    options.cursor_tick = 0;  // Left behind at the start of the score.
    LayoutEngine engine(GlyphTable{}, options);

    TimelineMap timeline(6.0);
    const LayoutResult at_start = engine.layout(score, timeline, 0, kBar);
    VCC_CHECK_EQ(count_lines(at_start.display_list, ItemRole::Cursor), std::size_t{1});

    // Scroll four bars along. The cursor is now off-screen.
    timeline.set_origin_tick(4 * kBar);
    const LayoutResult scrolled = engine.layout(score, timeline, 4 * kBar, 5 * kBar);

    // It is not drawn, because it is not in view.
    VCC_CHECK_EQ(count_lines(scrolled.display_list, ItemRole::Cursor), std::size_t{0});

    // And the page is in the same place relative to the music as before: the
    // header to the left of the first visible tick, nothing else.
    VCC_CHECK_NEAR(scrolled.extent.x, at_start.extent.x, 1e-9);
    VCC_CHECK_NEAR(scrolled.extent.width, at_start.extent.width, 1e-9);
}

// --- Voices -----------------------------------------------------------------

namespace {

notation::ScoreNote voiced(notation::Ticks tick, const notation::Duration& duration,
                           const notation::Pitch& pitch, std::uint8_t voice)
{
    notation::ScoreNote note = note_at(tick, duration, pitch);
    note.voice = voice;
    return note;
}

notation::ScoreNote voiced_rest(notation::Ticks tick, const notation::Duration& duration,
                                std::uint8_t voice)
{
    notation::ScoreNote note = rest_at(tick, duration);
    note.voice = voice;
    return note;
}

}  // namespace

VCC_TEST(two_voices_in_a_measure_take_stems_up_and_down_by_voice)
{
    notation::Score score;
    // Both lines sit below the middle line, so by pitch alone every stem
    // would go up and the two voices would be indistinguishable.
    const notation::EventId soprano =
        score.insert_note(0, voiced(0, notation::quarter(), {notation::Step::G, 4, 0}, 0));
    const notation::EventId alto =
        score.insert_note(0, voiced(0, notation::quarter(), {notation::Step::C, 4, 0}, 1));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    VCC_CHECK(find_note(result, soprano)->stem_up);
    VCC_CHECK(!find_note(result, alto)->stem_up);
    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::Stem), std::size_t{2});
    VCC_CHECK_EQ(find_note(result, soprano)->voice, std::uint8_t{0});
    VCC_CHECK_EQ(find_note(result, alto)->voice, std::uint8_t{1});
}

VCC_TEST(the_third_and_fourth_voices_take_stems_up_and_down_as_well)
{
    VCC_CHECK(stem_up_for_voice(0));
    VCC_CHECK(!stem_up_for_voice(1));
    VCC_CHECK(stem_up_for_voice(2));
    VCC_CHECK(!stem_up_for_voice(3));
}

VCC_TEST(a_measure_with_one_voice_keeps_the_stems_its_pitches_want)
{
    notation::Score score;
    // Bar one has two voices; bar two has the first voice alone, high up.
    score.insert_note(0, voiced(0, notation::quarter(), {notation::Step::G, 4, 0}, 0));
    score.insert_note(0, voiced(0, notation::quarter(), {notation::Step::C, 4, 0}, 1));
    const notation::EventId alone =
        score.insert_note(0, voiced(kBar, notation::quarter(), {notation::Step::B, 5, 0}, 0));

    // And a lone second voice in bar three, low down, which is a part
    // written entirely in voice 2 - and is engraved like any other.
    const notation::EventId low =
        score.insert_note(0, voiced(2 * kBar, notation::quarter(), {notation::Step::C, 4, 0}, 1));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, 3 * kBar);

    // A high note alone takes a down stem whatever voice it is in, and a
    // low one an up stem: the voice rule applies only where there is
    // another voice to be told apart from.
    VCC_CHECK(!find_note(result, alone)->stem_up);
    VCC_CHECK(find_note(result, low)->stem_up);

    VCC_CHECK_EQ(measures_with_several_voices(score, score.notes(0)),
                 (std::vector<std::int32_t>{1}));
}

VCC_TEST(a_stem_does_not_flip_as_the_other_voice_scrolls_into_view)
{
    notation::Score score;
    // A high note in the first voice on the first beat; the second voice
    // does not enter until the fourth beat.
    const notation::EventId high =
        score.insert_note(0, voiced(0, notation::quarter(), {notation::Step::B, 5, 0}, 0));
    score.insert_note(0, voiced((3 * kQuarter), notation::quarter(), {notation::Step::C, 4, 0}, 1));

    LayoutEngine engine;

    // With the whole bar in view the high note's stem goes up, as voice
    // one's do when voice two shares the bar.
    const LayoutResult whole_bar = engine.layout(score, TimelineMap(8.0), 0, kBar);
    VCC_CHECK(find_note(whole_bar, high)->stem_up);

    // With only the first two beats in view the answer is the same: which
    // measures are shared is decided over whole measures, not over what
    // happens to be on screen, so the stem does not flip as the view
    // scrolls.
    const LayoutResult half_bar = engine.layout(score, TimelineMap(8.0), 0, (2 * kQuarter));
    VCC_CHECK(find_note(half_bar, high)->stem_up);
}

VCC_TEST(beams_hold_within_a_voice_when_another_voice_shares_the_beat)
{
    notation::Score score;
    for (int index = 0; index < 4; ++index) {
        const notation::Ticks tick = static_cast<notation::Ticks>(index) * (kQuarter / 2);
        score.insert_note(0, voiced(tick, notation::eighth(), {notation::Step::G, 4, 0}, 0));
        score.insert_note(0, voiced(tick, notation::eighth(), {notation::Step::C, 4, 0}, 1));
    }

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    // Two beats of two eighths in each voice: four beams, no flags. The
    // chords are stored tick by tick, so the next chord after a voice-one
    // eighth is the voice-two eighth at the same tick, and a beam that
    // broke at a change of voice broke every beam on a two-voice staff.
    VCC_CHECK_EQ(result.display_list.quads.size(), std::size_t{4});
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Flag), std::size_t{0});
    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::Stem), std::size_t{8});

    // The upper voice's beams are above its notes and the lower voice's
    // below, following the stems.
    const double middle = metrics_for(result, 0).line_y(2);
    std::size_t above = 0;
    std::size_t below = 0;
    for (const QuadItem& quad : result.display_list.quads) {
        if (quad.corners[0].y < middle)
            ++above;
        else
            ++below;
    }
    VCC_CHECK_EQ(above, std::size_t{2});
    VCC_CHECK_EQ(below, std::size_t{2});
}

VCC_TEST(rests_move_apart_when_voices_share_a_measure)
{
    notation::Score score;
    const notation::EventId upper = score.insert_note(0, voiced_rest(0, notation::quarter(), 0));
    const notation::EventId lower = score.insert_note(0, voiced_rest(0, notation::quarter(), 1));
    score.insert_note(0, voiced(kQuarter, notation::quarter(), {notation::Step::G, 4, 0}, 0));
    score.insert_note(0, voiced(kQuarter, notation::quarter(), {notation::Step::C, 4, 0}, 1));

    // A rest alone in the next bar stays where a rest goes.
    const notation::EventId alone = score.insert_note(0, voiced_rest(kBar, notation::quarter(), 0));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, 2 * kBar);

    const StaffMetrics metrics = metrics_for(result, 0);
    const double middle = metrics.line_y(2);

    // The first voice's rest is lifted a space, the second's dropped one,
    // so the two do not print on top of each other.
    VCC_CHECK_NEAR(find_note(result, upper)->centre.y, middle - 1.0, 1e-9);
    VCC_CHECK_NEAR(find_note(result, lower)->centre.y, middle + 1.0, 1e-9);
    VCC_CHECK_NEAR(find_note(result, alone)->centre.y, middle, 1e-9);
}

VCC_TEST(a_rest_moves_further_when_the_other_voice_is_in_its_way)
{
    notation::Score score;
    // The first voice holds the middle line while the second voice rests:
    // a rest dropped one space would still print through the notehead.
    score.insert_note(0, voiced(0, notation::half(), {notation::Step::B, 4, 0}, 0));
    const notation::EventId lower = score.insert_note(0, voiced_rest(0, notation::quarter(), 1));
    const notation::EventId lower_later =
        score.insert_note(0, voiced_rest(kQuarter, notation::quarter(), 1));

    // And the other way about in the next bar: the second voice holds the
    // middle line, the first rests.
    score.insert_note(0, voiced(kBar, notation::half(), {notation::Step::B, 4, 0}, 1));
    const notation::EventId upper =
        score.insert_note(0, voiced_rest(kBar, notation::quarter(), 0));

    // A bar where the other voice is far away: the rest stays a space off.
    score.insert_note(0, voiced(2 * kBar, notation::half(), {notation::Step::A, 5, 0}, 0));
    const notation::EventId clear =
        score.insert_note(0, voiced_rest(2 * kBar, notation::quarter(), 1));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, 3 * kBar);
    const double middle = metrics_for(result, 0).line_y(2);

    // Clear of the head by the rest's own half-height: two spaces below the
    // middle line, which is the bottom line.
    VCC_CHECK(find_note(result, lower)->centre.y >= middle + 2.0 - 1e-9);
    // The held note is still sounding under the second rest, so it moves too.
    VCC_CHECK(find_note(result, lower_later)->centre.y >= middle + 2.0 - 1e-9);
    VCC_CHECK(find_note(result, upper)->centre.y <= middle - 2.0 + 1e-9);
    VCC_CHECK_NEAR(find_note(result, clear)->centre.y, middle + 1.0, 1e-9);
}

VCC_TEST(a_second_voice_close_to_the_first_moves_its_head_aside)
{
    notation::Score score;
    // A unison of two values: two heads in one place would read as one
    // note, and one head cannot be both a half and a quarter.
    const notation::EventId upper_unison =
        score.insert_note(0, voiced(0, notation::half(), {notation::Step::G, 4, 0}, 0));
    const notation::EventId lower_unison =
        score.insert_note(0, voiced(0, notation::quarter(), {notation::Step::G, 4, 0}, 1));

    // A second: the heads would overlap.
    const notation::EventId upper_second =
        score.insert_note(0, voiced(kQuarter, notation::quarter(), {notation::Step::G, 4, 0}, 0));
    const notation::EventId lower_second =
        score.insert_note(0, voiced(kQuarter, notation::quarter(), {notation::Step::F, 4, 0}, 1));

    // A third: the heads sit in one column, as they do in a chord.
    const notation::EventId upper_third =
        score.insert_note(0, voiced((2 * kQuarter), notation::quarter(), {notation::Step::G, 4, 0}, 0));
    const notation::EventId lower_third =
        score.insert_note(0, voiced((2 * kQuarter), notation::quarter(), {notation::Step::E, 4, 0}, 1));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    // The lower voice moves right, by about a notehead, and the upper stays
    // on the beat.
    const double head = find_note(result, upper_unison)->notehead_bounds.width;
    VCC_CHECK_NEAR(find_note(result, lower_unison)->centre.x,
                   find_note(result, upper_unison)->centre.x + head, 1e-9);
    VCC_CHECK_NEAR(find_note(result, lower_second)->centre.x,
                   find_note(result, upper_second)->centre.x + head, 1e-9);
    VCC_CHECK_NEAR(find_note(result, lower_third)->centre.x,
                   find_note(result, upper_third)->centre.x, 1e-9);

    // The stems of the moved pair stand side by side rather than crossing:
    // the up stem on the right of the first head, the down stem on the left
    // of the second, a stem's thickness apart.
    std::vector<const LineItem*> stems = lines_with(result.display_list, ItemRole::Stem);
    double upper_stem_x = 0.0;
    double lower_stem_x = 0.0;
    for (const LineItem* stem : stems) {
        if (stem->source_id == upper_second)
            upper_stem_x = stem->from.x;
        if (stem->source_id == lower_second)
            lower_stem_x = stem->from.x;
    }
    VCC_CHECK_NEAR(lower_stem_x - upper_stem_x, metrics_for(result, 0).stem_thickness, 1e-9);
}

VCC_TEST(crossed_voices_swap_sides_so_the_up_stem_voice_moves_right)
{
    notation::Score score;
    // The second voice wholly above the first: a crossing.
    const notation::EventId low =
        score.insert_note(0, voiced(0, notation::quarter(), {notation::Step::C, 4, 0}, 0));
    const notation::EventId high =
        score.insert_note(0, voiced(0, notation::quarter(), {notation::Step::G, 5, 0}, 1));
    // Crossed by a second: the heads would overlap as well.
    const notation::EventId below =
        score.insert_note(0, voiced(kQuarter, notation::quarter(), {notation::Step::G, 4, 0}, 0));
    const notation::EventId just_above =
        score.insert_note(0, voiced(kQuarter, notation::quarter(), {notation::Step::A, 4, 0}, 1));

    LayoutEngine engine;
    const TimelineMap timeline(8.0);
    const LayoutResult result = engine.layout(score, timeline, 0, kBar);

    const LaidOutNote& first = *find_note(result, low);
    const LaidOutNote& second = *find_note(result, high);
    VCC_CHECK(first.stem_up && !second.stem_up);

    // The down-stem voice keeps the beat; the up-stem voice moves a
    // notehead right, so its up stem rises on the far side of the other's
    // head and the other's down stem falls on the far side of its own.
    const double head = first.notehead_bounds.width;
    VCC_CHECK_NEAR(second.notehead_bounds.x, timeline.x_for_tick(0), 1e-9);
    VCC_CHECK_NEAR(first.centre.x, second.centre.x + head, 1e-9);
    VCC_CHECK(first.stem_x > second.stem_x + head);

    VCC_CHECK_NEAR(find_note(result, below)->centre.x,
                   find_note(result, just_above)->centre.x + head, 1e-9);
}

VCC_TEST(interleaved_voices_are_touching_not_crossed)
{
    notation::Score score;
    // The up-stem voice's chord reaches above and below the down-stem
    // voice's note: not a crossing, so the down-stem note moves, as at a
    // second.
    const std::vector<notation::EventId> chord = score.insert_chord(
        0, 0, notation::quarter(), {{notation::Step::C, 4, 0}, {notation::Step::G, 4, 0}}, 0);
    const notation::EventId inside =
        score.insert_note(0, voiced(0, notation::quarter(), {notation::Step::E, 4, 0}, 1));

    LayoutEngine engine;
    const TimelineMap timeline(8.0);
    const LayoutResult result = engine.layout(score, timeline, 0, kBar);

    const double head = find_note(result, chord[0])->notehead_bounds.width;
    VCC_CHECK_NEAR(find_note(result, chord[0])->notehead_bounds.x, timeline.x_for_tick(0), 1e-9);
    VCC_CHECK_NEAR(find_note(result, inside)->centre.x,
                   find_note(result, chord[0])->centre.x + head, 1e-9);
}

VCC_TEST(a_chord_moves_once_whatever_it_meets)
{
    notation::Score score;
    // Two down-stem voices both above the up-stem one: it moves right once,
    // not twice.
    const notation::EventId low =
        score.insert_note(0, voiced(0, notation::quarter(), {notation::Step::C, 4, 0}, 0));
    const notation::EventId mid =
        score.insert_note(0, voiced(0, notation::quarter(), {notation::Step::E, 5, 0}, 1));
    score.insert_note(0, voiced(0, notation::quarter(), {notation::Step::B, 5, 0}, 3));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);
    const double head = find_note(result, low)->notehead_bounds.width;
    VCC_CHECK_NEAR(find_note(result, low)->centre.x, find_note(result, mid)->centre.x + head,
                   1e-9);
}

VCC_TEST(a_unison_of_one_value_shares_one_head_with_a_stem_each_way)
{
    notation::Score score;
    const notation::EventId upper =
        score.insert_note(0, voiced(0, notation::quarter(), {notation::Step::G, 4, 0}, 0));
    const notation::EventId lower =
        score.insert_note(0, voiced(0, notation::quarter(), {notation::Step::G, 4, 0}, 1));
    // Two whole notes on one head would read as one note: they keep two.
    const notation::EventId upper_whole =
        score.insert_note(0, voiced(kBar, notation::whole(), {notation::Step::G, 4, 0}, 0));
    const notation::EventId lower_whole =
        score.insert_note(0, voiced(kBar, notation::whole(), {notation::Step::G, 4, 0}, 1));
    // A unison on a sharp and a natural is not a unison.
    const notation::EventId upper_sharp =
        score.insert_note(0, voiced(2 * kBar, notation::quarter(), {notation::Step::G, 4, 1}, 0));
    const notation::EventId lower_natural =
        score.insert_note(0, voiced(2 * kBar, notation::quarter(), {notation::Step::G, 4, 0}, 1));

    LayoutEngine engine;
    const TimelineMap timeline(8.0);
    const LayoutResult result = engine.layout(score, timeline, 0, 3 * kBar);

    // One head at the first beat, two stems from it: up on its right, down
    // on its left. Both notes are laid out, at the same place.
    std::size_t heads_at_start = 0;
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role == ItemRole::Notehead && std::abs(item.origin.x - timeline.x_for_tick(0)) < 1e-9)
            ++heads_at_start;
    }
    VCC_CHECK_EQ(heads_at_start, std::size_t{1});
    const LaidOutNote& above = *find_note(result, upper);
    const LaidOutNote& below = *find_note(result, lower);
    VCC_CHECK_NEAR(above.centre.x, below.centre.x, 1e-9);
    VCC_CHECK(above.stem_up && !below.stem_up);
    const double head = above.notehead_bounds.width;
    VCC_CHECK(above.stem_x > below.stem_x);
    VCC_CHECK_NEAR(above.stem_x - below.stem_x, head - metrics_for(result, 0).stem_thickness, 1e-9);
    VCC_CHECK_EQ(count_lines(result.display_list, ItemRole::Stem), std::size_t{4});

    // The whole notes and the false unison keep two heads, the lower moved
    // aside.
    VCC_CHECK_NEAR(find_note(result, lower_whole)->centre.x,
                   find_note(result, upper_whole)->centre.x
                       + find_note(result, upper_whole)->notehead_bounds.width,
                   1e-9);
    VCC_CHECK_NEAR(find_note(result, lower_natural)->centre.x,
                   find_note(result, upper_sharp)->centre.x + head, 1e-9);
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::Notehead), std::size_t{5});
}

VCC_TEST(the_dots_of_two_voices_share_a_column_clear_of_both_heads_and_a_space_each)
{
    notation::Score score;
    // A dotted second: the lower voice's head moves right, and a dot after
    // the upper voice's head would print through it.
    const notation::EventId upper =
        score.insert_note(0, voiced(0, notation::quarter(1), {notation::Step::G, 4, 0}, 0));
    const notation::EventId lower =
        score.insert_note(0, voiced(0, notation::quarter(1), {notation::Step::F, 4, 0}, 1));
    // Two dotted notes in one space: the lower voice's dot drops a space.
    const notation::EventId upper_space =
        score.insert_note(0, voiced(kBar, notation::half(1), {notation::Step::F, 4, 0}, 0));
    const notation::EventId lower_space =
        score.insert_note(0, voiced(kBar, notation::quarter(1), {notation::Step::F, 4, 0}, 1));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, 2 * kBar);
    const StaffMetrics metrics = metrics_for(result, 0);

    const auto dot_of = [&](notation::EventId id) {
        for (const GlyphItem& item : result.display_list.glyphs) {
            if (item.role == ItemRole::AugmentationDot && item.source_id == id)
                return item.origin;
        }
        return Point{-1.0, -1.0};
    };

    // One column, past the moved head.
    const Point upper_dot = dot_of(upper);
    const Point lower_dot = dot_of(lower);
    VCC_CHECK_NEAR(upper_dot.x, lower_dot.x, 1e-9);
    const LaidOutNote& moved = *find_note(result, lower);
    VCC_CHECK(upper_dot.x > moved.notehead_bounds.x + moved.notehead_bounds.width);
    // G4 is on a line: the upper voice's dot in the space above it, as
    // before. F4 is in a space, and the lower voice keeps its own space.
    VCC_CHECK_NEAR(upper_dot.y, metrics.y_for_staff_position(3), 1e-9);
    VCC_CHECK_NEAR(lower_dot.y, metrics.y_for_staff_position(1), 1e-9);

    // The same space twice: the upper voice keeps it and the lower drops.
    VCC_CHECK_NEAR(dot_of(upper_space).y, metrics.y_for_staff_position(1), 1e-9);
    VCC_CHECK_NEAR(dot_of(lower_space).y, metrics.y_for_staff_position(-1), 1e-9);
    VCC_CHECK_NEAR(dot_of(upper_space).x, dot_of(lower_space).x, 1e-9);
}

VCC_TEST(a_line_note_in_the_lower_voice_takes_its_dot_in_the_space_below)
{
    notation::Score score;
    // Both voices dotted on the middle line, a fifth apart on other beats
    // so neither moves: without the rule both dots would sit in the space
    // above the line, one on the other.
    const notation::EventId upper =
        score.insert_note(0, voiced(0, notation::quarter(1), {notation::Step::D, 5, 0}, 0));
    const notation::EventId lower =
        score.insert_note(0, voiced(0, notation::quarter(1), {notation::Step::G, 4, 0}, 1));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);
    const StaffMetrics metrics = metrics_for(result, 0);
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role != ItemRole::AugmentationDot)
            continue;
        if (item.source_id == upper)
            VCC_CHECK_NEAR(item.origin.y, metrics.y_for_staff_position(7), 1e-9);
        if (item.source_id == lower)
            VCC_CHECK_NEAR(item.origin.y, metrics.y_for_staff_position(1), 1e-9);
    }
    VCC_CHECK_EQ(count_glyphs(result.display_list, ItemRole::AugmentationDot), std::size_t{2});
}

VCC_TEST(articulations_and_ties_go_on_the_voices_outer_side_when_voices_share_a_measure)
{
    notation::Score score;
    notation::ScoreNote soprano = voiced(0, notation::quarter(), {notation::Step::G, 4, 0}, 0);
    soprano.articulations = notation::to_mask(notation::Articulation::Staccato);
    soprano.tied_to_next = true;
    const notation::EventId upper = score.insert_note(0, soprano);
    score.insert_note(0, voiced(kQuarter, notation::quarter(), {notation::Step::G, 4, 0}, 0));

    notation::ScoreNote alto = voiced(0, notation::quarter(), {notation::Step::C, 4, 0}, 1);
    alto.articulations = notation::to_mask(notation::Articulation::Staccato)
                       | notation::to_mask(notation::Articulation::Marcato);
    alto.tied_to_next = true;
    const notation::EventId lower = score.insert_note(0, alto);
    score.insert_note(0, voiced(kQuarter, notation::quarter(), {notation::Step::C, 4, 0}, 1));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    // The upper voice's staccato is above its stem's end, the lower
    // voice's below its stem's end - neither between the voices - and the
    // marcato takes the font's below form under the lower voice.
    const LaidOutNote& above = *find_note(result, upper);
    const LaidOutNote& below = *find_note(result, lower);
    VCC_CHECK(above.stem_up && !below.stem_up);
    std::size_t marks = 0;
    for (const GlyphItem& item : result.display_list.glyphs) {
        if (item.role != ItemRole::Articulation)
            continue;
        ++marks;
        if (item.source_id == upper) {
            VCC_CHECK(item.origin.y < above.stem_end_y);
            VCC_CHECK(item.glyph == Glyph::ArticStaccatoAbove);
        }
        if (item.source_id == lower) {
            VCC_CHECK(item.origin.y > below.stem_end_y);
            VCC_CHECK(item.glyph == Glyph::ArticStaccatoBelow
                      || item.glyph == Glyph::ArticMarcatoBelow);
        }
    }
    VCC_CHECK_EQ(marks, std::size_t{3});

    // The ties arch the same way: the upper voice's upward, the lower's
    // downward, away from each other.
    std::size_t ties = 0;
    for (const CurveItem& curve : result.display_list.curves) {
        if (curve.role != ItemRole::Tie)
            continue;
        ++ties;
        if (curve.source_id == upper)
            VCC_CHECK(curve.control_one.y < curve.start.y);
        if (curve.source_id == lower)
            VCC_CHECK(curve.control_one.y > curve.start.y);
    }
    VCC_CHECK_EQ(ties, std::size_t{2});
}

VCC_TEST(text_markings_are_drawn_above_the_top_staff_at_their_tick_and_clear_of_the_notes)
{
    notation::Score score;
    score.make_grand_staff();
    score.add_text(notation::TextMarking{0, 0, "A"});
    score.add_text(notation::TextMarking{2 * kBar, 1, "Hit"});
    // A high note under the second marking in the first voice of a shared
    // measure, so its stem goes up past the head: the text has to clear
    // the stem, not only the head.
    const notation::EventId high =
        score.insert_note(0, voiced(2 * kBar, notation::quarter(), {notation::Step::E, 6, 0}, 0));
    score.insert_note(0, voiced(2 * kBar + kQuarter, notation::quarter(), {notation::Step::C, 4, 0}, 1));

    LayoutEngine engine;
    const TimelineMap timeline(8.0);
    const LayoutResult result = engine.layout(score, timeline, 0, 3 * kBar);

    std::vector<const TextItem*> texts = texts_with(result.display_list, ItemRole::Text);
    VCC_CHECK_EQ(texts.size(), std::size_t{2});
    const StaffMetrics top = metrics_for(result, 0);
    for (const TextItem* text : texts) {
        VCC_CHECK(text->origin.y < top.top_y);
        if (text->text == "A")
            VCC_CHECK_NEAR(text->origin.x, timeline.x_for_tick(0), 1e-9);
        if (text->text == "Hit") {
            VCC_CHECK_NEAR(text->origin.x, timeline.x_for_tick(2 * kBar), 1e-9);
            const LaidOutNote& under = *find_note(result, high);
            VCC_CHECK(under.stem_up && under.has_stem);
            VCC_CHECK(text->origin.y < under.notehead_bounds.y);
            VCC_CHECK(text->origin.y < under.stem_end_y);
        }
    }

    // Only what is in view, and nothing when turned off.
    const LayoutResult later = engine.layout(score, timeline, kBar, 2 * kBar);
    VCC_CHECK(texts_with(later.display_list, ItemRole::Text).empty());
    LayoutOptions options;
    options.draw_texts = false;
    const LayoutEngine silent(GlyphTable{}, options);
    VCC_CHECK(texts_with(silent.layout(score, timeline, 0, 3 * kBar).display_list, ItemRole::Text).empty());
}

VCC_TEST_MAIN("engraving/LayoutEngine")
