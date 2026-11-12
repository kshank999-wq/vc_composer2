// A score as text (spec 02 sections 12, 13).
//
// The property that matters most here is not that the file parses - it is
// that a saved part comes back as the *same* part: the same notes at the
// same ticks with the same ids, playing the same MIDI. So the round trip is
// checked three ways: field by field, as canonical text, and by rendering
// both and comparing the events.
#include "vcc/notation/ScoreFile.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "vcc/core/Json.h"
#include "vcc/notation/MidiRenderer.h"
#include "vcc_test.h"

using namespace vcc::notation;

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

/// A score with something of everything in it: two staves, a transposing
/// one, meter and key changes, every kind of marking, a tuplet, a rest, a
/// chord, a tie and articulations.
Score a_rich_score()
{
    Score score;
    score.staff(0).name = "Clarinet in Bb";
    score.staff(0).clef = Clef::Treble;
    score.staff(0).transposition_semitones = -2;

    Staff second;
    second.name = "Bass clarinet";
    second.clef = Clef::Bass;
    second.transposition_semitones = -14;
    second.octave_shift = -1;
    score.add_staff(second);

    score.set_time_signature(0, vcc::music::TimeSignature{3, 4});
    score.set_time_signature(6 * kTicksPerQuarter, vcc::music::TimeSignature{7, 8});
    score.set_key_signature(0, KeySignature{-3, false});
    score.set_key_signature(3 * kTicksPerQuarter, KeySignature{2, true});

    // A plain note, an articulated and tied one, a dotted triplet, a rest,
    // and a chord.
    score.insert_note(0, note_at(0, Step::C));

    ScoreNote marked = note_at(kTicksPerQuarter, Step::D, 5, eighth());
    marked.articulations = to_mask(Articulation::Staccato) | to_mask(Articulation::Accent);
    marked.tied_to_next = true;
    marked.voice = 1;
    score.insert_note(0, marked);

    Duration awkward = eighth(1);
    awkward.tuplet = Tuplet{3, 2};
    ScoreNote tripleted = note_at(kTicksPerQuarter * 2, Step::E, 4, awkward);
    tripleted.pitch.alteration = -1;
    score.insert_note(0, tripleted);

    ScoreNote rest = note_at(kTicksPerQuarter * 3, Step::C, 4, half());
    rest.is_rest = true;
    score.insert_note(0, rest);

    score.insert_chord(1, 0, whole(), {Pitch{Step::G, 2, 0}, Pitch{Step::B, 2, -1}});

    score.add_dynamic(DynamicMarking{0, DynamicLevel::Pianissimo, 0, ""});
    score.add_dynamic(DynamicMarking{kTicksPerQuarter * 4, DynamicLevel::Forte, 1,
                                     "sotto voce"});

    Hairpin hairpin;
    hairpin.start_tick = 0;
    hairpin.end_tick = kTicksPerQuarter * 4;
    hairpin.kind = HairpinKind::Crescendo;
    hairpin.shape = CurveShape::SCurve;
    hairpin.voice = 1;
    hairpin.has_start_level = true;
    hairpin.start_level = DynamicLevel::Pianississimo;
    hairpin.has_end_level = true;
    hairpin.end_level = DynamicLevel::Fortissimo;
    score.add_hairpin(hairpin);

    // A second one with no levels at all, because their absence is what
    // tells the renderer to take them from the dynamics around it.
    Hairpin open_ended;
    open_ended.start_tick = kTicksPerQuarter * 6;
    open_ended.end_tick = kTicksPerQuarter * 8;
    open_ended.kind = HairpinKind::Diminuendo;
    score.add_hairpin(open_ended);

    score.add_slur(Slur{0, kTicksPerQuarter * 2, 0, 0});
    score.add_slur(Slur{kTicksPerQuarter * 4, kTicksPerQuarter * 6, 0, 1});
    score.add_tempo(TempoMarking{0, 88.0, NoteValue::Quarter, 0, "Andante"});
    score.add_tempo(TempoMarking{kTicksPerQuarter * 6, 60.0, NoteValue::Eighth, 1, "meno"});
    score.add_text(TextMarking{0, 0, "con sordino"});
    score.add_text(TextMarking{kTicksPerQuarter * 2, 1, "sul tasto"});

    return score;
}

bool same_note(const ScoreNote& a, const ScoreNote& b)
{
    return a.id == b.id && a.start_tick == b.start_tick && a.duration == b.duration
           && a.is_rest == b.is_rest && a.pitch == b.pitch && a.voice == b.voice
           && a.tied_to_next == b.tied_to_next && a.in_chord == b.in_chord
           && a.grace == b.grace && a.grace_order == b.grace_order
           && a.grace_slashed == b.grace_slashed && a.staff_shift == b.staff_shift
           && a.articulations == b.articulations;
}

bool same_event(const TimedMidiEvent& a, const TimedMidiEvent& b)
{
    return a.tick == b.tick && a.source_id == b.source_id && a.role == b.role
           && a.event.type == b.event.type && a.event.channel == b.event.channel
           && a.event.data1 == b.event.data1 && a.event.data2 == b.event.data2
           && a.event.bend == b.event.bend;
}

/// Saves and reloads, and requires that it worked.
ScoreLoadResult round_trip(const Score& original, Score& loaded)
{
    return score_from_json(score_to_json(original), loaded);
}

}  // namespace

// ---------------------------------------------------------------------------
// The names the file is written in
// ---------------------------------------------------------------------------

VCC_TEST(every_enumeration_reads_back_as_what_was_written)
{
    // Enumerations go into the file by name, and each parser is defined as
    // the writer's inverse. This is that property, over every value: a name
    // the writer can produce is a name the reader accepts, and reading it
    // gives back what was written.
    for (int index = 0; index <= 6; ++index) {
        const auto step = static_cast<Step>(index);
        Step read_back = Step::B;
        VCC_CHECK(step_from_string(to_string(step), read_back));
        VCC_CHECK_EQ(static_cast<int>(read_back), index);
    }
    for (int index = 0; index <= static_cast<int>(Clef::Percussion); ++index) {
        const auto clef = static_cast<Clef>(index);
        Clef read_back = Clef::Percussion;
        VCC_CHECK(clef_from_string(to_string(clef), read_back));
        VCC_CHECK_EQ(static_cast<int>(read_back), index);
    }
    for (int index = 0; index <= static_cast<int>(NoteValue::SixtyFourth); ++index) {
        const auto value = static_cast<NoteValue>(index);
        NoteValue read_back = NoteValue::Whole;
        VCC_CHECK(note_value_from_string(to_string(value), read_back));
        VCC_CHECK_EQ(static_cast<int>(read_back), index);
    }
    for (int index = 0; index < static_cast<int>(kDynamicLevelCount); ++index) {
        const auto level = static_cast<DynamicLevel>(index);
        DynamicLevel read_back = DynamicLevel::MezzoForte;
        VCC_CHECK(dynamic_from_string(to_string(level), read_back));
        VCC_CHECK_EQ(static_cast<int>(read_back), index);
    }
    for (const HairpinKind kind : {HairpinKind::Crescendo, HairpinKind::Diminuendo}) {
        HairpinKind read_back = HairpinKind::Crescendo;
        VCC_CHECK(hairpin_kind_from_string(to_string(kind), read_back));
        VCC_CHECK_EQ(static_cast<int>(read_back), static_cast<int>(kind));
    }
    for (int index = 0; index <= static_cast<int>(CurveShape::SCurve); ++index) {
        const auto shape = static_cast<CurveShape>(index);
        CurveShape read_back = CurveShape::Linear;
        VCC_CHECK(curve_shape_from_string(to_string(shape), read_back));
        VCC_CHECK_EQ(static_cast<int>(read_back), index);
    }
    for (const Articulation articulation : all_articulations()) {
        Articulation read_back = Articulation::None;
        VCC_CHECK(articulation_from_string(to_string(articulation), read_back));
        VCC_CHECK_EQ(static_cast<int>(read_back), static_cast<int>(articulation));
    }
}

VCC_TEST(a_name_this_build_does_not_write_is_not_a_name_it_reads)
{
    // Refused rather than defaulted: a value this build does not know is a
    // fact to report, not one to replace with a plausible guess.
    Step step = Step::C;
    VCC_CHECK(!step_from_string("H", step));
    VCC_CHECK(!step_from_string("", step));
    VCC_CHECK(!step_from_string("c", step));  // Case matters; the writer writes "C".

    Clef clef = Clef::Treble;
    VCC_CHECK(!clef_from_string("soprano", clef));

    NoteValue value = NoteValue::Quarter;
    VCC_CHECK(!note_value_from_string("128th", value));

    CurveShape shape = CurveShape::Linear;
    VCC_CHECK(!curve_shape_from_string("cubic", shape));

    HairpinKind kind = HairpinKind::Crescendo;
    VCC_CHECK(!hairpin_kind_from_string("swell", kind));

    Articulation articulation = Articulation::None;
    VCC_CHECK(!articulation_from_string("glissando", articulation));
}

VCC_TEST(every_articulation_in_the_enum_is_in_the_list)
{
    // A new articulation added to the enum and not to all_articulations()
    // would never reach a file, a diagnostic or an expression map, and the
    // omission would be invisible. This is what makes it visible: every bit
    // the enum uses either has a name in the list or has no name at all.
    const std::vector<Articulation>& list = all_articulations();

    for (int bit = 0; bit < 32; ++bit) {
        const auto articulation = static_cast<Articulation>(1u << bit);
        const bool named = std::string(to_string(articulation)) != "none";
        bool listed = false;
        for (const Articulation entry : list) {
            if (entry == articulation)
                listed = true;
        }
        VCC_CHECK_MSG(named == listed,
                      std::string("bit ") + std::to_string(bit) + " ('"
                          + to_string(articulation)
                          + "') is named but not listed, or listed but not named");
    }

    // And no duplicates, which would write an articulation twice.
    for (std::size_t i = 0; i < list.size(); ++i) {
        for (std::size_t j = i + 1; j < list.size(); ++j)
            VCC_CHECK(list[i] != list[j]);
    }
}

// ---------------------------------------------------------------------------
// The round trip
// ---------------------------------------------------------------------------

VCC_TEST(a_saved_score_comes_back_note_for_note)
{
    const Score original = a_rich_score();

    Score loaded;
    const ScoreLoadResult result = round_trip(original, loaded);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_MSG(result.warnings.empty(),
                  result.warnings.empty() ? "" : result.warnings.front());
    VCC_CHECK_EQ(result.notes_read, original.total_note_count());
    VCC_CHECK_EQ(result.staves_read, original.staff_count());

    VCC_CHECK_EQ(loaded.staff_count(), original.staff_count());
    for (std::size_t index = 0; index < original.staff_count(); ++index) {
        const Staff& a = original.staff(index);
        const Staff& b = loaded.staff(index);
        VCC_CHECK_EQ(b.name, a.name);
        VCC_CHECK_EQ(static_cast<int>(b.clef), static_cast<int>(a.clef));
        VCC_CHECK_EQ(b.transposition_semitones, a.transposition_semitones);
        VCC_CHECK_EQ(b.octave_shift, a.octave_shift);

        const std::vector<ScoreNote>& from = original.notes(index);
        const std::vector<ScoreNote>& to = loaded.notes(index);
        VCC_CHECK_EQ(to.size(), from.size());
        for (std::size_t note = 0; note < from.size() && note < to.size(); ++note) {
            VCC_CHECK_MSG(same_note(from[note], to[note]),
                          "staff " + std::to_string(index) + " note "
                              + std::to_string(note) + " came back different");
        }
    }
}

VCC_TEST(a_grace_note_comes_back_as_one_and_an_older_file_reads_as_plain_notes)
{
    Score original;
    ScoreNote grace;
    grace.start_tick = 960;
    grace.duration = eighth();
    grace.pitch = Pitch{Step::D, 5, 0};
    grace.grace = true;
    grace.grace_order = 1;
    grace.grace_slashed = false;
    original.insert_note(0, grace);
    ScoreNote principal;
    principal.start_tick = 960;
    principal.duration = quarter();
    principal.pitch = Pitch{Step::E, 5, 0};
    principal.staff_shift = -1;
    original.insert_note(0, principal);

    Score loaded;
    const ScoreLoadResult result = round_trip(original, loaded);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(loaded.notes(0).size(), std::size_t{2});
    if (loaded.notes(0).size() == 2) {
        VCC_CHECK(same_note(original.notes(0)[0], loaded.notes(0)[0]));
        VCC_CHECK(same_note(original.notes(0)[1], loaded.notes(0)[1]));
        VCC_CHECK(loaded.notes(0)[0].grace);
        VCC_CHECK_EQ(loaded.notes(0)[0].grace_order, std::uint8_t{1});
        VCC_CHECK(!loaded.notes(0)[0].grace_slashed);
        VCC_CHECK(!loaded.notes(0)[1].grace);
        VCC_CHECK_EQ(loaded.notes(0)[1].staff_shift, std::int8_t{-1});
        VCC_CHECK_EQ(loaded.notes(0)[0].staff_shift, std::int8_t{0});
    }
    VCC_CHECK(score_to_json(original).to_text(2).find("\"staff_shift\": -1") != std::string::npos);

    // The fields are written for every note, so a file's shape does not
    // depend on whether it has grace notes.
    const std::string text = score_to_json(original).to_text(2);
    VCC_CHECK(text.find("\"grace\": false") != std::string::npos);
    VCC_CHECK(text.find("\"grace\": true") != std::string::npos);
    VCC_CHECK(text.find("\"grace_slashed\": false") != std::string::npos);

    // A file from before there were grace notes says nothing about them,
    // and reads as the plain notes it holds.
    vcc::json::Value older = score_to_json(original);
    vcc::json::Value staves = older["staves"];
    // Rebuilt without the grace fields.
    vcc::json::Value staff = staves.at(0);
    vcc::json::Value notes = vcc::json::Value::make_array();
    for (std::size_t index = 0; index < staff["notes"].size(); ++index) {
        vcc::json::Value note = staff["notes"].at(index);
        note.remove("grace");
        note.remove("grace_order");
        note.remove("grace_slashed");
        note.remove("staff_shift");
        notes.push_back(note);
    }
    staff.set("notes", notes);
    vcc::json::Value rebuilt_staves = vcc::json::Value::make_array();
    rebuilt_staves.push_back(staff);
    older.set("staves", rebuilt_staves);
    Score plain;
    const ScoreLoadResult read = score_from_json(older, plain);
    VCC_CHECK_MSG(read.ok, read.error);
    VCC_CHECK_EQ(plain.notes(0).size(), std::size_t{2});
    for (const ScoreNote& note : plain.notes(0))
        VCC_CHECK(!note.grace && note.grace_slashed && note.staff_shift == 0);
}

VCC_TEST(a_marking_keeps_its_staff_through_the_file_and_an_older_file_reads_as_the_first)
{
    Score original;
    original.make_grand_staff();
    DynamicMarking marking{960, DynamicLevel::Piano, 0, ""};
    marking.staff = 1;
    original.add_dynamic(marking);
    Hairpin hairpin;
    hairpin.start_tick = 0;
    hairpin.end_tick = 960;
    hairpin.staff = 1;
    original.add_hairpin(hairpin);

    Score loaded;
    VCC_CHECK(round_trip(original, loaded).ok);
    VCC_CHECK_EQ(loaded.dynamics().size(), std::size_t{1});
    VCC_CHECK_EQ(loaded.hairpins().size(), std::size_t{1});
    if (!loaded.dynamics().empty())
        VCC_CHECK_EQ(loaded.dynamics().front().staff, std::uint8_t{1});
    if (!loaded.hairpins().empty())
        VCC_CHECK_EQ(loaded.hairpins().front().staff, std::uint8_t{1});

    // Without the field, as every file before had it: the first staff.
    vcc::json::Value older = score_to_json(original);
    vcc::json::Value dynamics = vcc::json::Value::make_array();
    vcc::json::Value entry = older["dynamics"].at(0);
    entry.remove("staff");
    dynamics.push_back(entry);
    older.set("dynamics", dynamics);
    vcc::json::Value hairpins = vcc::json::Value::make_array();
    vcc::json::Value pin = older["hairpins"].at(0);
    pin.remove("staff");
    hairpins.push_back(pin);
    older.set("hairpins", hairpins);
    Score plain;
    VCC_CHECK(score_from_json(older, plain).ok);
    VCC_CHECK_EQ(plain.dynamics().front().staff, std::uint8_t{0});
    VCC_CHECK_EQ(plain.hairpins().front().staff, std::uint8_t{0});
}

VCC_TEST(every_marking_comes_back_too)
{
    const Score original = a_rich_score();
    Score loaded;
    VCC_CHECK(round_trip(original, loaded).ok);

    VCC_CHECK_EQ(loaded.time_signature_changes().size(),
                 original.time_signature_changes().size());
    for (std::size_t i = 0; i < original.time_signature_changes().size(); ++i) {
        VCC_CHECK_EQ(loaded.time_signature_changes()[i].tick,
                     original.time_signature_changes()[i].tick);
        VCC_CHECK(loaded.time_signature_changes()[i].time_signature
                  == original.time_signature_changes()[i].time_signature);
    }

    VCC_CHECK_EQ(loaded.key_signature_changes().size(),
                 original.key_signature_changes().size());
    for (std::size_t i = 0; i < original.key_signature_changes().size(); ++i) {
        VCC_CHECK_EQ(loaded.key_signature_changes()[i].tick,
                     original.key_signature_changes()[i].tick);
        VCC_CHECK(loaded.key_signature_changes()[i].key
                  == original.key_signature_changes()[i].key);
    }

    VCC_CHECK_EQ(loaded.dynamics().size(), original.dynamics().size());
    for (std::size_t i = 0; i < original.dynamics().size(); ++i) {
        VCC_CHECK_EQ(loaded.dynamics()[i].tick, original.dynamics()[i].tick);
        VCC_CHECK_EQ(static_cast<int>(loaded.dynamics()[i].level),
                     static_cast<int>(original.dynamics()[i].level));
        VCC_CHECK_EQ(loaded.dynamics()[i].voice, original.dynamics()[i].voice);
        VCC_CHECK_EQ(loaded.dynamics()[i].staff, original.dynamics()[i].staff);
        VCC_CHECK_EQ(loaded.dynamics()[i].text, original.dynamics()[i].text);
    }

    VCC_CHECK_EQ(loaded.hairpins().size(), original.hairpins().size());
    for (std::size_t i = 0; i < original.hairpins().size(); ++i) {
        const Hairpin& a = original.hairpins()[i];
        const Hairpin& b = loaded.hairpins()[i];
        VCC_CHECK_EQ(b.start_tick, a.start_tick);
        VCC_CHECK_EQ(b.end_tick, a.end_tick);
        VCC_CHECK_EQ(static_cast<int>(b.kind), static_cast<int>(a.kind));
        VCC_CHECK_EQ(static_cast<int>(b.shape), static_cast<int>(a.shape));
        VCC_CHECK_EQ(b.voice, a.voice);

        // The absence of an endpoint level is meaning, not an omission:
        // it tells the renderer to take the level from the dynamics around
        // the hairpin.
        VCC_CHECK_EQ(b.has_start_level, a.has_start_level);
        VCC_CHECK_EQ(b.has_end_level, a.has_end_level);
        if (a.has_start_level)
            VCC_CHECK_EQ(static_cast<int>(b.start_level), static_cast<int>(a.start_level));
        if (a.has_end_level)
            VCC_CHECK_EQ(static_cast<int>(b.end_level), static_cast<int>(a.end_level));
    }

    VCC_CHECK_EQ(loaded.slurs().size(), original.slurs().size());
    for (std::size_t i = 0; i < original.slurs().size(); ++i) {
        VCC_CHECK_EQ(loaded.slurs()[i].start_tick, original.slurs()[i].start_tick);
        VCC_CHECK_EQ(loaded.slurs()[i].staff, original.slurs()[i].staff);
    }
    VCC_CHECK_EQ(loaded.texts().size(), original.texts().size());
    for (std::size_t i = 0; i < original.texts().size(); ++i) {
        VCC_CHECK_EQ(loaded.texts()[i].text, original.texts()[i].text);
        VCC_CHECK_EQ(loaded.texts()[i].voice, original.texts()[i].voice);
        VCC_CHECK_EQ(loaded.texts()[i].tick, original.texts()[i].tick);
    }

    VCC_CHECK_EQ(loaded.tempos().size(), original.tempos().size());
    for (std::size_t i = 0; i < original.tempos().size(); ++i) {
        const TempoMarking& a = original.tempos()[i];
        const TempoMarking& b = loaded.tempos()[i];
        VCC_CHECK_EQ(b.tick, a.tick);
        VCC_CHECK_NEAR(b.beats_per_minute, a.beats_per_minute, 1e-12);
        VCC_CHECK_EQ(static_cast<int>(b.beat_unit), static_cast<int>(a.beat_unit));
        VCC_CHECK_EQ(b.beat_unit_dots, a.beat_unit_dots);
        VCC_CHECK_EQ(b.text, a.text);
    }
}

VCC_TEST(saving_what_was_loaded_produces_the_same_text)
{
    // The canonical form of the whole score in one comparison. A field the
    // writer emits and the reader ignores shows up here and nowhere else.
    const Score original = a_rich_score();
    Score loaded;
    VCC_CHECK(round_trip(original, loaded).ok);

    const std::string before = score_to_json(original).to_text(2);
    const std::string after = score_to_json(loaded).to_text(2);
    VCC_CHECK_EQ(after.size(), before.size());
    VCC_CHECK_MSG(after == before, "the second save differs from the first");

    // And a third pass changes nothing either.
    Score again;
    VCC_CHECK(score_from_json(score_to_json(loaded), again).ok);
    VCC_CHECK(score_to_json(again).to_text(2) == before);
}

VCC_TEST(a_reloaded_score_plays_exactly_the_same_midi)
{
    // The strongest statement that nothing musically meaningful was lost:
    // the two scores render to identical events, ids and roles included.
    const Score original = a_rich_score();
    Score loaded;
    VCC_CHECK(round_trip(original, loaded).ok);

    const MidiRenderer renderer;
    const RenderResult from = renderer.render(original);
    const RenderResult to = renderer.render(loaded);

    VCC_CHECK_EQ(to.events.size(), from.events.size());
    VCC_CHECK(to.events.size() > 0);
    for (std::size_t i = 0; i < from.events.size() && i < to.events.size(); ++i) {
        VCC_CHECK_MSG(same_event(from.events[i], to.events[i]),
                      "event " + std::to_string(i) + " differs after a reload");
    }
    VCC_CHECK_EQ(to.note_count, from.note_count);
    VCC_CHECK_EQ(to.controller_count, from.controller_count);
    VCC_CHECK_EQ(to.keyswitch_count, from.keyswitch_count);
}

VCC_TEST(event_ids_survive_and_the_counter_comes_back_ahead_of_them)
{
    // Spec 13's requirement, and the reason restore_note exists. A reload
    // that renumbered notes would make the diff layer see a whole cue as
    // deleted and re-added the first time the composer touched it.
    const Score original = a_rich_score();
    Score loaded;
    VCC_CHECK(round_trip(original, loaded).ok);

    EventId highest = kInvalidEventId;
    for (std::size_t staff = 0; staff < original.staff_count(); ++staff) {
        for (const ScoreNote& note : original.notes(staff)) {
            VCC_CHECK_MSG(loaded.find_note(note.id) != nullptr,
                          "note " + std::to_string(note.id) + " lost its id");
            VCC_CHECK_EQ(loaded.staff_index_of(note.id), staff);
            if (note.id > highest)
                highest = note.id;
        }
    }
    VCC_CHECK(highest != kInvalidEventId);

    // A note entered after the reload must not collide with one that came
    // out of the file.
    const EventId fresh = loaded.insert_note(0, note_at(kTicksPerQuarter * 20, Step::A));
    VCC_CHECK(fresh != kInvalidEventId);
    VCC_CHECK_MSG(fresh > highest, "a new note reused an id from the file");
}

VCC_TEST(an_empty_score_round_trips)
{
    const Score original;
    Score loaded;
    loaded.insert_note(0, note_at(0, Step::C));  // Something to be replaced.

    const ScoreLoadResult result = round_trip(original, loaded);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(loaded.total_note_count(), static_cast<std::size_t>(0));
    VCC_CHECK_EQ(loaded.staff_count(), static_cast<std::size_t>(1));
    VCC_CHECK(score_to_json(loaded).to_text() == score_to_json(original).to_text());
}

VCC_TEST(loading_replaces_whatever_the_score_held)
{
    const Score original = a_rich_score();

    Score loaded;
    loaded.make_grand_staff();
    for (int i = 0; i < 10; ++i)
        loaded.insert_note(0, note_at(i * kTicksPerQuarter, Step::F));
    loaded.add_text(TextMarking{0, 0, "stale"});

    VCC_CHECK(round_trip(original, loaded).ok);
    VCC_CHECK_EQ(loaded.total_note_count(), original.total_note_count());
    VCC_CHECK_EQ(loaded.staff_count(), original.staff_count());
    for (const TextMarking& marking : loaded.texts())
        VCC_CHECK(marking.text != "stale");
}

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------

VCC_TEST(something_that_is_not_a_score_is_refused_and_leaves_nothing_behind)
{
    // Half a cue the composer might mistake for the whole one is worse than
    // none, so a failed load empties the score.
    // Compared against a score straight from the constructor, so "empty"
    // means empty - not merely "no notes", which a leftover marking or an
    // extra staff would still satisfy.
    const std::string fresh = score_to_json(Score{}).to_text();

    for (const char* text : {"42", "\"a score\"", "[]", "null", "{}",
                             "{\"staves\":[]}", "{\"staves\":{}}",
                             "{\"staves\":[42]}"}) {
        Score loaded;
        loaded.make_grand_staff();
        loaded.insert_note(0, note_at(0, Step::C));
        loaded.add_text(TextMarking{0, 0, "stale"});
        loaded.add_tempo(TempoMarking{0, 72.0, NoteValue::Quarter, 0, ""});

        const ScoreLoadResult result =
            score_from_json(vcc::json::parse_value(text), loaded);
        VCC_CHECK_MSG(!result.ok, std::string("accepted: ") + text);
        VCC_CHECK(!result.error.empty());
        VCC_CHECK_MSG(score_to_json(loaded).to_text() == fresh,
                      std::string("left something behind for: ") + text);
    }
}

// ---------------------------------------------------------------------------
// Partly readable files
// ---------------------------------------------------------------------------

VCC_TEST(a_note_with_no_id_is_skipped_and_named)
{
    // Defaulting an id would hand the note somebody else's identity, and
    // dropping it silently would lose music without saying so.
    const std::string text =
        "{\"staves\":[{\"clef\":\"treble\",\"notes\":["
        "{\"id\":7,\"tick\":0,\"value\":\"quarter\",\"step\":\"C\",\"octave\":4},"
        "{\"tick\":960,\"value\":\"quarter\",\"step\":\"D\",\"octave\":4},"
        "{\"id\":9,\"tick\":1920,\"value\":\"quarter\",\"step\":\"E\",\"octave\":4}"
        "]}]}";

    Score loaded;
    const ScoreLoadResult result = score_from_json(vcc::json::parse_value(text), loaded);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.notes_read, static_cast<std::size_t>(2));
    VCC_CHECK_EQ(loaded.total_note_count(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(result.warnings.size(), static_cast<std::size_t>(1));
    VCC_CHECK(result.warnings.front().find("no id") != std::string::npos);
    VCC_CHECK(result.warnings.front().find("note 2") != std::string::npos);
    VCC_CHECK(!result.carried_everything());

    // The two that were readable are there, with their own ids.
    VCC_CHECK(loaded.find_note(7) != nullptr);
    VCC_CHECK(loaded.find_note(9) != nullptr);
}

VCC_TEST(a_repeated_id_is_skipped_rather_than_overwriting_a_note)
{
    const std::string text =
        "{\"staves\":[{\"clef\":\"treble\",\"notes\":["
        "{\"id\":7,\"tick\":0,\"value\":\"quarter\",\"step\":\"C\",\"octave\":4},"
        "{\"id\":7,\"tick\":960,\"value\":\"quarter\",\"step\":\"D\",\"octave\":4}"
        "]}]}";

    Score loaded;
    const ScoreLoadResult result = score_from_json(vcc::json::parse_value(text), loaded);
    VCC_CHECK(result.ok);
    VCC_CHECK_EQ(result.notes_read, static_cast<std::size_t>(1));
    VCC_CHECK_EQ(result.warnings.size(), static_cast<std::size_t>(1));
    VCC_CHECK(result.warnings.front().find("already in use") != std::string::npos);

    // The first one kept its place: the second did not overwrite it.
    const ScoreNote* kept = loaded.find_note(7);
    VCC_CHECK(kept != nullptr);
    if (kept != nullptr)
        VCC_CHECK_EQ(kept->start_tick, static_cast<Ticks>(0));
}

VCC_TEST(an_articulation_this_build_does_not_know_costs_the_articulation_not_the_note)
{
    const std::string text =
        "{\"staves\":[{\"clef\":\"treble\",\"notes\":["
        "{\"id\":1,\"tick\":0,\"value\":\"quarter\",\"step\":\"C\",\"octave\":4,"
        "\"articulations\":[\"staccato\",\"flutter tongue\",\"accent\"]}"
        "]}]}";

    Score loaded;
    const ScoreLoadResult result = score_from_json(vcc::json::parse_value(text), loaded);
    VCC_CHECK(result.ok);
    VCC_CHECK_EQ(result.notes_read, static_cast<std::size_t>(1));

    const ScoreNote* note = loaded.find_note(1);
    VCC_CHECK(note != nullptr);
    if (note != nullptr) {
        VCC_CHECK(has_articulation(note->articulations, Articulation::Staccato));
        VCC_CHECK(has_articulation(note->articulations, Articulation::Accent));
    }

    VCC_CHECK_EQ(result.warnings.size(), static_cast<std::size_t>(1));
    VCC_CHECK(result.warnings.front().find("flutter tongue") != std::string::npos);
}

VCC_TEST(a_clef_this_build_does_not_know_costs_the_clef_not_the_staff)
{
    const std::string text =
        "{\"staves\":[{\"clef\":\"soprano\",\"name\":\"Voice\",\"notes\":["
        "{\"id\":1,\"tick\":0,\"value\":\"quarter\",\"step\":\"C\",\"octave\":4}"
        "]}]}";

    Score loaded;
    const ScoreLoadResult result = score_from_json(vcc::json::parse_value(text), loaded);
    VCC_CHECK(result.ok);
    VCC_CHECK_EQ(result.notes_read, static_cast<std::size_t>(1));
    VCC_CHECK_EQ(loaded.staff(0).name, std::string("Voice"));
    VCC_CHECK_EQ(result.warnings.size(), static_cast<std::size_t>(1));
    VCC_CHECK(result.warnings.front().find("soprano") != std::string::npos);
}

VCC_TEST(an_unreadable_note_value_skips_the_note_and_says_which)
{
    const std::string text =
        "{\"staves\":[{\"clef\":\"treble\",\"notes\":["
        "{\"id\":1,\"tick\":0,\"value\":\"128th\",\"step\":\"C\",\"octave\":4},"
        "{\"id\":2,\"tick\":0,\"step\":\"C\",\"octave\":4}"
        "]}]}";

    Score loaded;
    const ScoreLoadResult result = score_from_json(vcc::json::parse_value(text), loaded);
    VCC_CHECK(result.ok);
    VCC_CHECK_EQ(result.notes_read, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(result.warnings.size(), static_cast<std::size_t>(2));
    VCC_CHECK(result.warnings[0].find("128th") != std::string::npos);
    VCC_CHECK(result.warnings[1].find("no note value") != std::string::npos);
}

VCC_TEST(a_marking_the_model_will_not_hold_is_skipped_and_named)
{
    // A meter whose denominator is not a power of two, a key beyond seven
    // accidentals, a hairpin with no length: the model refuses each, and the
    // file says what was lost rather than appearing to load cleanly.
    const std::string text =
        "{\"staves\":[{\"clef\":\"treble\",\"notes\":[]}],"
        "\"time_signatures\":[{\"tick\":0,\"numerator\":4,\"denominator\":6}],"
        "\"key_signatures\":[{\"tick\":0,\"fifths\":9,\"minor\":false}],"
        "\"hairpins\":[{\"start_tick\":100,\"end_tick\":100,\"kind\":\"crescendo\"}],"
        "\"slurs\":[{\"start_tick\":10,\"end_tick\":5}],"
        "\"tempos\":[{\"tick\":0,\"bpm\":0}],"
        "\"texts\":[{\"tick\":0,\"text\":\"\"}],"
        "\"dynamics\":[{\"tick\":0,\"level\":\"mezzo\"}]}";

    Score loaded;
    const ScoreLoadResult result = score_from_json(vcc::json::parse_value(text), loaded);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.markings_read, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(result.warnings.size(), static_cast<std::size_t>(7));

    // The score is left as a score, with its own defaults in force.
    VCC_CHECK(loaded.time_signature_at(0) == vcc::music::TimeSignature{4, 4});
    VCC_CHECK(loaded.hairpins().empty());
    VCC_CHECK(loaded.slurs().empty());
    VCC_CHECK(loaded.tempos().empty());
    VCC_CHECK(loaded.texts().empty());
    VCC_CHECK(loaded.dynamics().empty());
}

VCC_TEST(a_staff_that_is_not_a_staff_is_skipped_and_the_others_are_read)
{
    const std::string text =
        "{\"staves\":["
        "{\"clef\":\"treble\",\"notes\":[{\"id\":1,\"tick\":0,\"value\":\"whole\","
        "\"step\":\"C\",\"octave\":4}]},"
        "42,"
        "{\"clef\":\"bass\",\"notes\":[{\"id\":2,\"tick\":0,\"value\":\"whole\","
        "\"step\":\"C\",\"octave\":2}]}"
        "]}";

    Score loaded;
    const ScoreLoadResult result = score_from_json(vcc::json::parse_value(text), loaded);
    VCC_CHECK(result.ok);
    VCC_CHECK_EQ(result.staves_read, static_cast<std::size_t>(2));
    VCC_CHECK_EQ(loaded.staff_count(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(static_cast<int>(loaded.staff(1).clef), static_cast<int>(Clef::Bass));
    VCC_CHECK_EQ(result.notes_read, static_cast<std::size_t>(2));
    VCC_CHECK_EQ(result.warnings.size(), static_cast<std::size_t>(1));
}

VCC_TEST(a_slur_without_a_staff_is_on_the_first_and_one_beyond_the_part_is_moved_there)
{
    // Files from before slurs carried a staff name none; the first staff is
    // the only one they could have meant, and no warning is owed. A staff
    // the part does not have is a different matter: said, and read as the
    // first rather than drawn on a staff that is not there.
    const std::string legacy =
        "{\"staves\":[{\"notes\":[]},{\"notes\":[]}],"
        "\"slurs\":[{\"start_tick\":0,\"end_tick\":960,\"voice\":0}]}";
    Score loaded;
    ScoreLoadResult result = score_from_json(vcc::json::parse_value(legacy), loaded);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK(result.warnings.empty());
    VCC_CHECK_EQ(loaded.slurs().size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(loaded.slurs().front().staff, static_cast<std::uint8_t>(0));

    const std::string beyond =
        "{\"staves\":[{\"notes\":[]},{\"notes\":[]}],"
        "\"slurs\":[{\"start_tick\":0,\"end_tick\":960,\"voice\":0,\"staff\":5}]}";
    Score moved;
    result = score_from_json(vcc::json::parse_value(beyond), moved);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.warnings.size(), static_cast<std::size_t>(1));
    VCC_CHECK(!result.warnings.empty() && result.warnings.front().find("staff") != std::string::npos);
    VCC_CHECK_EQ(moved.slurs().size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(moved.slurs().front().staff, static_cast<std::uint8_t>(0));

    // And the second staff is a staff the part has.
    const std::string second =
        "{\"staves\":[{\"notes\":[]},{\"notes\":[]}],"
        "\"slurs\":[{\"start_tick\":0,\"end_tick\":960,\"voice\":0,\"staff\":1}]}";
    Score lower;
    result = score_from_json(vcc::json::parse_value(second), lower);
    VCC_CHECK(result.warnings.empty());
    VCC_CHECK_EQ(lower.slurs().front().staff, static_cast<std::uint8_t>(1));
}

VCC_TEST(a_missing_field_takes_the_models_own_default)
{
    // Only the fields whose absence would silently move or renumber music
    // are required. The rest default, so a hand-written or older file is
    // still readable.
    const std::string text =
        "{\"staves\":[{\"notes\":["
        "{\"id\":1,\"tick\":0,\"value\":\"quarter\"}"
        "]}]}";

    Score loaded;
    const ScoreLoadResult result = score_from_json(vcc::json::parse_value(text), loaded);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK(result.warnings.empty());
    VCC_CHECK_EQ(result.notes_read, static_cast<std::size_t>(1));

    const ScoreNote* note = loaded.find_note(1);
    VCC_CHECK(note != nullptr);
    if (note != nullptr) {
        VCC_CHECK_EQ(static_cast<int>(note->pitch.step), static_cast<int>(Step::C));
        VCC_CHECK_EQ(note->pitch.octave, 4);
        VCC_CHECK_EQ(note->voice, static_cast<std::uint8_t>(0));
        VCC_CHECK(!note->is_rest);
        VCC_CHECK_EQ(note->articulations, static_cast<ArticulationMask>(0));
    }
    VCC_CHECK_EQ(static_cast<int>(loaded.staff(0).clef), static_cast<int>(Clef::Treble));
}

VCC_TEST(the_written_file_is_readable_by_a_person)
{
    // Not decoration: a composer whose part will not open needs to be able
    // to look at it, and a diff of two saves should show what changed.
    const std::string written = score_to_json(a_rich_score()).to_text(2);

    VCC_CHECK(written.find("\"clef\": \"treble\"") != std::string::npos);
    VCC_CHECK(written.find("\"Clarinet in Bb\"") != std::string::npos);
    VCC_CHECK(written.find("\"value\": \"quarter\"") != std::string::npos);
    VCC_CHECK(written.find("\"staccato\"") != std::string::npos);
    VCC_CHECK(written.find("\"Andante\"") != std::string::npos);

    // Names, never enum numbers: a file that said 0 would stop meaning what
    // it said the moment somebody inserted a value into the enum.
    VCC_CHECK(written.find("\"clef\": 0") == std::string::npos);
    VCC_CHECK(written.find("\"value\": 3") == std::string::npos);
}

// --- A corrupt file, and what it is allowed to do ---------------------------
//
// A score file is read from disk, so it is the one input here that arrives
// already wrong: a partial write, a bad sector, a sync conflict, an older or
// newer build, a collaborator's copy. What matters is not that the loader
// survives - it did - but *how* it fails. A note it cannot read faithfully
// must be refused and named, never quietly replaced by a plausible one.

namespace {

/// One note with `fields` spliced into it, wrapped in the smallest score
/// that will carry it.
std::string one_note_document(const std::string& fields)
{
    return "{\"staves\":[{\"clef\":\"treble\",\"notes\":[{\"id\":1,\"step\":\"G\","
           "\"tick\":0,\"value\":\"quarter\"," + fields + "}]}]}";
}

ScoreLoadResult load_text(const std::string& text, Score& out)
{
    vcc::json::ParseError error;
    const vcc::json::Value value = vcc::json::parse_value(text, error);
    VCC_CHECK(!error.failed);
    return score_from_json(value, out);
}

}  // namespace

VCC_TEST(an_octave_too_large_for_the_field_is_refused_rather_than_defaulted)
{
    // The bug this section was written for. `pitch.octave` is an int8_t, so
    // an octave of 1000 did not fit; the read reported failure, the caller
    // ignored it, and the default of 4 stood. The note loaded at a pitch
    // nobody wrote, with no warning - while an octave of 12, which *does*
    // fit and is merely out of musical range, was refused and named.
    //
    // So the more corrupt the file, the quieter the loader became. These two
    // have to behave the same way.
    for (const char* octave : {"12", "100", "1000", "32767", "-32768"}) {
        Score score;
        const ScoreLoadResult result =
            load_text(one_note_document(std::string("\"octave\":") + octave), score);

        VCC_CHECK(result.ok);
        VCC_CHECK_EQ(result.notes_read, std::size_t{0});
        VCC_CHECK(!result.warnings.empty());
        VCC_CHECK(score.notes(0).empty());
    }
}

VCC_TEST(a_refused_octave_says_it_was_the_octave)
{
    // "something was wrong with note 1" sends a composer looking through
    // every field. The loader knows which one it was.
    Score score;
    const ScoreLoadResult result = load_text(one_note_document("\"octave\":9000"), score);

    VCC_CHECK(!result.warnings.empty());
    VCC_CHECK(result.warnings.front().find("octave") != std::string::npos);
}

VCC_TEST(an_absent_field_still_takes_its_default_in_silence)
{
    // The other half, and the one a fix for the above could easily break. A
    // field that is simply *not there* is ordinary - an older file did not
    // carry it - and must load without a word.
    Score score;
    const ScoreLoadResult result = load_text(one_note_document("\"rest\":false"), score);

    VCC_CHECK(result.ok);
    VCC_CHECK_EQ(result.notes_read, std::size_t{1});
    VCC_CHECK(result.carried_everything());
    VCC_CHECK_EQ(score.notes(0).front().pitch.octave, std::int8_t{4});
}

VCC_TEST(a_field_of_the_wrong_type_is_refused_rather_than_ignored)
{
    // A string where a number belongs is not an absent field. Treating it as
    // one is the same silent substitution by another route.
    Score score;
    const ScoreLoadResult result = load_text(one_note_document("\"octave\":\"high\""), score);

    VCC_CHECK_EQ(result.notes_read, std::size_t{0});
    VCC_CHECK(!result.warnings.empty());
}

VCC_TEST(every_unreadable_note_field_refuses_its_note_and_names_itself)
{
    // Each of these is part of what the note *is* - its pitch, its length.
    // A note with any of them unreadable is not the composer's note, and a
    // default in its place would be this build inventing music.
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"\"octave\":70000", "octave"},
        {"\"alteration\":70000", "alteration"},
        {"\"voice\":70000", "voice"},
        {"\"dots\":70000", "dot"},
        {"\"tuplet\":{\"actual\":70000,\"normal\":1}", "tuplet"},
    };

    for (const auto& entry : cases) {
        Score score;
        const ScoreLoadResult result = load_text(one_note_document(entry.first), score);

        VCC_CHECK_EQ(result.notes_read, std::size_t{0});
        if (!result.warnings.empty()) {
            VCC_CHECK_MSG(result.warnings.front().find(entry.second) != std::string::npos,
                          "a note refused for " + entry.second + " should say so, not: "
                              + result.warnings.front());
        } else {
            VCC_CHECK_MSG(false, "no warning for " + entry.first);
        }
    }
}

VCC_TEST(a_staff_with_an_unreadable_transposition_keeps_its_notes_and_is_named)
{
    // A staff is not refused the way a note is: losing it would take every
    // note on it too. But a part that quietly loses its transposition plays
    // at the wrong pitch from end to end, so it is named.
    const std::string text =
        "{\"staves\":[{\"clef\":\"treble\",\"transposition_semitones\":70000,"
        "\"notes\":[{\"id\":1,\"step\":\"G\",\"tick\":0,\"value\":\"quarter\"}]}]}";

    Score score;
    const ScoreLoadResult result = load_text(text, score);

    VCC_CHECK(result.ok);
    VCC_CHECK_EQ(result.notes_read, std::size_t{1});
    VCC_CHECK(!result.warnings.empty());
    VCC_CHECK(result.warnings.front().find("transposition") != std::string::npos);
    VCC_CHECK_EQ(score.staff(0).transposition_semitones, std::int8_t{0});
}

VCC_TEST(a_marking_with_an_unreadable_voice_is_named)
{
    const std::string text =
        "{\"staves\":[{\"clef\":\"treble\",\"notes\":[]}],"
        "\"dynamics\":[{\"tick\":0,\"level\":\"mf\",\"voice\":70000}]}";

    Score score;
    const ScoreLoadResult result = load_text(text, score);

    VCC_CHECK(result.ok);
    VCC_CHECK(!result.warnings.empty());
    VCC_CHECK(result.warnings.front().find("voice") != std::string::npos);
}

VCC_TEST(no_out_of_range_value_in_any_numeric_field_loads_in_silence)
{
    // The property behind all of the above, checked across every numeric
    // field rather than the handful somebody thought to list. A value the
    // field cannot hold must never produce a clean load: either the note is
    // gone, or something was said about it.
    const std::vector<std::string> numeric_fields = {
        "octave", "alteration", "voice", "dots",
    };
    const std::vector<std::string> impossible = {
        "70000", "-70000", "9223372036854775807", "-9223372036854775807",
    };

    for (const std::string& field : numeric_fields) {
        for (const std::string& value : impossible) {
            Score score;
            const ScoreLoadResult result =
                load_text(one_note_document("\"" + field + "\":" + value), score);

            const bool refused = result.notes_read == 0;
            const bool named = !result.warnings.empty();
            VCC_CHECK_MSG(refused || named,
                          field + " = " + value + " loaded in silence");

            // And never both accepted and silent, which is the exact shape
            // the original bug had.
            VCC_CHECK_MSG(!(result.notes_read > 0 && result.warnings.empty()),
                          field + " = " + value + " was accepted with nothing said");
        }
    }
}

VCC_TEST(a_nested_tuplet_keeps_its_outer_ratio_through_the_file)
{
    // A tuplet inside a tuplet writes the ratio it sits inside; a plain
    // tuplet writes none, so a file with no nested tuplet reads exactly
    // as a file written before there were any.
    Score score;
    ScoreNote nested;
    nested.start_tick = kTicksPerQuarter;
    nested.duration = Duration{NoteValue::Sixteenth, 0, Tuplet{3, 2}};
    nested.duration.outer = Tuplet{3, 2};
    nested.pitch = Pitch{Step::G, 4, 0};
    score.insert_note(0, nested);
    ScoreNote plain;
    plain.start_tick = 0;
    plain.duration = Duration{NoteValue::Eighth, 0, Tuplet{3, 2}};
    plain.pitch = Pitch{Step::E, 4, 0};
    score.insert_note(0, plain);

    const std::string text = score_to_json(score).to_text(2);
    std::size_t outers = 0;
    for (std::size_t at = text.find("\"outer\""); at != std::string::npos;
         at = text.find("\"outer\"", at + 1)) {
        ++outers;
    }
    VCC_CHECK_EQ(outers, std::size_t{1});

    Score loaded;
    const ScoreLoadResult result = load_text(text, loaded);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(loaded.notes(0).size(), std::size_t{2});
    for (const ScoreNote& note : loaded.notes(0)) {
        if (note.start_tick == kTicksPerQuarter) {
            VCC_CHECK(note.duration.is_nested());
            VCC_CHECK_EQ(note.duration.ticks(), kTicksPerQuarter / 9);
        } else {
            VCC_CHECK(!note.duration.outer.is_tuplet());
        }
    }

    // An outer ratio this build cannot hold, or one with no tuplet inside
    // it, refuses the note and names the field, as every unreadable note
    // field does.
    const char* const bad[] = {
        "\"tuplet\":{\"actual\":3,\"normal\":2},\"outer\":{\"actual\":70000,\"normal\":1}",
        "\"outer\":{\"actual\":3,\"normal\":2}",
    };
    for (const char* field : bad) {
        Score refused;
        const ScoreLoadResult read = load_text(one_note_document(field), refused);
        VCC_CHECK_EQ(read.notes_read, std::size_t{0});
        VCC_CHECK(!read.warnings.empty());
        if (!read.warnings.empty())
            VCC_CHECK_MSG(read.warnings.front().find("outer") != std::string::npos, read.warnings.front());
    }
}

VCC_TEST_MAIN("score file")
