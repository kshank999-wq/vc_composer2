// Reading MIDI back into notation (spec 02 sections 5.2, 16).
//
// Going out is a translation; coming in is an interpretation, because MIDI
// has thrown away everything notation needs. So most of these cases are
// about what is inferred and whether the composer is told: how far the
// rhythm was moved, which notes became a chord, which became a tie, how a
// pitch was spelled, and what was dropped.
//
// The case that ties it together is the round trip through the exporter: a
// score written out and read back must play the same notes at the same
// times, even though it will not be the same *notation*.
#include "vcc/notation/MidiImport.h"

#include <algorithm>
#include <string>
#include <vector>

#include "vcc/notation/MidiRenderer.h"
#include "vcc/notation/ScoreMidiFile.h"
#include "vcc_test.h"

using namespace vcc::notation;
using vcc::music::MidiEventType;
using vcc::music::SmfEvent;
using vcc::music::SmfFile;
using vcc::music::SmfTrack;

/// A quarter note in ticks, so the arithmetic reads as rhythm.
constexpr Ticks kQuarter = kTicksPerQuarter;

namespace {

SmfTrack a_track(const std::string& name)
{
    SmfTrack track;
    track.name = name;
    return track;
}

void add_note(SmfTrack& track, std::int64_t start, std::int64_t length, int note,
              std::uint8_t velocity = 90, std::uint8_t channel = 0)
{
    track.events.push_back(
        SmfEvent{start, vcc::music::make_note_on(channel, static_cast<std::uint8_t>(note),
                                                 velocity)});
    track.events.push_back(
        SmfEvent{start + length,
                 vcc::music::make_note_off(channel, static_cast<std::uint8_t>(note))});
}

SmfFile a_file(std::uint16_t ppq = kQuarter)
{
    SmfFile file;
    file.format = vcc::music::SmfFormat::MultiTrack;
    file.ticks_per_quarter = ppq;
    return file;
}

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
        const bool is_off = event.event.type == MidiEventType::NoteOff
                            || (event.event.type == MidiEventType::NoteOn
                                && event.event.data2 == 0);
        if (is_off)
            out.push_back(event);
    }
    return out;
}

/// A score with something of everything the exporter has to carry: a scale,
/// a chord, and a length that only a dotted value expresses.
Score a_short_cue()
{
    Score score;
    score.staff(0).name = "Violin I";
    score.set_time_signature(0, vcc::music::TimeSignature{4, 4});
    score.add_tempo(TempoMarking{0, 96.0, NoteValue::Quarter, 0, ""});

    const Step steps[] = {Step::C, Step::D, Step::E, Step::F, Step::G, Step::A, Step::B};
    for (int i = 0; i < 8; ++i) {
        ScoreNote note;
        note.start_tick = i * (kTicksPerQuarter / 2);
        note.duration = eighth();
        note.pitch = Pitch{steps[i % 7], static_cast<std::int8_t>(4 + i / 7), 0};
        score.insert_note(0, note);
    }
    score.insert_chord(0, 4 * kTicksPerQuarter, quarter(),
                       {Pitch{Step::C, 5, 0}, Pitch{Step::E, 5, 0}});
    ScoreNote long_note;
    long_note.start_tick = 5 * kTicksPerQuarter;
    long_note.duration = half(1);
    long_note.pitch = Pitch{Step::G, 4, 0};
    score.insert_note(0, long_note);
    return score;
}

/// Writes a score out and reads it straight back in.
Score through_a_midi_file(const Score& original, const MidiRenderer& renderer,
                          MidiImportResult& result)
{
    const ScoreMidiResult exported =
        build_score_midi(original, renderer, ScoreMidiOptions{});
    VCC_CHECK(exported.carried_everything());

    std::vector<std::uint8_t> bytes;
    VCC_CHECK(vcc::music::write_smf(exported.file, bytes).ok);

    vcc::music::SmfFile read_back;
    const vcc::music::SmfReadResult read = vcc::music::read_smf(bytes, read_back);
    VCC_CHECK_MSG(read.ok, read.error);

    Score imported;
    result = import_midi(read_back, imported);
    return imported;
}

}  // namespace

// ---------------------------------------------------------------------------
// Quantization
// ---------------------------------------------------------------------------

VCC_TEST(a_tick_snaps_to_the_nearer_grid_point)
{
    const Ticks grid = grid_ticks(NoteValue::Sixteenth);
    VCC_CHECK_EQ(grid, static_cast<Ticks>((kQuarter / 4)));

    VCC_CHECK_EQ(quantize_tick(0, grid, false), static_cast<Ticks>(0));
    VCC_CHECK_EQ(quantize_tick(10, grid, false), static_cast<Ticks>(0));
    VCC_CHECK_EQ(quantize_tick(grid / 2 - 1, grid, false), static_cast<Ticks>(0));
    VCC_CHECK_EQ(quantize_tick(grid / 2 + 1, grid, false), static_cast<Ticks>(grid));
    VCC_CHECK_EQ(quantize_tick(grid - 1, grid, false), static_cast<Ticks>(grid));
    VCC_CHECK_EQ(quantize_tick(kQuarter + 10, grid, false), static_cast<Ticks>(kQuarter));

    // A grid of nothing leaves the tick alone rather than dividing by zero.
    VCC_CHECK_EQ(quantize_tick(123, 0, true), static_cast<Ticks>(123));

    // Every written value has a grid...
    for (int index = 0; index <= static_cast<int>(NoteValue::SixtyFourth); ++index)
        VCC_CHECK(grid_ticks(static_cast<NoteValue>(index)) > 0);

    // ...and nothing else does. base_ticks answers for its last enumerator
    // by falling through its switch, so a number cast in from outside the
    // enum would otherwise come back as a 64th note grid instead of as a
    // refusal.
    VCC_CHECK_EQ(grid_ticks(static_cast<NoteValue>(99)), static_cast<Ticks>(0));
    VCC_CHECK_EQ(grid_ticks(static_cast<NoteValue>(200)), static_cast<Ticks>(0));
}

VCC_TEST(a_triplet_lands_on_the_triplet_grid_and_a_straight_note_does_not)
{
    // Without this a triplet passage reads as a stumbling run of straight
    // notes, which is the single most obvious way an import goes wrong.
    const Ticks grid = grid_ticks(NoteValue::Eighth);  // (kQuarter / 2) ticks.
    const Ticks triplet = (grid * 2) / 3;              // (kQuarter / 3) ticks.
    VCC_CHECK_EQ(triplet, static_cast<Ticks>((kQuarter / 3)));

    // Eighth-note triplets in a quarter: at 0, a third and two thirds.
    VCC_CHECK_EQ(quantize_tick(triplet - 2, grid, true), triplet);
    VCC_CHECK_EQ(quantize_tick(2 * triplet + 5, grid, true), static_cast<Ticks>((2 * kQuarter / 3)));

    // Straight eighths stay straight, because their own grid is nearer.
    VCC_CHECK_EQ(quantize_tick(grid - 2, grid, true), static_cast<Ticks>((kQuarter / 2)));
    VCC_CHECK_EQ(quantize_tick(kQuarter + 2, grid, true), static_cast<Ticks>(kQuarter));

    // With triplets off, a triplet is dragged onto the straight grid - which
    // is exactly why the option exists.
    VCC_CHECK_EQ(quantize_tick((kQuarter / 3), grid, false), static_cast<Ticks>((kQuarter / 2)));

    // A tie goes to the straight grid: it is the simpler notation, and the
    // one a composer expects when nothing favours the other. A sixth of a
    // quarter is as far from the beat as from the first triplet eighth, so
    // it is a real tie - and it has to come back as the beat.
    VCC_CHECK_EQ(quantize_tick((kQuarter / 6), grid, true), static_cast<Ticks>(0));

    // The same tie a level down the grid, so the rule is not an accident of
    // one number.
    const Ticks sixteenths = grid_ticks(NoteValue::Sixteenth);  // Its triplet is a sixth.
    VCC_CHECK_EQ(quantize_tick((kQuarter / 12), sixteenths, true), static_cast<Ticks>(0));
    VCC_CHECK_EQ(quantize_tick((kQuarter + kQuarter / 12), sixteenths, true), static_cast<Ticks>(kQuarter));
}

VCC_TEST(a_negative_tick_snaps_the_same_way)
{
    // Nothing should produce one, and code that divides by a grid gets
    // negative rounding wrong by default.
    const Ticks grid = (kQuarter / 4);
    VCC_CHECK_EQ(quantize_tick(-10, grid, false), static_cast<Ticks>(0));
    VCC_CHECK_EQ(quantize_tick(-(grid / 2 + 1), grid, false), static_cast<Ticks>(-(kQuarter / 4)));
}

// ---------------------------------------------------------------------------
// The file's own resolution
// ---------------------------------------------------------------------------

VCC_TEST(a_file_at_another_resolution_is_rescaled_to_the_engines)
{
    // A file at 480 ticks per quarter has every position at half this
    // engine's scale. Reading it without rescaling would halve the piece.
    SmfFile file = a_file((kQuarter / 2));
    SmfTrack track = a_track("Violin I");
    add_note(track, 0, (kQuarter / 2), 60);       // A quarter note, in the file's ticks.
    add_note(track, (kQuarter / 2), (kQuarter / 4), 62);     // An eighth.
    file.tracks.push_back(std::move(track));

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.notes_read, static_cast<std::size_t>(2));

    const std::vector<ScoreNote>& notes = score.notes(0);
    VCC_CHECK_EQ(notes.size(), static_cast<std::size_t>(2));
    if (notes.size() == 2) {
        VCC_CHECK_EQ(notes[0].start_tick, static_cast<Ticks>(0));
        VCC_CHECK_EQ(notes[0].duration.ticks(), static_cast<Ticks>(kTicksPerQuarter));
        VCC_CHECK_EQ(notes[1].start_tick, static_cast<Ticks>(kTicksPerQuarter));
        VCC_CHECK_EQ(notes[1].duration.ticks(), static_cast<Ticks>(kTicksPerQuarter / 2));
    }
    VCC_CHECK_EQ(result.largest_move, static_cast<Ticks>(0));

    // A resolution this engine's does not divide by, where rescaling has to
    // round. 384 ticks per quarter is two and a half of this engine's ticks
    // per file tick, so a note on file tick 3 sounds at 7.5 and is read at
    // 8. Rounding it down instead would put every note in the file early,
    // which is a systematic error where rounding is a random one - and the
    // only place it shows is in how far the quantizer then had to move it.
    {
        SmfFile odd = a_file(384);
        SmfTrack part = a_track("Violin I");
        add_note(part, 3, 384, 60);  // Three file ticks late, a quarter long.
        odd.tracks.push_back(std::move(part));

        Score other;
        const MidiImportResult rescaled = import_midi(odd, other);
        VCC_CHECK_MSG(rescaled.ok, rescaled.error);
        VCC_CHECK_EQ(rescaled.notes_read, static_cast<std::size_t>(1));
        VCC_CHECK_EQ(other.notes(0).front().start_tick, static_cast<Ticks>(0));
        VCC_CHECK_EQ(other.notes(0).front().duration.ticks(),
                     static_cast<Ticks>(kTicksPerQuarter));
        // Three ticks of 384 are 22.5 of the engine's, rounded up to 23.
        VCC_CHECK_EQ(rescaled.largest_move, static_cast<Ticks>(23));
        VCC_CHECK_EQ(rescaled.largest_length_move, static_cast<Ticks>(23));
    }
}

VCC_TEST(a_tempo_on_a_track_that_is_not_the_first_still_reaches_the_score)
{
    // Convention puts tempo, meter and key on the first track. A file is
    // not obliged to follow it - some exporters write the tempo onto the
    // part instead - and one that does not still has a tempo.
    SmfFile file = a_file();

    SmfTrack first = a_track("Violin I");
    add_note(first, 0, kQuarter, 72);
    file.tracks.push_back(std::move(first));

    SmfTrack second = a_track("Celli");
    add_note(second, 0, kQuarter, 48);
    second.meta.push_back(vcc::music::smf_tempo(0, 72.0));
    second.meta.push_back(
        vcc::music::smf_time_signature(0, vcc::music::TimeSignature{5, 8}));
    second.meta.push_back(vcc::music::smf_key_signature(0, -3, false));
    file.tracks.push_back(std::move(second));

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.staves_read, static_cast<std::size_t>(2));
    VCC_CHECK_EQ(result.tracks_without_notes, static_cast<std::size_t>(0));

    VCC_CHECK_NEAR(score.tempo_at(0), 72.0, 0.01);
    VCC_CHECK(score.time_signature_at(0) == vcc::music::TimeSignature{5, 8});
    VCC_CHECK_EQ(score.key_signature_at(0).fifths, static_cast<std::int8_t>(-3));
}

// ---------------------------------------------------------------------------
// Tracks and staves
// ---------------------------------------------------------------------------

VCC_TEST(a_keyboard_track_is_split_at_the_point_asked_for_onto_two_staves)
{
    // A piano sketch is one track. Split at C4, the notes from C4 up go on
    // one staff and the rest on the staff below it, both named for the
    // track, the lower on a bass clef; a track wholly on one side stays
    // one staff; and with the split off nothing changes.
    SmfFile file = a_file();
    SmfTrack piano = a_track("Piano");
    add_note(piano, 0, kQuarter, 72);     // C5: upper.
    add_note(piano, 0, kQuarter, 48);     // C3: lower.
    add_note(piano, kQuarter, kQuarter, 60);   // C4 itself: upper.
    add_note(piano, kQuarter, kQuarter, 43);   // G2: lower.
    file.tracks.push_back(std::move(piano));
    SmfTrack bass = a_track("Bass");
    add_note(bass, 0, (2 * kQuarter), 40);
    file.tracks.push_back(std::move(bass));

    MidiImportOptions options;
    options.split_point = 60;
    Score score;
    const MidiImportResult result = import_midi(file, score, options);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.staves_read, std::size_t{3});
    VCC_CHECK_EQ(result.tracks_split, std::size_t{1});
    VCC_CHECK_EQ(result.notes_read, std::size_t{5});

    VCC_CHECK_EQ(score.staff(0).name, std::string("Piano"));
    VCC_CHECK_EQ(score.staff(1).name, std::string("Piano"));
    VCC_CHECK_EQ(score.staff(2).name, std::string("Bass"));
    VCC_CHECK(score.staff(0).clef == Clef::Treble);
    VCC_CHECK(score.staff(1).clef == Clef::Bass);
    for (const ScoreNote& note : score.notes(0))
        VCC_CHECK(midi_note_number(note.pitch) >= 60);
    for (const ScoreNote& note : score.notes(1))
        VCC_CHECK(midi_note_number(note.pitch) < 60);
    VCC_CHECK_EQ(score.notes(0).size(), std::size_t{2});
    VCC_CHECK_EQ(score.notes(1).size(), std::size_t{2});
    VCC_CHECK_EQ(score.notes(2).size(), std::size_t{1});

    // The dynamics warning is about several parts, and a split track is
    // still one part: two parts here, not three.
    bool warned = false;
    for (const std::string& warning : result.warnings)
        warned = warned || warning.find("all 2 parts") != std::string::npos;
    VCC_CHECK(warned);

    // A file of one split keyboard track is one part, so the warning does
    // not come at all.
    SmfFile solo = a_file();
    SmfTrack keys = a_track("Piano");
    add_note(keys, 0, kQuarter, 72);
    add_note(keys, 0, kQuarter, 48);
    solo.tracks.push_back(std::move(keys));
    Score alone;
    const MidiImportResult one_part = import_midi(solo, alone, options);
    VCC_CHECK_EQ(one_part.staves_read, std::size_t{2});
    VCC_CHECK(one_part.dynamics_read > 0);
    for (const std::string& warning : one_part.warnings)
        VCC_CHECK_MSG(warning.find("parts together") == std::string::npos, warning);

    // Off: one staff per track.
    Score plain;
    const MidiImportResult whole = import_midi(file, plain);
    VCC_CHECK_EQ(whole.staves_read, std::size_t{2});
    VCC_CHECK_EQ(whole.tracks_split, std::size_t{0});

    // Nothing below the point: one staff, whatever the setting says.
    SmfFile high = a_file();
    SmfTrack flute = a_track("Flute");
    add_note(flute, 0, kQuarter, 72);
    add_note(flute, kQuarter, kQuarter, 76);
    high.tracks.push_back(std::move(flute));
    Score one;
    const MidiImportResult unsplit = import_midi(high, one, options);
    VCC_CHECK_EQ(unsplit.staves_read, std::size_t{1});
    VCC_CHECK_EQ(unsplit.tracks_split, std::size_t{0});
}

VCC_TEST(one_staff_per_track_that_has_notes_and_the_conductor_is_not_one)
{
    SmfFile file = a_file();

    SmfTrack conductor = a_track("Conductor");
    conductor.meta.push_back(vcc::music::smf_tempo(0, 96.0));
    conductor.meta.push_back(
        vcc::music::smf_time_signature(0, vcc::music::TimeSignature{3, 4}));
    file.tracks.push_back(std::move(conductor));

    SmfTrack first = a_track("Violin I");
    add_note(first, 0, kQuarter, 72);
    file.tracks.push_back(std::move(first));

    SmfTrack second = a_track("Celli");
    add_note(second, 0, kQuarter, 48);
    file.tracks.push_back(std::move(second));

    file.tracks.push_back(a_track("Empty"));

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);

    VCC_CHECK_EQ(result.staves_read, static_cast<std::size_t>(2));
    VCC_CHECK_EQ(score.staff_count(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(score.staff(0).name, std::string("Violin I"));
    VCC_CHECK_EQ(score.staff(1).name, std::string("Celli"));

    // The conductor track and the empty one: counted, and their meta events
    // still reached the score.
    VCC_CHECK_EQ(result.tracks_without_notes, static_cast<std::size_t>(2));
    VCC_CHECK(score.time_signature_at(0) == vcc::music::TimeSignature{3, 4});
    VCC_CHECK_EQ(score.tempos().size(), static_cast<std::size_t>(1));
    VCC_CHECK_NEAR(score.tempo_at(0), 96.0, 0.01);
}

VCC_TEST(an_unnamed_track_still_gets_a_name)
{
    // An unnamed part is one the composer cannot pick out of a list.
    SmfFile file = a_file();
    SmfTrack track;
    add_note(track, 0, kQuarter, 60);
    file.tracks.push_back(std::move(track));

    Score score;
    VCC_CHECK(import_midi(file, score).ok);
    VCC_CHECK(!score.staff(0).name.empty());
}

VCC_TEST(a_clef_is_guessed_from_where_the_part_sits)
{
    // A bass line on a treble staff is unreadable, and the file says nothing
    // about clefs.
    SmfFile file = a_file();

    SmfTrack high = a_track("Flute");
    for (int i = 0; i < 4; ++i)
        add_note(high, i * kQuarter, kQuarter, 76 + i);
    file.tracks.push_back(std::move(high));

    SmfTrack low = a_track("Bassoon");
    for (int i = 0; i < 4; ++i)
        add_note(low, i * kQuarter, kQuarter, 40 + i);
    file.tracks.push_back(std::move(low));

    Score score;
    VCC_CHECK(import_midi(file, score).ok);
    VCC_CHECK_EQ(static_cast<int>(score.staff(0).clef), static_cast<int>(Clef::Treble));
    VCC_CHECK_EQ(static_cast<int>(score.staff(1).clef), static_cast<int>(Clef::Bass));

    // A single very low pedal note does not drag a treble part downstairs:
    // the median decides, not the mean.
    SmfFile mostly_high = a_file();
    SmfTrack track = a_track("Organ");
    for (int i = 0; i < 5; ++i)
        add_note(track, i * kQuarter, kQuarter, 74);
    add_note(track, 5 * kQuarter, kQuarter, 24);
    mostly_high.tracks.push_back(std::move(track));

    Score other;
    VCC_CHECK(import_midi(mostly_high, other).ok);
    VCC_CHECK_EQ(static_cast<int>(other.staff(0).clef), static_cast<int>(Clef::Treble));

    // And the guess can be turned off.
    MidiImportOptions plain;
    plain.guess_clefs = false;
    Score forced;
    VCC_CHECK(import_midi(file, forced, plain).ok);
    VCC_CHECK_EQ(static_cast<int>(forced.staff(1).clef), static_cast<int>(Clef::Treble));
}

// ---------------------------------------------------------------------------
// Pairing note-ons with releases
// ---------------------------------------------------------------------------

VCC_TEST(a_zero_velocity_note_on_releases_a_note)
{
    // Every reader treats it as a release, so this one has to.
    SmfFile file = a_file();
    SmfTrack track = a_track("Violin I");
    track.events.push_back(SmfEvent{0, vcc::music::make_note_on(0, 60, 90)});
    track.events.push_back(SmfEvent{kQuarter, vcc::music::make_note_on(0, 60, 0)});
    file.tracks.push_back(std::move(track));

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.notes_read, static_cast<std::size_t>(1));
    VCC_CHECK_EQ(result.unreleased_notes, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(score.notes(0).front().duration.ticks(),
                 static_cast<Ticks>(kTicksPerQuarter));
}

VCC_TEST(two_instances_of_one_pitch_are_released_oldest_first)
{
    // What every sequencer does. Releasing them any other way would swap
    // their lengths.
    SmfFile file = a_file();
    SmfTrack track = a_track("Violin I");
    track.events.push_back(SmfEvent{0, vcc::music::make_note_on(0, 60, 90)});
    track.events.push_back(SmfEvent{(kQuarter / 2), vcc::music::make_note_on(0, 60, 90)});
    track.events.push_back(SmfEvent{kQuarter, vcc::music::make_note_off(0, 60)});
    track.events.push_back(SmfEvent{(2 * kQuarter), vcc::music::make_note_off(0, 60)});
    file.tracks.push_back(std::move(track));

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.notes_read, static_cast<std::size_t>(2));

    const std::vector<ScoreNote>& notes = score.notes(0);
    VCC_CHECK_EQ(notes.size(), static_cast<std::size_t>(2));
    if (notes.size() == 2) {
        // The first attack lasted 960 ticks, the second 1440.
        VCC_CHECK_EQ(notes[0].start_tick, static_cast<Ticks>(0));
        VCC_CHECK_EQ(notes[0].duration.ticks(), static_cast<Ticks>(kQuarter));
        VCC_CHECK_EQ(notes[1].start_tick, static_cast<Ticks>((kQuarter / 2)));
        VCC_CHECK_EQ(notes[1].duration.ticks(), static_cast<Ticks>((3 * kQuarter / 2)));
    }
}

VCC_TEST(a_note_the_file_never_released_is_closed_and_the_guess_is_named)
{
    SmfFile file = a_file();
    SmfTrack track = a_track("Violin I");
    track.events.push_back(SmfEvent{0, vcc::music::make_note_on(0, 60, 90)});
    add_note(track, 0, (2 * kQuarter), 64);
    file.tracks.push_back(std::move(track));

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.unreleased_notes, static_cast<std::size_t>(1));
    VCC_CHECK(!result.carried_everything());

    bool said_so = false;
    for (const std::string& warning : result.warnings) {
        if (warning.find("never released") != std::string::npos)
            said_so = true;
    }
    VCC_CHECK_MSG(said_so, "the guess was made silently");

    // It is still music: the note is there, ending where its track does.
    VCC_CHECK(result.notes_read >= 2);
}

VCC_TEST(a_release_for_something_that_never_sounded_is_ignored)
{
    SmfFile file = a_file();
    SmfTrack track = a_track("Violin I");
    track.events.push_back(SmfEvent{0, vcc::music::make_note_off(0, 60)});
    add_note(track, kQuarter, kQuarter, 62);
    file.tracks.push_back(std::move(track));

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.notes_read, static_cast<std::size_t>(1));
    VCC_CHECK_EQ(result.unreleased_notes, static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// What notation needs and MIDI does not say
// ---------------------------------------------------------------------------

VCC_TEST(notes_that_start_together_become_a_chord)
{
    // Nothing in the file says so; notation has to.
    SmfFile file = a_file();
    SmfTrack track = a_track("Piano");
    add_note(track, 0, kQuarter, 60);
    add_note(track, 0, kQuarter, 64);
    add_note(track, 0, kQuarter, 67);
    add_note(track, kQuarter, kQuarter, 72);  // On its own.
    file.tracks.push_back(std::move(track));

    Score score;
    VCC_CHECK(import_midi(file, score).ok);

    const std::vector<ScoreNote>& notes = score.notes(0);
    VCC_CHECK_EQ(notes.size(), static_cast<std::size_t>(4));

    std::size_t in_chord = 0;
    for (const ScoreNote& note : notes) {
        if (note.in_chord)
            ++in_chord;
    }
    VCC_CHECK_EQ(in_chord, static_cast<std::size_t>(3));

    for (const ScoreNote& note : notes) {
        if (note.start_tick == kQuarter)
            VCC_CHECK_MSG(!note.in_chord, "a note on its own was called a chord");
    }
}

VCC_TEST(a_length_no_single_value_can_write_becomes_a_tie_chain)
{
    // Five sixteenths is not a note value. It is a quarter tied to a
    // sixteenth, which is how a composer would write it.
    SmfFile file = a_file();
    SmfTrack track = a_track("Violin I");
    add_note(track, 0, 5 * (kQuarter / 4), 60);
    file.tracks.push_back(std::move(track));

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.notes_read, static_cast<std::size_t>(2));

    const std::vector<ScoreNote>& notes = score.notes(0);
    VCC_CHECK_EQ(notes.size(), static_cast<std::size_t>(2));
    if (notes.size() == 2) {
        VCC_CHECK(notes[0].tied_to_next);
        VCC_CHECK(!notes[1].tied_to_next);
        VCC_CHECK_EQ(notes[0].start_tick + notes[0].duration.ticks(), notes[1].start_tick);
        VCC_CHECK_EQ(notes[0].duration.ticks() + notes[1].duration.ticks(),
                     static_cast<Ticks>(5 * (kQuarter / 4)));

        // The same pitch throughout, or it is not a tie.
        VCC_CHECK(notes[0].pitch == notes[1].pitch);
    }
}

VCC_TEST(a_triplet_passage_comes_back_as_triplets)
{
    // The case allow_triplets exists for, end to end rather than through
    // quantize_tick alone. The onsets were the easy half: no sum of plain
    // and dotted values is 320 ticks, so until the length side could write
    // a triplet, every note of a triplet passage was snapped onto the
    // triplet grid and then dropped for having a length notation could not
    // express. A quantizer that moves a note to a length it cannot write is
    // worse than one that does not move it at all.
    SmfFile file = a_file();
    SmfTrack track = a_track("Violin I");
    for (int i = 0; i < 3; ++i)
        add_note(track, i * (kQuarter / 3), (kQuarter / 3), 60 + i);  // Eighth triplets in a quarter.
    file.tracks.push_back(std::move(track));

    MidiImportOptions options;
    options.grid = NoteValue::Eighth;  // Whose triplet subdivision is 320.

    Score score;
    const MidiImportResult result = import_midi(file, score, options);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.notes_read, static_cast<std::size_t>(3));
    VCC_CHECK_EQ(result.notes_dropped, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(result.largest_move, static_cast<Ticks>(0));
    VCC_CHECK_EQ(result.largest_length_move, static_cast<Ticks>(0));
    VCC_CHECK_MSG(result.carried_everything(),
                  result.warnings.empty() ? "" : result.warnings.front());

    const std::vector<ScoreNote>& notes = score.notes(0);
    VCC_CHECK_EQ(notes.size(), static_cast<std::size_t>(3));
    for (std::size_t i = 0; i < notes.size(); ++i) {
        VCC_CHECK_MSG(notes[i].duration.tuplet.is_tuplet(),
                      "a triplet came back as something else");
        VCC_CHECK_EQ(notes[i].duration.ticks(), static_cast<Ticks>((kQuarter / 3)));
        VCC_CHECK(!notes[i].tied_to_next);
        VCC_CHECK_EQ(notes[i].start_tick, static_cast<Ticks>(i) * (kQuarter / 3));
    }

    // With triplets refused it is the stumbling run of straight notes the
    // option exists to avoid - which is a worse score, but still a score.
    {
        MidiImportOptions straight = options;
        straight.allow_triplets = false;
        Score other;
        const MidiImportResult dragged = import_midi(file, other, straight);
        VCC_CHECK_MSG(dragged.ok, dragged.error);
        VCC_CHECK(dragged.largest_move > 0);
        VCC_CHECK(!dragged.carried_everything());
    }
}

VCC_TEST(a_length_no_notation_can_write_is_written_short_and_said_so)
{
    // Lengths left as played can be any number of ticks, and a tie chain
    // reaches only some of them: 1021 ticks is a quarter, a 64th, and one
    // tick that nothing writes.
    //
    // Written short rather than long - a note that outlasted what was
    // played would overlap what comes after it - and counted, because a
    // note that came back at a different length is exactly the quiet kind
    // of change this import exists to report.
    SmfFile file = a_file();
    SmfTrack track = a_track("Violin I");
    add_note(track, 0, kQuarter + kQuarter / 16 + 1, 60);  // A quarter, a 64th, and a tick.
    file.tracks.push_back(std::move(track));

    MidiImportOptions options;
    options.quantize_lengths = false;

    Score score;
    const MidiImportResult result = import_midi(file, score, options);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.notes_read, static_cast<std::size_t>(2));
    VCC_CHECK_EQ(result.notes_dropped, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(result.largest_length_move, static_cast<Ticks>(1));

    Ticks total = 0;
    for (const ScoreNote& note : score.notes(0))
        total += note.duration.ticks();
    VCC_CHECK_EQ(total, static_cast<Ticks>(kQuarter + kQuarter / 16));

    bool said_so = false;
    for (const std::string& warning : result.warnings) {
        if (warning.find("shorter than played") != std::string::npos)
            said_so = true;
    }
    VCC_CHECK_MSG(said_so, "a note was shortened and nothing said so");
}

VCC_TEST(a_pitch_is_spelled_as_the_key_signature_implies)
{
    // A sounding pitch does not say how it was written. Middle C sharp and
    // D flat are the same key; the key signature is the only evidence.
    SmfFile file = a_file();
    SmfTrack conductor = a_track("Conductor");
    conductor.meta.push_back(vcc::music::smf_key_signature(0, -4, false));  // A flat major.
    file.tracks.push_back(std::move(conductor));

    SmfTrack track = a_track("Violin I");
    add_note(track, 0, kQuarter, 61);  // C sharp, or D flat.
    file.tracks.push_back(std::move(track));

    Score score;
    VCC_CHECK(import_midi(file, score).ok);
    VCC_CHECK_EQ(score.key_signature_at(0).fifths, static_cast<std::int8_t>(-4));

    const ScoreNote& note = score.notes(0).front();
    VCC_CHECK_MSG(note.pitch.alteration == -1,
                  "a flat key spelled a black key as a sharp");
    VCC_CHECK_EQ(static_cast<int>(note.pitch.step), static_cast<int>(Step::D));

    // The same note in a sharp key is spelled the other way.
    SmfFile sharp = a_file();
    SmfTrack sharp_conductor = a_track("Conductor");
    sharp_conductor.meta.push_back(vcc::music::smf_key_signature(0, 5, false));
    sharp.tracks.push_back(std::move(sharp_conductor));
    SmfTrack sharp_track = a_track("Violin I");
    add_note(sharp_track, 0, kQuarter, 61);
    sharp.tracks.push_back(std::move(sharp_track));

    Score other;
    VCC_CHECK(import_midi(sharp, other).ok);
    VCC_CHECK_EQ(other.notes(0).front().pitch.alteration, static_cast<std::int8_t>(1));
    VCC_CHECK_EQ(static_cast<int>(other.notes(0).front().pitch.step),
                 static_cast<int>(Step::C));
}

VCC_TEST(a_default_key_is_used_only_when_the_file_names_none)
{
    SmfFile file = a_file();
    SmfTrack track = a_track("Violin I");
    add_note(track, 0, kQuarter, 61);
    file.tracks.push_back(std::move(track));

    MidiImportOptions options;
    options.default_key = KeySignature{-4, false};

    Score score;
    VCC_CHECK(import_midi(file, score, options).ok);
    VCC_CHECK_EQ(score.key_signature_at(0).fifths, static_cast<std::int8_t>(-4));
    VCC_CHECK_EQ(score.notes(0).front().pitch.alteration, static_cast<std::int8_t>(-1));
}

// ---------------------------------------------------------------------------
// Reporting what it did
// ---------------------------------------------------------------------------

VCC_TEST(a_rhythm_that_had_to_move_is_reported)
{
    // An import that silently tidied a phrase is an import nobody should
    // trust.
    //
    // 80 ticks past the beat is as far as a sixteenth grid with triplets
    // allowed can ever move a note: the multiples of 240 and of 160
    // interleave, and 1040 sits equally far from 960 and from 1120. So this
    // is the case the warning exists for, and if it does not fire here it
    // fires nowhere.
    SmfFile file = a_file();
    SmfTrack track = a_track("Violin I");
    add_note(track, 0, kQuarter, 60);
    add_note(track, (kQuarter + kQuarter / 12), kQuarter, 62);
    file.tracks.push_back(std::move(track));

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.largest_move, static_cast<Ticks>(kQuarter / 12));
    VCC_CHECK(!result.carried_everything());

    bool said_so = false;
    for (const std::string& warning : result.warnings) {
        if (warning.find("moved") != std::string::npos)
            said_so = true;
    }
    VCC_CHECK_MSG(said_so, "the rhythm moved and nothing said so");

    // The same, on the straight grid alone, where a note can be dragged
    // twice as far.
    {
        SmfFile straight = a_file();
        SmfTrack part = a_track("Violin I");
        add_note(part, 0, kQuarter, 60);
        add_note(part, kQuarter + kQuarter / 9, kQuarter, 62);  // A ninth of a quarter late.
        straight.tracks.push_back(std::move(part));

        MidiImportOptions options;
        options.allow_triplets = false;

        Score other;
        const MidiImportResult without_triplets = import_midi(straight, other, options);
        VCC_CHECK_MSG(without_triplets.ok, without_triplets.error);
        VCC_CHECK_EQ(without_triplets.largest_move, static_cast<Ticks>(kQuarter / 9));
        VCC_CHECK(!without_triplets.carried_everything());
    }
}

VCC_TEST(a_note_barely_off_the_grid_is_not_worth_a_warning)
{
    // The counterpart to the case above, and the reason the threshold is
    // not simply "anything moved": a player is never exactly on the grid,
    // and a warning that fires for every performance is one nobody reads.
    SmfFile file = a_file();
    SmfTrack track = a_track("Violin I");
    add_note(track, 0, kQuarter, 60);
    add_note(track, kQuarter + 3, kQuarter, 62);  // Three ticks late: under a millisecond.
    file.tracks.push_back(std::move(track));

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.largest_move, static_cast<Ticks>(3));
    VCC_CHECK_MSG(result.carried_everything(),
                  result.warnings.empty() ? "" : result.warnings.front());
}

VCC_TEST(a_file_already_on_the_grid_moves_nothing_and_reads_cleanly)
{
    // The counterpart: a warning that fires always is a warning nobody
    // reads.
    SmfFile file = a_file();
    SmfTrack track = a_track("Violin I");
    for (int i = 0; i < 8; ++i)
        add_note(track, i * (kQuarter / 4), (kQuarter / 4), 60 + i);
    file.tracks.push_back(std::move(track));

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.largest_move, static_cast<Ticks>(0));
    VCC_CHECK_EQ(result.notes_dropped, static_cast<std::size_t>(0));
    VCC_CHECK_MSG(result.carried_everything(),
                  result.warnings.empty() ? "" : result.warnings.front());
}

VCC_TEST(a_note_too_short_to_notate_is_dropped_and_counted)
{
    // A two-tick note is a stuck key or a graced release, not notation.
    SmfFile file = a_file();
    SmfTrack track = a_track("Violin I");
    add_note(track, 0, 2, 60);
    add_note(track, kQuarter, kQuarter, 62);
    file.tracks.push_back(std::move(track));

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.notes_read, static_cast<std::size_t>(1));
    VCC_CHECK_EQ(result.notes_dropped, static_cast<std::size_t>(1));

    bool said_so = false;
    for (const std::string& warning : result.warnings) {
        if (warning.find("too short") != std::string::npos)
            said_so = true;
    }
    VCC_CHECK(said_so);
}

VCC_TEST(a_minimum_length_can_be_asked_for)
{
    SmfFile file = a_file();
    SmfTrack track = a_track("Violin I");
    add_note(track, 0, (kQuarter / 4), 60);   // A sixteenth.
    add_note(track, kQuarter, kQuarter, 62);
    file.tracks.push_back(std::move(track));

    MidiImportOptions options;
    options.drop_shorter_than = (kQuarter / 2);  // Anything under an eighth.

    Score score;
    const MidiImportResult result = import_midi(file, score, options);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.notes_read, static_cast<std::size_t>(1));
    VCC_CHECK_EQ(result.notes_dropped, static_cast<std::size_t>(1));
}

VCC_TEST(lengths_can_be_left_as_they_were_played)
{
    SmfFile file = a_file();
    SmfTrack track = a_track("Violin I");
    // A quarter note held a 64th too long. Not a quarter played a 64th
    // short, which would *also* be exactly a triple-dotted eighth - a
    // single written value, which is not what this case is about.
    add_note(track, 0, kQuarter + kQuarter / 16, 60);
    file.tracks.push_back(std::move(track));

    {
        Score score;
        const MidiImportResult result = import_midi(file, score);
        VCC_CHECK_MSG(result.ok, result.error);
        // Quantized: the release snaps to the grid, so it notates as a
        // quarter.
        VCC_CHECK_EQ(score.notes(0).size(), static_cast<std::size_t>(1));
        VCC_CHECK_EQ(score.notes(0).front().duration.ticks(),
                     static_cast<Ticks>(kTicksPerQuarter));

        // The onset was already on the beat. Only the release moved, and
        // that is counted separately - see the header: a release that moved
        // is ordinary, an onset that moved is a rhythm reinterpreted.
        VCC_CHECK_EQ(result.largest_move, static_cast<Ticks>(0));
        VCC_CHECK_EQ(result.largest_length_move, static_cast<Ticks>(kQuarter / 16));
    }
    {
        MidiImportOptions options;
        options.quantize_lengths = false;
        Score score;
        const MidiImportResult result = import_midi(file, score, options);
        VCC_CHECK_MSG(result.ok, result.error);

        // Unquantized, a quarter and a 64th is a tie chain rather than one
        // note: a quarter tied to a 64th.
        Ticks total = 0;
        for (const ScoreNote& note : score.notes(0))
            total += note.duration.ticks();
        VCC_CHECK_EQ(total, static_cast<Ticks>(kQuarter + kQuarter / 16));
        VCC_CHECK(score.notes(0).size() > 1);
        VCC_CHECK(score.notes(0).front().tied_to_next);

        // Nothing touched a length, so nothing is reported as having.
        VCC_CHECK_EQ(result.largest_length_move, static_cast<Ticks>(0));
    }
}

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------

VCC_TEST(a_file_with_nothing_in_it_is_refused_and_leaves_nothing_behind)
{
    const std::string fresh = std::to_string(Score{}.total_note_count());

    // No tracks at all.
    {
        Score score;
        score.insert_note(0, ScoreNote{});
        const MidiImportResult result = import_midi(a_file(), score);
        VCC_CHECK(!result.ok);
        VCC_CHECK(!result.error.empty());
        VCC_CHECK_EQ(score.total_note_count(), static_cast<std::size_t>(0));
        VCC_CHECK_EQ(score.staff_count(), static_cast<std::size_t>(1));
    }

    // Tracks, but no notes on any of them: there is no part in it.
    {
        SmfFile file = a_file();
        SmfTrack conductor = a_track("Conductor");
        conductor.meta.push_back(vcc::music::smf_tempo(0, 100.0));
        file.tracks.push_back(std::move(conductor));

        Score score;
        const MidiImportResult result = import_midi(file, score);
        VCC_CHECK(!result.ok);
        VCC_CHECK(result.error.find("no notes") != std::string::npos);
        VCC_CHECK_EQ(score.total_note_count(), static_cast<std::size_t>(0));
        VCC_CHECK(score.tempos().empty());
    }

    // A resolution of nothing.
    {
        SmfFile file = a_file(0);
        SmfTrack track = a_track("Violin I");
        add_note(track, 0, kQuarter, 60);
        file.tracks.push_back(std::move(track));

        Score score;
        VCC_CHECK(!import_midi(file, score).ok);
    }

    // A grid that is not a note value cannot quantize anything.
    {
        SmfFile file = a_file();
        SmfTrack track = a_track("Violin I");
        add_note(track, 0, kQuarter, 60);
        file.tracks.push_back(std::move(track));

        MidiImportOptions options;
        options.grid = static_cast<NoteValue>(99);
        Score score;
        VCC_CHECK(!import_midi(file, score, options).ok);
    }
    VCC_CHECK_EQ(fresh, std::string("0"));
}

VCC_TEST(loading_replaces_whatever_the_score_held)
{
    SmfFile file = a_file();
    SmfTrack track = a_track("Violin I");
    add_note(track, 0, kQuarter, 60);
    file.tracks.push_back(std::move(track));

    Score score;
    score.make_grand_staff();
    for (int i = 0; i < 10; ++i) {
        ScoreNote note;
        note.start_tick = i * kQuarter;
        note.duration = quarter();
        note.pitch = Pitch{Step::F, 4, 0};
        score.insert_note(0, note);
    }
    score.add_text(TextMarking{0, 0, "stale"});

    VCC_CHECK(import_midi(file, score).ok);
    VCC_CHECK_EQ(score.staff_count(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(score.total_note_count(), static_cast<std::size_t>(1));
    VCC_CHECK(score.texts().empty());
}

// ---------------------------------------------------------------------------
// The round trip
// ---------------------------------------------------------------------------

VCC_TEST(a_score_written_out_and_read_back_plays_the_same_notes)
{
    // Not the same *notation* - spelling, rests and voices do not survive a
    // format that has none of them. But the notes have to: same count, same
    // ticks, same sounding pitches, same lengths.
    //
    // Rendered with no articulation shortening, so the file's note-offs sit
    // at the written lengths. That is the case where the round trip can be
    // exact, and the one below is what happens when it cannot.
    const Score original = a_short_cue();

    ExpressionMap map;
    map.default_length_scale = 1.0;
    const MidiRenderer renderer(map);

    MidiImportResult result;
    const Score imported = through_a_midi_file(original, renderer, result);
    VCC_CHECK_MSG(result.ok, result.error);

    // Nothing had to move: the export was already on the grid, releases and
    // all.
    VCC_CHECK_EQ(result.largest_move, static_cast<Ticks>(0));
    VCC_CHECK_EQ(result.largest_length_move, static_cast<Ticks>(0));
    VCC_CHECK_EQ(result.notes_dropped, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(result.unreleased_notes, static_cast<std::size_t>(0));
    VCC_CHECK_MSG(result.carried_everything(),
                  result.warnings.empty() ? "" : result.warnings.front());

    // The comparison that matters: both play the same thing. Note-offs as
    // well as note-ons, because a round trip that kept every onset and
    // halved every length would pass on onsets alone.
    const std::vector<TimedMidiEvent> before_ons = note_ons(renderer.render(original));
    const std::vector<TimedMidiEvent> after_ons = note_ons(renderer.render(imported));
    const std::vector<TimedMidiEvent> before_offs = note_offs(renderer.render(original));
    const std::vector<TimedMidiEvent> after_offs = note_offs(renderer.render(imported));

    VCC_CHECK_EQ(after_ons.size(), before_ons.size());
    VCC_CHECK(before_ons.size() >= 10);
    for (std::size_t i = 0; i < before_ons.size() && i < after_ons.size(); ++i) {
        VCC_CHECK_MSG(before_ons[i].tick == after_ons[i].tick,
                      "note " + std::to_string(i) + " came back at a different time");
        VCC_CHECK_MSG(before_ons[i].event.data1 == after_ons[i].event.data1,
                      "note " + std::to_string(i) + " came back at a different pitch");
        // Equal because the velocity the default dynamic rendered at reads
        // back as that dynamic - the round trip for a part with written
        // dynamics is the case below.
        VCC_CHECK_MSG(before_ons[i].event.data2 == after_ons[i].event.data2,
                      "note " + std::to_string(i) + " came back at a different velocity");
    }

    VCC_CHECK_EQ(after_offs.size(), before_offs.size());
    for (std::size_t i = 0; i < before_offs.size() && i < after_offs.size(); ++i) {
        VCC_CHECK_MSG(before_offs[i].tick == after_offs[i].tick,
                      "note " + std::to_string(i) + " came back with a different length");
    }

    // And the tempo and meter came back with it.
    VCC_CHECK_NEAR(imported.tempo_at(0), 96.0, 0.01);
    VCC_CHECK(imported.time_signature_at(0) == vcc::music::TimeSignature{4, 4});
    VCC_CHECK_EQ(imported.staff(0).name, std::string("Violin I"));
}

VCC_TEST(a_shortened_render_comes_back_in_time_but_not_always_in_length)
{
    // The same round trip through the default renderer, which shortens every
    // unmarked note to 95% so repeated notes do not overlap. Nothing in a
    // MIDI file says it did that, so the quantizer has to treat the release
    // as played rather than written.
    //
    // Onsets still survive exactly. Lengths survive only while the 5% is
    // smaller than half the grid can see: an eighth loses 24 ticks and
    // snaps back, a dotted half loses 144 and does not. Recorded here
    // because it is a limit of the format, and because an import that
    // claimed otherwise would be the more dangerous kind of wrong.
    const Score original = a_short_cue();
    const MidiRenderer renderer;  // default_length_scale = 0.95.

    MidiImportResult result;
    const Score imported = through_a_midi_file(original, renderer, result);
    VCC_CHECK_MSG(result.ok, result.error);

    // Every onset landed where it was written...
    VCC_CHECK_EQ(result.largest_move, static_cast<Ticks>(0));
    const std::vector<TimedMidiEvent> before_ons = note_ons(renderer.render(original));
    const std::vector<TimedMidiEvent> after_ons = note_ons(renderer.render(imported));
    VCC_CHECK_EQ(after_ons.size(), before_ons.size());
    for (std::size_t i = 0; i < before_ons.size() && i < after_ons.size(); ++i)
        VCC_CHECK_EQ(after_ons[i].tick, before_ons[i].tick);

    // ...and the releases are the part that moved, which is reported rather
    // than warned about.
    VCC_CHECK(result.largest_length_move > 0);
    bool warned_about_the_rhythm = false;
    for (const std::string& warning : result.warnings) {
        if (warning.find("moved") != std::string::npos)
            warned_about_the_rhythm = true;
    }
    VCC_CHECK_MSG(!warned_about_the_rhythm,
                  "a release that moved was reported as a rhythm that moved");

    // The short notes came back at their written value.
    VCC_CHECK(!imported.notes(0).empty());
    VCC_CHECK_EQ(imported.notes(0).front().duration.ticks(),
                 static_cast<Ticks>(kTicksPerQuarter / 2));

    // The dotted half did not. Three quarters rendered at 95% is 8208
    // ticks, and the nearest the grid offers is 8160, a sixth of a quarter
    // short - which comes back as a tie chain of that length rather than
    // as the dotted half that was written. Shorter than written rather
    // than longer, and by less than a sixteenth.
    Ticks tied_length = 0;
    std::size_t parts = 0;
    for (const ScoreNote& note : imported.notes(0)) {
        if (note.start_tick < 5 * kTicksPerQuarter)
            continue;  // Everything before this is the scale and the chord.
        tied_length += note.duration.ticks();
        ++parts;
    }
    VCC_CHECK(parts >= 1);
    VCC_CHECK_EQ(tied_length, static_cast<Ticks>(3 * kQuarter - kQuarter / 6));
    VCC_CHECK(tied_length < (3 * kQuarter));
    VCC_CHECK((3 * kQuarter) - tied_length < grid_ticks(NoteValue::Sixteenth));
}

VCC_TEST(a_transposing_part_comes_back_at_sounding_pitch)
{
    // A MIDI file says nothing about which instrument transposes, so an
    // imported part is written at sounding pitch and the composer sets the
    // transposition afterwards. Recorded because it is a real limitation,
    // not an oversight.
    Score original;
    original.staff(0).name = "Clarinet in Bb";
    original.staff(0).transposition_semitones = -2;

    ScoreNote note;
    note.start_tick = 0;
    note.duration = quarter();
    note.pitch = Pitch{Step::C, 4, 0};  // Written C4, sounding B flat 3.
    original.insert_note(0, note);

    const ScoreMidiResult exported =
        build_score_midi(original, MidiRenderer{}, ScoreMidiOptions{});
    std::vector<std::uint8_t> bytes;
    VCC_CHECK(vcc::music::write_smf(exported.file, bytes).ok);

    vcc::music::SmfFile read_back;
    VCC_CHECK(vcc::music::read_smf(bytes, read_back).ok);

    Score imported;
    VCC_CHECK(import_midi(read_back, imported).ok);

    VCC_CHECK_EQ(imported.staff(0).transposition_semitones, static_cast<std::int8_t>(0));
    VCC_CHECK_EQ(midi_note_number(imported.notes(0).front().pitch), 58);  // B flat 3.
}

// ---------------------------------------------------------------------------
// Velocities, as dynamics
// ---------------------------------------------------------------------------

VCC_TEST(a_velocity_is_read_as_the_nearest_written_level)
{
    // The default expression map's own table, inverted: what the renderer
    // writes for a level reads back as that level.
    const ExpressionMap defaults;
    for (std::size_t index = 0; index < kDynamicLevelCount; ++index) {
        const auto level = static_cast<DynamicLevel>(index);
        VCC_CHECK_EQ(dynamic_for_velocity(defaults.velocity_for(level)), level);
    }

    // In between, the nearer; midway, the quieter. The map renders mp at
    // 64 and mf at 80.
    VCC_CHECK_EQ(dynamic_for_velocity(90), DynamicLevel::Forte);       // 96 is nearer than 80.
    VCC_CHECK_EQ(dynamic_for_velocity(70), DynamicLevel::MezzoPiano);  // 64 is nearer than 80.
    VCC_CHECK_EQ(dynamic_for_velocity(74), DynamicLevel::MezzoForte);  // 80 is nearer than 64.
    VCC_CHECK_EQ(dynamic_for_velocity(72), DynamicLevel::MezzoPiano);  // Midway: the quieter.
    VCC_CHECK_EQ(dynamic_for_velocity(1), DynamicLevel::Pianississimo);
    VCC_CHECK_EQ(dynamic_for_velocity(127), DynamicLevel::Fortississimo);
}

VCC_TEST(a_level_that_holds_becomes_a_dynamic_and_a_lone_loud_note_does_not)
{
    SmfFile file = a_file();
    SmfTrack part = a_track("Horn");
    // Four quiet quarters, one accented, then four loud ones.
    for (int i = 0; i < 4; ++i)
        add_note(part, i * kQuarter, kQuarter, 60, 32);  // pp
    add_note(part, 4 * kQuarter, kQuarter, 60, 112);     // ff: an accent, on its own.
    for (int i = 5; i < 9; ++i)
        add_note(part, i * kQuarter, kQuarter, 60, 96);  // f
    file.tracks.push_back(part);

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_MSG(result.carried_everything(),
                  result.warnings.empty() ? "" : result.warnings.front());

    // pp at the start, f where the loud notes begin, and nothing for the
    // single accented note between them.
    VCC_CHECK_EQ(result.dynamics_read, static_cast<std::size_t>(2));
    VCC_CHECK_EQ(score.dynamics().size(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(score.dynamics()[0].tick, static_cast<Ticks>(0));
    VCC_CHECK_EQ(score.dynamics()[0].level, DynamicLevel::Pianissimo);
    VCC_CHECK_EQ(score.dynamics()[1].tick, static_cast<Ticks>(5 * kQuarter));
    VCC_CHECK_EQ(score.dynamics()[1].level, DynamicLevel::Forte);

    // Which is what the part now plays at.
    VCC_CHECK_EQ(score.dynamic_at(2 * kQuarter), DynamicLevel::Pianissimo);
    VCC_CHECK_EQ(score.dynamic_at(7 * kQuarter), DynamicLevel::Forte);
}

VCC_TEST(the_opening_dynamic_is_at_the_first_note_even_when_that_note_is_the_odd_one)
{
    SmfFile file = a_file();
    SmfTrack part = a_track("Horn");
    add_note(part, 0, kQuarter, 60, 112);  // A loud first note, then a quiet line.
    for (int i = 1; i < 4; ++i)
        add_note(part, i * kQuarter, kQuarter, 60, 48);  // p
    file.tracks.push_back(part);

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);

    // One mark, p, at the start: the level that holds, placed where the
    // part begins so it opens with a dynamic as a written part does.
    VCC_CHECK_EQ(score.dynamics().size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(score.dynamics()[0].tick, static_cast<Ticks>(0));
    VCC_CHECK_EQ(score.dynamics()[0].level, DynamicLevel::Piano);

    // And a file with a single note takes that note's level.
    SmfFile one = a_file();
    SmfTrack lone = a_track("Horn");
    add_note(lone, 0, kQuarter, 60, 112);
    one.tracks.push_back(lone);
    Score single;
    VCC_CHECK(import_midi(one, single).ok);
    VCC_CHECK_EQ(single.dynamics().size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(single.dynamics()[0].level, DynamicLevel::Fortissimo);
}

VCC_TEST(a_chords_level_is_its_loudest_note)
{
    SmfFile file = a_file();
    SmfTrack part = a_track("Piano");
    for (int i = 0; i < 2; ++i) {
        add_note(part, i * kQuarter, kQuarter, 60, 40);
        add_note(part, i * kQuarter, kQuarter, 64, 96);  // The top of the chord is loud.
        add_note(part, i * kQuarter, kQuarter, 67, 40);
    }
    file.tracks.push_back(part);

    Score score;
    VCC_CHECK(import_midi(file, score).ok);
    VCC_CHECK_EQ(score.dynamics().size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(score.dynamics()[0].level, DynamicLevel::Forte);
}

VCC_TEST(dynamics_can_be_left_unread)
{
    SmfFile file = a_file();
    SmfTrack part = a_track("Horn");
    for (int i = 0; i < 4; ++i)
        add_note(part, i * kQuarter, kQuarter, 60, 32);
    file.tracks.push_back(part);

    MidiImportOptions options;
    options.read_dynamics = false;

    Score score;
    const MidiImportResult result = import_midi(file, score, options);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.dynamics_read, static_cast<std::size_t>(0));
    VCC_CHECK(score.dynamics().empty());
    // The notes are all there regardless.
    VCC_CHECK_EQ(score.total_note_count(), static_cast<std::size_t>(4));
}

VCC_TEST(dynamics_from_several_parts_are_read_together_and_said_so)
{
    SmfFile file = a_file();
    SmfTrack melody = a_track("Flute");
    SmfTrack bass = a_track("Bass");
    for (int i = 0; i < 4; ++i) {
        add_note(melody, i * kQuarter, kQuarter, 72, 96);  // f
        add_note(bass, i * kQuarter, kQuarter, 40, 32);    // pp, under it.
    }
    file.tracks.push_back(melody);
    file.tracks.push_back(bass);

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);

    // One score-wide level, the louder of the two, and a warning that
    // says it was read from both.
    VCC_CHECK_EQ(score.dynamics().size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(score.dynamics()[0].level, DynamicLevel::Forte);
    VCC_CHECK(!result.carried_everything());
    bool said = false;
    for (const std::string& warning : result.warnings)
        said = said || warning.find("2 parts together") != std::string::npos;
    VCC_CHECK_MSG(said, result.warnings.empty() ? "no warning" : result.warnings.front());

    // A single part reads cleanly: there is nothing to warn about.
    SmfFile alone = a_file();
    alone.tracks.push_back(melody);
    Score solo;
    const MidiImportResult clean = import_midi(alone, solo);
    VCC_CHECK(clean.carried_everything());
}

VCC_TEST(a_score_with_dynamics_comes_back_with_them)
{
    // The export writes each dynamic as the velocity the default map gives
    // it; the import reads that velocity back as the level. So a part this
    // application wrote keeps its dynamics through the file.
    Score original = a_short_cue();
    original.add_dynamic(DynamicMarking{0, DynamicLevel::Piano, 0, ""});
    original.add_dynamic(DynamicMarking{4 * kTicksPerQuarter, DynamicLevel::Fortissimo, 0, ""});

    ExpressionMap map;
    map.default_length_scale = 1.0;
    const MidiRenderer renderer(map);

    MidiImportResult result;
    const Score imported = through_a_midi_file(original, renderer, result);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_MSG(result.carried_everything(),
                  result.warnings.empty() ? "" : result.warnings.front());

    VCC_CHECK_EQ(imported.dynamics().size(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(imported.dynamics()[0].tick, static_cast<Ticks>(0));
    VCC_CHECK_EQ(imported.dynamics()[0].level, DynamicLevel::Piano);
    VCC_CHECK_EQ(imported.dynamics()[1].tick, static_cast<Ticks>(4 * kTicksPerQuarter));
    VCC_CHECK_EQ(imported.dynamics()[1].level, DynamicLevel::Fortissimo);

    // And so it plays at the same velocities, which is the point.
    const std::vector<TimedMidiEvent> before = note_ons(renderer.render(original));
    const std::vector<TimedMidiEvent> after = note_ons(renderer.render(imported));
    VCC_CHECK_EQ(before.size(), after.size());
    for (std::size_t i = 0; i < before.size() && i < after.size(); ++i) {
        VCC_CHECK_MSG(before[i].event.data2 == after[i].event.data2,
                      "note " + std::to_string(i) + " came back at a different velocity");
    }
}

VCC_TEST(markers_and_text_events_come_in_as_text_and_the_tracks_names_do_not)
{
    SmfFile file = a_file();
    SmfTrack conductor_track;
    conductor_track.name = "Conductor";
    conductor_track.meta.push_back(vcc::music::smf_tempo(0, 120.0));
    file.tracks.push_back(conductor_track);
    SmfTrack part;
    part.name = "Violin";
    part.events.push_back(SmfEvent{0, vcc::music::make_note_on(0, 60, 90)});
    part.events.push_back(SmfEvent{kQuarter, vcc::music::make_note_off(0, 60)});
    file.tracks.push_back(part);
    vcc::music::SmfTrack& conductor = file.tracks.front();
    conductor.meta.push_back(vcc::music::smf_marker(0, "A"));
    conductor.meta.push_back(vcc::music::smf_text(kQuarter, vcc::music::smf_meta::kText, "  Hit  "));
    conductor.meta.push_back(vcc::music::smf_text(kQuarter, vcc::music::smf_meta::kCopyright, "(c) nobody"));
    conductor.meta.push_back(vcc::music::smf_marker((2 * kQuarter), "   "));
    // The same marker on the part's track as well, as some exporters write
    // them: one marker, not two.
    file.tracks.back().meta.push_back(vcc::music::smf_marker(0, "A"));
    file.tracks.back().meta.push_back(
        vcc::music::smf_text(0, vcc::music::smf_meta::kInstrumentName, "Violin"));

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.markers_read, static_cast<std::size_t>(2));
    VCC_CHECK_EQ(score.texts().size(), static_cast<std::size_t>(2));
    if (score.texts().size() == 2) {
        VCC_CHECK_EQ(score.texts()[0].tick, static_cast<Ticks>(0));
        VCC_CHECK_EQ(score.texts()[0].text, std::string("A"));
        VCC_CHECK_EQ(score.texts()[1].tick, static_cast<Ticks>(kQuarter));
        VCC_CHECK_EQ(score.texts()[1].text, std::string("Hit"));
        VCC_CHECK_EQ(score.texts()[1].voice, static_cast<std::uint8_t>(0));
    }

    // Turned off, the file keeps them.
    MidiImportOptions options;
    options.read_markers = false;
    Score bare;
    const MidiImportResult left = import_midi(file, bare, options);
    VCC_CHECK_MSG(left.ok, left.error);
    VCC_CHECK_EQ(left.markers_read, static_cast<std::size_t>(0));
    VCC_CHECK(bare.texts().empty());
}

VCC_TEST(a_score_with_text_markings_comes_back_with_them)
{
    Score original = a_short_cue();
    original.add_text(TextMarking{0, 0, "A"});
    original.add_text(TextMarking{4 * kTicksPerQuarter, 1, "Hit"});

    MidiImportResult result;
    const Score imported = through_a_midi_file(original, MidiRenderer{}, result);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(imported.texts().size(), static_cast<std::size_t>(2));
    if (imported.texts().size() == 2) {
        VCC_CHECK_EQ(imported.texts()[0].text, std::string("A"));
        VCC_CHECK_EQ(imported.texts()[1].tick, static_cast<Ticks>(4 * kTicksPerQuarter));
        VCC_CHECK_EQ(imported.texts()[1].text, std::string("Hit"));
    }
}

// ---------------------------------------------------------------------------
// Voices
// ---------------------------------------------------------------------------

namespace {

/// The voice of every note at a tick with a pitch, or -1 for none.
int voice_of(const Score& score, Ticks tick, int note_number, std::size_t staff = 0)
{
    for (const ScoreNote& note : score.notes(staff)) {
        if (note.start_tick == tick && midi_note_number(note.pitch) == note_number)
            return note.voice;
    }
    return -1;
}

const ScoreNote* note_at(const Score& score, Ticks tick, int note_number, std::size_t staff = 0)
{
    for (const ScoreNote& note : score.notes(staff)) {
        if (note.start_tick == tick && midi_note_number(note.pitch) == note_number)
            return &note;
    }
    return nullptr;
}

}  // namespace

VCC_TEST(a_held_note_under_a_moving_line_takes_the_lower_voice)
{
    // A bass note held for the bar under four quarters: one voice cannot
    // hold a note that begins while another of its own still sounds, so
    // the file forces two, and the lower line takes the down-stem voice.
    SmfFile file = a_file();
    SmfTrack track = a_track("Piano");
    add_note(track, 0, (4 * kQuarter), 48);
    for (int beat = 0; beat < 4; ++beat)
        add_note(track, beat * kQuarter, kQuarter, 64);
    file.tracks.push_back(track);

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.notes_read, std::size_t{5});
    VCC_CHECK_EQ(voice_of(score, 0, 48), 1);
    for (int beat = 0; beat < 4; ++beat)
        VCC_CHECK_EQ(voice_of(score, beat * kQuarter, 64), 0);
    VCC_CHECK_EQ(result.notes_in_other_voices, std::size_t{1});
    // Nothing was guessed that the file did not force: no warning.
    VCC_CHECK_MSG(result.carried_everything(),
                  result.warnings.empty() ? "" : result.warnings.front());

    // The held note and the quarter above it start together but are not a
    // chord: they are in different voices.
    const ScoreNote* held = note_at(score, 0, 48);
    const ScoreNote* first = note_at(score, 0, 64);
    VCC_CHECK(held != nullptr && first != nullptr);
    if (held != nullptr && first != nullptr) {
        VCC_CHECK(!held->in_chord);
        VCC_CHECK(!first->in_chord);
    }
}

VCC_TEST(a_held_note_above_a_moving_line_keeps_the_upper_voice)
{
    // The lines are told apart by pitch: a held treble note over a moving
    // bass is the up-stem voice, and the movement under it the down-stem.
    SmfFile file = a_file();
    SmfTrack track = a_track("Piano");
    add_note(track, 0, (4 * kQuarter), 79);
    for (int beat = 0; beat < 4; ++beat)
        add_note(track, beat * kQuarter, kQuarter, 60);
    file.tracks.push_back(track);

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(voice_of(score, 0, 79), 0);
    for (int beat = 0; beat < 4; ++beat)
        VCC_CHECK_EQ(voice_of(score, beat * kQuarter, 60), 1);
    VCC_CHECK_EQ(result.notes_in_other_voices, std::size_t{4});
}

VCC_TEST(a_line_that_enters_above_a_held_note_still_takes_the_upper_voice)
{
    // The held note opens the passage alone, and the line that enters
    // over it a beat later is still the top line: the voices are numbered
    // by pitch, not by which line began first.
    SmfFile file = a_file();
    SmfTrack track = a_track("Piano");
    add_note(track, 0, (4 * kQuarter), 48);
    for (int beat = 1; beat < 4; ++beat)
        add_note(track, beat * kQuarter, kQuarter, 64);
    file.tracks.push_back(track);

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(voice_of(score, 0, 48), 1);
    for (int beat = 1; beat < 4; ++beat)
        VCC_CHECK_EQ(voice_of(score, beat * kQuarter, 64), 0);
}

VCC_TEST(notes_starting_together_with_different_lengths_are_two_voices_not_one_chord)
{
    // A half under two quarters at the same onset: the quarters are one
    // chord, the half is a voice of its own, and the quarter that follows
    // while the half still sounds joins the quarters' voice.
    SmfFile file = a_file();
    SmfTrack track = a_track("Piano");
    add_note(track, 0, (2 * kQuarter), 60);
    add_note(track, 0, kQuarter, 64);
    add_note(track, 0, kQuarter, 67);
    add_note(track, kQuarter, kQuarter, 62);
    file.tracks.push_back(track);

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.notes_read, std::size_t{4});
    VCC_CHECK_EQ(voice_of(score, 0, 60), 1);
    VCC_CHECK_EQ(voice_of(score, 0, 64), 0);
    VCC_CHECK_EQ(voice_of(score, 0, 67), 0);
    VCC_CHECK_EQ(voice_of(score, kQuarter, 62), 0);

    const ScoreNote* half = note_at(score, 0, 60);
    const ScoreNote* third = note_at(score, 0, 64);
    const ScoreNote* fifth = note_at(score, 0, 67);
    VCC_CHECK(half != nullptr && third != nullptr && fifth != nullptr);
    if (half != nullptr && third != nullptr && fifth != nullptr) {
        VCC_CHECK(!half->in_chord);
        VCC_CHECK(third->in_chord);
        VCC_CHECK(fifth->in_chord);
    }
}

VCC_TEST(a_file_with_no_overlaps_lands_in_the_first_voice_as_before)
{
    // Chords and a run, nothing sounding into anything else: one voice,
    // and the chord stays a chord.
    SmfFile file = a_file();
    SmfTrack track = a_track("Flute");
    add_note(track, 0, kQuarter, 60);
    add_note(track, 0, kQuarter, 64);
    add_note(track, kQuarter, (kQuarter / 2), 62);
    add_note(track, (3 * kQuarter / 2), (kQuarter / 2), 64);
    add_note(track, (2 * kQuarter), (2 * kQuarter), 65);
    file.tracks.push_back(track);

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.notes_in_other_voices, std::size_t{0});
    for (const ScoreNote& note : score.notes(0))
        VCC_CHECK_EQ(static_cast<int>(note.voice), 0);
    const ScoreNote* root = note_at(score, 0, 60);
    VCC_CHECK(root != nullptr && root->in_chord);
}

VCC_TEST(voices_are_not_inferred_when_the_option_is_off)
{
    // Off, everything lands in the first voice as it always did, and notes
    // starting together are one chord whatever their lengths.
    SmfFile file = a_file();
    SmfTrack track = a_track("Piano");
    add_note(track, 0, (4 * kQuarter), 48);
    for (int beat = 0; beat < 4; ++beat)
        add_note(track, beat * kQuarter, kQuarter, 64);
    file.tracks.push_back(track);

    MidiImportOptions options;
    options.infer_voices = false;
    Score score;
    const MidiImportResult result = import_midi(file, score, options);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.notes_in_other_voices, std::size_t{0});
    for (const ScoreNote& note : score.notes(0))
        VCC_CHECK_EQ(static_cast<int>(note.voice), 0);
    const ScoreNote* held = note_at(score, 0, 48);
    VCC_CHECK(held != nullptr && held->in_chord);
}

VCC_TEST(more_lines_than_four_voices_hold_is_said)
{
    // Five notes, each beginning while the four before it still sound:
    // four voices hold four lines, and the fifth is written in the fourth
    // voice and counted, rather than dropped or passed off as a chord.
    SmfFile file = a_file();
    SmfTrack track = a_track("Cluster");
    for (int line = 0; line < 5; ++line)
        add_note(track, line * (kQuarter / 4), (4 * kQuarter), 72 - line * 2);
    file.tracks.push_back(track);

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.notes_read, std::size_t{5});
    bool said = false;
    for (const std::string& warning : result.warnings)
        said = said || warning.find("four voices") != std::string::npos;
    VCC_CHECK_MSG(said, "five overlapping lines were written without a word");
    for (const ScoreNote& note : score.notes(0))
        VCC_CHECK(note.voice <= 3);
    // The highest line is the first voice, the lowest the fourth.
    VCC_CHECK_EQ(voice_of(score, 0, 72), 0);
    VCC_CHECK_EQ(voice_of(score, (3 * kQuarter / 4), 66), 3);
    VCC_CHECK_EQ(voice_of(score, kQuarter, 64), 3);
}

VCC_TEST(a_tie_chain_keeps_one_voice_through_its_parts)
{
    // A bass note held five quarters is a whole tied to a quarter, and
    // both parts are the same line - in the same voice, under the same
    // moving upper line.
    SmfFile file = a_file();
    SmfTrack track = a_track("Piano");
    add_note(track, 0, (5 * kQuarter), 48);
    for (int beat = 0; beat < 5; ++beat)
        add_note(track, beat * kQuarter, kQuarter, 64);
    file.tracks.push_back(track);

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    const ScoreNote* first_part = note_at(score, 0, 48);
    const ScoreNote* second_part = note_at(score, (4 * kQuarter), 48);
    VCC_CHECK(first_part != nullptr && second_part != nullptr);
    if (first_part != nullptr && second_part != nullptr) {
        VCC_CHECK(first_part->tied_to_next);
        VCC_CHECK_EQ(static_cast<int>(first_part->voice), 1);
        VCC_CHECK_EQ(static_cast<int>(second_part->voice), 1);
    }
    VCC_CHECK_EQ(voice_of(score, (4 * kQuarter), 64), 0);
    VCC_CHECK_EQ(result.notes_in_other_voices, std::size_t{2});
}

VCC_TEST(an_overlap_the_grid_removes_is_not_a_voice)
{
    // A note released a few ticks after the next begins, as a legato
    // player does: the grid snaps the release back, nothing overlaps, and
    // the line is one voice.
    SmfFile file = a_file();
    SmfTrack track = a_track("Flute");
    add_note(track, 0, 980, 60);
    add_note(track, kQuarter, kQuarter, 62);
    file.tracks.push_back(track);

    Score score;
    const MidiImportResult result = import_midi(file, score);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.notes_in_other_voices, std::size_t{0});
    VCC_CHECK_EQ(voice_of(score, 0, 60), 0);
    VCC_CHECK_EQ(voice_of(score, kQuarter, 62), 0);
}

VCC_TEST(a_two_voice_staff_comes_back_in_two_voices)
{
    // A soprano line in quarters over an alto line in halves, written out
    // and read back: the alto comes back as the second voice, so a part
    // this application exported keeps its lines.
    Score original;
    original.set_time_signature(0, vcc::music::TimeSignature{4, 4});
    const Step upper[] = {Step::E, Step::F, Step::G, Step::A};
    for (int beat = 0; beat < 4; ++beat) {
        ScoreNote note;
        note.start_tick = beat * kTicksPerQuarter;
        note.duration = quarter();
        note.pitch = Pitch{upper[beat], 5, 0};
        original.insert_note(0, note);
    }
    for (int half_index = 0; half_index < 2; ++half_index) {
        ScoreNote note;
        note.start_tick = half_index * 2 * kTicksPerQuarter;
        note.duration = half();
        note.pitch = Pitch{half_index == 0 ? Step::C : Step::B, static_cast<std::int8_t>(half_index == 0 ? 4 : 3), 0};
        note.voice = 1;
        original.insert_note(0, note);
    }

    ExpressionMap map;
    map.default_length_scale = 1.0;
    const MidiRenderer renderer(map);
    MidiImportResult result;
    const Score imported = through_a_midi_file(original, renderer, result);
    VCC_CHECK_MSG(result.ok, result.error);
    VCC_CHECK_EQ(result.notes_read, std::size_t{6});
    VCC_CHECK_EQ(result.notes_in_other_voices, std::size_t{2});
    VCC_CHECK_EQ(voice_of(imported, 0, 60), 1);
    VCC_CHECK_EQ(voice_of(imported, 2 * kTicksPerQuarter, 59), 1);
    for (int beat = 0; beat < 4; ++beat) {
        const Pitch pitch{upper[beat], 5, 0};
        VCC_CHECK_EQ(voice_of(imported, beat * kTicksPerQuarter, midi_note_number(pitch)), 0);
    }
}

VCC_TEST_MAIN("midi import")
