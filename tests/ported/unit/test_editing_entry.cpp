// Notation entry and the command bindings (spec 02 sections 5.2, 5.3, 8).
#include "vcc/editing/ScoreEditor.h"

#include <set>
#include <string>

#include "vcc_test.h"

using namespace vcc;
using namespace vcc::editing;

namespace {

constexpr notation::Ticks kQuarter = notation::kTicksPerQuarter;
constexpr notation::Ticks kBar = 4 * notation::kTicksPerQuarter;

notation::Pitch pitch_of(notation::Step step, int octave, int alteration = 0)
{
    notation::Pitch pitch;
    pitch.step = step;
    pitch.octave = static_cast<std::int8_t>(octave);
    pitch.alteration = static_cast<std::int8_t>(alteration);
    return pitch;
}

}  // namespace

VCC_TEST(entering_a_note_places_it_and_advances_past_it)
{
    notation::Score score;
    ScoreEditor editor(score);

    const EditResult result = editor.insert_pitch(pitch_of(notation::Step::C, 4));
    VCC_CHECK(result.succeeded());
    VCC_CHECK(result.score_changed);
    VCC_CHECK(result.new_event != notation::kInvalidEventId);

    VCC_CHECK_EQ(score.notes(0).size(), std::size_t{1});
    VCC_CHECK_EQ(score.notes(0).front().start_tick, notation::Ticks{0});
    VCC_CHECK_EQ(score.notes(0).front().duration.ticks(), kQuarter);

    // The cursor sits after the note, ready for the next one.
    VCC_CHECK_EQ(editor.cursor().tick, kQuarter);

    // And the note just entered is selected, so an articulation or a
    // dynamic applies to it without a second gesture.
    VCC_CHECK_EQ(editor.selection().size(), std::size_t{1});
    VCC_CHECK_EQ(editor.selection().front(), result.new_event);
}

VCC_TEST(grace_entry_writes_ornaments_before_the_note_typed_next)
{
    notation::Score score;
    ScoreEditor editor(score);

    // Two grace notes, then the note they ornament. The cursor stays for
    // the principal; each grace note is written as an eighth whatever the
    // selected value, dots and all, and takes its place after the one
    // before it.
    VCC_CHECK(editor.set_duration(notation::NoteValue::Half).succeeded());
    VCC_CHECK(editor.toggle_dot().succeeded());
    VCC_CHECK(editor.toggle_grace_entry().succeeded());
    VCC_CHECK(editor.cursor().grace_entry);
    const EditResult first = editor.insert_pitch(pitch_of(notation::Step::C, 5));
    VCC_CHECK(first.succeeded());
    VCC_CHECK_EQ(editor.cursor().tick, notation::Ticks{0});
    const EditResult second = editor.insert_pitch(pitch_of(notation::Step::D, 5));
    VCC_CHECK(second.succeeded());
    VCC_CHECK_EQ(editor.cursor().tick, notation::Ticks{0});
    VCC_CHECK_EQ(editor.selection().size(), std::size_t{1});

    // A rest cannot be a grace note, and the refusal says so rather than
    // only that the note was not one the model takes.
    VCC_CHECK(editor.toggle_rest_mode().succeeded());
    const EditResult rest = editor.insert_pitch(pitch_of(notation::Step::E, 5));
    VCC_CHECK(!rest.succeeded());
    VCC_CHECK_MSG(rest.message != nullptr && std::string(rest.message).find("rest") != std::string::npos,
                  rest.message != nullptr ? rest.message : "");
    VCC_CHECK(editor.toggle_rest_mode().succeeded());

    VCC_CHECK(editor.toggle_grace_entry().succeeded());
    VCC_CHECK(!editor.cursor().grace_entry);
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::E, 5)).succeeded());
    VCC_CHECK_EQ(editor.cursor().tick, kQuarter * 3);  // The dotted half.

    const std::vector<notation::ScoreNote>& notes = score.notes(0);
    VCC_CHECK_EQ(notes.size(), std::size_t{3});
    if (notes.size() != 3)
        return;
    VCC_CHECK(notes[0].grace && notes[0].grace_order == 0
              && notes[0].pitch.step == notation::Step::C);
    VCC_CHECK(notes[1].grace && notes[1].grace_order == 1
              && notes[1].pitch.step == notation::Step::D);
    VCC_CHECK(!notes[2].grace && notes[2].pitch.step == notation::Step::E);
    for (std::size_t index = 0; index < 2; ++index) {
        VCC_CHECK(notes[index].duration.value == notation::NoteValue::Eighth);
        VCC_CHECK_EQ(notes[index].duration.dots, std::uint8_t{0});
        VCC_CHECK(notes[index].grace_slashed);
        VCC_CHECK_EQ(notes[index].start_tick, notation::Ticks{0});
    }

    // Writing the principal did not take the grace notes with it, and
    // undoing the principal leaves them too.
    VCC_CHECK(editor.undo().succeeded());
    VCC_CHECK_EQ(score.notes(0).size(), std::size_t{2});
    VCC_CHECK(editor.undo().succeeded());
    VCC_CHECK_EQ(score.notes(0).size(), std::size_t{1});

    // The undo put the cursor back as it was when that grace note was
    // entered - in grace entry - which is where the next one goes. A
    // sixteenth selected is kept: shorter than an eighth is a value a
    // grace note is written in.
    VCC_CHECK(editor.cursor().grace_entry);
    VCC_CHECK(editor.set_duration(notation::NoteValue::Sixteenth).succeeded());
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::F, 5)).succeeded());
    VCC_CHECK(score.notes(0).back().grace
              && score.notes(0).back().duration.value == notation::NoteValue::Sixteenth);
    VCC_CHECK_EQ(score.notes(0).back().grace_order, std::uint8_t{1});
}

VCC_TEST(a_grace_note_displaces_nothing_already_written)
{
    // A quarter is there; a grace note entered before it is written
    // before it, and the quarter stays - a grace note takes no time, so
    // its written value clears no span.
    notation::Score score;
    ScoreEditor editor(score);
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::E, 5)).succeeded());
    EditCursor cursor = editor.cursor();
    cursor.tick = 0;
    editor.set_cursor(cursor);
    VCC_CHECK(editor.toggle_grace_entry().succeeded());
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::D, 5)).succeeded());

    VCC_CHECK_EQ(score.notes(0).size(), std::size_t{2});
    if (score.notes(0).size() == 2) {
        VCC_CHECK(score.notes(0)[0].grace && score.notes(0)[0].pitch.step == notation::Step::D);
        VCC_CHECK(!score.notes(0)[1].grace && score.notes(0)[1].pitch.step == notation::Step::E);
    }
}

VCC_TEST(cross_staff_draws_a_chord_on_the_other_staff_and_back)
{
    // On one staff there is nowhere to cross to. On a grand staff the
    // targeted chord moves whole - a chord split across the staves is not
    // drawn - and a second press puts it back. The notes never leave
    // their staff in the score, and the move undoes.
    notation::Score single;
    ScoreEditor alone(single);
    VCC_CHECK(alone.insert_pitch(pitch_of(notation::Step::C, 4)).succeeded());
    const EditResult nowhere = alone.cross_staff(1);
    VCC_CHECK(!nowhere.succeeded());
    VCC_CHECK_MSG(nowhere.message != nullptr && std::string(nowhere.message).find("below") != std::string::npos,
                  nowhere.message != nullptr ? nowhere.message : "");
    VCC_CHECK_EQ(single.notes(0).front().staff_shift, std::int8_t{0});
    VCC_CHECK(!alone.cross_staff(-1).succeeded());

    notation::Score score;
    score.make_grand_staff();
    ScoreEditor editor(score);
    const std::vector<notation::EventId> chord =
        score.insert_chord(0, 0, notation::quarter(),
                           {pitch_of(notation::Step::C, 4), pitch_of(notation::Step::E, 4)}, 0);
    VCC_CHECK_EQ(chord.size(), std::size_t{2});
    EditCursor cursor = editor.cursor();
    cursor.tick = kQuarter;
    editor.set_cursor(cursor);
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::G, 4)).succeeded());

    // One note of the chord selected: the whole chord crosses, the G after
    // it does not.
    editor.set_selection({chord.front()});
    const EditResult down = editor.cross_staff(1);
    VCC_CHECK(down.succeeded());
    VCC_CHECK_EQ(down.notes_changed, std::size_t{2});
    for (const notation::ScoreNote& note : score.notes(0)) {
        if (note.start_tick == 0)
            VCC_CHECK_EQ(note.staff_shift, std::int8_t{1});
        else
            VCC_CHECK_EQ(note.staff_shift, std::int8_t{0});
    }
    VCC_CHECK_EQ(score.notes(1).size(), std::size_t{0});
    VCC_CHECK_EQ(score.staff_index_of(chord.front()), std::size_t{0});

    // Up from the top staff: nowhere - unless the note is drawn below and
    // is coming back, which a second press down also does.
    VCC_CHECK(!editor.cross_staff(-1).succeeded());
    const EditResult back = editor.cross_staff(1);
    VCC_CHECK(back.succeeded());
    for (const notation::ScoreNote& note : score.notes(0))
        VCC_CHECK_EQ(note.staff_shift, std::int8_t{0});

    VCC_CHECK(editor.undo().succeeded());
    VCC_CHECK_EQ(score.find_note(chord.front())->staff_shift, std::int8_t{1});
    VCC_CHECK(editor.undo().succeeded());
    VCC_CHECK_EQ(score.find_note(chord.front())->staff_shift, std::int8_t{0});

    // From the bottom staff, up works and down does not.
    cursor = editor.cursor();
    cursor.staff_index = 1;
    cursor.tick = 0;
    editor.set_cursor(cursor);
    editor.clear_selection();
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::C, 3)).succeeded());
    VCC_CHECK(!editor.cross_staff(1).succeeded());
    VCC_CHECK(editor.cross_staff(-1).succeeded());
    VCC_CHECK_EQ(score.notes(1).front().staff_shift, std::int8_t{-1});
}

VCC_TEST(a_dynamic_and_a_hairpin_take_the_staff_the_cursor_is_on)
{
    notation::Score score;
    score.make_grand_staff();
    ScoreEditor editor(score);
    EditCursor cursor = editor.cursor();
    cursor.staff_index = 1;
    editor.set_cursor(cursor);

    VCC_CHECK(editor.add_dynamic(notation::DynamicLevel::Piano).succeeded());
    VCC_CHECK(editor.start_hairpin(notation::HairpinKind::Crescendo).succeeded());
    VCC_CHECK_EQ(score.dynamics().front().staff, std::uint8_t{1});
    VCC_CHECK_EQ(score.hairpins().front().staff, std::uint8_t{1});

    // The same tick on the other staff is another marking, not a
    // replacement; and undo takes the right one away.
    cursor.staff_index = 0;
    editor.set_cursor(cursor);
    VCC_CHECK(editor.add_dynamic(notation::DynamicLevel::Forte).succeeded());
    VCC_CHECK_EQ(score.dynamics().size(), std::size_t{2});
    VCC_CHECK(editor.undo().succeeded());
    VCC_CHECK_EQ(score.dynamics().size(), std::size_t{1});
    VCC_CHECK_EQ(score.dynamics().front().staff, std::uint8_t{1});

    // Pasted music carries its markings onto the staves its staves land
    // on: a two-staff source at the cursor's staff 0 puts the second
    // staff's marking on staff 1.
    notation::Score source;
    source.make_grand_staff();
    notation::DynamicMarking second{0, notation::DynamicLevel::Fortissimo, 0, ""};
    second.staff = 1;
    source.add_dynamic(second);
    notation::ScoreNote note;
    note.start_tick = 0;
    note.duration = notation::quarter();
    note.pitch = pitch_of(notation::Step::C, 3);
    source.insert_note(1, note);
    cursor.staff_index = 0;
    cursor.tick = 4 * kQuarter;
    editor.set_cursor(cursor);
    VCC_CHECK(editor.paste_score(source).succeeded());
    bool pasted = false;
    for (const notation::DynamicMarking& marking : score.dynamics())
        pasted = pasted || (marking.tick == 4 * kQuarter && marking.staff == 1
                            && marking.level == notation::DynamicLevel::Fortissimo);
    VCC_CHECK(pasted);

    // And counted from the cursor's staff: the same source's first-staff
    // marking, pasted with the cursor on staff 1, lands on staff 1.
    notation::Score upper_only;
    upper_only.make_grand_staff();
    upper_only.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::Pianissimo, 0, ""});
    upper_only.insert_note(0, note);
    cursor.staff_index = 1;
    cursor.tick = 8 * kQuarter;
    editor.set_cursor(cursor);
    VCC_CHECK(editor.paste_score(upper_only).succeeded());
    bool moved = false;
    for (const notation::DynamicMarking& marking : score.dynamics())
        moved = moved || (marking.tick == 8 * kQuarter && marking.staff == 1
                          && marking.level == notation::DynamicLevel::Pianissimo);
    VCC_CHECK(moved);
}

VCC_TEST(entry_spells_a_step_using_the_key_in_force)
{
    notation::Score score;
    VCC_CHECK(score.set_key_signature(0, notation::KeySignature{2, false}));  // D major.
    ScoreEditor editor(score);

    VCC_CHECK(editor.insert_step(notation::Step::F).succeeded());
    VCC_CHECK(editor.insert_step(notation::Step::G).succeeded());

    // F is sharp in D major; G is not.
    VCC_CHECK_EQ(score.notes(0)[0].pitch.step, notation::Step::F);
    VCC_CHECK_EQ(score.notes(0)[0].pitch.alteration, 1);
    VCC_CHECK_EQ(score.notes(0)[1].pitch.alteration, 0);
}

VCC_TEST(a_pending_accidental_applies_to_one_note_and_then_clears)
{
    notation::Score score;
    ScoreEditor editor(score);

    // Pressed before the note, as keyboard entry works.
    VCC_CHECK(editor.set_accidental(1).succeeded());
    VCC_CHECK(editor.cursor().has_pending_accidental);

    VCC_CHECK(editor.insert_step(notation::Step::F).succeeded());
    VCC_CHECK_EQ(score.notes(0)[0].pitch.alteration, 1);
    VCC_CHECK(!editor.cursor().has_pending_accidental);

    // The next F is natural: a sharp pressed once does not sharpen
    // everything typed afterwards.
    VCC_CHECK(editor.insert_step(notation::Step::F).succeeded());
    VCC_CHECK_EQ(score.notes(0)[1].pitch.alteration, 0);
}

VCC_TEST(entry_overwrites_what_it_lands_on)
{
    notation::Score score;
    ScoreEditor editor(score);

    VCC_CHECK(editor.set_duration(notation::NoteValue::Whole).succeeded());
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::C, 4)).succeeded());
    VCC_CHECK_EQ(score.notes(0).size(), std::size_t{1});

    // Back to the start with a shorter value: the whole note goes, rather
    // than being split into a tie chain the composer did not write.
    EditCursor cursor = editor.cursor();
    cursor.tick = 0;
    cursor.duration = notation::quarter();
    editor.set_cursor(cursor);

    const EditResult result = editor.insert_pitch(pitch_of(notation::Step::D, 4));
    VCC_CHECK(result.succeeded());
    VCC_CHECK_EQ(result.notes_changed, std::size_t{2});  // One removed, one added.
    VCC_CHECK_EQ(score.notes(0).size(), std::size_t{1});
    VCC_CHECK_EQ(score.notes(0).front().pitch.step, notation::Step::D);
}

VCC_TEST(entry_leaves_notes_it_does_not_overlap_alone)
{
    notation::Score score;
    ScoreEditor editor(score);

    for (int index = 0; index < 4; ++index)
        VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::C, 4)).succeeded());
    VCC_CHECK_EQ(score.notes(0).size(), std::size_t{4});

    EditCursor cursor = editor.cursor();
    cursor.tick = kQuarter;
    editor.set_cursor(cursor);
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::E, 4)).succeeded());

    // Only the note at that beat was replaced.
    VCC_CHECK_EQ(score.notes(0).size(), std::size_t{4});
    VCC_CHECK_EQ(score.notes(0)[1].pitch.step, notation::Step::E);
}

VCC_TEST(rest_mode_enters_rests)
{
    notation::Score score;
    ScoreEditor editor(score);

    VCC_CHECK(editor.toggle_rest_mode().succeeded());
    VCC_CHECK(editor.cursor().rest_mode);

    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::C, 4)).succeeded());
    VCC_CHECK(score.notes(0).front().is_rest);

    VCC_CHECK(editor.toggle_rest_mode().succeeded());
    VCC_CHECK(!editor.cursor().rest_mode);

    // And a one-off rest does not leave rest mode on.
    VCC_CHECK(editor.insert_rest().succeeded());
    VCC_CHECK(score.notes(0)[1].is_rest);
    VCC_CHECK(!editor.cursor().rest_mode);
}

VCC_TEST(a_chord_is_built_on_the_note_just_entered_without_advancing)
{
    notation::Score score;
    ScoreEditor editor(score);

    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::C, 4)).succeeded());
    const notation::Ticks after_first = editor.cursor().tick;

    VCC_CHECK(editor.add_to_chord(pitch_of(notation::Step::E, 4)).succeeded());
    VCC_CHECK(editor.add_to_chord(pitch_of(notation::Step::G, 4)).succeeded());

    VCC_CHECK_EQ(editor.cursor().tick, after_first);
    VCC_CHECK_EQ(score.notes(0).size(), std::size_t{3});
    for (const notation::ScoreNote& note : score.notes(0)) {
        VCC_CHECK_EQ(note.start_tick, notation::Ticks{0});
        VCC_CHECK_EQ(note.duration.ticks(), kQuarter);
        // The whole chord is marked, not just the pitches added after the
        // first.
        VCC_CHECK(note.in_chord);
    }

    // The same pitch twice is not a chord member.
    VCC_CHECK_EQ(editor.add_to_chord(pitch_of(notation::Step::E, 4)).status,
                 EditStatus::NothingToDo);
}

VCC_TEST(a_chord_by_step_takes_the_octave_and_the_key_and_a_grace_note_makes_a_grace_chord)
{
    // In D major the letter F spells F-sharp, in the entry octave, on the
    // note just entered. After a grace note, the chord is that grace
    // note's: the new note is a grace note of the same order, and the
    // principal typed afterwards is untouched.
    notation::Score score;
    VCC_CHECK(score.set_key_signature(0, notation::KeySignature{2, false}));
    ScoreEditor editor(score);

    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::D, 4)).succeeded());
    VCC_CHECK(editor.add_step_to_chord(notation::Step::F).succeeded());
    VCC_CHECK_EQ(score.notes(0).size(), std::size_t{2});
    VCC_CHECK(score.notes(0).back().pitch == pitch_of(notation::Step::F, 4, 1));
    VCC_CHECK(score.notes(0).front().in_chord && score.notes(0).back().in_chord);

    // A grace chord before the next beat.
    VCC_CHECK(editor.toggle_grace_entry().succeeded());
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::A, 4)).succeeded());
    VCC_CHECK(editor.add_step_to_chord(notation::Step::C).succeeded());
    VCC_CHECK(editor.toggle_grace_entry().succeeded());
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::D, 5)).succeeded());

    std::size_t grace_members = 0;
    for (const notation::ScoreNote& note : score.notes(0)) {
        if (note.start_tick != kQuarter)
            continue;
        if (note.grace) {
            ++grace_members;
            VCC_CHECK(note.in_chord);
            VCC_CHECK_EQ(note.grace_order, std::uint8_t{0});
            VCC_CHECK(note.grace_slashed);
        } else {
            VCC_CHECK(!note.in_chord);
            VCC_CHECK(note.pitch == pitch_of(notation::Step::D, 5));
        }
    }
    VCC_CHECK_EQ(grace_members, std::size_t{2});
    // The C was spelled sharp, in the key.
    bool c_sharp = false;
    for (const notation::ScoreNote& note : score.notes(0))
        c_sharp = c_sharp || (note.grace && note.pitch == pitch_of(notation::Step::C, 4, 1));
    VCC_CHECK(c_sharp);

    // A grace note written before a note that is already there: the chord
    // built on it is the grace note's alone - the principal at the same
    // tick is neither joined nor marked, and the new member is written in
    // the grace note's value, not the principal's.
    EditCursor cursor = editor.cursor();
    cursor.tick = 2 * kQuarter;
    editor.set_cursor(cursor);
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::G, 5)).succeeded());
    cursor.tick = 2 * kQuarter;
    editor.set_cursor(cursor);
    VCC_CHECK(editor.toggle_grace_entry().succeeded());
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::E, 5)).succeeded());
    VCC_CHECK(editor.add_step_to_chord(notation::Step::G).succeeded());
    VCC_CHECK(editor.toggle_grace_entry().succeeded());
    for (const notation::ScoreNote& note : score.notes(0)) {
        if (note.start_tick != 2 * kQuarter)
            continue;
        if (note.grace) {
            VCC_CHECK(note.in_chord);
            VCC_CHECK(note.duration.value == notation::NoteValue::Eighth);
        } else {
            VCC_CHECK(!note.in_chord);
            VCC_CHECK(note.pitch == pitch_of(notation::Step::G, 5));
        }
    }
}

VCC_TEST(the_slash_comes_off_the_selected_grace_notes_and_goes_back_on)
{
    notation::Score score;
    ScoreEditor editor(score);
    VCC_CHECK(editor.toggle_grace_entry().succeeded());
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::B, 4)).succeeded());
    VCC_CHECK(editor.toggle_grace_entry().succeeded());
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::C, 5)).succeeded());

    // The principal is selected now; it is no grace note, so nothing.
    VCC_CHECK_EQ(editor.toggle_grace_slash().status, EditStatus::NothingToDo);

    const notation::EventId grace = score.notes(0).front().id;
    VCC_CHECK(score.find_note(grace)->grace);
    editor.set_selection({grace});
    VCC_CHECK(editor.toggle_grace_slash().succeeded());
    VCC_CHECK(!score.find_note(grace)->grace_slashed);
    VCC_CHECK(editor.toggle_grace_slash().succeeded());
    VCC_CHECK(score.find_note(grace)->grace_slashed);
    VCC_CHECK(editor.undo().succeeded());
    VCC_CHECK(!score.find_note(grace)->grace_slashed);

    editor.clear_selection();
    EditCursor cursor = editor.cursor();
    cursor.tick = 2 * kQuarter;
    editor.set_cursor(cursor);
    VCC_CHECK_EQ(editor.toggle_grace_slash().status, EditStatus::NothingToDo);
}

VCC_TEST(a_chord_cannot_be_started_before_a_note_exists)
{
    notation::Score score;
    ScoreEditor editor(score);
    VCC_CHECK_EQ(editor.add_to_chord(pitch_of(notation::Step::C, 4)).status,
                 EditStatus::NothingToDo);
}

VCC_TEST(mouse_entry_places_a_note_where_the_click_landed)
{
    notation::Score score;
    score.make_grand_staff();
    ScoreEditor editor(score);

    const EditResult result = editor.insert_at(1, (2 * kQuarter), pitch_of(notation::Step::G, 2));
    VCC_CHECK(result.succeeded());

    VCC_CHECK_EQ(score.notes(1).size(), std::size_t{1});
    VCC_CHECK_EQ(score.notes(1).front().start_tick, notation::Ticks{(2 * kQuarter)});

    // Entry continues from the click, which is what makes clicking then
    // typing work.
    VCC_CHECK_EQ(editor.cursor().staff_index, std::size_t{1});
    VCC_CHECK_EQ(editor.cursor().tick, (2 * kQuarter) + kQuarter);

    VCC_CHECK_EQ(editor.insert_at(9, 0, pitch_of(notation::Step::C, 4)).status,
                 EditStatus::Refused);
}

// --- Durations --------------------------------------------------------------

VCC_TEST(choosing_a_duration_clears_the_dots_and_tuplet)
{
    notation::Score score;
    ScoreEditor editor(score);

    VCC_CHECK(editor.toggle_dot().succeeded());
    VCC_CHECK(editor.toggle_tuplet(notation::Tuplet{3, 2}).succeeded());
    VCC_CHECK_EQ(editor.cursor().duration.dots, std::uint8_t{1});
    VCC_CHECK(editor.cursor().duration.tuplet.is_tuplet());

    // The dot is pressed after the value it modifies, so choosing a value
    // starts from a plain note.
    VCC_CHECK(editor.set_duration(notation::NoteValue::Eighth).succeeded());
    VCC_CHECK_EQ(editor.cursor().duration.dots, std::uint8_t{0});
    VCC_CHECK(!editor.cursor().duration.tuplet.is_tuplet());
    VCC_CHECK_EQ(editor.cursor().duration.ticks(), kQuarter / 2);
}

VCC_TEST(dots_and_tuplets_change_the_length_entered)
{
    notation::Score score;
    ScoreEditor editor(score);

    VCC_CHECK(editor.toggle_dot().succeeded());
    VCC_CHECK_EQ(editor.cursor().duration.ticks(), kQuarter + kQuarter / 2);

    VCC_CHECK(editor.toggle_dot().succeeded());  // Off again.
    VCC_CHECK_EQ(editor.cursor().duration.ticks(), kQuarter);

    VCC_CHECK(editor.toggle_double_dot().succeeded());
    VCC_CHECK_EQ(editor.cursor().duration.ticks(), kQuarter + kQuarter / 2 + kQuarter / 4);

    VCC_CHECK(editor.set_duration(notation::NoteValue::Quarter).succeeded());
    VCC_CHECK(editor.toggle_tuplet(notation::Tuplet{3, 2}).succeeded());
    VCC_CHECK_EQ(editor.cursor().duration.ticks(), kQuarter * 2 / 3);

    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::C, 4)).succeeded());
    VCC_CHECK_EQ(score.notes(0).front().duration.ticks(), kQuarter * 2 / 3);
}

VCC_TEST(a_tuplet_can_be_nested_inside_the_tuplet_being_written)
{
    notation::Score score;
    ScoreEditor editor(score);

    // There has to be a tuplet to nest inside.
    const EditResult alone = editor.toggle_nested_tuplet();
    VCC_CHECK_EQ(alone.status, EditStatus::Refused);
    VCC_CHECK(std::string(alone.message).find("tuplet first") != std::string::npos);

    VCC_CHECK(editor.toggle_tuplet(notation::Tuplet{3, 2}).succeeded());
    VCC_CHECK(editor.toggle_nested_tuplet().succeeded());
    VCC_CHECK(editor.cursor().duration.is_nested());
    VCC_CHECK_EQ(editor.cursor().duration.ticks(), 4 * kQuarter / 9);

    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::C, 4)).succeeded());
    VCC_CHECK_EQ(score.notes(0).front().duration.ticks(), 4 * kQuarter / 9);
    VCC_CHECK(score.notes(0).front().duration.outer == notation::Tuplet{3, 2});

    // Off again by the same control; and turning the tuplet itself off
    // takes the nesting with it, since there is nothing left to nest in.
    VCC_CHECK(editor.toggle_nested_tuplet().succeeded());
    VCC_CHECK(!editor.cursor().duration.is_nested());
    VCC_CHECK_EQ(editor.cursor().duration.ticks(), 2 * kQuarter / 3);
    VCC_CHECK(editor.toggle_nested_tuplet().succeeded());
    VCC_CHECK(editor.toggle_tuplet(notation::Tuplet{3, 2}).succeeded());
    VCC_CHECK(!editor.cursor().duration.tuplet.is_tuplet());
    VCC_CHECK(!editor.cursor().duration.outer.is_tuplet());

    // Choosing a value starts from a plain note, nesting included. (With
    // nothing selected: with the note just entered still selected, the
    // value would retime it instead, keeping its tuplets.)
    editor.clear_selection();
    VCC_CHECK(editor.toggle_tuplet(notation::Tuplet{3, 2}).succeeded());
    VCC_CHECK(editor.toggle_nested_tuplet().succeeded());
    VCC_CHECK(editor.set_duration(notation::NoteValue::Eighth).succeeded());
    VCC_CHECK(!editor.cursor().duration.outer.is_tuplet());

    // A nesting that cannot be written exactly is refused and nothing
    // changes: a quintuplet eighth divides on its own, and not inside a
    // quintuplet.
    VCC_CHECK(editor.toggle_tuplet(notation::Tuplet{5, 4}).succeeded());
    VCC_CHECK_EQ(editor.cursor().duration.ticks(), 2 * kQuarter / 5);
    const EditResult inexact = editor.toggle_nested_tuplet();
    VCC_CHECK_EQ(inexact.status, EditStatus::Refused);
    VCC_CHECK(!editor.cursor().duration.outer.is_tuplet());
    VCC_CHECK_EQ(editor.cursor().duration.ticks(), 2 * kQuarter / 5);
}

VCC_TEST(a_duration_that_cannot_be_written_exactly_is_refused)
{
    notation::Score score;
    ScoreEditor editor(score);

    VCC_CHECK(editor.set_duration(notation::NoteValue::SixtyFourth).succeeded());

    // A triple-dotted sixty-fourth is 112.5 ticks at this resolution, so it
    // is not a duration this model can write - and refusing is better than
    // rounding a rhythm the composer typed.
    const EditResult result = editor.set_dots(3);
    VCC_CHECK_EQ(result.status, EditStatus::Refused);
    VCC_CHECK_EQ(editor.cursor().duration.dots, std::uint8_t{0});

    VCC_CHECK_EQ(editor.set_dots(-1).status, EditStatus::Refused);
    VCC_CHECK_EQ(editor.set_dots(4).status, EditStatus::Refused);
}

VCC_TEST(choosing_a_duration_with_a_selection_retimes_it)
{
    notation::Score score;
    ScoreEditor editor(score);

    const EditResult entered = editor.insert_pitch(pitch_of(notation::Step::C, 4));
    VCC_CHECK(entered.succeeded());

    // The note just entered is selected, so a duration key retimes it.
    const EditResult retimed = editor.set_duration(notation::NoteValue::Half);
    VCC_CHECK(retimed.succeeded());
    VCC_CHECK(retimed.score_changed);
    VCC_CHECK_EQ(score.notes(0).front().duration.ticks(), kQuarter * 2);
    VCC_CHECK_EQ(score.notes(0).front().id, entered.new_event);
}

// --- Accidentals and pitch --------------------------------------------------

VCC_TEST(an_accidental_with_a_selection_respells_it)
{
    notation::Score score;
    ScoreEditor editor(score);

    const EditResult entered = editor.insert_pitch(pitch_of(notation::Step::F, 4));
    VCC_CHECK(entered.succeeded());

    const EditResult sharpened = editor.set_accidental(1);
    VCC_CHECK(sharpened.succeeded());
    VCC_CHECK(sharpened.score_changed);
    VCC_CHECK_EQ(score.notes(0).front().pitch.alteration, 1);

    // And the note keeps its identity, so the host sees a change rather
    // than a deletion and an addition.
    VCC_CHECK_EQ(score.notes(0).front().id, entered.new_event);

    VCC_CHECK_EQ(editor.set_accidental(3).status, EditStatus::Refused);
}

VCC_TEST(respelling_keeps_the_sound_and_changes_the_spelling)
{
    notation::Score score;
    ScoreEditor editor(score);

    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::F, 4, 1)).succeeded());
    const int before = notation::midi_note_number(score.notes(0).front().pitch);

    VCC_CHECK(editor.cycle_enharmonic().succeeded());

    const notation::Pitch after = score.notes(0).front().pitch;
    VCC_CHECK_EQ(notation::midi_note_number(after), before);
    VCC_CHECK_EQ(after.step, notation::Step::G);
    VCC_CHECK_EQ(after.alteration, -1);
}

VCC_TEST(transposing_by_a_step_follows_the_key)
{
    notation::Score score;
    VCC_CHECK(score.set_key_signature(0, notation::KeySignature{2, false}));  // D major.
    ScoreEditor editor(score);

    VCC_CHECK(editor.insert_step(notation::Step::F).succeeded());  // F sharp.
    VCC_CHECK_EQ(score.notes(0).front().pitch.alteration, 1);

    VCC_CHECK(editor.transpose_selection(1).succeeded());

    // A step up from F sharp in D major is G, not G flat.
    VCC_CHECK_EQ(score.notes(0).front().pitch.step, notation::Step::G);
    VCC_CHECK_EQ(score.notes(0).front().pitch.alteration, 0);

    VCC_CHECK(editor.transpose_selection(-1).succeeded());
    VCC_CHECK_EQ(score.notes(0).front().pitch.step, notation::Step::F);
    VCC_CHECK_EQ(score.notes(0).front().pitch.alteration, 1);
}

VCC_TEST(transposing_by_octaves_keeps_the_spelling)
{
    notation::Score score;
    ScoreEditor editor(score);

    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::E, 4, -1)).succeeded());
    VCC_CHECK(editor.transpose_selection_octaves(1).succeeded());

    VCC_CHECK_EQ(score.notes(0).front().pitch.step, notation::Step::E);
    VCC_CHECK_EQ(score.notes(0).front().pitch.alteration, -1);
    VCC_CHECK_EQ(score.notes(0).front().pitch.octave, 5);
}

VCC_TEST(a_transposition_out_of_midi_range_is_refused_rather_than_wrapping)
{
    notation::Score score;
    ScoreEditor editor(score);

    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::C, -1)).succeeded());
    const notation::Pitch before = score.notes(0).front().pitch;

    VCC_CHECK_EQ(editor.transpose_selection_octaves(-1).status, EditStatus::NothingToDo);
    VCC_CHECK(score.notes(0).front().pitch == before);

    // And nothing was recorded, so undo does not have a no-op step in it.
    VCC_CHECK_EQ(editor.history().undo_depth(), std::size_t{1});  // Just the entry.
}

VCC_TEST(the_entry_octave_moves_when_nothing_is_selected)
{
    notation::Score score;
    ScoreEditor editor(score);

    const std::int8_t before = editor.cursor().entry_octave;
    const EditResult result = editor.apply(composer::Command::PitchUpOctave);
    VCC_CHECK(result.succeeded());
    VCC_CHECK(!result.score_changed);
    VCC_CHECK_EQ(editor.cursor().entry_octave, before + 1);

    VCC_CHECK(editor.insert_step(notation::Step::C).succeeded());
    VCC_CHECK_EQ(score.notes(0).front().pitch.octave, before + 1);
}

// --- Articulations, ties and markings ---------------------------------------

VCC_TEST(articulations_toggle_on_the_selection)
{
    notation::Score score;
    ScoreEditor editor(score);
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::C, 4)).succeeded());

    VCC_CHECK(editor.toggle_articulation(notation::Articulation::Staccato).succeeded());
    VCC_CHECK(notation::has_articulation(score.notes(0).front().articulations,
                                         notation::Articulation::Staccato));

    // A note can carry more than one.
    VCC_CHECK(editor.toggle_articulation(notation::Articulation::Accent).succeeded());
    VCC_CHECK(notation::has_articulation(score.notes(0).front().articulations,
                                         notation::Articulation::Staccato));
    VCC_CHECK(notation::has_articulation(score.notes(0).front().articulations,
                                         notation::Articulation::Accent));

    VCC_CHECK(editor.toggle_articulation(notation::Articulation::Staccato).succeeded());
    VCC_CHECK(!notation::has_articulation(score.notes(0).front().articulations,
                                          notation::Articulation::Staccato));
}

VCC_TEST(a_command_with_no_selection_acts_on_the_note_at_the_cursor)
{
    notation::Score score;
    ScoreEditor editor(score);

    VCC_CHECK(editor.set_duration(notation::NoteValue::Whole).succeeded());
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::C, 4)).succeeded());
    editor.clear_selection();

    // The cursor is inside the whole note, which is the note being looked
    // at even though nothing is selected.
    EditCursor cursor = editor.cursor();
    cursor.tick = kQuarter;
    editor.set_cursor(cursor);

    VCC_CHECK_EQ(editor.notes_at_cursor().size(), std::size_t{1});
    VCC_CHECK(editor.toggle_articulation(notation::Articulation::Tenuto).succeeded());
    VCC_CHECK(notation::has_articulation(score.notes(0).front().articulations,
                                         notation::Articulation::Tenuto));
}

VCC_TEST(a_tie_can_be_pressed_before_or_after_the_note)
{
    notation::Score score;
    ScoreEditor editor(score);

    // Before: sets the entry flag, and the next note carries it.
    const EditResult armed = editor.toggle_tie();
    VCC_CHECK(armed.succeeded());
    VCC_CHECK(!armed.score_changed);
    VCC_CHECK(editor.cursor().tie_to_next);

    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::C, 4)).succeeded());
    VCC_CHECK(score.notes(0).front().tied_to_next);
    // Consumed, so the whole passage is not tied together.
    VCC_CHECK(!editor.cursor().tie_to_next);

    // After: toggles it on the selected note.
    VCC_CHECK(editor.toggle_tie().succeeded());
    VCC_CHECK(!score.notes(0).front().tied_to_next);
}

VCC_TEST(a_rest_cannot_be_tied)
{
    notation::Score score;
    ScoreEditor editor(score);
    VCC_CHECK(editor.insert_rest().succeeded());
    VCC_CHECK_EQ(editor.toggle_tie().status, EditStatus::NothingToDo);
}

VCC_TEST(dynamics_are_written_at_the_cursor)
{
    notation::Score score;
    ScoreEditor editor(score);

    EditCursor cursor = editor.cursor();
    cursor.tick = (2 * kQuarter);
    editor.set_cursor(cursor);

    VCC_CHECK(editor.add_dynamic(notation::DynamicLevel::Forte).succeeded());
    VCC_CHECK_EQ(score.dynamics().size(), std::size_t{1});
    VCC_CHECK_EQ(score.dynamics().front().tick, notation::Ticks{(2 * kQuarter)});
    VCC_CHECK_EQ(score.dynamic_at((2 * kQuarter)), notation::DynamicLevel::Forte);

    // The same one again is not an edit.
    VCC_CHECK_EQ(editor.add_dynamic(notation::DynamicLevel::Forte).status,
                 EditStatus::NothingToDo);

    // A different one replaces it rather than stacking two at one tick.
    VCC_CHECK(editor.add_dynamic(notation::DynamicLevel::Piano).succeeded());
    VCC_CHECK_EQ(score.dynamics().size(), std::size_t{1});
    VCC_CHECK_EQ(score.dynamic_at((2 * kQuarter)), notation::DynamicLevel::Piano);
}

VCC_TEST(a_hairpin_starts_at_the_cursor_and_stretches_by_the_entry_duration)
{
    notation::Score score;
    ScoreEditor editor(score);

    VCC_CHECK(editor.start_hairpin(notation::HairpinKind::Crescendo).succeeded());
    VCC_CHECK_EQ(score.hairpins().size(), std::size_t{1});
    VCC_CHECK_EQ(score.hairpins().front().start_tick, notation::Ticks{0});
    VCC_CHECK_EQ(score.hairpins().front().end_tick, kQuarter);

    // Spec 5.3: stretched across beats and measures.
    VCC_CHECK(editor.extend_span(3).succeeded());
    VCC_CHECK_EQ(score.hairpins().front().end_tick, kQuarter * 4);

    VCC_CHECK(editor.extend_span(-1).succeeded());
    VCC_CHECK_EQ(score.hairpins().front().end_tick, kQuarter * 3);

    // It cannot be shortened past its own start.
    VCC_CHECK_EQ(editor.extend_span(-5).status, EditStatus::Refused);
    VCC_CHECK_EQ(score.hairpins().front().end_tick, kQuarter * 3);
}

VCC_TEST(a_slur_stretches_the_same_way)
{
    notation::Score score;
    ScoreEditor editor(score);

    VCC_CHECK(editor.start_slur().succeeded());
    VCC_CHECK_EQ(score.slurs().size(), std::size_t{1});
    VCC_CHECK(editor.extend_span(2).succeeded());
    VCC_CHECK_EQ(score.slurs().front().end_tick, kQuarter * 3);
    VCC_CHECK_EQ(score.slurs().size(), std::size_t{1});
}

VCC_TEST(a_slur_started_on_the_second_staff_belongs_to_it)
{
    notation::Score score;
    score.make_grand_staff();
    ScoreEditor editor(score);

    EditCursor cursor = editor.cursor();
    cursor.staff_index = 1;
    editor.set_cursor(cursor);

    VCC_CHECK(editor.start_slur().succeeded());
    VCC_CHECK_EQ(score.slurs().size(), std::size_t{1});
    VCC_CHECK_EQ(score.slurs().front().staff, static_cast<std::uint8_t>(1));
}

VCC_TEST(there_is_nothing_to_stretch_before_a_span_is_started)
{
    notation::Score score;
    ScoreEditor editor(score);
    VCC_CHECK_EQ(editor.extend_span(1).status, EditStatus::NothingToDo);
}

// --- Cursor -----------------------------------------------------------------

VCC_TEST(the_cursor_walks_by_the_entry_duration)
{
    notation::Score score;
    ScoreEditor editor(score);

    VCC_CHECK(editor.set_duration(notation::NoteValue::Eighth).succeeded());
    VCC_CHECK(editor.move_cursor(CursorStep::Duration, 1).succeeded());
    VCC_CHECK_EQ(editor.cursor().tick, kQuarter / 2);

    VCC_CHECK(editor.move_cursor(CursorStep::Duration, -1).succeeded());
    VCC_CHECK_EQ(editor.cursor().tick, notation::Ticks{0});

    // It cannot walk off the front of the score.
    VCC_CHECK_EQ(editor.move_cursor(CursorStep::Duration, -1).status, EditStatus::NothingToDo);
    VCC_CHECK_EQ(editor.cursor().tick, notation::Ticks{0});
}

VCC_TEST(the_cursor_moves_by_measures_across_a_meter_change)
{
    notation::Score score;
    VCC_CHECK(score.set_time_signature(kBar, vcc::music::TimeSignature{3, 4}));
    ScoreEditor editor(score);

    VCC_CHECK(editor.move_cursor(CursorStep::Measure, 1).succeeded());
    VCC_CHECK_EQ(editor.cursor().tick, kBar);  // 4/4 bar.

    VCC_CHECK(editor.move_cursor(CursorStep::Measure, 1).succeeded());
    VCC_CHECK_EQ(editor.cursor().tick, kBar + kQuarter * 3);  // 3/4 bar.

    VCC_CHECK(editor.move_cursor(CursorStep::Measure, -1).succeeded());
    VCC_CHECK_EQ(editor.cursor().tick, kBar);

    // From inside a bar, back goes to the start of that bar first.
    EditCursor cursor = editor.cursor();
    cursor.tick = kBar + kQuarter;
    editor.set_cursor(cursor);
    VCC_CHECK(editor.move_cursor(CursorStep::Measure, -1).succeeded());
    VCC_CHECK_EQ(editor.cursor().tick, kBar);
}

VCC_TEST(the_cursor_moves_between_staves_and_stops_at_the_ends)
{
    notation::Score score;
    score.make_grand_staff();
    ScoreEditor editor(score);

    VCC_CHECK_EQ(editor.move_cursor(CursorStep::Staff, -1).status, EditStatus::NothingToDo);
    VCC_CHECK(editor.move_cursor(CursorStep::Staff, 1).succeeded());
    VCC_CHECK_EQ(editor.cursor().staff_index, std::size_t{1});
    VCC_CHECK_EQ(editor.move_cursor(CursorStep::Staff, 1).status, EditStatus::NothingToDo);
}

VCC_TEST(a_cursor_set_beyond_the_score_is_clamped)
{
    notation::Score score;
    ScoreEditor editor(score);

    EditCursor cursor;
    cursor.staff_index = 7;
    cursor.tick = -500;
    editor.set_cursor(cursor);

    VCC_CHECK_EQ(editor.cursor().staff_index, std::size_t{0});
    VCC_CHECK_EQ(editor.cursor().tick, notation::Ticks{0});
}

// --- Commands ---------------------------------------------------------------

VCC_TEST(every_bound_command_gets_an_explicit_answer)
{
    // The shortcut profiles can bind any of these, so any command without a
    // case in the editor would silently do nothing when pressed. This fails
    // if one is ever added to the enum and not handled.
    notation::Score score;
    ScoreEditor editor(score);
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::C, 4)).succeeded());

    for (const composer::Command command : composer::all_commands()) {
        const EditResult result = editor.apply(command);
        VCC_CHECK_MSG(result.message != nullptr, composer::describe(command));
        VCC_CHECK_MSG(std::string(result.message) != "Unknown command",
                      composer::describe(command));
        VCC_CHECK_MSG(!std::string(result.message).empty(), composer::describe(command));
    }
}

VCC_TEST(commands_that_belong_elsewhere_say_so_rather_than_doing_nothing)
{
    notation::Score score;
    ScoreEditor editor(score);

    for (const composer::Command command :
         {composer::Command::PlayFromCursor, composer::Command::StopPlayback,
          composer::Command::ResyncWithHost, composer::Command::ShowComposerWindow,
          composer::Command::ToggleKeyboardEntryMode, composer::Command::EditTempo}) {
        VCC_CHECK_EQ(editor.apply(command).status, EditStatus::NotHandled);
    }

    // These are the editor's, but need a value the keystroke cannot carry.
    for (const composer::Command command :
         {composer::Command::EditKeySignature, composer::Command::EditTimeSignature,
          composer::Command::EditClef, composer::Command::AddText}) {
        VCC_CHECK_EQ(editor.apply(command).status, EditStatus::NeedsInput);
    }
}

VCC_TEST(the_command_path_and_the_direct_path_do_the_same_thing)
{
    notation::Score direct_score;
    notation::Score command_score;
    ScoreEditor direct(direct_score);
    ScoreEditor commanded(command_score);

    direct.set_duration(notation::NoteValue::Eighth);
    direct.toggle_dot();
    direct.insert_pitch(pitch_of(notation::Step::C, 4));
    direct.toggle_articulation(notation::Articulation::Accent);
    direct.add_dynamic(notation::DynamicLevel::Forte);

    commanded.apply(composer::Command::SelectDurationEighth);
    commanded.apply(composer::Command::ToggleDot);
    commanded.insert_pitch(pitch_of(notation::Step::C, 4));
    commanded.apply(composer::Command::ArticulationAccent);
    commanded.apply(composer::Command::DynamicForte);

    VCC_CHECK_EQ(direct_score.notes(0).size(), command_score.notes(0).size());
    VCC_CHECK_EQ(direct_score.notes(0).front().duration.ticks(),
                 command_score.notes(0).front().duration.ticks());
    VCC_CHECK_EQ(direct_score.notes(0).front().articulations,
                 command_score.notes(0).front().articulations);
    VCC_CHECK_EQ(direct_score.dynamics().size(), command_score.dynamics().size());
    VCC_CHECK_EQ(direct.cursor().tick, commanded.cursor().tick);
}

VCC_TEST(a_meter_change_off_a_barline_is_refused_with_a_reason)
{
    notation::Score score;
    ScoreEditor editor(score);

    VCC_CHECK(editor.set_time_signature(kBar, vcc::music::TimeSignature{3, 4}).succeeded());

    const EditResult refused =
        editor.set_time_signature(kQuarter, vcc::music::TimeSignature{5, 8});
    VCC_CHECK_EQ(refused.status, EditStatus::Refused);
    VCC_CHECK(std::string(refused.message).find("barline") != std::string::npos);

    // And the refusal left no trace in the history.
    VCC_CHECK_EQ(editor.history().undo_depth(), std::size_t{1});
    VCC_CHECK(!editor.history().is_recording());
}

VCC_TEST(a_clef_change_is_an_edit_and_the_same_clef_is_not)
{
    notation::Score score;
    ScoreEditor editor(score);

    VCC_CHECK(editor.set_clef(0, notation::Clef::Bass).succeeded());
    VCC_CHECK_EQ(score.staff(0).clef, notation::Clef::Bass);
    VCC_CHECK_EQ(editor.set_clef(0, notation::Clef::Bass).status, EditStatus::NothingToDo);
    VCC_CHECK_EQ(editor.set_clef(4, notation::Clef::Alto).status, EditStatus::Refused);
}

VCC_TEST(selecting_everything_selects_every_staff)
{
    notation::Score score;
    score.make_grand_staff();
    ScoreEditor editor(score);

    VCC_CHECK_EQ(editor.select_all().status, EditStatus::NothingToDo);

    editor.insert_at(0, 0, pitch_of(notation::Step::C, 5));
    editor.insert_at(1, 0, pitch_of(notation::Step::C, 3));

    VCC_CHECK(editor.select_all().succeeded());
    VCC_CHECK_EQ(editor.selection().size(), std::size_t{2});

    const std::set<notation::EventId> unique(editor.selection().begin(),
                                             editor.selection().end());
    VCC_CHECK_EQ(unique.size(), std::size_t{2});
}

// --- Voices -----------------------------------------------------------------

VCC_TEST(a_second_voice_is_written_beside_the_first_without_overwriting_it)
{
    notation::Score score;
    ScoreEditor editor(score);

    // A melody in the first voice.
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::C, 5)).succeeded());
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::D, 5)).succeeded());

    // Back to the start, into the second voice, and an alto line under it.
    EditCursor cursor = editor.cursor();
    cursor.tick = 0;
    editor.set_cursor(cursor);
    editor.clear_selection();
    const EditResult chosen = editor.select_voice(1);
    VCC_CHECK(chosen.succeeded());
    VCC_CHECK(!chosen.score_changed);
    VCC_CHECK_EQ(editor.cursor().voice, std::uint8_t{1});

    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::E, 4)).succeeded());
    VCC_CHECK(editor.insert_pitch(pitch_of(notation::Step::F, 4)).succeeded());

    // Four notes: entry in one voice does not overwrite the other, which is
    // the whole point of a second voice.
    VCC_CHECK_EQ(score.notes(0).size(), std::size_t{4});
    std::size_t in_first = 0;
    std::size_t in_second = 0;
    for (const notation::ScoreNote& note : score.notes(0)) {
        if (note.voice == 0)
            ++in_first;
        if (note.voice == 1)
            ++in_second;
    }
    VCC_CHECK_EQ(in_first, std::size_t{2});
    VCC_CHECK_EQ(in_second, std::size_t{2});

    // And a command at the cursor acts on the voice being written, not on
    // whatever else sounds at that tick.
    cursor = editor.cursor();
    cursor.tick = 0;
    editor.set_cursor(cursor);
    editor.clear_selection();
    const std::vector<notation::EventId> at_cursor = editor.notes_at_cursor();
    VCC_CHECK_EQ(at_cursor.size(), std::size_t{1});
    VCC_CHECK_EQ(score.find_note(at_cursor.front())->voice, std::uint8_t{1});
    VCC_CHECK_EQ(score.find_note(at_cursor.front())->pitch.step, notation::Step::E);
}

VCC_TEST(choosing_the_voice_already_being_written_is_nothing_to_do)
{
    notation::Score score;
    ScoreEditor editor(score);

    VCC_CHECK_EQ(editor.select_voice(0).status, EditStatus::NothingToDo);
    VCC_CHECK(editor.select_voice(3).succeeded());
    VCC_CHECK_EQ(editor.cursor().voice, std::uint8_t{3});

    // Four voices is what a staff holds; a fifth is refused with a reason
    // rather than clamped to the fourth.
    VCC_CHECK_EQ(editor.select_voice(4).status, EditStatus::Refused);
    VCC_CHECK_EQ(editor.cursor().voice, std::uint8_t{3});
}

VCC_TEST(choosing_a_voice_with_a_selection_moves_the_notes_into_it_as_one_undo_step)
{
    notation::Score score;
    ScoreEditor editor(score);

    const EditResult first = editor.insert_pitch(pitch_of(notation::Step::C, 5));
    const EditResult second = editor.insert_pitch(pitch_of(notation::Step::D, 5));
    editor.set_selection({first.new_event, second.new_event});

    const EditResult moved = editor.select_voice(1);
    VCC_CHECK(moved.succeeded());
    VCC_CHECK(moved.score_changed);
    VCC_CHECK_EQ(moved.notes_changed, std::size_t{2});
    VCC_CHECK_EQ(score.find_note(first.new_event)->voice, std::uint8_t{1});
    VCC_CHECK_EQ(score.find_note(second.new_event)->voice, std::uint8_t{1});

    // The ids survive, so the host sees two notes changed rather than two
    // deleted and two added.
    VCC_CHECK_EQ(score.notes(0).size(), std::size_t{2});

    // Entry carries on in the voice the notes went to.
    VCC_CHECK_EQ(editor.cursor().voice, std::uint8_t{1});

    // One undo puts both back.
    VCC_CHECK(editor.undo().succeeded());
    VCC_CHECK_EQ(score.find_note(first.new_event)->voice, std::uint8_t{0});
    VCC_CHECK_EQ(score.find_note(second.new_event)->voice, std::uint8_t{0});
    VCC_CHECK(editor.redo().succeeded());
    VCC_CHECK_EQ(score.find_note(first.new_event)->voice, std::uint8_t{1});

    // A selection already in that voice has nothing to move, and says so.
    VCC_CHECK_EQ(editor.select_voice(1).status, EditStatus::NothingToDo);
}

VCC_TEST(the_voice_commands_choose_voices_one_to_four)
{
    notation::Score score;
    ScoreEditor editor(score);

    VCC_CHECK(editor.apply(composer::Command::SelectVoice2).succeeded());
    VCC_CHECK_EQ(editor.cursor().voice, std::uint8_t{1});
    VCC_CHECK(editor.apply(composer::Command::SelectVoice4).succeeded());
    VCC_CHECK_EQ(editor.cursor().voice, std::uint8_t{3});
    VCC_CHECK(editor.apply(composer::Command::SelectVoice3).succeeded());
    VCC_CHECK_EQ(editor.cursor().voice, std::uint8_t{2});
    VCC_CHECK(editor.apply(composer::Command::SelectVoice1).succeeded());
    VCC_CHECK_EQ(editor.cursor().voice, std::uint8_t{0});
}

VCC_TEST(selecting_a_note_by_pointer_takes_the_cursor_to_its_staff_and_voice)
{
    notation::Score score;
    score.make_grand_staff();
    ScoreEditor editor(score);

    // A note in the second voice of the bass staff.
    notation::ScoreNote alto;
    alto.start_tick = kQuarter;
    alto.duration = notation::quarter();
    alto.pitch = pitch_of(notation::Step::E, 3);
    alto.voice = 1;
    const notation::EventId id = score.insert_note(1, alto);

    const EditResult picked = editor.select_and_follow(id);
    VCC_CHECK(picked.succeeded());
    VCC_CHECK(!picked.score_changed);
    VCC_CHECK_EQ(editor.selection().size(), std::size_t{1});
    VCC_CHECK_EQ(editor.selection().front(), id);

    // The cursor follows the note's staff and voice, so what is typed next
    // goes where the composer is looking - and not its tick, so typing does
    // not overwrite the note just picked.
    VCC_CHECK_EQ(editor.cursor().staff_index, std::size_t{1});
    VCC_CHECK_EQ(editor.cursor().voice, std::uint8_t{1});
    VCC_CHECK_EQ(editor.cursor().tick, notation::Ticks{0});

    VCC_CHECK_EQ(editor.select_and_follow(notation::kInvalidEventId).status,
                 EditStatus::NothingToDo);
}

VCC_TEST_MAIN("editing/Entry")
