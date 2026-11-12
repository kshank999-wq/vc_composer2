// The score model (spec 02 sections 5.1, 5.2, 12, 13).
//
// Acceptance criteria 3 and 4 are exercised directly here: the notation
// timeline staying musically aligned with bars and beats, and the user being
// able to set staff, key and time signature.
#include "vcc/notation/Score.h"

#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "vcc_test.h"

using namespace vcc::notation;

/// A quarter note in ticks, so the arithmetic reads as rhythm.
constexpr Ticks kQuarter = kTicksPerQuarter;

namespace {

ScoreNote note_at(Ticks tick, Step step, std::int8_t octave = 4,
                  Duration duration = quarter())
{
    ScoreNote note;
    note.start_tick = tick;
    note.duration = duration;
    note.pitch = Pitch{step, octave, 0};
    return note;
}

}  // namespace

VCC_TEST(a_new_score_is_usable_without_configuration)
{
    // A caller must never have to check whether a staff or a signature
    // exists before using the score.
    const Score score;
    VCC_CHECK_EQ(score.staff_count(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(score.staff(0).clef, Clef::Treble);
    VCC_CHECK(score.time_signature_at(0) == vcc::music::TimeSignature{4, 4});
    VCC_CHECK_EQ(score.key_signature_at(0).fifths, 0);
    VCC_CHECK_EQ(score.total_note_count(), static_cast<std::size_t>(0));
    VCC_CHECK_EQ(score.end_tick(), static_cast<Ticks>(0));
}

VCC_TEST(a_grace_note_takes_no_time_and_sorts_before_the_note_it_ornaments)
{
    Score score;
    const EventId principal = score.insert_note(0, note_at(kQuarter, Step::E));
    // The first grace note is the higher pitch, so order and pitch
    // disagree: the order wins.
    ScoreNote second = note_at(kQuarter, Step::C, 4, eighth());
    second.grace = true;
    second.grace_order = 1;
    ScoreNote first = note_at(kQuarter, Step::D, 4, eighth());
    first.grace = true;
    first.grace_order = 0;
    const EventId second_id = score.insert_note(0, second);
    const EventId first_id = score.insert_note(0, first);
    VCC_CHECK(second_id != kInvalidEventId && first_id != kInvalidEventId);

    // Its end is its start, whatever value it is written in.
    VCC_CHECK_EQ(first.end_tick(), Ticks{kQuarter});
    VCC_CHECK_EQ(second.end_tick(), Ticks{kQuarter});

    // Grace notes first, in their order, then the note they ornament.
    const std::vector<ScoreNote>& notes = score.notes(0);
    VCC_CHECK_EQ(notes.size(), std::size_t{3});
    if (notes.size() == 3) {
        VCC_CHECK_EQ(notes[0].id, first_id);
        VCC_CHECK_EQ(notes[1].id, second_id);
        VCC_CHECK_EQ(notes[2].id, principal);
    }

    // In the range its tick is, though it sounds for no time; and not in
    // the range before it, though its written value would reach.
    VCC_CHECK_EQ(score.notes_in_range(0, kQuarter, (2 * kQuarter)).size(), std::size_t{3});
    VCC_CHECK_EQ(score.notes_in_range(0, 0, kQuarter).size(), std::size_t{0});
    VCC_CHECK_EQ(score.notes_in_range(0, (kQuarter / 2), (3 * kQuarter / 2)).size(), std::size_t{3});

    // Never a rest and never tied: a grace note takes no time to tie or
    // to rest through.
    ScoreNote rest = note_at(0, Step::C, 4, eighth());
    rest.grace = true;
    rest.is_rest = true;
    VCC_CHECK(!rest.is_valid());
    ScoreNote tied = note_at(0, Step::C, 4, eighth());
    tied.grace = true;
    tied.tied_to_next = true;
    VCC_CHECK(!tied.is_valid());
    VCC_CHECK_EQ(score.insert_note(0, tied), kInvalidEventId);
}

VCC_TEST(a_note_crosses_to_the_staff_above_or_below_and_no_further)
{
    ScoreNote note = note_at(0, Step::C);
    note.staff_shift = 1;
    VCC_CHECK(note.is_valid());
    note.staff_shift = -1;
    VCC_CHECK(note.is_valid());
    note.staff_shift = 2;
    VCC_CHECK(!note.is_valid());
    note.staff_shift = -2;
    VCC_CHECK(!note.is_valid());

    // The shift is how it is drawn, not where it is: the note stays on
    // its own staff in the score.
    Score score;
    score.make_grand_staff();
    note.staff_shift = 1;
    const EventId id = score.insert_note(0, note);
    VCC_CHECK(id != kInvalidEventId);
    VCC_CHECK_EQ(score.staff_index_of(id), std::size_t{0});
    VCC_CHECK_EQ(score.notes(1).size(), std::size_t{0});
}

VCC_TEST(a_marking_governs_its_own_staff_and_a_staff_with_none_follows_the_others)
{
    Score score;
    score.make_grand_staff();
    score.add_dynamic(DynamicMarking{0, DynamicLevel::Forte, 0, ""});
    // One marking between the staves: both hands.
    VCC_CHECK_EQ(score.dynamic_at(0, 0, 0), DynamicLevel::Forte);
    VCC_CHECK_EQ(score.dynamic_at(0, 0, 1), DynamicLevel::Forte);
    VCC_CHECK(!score.staff_has_dynamics(1, 100000));
    VCC_CHECK(score.staff_has_dynamics(0, 0));

    // The lower staff gets its own p later: it is governed by that, and
    // the upper staff keeps the f.
    DynamicMarking left{kQuarter, DynamicLevel::Piano, 0, ""};
    left.staff = 1;
    score.add_dynamic(left);
    VCC_CHECK_EQ(score.dynamic_at(2 * kQuarter, 0, 0), DynamicLevel::Forte);
    VCC_CHECK_EQ(score.dynamic_at(2 * kQuarter, 0, 1), DynamicLevel::Piano);
    // Before its own marking, the lower staff still followed the upper's.
    VCC_CHECK_EQ(score.dynamic_at(kQuarter / 2, 0, 1), DynamicLevel::Forte);
    VCC_CHECK(score.staff_has_dynamics(1, kQuarter));
    VCC_CHECK(!score.staff_has_dynamics(1, kQuarter - 1));

    // One marking per tick per voice per staff: the same place on the
    // other staff is another marking, and the same place again replaces.
    DynamicMarking right{kQuarter, DynamicLevel::MezzoPiano, 0, ""};
    right.staff = 0;
    score.add_dynamic(right);
    VCC_CHECK_EQ(score.dynamics().size(), std::size_t{3});
    right.level = DynamicLevel::Fortissimo;
    score.add_dynamic(right);
    VCC_CHECK_EQ(score.dynamics().size(), std::size_t{3});
    VCC_CHECK_EQ(score.dynamic_at(kQuarter, 0, 0), DynamicLevel::Fortissimo);
    VCC_CHECK_EQ(score.dynamic_at(kQuarter, 0, 1), DynamicLevel::Piano);
    VCC_CHECK(score.remove_dynamic(kQuarter, 0, 1));
    VCC_CHECK(!score.remove_dynamic(kQuarter, 0, 1));
    VCC_CHECK_EQ(score.dynamic_at(kQuarter, 0, 1), DynamicLevel::Fortissimo);

    // Hairpins the same way: the staff's own first, any staff's otherwise.
    Hairpin upper_hairpin;
    upper_hairpin.start_tick = 0;
    upper_hairpin.end_tick = (2 * kQuarter);
    upper_hairpin.staff = 0;
    score.add_hairpin(upper_hairpin);
    VCC_CHECK(score.hairpin_at(500, 0, 1) != nullptr);
    Hairpin lower_hairpin = upper_hairpin;
    lower_hairpin.staff = 1;
    lower_hairpin.kind = HairpinKind::Diminuendo;
    score.add_hairpin(lower_hairpin);
    VCC_CHECK(score.hairpin_at(500, 0, 1)->kind == HairpinKind::Diminuendo);
    VCC_CHECK(score.hairpin_at(500, 0, 0)->kind == HairpinKind::Crescendo);
    VCC_CHECK(score.remove_hairpin(0, 0, 1));
    VCC_CHECK(score.hairpin_at(500, 0, 1)->kind == HairpinKind::Crescendo);
}

VCC_TEST(grand_staff_configuration_gives_treble_over_bass)
{
    // Spec 5.1 requires a grand staff for piano and similar instruments.
    Score score;
    score.make_grand_staff();

    VCC_CHECK_EQ(score.staff_count(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(score.staff(0).clef, Clef::Treble);
    VCC_CHECK_EQ(score.staff(1).clef, Clef::Bass);
    VCC_CHECK(!score.staff(0).name.empty());
    VCC_CHECK(!score.staff(1).name.empty());
}

VCC_TEST(measures_are_derived_from_the_time_signature)
{
    // Acceptance criterion 3: the notation timeline stays aligned to bars.
    Score score;
    VCC_CHECK(score.set_time_signature(0, vcc::music::TimeSignature{4, 4}));

    // 4/4 is 3840 ticks a bar.
    VCC_CHECK_EQ(score.measure_at(0).number, 1);
    VCC_CHECK_EQ(score.measure_at(0).start_tick, static_cast<Ticks>(0));
    VCC_CHECK_EQ(score.measure_at(0).length_ticks, static_cast<Ticks>((4 * kQuarter)));
    VCC_CHECK_EQ(score.measure_at(4 * kQuarter - 1).number, 1);
    VCC_CHECK_EQ(score.measure_at((4 * kQuarter)).number, 2);
    VCC_CHECK_EQ(score.measure_at((4 * kQuarter)).start_tick, static_cast<Ticks>((4 * kQuarter)));
    VCC_CHECK_EQ(score.measure_at(40 * kQuarter).number, 11);
}

VCC_TEST(measure_lengths_are_right_for_every_common_meter)
{
    VCC_CHECK_EQ(measure_length_ticks(vcc::music::TimeSignature{4, 4}),
                 static_cast<Ticks>((4 * kQuarter)));
    VCC_CHECK_EQ(measure_length_ticks(vcc::music::TimeSignature{3, 4}),
                 static_cast<Ticks>((3 * kQuarter)));
    VCC_CHECK_EQ(measure_length_ticks(vcc::music::TimeSignature{2, 2}),
                 static_cast<Ticks>((4 * kQuarter)));
    VCC_CHECK_EQ(measure_length_ticks(vcc::music::TimeSignature{6, 8}),
                 static_cast<Ticks>((3 * kQuarter)));
    VCC_CHECK_EQ(measure_length_ticks(vcc::music::TimeSignature{7, 8}),
                 static_cast<Ticks>(7 * kQuarter / 2));
    VCC_CHECK_EQ(measure_length_ticks(vcc::music::TimeSignature{5, 4}),
                 static_cast<Ticks>((5 * kQuarter)));
    VCC_CHECK_EQ(measure_length_ticks(vcc::music::TimeSignature{12, 8}),
                 static_cast<Ticks>((6 * kQuarter)));
}

VCC_TEST(a_meter_change_renumbers_the_bars_from_that_point)
{
    Score score;
    score.set_time_signature(0, vcc::music::TimeSignature{4, 4});
    // Bar 5 starts at four bars of four quarters. Change to 3/4 there.
    VCC_CHECK(score.set_time_signature((16 * kQuarter), vcc::music::TimeSignature{3, 4}));

    VCC_CHECK_EQ(score.measure_at((16 * kQuarter)).number, 5);
    VCC_CHECK_EQ(score.measure_at((16 * kQuarter)).length_ticks, static_cast<Ticks>((3 * kQuarter)));
    // Bar 6 now starts three quarters later, not four.
    VCC_CHECK_EQ(score.measure_at((16 * kQuarter) + (3 * kQuarter)).number, 6);
    VCC_CHECK_EQ(score.measure_start_tick(6), static_cast<Ticks>(19 * kQuarter));
}

VCC_TEST(a_meter_change_off_a_barline_is_refused)
{
    // Accepting one would leave the bar structure permanently out of step
    // with the host timeline, which acceptance criterion 3 forbids.
    Score score;
    score.set_time_signature(0, vcc::music::TimeSignature{4, 4});

    VCC_CHECK(!score.set_time_signature(kQuarter, vcc::music::TimeSignature{3, 4}));
    VCC_CHECK(!score.set_time_signature(3839, vcc::music::TimeSignature{3, 4}));
    // On the barline it is accepted.
    VCC_CHECK(score.set_time_signature((4 * kQuarter), vcc::music::TimeSignature{3, 4}));
}

VCC_TEST(invalid_signatures_are_refused)
{
    Score score;
    VCC_CHECK(!score.set_time_signature(0, vcc::music::TimeSignature{4, 3}));  // Not power of 2.
    VCC_CHECK(!score.set_time_signature(0, vcc::music::TimeSignature{0, 4}));
    VCC_CHECK(!score.set_time_signature(-kQuarter, vcc::music::TimeSignature{4, 4}));
    VCC_CHECK(!score.set_key_signature(0, KeySignature{8, false}));
    VCC_CHECK(!score.set_key_signature(-1, KeySignature{0, false}));
}

VCC_TEST(bar_and_beat_conversion_round_trips)
{
    Score score;
    score.set_time_signature(0, vcc::music::TimeSignature{4, 4});

    // Bar 1 beat 1 is tick 0; bar 1 beat 2 is a quarter in.
    VCC_CHECK_EQ(score.from_bar_beat(BarBeat{1, 1.0}), static_cast<Ticks>(0));
    VCC_CHECK_EQ(score.from_bar_beat(BarBeat{1, 2.0}), static_cast<Ticks>(kQuarter));
    VCC_CHECK_EQ(score.from_bar_beat(BarBeat{2, 1.0}), static_cast<Ticks>((4 * kQuarter)));
    VCC_CHECK_EQ(score.from_bar_beat(BarBeat{3, 3.5}), static_cast<Ticks>((8 * kQuarter) + (5 * kQuarter / 2)));

    const BarBeat back = score.to_bar_beat((8 * kQuarter) + (5 * kQuarter / 2));
    VCC_CHECK_EQ(back.measure, 3);
    VCC_CHECK_NEAR(back.beat, 3.5, 1e-9);
}

VCC_TEST(bar_and_beat_conversion_is_correct_in_compound_meter)
{
    // In 6/8 a beat is an eighth note, so bar 1 beat 4 is three eighths in.
    Score score;
    score.set_time_signature(0, vcc::music::TimeSignature{6, 8});

    VCC_CHECK_EQ(score.from_bar_beat(BarBeat{1, 4.0}), static_cast<Ticks>((3 * kQuarter / 2)));
    VCC_CHECK_EQ(score.from_bar_beat(BarBeat{2, 1.0}), static_cast<Ticks>((3 * kQuarter)));

    const BarBeat back = score.to_bar_beat((3 * kQuarter / 2));
    VCC_CHECK_EQ(back.measure, 1);
    VCC_CHECK_NEAR(back.beat, 4.0, 1e-9);
}

VCC_TEST(inserting_a_note_assigns_a_stable_unique_id)
{
    // Spec 13 requires stable ids linking notation to host MIDI events.
    Score score;
    std::set<EventId> ids;

    for (int index = 0; index < 200; ++index) {
        const EventId id = score.insert_note(0, note_at(index * kQuarter, Step::C));
        VCC_CHECK(id != kInvalidEventId);
        VCC_CHECK_MSG(ids.insert(id).second, "duplicate event id");
    }
    VCC_CHECK_EQ(score.total_note_count(), static_cast<std::size_t>(200));
}

VCC_TEST(ids_survive_edits_to_other_notes)
{
    // The property the diff layer depends on: editing one note must not
    // change any other note's identity.
    Score score;
    const EventId first = score.insert_note(0, note_at(0, Step::C));
    const EventId second = score.insert_note(0, note_at(kQuarter, Step::D));
    const EventId third = score.insert_note(0, note_at((2 * kQuarter), Step::E));

    VCC_CHECK(score.remove_note(second));

    VCC_CHECK(score.find_note(first) != nullptr);
    VCC_CHECK(score.find_note(third) != nullptr);
    VCC_CHECK(score.find_note(second) == nullptr);
    VCC_CHECK_EQ(score.find_note(first)->id, first);
}

VCC_TEST(ids_are_not_reused_after_clearing)
{
    // A cleared score must not hand out an id the host still associates
    // with a deleted note.
    Score score;
    const EventId before = score.insert_note(0, note_at(0, Step::C));
    score.clear();
    const EventId after = score.insert_note(0, note_at(0, Step::C));
    VCC_CHECK(after != before);
    VCC_CHECK(after > before);
}

VCC_TEST(modifying_a_note_keeps_its_id)
{
    Score score;
    const EventId id = score.insert_note(0, note_at(0, Step::C));

    ScoreNote changed = note_at(0, Step::D);
    changed.duration = half();
    VCC_CHECK(score.modify_note(id, changed));

    const ScoreNote* stored = score.find_note(id);
    VCC_CHECK(stored != nullptr);
    VCC_CHECK_EQ(stored->id, id);
    VCC_CHECK_EQ(stored->pitch.step, Step::D);
    VCC_CHECK(stored->duration == half());
}

VCC_TEST(invalid_notes_are_refused)
{
    Score score;

    ScoreNote negative = note_at(0, Step::C);
    negative.start_tick = -kQuarter;
    VCC_CHECK_EQ(score.insert_note(0, negative), kInvalidEventId);

    ScoreNote bad_duration = note_at(0, Step::C);
    bad_duration.duration = Duration{NoteValue::Quarter, 7, {}};
    VCC_CHECK_EQ(score.insert_note(0, bad_duration), kInvalidEventId);

    ScoreNote bad_pitch = note_at(0, Step::C);
    bad_pitch.pitch.alteration = 5;
    VCC_CHECK_EQ(score.insert_note(0, bad_pitch), kInvalidEventId);

    // A rest cannot be tied or in a chord: both would be nonsense the
    // renderer would have to guess about.
    ScoreNote tied_rest;
    tied_rest.start_tick = 0;
    tied_rest.duration = quarter();
    tied_rest.is_rest = true;
    tied_rest.tied_to_next = true;
    VCC_CHECK_EQ(score.insert_note(0, tied_rest), kInvalidEventId);

    // A staff that does not exist.
    VCC_CHECK_EQ(score.insert_note(7, note_at(0, Step::C)), kInvalidEventId);

    VCC_CHECK_EQ(score.total_note_count(), static_cast<std::size_t>(0));
}

VCC_TEST(rests_are_valid_and_occupy_time)
{
    Score score;
    ScoreNote rest;
    rest.start_tick = kQuarter;
    rest.duration = half();
    rest.is_rest = true;

    const EventId id = score.insert_note(0, rest);
    VCC_CHECK(id != kInvalidEventId);
    VCC_CHECK_EQ(score.find_note(id)->end_tick(), static_cast<Ticks>(kQuarter + (2 * kQuarter)));
}

VCC_TEST(notes_stay_ordered_by_position_then_pitch)
{
    // Inserted out of order; the score keeps them in musical order so
    // rendering never has to sort.
    Score score;
    score.insert_note(0, note_at((2 * kQuarter), Step::E));
    score.insert_note(0, note_at(0, Step::G));
    score.insert_note(0, note_at(kQuarter, Step::C));
    score.insert_note(0, note_at(0, Step::C));

    const std::vector<ScoreNote>& notes = score.notes(0);
    VCC_CHECK_EQ(notes.size(), static_cast<std::size_t>(4));
    VCC_CHECK_EQ(notes[0].start_tick, static_cast<Ticks>(0));
    VCC_CHECK_EQ(notes[0].pitch.step, Step::C);   // Lower pitch first at the same tick.
    VCC_CHECK_EQ(notes[1].start_tick, static_cast<Ticks>(0));
    VCC_CHECK_EQ(notes[1].pitch.step, Step::G);
    VCC_CHECK_EQ(notes[2].start_tick, static_cast<Ticks>(kQuarter));
    VCC_CHECK_EQ(notes[3].start_tick, static_cast<Ticks>((2 * kQuarter)));
}

VCC_TEST(a_chord_shares_a_position_and_is_marked_as_one)
{
    Score score;
    const std::vector<EventId> ids = score.insert_chord(
        0, kQuarter, half(), {Pitch{Step::C, 4, 0}, Pitch{Step::E, 4, 0}, Pitch{Step::G, 4, 0}});

    VCC_CHECK_EQ(ids.size(), static_cast<std::size_t>(3));
    for (const EventId id : ids) {
        const ScoreNote* note = score.find_note(id);
        VCC_CHECK(note != nullptr);
        VCC_CHECK_EQ(note->start_tick, static_cast<Ticks>(kQuarter));
        VCC_CHECK(note->in_chord);
        VCC_CHECK(note->duration == half());
    }
}

VCC_TEST(a_single_pitch_chord_is_just_a_note)
{
    Score score;
    const std::vector<EventId> ids = score.insert_chord(0, 0, quarter(), {Pitch{Step::C, 4, 0}});
    VCC_CHECK_EQ(ids.size(), static_cast<std::size_t>(1));
    VCC_CHECK(!score.find_note(ids[0])->in_chord);
}

VCC_TEST(a_chord_with_a_bad_pitch_leaves_nothing_behind)
{
    // Partially inserting a chord would leave the score in a state the
    // composer did not ask for.
    Score score;
    Pitch bad{Step::C, 4, 0};
    bad.alteration = 9;

    const std::vector<EventId> ids =
        score.insert_chord(0, 0, quarter(), {Pitch{Step::C, 4, 0}, bad});
    VCC_CHECK(ids.empty());
    VCC_CHECK_EQ(score.total_note_count(), static_cast<std::size_t>(0));
}

VCC_TEST(range_queries_return_overlapping_notes)
{
    Score score;
    score.insert_note(0, note_at(0, Step::C, 4, whole()));     // 0-(4 * kQuarter)
    score.insert_note(0, note_at((4 * kQuarter), Step::D, 4, quarter()));  // (4 * kQuarter)-(5 * kQuarter)
    score.insert_note(0, note_at((8 * kQuarter), Step::E, 4, quarter()));

    // A note sounding into the range counts, not only one starting in it.
    const std::vector<ScoreNote> overlapping =
        score.notes_in_range(0, (2 * kQuarter), 4 * kQuarter + kQuarter / 6);
    VCC_CHECK_EQ(overlapping.size(), static_cast<std::size_t>(2));

    VCC_CHECK_EQ(score.notes_in_range(0, 5 * kQuarter + kQuarter / 5, 7 * kQuarter + kQuarter / 3).size(),
                 static_cast<std::size_t>(0));
    VCC_CHECK_EQ(score.notes_in_range(0, 100, 100).size(), static_cast<std::size_t>(0));
}

// --- What a range query is allowed to skip ----------------------------------
//
// The query stops scanning early, which is what makes laying out a four-bar
// window cost what the window costs rather than what the whole cue does. The
// notes are sorted by start tick, so it binary-searches the start and breaks
// at the end - and the one thing that can go wrong with that is a note which
// *starts* before the search and sounds into the range. These pin that down,
// because a scan that misses one drops music off the page rather than
// slowing anything down.

VCC_TEST(a_long_note_starting_well_before_the_range_is_still_found)
{
    Score score;
    // Twenty bars of a held note, then the range asked about is bar 19.
    ScoreNote held = note_at(0, Step::C, 4, whole());
    held.duration.value = NoteValue::Whole;
    score.insert_note(0, held);

    // Something much later, so the staff is not one note long.
    score.insert_note(0, note_at(80 * kQuarter, Step::D, 4, quarter()));

    // The held note covers the first bar. A range inside it, starting after
    // it began, has to find it.
    const std::vector<ScoreNote> inside =
        score.notes_in_range(0, (3 * kQuarter), 3 * kQuarter + kQuarter / 8);
    VCC_CHECK_EQ(inside.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(inside.front().start_tick, Ticks{0});
}

VCC_TEST(the_longest_note_on_the_staff_is_what_the_search_reaches_back_for)
{
    // The bound is per staff and has to cover the longest note on it. A
    // short note inserted after a long one must not shorten the reach.
    Score score;
    score.insert_note(0, note_at(0, Step::C, 4, whole()));  // 0-(4 * kQuarter)
    for (int i = 1; i < 40; ++i)
        score.insert_note(0, note_at((4 * kQuarter) + i * kQuarter, Step::D, 4, quarter()));

    const std::vector<ScoreNote> inside =
        score.notes_in_range(0, 3 * kQuarter + kQuarter / 8, 3 * kQuarter + kQuarter / 4);
    VCC_CHECK_EQ(inside.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(inside.front().start_tick, Ticks{0});
}

VCC_TEST(a_note_lengthened_after_it_was_written_is_found_from_further_on)
{
    // The bound climbs when a note is modified, not only when one is
    // inserted. Without that, lengthening a note would make it invisible to
    // a range it now reaches.
    Score score;
    const EventId id = score.insert_note(0, note_at(0, Step::C, 4, sixteenth()));

    // The fillers start at bar two, so nothing but the note under test can
    // ever reach the range this asks about.
    for (int i = 0; i < 40; ++i)
        score.insert_note(0, note_at((4 * kQuarter) + i * kQuarter, Step::D, 4, quarter()));

    // It reaches nowhere near the end of bar one yet.
    VCC_CHECK_EQ(score.notes_in_range(0, 3000, 3100).size(), static_cast<std::size_t>(0));

    ScoreNote longer = note_at(0, Step::C, 4, whole());
    longer.id = id;
    VCC_CHECK(score.modify_note(id, longer));

    const std::vector<ScoreNote> inside =
        score.notes_in_range(0, 3 * kQuarter + kQuarter / 8, 3 * kQuarter + kQuarter / 4);
    VCC_CHECK_EQ(inside.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(inside.front().id, id);
}

VCC_TEST(a_restored_note_is_found_the_way_an_inserted_one_is)
{
    // Undo puts a note back through `restore_note` rather than
    // `insert_note`, so the bound has to climb there too - otherwise undoing
    // the deletion of a long note brings it back invisible.
    // Nothing longer than a quarter has ever been in this score, so its
    // bound has never been raised past one. Restoring a whole note has to
    // raise it - and because the bound is never lowered, a test that
    // inserted the long note first and removed it would pass whether
    // `restore_note` raised anything or not.
    Score score;
    for (int i = 0; i < 40; ++i)
        score.insert_note(0, note_at((4 * kQuarter) + i * kQuarter, Step::D, 4, quarter()));

    VCC_CHECK_EQ(score.notes_in_range(0, 3000, 3100).size(), static_cast<std::size_t>(0));

    ScoreNote held = note_at(0, Step::C, 4, whole());
    held.id = 9999;  // Unused here, which is all `restore_note` asks.
    VCC_CHECK(score.restore_note(0, held));

    const std::vector<ScoreNote> inside =
        score.notes_in_range(0, 3 * kQuarter + kQuarter / 8, 3 * kQuarter + kQuarter / 4);
    VCC_CHECK_EQ(inside.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(inside.front().id, EventId{9999});
}

VCC_TEST(removing_the_longest_note_never_hides_the_ones_that_remain)
{
    // The bound is an upper bound and is deliberately not lowered when a
    // note goes: recomputing it would put back the whole-staff walk it was
    // added to remove. Too large only ever searches wider, so this is a
    // check that it stays correct rather than fast.
    Score score;
    const EventId longest = score.insert_note(0, note_at(0, Step::C, 4, whole()));
    score.insert_note(0, note_at((3 * kQuarter), Step::E, 4, half()));  // (3 * kQuarter)-(5 * kQuarter)
    VCC_CHECK(score.remove_note(longest));

    const std::vector<ScoreNote> inside =
        score.notes_in_range(0, 3 * kQuarter + kQuarter / 8, 3 * kQuarter + kQuarter / 4);
    VCC_CHECK_EQ(inside.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(inside.front().start_tick, Ticks{(3 * kQuarter)});
}

VCC_TEST(a_range_at_the_very_start_of_a_score_does_not_search_backwards)
{
    // `from - longest` underflows for an unsigned tick at zero, and a
    // wrapped bound would binary-search from the end of the staff and find
    // nothing at all.
    Score score;
    score.insert_note(0, note_at(0, Step::C, 4, whole()));
    score.insert_note(0, note_at((4 * kQuarter), Step::D, 4, quarter()));

    const std::vector<ScoreNote> opening = score.notes_in_range(0, 0, kQuarter);
    VCC_CHECK_EQ(opening.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(opening.front().start_tick, Ticks{0});
}

VCC_TEST(the_edges_of_a_range_belong_to_exactly_one_side)
{
    // A note ending exactly where the range starts is not in it, and one
    // starting exactly where the range ends is not either. Both are the
    // boundaries the early break and the binary search sit on.
    Score score;
    score.insert_note(0, note_at(0, Step::C, 4, quarter()));      // 0-kQuarter
    score.insert_note(0, note_at((2 * kQuarter), Step::D, 4, quarter()));   // (2 * kQuarter)-(3 * kQuarter)

    VCC_CHECK_EQ(score.notes_in_range(0, kQuarter, (2 * kQuarter)).size(), static_cast<std::size_t>(0));
    VCC_CHECK_EQ(score.notes_in_range(0, 0, kQuarter).size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(score.notes_in_range(0, (2 * kQuarter), (3 * kQuarter)).size(), static_cast<std::size_t>(1));
}

VCC_TEST(a_range_query_returns_the_same_notes_a_whole_staff_walk_would)
{
    // The property behind all of the above, checked against the thing the
    // query replaced: walk every note and keep the overlapping ones. Any
    // disagreement is the early break cutting music off.
    std::mt19937 rng(20260917);

    Score score;
    for (int i = 0; i < 400; ++i) {
        const auto length = static_cast<NoteValue>(rng() % 5);
        ScoreNote note = note_at(static_cast<Ticks>(rng() % 40000), Step::C, 4);
        note.duration.value = length;
        note.pitch.step = static_cast<Step>(rng() % 7);
        score.insert_note(0, note);
    }

    for (int trial = 0; trial < 200; ++trial) {
        const auto from = static_cast<Ticks>(rng() % 42000);
        const auto to = from + static_cast<Ticks>(1 + rng() % 5000);

        std::vector<EventId> expected;
        for (const ScoreNote& note : score.notes(0)) {
            if (note.end_tick() > from && note.start_tick < to)
                expected.push_back(note.id);
        }

        std::vector<EventId> got;
        for (const ScoreNote& note : score.notes_in_range(0, from, to))
            got.push_back(note.id);

        VCC_CHECK_MSG(got == expected,
                      "range " + std::to_string(from) + "-" + std::to_string(to)
                          + " gave " + std::to_string(got.size()) + " notes, a full walk gave "
                          + std::to_string(expected.size()));
    }
}

VCC_TEST(dynamics_are_in_force_until_the_next_one)
{
    Score score;
    score.add_dynamic(DynamicMarking{0, DynamicLevel::Piano, 0, {}});
    score.add_dynamic(DynamicMarking{(4 * kQuarter), DynamicLevel::Forte, 0, {}});

    VCC_CHECK_EQ(score.dynamic_at(0), DynamicLevel::Piano);
    VCC_CHECK_EQ(score.dynamic_at(3839), DynamicLevel::Piano);
    VCC_CHECK_EQ(score.dynamic_at((4 * kQuarter)), DynamicLevel::Forte);
    VCC_CHECK_EQ(score.dynamic_at(100000), DynamicLevel::Forte);
}

VCC_TEST(a_score_with_no_dynamics_still_has_one)
{
    // Silence is not a sensible default for an unmarked score.
    const Score score;
    VCC_CHECK_EQ(score.dynamic_at(0), DynamicLevel::MezzoForte);
}

VCC_TEST(a_second_dynamic_at_one_position_replaces_the_first)
{
    Score score;
    score.add_dynamic(DynamicMarking{kQuarter, DynamicLevel::Piano, 0, {}});
    score.add_dynamic(DynamicMarking{kQuarter, DynamicLevel::Fortissimo, 0, {}});

    VCC_CHECK_EQ(score.dynamics().size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(score.dynamic_at(kQuarter), DynamicLevel::Fortissimo);
}

VCC_TEST(hairpins_cover_their_span_and_no_more)
{
    // Acceptance criterion 6: a crescendo across a selected musical range.
    Score score;
    Hairpin crescendo;
    crescendo.start_tick = kQuarter;
    crescendo.end_tick = (4 * kQuarter);
    crescendo.kind = HairpinKind::Crescendo;
    score.add_hairpin(crescendo);

    VCC_CHECK(score.hairpin_at(959) == nullptr);
    VCC_CHECK(score.hairpin_at(kQuarter) != nullptr);
    VCC_CHECK(score.hairpin_at(3839) != nullptr);
    VCC_CHECK(score.hairpin_at((4 * kQuarter)) == nullptr);  // End is exclusive.
}

VCC_TEST(a_contradictory_hairpin_is_refused)
{
    // A crescendo that ends quieter than it starts is a contradiction;
    // rendering it backwards silently would be worse than refusing it.
    Score score;
    Hairpin backwards;
    backwards.start_tick = 0;
    backwards.end_tick = kQuarter;
    backwards.kind = HairpinKind::Crescendo;
    backwards.has_start_level = true;
    backwards.start_level = DynamicLevel::Forte;
    backwards.has_end_level = true;
    backwards.end_level = DynamicLevel::Piano;

    score.add_hairpin(backwards);
    VCC_CHECK_EQ(score.hairpins().size(), static_cast<std::size_t>(0));

    // Zero-length and reversed spans too.
    Hairpin empty;
    empty.start_tick = kQuarter;
    empty.end_tick = kQuarter;
    score.add_hairpin(empty);
    VCC_CHECK_EQ(score.hairpins().size(), static_cast<std::size_t>(0));
}

VCC_TEST(tempo_is_in_force_until_the_next_marking)
{
    Score score;
    VCC_CHECK_NEAR(score.tempo_at(0), 120.0, 1e-9);  // Default.

    TempoMarking allegro;
    allegro.tick = 0;
    allegro.beats_per_minute = 144.0;
    score.add_tempo(allegro);

    TempoMarking slower;
    slower.tick = (4 * kQuarter);
    slower.beats_per_minute = 72.0;
    score.add_tempo(slower);

    VCC_CHECK_NEAR(score.tempo_at(0), 144.0, 1e-9);
    VCC_CHECK_NEAR(score.tempo_at(3839), 144.0, 1e-9);
    VCC_CHECK_NEAR(score.tempo_at((4 * kQuarter)), 72.0, 1e-9);
}

VCC_TEST(a_compound_beat_unit_converts_to_quarter_notes)
{
    // "Dotted quarter = 60" in 6/8 is 90 quarter notes a minute. Keeping the
    // written form means the marking renders as written while the timeline
    // gets what it needs.
    TempoMarking marking;
    marking.beats_per_minute = 60.0;
    marking.beat_unit = NoteValue::Quarter;
    marking.beat_unit_dots = 1;

    VCC_CHECK_NEAR(marking.quarter_notes_per_minute(), 90.0, 1e-9);

    TempoMarking plain;
    plain.beats_per_minute = 120.0;
    VCC_CHECK_NEAR(plain.quarter_notes_per_minute(), 120.0, 1e-9);
}

VCC_TEST(the_revision_counter_advances_on_every_mutation)
{
    // Lets a UI or renderer skip work without comparing the whole score.
    Score score;
    const std::uint64_t start = score.revision();

    score.insert_note(0, note_at(0, Step::C));
    VCC_CHECK(score.revision() > start);

    const std::uint64_t after_insert = score.revision();
    score.add_dynamic(DynamicMarking{0, DynamicLevel::Forte, 0, {}});
    VCC_CHECK(score.revision() > after_insert);

    // A refused mutation must not bump it, or the signal is worthless.
    const std::uint64_t after_dynamic = score.revision();
    score.insert_note(99, note_at(0, Step::C));
    VCC_CHECK_EQ(score.revision(), after_dynamic);
}

VCC_TEST(end_tick_covers_notes_and_markings)
{
    Score score;
    score.insert_note(0, note_at(0, Step::C, 4, whole()));
    VCC_CHECK_EQ(score.end_tick(), static_cast<Ticks>((4 * kQuarter)));

    Hairpin hairpin;
    hairpin.start_tick = (4 * kQuarter);
    hairpin.end_tick = (8 * kQuarter);
    score.add_hairpin(hairpin);
    VCC_CHECK_EQ(score.end_tick(), static_cast<Ticks>((8 * kQuarter)));
}

VCC_TEST(transposition_is_reported_as_a_total)
{
    Staff clarinet;
    clarinet.transposition_semitones = -2;  // B-flat instrument.
    VCC_CHECK_EQ(clarinet.total_transposition(), -2);

    Staff guitar;
    guitar.octave_shift = -1;  // Treble 8vb.
    VCC_CHECK_EQ(guitar.total_transposition(), -12);

    Staff both;
    both.transposition_semitones = -2;
    both.octave_shift = -1;
    VCC_CHECK_EQ(both.total_transposition(), -14);
}

VCC_TEST(staff_access_out_of_range_throws_rather_than_corrupting)
{
    Score score;
    bool threw = false;
    try {
        (void)score.staff(5);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    VCC_CHECK(threw);
}

VCC_TEST(a_staff_can_be_taken_away_only_while_it_is_empty_and_its_slurs_go_with_it)
{
    Score score;
    score.make_grand_staff();
    Staff third;
    third.clef = Clef::Alto;
    score.add_staff(third);
    VCC_CHECK_EQ(score.staff_count(), std::size_t{3});

    ScoreNote note;
    note.start_tick = 0;
    note.duration = quarter();
    note.pitch = Pitch{Step::C, 3, 0};
    VCC_CHECK(score.insert_note(1, note) != kInvalidEventId);

    Slur on_second;
    on_second.start_tick = 0;
    on_second.end_tick = kQuarter;
    on_second.staff = 1;
    score.add_slur(on_second);
    Slur on_third = on_second;
    on_third.staff = 2;
    score.add_slur(on_third);

    // The staff with a note on it stays; the only staff of a one-staff
    // score stays; an index that is not there is refused.
    VCC_CHECK(!score.remove_staff(1));
    VCC_CHECK(!score.remove_staff(3));
    VCC_CHECK_EQ(score.staff_count(), std::size_t{3});

    // The empty top staff goes: its slur with it, the music below it kept,
    // and the slurs below it moved up a number so they still name their
    // staff.
    const std::uint64_t before = score.revision();
    VCC_CHECK(score.remove_staff(0));
    VCC_CHECK_EQ(score.staff_count(), std::size_t{2});
    VCC_CHECK_EQ(score.staff(0).clef, Clef::Bass);
    VCC_CHECK_EQ(score.staff(1).clef, Clef::Alto);
    VCC_CHECK_EQ(score.notes(0).size(), std::size_t{1});
    VCC_CHECK_EQ(score.slurs().size(), std::size_t{2});
    VCC_CHECK_EQ(score.slurs()[0].staff, std::uint8_t{0});
    VCC_CHECK_EQ(score.slurs()[1].staff, std::uint8_t{1});
    VCC_CHECK(score.revision() > before);

    Score one;
    VCC_CHECK(!one.remove_staff(0));

    // Replacing how a staff reads counts as a change; writing through the
    // reference does not, which is why set_staff exists.
    const std::uint64_t unchanged = one.revision();
    one.staff(0).clef = Clef::Bass;
    VCC_CHECK_EQ(one.revision(), unchanged);
    Staff reads;
    reads.clef = Clef::Tenor;
    reads.transposition_semitones = -2;
    VCC_CHECK(one.set_staff(0, reads));
    VCC_CHECK(one.revision() > unchanged);
    VCC_CHECK_EQ(one.staff(0).clef, Clef::Tenor);
    VCC_CHECK(!one.set_staff(1, reads));
}

VCC_TEST_MAIN("notation/Score")
