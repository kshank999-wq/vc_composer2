// Event-diff synchronization (spec 02 section 13).
//
// Spec 13 requires "an event-diff synchronization layer rather than
// rewriting the entire MIDI region after every edit". The point is not only
// efficiency: rewriting a region destroys whatever the user changed in the
// host, and makes host undo useless. These cases check that an edit produces
// changes proportional to the edit, and that untouched notes are left alone.
#include "vcc/notation/EventDiff.h"

#include <algorithm>
#include <string>
#include <vector>

#include "vcc_test.h"

using namespace vcc::notation;
using vcc::music::MidiEventType;

/// A quarter note in ticks, so the arithmetic reads as rhythm.
constexpr Ticks kQuarter = kTicksPerQuarter;

namespace {

ScoreNote make_note(Ticks tick, Step step, Duration duration = quarter())
{
    ScoreNote note;
    note.start_tick = tick;
    note.duration = duration;
    note.pitch = Pitch{step, 4, 0};
    return note;
}

/// A score of `count` quarter notes, one per beat.
Score make_phrase(int count)
{
    Score score;
    for (int index = 0; index < count; ++index)
        score.insert_note(0, make_note(index * kQuarter, static_cast<Step>(index % 7)));
    return score;
}

}  // namespace

VCC_TEST(the_first_update_reports_everything_as_new)
{
    // The host has nothing yet, so every event is an addition.
    const Score score = make_phrase(4);
    ScoreSynchronizer synchronizer;

    const EventDiff diff = synchronizer.update(score);
    VCC_CHECK_EQ(diff.added, static_cast<std::size_t>(8));  // 4 note-ons, 4 note-offs.
    VCC_CHECK_EQ(diff.removed, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(diff.modified, static_cast<std::size_t>(0));
}

VCC_TEST(an_unchanged_score_produces_no_changes)
{
    const Score score = make_phrase(8);
    ScoreSynchronizer synchronizer;
    synchronizer.update(score);

    const EventDiff second = synchronizer.update(score);
    VCC_CHECK(second.empty());
    VCC_CHECK_EQ(second.added, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(second.removed, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(second.modified, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(second.unchanged, static_cast<std::size_t>(16));
}

VCC_TEST(the_revision_counter_reports_when_no_work_is_needed)
{
    Score score = make_phrase(4);
    ScoreSynchronizer synchronizer;

    VCC_CHECK(!synchronizer.is_up_to_date(score));
    synchronizer.update(score);
    VCC_CHECK(synchronizer.is_up_to_date(score));

    score.insert_note(0, make_note((4 * kQuarter), Step::G));
    VCC_CHECK(!synchronizer.is_up_to_date(score));
}

VCC_TEST(adding_one_note_to_a_long_phrase_changes_only_that_note)
{
    // The property that makes this worth having: an edit costs its own size,
    // not the size of the score.
    Score score = make_phrase(64);
    ScoreSynchronizer synchronizer;
    synchronizer.update(score);

    score.insert_note(0, make_note(64 * kQuarter, Step::C));
    const EventDiff diff = synchronizer.update(score);

    VCC_CHECK_EQ(diff.added, static_cast<std::size_t>(2));   // One note-on, one note-off.
    VCC_CHECK_EQ(diff.removed, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(diff.modified, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(diff.unchanged, static_cast<std::size_t>(128));

    // A whole-region rewrite would have touched all 130 events.
    VCC_CHECK_MSG(diff.size() < 5, "an edit to one note produced "
                                       + std::to_string(diff.size()) + " changes");
}

VCC_TEST(deleting_one_note_from_a_long_phrase_changes_only_that_note)
{
    Score score = make_phrase(32);
    const EventId middle = score.notes(0)[16].id;

    ScoreSynchronizer synchronizer;
    synchronizer.update(score);

    VCC_CHECK(score.remove_note(middle));
    const EventDiff diff = synchronizer.update(score);

    VCC_CHECK_EQ(diff.removed, static_cast<std::size_t>(2));
    VCC_CHECK_EQ(diff.added, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(diff.modified, static_cast<std::size_t>(0));
}

VCC_TEST(changing_a_notes_pitch_is_a_modification_of_the_same_event)
{
    // Transposing is the commonest edit there is, so what the host is told
    // about it matters. It is reported as a modification carrying both the
    // old event and the new one, not as a removal and an addition.
    //
    // In a MIDI *stream* a different note number is a different message,
    // which is what an earlier version of this reasoned from. But the
    // consumer of a diff is a host editing API with note objects that have
    // a pitch, and Pro Tools can change one in place. Reporting a removal
    // and an addition takes that choice away and discards whatever the user
    // attached to the note in the host - the loss spec 13 exists to
    // prevent. An adapter that genuinely cannot change pitch in place can
    // still apply a modification as a remove and an add; the information is
    // all there. The reverse is not true.
    Score score = make_phrase(4);
    const EventId id = score.notes(0)[1].id;

    ScoreSynchronizer synchronizer;
    synchronizer.update(score);

    ScoreNote changed = *score.find_note(id);
    changed.pitch = Pitch{Step::B, 5, 0};
    VCC_CHECK(score.modify_note(id, changed));

    const EventDiff diff = synchronizer.update(score);
    VCC_CHECK_EQ(diff.modified, static_cast<std::size_t>(2));  // Its note-on and note-off.
    VCC_CHECK_EQ(diff.added, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(diff.removed, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(diff.unchanged, static_cast<std::size_t>(6));

    // Both sides are reported, so the adapter knows which event to change
    // and what to change it to.
    for (const EventChange& change : diff.changes) {
        VCC_CHECK_EQ(change.kind, DiffKind::Modified);
        VCC_CHECK_EQ(change.previous.source_id, id);
        VCC_CHECK_EQ(change.event.source_id, id);
        VCC_CHECK(change.previous.event.data1 != change.event.event.data1);
    }
}

VCC_TEST(a_keyswitch_is_never_confused_with_the_note_that_triggered_it)
{
    // One note can emit a keyswitch note-on and its own note-on, on the
    // same channel with the same source id. They are told apart by role;
    // without that, transposing a note could match its note-on against its
    // own keyswitch.
    Score score;
    ScoreNote note;
    note.start_tick = 0;
    note.duration = quarter();
    note.pitch = Pitch{Step::C, 4, 0};
    note.articulations = to_mask(Articulation::Staccato);
    const EventId id = score.insert_note(0, note);
    VCC_CHECK(id != kInvalidEventId);

    ScoreSynchronizer synchronizer{MidiRenderer{make_orchestral_cc_expression_map()}};
    const EventDiff first = synchronizer.update(score);

    std::size_t keyswitches = 0;
    std::size_t note_events = 0;
    for (const EventChange& change : first.changes) {
        if (change.event.source_id != id)
            continue;
        if (change.event.role == EventRole::Keyswitch)
            ++keyswitches;
        if (change.event.role == EventRole::Note)
            ++note_events;
    }
    VCC_CHECK(keyswitches > 0);
    VCC_CHECK_EQ(note_events, static_cast<std::size_t>(2));

    // Transposing changes the sounding note and leaves the keyswitch alone.
    ScoreNote moved = *score.find_note(id);
    moved.pitch = Pitch{Step::D, 4, 0};
    VCC_CHECK(score.modify_note(id, moved));

    const EventDiff diff = synchronizer.update(score);
    VCC_CHECK_EQ(diff.modified, static_cast<std::size_t>(2));
    VCC_CHECK_EQ(diff.added, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(diff.removed, static_cast<std::size_t>(0));
    for (const EventChange& change : diff.changes)
        VCC_CHECK_EQ(change.event.role, EventRole::Note);
}

VCC_TEST(moving_a_note_in_time_is_reported_as_a_modification)
{
    // Same note, same pitch, new position: the host can move the existing
    // event rather than deleting and recreating it, which preserves
    // anything attached to it.
    Score score = make_phrase(4);
    const EventId id = score.notes(0)[2].id;

    ScoreSynchronizer synchronizer;
    synchronizer.update(score);

    ScoreNote moved = *score.find_note(id);
    moved.start_tick += (kQuarter / 2);
    VCC_CHECK(score.modify_note(id, moved));

    const EventDiff diff = synchronizer.update(score);
    VCC_CHECK_EQ(diff.modified, static_cast<std::size_t>(2));  // Note-on and note-off moved.
    VCC_CHECK_EQ(diff.added, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(diff.removed, static_cast<std::size_t>(0));

    for (const EventChange& change : diff.changes) {
        VCC_CHECK_EQ(change.kind, DiffKind::Modified);
        VCC_CHECK(change.event.tick != change.previous.tick);
        VCC_CHECK_EQ(change.event.source_id, id);
    }
}

VCC_TEST(changing_a_notes_duration_modifies_only_its_note_off)
{
    Score score = make_phrase(4);
    const EventId id = score.notes(0)[0].id;

    ScoreSynchronizer synchronizer;
    synchronizer.update(score);

    ScoreNote longer = *score.find_note(id);
    longer.duration = half();
    VCC_CHECK(score.modify_note(id, longer));

    const EventDiff diff = synchronizer.update(score);
    // The note-on stays put; only the note-off moves.
    VCC_CHECK_EQ(diff.modified, static_cast<std::size_t>(1));
    VCC_CHECK_EQ(diff.changes[0].event.event.type, MidiEventType::NoteOff);
}

VCC_TEST(changing_a_dynamic_modifies_the_velocities_it_governs)
{
    // A dynamic change is not a structural edit: the notes stay where they
    // are and only their velocity changes, so nothing should be added or
    // removed.
    Score score = make_phrase(4);
    score.add_dynamic(DynamicMarking{0, DynamicLevel::Piano, 0, {}});

    ScoreSynchronizer synchronizer;
    synchronizer.update(score);

    score.add_dynamic(DynamicMarking{0, DynamicLevel::Fortissimo, 0, {}});
    const EventDiff diff = synchronizer.update(score);

    VCC_CHECK_EQ(diff.added, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(diff.removed, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(diff.modified, static_cast<std::size_t>(4));  // Four note-ons revoiced.

    for (const EventChange& change : diff.changes) {
        VCC_CHECK_EQ(change.event.event.type, MidiEventType::NoteOn);
        VCC_CHECK(change.event.event.data2 > change.previous.event.data2);
    }
}

VCC_TEST(adding_a_hairpin_modifies_velocities_without_restructuring)
{
    // Acceptance criterion 6 from the synchronization side: drawing a
    // crescendo over existing notes must revoice them, not rewrite them.
    Score score = make_phrase(8);
    ScoreSynchronizer synchronizer;
    synchronizer.update(score);

    Hairpin crescendo;
    crescendo.start_tick = 0;
    crescendo.end_tick = 8 * kQuarter;
    crescendo.kind = HairpinKind::Crescendo;
    crescendo.has_start_level = true;
    crescendo.start_level = DynamicLevel::Pianissimo;
    crescendo.has_end_level = true;
    crescendo.end_level = DynamicLevel::Fortissimo;
    score.add_hairpin(crescendo);

    const EventDiff diff = synchronizer.update(score);
    VCC_CHECK_EQ(diff.added, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(diff.removed, static_cast<std::size_t>(0));
    VCC_CHECK(diff.modified > 0);
}

VCC_TEST(the_synchronizer_reports_what_a_rewrite_would_have_cost)
{
    // The concrete benefit spec 13 asks for, measured.
    Score score = make_phrase(50);
    ScoreSynchronizer synchronizer;
    synchronizer.update(score);  // 100 events.

    for (int edit = 0; edit < 10; ++edit) {
        score.insert_note(0, make_note((50 + edit) * kQuarter, Step::C));
        synchronizer.update(score);
    }

    VCC_CHECK(synchronizer.events_sent() < synchronizer.events_a_rewrite_would_have_sent());
    // Ten single-note edits after the initial load should cost far less
    // than ten full rewrites.
    VCC_CHECK_MSG(synchronizer.events_sent() * 3 < synchronizer.events_a_rewrite_would_have_sent(),
                  "diff sent " + std::to_string(synchronizer.events_sent())
                      + " events against a rewrite's "
                      + std::to_string(synchronizer.events_a_rewrite_would_have_sent()));
}

VCC_TEST(resetting_makes_the_next_update_report_everything_again)
{
    // Used when the host region has been cleared, or the binding moved to a
    // different track: the host no longer has what we last sent.
    Score score = make_phrase(4);
    ScoreSynchronizer synchronizer;
    synchronizer.update(score);
    VCC_CHECK(synchronizer.update(score).empty());

    synchronizer.reset();
    const EventDiff after = synchronizer.update(score);
    VCC_CHECK_EQ(after.added, static_cast<std::size_t>(8));
    VCC_CHECK_EQ(synchronizer.events_sent(), static_cast<std::uint64_t>(8));
}

VCC_TEST(a_diff_against_an_empty_render_is_all_additions)
{
    const Score score = make_phrase(3);
    const MidiRenderer renderer;
    const RenderResult rendered = renderer.render(score);
    const RenderResult empty;

    const EventDiff forward = diff_events(empty, rendered);
    VCC_CHECK_EQ(forward.added, rendered.events.size());
    VCC_CHECK_EQ(forward.removed, static_cast<std::size_t>(0));

    // And the reverse is all removals, which is what clearing a region is.
    const EventDiff backward = diff_events(rendered, empty);
    VCC_CHECK_EQ(backward.removed, rendered.events.size());
    VCC_CHECK_EQ(backward.added, static_cast<std::size_t>(0));
}

VCC_TEST(changes_are_ordered_by_position)
{
    // Lets the host adapter apply them in one forward pass.
    Score score = make_phrase(10);
    ScoreSynchronizer synchronizer;
    synchronizer.update(score);

    score.remove_note(score.notes(0)[8].id);
    score.remove_note(score.notes(0)[2].id);
    score.insert_note(0, make_note(5000, Step::A));

    const EventDiff diff = synchronizer.update(score);
    VCC_CHECK(diff.size() > 0);

    Ticks previous = -1;
    for (const EventChange& change : diff.changes) {
        const Ticks tick =
            change.kind == DiffKind::Removed ? change.previous.tick : change.event.tick;
        VCC_CHECK_MSG(tick >= previous, "diff changes are out of order");
        previous = tick;
    }
}

VCC_TEST(a_sequence_of_edits_converges_on_the_right_state)
{
    // The end-to-end property: applying every diff in turn to a running
    // event set must arrive at exactly what a full render of the final
    // score produces. If that fails, the host and the score have drifted -
    // the exact failure a diff layer risks.
    Score score = make_phrase(12);
    ScoreSynchronizer synchronizer;

    // Mirror of the host's event set, keyed the way a host would track it.
    std::vector<TimedMidiEvent> host_state;

    const auto apply = [&host_state](const EventDiff& diff) {
        for (const EventChange& change : diff.changes) {
            if (change.kind == DiffKind::Added) {
                host_state.push_back(change.event);
            } else if (change.kind == DiffKind::Removed) {
                for (auto it = host_state.begin(); it != host_state.end(); ++it) {
                    if (it->tick == change.previous.tick
                        && it->source_id == change.previous.source_id
                        && it->event.type == change.previous.event.type
                        && it->event.data1 == change.previous.event.data1) {
                        host_state.erase(it);
                        break;
                    }
                }
            } else {
                for (TimedMidiEvent& event : host_state) {
                    if (event.tick == change.previous.tick
                        && event.source_id == change.previous.source_id
                        && event.event.type == change.previous.event.type
                        && event.event.data1 == change.previous.event.data1) {
                        event = change.event;
                        break;
                    }
                }
            }
        }
    };

    apply(synchronizer.update(score));

    // A realistic run of edits.
    score.add_dynamic(DynamicMarking{0, DynamicLevel::Piano, 0, {}});
    apply(synchronizer.update(score));

    score.remove_note(score.notes(0)[3].id);
    apply(synchronizer.update(score));

    ScoreNote moved = score.notes(0)[5];
    moved.start_tick += (kQuarter / 4);
    score.modify_note(moved.id, moved);
    apply(synchronizer.update(score));

    score.insert_note(0, make_note(20000, Step::F));
    apply(synchronizer.update(score));

    ScoreNote accented = score.notes(0)[1];
    accented.articulations = to_mask(Articulation::Accent);
    score.modify_note(accented.id, accented);
    apply(synchronizer.update(score));

    // Compare against a fresh full render of the final score.
    const MidiRenderer renderer;
    const RenderResult expected = renderer.render(score);

    VCC_CHECK_EQ(host_state.size(), expected.events.size());

    for (const TimedMidiEvent& wanted : expected.events) {
        const bool present =
            std::any_of(host_state.begin(), host_state.end(),
                        [&wanted](const TimedMidiEvent& actual) {
                            return actual.tick == wanted.tick
                                && actual.event.type == wanted.event.type
                                && actual.event.data1 == wanted.event.data1
                                && actual.event.data2 == wanted.event.data2
                                && actual.source_id == wanted.source_id;
                        });
        VCC_CHECK_MSG(present, "an event from the final render is missing from the host state");
    }
}

VCC_TEST_MAIN("notation/EventDiff")
