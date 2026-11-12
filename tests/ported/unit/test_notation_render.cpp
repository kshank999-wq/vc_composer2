// Notation-to-MIDI translation (spec 02 sections 6, 7).
//
// These cases verify the acceptance criteria that define the product:
//
//   2. "Entering a note in VC Composer results in correctly timed MIDI"
//   5. "The user can enter dynamics/articulations and map them to MIDI"
//   6. "The user can create a crescendo/decrescendo across a musical range"
//
// Every expectation is derived from the spec 6 mapping table, not from
// whatever the implementation happens to produce.
#include "vcc/notation/MidiRenderer.h"

#include <algorithm>
#include <string>
#include <vector>

#include "vcc_test.h"

using namespace vcc::notation;
using vcc::music::MidiEventType;

/// A quarter note in ticks, so the arithmetic reads as rhythm.
constexpr Ticks kQuarter = kTicksPerQuarter;

namespace {

ScoreNote make_note(Ticks tick, Step step, std::int8_t octave = 4,
                    Duration duration = quarter(), ArticulationMask articulations = 0)
{
    ScoreNote note;
    note.start_tick = tick;
    note.duration = duration;
    note.pitch = Pitch{step, octave, 0};
    note.articulations = articulations;
    return note;
}

/// Every note-on in a render result, in order.
std::vector<TimedMidiEvent> note_ons(const RenderResult& result)
{
    std::vector<TimedMidiEvent> out;
    for (const TimedMidiEvent& event : result.events) {
        if (event.event.type == MidiEventType::NoteOn && event.event.data2 > 0)
            out.push_back(event);
    }
    return out;
}

std::vector<TimedMidiEvent> note_offs(const RenderResult& result)
{
    std::vector<TimedMidiEvent> out;
    for (const TimedMidiEvent& event : result.events) {
        if (event.event.type == MidiEventType::NoteOff)
            out.push_back(event);
    }
    return out;
}

std::vector<TimedMidiEvent> controllers(const RenderResult& result, std::uint8_t cc)
{
    std::vector<TimedMidiEvent> out;
    for (const TimedMidiEvent& event : result.events) {
        if (event.event.type == MidiEventType::ControlChange && event.event.data1 == cc)
            out.push_back(event);
    }
    return out;
}

}  // namespace

VCC_TEST(an_empty_score_renders_nothing)
{
    const Score score;
    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);

    VCC_CHECK(result.events.empty());
    VCC_CHECK_EQ(result.note_count, static_cast<std::size_t>(0));
}

VCC_TEST(a_note_renders_at_its_written_position)
{
    // Acceptance criterion 2: correctly timed MIDI.
    Score score;
    score.insert_note(0, make_note((2 * kQuarter), Step::C, 4, half()));

    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);

    const std::vector<TimedMidiEvent> ons = note_ons(result);
    VCC_CHECK_EQ(ons.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(ons[0].tick, static_cast<Ticks>((2 * kQuarter)));
    VCC_CHECK_EQ(ons[0].event.data1, static_cast<std::uint8_t>(60));  // Middle C.
    VCC_CHECK_EQ(result.note_count, static_cast<std::size_t>(1));

    // And it stops within its written length, never beyond it.
    const std::vector<TimedMidiEvent> offs = note_offs(result);
    VCC_CHECK_EQ(offs.size(), static_cast<std::size_t>(1));
    VCC_CHECK(offs[0].tick > ons[0].tick);
    VCC_CHECK(offs[0].tick <= static_cast<Ticks>((2 * kQuarter) + (2 * kQuarter)));
}

VCC_TEST(grace_notes_sound_just_before_the_beat_they_ornament)
{
    // Two grace notes before a quarter on beat 2: each takes a
    // thirty-second, the last ending on the beat, and the quarter itself
    // is where it was written.
    Score score;
    ScoreNote first = make_note(kQuarter, Step::C, 5, eighth());
    first.grace = true;
    first.grace_order = 0;
    ScoreNote second = make_note(kQuarter, Step::D, 5, eighth());
    second.grace = true;
    second.grace_order = 1;
    score.insert_note(0, first);
    score.insert_note(0, second);
    score.insert_note(0, make_note(kQuarter, Step::E, 5));

    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);
    const std::vector<TimedMidiEvent> ons = note_ons(result);
    const std::vector<TimedMidiEvent> offs = note_offs(result);
    VCC_CHECK_EQ(ons.size(), std::size_t{3});
    VCC_CHECK_EQ(offs.size(), std::size_t{3});
    if (ons.size() != 3 || offs.size() != 3)
        return;

    constexpr Ticks kGrace = kTicksPerQuarter / 8;
    VCC_CHECK_EQ(ons[0].tick, Ticks{kQuarter - 2 * kGrace});
    VCC_CHECK_EQ(ons[1].tick, Ticks{kQuarter - kGrace});
    VCC_CHECK_EQ(ons[2].tick, Ticks{kQuarter});
    VCC_CHECK_EQ(ons[0].event.data1, std::uint8_t{72});
    VCC_CHECK_EQ(ons[1].event.data1, std::uint8_t{74});
    VCC_CHECK_EQ(ons[2].event.data1, std::uint8_t{76});

    // Each grace note ends before the next begins.
    VCC_CHECK(offs[0].tick > ons[0].tick && offs[0].tick <= ons[1].tick);
    VCC_CHECK(offs[1].tick > ons[1].tick && offs[1].tick <= ons[2].tick);

    // A grace note on the first beat cannot sound before the score begins:
    // it is clamped to the start rather than dropped.
    Score at_start;
    ScoreNote early = make_note(0, Step::C, 5, eighth());
    early.grace = true;
    at_start.insert_note(0, early);
    at_start.insert_note(0, make_note(0, Step::D, 5));
    const std::vector<TimedMidiEvent> early_ons = note_ons(renderer.render(at_start));
    VCC_CHECK_EQ(early_ons.size(), std::size_t{2});
    if (early_ons.size() == 2)
        VCC_CHECK_EQ(early_ons[0].tick, Ticks{0});
}

VCC_TEST(a_staff_plays_its_own_dynamics_and_follows_the_others_when_it_has_none)
{
    // One f between the staves: both hands play it. Then the left hand
    // gets its own p, and only the left hand drops.
    Score score;
    score.make_grand_staff();
    score.insert_note(0, make_note(0, Step::E, 5));
    score.insert_note(1, make_note(0, Step::C, 3));
    score.insert_note(0, make_note(kQuarter, Step::E, 5));
    score.insert_note(1, make_note(kQuarter, Step::C, 3));
    score.add_dynamic(DynamicMarking{0, DynamicLevel::Forte, 0, ""});
    DynamicMarking left{kQuarter, DynamicLevel::Piano, 0, ""};
    left.staff = 1;
    score.add_dynamic(left);

    const MidiRenderer renderer;
    const ExpressionMap map;
    const std::vector<TimedMidiEvent> ons = note_ons(renderer.render(score));
    VCC_CHECK_EQ(ons.size(), std::size_t{4});
    if (ons.size() != 4)
        return;
    for (const TimedMidiEvent& on : ons) {
        const bool right_hand = on.event.data1 == 76;
        if (on.tick == 0 || right_hand)
            VCC_CHECK_EQ(on.event.data2, map.velocity_for(DynamicLevel::Forte));
        else
            VCC_CHECK_EQ(on.event.data2, map.velocity_for(DynamicLevel::Piano));
    }

    // The controllers a staff sends: its own markings', and another
    // staff's only until it has one of its own in force.
    ExpressionMap cc_map;
    cc_map.dynamic_target = DynamicTarget::ControlChange;
    MidiRenderer with_cc;
    with_cc.set_expression_map(cc_map);
    const RenderResult right = with_cc.render_staff(score, 0);
    const RenderResult left_hand = with_cc.render_staff(score, 1);
    std::size_t right_ccs = 0;
    std::size_t left_ccs = 0;
    for (const TimedMidiEvent& event : right.events)
        right_ccs += event.event.type == MidiEventType::ControlChange ? 1 : 0;
    for (const TimedMidiEvent& event : left_hand.events)
        left_ccs += event.event.type == MidiEventType::ControlChange ? 1 : 0;
    VCC_CHECK_EQ(right_ccs, std::size_t{1});  // Its own f; the p is the left hand's.
    VCC_CHECK_EQ(left_ccs, std::size_t{2});   // The f it followed, then its own p.
    // And a chase lands where each stream would be: the left hand before
    // its p follows the f, and each hand its own marking after.
    std::uint8_t chased = 0;
    VCC_CHECK(with_cc.dynamics_cc_at(score, (kQuarter / 2), chased, 1));
    VCC_CHECK_EQ(chased, cc_map.cc_value_for(DynamicLevel::Forte));
    VCC_CHECK(with_cc.dynamics_cc_at(score, (2 * kQuarter), chased, 0));
    VCC_CHECK_EQ(chased, cc_map.cc_value_for(DynamicLevel::Forte));
    VCC_CHECK(with_cc.dynamics_cc_at(score, (2 * kQuarter), chased, 1));
    VCC_CHECK_EQ(chased, cc_map.cc_value_for(DynamicLevel::Piano));

    // A hairpin is a staff's in the same way: the right hand's crescendo
    // is sent by the right hand, and not by a left hand marked for itself.
    Hairpin swell;
    swell.start_tick = (2 * kQuarter);
    swell.end_tick = (4 * kQuarter);
    swell.staff = 0;
    score.add_hairpin(swell);
    std::size_t right_with_swell = 0;
    std::size_t left_with_swell = 0;
    for (const TimedMidiEvent& event : with_cc.render_staff(score, 0).events)
        right_with_swell += event.event.type == MidiEventType::ControlChange ? 1 : 0;
    for (const TimedMidiEvent& event : with_cc.render_staff(score, 1).events)
        left_with_swell += event.event.type == MidiEventType::ControlChange ? 1 : 0;
    VCC_CHECK(right_with_swell > right_ccs);
    VCC_CHECK_EQ(left_with_swell, left_ccs);
}

VCC_TEST(every_rendered_event_carries_its_source_note_id)
{
    // Spec 13: stable ids linking notation events to host MIDI events.
    Score score;
    const EventId first = score.insert_note(0, make_note(0, Step::C));
    const EventId second = score.insert_note(0, make_note(kQuarter, Step::D));

    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);

    for (const TimedMidiEvent& event : note_ons(result))
        VCC_CHECK(event.source_id == first || event.source_id == second);

    const std::vector<TimedMidiEvent> ons = note_ons(result);
    VCC_CHECK_EQ(ons[0].source_id, first);
    VCC_CHECK_EQ(ons[1].source_id, second);
}

VCC_TEST(rests_produce_no_midi_but_occupy_their_time)
{
    Score score;
    ScoreNote rest;
    rest.start_tick = 0;
    rest.duration = half();
    rest.is_rest = true;
    score.insert_note(0, rest);
    score.insert_note(0, make_note((2 * kQuarter), Step::C));

    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);

    const std::vector<TimedMidiEvent> ons = note_ons(result);
    VCC_CHECK_EQ(ons.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(ons[0].tick, static_cast<Ticks>((2 * kQuarter)));
}

VCC_TEST(a_chord_renders_as_simultaneous_notes)
{
    Score score;
    score.insert_chord(0, kQuarter, half(),
                       {Pitch{Step::C, 4, 0}, Pitch{Step::E, 4, 0}, Pitch{Step::G, 4, 0}});

    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);

    const std::vector<TimedMidiEvent> ons = note_ons(result);
    VCC_CHECK_EQ(ons.size(), static_cast<std::size_t>(3));
    for (const TimedMidiEvent& event : ons)
        VCC_CHECK_EQ(event.tick, static_cast<Ticks>(kQuarter));

    // C E G, bottom to top.
    VCC_CHECK_EQ(ons[0].event.data1, static_cast<std::uint8_t>(60));
    VCC_CHECK_EQ(ons[1].event.data1, static_cast<std::uint8_t>(64));
    VCC_CHECK_EQ(ons[2].event.data1, static_cast<std::uint8_t>(67));
}

VCC_TEST(two_voices_on_one_staff_sound_together)
{
    Score score;
    // A held note in the first voice over a moving line in the second.
    score.insert_note(0, make_note(0, Step::C, 5, whole()));
    for (int index = 0; index < 4; ++index) {
        ScoreNote note = make_note(static_cast<Ticks>(index) * kQuarter, Step::E, 4);
        note.voice = 1;
        score.insert_note(0, note);
    }

    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);

    const std::vector<TimedMidiEvent> ons = note_ons(result);
    VCC_CHECK_EQ(ons.size(), static_cast<std::size_t>(5));

    // The whole note is still sounding when the last quarter starts: it
    // ends near where it was written to, not where the other voice's notes
    // do. (Unmarked notes are shortened a little by default, so "near".)
    const std::vector<TimedMidiEvent> offs = note_offs(result);
    Ticks held_off = -1;
    for (const TimedMidiEvent& off : offs) {
        if (off.event.data1 == 72)
            held_off = off.tick;
    }
    VCC_CHECK(held_off > static_cast<Ticks>(3 * kQuarter));
    VCC_CHECK(held_off <= static_cast<Ticks>((4 * kQuarter)));
}

VCC_TEST(a_tie_chain_sounds_as_one_note)
{
    // Two tied quarters are one half-note-long sound, not two attacks.
    // Retriggering mid-tie is audibly wrong.
    Score score;
    ScoreNote first = make_note(0, Step::C);
    first.tied_to_next = true;
    score.insert_note(0, first);
    score.insert_note(0, make_note(kQuarter, Step::C));

    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);

    const std::vector<TimedMidiEvent> ons = note_ons(result);
    VCC_CHECK_EQ(ons.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(ons[0].tick, static_cast<Ticks>(0));

    // And it lasts about two quarters, not one.
    const std::vector<TimedMidiEvent> offs = note_offs(result);
    VCC_CHECK_EQ(offs.size(), static_cast<std::size_t>(1));
    VCC_CHECK(offs[0].tick > static_cast<Ticks>(kQuarter));
}

VCC_TEST(a_three_note_tie_chain_still_sounds_once)
{
    Score score;
    for (int index = 0; index < 3; ++index) {
        ScoreNote note = make_note(index * kQuarter, Step::C);
        note.tied_to_next = index < 2;
        score.insert_note(0, note);
    }

    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);

    VCC_CHECK_EQ(note_ons(result).size(), static_cast<std::size_t>(1));
    VCC_CHECK(note_offs(result)[0].tick > static_cast<Ticks>((2 * kQuarter)));
}

VCC_TEST(a_tie_flag_with_nothing_to_tie_to_is_survivable)
{
    // A dangling tie is a score the user can produce by deleting the note
    // after it; rendering must not loop or crash.
    Score score;
    ScoreNote dangling = make_note(0, Step::C);
    dangling.tied_to_next = true;
    score.insert_note(0, dangling);

    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);
    VCC_CHECK_EQ(note_ons(result).size(), static_cast<std::size_t>(1));
}

VCC_TEST(transposition_shifts_the_sounding_pitch_not_the_written_one)
{
    // A written C on a B-flat clarinet staff sounds a B-flat (spec 5.1).
    Score score;
    score.staff(0).transposition_semitones = -2;
    score.insert_note(0, make_note(0, Step::C));

    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);

    const std::vector<TimedMidiEvent> ons = note_ons(result);
    VCC_CHECK_EQ(ons[0].event.data1, static_cast<std::uint8_t>(58));  // 60 - 2.

    // The written pitch in the score is untouched.
    VCC_CHECK_EQ(score.notes(0)[0].pitch.step, Step::C);
}

VCC_TEST(an_octave_shift_transposes_by_twelve)
{
    Score score;
    score.staff(0).octave_shift = -1;
    score.insert_note(0, make_note(0, Step::C));

    const MidiRenderer renderer;
    VCC_CHECK_EQ(note_ons(renderer.render(score))[0].event.data1,
                 static_cast<std::uint8_t>(48));
}

VCC_TEST(notes_pushed_out_of_midi_range_are_reported_not_dropped_silently)
{
    // The composer needs to know a note is unplayable rather than
    // discovering silence.
    Score score;
    score.staff(0).transposition_semitones = -24;
    score.insert_note(0, make_note(0, Step::C, -1));  // MIDI 0, minus 24.

    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);

    VCC_CHECK(note_ons(result).empty());
    VCC_CHECK_EQ(result.out_of_range_notes.size(), static_cast<std::size_t>(1));
}

// --- Dynamics ---------------------------------------------------------------

VCC_TEST(written_dynamics_set_velocity)
{
    // Acceptance criterion 5, and the spec 6 table: dynamic marking maps to
    // velocity.
    Score score;
    score.add_dynamic(DynamicMarking{0, DynamicLevel::Pianissimo, 0, {}});
    score.insert_note(0, make_note(0, Step::C));
    score.add_dynamic(DynamicMarking{kQuarter, DynamicLevel::Fortissimo, 0, {}});
    score.insert_note(0, make_note(kQuarter, Step::D));

    const MidiRenderer renderer;
    const std::vector<TimedMidiEvent> ons = note_ons(renderer.render(score));

    VCC_CHECK_EQ(ons.size(), static_cast<std::size_t>(2));
    VCC_CHECK_MSG(ons[0].event.data2 < ons[1].event.data2,
                  "pianissimo rendered louder than fortissimo");

    const ExpressionMap map = make_default_expression_map();
    VCC_CHECK_EQ(ons[0].event.data2, map.velocity_for(DynamicLevel::Pianissimo));
    VCC_CHECK_EQ(ons[1].event.data2, map.velocity_for(DynamicLevel::Fortissimo));
}

VCC_TEST(every_dynamic_level_is_distinct_and_ordered)
{
    // A map where two levels produce the same velocity would make a written
    // distinction inaudible.
    const ExpressionMap map = make_default_expression_map();
    for (std::size_t level = 1; level < kDynamicLevelCount; ++level) {
        const std::uint8_t quieter = map.velocity_for(static_cast<DynamicLevel>(level - 1));
        const std::uint8_t louder = map.velocity_for(static_cast<DynamicLevel>(level));
        VCC_CHECK_MSG(louder > quieter,
                      "dynamic level " + std::to_string(level) + " is not louder than the one below");
    }
}

VCC_TEST(no_dynamic_level_renders_as_silence)
{
    // Velocity 0 is a note-off in MIDI, so the quietest ppp must still sound.
    const ExpressionMap map = make_default_expression_map();
    for (std::size_t level = 0; level < kDynamicLevelCount; ++level)
        VCC_CHECK(map.velocity_for(static_cast<DynamicLevel>(level)) > 0);
}

VCC_TEST(a_cc_dynamics_map_emits_controllers_instead)
{
    // Spec 7: a written mezzo-forte must not be hard-coded to one MIDI
    // behaviour. The same score on a controller-based map produces
    // controller data.
    Score score;
    score.add_dynamic(DynamicMarking{0, DynamicLevel::Piano, 0, {}});
    score.insert_note(0, make_note(0, Step::C));

    ExpressionMap map = make_orchestral_cc_expression_map();
    const MidiRenderer renderer(map);
    const RenderResult result = renderer.render(score);

    const std::vector<TimedMidiEvent> cc = controllers(result, map.dynamics_cc);
    VCC_CHECK_MSG(!cc.empty(), "no dynamics controller emitted");
    VCC_CHECK_EQ(cc[0].event.data2, map.cc_value_for(DynamicLevel::Piano));
}

// --- Articulations ----------------------------------------------------------

VCC_TEST(an_accent_raises_velocity)
{
    Score score;
    score.add_dynamic(DynamicMarking{0, DynamicLevel::MezzoForte, 0, {}});
    score.insert_note(0, make_note(0, Step::C));
    score.insert_note(0, make_note(kQuarter, Step::D, 4, quarter(), to_mask(Articulation::Accent)));

    const MidiRenderer renderer;
    const std::vector<TimedMidiEvent> ons = note_ons(renderer.render(score));

    VCC_CHECK_EQ(ons.size(), static_cast<std::size_t>(2));
    VCC_CHECK_MSG(ons[1].event.data2 > ons[0].event.data2,
                  "an accented note was not louder than an unaccented one");
}

VCC_TEST(staccato_shortens_the_note_without_moving_it)
{
    Score score;
    score.insert_note(0, make_note(0, Step::C, 4, quarter()));
    score.insert_note(0, make_note(kQuarter, Step::D, 4, quarter(),
                                   to_mask(Articulation::Staccato)));

    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);

    const std::vector<TimedMidiEvent> ons = note_ons(result);
    const std::vector<TimedMidiEvent> offs = note_offs(result);
    VCC_CHECK_EQ(ons.size(), static_cast<std::size_t>(2));

    const Ticks plain_length = offs[0].tick - ons[0].tick;
    const Ticks staccato_length = offs[1].tick - ons[1].tick;

    VCC_CHECK_MSG(staccato_length < plain_length, "staccato did not shorten the note");
    // The position is unchanged: articulation changes how a note sounds, not
    // when it starts.
    VCC_CHECK_EQ(ons[1].tick, static_cast<Ticks>(kQuarter));
}

VCC_TEST(tenuto_gives_the_full_written_length)
{
    Score score;
    score.insert_note(0, make_note(0, Step::C, 4, quarter(), to_mask(Articulation::Tenuto)));

    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);
    const Ticks length = note_offs(result)[0].tick - note_ons(result)[0].tick;

    VCC_CHECK_EQ(length, static_cast<Ticks>(kQuarter));
}

VCC_TEST(a_note_never_sounds_longer_than_it_is_written)
{
    // Overlapping the next note would be audibly wrong on a monophonic
    // patch and wrong on any patch.
    Score score;
    score.insert_note(0, make_note(0, Step::C, 4, quarter(), to_mask(Articulation::Tenuto)));
    score.insert_note(0, make_note(kQuarter, Step::D, 4, quarter()));

    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);

    for (const TimedMidiEvent& off : note_offs(result)) {
        bool within = false;
        for (const ScoreNote& note : score.notes(0)) {
            if (off.source_id == note.id && off.tick <= note.end_tick())
                within = true;
        }
        VCC_CHECK_MSG(within, "a note sounded past its written duration");
    }
}

VCC_TEST(a_very_short_staccato_note_still_sounds)
{
    // Aggressive scaling on a 64th note must not produce a zero-length note
    // that the instrument ignores.
    Score score;
    score.insert_note(0, make_note(0, Step::C, 4,
                                   Duration{NoteValue::SixtyFourth, 0, {}},
                                   to_mask(Articulation::Staccatissimo)));

    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);
    const Ticks length = note_offs(result)[0].tick - note_ons(result)[0].tick;
    VCC_CHECK(length >= 1);
}

VCC_TEST(combined_articulations_both_apply)
{
    // Marcato scales velocity and length together, so both effects must be
    // visible relative to a plain note.
    Score score;
    score.insert_note(0, make_note(0, Step::C, 4, quarter()));
    score.insert_note(0, make_note(kQuarter, Step::D, 4, quarter(),
                                   to_mask(Articulation::Marcato)));

    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);
    const std::vector<TimedMidiEvent> ons = note_ons(result);
    const std::vector<TimedMidiEvent> offs = note_offs(result);

    VCC_CHECK(ons[1].event.data2 > ons[0].event.data2);                     // Louder.
    VCC_CHECK((offs[1].tick - ons[1].tick) < (offs[0].tick - ons[0].tick)); // Shorter.
}

VCC_TEST(a_keyswitch_is_sent_before_the_note_it_selects)
{
    // Most orchestral libraries need the switch to arrive first; a switch
    // arriving with or after the note selects nothing.
    Score score;
    score.insert_note(0, make_note((2 * kQuarter), Step::C, 4, quarter(),
                                   to_mask(Articulation::Pizzicato)));

    const ExpressionMap map = make_orchestral_cc_expression_map();
    const MidiRenderer renderer(map);
    const RenderResult result = renderer.render(score);

    const ArticulationRule* rule = map.rule_for(Articulation::Pizzicato);
    VCC_CHECK(rule != nullptr);
    VCC_CHECK_EQ(rule->action, ArticulationAction::Keyswitch);

    bool found = false;
    for (const TimedMidiEvent& event : result.events) {
        if (event.event.type == MidiEventType::NoteOn
            && event.event.data1 == rule->keyswitch_note && event.event.data2 > 0) {
            VCC_CHECK_MSG(event.tick < static_cast<Ticks>((2 * kQuarter)),
                          "keyswitch was not sent before the note");
            found = true;
        }
    }
    VCC_CHECK_MSG(found, "no keyswitch emitted for a mapped articulation");
    VCC_CHECK(result.keyswitch_count > 0);
}

VCC_TEST(a_keyswitch_is_released_rather_than_left_held)
{
    // A held keyswitch can consume a voice or latch the wrong articulation.
    Score score;
    score.insert_note(0, make_note((2 * kQuarter), Step::C, 4, quarter(),
                                   to_mask(Articulation::Pizzicato)));

    const ExpressionMap map = make_orchestral_cc_expression_map();
    const MidiRenderer renderer(map);
    const RenderResult result = renderer.render(score);
    const std::uint8_t switch_note = map.rule_for(Articulation::Pizzicato)->keyswitch_note;

    int ons = 0;
    int offs = 0;
    for (const TimedMidiEvent& event : result.events) {
        if (event.event.data1 != switch_note)
            continue;
        if (event.event.type == MidiEventType::NoteOn && event.event.data2 > 0)
            ++ons;
        if (event.event.type == MidiEventType::NoteOff)
            ++offs;
    }
    VCC_CHECK_EQ(ons, offs);
    VCC_CHECK(ons > 0);
}

VCC_TEST(an_unmapped_articulation_is_reported_rather_than_faked)
{
    // A library with no pizzicato patch cannot play one. Saying so beats
    // inventing a behaviour, and beats silence.
    Score score;
    score.insert_note(0, make_note(0, Step::C, 4, quarter(),
                                   to_mask(Articulation::Pizzicato)));

    const MidiRenderer renderer;  // Default map has no pizzicato rule.
    const RenderResult result = renderer.render(score);

    VCC_CHECK_EQ(result.unmapped_articulations.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(result.unmapped_articulations[0], Articulation::Pizzicato);
    // The note still plays; only the articulation is absent.
    VCC_CHECK_EQ(note_ons(result).size(), static_cast<std::size_t>(1));
}

// --- Hairpins ---------------------------------------------------------------

VCC_TEST(a_crescendo_shapes_velocity_across_its_span)
{
    // Acceptance criterion 6.
    Score score;
    score.add_dynamic(DynamicMarking{0, DynamicLevel::Piano, 0, {}});

    Hairpin crescendo;
    crescendo.start_tick = 0;
    crescendo.end_tick = (4 * kQuarter);
    crescendo.kind = HairpinKind::Crescendo;
    crescendo.has_start_level = true;
    crescendo.start_level = DynamicLevel::Piano;
    crescendo.has_end_level = true;
    crescendo.end_level = DynamicLevel::Forte;
    score.add_hairpin(crescendo);

    for (int index = 0; index < 4; ++index)
        score.insert_note(0, make_note(index * kQuarter, Step::C));

    const MidiRenderer renderer;
    const std::vector<TimedMidiEvent> ons = note_ons(renderer.render(score));
    VCC_CHECK_EQ(ons.size(), static_cast<std::size_t>(4));

    // Velocity must rise across the hairpin, note by note.
    for (std::size_t index = 1; index < ons.size(); ++index) {
        VCC_CHECK_MSG(ons[index].event.data2 >= ons[index - 1].event.data2,
                      "velocity fell during a crescendo at note " + std::to_string(index));
    }
    VCC_CHECK_MSG(ons.back().event.data2 > ons.front().event.data2,
                  "a crescendo produced no overall increase");
}

VCC_TEST(a_diminuendo_shapes_velocity_downward)
{
    Score score;
    Hairpin diminuendo;
    diminuendo.start_tick = 0;
    diminuendo.end_tick = (4 * kQuarter);
    diminuendo.kind = HairpinKind::Diminuendo;
    diminuendo.has_start_level = true;
    diminuendo.start_level = DynamicLevel::Forte;
    diminuendo.has_end_level = true;
    diminuendo.end_level = DynamicLevel::Piano;
    score.add_hairpin(diminuendo);

    for (int index = 0; index < 4; ++index)
        score.insert_note(0, make_note(index * kQuarter, Step::C));

    const MidiRenderer renderer;
    const std::vector<TimedMidiEvent> ons = note_ons(renderer.render(score));

    for (std::size_t index = 1; index < ons.size(); ++index) {
        VCC_CHECK_MSG(ons[index].event.data2 <= ons[index - 1].event.data2,
                      "velocity rose during a diminuendo");
    }
    VCC_CHECK(ons.back().event.data2 < ons.front().event.data2);
}

VCC_TEST(a_hairpin_on_a_cc_map_emits_a_controller_curve_reaching_its_target)
{
    Score score;
    Hairpin crescendo;
    crescendo.start_tick = 0;
    crescendo.end_tick = (2 * kQuarter);
    crescendo.kind = HairpinKind::Crescendo;
    crescendo.has_start_level = true;
    crescendo.start_level = DynamicLevel::Pianissimo;
    crescendo.has_end_level = true;
    crescendo.end_level = DynamicLevel::Fortissimo;
    score.add_hairpin(crescendo);

    ExpressionMap map = make_orchestral_cc_expression_map();
    map.hairpin_resolution_ticks = (kQuarter / 4);
    const MidiRenderer renderer(map);
    const RenderResult result = renderer.render(score);

    const std::vector<TimedMidiEvent> cc = controllers(result, map.dynamics_cc);
    // 1920 / 240 = 8 steps, plus the endpoint.
    VCC_CHECK_MSG(cc.size() >= 8, "hairpin produced only " + std::to_string(cc.size())
                                      + " controller messages");

    // Non-decreasing, and it actually arrives at the target value - a
    // crescendo that stops short of its written dynamic is wrong.
    for (std::size_t index = 1; index < cc.size(); ++index)
        VCC_CHECK(cc[index].event.data2 >= cc[index - 1].event.data2);

    VCC_CHECK_EQ(cc.front().event.data2, map.cc_value_for(DynamicLevel::Pianissimo));
    VCC_CHECK_EQ(cc.back().event.data2, map.cc_value_for(DynamicLevel::Fortissimo));
}

VCC_TEST(a_hairpin_whose_span_is_not_a_whole_number_of_steps_still_lands_on_target)
{
    Score score;
    Hairpin crescendo;
    crescendo.start_tick = 0;
    crescendo.end_tick = 1000;  // Not a multiple of the resolution.
    crescendo.kind = HairpinKind::Crescendo;
    crescendo.has_start_level = true;
    crescendo.start_level = DynamicLevel::Piano;
    crescendo.has_end_level = true;
    crescendo.end_level = DynamicLevel::Forte;
    score.add_hairpin(crescendo);

    ExpressionMap map = make_orchestral_cc_expression_map();
    map.hairpin_resolution_ticks = (kQuarter / 4);
    const MidiRenderer renderer(map);
    const RenderResult result = renderer.render(score);

    const std::vector<TimedMidiEvent> cc = controllers(result, map.dynamics_cc);
    VCC_CHECK(!cc.empty());
    VCC_CHECK_EQ(cc.back().event.data2, map.cc_value_for(DynamicLevel::Forte));
    VCC_CHECK_EQ(cc.back().tick, static_cast<Ticks>(1000));
}

VCC_TEST(curve_shapes_differ_but_all_span_zero_to_one)
{
    for (const CurveShape shape : {CurveShape::Linear, CurveShape::Exponential,
                                   CurveShape::Logarithmic, CurveShape::SCurve}) {
        VCC_CHECK_NEAR(evaluate_curve(shape, 0.0), 0.0, 1e-9);
        VCC_CHECK_NEAR(evaluate_curve(shape, 1.0), 1.0, 1e-9);

        // Monotonic, so a crescendo never dips.
        double previous = -1.0;
        for (int step = 0; step <= 20; ++step) {
            const double value = evaluate_curve(shape, step / 20.0);
            VCC_CHECK(value >= previous);
            previous = value;
        }

        // Clamped rather than extrapolated outside the span.
        VCC_CHECK_NEAR(evaluate_curve(shape, -1.0), 0.0, 1e-9);
        VCC_CHECK_NEAR(evaluate_curve(shape, 2.0), 1.0, 1e-9);
    }

    // Exponential is behind linear in the middle, logarithmic ahead: the
    // point of offering them at all.
    VCC_CHECK(evaluate_curve(CurveShape::Exponential, 0.5)
              < evaluate_curve(CurveShape::Linear, 0.5));
    VCC_CHECK(evaluate_curve(CurveShape::Logarithmic, 0.5)
              > evaluate_curve(CurveShape::Linear, 0.5));
}

// --- Ordering and structure -------------------------------------------------

VCC_TEST(events_are_ordered_by_tick)
{
    Score score;
    for (int index = 0; index < 20; ++index)
        score.insert_note(0, make_note(index * (kQuarter / 2), Step::C, 4, eighth()));

    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);

    for (std::size_t index = 1; index < result.events.size(); ++index) {
        VCC_CHECK_MSG(result.events[index].tick >= result.events[index - 1].tick,
                      "rendered events are out of order at index " + std::to_string(index));
    }
}

VCC_TEST(controllers_precede_the_note_on_at_the_same_tick)
{
    // A controller arriving after the note it applies to has no effect on
    // that note.
    Score score;
    score.add_dynamic(DynamicMarking{kQuarter, DynamicLevel::Forte, 0, {}});
    score.insert_note(0, make_note(kQuarter, Step::C));

    const MidiRenderer renderer(make_orchestral_cc_expression_map());
    const RenderResult result = renderer.render(score);

    bool seen_controller = false;
    for (const TimedMidiEvent& event : result.events) {
        if (event.tick != kQuarter)
            continue;
        if (event.event.type == MidiEventType::ControlChange)
            seen_controller = true;
        if (event.event.type == MidiEventType::NoteOn && event.event.data2 > 0) {
            VCC_CHECK_MSG(seen_controller,
                          "a note-on preceded the controller for the same tick");
        }
    }
}

VCC_TEST(rendering_is_deterministic)
{
    // The diff layer is only meaningful if the same score renders
    // identically every time.
    Score score;
    score.add_dynamic(DynamicMarking{0, DynamicLevel::MezzoPiano, 0, {}});
    for (int index = 0; index < 12; ++index) {
        score.insert_note(0, make_note(index * (kQuarter / 2), static_cast<Step>(index % 7), 4, eighth(),
                                       index % 3 == 0 ? to_mask(Articulation::Accent) : 0));
    }

    const MidiRenderer renderer;
    const RenderResult first = renderer.render(score);
    const RenderResult second = renderer.render(score);

    VCC_CHECK_EQ(first.events.size(), second.events.size());
    for (std::size_t index = 0; index < first.events.size(); ++index) {
        VCC_CHECK_EQ(first.events[index].tick, second.events[index].tick);
        VCC_CHECK_EQ(first.events[index].event.type, second.events[index].event.type);
        VCC_CHECK_EQ(first.events[index].event.data1, second.events[index].event.data1);
        VCC_CHECK_EQ(first.events[index].event.data2, second.events[index].event.data2);
        VCC_CHECK_EQ(first.events[index].source_id, second.events[index].source_id);
    }
}

VCC_TEST(a_grand_staff_renders_both_hands)
{
    Score score;
    score.make_grand_staff();
    score.insert_note(0, make_note(0, Step::C, 5));  // Right hand.
    score.insert_note(1, make_note(0, Step::C, 3));  // Left hand.

    const MidiRenderer renderer;
    const RenderResult result = renderer.render(score);

    const std::vector<TimedMidiEvent> ons = note_ons(result);
    VCC_CHECK_EQ(ons.size(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(result.note_count, static_cast<std::size_t>(2));
}

VCC_TEST(rendering_a_range_covers_only_that_range)
{
    Score score;
    for (int index = 0; index < 10; ++index)
        score.insert_note(0, make_note(index * kQuarter, Step::C));

    MidiRenderer renderer;
    RenderOptions options;
    options.from_tick = (3 * kQuarter);
    options.to_tick = (6 * kQuarter);
    renderer.set_options(options);

    const std::vector<TimedMidiEvent> ons = note_ons(renderer.render(score));
    VCC_CHECK_EQ(ons.size(), static_cast<std::size_t>(3));
    for (const TimedMidiEvent& event : ons) {
        VCC_CHECK(event.tick >= static_cast<Ticks>((3 * kQuarter)));
        VCC_CHECK(event.tick < static_cast<Ticks>((6 * kQuarter)));
    }
}

VCC_TEST(the_midi_channel_is_honoured)
{
    Score score;
    score.insert_note(0, make_note(0, Step::C));

    MidiRenderer renderer;
    RenderOptions options;
    options.channel = 9;
    renderer.set_options(options);

    for (const TimedMidiEvent& event : renderer.render(score).events)
        VCC_CHECK_EQ(event.event.channel, static_cast<std::uint8_t>(9));
}

VCC_TEST(every_rendered_event_is_a_legal_midi_event)
{
    // Malformed MIDI would be rejected by the bridge's own validation, so
    // catching it here localizes the fault.
    Score score;
    score.add_dynamic(DynamicMarking{0, DynamicLevel::Pianississimo, 0, {}});
    Hairpin crescendo;
    crescendo.start_tick = 0;
    crescendo.end_tick = (4 * kQuarter);
    crescendo.has_start_level = true;
    crescendo.start_level = DynamicLevel::Pianississimo;
    crescendo.has_end_level = true;
    crescendo.end_level = DynamicLevel::Fortississimo;
    score.add_hairpin(crescendo);

    for (int index = 0; index < 8; ++index) {
        score.insert_note(0, make_note(index * (kQuarter / 2), static_cast<Step>(index % 7), 4, eighth(),
                                       to_mask(Articulation::Accent)
                                           | to_mask(Articulation::Staccato)));
    }

    for (const ExpressionMap& map : {make_default_expression_map(),
                                     make_orchestral_cc_expression_map()}) {
        const MidiRenderer renderer(map);
        const RenderResult result = renderer.render(score);
        VCC_CHECK(!result.events.empty());
        for (const TimedMidiEvent& event : result.events) {
            VCC_CHECK_MSG(vcc::music::is_valid(event.event),
                          "rendered an invalid MIDI event");
            VCC_CHECK(event.tick >= 0);
        }
    }
}

// --- Tempo / timeline -------------------------------------------------------

VCC_TEST(tick_to_sample_conversion_matches_the_tempo)
{
    // At 120 BPM a quarter note is half a second: 24000 samples at 48 kHz.
    Score score;
    TempoMarking tempo;
    tempo.beats_per_minute = 120.0;
    score.add_tempo(tempo);

    VCC_CHECK_EQ(ticks_to_samples(score, 0, 48000.0), static_cast<std::int64_t>(0));
    VCC_CHECK_EQ(ticks_to_samples(score, kQuarter, 48000.0), static_cast<std::int64_t>(24000));
    VCC_CHECK_EQ(ticks_to_samples(score, (4 * kQuarter), 48000.0), static_cast<std::int64_t>(96000));

    // At 60 BPM a quarter note is a full second.
    Score slow;
    TempoMarking slow_tempo;
    slow_tempo.beats_per_minute = 60.0;
    slow.add_tempo(slow_tempo);
    VCC_CHECK_EQ(ticks_to_samples(slow, kQuarter, 48000.0), static_cast<std::int64_t>(48000));
}

VCC_TEST(conversion_integrates_across_a_tempo_change)
{
    // A score that changes tempo mid-way must not be converted as though
    // one tempo applied throughout.
    Score score;
    TempoMarking first;
    first.beats_per_minute = 120.0;
    score.add_tempo(first);

    TempoMarking second;
    second.tick = kQuarter;      // After one quarter note.
    second.beats_per_minute = 60.0;
    score.add_tempo(second);

    // First quarter at 120 = 24000 samples; second at 60 = 48000 more.
    VCC_CHECK_EQ(ticks_to_samples(score, kQuarter, 48000.0), static_cast<std::int64_t>(24000));
    VCC_CHECK_EQ(ticks_to_samples(score, (2 * kQuarter), 48000.0), static_cast<std::int64_t>(72000));
}

VCC_TEST(sample_to_tick_conversion_round_trips)
{
    Score score;
    TempoMarking tempo;
    tempo.beats_per_minute = 100.0;
    score.add_tempo(tempo);

    for (Ticks tick = 0; tick <= 38400; tick += (kQuarter / 4)) {
        const std::int64_t samples = ticks_to_samples(score, tick, 48000.0);
        const Ticks back = samples_to_ticks(score, samples, 48000.0);
        // Within a tick: the round trip goes through a sample count, which
        // is a coarser grid at low tempos.
        VCC_CHECK_MSG(back >= tick - 1 && back <= tick + 1,
                      "tick " + std::to_string(tick) + " round-tripped to "
                          + std::to_string(back));
    }
}

VCC_TEST(conversion_handles_degenerate_input)
{
    const Score score;
    VCC_CHECK_EQ(ticks_to_samples(score, -kQuarter, 48000.0), static_cast<std::int64_t>(0));
    VCC_CHECK_EQ(ticks_to_samples(score, kQuarter, 0.0), static_cast<std::int64_t>(0));
    VCC_CHECK_EQ(ticks_to_samples(score, kQuarter, -48000.0), static_cast<std::int64_t>(0));
    VCC_CHECK_EQ(samples_to_ticks(score, -100, 48000.0), static_cast<Ticks>(0));
    VCC_CHECK_EQ(samples_to_ticks(score, 100, 0.0), static_cast<Ticks>(0));
}

// --- Expression map validation ---------------------------------------------

VCC_TEST(the_built_in_maps_are_valid)
{
    VCC_CHECK(make_default_expression_map().is_valid());
    VCC_CHECK(make_orchestral_cc_expression_map().is_valid());
}

VCC_TEST(an_invalid_map_is_detected)
{
    ExpressionMap map = make_default_expression_map();
    map.dynamic_velocity[0] = 0;  // Velocity 0 is a note-off.
    VCC_CHECK(!map.is_valid());

    map = make_default_expression_map();
    map.hairpin_resolution_ticks = 0;
    VCC_CHECK(!map.is_valid());

    map = make_default_expression_map();
    map.dynamics_cc = 200;
    VCC_CHECK(!map.is_valid());

    // A keyswitch at velocity 0 sends a note-off, selecting nothing.
    map = make_default_expression_map();
    ArticulationRule bad;
    bad.articulation = Articulation::Pizzicato;
    bad.action = ArticulationAction::Keyswitch;
    bad.keyswitch_velocity = 0;
    map.set_rule(bad);
    VCC_CHECK(!map.is_valid());
}

VCC_TEST(setting_a_rule_replaces_rather_than_duplicates)
{
    ExpressionMap map = make_default_expression_map();
    const std::size_t before = map.articulations.size();

    ArticulationRule accent;
    accent.articulation = Articulation::Accent;
    accent.action = ArticulationAction::ScaleVelocity;
    accent.velocity_scale = 1.9;
    map.set_rule(accent);

    VCC_CHECK_EQ(map.articulations.size(), before);
    VCC_CHECK_NEAR(map.rule_for(Articulation::Accent)->velocity_scale, 1.9, 1e-9);
}

VCC_TEST_MAIN("notation/MidiRenderer")
